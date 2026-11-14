/* The vertex trace (GS/Renderers/Common/GSVertexTrace.c and its per-ISA
 * finders) over the draws of scenario.h: the hash is what the C++ class
 * it replaced gave on the same architecture, which build.sh holds. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/Renderers/Common/GSVertexTrace.h"

#include "scenario_c.h"

/* GSVertexTrace keeps its GSState pointer in the C struct's tail padding
 * (C++ may put a derived class's member there): init leaves it alone. */
static int init_keeps_tail(void)
{
	static union { struct gs_vertex_trace vt; unsigned char b[sizeof(struct gs_vertex_trace)]; } u;
	const size_t end = offsetof(struct gs_vertex_trace, m_lod) + sizeof(u.vt.m_lod);
	size_t i;
	memset(u.b, 0xa5, sizeof(u.b));
	gs_vertex_trace_init(&u.vt, 0);
	for (i = end; i < sizeof(u.b); i++)
		if (u.b[i] != 0xa5)
		{
			printf("FAIL: init wrote the tail padding, byte %u of %u\n", (unsigned)i, (unsigned)sizeof(u.b));
			return 0;
		}
	return 1;
}

int main(int argc, char** argv)
{
	unsigned long long want = argc > 1 ? strtoull(argv[1], NULL, 16) : 0;
	if (!init_keeps_tail())
		return 1;
	scenario();
	printf("vtrace: %016llx\n", vt_hash);
	if (want && want != vt_hash)
	{
		printf("FAIL: the C++ trace gave %016llx\n", want);
		return 1;
	}
	return 0;
}
