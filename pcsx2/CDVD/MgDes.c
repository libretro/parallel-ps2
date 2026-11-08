/*  PCSX2 - PS2 Emulator for PCs
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

/* DES as FIPS 46-3 specifies it. Bits are numbered 1..64 from the most
 * significant bit of the first byte, as in the standard's tables. The
 * permutations run through byte-indexed tables and the S-boxes through
 * combined S-and-P tables, all built from the standard's tables on first
 * use; FP is built as the inverse of IP. C89. */
#include <string.h>

#include "MgDes.h"

static const u8 s_ip[64] = {
	58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
	62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
	57, 49, 41, 33, 25, 17,  9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
	61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};

static const u8 s_p[32] = {
	16,  7, 20, 21, 29, 12, 28, 17,  1, 15, 23, 26,  5, 18, 31, 10,
	 2,  8, 24, 14, 32, 27,  3,  9, 19, 13, 30,  6, 22, 11,  4, 25};

static const u8 s_pc1[56] = {
	57, 49, 41, 33, 25, 17,  9,  1, 58, 50, 42, 34, 26, 18,
	10,  2, 59, 51, 43, 35, 27, 19, 11,  3, 60, 52, 44, 36,
	63, 55, 47, 39, 31, 23, 15,  7, 62, 54, 46, 38, 30, 22,
	14,  6, 61, 53, 45, 37, 29, 21, 13,  5, 28, 20, 12,  4};

static const u8 s_pc2[48] = {
	14, 17, 11, 24,  1,  5,  3, 28, 15,  6, 21, 10,
	23, 19, 12,  4, 26,  8, 16,  7, 27, 20, 13,  2,
	41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
	44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};

static const u8 s_shifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

static const u8 s_sbox[8][4][16] = {
	{{14,  4, 13,  1,  2, 15, 11,  8,  3, 10,  6, 12,  5,  9,  0,  7},
	 { 0, 15,  7,  4, 14,  2, 13,  1, 10,  6, 12, 11,  9,  5,  3,  8},
	 { 4,  1, 14,  8, 13,  6,  2, 11, 15, 12,  9,  7,  3, 10,  5,  0},
	 {15, 12,  8,  2,  4,  9,  1,  7,  5, 11,  3, 14, 10,  0,  6, 13}},
	{{15,  1,  8, 14,  6, 11,  3,  4,  9,  7,  2, 13, 12,  0,  5, 10},
	 { 3, 13,  4,  7, 15,  2,  8, 14, 12,  0,  1, 10,  6,  9, 11,  5},
	 { 0, 14,  7, 11, 10,  4, 13,  1,  5,  8, 12,  6,  9,  3,  2, 15},
	 {13,  8, 10,  1,  3, 15,  4,  2, 11,  6,  7, 12,  0,  5, 14,  9}},
	{{10,  0,  9, 14,  6,  3, 15,  5,  1, 13, 12,  7, 11,  4,  2,  8},
	 {13,  7,  0,  9,  3,  4,  6, 10,  2,  8,  5, 14, 12, 11, 15,  1},
	 {13,  6,  4,  9,  8, 15,  3,  0, 11,  1,  2, 12,  5, 10, 14,  7},
	 { 1, 10, 13,  0,  6,  9,  8,  7,  4, 15, 14,  3, 11,  5,  2, 12}},
	{{ 7, 13, 14,  3,  0,  6,  9, 10,  1,  2,  8,  5, 11, 12,  4, 15},
	 {13,  8, 11,  5,  6, 15,  0,  3,  4,  7,  2, 12,  1, 10, 14,  9},
	 {10,  6,  9,  0, 12, 11,  7, 13, 15,  1,  3, 14,  5,  2,  8,  4},
	 { 3, 15,  0,  6, 10,  1, 13,  8,  9,  4,  5, 11, 12,  7,  2, 14}},
	{{ 2, 12,  4,  1,  7, 10, 11,  6,  8,  5,  3, 15, 13,  0, 14,  9},
	 {14, 11,  2, 12,  4,  7, 13,  1,  5,  0, 15, 10,  3,  9,  8,  6},
	 { 4,  2,  1, 11, 10, 13,  7,  8, 15,  9, 12,  5,  6,  3,  0, 14},
	 {11,  8, 12,  7,  1, 14,  2, 13,  6, 15,  0,  9, 10,  4,  5,  3}},
	{{12,  1, 10, 15,  9,  2,  6,  8,  0, 13,  3,  4, 14,  7,  5, 11},
	 {10, 15,  4,  2,  7, 12,  9,  5,  6,  1, 13, 14,  0, 11,  3,  8},
	 { 9, 14, 15,  5,  2,  8, 12,  3,  7,  0,  4, 10,  1, 13, 11,  6},
	 { 4,  3,  2, 12,  9,  5, 15, 10, 11, 14,  1,  7,  6,  0,  8, 13}},
	{{ 4, 11,  2, 14, 15,  0,  8, 13,  3, 12,  9,  7,  5, 10,  6,  1},
	 {13,  0, 11,  7,  4,  9,  1, 10, 14,  3,  5, 12,  2, 15,  8,  6},
	 { 1,  4, 11, 13, 12,  3,  7, 14, 10, 15,  6,  8,  0,  5,  9,  2},
	 { 6, 11, 13,  8,  1,  4, 10,  7,  9,  5,  0, 15, 14,  2,  3, 12}},
	{{13,  2,  8,  4,  6, 15, 11,  1, 10,  9,  3, 14,  5,  0, 12,  7},
	 { 1, 15, 13,  8, 10,  3,  7,  4, 12,  5,  6, 11,  0, 14,  9,  2},
	 { 7, 11,  4,  1,  9, 12, 14,  2,  0,  6, 10, 13, 15,  3,  5,  8},
	 { 2,  1, 14,  7,  4, 10,  8, 13, 15, 12,  9,  0,  3,  5,  6, 11}}};

