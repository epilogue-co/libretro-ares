// SPDX-License-Identifier: ISC
// © 2026 Epilogue (epilogue.co)

#include "program.hpp"
#include "libretro_core_options.h"

// paraLLEl-RDP wraps Vulkan via volk + vulkan_headers.hpp; n64.hpp must come
// first so vulkan_core.h sees VK_NO_PROTOTYPES via volk before any other
// include resolves it.
#include <n64/n64.hpp>

// parallel-rdp's vulkan_headers.hpp (pulled in via n64.hpp) #defines
// VK_USE_PLATFORM_WIN32_KHR, which makes the bundled <vulkan/vulkan.h>
// (included by libretro_vulkan.h) drag in <windows.h>; its `boolean` then
// clashes with nall's `boolean`. The core gets its Vulkan context from the
// frontend and never creates a Win32 WSI surface, so suppress that include.
#undef VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include "libretro_vulkan.h"

// paraLLEl-RDP's Util::LoggingInterface lets us redirect its LOGI/LOGW/LOGE
// macros (Granite-style) into libretro's retro_log_callback instead of the
// default fprintf-to-stderr fallback.
#include "../ares/n64/vulkan/parallel-rdp/util/logging.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

retro_environment_t  environ_cb         = nullptr;
retro_video_refresh_t video_cb          = nullptr;
// Vulkan HW_RENDER state (libretro_vulkan.h protocol). When the frontend
// supports Vulkan HW_RENDER, we hand off the rendered VkImage directly to
// it instead of doing a GPU->CPU readback.
retro_hw_render_callback hw_render_cb = {};
const retro_hw_render_interface_vulkan* vulkan_iface = nullptr;
bool hw_render_requested = false;
retro_audio_sample_t  audio_sample_cb   = nullptr;
retro_audio_sample_batch_t audio_batch_cb = nullptr;
retro_input_poll_t   input_poll_cb      = nullptr;
retro_input_state_t  input_state_cb     = nullptr;
retro_log_printf_t   log_cb             = nullptr;
retro_set_rumble_state_t rumble_cb      = nullptr;

// libretro experimental extension: save-updated callback. The frontend
// registers a function we call once per save event (after a quiescent
// cooldown — see save_check_dirty_edge below). ares' bundled libretro.h
// doesn't define this yet, so we declare it locally — same approach
// mgba's libretro core uses.
#ifndef RETRO_ENVIRONMENT_SET_SAVE_UPDATED_CALLBACK
#define RETRO_ENVIRONMENT_SET_SAVE_UPDATED_CALLBACK (880 | RETRO_ENVIRONMENT_EXPERIMENTAL)
typedef void (RETRO_CALLCONV *retro_save_updated_callback_t)(void* context);
struct retro_save_updated_callback {
  retro_save_updated_callback_t callback;
};
#endif
static retro_save_updated_callback_t save_updated_cb = nullptr;
// Cooldown in emulated frames between the last save-memory write and
// the moment we fire save_updated_cb. mgba uses 15 frames (~250 ms at
// 60 Hz) — long enough to coalesce a burst of writes from one in-game
// save event into a single callback, short enough to be responsive.
static constexpr u32 save_dirty_cooldown_frames = 15;
static u32 save_frame_counter = 0;

// Refresh-rate auto-detect state. The ROM region byte is unreliable on
// hybrid discs (e.g. OoT GameCube Collector's Edition ships with a PAL
// header but programs the VI for NTSC line counts, so our 50fps timer
// drives a 60Hz emulated VI and the game runs at 50/60 = 83% speed).
// We measure the actual VI vblank rate from the live registers each
// frame and, after a stability window, push the correct fps to the
// frontend via SET_SYSTEM_AV_INFO.
static double refreshRateLast = 0.0;
static u32 refreshRateStable = 0;
static constexpr u32 refreshRateStableThreshold = 30; // ~0.5s at 60fps
static constexpr double refreshRateUpdateEpsilon = 0.5;

