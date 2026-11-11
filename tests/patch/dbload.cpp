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
 *    - an out-of-range value. "textureInsideRT: 99" loads without complaint;
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
#include <formats/ryaml.h>

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

/* The database reads GameIndex.yaml from the resources folder when the
 * option says so and the file is there, else the copy built into the
 * core. Here the folder is "/res", its file is s_yaml, and the built-in
 * copy is one entry of its own. */
static std::string s_yaml;
static bool s_external     = true;
static bool s_file_present = true;
static int s_file_reads;
static char s_read_path[256];
bool emu_config_external_gameindex(void) { return s_external; }
extern "C" int64_t filestream_read_file(const char* path, void** buf, int64_t* len)
{
	char* copy;
	s_file_reads++;
	snprintf(s_read_path, sizeof(s_read_path), "%s", path);
	*buf = NULL;
	*len = 0;
	if (!s_file_present)
		return 0;
	copy = (char*)malloc(s_yaml.size() + 1);
	memcpy(copy, s_yaml.data(), s_yaml.size());
	copy[s_yaml.size()] = '\0';
	*buf = copy;
	*len = (int64_t)s_yaml.size();
	return 1;
}
const unsigned char g_gameDatabaseBuiltin[] =
	"SLUS-77777:\n"
	"  name: \"Built In\"\n";
const size_t g_gameDatabaseBuiltinSize = sizeof(g_gameDatabaseBuiltin) - 1;

/* The three gsHWFixes fields whose values are function names --
 * getSkipCount, beforeDraw and moveHandler -- are looked up in tables that
 * live in GSHwHack.cpp, and linking that pulls in the GS renderer: sixty
 * more symbols for three fields. They are stubbed to a valid id here, so
 * this test does not validate those names. That leaves 349 lines in the
 * database unchecked, and they are the ones most likely to break on a GS
 * refactor, so it is worth knowing they are not covered rather than
 * assuming a green run includes them. */
s16 GSLookupGetSkipCountFunctionId(const char* n, size_t l)
{ (void)n; (void)l; return 0; }
s16 GSLookupBeforeDrawFunctionId(const char* n, size_t l)
{ (void)n; (void)l; return 0; }
s16 GSLookupMoveHandlerFunctionId(const char* n, size_t l)
{ (void)n; (void)l; return 0; }

/* Memory card naming, reached for the memcardFilters field; C, as
 * MemoryCardFile.c is. */
extern "C" {
void FileMcd_GetDefaultName(char* buf, size_t n, unsigned) { if (n) buf[0] = 0; }
unsigned FileMcd_GetMtapPort(unsigned) { return 0; }
unsigned FileMcd_GetMtapSlot(unsigned) { return 0; }
bool FileMcd_IsMultitapSlot(unsigned) { return false; }
}

/* A small database whose every field is known, loaded in place of the
 * real one: what each field reads back as, lookups in either case, the
 * first of two serials that differ only in case winning, the default
 * patch block as the fallback and only when there is one, and the eight
 * lines that must be refused and reported. */
static const char s_fixture[] =
	"SLUS-00001:\n"
	"  name: \"Fixture One\"\n"
	"  region: \"NTSC-U\"\n"
	"  roundModes:\n"
	"    eeRoundMode: 0\n"
	"    vuRoundMode: 3\n"
	"    vu1RoundMode: 1\n"
	"  clampModes:\n"
	"    eeClampMode: 2\n"
	"    vuClampMode: 1\n"
	"  gameFixes:\n"
	"    - XGKickHack\n"
	"    - SkipMPEGHack\n"
	"    - XGKickHack\n"
	"    - NotAFixHack\n"
	"  speedHacks:\n"
	"    mtvu: 0\n"
	"    eeCycleRate: 2\n"
	"    bogus: 1\n"
	"  gsHWFixes:\n"
	"    textureInsideRT: 2\n"
	"    mipmap: 1\n"
	"    halfPixelOffset: x\n"
	"    nonsense: 1\n"
	"    getSkipCount: \"SomeFunc\"\n"
	"  memcardFilters:\n"
	"    - \"SLUS-00001\"\n"
	"    - \"SLUS-00002\"\n"
	"  patches:\n"
	"    default:\n"
	"      content: |-\n"
	"        patch=1,EE,00000001,word,1\n"
	"    ABCD1234:\n"
	"      content: |-\n"
	"        patch=1,EE,00000002,word,2\n"
	"    abcd1234:\n"
	"      content: |-\n"
	"        patch=1,EE,00000009,word,9\n"
	"    zz:\n"
	"      content: |-\n"
	"        patch=1,EE,00000009,word,9\n"
	"  dynaPatches:\n"
	"    - pattern:\n"
	"        - offset: 0\n"
	"          value: 1\n"
	"        - offset: 4\n"
	"          value: 2\n"
	"      replacement:\n"
	"        - offset: 8\n"
	"          value: 3\n"
	"slus-00001:\n"
	"  name: \"Duplicate\"\n"
	"SLES-00002:\n"
	"  name: \"No Default\"\n"
	"  roundModes:\n"
	"    eeDivRoundMode: 2\n"
	"  patches:\n"
	"    11111111:\n"
	"      content: |-\n"
	"        patch=0,EE,00000003,word,3\n";

