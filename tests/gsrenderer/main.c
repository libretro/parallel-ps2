/* What GS.c's calls into the GSdx renderers (GS/Renderers/Common/
 * GSRenderer.c) do: each renderer's own through its table, the state's
 * where the table has none, in the order the C++ glue made them. The
 * state, the present, the device and the renderers are pretend ones that
 * log what is asked of them. C89. */

#include <stdio.h>
#include <string.h>

#include "pcsx2/GS/Renderers/Common/GSRenderer.h"
#include "pcsx2/GS/GSBackend.h"
#include "pcsx2/FreezeTypes.h"
#include "pcsx2/GS/Renderers/Common/GSDeviceBase.h"
#include "pcsx2/GS/Renderers/SW/GSRendererSW.h"

static int s_failed;
#define CHECK(c, what) \
	do \
	{ \
		if (!(c)) \
		{ \
			printf("  FAIL: %s (line %d)\n", what, __LINE__); \
			s_failed = 1; \
		} \
	} while (0)

/* --- the log ------------------------------------------------------------- */

static char s_log[2048];

static void say(const char* what)
{
	if (s_log[0])
		strcat(s_log, " ");
	strcat(s_log, what);
}

static void sayf(const char* fmt, unsigned long a, unsigned long b, unsigned long c)
{
	char buf[128];
	sprintf(buf, fmt, a, b, c);
	say(buf);
}

#define LOG_IS(want, what) \
	do \
	{ \
		CHECK(strcmp(s_log, want) == 0, what); \
		if (strcmp(s_log, want) != 0) \
			printf("    got  \"%s\"\n    want \"%s\"\n", s_log, want); \
		s_log[0] = 0; \
	} while (0)

/* --- what GSRenderer.c calls ---------------------------------------------- */

EmuGSOptions GSConfig;
gs_device* g_gs_device_c = (gs_device*)(void*)&s_failed; /* (any pointer) */
static unsigned long s_cpu;
static struct gs_state s_hw, s_sw;
static u8 s_basemem[0x2000];
static int s_idle;

uint64_t cpu_features_get(void) { return s_cpu; }

struct gs_state* gs_renderer_hw_create(void) { say("hw_create"); return &s_hw; }
void gs_renderer_hw_release_device(void) { say("hw_release_device"); }
struct gs_state* gs_renderer_sw_create_isa_sse4(int t) { sayf("sw_sse4(%lu)", t, 0, 0); return &s_sw; }
struct gs_state* gs_renderer_sw_create_isa_avx(int t) { sayf("sw_avx(%lu)", t, 0, 0); return &s_sw; }
struct gs_state* gs_renderer_sw_create_isa_avx2(int t) { sayf("sw_avx2(%lu)", t, 0, 0); return &s_sw; }
struct gs_state* gs_renderer_sw_create_isa_native(int t) { sayf("sw_native(%lu)", t, 0, 0); return &s_sw; }

static const char* who(const struct gs_state* s) { return s == &s_hw ? "hw" : s == &s_sw ? "sw" : "?"; }

void gs_state_init_read_fifo(struct gs_state* s, u8* mem, int len) { sayf("init_read_fifo(%lu)", (unsigned long)len, 0, 0); (void)s; (void)mem; }
void gs_state_read_fifo(struct gs_state* s, u8* mem, int size) { sayf("read_fifo(%lu)", (unsigned long)size, 0, 0); (void)s; (void)mem; }
void gs_state_read_local_memory_unsync(struct gs_state* s, u8* mem, int qwc, GIFRegBITBLTBUF b, GIFRegTRXPOS p, GIFRegTRXREG r)
{
	(void)s; (void)mem;
	sayf("read_unsync(%lu %lx %lx", (unsigned long)qwc, (unsigned long)b.U32[0], (unsigned long)p.U32[1]);
	sayf("%lx)", (unsigned long)r.U32[0], 0, 0);
}
void gs_state_transfer(struct gs_state* s, const u8* mem, u32 size) { sayf("transfer(%lu)", size, 0, 0); (void)s; (void)mem; }
void gs_state_flush(struct gs_state* s, int reason) { sayf("flush(%lx)", (unsigned long)reason, 0, 0); (void)s; }
int gs_state_freeze(struct gs_state* s, freezeData* fd, bool sizeonly) { sayf("freeze(%lu)", sizeonly, 0, 0); (void)s; (void)fd; return 7; }
int gs_state_defrost(struct gs_state* s, const freezeData* fd) { say("defrost"); (void)s; (void)fd; return 9; }
void gs_state_reset_base(struct gs_state* s, bool hard) { sayf("reset_base(%lu)", hard, 0, 0); (void)s; }
void gs_state_update_settings_base(struct gs_state* s, const EmuGSOptions* o) { say("update_settings_base"); (void)s; (void)o; }
void gs_present_vsync(gs_present* p, u32 field, bool written, bool idle) { sayf("present_vsync(%lu %lu %lu)", field, written, idle); (void)p; }
bool gs_present_idle_frame(const gs_present* p) { (void)p; return s_idle != 0; }
void gs_present_purge_pool(void) { say("purge_pool"); }
void gs_device_clear_current(gs_device* dev) { say(dev == g_gs_device_c ? "clear_current" : "clear_current(?)"); }
void gs_clut_gpu_create(gs_clut* c) { say(c == &s_hw.m_mem.m_clut.m_c || c == &s_sw.m_mem.m_clut.m_c ? "clut_create" : "clut_create(?)"); }
void gs_clut_gpu_release(gs_clut* c) { say(c == &s_hw.m_mem.m_clut.m_c || c == &s_sw.m_mem.m_clut.m_c ? "clut_release" : "clut_release(?)"); }
void gs_pcrtc_reset(gs_pcrtc* p, const GSPrivRegSet* regs)
{
	say((p == &s_hw.PCRTCDisplays || p == &s_sw.PCRTCDisplays) && (const void*)regs == (const void*)s_basemem
		? "pcrtc_reset" : "pcrtc_reset(?)");
}

static GSPage2TileMap* s_p2t = (GSPage2TileMap*)(void*)&s_idle;
GSPage2TileMap* gs_page2tile_map_get(gs_p2t_cache* c, const GIFRegTEX0* TEX0)
{
	sayf("page2tile(%lx)", (unsigned long)TEX0->U32[0], 0, 0);
	return (c == &s_hw.m_mem.lm.m_p2tmap || c == &s_sw.m_mem.lm.m_p2tmap) ? s_p2t : NULL;
}

gs_lm_psm gs_lm_psm_table[64];
static void rtxb(const gs_local_memory* m, u32 bp, u8* dst, int pitch, const GIFRegTEXA* TEXA)
{
	(void)m; (void)dst; (void)pitch; (void)TEXA;
	sayf("rtxb(%lx)", bp, 0, 0);
}
static void rtxbP(const gs_local_memory* m, u32 bp, u8* dst, int pitch, const GIFRegTEXA* TEXA)
{
	(void)dst;
	sayf("rtxbP(%lx %lu %lx)", bp, (unsigned long)pitch, (unsigned long)TEXA->U32[0]);
	CHECK(m == &s_hw.m_mem.lm || m == &s_sw.m_mem.lm, "rtxbP reads the open renderer's memory");
}

/* --- a renderer's table --------------------------------------------------- */

#define SELF_OF(gs) ((struct gs_state*)(void*)(gs))

static void op_free(gs_state_self* gs) { say(who(SELF_OF(gs))); strcat(s_log, ".free"); }
static void op_destroy(gs_state_self* gs) { say(who(SELF_OF(gs))); strcat(s_log, ".destroy"); }
static void op_reset(gs_state_self* gs, bool hard) { (void)gs; sayf("op_reset(%lu)", hard, 0, 0); }
static void op_update_settings(gs_state_self* gs, const EmuGSOptions* o) { (void)gs; CHECK(o == &GSConfig, "the old settings passed on"); say("op_update_settings"); }
static void op_update_render_fixes(gs_state_self* gs) { (void)gs; say("op_render_fixes"); }
static void op_vsync(gs_state_self* gs, u32 field, bool written, bool idle) { (void)gs; sayf("op_vsync(%lu %lu %lu)", field, written, idle); }
static void op_purge(gs_state_self* gs, bool a, bool b, bool c) { (void)gs; sayf("op_purge(%lu%lu%lu)", a, b, c); }
static void op_readback(gs_state_self* gs) { (void)gs; say("op_readback"); }
static struct GSTexture* op_palette(gs_state_self* gs, u32 cbp, u32 cpsm, u32 cbw, struct GSVector2i* offset, float* scale,
	float* sample, const struct GSVector2i* size)
{
	gs_point* o = (gs_point*)(void*)offset;
	const gs_point* sz = (const gs_point*)(const void*)size;
	(void)gs;
	sayf("op_palette(%lx %lx %lx", cbp, cpsm, cbw);
	sayf("%lu,%lu", (unsigned long)o->x, (unsigned long)o->y, 0);
	sayf("%lux%lu)", (unsigned long)sz->x, (unsigned long)sz->y, 0);
	o->x += 100;
	o->y += 200;
	*scale = 2.0f;
	*sample = 3.0f;
	return (struct GSTexture*)(void*)&s_hw;
}

