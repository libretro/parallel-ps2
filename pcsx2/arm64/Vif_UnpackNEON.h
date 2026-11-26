/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
 * SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-License-Identifier: GPL-3.0+ */

#ifndef ARM64_VIF_UNPACKNEON_H
#define ARM64_VIF_UNPACKNEON_H

#include "Common.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"
#include "Vif_Unpack.h" /* nVifT */
#include "arm64/AsmHelpers.h"

#define xmmCol0 A64_Q(2)
#define xmmCol1 A64_Q(3)
#define xmmCol2 A64_Q(4)
#define xmmCol3 A64_Q(5)
#define xmmRow A64_Q(6)
#define xmmTemp A64_Q(7)

/* One state for both unpack generators, as on x86 (newVif_UnpackSSE.h):
 * `dynarec` picks the masked write; the simple kernels leave doMode,
 * skipProcessing and inputMasked at zero, which makes the shared
 * write-protect / input-mask / unmasked tests come out as theirs. */
typedef struct VifUnpackNEON
{
	bool dynarec;
	bool usn;    /* unsigned flag */
	bool doMask; /* masking write enable flag */
	int UnpkLoopIteration;
	int UnpkNoOfIterations;
	int IsAligned;

	a64_mem dstIndirect;
	a64_mem srcIndirect;
	a64r workReg;
	a64r destReg;
	a64r workGprW;

	/* simple */
	int curCycle;

	/* dynarec */
	bool isFill;
	int doMode; /* two bit value representing difference mode */
	bool skipProcessing;
	bool inputMasked;
	const nVifStruct* v;  /* vif0 or vif1 */
	const nVifBlock* vB;  /* some pre-collected data from VifStruct */
	int vCL;              /* internal copy of vif->cl */
} VifUnpackNEON;

#ifdef __cplusplus
extern "C" {
#endif

void vneon_init(VifUnpackNEON* p);
void vneon_unpack(const VifUnpackNEON* p, int upktype);
void vneon_mov_dest(const VifUnpackNEON* p);
/* The dynarec's masked write (Vif_Dynarec_arm64.c). */
void vneon_dynarec_mask_write(const VifUnpackNEON* p, a64r regX);

#ifdef __cplusplus
}
#endif

#endif
