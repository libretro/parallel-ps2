/* What the built-in patch harness links against besides patches.c and
 * Patch.c: the memory and loader surface, which loading from a string
 * enters none of. Everything else - the calls, the log, the checks - is
 * in ingame.c. */
#include <string.h>
#include <libretro.h>

#include "Common.h"
#include "IopMem.h"
#include "Patch.h"
#include "../libretro/patches.h"

u8  vtlb_memRead8 (u32 a) { (void)a; return 0; }
u16 vtlb_memRead16(u32 a) { (void)a; return 0; }
u32 vtlb_memRead32(u32 a) { (void)a; return 0; }
u64 vtlb_memRead64(u32 a) { (void)a; return 0; }
void vtlb_memWrite8 (u32 a, u8  v) { (void)a; (void)v; }
void vtlb_memWrite16(u32 a, u16 v) { (void)a; (void)v; }
void vtlb_memWrite32(u32 a, u32 v) { (void)a; (void)v; }
void vtlb_memWrite64(u32 a, u64 v) { (void)a; (void)v; }
void* vtlb_GetPhyPtr(u32 a) { (void)a; return NULL; }
void iopMemWrite8 (u32 a, u8  v) { (void)a; (void)v; }
void iopMemWrite16(u32 a, u16 v) { (void)a; (void)v; }
void iopMemWrite32(u32 a, u32 v) { (void)a; (void)v; }
u8  iopMemRead8_slow (u32 a) { (void)a; return 0; }
u16 iopMemRead16_slow(u32 a) { (void)a; return 0; }
u32 iopMemRead32_slow(u32 a) { (void)a; return 0; }
static uptr s_rlut[0x10000];
const uptr* psxMemRLUT = s_rlut;

struct RDIR;
int path_is_directory(const char* p) { (void)p; return 0; }
struct RDIR* retro_opendir_include_hidden(const char* n, bool h) { (void)n; (void)h; return NULL; }
bool retro_dirent_error(struct RDIR* d) { (void)d; return true; }
int  retro_readdir(struct RDIR* d) { (void)d; return 0; }
const char* retro_dirent_get_name(struct RDIR* d) { (void)d; return ""; }
bool retro_dirent_is_dir(struct RDIR* d, const char* p) { (void)d; (void)p; return false; }
void retro_closedir(struct RDIR* d) { (void)d; }
int  rl_fnmatch(const char* a, const char* b, int f) { (void)a; (void)b; (void)f; return 1; }
int64_t path_get_size(const char* p) { (void)p; return -1; }
int64_t filestream_read_file(const char* p, void** b, int64_t* l) { (void)p; (void)b; (void)l; return 0; }
void* rinflate_new(void) { return NULL; }
void  rinflate_free(void* z) { (void)z; }
void  rinflate_set_in(void* z, const void* in, unsigned n) { (void)z; (void)in; (void)n; }
void  rinflate_set_out(void* z, void* out, unsigned n) { (void)z; (void)out; (void)n; }
int   rinflate_process(void* z, unsigned* r, unsigned* w) { (void)z; (void)r; (void)w; return -1; }

/* Every hint on, fast CDVD on so its patches load too, the given
 * renderer, widescreen and framerate settings; what was loaded is
 * dropped again. */
void ingame_harness_call(const char *serial, unsigned crc,
	const char *renderer, int widescreen, int framerate)
{
	lrps2_ingame_patches(serial, crc, renderer, true, true, true,
		(int8_t)widescreen, (int8_t)framerate, 1, true);
	ForgetLoadedPatches();
}
