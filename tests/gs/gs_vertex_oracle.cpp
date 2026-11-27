/* Oracle suite for the GS packed-vertex parse kernels (GS/GSVertexKick.h).
 *
 * Each kernel is checked against an independent scalar model of the GIF/GS
 * semantics, written in plain integer C with no GSVector, over directed edge
 * cases plus randomized sweeps. The two sides can only agree by both being
 * right, which is the point: an optimized kernel that lands later is held to
 * the same model rather than to a recording of the current implementation.
 *
 * Ported from yaps2's GV-0 oracle (gs_vertex_tests.cpp) into this tree's
 * standalone-test style -- no gtest, exits non-zero on the first mismatch.
 *
 * Build: sh tests/gs/build.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/GSVertexKick.h"

typedef struct
{
	u32 m0[4];
	u32 m1[4];
} RefVertex;

static u32 Rd32(const u8* rec, int dword)
{
	u32 v;
	memcpy(&v, rec + dword * 4, 4);
	return v;
}

/* Scalar model of packed {STQ, RGBAQ, XYZF2}. rec is 48 bytes (3 qwords). */
static RefVertex RefParseXYZF2(const u8* rec, u32 uv)
{
	RefVertex o;
	u32 q;

	o.m0[0] = Rd32(rec, 0); /* S */
	o.m0[1] = Rd32(rec, 1); /* T */
	o.m0[2] = (Rd32(rec, 4) & 0xff) | ((Rd32(rec, 5) & 0xff) << 8) |
	          ((Rd32(rec, 6) & 0xff) << 16) | ((Rd32(rec, 7) & 0xff) << 24); /* RGBA */

	/* Q is compared as an integer against +0.0, so -0.0 passes through. */
	q = Rd32(rec, 2);
	o.m0[3] = (q == 0) ? 0x00800000u : q;

	o.m1[0] = (Rd32(rec, 8) & 0xffff) | ((Rd32(rec, 9) & 0xffff) << 16); /* X | Y<<16 */
	o.m1[1] = (Rd32(rec, 10) >> 4) & 0x00ffffffu;                        /* Z, 24-bit */
	o.m1[2] = uv;
	o.m1[3] = (Rd32(rec, 11) >> 4) & 0xffu;                              /* F */
	return o;
}

/* Same, for XYZ2: Z keeps all 32 bits, and UV/FOG both come from vertex state. */
static RefVertex RefParseXYZ2(const u8* rec, u64 uvfog)
{
	RefVertex o = RefParseXYZF2(rec, (u32)uvfog);
	o.m1[1] = Rd32(rec, 10);
	o.m1[3] = (u32)(uvfog >> 32);
	return o;
}

static u64 g_seed = 0x243f6a8885a308d3ull;

static u32 Rand32(void)
{
	g_seed ^= g_seed << 13;
	g_seed ^= g_seed >> 7;
	g_seed ^= g_seed << 17;
	return (u32)(g_seed >> 16);
}

static unsigned long g_checked;
static unsigned long g_failed;

static void Compare(const char* what, const u32* got, const RefVertex* want, const u8* rec)
{
	int i;
	int bad = 0;

	for (i = 0; i < 4; i++)
		if (got[i] != want->m0[i] || got[4 + i] != want->m1[i])
			bad = 1;

	g_checked++;
	if (!bad)
		return;

	g_failed++;
	if (g_failed <= 5)
	{
		printf("MISMATCH %s\n", what);
		for (i = 0; i < 4; i++)
			printf("  m0[%d] kernel=%08x model=%08x%s\n", i, got[i], want->m0[i],
			       got[i] != want->m0[i] ? "   <<<" : "");
		for (i = 0; i < 4; i++)
			printf("  m1[%d] kernel=%08x model=%08x%s\n", i, got[4 + i], want->m1[i],
			       got[4 + i] != want->m1[i] ? "   <<<" : "");
		printf("  record:");
		for (i = 0; i < 12; i++)
			printf(" %08x", Rd32(rec, i));
		printf("\n");
	}
}

