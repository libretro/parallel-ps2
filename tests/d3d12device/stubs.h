#ifndef D3D12DEVICE_STUBS_H
#define D3D12DEVICE_STUBS_H

#include <stddef.h>

#include "common/Pcsx2Defs.h"

/* The log of everything the device asks of D3D12: a running hash of
 * words, and with D3D12DEV_DUMP set each word written there too, for a
 * diff of two runs */
void log_word(u64 v);
void log_float(float f);
void log_bytes(const void* p, size_t n);
void log_string(const char* s);
void log_name(const char* s);
void log_close(void);
extern u64 g_hash;
extern u64 g_words;

/* The stub objects, one kind per interface (gen_stubs.py's order) */
enum
{
	K_DEVICE, K_QUEUE, K_ALLOCATOR, K_LIST, K_FENCE, K_DHEAP, K_RESOURCE, K_HEAP, K_PSO, K_ROOTSIG, K_BLOB,
	K_FACTORY, K_ADAPTER, K_COUNT
};

typedef struct stub_obj
{
	const void* lpVtbl;
	long refs;
	u32 id;
	u32 kind;
	u8* mem; /* a buffer's, a blob's */
	size_t size;
	u64 value; /* a fence's */
	int buffer;
	u64 width;
	u32 height;
	u32 format;
	unsigned char desc[64]; /* a resource's D3D12_RESOURCE_DESC */
	struct stub_obj* next;
} stub_obj;

const void* stub_vtbl(unsigned kind);
void* stub_new(unsigned kind);
u64 stub_id(const void* obj);
void stub_call(stub_obj* o, const char* name);
void stub_log_objs(void* const* objs, unsigned n);

typedef struct stub_config
{
	unsigned vendor;
	int heap_tier;
} stub_config;
extern stub_config g_cfg;
extern int g_dead_calls;
unsigned stub_live_objects(void);

#endif