static struct gs_state_ops s_full, s_bare;

static void setup_ops(void)
{
	memset(&s_full, 0, sizeof(s_full));
	memset(&s_bare, 0, sizeof(s_bare));
	s_full.free = op_free;
	s_full.destroy = op_destroy;
	s_full.reset = op_reset;
	s_full.update_settings = op_update_settings;
	s_full.update_render_fixes = op_update_render_fixes;
	s_full.vsync = op_vsync;
	s_full.purge_texture_cache = op_purge;
	s_full.readback_texture_cache = op_readback;
	s_full.lookup_palette_source = op_palette;
	s_bare.free = op_free; /* (every renderer has one) */
}

/* --- the cases --------------------------------------------------------------- */

static const struct gs_renderer_ops* open_with(GSRendererType type, const struct gs_state_ops* ops)
{
	const struct gs_renderer_ops* r;
	s_hw.m_ops = ops;
	s_sw.m_ops = ops;
	s_log[0] = 0;
	r = gs_gsdx_open(type, s_basemem);
	return r;
}

static void test_open_close(void)
{
	const struct gs_renderer_ops* r;

	GSConfig.SWExtraThreads = 3;
	r = open_with(GS_RENDERER_SW, &s_full);
	CHECK(r != NULL && gs_gsdx_is_open(), "opened");
#ifdef MULTI_ISA_SHARED_COMPILATION
	LOG_IS("sw_sse4(3) pcrtc_reset op_render_fixes", "the software renderer, SSE4.1 tier");
#else
	LOG_IS("sw_native(3) pcrtc_reset op_render_fixes", "the software renderer, the one tier");
#endif
	CHECK(g_gs_renderer_c == &s_sw, "the software renderer is the open one");
	CHECK((void*)s_sw.m_regs == (void*)s_basemem && s_sw.m_present.regs == s_sw.m_regs, "the registers handed on");
	CHECK(r->regs_mem() == s_basemem, "the registers read back");
	gs_gsdx_close();
	LOG_IS("sw.destroy sw.free", "closed: destroyed, then freed");
	CHECK(!gs_gsdx_is_open() && g_gs_renderer_c == NULL, "none open");
	gs_gsdx_close();
	LOG_IS("", "closing twice does nothing");

#ifdef MULTI_ISA_SHARED_COMPILATION
	s_cpu = RETRO_SIMD_AVX;
	open_with(GS_RENDERER_SW, &s_bare);
	LOG_IS("sw_avx(3) pcrtc_reset", "the AVX tier, and nothing to fix");
	gs_gsdx_close();
	s_cpu = RETRO_SIMD_AVX | RETRO_SIMD_AVX2;
	open_with(GS_RENDERER_SW, &s_bare);
	LOG_IS("sw_avx2(3) pcrtc_reset", "the AVX2 tier");
	gs_gsdx_close();
	s_cpu = 0;
#endif
	s_log[0] = 0;

	open_with(GS_RENDERER_VK, &s_full);
	LOG_IS("hw_create pcrtc_reset op_render_fixes", "the hardware renderer");
	CHECK(g_gs_renderer_c == &s_hw, "the hardware renderer is the open one");
	gs_gsdx_close();
	LOG_IS("hw.destroy hw.free", "the hardware renderer closed");
}

