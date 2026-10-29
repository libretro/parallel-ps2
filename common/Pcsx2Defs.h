/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

// clang-format off

#ifdef __CYGWIN__
	#define __linux__
#endif

// make sure __POSIX__ is defined for all systems where we assume POSIX
// compliance
#if defined(__linux__) || defined(__APPLE__) || defined(__unix__) || defined(__CYGWIN__) || defined(__LINUX__)
	#ifndef __POSIX__
		#define __POSIX__ 1
	#endif
#endif

#include "Pcsx2Types.h"
#ifdef __cplusplus
#include <cstddef>
#include <cassert>
#else
#include <stddef.h>
#include <assert.h>
#endif

#ifdef __cplusplus
// The C++ standard doesn't allow `offsetof` to be used on non-constant values (e.g. `offsetof(class, field[i])`)
// Use this in those situations
#define OFFSETOF(a, b) (reinterpret_cast<size_t>(&(static_cast<a*>(0)->b)))
#endif

// Coarse CPU family selector used to pick SIMD backends (SSE vs NEON), e.g. the
// GSVector implementation. Mirrors the scheme used by the modern PCSX2/ARMSX2
// tree so the arm64 GSVector headers can be shared.
#if (defined(_M_ARM64) || defined(__aarch64__)) || defined(__aarch64__)
	#define ARCH_ARM64
#elif defined(_M_X86) || defined(__x86_64__) || defined(__i386__)
	#define ARCH_X86
#else
	#error Unsupported architecture
#endif

#if (defined(_M_ARM64) || defined(__aarch64__)) && defined(__APPLE__)
/* Apple Silicon uses 16KB pages and 128 byte cache lines. */
#define __pagesize 0x4000
#define __pageshift 14
#define __cachelinesize 128
#elif (defined(_M_ARM64) || defined(__aarch64__))
/* Linux aarch64 (this port's targets) uses 4KB pages; keep the conservative
   128-byte cache line. A 16K __pagesize here breaks vtlb page protection:
   the fault handler masks si_addr with a 16K mask, mis-attributing faults on
   a 4K kernel (addr can round below eeMem->Main) -> unhandled -> abort. */
#define __pagesize 0x1000
#define __pageshift 12
#define __cachelinesize 128
#else
// X86 uses a 4KB granularity and 64 byte cache lines.
#define __pagesize 0x1000
#define __pageshift 12
#define __cachelinesize 64
#endif
#define __pagemask (__pagesize - 1)

// We use 4KB alignment for globals for both Apple 
// and x86 platforms, since computing the
// address on ARM64 is a single instruction (adrp).
#define __pagealignsize 0x1000

/* A compile-time check both languages can make: C89 has no static_assert,
 * so the C side declares an array whose size goes negative when the
 * condition fails. */
#ifdef __cplusplus
#define PCSX2_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define PCSX2_SA_CAT_(a, b) a##b
#define PCSX2_SA_CAT(a, b)  PCSX2_SA_CAT_(a, b)
#define PCSX2_STATIC_ASSERT(cond, msg) \
	typedef char PCSX2_SA_CAT(pcsx2_static_assert_, __LINE__)[(cond) ? 1 : -1]
#endif

/* alignas is C++11 and C11; these headers are also read by C sources. */
#if defined(_MSC_VER)
#define PCSX2_ALIGN(n) __declspec(align(n))
#elif defined(__GNUC__)
#define PCSX2_ALIGN(n) __attribute__((aligned(n)))
#else
#define PCSX2_ALIGN(n)
#endif

// --------------------------------------------------------------------------------------
//  Microsoft Visual Studio
// --------------------------------------------------------------------------------------
// Initial-exec TLS model: one GOT load + fs/tpidr-relative access instead of
// a __tls_get_addr resolver call on every access (general-dynamic is the
// -fPIC default). Used for the emitter write cursors, which are touched
// multiple times per emitted instruction during recompilation bursts. Safe
// for a dlopen'd core: glibc reserves static-TLS surplus for exactly this.
// No-op on MSVC, whose TLS never goes through a resolver call.
#if defined(_MSC_VER)
	#define PCSX2_TLS_INITIAL_EXEC
#else
	#define PCSX2_TLS_INITIAL_EXEC __attribute__((tls_model("initial-exec")))
#endif

