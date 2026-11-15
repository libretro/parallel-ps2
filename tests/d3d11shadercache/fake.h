/* The D3D11 shader cache suite's device and log (fake.c) */
#ifndef D3D11SHADERCACHE_FAKE_H
#define D3D11SHADERCACHE_FAKE_H
#include <libretro.h>
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned long long fk_hash;
extern int fk_fail_vs, fk_fail_layout;
void fk_h(const void* p, size_t n);
void fk_hu(unsigned long long v);
void RETRO_CALLCONV fk_log(enum retro_log_level level, const char* fmt, ...);
void* fk_device(void); /* ID3D11Device* */
void fk_release(void* object);
int fk_live(void);
/* the cache's files, hashed whole; and made to be what a test needs */
void fk_hash_file(const char* path);
void fk_truncate(const char* path, long size);
void fk_poke(const char* path, long at, unsigned value);
void fk_dup_key(const char* path);
#ifdef __cplusplus
}
#endif
#endif
