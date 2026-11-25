// SPDX-FileCopyrightText: 2024 Arntzen Software AS
// SPDX-FileContributor: Hans-Kristian Arntzen
// SPDX-FileContributor: Runar Heyer
// SPDX-License-Identifier: LGPL-3.0+

#version 450

#ifndef PGS_NO_16BIT
#extension GL_EXT_shader_16bit_storage : require
#endif
#extension GL_EXT_samplerless_texture_functions : require
#include "data_structures.h"
#include "swizzle_utils.h"
#include "utils.h"

layout(location = 0) out vec4 FragColor;

#if PROMOTED
layout(set = 0, binding = 0) uniform texture2DArray uPromoted;
#else
layout(set = 0, binding = 0) readonly buffer VRAM32
{
    uint data[];
} vram32;

#ifdef PGS_NO_16BIT
// Without 16-bit storage a halfword is read out of its word.
#define VRAM16_LOAD(addr) bitfieldExtract(vram32.data[(addr) >> 1u], int(((addr) & 1u) << 4u), 16)
#else
layout(set = 0, binding = 0) readonly buffer VRAM16
{
    uint16_t data[];
} vram16;
#define VRAM16_LOAD(addr) uint(vram16.data[addr])
#endif
#endif

layout(push_constant) uniform Registers
{
    uint fbp;
    uint fbw;
    uint dbx;
    uint dby;
    uint phase;
    uint phase_stride;
} registers;

layout(constant_id = 0) const int PSM = 0;
layout(constant_id = 1) const uint VRAM_MASK = 4 * 1024 * 1024 - 1;
layout(constant_id = 2) const uint SUPER_SAMPLES = 1;

const bool is_tex_16bit = PSM == PSMCT16 || PSM == PSMCT16S || PSM == PSMZ16 || PSM == PSMZ16S;

#if PROMOTED
vec4 sample_vram(uvec2 coord, uint slice)
{
    return texelFetch(uPromoted, ivec3(coord, slice), 0);
}
#else
vec4 sample_vram(uint addr, uint slice)
{
    uint payload;

    if (is_tex_16bit)
    {
        payload = rgba16_to_rgba32(VRAM16_LOAD(addr + slice * (VRAM_MASK + 1) / 2), 0, 0x0, 0xff);
    }
    else
    {
        payload = vram32.data[addr + slice * (VRAM_MASK + 1) / 4];
        if (PSM != PSMCT32)
        {
            payload &= ~0xff000000u;
            payload |= 0x80000000u;
        }
    }

    return unpackUnorm4x8(payload);
}
#endif

#if PROMOTED
bool super_sample_is_valid(uvec2 coord) { return true; }
#else
bool super_sample_is_valid(uint addr)
{
    bool is_valid;

    if (is_tex_16bit)
    {
        is_valid = VRAM16_LOAD(addr + (VRAM_MASK + 1) / 2) == 0xffff;
    }
    else
    {
        uint payload = vram32.data[addr + (VRAM_MASK + 1) / 4];
        if (PSM != PSMCT32)
            is_valid = (payload & 0xffffffu) == 0xffffffu;
        else
            is_valid = payload == ~0u;
    }

    return is_valid;
}
#endif

// Fetch one sample from the ordered 4x4 grid addressed in 4x-output
// coordinates: G is the global grid coordinate (four grid steps per
// native pixel on each axis). Used by the tent reconstruction taps,
// which cross native pixel boundaries.
vec4 fetch_grid_sample(uvec2 G, uint phase_stride)
{
#if PROMOTED
    const uint TAP_BASE_LAYER = 1u;
#else
    const uint TAP_BASE_LAYER = 2u;
#endif
    uvec2 native = G >> 2u;
    uvec2 g = G & 3u;
    uint slice = (g.y & 1u) | ((g.x & 1u) << 1u) | ((g.y >> 1u) << 2u) | ((g.x >> 1u) << 3u);

    uvec2 c = native * uvec2(1u, phase_stride) +
        uvec2(registers.dbx, registers.dby + registers.phase);

#if PROMOTED
    return sample_vram(c, TAP_BASE_LAYER + slice);
#else
    uint a = swizzle_PS2(c.x, c.y, registers.fbp * PGS_BLOCKS_PER_PAGE, registers.fbw, PSM, VRAM_MASK);
    if (super_sample_is_valid(a))
        return sample_vram(a, TAP_BASE_LAYER + slice);
    else
        return sample_vram(a, 0);
#endif
}

