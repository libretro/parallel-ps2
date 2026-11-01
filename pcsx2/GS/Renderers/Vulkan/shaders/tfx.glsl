// The hardware renderer's draw shaders. Compiled ahead of time by
// tools/vk_spirv.py into VKShaders.inc; the build never sees this file.
//
// Every draw selector field is a specialization constant: a pipeline gets
// the shader its selector asks for from the driver, which folds the
// constants and drops what they turn off. What changes the shader's
// interface is settled in the SPIR-V instead:
//  - the vertex shader is two modules, with and without VS_EXPAND;
//  - the fragment shader is three, for how the target is read: not at
//    all, RT_SUBPASS (an input attachment) or RT_FETCH (a sampled image);
//  - each fragment module has the second output (dual-source blending),
//    gl_PrimitiveID (destination alpha test 1-3) and gl_FragDepth (depth
//    clamp). For a draw that does without one, GSDeviceVK turns it into a
//    private variable in the module's words (VKSpirvPatch.c), as if it was
//    never declared: no dual-source output, no geometry capability, no
//    depth replacing (early depth testing stays);
//  - the colour varying is flat, and the decoration is dropped from the
//    words for a draw that interpolates it.

#version 460 core
#extension GL_EXT_samplerless_texture_functions : require

#if defined(VERTEX_SHADER)

layout(constant_id = 0) const bool VS_TME = false;
layout(constant_id = 1) const bool VS_FST = false;
layout(constant_id = 2) const bool VS_POINT_SIZE = false;
// 1 point, 2 line, 3 sprite; the module without VS_EXPAND has none
layout(constant_id = 3) const int VS_EXPAND_MODE = 1;

layout(std140, set = 0, binding = 0) uniform cb0
{
	vec2 VertexScale;
	vec2 VertexOffset;
	vec2 TextureScale;
	vec2 TextureOffset;
	vec2 PointSize;
	uint MaxDepth;
	uint pad_cb0;
};

layout(location = 0) out VSOutput
{
	vec4 t;
	vec4 ti;
	flat vec4 c;
} vsOut;

#ifndef VS_EXPAND

layout(location = 0) in vec2 a_st;
layout(location = 1) in uvec4 a_c;
layout(location = 2) in float a_q;
layout(location = 3) in uvec2 a_p;
layout(location = 4) in uint a_z;
layout(location = 5) in uvec2 a_uv;
layout(location = 6) in vec4 a_f;

void main()
{
	// Clamp to max depth, gs doesn't wrap
	uint z = min(a_z, MaxDepth);

	// pos -= 0.05 (1/320 pixel) helps avoiding rounding problems (integral part of pos is usually 5 digits, 0.05 is about as low as we can go)
	// example: ceil(afterseveralvertextransformations(y = 133)) => 134 => line 133 stays empty
	// input granularity is 1/16 pixel, anything smaller than that won't step drawing up/left by one pixel
	// example: 133.0625 (133 + 1/16) should start from line 134, ceil(133.0625 - 0.05) still above 133

	gl_Position = vec4(a_p, float(z), 1.0f) - vec4(0.05f, 0.05f, 0, 0);
	gl_Position.xy = gl_Position.xy * vec2(VertexScale.x, -VertexScale.y) - vec2(VertexOffset.x, -VertexOffset.y);
	gl_Position.z *= exp2(-32.0f);		// integer->float depth
	gl_Position.y = -gl_Position.y;

	if (VS_TME)
	{
		vec2 uv = a_uv - TextureOffset;
		vec2 st = a_st - TextureOffset;

		// Integer nomalized
		vsOut.ti.xy = uv * TextureScale;

		if (VS_FST)
		{
			// Integer integral
			vsOut.ti.zw = uv;
		}
		else
		{
			// float for post-processing in some games
			vsOut.ti.zw = st / TextureScale;
		}

		// Float coords
		vsOut.t.xy = st;
		vsOut.t.w = a_q;
	}
	else
	{
		vsOut.t = vec4(0.0f, 0.0f, 0.0f, 1.0f);
		vsOut.ti = vec4(0.0f);
	}

	if (VS_POINT_SIZE)
		gl_PointSize = PointSize.x;

	vsOut.c = vec4(a_c);
	vsOut.t.z = a_f.r;
}

#else // VS_EXPAND

#extension GL_ARB_shader_draw_parameters : require

struct RawVertex
{
	vec2 ST;
	uint RGBA;
	float Q;
	uint XY;
	uint Z;
	uint UV;
	uint FOG;
};

layout(std140, set = 0, binding = 2) readonly buffer VertexBuffer {
	RawVertex vertex_buffer[];
};

struct ProcessedVertex
{
	vec4 p;
	vec4 t;
	vec4 ti;
	vec4 c;
};

ProcessedVertex load_vertex(uint index)
{
	RawVertex rvtx = vertex_buffer[gl_BaseVertexARB + index];

	vec2 a_st = rvtx.ST;
	uvec4 a_c = uvec4(bitfieldExtract(rvtx.RGBA, 0, 8), bitfieldExtract(rvtx.RGBA, 8, 8),
	                  bitfieldExtract(rvtx.RGBA, 16, 8), bitfieldExtract(rvtx.RGBA, 24, 8));
	float a_q = rvtx.Q;
	uvec2 a_p = uvec2(bitfieldExtract(rvtx.XY, 0, 16), bitfieldExtract(rvtx.XY, 16, 16));
	uint a_z = rvtx.Z;
	uvec2 a_uv = uvec2(bitfieldExtract(rvtx.UV, 0, 16), bitfieldExtract(rvtx.UV, 16, 16));
	vec4 a_f = unpackUnorm4x8(rvtx.FOG);

	ProcessedVertex vtx;

	uint z = min(a_z, MaxDepth);
	vtx.p = vec4(a_p, float(z), 1.0f) - vec4(0.05f, 0.05f, 0, 0);
	vtx.p.xy = vtx.p.xy * vec2(VertexScale.x, -VertexScale.y) - vec2(VertexOffset.x, -VertexOffset.y);
	vtx.p.z *= exp2(-32.0f);		// integer->float depth
	vtx.p.y = -vtx.p.y;

	if (VS_TME)
	{
		vec2 uv = a_uv - TextureOffset;
		vec2 st = a_st - TextureOffset;
		vtx.ti.xy = uv * TextureScale;

		if (VS_FST)
			vtx.ti.zw = uv;
		else
			vtx.ti.zw = st / TextureScale;

		vtx.t.xy = st;
		vtx.t.w = a_q;
	}
	else
	{
		vtx.t = vec4(0.0f, 0.0f, 0.0f, 1.0f);
		vtx.ti = vec4(0.0f);
	}

	vtx.c = a_c;
	vtx.t.z = a_f.r;

	return vtx;
}

