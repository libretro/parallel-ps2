/* A memory card's own MagicGate ID (pcsx2/MemoryCardFile.c): eight bytes
 * in <image>.id beside the card image -- a real arcade dongle's -- are what
 * FileMcd_GetMagicGateId hands the handshake for that slot. No file, or a
 * file of any other length, leaves the slot to the emulated card's ID. */

#include <stdio.h>
#include <string.h>
#include <string>
#include <libretro.h>
#include "Config.h"
#include "MemoryCardFile.h"
#include <file/file_path.h>

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

	/* A Python 1 game names its card by its whole path, somewhere other
	 * than the cards' folder, and its ID file by a name of its own. */
	{
		char abs_card[1024], id_path[1024];
		snprintf(abs_card, sizeof(abs_card), "%s/game/card.bin", dir);
		snprintf(id_path, sizeof(id_path), "%s/game/key.dat", dir);
		{
			std::string sub = std::string(dir) + "/game";
			path_mkdir(sub.c_str());
		}
		put(dir, "game/card.bin", card, sizeof(card));
		put(dir, "game/key.dat", id, sizeof(id));
		FileMcd_EmuClose();
		strcpy(EmuFolders::MemoryCards, "/nonexistent");
		strcpy(EmuConfig.Mcd[0].Filename, abs_card);
		FileMcd_SetIdPath(0, id_path);
		FileMcd_EmuOpen();
		got = FileMcd_GetMagicGateId(0, 0);
		check(FileMcd_IsPresent(0, 0) && got && memcmp(got, id, 8) == 0,
			"an absolute card path is opened where it is, its ID from the file named for it");
		FileMcd_SetIdPath(0, NULL);
	}

	FileMcd_EmuClose();
	printf(failures ? "mcdid: %d failure(s)\n" : "mcdid: ok\n", failures);
	return failures != 0;
}
