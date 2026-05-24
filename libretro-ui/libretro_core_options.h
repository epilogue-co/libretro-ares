// SPDX-License-Identifier: ISC
// © 2026 Epilogue (epilogue.co)

#pragma once

#include "libretro.h"

static const retro_core_option_v2_category option_cats[] = {
  {"performance", "Performance", "Trade emulation accuracy for gameplay smoothness"},
  {"video",       "Video",       "GPU rendering and upscaling"},
  {"cpu",         "CPU",         "Recompiler vs interpreter"},
  {"hardware",    "Hardware",    "Console hardware options"},
  {nullptr, nullptr, nullptr},
};

static const retro_core_option_v2_definition option_defs[] = {
  {
    "ares_n64_performance_mode",
    "Performance Mode", nullptr,
    "Accurate: full cycle-accurate emulation (default). Balanced: relaxes CPU↔GPU synchronization for ~10% gain; a small number of games (Resident Evil 2, Jet Force Gemini, Pokémon Snap, Body Harvest) may show visual glitches. Performance: above plus reduced VI filtering for ~20% gain at the cost of softer post-processing.",
    nullptr, "performance",
    {{"accurate", "Accurate"}, {"balanced", "Balanced"}, {"performance", "Performance"}, {nullptr, nullptr}},
    "accurate"
  },
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
    "ares_n64_frame_persistence",
    "Motion Blur", nullptr,
    "Blends each scanout with the previous one, approximating CRT phosphor persistence. Smooths motion at the cost of some ghosting.",
    nullptr, "video",
    {{"enabled", "On"}, {"disabled", "Off"}, {nullptr, nullptr}},
    "enabled"
  },
  {
    "ares_n64_renderer",
    "Renderer", nullptr,
    "Ubershader: one shader handles every RDP state — no compile stalls, modest steady-state GPU cost. Specialized: faster steady-state but pays compile cost on first encounter of each state combination (cached across sessions). Takes effect on next game load.",
    nullptr, "video",
    {{"ubershader", "Ubershader"}, {"specialized", "Specialized"}, {nullptr, nullptr}},
    "ubershader"
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
