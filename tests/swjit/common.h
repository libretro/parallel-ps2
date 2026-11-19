/* What the swjit drivers share: the generator's random draws, the hash,
 * and the code buffer, 64 bytes aligned (the generators align to the
 * address). C89. */
#ifndef SWJIT_COMMON_H
#define SWJIT_COMMON_H

#include <stdio.h>
#include <string.h>

#include "GS/Renderers/SW/GSSwJit.h"

static unsigned long long s_rng;
static u64 rnd64(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return s_rng;
}

static unsigned long long s_hash = 1469598103934665603ull;
static void hash_bytes(const u8* p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		s_hash = (s_hash ^ p[i]) * 1099511628211ull;
}

/* (C89 has no strtoull) */
static unsigned long long hex64(const char* s)
{
	unsigned long long v = 0;
	for (; *s; s++)
		v = v * 16 + (unsigned)(*s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10);
	return v;
}

#define SWJ_ROOM 65536
static union { u8 b[SWJ_ROOM + 64]; double align_; } s_buf_u;
static u8* swj_buf(void)
{
	return (u8*)(((size_t)s_buf_u.b + 63) & ~(size_t)63);
}

/* The hash against the expected one (argv[1]), and the exit status */
static int swj_done(const char* what, unsigned n, int argc, char** argv)
{
	const unsigned long long want = argc > 1 ? hex64(argv[1]) : 0;
	printf("swjit %s: %u generated, %016llx\n", what, n, s_hash);
	if (want && want != s_hash)
	{
		printf("FAIL: expected %016llx\n", want);
		return 1;
	}
	return 0;
}

#endif
