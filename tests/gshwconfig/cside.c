/* SPDX-License-Identifier: LGPL-3.0+ */
/* The config as C lays it out: each member's offset, and the bytes each
 * bit-field fills, for layout.cpp to hold against C++'s. */

#include <stddef.h>
#include <string.h>

#include "GS/Renderers/Common/GSHWDrawConfig.h"
#include "fields.h"

#define C_OFFSET(t, m) (unsigned)offsetof(t, m),
static const unsigned s_offsets[] = {GSHWCONFIG_OFFSETS(C_OFFSET) 0};

const unsigned* c_offsets(void) { return s_offsets; }

unsigned c_sizes(int i)
{
	switch (i)
	{
		case 0: return (unsigned)sizeof(gs_hw_draw_config);
		case 1: return (unsigned)sizeof(gs_hw_vs_cb);
		case 2: return (unsigned)sizeof(gs_hw_ps_cb);
		case 3: return (unsigned)sizeof(gs_hw_alpha_pass);
		case 4: return (unsigned)sizeof(gs_hw_blend_multi_pass);
		default: return 0;
	}
}

/* Bit-field i at all ones in an object otherwise zero, its bytes into out */
void c_bits(int i, unsigned char* out)
{
	gs_hw_draw_config c;
	int k = 0;
	memset(&c, 0, sizeof(c));
#define C_BITS(path, bits) if (k++ == i) c.path = (1u << (bits)) - 1u;
	GSHWCONFIG_BITS(C_BITS)
	memcpy(out, &c, sizeof(c));
}
