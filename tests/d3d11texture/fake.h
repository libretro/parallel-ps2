/* A D3D11 device and context that hash what the textures ask of them:
 * every texture and view made or released, every update, copy, map and
 * mipmap, with objects numbered in the order they are made. Shared by the
 * C textures' suite (main.c) and the C++ GSTexture11 they replaced (the
 * oracle), so the two see the same device. */
#ifndef D3D11TEXTURE_FAKE_H
#define D3D11TEXTURE_FAKE_H

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned long long fk_hash;
void fk_h(const void* p, size_t n);
void fk_hu(unsigned long long v);
void* fk_device(void);  /* ID3D11Device* */
void* fk_context(void); /* ID3D11DeviceContext* */
unsigned long long fk_id(const void* object);
void fk_init(void);
int fk_live_objects(void);

#ifdef __cplusplus
}
#endif

#endif