static void RunXYZF2(const u8* rec, u32 uv)
{
	alignas(16) gs_vec4i out[2];
	RefVertex want;

	gs_vk_parse_stqrgbaxyzf2((const GIFPackedReg*)rec, uv, out);
	want = RefParseXYZF2(rec, uv);
	Compare("STQRGBAXYZF2", (const u32*)out, &want, rec);
}

static void RunXYZ2(const u8* rec, u64 uvfog)
{
	alignas(16) gs_vec4i out[2];
	RefVertex want;

	gs_vk_parse_stqrgbaxyz2((const GIFPackedReg*)rec, &uvfog, out);
	want = RefParseXYZ2(rec, uvfog);
	Compare("STQRGBAXYZ2", (const u32*)out, &want, rec);
}

static void SetDword(u8* rec, int dword, u32 v)
{
	memcpy(rec + dword * 4, &v, 4);
}


/* ---- cull kernel oracle ------------------------------------------------ */

typedef struct { int x, y, z, w; } RefV;

static int RefMin(int a, int b) { return a < b ? a : b; }
static int RefMax(int a, int b) { return a > b ? a : b; }

/* Scalar model of the accept/cull decision. Mirrors the documented rules
 * rather than the vector code: bbox wholly outside the cull rect, a
 * degenerate extent covering no pixel (native res only), and for triangles
 * any two coincident vertices. */
static u32 RefCullSkip(int prim, RefV v0, RefV v1, RefV v2, RefV cmin, RefV cmax, int nativeres)
{
	RefV pmin, pmax;
	int is_tri, is_quad_like;
	int outside;

	is_tri = (prim == GS_TRIANGLELIST || prim == GS_TRIANGLESTRIP || prim == GS_TRIANGLEFAN);
	is_quad_like = is_tri || (prim == GS_SPRITE);

	if (prim == GS_POINTLIST)
	{
		pmin = v0;
		pmax = v0;
	}
	else if (is_tri)
	{
		pmin.x = RefMin(v0.x, RefMin(v1.x, v2.x)); pmin.y = RefMin(v0.y, RefMin(v1.y, v2.y));
		pmin.z = RefMin(v0.z, RefMin(v1.z, v2.z)); pmin.w = RefMin(v0.w, RefMin(v1.w, v2.w));
		pmax.x = RefMax(v0.x, RefMax(v1.x, v2.x)); pmax.y = RefMax(v0.y, RefMax(v1.y, v2.y));
		pmax.z = RefMax(v0.z, RefMax(v1.z, v2.z)); pmax.w = RefMax(v0.w, RefMax(v1.w, v2.w));
	}
	else
	{
		pmin.x = RefMin(v0.x, v1.x); pmin.y = RefMin(v0.y, v1.y);
		pmin.z = RefMin(v0.z, v1.z); pmin.w = RefMin(v0.w, v1.w);
		pmax.x = RefMax(v0.x, v1.x); pmax.y = RefMax(v0.y, v1.y);
		pmax.z = RefMax(v0.z, v1.z); pmax.w = RefMax(v0.w, v1.w);
	}

	/* Only the xy lanes decide the skip; zw carries offset coordinates. */
	outside = (pmax.x < cmin.x) || (pmin.x > cmax.x) || (pmax.y < cmin.y) || (pmin.y > cmax.y);

	if (is_quad_like)
	{
		if (nativeres)
		{
			/* zwzw: the z/w lanes' degeneracy is broadcast into xy. */
			if (pmin.z == pmax.z || pmin.w == pmax.w)
				outside = 1;
		}
		else
		{
			if (pmin.x == pmax.x || pmin.y == pmax.y)
				outside = 1;
		}
	}

	if (is_tri)
	{
		if ((v0.x == v1.x && v0.y == v1.y) || (v1.x == v2.x && v1.y == v2.y) ||
		    (v0.x == v2.x && v0.y == v2.y))
			outside = 1;
	}

	return outside ? 1u : 0u;
}