static double measureViRefreshRate() {
  if(!program.loaded) return 0.0;
  auto& vi = ares::Nintendo64::vi;
  u64 vfreq = ares::Nintendo64::system.videoFrequency();
  u64 lpf = (u64)vi.io.halfLinesPerField + 1;
  u64 qld = (u64)vi.io.quarterLineDuration + 1;
  if(vfreq == 0 || lpf <= 1 || qld == 0) return 0.0;
  // Per VI::main: vblank fires every (halfLinesPerField+1)/2 iterations
  // (vcounter increments once per iter, halfline = vcounter*2 + field
  // and the threshold is halfLinesPerField+1). Each iter steps the VI
  // clock by quarterLineDuration+1 pixel clocks.
  return 2.0 * (double)vfreq / ((double)lpf * (double)qld);
}

static void updateRefreshRateIfChanged() {
  double measured = measureViRefreshRate();
  if(measured < 30.0 || measured > 70.0) {
    refreshRateStable = 0;
    return;
  }
  if(std::abs(measured - refreshRateLast) > refreshRateUpdateEpsilon) {
    refreshRateLast = measured;
    refreshRateStable = 0;
    return;
  }
  if(refreshRateStable < refreshRateStableThreshold) {
    ++refreshRateStable;
    return;
  }
  if(refreshRateStable == refreshRateStableThreshold) {
    ++refreshRateStable;
    if(std::abs(measured - program.refreshRate) > refreshRateUpdateEpsilon) {
      double previous = program.refreshRate;
      program.refreshRate = measured;
      retro_system_av_info av = {};
      retro_get_system_av_info(&av);
      if(environ_cb) environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
      if(log_cb) log_cb(RETRO_LOG_INFO,
        "ares: VI refresh rate updated to %.3f Hz (was %.3f Hz)\n",
        measured, previous);
    }
  }
}

namespace {
  void fallback_log(retro_log_level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
  }

  // Forwards paraLLEl-RDP's interface_log() calls into libretro's log_cb so
  // the "[INFO]: Found Vulkan GPU..." and "[WARN]: Stalled compile" chunks
  // (Granite/Util LOGI/LOGW/LOGE) get a proper [time][level][thread] frame
  // on the frontend side instead of fprintf'ing straight to stderr.
  // Installed via the process-wide fallback so internal pipeline-compile
  // worker threads spawned by Granite are caught alongside the retro thread.
  struct ParallelRdpLogForwarder : Util::LoggingInterface {
    bool log(const char* tag, const char* fmt, va_list va) override {
      if(!log_cb) return false;
      char buf[2048];
      int n = std::vsnprintf(buf, sizeof(buf), fmt, va);
      if(n < 0) return false;
      if(n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
      // Drop trailing newlines — log_cb adds its own line terminator.
      while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
      retro_log_level level = RETRO_LOG_INFO;
      if(tag) {
        if(std::strstr(tag, "ERROR"))     level = RETRO_LOG_ERROR;
        else if(std::strstr(tag, "WARN")) level = RETRO_LOG_WARN;
      }
      log_cb(level, "[parallel-rdp] %s\n", buf);
      return true;
    }
  };
  ParallelRdpLogForwarder gParallelRdpLogger;

