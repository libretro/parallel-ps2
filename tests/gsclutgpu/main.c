/* The GPU palette (GS/GSClutGPU.c) on a pretend device and renderer: its
 * targets made only with a hardware device, given up whole, and every
 * read taking the palette from a target exactly as the C++ GSClut did
 * (expect() below is its Read32 after the CPU read, kept as it was):
 * which load names the palette, the CSA reach per entry size, CSM2 and
 * a CSA past 15 left to the CPU palette, the 4-bit and 8-bit windows and
 * slots, and no device to copy with. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/GSClut.h"
#include "GS/GSBackend.h"
#include "GS/Renderers/Common/GSTextureBase.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

/* the device */
static int device_up;
static int made, freed;
static int made_width[8];
static void tex_free(gs_texture* t)
{
	freed++;
	free(t);
}
static const gs_texture_ops tex_ops = { tex_free, NULL, NULL, NULL, NULL };
gs_texture* gs_device_create_clut_target(int width)
{
	gs_texture* t;
	if (!device_up)
		return NULL;
	t = (gs_texture*)calloc(1, sizeof(*t));
	t->m_ops = &tex_ops;
	t->m_size.x = width;
	t->m_size.y = 1;
	made_width[made++ & 7] = width;
	return t;
}
static struct
{
	int calls;
	gs_texture* src;
	float scale, sample;
	u32 ox, oy, doff, dsize;
	gs_texture* dst;
} upd;
bool gs_device_update_clut_texture(gs_texture* src, float scale, float sample, u32 offset_x, u32 offset_y,
	gs_texture* dst, u32 dst_offset, u32 dst_size)
{
	if (!device_up)
		return false;
	upd.calls++;
	upd.src = src;
	upd.scale = scale;
	upd.sample = sample;
	upd.ox = offset_x;
	upd.oy = offset_y;
	upd.dst = dst;
	upd.doff = dst_offset;
	upd.dsize = dst_size;
	return true;
}
/* the renderer */
static gs_texture src_tex;
static int has_src;
static struct
{
	int calls;
	u32 cbp, cpsm, cbw;
	int w, h;
} look;
gs_texture* gs_gsdx_lookup_palette_source(u32 cbp, u32 cpsm, u32 cbw, int* offset_x, int* offset_y, float* scale,
	float* sample, int w, int h)
{
	look.calls++;
	look.cbp = cbp;
	look.cpsm = cpsm;
	look.cbw = cbw;
	look.w = w;
	look.h = h;
	if (!has_src)
		return NULL;
	*offset_x = 3;
	*offset_y = 5;
	*scale = 2.0f;
	*sample = 0.5f;
	return &src_tex;
}

static unsigned rng = 99u;
static u32 rnd(void)
{
	u32 hi;
	rng = rng * 1103515245u + 12345u;
	hi = rng >> 16;
	rng = rng * 1103515245u + 12345u;
	return (hi << 16) | (rng >> 16);
}

/* The C++ Read32's GPU half, after a read that happened: the update it
 * made (dsize 0: none) and the palette it left current */
struct want
{
	int lookup;
	int w, h;
	u32 doff, dsize;
	gs_texture* current;
};
static struct want expect(const gs_clut* c, const GIFRegTEX0* TEX0)
{
	struct want r;
	const u32 csa = TEX0->CSA & ((TEX0->CPSM == PSMCT16 || TEX0->CPSM == PSMCT16S) ? 31u : 15u);
	memset(&r, 0, sizeof(r));
	if (!TEX0->CSM && csa <= 15)
	{
		const int is_4bit = (TEX0->PSM == PSMT4 || TEX0->PSM == PSMT4HL || TEX0->PSM == PSMT4HH);
		r.lookup = 1;
		r.w = is_4bit ? 8 : 16;
		r.h = is_4bit ? 2 : 16;
		/* if (src && g_gs_device) { if (slot) { update; current = slot; } } */
		if (has_src && device_up)
		{
			gs_texture* const slot = is_4bit ? c->gpu_clut4 : c->gpu_clut8;
			if (slot)
			{
				r.doff = csa << 4;
				r.dsize = is_4bit ? 16 : 256;
				r.current = slot;
			}
		}
	}
	return r;
}

