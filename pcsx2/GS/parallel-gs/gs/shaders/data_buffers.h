// SPDX-FileCopyrightText: 2024 Arntzen Software AS
// SPDX-FileContributor: Hans-Kristian Arntzen
// SPDX-FileContributor: Runar Heyer
// SPDX-License-Identifier: LGPL-3.0+

#ifndef DATA_BUFFERS_H_
#define DATA_BUFFERS_H_

#include "data_structures.h"

#ifdef NEED_VRAM
layout(set = 0, binding = BINDING_VRAM, std430) buffer VRAM32
{
	uint data[];
} vram32;

#ifndef PGS_NO_8BIT
layout(set = 0, binding = BINDING_VRAM, std430) buffer VRAM24
{
	u8vec3 data[];
} vram24;
#endif

#ifndef PGS_NO_16BIT
layout(set = 0, binding = BINDING_VRAM, std430) buffer VRAM16
{
	uint16_t data[];
} vram16;
#define pgs_vram16_load(i) uint(vram16.data[i])
#define pgs_vram16_store(i, v) vram16.data[i] = uint16_t(v)
#define pgs_vram16_store_flag(i, b) vram16.data[i] = (b) ? 0xffffus : 0us
#else
// Without 16-bit storage a halfword is half of a VRAM32 word.
uint pgs_vram16_load(uint i)
{
	return bitfieldExtract(vram32.data[i >> 1u], int((i & 1u) << 4u), 16);
}

// The other half is a neighbouring pixel's, so both steps are atomic.
void pgs_vram16_store(uint i, uint v)
{
	uint shamt = (i & 1u) << 4u;
	atomicAnd(vram32.data[i >> 1u], ~(0xffffu << shamt));
	atomicOr(vram32.data[i >> 1u], (v & 0xffffu) << shamt);
}
#define pgs_vram16_store_flag(i, b) pgs_vram16_store(i, (b) ? 0xffffu : 0u)
#endif
#endif

#ifdef NEED_PRIMITIVE_LIST
layout(set = 0, binding = BINDING_COARSE_TILE_LIST, std430)
PRIMITIVE_LIST_QUALIFIER buffer CoarseTileList
{
#ifdef PGS_NO_16BIT
	// A word per entry: the renderer sizes the list to match.
	uint data[];
#else
	uint16_t data[];
#endif
} coarse_primitive_list;
#ifdef PGS_NO_16BIT
#define pgs_list_entry(v) uint(v)
#else
#define pgs_list_entry(v) uint16_t(v)
#endif
#endif

#ifdef NEED_CONSTANTS
layout(set = 0, binding = BINDING_CONSTANTS, std140) uniform UBO
{
	GlobalConstants constants;
};
#endif

#ifdef NEED_PRIMITIVE_COUNTS
layout(set = 0, binding = BINDING_COARSE_PRIMITIVE_COUNT, std430)
PRIMITIVE_LIST_QUALIFIER buffer CoarsePrimitiveCount
{
	int data[];
} coarse_primitive_counts;
#endif

#ifdef NEED_PRIMITIVE_SETUP
layout(set = 0, binding = BINDING_PRIMITIVE_SETUP, std430)
PRIMITIVE_SETUP_QUALIFIER buffer PrimitiveSetups
{
	PrimitiveSetup data[];
} primitive_setup;
#endif

#ifdef NEED_VERTEX_POSITION
layout(set = 0, binding = BINDING_VERTEX_POSITION, std430)
readonly buffer VertexPositions
{
	VertexPosition data[];
} vertex_position;
#endif

#ifdef NEED_VERTEX_ATTRIBUTE
layout(set = 0, binding = BINDING_VERTEX_ATTRIBUTES, std430)
readonly buffer VertexAttributes
{
	VertexAttribute data[];
} vertex_attr;
#endif

const int MAX_PRIMITIVES = 64 * 1024;

#ifdef NEED_TEXTURE_INFO
layout(set = 0, binding = BINDING_TEXTURE_INFO, std140)
uniform TextureInfo
{
	TexInfo data[1024];
} texture_info;
#endif

#ifdef NEED_TRANSFORMED_ATTRIBUTE
layout(set = 0, binding = BINDING_TRANSFORMED_ATTRIBUTES, std430)
PRIMITIVE_SETUP_QUALIFIER buffer TransformedAttribute
{
	TransformedAttributes data[];
} transformed_attr;
#endif

#ifdef NEED_PRIMITIVE_ATTRIBUTE
layout(set = 0, binding = BINDING_PRIMITIVE_ATTRIBUTES, std430)
buffer PrimitiveAttributes
{
	PrimitiveAttribute data[];
} primitive_attr;
#endif

#ifdef NEED_STATE_VECTORS
layout(set = 0, binding = BINDING_STATE_VECTORS, std140) uniform StateVectors
{
	StateVector data[1 << STATE_INDEX_BIT_COUNT];
} state_vectors;
#endif

#ifdef BINDLESS
layout(set = 0, binding = BINDING_SAMPLER_NEAREST) uniform sampler nearest_sampler;
layout(set = 0, binding = BINDING_SAMPLER_LINEAR) uniform sampler linear_sampler;
#ifdef PGS_NO_DESCRIPTOR_INDEXING
layout(set = DESCRIPTOR_SET_IMAGES, binding = 0) uniform texture2D bindless_textures[PGS_FIXED_TEXTURES];
layout(set = DESCRIPTOR_SET_IMAGES, binding = 0) uniform texture2DArray bindless_textures_array[PGS_FIXED_TEXTURES];
#else
layout(set = DESCRIPTOR_SET_IMAGES, binding = 0) uniform texture2D bindless_textures[];
layout(set = DESCRIPTOR_SET_IMAGES, binding = 0) uniform texture2DArray bindless_textures_array[];
#endif
#endif

#ifdef USE_RCP_FIXED
layout(set = 0, binding = BINDING_FIXED_RCP_LUT) uniform utextureBuffer RCPLut;
#endif

#ifdef USE_RCP_FLOAT
layout(set = 0, binding = BINDING_FLOAT_RCP_LUT) uniform textureBuffer RCPLutFloat;
#endif

#endif
