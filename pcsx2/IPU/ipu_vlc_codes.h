/*  PCSX2 - PS2 Emulator for PCs
 *
 *  Macroblock-layer VLC code tables for the IPU, stated the way H.262
 *  (MPEG-2 Part 2) states them: one row per code, giving the code bits,
 *  the code length, and the value the code denotes.
 *
 *    VLC_MB_I/MB_P/MB_B   Tables B-2/B-3/B-4, macroblock_type
 *    VLC_MV_4/MV_10       Table B-10, motion_code
 *    VLC_DMV_2            Table B-11, dmvector
 *    VLC_CBP_7/CBP_9      Table B-9, coded_block_pattern
 *    VLC_MBA5/MBA11       Table B-1, macroblock_address_increment
 *    VLC_DC_LUM0/LUM1     Table B-12, dct_dc_size_luminance
 *    VLC_DC_CHROM0/CHROM1 Table B-13, dct_dc_size_chrominance
 *
 *  Each pair (short window / long window) is the split the decoder's
 *  lookups use, not a property of the standard: the short table covers
 *  the codes resolvable in the narrow bit window and the long table the
 *  rest.  ipu_tables.c expands these rows back into the indexed arrays the
 *  decode paths read, at the IPU's reset.
 */

#pragma once

#include "../../common/Pcsx2Types.h"

typedef struct VLCcode
{
	u16 bits;   /* code value, right-aligned; prefix truncated to
	             * the lookup window when the code is longer */
	u8  len;    /* code length in bits */
	s16 value;  /* decoded value (modes, delta, cbp, mba, size) */
} VLCcode;

/* MB_I: 3 codes, window 2 bits */
static const VLCcode VLC_MB_I[] =
{
	{0x001, 1,  1},{0x000, 2, 17},{0x001, 2, 17},
};

/* MB_P: 8 codes, window 6 bits */
static const VLCcode VLC_MB_P[] =
{
	{0x001, 1, 10},{0x001, 2,  2},{0x001, 3,  8},{0x001, 5, 18},{0x002, 5, 26},
	{0x003, 5,  1},{0x000, 6, 17},{0x001, 6, 17},
};

/* MB_B: 11 codes, window 6 bits */
static const VLCcode VLC_MB_B[] =
{
	{0x002, 2, 12},{0x003, 2, 14},{0x002, 3,  4},{0x003, 3,  6},{0x002, 4,  8},
	{0x003, 4, 10},{0x002, 5, 30},{0x003, 5,  1},{0x001, 6, 17},{0x002, 6, 22},
	{0x003, 6, 26},
};

/* MV_4: 4 codes, window 4 bits */
static const VLCcode VLC_MV_4[] =
{
	{0x001, 2,  0},{0x001, 3,  1},{0x001, 4,  2},{0x000, 6,  3},
};

/* MV_10: 24 codes, window 10 bits */
static const VLCcode VLC_MV_10[] =
{
	{0x003, 7,  6},{0x004, 7,  5},{0x005, 7,  4},{0x009, 9,  9},{0x00a, 9,  8},
	{0x00b, 9,  7},{0x000,10,  0},{0x001,10,  0},{0x002,10,  0},{0x003,10,  0},
	{0x004,10,  0},{0x005,10,  0},{0x006,10,  0},{0x007,10,  0},{0x008,10,  0},
	{0x009,10,  0},{0x00a,10,  0},{0x00b,10,  0},{0x00c,10, 15},{0x00d,10, 14},
	{0x00e,10, 13},{0x00f,10, 12},{0x010,10, 11},{0x011,10, 10},
};

/* DMV_2: 3 codes, window 2 bits */
static const VLCcode VLC_DMV_2[] =
{
	{0x000, 1,  0},{0x002, 2,  1},{0x003, 2, -1},
};

