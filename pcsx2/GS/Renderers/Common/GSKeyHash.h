/*
 * The hash of a lookup key that is compared bit for bit: pipeline and
 * program selectors, the texture cache's hash cache key, descriptor
 * table keys. One XXH3 pass over the key's bytes.
 *
 * These were HashCombine over the key's fields, which is one seeded XXH3
 * call per field, chained: seven of them for a pipeline selector, about
 * 16 ns, where one pass over its 32 bytes is under 2 ns (its size is a
 * constant at every call, so XXH3's fixed-length path inlines). A
 * pipeline lookup went from about 30 ns to about 11.
 *
 * Only for keys whose equality compares every byte (BitEqual, memcmp):
 * the hash reads every byte too, padding included, so two keys that
 * compare equal always hash equal. Static in the header, as the object
 * pool's take and give are: C89 has no inline.
 */

#ifndef GS_KEY_HASH_H
#define GS_KEY_HASH_H

#include <stddef.h>

#ifndef XXH_versionNumber
#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include <xxhash.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

static size_t gs_key_hash(const void *key, size_t size)
{
   return (size_t)XXH3_64bits(key, size);
}

#ifdef __cplusplus
}
#endif

#endif
