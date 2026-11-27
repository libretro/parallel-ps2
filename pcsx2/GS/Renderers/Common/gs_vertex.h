/* SPDX-License-Identifier: GPL-3.0+ */
#ifndef GS_VERTEX_H
#define GS_VERTEX_H

/* The GS vertex record and the operations on it, as C89 with a body per
 * instruction set.
 *
 * The record is 32 bytes and its layout is the one the whole GS agrees on --
 * the GIF register handlers build it, VertexKick stores it, the vertex trace
 * reads it and the HW renderers upload it -- so the layout here is fixed and
 * pinned by tests, not chosen.
 *
 *     bytes  0..7   ST      S, T            two floats
 *     bytes  8..15  RGBAQ   R, G, B, A, Q   four bytes then a float
 *     bytes 16..23  XYZ     X, Y            two u16, then Z as u32
 *     bytes 24..27  UV      U, V            two u16
 *     bytes 28..31  FOG                     u32
 *
 * Backends: SSE2, SSE4.1, AVX and NEON. Nothing here reaches for GSVector,
 * which is SSE4.1-only as a class and so cannot express an SSE2 backend at
 * all; these use the intrinsics directly, which is also what lets the file
 * compile as C89.
 *
 * Everything here resolves at compile time. A field accessor is two or three
 * instructions and the store is one or two, so reaching any of them through
 * a function pointer would cost several times what the work itself does;
 * run-time dispatch only pays where one call covers many vertices, and the
 * GS has no such caller -- the vertex trace fuses colour, texture and
 * position into a single pass of its own.
 *
 * Shape note: written in the C89 form the emitters in this tree use --
 * declarations at the head of each block, no mixed declarations, no early
 * returns in the middle of a body, block comments.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(_MSC_VER)
#define GS_VERTEX_INLINE __forceinline
#elif defined(__GNUC__)
#define GS_VERTEX_INLINE __inline__ __attribute__((always_inline))
#else
#define GS_VERTEX_INLINE
#endif

/* ------------------------------------------------------------------ */
/* Which instruction sets this build can emit at all.                   */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || \
    defined(__i386__) || defined(_M_IX86)
#define GS_VERTEX_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
/* What this build may emit. Not keyed on the compiler: MSVC defines no
 * SSE4.1 macro of its own, and treating "is MSVC" as "has SSE4.1" would put
 * pmovzx in a baseline x64 build, which is an SSE2 target. */
#if defined(_MSC_VER) && !defined(__clang__)
/* cl.exe predefines no SSE4.1 macro and checks no target feature, so the
 * project's ladder is both the only statement of intent available and one
 * it will honour. */
#if defined(_M_SSE) && _M_SSE >= 0x401
#define GS_VERTEX_CAN_SSE41 1
#endif
#if defined(_M_SSE) && _M_SSE >= 0x500
#define GS_VERTEX_CAN_AVX 1
#endif
#else
/* Everywhere else the compiler's own macros are the ground truth for what
 * this translation unit may emit, and refusing an intrinsic above them is
 * an error rather than a slower build. VectorIntrin.h derives _M_SSE from
 * these same macros, so in-tree the two agree; where a build forces _M_SSE
 * by hand they need not, and the compiler wins. */
#if defined(__SSE4_1__) || defined(__AVX__)
#define GS_VERTEX_CAN_SSE41 1
#endif
#if defined(__AVX__)
#define GS_VERTEX_CAN_AVX 1
#endif
#endif
#ifndef GS_VEC4I_TYPEDEF
#define GS_VEC4I_TYPEDEF
typedef __m128i gs_vec4i; /* (gs_vector.h's too: C89 takes one) */
#endif
typedef __m128  gs_vec4f;
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#define GS_VERTEX_NEON 1
#include <arm_neon.h>
#ifndef GS_VEC4I_TYPEDEF
#define GS_VEC4I_TYPEDEF
typedef int32x4_t   gs_vec4i;
#endif
typedef float32x4_t gs_vec4f;
#else
#define GS_VERTEX_SCALAR 1
typedef struct { int32_t v[4]; } gs_vec4i;
typedef struct { float   v[4]; } gs_vec4f;
#endif

/* ------------------------------------------------------------------ */
/* The record.                                                          */
/* ------------------------------------------------------------------ */

struct gs_vertex_st    { float S; float T; };
struct gs_vertex_rgbaq { uint8_t R, G, B, A; float Q; };
struct gs_vertex_xyz   { uint16_t X, Y; uint32_t Z; };

struct gs_vertex_fields
{
   struct gs_vertex_st    ST;
   struct gs_vertex_rgbaq RGBAQ;
   struct gs_vertex_xyz   XYZ;
   uint16_t               U, V;
   uint32_t               FOG;
};

/* 32-byte alignment is load-bearing: it is what lets the whole record move
 * in one instruction where the host has a 32-byte one. */