  std::string pipelineCachePath() {
    if(!environ_cb) return {};
    const char* dir = nullptr;
    if(!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) || !dir) {
      if(!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) || !dir) return {};
    }
    return std::string{dir} + "/ares-rdp-pipeline.cache";
  }

  void loadPipelineCache() {
    auto path = pipelineCachePath();
    if(path.empty()) return;
    FILE* fp = std::fopen(path.c_str(), "rb");
    if(!fp) return;
    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if(size > 0 && size < (1 << 28)) {
      ares::Nintendo64::vulkan.pipelineCache.resize((size_t)size);
      if(std::fread(ares::Nintendo64::vulkan.pipelineCache.data(), 1, (size_t)size, fp) != (size_t)size) {
        ares::Nintendo64::vulkan.pipelineCache.clear();
      }
    }
    std::fclose(fp);
  }

  void savePipelineCache() {
    auto& blob = ares::Nintendo64::vulkan.pipelineCache;
    if(blob.empty()) return;
    auto path = pipelineCachePath();
    if(path.empty()) return;
    FILE* fp = std::fopen(path.c_str(), "wb");
    if(!fp) return;
    std::fwrite(blob.data(), 1, blob.size(), fp);
    std::fclose(fp);
  }

  // === Vulkan HW_RENDER negotiation =====================================
  // libretro_vulkan.h protocol v2. When the frontend supports Vulkan
  // HW_RENDER, we hand paraLLEl-RDP an external instance + device and skip
  // the GPU->CPU readback.

  VkApplicationInfo vk_app_info = {
    VK_STRUCTURE_TYPE_APPLICATION_INFO,
    nullptr,
    "ares-libretro", 0,
    "ares-libretro", 0,
    VK_API_VERSION_1_1,
  };

  const VkApplicationInfo* hw_get_application_info(void) {
    return &vk_app_info;
  }

  // v1 negotiation: frontend asks core to pick a physical device and create
  // a VkDevice. Defer to paraLLEl-RDP's init_device_from_instance helper,
  // which knows what extensions/features it needs.
  bool hw_create_device(
      struct retro_vulkan_context* context,
      VkInstance instance,
      VkPhysicalDevice gpu,
      VkSurfaceKHR surface,
      PFN_vkGetInstanceProcAddr get_instance_proc_addr,
      const char** required_device_extensions,
      unsigned num_required_device_extensions,
      const char** required_device_layers,
      unsigned num_required_device_layers,
      const VkPhysicalDeviceFeatures* required_features) {
    return ares::Nintendo64::vulkan.createDeviceFromInstance(
      context, instance, gpu, surface, get_instance_proc_addr,
      required_device_extensions, num_required_device_extensions,
      required_features);
  }

  void hw_destroy_device(void) {
    ares::Nintendo64::vulkan.destroyExternalDevice();
  }

  retro_hw_render_context_negotiation_interface_vulkan vk_negotiation = {
    RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
    RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
    hw_get_application_info,
    hw_create_device,
    hw_destroy_device,
    nullptr,  // v2 create_instance — not needed for paraLLEl-RDP
    nullptr,  // v2 create_device2 — not needed
  };

  void hw_context_reset(void) {
    // Frontend has created the Vulkan context. Fetch the live interface so
    // we can call set_image() per frame.
    const retro_hw_render_interface* base_iface = nullptr;
    if(!environ_cb || !environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &base_iface) || !base_iface) {
      if(log_cb) log_cb(RETRO_LOG_ERROR, "ares: GET_HW_RENDER_INTERFACE failed; falling back to CPU readback\n");
      vulkan_iface = nullptr;
      return;
    }
    if(base_iface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN) {
      if(log_cb) log_cb(RETRO_LOG_ERROR, "ares: HW interface type mismatch; expected Vulkan\n");
      vulkan_iface = nullptr;
      return;
    }
    vulkan_iface = (const retro_hw_render_interface_vulkan*)base_iface;
    ares::Nintendo64::vulkan.setHwRenderInterface(vulkan_iface);
    if(log_cb) log_cb(RETRO_LOG_INFO, "ares: Vulkan HW_RENDER active; zero-copy frame handoff enabled\n");
  }

  void hw_context_destroy(void) {
    ares::Nintendo64::vulkan.setHwRenderInterface(nullptr);
    vulkan_iface = nullptr;
  }
}

RETRO_API void retro_set_environment(retro_environment_t cb) {
  environ_cb = cb;

  bool no_rom = false;
  cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);

  unsigned options_version = 0;
  if(cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &options_version) && options_version >= 2) {
    retro_core_options_v2 opts = options_us;
    cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &opts);
  }

  static const retro_controller_description controllers[] = {
    {"Nintendo 64 Controller", RETRO_DEVICE_JOYPAD},
  };
  static const retro_controller_info ports[] = {
    {controllers, 1},
    {controllers, 1},
    {controllers, 1},
    {controllers, 1},
    {nullptr, 0},
  };
  cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

  static const retro_input_descriptor descriptors[] = {
#define ARES_PORT_DESC(p)                                                                               \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up"},                             \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down"},                           \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left"},                           \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right"},                          \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A"},                                    \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B"},                                    \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Z"},                                    \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start"},                                \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L"},                                    \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R"},                                    \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "C-Up"},                                 \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "C-Down"},                               \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "C-Left"},                               \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "C-Right"},                              \
    {p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, "Analog Stick X"}, \
    {p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y, "Analog Stick Y"}, \
    {p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "C-Buttons X"},  \
    {p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "C-Buttons Y"}
    ARES_PORT_DESC(0),
    ARES_PORT_DESC(1),
    ARES_PORT_DESC(2),
    ARES_PORT_DESC(3),
#undef ARES_PORT_DESC
    {0, 0, 0, 0, nullptr},
  };
  cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)descriptors);

  // Save-updated callback: frontend fills in cb->callback; we invoke it
  // from retro_run when the save dirty bit clears (mgba-style cooldown).
  retro_save_updated_callback save_cb_struct = {};
  if(cb(RETRO_ENVIRONMENT_SET_SAVE_UPDATED_CALLBACK, &save_cb_struct)) {
    save_updated_cb = save_cb_struct.callback;
  }
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)        { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)          { audio_sample_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)              { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)            { input_state_cb = cb; }

