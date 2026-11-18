/* SPDX-License-Identifier: LGPL-3.0+ */
/* The CPU-only device (GS/Renderers/SW/GSDeviceSW.c) held to the C++ it
 * replaced (reference.cpp, kept as it was): the same random merges,
 * deinterlaces of every mode, copies, stretches and presents through
 * both, on textures of the same random texels, every destination texel
 * and every frame handed to the frontend compared byte for byte - with
 * the frontend offering a buffer of its own or not. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "reference.h"
#include "GS/Renderers/Common/GSTextureFormat.h"
#include "GS/Renderers/SW/GSDeviceSW.h"

extern "C" {
static void quiet(enum retro_log_level, const char*, ...) {}
retro_log_printf_t log_cb = quiet;
retro_environment_t environ_cb;
retro_video_refresh_t video_cb;
/* (the base device's, which these ops do not reach) */
void gs_device_init(gs_device*) {}
void gs_device_deinit(gs_device*) {}
void gs_device_acquire_window(gs_device*) {}
void gs_device_destroy_base(gs_device*) {}
void gs_device_features_init(gs_device_features*) {}
}

static u32 s_rng = 0x3c6ef372u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static int rnd_n(int n) { return (int)(rnd() % (u32)n); }

/* The frontend: its buffer offered or not, and the frames it was given */
static int s_offer;
static u8* s_offer_buf;
static std::vector<u8>* s_frames;

static bool env(unsigned cmd, void* data)
{
	if (cmd == RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER && s_offer)
	{
		retro_framebuffer* fb = (retro_framebuffer*)data;
		/* (now and then the wrong size, or format: refused) */
		fb->data   = s_offer_buf;
		fb->pitch  = (fb->width * 4 + 63) & ~63u;
		fb->format = s_offer == 2 ? RETRO_PIXEL_FORMAT_RGB565 : RETRO_PIXEL_FORMAT_XRGB8888;
		return true;
	}
	return false;
}

static void video(const void* data, unsigned w, unsigned h, size_t pitch)
{
	unsigned y;
	const u32 hdr[3] = {w, h, data == s_offer_buf ? 1u : 0u};
	s_frames->insert(s_frames->end(), (const u8*)hdr, (const u8*)hdr + sizeof(hdr));
	for (y = 0; y < h; y++)
		s_frames->insert(s_frames->end(), (const u8*)data + y * pitch, (const u8*)data + y * pitch + w * 4);
}

/* A pair of textures, one each side, of the same texels */
struct Tex
{
	gs_texture* a;
	gs_texture* b;
};

static Tex tex_new(int w, int h)
{
	Tex t;
	gs_texture_sw* s;
	int y, x;
	t.a = gs_texture_sw_create(GS_TEXTYPE_RENDER_TARGET, w, h, GS_TEX_COLOR);
	t.b = gs_texture_sw_create(GS_TEXTYPE_RENDER_TARGET, w, h, GS_TEX_COLOR);
	s = (gs_texture_sw*)t.a;
	/* noise, or a picture that changes little from texel to texel and
	 * field to field (where the motion test has something to tell) */
	const int smooth = rnd_n(2);
	const int noise = 1 + rnd_n(48);
	for (y = 0; y < h; y++)
		for (x = 0; x < w * 4; x++)
		{
			u8 v = smooth ? (u8)(((x >> 2) + y + (x & 3) * 50) + rnd_n(noise)) : (u8)rnd();
			/* (alphas around 0x80, the PS2's opaque) */
			if ((x & 3) == 3 && rnd_n(2))
				v = (u8)(0x70 + rnd_n(0x20));
			s->data[y * s->pitch + x] = v;
		}
	for (y = 0; y < h; y++)
		memcpy(((gs_texture_sw*)t.b)->data + y * ((gs_texture_sw*)t.b)->pitch, s->data + y * s->pitch, (size_t)w * 4);
	return t;
}

static void tex_free(Tex t)
{
	t.a->m_ops->free(t.a);
	t.b->m_ops->free(t.b);
}

static bool tex_same(Tex t)
{
	const gs_texture_sw* a = (const gs_texture_sw*)t.a;
	const gs_texture_sw* b = (const gs_texture_sw*)t.b;
	int y;
	for (y = 0; y < a->base.m_size.y; y++)
		if (memcmp(a->data + y * a->pitch, b->data + y * b->pitch, (size_t)a->base.m_size.x * 4))
			return false;
	return true;
}

static float edge(int n)
{
	switch (rnd_n(6))
	{
		case 0: return 0.0f;
		case 1: return (float)n;
		case 2: return (float)(rnd_n(n + 40) - 20) + 0.5f;
		default: return (float)(rnd_n(n + 40) - 20) + (float)rnd_n(8) / 8.0f;
	}
}

