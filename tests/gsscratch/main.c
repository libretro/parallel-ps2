/* gs_scratch (GSScratch.h): every buffer it hands out is 16-byte aligned
 * and as large as asked, a smaller request reuses what it holds, a larger
 * one grows it, and free leaves it empty. Run under ASan, every byte asked
 * for is written, so a buffer short of its request is caught. C89. */
#include <stdio.h>
#include <string.h>

#include "GS/Renderers/Common/GSScratch.h"

int main(void)
{
	struct gs_scratch s;
	static const size_t sizes[] = { 1, 15, 16, 48, 4096, 3, 98304, 100, 1 << 20, 0 };
	void* held = NULL;
	size_t i, largest = 0;
	int failures = 0;

	memset(&s, 0, sizeof(s));
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
	{
		unsigned char* p = (unsigned char*)gs_scratch_get(&s, sizes[i]);
		if (!p || ((size_t)p & 15))
		{
			printf("  FAIL: %lu bytes: %p, not a 16-byte aligned buffer\n", (unsigned long)sizes[i], (void*)p);
			failures++;
			continue;
		}
		memset(p, 0x5a, sizes[i]);
		if (sizes[i] <= largest && (void*)p != held)
		{
			printf("  FAIL: %lu bytes after %lu: not the buffer it held\n",
				(unsigned long)sizes[i], (unsigned long)largest);
			failures++;
		}
		if (sizes[i] > largest)
			largest = sizes[i];
		held = p;
	}
	gs_scratch_free(&s);
	if (s.mem || s.size)
	{
		printf("  FAIL: free left a buffer\n");
		failures++;
	}
	gs_scratch_free(&s);
	printf(failures ? "gsscratch: FAILED (%d)\n" : "gsscratch: ok\n", failures);
	return failures != 0;
}
