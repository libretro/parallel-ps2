/*  PCSX2 - PS2 Emulator for PCs
 *
 *  IPU macroblock-layer VLC decode tables.
 *
 *  The IPU decodes the MPEG-1/MPEG-2 macroblock layer in hardware, and
 *  this header supplies the lookup tables its emulation reads: the
 *  syntax element enumerations from H.262, and the indexed arrays the
 *  decode paths address directly with a fixed-width bit window.
 *
 *  The arrays are not written out by hand.  ipu_vlc_codes.h states each
 *  table as the standard states it - one row per code, giving the code
 *  bits, the code length, and the value denoted - and the expander
 *  below fills the indexed form at compile time, replicating each code
 *  across the window values its prefix covers.  That means no runtime
 *  initialization and no lazy-init race, and the arrays remain exactly
 *  the shape the decode paths expect, including the index bases the
 *  guarded lookups apply (CBP_7 at 16, MBA5 at 2, MBA11 at 24, the DC
 *  long tables at 1F0h/3E0h) and the one-bit index shift the
 *  macroblock_type I/P lookups use.
 */

#pragma once

#include <cstdint>

#include "ipu_vlc_codes.h"
#include "ipu_modes.h"

struct MBtab
{
	uint8_t modes;
	uint8_t len;
};

struct MVtab
{
	uint8_t delta;
	uint8_t len;
};

struct DMVtab
{
	int8_t dmv;
	uint8_t len;
};

struct CBPtab
{
	uint8_t cbp;
	uint8_t len;
};

struct DCtab
{
	uint8_t size;
	uint8_t len;
};

struct MBAtab
{
	uint8_t mba;
	uint8_t len;
};

static constexpr void vlc_store(MBtab& t, int v) { t.modes = (uint8_t)v; }
static constexpr void vlc_store(MVtab& t, int v) { t.delta = (uint8_t)v; }
static constexpr void vlc_store(DMVtab& t, int v) { t.dmv = (int8_t)v; }
static constexpr void vlc_store(CBPtab& t, int v) { t.cbp = (uint8_t)v; }
static constexpr void vlc_store(DCtab& t, int v) { t.size = (uint8_t)v; }
static constexpr void vlc_store(MBAtab& t, int v) { t.mba = (uint8_t)v; }


/* Expands a code list into the indexed array a decode path reads.
 *
 *   W  width of the bit window the lookup takes
 *   S  right shift applied to the window before indexing
 *   B  index base subtracted after the shift
 *
 * A code of length L covers every window value sharing its prefix; each
 * of those maps to index (value >> S) - B, and entries outside the
 * array are values the call site's guard never reaches. */
template <typename T, int N, int M>
static constexpr void vlc_expand(T (&out)[N], const VLCcode (&codes)[M], int W, int S, int B)
{
	for (int c = 0; c < M; c++)
	{
		const int pw = codes[c].len < W ? codes[c].len : W;
		const int shift = W - pw;
		const int start = codes[c].bits << shift;
		for (int v = start; v < start + (1 << shift); v++)
		{
			const int idx = (v >> S) - B;
			if (idx >= 0 && idx < N)
			{
				out[idx].len = codes[c].len;
				vlc_store(out[idx], codes[c].value);
			}
		}
	}
}

template <typename T, int N, int M>
struct VLCarray
{
	T e[N];
	constexpr VLCarray(const VLCcode (&codes)[M], int W, int S, int B) : e{}
	{
		vlc_expand(e, codes, W, S, B);
	}
};

/* macroblock_type, Tables B-2/B-3/B-4 */
alignas(16) static constexpr VLCarray<MBtab, 2, 3> MB_I_A(VLC_MB_I, 2, 1, 0);
alignas(16) static constexpr VLCarray<MBtab, 32, 8> MB_P_A(VLC_MB_P, 6, 1, 0);
alignas(16) static constexpr VLCarray<MBtab, 64, 11> MB_B_A(VLC_MB_B, 6, 0, 0);

/* motion_code, Table B-10, and dmvector, Table B-11 */
alignas(16) static constexpr VLCarray<MVtab, 8, 4> MV_4_A(VLC_MV_4, 4, 0, 0);
alignas(16) static constexpr VLCarray<MVtab, 48, 24> MV_10_A(VLC_MV_10, 10, 0, 0);
alignas(16) static constexpr VLCarray<DMVtab, 4, 3> DMV_2_A(VLC_DMV_2, 2, 0, 0);