void main()
{
	ProcessedVertex vtx;
	uint vid = uint(gl_VertexIndex - gl_BaseVertexARB);

	if (VS_EXPAND_MODE == 1) // Point
	{
		vtx = load_vertex(vid >> 2);

		vtx.p.x += ((vid & 1u) != 0u) ? PointSize.x : 0.0f;
		vtx.p.y += ((vid & 2u) != 0u) ? PointSize.y : 0.0f;
	}
	else if (VS_EXPAND_MODE == 2) // Line
	{
		uint vid_base = vid >> 2;

		bool is_bottom = (vid & 2u) != 0u;
		bool is_right = (vid & 1u) != 0u;
		// The other end of the line as with the provoking vertex last;
		// the runtime-built shaders before these took this one whatever
		// the device's provoking vertex.
		uint vid_other = is_bottom ? vid_base - 1 : vid_base + 1;

		vtx = load_vertex(vid_base);
		ProcessedVertex other = load_vertex(vid_other);

		vec2 line_vector = normalize(vtx.p.xy - other.p.xy);
		vec2 line_normal = vec2(line_vector.y, -line_vector.x);
		vec2 line_width = (line_normal * PointSize) / 2;
		// line_normal is inverted for bottom point
		vec2 offset = ((uint(is_bottom) ^ uint(is_right)) != 0u) ? line_width : -line_width;
		vtx.p.xy += offset;

		// Lines will be run as (0 1 2) (1 2 3)
		// This means that both triangles will have a point based off the top line point as their first point
		// So we don't have to do anything for !IIP
	}
	else // Sprite
	{
		// Sprite points are always in pairs
		uint vid_base = vid >> 1;
		uint vid_lt = vid_base & ~1u;
		uint vid_rb = vid_base | 1u;

		ProcessedVertex lt = load_vertex(vid_lt);
		ProcessedVertex rb = load_vertex(vid_rb);
		vtx = rb;

		bool is_right = ((vid & 1u) != 0u);
		vtx.p.x = is_right ? lt.p.x : vtx.p.x;
		vtx.t.x = is_right ? lt.t.x : vtx.t.x;
		vtx.ti.xz = is_right ? lt.ti.xz : vtx.ti.xz;

		bool is_bottom = ((vid & 2u) != 0u);
		vtx.p.y = is_bottom ? lt.p.y : vtx.p.y;
		vtx.t.y = is_bottom ? lt.t.y : vtx.t.y;
		vtx.ti.yw = is_bottom ? lt.ti.yw : vtx.ti.yw;
	}

	gl_Position = vtx.p;
	vsOut.t = vtx.t;
	vsOut.ti = vtx.ti;
	vsOut.c = vtx.c;
}

#endif // VS_EXPAND

#endif // VERTEX_SHADER

#ifdef FRAGMENT_SHADER

#define FMT_32 0
#define FMT_24 1
#define FMT_16 2

#define SHUFFLE_READ  1
#define SHUFFLE_WRITE 2
#define SHUFFLE_READWRITE 3

// GSHWDrawConfig::PSSelector, a constant a field
layout(constant_id = 0) const bool PS_FST = false;
layout(constant_id = 1) const int PS_WMS = 0;
layout(constant_id = 2) const int PS_WMT = 0;
layout(constant_id = 3) const bool PS_ADJS = false;
layout(constant_id = 4) const bool PS_ADJT = false;
layout(constant_id = 5) const int PS_AEM_FMT = 0;
layout(constant_id = 6) const int PS_PAL_FMT = 0;
layout(constant_id = 7) const int PS_DST_FMT = 0;
layout(constant_id = 8) const int PS_DEPTH_FMT = 0;
layout(constant_id = 9) const int PS_CHANNEL_FETCH = 0;
layout(constant_id = 10) const bool PS_URBAN_CHAOS_HLE = false;
layout(constant_id = 11) const bool PS_TALES_OF_ABYSS_HLE = false;
layout(constant_id = 12) const bool PS_AEM = false;
layout(constant_id = 13) const int PS_TFX = 0;
layout(constant_id = 14) const bool PS_TCC = true;
layout(constant_id = 15) const int PS_ATST = 0;
layout(constant_id = 16) const int PS_AFAIL = 0;
layout(constant_id = 17) const bool PS_FOG = false;
layout(constant_id = 18) const int PS_BLEND_HW = 0;
layout(constant_id = 19) const bool PS_A_MASKED = false;
layout(constant_id = 20) const bool PS_FBA = false;
layout(constant_id = 21) const bool PS_LTF = false;
layout(constant_id = 22) const bool PS_AUTOMATIC_LOD = false;
layout(constant_id = 23) const bool PS_MANUAL_LOD = false;
layout(constant_id = 24) const bool PS_COLCLIP = false;
layout(constant_id = 25) const int PS_DATE = 0;
layout(constant_id = 26) const bool PS_TCOFFSETHACK = false;
layout(constant_id = 27) const bool PS_REGION_RECT = false;
layout(constant_id = 28) const int PS_BLEND_A = 0;
layout(constant_id = 29) const int PS_BLEND_B = 0;
layout(constant_id = 30) const int PS_BLEND_C = 0;
layout(constant_id = 31) const int PS_BLEND_D = 0;
layout(constant_id = 32) const int PS_BLEND_MIX = 0;
layout(constant_id = 33) const bool PS_ROUND_INV = false;
layout(constant_id = 34) const bool PS_FIXED_ONE_A = false;
layout(constant_id = 35) const bool PS_SHUFFLE = false;
layout(constant_id = 36) const bool PS_SHUFFLE_SAME = false;
layout(constant_id = 37) const int PS_PROCESS_BA = 0;
layout(constant_id = 38) const int PS_PROCESS_RG = 0;
layout(constant_id = 39) const bool PS_SHUFFLE_ACROSS = false;
layout(constant_id = 40) const bool PS_READ16_SRC = false;
layout(constant_id = 41) const bool PS_WRITE_RG = false;
layout(constant_id = 42) const bool PS_FBMASK = false;
layout(constant_id = 43) const bool PS_COLCLIP_HW = false;
layout(constant_id = 44) const bool PS_RTA_CORRECTION = false;
layout(constant_id = 45) const bool PS_RTA_SRC_CORRECTION = false;
layout(constant_id = 46) const int PS_DITHER = 0;
layout(constant_id = 47) const bool PS_DITHER_ADJUST = false;
layout(constant_id = 48) const bool PS_PABE = false;
layout(constant_id = 49) const int PS_SCANMSK = 0;
layout(constant_id = 50) const bool PS_SAMPLE_MAP = false;
layout(constant_id = 51) const bool PS_NATIVE_TAPS = false;
layout(constant_id = 52) const bool PS_TEX_IS_FB = false;
layout(constant_id = 53) const bool PS_NO_COLOR = false;

const bool SW_BLEND = (PS_BLEND_A != 0 || PS_BLEND_B != 0 || PS_BLEND_D != 0);
const bool SW_BLEND_NEEDS_RT = SW_BLEND && (PS_BLEND_A == 1 || PS_BLEND_B == 1 || PS_BLEND_C == 1 || PS_BLEND_D == 1);
const bool SW_AD_TO_HW = (PS_BLEND_C == 1 && PS_A_MASKED);
const bool NEEDS_TEX = (PS_TFX != 4);
const bool DITHER_ON = (PS_DITHER > 0 && PS_DITHER < 3);