/* ip_tab[i][b]: IP applied to a block holding byte b at byte i and zeros
 * elsewhere; IP of a block is the OR over its eight bytes. Same for FP. */
static u64 s_ip_tab[8][256];
static u64 s_fp_tab[8][256];
/* sp_tab[i][x]: P applied to S-box i's output for the 6-bit input x,
 * placed where that S-box's four bits sit. */
static u32 s_sp_tab[8][64];
static int s_ready;

/* Bit k (1-based, from the most significant) of an n-bit value. */
static u32 bit_of(u64 v, int n, int k)
{
	return (u32)((v >> (n - k)) & 1);
}

static void build_perm_tab(u64 tab[8][256], const u8 perm[64])
{
	int i, b, k;
	for (i = 0; i < 8; i++)
	{
		for (b = 0; b < 256; b++)
		{
			const u64 in = (u64)b << (56 - 8 * i);
			u64 out = 0;
			for (k = 0; k < 64; k++)
				out |= (u64)bit_of(in, 64, perm[k]) << (63 - k);
			tab[i][b] = out;
		}
	}
}

static void des_init(void)
{
	u8 fp[64];
	int i, x, k;

	if (s_ready)
		return;

	/* FP = IP^-1: output bit k of FP is the input bit that IP sent to k. */
	for (k = 0; k < 64; k++)
		fp[s_ip[k] - 1] = (u8)(k + 1);
	build_perm_tab(s_ip_tab, s_ip);
	build_perm_tab(s_fp_tab, fp);

	for (i = 0; i < 8; i++)
	{
		for (x = 0; x < 64; x++)
		{
			const int row = ((x >> 4) & 2) | (x & 1);
			const int col = (x >> 1) & 15;
			const u32 pre = (u32)s_sbox[i][row][col] << (28 - 4 * i);
			u32 out = 0;
			for (k = 0; k < 32; k++)
				out |= bit_of(pre, 32, s_p[k]) << (31 - k);
			s_sp_tab[i][x] = out;
		}
	}
	s_ready = 1;
}

static u64 load_be64(const u8 b[8])
{
	u64 v = 0;
	int i;
	for (i = 0; i < 8; i++)
		v = (v << 8) | b[i];
	return v;
}

