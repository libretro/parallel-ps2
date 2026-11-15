/* The texture cache suite's hash and block reader (fake.c) */
#ifndef GSTEXCACHESW_FAKE_H
#define GSTEXCACHESW_FAKE_H
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned long long fk_hash;
void fk_h(const void* p, size_t n);
void fk_hu(unsigned long long v);
/* a block asked for: its number, pitch and TEXA hashed, the number left
 * at the place it goes */
void fk_read(unsigned bp, unsigned char* dst, int pitch, unsigned long long texa);
#ifdef __cplusplus
}
#endif
#endif
