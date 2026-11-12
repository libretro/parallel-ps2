/* JIT emission oracle. */

/* The call-site migration from the C++ emitter API to the C89 macro API can */
/* only be trusted if each converted file provably emits the same bytes. JIT */
/* emission for a fixed content trace is deterministic, and every code cache */
/* lives at a fixed offset inside the code arena (HOSTMEM_ in Memory.h), so hashing */
/* each region after a fixed number of frames gives a per-cache fingerprint: */
/* run the same trace on two builds, and any divergence in emitted code -- */
/* including patched displacements, since this hashes final bytes rather than */
/* an emission stream -- moves at least one hash. */

/* Enabled at runtime by PCSX2_JITHASH=1 in the environment; prints one line */
/* per region to stderr at shutdown. Zero cost when the variable is unset. */

#include "common/Pcsx2Defs.h"

#include <stdio.h>
#include <stdlib.h>

/* The exported entry point and the A/B mode flags exist on every */
/* architecture and compiler: libretro/main.cpp calls the dump from */
/* retro_deinit unconditionally, and the aarch64 link failed the moment */
/* this TU compiled empty there. Only the hashing internals are x86-64. */
#if defined(_MSC_VER)
#define JITHASH_EXPORT __declspec(dllexport)
#else
#define JITHASH_EXPORT __attribute__((visibility("default")))
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)

#include "Memory.h"
#include "VirtualMemory.h"
#include <stdlib.h>

typedef struct JitRegion
{
	const char* name;
	u32 offset;
	u32 size;
} JitRegion;

/* Offsets and sizes per the HOSTMEM_ map and the Assign() calls in the */
/* respective recompilers. Sizes are the reserve sizes, not high-water */
/* marks: unwritten arena pages read as zero, so hashing the full */
/* reserve is deterministic and needs no per-cache cursor plumbing. */
static const JitRegion s_regions[] = {
	{"EErec", HOSTMEM_EEREC_OFFSET, 64 * _1mb},
	{"IOPrec", HOSTMEM_IOPREC_OFFSET, 32 * _1mb},
	{"VIF0rec", HOSTMEM_VIF0REC_OFFSET, 8 * _1mb},
	{"VIF1rec", HOSTMEM_VIF1REC_OFFSET, 8 * _1mb},
	{"mVU0rec", HOSTMEM_MVU0REC_OFFSET, 64 * _1mb},
	{"mVU1rec", HOSTMEM_MVU1REC_OFFSET, 64 * _1mb},
	{"VIFUnpackRec", HOSTMEM_VIFUNPACKREC_OFFSET, _1mb},
};

void mvu_addsub_counters(u32* out4);

JITHASH_EXPORT void pcsx2_jithash_dump(void)
{
	const char* env = getenv("PCSX2_JITHASH");
	const u8* base;
	size_t ri;
	u32 c[4];
	if (!env || env[0] == '0')
		return;

	base = GetVmMemory()->code.baseptr;
	if (!base)
		return;

	for (ri = 0; ri < sizeof(s_regions) / sizeof(s_regions[0]); ri++)
	{
		/* FNV-1a 64, plus a count of nonzero bytes as an independent */
		/* sensitivity check (a hash collision and an equal nonzero count */
		/* together are not a plausible accident). */
		const JitRegion* r = &s_regions[ri];
		u64 h = 14695981039346656037ull;
		u64 nz = 0;
		const u8* p = base + r->offset;
		u32 i;
		for (i = 0; i < r->size; i++)
		{
			h = (h ^ p[i]) * 1099511628211ull;
			nz += (p[i] != 0);
		}
		fprintf(stderr, "[JITHASH] %-12s %016llx nz=%llu\n",
			r->name, (unsigned long long)h, (unsigned long long)nz);
	}
	mvu_addsub_counters(c);
	fprintf(stderr, "[ADDSUB] mVU0 elided %u/%u  mVU1 elided %u/%u\n",
		c[0], c[1], c[2], c[3]);
	fflush(stderr);
}

#else /* not x86-64: keep the contract, hash nothing */

JITHASH_EXPORT void pcsx2_jithash_dump(void)
{
}

#endif /* x86-64 */
