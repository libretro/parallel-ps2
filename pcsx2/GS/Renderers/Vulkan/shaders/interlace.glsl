// The utility pipelines' shaders (interlace). Compiled ahead of time by
// tools/vk_spirv.py into VKShaders.inc, one module an entry point.

#version 460 core
#extension GL_EXT_samplerless_texture_functions : require

#ifdef VERTEX_SHADER

layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec2 a_tex;

layout(location = 0) out vec2 v_tex;

void main()
{
	gl_Position = vec4(a_pos.x, -a_pos.y, a_pos.z, a_pos.w);
	v_tex = a_tex;
}

#endif

#ifdef FRAGMENT_SHADER

layout(location = 0) in vec2 v_tex;
layout(location = 0) out vec4 o_col0;

layout(push_constant) uniform cb0
{
	vec4 ZrH;
};

layout(set = 0, binding = 0) uniform sampler2D samp0;


// The source row of an output row, as an offset to its own coordinate. A
// half-height field (ZrH.w > 0) was stretched twice into the source, and
// its line's block of rows spans two frame lines, since the field's rows
// are half a frame line apart: the line takes the first half of the block,
// and the second half is the other field's positions. The output's rows
// between the field's are the mean of the two around them, read with the
// linear sampler at the edge between them (the source holds each field
// row twice, so a row's centre reads that row alone). A full-height field
// reads its own row. yb is the frame line with the row's place in it, l
// the line.
float line_rows(float yb, int l, float y, float rcp_h)
{
	if (ZrH.w > 0.0f)
	{
		const float place = ((yb - float(l)) * ZrH.w - 0.5f) * 0.5f; // the row's place in the line, in field rows
		const float row  = floor(float(l >> 1) * ZrH.w) + floor(place);
		return (2.0f * row + 0.5f + 3.0f * (place - floor(place)) - y) * rcp_h;
	}
	return 0.0f;
}

// Weave shader
#ifdef ps_main0
void ps_main0()
{
	const int idx   = int(ZrH.x);          // buffer index passed from CPU
	const int field = idx & 1;             // current field
	// A frame line is |ZrH.w| rows at scale; the rows of a line come from one
	// field, so a field drawn a half line off the other weaves by lines as it
	// does natively, not by rows; line_rows places them.
	const float yb = gl_FragCoord.y / abs(ZrH.w);
	const int   l  = int(yb);

	if ((l & 1) == field)
		o_col0 = textureLod(samp0, vec2(v_tex.x, v_tex.y + line_rows(yb, l, gl_FragCoord.y, ZrH.y)), 0);
	else
		discard;
}
#endif


// Bob shader
#ifdef ps_main1
void ps_main1()
{
	o_col0 = textureLod(samp0, v_tex, 0);
}
#endif


// Blend shader
#ifdef ps_main2
void ps_main2()
{
	vec2 vstep = vec2(0.0f, ZrH.y * abs(ZrH.w)); // the lines above and below, at scale
	vec4 c0 = textureLod(samp0, v_tex - vstep, 0);
	vec4 c1 = textureLod(samp0, v_tex, 0);
	vec4 c2 = textureLod(samp0, v_tex + vstep, 0);

	o_col0 = (c0 + c1 * 2.0f + c2) / 4.0f;
}
#endif