layout(std140, set = 0, binding = 1) uniform cb1
{
	vec3 FogColor;
	float AREF;
	vec4 WH;
	vec2 TA;
	float MaxDepthPS;
	float Af;
	uvec4 FbMask;
	vec4 HalfTexel;
	vec4 MinMax;
	vec4 LODParams;
	vec4 STRange;
	ivec4 ChannelShuffle;
	vec2 TC_OffsetHack;
	vec2 STScale;
	vec4 ChannelOffset;
	mat4 DitherMatrix;
	float ScaledScaleFactor;
	float RcpScaleFactor;
	float RtScaleFactor;
	// Fragments the draw's native pixel grid starts at: half a native pixel
	// under the half-pixel offset that aligns the draw to native.
	float NativeOffset;
};

layout(location = 0) in VSOutput
{
	vec4 t;
	vec4 ti;
	flat vec4 c;
} vsIn;

layout(location = 0, index = 0) out vec4 o_col0;
layout(location = 0, index = 1) out vec4 o_col1;

layout(set = 1, binding = 0) uniform sampler2D Texture;
layout(set = 1, binding = 1) uniform texture2D Palette;

#if defined(RT_FETCH)
layout(set = 2, binding = 0) uniform texture2D RtSampler;
vec4 sample_from_rt() { return texelFetch(RtSampler, ivec2(gl_FragCoord.xy), 0); }
#elif defined(RT_SUBPASS)
layout(input_attachment_index = 0, set = 2, binding = 0) uniform subpassInput RtSampler;
vec4 sample_from_rt() { return subpassLoad(RtSampler); }
#else
// The module for a draw that never reads its target
vec4 sample_from_rt() { return vec4(0.0f); }
#endif

layout(set = 2, binding = 1) uniform texture2D PrimMinTexture;

vec4 sample_c(vec2 uv)
{
	if (PS_TEX_IS_FB)
		return sample_from_rt();
	if (PS_REGION_RECT)
		return texelFetch(Texture, ivec2(uv), 0);

	if (!PS_ADJS && !PS_ADJT)
	{
		uv *= STScale;
	}
	else
	{
		if (PS_ADJS)
			uv.x = (uv.x - STRange.x) * STRange.z;
		else
			uv.x = uv.x * STScale.x;
		if (PS_ADJT)
			uv.y = (uv.y - STRange.y) * STRange.w;
		else
			uv.y = uv.y * STScale.y;
	}

	if (PS_AUTOMATIC_LOD)
	{
		return texture(Texture, uv);
	}
	else if (PS_MANUAL_LOD)
	{
		// FIXME add LOD: K - ( LOG2(Q) * (1 << L))
		float K = LODParams.x;
		float L = LODParams.y;
		float bias = LODParams.z;
		float max_lod = LODParams.w;

		float gs_lod = K - log2(abs(vsIn.t.w)) * L;
		// FIXME max useful ?
		//float lod = max(min(gs_lod, max_lod) - bias, 0.0f);
		float lod = min(gs_lod, max_lod) - bias;

		return textureLod(Texture, uv, lod);
	}
	return textureLod(Texture, uv, 0); // No lod
}

vec4 sample_p(uint idx)
{
	// A palette read from a target drawn at an integer scale keeps the
	// target's samples (GSClut::Read32): an entry is as many texels wide as
	// the palette has rows. The fragment's place inside its native pixel
	// picks the sample, as PS_SAMPLE_MAP picks a texel's; a palette at
	// native size has only the one.
	int n = textureSize(Palette, 0).y;
	ivec2 sub = ivec2(0);
	if (n > 1)
	{
		float s = RtScaleFactor;
		vec2 frag = gl_FragCoord.xy;
		vec2 start = floor((frag - NativeOffset) / s) * s + NativeOffset;
		sub = min(ivec2((frag - start) * float(n) / s), ivec2(n - 1));
	}
	return texelFetch(Palette, ivec2(int(idx) * n + sub.x, sub.y), 0);
}

vec4 sample_p_norm(float u)
{
	return sample_p(uint(u * 255.5f));
}

vec4 clamp_wrap_uv(vec4 uv)
{
	vec4 tex_size = WH.xyxy;

	if (PS_WMS == PS_WMT)
	{
		if (PS_REGION_RECT && PS_WMS == 0)
		{
			uv = fract(uv);
		}
		else if (PS_REGION_RECT && PS_WMS == 1)
		{
			uv = clamp(uv, vec4(0.0f), vec4(1.0f));
		}
		else if (PS_WMS == 2)
		{
			uv = clamp(uv, MinMax.xyxy, MinMax.zwzw);
		}
		else if (PS_WMS == 3)
		{
			if (!PS_FST)
			{
				// wrap negative uv coords to avoid an off by one error that shifted
				// textures. Fixes Xenosaga's hair issue.
				uv = fract(uv);
			}
			uv = vec4((uvec4(uv * tex_size) & floatBitsToUint(MinMax.xyxy)) | floatBitsToUint(MinMax.zwzw)) / tex_size;
		}
	}
	else
	{
		if (PS_REGION_RECT && PS_WMS == 0)
		{
			uv.xz = fract(uv.xz);
		}
		else if (PS_REGION_RECT && PS_WMS == 1)
		{
			uv.xz = clamp(uv.xz, vec2(0.0f), vec2(1.0f));
		}
		else if (PS_WMS == 2)
		{
			uv.xz = clamp(uv.xz, MinMax.xx, MinMax.zz);
		}
		else if (PS_WMS == 3)
		{
			if (!PS_FST)
				uv.xz = fract(uv.xz);
			uv.xz = vec2((uvec2(uv.xz * tex_size.xx) & floatBitsToUint(MinMax.xx)) | floatBitsToUint(MinMax.zz)) / tex_size.xx;
		}
		if (PS_REGION_RECT && PS_WMT == 0)
		{
			uv.yw = fract(uv.yw);
		}
		else if (PS_REGION_RECT && PS_WMT == 1)
		{
			uv.yw = clamp(uv.yw, vec2(0.0f), vec2(1.0f));
		}
		else if (PS_WMT == 2)
		{
			uv.yw = clamp(uv.yw, MinMax.yy, MinMax.ww);
		}
		else if (PS_WMT == 3)
		{
			if (!PS_FST)
				uv.yw = fract(uv.yw);
			uv.yw = vec2((uvec2(uv.yw * tex_size.yy) & floatBitsToUint(MinMax.yy)) | floatBitsToUint(MinMax.ww)) / tex_size.yy;
		}
	}

	if (PS_REGION_RECT)
	{
		// Normalized -> Integer Coordinates.
		uv = clamp(uv * WH.zwzw + STRange.xyxy, STRange.xyxy, STRange.zwzw);
	}

	return uv;
}

mat4 sample_4c(vec4 uv)
{
	mat4 c;

	c[0] = sample_c(uv.xy);
	c[1] = sample_c(uv.zy);
	c[2] = sample_c(uv.xw);
	c[3] = sample_c(uv.zw);

	return c;
}

