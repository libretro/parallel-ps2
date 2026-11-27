/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

/* The scanline code of a draw (GSDrawScanline.c): on x86 the functions
 * generated for its selector (GSSwJit.h), kept by selector; elsewhere the
 * C rasterizer below them. C89; compiled once a tier, so its functions
 * are named after the tier (GS_SW). */

#ifndef GS_DRAW_SCANLINE_H
#define GS_DRAW_SCANLINE_H

#include "GSScanlineEnvironment.h"
#include "GSRaster.h"
#include "../Common/GSCodeMap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A function of the tier this is compiled for */
#ifdef MULTI_ISA_UNSHARED_COMPILATION
#define GS_SW_ISA MULTI_ISA_UNSHARED_COMPILATION
#else
#define GS_SW_ISA isa_native
#endif
#define GS_SW_CAT_(a, b) a##_##b
#define GS_SW_CAT(a, b) GS_SW_CAT_(a, b)
#define GS_SW(name) GS_SW_CAT(name, GS_SW_ISA)

/* Generated code (x86), or the C rasterizer: GS_DS_INTERPRET asks for the
 * latter anywhere (tests/gsscanline) */
#if (defined(ARCH_X86) || defined(__x86_64__) || defined(_M_X64)) && !defined(GS_DS_INTERPRET)
#define GS_DS_JIT 1
#endif

struct gs_rasterizer_data;

typedef struct gs_draw_scanline
{
#ifdef GS_DS_JIT
	/* The setup and scanline functions made, by selector: made at start
	 * for more than a game makes (random selectors average 0.3 to 0.7 KiB
	 * of setup and 1.1 to 1.3 KiB of scanline, so the 64 MiB code reserve
	 * fills first) */
	gs_code_map sp_map;
	gs_code_map ds_map;
#else
	int unused;
#endif
} gs_draw_scanline;

void GS_SW(gs_draw_scanline_init)(gs_draw_scanline* ds);
void GS_SW(gs_draw_scanline_free)(gs_draw_scanline* ds);

/* Forgets every function made, for a start of the code reserve again */
void GS_SW(gs_draw_scanline_reset_code_cache)(gs_draw_scanline* ds);

/* The draw's setup, scanline and edge functions; false when the code
 * reserve or the maps are out of room */
bool GS_SW(gs_draw_scanline_setup_draw)(gs_draw_scanline* ds, struct gs_rasterizer_data* data);

/* What a thread works out of a draw before its primitives */
void GS_SW(gs_draw_scanline_begin_draw)(const struct gs_rasterizer_data* data, GSScanlineLocalData* local);

#ifdef __cplusplus
}
#endif

#endif