#if defined(_MSC_VER)
#define GS_VERTEX_ALIGN32 __declspec(align(32))
#elif defined(__GNUC__)
#define GS_VERTEX_ALIGN32 __attribute__((aligned(32)))
#else
#define GS_VERTEX_ALIGN32
#endif

union GS_VERTEX_ALIGN32 gs_vertex
{
   struct gs_vertex_fields f;
   gs_vec4i               m[2];
   uint32_t               w[8];
   uint8_t                b[32];
};

/* ------------------------------------------------------------------ */
/* Per-vertex store. dst must be 32-byte aligned, which any object of    */
/* this union type is.                                                   */
/*                                                                       */
/* The hot one: VertexKick runs this for every vertex the GIF delivers.  */
/* Compile-time, deliberately: a call to reach a wider store costs more  */
/* than the store saves at this size.                                    */
/*                                                                       */
/* On AVX this loads two halves and stores one 32-byte line. Never a     */
/* 32-byte load: a caller typically fills half the record immediately    */
/* before storing it -- the GIF handlers assign m_v.m[1] an instruction  */
/* before VertexKick -- and a 32-byte load spanning a just-written half  */
/* cannot be store-forwarded, so it stalls until that store reaches L1.  */
/* Two narrow loads forward cleanly and still leave one store to retire. */
/* ------------------------------------------------------------------ */

static GS_VERTEX_INLINE void gs_vertex_store(
      union gs_vertex *dst, const union gs_vertex *src)
{
#if defined(GS_VERTEX_CAN_AVX)
   _mm256_store_si256((__m256i *)dst,
         _mm256_set_m128i(_mm_load_si128(((const __m128i *)src) + 1),
                          _mm_load_si128((const __m128i *)src)));
#elif defined(GS_VERTEX_X86)
   _mm_store_si128((__m128i *)dst,       _mm_load_si128((const __m128i *)src));
   _mm_store_si128(((__m128i *)dst) + 1, _mm_load_si128(((const __m128i *)src) + 1));
#elif defined(GS_VERTEX_NEON)
   vst1q_s32((int32_t *)dst,       vld1q_s32((const int32_t *)src));
   vst1q_s32(((int32_t *)dst) + 4, vld1q_s32(((const int32_t *)src) + 4));
#else
   memcpy(dst, src, 32);
#endif
}

/* ------------------------------------------------------------------ */
/* Field accessors.                                                     */
/*                                                                      */
/* Each yields the same four lanes the GS code expects, and each is two  */
/* or three instructions -- which is exactly why none of them dispatches */
/* through a pointer. Where SSE4.1 has a shorter encoding for one, it is */
/* taken; the SSE2 body is the contract.                                 */
/* ------------------------------------------------------------------ */

/* { X, Y, X, Y }, zero-extended from u16. */
static GS_VERTEX_INLINE gs_vec4i gs_vertex_xy(const union gs_vertex *v)
{
#if defined(GS_VERTEX_CAN_SSE41)
   /* pmovzxwd takes the low four u16 straight to u32.
    *
    * Reading the whole of m[1], not just the 8 bytes that hold X,Y: a
    * narrow load measures better in isolation but worse where it is
    * actually called. GSState fetches XY, UV, Z and FOG from one vertex in
    * one expression, so m[1] wants to be loaded once and shuffled four
    * ways; a per-accessor narrow load cannot be shared and costs more than
    * the folding saves. */
   return _mm_shuffle_epi32(_mm_cvtepu16_epi32(v->m[1]), 0x44);
#elif defined(GS_VERTEX_X86)
   return _mm_shuffle_epi32(_mm_unpacklo_epi16(v->m[1], _mm_setzero_si128()), 0x44);
#elif defined(GS_VERTEX_NEON)
   {
      uint32x4_t w = vmovl_u16(vget_low_u16(vreinterpretq_u16_s32(v->m[1])));
      int32x2_t  l = vget_low_s32(vreinterpretq_s32_u32(w));
      return vcombine_s32(l, l);
   }
#else
   {
      gs_vec4i r;
      r.v[0] = (int32_t)v->f.XYZ.X; r.v[1] = (int32_t)v->f.XYZ.Y;
      r.v[2] = r.v[0];              r.v[3] = r.v[1];
      return r;
   }
#endif
}

/* { U, V, U, V }, zero-extended from u16. No pmovzx reaches the high half,
 * so every backend unpacks here. */
static GS_VERTEX_INLINE gs_vec4i gs_vertex_uv(const union gs_vertex *v)
{
   /* Shares the m[1] load with gs_vertex_xy/z/fog -- see the note there. */
#if defined(GS_VERTEX_X86)
   return _mm_shuffle_epi32(_mm_unpackhi_epi16(v->m[1], _mm_setzero_si128()), 0x44);
#elif defined(GS_VERTEX_NEON)
   {
      uint32x4_t w = vmovl_u16(vget_high_u16(vreinterpretq_u16_s32(v->m[1])));
      int32x2_t  l = vget_low_s32(vreinterpretq_s32_u32(w));
      return vcombine_s32(l, l);
   }
#else
   {
      gs_vec4i r;
      r.v[0] = (int32_t)v->f.U; r.v[1] = (int32_t)v->f.V;
      r.v[2] = r.v[0];          r.v[3] = r.v[1];
      return r;
   }
#endif
}

