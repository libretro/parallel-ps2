/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GS_GL_DEVICE_H
#define GS_GL_DEVICE_H

#include "glad.h"
#include "GS/Renderers/Common/GSTextureBase.h"
#include "GLStreamBuffer.h"

/* What the GL device's C parts (textures) ask of it */

#ifdef __cplusplus
extern "C" {
#endif

/* Does a clear the texture still has pending */
void gs_gl_device_commit_clear(gs_texture* t, int use_write_fbo);
/* Detaches it from the framebuffer before it goes */
void gs_gl_device_unbind_texture(gs_texture* t);
/* The PBO uploads go through, or NULL */
gs_gl_stream_buffer* gs_gl_device_upload_buffer(void);
int gs_gl_device_colclip_float(void);
int gs_gl_device_framebuffer_fetch(void);
int gs_gl_device_download_pbo_disabled(void);
/* Finishes the GL where a CPU read would otherwise read stale */
void gs_gl_device_finish_before_cpu_read(void);
GLuint gs_gl_device_fbo_read(void);
/* The frontend's framebuffer */
GLuint gs_gl_device_default_framebuffer(void);

#ifdef __cplusplus
}
#endif

#endif
