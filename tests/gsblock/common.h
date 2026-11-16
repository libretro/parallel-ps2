/* The suite's buffers, random fill and hash: a 32-byte aligned block,
 * rows SC_PITCH apart for sources and destinations (sources offset by 4
 * bytes for the unaligned calls) and the palettes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_PITCH 128
#define SC_ROWS 64

static unsigned char sc_block_buf[256 + 64];
static unsigned char sc_src_buf[SC_ROWS * SC_PITCH + 64];
static unsigned char sc_rows_buf[SC_ROWS * SC_PITCH + 64];
static unsigned char* sc_block;
static unsigned char* sc_rows;
static unsigned char* sc_src_base;
static unsigned char sc_pal_buf[256 * 8 * 2 + 64];
static unsigned int* sc_pal32;
static unsigned long long* sc_pal64;

static unsigned long long sc_hash = 1469598103934665603ull;
static unsigned sc_rng = 0x1234567u;

static unsigned char* sc_align32(unsigned char* p)
{
	return p + ((32 - ((size_t)p & 31)) & 31);
}

static void sc_init(void)
{
	sc_block    = sc_align32(sc_block_buf);
	sc_rows     = sc_align32(sc_rows_buf);
	sc_src_base = sc_align32(sc_src_buf);
	sc_pal32    = (unsigned int*)sc_align32(sc_pal_buf);
	sc_pal64    = (unsigned long long*)(sc_align32(sc_pal_buf) + 256 * 8);
}

static unsigned char* sc_src(int aligned)
{
	return sc_src_base + (aligned ? 0 : 4);
}

static void sc_fill_bytes(unsigned char* p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
	{
		sc_rng ^= sc_rng << 13;
		sc_rng ^= sc_rng >> 17;
		sc_rng ^= sc_rng << 5;
		p[i] = (unsigned char)(sc_rng >> 7);
	}
}

static void sc_fill(void)
{
	sc_fill_bytes(sc_block, 256);
	sc_fill_bytes(sc_src_base, SC_ROWS * SC_PITCH);
	sc_fill_bytes(sc_rows, SC_ROWS * SC_PITCH);
	sc_fill_bytes(sc_align32(sc_pal_buf), 256 * 8 * 2);
}

/* Some colours zero in the block (the 24-bit and both 16-bit halves of a
 * 32-bit word), for TEXA.AEM */
static void sc_zeros(void)
{
	int i;
	for (i = 0; i < 64; i += 3)
	{
		sc_block[i * 4 + 0] = 0;
		sc_block[i * 4 + 1] = 0;
		sc_block[i * 4 + 2] = 0;
	}
	for (i = 1; i < 64; i += 5)
	{
		sc_block[i * 4 + 0] = 0;
		sc_block[i * 4 + 1] = (unsigned char)(sc_block[i * 4 + 1] & 0x80);
	}
	for (i = 2; i < 64; i += 7)
	{
		sc_block[i * 4 + 2] = 0;
		sc_block[i * 4 + 3] = (unsigned char)(sc_block[i * 4 + 3] & 0x80);
	}
}

static void sc_h(const unsigned char* p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
	{
		sc_hash ^= p[i];
		sc_hash *= 1099511628211ull;
	}
}

static void sc_hash_block(void) { sc_h(sc_block, 256); }
static void sc_hash_rows(void) { sc_h(sc_rows, SC_ROWS * SC_PITCH); }