/* A source rect, normalised: whole, inside, or past the edges */
static GSVector4 nrect(void)
{
	if (rnd_n(2))
		return GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
	return GSVector4((float)(rnd_n(7) - 2) / 8.0f, (float)(rnd_n(7) - 2) / 8.0f, 0.5f + (float)rnd_n(7) / 8.0f,
		0.5f + (float)rnd_n(7) / 8.0f);
}

static long s_checks;

int main(void)
{
	gs_device_sw* dev = (gs_device_sw*)memalign_alloc(32, sizeof(gs_device_sw));
	RefDeviceSW* ref = new RefDeviceSW();
	gs_device* d = &dev->base;
	const gs_device_ops* ops = &gs_device_sw_ops;
	std::vector<u8> frames_a, frames_b;
	int n;

	setvbuf(stdout, NULL, _IONBF, 0);
	memset(dev, 0, sizeof(*dev));
	s_offer_buf = (u8*)malloc(1024 * 1024 * 4);
	environ_cb = env;
	video_cb = video;
	dev->base.m_ops = ops;

	for (n = 0; n < 4000; n++)
	{
		const int op = rnd_n(6);
		const int w = 1 + rnd_n(200), h = 1 + rnd_n(160);
		Tex dst = tex_new(w, h);
		bool same;

		if (op == 0)
		{
			/* the merge: circuits of their own sizes, rects anywhere */
			Tex s0 = tex_new(1 + rnd_n(200), 1 + rnd_n(160)), s1 = tex_new(1 + rnd_n(200), 1 + rnd_n(160));
			GSTexture* sa[3];
			gs_texture* sb[3];
			GSVector4 sr[3], dr[3];
			gs_rectf sr_c[3], dr_c[3];
			GSRegPMODE pmode;
			GSRegEXTBUF extbuf;
			const u32 c = rnd();
			int k;
			memset(&pmode, 0, sizeof(pmode));
			memset(&extbuf, 0, sizeof(extbuf));
			pmode.SLBG = rnd_n(2);
			pmode.MMOD = rnd_n(2);
			for (k = 0; k < 3; k++)
			{
				const int full = rnd_n(2);
				sr[k] = nrect();
				dr[k] = full ? GSVector4(0.0f, 0.0f, (float)((gs_texture_sw*)(k ? s1.a : s0.a))->base.m_size.x, (float)h)
					: GSVector4(edge(w), edge(h), edge(w), edge(h));
				memcpy(&sr_c[k], &sr[k], 16);
				memcpy(&dr_c[k], &dr[k], 16);
			}
			sa[0] = rnd_n(4) ? s0.a : NULL;
			sa[1] = rnd_n(4) ? s1.a : NULL;
			sa[2] = NULL;
			sb[0] = sa[0] ? s0.b : NULL;
			sb[1] = sa[1] ? s1.b : NULL;
			sb[2] = NULL;
			ref->DoMerge(sa, sr, dst.a, dr, pmode, extbuf, c, true);
			ops->do_merge(d, sb, sr_c, dst.b, dr_c, &pmode, &extbuf, c, true);
			tex_free(s0);
			tex_free(s1);
		}
		else if (op == 1)
		{
			/* deinterlacing: each mode's textures as GSDevice::Interlace
			 * sizes them */
			const int shader = rnd_n(5);
			const int idx = rnd_n(4);
			const int block = 1 + rnd_n(4);
			const bool half = rnd_n(2) != 0;
			int sh = h, dh = h;
			GSVector4 dr(0.0f, 0.0f, (float)w, (float)h), zrh;
			gs_rectf dr_c, zrh_c, sr_c = {0.0f, 0.0f, 1.0f, 1.0f};
			InterlaceConstantBuffer cb;
			Tex src;
			if (shader == 3)
			{
				/* the frame into a bank of the buffer twice its height */
				const int bank = rnd_n(2);
				dh = 2 * h;
				dr = GSVector4(0.0f, (float)(bank * h), (float)w, (float)((bank + 1) * h));
			}
			else if (shader == 4)
				sh = half ? 4 * (1 + rnd_n(80)) : 2 * h;
			else if (rnd_n(3) == 0)
			{
				/* shifted a row or two, a slot of its own height, or
				 * none at all */
				const float y0 = (float)rnd_n(4) - 1.5f + (float)rnd_n(2) * 0.25f;
				dr = GSVector4(0.0f, y0, (float)w, rnd_n(2) ? y0 + (float)h : (float)(rnd_n(h + 2) - 1));
			}
			tex_free(dst);
			dst = tex_new(w, dh);
			src = tex_new(w, sh);
			zrh = GSVector4((float)idx, 1.0f / (float)dh, (float)(rnd_n(2) ? dh : dh + 1),
				half ? (float)block : -(float)block);
			cb.ZrH = zrh;
			memcpy(&dr_c, &dr, 16);
			memcpy(&zrh_c, &zrh, 16);
			ref->DoInterlace(src.a, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), dst.a, dr, (ShaderInterlace)shader, true, cb);
			ops->do_interlace(d, src.b, &sr_c, dst.b, &dr_c, shader, true, &zrh_c);
			tex_free(src);
		}
		else if (op == 2)
		{
			Tex src = tex_new(1 + rnd_n(200), 1 + rnd_n(160));
			const int x0 = rnd_n(220) - 10, y0 = rnd_n(180) - 10;
			const GSVector4i r(x0, y0, rnd_n(4) ? x0 + rnd_n(100) : rnd_n(220) - 10, rnd_n(4) ? y0 + rnd_n(100) : rnd_n(180) - 10);
			const u32 x = (u32)(rnd_n(2) ? rnd_n(8) : rnd_n(220)), y = (u32)(rnd_n(2) ? rnd_n(8) : rnd_n(180));
			gs_recti r_c = {r.x, r.y, r.z, r.w};
			ref->CopyRect(src.a, dst.a, r, x, y);
			ops->copy_rect(d, src.b, dst.b, &r_c, x, y);
			tex_free(src);
		}
		else if (op == 3)
		{
			Tex src = tex_new(1 + rnd_n(200), 1 + rnd_n(160));
			const GSVector4 sr = nrect();
			const GSVector4 dr = rnd_n(2) ? GSVector4(0.0f, 0.0f, (float)w, (float)h) : GSVector4(edge(w), edge(h), edge(w), edge(h));
			gs_rectf sr_c, dr_c;
			memcpy(&sr_c, &sr, 16);
			memcpy(&dr_c, &dr, 16);
			if (rnd_n(2))
			{
				ref->StretchRect(src.a, sr, dst.a, dr, ShaderConvert::COPY, true);
				ops->stretch_rect(d, src.b, &sr_c, dst.b, &dr_c, 0, true);
			}
			else
			{
				ref->StretchRect(src.a, sr, dst.a, dr, true, false, true, false, ShaderConvert::COPY);
				ops->stretch_rect_mask(d, src.b, &sr_c, dst.b, &dr_c, true, false, true, false, 0);
			}
			tex_free(src);
		}
		else
		{
			/* presents: into a texture, or the frontend's frame (its
			 * buffer or the device's), blank or not, then handed over */
			Tex src = tex_new(1 + rnd_n(200), 1 + rnd_n(160));
			const bool skip = rnd_n(8) == 0;
			const int presents = rnd_n(3);
			GSVector4 sr[2], dr[2];
			gs_rectf sr_c[2], dr_c[2];
			int to_tex[2];
			int k, ra, rb;
			for (k = 0; k < presents; k++)
			{
				static const int sizes[4][2] = {{320, 240}, {320, 224}, {64, 48}, {64, 40}};
				const int z = rnd_n(6);
				sr[k] = z ? nrect() : rnd_n(2) ? GSVector4(0.0f, 0.0f, 0.0f, 0.0f) : GSVector4(0.0f, 0.0f, 0.0f, 1.0f);
				to_tex[k] = rnd_n(3) == 0;
				/* (the frame's sizes repeat: the frontend's buffer is asked
				 * for again when they change) */
				dr[k] = to_tex[k] ? GSVector4(edge(w), edge(h), edge(w), edge(h))
					: rnd_n(4) ? GSVector4(0.0f, 0.0f, (float)sizes[rnd_n(4)][0], (float)sizes[rnd_n(4)][1])
					: GSVector4(0.0f, 0.0f, (float)(1 + rnd_n(300)), (float)(1 + rnd_n(240)));
				memcpy(&sr_c[k], &sr[k], 16);
				memcpy(&dr_c[k], &dr[k], 16);
			}
			/* (the frontend's answer stays a while) */
			if (rnd_n(4) == 0)
				s_offer = rnd_n(3);
			frames_a.clear();
			frames_b.clear();

			s_frames = &frames_a;
			ra = ref->BeginPresent(skip);
			for (k = 0; k < presents; k++)
				ref->PresentRect(src.a, sr[k], to_tex[k] ? dst.a : NULL, dr[k]);
			ref->EndPresent();

			s_frames = &frames_b;
			rb = ops->begin_present(d, skip);
			for (k = 0; k < presents; k++)
				ops->present_rect(d, src.b, &sr_c[k], to_tex[k] ? dst.b : NULL, &dr_c[k]);
			ops->end_present(d);

			s_checks += 3;
			if (ra != rb || frames_a != frames_b || !tex_same(src))
			{
				printf("present %d: begin %d and %d, frames of %u and %u bytes%s\n", n, ra, rb,
					(unsigned)frames_a.size(), (unsigned)frames_b.size(), tex_same(src) ? "" : ", the sources differ");
				return 1;
			}
			tex_free(src);
		}

		same = tex_same(dst);
		s_checks++;
		if (!same)
		{
			printf("op %d (%d): the destinations differ (%dx%d)\n", n, op, w, h);
			return 1;
		}
		tex_free(dst);
	}

	printf("gsdevsw: %ld checks, 0 differed\n", s_checks);
	free(s_offer_buf);
	ops->free(d);
	RefFree(ref);
	delete ref;
	return 0;
}
