/* The device texture pool's scenario, run against the C pool (c89.c) and
 * against the C++ pool it replaced (oracle.cpp): fetches of a few shapes
 * and of many tiny ones, gives back, frames aged, purges, a prewarm at
 * each scale, the software device's scale, preferring new textures, and
 * allocation failures with and without the retry succeeding. Every
 * result goes into one hash, the same from both. */
#ifndef GSTEXPOOL_SCENARIO_H
#define GSTEXPOOL_SCENARIO_H

#include <stdio.h>
#include <string.h>

typedef struct pool_api
{
	/* the id fetched, or -1; its state and clear value */
	int (*fetch)(unsigned type, int w, int h, int levels, unsigned format, int clear, int prefer_reuse,
		unsigned* state, unsigned* clearbits);
	void (*put)(int id);
	void (*age)(void);
	void (*purge)(void);
	unsigned (*prewarm)(void);
	void (*set)(int software, int prefer_new);
	void (*stats)(unsigned long long mem[2], unsigned count[2]);
} pool_api;

/* gs_texture_type, gs_texture_format */
enum { T_RT = 1, T_DS = 2, T_TEX = 3 };
enum { F_COLOR = 1, F_DS = 3, F_UNORM8 = 4 };

#define SC_MAX_TEX 8192
static struct
{
	unsigned type, format;
	int w, h, levels;
	int live; /* made and not freed */
} sc_tex[SC_MAX_TEX];
static int sc_made;
static int sc_fail; /* the next this many creations fail */
static unsigned long long sc_hash = 1469598103934665603ULL;

static void sc_mix(unsigned long long v)
{
	int i;
	for (i = 0; i < 8; i++)
	{
		sc_hash ^= (v >> (i * 8)) & 0xff;
		sc_hash *= 1099511628211ULL;
	}
}

/* the device: an id, or -1 */
static int sc_create(unsigned type, int w, int h, int levels, unsigned format)
{
	int id;
	sc_mix(0xC0ULL ^ ((unsigned long long)type << 8) ^ ((unsigned long long)w << 16) ^ ((unsigned long long)h << 32)
		^ ((unsigned long long)levels << 48) ^ ((unsigned long long)format << 56));
	if (sc_fail > 0)
	{
		sc_fail--;
		sc_mix(0xFA11);
		return -1;
	}
	if (sc_made >= SC_MAX_TEX)
		return -1;
	id = sc_made++;
	sc_tex[id].type = type;
	sc_tex[id].format = format;
	sc_tex[id].w = w;
	sc_tex[id].h = h;
	sc_tex[id].levels = levels;
	sc_tex[id].live = 1;
	return id;
}

static int sc_double_free;
static void sc_freed(int id)
{
	if (!sc_tex[id].live)
		sc_double_free++;
	sc_tex[id].live = 0;
	sc_mix(0xF4EEULL ^ ((unsigned long long)id << 16));
}

static unsigned sc_rng = 12345u;
static unsigned sc_rand(unsigned n)
{
	sc_rng = sc_rng * 1103515245u + 12345u;
	return ((sc_rng >> 8) & 0xffffffu) % n;
}

static void sc_set_upscale(float f);

static void sc_stats(const pool_api* a)
{
	unsigned long long mem[2];
	unsigned count[2];
	a->stats(mem, count);
	sc_mix(mem[0]);
	sc_mix(mem[1]);
	sc_mix(((unsigned long long)count[0] << 32) | count[1]);
}

/* What is out, to give back */
static int sc_out[4096];
static int sc_nout;

static void sc_fetch(const pool_api* a, unsigned type, int w, int h, int levels, unsigned format, int clear,
	int prefer_reuse)
{
	unsigned state = 0, clearbits = 0;
	int id = a->fetch(type, w, h, levels, format, clear, prefer_reuse, &state, &clearbits);
	sc_mix(0xFE7CULL ^ ((unsigned long long)(unsigned)id << 16) ^ ((unsigned long long)state << 48));
	sc_mix(clearbits);
	if (id >= 0 && sc_nout < (int)(sizeof(sc_out) / sizeof(sc_out[0])))
		sc_out[sc_nout++] = id;
}

static void sc_put_one(const pool_api* a, int k)
{
	a->put(sc_out[k]);
	sc_out[k] = sc_out[--sc_nout];
}

