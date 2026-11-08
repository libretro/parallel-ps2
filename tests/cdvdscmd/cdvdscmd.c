/*  CDVD S-commands through cdvdWrite17/cdvdWrite16, as the IOP issues them.
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

int main(void)
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

	if (failures)
	{
		printf("cdvdscmd: %d failure(s)\n", failures);
		return 1;
	}
	printf("cdvdscmd: the iLink ID written is the one read back\n");
	return 0;
}
