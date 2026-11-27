/* Micro-benchmark for the GS vertex front-end kernels.
 *
 * The kernels are isolated in GS/GSVertexKick.h, so they can be timed on their
 * own instead of guessing from a whole-emulator frame rate. This exists to
 * answer one question before any of the yaps2 GV optimizations are ported:
 * does the vector accept/cull decision actually cost anything on this host, or
 * is its replacement only a win on the in-order aarch64 cores it was written
 * for?
 *
 * Alongside the shipped kernel it times a scalar-outcode reformulation of the
 * same decision (the shape GV-3 uses), so the two can be compared directly on
 * whatever machine this is built on. The scalar variant is checked against the
 * shipped kernel first -- a faster wrong answer is not interesting.
 *
 * Build: sh tests/gs/build.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "GS/GSVertexKick.h"

#define PRIM_COUNT 4096
#define ITERATIONS 20000

typedef struct
{
	gs_vec4i v0, v1, v2;
} PrimSet;

static u64 g_seed = 0x9e3779b97f4a7c15ull;

static u32 Rand32(void)
{
	g_seed ^= g_seed << 13;
	g_seed ^= g_seed >> 7;
	g_seed ^= g_seed << 17;
	return (u32)(g_seed >> 16);
}

static double NowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Scalar-outcode form of the same decision, in the shape GV-3 argues for: the
 * bbox is the min/max of the vertices, so "bbox outside an edge" is "every
 * vertex outside that edge", which is an AND of per-vertex outcodes and never
 * builds the bbox or crosses a vector-to-scalar boundary for the verdict. */
static __forceinline u32 ScalarOutcode(int x, int y, int cx0, int cy0, int cx1, int cy1)
{
	u32 code = 0;
	if (x < cx0) code |= 1;
	if (x > cx1) code |= 2;
	if (y < cy0) code |= 4;
	if (y > cy1) code |= 8;
	return code;
}

static __forceinline u32 CullSkipScalarTri(const int* v0, const int* v1, const int* v2,
	int cx0, int cy0, int cx1, int cy1, int nativeres)
{
	u32 c0, c1, c2;
	int minz, maxz, minw, maxw;

	c0 = ScalarOutcode(v0[0], v0[1], cx0, cy0, cx1, cy1);
	c1 = ScalarOutcode(v1[0], v1[1], cx0, cy0, cx1, cy1);
	c2 = ScalarOutcode(v2[0], v2[1], cx0, cy0, cx1, cy1);
	if ((c0 & c1 & c2) != 0)
		return 1;

	/* coincident vertices */
	if ((v0[0] == v1[0] && v0[1] == v1[1]) || (v1[0] == v2[0] && v1[1] == v2[1]) ||
	    (v0[0] == v2[0] && v0[1] == v2[1]))
		return 1;

	if (nativeres)
	{
		minz = v0[2]; maxz = v0[2];
		if (v1[2] < minz) minz = v1[2];
		if (v2[2] < minz) minz = v2[2];
		if (v1[2] > maxz) maxz = v1[2];
		if (v2[2] > maxz) maxz = v2[2];
		minw = v0[3]; maxw = v0[3];
		if (v1[3] < minw) minw = v1[3];
		if (v2[3] < minw) minw = v2[3];
		if (v1[3] > maxw) maxw = v1[3];
		if (v2[3] > maxw) maxw = v2[3];
		if (minz == maxz || minw == maxw)
			return 1;
	}
	else
	{
		int minx = v0[0], maxx = v0[0], miny = v0[1], maxy = v0[1];
		if (v1[0] < minx) minx = v1[0];
		if (v2[0] < minx) minx = v2[0];
		if (v1[0] > maxx) maxx = v1[0];
		if (v2[0] > maxx) maxx = v2[0];
		if (v1[1] < miny) miny = v1[1];
		if (v2[1] < miny) miny = v2[1];
		if (v1[1] > maxy) maxy = v1[1];
		if (v2[1] > maxy) maxy = v2[1];
		if (minx == maxx || miny == maxy)
			return 1;
	}

	return 0;
}

