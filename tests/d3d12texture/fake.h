/* A device and D3D12 that hash what the textures ask of them: every
 * command recorded, every resource and view made or released, every byte
 * a copy reads, with resources numbered in the order they are made.
 * Shared by the C textures' suite (main.c) and the C++ GSTexture12 they
 * replaced (the oracle), so the two see the same device. */
#ifndef D3D12TEXTURE_FAKE_H
#define D3D12TEXTURE_FAKE_H

#include "GS/Renderers/DX12/GSD3D12Heap.h"
#include "GS/Renderers/DX12/D3D12DescriptorHeapManager.h"
#include "GS/Renderers/DX12/D3D12StreamBuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned long long fk_hash;
extern unsigned long long fk_fence, fk_completed;
void fk_h(const void* p, size_t n);
void fk_hu(unsigned long long v);

/* the device */
void* fk_device(void);                 /* ID3D12Device* */
void* fk_command_list(void);           /* ID3D12GraphicsCommandList* */
void* fk_init_command_list(void);
unsigned long long fk_resource_id(const void* resource);
int fk_create_placed(const void* desc, unsigned heap_type, unsigned state, const void* clear, void** resource,
	gs_d3d12_alloc_t* alloc);
void fk_defer_resource(const gs_d3d12_alloc_t* alloc, void* resource);
/* the heaps the descriptors come from, and their deferral */
gs_d3d12_descriptor_heap_manager* fk_heap(unsigned kind);
void fk_defer_descriptor(unsigned kind, unsigned index);
void fk_end_render_pass(void);
void fk_execute(int wait);
void fk_execute_for_readback(void);
void fk_wait(unsigned long long counter);
void fk_report(long hr);
void fk_render_mipmap(const void* resource, unsigned dst_level, unsigned dst_w, unsigned dst_h, unsigned src_level,
	unsigned src_w, unsigned src_h);
gs_d3d12_stream_buffer* fk_stream(void);
/* made at the start, released at the end */
void fk_init(void);
void fk_shutdown(void);
int fk_live_resources(void);

#ifdef __cplusplus
}
#endif

#endif
