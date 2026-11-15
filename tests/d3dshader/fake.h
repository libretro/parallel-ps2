/* The D3D.c suite's log and adapters (fake.c) */
#ifndef D3DSHADER_FAKE_H
#define D3DSHADER_FAKE_H
#include <libretro.h>
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned long long fk_hash;
void fk_h(const void* p, size_t n);
void fk_hu(unsigned long long v);
void RETRO_CALLCONV fk_log(enum retro_log_level level, const char* fmt, ...);
void* fk_adapter(unsigned vendor, int ok); /* IDXGIAdapter1* */
#ifdef __cplusplus
}
#endif
#endif