RETRO_API unsigned retro_api_version(void) {
  return RETRO_API_VERSION;
}

RETRO_API void retro_init(void) {
  retro_log_callback log{};
  if(environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
    log_cb = log.log;
  } else {
    log_cb = fallback_log;
  }

  // Route paraLLEl-RDP's LOGI/LOGW/LOGE through libretro's logger.
  // Process-wide install so pipeline-compile worker threads spawned
  // internally by Granite are caught too (their thread-local is unset).
  Util::set_process_logging_interface(&gParallelRdpLogger);

  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
  if(environ_cb) environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

#ifdef __APPLE__
  // paraLLEl-RDP issues descriptor-heavy compute dispatches; argument buffers
  // batch descriptor updates into one Metal binding, cutting per-dispatch CPU
  // cost. Parallelizing the SPIR-V→MSL→Metal compile pipeline absorbs the
  // remaining first-encounter pipeline stalls across cores.
  setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 0);
  setenv("MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "1", 0);
#endif

  mia::setSaveLocation([] { return string{}; });

  retro_rumble_interface rumble{};
  if(environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble)) {
    rumble_cb = rumble.set_rumble_state;
  }
}

RETRO_API void retro_deinit(void) {
  program.unload();
}

RETRO_API void retro_get_system_info(retro_system_info* info) {
  std::memset(info, 0, sizeof(*info));
  info->library_name     = "ares";
  info->library_version  = "v0.1";
  info->valid_extensions = "n64|v64|z64|ndd";
  info->need_fullpath    = true;
  info->block_extract    = false;
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info) {
  std::memset(info, 0, sizeof(*info));
  info->geometry.base_width   = 320;
  info->geometry.base_height  = 240;
  info->geometry.max_width    = 1280;
  info->geometry.max_height   = 960;
  info->geometry.aspect_ratio = 4.0f / 3.0f;
  info->timing.fps           = program.refreshRate > 0 ? program.refreshRate : 60.0;
  info->timing.sample_rate   = 44100.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
  if(port < 4) {
    program.portState[port].connected = (device == RETRO_DEVICE_JOYPAD);
  }
}

RETRO_API void retro_reset(void) {
  program.reset();
}

RETRO_API void retro_run(void) {
  if(program.shutdownRequested) return;
  program.runFrame();

  updateRefreshRateIfChanged();

  // Save-dirty falling-edge detection (mgba pattern).
  // Cartridge::markSaveDirty() OR's in DirtyNew on every save-memory
  // mutation. Here we snapshot the dirty state, run the state machine,
  // and fire save_updated_cb on the falling edge:
  //   DirtyNew -> stamp frame, promote to DirtySeen.
  //   DirtySeen aged > N frames -> clear, fires callback once.
  if(save_updated_cb && program.loaded) {
    auto& cart = ares::Nintendo64::cartridge;
    ++save_frame_counter;
    u32 wasDirty = cart.saveDirtyState;
    if(cart.saveDirtyState & ares::Nintendo64::Cartridge::DirtyNew) {
      cart.saveDirtyState &= ~ares::Nintendo64::Cartridge::DirtyNew;
      cart.saveDirtyState |= ares::Nintendo64::Cartridge::DirtySeen;
      cart.saveLastDirtyFrame = save_frame_counter;
    } else if(cart.saveDirtyState & ares::Nintendo64::Cartridge::DirtySeen) {
      if(save_frame_counter - cart.saveLastDirtyFrame > save_dirty_cooldown_frames) {
        cart.saveDirtyState = 0;
      }
    }
    if(wasDirty && cart.saveDirtyState == 0) {
      save_updated_cb(nullptr);
    }
  }
}

