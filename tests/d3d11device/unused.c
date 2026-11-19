/* What the shader cache's file side links against and, its files off
 * (DisableShaderCache), never calls: each fails the run if it is. */
#include <stdio.h>
#include <stdlib.h>

#define UNUSED(name) \
	void name(void) \
	{ \
		printf("d3d11device: FAIL: %s called\n", #name); \
		exit(1); \
	}

UNUSED(fill_pathname_join)
UNUSED(strlcpy_retro__)
UNUSED(strlcat_retro__)
UNUSED(filestream_open)
UNUSED(filestream_read)
UNUSED(filestream_write)
UNUSED(filestream_close)
UNUSED(filestream_seek)
UNUSED(filestream_tell)
UNUSED(filestream_get_size)
UNUSED(filestream_delete)
UNUSED(filestream_flush)
UNUSED(filestream_eof)
UNUSED(path_is_valid)
UNUSED(path_mkdir)
