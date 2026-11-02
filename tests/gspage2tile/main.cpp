/* gs_page2tile_sort/fill (GS/GSPage2Tile.c) against the containers they
 * replaced in GSLocalMemory::GetPage2TileMap, kept here as the model: for
 * texture-shaped key sets (runs of blocks over a few pages, addresses
 * repeated) and random ones, the same entries in the same order and the
 * same page starts. */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "GS/GSPage2Tile.h"

static const unsigned PAGES = 512;
static int s_fails;

/* the old code, as it was, over (page, addr) pairs */
static void model(const std::vector<unsigned>& keys, std::vector<int>& entries, std::vector<unsigned>& first)
{
	std::unordered_map<unsigned, std::unordered_set<unsigned>> tmp;
	for (unsigned k : keys)
		tmp[k >> 16].insert(k & 0xffff);
	std::vector<std::vector<std::pair<int, int>>> rows(PAGES);
	for (const auto& i : tmp)
	{
		std::unordered_map<unsigned, unsigned> m;
		for (const auto addr : i.second)
		{
			const unsigned row = addr >> 5, col = 1u << (addr & 31);
			auto k = m.find(row);
			if (k != m.end())
				k->second |= col;
			else
				m[row] = col;
		}
		for (const auto& j : m)
			rows[i.first].push_back(std::make_pair((int)j.first, (int)~j.second));
		std::sort(rows[i.first].begin(), rows[i.first].end(),
			[](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.first < b.first; });
	}
	entries.clear();
	first.assign(PAGES + 1, 0);
	unsigned at = 0;
	for (unsigned p = 0; p < PAGES; p++)
	{
		first[p] = at;
		for (const auto& e : rows[p])
		{
			entries.push_back(e.first);
			entries.push_back(e.second);
			at++;
		}
	}
	first[PAGES] = at;
}

static void check(std::vector<unsigned> keys, const char* what)
{
	std::vector<int> me;
	std::vector<unsigned> mf;
	model(keys, me, mf);

	std::vector<unsigned> scratch(keys.size());
	const size_t n = gs_page2tile_sort(keys.data(), scratch.data(), keys.size());
	std::vector<int> e(2 * n + 2);
	std::vector<unsigned> f(PAGES + 1);
	gs_page2tile_fill(keys.data(), keys.size(), PAGES, e.data(), f.data());
	e.resize(2 * n);
	if (e != me || f != mf)
	{
		std::printf("  FAIL: %s: %zu entries, %zu expected\n", what, n, me.size() / 2);
		s_fails++;
	}
}

int main()
{
	unsigned long r = 1;
	auto rnd = [&r](unsigned n) { r = r * 6364136223846793005ull + 1442695040888963407ull; return (unsigned)(r >> 33) % n; };

	check({}, "no blocks");
	check({GS_PAGE2TILE_KEY(511, 65535)}, "the last page and address");
	/* textures: blocks in row order over pages from a base, as the walk
	 * makes them, with the page wrapping */
	for (int t = 0; t < 200; t++)
	{
		std::vector<unsigned> keys;
		const unsigned bw = 1 + rnd(256), bh = 1 + rnd(256), base = rnd(PAGES);
		const unsigned per_page = 1u << rnd(6);
		for (unsigned y = 0; y < bh; y++)
			for (unsigned x = 0; x < bw; x++)
				keys.push_back(GS_PAGE2TILE_KEY((base + (y * bw + x) / per_page) % PAGES, (y << 7) + x));
		check(keys, "a texture's blocks");
	}
	/* random keys, repeated */
	for (int t = 0; t < 200; t++)
	{
		std::vector<unsigned> keys(rnd(5000));
		for (unsigned& k : keys)
			k = GS_PAGE2TILE_KEY(rnd(PAGES), rnd(65536));
		for (size_t i = 0, n = keys.size(); i < n / 4; i++)
			keys.push_back(keys[rnd((unsigned)n)]);
		check(keys, "random keys");
	}
	if (s_fails)
		return 1;
	std::printf("page2tile: ok\n");
	return 0;
}
