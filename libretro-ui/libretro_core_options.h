// SPDX-License-Identifier: ISC
// © 2026 Epilogue (epilogue.co)

#pragma once

#include "libretro.h"

static const retro_core_option_v2_category option_cats[] = {
  {"video",    "Video",    "GPU rendering and upscaling"},
  {"cpu",      "CPU",      "Recompiler vs interpreter"},
  {"hardware", "Hardware", "Console hardware options"},
  {nullptr, nullptr, nullptr},
};

static const retro_core_option_v2_definition option_defs[] = {
  {
    "ares_n64_quality",
    "Internal Resolution", nullptr,
    "Renders the N64 framebuffer at a multiple of native resolution. Higher values are sharper but cost GPU performance.",
    nullptr, "video",
    {{"SD", "1x (320x240)"}, {"HD", "2x (640x480)"}, {"UHD", "4x (1280x960)"}, {nullptr, nullptr}},
    "SD"
  },
  {
    "ares_n64_supersampling",
    "Supersampling", nullptr,
    "Supersample the scanout to reduce aliasing. Requires Internal Resolution above 1x.",
    nullptr, "video",
    {{"disabled", "Off"}, {"enabled", "On"}, {nullptr, nullptr}},
    "disabled"
  },
  {
    "ares_n64_vi_processing",
    "VI Processing", nullptr,
    "Run the N64 Video Interface filtering pipeline (anti-alias, divot, dither). Disabling is faster but less accurate.",
    nullptr, "video",
    {{"enabled", "On"}, {"disabled", "Off"}, {nullptr, nullptr}},
    "enabled"
  },
  {
    "ares_n64_weave_deinterlacing",
    "Weave Deinterlacing", nullptr,
    "Interlaced output uses weave instead of bob. Most games are non-interlaced; safe to leave off.",
    nullptr, "video",
    {{"disabled", "Off"}, {"enabled", "On"}, {nullptr, nullptr}},
    "disabled"
  },
  {
    "ares_n64_recompiler",
    "CPU Recompiler", nullptr,
    "Use JIT recompilation for the R4300 CPU. Disable to fall back to interpreter (slower, edge-case accuracy).",
    nullptr, "cpu",
    {{"enabled", "Enabled"}, {"disabled", "Disabled"}, {nullptr, nullptr}},
    "enabled"
  },
  {
    "ares_n64_expansion_pak",
    "Expansion Pak", nullptr,
    "Enables the 4MB Expansion Pak. Required for some titles (Donkey Kong 64, Majora's Mask, Perfect Dark).",
    nullptr, "hardware",
    {{"enabled", "On"}, {"disabled", "Off"}, {nullptr, nullptr}},
    "enabled"
  },
  {
    "ares_n64_homebrew_mode",
    "Homebrew Mode", nullptr,
    "Relax certain accuracy checks for homebrew/decomp ROMs that rely on non-retail-cartridge timing.",
    nullptr, "hardware",
    {{"disabled", "Off"}, {"enabled", "On"}, {nullptr, nullptr}},
    "disabled"
  },
  {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr},
};

static const retro_core_options_v2 options_us = {
  (retro_core_option_v2_category*)option_cats,
  (retro_core_option_v2_definition*)option_defs,
};