static void test_ops(const struct gs_state_ops* ops, bool full)
{
	const struct gs_renderer_ops* r = open_with(GS_RENDERER_SW, ops);
	freezeData fd;
	u8 mem[64];
	s_log[0] = 0;

	r->reset(true);
	LOG_IS(full ? "op_reset(1)" : "reset_base(1)", "reset");
	r->reset(false);
	LOG_IS(full ? "op_reset(0)" : "reset_base(0)", "soft reset");

	r->init_and_read_fifo(mem, 5);
	LOG_IS("init_read_fifo(5) read_fifo(5)", "the FIFO read set up, then read");

	r->read_local_memory_unsync(mem, 6, 0x11223344ul, ((u64)0x55667788ul) << 32, 0x99aabbccul);
	LOG_IS("read_unsync(6 11223344 55667788 99aabbcc)", "an unsynced read with its registers");

	r->transfer(mem, 48);
	LOG_IS("transfer(48)", "a transfer");

	s_idle = 1;
	r->vsync(1, true);
	LOG_IS(full ? "flush(2000) op_vsync(1 1 1)" : "flush(2000) present_vsync(1 1 1)", "vsync: drawn out first, idle as the present says");
	s_idle = 0;
	r->vsync(0, false);
	LOG_IS(full ? "flush(2000) op_vsync(0 0 0)" : "flush(2000) present_vsync(0 0 0)", "a busy vsync");

	CHECK(r->freeze((int)FREEZE_SAVE, &fd) == 7, "save's result");
	LOG_IS("freeze(0)", "save");
	CHECK(r->freeze((int)FREEZE_SIZE, &fd) == 7, "size's result");
	LOG_IS("freeze(1)", "size");
	CHECK(r->freeze((int)FREEZE_LOAD, &fd) == 9, "load's result");
	LOG_IS("clear_current defrost", "load: the current textures dropped first");
	CHECK(r->freeze(42, &fd) == 0, "nothing else");
	LOG_IS("", "nothing done for an unknown mode");

	gs_gsdx_update_render_fixes();
	LOG_IS(full ? "op_render_fixes" : "", "render fixes");
	gs_gsdx_update_settings(&GSConfig);
	LOG_IS(full ? "op_update_settings" : "update_settings_base", "settings");
	gs_gsdx_readback_texture_cache();
	LOG_IS(full ? "op_readback" : "", "read back");
	gs_gsdx_purge_texture_cache();
	LOG_IS(full ? "op_purge(111) purge_pool" : "purge_pool", "purged, the pool too");

	gs_gsdx_before_reopen(true, false);
	LOG_IS(full ? "flush(4000) op_purge(111) clut_release purge_pool" : "flush(4000) clut_release purge_pool",
		"a new device under the software renderer");
	gs_gsdx_after_device_reopen();
	LOG_IS("clut_create", "the palette made on the new one");
	GSConfig.UserHacks_ReadTCOnClose = 0;
	gs_gsdx_before_reopen(true, true);
	LOG_IS("flush(4000)", "a new renderer: drawn out only");
	GSConfig.UserHacks_ReadTCOnClose = 1;
	gs_gsdx_before_reopen(false, true);
	LOG_IS(full ? "flush(4000) op_readback" : "flush(4000)", "read back on close");
	GSConfig.UserHacks_ReadTCOnClose = 0;

	{
		int ox = 4, oy = 5;
		float scale = 0, sample = 0;
		gs_texture* t = gs_gsdx_lookup_palette_source(0x10, 0x13, 2, &ox, &oy, &scale, &sample, 16, 1);
		if (full)
		{
			LOG_IS("op_palette(10 13 2 4,5 16x1)", "a palette source looked up");
			CHECK((void*)t == (void*)&s_hw && ox == 104 && oy == 205 && scale == 2.0f && sample == 3.0f,
				"the source, its offset, scale and sample handed back");
		}
		else
		{
			LOG_IS("", "no palette sources");
			CHECK(t == NULL && ox == 4 && oy == 5, "none, the offset as it was");
		}
	}

	{
		GIFRegTEXA texa;
		GIFRegTEX0 tex0;
		texa.U64 = 0x12345678ul;
		tex0.U64 = 0x9abcul;
		gs_lm_psm_table[0x13].rtxb = rtxb;
		gs_lm_psm_table[0x13].rtxbP = rtxbP;
		gs_sw_texture_read_block(0x13, 0x40, mem, 32, &texa);
		LOG_IS("rtxbP(40 32 12345678)", "a block read as a palette's indices");
		CHECK(gs_sw_texture_page2tile(&tex0) == s_p2t, "the open renderer's page map");
		LOG_IS("page2tile(9abc)", "a page map looked up");
	}

	gs_gsdx_close();
	s_log[0] = 0;
}

static void test_hw_reopen(void)
{
	open_with(GS_RENDERER_VK, &s_full);
	s_log[0] = 0;
	gs_gsdx_before_reopen(true, false);
	LOG_IS("flush(4000) hw_release_device clut_release purge_pool", "a new device under the hardware renderer");
	gs_gsdx_close();
	s_log[0] = 0;
	CHECK(gs_gsdx_lookup_palette_source(0, 0, 0, NULL, NULL, NULL, NULL, 0, 0) == NULL, "no palette with none open");
	gs_gsdx_before_reopen(true, false);
	gs_gsdx_after_device_reopen();
	LOG_IS("", "nothing to reopen with none open");
}

int main(void)
{
	setup_ops();
	test_open_close();
	test_ops(&s_full, true);
	test_ops(&s_bare, false);
	test_hw_reopen();
	if (s_failed)
	{
		printf("gsrenderer: FAILED\n");
		return 1;
	}
	printf("gsrenderer: ok\n");
	return 0;
}
