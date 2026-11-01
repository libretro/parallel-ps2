/*
 * A shader module's interface, settled in its SPIR-V words. In C.
 *
 * The draw shaders (shaders/tfx.glsl) are compiled ahead of time with
 * every input and output a draw may use; a draw that does without one
 * gets the module with it turned into a private variable, so the driver
 * sees a shader that never declared it. That is three fragment modules in
 * the core instead of one per combination, and no compiler at run time.
 *
 *  - VKSPV_DROP_COLOR1:    the second colour output (location 0, index 1),
 *                          for a draw that does not blend with it;
 *  - VKSPV_DROP_PRIMID:    gl_PrimitiveID, with the geometry capability it
 *                          brings, for a draw with no destination alpha
 *                          test that needs it;
 *  - VKSPV_DROP_FRAGDEPTH: gl_FragDepth and the depth replacing mode, for
 *                          a draw that does not clamp depth (so the
 *                          driver may test depth early);
 *  - VKSPV_DROP_FLAT:      the Flat decoration of block members (the
 *                          colour varying), for a draw that interpolates.
 *
 * A dropped variable may only be loaded or stored: tools/vk_spirv.py
 * checks that of the modules it writes.
 */

#ifndef VK_SPIRV_PATCH_H
#define VK_SPIRV_PATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKSPV_DROP_COLOR1    1u
#define VKSPV_DROP_PRIMID    2u
#define VKSPV_DROP_FRAGDEPTH 4u
#define VKSPV_DROP_FLAT      8u

/* The most words vkspv_patch writes beyond the module's own: a pointer
 * type for each variable it makes private. */
#define VKSPV_PATCH_EXTRA 12u

/* Writes the module in[0..n) with what flags drop to out, which holds
 * n + VKSPV_PATCH_EXTRA words. Returns the words written, or 0 if the
 * module is not SPIR-V this understands (it is then not to be used). */
size_t vkspv_patch(const uint32_t *in, size_t n, unsigned flags, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif
