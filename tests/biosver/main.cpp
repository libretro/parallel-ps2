/* IsBIOS on an image with no EXTINFO entry (FPS2BIOS has none, nor have
 * the test images tests/swdraw writes). The description ends with the
 * BIOS's serial, which comes from EXTINFO; without it the serial must be
 * empty, not whatever the caller's buffer held. The stack is filled with
 * 'X' first so a serial nobody wrote shows up as one. */
#include "../../pcsx2/ps2/BiosTools.cpp"

#include <stdio.h>
#include <string.h>

/* What the parts of BiosTools.cpp reached here refer to. */
retro_log_printf_t log_cb;
static void quiet_log(enum retro_log_level, const char*, ...) {}

static int failures;
static void check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static void __attribute__((noinline)) poison_stack(void)
{
	volatile char junk[16384];
	for (size_t i = 0; i < sizeof(junk); i++)
		junk[i] = 'X';
}

/* RESET (the table itself), ROMDIR and ROMVER; no EXTINFO. */
static bool write_image(const char* path, const char* romver)
{
	static unsigned char img[0x200];
	struct entry { const char* name; unsigned size; } dir[] = {
		{ "RESET", 0x100 }, { "ROMDIR", 0x40 }, { "ROMVER", 0x10 },
	};
	unsigned i;
	memset(img, 0, sizeof(img));
	for (i = 0; i < 3; i++)
	{
		unsigned char* e = img + i * 16;
		strncpy(reinterpret_cast<char*>(e), dir[i].name, 10);
		e[12] = dir[i].size & 0xff;
		e[13] = (dir[i].size >> 8) & 0xff;
	}
	memcpy(img + 0x140, romver, 14);
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	const bool ok = fwrite(img, 1, sizeof(img), f) == sizeof(img);
	return fclose(f) == 0 && ok;
}

int main(int argc, char** argv)
{
	char path[1024], description[256], zone[BIOS_ZONE_MAX];
	u32 version = 0, region = 0;
	bool (*volatile is_bios)(const char*, u32*, char*, size_t, u32*, char*, size_t) = IsBIOS;
	log_cb = quiet_log;
	snprintf(path, sizeof(path), "%s/bios.bin", argc > 1 ? argv[1] : ".");
	if (!write_image(path, "0220EC20040604"))
	{
		printf("FAIL: cannot write %s\n", path);
		return 1;
	}

	poison_stack();
	const bool found = is_bios(path, &version, description, sizeof(description), &region, zone, sizeof(zone));
	check(found, "an image with RESET, ROMDIR and ROMVER is a BIOS");
	/* The minor is kept as the decimal number it reads as: v2.20 is 2, 20. */
	check(version == ((2u << 8) | 20u) && region == 2 && !strcmp(zone, "Europe"), "ROMVER: v2.20, Europe");
	check(!strcmp(description, "Europe  v02.20(04/06/2004)  Console "),
		"no EXTINFO: the description ends with an empty serial");
	if (strcmp(description, "Europe  v02.20(04/06/2004)  Console "))
		printf("      description: \"%s\"\n", description);

	/* ROMVER zone T, model Z: a COH-H arcade board, which LoadBIOS takes
	 * for BiosIsArcade; model D is a T10K devkit, not arcade. */
	write_image(path, "0220TZ20040604");
	is_bios(path, &version, description, sizeof(description), &region, zone, sizeof(zone));
	check(!strcmp(zone, "COH-H") && region == 8, "ROMVER zone T, model Z: COH-H");
	write_image(path, "0220TD20040604");
	is_bios(path, &version, description, sizeof(description), &region, zone, sizeof(zone));
	check(!strcmp(zone, "T10K") && region == 8, "ROMVER zone T, model D: T10K");
	remove(path);

	printf(failures ? "biosver: %d failure(s)\n" : "biosver: ok\n", failures);
	return failures != 0;
}
