/* The hash cache across a GS reset or a state load.
 *
 * GSRendererHW::Reset drops every source and target. It used to drop the
 * hash cache too, so after a state load - every frame under runahead -
 * each texture the next frame used was hashed, missed, allocated and
 * uploaded again, even where the state brought back the very bytes the
 * cache held. An entry is found by a hash of the texture's bytes and the
 * registers that read them (HashCacheKey::Create), so no reset or load
 * can make one wrong: a texture whose bytes changed hashes to another key.
 *
 * Two parts:
 *
 *  - a model: frames look up textures by content, a load replaces memory
 *    (some textures unchanged, some changed) and drops the sources. With
 *    the cache kept, the frame after the load uploads only the changed
 *    textures and never serves a changed one from an old entry; every
 *    reference the dropped sources held is given back. The old rule, purge
 *    on reset, is run beside it as the negative control: it uploads all.
 *
 *  - the shape of the real code: Reset keeps the hash cache, the two ways
 *    a source is dropped wholesale give their references back, and the key
 *    is a hash of the texture's levels.
 *
 * Usage: hash_keep <GSRendererHW.cpp> <GSTextureCache.cpp> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTEX   16
#define NENTRY 64

struct entry
{
	unsigned key;
	unsigned content; /* what the texture holds, as uploaded */
	unsigned refcount;
	int      used;
};

struct cache
{
	struct entry e[NENTRY];
	unsigned     uploads;
	unsigned     stale;
};

/* The texture memory: what each texture's bytes are now. */
static unsigned s_mem[NTEX];

/* The key: a hash of the bytes. Distinct contents give distinct keys here,
 * as the 128-bit block hash does for any two the model makes. */
static unsigned key_of(unsigned content)
{
	return content * 2654435761u + 1u;
}

/* A source for texture t: a hit takes a reference, a miss uploads. */
static struct entry* lookup(struct cache* c, int t)
{
	const unsigned k = key_of(s_mem[t]);
	int i, free_slot = -1;

	for (i = 0; i < NENTRY; i++)
	{
		if (c->e[i].used && c->e[i].key == k)
		{
			if (c->e[i].content != s_mem[t])
				c->stale++;
			c->e[i].refcount++;
			return &c->e[i];
		}
		if (!c->e[i].used && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0)
		return NULL;
	c->uploads++;
	c->e[free_slot].used     = 1;
	c->e[free_slot].key      = k;
	c->e[free_slot].content  = s_mem[t];
	c->e[free_slot].refcount = 1;
	return &c->e[free_slot];
}

/* RemoveAll(sources...): every live source gives its reference back. */
static void drop_sources(struct entry** src)
{
	int t;
	for (t = 0; t < NTEX; t++)
	{
		if (src[t])
			src[t]->refcount--;
		src[t] = NULL;
	}
}

static void purge(struct cache* c)
{
	memset(c->e, 0, sizeof(c->e));
}

/* One frame: a source for every texture, kept until the next drop. */
static void frame(struct cache* c, struct entry** src)
{
	int t;
	for (t = 0; t < NTEX; t++)
		if (!src[t])
			src[t] = lookup(c, t);
}

static unsigned model(int keep, unsigned changed_mask, unsigned* stale,
	unsigned* leaked)
{
	static struct cache c;
	struct entry* src[NTEX];
	unsigned before;
	int t, i;

	memset(&c, 0, sizeof(c));
	memset(src, 0, sizeof(src));
	for (t = 0; t < NTEX; t++)
		s_mem[t] = 100u + (unsigned)t;

	frame(&c, src);
	frame(&c, src);

	/* The load: memory replaced, some textures with other bytes. */
	for (t = 0; t < NTEX; t++)
		if (changed_mask & (1u << t))
			s_mem[t] = 1000u + (unsigned)t;
	drop_sources(src);
	if (!keep)
		purge(&c);

	before = c.uploads;
	frame(&c, src);
	drop_sources(src);

	*leaked = 0;
	for (i = 0; i < NENTRY; i++)
		if (c.e[i].used && c.e[i].refcount)
			(*leaked)++;
	*stale = c.stale;
	return c.uploads - before;
}

static char* read_file(const char* path)
{
	FILE* f = fopen(path, "rb");
	long n;
	char* buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (char*)malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	if (buf)
		buf[n] = '\0';
	fclose(f);
	return buf;
}

/* The body of the function whose definition starts with head. */
static const char* body(const char* src, const char* head, const char** end)
{
	const char* s = strstr(src, head);
	*end = s ? strstr(s, "\n}\n") : NULL;
	return *end ? s : NULL;
}

static const char* within(const char* s, const char* e, const char* needle)
{
	const char* p = s ? strstr(s, needle) : NULL;
	return (p && p < e) ? p : NULL;
}

static int popcount(unsigned v)
{
	int n = 0;
	while (v)
	{
		n += (int)(v & 1u);
		v >>= 1;
	}
	return n;
}

int main(int argc, char** argv)
{
	static const unsigned masks[] = { 0x0000u, 0x0001u, 0x8421u, 0x00FFu, 0xFFFFu };
	char *hw, *tc;
	const char *s, *e;
	unsigned stale, leaked, up;
	int bad = 0;
	size_t i;

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s <GSRendererHW.cpp> <GSTextureCache.cpp>\n", argv[0]);
		return 2;
	}

	for (i = 0; i < sizeof(masks) / sizeof(masks[0]); i++)
	{
		up = model(1, masks[i], &stale, &leaked);
		if (up != (unsigned)popcount(masks[i]) || stale || leaked)
		{
			printf("  kept, %04x changed: %u uploads (want %d), %u stale, %u held\n",
				masks[i], up, popcount(masks[i]), stale, leaked);
			bad++;
		}
		/* The negative control: purging uploads every texture. */
		up = model(0, masks[i], &stale, &leaked);
		if (up != NTEX)
		{
			printf("  control: purged cache uploaded %u, not %d\n", up, NTEX);
			bad++;
		}
	}
	printf("  model: a kept cache uploads only changed textures, none stale\n");

	hw = read_file(argv[1]);
	tc = read_file(argv[2]);
	if (!hw || !tc)
	{
		fprintf(stderr, "cannot read the sources\n");
		return 2;
	}

	s = body(hw, "void GSRendererHW::Reset(bool hardware_reset)", &e);
	if (!within(s, e, "RemoveAll(true, true, false)") || within(s, e, "RemoveAll(true, true, true)"))
	{
		printf("  GSRendererHW::Reset does not keep the hash cache\n");
		bad++;
	}

	s = body(tc, "void GSTextureCache::SourceMap::RemoveAll()", &e);
	if (!within(s, e, "--s->m_from_hash_cache->refcount"))
	{
		printf("  SourceMap::RemoveAll does not give hash cache references back\n");
		bad++;
	}
	s = body(tc, "void GSTextureCache::InvalidateTemporarySource()", &e);
	if (!within(s, e, "--m_temporary_source->m_from_hash_cache->refcount"))
	{
		printf("  the temporary source does not give its reference back\n");
		bad++;
	}
	s = body(tc, "GSTextureCache::HashCacheKey GSTextureCache::HashCacheKey::Create(", &e);
	if (!within(s, e, "HashTextureLevel(TEX0") || !within(s, e, "ret.TEX0Hash = FinishBlockHash("))
	{
		printf("  the hash cache key is not a hash of the texture's bytes\n");
		bad++;
	}

	free(hw);
	free(tc);
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad ? 1 : 0;
}
