/*
 * The software renderer's generated code, in C: what the generators need
 * of the scanline environment (GSScanlineEnvironment.h), and the
 * generators themselves.
 *
 * The selector is read from its key by bit position. The per-primitive
 * data (GSScanlineLocalData) has two layouts: the AVX2 build's eight-pixel
 * one (GS_SL8_*) and the four-pixel one of the others (GS_SL4_*); the
 * global data's last fields likewise (GS_SLG8_*, GS_SLG4_*). Each tier's
 * GSDrawScanline.c includes GSSwJitLayout.h, which holds these offsets to
 * the structs'.
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
#define GS_SEL_FPSM(k)   GS_SEL_BITS(k, 0, 2)
#define GS_SEL_ZPSM(k)   GS_SEL_BITS(k, 2, 2)
#define GS_SEL_ZTST(k)   GS_SEL_BITS(k, 4, 2)
#define GS_SEL_ATST(k)   GS_SEL_BITS(k, 6, 3)
#define GS_SEL_AFAIL(k)  GS_SEL_BITS(k, 9, 2)
#define GS_SEL_IIP(k)    GS_SEL_BITS(k, 11, 1)
#define GS_SEL_TFX(k)    GS_SEL_BITS(k, 12, 3)
#define GS_SEL_TCC(k)    GS_SEL_BITS(k, 15, 1)
#define GS_SEL_FST(k)    GS_SEL_BITS(k, 16, 1)
#define GS_SEL_LTF(k)    GS_SEL_BITS(k, 17, 1)
#define GS_SEL_TLU(k)    GS_SEL_BITS(k, 18, 1)
#define GS_SEL_FGE(k)    GS_SEL_BITS(k, 19, 1)
#define GS_SEL_DATE(k)   GS_SEL_BITS(k, 20, 1)
#define GS_SEL_ABE(k)    GS_SEL_BITS(k, 21, 1)
#define GS_SEL_ABA(k)    GS_SEL_BITS(k, 22, 2)
#define GS_SEL_ABB(k)    GS_SEL_BITS(k, 24, 2)
#define GS_SEL_ABC(k)    GS_SEL_BITS(k, 26, 2)
#define GS_SEL_ABD(k)    GS_SEL_BITS(k, 28, 2)
#define GS_SEL_PABE(k)   GS_SEL_BITS(k, 30, 1)
#define GS_SEL_AA1(k)    GS_SEL_BITS(k, 31, 1)
#define GS_SEL_FWRITE(k) GS_SEL_BITS(k, 32, 1)
#define GS_SEL_FB(k)     GS_SEL_BITS(k, 32, 2) /* fwrite, ftest */
#define GS_SEL_RFB(k)    GS_SEL_BITS(k, 34, 1)
#define GS_SEL_ZWRITE(k) GS_SEL_BITS(k, 35, 1)
#define GS_SEL_ZB(k)     GS_SEL_BITS(k, 35, 2) /* zwrite, ztest */
#define GS_SEL_ZTEST(k)  GS_SEL_BITS(k, 36, 1)
#define GS_SEL_ZOVERFLOW(k) GS_SEL_BITS(k, 37, 1)
#define GS_SEL_ZCLAMP(k) GS_SEL_BITS(k, 38, 1)
#define GS_SEL_WMS(k)    GS_SEL_BITS(k, 39, 2)
#define GS_SEL_WMT(k)    GS_SEL_BITS(k, 41, 2)
#define GS_SEL_DATM(k)   GS_SEL_BITS(k, 43, 1)
#define GS_SEL_COLCLAMP(k) GS_SEL_BITS(k, 44, 1)
#define GS_SEL_FBA(k)    GS_SEL_BITS(k, 45, 1)
#define GS_SEL_DTHE(k)   GS_SEL_BITS(k, 46, 1)
#define GS_SEL_PRIM(k)   GS_SEL_BITS(k, 47, 2)
#define GS_SEL_EDGE(k)   GS_SEL_BITS(k, 49, 1)
#define GS_SEL_TW(k)     GS_SEL_BITS(k, 50, 3)
#define GS_SEL_LCM(k)    GS_SEL_BITS(k, 53, 1)
#define GS_SEL_MMIN(k)   GS_SEL_BITS(k, 54, 2)
#define GS_SEL_NOTEST(k) GS_SEL_BITS(k, 56, 1)
#define GS_SEL_ZEQUAL(k) GS_SEL_BITS(k, 57, 1)
#define GS_SEL_HIRES(k)  GS_SEL_BITS(k, 58, 1)
#define GS_SEL_TW_HI(k)  GS_SEL_BITS(k, 59, 1)

