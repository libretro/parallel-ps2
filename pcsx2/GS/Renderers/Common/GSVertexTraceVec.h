/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_VERTEX_TRACE_VEC_H
#define GS_VERTEX_TRACE_VEC_H

/* The vector operations the vertex trace does, as the intrinsics
 * GSVector4 and GSVector4i used for them on each architecture: a float
 * operation's NaN and signed-zero cases are the instruction's, so the
 * same instruction is what keeps a draw's trace what it was. */

#include <string.h>

#include "GSVertexTrace.h"
#include "../../gs_vector.h"

#if defined(_MSC_VER)
#define GS_VT_INLINE static __forceinline
#else
#define GS_VT_INLINE static __inline__ __attribute__((always_inline))
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || defined(__i386__) || defined(_M_IX86)
#define GS_VT_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#endif

typedef gs_vt_vec4f vt_f;
typedef gs_vec4i vt_i;

#ifdef GS_VT_X86

GS_VT_INLINE vt_f vt_set(float x, float y, float z, float w) { return _mm_set_ps(w, z, y, x); }
GS_VT_INLINE vt_f vt_splat(float f) { return _mm_set1_ps(f); }
GS_VT_INLINE vt_f vt_fzero(void) { return _mm_setzero_ps(); }
GS_VT_INLINE vt_f vt_add(vt_f a, vt_f b) { return _mm_add_ps(a, b); }
GS_VT_INLINE vt_f vt_sub(vt_f a, vt_f b) { return _mm_sub_ps(a, b); }
GS_VT_INLINE vt_f vt_mul(vt_f a, vt_f b) { return _mm_mul_ps(a, b); }
GS_VT_INLINE vt_f vt_div(vt_f a, vt_f b) { return _mm_div_ps(a, b); }
GS_VT_INLINE vt_f vt_fmin(vt_f a, vt_f b) { return _mm_min_ps(a, b); }
GS_VT_INLINE vt_f vt_fmax(vt_f a, vt_f b) { return _mm_max_ps(a, b); }
GS_VT_INLINE vt_f vt_cvt(vt_i a) { return _mm_cvtepi32_ps(a); }
GS_VT_INLINE vt_f vt_fcast(vt_i a) { return _mm_castsi128_ps(a); }
GS_VT_INLINE vt_i vt_icast(vt_f a) { return _mm_castps_si128(a); }
GS_VT_INLINE vt_f vt_for(vt_f a, vt_f b) { return _mm_or_ps(a, b); }
GS_VT_INLINE vt_f vt_neg(vt_f a) { return _mm_xor_ps(a, _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u))); }
GS_VT_INLINE vt_f vt_uph(vt_f a, vt_f b) { return _mm_unpackhi_ps(a, b); }
/* (a's lanes x and y, then b's z and w, by index) */
#define VT_SHUF(a, b, x, y, z, w) _mm_shuffle_ps(a, b, _MM_SHUFFLE(w, z, y, x))
/* NaN lanes zero (replace_nan(zero)), then held to [lo, hi] */
GS_VT_INLINE vt_f vt_denan_sat(vt_f a, vt_f lo, vt_f hi)
{
	a = _mm_blendv_ps(_mm_setzero_ps(), a, _mm_cmpeq_ps(a, a));
	return _mm_min_ps(_mm_max_ps(a, lo), hi);
}
/* lane 2 from lane 0 of b */
GS_VT_INLINE vt_f vt_insert_z(vt_f a, vt_f b) { return _mm_insert_ps(a, b, _MM_MK_INSERTPS_NDX(0, 2, 0)); }
GS_VT_INLINE vt_f vt_load_ss(float f) { return _mm_load_ss(&f); }
GS_VT_INLINE unsigned vt_fmask_eq(vt_f a, vt_f b) { return (unsigned)_mm_movemask_ps(_mm_cmpeq_ps(a, b)); }
GS_VT_INLINE void vt_storel(void* p, vt_f a) { memcpy(p, &a, 8); }

