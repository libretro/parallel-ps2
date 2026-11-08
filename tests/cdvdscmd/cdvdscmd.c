/*  CDVD S-commands through cdvdWrite17/cdvdWrite16, as the IOP issues them,
 *  and the NVRAM file as cdvdLoadNVRAM takes it.
 *
 *  sceCdWriteILinkID (0x13) takes the ID as its eight parameter bytes
 *  (ps2sdk cdvdman sceCdWI: set_prev_command(0x13, buffer, 8, ...)), and
 *  sceCdReadILinkId (0x12) must then hand those eight bytes back. The
 *  write used to store the result buffer instead.
 *
 *  Includes CDVD.c for its static command handlers; the build drops the
 *  sections nothing reaches, which keeps the stubs below to a handful.
 */

#include "../../pcsx2/CDVD/CDVD.c"

u32 BiosVersion = 0x0220;
u32 BiosRegion = 2;
bool BiosIsArcade;
char BiosPath[PCSX2_PATH_MAX];
bool NoOSD;
bool g_GameStarted;
bool g_SkipBiosHack;
retro_log_printf_t log_cb;
static CDVD_API s_api;
CDVD_API* CDVD = &s_api;

CDVD_SourceType CDVDsys_GetSourceType(void) { return CDVD_SOURCE_NODISC; }
s32 DoCDVDdetectDiskType(void) { return 0; }
void DoCDVDresetDiskTypeCache(void) {}
bool host_option_fast_boot(void) { return false; }
void iopIntcIrq(uint irqType) { (void)irqType; }
void psxSetNextBranchDelta(s32 delta) { (void)delta; }
void vm_request_stop(void) {}
bool mecha_mg_keyed(void) { return false; }
int mecha_mg_scmd(u8 cmd, const u8* param, int param_cnt, u8* result, const MechaMgIds* ids)
{
	(void)cmd; (void)param; (void)param_cnt; (void)result; (void)ids;
	return -1;
}

static void quiet_log(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}

static int failures;

static void scmd(u8 cmd, const u8* param, int n)
{
	int i;
	cdvd.SCMDParamPos = 0;
	cdvd.SCMDParamCnt = 0;
	for (i = 0; i < n; i++)
		cdvdWrite17(param[i]);
	cdvdWrite16(cmd);
}

/* A COH-H board's NVRAM has no language or PStwo region block; read
 * back with those blank it must stay as it was, where a console's gets
 * the defaults filled in. */
static void test_nvram(const char* dir)
{
	const NVMLayout* l;
	FILE* f;
	char path[PCSX2_PATH_MAX];
	static u8 board[sizeof(s_nvram)];
	size_t i;

	for (i = 0; i < sizeof(board); i++)
		board[i] = (u8)(i * 7 + 3);
	BiosVersion = 0x0220;
	l = getNvmLayout();
	memset(board + l->config1 + 0x10, 0, 16);
	memset(board + l->regparams, 0, 12);
	snprintf(path, sizeof(path), "%s/bios.nvm", dir);
	f = fopen(path, "wb");
	if (!f || fwrite(board, 1, sizeof(board), f) != sizeof(board))
	{
		printf("  FAIL cannot write %s\n", path);
		failures++;
		if (f)
			fclose(f);
		return;
	}
	fclose(f);
	snprintf(BiosPath, PCSX2_PATH_MAX, "%s/bios.bin", dir);

	BiosIsArcade = true;
	memset(s_nvram, 0xEE, sizeof(s_nvram));
	cdvdLoadNVRAM();
	if (memcmp(s_nvram, board, sizeof(board)) != 0)
	{
		printf("  FAIL a COH-H BIOS: the NVRAM read back is not the file\n");
		failures++;
	}

	BiosIsArcade = false;
	memset(s_nvram, 0xEE, sizeof(s_nvram));
	cdvdLoadNVRAM();
	if (memcmp(s_nvram + l->config1 + 0x10, biosLangDefaults[BiosRegion], 16) != 0 ||
		memcmp(s_nvram + l->regparams, PStwoRegionDefaults[BiosRegion], 12) != 0)
	{
		printf("  FAIL a console BIOS: blank language and region not filled in\n");
		failures++;
	}
}

static void put(const char* path, const void* data, size_t n)
{
	FILE* f = fopen(path, "wb");
	if (f)
	{
		fwrite(data, 1, n, f);
		fclose(f);
	}
}

static int same_file(const char* path, const void* data, size_t n)
{
	static u8 buf[2048];
	FILE* f = fopen(path, "rb");
	size_t got;
	if (!f)
		return 0;
	got = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	return got == n && !memcmp(buf, data, n);
}

static void expect(int ok, const char* what)
{
	if (!ok)
	{
		printf("  FAIL %s\n", what);
		failures++;
	}
}

/* A Python 2 game brings its machine's NVRAM, and an i.Link ID for one
 * made afresh: cdvd_set_nvram_files. */