/* CBP_7: 29 codes, window 7 bits */
static const VLCcode VLC_CBP_7[] =
{
	{0x007, 3, 60},{0x00a, 4, 32},{0x00b, 4, 16},{0x00c, 4,  8},{0x00d, 4,  4},
	{0x008, 5, 62},{0x009, 5,  2},{0x00a, 5, 61},{0x00b, 5,  1},{0x00c, 5, 56},
	{0x00d, 5, 52},{0x00e, 5, 44},{0x00f, 5, 28},{0x010, 5, 40},{0x011, 5, 20},
	{0x012, 5, 48},{0x013, 5, 12},{0x00c, 6, 63},{0x00d, 6,  3},{0x00e, 6, 36},
	{0x00f, 6, 24},{0x010, 7, 34},{0x011, 7, 18},{0x012, 7, 10},{0x013, 7,  6},
	{0x014, 7, 33},{0x015, 7, 17},{0x016, 7,  9},{0x017, 7,  5},
};

/* CBP_9: 35 codes, window 9 bits */
static const VLCcode VLC_CBP_9[] =
{
	{0x004, 8, 58},{0x005, 8, 54},{0x006, 8, 46},{0x007, 8, 30},{0x008, 8, 57},
	{0x009, 8, 53},{0x00a, 8, 45},{0x00b, 8, 29},{0x00c, 8, 38},{0x00d, 8, 26},
	{0x00e, 8, 37},{0x00f, 8, 25},{0x010, 8, 43},{0x011, 8, 23},{0x012, 8, 51},
	{0x013, 8, 15},{0x014, 8, 42},{0x015, 8, 22},{0x016, 8, 50},{0x017, 8, 14},
	{0x018, 8, 41},{0x019, 8, 21},{0x01a, 8, 49},{0x01b, 8, 13},{0x01c, 8, 35},
	{0x01d, 8, 19},{0x01e, 8, 11},{0x01f, 8,  7},{0x001, 9,  0},{0x002, 9, 39},
	{0x003, 9, 27},{0x004, 9, 59},{0x005, 9, 55},{0x006, 9, 47},{0x007, 9, 31},
	
};

/* MBA5: 7 codes, window 5 bits */
static const VLCcode VLC_MBA5[] =
{
	{0x001, 1,  0},{0x002, 3,  2},{0x003, 3,  1},{0x002, 4,  4},{0x003, 4,  3},
	{0x002, 5,  6},{0x003, 5,  5},
};

/* MBA11: 26 codes, window 11 bits */
static const VLCcode VLC_MBA11[] =
{
	{0x006, 7,  8},{0x007, 7,  7},{0x006, 8, 14},{0x007, 8, 13},{0x008, 8, 12},
	{0x009, 8, 11},{0x00a, 8, 10},{0x00b, 8,  9},{0x012,10, 20},{0x013,10, 19},
	{0x014,10, 18},{0x015,10, 17},{0x016,10, 16},{0x017,10, 15},{0x018,11, 32},
	{0x019,11, 31},{0x01a,11, 30},{0x01b,11, 29},{0x01c,11, 28},{0x01d,11, 27},
	{0x01e,11, 26},{0x01f,11, 25},{0x020,11, 24},{0x021,11, 23},{0x022,11, 22},
	{0x023,11, 21},
};

/* DC_LUM0: 7 codes, window 5 bits */
static const VLCcode VLC_DC_LUM0[] =
{
	{0x000, 2,  1},{0x001, 2,  2},{0x004, 3,  0},{0x005, 3,  3},{0x006, 3,  4},
	{0x00e, 4,  5},{0x01e, 5,  6},
};

/* DC_LUM1: 5 codes, window 9 bits */
static const VLCcode VLC_DC_LUM1[] =
{
	{0x03e, 6,  7},{0x07e, 7,  8},{0x0fe, 8,  9},{0x1fe, 9, 10},{0x1ff, 9, 11},
	
};

/* DC_CHROM0: 6 codes, window 5 bits */
static const VLCcode VLC_DC_CHROM0[] =
{
	{0x000, 2,  0},{0x001, 2,  1},{0x002, 2,  2},{0x006, 3,  3},{0x00e, 4,  4},
	{0x01e, 5,  5},
};

/* DC_CHROM1: 6 codes, window 10 bits */
static const VLCcode VLC_DC_CHROM1[] =
{
	{0x03e, 6,  6},{0x07e, 7,  7},{0x0fe, 8,  8},{0x1fe, 9,  9},{0x3fe,10, 10},
	{0x3ff,10, 11},
};