GS_VT_INLINE vt_i vt_iload(const void* p) { return _mm_load_si128((const __m128i*)p); }
GS_VT_INLINE vt_i vt_iload32(u32 v) { return _mm_cvtsi32_si128((int)v); }
GS_VT_INLINE vt_i vt_izero(void) { return _mm_setzero_si128(); }
GS_VT_INLINE vt_i vt_iones(void) { return _mm_set1_epi32(-1); }
GS_VT_INLINE vt_i vt_min_u8(vt_i a, vt_i b) { return _mm_min_epu8(a, b); }
GS_VT_INLINE vt_i vt_max_u8(vt_i a, vt_i b) { return _mm_max_epu8(a, b); }
GS_VT_INLINE vt_i vt_min_u32(vt_i a, vt_i b) { return _mm_min_epu32(a, b); }
GS_VT_INLINE vt_i vt_max_u32(vt_i a, vt_i b) { return _mm_max_epu32(a, b); }
GS_VT_INLINE vt_i vt_upl16(vt_i a) { return _mm_unpacklo_epi16(a, _mm_setzero_si128()); }
GS_VT_INLINE vt_i vt_uph16(vt_i a) { return _mm_unpackhi_epi16(a, _mm_setzero_si128()); }
GS_VT_INLINE vt_i vt_ywyw(vt_i a) { return _mm_shuffle_epi32(a, _MM_SHUFFLE(3, 1, 3, 1)); }
/* a's x and y, b's z and w */
GS_VT_INLINE vt_i vt_blend_zw(vt_i a, vt_i b) { return _mm_blend_epi16(a, b, 0xf0); }
GS_VT_INLINE u32 vt_extract_z(vt_i a) { return (u32)_mm_extract_epi32(a, 2); }
GS_VT_INLINE vt_i vt_u8to32(vt_i a) { return _mm_cvtepu8_epi32(a); }
GS_VT_INLINE vt_i vt_shl(vt_i a, int n) { return _mm_slli_epi32(a, n); }
GS_VT_INLINE vt_i vt_srl(vt_i a, int n) { return _mm_srli_epi32(a, n); }
GS_VT_INLINE vt_i vt_isub(vt_i a, vt_i b) { return _mm_sub_epi32(a, b); }
GS_VT_INLINE unsigned vt_imask_eq(vt_i a, vt_i b) { return (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi32(a, b)); }

#else /* NEON */

GS_VT_INLINE vt_f vt_set(float x, float y, float z, float w)
{
	const float v[4] = {x, y, z, w};
	return vld1q_f32(v);
}
GS_VT_INLINE vt_f vt_splat(float f) { return vdupq_n_f32(f); }
GS_VT_INLINE vt_f vt_fzero(void) { return vdupq_n_f32(0.0f); }
GS_VT_INLINE vt_f vt_add(vt_f a, vt_f b) { return vaddq_f32(a, b); }
GS_VT_INLINE vt_f vt_sub(vt_f a, vt_f b) { return vsubq_f32(a, b); }
GS_VT_INLINE vt_f vt_mul(vt_f a, vt_f b) { return vmulq_f32(a, b); }
GS_VT_INLINE vt_f vt_div(vt_f a, vt_f b) { return vdivq_f32(a, b); }
GS_VT_INLINE vt_f vt_fmin(vt_f a, vt_f b) { return vminq_f32(a, b); }
GS_VT_INLINE vt_f vt_fmax(vt_f a, vt_f b) { return vmaxq_f32(a, b); }
GS_VT_INLINE vt_f vt_cvt(vt_i a) { return vcvtq_f32_s32(a); }
GS_VT_INLINE vt_f vt_fcast(vt_i a) { return vreinterpretq_f32_s32(a); }
GS_VT_INLINE vt_i vt_icast(vt_f a) { return vreinterpretq_s32_f32(a); }
GS_VT_INLINE vt_f vt_for(vt_f a, vt_f b)
{
	return vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(a), vreinterpretq_u32_f32(b)));
}
GS_VT_INLINE vt_f vt_neg(vt_f a) { return vnegq_f32(a); }
GS_VT_INLINE vt_f vt_uph(vt_f a, vt_f b) { return vzip2q_f32(a, b); }
#define VT_SHUF(a, b, x, y, z, w) __builtin_shufflevector(a, b, x, y, 4 + (z), 4 + (w))
GS_VT_INLINE vt_f vt_denan_sat(vt_f a, vt_f lo, vt_f hi)
{
	const uint32x4_t keep = vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(vceqq_f32(a, a)), 31));
	a = vbslq_f32(keep, a, vdupq_n_f32(0.0f));
	return vminq_f32(vmaxq_f32(a, lo), hi);
}
GS_VT_INLINE vt_f vt_insert_z(vt_f a, vt_f b) { return vcopyq_laneq_f32(a, 2, b, 0); }
GS_VT_INLINE vt_f vt_load_ss(float f) { return vsetq_lane_f32(f, vdupq_n_f32(0.0f), 0); }
GS_VT_INLINE unsigned vt_fmask_eq(vt_f a, vt_f b)
{
	const uint32x4_t e = vshrq_n_u32(vceqq_f32(a, b), 31);
	return vgetq_lane_u32(e, 0) | (vgetq_lane_u32(e, 1) << 1) | (vgetq_lane_u32(e, 2) << 2) | (vgetq_lane_u32(e, 3) << 3);
}
GS_VT_INLINE void vt_storel(void* p, vt_f a) { vst1_f32((float*)p, vget_low_f32(a)); }

