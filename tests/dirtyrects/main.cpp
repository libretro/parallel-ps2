/* A target's dirty rectangles (GSDirtyRectList, GS/Renderers/Common/
 * GSDirtyRect.h): the first sixteen live in the list and allocate
 * nothing; past them they move to the heap in order; erase, copy and
 * take keep every rectangle and its order whichever storage either list
 * is in, and the heap goes with the list (the sanitizers see a leak or a
 * double free). */
#include <cstdio>

#include "GS/Renderers/Common/GSDirtyRect.h"

static int fails;

#define CHECK(c) \
	do \
	{ \
		if (!(c)) \
		{ \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			fails++; \
		} \
	} while (0)

static GSDirtyRect rect(int i)
{
	RGBAMask m;
	m._u32 = (u32)i * 3u;
	return GSDirtyRect(GSVector4i(i, i + 1, i + 2, i + 3), (u32)i & 0x3f, (u32)i + 1, m);
}

static bool holds(const GSDirtyRectList& l, int first, int n)
{
	int i;
	if ((int)l.size() != n)
		return false;
	for (i = 0; i < n; i++)
	{
		const GSDirtyRect& r = l[i];
		const GSDirtyRect w = rect(first + i);
		if (r.r.x != w.r.x || r.r.w != w.r.w || r.psm != w.psm || r.bw != w.bw || r.rgba._u32 != w.rgba._u32)
			return false;
	}
	return true;
}

/* the rectangles inside the list object: no heap */
static bool in_place(const GSDirtyRectList& l)
{
	const unsigned char* p = reinterpret_cast<const unsigned char*>(l.begin());
	const unsigned char* o = reinterpret_cast<const unsigned char*>(&l);
	return p >= o && p < o + sizeof(l);
}

static void fill(GSDirtyRectList& l, int first, int n)
{
	int i;
	l.clear();
	for (i = 0; i < n; i++)
		l.push_back(rect(first + i));
}

int main()
{
	int i;

	{
		GSDirtyRectList l;
		CHECK(l.empty() && in_place(l));
		fill(l, 0, 16);
		CHECK(holds(l, 0, 16) && in_place(l));
		l.push_back(rect(16));
		CHECK(holds(l, 0, 17) && !in_place(l));
		for (i = 17; i < 100; i++)
			l.push_back(rect(i));
		CHECK(holds(l, 0, 100));
		l.erase(0);
		CHECK(holds(l, 1, 99));
		l.erase(98);
		CHECK(holds(l, 1, 98));
		l.clear();
		CHECK(l.empty());
	}

	/* erase in the middle, in place */
	{
		GSDirtyRectList l;
		fill(l, 0, 5);
		l.erase(2);
		CHECK(l.size() == 4 && l[1].r.x == 1 && l[2].r.x == 3 && in_place(l));
	}

	/* copies, each storage into each */
	{
		const int sizes[2] = {5, 40};
		int a, b;
		for (a = 0; a < 2; a++)
			for (b = 0; b < 2; b++)
			{
				GSDirtyRectList src, dst;
				fill(src, 10, sizes[a]);
				fill(dst, 500, sizes[b]);
				dst = src;
				CHECK(holds(dst, 10, sizes[a]) && holds(src, 10, sizes[a]));
				CHECK(dst.begin() != src.begin());
				GSDirtyRectList cl(src);
				CHECK(holds(cl, 10, sizes[a]) && cl.begin() != src.begin());
				CHECK(in_place(cl) == (sizes[a] <= 16));
			}
	}

	/* take, each storage from each: all of the other's, the other empty
	 * and still usable */
	{
		const int sizes[2] = {5, 40};
		int a, b;
		for (a = 0; a < 2; a++)
			for (b = 0; b < 2; b++)
			{
				GSDirtyRectList dst, src;
				fill(dst, 500, sizes[a]);
				fill(src, 20, sizes[b]);
				dst.take(src);
				CHECK(holds(dst, 20, sizes[b]));
				CHECK(src.empty());
				CHECK(dst.begin() != src.begin());
				fill(src, 70, 30);
				CHECK(holds(src, 70, 30) && holds(dst, 20, sizes[b]));
			}
	}

	if (fails)
	{
		std::printf("dirtyrects: %d failed\n", fails);
		return 1;
	}
	std::printf("dirtyrects: ok\n");
	return 0;
}
