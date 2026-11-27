/* The GIF path (GS/GSState.c) over random GIF streams, through the class
 * the renderers reach it by: every draw handed to the renderer (its
 * vertices, indices, trace, context, why it was flushed), every local
 * memory invalidation asked for, the savestates taken and what is left in
 * the registers and local memory, hashed, held to what the C++ GSState
 * drew from the same streams. The streams take every primitive, both
 * provoking vertices, plain and segmented auto flush, the register
 * handlers, image transfers both ways and partial, moves, soft resets,
 * savestates and packets split between transfers. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <initializer_list>
#include "GS/GSState.h"
#include "GS/GSUtil.h"

static void quiet(enum retro_log_level, const char*, ...) {}
extern "C" {
retro_log_printf_t log_cb = quiet;
EmuGSOptions GSConfig;
bool GSSnapshotSave = false;
u64 GSSnapshotKey = 0;
int m_disp_fb_sprite_blits = 0;
static bool s_hw;
bool GSIsHardwareRenderer(void) { return s_hw; }
static gs_device s_dev;
gs_device* g_gs_device_c = &s_dev;
void gs_device_clear_current(gs_device* dev) { (void)dev; }
struct gs_texture* gs_device_create_clut_target(int width) { (void)width; return NULL; }
bool gs_device_update_clut_texture(struct gs_texture*, float, float, u32, u32, struct gs_texture*, u32, u32) { return false; }
struct gs_texture* gs_gsdx_lookup_palette_source(u32, u32, u32, int*, int*, float*, float*, int, int) { return NULL; }
void gs_present_init(gs_present* p, void* gs, const gs_present_ops* ops, gs_pcrtc* pcrtc, u8* scanmask_used,
	int* draw_n, const int* transfer_n)
{
	(void)p; (void)gs; (void)ops; (void)pcrtc; (void)scanmask_used; (void)draw_n; (void)transfer_n;
}
}

static u64 s_rng;
static u32 rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (u32)(s_rng >> 16); }
static u32 rn(u32 n) { return rnd() % n; }
static u64 rnd64(void) { const u64 lo = rnd(); return lo | ((u64)rnd() << 32); }
/* Fields drawn in the order written: a braced list is evaluated left to
 * right, the operands of | are not (and compilers differ). */
struct field { u64 v; unsigned sh; };
static u64 pk(std::initializer_list<field> l)
{
	u64 r = 0;
	for (const field& f : l)
		r |= f.v << f.sh;
	return r;
}

static u64 s_hash;
static unsigned s_draws, s_vtx, s_idx;
static void mix(const void* p, size_t n)
{
	const u8* b = (const u8*)p;
	size_t i;
	for (i = 0; i < n; i++)
		s_hash = (s_hash ^ b[i]) * 0x100000001b3ull;
}

static void op_draw(GSState* gs)
{
	u32 i;
	s_draws++;
	s_vtx += gs->m_vertex.tail;
	s_idx += gs->m_index.tail;
	mix(&gs->PRIM->U32[0], 4);
	mix(&gs->m_vertex.tail, 4);
	mix(&gs->m_index.tail, 4);
	mix(gs->m_index.buff, gs->m_index.tail * 2);
	for (i = 0; i < gs->m_vertex.tail; i++)
		mix(&gs->m_vertex.buff[i], 32);
	mix(gs->m_context, offsetof(gs_drawing_regs, scissor) + 48);
	mix(&gs->m_vt.m_min, sizeof(gs->m_vt.m_min));
	mix(&gs->m_vt.m_max, sizeof(gs->m_vt.m_max));
	mix(&gs->m_vt.m_eq, 4);
	mix(&gs->m_state_flush_reason, sizeof(gs->m_state_flush_reason));
	mix(&gs->temp_draw_rect, 16);
}

