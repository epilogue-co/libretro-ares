// SPDX-License-Identifier: ISC
// © 2026 Epilogue (epilogue.co)

#include "program.hpp"
#include "libretro_core_options.h"

#include <n64/n64.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

retro_environment_t  environ_cb         = nullptr;
retro_video_refresh_t video_cb          = nullptr;
retro_audio_sample_t  audio_sample_cb   = nullptr;
retro_audio_sample_batch_t audio_batch_cb = nullptr;
retro_input_poll_t   input_poll_cb      = nullptr;
retro_input_state_t  input_state_cb     = nullptr;
retro_log_printf_t   log_cb             = nullptr;
retro_set_rumble_state_t rumble_cb      = nullptr;

namespace {
  void fallback_log(retro_log_level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
  }

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
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Z"},                                    \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start"},                                \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L"},                                    \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R"},                                    \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "C-Up"},                                 \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "C-Down"},                               \
    {p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "C-Left"},                               \
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
}

RETRO_API size_t retro_serialize_size(void) {
  return program.serializeSize();
}

RETRO_API bool retro_serialize(void* data, size_t size) {
  return program.serialize(data, size);
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
  return program.unserialize(data, size);
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
  loadPipelineCache();
  return program.load(string{game->path});
}

RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t) {
  return false;
}

RETRO_API void retro_unload_game(void) {
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