static gs_vec4i FromRef(RefV v)
{
	return gs_v4i_set4(v.x, v.y, v.z, v.w);
}

static unsigned long g_cull_checked;
static unsigned long g_cull_failed;

template <u32 prim>
static void RunCull(RefV v0, RefV v1, RefV v2, RefV cmin, RefV cmax, int nativeres)
{
	gs_vec4i pmin, pmax;
	u32 got, want;

	gs_vk_prim_bbox(prim, FromRef(v0), FromRef(v1), FromRef(v2), &pmin, &pmax);
	got = gs_vk_cull_skip(prim, pmin, pmax, FromRef(v0), FromRef(v1), FromRef(v2),
		FromRef(cmin), FromRef(cmax), nativeres != 0) ? 1u : 0u;
	want = RefCullSkip(prim, v0, v1, v2, cmin, cmax, nativeres);

	g_cull_checked++;
	if (got != want)
	{
		g_cull_failed++;
		if (g_cull_failed <= 5)
			printf("CULL MISMATCH prim=%u native=%d kernel=%u model=%u\n"
			       "  v0=(%d,%d,%d,%d) v1=(%d,%d,%d,%d) v2=(%d,%d,%d,%d)\n"
			       "  cull=(%d,%d,%d,%d)..(%d,%d,%d,%d)\n",
			       prim, nativeres, got, want,
			       v0.x, v0.y, v0.z, v0.w, v1.x, v1.y, v1.z, v1.w, v2.x, v2.y, v2.z, v2.w,
			       cmin.x, cmin.y, cmin.z, cmin.w, cmax.x, cmax.y, cmax.z, cmax.w);
	}
}

static RefV RandV(int range)
{
	RefV v;
	v.x = (int)(Rand32() % (u32)range) - range / 2;
	v.y = (int)(Rand32() % (u32)range) - range / 2;
	v.z = (int)(Rand32() % (u32)range) - range / 2;
	v.w = (int)(Rand32() % (u32)range) - range / 2;
	return v;
}

static void CullSweep(void)
{
	RefV cmin, cmax, v0, v1, v2;
	long n;
	int native;

	cmin.x = 0; cmin.y = 0; cmin.z = 0; cmin.w = 0;
	cmax.x = 640; cmax.y = 448; cmax.z = 640; cmax.w = 448;

	/* Directed: fully inside, fully outside each edge, exactly on the bounds,
	 * degenerate extents, and coincident vertices. */
	v0.x = 10; v0.y = 10; v0.z = 10; v0.w = 10;
	v1.x = 100; v1.y = 100; v1.z = 100; v1.w = 100;
	v2.x = 50; v2.y = 80; v2.z = 50; v2.w = 80;
	RunCull<GS_TRIANGLELIST>(v0, v1, v2, cmin, cmax, 1);
	RunCull<GS_TRIANGLELIST>(v0, v1, v2, cmin, cmax, 0);
	RunCull<GS_SPRITE>(v0, v1, v2, cmin, cmax, 1);
	RunCull<GS_POINTLIST>(v0, v1, v2, cmin, cmax, 1);
	RunCull<GS_LINELIST>(v0, v1, v2, cmin, cmax, 1);

	/* coincident v0/v1 -> triangles must reject */
	RunCull<GS_TRIANGLELIST>(v0, v0, v2, cmin, cmax, 1);
	RunCull<GS_TRIANGLESTRIP>(v0, v2, v0, cmin, cmax, 0);
	/* degenerate extent */
	RunCull<GS_SPRITE>(v0, v0, v2, cmin, cmax, 1);
	RunCull<GS_SPRITE>(v0, v0, v2, cmin, cmax, 0);

	for (n = 0; n < 300000; n++)
	{
		v0 = RandV(1024); v1 = RandV(1024); v2 = RandV(1024);
		native = (int)(Rand32() & 1);

		/* Occasionally collapse a coordinate so the degenerate and
		 * coincident paths are hit often, not just by luck. */
		if ((n & 3) == 0) v1 = v0;
		if ((n & 7) == 0) v2 = v1;
		if ((n & 15) == 0) { v1.x = v0.x; v2.x = v0.x; }

		RunCull<GS_POINTLIST>(v0, v1, v2, cmin, cmax, native);
		RunCull<GS_LINELIST>(v0, v1, v2, cmin, cmax, native);
		RunCull<GS_LINESTRIP>(v0, v1, v2, cmin, cmax, native);
		RunCull<GS_SPRITE>(v0, v1, v2, cmin, cmax, native);
		RunCull<GS_TRIANGLELIST>(v0, v1, v2, cmin, cmax, native);
		RunCull<GS_TRIANGLESTRIP>(v0, v1, v2, cmin, cmax, native);
		RunCull<GS_TRIANGLEFAN>(v0, v1, v2, cmin, cmax, native);
	}
}

