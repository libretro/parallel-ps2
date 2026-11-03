/* LoadPatchesFromDir and LoadPatchesFromZip against a real folder and
 * real archives, through libretro-common's directory, file and inflate
 * code rather than stubs.
 *
 * The folder holds pnach files for the CRC in either case and with a
 * suffix, one for another CRC, a non-pnach file, a hidden file and a
 * directory named like a pnach: three files load, in whatever order the
 * directory gives, and nothing else. The archives (made by build.sh with
 * Python's zipfile) hold the member deflated, stored under an upper-case
 * name, under a subfolder only, and behind a comment; each loads what it
 * should, and every truncation of one loads nothing rather than reading
 * past it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libretro.h>

#include "Common.h"
#include "Patch.h"

static u8 s_mem[0x10000];
static u32 clamp(u32 a) { return a & 0xFFFF; }
u8  vtlb_memRead8 (u32 a) { return s_mem[clamp(a)]; }
u16 vtlb_memRead16(u32 a) { u16 v; memcpy(&v, s_mem + clamp(a), 2); return v; }
u32 vtlb_memRead32(u32 a) { u32 v; memcpy(&v, s_mem + clamp(a), 4); return v; }
u64 vtlb_memRead64(u32 a) { u64 v; memcpy(&v, s_mem + clamp(a), 8); return v; }
void vtlb_memWrite8 (u32 a, u8  v) { s_mem[clamp(a)] = v; }
void vtlb_memWrite16(u32 a, u16 v) { memcpy(s_mem + clamp(a), &v, 2); }
void vtlb_memWrite32(u32 a, u32 v) { memcpy(s_mem + clamp(a), &v, 4); }
void vtlb_memWrite64(u32 a, u64 v) { memcpy(s_mem + clamp(a), &v, 8); }
void* vtlb_GetPhyPtr(u32 a) { return s_mem + clamp(a); }
RETURNS_R128 vtlb_memRead128(u32) { r128 v; memset(&v, 0, sizeof(v)); return v; }
void vtlb_memWrite128(u32, r128) { }
extern "C" void iopMemWrite8 (u32 a, u8  v) { s_mem[clamp(a)] = v; }
extern "C" void iopMemWrite16(u32, u16) { }
extern "C" void iopMemWrite32(u32, u32) { }
extern "C" u8  iopMemRead8_slow (u32) { return 0; }
extern "C" u16 iopMemRead16_slow(u32) { return 0; }
extern "C" u32 iopMemRead32_slow(u32) { return 0; }
static uptr s_rlut[0x10000];
const uptr* psxMemRLUT = s_rlut;

static void test_log(enum retro_log_level, const char*, ...) { }
extern "C" retro_log_printf_t log_cb = test_log;

static int s_fails;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); s_fails++; } else printf("  ok: %s\n", what); } while (0)

static void put(const char* dir, const char* name, const char* text)
{
	char path[1024];
	FILE* f;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (!f) { perror(path); exit(2); }
	fputs(text, f);
	fclose(f);
}

/* The loaded patches' addresses, as a set: OR of (1 << addr) over them. */
static unsigned addr_set(void)
{
	unsigned set = 0;
	size_t i;
	for (i = 0; i < LoadedPatchCount(); i++)
		set |= 1u << (LoadedPatch(i)->addr & 31);
	return set;
}

static u8* slurp(const char* path, size_t* len)
{
	FILE* f = fopen(path, "rb");
	long n;
	u8* buf;
	if (!f) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (u8*)malloc(n ? n : 1);
	if (fread(buf, 1, n, f) != (size_t)n) { perror(path); exit(2); }
	fclose(f);
	*len = (size_t)n;
	return buf;
}

