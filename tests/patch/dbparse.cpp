/*  Every patch line in the game database, through the real parser.
 *
 *  A pnach line the parser does not understand is rejected with a console
 *  error and nothing else happens: no failure, no test, and the game
 *  quietly misbehaves. That is how the "bytes" operand size went
 *  unimplemented while six WRC 4 serials carried patches written in it --
 *  the patches were in GameIndex.yaml, the parser refused them, and the
 *  only sign was a line in a log nobody reads.
 *
 *  So this feeds bin/resources/GameIndex.yaml's patch blocks to
 *  LoadPatchesFromString and requires that every line is accepted. It
 *  links pcsx2/Patch.cpp rather than reimplementing the format, so it
 *  cannot drift from what the emulator actually parses.
 *
 *  What it checks:
 *    - every "patch=" line loads,
 *    - the count loaded matches the count in the file, so a line that is
 *      silently dropped rather than refused is caught too,
 *    - author= and comment= lines are tolerated, since they share the
 *      blocks.
 *
 *  What it does not check: whether a patch is correct for its game -- that
 *  needs the game -- and any operand size the database does not currently
 *  use. Deleting "ledouble" from the parser leaves this at 854 of 854,
 *  because nothing in GameIndex.yaml is written in it. The test tracks
 *  what the database contains, so it catches a type going missing only
 *  once some entry depends on it, which is exactly when it starts to
 *  matter. Deleting "bytes" costs 12 lines and "extended" costs 28.
 *
 *  Usage: tests/patch/dbparse <path-to-GameIndex.yaml>
 */

#include <stdio.h>
#include <libretro.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <vector>
#include <string>

#include "Common.h"
#include "Patch.h"

/* Memory the patch application would reach. The parse path does not use
 * it, but _ApplyPatch does, and applying is how a bytes payload is
 * checked for having survived the parse. */
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

/* The loader's other surface; none of it is entered from a string. */
extern "C" int path_is_directory(const char*) { return 0; }
#include "HostFS.h"
namespace FileSystem {
	bool FindFiles(const char*, const char*, unsigned,
	               std::vector<FILESYSTEM_FIND_DATA>*) { return false; }
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

/* Errors are counted rather than printed one per line: a broken operand
 * size fails hundreds at once and the first few say everything. */
static int g_errors = 0;
static int g_shown  = 0;
static void test_log(enum retro_log_level level, const char* fmt, ...)
{
	if (level != RETRO_LOG_ERROR)
		return;
	g_errors++;
	if (g_shown++ < 5)
	{
		va_list a;
		va_start(a, fmt);
		printf("  ");
		vprintf(fmt, a);   /* the format carries its own newline now */
		va_end(a);
	}
}
extern "C" retro_log_printf_t log_cb = test_log;

extern std::vector<IniPatch> Patch;

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "bin/resources/GameIndex.yaml";
	FILE* f = fopen(path, "r");
	char buf[1024];
	std::string block;
	int in_content = 0, file_lines = 0, blocks = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

	/* The patch text sits under "content: |-" as an indented literal
	 * block. Any line indented further than the key belongs to it; the
	 * first line that is not ends the block. */
	while (fgets(buf, sizeof(buf), f))
	{
		const char* p = buf;
		int indent = 0;
		while (*p == ' ') { p++; indent++; }

		if (strstr(buf, "content: |-"))
		{ in_content = 1; blocks++; continue; }

		if (in_content)
		{
			if (*p == '\n' || *p == '\0') continue;
			if (indent < 8) { in_content = 0; }
			else
			{
				if (!strncmp(p, "patch=", 6)) file_lines++;
				block += p;
				continue;
			}
		}
	}
	fclose(f);

	{
		const int loaded = LoadPatchesFromString(block.c_str());
		size_t i, applied = 0;

		printf("  %d patch lines in %d blocks, %d loaded, %d refused\n",
		       file_lines, blocks, loaded, g_errors);

		/* Applying them all is what proves a bytes payload survived the
		 * parse: a line that loaded but kept an empty payload writes
		 * nothing. */
		for (i = 0; i < Patch.size(); i++)
		{
			_ApplyPatch(&Patch[i]);
			if (Patch[i].type == 9 /* BYTES_T */ && Patch[i].bytes.empty())
			{
				printf("  a bytes patch at %08x parsed with no payload\n",
				       Patch[i].addr);
				g_errors++;
			}
			applied++;
		}

		if (loaded != file_lines)
		{
			printf("  %d lines in the file but %d loaded: some were dropped"
			       " without an error\n", file_lines, loaded);
			g_errors++;
		}

		printf("dbparse: %d/%d game database patch lines accepted\n",
		       loaded, file_lines);
		(void)applied;
	}

