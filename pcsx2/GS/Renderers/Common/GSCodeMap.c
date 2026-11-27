/* See GSCodeMap.h. C89. */
#include "GSCodeMap.h"
#include "GSCodeReserve.h"
#include "GSKeyHash.h"

/* The room a function is generated into first, and the most it is given:
 * a function that fills its room is generated again in twice the room. */
#define GS_CODE_MAP_MIN_SIZE 8192
#define GS_CODE_MAP_MAX_SIZE 65536

PCSX2_STATIC_ASSERT(sizeof(u64) % sizeof(size_t) == 0, "the key fills whole words");

void gs_code_map_init(gs_code_map* m, gs_code_gen_fn gen, size_t capacity)
{
	m->gen = gen;
	gs_flat_map_init(&m->map, sizeof(u64), sizeof(void*));
	gs_flat_map_reserve(&m->map, capacity);
}

void gs_code_map_free(gs_code_map* m)
{
	gs_flat_map_free(&m->map);
}

void gs_code_map_clear(gs_code_map* m)
{
	gs_flat_map_clear(&m->map);
}

/* NULL when the code reserve has no room left for the function */
static void* generate(gs_code_gen_fn gen, u64 key)
{
	size_t room;
	for (room = GS_CODE_MAP_MIN_SIZE; room <= GS_CODE_MAP_MAX_SIZE; room *= 2)
	{
		u8* const code = gs_code_reserve_reserve(room);
		size_t size;
		if (!code)
			return NULL;
		size = gen(key, code, room);
		if (size >= room)
			continue; /* cut short at the end of its room */
		gs_code_reserve_commit(size);
		return code;
	}
	return NULL;
}

void* gs_code_map_get(gs_code_map* m, u64 key)
{
	const size_t hash = gs_key_hash(&key, sizeof(key));
	void** const found = (void**)gs_flat_map_find(&m->map, &key, sizeof(key), hash);
	void* f;
	void** slot;
	int inserted;
	if (found)
		return *found;
	if (gs_flat_map_full(&m->map))
		return NULL;
	f = generate(m->gen, key);
	slot = (void**)gs_flat_map_insert(&m->map, &key, hash, &inserted);
	*slot = f; /* (not full: never NULL) */
	return f;
}