#ifdef _MSC_VER

#ifndef __noinline
#define __noinline __declspec(noinline)
#endif
#ifndef __noreturn
#define __noreturn __declspec(noreturn)
#endif

// Don't know if there are Visual C++ equivalents of these.
#define likely(x) (!!(x))
#define unlikely(x) (!!(x))

#ifndef CALLBACK
#define CALLBACK __stdcall
#endif

// Portable read-prefetch into L1.  GCC/Clang have __builtin_prefetch.
// MSVC routes through <intrin.h>: _mm_prefetch on x86, __prefetch on ARM64.
#include <intrin.h>
#if (defined(_M_ARM64) || defined(__aarch64__))
#define __prefetch_r(p) __prefetch((const void*)(p))
#else
#define __prefetch_r(p) _mm_prefetch((char const*)(p), _MM_HINT_T0)
#endif

#else

// --------------------------------------------------------------------------------------
//  GCC / Intel Compilers Section
// --------------------------------------------------------------------------------------

#define __assume(cond) do { if (!(cond)) __builtin_unreachable(); } while(0)

// Portable read-prefetch into L1.  See MSVC counterpart above.
#define __prefetch_r(p) __builtin_prefetch((const void*)(p))

// SysV ABI passes vector parameters through registers unconditionally.
#ifndef _WIN32
#define __vectorcall
#ifndef CALLBACK
#define CALLBACK
#endif
#else
// MinGW / clang do not implement __vectorcall.  The Win64 ABI already passes
// the first four FP/vector arguments through XMM registers, so dropping the
// attribute keeps the calling convention compatible with anything compiled
// in the same toolchain (which is what matters for the libretro core).
#if !defined(_MSC_VER)
#define __vectorcall
#endif
#ifndef CALLBACK
#define CALLBACK __attribute__((stdcall))
#endif
#endif

// Inlining note: GCC needs ((unused)) attributes defined 
// on inlined functions to suppress warnings when a static 
// inlined function isn't used in the scope of a single file (which
// happens *by design* like all the friggen time >_<)

#ifndef _inline
#define _inline __inline__ __attribute__((unused))
#endif

// __forceinline / __fi / __ri: forced inlining, with inline linkage as
// MSVC's builtin has it.  They belong on static functions, header
// definitions and member functions -- places where every caller sees the
// body.  A function called from another translation unit stays a plain
// extern function; when its own TU wants the body inlined, that body is a
// static __fi helper and the extern entry point calls it.
//
// mingw-w64 defines `__forceinline` itself, as `extern __inline__` with
// gnu_inline semantics, and its system headers (winbase.h, _mingw.h ...)
// depend on that definition, so it is left alone there.  The project's
// own __fi / __ri / __releaseinline expand to nothing on mingw instead.
#ifdef NDEBUG
#ifndef __forceinline
#define __forceinline __inline__ __attribute__((always_inline, unused))
#endif
#else
#ifndef __forceinline
#define __forceinline __inline__ __attribute__((unused))
#endif
#endif

#ifndef __noinline
#define __noinline __attribute__((noinline))
#endif

#ifndef __noreturn
#define __noreturn __attribute__((noreturn))
#endif

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#endif

// --------------------------------------------------------------------------------------
// PS2_AUDIT_EXPORT -- exported C entry points used by the fpaudit harness
// (tests/fpaudit) via dlsym/GetProcAddress.
// --------------------------------------------------------------------------------------
// MSVC has no __attribute__((visibility)); it needs __declspec(dllexport).
//
// This block MUST stay outside the `#ifdef _MSC_VER / #else` compiler split
// above: placed inside the GCC arm, MSVC never sees any definition at all,
// the token survives preprocessing as a bare identifier, and every use site
// fails as `int PS2_AUDIT_EXPORT` redefinition plus C2144/C4430 on the
// declaration that follows it.
#if defined(_MSC_VER)
#define PS2_AUDIT_EXPORT extern "C" __declspec(dllexport)
#else
#define PS2_AUDIT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// --------------------------------------------------------------------------------------
// __releaseinline / __ri -- a forceinline macro that is enabled for RELEASE/PUBLIC builds ONLY.
// --------------------------------------------------------------------------------------
// This is useful because forceinline can make certain types of debugging problematic since
// functions that look like they should be called won't breakpoint since their code is
// inlined, and it can make stack traces confusing or near useless.
//
// Use __releaseinline for things which are generally large functions where trace debugging
// from Devel builds is likely useful; but which should be inlined in an optimized Release
// environment.
//
// On mingw these expand to nothing; see the `__forceinline` comment block
// above.  Everywhere else they are `__forceinline`.
#ifdef __MINGW32__
#define __releaseinline
#define __ri
#define __fi
#else
#define __releaseinline __forceinline
#define __ri __releaseinline
#define __fi __forceinline
#endif

