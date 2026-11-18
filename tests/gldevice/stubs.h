#ifndef GLDEVICE_STUBS_H
#define GLDEVICE_STUBS_H

#include <stddef.h>
#include <stdint.h>

#include "common/Pcsx2Defs.h"

/* The log of everything the device asks of GL and logs: a running hash
 * of words, and with GLDEV_DUMP set each word written there too, for a
 * diff of two runs */
void log_word(u64 v);
void log_float(float f);
void log_double(double d);
void log_bytes(const void* p, size_t n);
void log_string(const char* s);
void log_name(const char* s);
extern u64 g_hash;
extern u64 g_words;

/* The answers of the stub driver */
void stub_gen(int n, unsigned* ids);
unsigned stub_new_id(void);
void* stub_new_sync(void);
u64 stub_sync_id(const void* sync);
void stub_shader_source(int count, const char* const* strings, const int* lengths);
void stub_get_integerv(unsigned pname, int* data);
int stub_get_object_iv(unsigned pname);
const unsigned char* stub_get_string(unsigned name);
/* A persistent map's memory (freed at exit) */
void* stub_map(size_t size);
void stub_unmap_all(void);

/* The context the run asks for (main.c) */
extern int g_gles;

#endif