static void store_be64(u8 b[8], u64 v)
{
	int i;
	for (i = 7; i >= 0; i--)
	{
		b[i] = (u8)v;
		v >>= 8;
	}
}

static u64 permute64(u64 tab[8][256], u64 v)
{
	u64 out = 0;
	int i;
	for (i = 0; i < 8; i++)
		out |= tab[i][(v >> (56 - 8 * i)) & 0xff];
	return out;
}

void mg_des_setkey(MgDesKey* ks, const u8 key[8])
{
	const u64 k64 = load_be64(key);
	u32 c = 0, d = 0;
	int r, k;

	des_init();
	for (k = 0; k < 28; k++)
	{
		c |= bit_of(k64, 64, s_pc1[k]) << (27 - k);
		d |= bit_of(k64, 64, s_pc1[28 + k]) << (27 - k);
	}
	for (r = 0; r < 16; r++)
	{
		u64 cd, sub = 0;
		int s;
		for (s = 0; s < s_shifts[r]; s++)
		{
			c = ((c << 1) | (c >> 27)) & 0x0fffffff;
			d = ((d << 1) | (d >> 27)) & 0x0fffffff;
		}
		cd = ((u64)c << 28) | d;
		for (k = 0; k < 48; k++)
			sub |= (u64)bit_of(cd, 56, s_pc2[k]) << (47 - k);
		for (k = 0; k < 8; k++)
			ks->round[r][k] = (u8)((sub >> (42 - 6 * k)) & 0x3f);
	}
}

/* The round function: E expansion read straight off a rotation of R --
 * S-box i takes bits 4i..4i+5 of R, bit 0 meaning bit 32 and bit 33 bit
 * 1 -- then the combined S-and-P tables. */
static u32 des_f(u32 r, const u8 sub[8])
{
	u32 out = 0;
	int i;
	for (i = 0; i < 8; i++)
	{
		const int s = (4 * i + 5) & 31;
		const u32 rot = s ? ((r << s) | (r >> (32 - s))) : r;
		out |= s_sp_tab[i][(rot & 0x3f) ^ sub[i]];
	}
	return out;
}

static void des_crypt(const MgDesKey* ks, u8 block[8], int decrypt)
{
	u64 v = permute64(s_ip_tab, load_be64(block));
	u32 l = (u32)(v >> 32), r = (u32)v, t;
	int i;

	for (i = 0; i < 16; i++)
	{
		t = r;
		r = l ^ des_f(r, ks->round[decrypt ? 15 - i : i]);
		l = t;
	}
	/* The halves are swapped before FP. */
	v = ((u64)r << 32) | l;
	store_be64(block, permute64(s_fp_tab, v));
}

void mg_des_encrypt_ks(const MgDesKey* ks, u8 block[8])
{
	des_crypt(ks, block, 0);
}

void mg_des_decrypt_ks(const MgDesKey* ks, u8 block[8])
{
	des_crypt(ks, block, 1);
}

void mg_des_encrypt(const u8 key[8], u8 block[8])
{
	MgDesKey ks;
	mg_des_setkey(&ks, key);
	des_crypt(&ks, block, 0);
}

void mg_des_decrypt(const u8 key[8], u8 block[8])
{
	MgDesKey ks;
	mg_des_setkey(&ks, key);
	des_crypt(&ks, block, 1);
}

void mg_3des_encrypt(const u8 key[16], u8 block[8])
{
	MgDesKey k1, k2;
	mg_des_setkey(&k1, key);
	mg_des_setkey(&k2, key + 8);
	des_crypt(&k1, block, 0);
	des_crypt(&k2, block, 1);
	des_crypt(&k1, block, 0);
}

void mg_3des_decrypt(const u8 key[16], u8 block[8])
{
	MgDesKey k1, k2;
	mg_des_setkey(&k1, key);
	mg_des_setkey(&k2, key + 8);
	des_crypt(&k1, block, 1);
	des_crypt(&k2, block, 0);
	des_crypt(&k1, block, 1);
}