RETRO_API size_t retro_serialize_size(void) {
  return program.serializeSize();
}

RETRO_API bool retro_serialize(void* data, size_t size) {
  return program.serialize(data, size);
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
  bool ok = program.unserialize(data, size);
  if(ok && program.loaded) {
    // State load replaces in-memory save bytes; flag dirty so the next
    // quiescent window flushes them to disk via save_updated_cb.
    ares::Nintendo64::cartridge.markSaveDirty();
  }
  return ok;
}

RETRO_API void retro_cheat_reset(void) {
  program.activeCheats.clear();
}

RETRO_API void retro_cheat_set(unsigned, bool enabled, const char* code) {
  if(!enabled || !code) return;
  program.applyCheat(code);
}

RETRO_API bool retro_load_game(const retro_game_info* game) {
  if(!game || !game->path) return false;

  // Attempt Vulkan HW_RENDER before loading the game. If the frontend
  // accepts, paraLLEl-RDP will be initialized later via context_reset
  // with the frontend's instance/device, and we'll skip CPU readback.
  hw_render_cb = {};
  hw_render_cb.context_type    = RETRO_HW_CONTEXT_VULKAN;
  hw_render_cb.version_major   = VK_API_VERSION_MAJOR(VK_API_VERSION_1_1);
  hw_render_cb.version_minor   = VK_API_VERSION_MINOR(VK_API_VERSION_1_1);
  hw_render_cb.context_reset   = hw_context_reset;
  hw_render_cb.context_destroy = hw_context_destroy;
  hw_render_cb.cache_context   = true;
  hw_render_requested = environ_cb && environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render_cb);
  // Defensive: some frontends accept SET_HW_RENDER but then overwrite the
  // requested context_type to OpenGL. Only proceed with Vulkan negotiation
  // if the frontend left our requested type intact.
  if(hw_render_requested && hw_render_cb.context_type != RETRO_HW_CONTEXT_VULKAN) {
    if(log_cb) log_cb(RETRO_LOG_WARN,
      "ares: frontend accepted SET_HW_RENDER but switched context_type to %u; "
      "skipping Vulkan negotiation and falling back to CPU video output\n",
      (unsigned)hw_render_cb.context_type);
    hw_render_requested = false;
  }
  if(hw_render_requested) {
    environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE,
               (void*)&vk_negotiation);
    if(log_cb) log_cb(RETRO_LOG_INFO, "ares: Vulkan HW_RENDER negotiated with frontend\n");
  } else {
    if(log_cb) log_cb(RETRO_LOG_INFO, "ares: using CPU video output (no Vulkan HW_RENDER)\n");
  }

  loadPipelineCache();
  return program.load(string{game->path});
}

RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t) {
  return false;
}

RETRO_API void retro_unload_game(void) {
  // Flush any pending save-dirty state before tearing down. If the player
  // saves in-game and exits within the cooldown window (15 emulated
  // frames), retro_run's falling-edge detector never fires and the
  // frontend's disk-side mirror stays stale. Force the callback once if
  // we have unflushed dirty bits.
  if(save_updated_cb && program.loaded) {
    auto& cart = ares::Nintendo64::cartridge;
    if(cart.saveDirtyState != 0) {
      cart.saveDirtyState = 0;
      save_updated_cb(nullptr);
    }
  }
  program.unload();
  savePipelineCache();
}

RETRO_API unsigned retro_get_region(void) {
  return program.videoFrequency == 1 ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned id) {
  switch(id) {
    case RETRO_MEMORY_SAVE_RAM:   return saveRegions[0].data;
    case RETRO_MEMORY_RTC:        return saveRegions[3].data;
    case RETRO_MEMORY_SYSTEM_RAM: return program.loaded ? ares::Nintendo64::rdram.ram.data : nullptr;
    default: return nullptr;
  }
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
  switch(id) {
    case RETRO_MEMORY_SAVE_RAM:   return saveRegions[0].size;
    case RETRO_MEMORY_RTC:        return saveRegions[3].size;
    case RETRO_MEMORY_SYSTEM_RAM: return program.loaded ? ares::Nintendo64::rdram.ram.size : 0;
    default: return 0;
  }
}
