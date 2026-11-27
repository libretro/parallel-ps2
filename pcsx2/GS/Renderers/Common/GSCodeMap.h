/* The functions a code generator has made, by the key each was made for,
 * in a GSFlatMap from the key to the function: a lookup reads one slot.
 * A key the generator could not make a function for (the code reserve
 * full) is kept with none, so it is not tried again until cleared.
 *
 * Its slots are made with it, for capacity functions, and never grow: a
 * new key past them gets no function, as when the code reserve is full,
 * and the caller starts both over (gs_rasterizer_list's setup). C89. */
#ifndef GS_CODE_MAP_H
#define GS_CODE_MAP_H

#include <stddef.h>

#include "common/Pcsx2Defs.h"
#include "GSFlatMap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generates key's function into room bytes at code: its size, or room
 * when it did not fit (a generator stops writing at the end of its room) */
typedef size_t (*gs_code_gen_fn)(u64 key, void* code, size_t room);

typedef struct gs_code_map
{
	gs_flat_map_t map;
	gs_code_gen_fn gen;
} gs_code_map;

/* (no slots if the allocation fails: full, and every lookup gets none) */
void gs_code_map_init(gs_code_map* m, gs_code_gen_fn gen, size_t capacity);
void gs_code_map_free(gs_code_map* m);

/* Forgets every function, for a reset of the code reserve: none of them
 * is handed out again */
void gs_code_map_clear(gs_code_map* m);

/* key's function, generated into the code reserve now if it is not yet;
 * NULL when it cannot be */
void* gs_code_map_get(gs_code_map* m, u64 key);

#ifdef __cplusplus
}
#endif

#endif
