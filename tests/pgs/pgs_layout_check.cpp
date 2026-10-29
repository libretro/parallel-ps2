/* Shared-struct layout lane.
 *
 * data_structures.h is compiled twice: once here as C++, once as GLSL into
 * the shader bank. The bank ships precompiled, so a C++-side change to these
 * structs that moves a field by even a byte corrupts every draw with no
 * compile error anywhere. Every size and offset the GPU side depends on is
 * pinned here.
 */
#include "common.h"
#include <cstdio>
#include <cstddef>
using namespace ParallelGS;

#define PIN(T, n)        static_assert(sizeof(T) == (n), #T " size moved")
#define PINA(T, n)       static_assert(alignof(T) == (n), #T " alignment moved")
#define PINF(T, f, n)    static_assert(offsetof(T, f) == (n), #T "." #f " moved")

PIN(VertexPosition, 16);   PINA(VertexPosition, 4);
PINF(VertexPosition, pos, 0);
PINF(VertexPosition, z, 8);
PINF(VertexPosition, padding, 12);

PIN(VertexAttribute, 24);  PINA(VertexAttribute, 4);
PINF(VertexAttribute, st, 0);
PINF(VertexAttribute, q, 8);
PINF(VertexAttribute, rgba, 12);
PINF(VertexAttribute, fog, 16);
PINF(VertexAttribute, uv, 20);

PIN(PrimitiveAttribute, 32); PINA(PrimitiveAttribute, 4);
PINF(PrimitiveAttribute, bb, 0);
PINF(PrimitiveAttribute, state, 8);
PINF(PrimitiveAttribute, tex, 12);
PINF(PrimitiveAttribute, tex2, 16);
PINF(PrimitiveAttribute, alpha, 20);
PINF(PrimitiveAttribute, fbmsk, 24);
PINF(PrimitiveAttribute, fogcol, 28);

PIN(PrimitiveSetup, 80);
PINF(PrimitiveSetup, a, 0);
PINF(PrimitiveSetup, inv_area, 12);
PINF(PrimitiveSetup, b, 16);
PINF(PrimitiveSetup, error_i, 28);
PINF(PrimitiveSetup, c, 32);
PINF(PrimitiveSetup, error_j, 44);
PINF(PrimitiveSetup, bb, 48);
PINF(PrimitiveSetup, z, 64);

PIN(TransformedAttributes, 64);
PINF(TransformedAttributes, stqf0, 0);
PINF(TransformedAttributes, stqf1, 16);
PINF(TransformedAttributes, stqf2, 32);
PINF(TransformedAttributes, rgba0, 48);

PIN(StateVector, 16);
PINF(StateVector, combiner, 0);
PINF(StateVector, blend_mode, 4);
PINF(StateVector, dimx, 8);

/* An array of these is a std140 uniform block: the uvec4 starts on 16
 * bytes and the size is a multiple of 16, which is the array stride. */
PIN(CLUTDescriptor, 64);
PINF(CLUTDescriptor, csm2_x_bias, 32);
PINF(CLUTDescriptor, csm1_mask, 40);
PINF(CLUTDescriptor, csm2_x_offsets, 48);

int main(void)
{
	/* Print them too, so a change shows the numbers rather than only a
	 * static_assert line. */
	printf("VertexPosition      %2zu  pos %zu z %zu padding %zu\n",
	       sizeof(VertexPosition), offsetof(VertexPosition, pos),
	       offsetof(VertexPosition, z), offsetof(VertexPosition, padding));
	printf("VertexAttribute     %2zu  st %zu q %zu rgba %zu fog %zu uv %zu\n",
	       sizeof(VertexAttribute), offsetof(VertexAttribute, st),
	       offsetof(VertexAttribute, q), offsetof(VertexAttribute, rgba),
	       offsetof(VertexAttribute, fog), offsetof(VertexAttribute, uv));
	printf("PrimitiveAttribute  %2zu  bb %zu state %zu tex %zu tex2 %zu alpha %zu fbmsk %zu fogcol %zu\n",
	       sizeof(PrimitiveAttribute), offsetof(PrimitiveAttribute, bb),
	       offsetof(PrimitiveAttribute, state), offsetof(PrimitiveAttribute, tex),
	       offsetof(PrimitiveAttribute, tex2), offsetof(PrimitiveAttribute, alpha),
	       offsetof(PrimitiveAttribute, fbmsk), offsetof(PrimitiveAttribute, fogcol));
	printf("PrimitiveSetup      %2zu  TransformedAttributes %2zu  StateVector %2zu\n",
	       sizeof(PrimitiveSetup), sizeof(TransformedAttributes), sizeof(StateVector));
	printf("CLUTDescriptor      %2zu  csm2_x_bias %zu csm1_mask %zu csm2_x_offsets %zu\n",
	       sizeof(CLUTDescriptor), offsetof(CLUTDescriptor, csm2_x_bias),
	       offsetof(CLUTDescriptor, csm1_mask), offsetof(CLUTDescriptor, csm2_x_offsets));
	printf("PASS: shared struct layout\n");
	return 0;
}
