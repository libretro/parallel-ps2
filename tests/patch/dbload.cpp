/*  The whole game database, through the real parser.
 *
 *  Companion to dbparse.cpp, which covers the patch lines. This covers
 *  everything else in bin/resources/GameIndex.yaml: the game fixes, speed
 *  hacks, GS hardware fixes, rounding and clamping modes and the memory
 *  card filters.
 *
 *  The failure it exists to catch is the same one that hid the "bytes"
 *  operand size. A name the parser does not recognise is reported with a
 *  console error and then dropped -- the entry loses its fix and the game
 *  quietly misbehaves, with nothing failing and nothing to notice. A
 *  rename on either side of the database, or a fix removed from the enum
 *  while entries still ask for it, looks exactly like that.
 *
 *  So this parses every entry and requires no errors and no warnings. It
 *  links pcsx2/GameDatabase.cpp and the real YAML reader rather than
 *  reimplementing either.
 *
 *  What it catches, checked by corrupting the database and re-running:
 *  renaming a gamefix so the parser no longer knows it -- "XGKickHackk"
 *  -- is reported and fails the run. That is the WRC-class failure, a
 *  name in the database that the code has stopped recognising.
 *
 *  What it does not catch, and both are worth knowing:
 *    - a renamed key. Changing "speedHacks:" to "speedHackss:" makes the
 *      whole block invisible and the parser says nothing, because an
 *      unrecognised key is skipped rather than reported. Every entry
 *      under it is silently lost.
 *    - an out-of-range value. "autoFlush: 99" loads without complaint;
 *      the numeric gsHWFixes are range-checked nowhere.
 *
 *  Both are real silent failures this does not close. Catching them means
 *  the parser reporting unknown keys and validating ranges, which is a
 *  change to GameDatabase.cpp rather than to a test.
 *
 *  Usage: tests/patch/dbload <path-to-GameIndex.yaml>
 */

#include <stdio.h>
#include <libretro.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <vector>

#include "Common.h"
#include "GameDatabase.h"

static int g_errors   = 0;
static int g_warnings = 0;
static int g_shown    = 0;

static void note(const char* kind, const char* fmt, va_list a)
{
	if (g_shown++ < 8)
	{
		printf("  %s: ", kind);
		vprintf(fmt, a);
		printf("\n");
	}
}

static void test_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list a;
	if (level != RETRO_LOG_ERROR && level != RETRO_LOG_WARN)
		return;
	va_start(a, fmt);
	if (level == RETRO_LOG_ERROR) { g_errors++;   note("error", fmt, a); }
	else                          { g_warnings++; note("warning", fmt, a); }
	va_end(a);
}
extern "C" retro_log_printf_t log_cb = test_log;

/* The database reads its yaml through this. */
static std::string s_yaml;
namespace Host
{
	std::optional<std::string> ReadResourceFileToString(const char* name)
	{
		(void)name;
		return s_yaml;
	}
}

/* The three gsHWFixes fields whose values are function names --
 * getSkipCount, beforeDraw and moveHandler -- are looked up in tables that
 * live in GSHwHack.cpp, and linking that pulls in the GS renderer: sixty
 * more symbols for three fields. They are stubbed to a valid id here, so
 * this test does not validate those names. That leaves 349 lines in the
 * database unchecked, and they are the ones most likely to break on a GS
 * refactor, so it is worth knowing they are not covered rather than
 * assuming a green run includes them. */
s16 GSLookupGetSkipCountFunctionId(const char*, size_t)
{ return 0; }
s16 GSLookupBeforeDrawFunctionId(const char*, size_t)
{ return 0; }
s16 GSLookupMoveHandlerFunctionId(const char*, size_t)
{ return 0; }

/* Memory card naming, reached for the memcardFilters field. */
void FileMcd_GetDefaultName(char* buf, size_t n, unsigned) { if (n) buf[0] = 0; }
unsigned FileMcd_GetMtapPort(unsigned) { return 0; }
unsigned FileMcd_GetMtapSlot(unsigned) { return 0; }
bool FileMcd_IsMultitapSlot(unsigned) { return false; }

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "bin/resources/GameIndex.yaml";
	FILE* f = fopen(path, "rb");
	long size;
	int entries = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
	s_yaml.resize((size_t)size);
	if (fread(&s_yaml[0], 1, (size_t)size, f) != (size_t)size)
	{ fclose(f); fprintf(stderr, "short read\n"); return 2; }
	fclose(f);

	GameDatabase::ensureLoaded();

	/* Spot-check that entries actually came out, so an empty parse cannot
	 * pass by reporting nothing. WRC 4's serial is the one this suite grew
	 * out of; the other two are an ordinary retail entry and one carrying
	 * a gsHWFixes block. */
	{
		static const char* kSerials[] = {
			"SCES-52389",   /* WRC 4, gameFixes + patches */
			"SLUS-20062",   /* a plain entry */
			"SCED-52880",   /* WRC 4 demo, gsHWFixes + roundModes */
		};
		size_t i;
		for (i = 0; i < sizeof(kSerials)/sizeof(kSerials[0]); i++)
		{
			const GameDatabaseSchema::GameEntry* e =
				GameDatabase::findGame(kSerials[i]);
			if (e) entries++;
			else
			{
				printf("  %s is not in the loaded database\n", kSerials[i]);
				g_errors++;
			}
		}
	}

	printf("dbload: %d errors, %d warnings, %d/3 spot-checked serials found\n",
	       g_errors, g_warnings, entries);
	return (g_errors != 0 || g_warnings != 0 || entries != 3);
}

std::string libretro_content;


/* Reached from Pcsx2Config's paths and the memory map; neither is entered
 * by a database load. */
extern "C" {
	/* path_is_valid and path_mkdir are the two file_path.c leaves out of
	 * this build; the rest of that file links. */
	int path_is_valid(const char*) { return 0; }
	int path_mkdir(const char*) { return 0; }
}
void vtlb_Alloc_Ppmap(void) { }