uvec4 sample_4_index(vec4 uv)
{
	vec4 c;

	c.x = sample_c(uv.xy).a;
	c.y = sample_c(uv.zy).a;
	c.z = sample_c(uv.xw).a;
	c.w = sample_c(uv.zw).a;

	// Denormalize value
	uvec4 i;
	if (PS_RTA_SRC_CORRECTION)
		i = uvec4(round(c * 128.25f));
	else
		i = uvec4(c * 255.5f);

	if (PS_PAL_FMT == 1)
		return i & 0xFu; // 4HL
	if (PS_PAL_FMT == 2)
		return i >> 4u;  // 4HH
	return i;                // 8
}

mat4 sample_4p(uvec4 u)
{
	mat4 c;

	c[0] = sample_p(u.x);
	c[1] = sample_p(u.y);
	c[2] = sample_p(u.z);
	c[3] = sample_p(u.w);

	return c;
}

vec4 fetch_raw_color(ivec2 xy)
{
	if (PS_TEX_IS_FB)
		return sample_from_rt();
	return texelFetch(Texture, xy, 0);
}

int fetch_raw_depth(ivec2 xy)
{
	return int(fetch_raw_color(xy).r * exp2(32.0f));
}

vec4 fetch_c(ivec2 uv)
{
	return fetch_raw_color(uv);
}

//////////////////////////////////////////////////////////////////////
// Depth sampling
//////////////////////////////////////////////////////////////////////

ivec2 clamp_wrap_uv_depth(ivec2 uv)
{
	ivec4 mask = floatBitsToInt(MinMax) << 4;
	if (PS_WMS == PS_WMT)
	{
		if (PS_WMS == 2)
			uv = clamp(uv, mask.xy, mask.zw);
		else if (PS_WMS == 3)
			uv = (uv & mask.xy) | mask.zw;
	}
	else
	{
		if (PS_WMS == 2)
			uv.x = clamp(uv.x, mask.x, mask.z);
		else if (PS_WMS == 3)
			uv.x = (uv.x & mask.x) | mask.z;
		if (PS_WMT == 2)
			uv.y = clamp(uv.y, mask.y, mask.w);
		else if (PS_WMT == 3)
			uv.y = (uv.y & mask.y) | mask.w;
	}
	return uv;
}

vec4 sample_depth(vec2 st, ivec2 pos)
{
	vec2 uv_f = vec2(clamp_wrap_uv_depth(ivec2(st))) * vec2(ScaledScaleFactor);

	if (PS_REGION_RECT)
		uv_f = clamp(uv_f + STRange.xy, STRange.xy, STRange.zw);

	ivec2 uv = ivec2(uv_f);
	vec4 t = vec4(0.0f);

	if (PS_TALES_OF_ABYSS_HLE)
	{
		// Warning: UV can't be used in channel effect
		int depth = fetch_raw_depth(pos);

		// Convert msb based on the palette
		t = sample_p(uint((depth >> 8) & 0xFF)) * 255.0f;
	}
	else if (PS_URBAN_CHAOS_HLE)
	{
		// Depth buffer is read as a RGB5A1 texture. The game try to extract the green channel.
		// So it will do a first channel trick to extract lsb, value is right-shifted.
		// Then a new channel trick to extract msb which will shifted to the left.
		// OpenGL uses a vec32 format for the depth so it requires a couple of conversion.
		// To be faster both steps (msb&lsb) are done in a single pass.

		// Warning: UV can't be used in channel effect
		int depth = fetch_raw_depth(pos);

		// Convert lsb based on the palette
		t = sample_p(uint(depth & 0xFF)) * 255.0f;

		// Msb is easier
		float green = float(((depth >> 8) & 0xFF) * 36.0f);
		green = min(green, 255.0f);
		t.g += green;
	}
	else if (PS_DEPTH_FMT == 1)
	{
		// Based on ps_convert_float32_rgba8 of convert

		// Convert a vec32 depth texture into a RGBA color texture
		uint d = uint(fetch_c(uv).r * exp2(32.0f));
		t = vec4(uvec4((d & 0xFFu), ((d >> 8) & 0xFFu), ((d >> 16) & 0xFFu), (d >> 24)));
	}
	else if (PS_DEPTH_FMT == 2)
	{
		// Based on ps_convert_float16_rgb5a1 of convert

		// Convert a vec32 (only 16 lsb) depth into a RGB5A1 color texture
		uint d = uint(fetch_c(uv).r * exp2(32.0f));
		t = vec4(uvec4((d & 0x1Fu), ((d >> 5) & 0x1Fu), ((d >> 10) & 0x1Fu), (d >> 15) & 0x01u)) * vec4(8.0f, 8.0f, 8.0f, 128.0f);
	}
	else if (PS_DEPTH_FMT == 3)
	{
		// Convert a RGBA/RGB5A1 color texture into a RGBA/RGB5A1 color texture
		t = fetch_c(uv) * 255.0f;
	}

	if (PS_AEM_FMT == FMT_24)
	{
		t.a = (!PS_AEM || any(bvec3(t.rgb))) ? 255.0f * TA.x : 0.0f;
	}
	else if (PS_AEM_FMT == FMT_16)
	{
		t.a = t.a >= 128.0f ? 255.0f * TA.y : (!PS_AEM || any(bvec3(t.rgb))) ? 255.0f * TA.x : 0.0f;
	}
	else if (PS_PAL_FMT != 0 && !PS_TALES_OF_ABYSS_HLE && !PS_URBAN_CHAOS_HLE)
	{
		t = trunc(sample_4p(uvec4(t.aaaa))[0] * 255.0f + 0.05f);
	}

	return t;
}

//////////////////////////////////////////////////////////////////////
// Fetch a Single Channel
//////////////////////////////////////////////////////////////////////

// A channel of the texel: a byte of a depth texture's value, else the
// colour's own
vec4 fetch_channel(ivec2 xy, int shift)
{
	if (PS_DEPTH_FMT == 1 || PS_DEPTH_FMT == 2)
	{
		int depth = (fetch_raw_depth(xy) >> shift) & 0xFF;
		return vec4(float(depth) / 255.0f);
	}
	return fetch_raw_color(xy);
}

vec4 fetch_red(ivec2 xy)
{
	return sample_p_norm(fetch_channel(xy, 0).r) * 255.0f;
}

vec4 fetch_green(ivec2 xy)
{
	return sample_p_norm(fetch_channel(xy, 8).g) * 255.0f;
}

vec4 fetch_blue(ivec2 xy)
{
	return sample_p_norm(fetch_channel(xy, 16).b) * 255.0f;
}

vec4 fetch_alpha(ivec2 xy)
{
	vec4 rt = fetch_raw_color(xy);
	return sample_p_norm(rt.a) * 255.0f;
}

