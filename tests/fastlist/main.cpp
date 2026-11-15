/* FastList (GS/Renderers/Common/GSFastList.h), the C list's C++ view:
 * a list that has never held anything, insertion, erasure and moves,
 * indexes kept across a purge, the most it holds; and scenario.h's run,
 * whose hash the C++ template it replaced gave (on the command line). */
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSFastList.h"
#include "scenario.h"
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)
int main(int argc, char** argv)
{
	{	/* never touched */
		FastList<int> l;
		CHECK(l.size() == 0, "fresh: size 0");
		CHECK(l.empty(), "fresh: empty");
		CHECK(l.begin() == l.end(), "fresh: begin == end");
		int n = 0;
		for (auto it = l.begin(); it != l.end(); ++it) n++;
		CHECK(n == 0, "fresh: iterates nothing");
		n = 0;
		for (auto it = l.rbegin(); it != l.rend(); ++it) n++;
		CHECK(n == 0, "fresh: reverse-iterates nothing");
		l.clear();
		CHECK(l.empty(), "cleared while empty: still empty");
	}
	{	/* insert, read back, erase, and again after a clear */
		FastList<int> l;
		for (int pass = 0; pass < 3; pass++)
		{
			std::vector<int> want;
			for (int i = 0; i < 300; i++) { l.push_front(i); want.push_back(i); }
			std::reverse(want.begin(), want.end());
			CHECK(l.size() == 300, "300 inserted: size");
			std::vector<int> got;
			for (auto it = l.begin(); it != l.end(); ++it) got.push_back(*it);
			CHECK(got == want, "front insertion order preserved");
			std::vector<int> rgot;
			for (auto it = l.rbegin(); it != l.rend(); ++it) rgot.push_back(*it);
			std::reverse(rgot.begin(), rgot.end());
			CHECK(rgot == want, "reverse iteration is the mirror");
			CHECK(l.back() == 0, "back is the oldest");
			l.pop_back();
			CHECK(l.size() == 299, "pop_back: size");
			for (auto it = l.begin(); it != l.end();)
				it = (*it % 2) ? l.erase(it) : ++it;
			CHECK(l.size() == 149, "erase every odd: size (1..299 has 150 odds)");
			for (auto it = l.begin(); it != l.end(); ++it)
				CHECK(*it % 2 == 0, "only evens remain");
			l.clear();
			CHECK(l.size() == 0 && l.begin() == l.end(), "clear empties it");
		}
	}
	{	/* MoveFront, which walks the chain through index 0 */
		FastList<int> l;
		u16 idx[8];
		for (int i = 0; i < 8; i++) idx[i] = l.InsertFront(i);
		l.MoveFront(idx[0]);
		CHECK(*l.begin() == 0, "MoveFront brings it to the front");
		l.MoveFront(idx[0]);
		CHECK(*l.begin() == 0, "MoveFront of the front is a no-op");
	}
	{	/* A stale index - one into a list that has since been emptied -
		 * must do nothing: not corrupt the list, not fault, and above all
		 * not write past the buffer. This is what a Source's per-page
		 * m_erase_it becomes when the texture cache is purged under it. */
		FastList<int> l;
		u16 idx[4];
		int i;
		for (i = 0; i < 4; i++) idx[i] = l.InsertFront(i);
		l.clear();
		for (i = 0; i < 4; i++) l.EraseIndex(idx[i]);
		for (i = 0; i < 4; i++) l.MoveFront(idx[i]);
		CHECK(l.size() == 0 && l.begin() == l.end(), "stale erase/move on a cleared list: still empty");
		l.EraseIndex(0);
		l.EraseIndex(60000);
		CHECK(l.size() == 0, "index zero and an out-of-range one: ignored");

		/* The heap corruptor: erasing from a list that is allocated but
		 * holds nothing. The free-index top is a u16 at zero, and the
		 * pre-decrement wrapped it to 65535, so the store landed 128 KB
		 * past the buffer. Run this file under ASan and that write is
		 * what it catches. */
		FastList<int> l2;
		l2.push_front(1);
		l2.pop_back();
		CHECK(l2.size() == 0, "allocated then emptied");
		l2.EraseIndex(1);
		l2.EraseIndex(2);
		CHECK(l2.size() == 0, "erase from an allocated empty list: ignored");
		l2.push_front(7);
		CHECK(l2.size() == 1 && *l2.begin() == 7, "and it still works after");
	}
	{	/* The most it holds: index 0 is the head, so 65534. The array
		 * cannot grow past 65535, and the insertion after that read its
		 * free index from past the end of the stack. */
		static FastList<u32> l;
		u32 i;
		u16 at;
		for (i = 0; i < 65534; i++)
			if (!l.InsertFront(i))
				break;
		CHECK(i == 65534 && l.size() == 65534, "full: 65534 held");
		at = l.InsertFront(65534);
		CHECK(at == 0 && l.size() == 65534, "one more: refused, index 0, nothing changed");
		CHECK(*l.begin() == 65533 && l.back() == 0, "full: the ends as they were");
		l.pop_back();
		at = l.InsertFront(70000);
		CHECK(at != 0 && *l.begin() == 70000 && l.size() == 65534, "room again after an erase");
	}
	scenario();
	printf("fastlist scenario: %016llx\n", sc_hash);
	if (argc > 1 && strtoull(argv[1], NULL, 16) != sc_hash)
	{
		printf("  FAIL: the C++ template gave %s\n", argv[1]);
		fails++;
	}
	printf(fails ? "fastlist: FAILED (%d)\n" : "fastlist: ok\n", fails);
	return fails != 0;
}