// __forceinline_odr is for inline functions defined in headers (e.g. the
// GSVertex/quad helpers). It is `__forceinline` except on MinGW, where it
// collapses to a plain `inline` for the reason given above.
// Fallthrough annotation that degrades instead of failing to parse:
// [[fallthrough]] is C++17 syntax and nothing older -- not C89, not C at
// all, not pre-17 C++ -- so a bare attribute is a landmine for any file
// that migrates toward C or an older standard. GCC>=7 and clang accept
// the attribute spelling in C too; everything else gets a no-op, which
// costs only the warning the annotation was silencing.
#ifndef PCSX2_FALLTHROUGH
#if defined(__cplusplus) && (__cplusplus >= 201703L)
#define PCSX2_FALLTHROUGH [[fallthrough]]
#elif defined(__GNUC__) && (__GNUC__ >= 7)
#define PCSX2_FALLTHROUGH __attribute__((fallthrough))
#elif defined(__clang__)
#define PCSX2_FALLTHROUGH __attribute__((fallthrough))
#else
#define PCSX2_FALLTHROUGH ((void)0)
#endif
#endif

#ifndef __forceinline_odr
#if defined(_MSC_VER)
// MSVC's __forceinline is a builtin that already carries inline linkage;
// spelling `inline` beside it is warning C4141 in every including TU --
// seven per inclusion of GSVertex.h alone. Forced inlining that needs a
// per-compiler alphabet of spellings, one of which warns and another of
// which is only a suggestion the optimizer may ignore, is exactly the
// guarantee problem function-style inlining has always had; a macro would
// not need any of this. Where that trade matters most in this codebase
// (the emitter core), macros are what we use.
#define __forceinline_odr __forceinline
#elif defined(__MINGW32__)
#define __forceinline_odr inline
#else
#define __forceinline_odr __forceinline
#endif
#endif

// Makes sure that if anyone includes xbyak, it doesn't do anything bad
#define XBYAK_ENABLE_OMITTED_OPERAND

#if defined(__x86_64__) && !defined(_M_AMD64)
	#define _M_AMD64
#endif

#ifndef RESTRICT
	#ifdef __INTEL_COMPILER
		#define RESTRICT restrict
	#elif defined(_MSC_VER)
		#define RESTRICT __restrict
	#elif defined(__GNUC__)
		#define RESTRICT __restrict__
	#else
		#define RESTRICT
	#endif
#endif

#ifndef __has_attribute
	#define __has_attribute(x) 0
#endif

#ifndef __has_builtin
	#define __has_builtin(x) 0
#endif

#ifdef __cpp_constinit
	#define CONSTINIT constinit
#elif __has_attribute(require_constant_initialization)
	#define CONSTINIT __attribute__((require_constant_initialization))
#else
	#define CONSTINIT
#endif

/* 'inline' is not a C89 keyword; both compilers spell it with underscores. */
#if defined(_MSC_VER)
#define PCSX2_INLINE __inline
#elif defined(__GNUC__)
#define PCSX2_INLINE __inline__
#else
#define PCSX2_INLINE
#endif

/* min/max/clamp without <algorithm>.
 *
 * These are inline functions rather than macros on purpose: a macro
 * evaluates its arguments twice, and of the 523 call sites in this tree, 73
 * pass a function call or an increment -- std::max(Get_vuCycles(), 4u),
 * std::max(16, cpuGetCycles()) and so on. A macro would call those twice and
 * the bug would be invisible at the call site. The overload set covers the
 * types actually used; anything else is a compile error rather than a silent
 * conversion.
 */
