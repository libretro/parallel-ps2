/* What BiosTools.c makes of a BIOS image, linked against the real unit.
 *
 * IsBIOS on an image with no EXTINFO entry (FPS2BIOS has none, nor have
 * the test images tests/swdraw writes): the description ends with the
 * BIOS's serial, which comes from EXTINFO, so without it the serial must
 * be empty, not whatever the caller's buffer held. The stack is filled
 * with 'X' first so a serial nobody wrote shows up as one.
 *
 * The romdir is looked for a chunk of entries at a time; a table on
 * either side of a chunk's edge, or deep in the image as a retail BIOS
 * has it, must still be found, and the scan must resume right after the
 * RESET entry.
 *
 * LoadBIOS: with no BIOS configured it takes the first in the BIOS
 * folder, reads it into the ROM, checksums it, and finds ROM1 as
 * name.rom1 and ROM2 as name.bin.rom2 beside it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/Pcsx2Defs.h"
#include "MemoryTypes.h"
#include "ps2/BiosTools.h"

/* What BiosTools.c refers to outside itself. */
retro_log_printf_t log_cb;
EEVM_MemoryAllocMess* eeMem;
static char cfg_folder[1024];
static char cfg_path[1024];
static char cfg_irx[1024];
void emu_config_bios_path(char* out, size_t out_size)
{
	snprintf(out, out_size, "%s", cfg_path);
}
const char* emu_config_bios_name(void) { return cfg_path; }
const char* emu_config_current_irx(void) { return cfg_irx; }
bool emu_config_irx_injected(void) { return strlen(cfg_irx) > 3; }
const char* emu_folders_bios(void) { return cfg_folder; }

static void quiet_log(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}

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
	size_t i;
	for (i = 0; i < sizeof(junk); i++)
		junk[i] = 'X';
}

/* RESET (the table itself), ROMDIR and ROMVER, no EXTINFO, with the table
 * at byte at (a multiple of 16), padded to size bytes (0: just enough). */
static bool write_image_at(const char* path, const char* romver, unsigned at, long size)
{
	static const struct { const char* name; unsigned size; } dir[] = {
		{ "RESET", 0x100 }, { "ROMDIR", 0x40 }, { "ROMVER", 0x10 },
	};
	unsigned char* img;
	unsigned i;
	size_t len = at + 0x200;
	FILE* f;
	bool ok;

	if ((size_t)size > len)
		len = (size_t)size;
	img = (unsigned char*)calloc(1, len);
	if (!img)
		return false;
	for (i = 0; i < 3; i++)
	{
		unsigned char* e = img + at + i * 16;
		/* RESET's size is everything before the ROMDIR: the boot code
		 * ahead of the table, and the table's own first 0x100 bytes. */
		unsigned sz = dir[i].size + (i == 0 ? at : 0);
		strncpy((char*)e, dir[i].name, 10);
		e[12] = sz & 0xff;
		e[13] = (sz >> 8) & 0xff;
		e[14] = (sz >> 16) & 0xff;
	}
	memcpy(img + at + 0x140, romver, 14);
	f = fopen(path, "wb");
	if (!f)
	{
		free(img);
		return false;
	}
	ok = fwrite(img, 1, len, f) == len;
	free(img);
	return fclose(f) == 0 && ok;
}

static bool write_image(const char* path, const char* romver)
{
	return write_image_at(path, romver, 0, 0);
}

/* Windows' folder listing joins with '\\', where the test wrote '/'. */
static bool same_path(const char* a, const char* b)
{
	for (; *a && *b; a++, b++)
		if (*a != *b && !((*a == '/' || *a == '\\') && (*b == '/' || *b == '\\')))
			return false;
	return *a == *b;
}

static bool write_bytes(const char* path, unsigned char fill, size_t len)
{
	FILE* f = fopen(path, "wb");
	size_t i;
	bool ok = f != NULL;
	for (i = 0; ok && i < len; i++)
		ok = fputc(fill, f) != EOF;
	return f && fclose(f) == 0 && ok;
}