static bool s_runs;
static bool op_can_draw_runs(GSState* gs) { (void)gs; return s_runs; }
static void op_inv_video(GSState* gs, const GIFRegBITBLTBUF* b, const GSVector4i* r)
{
	(void)gs;
	mix("V", 1);
	mix(b, 8);
	mix(r, 16);
}
static void op_inv_local(GSState* gs, const GIFRegBITBLTBUF* b, const GSVector4i* r, bool clut)
{
	(void)gs;
	mix("L", 1);
	mix(b, 8);
	mix(r, 16);
	mix(&clut, 1);
}
static u64 vmh(const u8* v)
{
	u64 vh = 0xcbf29ce484222325ull;
	u32 q;
	for (q = 0; q < GS_LOCAL_MEMORY_SIZE; q++)
		vh = (vh ^ v[q]) * 0x100000001b3ull;
	return vh;
}
static void op_mem_restored(GSState* gs, const u32* changed)
{
	(void)gs;
	mix(changed, GS_MAX_PAGES / 8);
}
static void op_draw_count(GSState* gs) { s_draws++; s_vtx += gs->m_vertex.tail; }

struct Bench : public GSState
{
};

/* ---- the stream ---- */
static u8 s_buf[1 << 24];
static u32 s_qw;
static u32 s_pk[1 << 16];
static u32 s_npk;

static void put64(u64 lo, u64 hi)
{
	memcpy(s_buf + s_qw * 16, &lo, 8);
	memcpy(s_buf + s_qw * 16 + 8, &hi, 8);
	s_qw++;
}

static u64 tag(u32 nloop, bool pre, u32 prim, u32 flg, u32 nreg, u64 regs)
{
	s_pk[s_npk++] = s_qw;
	return (u64)nloop | ((u64)(pre ? 1 : 0) << 46) | ((u64)prim << 47) | ((u64)flg << 58) | ((u64)(nreg & 15) << 60) | 0 * regs;
}

static void ad(u32 addr, u64 data) { put64(data, addr); }
static void put2(std::initializer_list<u64> l) { put64(l.begin()[0], l.begin()[1]); }

static u32 s_ofs = (2048 - 320) << 4;

static u64 xyz(int x, int y, u32 z, bool f2, u32 fog, bool adc)
{
	const u64 X = (u64)(u16)(s_ofs + x * 16 + (int)rn(16));
	const u64 Y = (u64)(u16)(s_ofs + y * 16 + (int)rn(16));
	(void)f2;
	return X | (Y << 32) | ((u64)adc << 47) * 0 | ((u64)z << 0) * 0 | ((u64)fog * 0);
}

static void vertex_packed(bool f2, int x, int y, bool adc)
{
	/* STQ, RGBA, XYZF2/XYZ2 */
	const float s = (float)rn(1024) / 512.0f, t = (float)rn(1024) / 512.0f;
	float q = (rn(16) == 0) ? 0.0f : 1.0f + (float)rn(8);
	u32 si, ti, qi;
	u64 hi;
	if (rn(64) == 0)
		qi = 0x7fc00000;
	else
		memcpy(&qi, &q, 4);
	memcpy(&si, &s, 4);
	memcpy(&ti, &t, 4);
	put64((u64)si | ((u64)ti << 32), qi);
	put2({pk({{rn(256), 0}, {rn(256), 32}}), pk({{rn(256), 0}, {rn(256), 32}})});
	hi = f2 ? pk({{rn(1 << 24), 4}, {rn(256), 36}}) : (u64)rnd();
	hi |= (u64)(adc ? 1 : 0) << 47;
	put64(xyz(x, y, 0, f2, 0, false) & 0x0000ffff0000ffffull, hi);
}