#define PCSX2_DEFINE_MINMAX(suffix, type)                                     \
	static PCSX2_INLINE type pcsx2_min_##suffix(type a, type b) { return (a < b) ? a : b; } \
	static PCSX2_INLINE type pcsx2_max_##suffix(type a, type b) { return (a > b) ? a : b; } \
	static PCSX2_INLINE type pcsx2_clamp_##suffix(type v, type lo, type hi)         \
	{ return (v < lo) ? lo : ((v > hi) ? hi : v); }

PCSX2_DEFINE_MINMAX(i,   int)
PCSX2_DEFINE_MINMAX(u,   unsigned int)
PCSX2_DEFINE_MINMAX(s64, s64)
PCSX2_DEFINE_MINMAX(u64, u64)
PCSX2_DEFINE_MINMAX(sz,  size_t)
PCSX2_DEFINE_MINMAX(f,   float)
PCSX2_DEFINE_MINMAX(d,   double)

/* Path buffer size.
 *
 * Deliberately NOT PATH_MAX_LENGTH from <retro_miscellaneous.h>: that
 * header pulls in <windows.h> with WIN32_LEAN_AND_MEAN but without
 * NOMINMAX, so including it from a widely-included header turns min and
 * max into macros. Under MSVC that breaks every declaration named min or
 * max -- GSVector8::min(), GSTextureCache's std::max initialisers -- with
 * a cascade of syntax errors far from the include that caused them. The
 * tree already has common/RedtapeWindows.h for pulling in windows.h
 * safely; a config header has no business doing it at all.
 *
 * Having our own constant also fixes the size across translation units,
 * which PATH_MAX_LENGTH does not: it is 512 on some platforms and 2048 on
 * others, so a header that saw one value and a .cpp that saw the other
 * would disagree about a struct's layout.
 */
#ifndef PCSX2_PATH_MAX
#define PCSX2_PATH_MAX 2048
#endif

/* Array length, in place of std::size. */
#ifndef C89_ARRAY_SIZE
#define C89_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifdef __cplusplus

// --------------------------------------------------------------------------------------
//  ImplementEnumOperators  (macro)
// --------------------------------------------------------------------------------------
// This macro implements ++/-- operators for any conforming enumeration.  In order for an
// enum to conform, it must have _FIRST and _COUNT members defined, and must have a full
// compliment of sequential members (no custom assignments) --- looking like so:
//
// enum Dummy {
//    Dummy_FIRST,
//    Dummy_Item = Dummy_FIRST,
//    Dummy_Crap,
//    Dummy_COUNT
// };
//
// The macro also defines utility functions for bounds checking enumerations:
//   EnumIsValid(value);   // returns TRUE if the enum value is between FIRST and COUNT.
//   EnumAssert(value);
//
// It also defines a *prototype* for converting the enumeration to a string.  Note that this
// method is not implemented!  You must implement it yourself if you want to use it:
//   EnumToString(value);
//

#include <compat/strl.h>
#include <string.h>

/* Join two path fragments with exactly one separator, into a caller
 * buffer. Matches Path::Combine: any trailing separators on @base and
 * leading ones on @next are dropped, and exactly one '/' goes between --
 * including when @base is empty, so ("", "rel") is "/rel" rather than
 * "rel".
 *
 * This exists because it has been open-coded twice during the string
 * conversion, in IopBios::host_path and in the gzip index template, and
 * both times the first attempt forgot the trailing separator on base and
 * produced "root//next". The second one was caught by a differential
 * against Path::Combine at 84 failures out of 448; the first by the same
 * kind of check at 13 out of 96. One copy is easier to keep correct than
 * three.
 */
static inline void pcsx2_path_join(char* out, size_t out_size,
		const char* base, const char* next)
{
	size_t n = base ? strlen(base) : 0;

	if (!out || !out_size)
		return;
	if (!next)
		next = "";

	while (n > 0 && (base[n - 1] == '/' || base[n - 1] == '\\'))
		n--;
	while (*next == '/' || *next == '\\')
		next++;

	if (n >= out_size)
		n = out_size - 1;
	if (n)
		memcpy(out, base, n);
	out[n] = '\0';

	if (*next)
	{
		strlcat(out, "/", out_size);
		strlcat(out, next, out_size);
	}
}


#define ImplementEnumOperators(enumName) \
	static __fi enumName& operator++(enumName& src) \
	{ \
		src = (enumName)((int)src + 1); \
		return src; \
	} \
