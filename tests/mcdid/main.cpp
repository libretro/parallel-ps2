/* A memory card's own MagicGate ID (pcsx2/MemoryCardFile.cpp): eight bytes
 * in <image>.id beside the card image -- a real arcade dongle's -- are what
 * FileMcd_GetMagicGateId hands the handshake for that slot. No file, or a
 * file of any other length, leaves the slot to the emulated card's ID. */

#include <stdio.h>
#include <string.h>
#include <string>
#include <libretro.h>
#include "Config.h"
#include "MemoryCardFile.h"

static void test_log(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
extern "C" retro_log_printf_t log_cb = test_log;
std::string libretro_content;

static int failures;

static void check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static void put(const char* dir, const char* name, const void* data, size_t n)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE* f = fopen(path, "wb");
	if (f)
	{
		fwrite(data, 1, n, f);
		fclose(f);
	}
}

static void open_cards(const char* dir)
{
	FileMcd_EmuClose();
	strcpy(EmuFolders::MemoryCards, dir);
	EmuConfig.Mcd[0].Enabled = true;
	strcpy(EmuConfig.Mcd[0].Filename, "dongle.ps2");
	EmuConfig.Mcd[1].Enabled = true;
	strcpy(EmuConfig.Mcd[1].Filename, "plain.ps2");
	FileMcd_EmuOpen();
}

int main(int argc, char** argv)
{
	static const u8 id[8] = {0x31, 0x9a, 0x05, 0xc7, 0x4e, 0x12, 0xb8, 0x60};
	static u8 card[0x20000];
	const char* dir = argc > 1 ? argv[1] : ".";
	const u8* got;

	memset(card, 0xff, sizeof(card));
	put(dir, "dongle.ps2", card, sizeof(card));
	put(dir, "plain.ps2", card, sizeof(card));
	put(dir, "dongle.id", id, sizeof(id));

	open_cards(dir);
	got = FileMcd_GetMagicGateId(0, 0);
	check(got && memcmp(got, id, 8) == 0, "dongle.id beside dongle.ps2 is slot 0's MagicGate ID");
	check(FileMcd_GetMagicGateId(1, 0) == NULL, "a card with no .id file has none of its own");

	put(dir, "dongle.id", id, 7);
	open_cards(dir);
	check(FileMcd_GetMagicGateId(0, 0) == NULL, "a .id file of 7 bytes is ignored");

	FileMcd_EmuClose();
	printf(failures ? "mcdid: %d failure(s)\n" : "mcdid: ok\n", failures);
	return failures != 0;
}