vec4 fetch_rgb(ivec2 xy)
{
	vec4 rt = fetch_raw_color(xy);
	vec4 c = vec4(sample_p_norm(rt.r).r, sample_p_norm(rt.g).g, sample_p_norm(rt.b).b, 1.0);
	return c * 255.0f;
}

vec4 fetch_gXbY(ivec2 xy)
{
	if (PS_DEPTH_FMT == 1 || PS_DEPTH_FMT == 2)
	{
		int depth = fetch_raw_depth(xy);
		int bg = (depth >> (8 + ChannelShuffle.w)) & 0xFF;
		return vec4(bg);
	}
	ivec4 rt = ivec4(fetch_raw_color(xy) * 255.0);
	int green = (rt.g >> ChannelShuffle.w) & ChannelShuffle.z;
	int blue = (rt.b << ChannelShuffle.y) & ChannelShuffle.x;
	return vec4(float(green | blue));
}

vec4 sample_color(vec2 st)
{
	if (PS_TCOFFSETHACK)
		st += TC_OffsetHack.xy;

	vec4 t;
	mat4 c;
	vec2 dd;

	if (!PS_LTF && PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && !PS_REGION_RECT && PS_WMS < 2 && PS_WMT < 2 && !PS_SAMPLE_MAP)
	{
		c[0] = sample_c(st);
	}
	else
	{
		vec4 uv;

		if (PS_LTF)
		{
			if (PS_NATIVE_TAPS)
			{
				// The native draw blends the two texels around each pixel's coordinate;
				// drawn at scale, the taps stay a native texel apart and keep the pixel's
				// weights, taken where the pixel's first fragment sits, while each
				// fragment reads its own samples of those texels: a copy stays a copy, a
				// blur blurs as it does natively.
				float s = RtScaleFactor;
				float ts = 1.0f / RcpScaleFactor;
				vec2 frag = gl_FragCoord.xy;
				vec2 b = floor((frag - 0.5f - NativeOffset) / s) * s + NativeOffset + 0.5f - frag;
				vec2 un = st * WH.xy;
				vec2 un0 = un + dFdx(un) * b.x + dFdy(un) * b.y;
				vec2 g = un0 - 0.5f;
				vec2 gi = floor(g);
				// Each fragment reads its own sample of the pixel's texel: its place in
				// the native pixel, whatever the texels per pixel (a minifying blur at
				// 8x otherwise ran its samples into the next texels, in stripes).
				vec2 rx = max(abs(vec2(dFdx(un).x, dFdy(un).y)) * s, vec2(1.0f));
				vec2 p0;
				if (PS_PAL_FMT != 0)
				{
					// An index is a native texel's, one per texel: the texel's first sample,
					// the one a native draw writes, whatever the fragment's place; the
					// palette colours are what the taps blend.
					p0 = gi * ts + 0.5f;
				}
				else
				{
					// A texture at native size has no samples within a texel: every
					// fragment reads the native texel, as the native draw does.
					p0 = (gi + (un - un0) / rx * step(1.0001f, ts)) * ts + 0.5f;
				}
				dd = g - gi;
				uv = vec4(p0, p0 + ts) / WH.zwzw;
			}
			else
			{
				uv = st.xyxy + HalfTexel;
				dd = fract(uv.xy * WH.zw);

				if (!PS_FST)
					dd = clamp(dd, vec2(0.0f), vec2(0.9999999f));
			}
		}
		else
		{
			uv = st.xyxy;
		}

		if (PS_SAMPLE_MAP)
		{
			// The texel is the one the native-resolution pixel would read: the
			// coordinate is taken where the first fragment of that pixel sits,
			// which is where a native draw samples, so the draw picks the same
			// texels at any scale. This fragment's position inside the native
			// pixel then picks the sample inside that texel; a texture at native
			// size has only the one.
			float s = RtScaleFactor;
			float ts = 1.0f / RcpScaleFactor;
			vec2 frag = gl_FragCoord.xy;
			// The native pixel is the draw's: its grid starts NativeOffset fragments
			// in, where a half-pixel offset aligned to native puts the draw.
			vec2 d = floor((frag - NativeOffset) / s) * s + NativeOffset + 0.5f - frag;
			vec2 st_n = uv.xy + dFdx(uv.xy) * d.x + dFdy(uv.xy) * d.y;
			ivec2 texel = ivec2(floor(st_n * WH.xy));
			ivec2 size = ivec2(WH.xy);
			if (PS_WMS == 0)
				texel.x &= size.x - 1;
			else if (PS_WMS == 1)
				texel.x = clamp(texel.x, 0, size.x - 1);
			else if (PS_WMS == 2)
				texel.x = clamp(texel.x, int(MinMax.x * WH.x), int(MinMax.z * WH.x));
			else
				texel.x = (texel.x & floatBitsToInt(MinMax.x)) | floatBitsToInt(MinMax.z);
			if (PS_WMT == 0)
				texel.y &= size.y - 1;
			else if (PS_WMT == 1)
				texel.y = clamp(texel.y, 0, size.y - 1);
			else if (PS_WMT == 2)
				texel.y = clamp(texel.y, int(MinMax.y * WH.y), int(MinMax.w * WH.y));
			else
				texel.y = (texel.y & floatBitsToInt(MinMax.y)) | floatBitsToInt(MinMax.w);
			ivec2 p = texel * ivec2(ts) + ivec2(0.5f - d) * ivec2(ts) / ivec2(s);
			if (PS_REGION_RECT)
				p = clamp(p + ivec2(STRange.xy), ivec2(STRange.xy), ivec2(STRange.zw));
			if (PS_PAL_FMT != 0)
				c[0] = sample_p(uint(texelFetch(Texture, p, 0).a * 255.5f));
			else
				c[0] = texelFetch(Texture, p, 0);
		}
		else
		{
			uv = clamp_wrap_uv(uv);

			if (PS_PAL_FMT != 0)
				c = sample_4p(sample_4_index(uv));
			else
				c = sample_4c(uv);
		}
	}

	for (uint i = 0; i < 4; i++)
	{
		if (PS_AEM_FMT == FMT_24)
			c[i].a = (!PS_AEM || any(bvec3(c[i].rgb))) ? TA.x : 0.0f;
		else if (PS_AEM_FMT == FMT_16)
			c[i].a = (c[i].a >= 0.5) ? TA.y : ((!PS_AEM || any(bvec3(ivec3(c[i].rgb * 255.0f) & ivec3(0xF8)))) ? TA.x : 0.0f);
	}

	if (PS_LTF)
		t = mix(mix(c[0], c[1], dd.x), mix(c[2], c[3], dd.x), dd.y);
	else
		t = c[0];

	if (PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && PS_RTA_SRC_CORRECTION)
		t.a = t.a * (128.5f / 255.0f);
	return trunc(t * 255.0f + 0.05f);
}

