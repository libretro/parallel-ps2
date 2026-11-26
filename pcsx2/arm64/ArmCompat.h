/* SPDX-FileCopyrightText: lrps2 arm64 port
 * SPDX-License-Identifier: LGPL-3.0+
 *
 * Compatibility shims for code transplanted from armsx2 (isztldav/pcsx2):
 * the libretro fork stripped the upstream assertion/logging/bit-util
 * facilities these files expect. Assertions become no-ops (they are
 * developer sanity checks in the donor tree) and the one Common::
 * helper used is provided inline. DevCon comes from common/Console.h
 * like everywhere else. */

#pragma once

#include "common/Pcsx2Defs.h"
#include "VU.h"

#ifdef __cplusplus
/* Upstream VU.h aliases the two VU register banks; the libretro fork dropped
 * them. Internal-linkage references per TU, same as upstream. */
static VURegs& VU0 = vuRegs[0];
static VURegs& VU1 = vuRegs[1];
#endif

#ifndef pxAssert
#define pxAssert(x)           ((void)0)
#define pxAssertMsg(x, msg)   ((void)0)
#define pxAssertRel(x, msg)   ((void)0)
#define pxAssertDev(x, msg)   ((void)0)
#define pxFail(msg)           ((void)0)
#define pxFailRel(msg)        ((void)0)
#endif

#ifndef pxAssumeMsg
#define pxAssumeMsg(x, msg) ((void)0)
#define pxAssume(x)         ((void)0)
#endif

#ifdef __cplusplus
#ifndef safe_delete_array
#define safe_delete_array(x) { delete[] (x); (x) = nullptr; }
#define safe_delete(x)       { delete (x); (x) = nullptr; }
#endif

static constexpr bool IsDevBuild = false;

inline bool operator==(const u128& a, const u128& b) { return a.lo == b.lo && a.hi == b.hi; }
inline bool operator!=(const u128& a, const u128& b) { return !(a == b); }

namespace Common
{
	template <typename T>
	static constexpr __fi bool IsAligned(T value, unsigned int alignment)
	{
		return (value % static_cast<T>(alignment)) == 0;
	}
	template <typename T>
	static constexpr __fi T AlignUpPow2(T value, unsigned int alignment)
	{
		return (value + static_cast<T>(alignment - 1)) & ~static_cast<T>(alignment - 1);
	}
} /* namespace Common */
#endif
