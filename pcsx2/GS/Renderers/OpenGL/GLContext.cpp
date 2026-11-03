/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include "GLContext.h"
#include "common/Pcsx2Defs.h"
#include "glad.h"

#include <cstdlib>
#include <cstring>
#ifdef __APPLE__
#include <stdlib.h>
#else
#include <malloc.h>
#endif

#include <libretro.h>
extern retro_hw_render_callback hw_render;

GLContext::GLContext() { }
GLContext::~GLContext() = default;

static void *gl_retro_proc_addr(const char *name)
{
	return (void*)(hw_render.get_proc_address(name));
}

GLContext* GLContext::Create()
{
	GLContext* context = new GLContext();

	/* The profile is the context type the frontend accepted, and it picks
	 * the loader: the desktop one would read an ES context's version as a
	 * desktop GL one and leave every ES-only entry point null. */
	context->m_gles = (hw_render.context_type == RETRO_HW_CONTEXT_OPENGLES3 ||
		hw_render.context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION);

	if (!(context->m_gles ? gladLoadGLES2Loader(gl_retro_proc_addr) : gladLoadGLLoader(gl_retro_proc_addr)))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to load %s functions for GLAD\n", context->m_gles ? "OpenGL ES" : "GL");
		delete context;
		return nullptr;
	}

	return context;
}
