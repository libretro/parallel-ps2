/*
 * The hash of a lookup key that is compared bit for bit: pipeline and
 * program selectors, the texture cache's hash cache key, descriptor
 * table keys. One XXH3 pass over the key's bytes, libretro-common's: a
 * key of a size known where this inlines, up to 128 bytes, is hashed in
 * place.
 *
 * Hashing a key field by field (the HashCombine this replaced) is one
 * seeded XXH3 call per field, chained: seven of them for a pipeline selector, about
 * 16 ns, where one pass over its 32 bytes is under 2 ns. A pipeline
 * lookup went from about 30 ns to about 11.
 *
 * Only for keys whose equality compares every byte (BitEqual, memcmp):
 * the hash reads every byte too, padding included, so two keys that
 * compare equal always hash equal. Static in the header, as the object
 * pool's take and give are: C89 has no inline.
 */

#ifndef GS_KEY_HASH_H
#define GS_KEY_HASH_H

#include <stddef.h>

#include <encodings/xxh3.h>

#ifdef __cplusplus
extern "C" {
#endif

static size_t gs_key_hash(const void *key, size_t size)
{
   return (size_t)xxh3_64(key, size, 0);
}

#ifdef __cplusplus
}
#endif

#endif
