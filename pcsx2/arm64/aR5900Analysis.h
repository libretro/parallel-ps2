/* SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
 * SPDX-License-Identifier: GPL-3.0+ */

/* EE instruction info (EEINST) for the arm64 rec's COP2 macro mode: the
 * EEINST_COP2_* bits the macro emitters (aVU_Macro.inl) read through
 * g_pCurInstInfo, laid out as the x86 rec's (pcsx2/x86/iCore.h). */

#ifndef ARM64_AR5900ANALYSIS_H
#define ARM64_AR5900ANALYSIS_H

#include "common/Pcsx2Defs.h"

/* --------------------------------------------------------------------------------------
 * Instruction info (mirrors pcsx2/x86/iCore.h:170-220)
 * --------------------------------------------------------------------------------------
 * Liveness flags (general). On ARM64 only the COP2_* bits below are used so far. */
#define EEINST_LIVE    1 /* if var is ever used (read or write) */
#define EEINST_LASTUSE 8 /* if var isn't written/read anymore */
#define EEINST_XMM  0x20 /* var will be used in xmm ops */
#define EEINST_USED 0x40

/* COP2 macro-mode analysis bits — set by the M1 passes, consumed by the M2/M3 emit. */
#define EEINST_COP2_DENORMALIZE_STATUS_FLAG 0x100
#define EEINST_COP2_NORMALIZE_STATUS_FLAG   0x200
#define EEINST_COP2_STATUS_FLAG             0x400
#define EEINST_COP2_MAC_FLAG                0x800
#define EEINST_COP2_CLIP_FLAG               0x1000
#define EEINST_COP2_SYNC_VU0                0x2000
#define EEINST_COP2_FINISH_VU0              0x4000
#define EEINST_COP2_FLUSH_VU0_REGISTERS     0x8000

typedef struct EEINST
{
	u16 info; /* extra info, if 1 inst is COP1, 2 inst is COP2. Also uses EEINST_XMM */
	u8 regs[34]; /* includes HI/LO (HI=32, LO=33) */
	u8 fpuregs[33]; /* ACC=32 */
	u8 vfregs[34]; /* ACC=32, I=33 */
	u8 viregs[16];

	/* uses XMMTYPE_ flags; if type == XMMTYPE_TEMP, not used */
	u8 writeType[3], writeReg[3]; /* reg written in this inst, 0 if no reg */
	u8 readType[4], readReg[4];
} EEINST;

/* info for the cur instruction (set per-op in recRecompile; mirrors x86 g_pCurInstInfo) */
#ifdef __cplusplus
extern "C" {
#endif
extern EEINST* g_pCurInstInfo;
#ifdef __cplusplus
}
#endif

#endif