/* (GS_PRIM_CLASS and the TFX the generators test) */
#define GS_SWJ_POINT_CLASS    0
#define GS_SWJ_LINE_CLASS     1
#define GS_SWJ_TRIANGLE_CLASS 2
#define GS_SWJ_SPRITE_CLASS   3
#define GS_SWJ_TFX_MODULATE   0
#define GS_SWJ_TFX_DECAL      1
#define GS_SWJ_TFX_HIGHLIGHT  2
#define GS_SWJ_TFX_HIGHLIGHT2 3
#define GS_SWJ_TFX_NONE       4

/* (GSRegs.h's ZTST, ATST and AFAIL the scanline code tests) */
#define GS_SWJ_ZTST_GEQUAL    2
#define GS_SWJ_ZTST_GREATER   3
#define GS_SWJ_ATST_NEVER     0
#define GS_SWJ_ATST_ALWAYS    1
#define GS_SWJ_ATST_LESS      2
#define GS_SWJ_ATST_LEQUAL    3
#define GS_SWJ_ATST_EQUAL     4
#define GS_SWJ_ATST_GEQUAL    5
#define GS_SWJ_ATST_GREATER   6
#define GS_SWJ_ATST_NOTEQUAL  7
#define GS_SWJ_AFAIL_KEEP     0
#define GS_SWJ_AFAIL_FB_ONLY  1
#define GS_SWJ_AFAIL_ZB_ONLY  2
#define GS_SWJ_AFAIL_RGB_ONLY 3
/* (HALF_VM_SIZE) */
#define GS_SWJ_HALF_VM_SIZE   0x200000u

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
	GS_SL8_TGRID    = 2176,
	GS_SL8_TGRID_RCP = 2192,
	GS_SL8_ZGRID_GRID = 2208,
	GS_SL8_ZGRID_RCP = 2216,
	GS_SL8_TEMP_Z0  = 2240,
	GS_SL8_TEMP_ZS  = 2496,
	GS_SL8_TEMP_ZD  = 2528,
	GS_SL8_TEMP_VF  = 2592,
	GS_SL8_TEMP_COV = 2624,
	GS_SL8_TEMP_LOD_I = 2656,
	GS_SL8_TEMP_LOD_F = 2688,
	GS_SL8_TEMP_UV0 = 2720,
	GS_SL8_TEMP_UV1 = 2752,
	GS_SL8_TEMP_UVMM0 = 2784,
	GS_SL8_TEMP_UVMM1 = 2816,
	GS_SL8_TEMP_TRB = 2848,
	GS_SL8_TEMP_TGA = 2880,
	GS_SL8_GD       = 2944
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
	GS_SL4_P_F    = 624,
	GS_SL4_TGRID  = 640,
	GS_SL4_TGRID_RCP = 656,
	GS_SL4_ZGRID_GRID = 672,
	GS_SL4_ZGRID_RCP = 680,
	GS_SL4_TEMP_Z0 = 688,
	GS_SL4_TEMP_ZS = 816,
	GS_SL4_TEMP_ZD = 832,
	GS_SL4_TEMP_VF = 864,
	GS_SL4_TEMP_COV = 880,
	GS_SL4_TEMP_LOD_I = 896,
	GS_SL4_TEMP_LOD_F = 912,
	GS_SL4_TEMP_UV0 = 928,
	GS_SL4_TEMP_UV1 = 944,
	GS_SL4_TEMP_UVMM0 = 960,
	GS_SL4_TEMP_UVMM1 = 976,
	GS_SL4_TEMP_TRB = 992,
	GS_SL4_TEMP_TGA = 1008,
	GS_SL4_GD     = 1040
};

