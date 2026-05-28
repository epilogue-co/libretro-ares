/* Copyright (c) 2017-2023 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "logging.hpp"

#include <atomic>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Util
{
// Built into a shared object (ares_libretro.so). LTO marks this file-static
// thread_local hidden and, on x86_64 ELF, downgrades it to the local-exec TLS
// model (R_X86_64_TPOFF32) — illegal in a shared object, so ld.bfd rejects the
// link. Force global-dynamic on ELF targets; this is a cold logging path, so
// the __tls_get_addr indirection costs nothing measurable. (Windows PE and
// macOS Mach-O use different TLS mechanisms and link fine, so leave them be.)
#if defined(__ELF__)
static thread_local LoggingInterface *logging_iface __attribute__((tls_model("global-dynamic")));
#else
static thread_local LoggingInterface *logging_iface;
#endif
static std::atomic<LoggingInterface *> process_logging_iface{nullptr};

bool interface_log(const char *tag, const char *fmt, ...)
{
	LoggingInterface *iface = logging_iface;
	if (!iface)
		iface = process_logging_iface.load(std::memory_order_acquire);
	if (!iface)
		return false;

	va_list va;
	va_start(va, fmt);
	bool ret = iface->log(tag, fmt, va);
	va_end(va);
	return ret;
}

void set_thread_logging_interface(LoggingInterface *iface)
{
	logging_iface = iface;
}

void set_process_logging_interface(LoggingInterface *iface)
{
	process_logging_iface.store(iface, std::memory_order_release);
}

#ifdef _WIN32
void debug_output_log(const char *tag, const char *fmt, ...)
{
	if (!IsDebuggerPresent())
		return;

	va_list va;
	va_start(va, fmt);
	auto len = vsnprintf(nullptr, 0, fmt, va);
	if (len > 0)
	{
		size_t tag_len = strlen(tag);
		char *buf = new char[len + tag_len + 1];
		memcpy(buf, tag, tag_len);
		vsnprintf(buf + tag_len, len + 1, fmt, va);
		OutputDebugStringA(buf);
		delete[] buf;
	}
	va_end(va);
}
#endif
}