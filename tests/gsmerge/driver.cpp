/* SPDX-License-Identifier: LGPL-3.0+ */
/* The vsync of Renderers/Common/GSPresent.c held to the C++ it replaced
 * (reference.cpp): the same random registers, options, renderer answers
 * and frontend on both, each on a device of its own that records what it
 * is asked, frame by frame. A frame's record is a hash of everything
 * either asked of its device and of the frontend, and of the state left;
 * the two runs' hashes are compared frame by frame. */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "reference.h"
#include "GS/Renderers/Common/GSPresent.h"
#include "GS/GSUtil.h"

EmuGSOptions GSConfig;
extern "C" {
int m_disp_fb_sprite_blits;
int gs_vr_mode;
float gs_vr_depth;
float gs_vr_aspect;
}

/* --- the record --------------------------------------------------------- */

static u64 s_hash;
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
static void mixr(const gs_rectf* r) { mixf(r->x); mixf(r->y); mixf(r->z); mixf(r->w); }

/* the same sequence for both runs, restarted for each */
static u32 s_rng;
static u32 rn(u32 n)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng % n;
}

/* Textures by when they were made, which is the same in both runs */
static int s_made;
struct FakeTex
{
	GSTexture tex;
	int id;
};
static void fake_free(gs_texture* t)
{
	mixu(0xF2EE0000u | (u32)reinterpret_cast<FakeTex*>(t)->id);
	free(t);
}
static const gs_texture_ops s_fake_tex_ops = {fake_free, NULL, NULL, NULL, NULL};
static int tid(const gs_texture* t) { return t ? reinterpret_cast<const FakeTex*>(t)->id : -1; }
static gs_texture* make_tex(int w, int h, GSTexture::Type type, GSTexture::Format format, int levels)
{
	FakeTex* t = static_cast<FakeTex*>(calloc(1, sizeof(FakeTex)));
	gs_texture* g = &t->tex;
	g->m_ops = &s_fake_tex_ops;
	g->m_size.x = w;
	g->m_size.y = h;
	g->m_mipmap_levels = levels;
	g->m_type = type;
	g->m_format = format;
	t->id = s_made++;
	return g;
}

/* --- the device --------------------------------------------------------- */

class FakeDev final : public GSDevice
{
public:
	FakeDev();
	RenderAPI api = RenderAPI::Vulkan;
	PresentResult present = PresentResult::OK;

	bool Create() { return CreateBase(); }
	void Destroy() { DestroyBase(); }
	void ResetAPIState() {}
	void RestoreAPIState() {}
	RenderAPI GetRenderAPI() const { return api; }
	PresentResult BeginPresent(bool skip)
	{
		mixu(0xBE60 | (u64)skip << 20 | (u64)static_cast<int>(present) << 24);
		return present;
	}
	void EndPresent() { mixu(0xE4D0); }
	GSTexture* CreateSurface(GSTexture::Type type, int w, int h, int levels, GSTexture::Format format)
	{
		mixu(0xC0000000ull | ((u64)(u32)w << 32) | (u32)h);
		return static_cast<GSTexture*>(make_tex(w, h, type, format, levels));
	}
	GSDownloadTexture* CreateDownloadTexture(u32, u32, GSTexture::Format) { return NULL; }
	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
	{
		mixu(0xD0E7);
		for (int i = 0; i < 3; i++)
		{
			mixu((u64)(s64)tid(sTex[i]));
			/* (two source rects: the feedback write has none) */
			if (i < 2)
				mixr(reinterpret_cast<const gs_rectf*>(&sRect[i]));
			mixr(reinterpret_cast<const gs_rectf*>(&dRect[i]));
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
		mixu((u64)(s64)tid(sTex));
		mixu((u64)(s64)tid(dTex));
		mixr(reinterpret_cast<const gs_rectf*>(&sRect));
		mixr(reinterpret_cast<const gs_rectf*>(&dRect));
		mixu((u64)static_cast<int>(shader));
		mixu(linear);
		mix(&cb, sizeof(cb));
	}
	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 x, u32 y)
	{
		mixu(0xC097);
		mixu((u64)(s64)tid(sTex));
		mixu((u64)(s64)tid(dTex));
		mix(&r, sizeof(r));
		mixu(((u64)x << 32) | y);
	}
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader = ShaderConvert::COPY, bool linear = true)
	{
		mixu(0x57E7);
		mixu((u64)(s64)tid(sTex));
		mixu((u64)(s64)tid(dTex));
		mixr(reinterpret_cast<const gs_rectf*>(&sRect));
		mixr(reinterpret_cast<const gs_rectf*>(&dRect));
		mixu((u64)static_cast<int>(shader));
		mixu(linear);
	}
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool r, bool g, bool b, bool a, ShaderConvert shader = ShaderConvert::COPY)
	{
		mixu(0x57E8);
		mixu((u64)(s64)tid(sTex));
		mixu((u64)(s64)tid(dTex));
		mixr(reinterpret_cast<const gs_rectf*>(&sRect));
		mixr(reinterpret_cast<const gs_rectf*>(&dRect));
		mixu((u64)(r | g << 1 | b << 2 | a << 3));
		mixu((u64)static_cast<int>(shader));
	}
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
	{
		mixu(0x9E5E);
		mixu((u64)(s64)tid(sTex));
		mixu((u64)(s64)tid(dTex));
		mixr(reinterpret_cast<const gs_rectf*>(&sRect));
		mixr(reinterpret_cast<const gs_rectf*>(&dRect));
	}
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
		mixu((u64)(s64)tid(s));
		mixu((u64)(s64)tid(d));
		mixr(reinterpret_cast<const gs_rectf*>(&r));
		mix(&cb, sizeof(cb));
	}
	void VREye(GSTexture* s, GSTexture* d, const GSVector4& r, const VREyeConstants& cb)
	{
		mixu(0x7E7E);
		mixu((u64)(s64)tid(s));
		mixu((u64)(s64)tid(d));
		mixr(reinterpret_cast<const gs_rectf*>(&r));
		mix(&cb, sizeof(cb));
	}
	void set_vr(bool on);
};

