/* See GSScratch.h. C89. */
#include <memalign.h>

#include "GSScratch.h"

void* gs_scratch_get(struct gs_scratch* s, size_t bytes)
{
	void* grown;

	if (bytes <= s->size && s->mem)
		return s->mem;
	grown = memalign_alloc(16, bytes ? bytes : 1);
	if (!grown)
		return NULL;
	memalign_free(s->mem);
	s->mem  = grown;
	s->size = bytes;
	return grown;
}

void gs_scratch_free(struct gs_scratch* s)
{
	memalign_free(s->mem);
	s->mem  = NULL;
	s->size = 0;
}
