/*  PCSX2 - PS2 Emulator for PCs
 *
 *  IPU macroblock-layer VLC decode tables.
 *
 *  The IPU decodes the MPEG-1/MPEG-2 macroblock layer in hardware, and
 *  this header supplies the lookup tables its emulation reads: the
 *  indexed arrays the decode paths address directly with a fixed-width
 *  bit window.
 *
 *  ipu_vlc_codes.h states each table as the standard states it - one row
 *  per code, giving the code bits, the code length, and the value denoted
 *  - and ipu_tables.c expands those codes into these arrays, each code
 *  replicated across the window values its prefix covers, once, at the
 *  IPU's reset on the EE thread, before anything decodes. The arrays keep
 *  the shape the decode paths expect, including the index bases the
 *  guarded lookups apply (CBP_7 at 16, MBA5 at 2, MBA11 at 24, the DC
 *  long tables at 1F0h/3E0h) and the one-bit index shift the
 *  macroblock_type I/P lookups use.
 */

#pragma once

#include "../../common/Pcsx2Defs.h"
#include "ipu_modes.h"

/* Each entry: the value the code denotes, and the code's length. */
typedef struct MBtab  { u8 modes; u8 len; } MBtab;
typedef struct MVtab  { u8 delta; u8 len; } MVtab;
typedef struct DMVtab { s8 dmv;   u8 len; } DMVtab;
typedef struct CBPtab { u8 cbp;   u8 len; } CBPtab;
typedef struct DCtab  { u8 size;  u8 len; } DCtab;
typedef struct MBAtab { u8 mba;   u8 len; } MBAtab;

/* macroblock_address_increment, Table B-1 */
typedef struct MBAtabSet
{
	MBAtab mba5[30];
	MBAtab mba11[26 * 4];
} MBAtabSet;

/* dct_dc_size_luminance / _chrominance, Tables B-12 and B-13 */
typedef struct DCtabSet
{
	DCtab lum0[32];    /* codes 00xxx ... 11110 */
	DCtab lum1[16];    /* codes 111110xxx ... 111111111 */
	DCtab chrom0[32];  /* codes 00xxx ... 11110 */
	DCtab chrom1[32];  /* codes 111110xxxx ... 1111111111 */
} DCtabSet;

/* macroblock_type, Tables B-2/B-3/B-4; motion_code, Table B-10;
 * dmvector, Table B-11; coded_block_pattern, Table B-9. */
typedef struct VLCtabSet
{
	MBtab  mb_i[2];
	MBtab  mb_p[32];
	MBtab  mb_b[64];
	MVtab  mv_4[8];
	MVtab  mv_10[48];
	DMVtab dmv_2[4];
	CBPtab cbp_7[112];
	CBPtab cbp_9[64];
} VLCtabSet;

#define IPU_VLC_ENTRIES(set, field, type) (sizeof(((set*)0)->field) / sizeof(type))

/* Reachable-index bounds.
 *
 * Every array above is addressed as base + (window >> S) - B, with the
 * window value constrained by the guard at the call site.  These
 * assertions state the largest index each guard can produce, so a table
 * built too small is a compile error rather than a read past the end.
 * They exist because CBP_7 was once rebuilt with 64 entries against a
 * reachable maximum of 111, and the resulting garbage coded block
 * patterns broke FMV playback.
 *
 * Derivations, from the guards in IPU_MultiISA.c:
 *   MB_I    macroblock_modes = UBITS(2), index >> 1        ->  0..1
 *   MB_P    macroblock_modes = UBITS(6), index >> 1        ->  0..31
 *   MB_B    macroblock_modes = UBITS(6)                    ->  0..63
 *   MV_4    taken when the 16-bit window has a clear sign bit and
 *           (code & 0xF000) or (code & 0xFC00) == 0x0C00; UBITS(4) is
 *           then at most 7                                 ->  0..7
 *   MV_10   the other branch: code < 0x0C00, so UBITS(10) < 48
 *                                                          ->  0..47
 *   DMV_2   UBITS(2)                                       ->  0..3
 *   CBP_7   taken when code >= 0x2000, so UBITS(7) is 16..127,
 *           minus the base of 16                           ->  0..111
 *   CBP_9   the other branch: code < 0x2000, so UBITS(9) < 64
 *                                                          ->  0..63
 *   MBA5    taken when code >= 4096, so UBITS(5) is 2..31, minus 2
 *                                                          ->  0..29
 *   MBA11   taken when 768 <= code < 4096, so UBITS(11) is 24..127,
 *           minus 24                                       ->  0..103
 *   lum0    UBITS(5) when below 31                         ->  0..30
 *   lum1    UBITS(9) is 0x1F0..0x1FF, minus 0x1F0          ->  0..15
 *   chrom0  UBITS(5) when below 31                         ->  0..30
 *   chrom1  UBITS(10) is 0x3E0..0x3FF, minus 0x3E0         ->  0..31
 */
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(VLCtabSet, mb_i, MBtab) > 1, "MB_I too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(VLCtabSet, mb_p, MBtab) > 31, "MB_P too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(VLCtabSet, mb_b, MBtab) > 63, "MB_B too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(VLCtabSet, mv_4, MVtab) > 7, "MV_4 too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(VLCtabSet, mv_10, MVtab) > 47, "MV_10 too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(VLCtabSet, dmv_2, DMVtab) > 3, "DMV_2 too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(VLCtabSet, cbp_7, CBPtab) > 111, "CBP_7 too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(VLCtabSet, cbp_9, CBPtab) > 63, "CBP_9 too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(MBAtabSet, mba5, MBAtab) > 29, "mba5 too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(MBAtabSet, mba11, MBAtab) > 103, "mba11 too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(DCtabSet, lum0, DCtab) > 30, "lum0 too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(DCtabSet, lum1, DCtab) > 15, "lum1 too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(DCtabSet, chrom0, DCtab) > 30, "chrom0 too small");
PCSX2_STATIC_ASSERT(IPU_VLC_ENTRIES(DCtabSet, chrom1, DCtab) > 31, "chrom1 too small");