GS_DEVICE_OPS_DEFINE(FakeDev, fake);

static void fake_vr_range(gs_device* d, gs_texture* s, gs_texture* t, const gs_rectf* r, const gs_vr_range_constants* cb)
{
	GS_DEV_THIS(FakeDev, d)->VRRange(static_cast<GSTexture*>(s), static_cast<GSTexture*>(t), GS_V4(r), *reinterpret_cast<const VRRangeConstants*>(cb));
}
static void fake_vr_eye(gs_device* d, gs_texture* s, gs_texture* t, const gs_rectf* r, const gs_vr_eye_constants* cb)
{
	GS_DEV_THIS(FakeDev, d)->VREye(static_cast<GSTexture*>(s), static_cast<GSTexture*>(t), GS_V4(r), *reinterpret_cast<const VREyeConstants*>(cb));
}
static const struct gs_vr_ops s_fake_vr_ops = {fake_vr_range, fake_vr_eye};

FakeDev::FakeDev() { m_ops = &s_fake_device_ops; }
void FakeDev::set_vr(bool on) { m_vr_ops = on ? &s_fake_vr_ops : NULL; }

/* --- the core and the frontend ------------------------------------------ */

static void quiet(enum retro_log_level, const char* fmt, ...) { mixu(0x106); mix(fmt, strlen(fmt)); }
extern "C" {
retro_log_printf_t log_cb = quiet;
extern retro_environment_t environ_cb;
extern retro_video_refresh_t video_cb;
}
static unsigned s_views_status;
static bool s_views_accept;
static bool env(unsigned cmd, void* data)
{
	mixu(0xE40 | (u64)cmd << 16);
	if (cmd == RETRO_ENVIRONMENT_GET_VIDEO_VIEWS_STATUS)
	{
		/* (refused, with the status written over or not) */
		if (rn(8) == 0)
		{
			if (rn(2))
				*static_cast<unsigned*>(data) = s_views_status;
			return false;
		}
		*static_cast<unsigned*>(data) = s_views_status;
		return true;
	}
	if (cmd == RETRO_ENVIRONMENT_SET_VIDEO_VIEWS)
	{
		const struct retro_video_views* m = static_cast<const struct retro_video_views*>(data);
		mixu(m->num_views);
		mixu(m->flags | (u64)m->reference_space << 32);
		for (unsigned i = 0; i < m->num_views; i++)
			mix(&m->views[i], sizeof(m->views[i]));
		return s_views_accept;
	}
	return false;
}
static void video(const void* data, unsigned w, unsigned h, size_t pitch)
{
	mixu(0x51DE0 ^ (u64)(uintptr_t)data ^ w ^ (u64)h << 20 ^ (u64)pitch << 40);
}
retro_environment_t environ_cb = env;
retro_video_refresh_t video_cb = video;

