/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_VECTOR_H
#define GS_VECTOR_H

/* The GS 128-bit integer vector, as C89.
 *
 * gs_vec4i is the machine vector itself, not a class wrapping one, so an
 * operation is the intrinsic and nothing else. Operators become named
 * functions and the template parameters that carry an immediate become
 * macros, which is forced rather than chosen: pslld, pshufd and their
 * relatives take a literal, so the count has to survive to the instruction
 * as a constant expression.
 *
 * Four tiers, chosen the way gs_vertex.h chooses them:
 *
 *   SSE2    every operation, including the fourteen that GSVector4i takes
 *           from SSE4.1 with no fallback -- which is what makes SSE4.1 a
 *           hard floor for the GS today.
 *   SSE4.1  pminsd, pmaxsd, pblendvb, pmovzx, packusdw, ptest, pinsr/pextr.
 *   AVX     the same, VEX-encoded, three-operand.
 *   NEON    arm64, mirroring GSVector4i_arm64.h.
 *
 * Shape note: written in the C89 form the emitters in this tree use --
 * declarations at the head of each block, no mixed declarations, block
 * comments.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(_MSC_VER)
#define GS_VEC_INLINE __forceinline
#elif defined(__GNUC__)
#define GS_VEC_INLINE __inline__ __attribute__((always_inline))
#else
#define GS_VEC_INLINE
#endif

/* ------------------------------------------------------------------ */
/* Which instruction sets this build may emit. Same rule as gs_vertex:  */
/* the compiler's own macros decide wherever the compiler checks them,  */
/* and the project ladder decides on cl.exe, which checks nothing.      */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || \
    defined(__i386__) || defined(_M_IX86)
#define GS_VEC_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#if defined(_M_SSE) && _M_SSE >= 0x401
#define GS_VEC_CAN_SSE41 1
#endif
#if defined(_M_SSE) && _M_SSE >= 0x500
#define GS_VEC_CAN_AVX 1
#endif
#else
#if defined(__SSE4_1__) || defined(__AVX__)
#define GS_VEC_CAN_SSE41 1
#endif
#if defined(__AVX__)
#define GS_VEC_CAN_AVX 1
#endif
#endif
#ifndef GS_VEC4I_TYPEDEF
#define GS_VEC4I_TYPEDEF
typedef __m128i gs_vec4i; /* (gs_vertex.h's too: C89 takes one) */
#endif
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#define GS_VEC_NEON 1
#include <arm_neon.h>
#ifndef GS_VEC4I_TYPEDEF
#define GS_VEC4I_TYPEDEF
typedef int32x4_t gs_vec4i;
#endif
#else
#error gs_vector requires SSE2 or NEON
#endif

/* ------------------------------------------------------------------ */
/* Named view.                                                          */
/*                                                                      */
/* Most of what the GS does with a vector is read or write one lane by  */
/* name -- a rect's edges, a coordinate pair -- rather than operate on  */
/* all four. A bare vector cannot spell that, so this union gives the   */
/* same names the class carries while keeping the vector itself in v.   */
/* Reading a lane through it costs nothing: both compilers keep the     */
/* value in a register rather than round-tripping it through memory.    */
/* ------------------------------------------------------------------ */

union gs_v4i_view
{
   gs_vec4i v;
   int32_t  i32[4];
   uint32_t u32[4];
   int16_t  i16[8];
   uint16_t u16[8];
   int8_t   i8[16];
   uint8_t  u8[16];
   struct { int32_t x, y, z, w; } lane;
   struct { int32_t left, top, right, bottom; } rect;
};

/* ------------------------------------------------------------------ */
/* Load and store.                                                      */
/* ------------------------------------------------------------------ */

