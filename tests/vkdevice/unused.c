/* What the device links against and, its files off (DisableShaderCache)
 * and the GS not opened by a frontend's negotiation, never calls: each
 * fails the run if it is. */
#include <stdio.h>
#include <stdlib.h>

#define UNUSED(name) \
	void name(void) \
	{ \
		printf("vkdevice: FAIL: %s called\n", #name); \
		exit(1); \
	}

UNUSED(fill_pathname_join)
UNUSED(filestream_delete)
UNUSED(filestream_read_file)
UNUSED(filestream_write_file_atomic)
UNUSED(path_is_valid)
UNUSED(mtgs_is_open)
UNUSED(mtgs_try_open_gs)
UNUSED(mtgs_close_gs)