int main(int argc, char** argv)
{
	char dir[] = "/tmp/pnachload.XXXXXX";
	char sub[1100], empty[1100];
	const char* zipdir = argc > 1 ? argv[1] : ".";

	if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }

	put(dir, "ABCD1234.pnach", "patch=1,EE,00000001,word,1\n");
	put(dir, "abcd1234 - widescreen.pnach", "// a comment\npatch=0,EE,00000002,word,2\r\npatch=1,IOP,00000003,byte,3\n");
	put(dir, "ABCD1234-empty.pnach", "");
	put(dir, "EEEE0000.pnach", "patch=1,EE,00000004,word,4\n");
	put(dir, "ABCD1234.txt", "patch=1,EE,00000005,word,5\n");
	put(dir, ".ABCD1234.pnach", "patch=1,EE,00000006,word,6\n");
	snprintf(sub, sizeof(sub), "%s/ABCD1234 folder.pnach", dir);
	mkdir(sub, 0700);
	put(sub, "ABCD1234.pnach", "patch=1,EE,00000007,word,7\n");
	snprintf(empty, sizeof(empty), "%s/empty", dir);
	mkdir(empty, 0700);

	printf("== pnach files in a folder ==\n");
	ForgetLoadedPatches();
	CHECK(LoadPatchesFromDir("ABCD1234", dir, "Cheats", true) == 3, "three patches load from the folder");
	CHECK(addr_set() == ((1u << 1) | (1u << 2) | (1u << 3)),
		"the CRC's files in either case and with a suffix, not another CRC, a .txt, a hidden file or a folder");
	ForgetLoadedPatches();
	CHECK(LoadPatchesFromDir("abcd1234", dir, "Cheats", true) == 3, "a lower-case CRC matches the same files");
	ForgetLoadedPatches();
	CHECK(LoadPatchesFromDir("EEEE0000", dir, "Cheats", false) == 1 && addr_set() == (1u << 4), "another CRC loads its own");
	ForgetLoadedPatches();
	CHECK(LoadPatchesFromDir("ABCD1234", empty, "Cheats", true) == 0 && LoadedPatchCount() == 0, "an empty folder loads nothing");
	CHECK(LoadPatchesFromDir("ABCD1234", "/nonexistent/pnachload", "Cheats", true) == 0, "a missing folder loads nothing");
	ForgetLoadedPatches();
	LoadPatchesFromDir("ABCD1234", dir, "Cheats", true);
	LoadPatchesFromDir("EEEE0000", dir, "Cheats", true);
	CHECK(LoadedPatchCount() == 4, "loads add to what is loaded until forgotten");
	ForgetLoadedPatches();

	printf("== pnach members in an archive ==\n");
	{
		static const struct { const char* zip; const char* crc; int loads; unsigned set; const char* what; } zips[] = {
			{"deflated.zip", "ABCD1234", 2, (1u << 8) | (1u << 9), "a deflated member loads"},
			{"deflated.zip", "abcd1234", 2, (1u << 8) | (1u << 9), "matched without case"},
			{"stored.zip", "ABCD1234", 1, (1u << 10), "a stored member, named upper-case, loads"},
			{"subdir.zip", "ABCD1234", 0, 0, "a member only under a subfolder does not"},
			{"comment.zip", "ABCD1234", 1, (1u << 11), "an archive with a comment after the directory loads"},
			{"deflated.zip", "EEEE0000", 0, 0, "a CRC the archive does not have loads nothing"},
		};
		size_t z;
		for (z = 0; z < sizeof(zips) / sizeof(zips[0]); z++)
		{
			char path[1100];
			size_t len;
			u8* data;
			snprintf(path, sizeof(path), "%s/%s", zipdir, zips[z].zip);
			data = slurp(path, &len);
			ForgetLoadedPatches();
			CHECK(LoadPatchesFromZip(zips[z].crc, data, len) == zips[z].loads && addr_set() == zips[z].set, zips[z].what);
			free(data);
		}
	}

	/* Every truncation and a byte flipped at every offset: no load reads
	 * past the buffer (ASan's lane checks that) and a short archive loads
	 * nothing. */
	{
		char path[1100];
		size_t len, cut, bad = 0;
		u8* data;
		snprintf(path, sizeof(path), "%s/deflated.zip", zipdir);
		data = slurp(path, &len);
		for (cut = 0; cut < len; cut++)
		{
			u8* copy = (u8*)malloc(cut ? cut : 1);
			memcpy(copy, data, cut);
			ForgetLoadedPatches();
			if (LoadPatchesFromZip("ABCD1234", copy, cut) != 0)
				bad++;
			free(copy);
		}
		CHECK(bad == 0, "every truncated archive loads nothing");
		for (cut = 0; cut < len; cut++)
		{
			u8* copy = (u8*)malloc(len);
			memcpy(copy, data, len);
			copy[cut] ^= 0xA5;
			ForgetLoadedPatches();
			LoadPatchesFromZip("ABCD1234", copy, len);
			free(copy);
		}
		CHECK(1, "an archive with any one byte damaged is read within its bounds");
		CHECK(LoadPatchesFromZip("ABCD1234", NULL, 0) == 0, "no archive loads nothing");
		free(data);
	}
	ForgetLoadedPatches();

	{
		char cmd[1200];
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
		if (system(cmd) != 0)
			printf("  (could not remove %s)\n", dir);
	}

	printf("pnachload: %s\n", s_fails ? "FAIL" : "ok");
	return s_fails ? 1 : 0;
}