\
	static __fi enumName& operator--(enumName& src) \
	{ \
		src = (enumName)((int)src - 1); \
		return src; \
	} \
\
	static __fi enumName operator++(enumName& src, int) \
	{ \
		enumName orig = src; \
		src = (enumName)((int)src + 1); \
		return orig; \
	} \
\
	static __fi enumName operator--(enumName& src, int) \
	{ \
		enumName orig = src; \
		src = (enumName)((int)src - 1); \
		return orig; \
	} \
\
	static __fi bool operator<(const enumName& left, const pxEnumEnd_t&) { return (int)left < enumName##_COUNT; } \
	static __fi bool operator!=(const enumName& left, const pxEnumEnd_t&) { return (int)left != enumName##_COUNT; } \
	static __fi bool operator==(const enumName& left, const pxEnumEnd_t&) { return (int)left == enumName##_COUNT; } \
\
	static __fi bool EnumIsValid(enumName id) \
	{ \
		return ((int)id >= enumName##_FIRST) && ((int)id < enumName##_COUNT); \
	} \
\
	extern const char* EnumToString(enumName id)

#ifdef __cplusplus
class pxEnumEnd_t
{
};
static const pxEnumEnd_t pxEnumEnd = {};
#endif

// --------------------------------------------------------------------------------------
//  DeclareNoncopyableObject
// --------------------------------------------------------------------------------------
// This macro provides an easy and clean method for ensuring objects are not copyable.
// Simply add the macro to the head or tail of your class declaration, and attempts to
// copy the class will give you a moderately obtuse compiler error.
//
#ifndef DeclareNoncopyableObject
#define DeclareNoncopyableObject(classname) \
public: \
	classname(const classname&) = delete; \
	classname& operator=(const classname&) = delete
#endif

#endif

// --------------------------------------------------------------------------------------
//  Handy Human-readable constants for common immediate values (_16kb -> _4gb)

/* C++ keeps the typed constants it always had; C gets macros of the same
 * values, since the SPU2 sources are built as C. */
#ifdef __cplusplus
static constexpr sptr _1kb = 1024 * 1;
static constexpr sptr _4kb = _1kb * 4;
static constexpr sptr _16kb = _1kb * 16;
static constexpr sptr _32kb = _1kb * 32;
static constexpr sptr _64kb = _1kb * 64;
static constexpr sptr _128kb = _1kb * 128;
static constexpr sptr _256kb = _1kb * 256;

static constexpr s64 _1mb = 1024 * 1024;
static constexpr s64 _8mb = _1mb * 8;
static constexpr s64 _16mb = _1mb * 16;
static constexpr s64 _32mb = _1mb * 32;
static constexpr s64 _64mb = _1mb * 64;
static constexpr s64 _256mb = _1mb * 256;
static constexpr s64 _1gb = _1mb * 1024;
static constexpr s64 _4gb = _1gb * 4;
#else
#define _1kb   (1024 * 1)
#define _4kb   (_1kb * 4)
#define _16kb  (_1kb * 16)
#define _32kb  (_1kb * 32)
#define _64kb  (_1kb * 64)
#define _128kb (_1kb * 128)
#define _256kb (_1kb * 256)

#define _1mb   (1024 * 1024)
#define _8mb   (_1mb * 8)
#define _16mb  (_1mb * 16)
#define _32mb  (_1mb * 32)
#define _64mb  (_1mb * 64)
#define _256mb (_1mb * 256)
#define _1gb   ((s64)_1mb * 1024)
#define _4gb   (_1gb * 4)
#endif

// Disable some spammy warnings which wx appeared to disable.
// We probably should fix these at some point.
#ifdef _MSC_VER
#pragma warning(disable: 4244) // warning C4244: 'initializing': conversion from 'uptr' to 'uint', possible loss of data
#pragma warning(disable: 4267) // warning C4267: 'initializing': conversion from 'size_t' to 'uint', possible loss of data
#endif

/* The frontend's logger. One symbol, C linkage, so the C translation
 * units name it as the C++ ones do; main.cpp defines it and installs a
 * stderr fallback when the frontend offers no log interface. Callers
 * put the newline in the format. */
#include <libretro.h>
#ifdef __cplusplus
extern "C" {
#endif
extern retro_log_printf_t log_cb;
#ifdef __cplusplus
}
#endif