extern "C" bool GSIsHardwareRenderer(void) { return true; }
extern "C" WindowInfo host_acquire_render_window(void)
{
	WindowInfo w;
	memset(&w, 0, sizeof(w));
	w.surface_width  = 1280;
	w.surface_height = 720;
	return w;
}
extern "C" const char* GSTextureCacheMemoryUsage(void) { return "-"; }
extern "C" gs_device* gs_device_new_sw(void) { return NULL; }
extern "C" gs_device* gs_device_new_opengl(void) { return NULL; }
extern "C" gs_device* gs_device_new_vulkan(void) { return NULL; }
extern "C" gs_device* gs_device_new_d3d11(void) { return NULL; }
extern "C" gs_device* gs_device_new_d3d12(void) { return NULL; }

static enum perf_fps_method s_fps_method;
extern "C" enum perf_fps_method perf_metrics_internal_fps_method(void) { return s_fps_method; }
extern "C" void perf_metrics_update(bool gs_register_write, bool fb_blit) { mixu(0x9EF0 | gs_register_write << 1 | fb_blit); }
extern "C" bool GSreopen(bool recreate_device, bool recreate_renderer, const EmuGSOptions*)
{
	mixu(0x2E09 | recreate_device << 1 | recreate_renderer);
	return rn(2) != 0;
}

/* The renderer's answers: one of a few textures of its own, or none */
static gs_texture* s_outputs[4];
static gs_texture* s_depth;
static const float s_scales[4] = {1.0f, 2.0f, 1.5f, 3.0f};
gs_texture* mock_get_output(int i, float* scale, int* y_offset)
{
	mixu(0x0A7 ^ (u64)(s64)i);
	if (rn(12) == 0)
		return NULL;
	*scale    = s_scales[rn(4)];
	*y_offset = (int)rn(3);
	return s_outputs[rn(4)];
}
gs_texture* mock_get_feedback_output(float* scale)
{
	mixu(0xFEED);
	if (rn(4) == 0)
		return NULL;
	*scale = s_scales[rn(4)];
	return s_outputs[rn(4)];
}
bool mock_get_vr_depth(gs_texture* color, gs_texture** depth, bool* encoded, float* z_scale)
{
	mixu(0xDE97 ^ (u64)(s64)tid(color));
	if (rn(3) == 0)
		return false;
	*depth   = s_depth;
	*encoded = rn(2) != 0;
	*z_scale = 1.0f / (float)(1u << (16 + rn(9)));
	return true;
}
/* (an answer, not a call to count: the C++ asked twice) */
static float s_upscale;
float mock_upscale(void) { return s_upscale; }

static bool ref_vr_depth(RefPresent*, GSTexture* color, GSTexture** depth, bool* encoded, float* z_scale)
{
	gs_texture* d = NULL;
	const bool r = mock_get_vr_depth(color, &d, encoded, z_scale);
	if (r)
		*depth = static_cast<GSTexture*>(d);
	return r;
}
static const RefPresent::Ops s_ref_ops = {ref_vr_depth};

static gs_texture* c_get_output(void*, int i, float* scale, int* y) { return mock_get_output(i, scale, y); }
static gs_texture* c_get_feedback(void*, float* scale) { return mock_get_feedback_output(scale); }
static bool c_vr_depth(void*, gs_texture* c, gs_texture** d, bool* e, float* z) { return mock_get_vr_depth(c, d, e, z); }
static float c_upscale(void*) { return mock_upscale(); }
static const gs_present_ops s_c_ops = {c_get_output, c_get_feedback, c_vr_depth, c_upscale};

/* --- a run ---------------------------------------------------------------- */

enum { FRAMES = 6000, SEED = 0x2545F491 };

