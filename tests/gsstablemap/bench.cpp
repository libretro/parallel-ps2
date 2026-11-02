/* The texture cache's hash cache map, timed both ways: the
 * std::unordered_map it was (GSNodeAllocator's node pool, gs_key_hash)
 * and GSStableMap. 40-byte keys, 24-byte values, the map at a few sizes
 * up to the cache's 800 entries. Per operation:
 *  - find, hit: what every draw with a cached texture does, up to three
 *    times;
 *  - find, miss: a texture not cached yet;
 *  - the per-frame walk, aging each entry (AgeHashCache);
 *  - insert then erase: a texture made and purged.
 * Prints ns per operation for each, the best of five runs, and the
 * ratio. */

#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <unordered_map>
#include <vector>

#include "GS/Renderers/Common/GSKeyHash.h"
#include "GS/Renderers/Common/GSStableMap.h"

struct Key
{
	unsigned long long TEX0Hash, CLUTHash, TEX0, TEXA;
	unsigned region_width, region_height;
	bool operator==(const Key& e) const { return std::memcmp(this, &e, sizeof(*this)) == 0; }
};
struct KeyHash
{
	unsigned long long operator()(const Key& k) const { return gs_key_hash(&k, sizeof(k)); }
};
struct Entry
{
	void* texture;
	unsigned refcount;
	unsigned short age;
	unsigned char alpha[2];
	bool valid, replacement;
};

/* GSTextureCache.h's, as it is there. */
template <typename T>
class NodeAllocator
{
	static constexpr bool POOLED = !GS_OBJECT_POOL_SANITIZED && alignof(T) <= GS_OBJECT_POOL_ALIGN;

public:
	using value_type = T;
	NodeAllocator() noexcept = default;
	template <typename U>
	NodeAllocator(const NodeAllocator<U>&) noexcept {}
	T* allocate(std::size_t n)
	{
		if (POOLED && n == 1)
			return static_cast<T*>(gs_node_pool_take(&s_pool));
		return static_cast<T*>(::operator new(n * sizeof(T)));
	}
	void deallocate(T* p, std::size_t n) noexcept
	{
		if (POOLED && n == 1)
			gs_node_pool_give(&s_pool, p);
		else
			::operator delete(p);
	}
	template <typename U>
	bool operator==(const NodeAllocator<U>&) const noexcept { return true; }
	template <typename U>
	bool operator!=(const NodeAllocator<U>&) const noexcept { return false; }

private:
	static inline gs_node_pool_t s_pool = GS_NODE_POOL_INIT(sizeof(T), alignof(T));
};

using StdMap = std::unordered_map<Key, Entry, KeyHash, std::equal_to<Key>, NodeAllocator<std::pair<const Key, Entry>>>;

static gs_node_pool_t s_nodes = GS_NODE_POOL_INIT(GS_STABLE_MAP_NODE_SIZE(sizeof(Key), sizeof(Entry)), 16);

static Key make_key(unsigned i)
{
	Key k;
	std::memset(&k, 0, sizeof(k));
	k.TEX0Hash = 0x9e3779b97f4a7c15ull * (i + 1);
	k.CLUTHash = (i & 3) ? 0 : 0xabcdef12345ull * i;
	k.TEX0 = (unsigned long long)(i & 0x3ff) << 20;
	k.region_width = i & 7;
	return k;
}

static volatile unsigned long long s_sink;

/* The best of five: a run the scheduler interrupted says nothing. */
#define BEST(out, ops, body) \
	do \
	{ \
		out = 1e30; \
		for (int run_ = 0; run_ < 5; run_++) \
		{ \
			auto t_ = std::chrono::steady_clock::now(); \
			body; \
			const double ns_ = ns_since(t_, ops); \
			if (ns_ < out) \
				out = ns_; \
		} \
	} while (0)

static double ns_since(std::chrono::steady_clock::time_point t, double ops)
{
	return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t).count() / ops;
}