int main(void)
{
	alignas(16) u8 rec[48];
	u64 uvfog;
	u32 uv;
	int i;
	long n;

	/* ---- directed edges ---------------------------------------------- */

	/* Q == +0.0 must become FLT_MIN; Q == -0.0 must pass through unchanged.
	 * The distinction is why the kernel compares Q as an integer. */
	memset(rec, 0, sizeof(rec));
	SetDword(rec, 2, 0x00000000u);
	RunXYZF2(rec, 0);
	SetDword(rec, 2, 0x80000000u);
	RunXYZF2(rec, 0);

	/* Sub-texel boundaries: XY carry 4 fractional bits, Z and F are shifted
	 * down by 4, so the low nibbles must not survive into the result. */
	memset(rec, 0, sizeof(rec));
	SetDword(rec, 8, 0x0000ffffu);
	SetDword(rec, 9, 0x0000ffffu);
	SetDword(rec, 10, 0xffffffffu);
	SetDword(rec, 11, 0xffffffffu);
	RunXYZF2(rec, 0xdeadbeefu);
	RunXYZ2(rec, 0x1122334455667788ull);

	/* All-ones and alternating patterns through every field. */
	memset(rec, 0xff, sizeof(rec));
	RunXYZF2(rec, 0xffffffffu);
	RunXYZ2(rec, 0xffffffffffffffffull);
	memset(rec, 0xaa, sizeof(rec));
	RunXYZF2(rec, 0x55555555u);
	RunXYZ2(rec, 0x5555555555555555ull);

	/* RGBA is gathered from the low byte of four separate dwords: prove the
	 * other 24 bits of each are ignored. */
	memset(rec, 0, sizeof(rec));
	SetDword(rec, 4, 0xffffff01u);
	SetDword(rec, 5, 0xffffff02u);
	SetDword(rec, 6, 0xffffff03u);
	SetDword(rec, 7, 0xffffff04u);
	RunXYZF2(rec, 0);

	/* ---- randomized sweep -------------------------------------------- */

	for (n = 0; n < 2000000; n++)
	{
		for (i = 0; i < 12; i++)
			SetDword(rec, i, Rand32());

		/* Bias Q toward the zero cases the fixup exists for. */
		if ((n & 7) == 0)
			SetDword(rec, 2, (n & 8) ? 0x80000000u : 0x00000000u);

		uv = Rand32();
		uvfog = ((u64)Rand32() << 32) | Rand32();

		RunXYZF2(rec, uv);
		RunXYZ2(rec, uvfog);
	}

	CullSweep();

	printf("gs_vertex_oracle: parse %lu comparisons, %lu mismatches\n", g_checked, g_failed);
	printf("gs_vertex_oracle: cull  %lu comparisons, %lu mismatches\n", g_cull_checked, g_cull_failed);
	return (g_failed || g_cull_failed) ? 1 : 0;
}