static void random_regs(GSPrivRegSet* regs)
{
	static const u32 psm[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S};
	int i;
	regs->PMODE.U64   = 0;
	regs->PMODE.EN1   = rn(4) != 0;
	regs->PMODE.EN2   = rn(3) == 0;
	regs->PMODE.MMOD  = rn(2);
	regs->PMODE.AMOD  = rn(2);
	regs->PMODE.SLBG  = rn(2);
	regs->PMODE.ALP   = rn(3) == 0 ? 0 : (rn(2) ? 255 : rn(256));
	regs->SMODE1.U64  = 0;
	regs->SMODE1.CMOD = rn(4) ? 2 + rn(2) : 0;
	regs->SMODE1.LC   = rn(2) ? 22 : 32;
	regs->SMODE2.U64  = 0;
	regs->SMODE2.INT  = rn(2);
	regs->SMODE2.FFMD = rn(2);
	regs->SYNCV.U64   = 0;
	regs->SYNCV.VFP   = rn(4);
	regs->EXTBUF.U64  = 0;
	regs->EXTBUF.FBIN = rn(4);
	regs->EXTBUF.WFFMD = rn(2);
	regs->EXTBUF.WDX  = rn(64);
	regs->EXTBUF.WDY  = rn(64);
	regs->EXTDATA.U64 = 0;
	regs->EXTDATA.SMPH = rn(16);
	regs->EXTDATA.SMPV = rn(4);
	regs->EXTDATA.WW  = rn(2048);
	regs->EXTDATA.WH  = rn(2048);
	regs->EXTWRITE.U64 = 0;
	regs->EXTWRITE.WRITE = rn(6) == 0;
	regs->BGCOLOR.U64 = (u64)rn(1u << 24) | (u64)rn(256) << 24;
	for (i = 0; i < 2; i++)
	{
		GSRegDISPLAY* dp = &regs->DISP[i].DISPLAY;
		GSRegDISPFB* fb  = &regs->DISP[i].DISPFB;
		dp->U64  = 0;
		fb->U64  = 0;
		dp->DX   = 600 + rn(80);
		dp->DY   = 20 + rn(30);
		dp->MAGH = rn(4) ? 3 : rn(16);
		dp->MAGV = rn(4) ? 0 : rn(4);
		dp->DW   = rn(4) ? 2559 : rn(4096);
		dp->DH   = 223 + rn(300);
		fb->FBW  = 10;
		fb->PSM  = psm[rn(4)];
		fb->DBX  = rn(4) ? 0 : rn(4);
		fb->DBY  = rn(3) ? 0 : rn(3);
	}
	if (rn(2))
	{
		regs->DISP[1] = regs->DISP[0];
		regs->DISP[1].DISPLAY.DY += rn(3);
		regs->DISP[1].DISPFB.DBY += rn(3);
	}
}

/* The options, the frontend and the device's answers for a frame */
static void random_world(FakeDev* dev)
{
	GSConfig.PCRTCAntiBlur       = rn(2);
	GSConfig.PCRTCOffsets        = rn(2);
	GSConfig.PCRTCOverscan       = rn(2);
	GSConfig.InterlaceMode       = static_cast<GSInterlaceMode>(rn(8));
	GSConfig.SkipDuplicateFrames = rn(2);
	GSConfig.UpscaleMultiplier   = 1.0f;
	s_upscale = s_scales[rn(4)];
	s_fps_method   = static_cast<enum perf_fps_method>(rn(3));
	gs_vr_mode     = rn(3) ? GSVR_STEREOSCOPIC : GSVR_OFF;
	gs_vr_depth    = 0.5f * (float)rn(5);
	gs_vr_aspect   = rn(2) ? 0.0f : 4.0f / 3.0f;
	s_views_status = rn(4) ? (RETRO_VIDEO_VIEWS_STATUS_PRESENTS | RETRO_VIDEO_VIEWS_STATUS_STEREO) : rn(4);
	s_views_accept = rn(8) != 0;
	if (rn(3) == 0)
		m_disp_fb_sprite_blits = (int)rn(3);
	dev->api     = rn(3) ? RenderAPI::Vulkan : RenderAPI::OpenGL;
	dev->present = rn(10) ? GSDevice::PresentResult::OK
	             : (rn(2) ? GSDevice::PresentResult::FrameSkipped : GSDevice::PresentResult::DeviceLost);
	dev->m_window_info.surface_width  = 640 + rn(4) * 640;
	dev->m_window_info.surface_height = 448 + rn(3) * 300;
	dev->set_vr(rn(5) != 0);
}

static void record_pcrtc(const gs_pcrtc* p)
{
	mix(&p->videomode, sizeof(int) * 3);
	mixu(p->PCRTCSameSrc | p->toggling_field << 1);
	for (int i = 0; i < 2; i++)
	{
		const gs_pcrtc_display* d = &p->PCRTCDisplays[i];
		mixu(d->enabled);
		mix(&d->displayRect, sizeof(d->displayRect));
		mix(&d->framebufferRect, sizeof(d->framebufferRect));
		mix(&d->displayOffset, sizeof(d->displayOffset));
		mix(&d->framebufferOffsets, sizeof(d->framebufferOffsets));
	}
}

