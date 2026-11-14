/* Draws for the vertex trace: vertices whose ST, Q, colour, position and
 * UV take ordinary and awkward values (zero, signed zero, denormal, huge,
 * infinite, NaN), every primitive class, both provoking-vertex orders and
 * picked registers. The hash covers everything the trace leaves. Built
 * against the C trace (main.c) and the C++ class it replaced, for the
 * oracle. Every value is picked one statement at a time. */
#include <float.h>
#include <string.h>

static unsigned long long vt_hash = 1469598103934665603ull;
static void H(const void* p, size_t n)
{
	const unsigned char* c = (const unsigned char*)p;
	size_t i;
	for (i = 0; i < n; i++)
	{
		vt_hash ^= c[i];
		vt_hash *= 1099511628211ull;
	}
}

static unsigned vt_rng = 2463534242u;
static unsigned R(unsigned n)
{
	vt_rng ^= vt_rng << 13;
	vt_rng ^= vt_rng >> 17;
	vt_rng ^= vt_rng << 5;
	return vt_rng % n;
}
static u32 R32(void)
{
	u32 a = R(65536);
	u32 b = R(65536);
	return (a << 16) | b;
}

static float pick_float(void)
{
	const unsigned k = R(16);
	u32 bits;
	float f;
	switch (k)
	{
		case 0: return 0.0f;
		case 1: return -0.0f;
		case 2: bits = 0x00000001u + R(1000); memcpy(&f, &bits, 4); return f;
		case 3: return 1e35f;
		case 4: return -1e35f;
		case 5: bits = 0x7f800000u; memcpy(&f, &bits, 4); return f;
		case 6: bits = 0xff800000u; memcpy(&f, &bits, 4); return f;
		case 7: bits = 0x7fc00000u | R(1000); memcpy(&f, &bits, 4); return f;
		case 8: bits = 0xffc00000u | R(1000); memcpy(&f, &bits, 4); return f;
		case 9: return FLT_MIN;
		case 10: return (float)R(2048);
		default:
		{
			const float a = (float)R(100000) / 1000.0f;
			const unsigned s = R(2);
			return s ? -a : a;
		}
	}
}

static float pick_q(void)
{
	const unsigned k = R(5);
	if (k == 0)
		return pick_float();
	if (k == 1)
	{
		/* about the 1e30 past which the trace divides the slow way: 1e30f
		 * is above it as a double, the float below it is not */
		const unsigned w = R(3);
		u32 bits;
		float f = 1e30f;
		memcpy(&bits, &f, 4);
		bits += w - 1u;
		memcpy(&f, &bits, 4);
		return f;
	}
	return 0.0001f + (float)R(100000) / 25000.0f;
}

#define VT_MAXV 48

static void scenario(void)
{
	static unsigned char vbuf[VT_MAXV * 32 + 32];
	unsigned char* vmem = (unsigned char*)(((size_t)vbuf + 31) & ~(size_t)31);
	u16 index[VT_MAXV];
	unsigned round;

	for (round = 0; round < 6000; round++)
	{
		VT_DECL;
		GIFRegPRIM prim;
		GIFRegXYOFFSET xyoffset;
		GIFRegTEX0 tex0;
		GIFRegTEX1 tex1;
		GIFRegTEST test;
		int pvf, filtering, primclass, n, count, i, pick;
		u32 a, b;

		pvf = R(2);
		VT_INIT(pvf);
		primclass = R(4);
		n = primclass == 0 ? 1 : (primclass == 2 ? 3 : 2);
		count = n * (1 + R(VT_MAXV / n));
		a = R32(); b = R32();
		prim.U32[0] = a; prim.U32[1] = b;
		a = R32(); b = R32();
		xyoffset.U32[0] = a; xyoffset.U32[1] = b;
		a = R32(); b = R32();
		tex0.U32[0] = a; tex0.U32[1] = b;
		a = R32(); b = R32();
		tex1.U32[0] = a; tex1.U32[1] = b;
		a = R32(); b = R32();
		test.U32[0] = a; test.U32[1] = b;
		filtering = R(4);
		pick = R(3);
		for (i = 0; i < count; i++)
		{
			unsigned char* v = vmem + 32 * i;
			float s, t, q;
			u32 rgba, xy, z, uvw, fog;
			s = pick_float(); t = pick_float(); q = pick_q();
			rgba = R32(); xy = R32(); uvw = R32(); fog = R32();
			z = (pick == 0) ? 0x1234567u : (pick == 1 ? (0x1234566u | R(2)) : R32());
			memcpy(v + 0, &s, 4); memcpy(v + 4, &t, 4); memcpy(v + 8, &rgba, 4); memcpy(v + 12, &q, 4);
			memcpy(v + 16, &xy, 4); memcpy(v + 20, &z, 4); memcpy(v + 24, &uvw, 4); memcpy(v + 28, &fog, 4);
		}
		for (i = 0; i < count; i++)
		{
			const unsigned j = (primclass == 3) ? (unsigned)i : R((unsigned)count);
			index[i] = (u16)j;
		}
		VT_UPDATE(&prim, &xyoffset, &tex0, &tex1, &test, filtering, vmem, index, count, count, primclass);
		VT_HASH();
		/* and again, a draw that leaves the last one's trace behind it */
		VT_UPDATE(&prim, &xyoffset, &tex0, &tex1, &test, filtering, vmem, index, count, 0, primclass);
		VT_HASH();
	}
}