// The same for the ordered 4x8 grid, addressed in the output's
// coordinates: four columns per native pixel across, and down either
// eight rows (rows_log2 3, a field at its full height, one sample each)
// or four (rows_log2 2, a frame at 4x, two rows averaged). Layer bits as
// in compute_sample_points: the row in 0, 2 and 4, the column in 1 and 3.
vec4 fetch_grid32_sample(uvec2 G, uint rows_log2, uint phase_stride)
{
#if PROMOTED
    const uint TAP_BASE_LAYER = 1u;
#else
    const uint TAP_BASE_LAYER = 2u;
#endif
    uvec2 native = uvec2(G.x >> 2u, G.y >> rows_log2);
    uint gx = G.x & 3u;
    uint column = ((gx & 1u) << 1u) | ((gx >> 1u) << 3u);
    uvec2 c = native * uvec2(1u, phase_stride) +
        uvec2(registers.dbx, registers.dby + registers.phase);
#if !PROMOTED
    uint a = swizzle_PS2(c.x, c.y, registers.fbp * PGS_BLOCKS_PER_PAGE, registers.fbw, PSM, VRAM_MASK);
    if (!super_sample_is_valid(a))
        return sample_vram(a, 0);
#define GRID32_ADDR a
#else
#define GRID32_ADDR c
#endif
    if (rows_log2 == 3u)
    {
        uint gy = G.y & 7u;
        uint row = (gy & 1u) | (((gy >> 1u) & 1u) << 2u) | ((gy >> 2u) << 4u);
        return sample_vram(GRID32_ADDR, TAP_BASE_LAYER + column + row);
    }
    else
    {
        uint gy = G.y & 3u;
        uint row = ((gy & 1u) << 2u) | ((gy >> 1u) << 4u);
        return 0.5 * (sample_vram(GRID32_ADDR, TAP_BASE_LAYER + column + row) +
                      sample_vram(GRID32_ADDR, TAP_BASE_LAYER + column + row + 1u));
    }
#undef GRID32_ADDR
}

