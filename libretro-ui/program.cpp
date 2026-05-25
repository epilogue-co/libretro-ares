// SPDX-License-Identifier: ISC
// © 2026 Epilogue (epilogue.co)

#include "program.hpp"

#include <n64/n64.hpp>

#define VK_NO_PROTOTYPES
#include "libretro_vulkan.h"

#include <cstdlib>
#include <cstring>
#include <vector>

extern const retro_hw_render_interface_vulkan* vulkan_iface;

Program program;
SaveRegion saveRegions[5] = {};

namespace {
  auto portOf(ares::Node::Object node) -> int {
    auto current = node;
    while(current) {
      auto parent = current->parent().lock();
      if(!parent) break;
      string name = parent->name();
      if(name == "Controller Port 1") return 0;
      if(name == "Controller Port 2") return 1;
      if(name == "Controller Port 3") return 2;
      if(name == "Controller Port 4") return 3;
      current = parent;
    }
    return -1;
  }
}

auto Program::attach(ares::Node::Object node) -> void {
  if(auto screen = node->cast<ares::Node::Video::Screen>()) {
    if(root) screens = root->find<ares::Node::Video::Screen>();
  }
  if(auto stream = node->cast<ares::Node::Audio::Stream>()) {
    if(root) streams = root->find<ares::Node::Audio::Stream>();
    stream->setResamplerFrequency(44100.0);
  }
}

auto Program::detach(ares::Node::Object node) -> void {
  if(node->cast<ares::Node::Video::Screen>()) {
    if(root) screens = root->find<ares::Node::Video::Screen>();
  }
  if(node->cast<ares::Node::Audio::Stream>()) {
    if(root) streams = root->find<ares::Node::Audio::Stream>();
  }
}

auto Program::pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> {
  if(!node) return {};
  string name = node->name();
  if(name == "Nintendo 64") return systemPak ? systemPak->pak : nullptr;
  if(name == "Nintendo 64 Cartridge") return gamePak ? gamePak->pak : nullptr;
  return {};
}

auto Program::event(ares::Event event) -> void {
  if(event == ares::Event::Shutdown) {
    shutdownRequested = true;
  }
}

auto Program::log(ares::Node::Debugger::Tracer::Tracer, string_view message) -> void {
  if(log_cb) log_cb(RETRO_LOG_DEBUG, "%.*s", (int)message.size(), message.data());
}

auto Program::status(string_view message) -> void {
  if(log_cb) log_cb(RETRO_LOG_INFO, "%.*s\n", (int)message.size(), message.data());
}

// Per-sync-index ring of retro_vulkan_image descriptors. The frontend stores
// a raw pointer to the struct we hand to set_image and re-reads it on duped
// frames (RetroArch gfx/drivers/vulkan.c:vk->hw.image), so a stack-local
// struct UAFs the moment Program::video returns. Sized to the sync mask and
// indexed by get_sync_index — pattern used by beetle-psx-hw and parallel-n64.
static std::vector<retro_vulkan_image> vulkanFrameRing;

static void ensureVulkanFrameRing() {
  if(!vulkan_iface) return;
  uint32_t mask = vulkan_iface->get_sync_index_mask(vulkan_iface->handle);
  uint32_t n = 0;
  for(uint32_t i = 0; i < 32; i++) if(mask & (1u << i)) n = i + 1;
  if(n == 0) n = 1;
  if(vulkanFrameRing.size() < n) vulkanFrameRing.resize(n);
}

