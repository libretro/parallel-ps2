/* A scratch buffer that grows to the largest size asked of it and is
 * kept until freed: for work that is too large, or sized by input too
 * loosely bounded, to put on the stack. 16-byte aligned, for the vector
 * members of what goes in it. Contents do not survive a call that grows
 * it. Single-threaded, like its owners. C89. */
#ifndef GS_SCRATCH_H
#define GS_SCRATCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gs_scratch
{
	void*  mem;
	size_t size;
};

/* At least bytes of scratch, or NULL when that cannot be had. */
void* gs_scratch_get(struct gs_scratch* s, size_t bytes);
void  gs_scratch_free(struct gs_scratch* s);

#ifdef __cplusplus
}
#endif

#endif
