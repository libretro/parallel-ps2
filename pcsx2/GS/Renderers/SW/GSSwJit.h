/*
 * The software renderer's generated code, in C: what the generators need
 * of the scanline environment (GSScanlineEnvironment.h) without its C++
 * types, and the generators themselves.
 *
 * The selector is read from its key by bit position. The per-primitive
 * data (GSScanlineLocalData) has two layouts: the AVX2 build's eight-pixel
 * one (GS_SL8_*) and the four-pixel one of the others (GS_SL4_*); each
 * tier's GSSetupPrimCodeGenerator.cpp holds these offsets to the C++
 * struct's.
 */

#ifndef GS_SW_JIT_H
#define GS_SW_JIT_H

#include <stddef.h>

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- the selector (GSScanlineSelector), by bit -------------------------- */
#define GS_SEL_BITS(key, pos, n) ((unsigned)(((key) >> (pos)) & ((1u << (n)) - 1)))
#define GS_SEL_IIP(k)    GS_SEL_BITS(k, 11, 1)
#define GS_SEL_TFX(k)    GS_SEL_BITS(k, 12, 3)
#define GS_SEL_TCC(k)    GS_SEL_BITS(k, 15, 1)
#define GS_SEL_FST(k)    GS_SEL_BITS(k, 16, 1)
#define GS_SEL_FGE(k)    GS_SEL_BITS(k, 19, 1)
#define GS_SEL_FB(k)     GS_SEL_BITS(k, 32, 2) /* fwrite, ftest */
#define GS_SEL_ZB(k)     GS_SEL_BITS(k, 35, 2) /* zwrite, ztest */
#define GS_SEL_PRIM(k)   GS_SEL_BITS(k, 47, 2)
#define GS_SEL_NOTEST(k) GS_SEL_BITS(k, 56, 1)

/* (GS_PRIM_CLASS and the TFX the generators test) */
#define GS_SWJ_POINT_CLASS    0
#define GS_SWJ_LINE_CLASS     1
#define GS_SWJ_TRIANGLE_CLASS 2
#define GS_SWJ_SPRITE_CLASS   3
#define GS_SWJ_TFX_DECAL      1
#define GS_SWJ_TFX_NONE       4

/* --- GSVertexSW: p, _pad, t, c, a vector each --------------------------- */
enum
{
	GS_VSW_P    = 0,
	GS_VSW_T    = 32,
	GS_VSW_C    = 48,
	GS_VSW_SIZE = 64
};

/* --- GSScanlineLocalData, eight pixels a step (AVX2) -------------------- */
enum
{
	GS_SL8_SKIP     = 256, /* sizeof d[0] */
	GS_SL8_D_Z0     = 0,
	GS_SL8_D_Z1     = 32,
	GS_SL8_D_S      = 64,
	GS_SL8_D_T      = 96,
	GS_SL8_D_Q      = 128,
	GS_SL8_D_RB     = 160,
	GS_SL8_D_GA     = 192,
	GS_SL8_D_F      = 224,
	GS_SL8_D8_STQ   = 2048,
	GS_SL8_D8_C     = 2064,
	GS_SL8_D8_P_Z   = 2072,
	GS_SL8_D8_P_F   = 2080,
	GS_SL8_P_Z      = 2096,
	GS_SL8_P_F      = 2100,
	GS_SL8_C_RB     = 2112,
	GS_SL8_C_GA     = 2144,
	GS_SL8_TEMP_ZS  = 2496,
	GS_SL8_TEMP_ZD  = 2528,
	GS_SL8_TEMP_LOD_I = 2656,
	GS_SL8_TEMP_LOD_F = 2688,
	GS_SL8_TEMP_UV0 = 2720,
	GS_SL8_TEMP_UV1 = 2752,
	GS_SL8_TEMP_TRB = 2848,
	GS_SL8_TEMP_TGA = 2880
};

/* --- GSScanlineLocalData, four pixels a step ---------------------------- */
enum
{
	GS_SL4_SKIP   = 128,
	GS_SL4_D_Z0   = 0,
	GS_SL4_D_Z1   = 16,
	GS_SL4_D_S    = 32,
	GS_SL4_D_T    = 48,
	GS_SL4_D_Q    = 64,
	GS_SL4_D_RB   = 80,
	GS_SL4_D_GA   = 96,
	GS_SL4_D_F    = 112,
	GS_SL4_D4_Z   = 512,
	GS_SL4_D4_STQ = 528,
	GS_SL4_D4_C   = 544,
	GS_SL4_D4_F   = 560,
	GS_SL4_C_RB   = 576,
	GS_SL4_C_GA   = 592,
	GS_SL4_P_Z    = 608,
	GS_SL4_P_F    = 624
};

/* What a generator is asked for: the selector, the instruction set (AVX
 * encodings; the eight-pixel layout and ymm registers), the calling
 * convention, and where the constant shift tables are (g_const's
 * m_shift_128b and m_shift_256b) */
typedef struct gs_swj_target
{
	u64 key;
	int avx;
	int ymm;
	int win64;
	const void* shift_128b;
	const void* shift_256b;
} gs_swj_target;

/* The calling convention this build's code is called with */
#ifdef _WIN32
#define GS_SWJ_WIN64 1
#else
#define GS_SWJ_WIN64 0
#endif

/* The setup of a primitive's steps (GSSetupPrimCodeGenerator), generated
 * into room bytes at code: its size, or room when it did not fit */
size_t gs_swj_setup_prim(const gs_swj_target* t, void* code, size_t room);

#ifdef __cplusplus
}
#endif

#endif
