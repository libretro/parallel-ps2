/* The device texture pool (GS/Renderers/Common/GSTexturePool.c) from C89,
 * on a pretend device: the scenario's hash against the one the C++ pool
 * gave (argv[1]), and what the C pool adds - lists reserved at init, so
 * a frame's textures given back allocate nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/Renderers/Common/GSTexturePool.h"
#include "scenario.h"

static void quiet_log(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}
retro_log_printf_t log_cb = quiet_log;
const char* GSTextureCacheMemoryUsage(void)
{
	return "-";
}

static gs_tex_pool pool;
static void sc_set_upscale(float f)
{
	pool.upscale = f;
}

typedef struct ftex
{
	gs_texture base;
	int id;
} ftex;

static void ftex_free(gs_texture* t)
{
	sc_freed(((ftex*)t)->id);
	free(t);
}
static const gs_texture_ops ftex_ops = { ftex_free, NULL, NULL, NULL, NULL };

static gs_texture* ftex_create(void* device, unsigned type, int w, int h, int levels, unsigned format)
{
	ftex* t;
	const int id = sc_create(type, w, h, levels, format);
	(void)device;
	if (id < 0)
		return NULL;
	t = (ftex*)calloc(1, sizeof(*t));
	t->base.m_ops = &ftex_ops;
	t->base.m_size.x = w;
	t->base.m_size.y = h;
	t->base.m_mipmap_levels = levels;
	t->base.m_type = (u8)type;
	t->base.m_format = (u8)format;
	t->base.m_state = GS_TEXSTATE_DIRTY;
	t->base.m_clear_value.color = 0xdeadbeefu;
	t->id = id;
	return &t->base;
}

static ftex* by_id[SC_MAX_TEX];

static int c_fetch(unsigned type, int w, int h, int levels, unsigned format, int clear, int prefer_reuse,
	unsigned* state, unsigned* clearbits)
{
	ftex* t = (ftex*)gs_tex_pool_fetch(&pool, type, w, h, levels, format, clear != 0, prefer_reuse != 0);
	if (!t)
		return -1;
	*state = t->base.m_state;
	*clearbits = t->base.m_clear_value.color;
	/* the next fetch's state is the pool's doing, not a leftover */
	t->base.m_state = GS_TEXSTATE_DIRTY;
	t->base.m_clear_value.color = 0xdeadbeefu;
	by_id[t->id] = t;
	return t->id;
}
static void c_put(int id)
{
	gs_tex_pool_put(&pool, &by_id[id]->base);
}
static void c_age(void)
{
	gs_tex_pool_age(&pool);
}
static void c_purge(void)
{
	gs_tex_pool_purge(&pool);
}
static unsigned c_prewarm(void)
{
	return gs_tex_pool_prewarm(&pool);
}
static void c_set(int software, int prefer_new)
{
	pool.software = software;
	pool.prefer_new = prefer_new;
}
static void c_stats(unsigned long long mem[2], unsigned count[2])
{
	mem[0] = pool.mem_usage[0];
	mem[1] = pool.mem_usage[1];
	count[0] = pool.list[0].count;
	count[1] = pool.list[1].count;
}

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

int main(int argc, char** argv)
{
	static const pool_api api = { c_fetch, c_put, c_age, c_purge, c_prewarm, c_set, c_stats };
	unsigned long long h;
	unsigned char* bufs[2];
	char got[32];
	int i;

	CHECK(gs_tex_pool_init(&pool, ftex_create, NULL), "init reserves both lists");
	CHECK(pool.list[0].capacity >= GS_TEX_POOL_RESERVE && pool.list[1].capacity >= GS_TEX_POOL_RESERVE,
		"reserved for more than the ceilings");

	/* A frame giving back more than either ceiling: no list grows */
	pool.upscale = 1.0f;
	bufs[0] = pool.list[0].buf;
	bufs[1] = pool.list[1].buf;
	for (i = 0; i < 1000; i++)
		sc_fetch(&api, T_TEX, 8, 8, 1, F_COLOR, 0, 0);
	for (i = 0; i < 1000; i++)
		sc_fetch(&api, T_RT, 8, 8, 1, F_COLOR, 0, 0);
	sc_give_all_back(&api);
	CHECK(pool.list[0].buf == bufs[0] && pool.list[1].buf == bufs[1], "1000 each given back: nothing allocated");
	CHECK(pool.list[0].count == 1000 && pool.list[1].count == 1000, "all of them held");
	gs_tex_pool_age(&pool);
	CHECK(pool.list[0].count == GS_TEX_POOL_MAX_TEXTURES && pool.list[1].count == GS_TEX_POOL_MAX_TARGETS,
		"aged down to the ceilings");
	gs_tex_pool_purge(&pool);
	CHECK(pool.list[0].buf == bufs[0] && pool.list[0].count == 0 && pool.mem_usage[0] == 0
		&& pool.mem_usage[1] == 0, "a purge empties, the reservation stays");

	/* A target fetched cleared, invalidated, and a texture untouched */
	{
		gs_texture* t = gs_tex_pool_fetch(&pool, T_RT, 16, 16, 1, F_COLOR, true, true);
		CHECK(t && t->m_state == GS_TEXSTATE_CLEARED && t->m_clear_value.color == 0, "cleared target: to 0");
		gs_tex_pool_put(&pool, t);
		t = gs_tex_pool_fetch(&pool, T_RT, 16, 16, 1, F_COLOR, false, true);
		CHECK(t && t->m_state == GS_TEXSTATE_INVALIDATED, "uncleared target: invalidated");
		gs_tex_pool_put(&pool, t);
		t = gs_tex_pool_fetch(&pool, T_DS, 16, 16, 1, F_DS, true, true);
		CHECK(t && t->m_state == GS_TEXSTATE_CLEARED && t->m_clear_value.depth == 0.0f, "cleared depth: to 0.0");
		gs_tex_pool_put(&pool, t);
		gs_tex_pool_purge(&pool);
	}

	/* The scenario, from the start */
	memset(sc_tex, 0, sizeof(sc_tex));
	sc_made = 0;
	sc_fail = 0;
	sc_nout = 0;
	sc_rng = 12345u;
	sc_hash = 1469598103934665603ULL;
	sc_double_free = 0;
	pool.frame = 0;
	h = sc_run(&api);
	gs_tex_pool_deinit(&pool);
	CHECK(sc_double_free == 0, "nothing freed twice");
	sprintf(got, "%08lx%08lx", (unsigned long)(h >> 32), (unsigned long)(h & 0xffffffffu));
	if (argc > 1)
		CHECK(strcmp(got, argv[1]) == 0, "the C++ pool's hash");
	printf("gstexpool: %s, %d failure(s)\n", got, fails);
	return fails != 0;
}