auto Program::video(ares::Node::Video::Screen node, const u32* data, u32 pitch, u32 width, u32 height) -> void {
  if(!video_cb) return;
  // HW_RENDER (Vulkan): hand the rendered VkImage to the frontend instead
  // of copying back through a CPU buffer. Signaled to the frontend via the
  // RETRO_HW_FRAME_BUFFER_VALID sentinel passed in place of `data`.
  if(vulkan_iface && ares::Nintendo64::vulkan.hwRenderActive
      && ares::Nintendo64::vulkan.lastImage != VK_NULL_HANDLE) {
    ensureVulkanFrameRing();
    uint32_t idx = vulkan_iface->get_sync_index(vulkan_iface->handle);
    if(idx >= vulkanFrameRing.size()) idx = idx % vulkanFrameRing.size();
    auto& image = vulkanFrameRing[idx];
    image = {};
    image.image_view  = ares::Nintendo64::vulkan.lastImageView;
    image.image_layout = ares::Nintendo64::vulkan.lastImageLayout;
    image.create_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    image.create_info.image    = ares::Nintendo64::vulkan.lastImage;
    image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    image.create_info.format   = ares::Nintendo64::vulkan.lastImageFormat;
    image.create_info.components = {
      VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
      VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A
    };
    image.create_info.subresourceRange = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
    };
    vulkan_iface->set_image(vulkan_iface->handle, &image, 0, nullptr,
                            VK_QUEUE_FAMILY_IGNORED);
    video_cb(RETRO_HW_FRAME_BUFFER_VALID,
             ares::Nintendo64::vulkan.lastImageWidth,
             ares::Nintendo64::vulkan.lastImageHeight, 0);
    return;
  }
  video_cb(data, width, height, pitch);
}

auto Program::refreshRateHint(double rate) -> void {
  refreshRate = rate;
}

auto Program::audio(ares::Node::Audio::Stream) -> void {
  if(streams.empty()) return;

  auto clamp = [](f64 v) -> int16_t {
    if(v < -1.0) v = -1.0;
    if(v > +1.0) v = +1.0;
    return (int16_t)(v * 32767.0);
  };

  while(true) {
    for(auto& stream : streams) {
      if(!stream->pending()) return;
    }
    f64 mix[2] = {0.0, 0.0};
    for(auto& stream : streams) {
      f64 buffer[2];
      u32 channels = stream->read(buffer);
      if(channels == 1) {
        mix[0] += buffer[0];
        mix[1] += buffer[0];
      } else {
        mix[0] += buffer[0];
        mix[1] += buffer[1];
      }
    }
    audioBatch.push_back(clamp(mix[0]));
    audioBatch.push_back(clamp(mix[1]));
  }
}