GS_VT_INLINE vt_i vt_iload(const void* p) { return vld1q_s32((const int32_t*)p); }
GS_VT_INLINE vt_i vt_iload32(u32 v) { return vsetq_lane_s32((int32_t)v, vdupq_n_s32(0), 0); }
GS_VT_INLINE vt_i vt_izero(void) { return vdupq_n_s32(0); }
GS_VT_INLINE vt_i vt_iones(void) { return vdupq_n_s32(-1); }
GS_VT_INLINE vt_i vt_min_u8(vt_i a, vt_i b)
{
	return vreinterpretq_s32_u8(vminq_u8(vreinterpretq_u8_s32(a), vreinterpretq_u8_s32(b)));
}
GS_VT_INLINE vt_i vt_max_u8(vt_i a, vt_i b)
{
	return vreinterpretq_s32_u8(vmaxq_u8(vreinterpretq_u8_s32(a), vreinterpretq_u8_s32(b)));
}
GS_VT_INLINE vt_i vt_min_u32(vt_i a, vt_i b)
{
	return vreinterpretq_s32_u32(vminq_u32(vreinterpretq_u32_s32(a), vreinterpretq_u32_s32(b)));
}
GS_VT_INLINE vt_i vt_max_u32(vt_i a, vt_i b)
{
	return vreinterpretq_s32_u32(vmaxq_u32(vreinterpretq_u32_s32(a), vreinterpretq_u32_s32(b)));
}
GS_VT_INLINE vt_i vt_upl16(vt_i a) { return vreinterpretq_s32_s16(vzip1q_s16(vreinterpretq_s16_s32(a), vdupq_n_s16(0))); }
GS_VT_INLINE vt_i vt_uph16(vt_i a) { return vreinterpretq_s32_s16(vzip2q_s16(vreinterpretq_s16_s32(a), vdupq_n_s16(0))); }
GS_VT_INLINE vt_i vt_ywyw(vt_i a) { return __builtin_shufflevector(a, a, 1, 3, 1, 3); }
GS_VT_INLINE vt_i vt_blend_zw(vt_i a, vt_i b) { return __builtin_shufflevector(a, b, 0, 1, 6, 7); }
GS_VT_INLINE u32 vt_extract_z(vt_i a) { return (u32)vgetq_lane_s32(a, 2); }
GS_VT_INLINE vt_i vt_u8to32(vt_i a)
{
	return vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vget_low_u8(vreinterpretq_u8_s32(a))))));
}
GS_VT_INLINE vt_i vt_shl(vt_i a, int n) { return vshlq_s32(a, vdupq_n_s32(n)); }
GS_VT_INLINE vt_i vt_srl(vt_i a, int n)
{
	return vreinterpretq_s32_u32(vshlq_u32(vreinterpretq_u32_s32(a), vdupq_n_s32(-n)));
}
GS_VT_INLINE vt_i vt_isub(vt_i a, vt_i b) { return vsubq_s32(a, b); }
GS_VT_INLINE unsigned vt_imask_eq(vt_i a, vt_i b)
{
	/* the byte sign mask of a 32-bit compare: four bits a lane */
	const uint32x4_t e = vshrq_n_u32(vceqq_s32(a, b), 31);
	return (vgetq_lane_u32(e, 0) * 0xfu) | (vgetq_lane_u32(e, 1) * 0xf0u)
		| (vgetq_lane_u32(e, 2) * 0xf00u) | (vgetq_lane_u32(e, 3) * 0xf000u);
}

#endif

#endif