/* coded_block_pattern, Table B-9 */
alignas(16) static constexpr VLCarray<CBPtab, 112, 29> CBP_7_A(VLC_CBP_7, 7, 0, 16);
alignas(16) static constexpr VLCarray<CBPtab, 64, 35> CBP_9_A(VLC_CBP_9, 9, 0, 0);

static constexpr const MBtab* MB_I = MB_I_A.e;
static constexpr const MBtab* MB_P = MB_P_A.e;
static constexpr const MBtab* MB_B = MB_B_A.e;
static constexpr const MVtab* MV_4 = MV_4_A.e;
static constexpr const MVtab* MV_10 = MV_10_A.e;
static constexpr const DMVtab* DMV_2 = DMV_2_A.e;
static constexpr const CBPtab* CBP_7 = CBP_7_A.e;
static constexpr const CBPtab* CBP_9 = CBP_9_A.e;


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
 * Derivations, from the guards in IPU_MultiISA.cpp:
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
static_assert(sizeof(MB_I_A.e) / sizeof(MBtab) > 1, "MB_I too small");
static_assert(sizeof(MB_P_A.e) / sizeof(MBtab) > 31, "MB_P too small");
static_assert(sizeof(MB_B_A.e) / sizeof(MBtab) > 63, "MB_B too small");
static_assert(sizeof(MV_4_A.e) / sizeof(MVtab) > 7, "MV_4 too small");
static_assert(sizeof(MV_10_A.e) / sizeof(MVtab) > 47, "MV_10 too small");
static_assert(sizeof(DMV_2_A.e) / sizeof(DMVtab) > 3, "DMV_2 too small");
static_assert(sizeof(CBP_7_A.e) / sizeof(CBPtab) > 111, "CBP_7 too small");
static_assert(sizeof(CBP_9_A.e) / sizeof(CBPtab) > 63, "CBP_9 too small");

/* macroblock_address_increment, Table B-1 */
struct MBAtabSet
{
	MBAtab mba5[30];
	MBAtab mba11[26 * 4];
};

static constexpr MBAtabSet mba_make(void)
{
	MBAtabSet t{};
	vlc_expand(t.mba5, VLC_MBA5, 5, 0, 2);
	vlc_expand(t.mba11, VLC_MBA11, 11, 0, 24);
	return t;
}

alignas(16) static constexpr MBAtabSet MBA = mba_make();


static_assert(sizeof(MBA.mba5) / sizeof(MBAtab) > 29, "mba5 too small");
static_assert(sizeof(MBA.mba11) / sizeof(MBAtab) > 103, "mba11 too small");

/* dct_dc_size_luminance / _chrominance, Tables B-12 and B-13 */
struct DCtabSet
{
	DCtab lum0[32];    /* codes 00xxx ... 11110 */
	DCtab lum1[16];    /* codes 111110xxx ... 111111111 */
	DCtab chrom0[32];  /* codes 00xxx ... 11110 */
	DCtab chrom1[32];  /* codes 111110xxxx ... 1111111111 */
};

static constexpr DCtabSet dc_make(void)
{
	DCtabSet t{};
	vlc_expand(t.lum0, VLC_DC_LUM0, 5, 0, 0);
	vlc_expand(t.lum1, VLC_DC_LUM1, 9, 0, 0x1f0);
	vlc_expand(t.chrom0, VLC_DC_CHROM0, 5, 0, 0);
	vlc_expand(t.chrom1, VLC_DC_CHROM1, 10, 0, 0x3e0);
	return t;
}

alignas(16) static constexpr DCtabSet DCtable = dc_make();

static_assert(sizeof(DCtable.lum0) / sizeof(DCtab) > 30, "lum0 too small");
static_assert(sizeof(DCtable.lum1) / sizeof(DCtab) > 15, "lum1 too small");
static_assert(sizeof(DCtable.chrom0) / sizeof(DCtab) > 30, "chrom0 too small");
static_assert(sizeof(DCtable.chrom1) / sizeof(DCtab) > 31, "chrom1 too small");