vec4 tfx(vec4 T, vec4 C)
{
	vec4 C_out;
	vec4 FxT = trunc((C * T) / 128.0f);

	if (PS_TFX == 0)
	{
		C_out = FxT;
	}
	else if (PS_TFX == 1)
	{
		C_out = T;
	}
	else if (PS_TFX == 2)
	{
		C_out.rgb = FxT.rgb + C.a;
		C_out.a = T.a + C.a;
	}
	else if (PS_TFX == 3)
	{
		C_out.rgb = FxT.rgb + C.a;
		C_out.a = T.a;
	}
	else
	{
		C_out = C;
	}

	if (!PS_TCC && NEEDS_TEX)
		C_out.a = C.a;

	// Clamp only when it is useful
	if (PS_TFX == 0 || PS_TFX == 2 || PS_TFX == 3)
		C_out = min(C_out, 255.0f);

	return C_out;
}

bool atst(vec4 C)
{
	float a = C.a;

	if (PS_ATST == 1)
		return (a <= AREF);
	if (PS_ATST == 2)
		return (a >= AREF);
	if (PS_ATST == 3)
		return (abs(a - AREF) <= 0.5f);
	if (PS_ATST == 4)
		return (abs(a - AREF) >= 0.5f);
	// nothing to do
	return true;
}

vec4 fog(vec4 c, float f)
{
	if (PS_FOG)
		c.rgb = trunc(mix(FogColor, c.rgb, f));

	return c;
}

// The texture coordinates, where the texture is sampled
vec2 tex_st()
{
	// The GS holds S/Q and T/Q in a signed 12-bit range; past it the
	// coordinate saturates rather than wrapping (parallel-gs does the same).
	if (!PS_FST)
		return clamp(vsIn.t.xy / vsIn.t.w, vec2(-2047.0f), vec2(2047.0f));
	return vsIn.ti.xy;
}

vec2 tex_st_int()
{
	if (!PS_FST)
		return vsIn.ti.zw / vsIn.t.w;
	return vsIn.ti.zw;
}

vec4 ps_color()
{
	vec4 T;
	ivec2 fetch_pos = ivec2(gl_FragCoord.xy + ChannelOffset.xy);
	if (!NEEDS_TEX)
		T = vec4(0.0f);
	else if (PS_CHANNEL_FETCH == 1)
		T = fetch_red(fetch_pos);
	else if (PS_CHANNEL_FETCH == 2)
		T = fetch_green(fetch_pos);
	else if (PS_CHANNEL_FETCH == 3)
		T = fetch_blue(fetch_pos);
	else if (PS_CHANNEL_FETCH == 4)
		T = fetch_alpha(fetch_pos);
	else if (PS_CHANNEL_FETCH == 5)
		T = fetch_rgb(fetch_pos);
	else if (PS_CHANNEL_FETCH == 6)
		T = fetch_gXbY(fetch_pos);
	else if (PS_DEPTH_FMT > 0)
		T = sample_depth(tex_st_int(), ivec2(gl_FragCoord.xy));
	else
		T = sample_color(tex_st());

	if (PS_SHUFFLE && !PS_READ16_SRC && !PS_SHUFFLE_SAME)
	{
		uvec4 denorm_c_before = uvec4(T);
		if ((PS_PROCESS_BA & SHUFFLE_READ) != 0)
		{
			T.r = float((denorm_c_before.b << 3) & 0xF8);
			T.g = float(((denorm_c_before.b >> 2) & 0x38) | ((denorm_c_before.a << 6) & 0xC0));
			T.b = float((denorm_c_before.a << 1) & 0xF8);
			T.a = float(denorm_c_before.a & 0x80);
		}
		else
		{
			T.r = float((denorm_c_before.r << 3) & 0xF8);
			T.g = float(((denorm_c_before.r >> 2) & 0x38) | ((denorm_c_before.g << 6) & 0xC0));
			T.b = float((denorm_c_before.g << 1) & 0xF8);
			T.a = float(denorm_c_before.g & 0x80);
		}
		T.a = ((T.a >= 127.5f) ? TA.y : ((!PS_AEM || any(bvec3(ivec3(T.rgb) & ivec3(0xF8)))) ? TA.x : 0.0f)) * 255.0f;
	}

	vec4 C = tfx(T, vsIn.c);

	C = fog(C, vsIn.t.z);

	return C;
}

void ps_fbmask(inout vec4 C)
{
	if (PS_FBMASK)
	{
		vec4 RT;
		if (PS_COLCLIP_HW)
			RT = trunc(sample_from_rt() * 65535.0f);
		else
			RT = trunc(sample_from_rt() * 255.0f + 0.1f);
		C = vec4((uvec4(C) & ~FbMask) | (uvec4(RT) & FbMask));
	}
}

void ps_dither(inout vec3 C, float As)
{
	if (DITHER_ON)
	{
		ivec2 fpos;

		if (PS_DITHER == 2)
			fpos = ivec2(gl_FragCoord.xy);
		else
			fpos = ivec2(gl_FragCoord.xy * RcpScaleFactor);

		float value = DitherMatrix[fpos.y & 3][fpos.x & 3];

		// The idea here is we add on the dither amount adjusted by the alpha before it goes to the hw blend
		// so after the alpha blend the resulting value should be the same as (Cs - Cd) * As + Cd + Dither.
		if (PS_DITHER_ADJUST)
		{
			float Alpha = (PS_BLEND_C == 2) ? Af : As;
			value *= Alpha > 0.0f ? min(1.0f / Alpha, 1.0f) : 1.0f;
		}

		if (PS_ROUND_INV)
			C -= value;
		else
			C += value;
	}
}

void ps_color_clamp_wrap(inout vec3 C)
{
	// When dithering the bottom 3 bits become meaningless and cause lines in the picture
	// so we need to limit the color depth on dithered items
	if (SW_BLEND || DITHER_ON || PS_FBMASK)
	{
		if (PS_DST_FMT == FMT_16 && PS_BLEND_MIX == 0 && PS_ROUND_INV)
			C += 7.0f; // Need to round up, not down since the shader will invert

		// Correct the Color value based on the output format
		if (!PS_COLCLIP && !PS_COLCLIP_HW)
		{
			// Standard Clamp
			C = clamp(C, vec3(0.0f), vec3(255.0f));
		}

		// FIXME rouding of negative float?
		// compiler uses trunc but it might need floor

		// Warning: normally blending equation is mult(A, B) = A * B >> 7. GPU have the full accuracy
		// GS: Color = 1, Alpha = 255 => output 1
		// GPU: Color = 1/255, Alpha = 255/255 * 255/128 => output 1.9921875
		if (PS_DST_FMT == FMT_16 && PS_DITHER != 3 && (PS_BLEND_MIX == 0 || PS_DITHER > 0))
		{
			// In 16 bits format, only 5 bits of colors are used. It impacts shadows computation of Castlevania
			C = vec3(ivec3(C) & ivec3(0xF8));
		}
		else if (PS_COLCLIP || PS_COLCLIP_HW)
		{
			C = vec3(ivec3(C) & ivec3(0xFF));
		}
	}
}