static void gen_regs(void)
{
	static const u32 psms[] = {0x00, 0x01, 0x02, 0x0a, 0x13, 0x14, 0x1b, 0x24, 0x2c, 0x30, 0x31, 0x32, 0x3a};
	u32 n = 1 + rn(8), i;
	put64(tag(n, false, 0, 0, 1, 0xe), 0xe);
	for (i = 0; i < n; i++)
	{
		const u32 ctx = rn(2);
		switch (rn(26))
		{
			case 0: ad(0x40 + ctx, pk({{rn(4) == 0 ? rn(640) : 0, 0}, {rn(4) == 0 ? rn(640) : 639, 16}, {rn(4) == 0 ? rn(448) : 447, 48}})); break;
			case 1: ad(0x18 + ctx, rn(8) ? ((u64)s_ofs | ((u64)s_ofs << 32)) : rnd64()); break;
			case 2: ad(0x47 + ctx, rnd() & 0x7ffff); break;
			case 3: ad(0x4c + ctx, pk({{rn(64), 0}, {1 + rn(16), 16}, {psms[rn(13)], 24}, {rn(4) ? 0 : rnd(), 32}})); break;
			case 4: ad(0x4e + ctx, pk({{rn(512), 0}, {psms[rn(13)], 24}, {rn(2), 32}})); break;
			case 5: ad(0x06 + ctx, pk({{rn(1 << 14), 0}, {rn(64), 14}, {psms[rn(13)], 20}, {rn(16), 26}, {rn(16), 30}, {rnd(), 34}})); break;
			case 6: ad(0x16 + ctx, pk({{rnd(), 20}, {rnd(), 40}})); break;
			case 7: ad(0x08 + ctx, rnd64()); break;
			case 8: ad(0x42 + ctx, pk({{rnd(), 0}, {rn(256), 32}})); break;
			case 9: ad(0x14 + ctx, rnd64()); break;
			case 10: ad(0x1a, rn(4) ? 1 : 0); break;
			case 11: ad(0x1b, rnd() & 0x7ff); break;
			case 12: ad(0x3f, 0); break;
			case 13: ad(0x3b, rnd64()); break;
			case 14: ad(0x3d, rnd()); break;
			case 15: ad(0x44, rnd64()); break;
			case 16: ad(0x45, rn(2)); break;
			case 17: ad(0x46, rn(2)); break;
			case 18: ad(0x49, rn(2)); break;
			case 19: ad(0x4a + ctx, rn(2)); break;
			case 20: ad(0x22, rn(4)); break;
			case 21: ad(0x00, rnd() & 0x7ff); break;
			case 22: ad(0x34 + ctx, rnd64()); break;
			case 23: ad(0x36 + ctx, rnd64()); break;
			case 24: ad(0x1c, rnd() & 0x3fffff); break;
			case 25: ad(0x54, rnd64()); break;
		}
	}
}

static void gen_feedback(void)
{
	const u32 ctx = rn(2), p = rn(32), psm = rn(4) ? 0 : 2;
	put64(tag(4, false, 0, 0, 1, 0xe), 0xe);
	ad(0x4c + ctx, (u64)p | ((u64)10 << 16) | ((u64)psm << 24));
	ad(0x06 + ctx, pk({{p << 5, 0}, {10, 14}, {psm, 20}, {5 + rn(6), 26}, {5 + rn(6), 30}}));
	ad(0x47 + ctx, rn(2) ? 0x30000 : rnd() & 0x7ffff);
	ad(0x40 + ctx, (u64)639 << 16 | (u64)447 << 48);
	if (rn(4) == 0)
	{
		put64(tag(1, false, 0, 0, 1, 0xe), 0xe);
		ad(0x3f, 0);
	}
}

static void gen_image(void)
{
	const u32 w = 8 << rn(3), h = 1 + rn(16);
	const u32 psm = rn(2) ? 0x00 : 0x02;
	const u32 bpp = psm == 0 ? 4 : 2;
	u32 qws = (w * h * bpp + 15) / 16, i;
	const u32 dir = rn(6) == 0 ? 2 : (rn(5) == 0 ? 1 : 0);
	put64(tag(4, false, 0, 0, 1, 0xe), 0xe);
	ad(0x50, pk({{rn(4096), 0}, {1 + rn(8), 16}, {psm, 24}, {rn(4096), 32}, {1 + rn(8), 48}, {psm, 56}}));
	ad(0x51, pk({{rn(256), 0}, {rn(256), 16}, {rn(256), 32}, {rn(256), 48}}));
	ad(0x52, (u64)w | ((u64)h << 32));
	ad(0x53, dir);
	if (dir == 2 && rn(2))
	{
		put64(tag(1, false, 0, 2, 0, 0), 0);
		put64(0, 0);
	}
	if (dir != 0)
		return;
	if (rn(4) == 0)
		qws = 1 + rn(qws);
	/* in one IMAGE packet, or in pieces (the last one completing it from
	 * the buffer), maybe with a register written between; and maybe more
	 * data than was asked for after it */
	while (qws)
	{
		const u32 n = (qws > 1 && rn(3) == 0) ? 1 + rn(qws - 1) : qws;
		put64(tag(n, false, 0, 2, 0, 0), 0);
		for (i = 0; i < n; i++)
			put2({rnd64(), rnd64()});
		qws -= n;
		if (qws && rn(2))
		{
			put64(tag(1, false, 0, 0, 1, 0xe), 0xe);
			ad(0x3f, 0);
		}
	}
	if (rn(8) == 0)
	{
		put64(tag(2, false, 0, 2, 0, 0), 0);
		put2({rnd64(), rnd64()});
		put2({rnd64(), rnd64()});
	}
}