int main()
{
	const unsigned sizes[] = {32, 64, 100, 256, 400, 800};
	const unsigned rounds = 2000000;
	std::printf("%-6s %-12s %10s %10s %7s\n", "size", "op", "std ns", "stable ns", "ratio");
	for (unsigned size : sizes)
	{
		std::vector<Key> keys, misses;
		std::vector<unsigned> order(4096);
		for (unsigned i = 0; i < size; i++)
			keys.push_back(make_key(i));
		for (unsigned i = 0; i < 4096; i++)
			misses.push_back(make_key(100000 + i));
		/* Lookups in an order no prefetcher follows. */
		unsigned long long r = 88172645463325252ull;
		for (unsigned& o : order)
		{
			r ^= r << 13; r ^= r >> 7; r ^= r << 17;
			o = (unsigned)(r % size);
		}

		StdMap sm;
		gs_stable_map_t gm;
		gs_stable_map_init(&gm, sizeof(Key), sizeof(Entry), &s_nodes);
		for (const Key& k : keys)
		{
			int ins;
			sm.emplace(k, Entry{});
			gs_stable_map_insert(&gm, &k, gs_key_hash(&k, sizeof(k)), &ins);
		}

		double t_std, t_gs;
		unsigned long long acc = 0;
		/* find, hit */
		BEST(t_std, rounds, {
		for (unsigned i = 0; i < rounds; i++)
		{
			auto it = sm.find(keys[order[i & 4095]]);
			acc += ++it->second.refcount;
		}
		});
		BEST(t_gs, rounds, {
		for (unsigned i = 0; i < rounds; i++)
		{
			const Key& k = keys[order[i & 4095]];
			Entry* e = (Entry*)gs_stable_map_find(&gm, &k, sizeof(Key), gs_key_hash(&k, sizeof(k)));
			acc += ++e->refcount;
		}
		});
		std::printf("%-6u %-12s %10.2f %10.2f %7.2f\n", size, "find hit", t_std, t_gs, t_std / t_gs);

		/* find, miss */
		BEST(t_std, rounds, {
		for (unsigned i = 0; i < rounds; i++)
			acc += sm.find(misses[i & 4095]) == sm.end();
		});
		BEST(t_gs, rounds, {
		for (unsigned i = 0; i < rounds; i++)
		{
			const Key& k = misses[i & 4095];
			acc += gs_stable_map_find(&gm, &k, sizeof(Key), gs_key_hash(&k, sizeof(k))) == NULL;
		}
		});
		std::printf("%-6u %-12s %10.2f %10.2f %7.2f\n", size, "find miss", t_std, t_gs, t_std / t_gs);

		/* the per-frame walk */
		const unsigned walks = rounds / size;
		BEST(t_std, (double)walks * size, {
		for (unsigned w = 0; w < walks; w++)
			for (auto& kv : sm)
				if (kv.second.refcount == 0 || ++kv.second.age)
					acc++;
		});
		BEST(t_gs, (double)walks * size, {
		for (unsigned w = 0; w < walks; w++)
			for (size_t s = GS_STABLE_MAP_COUNT(&gm); s-- > 0;)
			{
				Entry* e = (Entry*)gs_stable_map_at(&gm, s);
				if (e->refcount == 0 || ++e->age)
					acc++;
			}
		});
		std::printf("%-6u %-12s %10.2f %10.2f %7.2f\n", size, "walk/entry", t_std, t_gs, t_std / t_gs);

		/* insert then erase */
		BEST(t_std, rounds / 4, {
		for (unsigned i = 0; i < rounds / 4; i++)
		{
			const Key& k = misses[i & 4095];
			auto it = sm.emplace(k, Entry{}).first;
			sm.erase(it);
		}
		});
		BEST(t_gs, rounds / 4, {
		for (unsigned i = 0; i < rounds / 4; i++)
		{
			const Key& k = misses[i & 4095];
			int ins;
			void* v = gs_stable_map_insert(&gm, &k, gs_key_hash(&k, sizeof(k)), &ins);
			gs_stable_map_erase(&gm, v);
		}
		});
		std::printf("%-6u %-12s %10.2f %10.2f %7.2f\n", size, "insert+erase", t_std, t_gs, t_std / t_gs);

		s_sink = acc;
		gs_stable_map_free(&gm);
	}
	return 0;
}