void ps_blend(inout vec4 Color, inout vec4 As_rgba)
{
	float As = As_rgba.a;

	if (SW_BLEND)
	{
		// PABE
		if (PS_PABE)
		{
			// As_rgba needed for accumulation blend to manipulate Cd
			// No blending so early exit
			if (As < 1.0f)
			{
				As_rgba.rgb = vec3(0.0f);
				return;
			}

			As_rgba.rgb = vec3(1.0f);
		}

		// The target, where the blend reads it
		vec4 RT = vec4(0.0f);
		if (SW_BLEND_NEEDS_RT)
			RT = sample_from_rt();

		float Ad;
		if (PS_RTA_CORRECTION)
			Ad = floor(RT.a * 128.0f + 0.5f) / 128.0f;
		else
			Ad = trunc(RT.a * 255.0f + 0.1f) / 128.0f;

		if (PS_SHUFFLE)
		{
			uvec4 denorm_rt = uvec4(RT);
			if ((PS_PROCESS_BA & SHUFFLE_WRITE) != 0)
			{
				RT.r = float((denorm_rt.b << 3) & 0xF8);
				RT.g = float(((denorm_rt.b >> 2) & 0x38) | ((denorm_rt.a << 6) & 0xC0));
				RT.b = float((denorm_rt.a << 1) & 0xF8);
				RT.a = float(denorm_rt.a & 0x80);
			}
			else
			{
				RT.r = float((denorm_rt.r << 3) & 0xF8);
				RT.g = float(((denorm_rt.r >> 2) & 0x38) | ((denorm_rt.g << 6) & 0xC0));
				RT.b = float((denorm_rt.g << 1) & 0xF8);
				RT.a = float(denorm_rt.g & 0x80);
			}
		}

		// Let the compiler do its jobs !
		vec3 Cd;
		if (PS_COLCLIP_HW)
			Cd = trunc(RT.rgb * 65535.0f);
		else
			Cd = trunc(RT.rgb * 255.0f + 0.1f);
		vec3 Cs = Color.rgb;

		vec3 A = (PS_BLEND_A == 0) ? Cs : (PS_BLEND_A == 1) ? Cd : vec3(0.0f);
		vec3 B = (PS_BLEND_B == 0) ? Cs : (PS_BLEND_B == 1) ? Cd : vec3(0.0f);
		float C = (PS_BLEND_C == 0) ? As : (PS_BLEND_C == 1) ? Ad : Af;
		vec3 D = (PS_BLEND_D == 0) ? Cs : (PS_BLEND_D == 1) ? Cd : vec3(0.0f);

		// As/Af clamp alpha for Blend mix
		// We shouldn't clamp blend mix with blend hw 1 as we want alpha higher
		float C_clamped = C;
		if (PS_BLEND_MIX > 0 && PS_BLEND_HW != 1 && PS_BLEND_HW != 2)
			C_clamped = min(C_clamped, 1.0f);

		if (PS_BLEND_A == PS_BLEND_B)
		{
			Color.rgb = D;
		}
		// In blend_mix, HW adds on some alpha factor * dst.
		// Truncating here wouldn't quite get the right result because it prevents the <1 bit here from combining with a <1 bit in dst to form a ≥1 amount that pushes over the truncation.
		// Instead, apply an offset to convert HW's round to a floor.
		// Since alpha is in 1/128 increments, subtracting (0.5 - 0.5/128 == 127/256) would get us what we want if GPUs blended in full precision.
		// But they don't.  Details here: https://github.com/PCSX2/pcsx2/pull/6809#issuecomment-1211473399
		// Based on the scripts at the above link, the ideal choice for Intel GPUs is 126/256, AMD 120/256.  Nvidia is a lost cause.
		// 124/256 seems like a reasonable compromise, providing the correct answer 99.3% of the time on Intel (vs 99.6% for 126/256), and 97% of the time on AMD (vs 97.4% for 120/256).
		else if (PS_BLEND_MIX == 2)
		{
			Color.rgb = ((A - B) * C_clamped + D) + (124.0f/256.0f);
		}
		else if (PS_BLEND_MIX == 1)
		{
			Color.rgb = ((A - B) * C_clamped + D) - (124.0f/256.0f);
		}
		else
		{
			Color.rgb = trunc((A - B) * C + D);
		}

		if (PS_BLEND_HW == 1)
		{
			// As or Af
			As_rgba.rgb = vec3(C);
			// Subtract 1 for alpha to compensate for the changed equation,
			// if c.rgb > 255.0f then we further need to adjust alpha accordingly,
			// we pick the lowest overflow from all colors because it's the safest,
			// we divide by 255 the color because we don't know Cd value,
			// changed alpha should only be done for hw blend.
			vec3 alpha_compensate = max(vec3(1.0f), Color.rgb / vec3(255.0f));
			As_rgba.rgb -= alpha_compensate;
		}
		else if (PS_BLEND_HW == 2)
		{
			// Since we can't do Cd*(Aalpha + 1) - Cs*Alpha in hw blend
			// what we can do is adjust the Cs value that will be
			// subtracted, this way we can get a better result in hw blend.
			// Result is still wrong but less wrong than before.
			float division_alpha = 1.0f + C;
			Color.rgb /= vec3(division_alpha);
		}
		else if (PS_BLEND_HW == 3)
		{
			// As, Ad or Af clamped.
			As_rgba.rgb = vec3(C_clamped);
			// Cs*(Alpha + 1) might overflow, if it does then adjust alpha value
			// that is sent on second output to compensate.
			vec3 overflow_check = (Color.rgb - vec3(255.0f)) / 255.0f;
			vec3 alpha_compensate = max(vec3(0.0f), overflow_check);
			As_rgba.rgb -= alpha_compensate;
		}
	}
	else
	{
		vec3 Alpha = vec3((PS_BLEND_C == 2) ? Af : As);

		if (PS_BLEND_HW == 1)
		{
			// Needed for Cd * (As/Ad/F + 1) blending modes
			Color.rgb = vec3(255.0f);
		}
		else if (PS_BLEND_HW == 2)
		{
			// Cd*As,Cd*Ad or Cd*F

			Color.rgb = max(vec3(0.0f), (Alpha - vec3(1.0f)));
			Color.rgb *= vec3(255.0f);
		}
		else if (PS_BLEND_HW == 3 && !PS_RTA_CORRECTION)
		{
			// Needed for Cs*Ad, Cs*Ad + Cd, Cd - Cs*Ad
			// Multiply Color.rgb by (255/128) to compensate for wrong Ad/255 value when rgb are below 128.
			// When any color channel is higher than 128 then adjust the compensation automatically
			// to give us more accurate colors, otherwise they will be wrong.
			// The higher the value (>128) the lower the compensation will be.
			float max_color = max(max(Color.r, Color.g), Color.b);
			float color_compensate = 255.0f / max(128.0f, max_color);
			Color.rgb *= vec3(color_compensate);
		}
		else if (PS_BLEND_HW == 4)
		{
			// Needed for Cd * (1 - Ad) and Cd*(1 + Alpha).

			As_rgba.rgb = Alpha * vec3(128.0f / 255.0f);
			Color.rgb = vec3(127.5f);
		}
		else if (PS_BLEND_HW == 5)
		{
			// Needed for Cs*Alpha + Cd*(1 - Alpha).
			Alpha *= vec3(128.0f / 255.0f);
			As_rgba.rgb = (Alpha - vec3(0.5f));
			Color.rgb = (Color.rgb * Alpha);
		}
		else if (PS_BLEND_HW == 6)
		{
			// Needed for Cd*Alpha + Cs*(1 - Alpha).
			Alpha *= vec3(128.0f / 255.0f);
			As_rgba.rgb = Alpha;
			Color.rgb *= (Alpha - vec3(0.5f));
		}
	}
}