static void gen_vertices(bool heavy)
{
	const u32 prim = heavy ? (3 + rn(2)) : rn(8);
	const u32 tme = rn(2), fst = rn(2), ctxt = rn(2), iip = rn(2), abe = rn(2), fge = rn(2);
	const u32 primreg = prim | (iip << 3) | (tme << 4) | (fge << 5) | (abe << 6) | (fst << 8) | (ctxt << 9);
	const bool f2 = rn(2) != 0;
	const u32 n = heavy ? 96 : 1 + rn(48);
	int x = (int)rn(640), y = (int)rn(448);
	u32 i;
	const u32 split = (!heavy && n >= 3 && rn(4) == 0) ? 1 + rn((n - 1) * 3) : 0;
	const u32 start = s_qw;
	put64(tag(n, true, primreg, 0, 3, 0), f2 ? 0x412 : 0x512);
	if (split)
		s_pk[s_npk++] = start + split;
	for (i = 0; i < n; i++)
	{
		if (!heavy || rn(8) == 0)
		{
			x += (int)rn(64) - 32;
			y += (int)rn(64) - 32;
		}
		else
		{
			x += (int)rn(9) - 4;
			y += (int)rn(9) - 4;
		}
		vertex_packed(f2, x, y, !heavy && rn(16) == 0);
	}
}

static void gen_long(void)
{
	/* one primitive for more vertices than a draw takes: culled all along
	 * (the same point) or drawn (tiny distinct triangles) */
	const bool culled = rn(2) != 0;
	const u32 prim = culled ? (rn(2) ? 4 : 5) : 3;
	u32 left = 67000, first = 1;
	int x = 100, y = 100;
	while (left)
	{
		const u32 n = left > 30000 ? 30000 : left;
		u32 i;
		put64(tag(n, first != 0, prim | (1u << 9) * rn(2), 0, 1, 0), 0x5);
		first = 0;
		for (i = 0; i < n; i++)
		{
			if (!culled)
			{
				x = 10 + (int)(i % 600);
				y = 10 + (int)((i / 600) % 400) + (int)(i % 3);
			}
			put64((u64)(u16)(s_ofs + x * 16) | ((u64)(u16)(s_ofs + y * 16) << 32), (u64)i);
		}
		left -= n;
	}
}

static void gen_generic(void)
{
	/* RGBA, ST, UV, FOG, XYZ3/XYZF3/XYZ2 in random order: the unknown-type
	 * packed path, and REGLIST */
	static const u8 regs[] = {0x1, 0x2, 0x3, 0xa, 0x4, 0x5, 0xc, 0xd, 0xf, 0xe, 0x0, 0x6, 0x8};
	const u32 nreg = 1 + rn(6);
	u64 r = 0;
	u32 i, j, nloop = 1 + rn(16);
	for (i = 0; i < nreg; i++)
		r |= (u64)regs[rn(13)] << (4 * i);
	if (rn(4) == 0)
	{
		put64(tag(nloop, false, 0, 1, nreg, 0), r);
		for (i = 0; i < (nloop * nreg + 1) / 2; i++)
			put2({rnd64(), rnd64()});
		return;
	}
	put64(tag(nloop, true, rnd() & 0x7ff, 0, nreg, 0), r);
	for (j = 0; j < nloop; j++)
		for (i = 0; i < nreg; i++)
			put2({pk({{s_ofs + rn(640 * 16), 0}, {s_ofs + rn(448 * 16), 32}, {(u64)rnd() * 0, 0}}), rnd64()});
}

alignas(64) static u8 s_regs[0x2000];

static void run(Bench* gs, bool heavy)
{
	u32 off = 0, k = 0;
	while (off < s_qw)
	{
		/* in runs of whole packets, as the MTGS hands them over */
		u32 end;
		k += heavy ? s_npk : 1 + rn(8);
		end = (k < s_npk) ? s_pk[k] : s_qw;
		gs->Transfer(s_buf + off * 16, end - off);
		off = end;
	}
	if (heavy)
		gs->Flush(GSState::VSYNC);
}

