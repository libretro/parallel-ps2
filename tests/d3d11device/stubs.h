#ifndef D3D11DEVICE_STUBS_H
#define D3D11DEVICE_STUBS_H

#include <stddef.h>

#include "common/Pcsx2Defs.h"

/* The log of everything the device asks of D3D11: a running hash of
 * words, and with D3D11DEV_DUMP set each word written there too, for a
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
	K_DEVICE, K_CONTEXT, K_DXGI_DEVICE, K_ADAPTER, K_BLOB, K_BUFFER, K_TEXTURE2D, K_SRV, K_RTV, K_DSV, K_UAV,
	K_VS, K_PS, K_INPUT_LAYOUT, K_BLEND, K_DEPTH_STENCIL, K_RASTERIZER, K_SAMPLER
};

typedef struct stub_obj
{
	const void* lpVtbl;
	long refs;
	u32 id;
	u32 kind;
	u8* mem;              /* a buffer's, a mappable texture's, a blob's */
	size_t size;
	u32 row_pitch;
	u32 bind;             /* a buffer's bind flags */
	struct stub_obj* res; /* a view's resource */
	struct stub_obj* next;
} stub_obj;

const void* stub_vtbl(unsigned kind);
void* stub_new(unsigned kind);
u64 stub_id(const void* obj);
void stub_call(stub_obj* o, const char* name);
void stub_log_objs(void* const* objs, unsigned n);

/* The run's device (main.c) */
typedef struct stub_config
{
	unsigned feature_level;
	unsigned vendor;
	int typed_uav_loads;
	int context1;
	int single_threaded;
} stub_config;
extern stub_config g_cfg;
extern int g_dead_calls;
unsigned stub_live_objects(void);

#endif
