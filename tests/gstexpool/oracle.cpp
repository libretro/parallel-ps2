/* The pool as GSDevice had it in C++ (FetchSurface, Recycle, AgePool,
 * PurgePool, PrewarmPool, PoolScale, PoolByteBudget before
 * GSTexturePool.c), on the same pretend device, for the hash the C pool
 * has to give. Kept as it was: only GSTexture became Tex and the device
 * calls the scenario's. */
#include <stdio.h>
#include <stdlib.h>

#include "GS/Renderers/Common/GSFastList.h"
#include "scenario.h"

static float UpscaleMultiplier;
static void sc_set_upscale(float f)
{
	UpscaleMultiplier = f;
}

enum class Type : unsigned { Invalid, RenderTarget, DepthStencil, Texture };
enum class Format : unsigned { Invalid, Color, ColorClip, DepthStencil, UNorm8 };
enum class State : unsigned { Dirty, Cleared, Invalidated };

struct Tex
{
	int id;
	Type type;
	Format format;
	int w, h, levels;
	State state;
	union { unsigned color; float depth; } clear;
	unsigned last_frame;
	unsigned GetMemUsage() const { return w * h * (format == Format::UNorm8 ? 1 : 4); }
	bool IsTexture() const { return type == Type::Texture; }
};

static const unsigned VM_SIZE = 4194304u;
static const unsigned MAX_POOLED_TARGETS = 300;
static const unsigned MAX_POOLED_TEXTURES = 300;
static const unsigned POOL_LIVE_SETS_TARGETS = 4;
static const unsigned POOL_LIVE_SETS_TEXTURES = 2;

struct Device
{
	FastList<Tex*> m_pool[2];
	unsigned long long m_pool_memory_usage[2] = {};
	unsigned m_frame = 0;
	bool software = false;
	bool prefer_new_textures = false;

	Tex* CreateSurface(Type type, int w, int h, int levels, Format format)
	{
		const int id = sc_create((unsigned)type, w, h, levels, (unsigned)format);
		if (id < 0)
			return nullptr;
		Tex* t = new Tex();
		t->id = id;
		t->type = type;
		t->format = format;
		t->w = w;
		t->h = h;
		t->levels = levels;
		t->state = State::Dirty;
		t->clear.color = 0xdeadbeefu;
		return t;
	}
	static void Free(Tex* t)
	{
		sc_freed(t->id);
		delete t;
	}

	float PoolScale() const
	{
		if (software)
			return UpscaleMultiplier >= 2.0f ? 2.0f : 1.0f;
		return UpscaleMultiplier > 0.0f ? UpscaleMultiplier : 1.0f;
	}

	unsigned long long PoolByteBudget(unsigned pool_idx) const
	{
		const float scale = PoolScale();
		const unsigned long long live_set = (unsigned long long)((float)VM_SIZE * scale * scale);
		const unsigned long long sets = (pool_idx == 0) ? POOL_LIVE_SETS_TEXTURES : POOL_LIVE_SETS_TARGETS;
		return live_set * sets;
	}

	Tex* FetchSurface(Type type, int width, int height, int levels, Format format, bool clear, bool prefer_reuse)
	{
		const bool prefer_new_texture = (prefer_new_textures && type == Type::Texture && !prefer_reuse);
		FastList<Tex*>& pool = m_pool[type != Type::Texture];

		Tex* t = nullptr;
		auto fallback = pool.end();

		for (auto i = pool.begin(); i != pool.end(); ++i)
		{
			t = *i;

			if (t->type == type && t->format == format && t->w == width && t->h == height && t->levels == levels)
			{
				if (!prefer_new_texture || t->last_frame != m_frame)
				{
					m_pool_memory_usage[type != Type::Texture] -= t->GetMemUsage();
					pool.erase(i);
					break;
				}
				else if (fallback == pool.end())
				{
					fallback = i;
				}
			}

			t = nullptr;
		}

		if (!t)
		{
			if (pool.size() >= ((type == Type::Texture) ? MAX_POOLED_TEXTURES : MAX_POOLED_TARGETS) &&
				fallback != pool.end())
			{
				t = *fallback;
				m_pool_memory_usage[type != Type::Texture] -= t->GetMemUsage();
				pool.erase(fallback);
			}
			else
			{
				t = CreateSurface(type, width, height, levels, format);
				if (!t)
				{
					PurgePool();
					t = CreateSurface(type, width, height, levels, format);
					if (!t)
						return nullptr;
				}
			}
		}

		switch (type)
		{
		case Type::RenderTarget:
			if (clear)
			{
				t->state = State::Cleared;
				t->clear.color = 0;
			}
			else
				t->state = State::Invalidated;
			break;
		case Type::DepthStencil:
			if (clear)
			{
				t->state = State::Cleared;
				t->clear.depth = 0.0f;
			}
			else
				t->state = State::Invalidated;
			break;
		default:
			break;
		}

		return t;
	}