int main(void)
{
	static const u32 psms[] = { PSMT8, PSMT8H, PSMT4, PSMT4HL, PSMT4HH, PSMCT32 };
	static const u32 cpsms[] = { PSMCT32, PSMCT16, PSMCT16S, PSMCT24 };
	/* the core's local memory is mapped wrapped: a palette past the end
	 * reads the start again */
	static u8 vm[2 * 4 * 1024 * 1024];
	gs_clut c, twin;
	GIFRegTEX0 last0;
	GIFRegTEXA lasta;
	int i, reads = 0, gpu = 0, reopened = 0;

	/* No device, or the software renderer: no targets, no GPU palette */
	gs_clut_init(&c);
	gs_clut_gpu_create(&c);
	CHECK(!c.gpu_clut4 && !c.gpu_clut8 && made == 0, "no device: no targets");
	gs_clut_gpu_release(&c);
	CHECK(freed == 0, "nothing to free");

	/* A hardware device: one of each, once */
	device_up = 1;
	gs_clut_gpu_create(&c);
	CHECK(c.gpu_clut4 && c.gpu_clut4->m_size.x == 16 && c.gpu_clut8 && c.gpu_clut8->m_size.x == 256,
		"16x1 and 256x1");
	gs_clut_gpu_create(&c);
	CHECK(made == 2, "made again: nothing new");

	/* Reads against the C++ one; the twin's plain read says whether one
	 * happened */
	gs_clut_init(&twin);
	for (i = 0; i < (int)sizeof(vm) / 2; i++)
		vm[i] = vm[i + sizeof(vm) / 2] = (u8)rnd();
	for (i = 0; i < 20000; i++)
	{
		GIFRegTEX0 TEX0;
		GIFRegTEXA TEXA;
		struct want w;
		int read;
		gs_texture* before = c.gpu_current;

		if (rnd() % 3 == 0)
		{
			GIFRegTEX0 L;
			GIFRegTEXCLUT TC;
			L.U64 = ((u64)rnd() << 32) | rnd();
			TC.U64 = ((u64)rnd() << 32) | rnd();
			L.CPSM = cpsms[rnd() % 4];
			gs_clut_write(&c, vm, &L, &TC);
			gs_clut_write(&twin, vm, &L, &TC);
		}
		if (i && rnd() % 4 == 0)
		{
			/* the same again: read only if a load made it stale */
			TEX0 = last0;
			TEXA = lasta;
		}
		else
		{
			TEX0.U64 = ((u64)rnd() << 32) | rnd();
			TEXA.U64 = ((u64)rnd() << 32) | rnd();
			TEX0.PSM = psms[rnd() % 6];
			TEX0.CPSM = cpsms[rnd() % 4];
			if (rnd() & 1)
				TEX0.CSM = 0;
		}
		last0 = TEX0;
		lasta = TEXA;
		has_src = (rnd() % 4) != 0;
		if (i >= 10000 && !reopened && c.gpu_current)
		{
			/* a reopen: the targets given up with one current, and made
			 * again */
			reopened = 1;
			gs_clut_gpu_release(&c);
			CHECK(!c.gpu_current && !c.gpu_clut4 && !c.gpu_clut8, "released: none current");
			gs_clut_gpu_create(&c);
			before = c.gpu_current;
		}
		if (i == 15000)
			device_up = 0; /* the device gone with the targets still held */
		memset(&upd, 0, sizeof(upd));
		memset(&look, 0, sizeof(look));
		read = gs_clut_read32(&twin, &TEX0, &TEXA);
		gs_clut_read32_gpu(&c, &TEX0, &TEXA);
		if (!read)
		{
			CHECK(c.gpu_current == before && look.calls == 0 && upd.calls == 0, "no read: nothing changes");
			continue;
		}
		reads++;
		w = expect(&c, &TEX0);
		CHECK(look.calls == w.lookup, "looked up exactly when the C++ did");
		if (w.lookup)
			CHECK(look.cbp == c.write.TEX0.CBP && look.cpsm == c.write.TEX0.CPSM && look.cbw == 0 && look.w == w.w
				&& look.h == w.h, "the load's CBP and CPSM, the window");
		CHECK(upd.calls == (w.dsize != 0), "updated exactly when the C++ did");
		if (w.dsize)
		{
			gpu++;
			CHECK(upd.src == &src_tex && upd.dst == w.current && upd.doff == w.doff && upd.dsize == w.dsize
				&& upd.ox == 3 && upd.oy == 5 && upd.scale == 2.0f && upd.sample == 0.5f, "the copy's arguments");
		}
		CHECK(c.gpu_current == w.current, "the current GPU palette");
		CHECK(memcmp(c.buff32, twin.buff32, 256 * 4) == 0, "the CPU palette as without the GPU");
	}
	CHECK(reads > 1000 && reads < 19000 && gpu > 500, "the sweep reached every path");

	gs_clut_gpu_release(&c);
	CHECK(reopened && freed == 4 && !c.gpu_clut4 && !c.gpu_clut8 && !c.gpu_current, "released whole");
	gs_clut_deinit(&c);
	gs_clut_deinit(&twin);
	printf("gsclutgpu: %d of 20000 read, %d from a target, %d failure(s)\n", reads, gpu, fails);
	return fails != 0;
}