static GS_VEC_INLINE gs_vec4i gs_v4i_load(const void *p)
{
#if defined(GS_VEC_X86)
   return _mm_load_si128((const __m128i *)p);
#else
   return vld1q_s32((const int32_t *)p);
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_loadu(const void *p)
{
#if defined(GS_VEC_X86)
   return _mm_loadu_si128((const __m128i *)p);
#else
   return vld1q_s32((const int32_t *)p);
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_loadl(const void *p)
{
#if defined(GS_VEC_X86)
   return _mm_loadl_epi64((const __m128i *)p);
#else
   return vcombine_s32(vld1_s32((const int32_t *)p), vdup_n_s32(0));
#endif
}

static GS_VEC_INLINE void gs_v4i_store(void *p, gs_vec4i v)
{
#if defined(GS_VEC_X86)
   _mm_store_si128((__m128i *)p, v);
#else
   vst1q_s32((int32_t *)p, v);
#endif
}

static GS_VEC_INLINE void gs_v4i_storeu(void *p, gs_vec4i v)
{
#if defined(GS_VEC_X86)
   _mm_storeu_si128((__m128i *)p, v);
#else
   vst1q_s32((int32_t *)p, v);
#endif
}

static GS_VEC_INLINE void gs_v4i_storel(void *p, gs_vec4i v)
{
#if defined(GS_VEC_X86)
   _mm_storel_epi64((__m128i *)p, v);
#else
   vst1_s32((int32_t *)p, vget_low_s32(v));
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_zero(void)
{
#if defined(GS_VEC_X86)
   return _mm_setzero_si128();
#else
   return vdupq_n_s32(0);
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_ones(void)
{
#if defined(GS_VEC_X86)
   /* pcmpeqd against itself: no constant to load. */
   gs_vec4i z = _mm_setzero_si128();
   return _mm_cmpeq_epi32(z, z);
#else
   return vdupq_n_s32(-1);
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_set32(int32_t x)
{
#if defined(GS_VEC_X86)
   return _mm_set1_epi32(x);
#else
   return vdupq_n_s32(x);
#endif
}

/* ------------------------------------------------------------------ */
/* Arithmetic and logic.                                                */
/* ------------------------------------------------------------------ */

#if defined(GS_VEC_X86)
#define GS_V4I_BIN(name, sse, neon) \
   static GS_VEC_INLINE gs_vec4i name(gs_vec4i a, gs_vec4i b) { return sse(a, b); }
#else
#define GS_V4I_BIN(name, sse, neon) \
   static GS_VEC_INLINE gs_vec4i name(gs_vec4i a, gs_vec4i b) { return neon; }
#endif

GS_V4I_BIN(gs_v4i_add32, _mm_add_epi32, vaddq_s32(a, b))
GS_V4I_BIN(gs_v4i_sub32, _mm_sub_epi32, vsubq_s32(a, b))
GS_V4I_BIN(gs_v4i_and,   _mm_and_si128, vandq_s32(a, b))
GS_V4I_BIN(gs_v4i_or,    _mm_or_si128,  vorrq_s32(a, b))
GS_V4I_BIN(gs_v4i_xor,   _mm_xor_si128, veorq_s32(a, b))
GS_V4I_BIN(gs_v4i_eq32,  _mm_cmpeq_epi32,
           vreinterpretq_s32_u32(vceqq_s32(a, b)))
GS_V4I_BIN(gs_v4i_gt32,  _mm_cmpgt_epi32,
           vreinterpretq_s32_u32(vcgtq_s32(a, b)))
GS_V4I_BIN(gs_v4i_eq16,  _mm_cmpeq_epi16,
           vreinterpretq_s32_u16(vceqq_s16(vreinterpretq_s16_s32(a), vreinterpretq_s16_s32(b))))

static GS_VEC_INLINE gs_vec4i gs_v4i_add16(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_add_epi16(a, b);
#else
   return vreinterpretq_s32_s16(vaddq_s16(vreinterpretq_s16_s32(a),
                                          vreinterpretq_s16_s32(b)));
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_sub16(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_sub_epi16(a, b);
#else
   return vreinterpretq_s32_s16(vsubq_s16(vreinterpretq_s16_s32(a),
                                          vreinterpretq_s16_s32(b)));
#endif
}

/* ------------------------------------------------------------------ */
/* The three the reverb resampler's FIR is made of: saturating 16-bit    */
/* add, saturating add of neighbouring lanes, and the rounded high half  */
/* of a 16-bit product. The last two are SSSE3 instructions, so the      */
/* SSE2 tier builds them from mullo/mulhi and a gather.                  */
/* ------------------------------------------------------------------ */

static GS_VEC_INLINE gs_vec4i gs_v4i_adds16(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_adds_epi16(a, b);
#else
   return vreinterpretq_s32_s16(vqaddq_s16(vreinterpretq_s16_s32(a),
                                           vreinterpretq_s16_s32(b)));
#endif
}

#if defined(GS_VEC_X86) && !defined(GS_VEC_CAN_SSE41)
/* The low 16 bits of each 32-bit lane of a, then of b. _mm_packs_epi32
 * would saturate, which is not what either caller wants, so gather the
 * halves by shuffle instead. */
static GS_VEC_INLINE gs_vec4i gs_v4i_pack_lo16(gs_vec4i a, gs_vec4i b)
{
   a = _mm_shufflelo_epi16(a, _MM_SHUFFLE(3, 1, 2, 0));
   a = _mm_shufflehi_epi16(a, _MM_SHUFFLE(3, 1, 2, 0));
   a = _mm_shuffle_epi32(a, _MM_SHUFFLE(3, 1, 2, 0));
   b = _mm_shufflelo_epi16(b, _MM_SHUFFLE(3, 1, 2, 0));
   b = _mm_shufflehi_epi16(b, _MM_SHUFFLE(3, 1, 2, 0));
   b = _mm_shuffle_epi32(b, _MM_SHUFFLE(3, 1, 2, 0));
   return _mm_unpacklo_epi64(a, b);
}
#endif

/* Neighbouring 16-bit lanes added with saturation: a0+a1, a2+a3 ... then
 * the same for b. */
static GS_VEC_INLINE gs_vec4i gs_v4i_hadds16(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_hadds_epi16(a, b);
#elif defined(GS_VEC_X86)
   {
      gs_vec4i even = gs_v4i_pack_lo16(a, b);
      gs_vec4i odd  = gs_v4i_pack_lo16(_mm_srli_epi32(a, 16),
                                       _mm_srli_epi32(b, 16));
      return _mm_adds_epi16(even, odd);
   }
#else
   {
      int16x8_t x = vreinterpretq_s16_s32(a);
      int16x8_t y = vreinterpretq_s16_s32(b);
      return vreinterpretq_s32_s16(vqaddq_s16(vuzp1q_s16(x, y),
                                              vuzp2q_s16(x, y)));
   }
#endif
}

/* (a * b + 0x4000) >> 15 per 16-bit lane, keeping the low 16 bits. */
static GS_VEC_INLINE gs_vec4i gs_v4i_mul16hrs(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_mulhrs_epi16(a, b);
#elif defined(GS_VEC_X86)
   {
      __m128i lo   = _mm_mullo_epi16(a, b);
      __m128i hi   = _mm_mulhi_epi16(a, b);
      __m128i half = _mm_set1_epi32(0x4000);
      __m128i p0   = _mm_srai_epi32(
                        _mm_add_epi32(_mm_unpacklo_epi16(lo, hi), half), 15);
      __m128i p1   = _mm_srai_epi32(
                        _mm_add_epi32(_mm_unpackhi_epi16(lo, hi), half), 15);
      return gs_v4i_pack_lo16(p0, p1);
   }
#else
   {
      int16x8_t x = vreinterpretq_s16_s32(a);
      int16x8_t y = vreinterpretq_s16_s32(b);
      int32x4_t l = vmull_s16(vget_low_s16(x), vget_low_s16(y));
      int32x4_t h = vmull_s16(vget_high_s16(x), vget_high_s16(y));
      return vreinterpretq_s32_s16(vcombine_s16(vrshrn_n_s32(l, 15),
                                                vrshrn_n_s32(h, 15)));
   }
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_andnot(gs_vec4i a, gs_vec4i b)
{
   /* ~a & b, the andnps order: the x86 instruction negates its first
    * operand, NEON's bit-clear negates its second. */
#if defined(GS_VEC_X86)
   return _mm_andnot_si128(a, b);
#else
   return vbicq_s32(b, a);
#endif
}

/* ------------------------------------------------------------------ */
/* Min and max. The signed and unsigned 32-bit forms are SSE4.1; the    */
/* SSE2 shapes below are why this header can offer a tier the class     */
/* cannot.                                                              */
/* ------------------------------------------------------------------ */

static GS_VEC_INLINE gs_vec4i gs_v4i_min_i32(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_min_epi32(a, b);
#elif defined(GS_VEC_X86)
   {
      __m128i m = _mm_cmpgt_epi32(a, b);
      return _mm_or_si128(_mm_and_si128(m, b), _mm_andnot_si128(m, a));
   }
#else
   return vminq_s32(a, b);
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_max_i32(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_max_epi32(a, b);
#elif defined(GS_VEC_X86)
   {
      __m128i m = _mm_cmpgt_epi32(a, b);
      return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b));
   }
#else
   return vmaxq_s32(a, b);
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_min_u32(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_min_epu32(a, b);
#elif defined(GS_VEC_X86)
   {
      /* No unsigned compare before SSE4.1: bias both into signed space. */
      __m128i s = _mm_set1_epi32((int)0x80000000u);
      __m128i m = _mm_cmpgt_epi32(_mm_xor_si128(a, s), _mm_xor_si128(b, s));
      return _mm_or_si128(_mm_and_si128(m, b), _mm_andnot_si128(m, a));
   }
#else
   return vreinterpretq_s32_u32(vminq_u32(vreinterpretq_u32_s32(a),
                                          vreinterpretq_u32_s32(b)));
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_max_u32(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_max_epu32(a, b);
#elif defined(GS_VEC_X86)
   {
      __m128i s = _mm_set1_epi32((int)0x80000000u);
      __m128i m = _mm_cmpgt_epi32(_mm_xor_si128(a, s), _mm_xor_si128(b, s));
      return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b));
   }
#else
   return vreinterpretq_s32_u32(vmaxq_u32(vreinterpretq_u32_s32(a),
                                          vreinterpretq_u32_s32(b)));
#endif
}

/* ------------------------------------------------------------------ */
/* Unpack and widen.                                                    */
/* ------------------------------------------------------------------ */

static GS_VEC_INLINE gs_vec4i gs_v4i_upl16v(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_unpacklo_epi16(a, b);
#else
   return vreinterpretq_s32_s16(vzip1q_s16(vreinterpretq_s16_s32(a),
                                           vreinterpretq_s16_s32(b)));
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_uph16v(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_unpackhi_epi16(a, b);
#else
   return vreinterpretq_s32_s16(vzip2q_s16(vreinterpretq_s16_s32(a),
                                           vreinterpretq_s16_s32(b)));
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_upl32v(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_unpacklo_epi32(a, b);
#else
   return vzip1q_s32(a, b);
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_uph32v(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_unpackhi_epi32(a, b);
#else
   return vzip2q_s32(a, b);
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_upl64v(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_unpacklo_epi64(a, b);
#else
   return vreinterpretq_s32_s64(vzip1q_s64(vreinterpretq_s64_s32(a),
                                           vreinterpretq_s64_s32(b)));
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_upl8v(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_unpacklo_epi8(a, b);
#else
   return vreinterpretq_s32_s8(vzip1q_s8(vreinterpretq_s8_s32(a),
                                         vreinterpretq_s8_s32(b)));
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_uph8v(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_unpackhi_epi8(a, b);
#else
   return vreinterpretq_s32_s8(vzip2q_s8(vreinterpretq_s8_s32(a),
                                         vreinterpretq_s8_s32(b)));
#endif
}

/* Widening against zero. Spelled as the unpack rather than pmovzx even
 * where SSE4.1 is available: both compilers already fold this form into
 * pmovzx with the load attached, and the explicit intrinsic costs gcc a
 * separate load. The zero is also shared when several of these appear
 * together, which pmovzx cannot do. */
static GS_VEC_INLINE gs_vec4i gs_v4i_upl16(gs_vec4i a)
{
   return gs_v4i_upl16v(a, gs_v4i_zero());
}

static GS_VEC_INLINE gs_vec4i gs_v4i_uph16(gs_vec4i a)
{
   return gs_v4i_uph16v(a, gs_v4i_zero());
}

static GS_VEC_INLINE gs_vec4i gs_v4i_upl32(gs_vec4i a)
{
   return gs_v4i_upl32v(a, gs_v4i_zero());
}

static GS_VEC_INLINE gs_vec4i gs_v4i_uph32(gs_vec4i a)
{
   return gs_v4i_uph32v(a, gs_v4i_zero());
}

static GS_VEC_INLINE gs_vec4i gs_v4i_upl8(gs_vec4i a)
{
   return gs_v4i_upl8v(a, gs_v4i_zero());
}

static GS_VEC_INLINE gs_vec4i gs_v4i_uph8(gs_vec4i a)
{
   return gs_v4i_uph8v(a, gs_v4i_zero());
}

/* ------------------------------------------------------------------ */
/* Pack. packusdw is SSE4.1; the SSE2 shape saturates by hand.          */
/* ------------------------------------------------------------------ */

static GS_VEC_INLINE gs_vec4i gs_v4i_ps32(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_packs_epi32(a, b);
#else
   return vreinterpretq_s32_s16(vcombine_s16(vqmovn_s32(a), vqmovn_s32(b)));
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_pu32(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_packus_epi32(a, b);
#elif defined(GS_VEC_X86)
   {
      /* Clamp into 0..0xffff, bias down so the range fits a signed 16-bit
       * pack exactly, pack, then undo the bias -- flipping the top bit is
       * the whole of it, since the biased value spans the signed range. */
      __m128i hi   = _mm_set1_epi32(0xffff);
      __m128i bias = _mm_set1_epi32(0x8000);
      __m128i za   = _mm_and_si128(a, _mm_cmpgt_epi32(a, _mm_setzero_si128()));
      __m128i zb   = _mm_and_si128(b, _mm_cmpgt_epi32(b, _mm_setzero_si128()));
      __m128i ma   = _mm_cmpgt_epi32(za, hi);
      __m128i mb   = _mm_cmpgt_epi32(zb, hi);
      __m128i ca   = _mm_or_si128(_mm_and_si128(ma, hi), _mm_andnot_si128(ma, za));
      __m128i cb   = _mm_or_si128(_mm_and_si128(mb, hi), _mm_andnot_si128(mb, zb));

      return _mm_xor_si128(_mm_packs_epi32(_mm_sub_epi32(ca, bias),
                                           _mm_sub_epi32(cb, bias)),
                           _mm_set1_epi16((short)0x8000));
   }
#else
   return vreinterpretq_s32_u16(vcombine_u16(vqmovun_s32(a), vqmovun_s32(b)));
#endif
}

/* ------------------------------------------------------------------ */
/* Blend. pblendvb is SSE4.1; the SSE2 shape is the select it replaced. */
/* ------------------------------------------------------------------ */

static GS_VEC_INLINE gs_vec4i gs_v4i_blend8(gs_vec4i a, gs_vec4i b, gs_vec4i mask)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_blendv_epi8(a, b, mask);
#elif defined(GS_VEC_X86)
   {
      __m128i m = _mm_cmplt_epi8(mask, _mm_setzero_si128());
      return _mm_or_si128(_mm_and_si128(m, b), _mm_andnot_si128(m, a));
   }
#else
   /* Per byte, not per lane: a byte of the result comes from b when the
    * corresponding mask byte has its top bit set. */
   return vreinterpretq_s32_u8(
         vbslq_u8(vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_s32(mask), 7)),
                  vreinterpretq_u8_s32(b), vreinterpretq_u8_s32(a)));
#endif
}

/* ------------------------------------------------------------------ */
/* Immediate-carrying operations. Macros, because the instruction wants */
/* the count or selector as a literal.                                  */
/* ------------------------------------------------------------------ */

#if defined(GS_VEC_X86)

#define GS_V4I_SLL16(v, i)   _mm_slli_epi16((v), (i))
#define GS_V4I_SRL16(v, i)   _mm_srli_epi16((v), (i))
#define GS_V4I_SRA16(v, i)   _mm_srai_epi16((v), (i))
#define GS_V4I_SLL32(v, i)   _mm_slli_epi32((v), (i))
#define GS_V4I_SRL32(v, i)   _mm_srli_epi32((v), (i))
#define GS_V4I_SRA32(v, i)   _mm_srai_epi32((v), (i))
#define GS_V4I_SLL(v, i)     _mm_slli_si128((v), (i))
#define GS_V4I_SRL(v, i)     _mm_srli_si128((v), (i))
#define GS_V4I_SHUFFLE32(v, i) _mm_shuffle_epi32((v), (i))
/* Immediate blends. pblendw is SSE4.1, so below it the selector becomes a
 * constant mask vector and the select is the usual three operations. The
 * mask is built from literals, so it is materialised once. */
#if defined(GS_VEC_CAN_SSE41)
#define GS_V4I_BLEND16(a, b, m) _mm_blend_epi16((a), (b), (m))
#else
#define GS_V4I_BLEND16(a, b, m) \
   _mm_or_si128(_mm_and_si128(_mm_set_epi16( \
         ((m) & 0x80) ? -1 : 0, ((m) & 0x40) ? -1 : 0, \
         ((m) & 0x20) ? -1 : 0, ((m) & 0x10) ? -1 : 0, \
         ((m) & 0x08) ? -1 : 0, ((m) & 0x04) ? -1 : 0, \
         ((m) & 0x02) ? -1 : 0, ((m) & 0x01) ? -1 : 0), (b)), \
                _mm_andnot_si128(_mm_set_epi16( \
         ((m) & 0x80) ? -1 : 0, ((m) & 0x40) ? -1 : 0, \
         ((m) & 0x20) ? -1 : 0, ((m) & 0x10) ? -1 : 0, \
         ((m) & 0x08) ? -1 : 0, ((m) & 0x04) ? -1 : 0, \
         ((m) & 0x02) ? -1 : 0, ((m) & 0x01) ? -1 : 0), (a)))
#endif

/* The class expands a 32-bit blend mask into a 16-bit one with constexpr
 * arithmetic; on a literal the same expression folds here. */
#if defined(__AVX2__)
#define GS_V4I_BLEND32(a, b, m) _mm_blend_epi32((a), (b), (m))
#elif defined(GS_VEC_CAN_SSE41)
#define GS_V4I_BLEND32(a, b, m) _mm_blend_epi16((a), (b), \
   ((((m) & 8) * 3) << 3) | ((((m) & 4) * 3) << 2) | \
   ((((m) & 2) * 3) << 1) | (((m) & 1) * 3))
#else
#define GS_V4I_BLEND32(a, b, m) \
   _mm_or_si128(_mm_and_si128(_mm_set_epi32( \
         ((m) & 8) ? -1 : 0, ((m) & 4) ? -1 : 0, \
         ((m) & 2) ? -1 : 0, ((m) & 1) ? -1 : 0), (b)), \
                _mm_andnot_si128(_mm_set_epi32( \
         ((m) & 8) ? -1 : 0, ((m) & 4) ? -1 : 0, \
         ((m) & 2) ? -1 : 0, ((m) & 1) ? -1 : 0), (a)))
#endif

#else /* NEON */

#define GS_V4I_SLL16(v, i) vreinterpretq_s32_s16(vshlq_n_s16(vreinterpretq_s16_s32(v), (i)))
#define GS_V4I_SRL16(v, i) vreinterpretq_s32_u16(vshrq_n_u16(vreinterpretq_u16_s32(v), (i)))
#define GS_V4I_SRA16(v, i) vreinterpretq_s32_s16(vshrq_n_s16(vreinterpretq_s16_s32(v), (i)))
#define GS_V4I_SLL32(v, i) vshlq_n_s32((v), (i))
#define GS_V4I_SRL32(v, i) vreinterpretq_s32_u32(vshrq_n_u32(vreinterpretq_u32_s32(v), (i)))
#define GS_V4I_SRA32(v, i) vshrq_n_s32((v), (i))
#define GS_V4I_SLL(v, i)   vreinterpretq_s32_s8(vextq_s8(vdupq_n_s8(0), vreinterpretq_s8_s32(v), 16 - (i)))
#define GS_V4I_SRL(v, i)   vreinterpretq_s32_s8(vextq_s8(vreinterpretq_s8_s32(v), vdupq_n_s8(0), (i)))

/* NEON has no immediate lane selector, so the selector is decomposed here
 * and the compiler picks the instruction. Same builtin GSVector4i_arm64.h
 * uses for its shuffle family. */
#define GS_V4I_SHUFFLE32(v, i) \
   __builtin_shufflevector((v), (v), (i) & 3, ((i) >> 2) & 3, \
                           ((i) >> 4) & 3, ((i) >> 6) & 3)

/* A set bit takes that lane from b; lanes 4..7 name b's. */
#define GS_V4I_BLEND32(a, b, m) \
   __builtin_shufflevector((a), (b), ((m) & 1) ? 4 : 0, ((m) & 2) ? 5 : 1, \
                           ((m) & 4) ? 6 : 2, ((m) & 8) ? 7 : 3)

/* The same over the eight 16-bit lanes, where 8..15 name b's. */
#define GS_V4I_BLEND16(a, b, m) \
   vreinterpretq_s32_s16(__builtin_shufflevector( \
      vreinterpretq_s16_s32(a), vreinterpretq_s16_s32(b), \
      ((m) & 0x01) ?  8 : 0, ((m) & 0x02) ?  9 : 1, \
      ((m) & 0x04) ? 10 : 2, ((m) & 0x08) ? 11 : 3, \
      ((m) & 0x10) ? 12 : 4, ((m) & 0x20) ? 13 : 5, \
      ((m) & 0x40) ? 14 : 6, ((m) & 0x80) ? 15 : 7))

#endif

/* ------------------------------------------------------------------ */
/* Lane access. pinsrd/pextrd are SSE4.1; the SSE2 shapes go through    */
/* the 16-bit forms, which have been there since SSE2.                  */
/* ------------------------------------------------------------------ */

static GS_VEC_INLINE int32_t gs_v4i_extract32_0(gs_vec4i v)
{
#if defined(GS_VEC_X86)
   return _mm_cvtsi128_si32(v);
#else
   return vgetq_lane_s32(v, 0);
#endif
}

#if defined(GS_VEC_CAN_SSE41)
#define GS_V4I_EXTRACT32(v, i) \
   ((i) == 0 ? _mm_cvtsi128_si32(v) : _mm_extract_epi32((v), (i)))
#define GS_V4I_INSERT32(v, x, i) _mm_insert_epi32((v), (x), (i))
#define GS_V4I_INSERT8(v, x, i)  _mm_insert_epi8((v), (x), (i))
#elif defined(GS_VEC_X86)
#define GS_V4I_EXTRACT32(v, i) \
   ((i) == 0 ? _mm_cvtsi128_si32(v) \
             : _mm_cvtsi128_si32(_mm_shuffle_epi32((v), 0xff & ((i) * 0x55))))
#define GS_V4I_INSERT32(v, x, i) \
   _mm_insert_epi16(_mm_insert_epi16((v), (int)((uint32_t)(x) & 0xffff), (i) * 2), \
                    (int)((uint32_t)(x) >> 16), (i) * 2 + 1)
#define GS_V4I_INSERT8(v, x, i) \
   _mm_insert_epi16((v), \
      (((i) & 1) \
         ? (int)((((uint32_t)_mm_extract_epi16((v), (i) / 2)) & 0x00ffu) | \
                 (((uint32_t)(x) & 0xffu) << 8)) \
         : (int)((((uint32_t)_mm_extract_epi16((v), (i) / 2)) & 0xff00u) | \
                 ((uint32_t)(x) & 0xffu))), \
      (i) / 2)
#else
#define GS_V4I_EXTRACT32(v, i)   vgetq_lane_s32((v), (i))
#define GS_V4I_INSERT32(v, x, i) vsetq_lane_s32((x), (v), (i))
#define GS_V4I_INSERT8(v, x, i) \
   vreinterpretq_s32_s8(vsetq_lane_s8((int8_t)(x), vreinterpretq_s8_s32(v), (i)))
#endif

/* ------------------------------------------------------------------ */
/* Tests. ptest is SSE4.1; movmskb has been there since SSE2 and is     */
/* what the SSE2 tier uses.                                             */
/* ------------------------------------------------------------------ */

static GS_VEC_INLINE int gs_v4i_alltrue(gs_vec4i v)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_testc_si128(v, _mm_cmpeq_epi32(v, v));
#elif defined(GS_VEC_X86)
   {
      /* Every bit set, not merely every byte's top bit: compare against
       * all-ones and require every byte to match. */
      __m128i ones = _mm_cmpeq_epi32(v, v);
      return _mm_movemask_epi8(_mm_cmpeq_epi8(v, ones)) == 0xffff;
   }
#else
   return vminvq_u32(vreinterpretq_u32_s32(v)) == 0xffffffffu;
#endif
}

static GS_VEC_INLINE int gs_v4i_allfalse(gs_vec4i v)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_testz_si128(v, v);
#elif defined(GS_VEC_X86)
   /* Every bit clear, so compare against zero rather than reading sign
    * bits, which say nothing about the other seven. */
   return _mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_setzero_si128())) == 0xffff;
#else
   return vmaxvq_u32(vreinterpretq_u32_s32(v)) == 0;
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_set4(int32_t x, int32_t y, int32_t z, int32_t w)
{
#if defined(GS_VEC_X86)
   return _mm_set_epi32(w, z, y, x);
#else
   {
      int32_t t[4];
      t[0] = x; t[1] = y; t[2] = z; t[3] = w;
      return vld1q_s32(t);
   }
#endif
}

/* Two 32-bit values into the high half, zero below -- how a size becomes
 * the right/bottom edges of a rect. */
static GS_VEC_INLINE gs_vec4i gs_v4i_loadh(const void *p)
{
#if defined(GS_VEC_X86)
   return _mm_unpacklo_epi64(_mm_setzero_si128(),
                             _mm_loadl_epi64((const __m128i *)p));
#else
   return vcombine_s32(vdup_n_s32(0), vld1_s32((const int32_t *)p));
#endif
}

/* ------------------------------------------------------------------ */
/* Rectangles. The GS keeps a rect as x,y,z,w = left,top,right,bottom,  */
/* so intersect and union are one min/max pair with the halves swapped. */
/* ------------------------------------------------------------------ */

/* Clamped into b: the left/top edges take the later start, the
 * right/bottom edges the earlier end. */
static GS_VEC_INLINE gs_vec4i gs_v4i_rintersect(gs_vec4i a, gs_vec4i b)
{
   gs_vec4i xyxy = GS_V4I_SHUFFLE32(b, 0x44);
   gs_vec4i zwzw = GS_V4I_SHUFFLE32(b, 0xee);

   return gs_v4i_min_i32(gs_v4i_max_i32(a, xyxy), zwzw);
}

/* The enclosing rectangle: the earlier start in the low pair, the later
 * end moved down into the high pair. */
static GS_VEC_INLINE gs_vec4i gs_v4i_runion(gs_vec4i a, gs_vec4i b)
{
   gs_vec4i lo = gs_v4i_min_i32(a, b);
   gs_vec4i hi = GS_V4I_SRL(gs_v4i_max_i32(a, b), 8);

   return gs_v4i_upl64v(lo, hi);
}

/* Align a rect to a block size, which is always a power of two. bs points
 * at the two sizes as a packed pair, because that is how callers hold them
 * and one 8-byte load beats rebuilding the vector from two ints.
 *
 * The mask is (bsx-1, bsy-1, 0, 0), so adding its halves swapped leaves the
 * left and top edges alone and pushes the right and bottom ones up to the
 * next boundary; the andnot then rounds every edge down, which for the two
 * that were pushed is a round up. NegInf skips the addend and rounds all
 * four down. */
static GS_VEC_INLINE gs_vec4i gs_v4i_ralign_mask(const void *bs)
{
   return gs_v4i_sub32(gs_v4i_loadl(bs), gs_v4i_set4(1, 1, 0, 0));
}

static GS_VEC_INLINE gs_vec4i gs_v4i_ralign_outside(gs_vec4i r, const void *bs)
{
   gs_vec4i mask = gs_v4i_ralign_mask(bs);

   return gs_v4i_andnot(GS_V4I_SHUFFLE32(mask, 0x44),
                        gs_v4i_add32(r, GS_V4I_SHUFFLE32(mask, 0x4e)));
}

static GS_VEC_INLINE gs_vec4i gs_v4i_ralign_neginf(gs_vec4i r, const void *bs)
{
   return gs_v4i_andnot(GS_V4I_SHUFFLE32(gs_v4i_ralign_mask(bs), 0x44), r);
}

/* ------------------------------------------------------------------ */
/* What the block swizzle (gs_block.h) takes besides.                  */
/* ------------------------------------------------------------------ */

static GS_VEC_INLINE gs_vec4i gs_v4i_uph64v(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_unpackhi_epi64(a, b);
#else
   return vreinterpretq_s32_s64(vzip2q_s64(vreinterpretq_s64_s32(a),
                                           vreinterpretq_s64_s32(b)));
#endif
}

/* a's eight and b's eight 16-bit lanes, each to an unsigned byte */
static GS_VEC_INLINE gs_vec4i gs_v4i_pu16(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_packus_epi16(a, b);
#else
   return vreinterpretq_s32_u8(vcombine_u8(vqmovun_s16(vreinterpretq_s16_s32(a)),
                                           vqmovun_s16(vreinterpretq_s16_s32(b))));
#endif
}

/* Each byte of a by the index in that byte of mask; a mask byte with its
 * top bit set gives 0 (pshufb; tbl gives 0 for any index past 15) */
static GS_VEC_INLINE gs_vec4i gs_v4i_shuffle8(gs_vec4i a, gs_vec4i mask)
{
#if defined(GS_VEC_CAN_SSE41) || defined(__SSSE3__)
   return _mm_shuffle_epi8(a, mask);
#elif defined(GS_VEC_X86)
   {
      union gs_v4i_view x, m, r;
      int i;
      x.v = a;
      m.v = mask;
      for (i = 0; i < 16; i++)
         r.u8[i] = (m.u8[i] & 0x80) ? 0 : x.u8[m.u8[i] & 15];
      return r.v;
   }
#else
   return vreinterpretq_s32_s8(vqtbl1q_s8(vreinterpretq_s8_s32(a), vreinterpretq_u8_s32(mask)));
#endif
}

/* b where mask's bits are set, a where clear */
static GS_VEC_INLINE gs_vec4i gs_v4i_select(gs_vec4i a, gs_vec4i b, gs_vec4i mask)
{
#if defined(GS_VEC_X86)
   return _mm_or_si128(_mm_andnot_si128(mask, a), _mm_and_si128(mask, b));
#else
   return vbslq_s32(vreinterpretq_u32_s32(mask), b, a);
#endif
}

static GS_VEC_INLINE void gs_v4i_storeh(void *p, gs_vec4i v)
{
#if defined(GS_VEC_X86)
   _mm_storeh_pi((__m64 *)p, _mm_castsi128_ps(v));
#else
   vst1_s32((int32_t *)p, vget_high_s32(v));
#endif
}

/* The 16-bit lanes of each 64-bit half by one _MM_SHUFFLE immediate
 * (pshuflw then pshufhw) */
#if defined(GS_VEC_X86)
#define GS_V4I_SHUFFLE16LH(v, i) _mm_shufflehi_epi16(_mm_shufflelo_epi16((v), (i)), (i))
#else
#define GS_V4I_SHUFFLE16LH(v, i) \
   vreinterpretq_s32_s16(__builtin_shufflevector( \
      vreinterpretq_s16_s32(v), vreinterpretq_s16_s32(v), \
      (i) & 3, ((i) >> 2) & 3, ((i) >> 4) & 3, ((i) >> 6) & 3, \
      4 + ((i) & 3), 4 + (((i) >> 2) & 3), 4 + (((i) >> 4) & 3), 4 + (((i) >> 6) & 3)))
#endif

/* Two of a's 32-bit lanes then two of b's, by an _MM_SHUFFLE immediate
 * (shufps on integers) */
#if defined(GS_VEC_X86)
#define GS_V4I_SHUFFLE2(a, b, i) \
   _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b), (i)))
#else
#define GS_V4I_SHUFFLE2(a, b, i) \
   __builtin_shufflevector((a), (b), (i) & 3, ((i) >> 2) & 3, \
                           4 + (((i) >> 4) & 3), 4 + (((i) >> 6) & 3))
#endif

/* Bytes n.. of lo then hi, sixteen of them (palignr) */
#if defined(GS_VEC_CAN_SSE41) || defined(__SSSE3__)
#define GS_V4I_ALIGNR(hi, lo, n) _mm_alignr_epi8((hi), (lo), (n))
#elif defined(GS_VEC_X86)
#define GS_V4I_ALIGNR(hi, lo, n) _mm_or_si128(_mm_srli_si128((lo), (n)), _mm_slli_si128((hi), 16 - (n)))
#else
#define GS_V4I_ALIGNR(hi, lo, n) \
   vreinterpretq_s32_s8(vextq_s8(vreinterpretq_s8_s32(lo), vreinterpretq_s8_s32(hi), (n)))
#endif

static GS_VEC_INLINE gs_vec4i gs_v4i_set16(int a, int b, int c, int d, int e, int f, int g, int h)
{
#if defined(GS_VEC_X86)
   return _mm_set_epi16((short)h, (short)g, (short)f, (short)e, (short)d, (short)c, (short)b, (short)a);
#else
   int16_t t[8];
   t[0] = (int16_t)a; t[1] = (int16_t)b; t[2] = (int16_t)c; t[3] = (int16_t)d;
   t[4] = (int16_t)e; t[5] = (int16_t)f; t[6] = (int16_t)g; t[7] = (int16_t)h;
   return vreinterpretq_s32_s16(vld1q_s16(t));
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_set64(uint64_t lo, uint64_t hi)
{
#if defined(GS_VEC_X86)
   return _mm_set_epi64x((long long)hi, (long long)lo);
#else
   return vreinterpretq_s32_u64(vcombine_u64(vcreate_u64(lo), vcreate_u64(hi)));
#endif
}

/* ------------------------------------------------------------------ */
/* What the GIF path (GSState.c) takes besides.                        */
/* ------------------------------------------------------------------ */

/* x into lane 0, zero above (movd) */
static GS_VEC_INLINE gs_vec4i gs_v4i_load32(int32_t x)
{
#if defined(GS_VEC_X86)
   return _mm_cvtsi32_si128(x);
#else
   return vsetq_lane_s32(x, vdupq_n_s32(0), 0);
#endif
}

/* Eight bytes from lo, then eight from hi (movq, movhps) */
static GS_VEC_INLINE gs_vec4i gs_v4i_loadlh(const void *lo, const void *hi)
{
#if defined(GS_VEC_X86)
   return _mm_castps_si128(_mm_loadh_pi(_mm_castsi128_ps(
         _mm_loadl_epi64((const __m128i *)lo)), (const __m64 *)hi));
#else
   return vreinterpretq_s32_s64(vcombine_s64(vld1_s64((const int64_t *)lo),
                                             vld1_s64((const int64_t *)hi)));
#endif
}

static GS_VEC_INLINE gs_vec4i gs_v4i_lt32(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_X86)
   return _mm_cmplt_epi32(a, b);
#else
   return vreinterpretq_s32_u32(vcltq_s32(a, b));
#endif
}

/* pcmpeqq is SSE4.1: below it a 64-bit lane is equal when both its
 * 32-bit halves are */
static GS_VEC_INLINE gs_vec4i gs_v4i_eq64(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_cmpeq_epi64(a, b);
#elif defined(GS_VEC_X86)
   {
      __m128i e = _mm_cmpeq_epi32(a, b);
      return _mm_and_si128(e, _mm_shuffle_epi32(e, 0xb1));
   }
#else
   return vreinterpretq_s32_u64(vceqq_s64(vreinterpretq_s64_s32(a),
                                          vreinterpretq_s64_s32(b)));
#endif
}

/* Each byte's top bit, byte 0 in bit 0 (pmovmskb) */
static GS_VEC_INLINE int gs_v4i_mask(gs_vec4i v)
{
#if defined(GS_VEC_X86)
   return _mm_movemask_epi8(v);
#else
   uint16x8_t h = vreinterpretq_u16_u8(vshrq_n_u8(vreinterpretq_u8_s32(v), 7));
   uint32x4_t p16 = vreinterpretq_u32_u16(vsraq_n_u16(h, h, 7));
   uint64x2_t p32 = vreinterpretq_u64_u32(vsraq_n_u32(p16, p16, 14));
   uint8x16_t p64 = vreinterpretq_u8_u64(vsraq_n_u64(p32, p32, 28));
   return (int)(vgetq_lane_u8(p64, 0) | ((int)vgetq_lane_u8(p64, 8) << 8));
#endif
}

/* The low four 16-bit lanes widened to 32 bits */
static GS_VEC_INLINE gs_vec4i gs_v4i_u16to32(gs_vec4i a)
{
#if defined(GS_VEC_CAN_SSE41)
   return _mm_cvtepu16_epi32(a);
#elif defined(GS_VEC_X86)
   return _mm_unpacklo_epi16(a, _mm_setzero_si128());
#else
   return vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vreinterpretq_u16_s32(a))));
#endif
}

