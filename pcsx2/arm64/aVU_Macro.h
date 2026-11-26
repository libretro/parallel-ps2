/* SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-License-Identifier: GPL-3.0+ */

#ifndef ARM64_AVU_MACRO_H
#define ARM64_AVU_MACRO_H

#include "common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* COP2 macro ops through microVU0's single-op emitters (aVU_Macro.inl),
 * for the EE rec. */
bool recVUMacroIsMode0(u32 op);   /* classify COP2 SPECIAL ALU */
bool recVUMacroEmitMode0(u32 op); /* emit it */
/* C.78: run-hoisted macro bracket -- the first op materializes x19/x27,
 * the last restores x19. */
extern bool g_cop2MacroFirst;
extern bool g_cop2MacroLast;

#ifdef __cplusplus
}
#endif

#endif
