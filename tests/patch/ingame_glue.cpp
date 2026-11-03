/* The C++ the built-in patch harness cannot avoid: patches.cpp and the
 * patch parser are C++, with C++ linkage, and the parser links against
 * the memory and loader surface stubbed here. Everything else - the
 * calls, the log, the checks - is in ingame.c. */
#include <string.h>
#include <libretro.h>
#include <vector>
#include <string>

#include "Common.h"
#include "Patch.h"

/* Memory and loader surface Patch.cpp links against; loading from a
 * string enters none of it. */
u8  vtlb_memRead8 (u32) { return 0; }
u16 vtlb_memRead16(u32) { return 0; }
u32 vtlb_memRead32(u32) { return 0; }
u64 vtlb_memRead64(u32) { return 0; }
void vtlb_memWrite8 (u32, u8) { }
void vtlb_memWrite16(u32, u16) { }
void vtlb_memWrite32(u32, u32) { }
void vtlb_memWrite64(u32, u64) { }
void* vtlb_GetPhyPtr(u32) { return 0; }
RETURNS_R128 vtlb_memRead128(u32) { r128 v; memset(&v, 0, sizeof(v)); return v; }
void vtlb_memWrite128(u32, r128) { }
extern "C" void iopMemWrite8 (u32, u8) { }
extern "C" void iopMemWrite16(u32, u16) { }
extern "C" void iopMemWrite32(u32, u32) { }
extern "C" u8  iopMemRead8_slow (u32) { return 0; }
extern "C" u16 iopMemRead16_slow(u32) { return 0; }
extern "C" u32 iopMemRead32_slow(u32) { return 0; }
static uptr s_rlut[0x10000];
const uptr* psxMemRLUT = s_rlut;
extern "C" int path_is_directory(const char*) { return 0; }
struct RDIR;
extern "C" {
	struct RDIR* retro_opendir_include_hidden(const char*, bool) { return 0; }
	bool retro_dirent_error(struct RDIR*) { return true; }
	int  retro_readdir(struct RDIR*) { return 0; }
	const char* retro_dirent_get_name(struct RDIR*) { return ""; }
	bool retro_dirent_is_dir(struct RDIR*, const char*) { return false; }
	void retro_closedir(struct RDIR*) { }
	int  rl_fnmatch(const char*, const char*, int) { return 1; }
	int64_t path_get_size(const char*) { return -1; }
}
extern "C" int64_t filestream_read_file(const char*, void**, int64_t*) { return 0; }
extern "C" {
	void* rinflate_new(void) { return 0; }
	void  rinflate_free(void*) { }
	void  rinflate_set_in(void*, const void*, unsigned) { }
	void  rinflate_set_out(void*, void*, unsigned) { }
	int   rinflate_process(void*, unsigned*, unsigned*) { return -1; }
	int   rinflate(void*, const void*, unsigned, void*, unsigned, unsigned*)
	{ return -1; }
}
namespace Path {
	std::string_view GetFileName(const std::string_view& p) { return p; }
}

int lrps2_ingame_patches(const char *serial, u32 game_crc,
		const char *renderer, bool hint_nointerlacing,
		bool hint_disable_mipmaps, bool hint_game_enhancements,
		int8_t hint_widescreen, int8_t hint_uncapped_framerate,
		int8_t hint_language_unlock);

/* The one option the patches read, pcsx2_fastcdvd, is on. */
static bool test_environ(unsigned cmd, void *data)
{
	if (cmd != RETRO_ENVIRONMENT_GET_VARIABLE)
		return false;
	((struct retro_variable*)data)->value = "enabled";
	return true;
}
retro_environment_t environ_cb = test_environ;
void retro_set_region(unsigned) { }

extern "C" {
/* Every hint on, the given renderer, widescreen and framerate settings;
 * what was loaded is dropped again. */
void ingame_harness_call(const char *serial, unsigned crc,
	const char *renderer, int widescreen, int framerate)
{
	lrps2_ingame_patches(serial, crc, renderer, true, true, true,
		(int8_t)widescreen, (int8_t)framerate, 1);
	ForgetLoadedPatches();
}
}