void main()
{
	if ((PS_SCANMSK & 2) != 0)
	{
		// fail depth test on prohibited lines
		if ((int(gl_FragCoord.y) & 1) == (PS_SCANMSK & 1))
			discard;
	}

	if (PS_DATE >= 5)
	{
		// Pseudo 16 bits access.
		float rt_a = PS_WRITE_RG ? sample_from_rt().g : sample_from_rt().a;

		bool bad;
		if ((PS_DATE & 3) == 1)
		{
			// DATM == 0: Pixel with alpha equal to 1 will failed
			bad = (PS_RTA_CORRECTION ? (254.5f / 255.0f) : (127.5f / 255.0f)) < rt_a;
		}
		else
		{
			// DATM == 1: Pixel with alpha equal to 0 will failed
			bad = rt_a < (PS_RTA_CORRECTION ? (254.5f / 255.0f) : (127.5f / 255.0f));
		}

		if (bad)
			discard;
	}

	if (PS_DATE == 3)
	{
		int stencil_ceil = int(texelFetch(PrimMinTexture, ivec2(gl_FragCoord.xy), 0).r);
		// Note gl_PrimitiveID == stencil_ceil will be the primitive that will update
		// the bad alpha value so we must keep it.

		if (gl_PrimitiveID > stencil_ceil)
			discard;
	}

	vec4 C = ps_color();
	bool atst_pass = atst(C);

	if (PS_AFAIL == 0) // KEEP or ATST off
	{
		if (!atst_pass)
			discard;
	}

	// Must be done before alpha correction

	// AA (Fixed one) will output a coverage of 1.0 as alpha
	if (PS_FIXED_ONE_A)
		C.a = 128.0f;

	vec4 alpha_blend;
	if (SW_AD_TO_HW)
	{
		vec4 RT;
		if (PS_RTA_CORRECTION)
			RT = floor(sample_from_rt() * 128.0f + 0.5f);
		else
			RT = trunc(sample_from_rt() * 255.0f + 0.1f);

		alpha_blend = vec4(RT.a / 128.0f);
	}
	else
	{
		alpha_blend = vec4(C.a / 128.0f);
	}

	// Correct the ALPHA value based on the output format
	if (PS_DST_FMT == FMT_16)
	{
		float A_one = 128.0f; // alpha output will be 0x80
		C.a = PS_FBA ? A_one : step(128.0f, C.a) * A_one;
	}
	else if (PS_DST_FMT == FMT_32 && PS_FBA)
	{
		if (C.a < 128.0f)
			C.a += 128.0f;
	}

	// Get first primitive that will write a failling alpha value
	if (PS_DATE == 1)
	{
		// DATM == 0
		// Pixel with alpha equal to 1 will failed (128-255)
		o_col0 = (C.a > 127.5f) ? vec4(gl_PrimitiveID) : vec4(0x7FFFFFFF);
	}
	else if (PS_DATE == 2)
	{
		// DATM == 1
		// Pixel with alpha equal to 0 will failed (0-127)
		o_col0 = (C.a < 127.5f) ? vec4(gl_PrimitiveID) : vec4(0x7FFFFFFF);
	}
	else
	{
		ps_blend(C, alpha_blend);

		if (PS_SHUFFLE)
		{
			if (!PS_READ16_SRC && !PS_SHUFFLE_SAME)
			{
				uvec4 denorm_c_after = uvec4(C);
				if ((PS_PROCESS_BA & SHUFFLE_READ) != 0)
				{
					C.b = float(((denorm_c_after.r >> 3) & 0x1F) | ((denorm_c_after.g << 2) & 0xE0));
					C.a = float(((denorm_c_after.g >> 6) & 0x3) | ((denorm_c_after.b >> 1) & 0x7C) | (denorm_c_after.a & 0x80));
				}
				else
				{
					C.r = float(((denorm_c_after.r >> 3) & 0x1F) | ((denorm_c_after.g << 2) & 0xE0));
					C.g = float(((denorm_c_after.g >> 6) & 0x3) | ((denorm_c_after.b >> 1) & 0x7C) | (denorm_c_after.a & 0x80));
				}
			}

			// Special case for 32bit input and 16bit output, shuffle used by The Godfather
			if (PS_SHUFFLE_SAME)
			{
				if ((PS_PROCESS_BA & SHUFFLE_READ) != 0)
				{
					uvec4 denorm_c = uvec4(C);
					C = vec4(float((denorm_c.b & 0x7Fu) | (denorm_c.a & 0x80u)));
				}
				else
				{
					C.ga = C.rg;
				}
			}
			// Copy of a 16bit source in to this target
			else if (PS_READ16_SRC)
			{
				uvec4 denorm_c = uvec4(C);
				uvec2 denorm_TA = uvec2(vec2(TA.xy) * 255.0f + 0.5f);
				C.rb = vec2(float((denorm_c.r >> 3) | (((denorm_c.g >> 3) & 0x7u) << 5)));
				C.ga = vec2(float((denorm_c.g >> 6) | ((denorm_c.b >> 3) << 2) | (denorm_TA.x & 0x80u)));
			}
			// Write RB part. Mask will take care of the correct destination
			else if (PS_SHUFFLE_ACROSS)
			{
				if (PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE)
				{
					C.rb = C.br;
					float g_temp = C.g;

					C.g = C.a;
					C.a = g_temp;
				}
				else if ((PS_PROCESS_BA & SHUFFLE_READ) != 0)
				{
					C.rb = C.bb;
					C.ga = C.aa;
				}
				else
				{
					C.rb = C.rr;
					C.ga = C.gg;
				}
			}
		}

		ps_dither(C.rgb, alpha_blend.a);

		// Color clamp/wrap needs to be done after sw blending and dithering
		ps_color_clamp_wrap(C.rgb);

		ps_fbmask(C);

		if (PS_AFAIL == 3) // RGB_ONLY
		{
			// Use alpha blend factor to determine whether to update A.
			alpha_blend.a = float(atst_pass);
		}

		if (!PS_NO_COLOR)
		{
			if (PS_RTA_CORRECTION)
				o_col0.a = C.a / 128.0f;
			else
				o_col0.a = C.a / 255.0f;
			if (PS_COLCLIP_HW)
				o_col0.rgb = vec3(C.rgb / 65535.0f);
			else
				o_col0.rgb = C.rgb / 255.0f;
			o_col1 = alpha_blend;
		}

		gl_FragDepth = min(gl_FragCoord.z, MaxDepthPS);
	}
}

#endif