/* The C++ GSState's, over SEEDS streams a mode (it is gone; these are
 * what it drew). The architectures differ in what the trace and local
 * memory make of the same stream, so each has its own. */
#define SEEDS 16
#if defined(__aarch64__) || defined(_M_ARM64)
static const u64 s_golden[4] = {0x7b9641f3c0b2b9a3ull, 0xb3a73525a7eaf1faull, 0xd70849551838715dull, 0x5c5c88c0339d3e34ull};
#else
static const u64 s_golden[4] = {0x41ab1f96480ffb32ull, 0x054767a4ef0c8f6full, 0x584738c352d4800eull, 0x0676d8f446de6f56ull};
#endif
static const char* const s_mode_name[4] = {
	"software, last provoking vertex", "hardware, last provoking vertex",
	"software, first, segmented auto flush", "hardware, first, segmented auto flush"};

int main(int argc, char** argv)
{
	const int seeds = SEEDS;
	const bool verbose = argc > 1;
	int seed, mode, fails = 0;
	unsigned draws = 0;
	static gs_state_ops ops;

	gs_util_init();
	gs_local_memory_populate_transfers();
	memset(&GSConfig, 0, sizeof(GSConfig));
	GSConfig.UpscaleMultiplier = 1.0f;
	ops.draw = op_draw;
	ops.invalidate_video_mem = op_inv_video;
	ops.invalidate_local_mem = op_inv_local;
	ops.mem_restored = op_mem_restored;

	for (mode = 0; mode < 4; mode++)
	{
		u64 total = 0;
		s_hw = (mode & 1) != 0;
		GSConfig.Renderer = s_hw ? GS_RENDERER_VK : GS_RENDERER_SW;
		s_dev.m_features.provoking_vertex_last = (mode & 2) ? 1 : 0;
		ops.can_draw_runs = (mode & 2) ? op_can_draw_runs : NULL;
		ops.draw = op_draw;
		for (seed = 0; seed < seeds; seed++)
		{
			Bench* gs;
			u32 k;
			GSConfig.UserHacks_ForceEvenSpritePosition = (seed % 3) == 1;
			GSConfig.UserHacks_Limit24BitDepth = (GSLimit24BitDepth)(seed % 3);
			gs = new Bench();
			gs->m_ops = &ops;
			/* (left as the heap had it, in both: the unsync read can hand
			 * back bytes no image wrote) */
			memset(gs->m_tr.buff, 0, GS_TRANSFER_BUFFER_SIZE);
			gs->m_pixel_feedback_ok = (seed & 1) != 0;
			gs->SetRegsMem(s_regs);
			s_rng = 0x9E3779B97F4A7C15ull ^ ((u64)seed << 20) ^ (u64)mode;
			s_qw = 0;
			s_npk = 0;
			s_hash = 0xcbf29ce484222325ull;
			s_draws = s_vtx = s_idx = 0;
			s_runs = (seed & 2) != 0;
			for (k = 0; k < 300; k++)
			{
				u32 ev;
				s_qw = 0;
				s_npk = 0;
				ev = rn(10);
				ev += rn(60) == 0 ? 10 : 0;
				switch (ev)
				{
					case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19:
						gen_long();
						break;
					case 0: case 1: gen_regs(); break;
					case 2: if (rn(3) == 0) gen_image(); else gen_regs(); break;
					case 3: gen_generic(); break;
					case 4: gen_feedback(); break;
					default: gen_vertices(false); break;
				}
				run(gs, false);
				switch (rn(40))
				{
					case 0:
						gs->SoftReset(rn(8));
						break;
					case 1:
					{
						static u8 rb[1 << 16];
						const int len = 16 * (1 + (int)rn(64));
						/* a local -> host transfer set up first, as the EE does */
						s_qw = 0;
						s_npk = 0;
						put64(tag(4, false, 0, 0, 1, 0xe), 0xe);
						ad(0x50, pk({{rn(4096), 0}, {1 + rn(8), 16}, {rn(2) ? 0u : 2u, 24}}));
						ad(0x51, pk({{rn(256), 0}, {rn(256), 16}}));
						ad(0x52, pk({{8u << rn(3), 0}, {1 + rn(16), 32}}));
						ad(0x53, 1);
						run(gs, false);
						memset(rb, 0, sizeof(rb));
						gs->InitReadFIFO(rb, len / 16);
						gs->ReadFIFO(rb, len / 16);
						mix(rb, len);
						break;
					}
					case 2:
					{
						static u8 rb[1 << 16];
						GIFRegBITBLTBUF b;
						/* (not with a write pending: moving its end makes the next
						 * flush write past what it holds, from wherever) */
						if (gs->m_tr.write)
							break;
						GIFRegTRXPOS p;
						GIFRegTRXREG r;
						b.U64 = pk({{rn(4096), 0}, {1 + rn(8), 16}});
						p.U64 = pk({{rn(64), 0}, {rn(64), 16}});
						r.U64 = pk({{8u << rn(3), 0}, {1 + rn(16), 32}});
						memset(rb, 0, sizeof(rb));
						gs->ReadLocalMemoryUnsync(rb, 1 + rn(32), b, p, r);
						mix(rb, 1024);
						break;
					}
					case 3:
					{
						static u8 st[1 << 23];
						freezeData fd;
						fd.size = GSState::GetSaveStateSize();
						fd.data = st;
						if (gs->Freeze(&fd, false) == 0)
						{
							mix(st, fd.size);
							if (rn(2))
								{
									const u32 at = rn(GS_LOCAL_MEMORY_SIZE - 8192);
									memset(gs->m_mem.m_vm8 + at, (int)rn(256), 8192);
								}
							gs->Defrost(&fd);
						}
						break;
					}
					default:
						break;
				}
			}
			gs->Flush(GSState::VSYNC);
			mix(&gs->m_env, offsetof(GSDrawingEnvironment, CTXT));
			mix(&gs->m_env.CTXT[0], offsetof(gs_drawing_regs, scissor) + 48);
			mix(&gs->m_env.CTXT[1], offsetof(gs_drawing_regs, scissor) + 48);
			mix(&gs->m_v, 32);
			mix(&gs->m_q, 4);
			mix(gs->m_mem.m_vm8, GS_LOCAL_MEMORY_SIZE);
			mix(&gs->m_dirty_gs_regs, 4);
			if (verbose)
				printf("mode %d seed %3d: %5u draws %7u vertices %7u indices hash %016llx\n", mode, seed, s_draws, s_vtx,
					s_idx, (unsigned long long)s_hash);
			total ^= s_hash * (seed + 1);
			draws += s_draws;
			delete gs;
		}
		if (total != s_golden[mode])
		{
			printf("  FAIL: %s: %016llx, the C++ GSState's %016llx\n", s_mode_name[mode], (unsigned long long)total,
				(unsigned long long)s_golden[mode]);
			fails++;
		}
		else
			printf("  %s: as the C++ GSState drew, %d streams\n", s_mode_name[mode], seeds);
	}

	/* timed: triangle lists and strips in large packets */
	{
		Bench* gs = new Bench();
		double best = 1e30;
		int rep;
		u32 k;
		s_hw = false;
		GSConfig.Renderer = GS_RENDERER_SW;
		ops.can_draw_runs = NULL;
		ops.draw = op_draw_count;
		gs->m_ops = &ops;
		gs->SetRegsMem(s_regs);
		s_rng = 12345;
		s_qw = 0;
		s_npk = 0;
		/* a full scissor and the offset, then vertices */
		put64(tag(4, false, 0, 0, 1, 0xe), 0xe);
		ad(0x40, (u64)639 << 16 | (u64)447 << 48);
		ad(0x18, (u64)s_ofs | ((u64)s_ofs << 32));
		ad(0x47, 0x30000);
		ad(0x1a, 1);
		for (k = 0; k < 2000; k++)
			gen_vertices(true);
		for (rep = 0; rep < 3; rep++)
		{
			double ns;
			const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
			run(gs, true);
			ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
			if (ns < best)
				best = ns;
		}
		printf("  %.1f ns a vertex through the packed STQ/RGBA/XYZ path\n", best / (2000.0 * 96.0));
		delete gs;
	}

	if (fails)
	{
		printf("FAIL: gsstate: %d of 4 modes differ (%u draws)\n", fails, draws);
		return 1;
	}
	printf("gsstate: ok, %u draws\n", draws);
	return 0;
}
