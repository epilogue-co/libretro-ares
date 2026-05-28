// SPDX-License-Identifier: ISC
// © 2026 Epilogue (epilogue.co)

// nall's main.cpp (linked via the nall object library) defines
// nall::main(int, char**), which calls the application-provided
// nall::main(Arguments). The libretro core is a shared library with no
// application entry point and never defines that overload, so the symbol is
// left undefined. Unix shared objects tolerate the dead, never-called
// reference; a Windows DLL link (ld.lld) rejects any undefined symbol. Provide
// an unused stub — the DLL has no nall OS entry that could ever invoke it.
#include <nall/arguments.hpp>

namespace nall {
auto main(Arguments) -> void {}
}