int main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : ".";
	char path[1024], description[256], zone[BIOS_ZONE_MAX];
	u32 version = 0, region = 0;
	bool (*volatile is_bios)(const char*, u32*, char*, size_t, u32*, char*, size_t) = IsBIOS;
	bool found;

	log_cb = quiet_log;
	snprintf(path, sizeof(path), "%s/bios.bin", dir);
	if (!write_image(path, "0220EC20040604"))
	{
		printf("FAIL: cannot write %s\n", path);
		return 1;
	}

	poison_stack();
	found = is_bios(path, &version, description, sizeof(description), &region, zone, sizeof(zone));
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

	/* The table wherever it sits: the last entry of the first chunk, the
	 * first of the second, and 0x2700 in, where retail BIOSes have it. */
	{
		static const unsigned at[] = { 63 * 16, 64 * 16, 65 * 16, 0x2700 };
		char what[96];
		unsigned i;
		for (i = 0; i < sizeof(at) / sizeof(at[0]); i++)
		{
			write_image_at(path, "0160JC20011004", at[i], 0);
			zone[0] = '\0';
			found = is_bios(path, &version, description, sizeof(description), &region, zone, sizeof(zone));
			snprintf(what, sizeof(what), "romdir at 0x%x: found, ROMVER read after it", at[i]);
			check(found && version == ((1u << 8) | 60u) && !strcmp(zone, "Japan"), what);
		}
		/* A file with no RESET entry at all is not a BIOS, however long. */
		write_bytes(path, 0x5a, 0x12345);
		check(!is_bios(path, &version, description, sizeof(description), &region, zone, sizeof(zone)),
			"no RESET entry: not a BIOS");
		write_bytes(path, 0, 0);
		check(!is_bios(path, &version, description, sizeof(description), &region, zone, sizeof(zone)),
			"an empty file: not a BIOS");
	}
	remove(path);

	/* A Python game's BIOS: the selected one when it is COH-H, else the
	 * first COH-H BIOS by name; one under 4 MiB is not a BIOS image. */
	{
		static const struct { const char* name; const char* romver; long size; } files[] = {
			{ "a_retail.bin", "0220EC20040604", 4 << 20 },
			{ "z_coh.bin",    "0220TZ20040604", 4 << 20 },
			{ "m_coh.bin",    "0220TZ20040604", 4 << 20 },
			{ "c_small.bin",  "0220TZ20040604", 0x200 },
		};
		char name[64];
		unsigned i;
		for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
		{
			snprintf(path, sizeof(path), "%s/%s", dir, files[i].name);
			write_image_at(path, files[i].romver, 0, files[i].size);
		}
		check(FindArcadeBios(dir, "a_retail.bin", name, sizeof(name)) && !strcmp(name, "m_coh.bin"),
			"a retail BIOS selected: the first COH-H BIOS by name");
		check(FindArcadeBios(dir, "z_coh.bin", name, sizeof(name)) && !strcmp(name, "z_coh.bin"),
			"a COH-H BIOS selected: that one");
		snprintf(path, sizeof(path), "%s/m_coh.bin", dir);
		remove(path);
		snprintf(path, sizeof(path), "%s/z_coh.bin", dir);
		remove(path);
		check(!FindArcadeBios(dir, "a_retail.bin", name, sizeof(name)), "none in the folder: none");
		for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
		{
			snprintf(path, sizeof(path), "%s/%s", dir, files[i].name);
			remove(path);
		}
	}

	/* LoadBIOS: nothing configured, so the folder's one BIOS; ROM1 by
	 * the name with its extension swapped, ROM2 by the name with one
	 * added. */
	eeMem = (EEVM_MemoryAllocMess*)calloc(1, sizeof(*eeMem));
	if (!eeMem)
	{
		printf("FAIL: no memory for the EE\n");
		return 1;
	}
	{
		u32 sum = 0;
		size_t i;
		snprintf(cfg_folder, sizeof(cfg_folder), "%s", dir);
		cfg_path[0] = '\0';
		cfg_irx[0] = '\0';
		snprintf(path, sizeof(path), "%s/scph.bin", dir);
		write_image_at(path, "0220AC20040604", 0x2700, 4 << 20);
		snprintf(path, sizeof(path), "%s/scph.rom1", dir);
		write_bytes(path, 0x11, 0x1000);
		snprintf(path, sizeof(path), "%s/scph.bin.rom2", dir);
		write_bytes(path, 0x22, 0x1000);
		memset(eeMem->ROM1, 0xee, 0x1001);

		check(IsBIOSAvailable(""), "nothing configured, a BIOS in the folder: available");
		check(LoadBIOS(), "LoadBIOS takes the folder's BIOS");
		snprintf(path, sizeof(path), "%s/scph.bin", dir);
		check(same_path(BiosPath, path), "BiosPath is the one it loaded");
		check(BiosRegion == 1 && BiosVersion == ((2u << 8) | 20u) && !BiosIsArcade && !NoOSD,
			"its version and region; 4 MiB has an OSD");
		check(!memcmp(eeMem->ROM + 0x2700, "RESET", 5) && !memcmp(eeMem->ROM + 0x2700 + 0x140, "0220AC", 6),
			"the image is in the ROM");
		for (i = 0; i < sizeof(eeMem->ROM) / 4; i++)
		{
			u32 w;
			memcpy(&w, eeMem->ROM + i * 4, 4);
			sum ^= w;
		}
		check(BiosChecksum == sum, "BiosChecksum is the XOR of the ROM's words");
		check(eeMem->ROM1[0] == 0x11 && eeMem->ROM1[0xfff] == 0x11 && eeMem->ROM1[0x1000] == 0xee,
			"ROM1 from scph.rom1, its size and no more");
		check(eeMem->ROM2[0] == 0x22 && eeMem->ROM2[0xfff] == 0x22, "ROM2 from scph.bin.rom2");

		/* Configured, with an IRX to run in place of the BIOS's. */
		snprintf(cfg_path, sizeof(cfg_path), "%s/scph.bin", dir);
		snprintf(cfg_irx, sizeof(cfg_irx), "%s/boot.irx", dir);
		write_bytes(cfg_irx, 0x33, 0x80);
		check(LoadBIOS() && eeMem->ROM[0x3C0000] == 0x33 && eeMem->ROM[0x3C007f] == 0x33
			&& eeMem->ROM[0x3C0080] == 0, "an IRX goes in at 0x3C0000");
		remove(cfg_irx);

		/* Configured but missing, and no other in the folder: no BIOS. */
		snprintf(cfg_path, sizeof(cfg_path), "%s/missing.bin", dir);
		cfg_irx[0] = '\0';
		remove(path);
		snprintf(path, sizeof(path), "%s/scph.rom1", dir);
		remove(path);
		snprintf(path, sizeof(path), "%s/scph.bin.rom2", dir);
		remove(path);
		check(!LoadBIOS(), "the configured BIOS missing and none in the folder: no BIOS");
		check(!IsBIOSAvailable(cfg_path), "nor is one available");
	}
	free(eeMem);

	printf(failures ? "biosver: %d failure(s)\n" : "biosver: ok\n", failures);
	return failures != 0;
}
