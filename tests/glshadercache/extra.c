/* The GL the shader cache adds to the program stubs: the binary formats
 * query and the extension flag */

#include "glad.h"

int GLAD_GL_ARB_get_program_binary = 1;
GLint g_binary_formats = 1;

static void APIENTRY s_GetIntegerv(GLenum pname, GLint* out)
{
	*out = pname == GL_NUM_PROGRAM_BINARY_FORMATS ? g_binary_formats : 0;
}

PFNGLGETINTEGERVPROC glad_glGetIntegerv = s_GetIntegerv;