	/* Lines the database does not have but a pnach can: spacing around
	 * the '=' and the commas, a trailing comment, a line with no '=', a
	 * place out of range, a bytes payload that is not hex or not whole
	 * bytes, and numbers past their field. Each says whether it loads and,
	 * when it does, what it loads as. */
	{
		static const struct
		{
			const char* line;
			int loads;
			int place;
			u32 addr;
			u64 data;
			const char* bytes; /* hex of the payload, for a bytes line */
		} cases[] = {
			{"patch=1,EE,00100000,word,0000002A", 1, 1, 0x00100000u, 0x2Au, nullptr},
			{"  patch = 0 , EE , 2010abcd , word , DEADBEEF   // note", 1, 0, 0x2010abcdu, 0xDEADBEEFu, nullptr},
			{"patch=2,IOP,0001ffff,byte,ff", 1, 2, 0x0001ffffu, 0xffu, nullptr},
			{"patch=1,EE,00100000,double,123456789abcdef0", 1, 1, 0x00100000u, 0x123456789abcdef0ull, nullptr},
			{"patch=1,EE,00100000,bytes,00a1FF7e", 1, 1, 0x00100000u, 0, "00a1ff7e"},
			{"patch 1,EE,00100000,word,1", 0, 0, 0, 0, nullptr},
			{"// patch=1,EE,00100000,word,1", 0, 0, 0, 0, nullptr},
			{"patch=", 0, 0, 0, 0, nullptr},
			{"patch=3,EE,00100000,word,1", 0, 0, 0, 0, nullptr},
			{"patch=x,EE,00100000,word,1", 0, 0, 0, 0, nullptr},
			{"patch=-1,EE,00100000,word,1", 0, 0, 0, 0, nullptr},
			{"patch=1,EE,00100000,bytes,0g", 0, 0, 0, 0, nullptr},
			{"patch=1,EE,00100000,bytes,abc", 0, 0, 0, 0, nullptr},
			{"patch=1,ARM,00100000,word,1", 0, 0, 0, 0, nullptr},
			{"patch=1,EE,00100000,dword,1", 0, 0, 0, 0, nullptr},
			{"patch=1,EE,00100000,word", 0, 0, 0, 0, nullptr},
			/* An address past 32 bits is no address, so zero, as before. */
			{"patch=1,EE,100000000,word,1", 1, 1, 0, 1, nullptr},
			/* A value past 64 bits likewise. */
			{"patch=1,EE,00100000,word,10000000000000000", 1, 1, 0x00100000u, 0, nullptr},
		};
		const int db_errors = g_errors;
		int case_fails = 0;
		g_shown = 5; /* the refusals below are expected; do not print them */
		size_t c;
		for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
		{
			int loaded;
			bool ok;
			ForgetLoadedPatches();
			loaded = LoadPatchesFromString(cases[c].line);
			ok = (loaded == cases[c].loads) && (Patch.size() == (size_t)cases[c].loads);
			if (ok && cases[c].loads)
			{
				const IniPatch& ip = Patch[0];
				ok = ip.placetopatch == cases[c].place && ip.addr == cases[c].addr
					&& ip.data == cases[c].data;
				if (ok && cases[c].bytes)
				{
					char hex[64] = "";
					size_t b;
					for (b = 0; b < ip.bytes.size() && b * 2 + 2 < sizeof(hex); b++)
						snprintf(hex + b * 2, 3, "%02x", ip.bytes[b]);
					ok = strcmp(hex, cases[c].bytes) == 0;
				}
			}
			if (!ok)
			{
				printf("  FAIL: \"%s\" loaded %d (want %d)", cases[c].line, loaded, cases[c].loads);
				if (Patch.size())
					printf(": place %d addr %08x data %llx", Patch[0].placetopatch,
						Patch[0].addr, (unsigned long long)Patch[0].data);
				printf("\n");
				case_fails++;
			}
		}
		ForgetLoadedPatches();
		g_errors = db_errors + case_fails;
		printf("dbparse: %d of %d pnach edge cases as expected\n",
			(int)(sizeof(cases) / sizeof(cases[0])) - case_fails,
			(int)(sizeof(cases) / sizeof(cases[0])));
	}

	return (g_errors != 0 || file_lines == 0);
}