int main(void)
{
	static PrimSet prims[PRIM_COUNT];
	static int scalar_v[PRIM_COUNT][3][4];
	alignas(16) u8 rec[48];
	gs_vec4i cull_min, cull_max;
	double t0, t1, vec_time, scl_time, parse_time;
	unsigned long accum;
	int i, k;
	long it;
	int disagreements;

	cull_min = gs_v4i_set4(0, 0, 0, 0);
	cull_max = gs_v4i_set4(640, 448, 640, 448);

	/* A mix that resembles a real draw: most prims inside, some off-screen,
	 * some degenerate. */
	for (i = 0; i < PRIM_COUNT; i++)
	{
		int c[3][4];
		int j, l;
		for (j = 0; j < 3; j++)
		{
			for (l = 0; l < 4; l++)
				c[j][l] = (int)(Rand32() % 900) - 100;
		}
		if ((i & 7) == 0)
			for (l = 0; l < 4; l++) c[1][l] = c[0][l];   /* degenerate/coincident */
		if ((i & 15) == 0)
			for (j = 0; j < 3; j++) c[j][0] += 4000;     /* wholly off-screen */

		prims[i].v0 = gs_v4i_set4(c[0][0], c[0][1], c[0][2], c[0][3]);
		prims[i].v1 = gs_v4i_set4(c[1][0], c[1][1], c[1][2], c[1][3]);
		prims[i].v2 = gs_v4i_set4(c[2][0], c[2][1], c[2][2], c[2][3]);
		memcpy(scalar_v[i], c, sizeof(c));
	}

	/* Correctness first: the scalar form must agree with the shipped kernel. */
	disagreements = 0;
	for (i = 0; i < PRIM_COUNT; i++)
	{
		gs_vec4i pmin, pmax;
		u32 a, b;
		for (k = 0; k < 2; k++)
		{
			gs_vk_prim_bbox(GS_TRIANGLELIST, prims[i].v0, prims[i].v1, prims[i].v2, &pmin, &pmax);
			a = gs_vk_cull_skip(GS_TRIANGLELIST, pmin, pmax, prims[i].v0, prims[i].v1,
				prims[i].v2, cull_min, cull_max, k) ? 1u : 0u;
			b = CullSkipScalarTri(scalar_v[i][0], scalar_v[i][1], scalar_v[i][2], 0, 0, 640, 448, k);
			if (a != b)
				disagreements++;
		}
	}
	printf("cull variants disagree on %d of %d cases\n", disagreements, PRIM_COUNT * 2);

	/* Timed: shipped vector kernel. */
	accum = 0;
	t0 = NowSeconds();
	for (it = 0; it < ITERATIONS; it++)
	{
		for (i = 0; i < PRIM_COUNT; i++)
		{
			gs_vec4i pmin, pmax;
			gs_vk_prim_bbox(GS_TRIANGLELIST, prims[i].v0, prims[i].v1, prims[i].v2, &pmin, &pmax);
			accum += gs_vk_cull_skip(GS_TRIANGLELIST, pmin, pmax, prims[i].v0, prims[i].v1,
				prims[i].v2, cull_min, cull_max, 1);
		}
	}
	t1 = NowSeconds();
	vec_time = t1 - t0;

	/* Timed: scalar outcode form. */
	t0 = NowSeconds();
	for (it = 0; it < ITERATIONS; it++)
	{
		for (i = 0; i < PRIM_COUNT; i++)
			accum += CullSkipScalarTri(scalar_v[i][0], scalar_v[i][1], scalar_v[i][2], 0, 0, 640, 448, 1);
	}
	t1 = NowSeconds();
	scl_time = t1 - t0;

	/* Timed: the packed parse, for scale. */
	memset(rec, 0x5a, sizeof(rec));
	t0 = NowSeconds();
	for (it = 0; it < ITERATIONS; it++)
	{
		for (i = 0; i < PRIM_COUNT; i++)
		{
			alignas(16) gs_vec4i out[2];
			gs_vk_parse_stqrgbaxyzf2((const GIFPackedReg*)rec, (u32)i, out);
			accum += (unsigned long)((const u32*)out)[0];
		}
	}
	t1 = NowSeconds();
	parse_time = t1 - t0;

	{
		const double prims_total = (double)PRIM_COUNT * (double)ITERATIONS;
		printf("cull  vector  %8.3f s   %6.2f ns/prim\n", vec_time, vec_time / prims_total * 1e9);
		printf("cull  scalar  %8.3f s   %6.2f ns/prim   (%.2fx vs vector)\n",
		       scl_time, scl_time / prims_total * 1e9, vec_time / scl_time);
		printf("parse vector  %8.3f s   %6.2f ns/vertex\n", parse_time, parse_time / prims_total * 1e9);
	}

	/* keep the accumulator observable so nothing is optimized away */
	if (accum == 0x123456789abcdefull)
		printf("unreachable %lu\n", accum);
	return disagreements ? 1 : 0;
}
