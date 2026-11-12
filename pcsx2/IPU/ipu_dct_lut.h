/*  PCSX2 - PS2 Emulator for PCs
 *
 *  Fused DCT coefficient lookup for the IPU's VLC decode hot loop.
 *
 *  The macroblock-layer coefficient decoder used to select among eight
 *  DCT sub-tables with a chain of magnitude compares on every symbol,
 *  once per coefficient, in the innermost loop of BDEC/IDEC.  The
 *  sub-tables are just successively finer-grained views of the same
 *  prefix code (H.262 Table B-14/B-15), so they flatten into one array
 *  indexed by the code prefix.
 *
 *  Two arrays cover the whole 16-bit code space:
 *
 *    hi[]  indexed by (code >> 4), used when code >= 256.  The finest
 *          sub-table in that range (tab2) is already indexed by
 *          code >> 4, so the coarser ones (tab1/tab1a at code >> 6,
 *          tab0/tab0a at code >> 8, first/next at code >> 12) simply
 *          repeat across the entries their prefix covers.
 *    lo[]  indexed by code directly, used when 16 <= code < 256,
 *          covering tab3..tab6.
 *
 *  code < 16 terminates the block, exactly as the compare chain did.
 *
 *  Three hi variants correspond to the three ways the old chain could
 *  resolve the top of the code space:
 *
 *    HI_NEXT  table zero, subsequent coefficients (first/next -> next)
 *    HI_FIRST table zero, first (DC) coefficient  (first/next -> first)
 *    HI_INTRA intra VLC format (Table B-15): tab0a/tab1a, and note the
 *             old chain deliberately let code >= 16384 fall through to
 *             the tab0a branch in this case rather than using next[].
 *
 *  The tables are built from the standard's own code assignments in
 *  ipu_dct_codes.h by ipu_tables.c, once, at the IPU's reset on the EE
 *  thread, before anything decodes.
 *
 *  Equivalence is exhaustive, not sampled: every one of the 65536
 *  codes in each variant resolves to the same {run, level, len} the
 *  previous table layout produced.
 */

#pragma once

#include "../../common/Pcsx2Defs.h"

typedef struct DCTtabLut
{
	u8 run;
	u8 level;
	u8 len;
} DCTtabLut;

typedef struct DCTlutHi
{
	DCTtabLut e[4096];
} DCTlutHi;

typedef struct DCTlutLo
{
	DCTtabLut e[256];
} DCTlutLo;

enum DCTlutVariant
{
	DCT_LUT_NEXT = 0,
	DCT_LUT_FIRST = 1,
	DCT_LUT_INTRA = 2
};

/* Codes of 12 bits or fewer land in hi[], indexed by the top 12 bits of
 * the 16-bit window; each code fills the span of indices its prefix
 * covers.  Longer codes have at least eight leading zeros, so their
 * 16-bit window value is below 256 and they land in lo[], indexed by
 * that value directly. */
typedef struct DCTlut
{
	DCTlutHi hi[3];
	DCTlutLo lo;
} DCTlut;