static void sc_random_fetch(const pool_api* a, int tiny)
{
	static const int shapes[][2] = {
		{ 640, 448 }, { 512, 448 }, { 640, 224 }, { 1024, 1024 }, { 64, 64 }, { 256, 256 }, { 2048, 1792 },
	};
	const unsigned kind = sc_rand(3);
	const unsigned type = kind == 0 ? T_RT : kind == 1 ? T_DS : T_TEX;
	unsigned format = type == T_DS ? F_DS : (sc_rand(4) == 0 ? F_UNORM8 : F_COLOR);
	int w, h;
	int levels = 1;
	int clear, reuse;
	if (tiny)
	{
		w = 8 + 8 * (int)sc_rand(2);
		h = 8;
	}
	else
	{
		const unsigned s = sc_rand(sizeof(shapes) / sizeof(shapes[0]));
		w = shapes[s][0];
		h = shapes[s][1];
	}
	if (type == T_TEX && sc_rand(3) == 0)
		levels = 1 + (int)sc_rand(3);
	/* (one at a time: the order arguments are evaluated in is the
	 * compiler's) */
	clear = (int)sc_rand(2);
	reuse = (int)sc_rand(2);
	sc_fetch(a, type, w, h, levels, format, clear, reuse);
}

static void sc_phase(const pool_api* a, int ops, int tiny, int age_every)
{
	int i;
	for (i = 0; i < ops; i++)
	{
		const unsigned r = sc_rand(100);
		if (r < 45)
			sc_random_fetch(a, tiny);
		else if (r < 90)
		{
			if (sc_nout)
				sc_put_one(a, (int)sc_rand((unsigned)sc_nout));
		}
		else if (r < 92 && sc_rand(4) == 0)
			sc_fail = 1 + (int)sc_rand(2);
		if (age_every && i % age_every == age_every - 1)
		{
			a->age();
			sc_mix(0xA9E);
		}
		sc_stats(a);
	}
}

static void sc_give_all_back(const pool_api* a)
{
	while (sc_nout)
		sc_put_one(a, sc_nout - 1);
	sc_stats(a);
}

/* The hash; the double frees and what is still live go in too */
static unsigned long long sc_run(const pool_api* a)
{
	static const float scales[] = { 1.0f, 2.0f, 3.0f, 4.0f, 0.0f };
	unsigned s;
	int i;
	int live = 0;

	for (s = 0; s < sizeof(scales) / sizeof(scales[0]); s++)
	{
		sc_set_upscale(scales[s]);
		a->set(0, 0);
		sc_mix(a->prewarm());
		sc_stats(a);
		sc_phase(a, 3000, 0, 7);
		a->set(0, 1);
		sc_phase(a, 3000, 0, 11);
		/* Many tiny ones: over the count ceiling, and the full list
		 * handing out one used this frame */
		sc_phase(a, 2500, 1, 0);
		sc_give_all_back(a);
		for (i = 0; i < 400; i++)
			sc_fetch(a, T_TEX, 8, 8, 1, F_COLOR, 0, 0);
		sc_give_all_back(a);
		a->age();
		sc_stats(a);
		for (i = 0; i < 600; i++)
			sc_fetch(a, T_TEX, 8, 8, 1, F_COLOR, 0, (int)sc_rand(2));
		sc_give_all_back(a);
		a->age();
		a->age();
		sc_stats(a);
		/* Exactly at the count ceiling, everything given back this
		 * frame and a new one preferred: the full list hands one out;
		 * one short of it, one is made */
		a->purge();
		a->set(0, 1);
		for (i = 0; i < 299; i++)
			sc_fetch(a, T_TEX, 8, 8, 1, F_COLOR, 0, 0);
		sc_give_all_back(a);
		sc_fetch(a, T_TEX, 8, 8, 1, F_COLOR, 0, 0);
		sc_give_all_back(a);
		a->purge();
		for (i = 0; i < 300; i++)
			sc_fetch(a, T_TEX, 8, 8, 1, F_COLOR, 0, 0);
		sc_give_all_back(a);
		sc_fetch(a, T_TEX, 8, 8, 1, F_COLOR, 0, 0);
		sc_give_all_back(a);
		/* The software device: at most 2x */
		a->set(1, (int)(s & 1));
		sc_mix(a->prewarm());
		sc_phase(a, 1500, 0, 5);
		sc_give_all_back(a);
		a->age();
		sc_stats(a);
		/* A failure the retry rides out, then one it does not */
		sc_fail = 1;
		sc_fetch(a, T_RT, 640, 448, 1, F_COLOR, 1, 1);
		sc_stats(a);
		sc_fail = 2;
		sc_fetch(a, T_DS, 640, 448, 1, F_DS, 1, 1);
		sc_stats(a);
		sc_give_all_back(a);
		a->purge();
		sc_stats(a);
	}
	for (i = 0; i < sc_made; i++)
		live += sc_tex[i].live;
	sc_mix((unsigned long long)live);
	sc_mix((unsigned long long)sc_double_free);
	sc_mix((unsigned long long)sc_made);
	return sc_hash;
}

#endif
