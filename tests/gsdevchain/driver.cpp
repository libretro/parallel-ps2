/* What every device does the same (GS/Renderers/Common/GSDevice.c), on a
 * pretend backend that records every call made of it: the merge into its
 * target, each deinterlace mode for both fields and both field heights,
 * the targets a resize keeps, recycles, retires or frees, the pool's age,
 * a swap of a present texture, the VR frame with and without depth, and
 * the state dropped. Everything recorded goes into one hash. The backend
 * is written against GSDevice's C++ interface, which the device code this
 * replaced had too, so the same driver builds against GSDevice.cpp:
 * ORACLE in build.sh is what it gave. */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "GS/Renderers/Common/GSDevice.h"

EmuGSOptions GSConfig;
static void quiet(enum retro_log_level, const char*, ...) {}
extern "C" retro_log_printf_t log_cb;
retro_log_printf_t log_cb = quiet;
extern "C" bool GSIsHardwareRenderer(void) { return true; }
extern "C" WindowInfo host_acquire_render_window(void)
{
	WindowInfo w;
	w.surface_width  = 640;
	w.surface_height = 448;
	return w;
}
extern "C" const char* GSTextureCacheMemoryUsage(void) { return "-"; }
#ifdef GS_DEVICE_BASE_H
/* (gs_device_open's devices: not made here, and not every linker drops
 * the unreferenced open) */
extern "C" gs_device* gs_device_new_sw(void) { return NULL; }
extern "C" gs_device* gs_device_new_opengl(void) { return NULL; }
extern "C" gs_device* gs_device_new_vulkan(void) { return NULL; }
extern "C" gs_device* gs_device_new_d3d11(void) { return NULL; }
extern "C" gs_device* gs_device_new_d3d12(void) { return NULL; }
#endif

static u64 s_hash = 1469598103934665603ULL;
static void mix(const void* p, size_t n)
{
	const u8* b = static_cast<const u8*>(p);
	for (size_t i = 0; i < n; i++)
	{
		s_hash ^= b[i];
		s_hash *= 1099511628211ULL;
	}
}
static void mixu(u64 v) { mix(&v, sizeof(v)); }
static void mixf(float f) { mix(&f, sizeof(f)); }
static void mixv(const GSVector4& v) { mixf(v.x); mixf(v.y); mixf(v.z); mixf(v.w); }

/* Textures by when they were made: a pointer means nothing from run to
 * run, the order they were made in does */
static int s_made, s_freed;
struct FakeTex
{
	GSTexture tex;
	int id;
};
static void fake_free(gs_texture* t)
{
	mixu(0xF2EE0000u | (u32)reinterpret_cast<FakeTex*>(t)->id);
	s_freed++;
	free(t);
}
static const gs_texture_ops s_fake_tex_ops = {fake_free, NULL, NULL, NULL, NULL};
static int tid(const GSTexture* t) { return t ? reinterpret_cast<const FakeTex*>(t)->id : -1; }
/* A target drawn into: what it held first (cleared, and to what, or
 * invalidated, or kept) is what a real device loads it with, and after,
 * it holds what was drawn */
static void drawn(GSTexture* t)
{
	if (!t)
		return;
	mix(&t->m_state, sizeof(t->m_state));
	if (t->GetState() == GSTexture::State::Cleared)
		mix(&t->m_clear_value, sizeof(t->m_clear_value));
	t->SetState(GSTexture::State::Dirty);
}

static int s_fail_next; /* the next CreateSurface fails */
static GSVector4 s_last_src; /* the last stretch's source rect */

