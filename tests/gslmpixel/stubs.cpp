/* What GSLocalMemory needs of the emulator besides its backing (the
 * core's own, GS/GSWrappedMemory.c): the palette paletted texels read
 * through, filled with known colours. */
#include <cstring>
#include "GS/GSLocalMemory.h"
#include <libretro.h>

static void RETRO_CALLCONV quiet(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}
extern "C" retro_log_printf_t log_cb;
retro_log_printf_t log_cb = quiet;

static u32 s_palette[512];

/* GSClut's C half, with a fixed palette and no device */
extern "C" void gs_clut_init(gs_clut* c)
{
	memset(c, 0, sizeof(*c));
	for (u32 i = 0; i < 512; i++)
		s_palette[i] = i * 0x9E3779B9u ^ 0x5A5A5A5Au;
	c->buff32 = s_palette;
}
extern "C" void gs_clut_deinit(gs_clut*) {}
extern "C" void gs_clut_gpu_create(gs_clut*) {}
extern "C" void gs_clut_gpu_release(gs_clut*) {}

#include "GS/GSTables.h"
#include "GS/GSPsm.h"
static const struct gslmpixel_tables
{
	gslmpixel_tables()
	{
		gs_tables_init();
		gs_psm_init();
	}
} s_tables;