auto Program::applyCheat(const char* code) -> void {
  if(!code) return;
  auto parseHex = [](const char* s, size_t n, uint32_t& out) -> bool {
    out = 0;
    for(size_t i = 0; i < n; i++) {
      char c = s[i];
      uint32_t d;
      if(c >= '0' && c <= '9') d = c - '0';
      else if(c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if(c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return false;
      out = (out << 4) | d;
    }
    return true;
  };

  const char* p = code;
  while(*p) {
    while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '+') p++;
    if(!*p) break;
    const char* lineStart = p;
    while(*p && *p != '\n' && *p != '\r' && *p != '+') p++;
    size_t lineLen = (size_t)(p - lineStart);
    if(lineLen < 13) continue;
    uint32_t address = 0, value = 0;
    if(!parseHex(lineStart, 8, address)) continue;
    if(lineStart[8] != ' ' && lineStart[8] != '\t') continue;
    if(!parseHex(lineStart + 9, 4, value)) continue;
    activeCheats[address] = value;
  }
}

auto Program::flushAudio() -> void {
  if(audioBatch.empty()) return;
  if(audio_batch_cb) audio_batch_cb(audioBatch.data(), audioBatch.size() / 2);
  audioBatch.clear();
}

auto Program::input(ares::Node::Input::Input node) -> void {
  int port = portOf(node);
  if(port < 0 || port >= 4) return;
  if(!portState[port].connected) return;

  string name = node->name();
  auto& s = portState[port];

  if(auto button = node->cast<ares::Node::Input::Button>()) {
    bool value = false;
    if(name == "A")        value = s.buttons[RETRO_DEVICE_ID_JOYPAD_A];
    else if(name == "B")   value = s.buttons[RETRO_DEVICE_ID_JOYPAD_B];
    else if(name == "Z")   value = s.buttons[RETRO_DEVICE_ID_JOYPAD_L2];
    else if(name == "Start") value = s.buttons[RETRO_DEVICE_ID_JOYPAD_START];
    else if(name == "L")   value = s.buttons[RETRO_DEVICE_ID_JOYPAD_L];
    else if(name == "R")   value = s.buttons[RETRO_DEVICE_ID_JOYPAD_R];
    else if(name == "Up")    value = s.buttons[RETRO_DEVICE_ID_JOYPAD_UP];
    else if(name == "Down")  value = s.buttons[RETRO_DEVICE_ID_JOYPAD_DOWN];
    else if(name == "Left")  value = s.buttons[RETRO_DEVICE_ID_JOYPAD_LEFT];
    else if(name == "Right") value = s.buttons[RETRO_DEVICE_ID_JOYPAD_RIGHT];
    else if(name == "C-Up")    value = s.analogCY < -16000 || s.buttons[RETRO_DEVICE_ID_JOYPAD_X];
    else if(name == "C-Down")  value = s.analogCY > +16000 || s.buttons[RETRO_DEVICE_ID_JOYPAD_SELECT];
    else if(name == "C-Left")  value = s.analogCX < -16000 || s.buttons[RETRO_DEVICE_ID_JOYPAD_Y];
    else if(name == "C-Right") value = s.analogCX > +16000 || s.buttons[RETRO_DEVICE_ID_JOYPAD_R2];
    button->setValue(value);
    return;
  }

  if(auto axis = node->cast<ares::Node::Input::Axis>()) {
    if(name == "X-Axis") axis->setValue(s.analogX);
    else if(name == "Y-Axis") axis->setValue(s.analogY);
    return;
  }

  if(auto motor = node->cast<ares::Node::Input::Rumble>()) {
    if(rumble_cb) {
      uint16_t strength = motor->enable() ? 0xFFFF : 0;
      rumble_cb((unsigned)port, RETRO_RUMBLE_STRONG, strength);
      rumble_cb((unsigned)port, RETRO_RUMBLE_WEAK, strength);
    }
    return;
  }
}

auto Program::pollInputs() -> void {
  if(!input_poll_cb || !input_state_cb) return;
  input_poll_cb();
  for(int port = 0; port < 4; port++) {
    auto& s = portState[port];
    for(int id = 0; id < 16; id++) {
      s.buttons[id] = input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id);
    }
    s.analogX = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    s.analogY = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    s.analogCX = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
    s.analogCY = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
  }
}

auto Program::videoOptionsFromCore() -> void {
  auto getVar = [](const char* key) -> string {
    retro_variable var{key, nullptr};
    if(environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      return string{var.value};
    }
    return {};
  };

  auto enabled = [&](const char* key) { return getVar(key) == "enabled"; };

  if(auto v = getVar("ares_n64_quality")) ares::Nintendo64::option("Quality", v);
  ares::Nintendo64::option("Supersampling", enabled("ares_n64_supersampling"));
  ares::Nintendo64::option("Disable Video Interface Processing", !enabled("ares_n64_vi_processing"));
  ares::Nintendo64::option("Weave Deinterlacing", enabled("ares_n64_weave_deinterlacing"));
  ares::Nintendo64::option("Recompiler", enabled("ares_n64_recompiler"));
  ares::Nintendo64::option("Expansion Pak", enabled("ares_n64_expansion_pak"));
  ares::Nintendo64::option("Homebrew Mode", enabled("ares_n64_homebrew_mode"));
  ares::Nintendo64::vulkan.framePersistence = enabled("ares_n64_frame_persistence");

  setenv("PARALLEL_RDP_UBERSHADER", getVar("ares_n64_renderer") == "specialized" ? "0" : "1", 1);

#if defined(VULKAN)
  ares::Nintendo64::option("Enable GPU acceleration", true);
#else
  ares::Nintendo64::option("Enable GPU acceleration", false);
#endif
}

auto Program::registerSaveMemory() -> void {
  for(auto& r : saveRegions) r = {};
  if(!root) return;

  auto& cart = ares::Nintendo64::cartridge;
  if(cart.ram.size > 0) {
    saveRegions[0] = {cart.ram.data, cart.ram.size};
  } else if(cart.eeprom.size > 0) {
    saveRegions[0] = {cart.eeprom.data, cart.eeprom.size};
  } else if(cart.flash.size > 0) {
    saveRegions[0] = {cart.flash.data, cart.flash.size};
  }
  if(cart.rtc.ram.size > 0) {
    saveRegions[3] = {cart.rtc.ram.data, cart.rtc.ram.size};
  }
}