class FakeDev final : public GSDevice
{
public:
	FakeDev();
	bool Create() { return CreateBase(); }
	void Destroy() { DestroyBase(); }
	void ResetAPIState() {}
	void RestoreAPIState() {}
	RenderAPI GetRenderAPI() const { return RenderAPI::Vulkan; }
	PresentResult BeginPresent(bool) { return PresentResult::OK; }
	void EndPresent() {}
	GSTexture* CreateSurface(GSTexture::Type type, int w, int h, int levels, GSTexture::Format format)
	{
		mixu(0xC0000000ull | ((u64)(u32)w << 32) | (u32)h);
		mixu(((u64)static_cast<int>(type) << 16) | ((u64)static_cast<int>(format) << 8) | (u32)levels);
		if (s_fail_next)
		{
			s_fail_next--;
			return NULL;
		}
		FakeTex* t = static_cast<FakeTex*>(calloc(1, sizeof(FakeTex)));
		gs_texture* g = &t->tex;
		g->m_ops = &s_fake_tex_ops;
		g->m_size.x = w;
		g->m_size.y = h;
		g->m_mipmap_levels = levels;
		g->m_type = type;
		g->m_format = format;
		t->id = s_made++;
		return &t->tex;
	}
	GSDownloadTexture* CreateDownloadTexture(u32, u32, GSTexture::Format) { return NULL; }
	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
	{
		mixu(0xD0E7);
		drawn(dTex);
		for (int i = 0; i < 3; i++)
		{
			mixu((u64)(s64)tid(sTex[i]));
			mixv(sRect[i]);
			mixv(dRect[i]);
		}
		mixu((u64)(s64)tid(dTex));
		mixu(PMODE.U64);
		mixu(EXTBUF.U64);
		mixu(c);
		mixu(linear);
	}
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
	{
		mixu(0x1A7E);
		drawn(dTex);
		mixu((u64)(s64)tid(sTex));
		mixu((u64)(s64)tid(dTex));
		mixv(sRect);
		mixv(dRect);
		mixu((u64)static_cast<int>(shader));
		mixu(linear);
		mixv(cb.ZrH);
	}
	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 x, u32 y)
	{
		mixu(0xC097);
		drawn(dTex);
		mixu((u64)(s64)tid(sTex));
		mixu((u64)(s64)tid(dTex));
		mix(&r, sizeof(r));
		mixu(((u64)x << 32) | y);
	}
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader = ShaderConvert::COPY, bool linear = true)
	{
		mixu(0x57E7);
		drawn(dTex);
		s_last_src = sRect;
		mixu((u64)(s64)tid(sTex));
		mixu((u64)(s64)tid(dTex));
		mixv(sRect);
		mixv(dRect);
		mixu((u64)static_cast<int>(shader));
		mixu(linear);
	}
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool r, bool g, bool b, bool a, ShaderConvert shader = ShaderConvert::COPY)
	{
		mixu(0x57E8);
		drawn(dTex);
		mixu((u64)(s64)tid(sTex));
		mixu((u64)(s64)tid(dTex));
		mixv(sRect);
		mixv(dRect);
		mixu((u64)(r | g << 1 | b << 2 | a << 3));
		mixu((u64)static_cast<int>(shader));
	}
	void PresentRect(GSTexture*, const GSVector4&, GSTexture*, const GSVector4&) {}
	void DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
	{
		DrawMultiStretchRectsBase(rects, num_rects, dTex, shader);
	}
	void UpdateCLUTTexture(GSTexture*, float, float, u32, u32, GSTexture*, u32, u32) {}
	void ConvertToIndexedTexture(GSTexture*, float, u32, u32, u32, u32, GSTexture*, u32, u32) {}
	void FilteredDownsampleTexture(GSTexture*, GSTexture*, u32, const GSVector2i&, const GSVector4&) {}
	void MoveTexels(GSTexture*, GSTexture*, const GSVector4&, const MoveTexelsConstants&) {}
	void RenderHW(GSHWDrawConfig&) {}
	void ClearSamplerCache() {}

	void VRRange(GSTexture* s, GSTexture* d, const GSVector4& r, const VRRangeConstants& cb)
	{
		mixu(0x7A46);
		drawn(d);
		mixu((u64)(s64)tid(s));
		mixu((u64)(s64)tid(d));
		mixv(r);
		mix(&cb, sizeof(cb));
	}
	void VREye(GSTexture* s, GSTexture* d, const GSVector4& r, const VREyeConstants& cb)
	{
		mixu(0x7E7E);
		drawn(d);
		mixu((u64)(s64)tid(s));
		mixu((u64)(s64)tid(d));
		mixv(r);
		mix(&cb, sizeof(cb));
	}

	/* What the tests look at */
	int cur() const { return tid(GetCurrent()); }
	void state()
	{
		mixu((u64)(s64)cur());
		mixu((u64)(s64)tid(static_cast<GSTexture*>(m_merge)));
		mixu((u64)(s64)tid(static_cast<GSTexture*>(m_weavebob)));
		mixu((u64)(s64)tid(static_cast<GSTexture*>(m_blend)));
		mixu((u64)(s64)tid(static_cast<GSTexture*>(m_mad)));
		mixu((u64)(s64)tid(static_cast<GSTexture*>(m_weave_field[0])));
		mixu((u64)(s64)tid(static_cast<GSTexture*>(m_weave_field[1])));
		mixu((u64)(u32)s_freed);
		/* what was asked of the targets that are not drawn here: cleared
		 * (and to what), invalidated or kept */
		{
			gs_texture* const t[3] = {m_vr_atlas, m_weavebob, m_merge};
			for (int i = 0; i < 3; i++)
				mixu(t[i] ? ((u64)t[i]->m_state << 32 | t[i]->m_clear_value.color) : 0xFFFFFFFFFFull);
		}
	}
	void vr_state() { mixu(m_vr_out ? ((u64)m_vr_out->m_state << 32 | m_vr_out->m_clear_value.color) : 1); }
	void set_vr(bool on);
};