/* Both runs: the same seeds, a device each; the hash of each frame */
static void run(bool c, u64* hashes)
{
	static GSPrivRegSet regs;
	FakeDev* dev = new FakeDev();
	RefPresent* ref = NULL;
	gs_present* pr = NULL;
	gs_pcrtc* pcrtc;
	u8 scanmask = 0;
	int draws = 0, transfers = 0;
	int f, i;

	s_rng  = SEED;
	s_made = 0;
	s_hash = 1469598103934665603ULL;
	memset(&regs, 0, sizeof(regs));
	memset(&GSConfig, 0, sizeof(GSConfig));
	m_disp_fb_sprite_blits = 0;
	GSConfig.UpscaleMultiplier = 1.0f;
	g_gs_device_c = dev;
	dev->Create();

	for (i = 0; i < 4; i++)
		s_outputs[i] = make_tex(320 << (i & 1), 224 << (i >> 1), GSTexture::Type::RenderTarget, GSTexture::Format::Color, 1);
	s_depth = make_tex(640, 448, GSTexture::Type::DepthStencil, GSTexture::Format::DepthStencil, 1);

	if (c)
	{
		pr = static_cast<gs_present*>(calloc(1, sizeof(gs_present)));
		pcrtc = static_cast<gs_pcrtc*>(calloc(1, sizeof(gs_pcrtc)));
		gs_present_init(pr, NULL, &s_c_ops, pcrtc, &scanmask, &draws, &transfers);
		pr->regs = &regs;
	}
	else
	{
		ref = static_cast<RefPresent*>(calloc(1, sizeof(RefPresent)));
		ref->m_regs = &regs;
		ref->m_ops = &s_ref_ops;
		pcrtc = &ref->PCRTCDisplays;
	}

	for (f = 0; f < FRAMES; f++)
	{
		u32 field;
		bool written, idle;

		random_regs(&regs);
		random_world(dev);
		if (rn(4) == 0)
			scanmask = (u8)rn(3);
		if (rn(3))
			draws += (int)rn(3);
		if (rn(3) == 0)
			transfers++;
		field   = rn(2);
		written = rn(2) != 0;

		if (c)
		{
			idle = gs_present_idle_frame(pr);
			mixu(idle);
			gs_present_vsync(pr, field, written, idle);
			mix(&pr->real_size, sizeof(pr->real_size));
			mixu(pr->vr_active | pr->vr_map_set << 1);
			mix(&pr->vr_eye_size, sizeof(pr->vr_eye_size));
			mixu((u64)(s64)tid(pr->vr_merge_tex));
			if (pr->vr_merge_tex)
			{
				mixr(&pr->vr_merge_src);
				mixr(&pr->vr_merge_dst);
			}
			mixu(pr->skipped_duplicate_frames);
			mixu((u64)pr->last_draw_n << 32 | (u32)pr->last_transfer_n);
		}
		else
		{
			ref->s_n          = draws;
			ref->s_transfer_n = transfers;
			ref->m_scanmask_used = scanmask;
			idle = ref->IsIdleFrame();
			mixu(idle);
			ref->VSyncBase(field, written, idle);
			draws    = ref->s_n;
			scanmask = ref->m_scanmask_used;
			mix(&ref->m_real_size, sizeof(ref->m_real_size));
			mixu(ref->m_vr_active | ref->m_vr_map_set << 1);
			mix(&ref->m_vr_eye_size, sizeof(ref->m_vr_eye_size));
			mixu((u64)(s64)tid(ref->m_vr_merge_tex));
			if (ref->m_vr_merge_tex)
			{
				mixr(reinterpret_cast<const gs_rectf*>(&ref->m_vr_merge_src));
				mixr(reinterpret_cast<const gs_rectf*>(&ref->m_vr_merge_dst));
			}
			mixu(ref->m_skipped_duplicate_frames);
			mixu((u64)ref->m_last_draw_n << 32 | (u32)ref->m_last_transfer_n);
		}
		mixu(scanmask);
		mixu((u64)(u32)m_disp_fb_sprite_blits);
		mixu((u64)(u32)draws);
		mixu((u64)(s64)tid(dev->m_current));
		record_pcrtc(pcrtc);
		if (rn(20) == 0)
			dev->ClearCurrent();
		hashes[f] = s_hash;
	}

	dev->Destroy();
	dev->Free();
	for (i = 0; i < 4; i++)
		s_outputs[i]->m_ops->free(s_outputs[i]);
	s_depth->m_ops->free(s_depth);
	free(pr);
	if (c)
		free(pcrtc);
	free(ref);
	g_gs_device_c = NULL;
}

int main(void)
{
	static u64 ref_hashes[FRAMES], c_hashes[FRAMES];
	int f;

	gs_util_init();
	run(false, ref_hashes);
	run(true, c_hashes);
	for (f = 0; f < FRAMES; f++)
	{
		if (ref_hashes[f] != c_hashes[f])
		{
			printf("gsmerge: frame %d differs from the C++ (%d frames)\n", f, FRAMES);
			return 1;
		}
	}
	printf("gsmerge: %d frames as the C++ did, %d textures\n", FRAMES, s_made);
	return 0;
}
