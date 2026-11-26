/* SPDX-FileCopyrightText: lrps2 arm64 port
 * SPDX-License-Identifier: LGPL-3.0+
 *
 * The assertion macros the code transplanted from armsx2 (isztldav/pcsx2)
 * expects; the libretro fork stripped them. They are developer sanity
 * checks in the donor tree, so here they are no-ops. */

#ifndef ARM64_ARMCOMPAT_H
#define ARM64_ARMCOMPAT_H

#include "common/Pcsx2Defs.h"
#include "VU.h"


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

#endif