// MAD shader - buffering
#ifdef ps_main3
void ps_main3()
{
	// We take half the lines from the current frame and stores them in the MAD frame buffer.
	// the MAD frame buffer is split in 2 consecutive banks of 2 fields each, the fields in each bank
	// are interleaved (top field at even lines and bottom field at odd lines).
	// When the source texture has an odd vres, the first line of bank 1 would be an odd index
	// causing the wrong lines to be discarded, so a vertical offset (lofs) is added to the vertical
	// position of the destination texture to force the proper field alignment

	const int  idx    = int(ZrH.x);                               // buffer index passed from CPU
	const int  bank   = idx >> 1;                                 // current bank
	const int  field  = idx & 1;                                  // current field
	const int  vres   = int(ZrH.z) >> 1;                          // vertical resolution of source texture
	const int  lofs   = ((((vres + 1) >> 1) << 1) - vres) & bank; // line alignment offset for bank 1
	const float yb = (gl_FragCoord.y + float(lofs)) / abs(ZrH.w);             // frame line at scale (|ZrH.w| rows each; the bank is the source's height, half of ZrH.z)
	const int   l  = int(yb);

	// if the line belongs to the current field we update its rows, otherwise we
	// leave the old rows in the destination buffer (full-height fields; half-height
	// ones are kept whole, see mad_fields)
	if ((l & 1) == field)
		o_col0 = textureLod(samp0, v_tex, 0);
	else
		discard;
}
#endif


// MAD shader - reconstruction
#ifdef ps_main4
// The adaptive reconstruction of half-height fields. The buffer holds the last
// four fields whole, one to a quarter of it, the current one in slot idx. A
// field of parity p drew its line j at frame line 2j + p, and at a scale of
// two or more it holds rows of its own for the frame line after it as well:
// its rows cover the output two rows each from row p * block. At 1x the line
// after it is between two of its rows and reads their mean.
// The current field's lines are taken as they are. Between them, where the
// picture holds still, the previous field's line (weave); where it moves, the
// current field's own picture there, of the same instant.
// Motion is measured once per native pixel, on the first row of its block, in
// luma: the lines above and below against the same field two back, or the
// line itself against its field two back; each fragment blends by it.
vec4 mad_field(int back, float y, float x)
{
	const int   idx    = int(ZrH.x);
	const float slot_h = ZrH.z * 0.5f;
	const int   slot   = (idx - back) & 3;
	const int   par    = (idx ^ back) & 1;
	const float rel    = (y - float(par) * ZrH.w) * 0.5f;
	const float r      = clamp(((ZrH.w >= 2.0f) ? floor(rel) : rel) + 0.5f, 0.5f, slot_h - 0.5f);
	return textureLod(samp0, vec2(x, (float(slot) * slot_h + r) / (ZrH.z * 2.0f)), 0);
}

float mad_luma(vec4 c)
{
	return dot(c.rgb, vec3(0.299f, 0.587f, 0.114f));
}

void mad_fields()
{
	const float block = ZrH.w;
	const int   field = int(ZrH.x) & 1;
	const float y     = floor(gl_FragCoord.y);
	const int   l     = int(y / block);

	const vec4 cur = mad_field(0, y, v_tex.x);
	if ((l & 1) == field)
	{
		o_col0 = cur;
		return;
	}

	const float yl = float(l) * block;
	const float xc = v_tex.x + dFdx(v_tex.x) * ((floor(gl_FragCoord.x / block) + 0.5f) * block - gl_FragCoord.x);
	const float mh = abs(mad_luma(mad_field(0, yl - block, xc)) - mad_luma(mad_field(2, yl - block, xc)));
	const float ml = abs(mad_luma(mad_field(0, yl + block, xc)) - mad_luma(mad_field(2, yl + block, xc)));
	const float mc = abs(mad_luma(mad_field(1, yl, xc)) - mad_luma(mad_field(3, yl, xc)));
	const float moving = smoothstep(0.01f, 0.03f, max(max(mh, ml), mc));

	o_col0 = mix(mad_field(1, y, v_tex.x), cur, moving);
}