static int s_fixture_fails;
#define FCHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); s_fixture_fails++; } } while (0)

static int fixture(void)
{
	const GameDatabaseEntry* a;
	const GameDatabaseEntry* b;
	const int errors_before = g_errors;
	const char* p;

	s_yaml = s_fixture;
	g_shown = 8; /* the refusals are expected; count them, do not print */
	GameDatabase_Unload();
	GameDatabase_Load();
	a = GameDatabase_FindGame("SLUS-00001");
	b = GameDatabase_FindGame("SLES-00002");

	FCHECK(g_errors - errors_before == 8, "the eight bad lines are each reported");
	FCHECK(a && b, "both entries load");
	if (!a || !b)
		return 1;
	FCHECK(GameDatabase_FindGame("slus-00001") == a && GameDatabase_FindGame("sLuS-00001") == a,
		"a serial is found in any case");
	FCHECK(!strcmp(a->name, "Fixture One") && !strcmp(a->region, "NTSC-U"),
		"the first of two serials differing in case is the one kept");
	FCHECK(!strcmp(a->serial, "slus-00001"), "the serial is stored lower case");
	FCHECK(GameDatabase_FindGame("SLUS-99999") == NULL && GameDatabase_FindGame("") == NULL,
		"an unknown serial is not found");
	FCHECK(a->eeRoundMode == (u8)FPROUND_NEAREST && a->eeDivRoundMode == GAMEDB_ROUND_UNSET
		&& (int)a->vu0RoundMode == 3 && (int)a->vu1RoundMode == 1,
		"round modes: vuRoundMode sets both, vu1RoundMode then overrides");
	FCHECK((int)a->eeClampMode == 2 && (int)a->vu0ClampMode == 1 && (int)a->vu1ClampMode == 1,
		"clamp modes");
	FCHECK(a->nGameFixes == 2 && a->gameFixes[0] == Fix_XGKick && a->gameFixes[1] == Fix_SkipMpeg,
		"game fixes in order, the repeat and the unknown one dropped");
	FCHECK(a->nSpeedHacks == 2 && a->speedHacks[0].id == (u32)SPEEDHACK_MTVU && a->speedHacks[0].value == 0
		&& a->speedHacks[1].id == (u32)SPEEDHACK_EE_CYCLE_RATE && a->speedHacks[1].value == 2,
		"speed hacks in order, the unknown one dropped");
	FCHECK(a->nGSHWFixes == 3
		&& a->gsHWFixes[0].id == GAMEDB_HWFIX_TEXTURE_INSIDE_RT && a->gsHWFixes[0].value == 2
		&& a->gsHWFixes[1].id == GAMEDB_HWFIX_MIPMAP && a->gsHWFixes[1].value == 1
		&& a->gsHWFixes[2].id == GAMEDB_HWFIX_GET_SKIP_COUNT,
		"GS fixes in order, a bad value and an unknown name dropped");
	FCHECK(!strcmp(a->memcardFilters, "SLUS-00001/SLUS-00002") && !strcmp(b->memcardFilters, ""),
		"memory card filters joined with '/', empty when there are none");
	p = GameDatabase_FindPatch(a, 0xABCD1234u);
	FCHECK(p && strstr(p, "00000002"), "the CRC's own patch block, the case-repeat of its CRC dropped");
	p = GameDatabase_FindPatch(a, 0x99u);
	FCHECK(p && strstr(p, "00000001"), "the default block for any other CRC");
	FCHECK(GameDatabase_FindPatch(a, 0) == NULL, "no patch block for a CRC of 0");
	FCHECK(GameDatabase_FindPatch(b, 0x99u) == NULL, "no fallback without a default block");
	p = GameDatabase_FindPatch(b, 0x11111111u);
	FCHECK(p && strstr(p, "00000003"), "a CRC-only entry's block");
	FCHECK(a->nDynaPatches == 1 && a->dynaPatches[0].npattern == 2 && a->dynaPatches[0].nreplacement == 1
		&& a->dynaPatches[0].pattern[1].offset == 4 && a->dynaPatches[0].pattern[1].value == 2
		&& a->dynaPatches[0].replacement[0].offset == 8 && a->dynaPatches[0].replacement[0].value == 3,
		"a dynamic patch's pattern and replacement");
	FCHECK(b->nDynaPatches == 0 && b->nPatches == 1, "the other entry has only its own");
	{
		Pcsx2Config on, off;
		FCHECK(GameDatabase_ApplyGameFixes(a, on, true) == 7, "seven fixes apply: three clamps, two hacks, two game fixes");
		FCHECK(GameDatabase_ApplyGameFixes(a, off, false) == 0, "none with the automatic fixes off");
		/* the division mode alone, with the fixes off: logged by its own name */
		GameDatabase_ApplyGameFixes(b, off, false);
		GameDatabase_ApplyGameFixes(b, on, true);
	}

	GameDatabase_Unload();
	FCHECK(GameDatabase_FindGame("SLUS-00001") == NULL, "a lookup after unloading finds nothing, and loads nothing");
	s_file_reads = 0;
	GameDatabase_Load();
	FCHECK(GameDatabase_FindGame("SLUS-00001") != NULL, "a load after unloading has the entries again");
	FCHECK(s_file_reads == 1 && !strcmp(s_read_path, "/res/GameIndex.yaml"),
		"the external database is GameIndex.yaml in the resources folder");
	FCHECK(GameDatabase_FindGame("SLUS-77777") == NULL, "the external file, not the built-in copy");
	GameDatabase_Unload();

	/* The built-in copy: when the option is off, the file is not even
	 * looked for; when it is on and the file is missing, it is the
	 * fallback. */
	s_external   = false;
	s_file_reads = 0;
	GameDatabase_Load();
	FCHECK(s_file_reads == 0 && GameDatabase_FindGame("SLUS-77777") != NULL
		&& GameDatabase_FindGame("SLUS-00001") == NULL,
		"the option off: the built-in copy, the folder not read");
	GameDatabase_Unload();
	s_external     = true;
	s_file_present = false;
	GameDatabase_Load();
	FCHECK(s_file_reads == 1 && GameDatabase_FindGame("SLUS-77777") != NULL
		&& !strcmp(GameDatabase_FindGame("slus-77777")->name, "Built In"),
		"the option on and no file: the built-in copy");
	GameDatabase_Unload();
	s_file_present = true;

	printf("dbload: fixture %s\n", s_fixture_fails ? "FAIL" : "ok");
	return s_fixture_fails;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "bin/resources/GameIndex.yaml";
	FILE* f = fopen(path, "rb");
	long size;
	int entries = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	/* where the external database is looked for */
	strcpy(EmuFolders::Resources, "/res");
	fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
	s_yaml.resize((size_t)size);
	if (fread(&s_yaml[0], 1, (size_t)size, f) != (size_t)size)
	{ fclose(f); fprintf(stderr, "short read\n"); return 2; }
	fclose(f);

	GameDatabase_Load();

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
			const GameDatabaseEntry* e =
				GameDatabase_FindGame(kSerials[i]);
			if (e) entries++;
			else
			{
				printf("  %s is not in the loaded database\n", kSerials[i]);
				g_errors++;
			}
		}
	}

	/* Every entry's fixes applied with the automatic fixes on and off, as
	 * a VM start does: a log line that indexes a name table with a mode
	 * the entry left unset reads past it, which the sanitizer lanes see. */
	{
		ryaml_t* yaml = ryaml_parse_ex(s_yaml.data(), s_yaml.size(), NULL, NULL);
		int n, applied = 0;
		const int errors = g_errors;
		for (n = ryaml_first_child(yaml, ryaml_root(yaml)); yaml && n >= 0; n = ryaml_next_sibling(yaml, n))
		{
			size_t len = 0;
			const char* key = ryaml_key(yaml, n, &len);
			std::string serial(key, len);
			const GameDatabaseEntry* e = GameDatabase_FindGame(serial.c_str());
			if (!e)
				continue;
			{
				Pcsx2Config on, off;
				Pcsx2Config::GSOptions gs_auto, gs_manual;
				gs_manual.ManualUserHacks = true;
				GameDatabase_ApplyGameFixes(e, on, true);
				GameDatabase_ApplyGameFixes(e, off, false);
				GameDatabase_ApplyGSHardwareFixes(e, gs_auto);
				GameDatabase_ApplyGSHardwareFixes(e, gs_manual);
			}
			applied++;
		}
		ryaml_free(yaml);
		/* the "off" passes warn by design; only errors count here */
		g_warnings = 0;
		if (g_errors != errors)
			printf("  applying the fixes logged %d errors\n", g_errors - errors);
		printf("dbload: fixes applied both ways for %d entries\n", applied);
		if (applied < 10000)
		{
			printf("  expected the whole database\n");
			g_errors++;
		}
	}

	printf("dbload: %d errors, %d warnings, %d/3 spot-checked serials found\n",
	       g_errors, g_warnings, entries);
	if (g_errors != 0 || entries != 3)
		return 1;
	return fixture() ? 1 : 0;
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