void main()
{
    // The upper half of phase_stride carries the per-axis scanout scale
    // log2s (X in bits 16..19, Y in bits 20..23; 1 = 2x, 2 = 4x) so the
    // push constant layout stays identical to the original. A field-aware
    // scanout counts Y over the field, one step above X on a square pixel.
    uint scale_x_log2 = (registers.phase_stride >> 16u) & 0xfu;
    uint scale_y_log2 = (registers.phase_stride >> 20u) & 0xfu;
    bool tent_filter = ((registers.phase_stride >> 24u) & 1u) != 0u;
    if (scale_x_log2 == 0u)
        scale_x_log2 = 1u;
    if (scale_y_log2 == 0u)
        scale_y_log2 = scale_x_log2;
    uint phase_stride = registers.phase_stride & 0xffffu;

    uvec2 super_sampled_coord = uvec2(gl_FragCoord.xy);
    uvec2 single_sampled_coord;
    if (SUPER_SAMPLES >= 4)
        single_sampled_coord = super_sampled_coord >> uvec2(scale_x_log2, scale_y_log2);
    else
        single_sampled_coord = super_sampled_coord;

    // Is this how full-height interlace is supposed to work? :|
    uvec2 coord = single_sampled_coord * uvec2(1u, phase_stride) +
        uvec2(registers.dbx, registers.dby + registers.phase);

#if PROMOTED
    #define addr coord
    const int BASE_SSAA_LAYER = 1;
#else
    uint addr = swizzle_PS2(coord.x, coord.y, registers.fbp * PGS_BLOCKS_PER_PAGE, registers.fbw, PSM, VRAM_MASK);
    const int BASE_SSAA_LAYER = 2;
#endif

    if (SUPER_SAMPLES == 1)
    {
        // SUPER_SAMPLES == 2 forces 1 path.
        FragColor = sample_vram(addr, 0);
    }
    else if (SUPER_SAMPLES == 16 && scale_x_log2 == 2u && scale_y_log2 == 2u)
    {
        // 4x scanout over the ordered 4x4 grid: every output pixel maps to
        // exactly one sample layer. The rasterizer orders layers with the
        // fine position in bits 0:1 and the quadrant in bits 2:3
        // (see compute_sample_points), so rebuild the index from the two
        // low output coordinate bits per axis. No averaging: the whole
        // grid is spent on resolution.
        if (tent_filter)
        {
            // Reconstruct with a separable [1 2 1]/4 tent over the
            // neighboring grid samples: nine real rasterized samples per
            // output pixel instead of one. Texture anti-aliasing at full
            // resolution for about half an output pixel of softness.
            const float w[3] = float[3](0.25, 0.5, 0.25);
            ivec2 base = ivec2(super_sampled_coord);
            vec4 acc = vec4(0.0);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    uvec2 G = uvec2(max(base + ivec2(dx, dy), ivec2(0)));
                    acc += w[dx + 1] * w[dy + 1] * fetch_grid_sample(G, phase_stride);
                }
            FragColor = acc;
        }
        else if (super_sample_is_valid(addr))
        {
            uvec2 g = super_sampled_coord & 3u;
            uint slice = (g.y & 1u) | ((g.x & 1u) << 1u) | ((g.y >> 1u) << 2u) | ((g.x >> 1u) << 3u);
            FragColor = sample_vram(addr, BASE_SSAA_LAYER + slice);
        }
        else
            FragColor = sample_vram(addr, 0);
    }
    else if ((SUPER_SAMPLES == 8 || SUPER_SAMPLES == 16) && scale_x_log2 == 1u && scale_y_log2 == 2u)
    {
        // A field scanned out at 4x over its lines, 2x across: each output
        // row is one of the four sample rows of its field line. The 8x
        // checkerboard has one sample in each row of each half pixel
        // (layer: y bit 0, the half in bit 1, y bit 1 in bit 2); the
        // ordered 4x4 grid two, which the row averages.
        if (super_sample_is_valid(addr))
        {
            uint gx = super_sampled_coord.x & 1u;
            uint gy = super_sampled_coord.y & 3u;
            if (SUPER_SAMPLES == 8)
                FragColor = sample_vram(addr, BASE_SSAA_LAYER + ((gy & 1u) | (gx << 1u) | ((gy >> 1u) << 2u)));
            else
            {
                uint base = (gy & 1u) | ((gy >> 1u) << 2u) | (gx << 3u);
                FragColor = 0.5 * (sample_vram(addr, BASE_SSAA_LAYER + base) +
                                   sample_vram(addr, BASE_SSAA_LAYER + base + 2u));
            }
        }
        else
            FragColor = sample_vram(addr, 0);
    }
    else if (SUPER_SAMPLES == 32 && scale_x_log2 == 2u)
    {
        // The ordered 4x8 grid: four columns of samples across a pixel
        // and eight rows down it, a layer indexed with the row in bits
        // 0, 2 and 4 and the column in bits 1 and 3 (see
        // compute_sample_points). Across, each output pixel is one
        // column. Down, a field scanned out at its full height takes one
        // row per output row; a frame at 4x takes two rows, averaged.
        if (tent_filter)
        {
            // The same [1 2 1]/4 tent as the 4x4 grid's, over this
            // grid's samples: nine of them per output pixel, a column
            // and a row apart, across native pixel edges too.
            const float w[3] = float[3](0.25, 0.5, 0.25);
            ivec2 base = ivec2(super_sampled_coord);
            vec4 acc = vec4(0.0);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    uvec2 G = uvec2(max(base + ivec2(dx, dy), ivec2(0)));
                    acc += w[dx + 1] * w[dy + 1] * fetch_grid32_sample(G, scale_y_log2, phase_stride);
                }
            FragColor = acc;
        }
        else if (super_sample_is_valid(addr))
        {
            uint gx = super_sampled_coord.x & 3u;
            uint column = ((gx & 1u) << 1u) | ((gx >> 1u) << 3u);
            if (scale_y_log2 == 3u)
            {
                uint gy = super_sampled_coord.y & 7u;
                uint row = (gy & 1u) | (((gy >> 1u) & 1u) << 2u) | ((gy >> 2u) << 4u);
                FragColor = sample_vram(addr, BASE_SSAA_LAYER + column + row);
            }
            else
            {
                uint gy = super_sampled_coord.y & 3u;
                uint row = ((gy & 1u) << 2u) | ((gy >> 1u) << 4u);
                FragColor = 0.5 * (sample_vram(addr, BASE_SSAA_LAYER + column + row) +
                                   sample_vram(addr, BASE_SSAA_LAYER + column + row + 1u));
            }
        }
        else
            FragColor = sample_vram(addr, 0);
    }
    else if (SUPER_SAMPLES == 32 && scale_x_log2 == 1u)
    {
        // The same grid scanned out at 2x across: each output pixel is
        // two columns, averaged, with either two rows (a frame at 2x, or
        // a field at 4x over its lines) or four (a frame at 2x with the
        // rest of the grid as anti-aliasing).
        if (super_sample_is_valid(addr))
        {
            uint column = (super_sampled_coord.x & 1u) << 3u;
            uint row, rows;
            if (scale_y_log2 == 2u)
            {
                uint gy = super_sampled_coord.y & 3u;
                row = ((gy & 1u) << 2u) | ((gy >> 1u) << 4u);
                rows = 2u;
            }
            else
            {
                row = (super_sampled_coord.y & 1u) << 4u;
                rows = 4u;
            }
            FragColor = vec4(0.0);
            for (uint i = 0; i < rows; i++)
            {
                uint r = row + (i & 1u) + ((i >> 1u) << 2u);
                FragColor += sample_vram(addr, BASE_SSAA_LAYER + column + r) +
                             sample_vram(addr, BASE_SSAA_LAYER + column + r + 2u);
            }
            FragColor /= float(2u * rows);
        }
        else
            FragColor = sample_vram(addr, 0);
    }
    else if (SUPER_SAMPLES >= 4)
    {
        if (super_sample_is_valid(addr))
        {
            uint quad_offset;

            const uint NUM_SSAA_SAMPLES = SUPER_SAMPLES / 4;

            // The swizzling pattern for checkerboard is a bit different.
            if (SUPER_SAMPLES != 8)
                quad_offset = (super_sampled_coord.y & 1u) + (super_sampled_coord.x & 1u) * 2u;
            else
                quad_offset = (super_sampled_coord.x & 1u) + (super_sampled_coord.y & 1u) * 2u;

            uint base_slice = BASE_SSAA_LAYER + NUM_SSAA_SAMPLES * quad_offset;

            FragColor = vec4(0.0);
            for (uint i = 0; i < NUM_SSAA_SAMPLES; i++)
                FragColor += sample_vram(addr, base_slice + i);
            FragColor /= float(NUM_SSAA_SAMPLES);
        }
        else
            FragColor = sample_vram(addr, 0);
    }
}