GS_DEVICE_OPS_DEFINE(FakeDev, fake);

/* The VR passes' table: its argument types are the device's C ones where
 * the device is C (GSDeviceBase.h) */
#ifdef GS_DEVICE_BASE_H
static void fake_vr_range(gs_device* d, gs_texture* s, gs_texture* t, const gs_rectf* r, const gs_vr_range_constants* cb)
{
	GS_DEV_THIS(FakeDev, d)->VRRange(static_cast<GSTexture*>(s), static_cast<GSTexture*>(t), GS_V4(r), *reinterpret_cast<const VRRangeConstants*>(cb));
}
static void fake_vr_eye(gs_device* d, gs_texture* s, gs_texture* t, const gs_rectf* r, const gs_vr_eye_constants* cb)
{
	GS_DEV_THIS(FakeDev, d)->VREye(static_cast<GSTexture*>(s), static_cast<GSTexture*>(t), GS_V4(r), *reinterpret_cast<const VREyeConstants*>(cb));
}
#else
static void fake_vr_range(GSDevice* d, GSTexture* s, GSTexture* t, const GSVector4* r, const VRRangeConstants* cb)
{
	static_cast<FakeDev*>(d)->VRRange(s, t, *r, *cb);
}
static void fake_vr_eye(GSDevice* d, GSTexture* s, GSTexture* t, const GSVector4* r, const VREyeConstants* cb)
{
	static_cast<FakeDev*>(d)->VREye(s, t, *r, *cb);
}
#endif
static const struct gs_vr_ops s_fake_vr_ops = {fake_vr_range, fake_vr_eye};

FakeDev::FakeDev()
{
	m_ops = &s_fake_device_ops;
}
void FakeDev::set_vr(bool on) { m_vr_ops = on ? &s_fake_vr_ops : NULL; }

static u32 s_rng = 0x9E3779B9u;
static u32 rnd(u32 n)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng % n;
}

static void frame(FakeDev& dev, int w, int h, int field, int mode, bool half)
{
	GSTexture* src[3] = {NULL, NULL, NULL};
	GSVector4 sr[3], dr[3];
	GSRegPMODE pmode;
	GSRegEXTBUF ext;
	pmode.U64 = 0x1234 + (u64)rnd(64);
	ext.U64 = 0;
	for (int i = 0; i < 3; i++)
	{
		sr[i] = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
		dr[i] = GSVector4(0.0f, 0.0f, (float)w, (float)h);
	}
	src[0] = dev.CreateRenderTarget(w, h, GSTexture::Format::Color);
	dev.Merge(src, sr, dr, GSVector2i(w, h), pmode, ext, 0x80808080u);
	dev.state();
	/* (in order: the order a call's arguments are evaluated in is the
	 * compiler's) */
	const float yoffset = 0.5f * (float)(1 + rnd(3));
	const float block = 1.0f + (float)rnd(2);
	dev.Interlace(GSVector2i(w, h), field, mode, yoffset, block, half);
	dev.state();
	dev.Recycle(src[0]);
}