/* { Z, Z, Z, Z } */
static GS_VERTEX_INLINE gs_vec4i gs_vertex_z(const union gs_vertex *v)
{
#if defined(GS_VERTEX_X86)
   return _mm_shuffle_epi32(v->m[1], 0x55);
#elif defined(GS_VERTEX_NEON)
   return vdupq_laneq_s32(v->m[1], 1);
#else
   {
      gs_vec4i r;
      r.v[0] = (int32_t)v->f.XYZ.Z; r.v[1] = r.v[0];
      r.v[2] = r.v[0];              r.v[3] = r.v[0];
      return r;
   }
#endif
}

/* { FOG, FOG, FOG, FOG } */
static GS_VERTEX_INLINE gs_vec4i gs_vertex_fog(const union gs_vertex *v)
{
#if defined(GS_VERTEX_X86)
   return _mm_shuffle_epi32(v->m[1], 0xff);
#elif defined(GS_VERTEX_NEON)
   return vdupq_laneq_s32(v->m[1], 3);
#else
   {
      gs_vec4i r;
      r.v[0] = (int32_t)v->f.FOG; r.v[1] = r.v[0];
      r.v[2] = r.v[0];            r.v[3] = r.v[0];
      return r;
   }
#endif
}

/* { R, G, B, A } as four int32. */
static GS_VERTEX_INLINE gs_vec4i gs_vertex_rgba(const union gs_vertex *v)
{
#if defined(GS_VERTEX_CAN_SSE41)
   /* The four bytes sit at offset 8; one pmovzxbd covers the whole widen. */
   return _mm_cvtepu8_epi32(_mm_cvtsi32_si128((int)v->w[2]));
#elif defined(GS_VERTEX_X86)
   {
      __m128i z = _mm_setzero_si128();
      return _mm_unpacklo_epi16(_mm_unpackhi_epi8(v->m[0], z), z);
   }
#elif defined(GS_VERTEX_NEON)
   {
      uint8x8_t  b  = vreinterpret_u8_u32(vdup_n_u32(v->w[2]));
      uint16x8_t w  = vmovl_u8(b);
      return vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(w)));
   }
#else
   {
      gs_vec4i r;
      r.v[0] = v->f.RGBAQ.R; r.v[1] = v->f.RGBAQ.G;
      r.v[2] = v->f.RGBAQ.B; r.v[3] = v->f.RGBAQ.A;
      return r;
   }
#endif
}

/* { S, T, S, T } */
static GS_VERTEX_INLINE gs_vec4f gs_vertex_st(const union gs_vertex *v)
{
#if defined(GS_VERTEX_X86)
   /* Both spellings below are the same instruction -- pshufd $0x44, which
    * takes its operand straight from memory -- but each compiler keeps one
    * and canonicalises the other into movaps + movlhps, which is a whole
    * instruction more because movlhps cannot fold the load. So pick the
    * spelling each one leaves alone. */
#if defined(__clang__)
   return _mm_castsi128_ps(_mm_unpacklo_epi64(v->m[0], v->m[0]));
#else
   return _mm_castsi128_ps(_mm_shuffle_epi32(v->m[0], 0x44));
#endif
#elif defined(GS_VERTEX_NEON)
   {
      float32x4_t f = vreinterpretq_f32_s32(v->m[0]);
      float32x2_t l = vget_low_f32(f);
      return vcombine_f32(l, l);
   }
#else
   {
      gs_vec4f r;
      r.v[0] = v->f.ST.S; r.v[1] = v->f.ST.T;
      r.v[2] = r.v[0];    r.v[3] = r.v[1];
      return r;
   }
#endif
}

/* { Q, Q, Q, Q } */
static GS_VERTEX_INLINE gs_vec4f gs_vertex_q(const union gs_vertex *v)
{
#if defined(GS_VERTEX_X86)
   /* Same reason as gs_vertex_st: pshufd folds the load, shufps does not. */
   return _mm_castsi128_ps(_mm_shuffle_epi32(v->m[0], 0xff));
#elif defined(GS_VERTEX_NEON)
   return vdupq_laneq_f32(vreinterpretq_f32_s32(v->m[0]), 3);
#else
   {
      gs_vec4f r;
      r.v[0] = v->f.RGBAQ.Q; r.v[1] = r.v[0];
      r.v[2] = r.v[0];       r.v[3] = r.v[0];
      return r;
   }
#endif
}

#endif
