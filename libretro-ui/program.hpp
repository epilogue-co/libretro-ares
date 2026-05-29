// SPDX-License-Identifier: ISC
// © 2026 Epilogue (epilogue.co)

#pragma once

#include <ares/ares.hpp>
#include <mia/mia.hpp>
#include <nall/vfs.hpp>

#include <unordered_map>

#include "libretro.h"

struct Program : ares::Platform {
  auto attach(ares::Node::Object) -> void override;
  auto detach(ares::Node::Object) -> void override;
  auto pak(ares::Node::Object) -> std::shared_ptr<vfs::directory> override;
  auto event(ares::Event) -> void override;
  auto log(ares::Node::Debugger::Tracer::Tracer, string_view) -> void override;
  auto status(string_view) -> void override;
  auto video(ares::Node::Video::Screen, const u32* data, u32 pitch, u32 width, u32 height) -> void override;
  auto refreshRateHint(double) -> void override;
  auto audio(ares::Node::Audio::Stream) -> void override;
  auto input(ares::Node::Input::Input) -> void override;
  auto cheat(u32 address) -> nall::maybe<u32> override {
    if(activeCheats.empty()) return nall::nothing;
    auto it = activeCheats.find(address);
    if(it == activeCheats.end()) return nall::nothing;
    return it->second;
  }

  auto load(const string& filename) -> bool;
  auto unload() -> void;
  auto runFrame() -> void;
  auto reset() -> void;
  auto serializeSize() -> size_t;
  auto serialize(void* data, size_t size) -> bool;
  auto unserialize(const void* data, size_t size) -> bool;

  std::shared_ptr<mia::Pak> gamePak;
  std::shared_ptr<mia::Pak> systemPak;
  std::shared_ptr<vfs::directory> controllerPak[4];
  nall::string controllerPakPath[4];
  ares::Node::System root;

  std::vector<uint8_t> saveShadow;
  uint8_t* saveSwizzledCore = nullptr;
  std::vector<ares::Node::Video::Screen> screens;
  std::vector<ares::Node::Audio::Stream> streams;

  bool loaded = false;
  bool shutdownRequested = false;
  double refreshRate = 60.0;
  u32 videoFrequency = 0;

  // Dimensions of the last real frame, replayed when emitting a NULL dupe
  // so the frontend's cached frame metadata stays consistent.
  u32 lastVideoWidth = 0;
  u32 lastVideoHeight = 0;
  u32 lastVideoPitch = 0;

  std::unordered_map<uint32_t, uint32_t> activeCheats;

  auto applyCheat(const char* code) -> void;

  std::vector<int16_t> audioBatch;

  struct PortState {
    bool connected = false;
    i16 buttons[16] = {};
    i16 analogX = 0;
    i16 analogY = 0;
    i16 analogCX = 0;
    i16 analogCY = 0;
  } portState[4];

  auto pollInputs() -> void;
  auto flushAudio() -> void;

private:
  auto videoOptionsFromCore() -> void;
  auto registerSaveMemory() -> void;
};

extern Program program;

extern retro_environment_t environ_cb;
extern retro_video_refresh_t video_cb;
extern retro_audio_sample_t audio_sample_cb;
extern retro_audio_sample_batch_t audio_batch_cb;
extern retro_input_poll_t input_poll_cb;
extern retro_input_state_t input_state_cb;
extern retro_log_printf_t log_cb;
extern retro_set_rumble_state_t rumble_cb;

struct SaveRegion {
  void* data = nullptr;
  size_t size = 0;
};
extern SaveRegion saveRegions[5];