static void test_nvram_files(const char* dir)
{
	static u8 game[sizeof(s_nvram)], bios[sizeof(s_nvram)], dump[0x400];
	static const u8 id8[8] = {0x08, 0x00, 0x46, 0x01, 0xaa, 0xbb, 0xcc, 0xdd};
	char game_nvm[PCSX2_PATH_MAX], bios_nvm[PCSX2_PATH_MAX], idfile[PCSX2_PATH_MAX], none[PCSX2_PATH_MAX];
	const NVMLayout* l;
	size_t i;

	BiosVersion  = 0x0220;
	BiosIsArcade = false;
	l = getNvmLayout();
	for (i = 0; i < sizeof(game); i++)
	{
		game[i] = (u8)(i * 13 + 1);
		bios[i] = (u8)(i * 5 + 2);
	}
	/* Language and region present, so neither is taken for blank. */
	memcpy(game + l->config1 + 0x10, biosLangDefaults[BiosRegion], 16);
	memcpy(game + l->regparams, PStwoRegionDefaults[BiosRegion], 12);
	memcpy(bios + l->config1 + 0x10, biosLangDefaults[BiosRegion], 16);
	memcpy(bios + l->regparams, PStwoRegionDefaults[BiosRegion], 12);

	snprintf(BiosPath, PCSX2_PATH_MAX, "%s/bios.bin", dir);
	snprintf(bios_nvm, sizeof(bios_nvm), "%s/bios.nvm", dir);
	snprintf(game_nvm, sizeof(game_nvm), "%s/game.nvm", dir);
	snprintf(idfile, sizeof(idfile), "%s/ilink.bin", dir);
	snprintf(none, sizeof(none), "%s/none.nvm", dir);
	put(bios_nvm, bios, sizeof(bios));
	put(game_nvm, game, sizeof(game));

	cdvd_set_nvram_files(game_nvm, NULL);
	cdvdLoadNVRAM();
	expect(!memcmp(s_nvram, game, sizeof(game)), "the game's NVRAM is loaded, not the BIOS's");
	s_nvram[0x123] ^= 0xff;
	cdvdSaveNVRAM();
	game[0x123] ^= 0xff;
	expect(same_file(game_nvm, game, sizeof(game)), "and saved back to the game's file");
	expect(same_file(bios_nvm, bios, sizeof(bios)), "the BIOS's NVRAM is left alone");

	/* No NVRAM yet: made afresh with the i.Link ID file's ID. */
	put(idfile, id8, 8);
	cdvd_set_nvram_files(none, idfile);
	cdvdLoadNVRAM();
	expect(!memcmp(s_nvram + l->ilinkId, id8, 8), "a new NVRAM takes an 8-byte i.Link ID file's ID");
	for (i = 0; i < sizeof(dump); i++)
		dump[i] = (u8)i;
	put(idfile, dump, sizeof(dump));
	cdvdLoadNVRAM();
	expect(!memcmp(s_nvram + l->ilinkId, dump + 0x1e0, 8), "or a 1 KiB NVRAM dump's, from 0x1e0");
	put(idfile, id8, 7);
	cdvdLoadNVRAM();
	expect(s_nvram[l->ilinkId + 1] == 0xAC, "a file of another size: the dummy ID");

	cdvd_set_nvram_files(NULL, NULL);
	cdvdLoadNVRAM();
	expect(!memcmp(s_nvram, bios, sizeof(bios)), "cleared, the BIOS's NVRAM is back");
	remove(none);
}

int main(int argc, char** argv)
{
	static const u8 id[8] = {0x08, 0x00, 0x46, 0x01, 0x23, 0x45, 0x67, 0x89};
	int i;

	log_cb = quiet_log;
	memset(s_nvram, 0, sizeof(s_nvram));

	scmd(0x13, id, 8);
	if (cdvd.SCMDResultCnt != 1 || cdvd.SCMDResultBuff[0] != 0)
	{
		printf("  FAIL sceCdWriteILinkID: result %u bytes, status %02x, want 1 byte, 00\n",
			(unsigned)cdvd.SCMDResultCnt, cdvd.SCMDResultBuff[0]);
		failures++;
	}

	scmd(0x12, NULL, 0);
	for (i = 0; i < 8 && cdvd.SCMDResultBuff[1 + i] == id[i]; i++) {}
	if (cdvd.SCMDResultCnt != 9 || cdvd.SCMDResultBuff[0] != 0 || i != 8)
	{
		printf("  FAIL sceCdReadILinkId after a write: got");
		for (i = 0; i < 8; i++)
			printf(" %02x", cdvd.SCMDResultBuff[1 + i]);
		printf(", want the ID written\n");
		failures++;
	}

	/* Seven bytes is not an ID: refused, NVRAM untouched. */
	scmd(0x13, id + 1, 7);
	scmd(0x12, NULL, 0);
	if (cdvd.SCMDResultBuff[1] != id[0])
	{
		printf("  FAIL a short sceCdWriteILinkID changed the stored ID\n");
		failures++;
	}

	test_nvram(argc > 1 ? argv[1] : ".");
	test_nvram_files(argc > 1 ? argv[1] : ".");

	if (failures)
	{
		printf("cdvdscmd: %d failure(s)\n", failures);
		return 1;
	}
	printf("cdvdscmd: the iLink ID written is the one read back; NVRAM loads as the BIOS needs,\n"
		"  from the game's own file when it brings one\n");
	return 0;
}