/* --- GSScanlineGlobalData: the same in both builds ---------------------- */
enum
{
	GS_SLG_VM       = 8,
	GS_SLG_TEX      = 16,
	GS_SLG_CLUT     = 72,
	GS_SLG_DIMX     = 80,
	GS_SLG_FZBR     = 248,
	GS_SLG_FZBC     = 256,
	GS_SLG_FBASE    = 264,
	GS_SLG_ZBASE    = 268,
	GS_SLG_AREF     = 272,
	GS_SLG_AFIX     = 288,
	GS_SLG_T_MIN    = 304,
	GS_SLG_T_MAX    = 320,
	GS_SLG_T_MINMAX = 336,
	GS_SLG_T_MASK   = 352
};

/* ... and per build: eight pixels a step (AVX2), and four */
enum
{
	GS_SLG8_FM = 384, GS_SLG8_ZM = 388, GS_SLG8_FRB = 392, GS_SLG8_FGA = 396,
	GS_SLG8_MXL = 416, GS_SLG8_L = 448, GS_SLG8_LOD_C3 = 480, GS_SLG8_LOD_C2 = 512,
	GS_SLG8_LOD_C1 = 544, GS_SLG8_LOD_C0 = 576, GS_SLG8_LOD_I = 608, GS_SLG8_LOD_F = 640
};
enum
{
	GS_SLG4_FM = 384, GS_SLG4_ZM = 400, GS_SLG4_FRB = 416, GS_SLG4_FGA = 432,
	GS_SLG4_MXL = 448, GS_SLG4_L = 464, GS_SLG4_LOD_C3 = 480, GS_SLG4_LOD_C2 = 496,
	GS_SLG4_LOD_C1 = 512, GS_SLG4_LOD_C0 = 528, GS_SLG4_LOD_I = 544, GS_SLG4_LOD_F = 560
};

/* --- GSScanlineConstantData (g_const) ------------------------------------ */
enum
{
	GS_SLC_TEST_256B  = 0,
	GS_SLC_SHIFT_256B = 128,
	GS_SLC_ONE_256B   = 416,
	GS_SLC_TEST_128B  = 448,
	GS_SLC_SHIFT_128B = 576,
	GS_SLC_ONE_128B   = 656
};

/* What a generator is asked for: the selector, the instruction set (AVX
 * encodings, AVX2 instructions; the eight-pixel layout and ymm
 * registers), the calling convention, and where the constants are:
 * g_const's shift tables, g_const itself, and
 * GSVector4::m_xc1e00000000fffff (the scanline's z overflow bias) */
typedef struct gs_swj_target
{
	u64 key;
	int avx;
	int avx2;
	int ymm;
	int win64;
	const void* shift_128b;
	const void* shift_256b;
	const void* consts;
	const void* zbias;
} gs_swj_target;

/* The calling convention this build's code is called with */
#ifdef _WIN32
#define GS_SWJ_WIN64 1
#else
#define GS_SWJ_WIN64 0
#endif

/* The setup of a primitive's steps, generated
 * into room bytes at code: its size, or room when it did not fit */
size_t gs_swj_setup_prim(const gs_swj_target* t, void* code, size_t room);

/* A scanline's pixels, likewise. Called as
 * void (*)(int pixels, int left, int top, const GSVertexSW* scan,
 * GSScanlineLocalData* local). */
size_t gs_swj_draw_scanline(const gs_swj_target* t, void* code, size_t room);

#ifdef __cplusplus
}
#endif

#endif