int main(int argc, char** argv)
{
	FakeDev* dev = new FakeDev();
	int mode, field, i;
	int bad_resize = 0;

	GSConfig.UpscaleMultiplier = 1.0f;
	dev->Create();
	dev->PrewarmPool();

	for (mode = 0; mode < 5; mode++)
	{
		for (i = 0; i < 12; i++)
		{
			field = i & 1;
			GSConfig.DisableInterlaceOffset = (i & 4) != 0;
			GSConfig.PCRTCOffsets = (i & 2) != 0;
			/* a resize now and then: each target's size follows */
			frame(*dev, i < 6 ? 640 : 512, i < 9 ? 448 : 224, field, mode, (i % 3) == 1);
			if (i % 4 == 3)
				dev->AgePool();
		}
		/* the frontend handing one back for another */
		if (dev->GetCurrent())
		{
			GSTexture* other = dev->CreateRenderTarget(64, 64, GSTexture::Format::Color);
			GSTexture* handed = dev->GetCurrent();
			mixu(dev->SwapPresentTexture(handed, other));
			dev->state();
			mixu(dev->SwapPresentTexture(other, other));
			/* (the frontend's now, which lets it go) */
			gs_texture_free(handed);
		}
		dev->ClearCurrent();
		dev->state();
	}

	/* a merge whose target cannot be made, then the interlace with none */
	s_fail_next = 2;
	frame(*dev, 704, 480, 0, 0, false);
	frame(*dev, 704, 480, 1, 3, true);

	/* resize: preserved, recycled, retired, freed */
	{
		GSTexture* t = NULL;
		mixu(dev->ResizeRenderTarget(&t, 100, 50, true, false));
		mixu(dev->ResizeRenderTarget(&t, 100, 50, false, false));
		mixu((u64)static_cast<int>(t->GetState()));
		mixu(dev->ResizeRenderTarget(&t, 120, 60, true, true));
		/* all of the old target into the new: the C++ device's source
		 * rect was GSVector4::cxpr(0, 0, 1, 1), the int overload, whose
		 * 1s are the bits of the smallest denormal - one texel smeared
		 * over the target */
		if (!(s_last_src == GSVector4(0.0f, 0.0f, 1.0f, 1.0f)).alltrue())
			bad_resize = 1;
		mixu(dev->ResizeRenderTarget(&t, 140, 70, true, false, true));
		mixu(dev->ResizeRenderTarget(&t, 160, 80, false, false));
		s_fail_next = 2;
		mixu(dev->ResizeRenderTarget(&t, 180, 90, true, true));
		mixu((u64)(s64)tid(t));
		gs_texture_free(t);
	}

	/* VR: none without the passes, a blank, with and without depth, a
	 * snapshot of a depth buffer */
	{
		GSVRFrame f;
		GSTexture* pic = dev->CreateRenderTarget(320, 224, GSTexture::Format::Color);
		GSTexture* depth = dev->CreateDepthStencil(320, 224, GSTexture::Format::DepthStencil);
		memset(&f, 0, sizeof(f));
		f.eye_size = GSVector2i(400, 300);
		f.picture_rect = GSVector4(0.0f, 0.0f, 320.0f, 224.0f);
		f.depth_rect = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
		f.z_scale = 1.0f / 16777216.0f;
		for (i = 0; i < 2; i++)
			f.eyes[i].eye_pos = GSVector4((float)i, 0.0f, 0.0f, 1.0f);
		mixu((u64)(s64)tid(dev->VRCompose(pic, f)));
		dev->set_vr(true);
		mixu((u64)(s64)tid(dev->VRBlank(GSVector2i(400, 300))));
		dev->vr_state();
		/* a frame now and then without depth, then a run longer than
		 * the hold; a picture taller than wide too, whose reduction is
		 * taller than wide */
		for (i = 0; i < 40; i++)
		{
			if (i == 26)
			{
				dev->Recycle(pic);
				dev->Recycle(depth);
				pic = dev->CreateRenderTarget(224, 640, GSTexture::Format::Color);
				depth = dev->CreateDepthStencil(224, 640, GSTexture::Format::DepthStencil);
				f.picture_rect = GSVector4(0.0f, 0.0f, 224.0f, 640.0f);
			}
			f.depth = (i % 5 == 4 || (i >= 14 && i < 25)) ? NULL : depth;
			f.depth_encoded = (i & 1) != 0;
			mixu((u64)(s64)tid(dev->VRCompose(pic, f)));
			dev->state();
		}
		depth->SetState(GSTexture::State::Dirty);
		mixu(dev->VRSnapshotDepth(depth));
		depth->SetState(GSTexture::State::Cleared);
		mixu(dev->VRSnapshotDepth(depth));
		dev->VRRelease();
		dev->Recycle(pic);
		dev->Recycle(depth);
	}

	for (i = 0; i < 10; i++)
		dev->AgePool();
	dev->state();
	dev->Destroy();
	dev->state();
	mixu((u64)(u32)s_made);
	dev->Free();

	char got[32];
	snprintf(got, sizeof(got), "%016llx", (unsigned long long)s_hash);
	if (argc > 1 && strcmp(argv[1], got) != 0)
		printf("gsdevchain: %s, the C++ device gave %s\n", got, argv[1]);
	else
		printf("gsdevchain: %s (%d textures made, %d freed)\n", got, s_made, s_freed);
	if (bad_resize)
		printf("gsdevchain: a resize that keeps the contents copied a texel of them, not all\n");
	return (argc > 1 && strcmp(argv[1], got) != 0) || bad_resize;
}