auto Program::load(const string& filename) -> bool {
  auto err = [](const char* fmt, auto... args) {
    if(log_cb) log_cb(RETRO_LOG_ERROR, fmt, args...);
  };

  auto systems = mia::identify(filename);
  bool isN64 = false;
  for(auto& s : systems) if(s == "Nintendo 64") { isN64 = true; break; }
  if(!isN64) {
    err("[ares] ROM not identified as Nintendo 64: %s\n", filename.data());
    return false;
  }

  ares::platform = this;

  gamePak = mia::Medium::create("Nintendo 64");
  if(!gamePak) { err("[ares] mia::Medium::create returned null\n"); return false; }
  auto result = gamePak->load(filename);
  if(result != ::successful) {
    err("[ares] mia game load failed (%d)\n", (int)result.result);
    return false;
  }

  systemPak = mia::System::create("Nintendo 64");
  if(!systemPak) { err("[ares] mia::System::create returned null\n"); return false; }
  result = systemPak->load();
  if(result != ::successful) {
    err("[ares] mia system load failed (%d)\n", (int)result.result);
    return false;
  }

  videoOptionsFromCore();

  string region = gamePak->pak->attribute("region");
  if(region.find("PAL")) region = "PAL"; else region = "NTSC";
  string systemName = {"[Nintendo] Nintendo 64 (", region, ")"};

  if(!ares::Nintendo64::load(root, systemName)) {
    err("[ares] ares::Nintendo64::load returned false for %s\n", systemName.data());
    return false;
  }
  if(!root) { err("[ares] ares::Nintendo64::load produced a null root node\n"); return false; }

  if(auto port = root->find<ares::Node::Port>("Cartridge Slot")) {
    port->allocate();
    port->connect();
  } else {
    err("[ares] Cartridge Slot port not found on root\n");
  }

  for(int id = 0; id < 4; id++) {
    string portName = {"Controller Port ", id + 1};
    if(auto port = root->find<ares::Node::Port>(portName)) {
      auto peripheral = port->allocate("Gamepad");
      port->connect();
      portState[id].connected = true;

      if(peripheral) {
        if(auto pakPort = peripheral->find<ares::Node::Port>("Pak")) {
          if(gamePak->pak->attribute({"port", id + 1, "/rpak"}).boolean()) {
            pakPort->allocate("Rumble Pak");
            pakPort->connect();
          }
        }
      }
    }
  }

  root->power();

  registerSaveMemory();
  loaded = true;

  switch(ares::Nintendo64::system.region()) {
    case ares::Nintendo64::System::Region::NTSC:
      videoFrequency = 0;
      refreshRate = 60.0;
      break;
    case ares::Nintendo64::System::Region::PAL:
      videoFrequency = 1;
      refreshRate = 50.0;
      break;
  }
  return true;
}

auto Program::unload() -> void {
  if(root) {
    root->unload();
    root.reset();
  }
  gamePak.reset();
  systemPak.reset();
  screens.clear();
  streams.clear();
  loaded = false;
  shutdownRequested = false;
  for(auto& s : portState) s = {};
  for(auto& r : saveRegions) r = {};
  audioBatch.clear();
}

auto Program::runFrame() -> void {
  if(!loaded || !root) return;
  pollInputs();
  root->run();
  flushAudio();
}

auto Program::reset() -> void {
  if(!root) return;
  root->power(true);
}

auto Program::serializeSize() -> size_t {
  if(!root) return 0;
  serializer s = root->serialize(false);
  return s.size();
}

auto Program::serialize(void* data, size_t size) -> bool {
  if(!root) return false;
  serializer s = root->serialize(false);
  if(s.size() > size) return false;
  std::memcpy(data, s.data(), s.size());
  return true;
}

auto Program::unserialize(const void* data, size_t size) -> bool {
  if(!root) return false;
  serializer s{(const uint8_t*)data, (u32)size};
  return root->unserialize(s);
}
