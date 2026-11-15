#include <stddef.h>
#include <string.h>
#include "fake.h"

unsigned long long fk_hash = 1469598103934665603ull;

void fk_h(const void* p, size_t n)
{
	const unsigned char* c = (const unsigned char*)p;
	size_t i;
	for (i = 0; i < n; i++)
	{
		fk_hash ^= c[i];
		fk_hash *= 1099511628211ull;
	}
}
void fk_hu(unsigned long long v) { fk_h(&v, sizeof(v)); }

void fk_read(unsigned bp, unsigned char* dst, int pitch, unsigned long long texa)
{
	fk_hu(0xB10Cull);
	fk_hu(bp);
	fk_hu((unsigned)pitch);
	fk_hu(texa);
	memcpy(dst, &bp, sizeof(bp));
}
