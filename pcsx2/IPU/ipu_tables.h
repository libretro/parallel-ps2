/*  PCSX2 - PS2 Emulator for PCs
 *
 *  The IPU's decode tables: one copy for every instruction set tier's
 *  decoder, filled from the standard's code lists by ipu_tables_init
 *  (ipu_tables.c) at the IPU's reset, on the EE thread, before anything
 *  decodes; read-only afterwards.
 */

#pragma once

#include "ipu_vlc.h"
#include "ipu_dct_lut.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IPUTables
{
	DCTlut    dct;
	VLCtabSet vlc;
	MBAtabSet mba;
	DCtabSet  dc;
} IPUTables;

PCSX2_ALIGN(16) extern IPUTables ipu_tables;

/* Fills ipu_tables; the second and later calls do nothing. */
void ipu_tables_init(void);

/* One lookup for the DCT coefficient code in the 16-bit window; NULL
 * when the code ends the block. */
static __fi const DCTtabLut* dct_lookup(int code, int variant)
{
	if (code >= 256)
		return &ipu_tables.dct.hi[variant].e[code >> 4];
	if (code >= 16)
		return &ipu_tables.dct.lo.e[code];
	return NULL;
}

#ifdef __cplusplus
}
#endif