/* Every bit of a and b the same */
static GS_VEC_INLINE int gs_v4i_eq(gs_vec4i a, gs_vec4i b)
{
#if defined(GS_VEC_CAN_SSE41)
   __m128i t = _mm_xor_si128(a, b);
   return _mm_testz_si128(t, t) != 0;
#elif defined(GS_VEC_X86)
   return _mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) == 0xffff;
#else
   return vmaxvq_u32(vreinterpretq_u32_s32(veorq_s32(a, b))) == 0;
#endif
}

/* A rect (left, top, right, bottom) with no area: not left < right and
 * top < bottom both */
static GS_VEC_INLINE int gs_v4i_rempty(gs_vec4i r)
{
#if defined(GS_VEC_X86)
   return _mm_movemask_epi8(_mm_cmplt_epi32(r, _mm_shuffle_epi32(r, 0xee))) != 0x00ff;
#else
   return vminv_u32(vreinterpret_u32_s32(vget_low_s32(gs_v4i_lt32(r, GS_V4I_SHUFFLE32(r, 0xee))))) == 0;
#endif
}

/* The four lanes as floats, a NaN one replaced by r's */
static GS_VEC_INLINE gs_vec4i gs_v4i_replace_nan_f32(gs_vec4i v, gs_vec4i r)
{
#if defined(GS_VEC_X86)
   __m128 f = _mm_castsi128_ps(v);
   __m128 m = _mm_cmpeq_ps(f, f);
#if defined(GS_VEC_CAN_SSE41)
   return _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(r), f, m));
#else
   return _mm_castps_si128(_mm_or_ps(_mm_and_ps(m, f), _mm_andnot_ps(m, _mm_castsi128_ps(r))));
#endif
#else
   float32x4_t f = vreinterpretq_f32_s32(v);
   return vreinterpretq_s32_f32(vbslq_f32(vceqq_f32(f, f), f, vreinterpretq_f32_s32(r)));
#endif
}

/* The bits of the floats the GIF path substitutes */
#define GS_V4I_F32_ONE     0x3f800000 /* 1.0f */
#define GS_V4I_F32_MAX     0x7f7fffff /* FLT_MAX */
#define GS_V4I_F32_MIN_POS 0x00800000 /* FLT_MIN */

#endif /* GS_VECTOR_H */