	void Recycle(Tex* t)
	{
		if (!t)
			return;
		t->last_frame = m_frame;
		const unsigned idx = !t->IsTexture();
		m_pool[idx].push_front(t);
		m_pool_memory_usage[idx] += t->GetMemUsage();
	}

	void AgePool()
	{
		m_frame++;
		for (unsigned pool_idx = 0; pool_idx < 2; pool_idx++)
		{
			const unsigned max_size = (pool_idx == 0) ? MAX_POOLED_TEXTURES : MAX_POOLED_TARGETS;
			const unsigned long long max_bytes = PoolByteBudget(pool_idx);
			FastList<Tex*>& pool = m_pool[pool_idx];
			while (!pool.empty())
			{
				Tex* back = pool.back();
				if (pool.size() <= max_size && m_pool_memory_usage[pool_idx] <= max_bytes)
					break;
				if (back->last_frame == m_frame)
					break;
				m_pool_memory_usage[pool_idx] -= back->GetMemUsage();
				Free(back);
				pool.pop_back();
			}
		}
	}

	void PurgePool()
	{
		unsigned idx = 0;
		for (FastList<Tex*>& pool : m_pool)
		{
			for (Tex* t : pool)
				Free(t);
			pool.clear();
			m_pool_memory_usage[idx++] = 0;
		}
	}

	unsigned PrewarmPool()
	{
		static const struct { int w, h; } sizes[] =
		{
			{ 640, 448 }, { 512, 448 }, { 640, 224 }, { 512, 224 },
			{ 640, 512 }, { 512, 256 }, { 640, 256 }, { 512, 512 },
			{ 704, 448 }, { 704, 512 }, { 704, 224 }, { 704, 256 },
		};
		const float scale = PoolScale();
		const unsigned long long budget = PoolByteBudget(1);
		unsigned long long spent = 0;
		unsigned made = 0;
		for (unsigned pass = 0; pass < 2; pass++)
		{
			const Type type = (pass == 0) ? Type::RenderTarget : Type::DepthStencil;
			const Format format = (pass == 0) ? Format::Color : Format::DepthStencil;
			for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
			{
				const int w = (int)((float)sizes[i].w * scale);
				const int h = (int)((float)sizes[i].h * scale);
				const unsigned long long cost = (unsigned long long)w * (unsigned long long)h * 4u;
				if (spent + cost > (budget / 4u) * 3u)
					break;
				Tex* t = CreateSurface(type, w, h, 1, format);
				if (!t)
					break;
				spent += cost;
				made++;
				Recycle(t);
			}
		}
		return made;
	}
};

static Device dev;
static Tex* by_id[SC_MAX_TEX];

static int o_fetch(unsigned type, int w, int h, int levels, unsigned format, int clear, int prefer_reuse,
	unsigned* state, unsigned* clearbits)
{
	Tex* t = dev.FetchSurface((Type)type, w, h, levels, (Format)format, clear != 0, prefer_reuse != 0);
	if (!t)
		return -1;
	*state = (unsigned)t->state;
	*clearbits = t->clear.color;
	t->state = State::Dirty;
	t->clear.color = 0xdeadbeefu;
	by_id[t->id] = t;
	return t->id;
}
static void o_put(int id) { dev.Recycle(by_id[id]); }
static void o_age(void) { dev.AgePool(); }
static void o_purge(void) { dev.PurgePool(); }
static unsigned o_prewarm(void) { return dev.PrewarmPool(); }
static void o_set(int software, int prefer_new)
{
	dev.software = software != 0;
	dev.prefer_new_textures = prefer_new != 0;
}
static void o_stats(unsigned long long mem[2], unsigned count[2])
{
	mem[0] = dev.m_pool_memory_usage[0];
	mem[1] = dev.m_pool_memory_usage[1];
	count[0] = dev.m_pool[0].size();
	count[1] = dev.m_pool[1].size();
}

int main()
{
	static const pool_api api = { o_fetch, o_put, o_age, o_purge, o_prewarm, o_set, o_stats };
	const unsigned long long h = sc_run(&api);
	printf("%08lx%08lx\n", (unsigned long)(h >> 32), (unsigned long)(h & 0xffffffffu));
	return 0;
}
