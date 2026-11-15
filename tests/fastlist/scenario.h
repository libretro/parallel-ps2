/* A long run of what the caches do to their lists - insert at the front
 * keeping the index, erase by a kept index or through an iterator, move
 * to the front, pop the oldest, clear and then erase or move indexes kept
 * from before - on lists of 4-, 8- and 24-byte elements, grown to a few
 * thousand. The hash takes every index handed out, the size after each
 * step, and both walks through the list now and then. Built against the
 * C list's FastList (main.cpp) and against the C++ template it replaced
 * (the oracle). */
#include <vector>

static unsigned long long sc_hash = 1469598103934665603ull;
static void sc_h(const void* p, size_t n)
{
	const unsigned char* c = (const unsigned char*)p;
	for (size_t i = 0; i < n; i++)
	{
		sc_hash ^= c[i];
		sc_hash *= 1099511628211ull;
	}
}
static void sc_hu(unsigned long long v) { sc_h(&v, sizeof(v)); }

static u32 sc_rng = 0x9E3779B9u;
static u32 sc_rand(u32 n)
{
	sc_rng ^= sc_rng << 13;
	sc_rng ^= sc_rng >> 17;
	sc_rng ^= sc_rng << 5;
	return sc_rng % n;
}

struct sc_big
{
	u32 a;
	u64 b;
	u32 c[3];
};

static void sc_make(u32* d, u32 v) { *d = v; }
static void sc_make(u64* d, u32 v) { *d = 0x100000000ull * v + (v ^ 0x5A5A5A5Au); }
static void sc_make(sc_big* d, u32 v)
{
	memset(d, 0, sizeof(*d));
	d->a = v;
	d->b = ~(u64)v;
	d->c[0] = v * 3; d->c[1] = v * 5; d->c[2] = v * 7;
}

template <typename T>
static void sc_walk(const FastList<T>& l)
{
	u32 n = 0;
	for (auto it = l.begin(); it != l.end(); ++it, n++)
	{
		sc_hu(it.Index());
		sc_h(&*it, sizeof(T));
	}
	sc_hu(n);
	n = 0;
	for (auto it = l.rbegin(); it != l.rend(); ++it, n++)
		sc_hu(it.Index());
	sc_hu(n);
}

template <typename T>
static void sc_list(u32 steps, u32 most)
{
	FastList<T> l;
	std::vector<u16> kept, stale;
	T d;
	u32 v = 0;

	for (u32 s = 0; s < steps; s++)
	{
		const u32 op = sc_rand(100);
		if (op < 45 && l.size() < most)
		{
			sc_make(&d, v++);
			const u16 i = l.InsertFront(d);
			sc_hu(i);
			kept.push_back(i);
		}
		else if (op < 65 && !kept.empty())
		{
			const u32 k = sc_rand((u32)kept.size());
			l.EraseIndex(kept[k]);
			kept[k] = kept.back();
			kept.pop_back();
		}
		else if (op < 80 && !kept.empty())
			l.MoveFront(kept[sc_rand((u32)kept.size())]);
		else if (op < 85 && !l.empty())
		{
			const u16 last = l.rbegin().Index();
			l.pop_back();
			for (size_t k = 0; k < kept.size(); k++)
				if (kept[k] == last)
				{
					kept[k] = kept.back();
					kept.pop_back();
					break;
				}
		}
		else if (op < 88)
		{
			/* every third, through the iterator */
			u32 n = 0;
			for (auto it = l.begin(); it != l.end(); n++)
			{
				if (n % 3 == 0)
				{
					const u16 gone = it.Index();
					it = l.erase(it);
					for (size_t k = 0; k < kept.size(); k++)
						if (kept[k] == gone)
						{
							kept[k] = kept.back();
							kept.pop_back();
							break;
						}
				}
				else
					++it;
			}
		}
		else if (op < 89)
		{
			/* a purge, and what was kept from before it */
			l.clear();
			stale.swap(kept);
			kept.clear();
			for (size_t k = 0; k < stale.size(); k++)
			{
				l.EraseIndex(stale[k]);
				l.MoveFront(stale[k]);
			}
			l.EraseIndex(0);
			l.EraseIndex(65535);
			stale.clear();
		}
		else if (op < 91 && l.empty())
		{
			/* emptied but with its array: erase must not push past it */
			l.EraseIndex((u16)(1 + sc_rand(64)));
			l.MoveFront(0);
		}
		sc_hu(l.size());
		sc_hu(l.empty());
		if (!l.empty())
			sc_h(&l.back(), sizeof(T));
		if (s % 61 == 0)
			sc_walk(l);
	}
	sc_walk(l);
}

static void scenario()
{
	sc_list<u32>(60000, 3000);
	sc_list<u64>(60000, 9000);
	sc_list<sc_big>(40000, 700);
	sc_list<u32>(5000, 40);
}