void ps_main4()
{
	if (ZrH.w > 0.0f)
	{
		mad_fields();
		return;
	}

	// we use the contents of the MAD frame buffer to reconstruct the missing lines from the current
	// field.

	const int   idx          = int(ZrH.x);                         // buffer index passed from CPU
	const int   bank         = idx >> 1;                           // current bank
	const int   field        = idx & 1;                            // current field
	const float sensitivity  = 0.08f;                                 // higher values mean more likely to use weave
	const vec3  motion_thr   = vec3(1.0, 1.0, 1.0) * sensitivity;  //
	const vec2  bofs         = vec2(0.0f, 0.5f);                   // position of the bank 1 relative to source texture size
	const vec2  vscale       = vec2(1.0f, 0.5f);                   // scaling factor from source to destination texture
	const vec2  lofs         = vec2(0.0f, ZrH.y * abs(ZrH.w)) * vscale;         // distance between two adjacent lines relative to source texture size
	const vec2  iptr         = v_tex * vscale;                     // pointer to the current pixel in the source texture

	vec2 p_t0; // pointer to current pixel (missing or not) from most recent frame
	vec2 p_t1; // pointer to current pixel (missing or not) from one frame back
	vec2 p_t2; // pointer to current pixel (missing or not) from two frames back
	vec2 p_t3; // pointer to current pixel (missing or not) from three frames back

	switch (idx)
	{
		case 1:
			p_t0 = iptr;
			p_t1 = iptr;
			p_t2 = iptr + bofs;
			p_t3 = iptr + bofs;
			break;
		case 2:
			p_t0 = iptr + bofs;
			p_t1 = iptr;
			p_t2 = iptr;
			p_t3 = iptr + bofs;
			break;
		case 3:
			p_t0 = iptr + bofs;
			p_t1 = iptr + bofs;
			p_t2 = iptr;
			p_t3 = iptr;
			break;
		default:
			p_t0 = iptr;
			p_t1 = iptr + bofs;
			p_t2 = iptr + bofs;
			p_t3 = iptr;
			break;
	}

	// calculating motion, only relevant for missing lines where the "center line" is pointed by p_t1

	vec4 hn = textureLod(samp0, p_t0 - lofs, 0); // new high pixel
	vec4 cn = textureLod(samp0, p_t1, 0);        // new center pixel
	vec4 ln = textureLod(samp0, p_t0 + lofs, 0); // new low pixel

	vec4 ho = textureLod(samp0, p_t2 - lofs, 0); // old high pixel
	vec4 co = textureLod(samp0, p_t3, 0);        // old center pixel
	vec4 lo = textureLod(samp0, p_t2 + lofs, 0); // old low pixel

	vec3 mh = hn.rgb - ho.rgb; // high pixel motion
	vec3 mc = cn.rgb - co.rgb; // center pixel motion
	vec3 ml = ln.rgb - lo.rgb; // low pixel motion

	mh = max(mh, -mh) - motion_thr;
	mc = max(mc, -mc) - motion_thr;
	ml = max(ml, -ml) - motion_thr;

	#if 1 // use this code to evaluate each color motion separately
		float mh_max = max(max(mh.x, mh.y), mh.z);
		float mc_max = max(max(mc.x, mc.y), mc.z);
		float ml_max = max(max(ml.x, ml.y), ml.z);
	#else // use this code to evaluate average color motion
		float mh_max = mh.x + mh.y + mh.z;
		float mc_max = mc.x + mc.y + mc.z;
		float ml_max = ml.x + ml.y + ml.z;
	#endif

	// selecting deinterlacing output

	if ((int(gl_FragCoord.y / abs(ZrH.w)) & 1) == field) // output line present on current field
	{
		// output coordinate present on current field
		o_col0 = textureLod(samp0, p_t0, 0);
	}
	else if ((iptr.y > 0.5f - lofs.y) || (iptr.y < 0.0 + lofs.y))
	{
		// top and bottom lines are always weaved
		o_col0 = cn;
	}
	else
	{
		// missing line needs to be reconstructed
		if(((mh_max > 0.0f) || (ml_max > 0.0f)) || (mc_max > 0.0f))
			// high motion -> interpolate pixels above and below
			o_col0 = (hn + ln) / 2.0f;
		else
			// low motion -> weave
			o_col0 = cn;
	}
}
#endif

#endif
