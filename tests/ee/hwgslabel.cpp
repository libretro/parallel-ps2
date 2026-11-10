/*  GS SIGNAL and LABEL against console captures.
 *
 *  Scores Gif_HandlerAD in pcsx2/Gif_Unit.c against ps2autotests
 *  tests/gs/label.expected and tests/gs/signal.expected, two captures
 *  nothing read.
 *
 *  Both registers take an id and a mask in one quadword and merge rather
 *  than assign: the written field becomes (old & ~mask) | (id & mask).
 *  That is why the captures write 0xF0 twice -- once with a mask that
 *  admits it and once with a mask that does not -- and print SIGLBLID
 *  each time. A plain assignment passes the first line of each pair and
 *  fails the second, which is the shape this is here to catch.
 *
 *  The two fields sit in one 64-bit register, LABEL in the high half and
 *  SIGNAL in the low, so each capture also pins that the other half is
 *  left alone.
 *
 *  Only the register merge is scored. SIGNAL also raises an interrupt and
 *  stalls the GIF unless the previous signal was acknowledged; that needs
 *  the GS thread and the interrupt controller, and is not covered here.
 *
 *  Usage: tests/ee/hwgslabel
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "Common.h"
#include "GS.h"
#include "GS/GSRegs.h"
#include "Gif_Unit.h"
#include "Vif_Dma.h"
#include "SaveState.h"

u32 emu_hot; /* HotConfig.h: VU1 runs on the EE thread here */

alignas(16) u8 eeHw[0x10000];
alignas(16) u8 g_RealGSMem[0x2000];
alignas(16) vifStruct vif1;

alignas(16) cpuRegisters cpuRegs;

/* The rest of the GIF unit, which Gif_Unit.c reaches: never run here. */
extern "C" {
void Gif_AddBlankGSPacket(u32, GIF_PATH) { }
void Gif_AddCompletedGSPacket(const GS_Packet*, GIF_PATH) { }
void Gif_MTVU_KickSema(void) { }
void Gif_HandlerAD_MTVU(u8*) { }
void mtgs_wait_gs(bool) { }
void gifCheckPathStatus(bool) { }
void CPU_INT(EE_EventType, s32) { }
void hwIntcIrq(int) { }
bool emu_config_xgkick_hack(void) { return false; }
bool emu_config_enable_vu1(void) { return true; }
void SaveState_FreezeMem(SaveStateBase*, void*, int) { }
bool SaveState_FreezeTag(SaveStateBase*, const char*) { return true; }
}

/* One AD packet: the data quadword followed by the register number. */
static void send_ad(u64 lo, u64 hi, u8 reg)
{
	alignas(16) u8 mem[16];
	memcpy(mem + 0, &lo, 8);
	memcpy(mem + 8, &hi, 8);
	{
		/* Gif_HandlerAD reads the register from the second doubleword's
		 * low byte, so it is written after the data. */
		u64 r = reg;
		memcpy(mem + 8, &r, 8);
		memcpy(mem + 0, &lo, 8);
	}
	Gif_HandlerAD(mem);
}

int main(void)
{
	int pass = 0, cases = 0;

	/* label.expected. The harness leaves SIGLBLID's low half at
	 * 0x76543210 and writes LABEL twice. */
	{
		GSSIGLBLID.SIGID = 0x76543210u;
		GSSIGLBLID.LBLID = 0;
		send_ad(((u64)0xF0u << 32) | 0xF0u, 0, GIF_A_D_REG_LABEL);
		cases++;
		if (GSSIGLBLID.LBLID == 0x000000F0u && GSSIGLBLID.SIGID == 0x76543210u)
			pass++;
		else
			printf("  set label bits: SIGLBLID is %08x%08x,"
			       " console reads 000000f076543210\n",
			       GSSIGLBLID.LBLID, GSSIGLBLID.SIGID);

		/* Then a write of 0x00 through the same mask, from 0x33333333. */
		GSSIGLBLID.LBLID = 0x33333333u;
		GSSIGLBLID.SIGID = 0x33333333u;
		send_ad(((u64)0xF0u << 32) | 0x00u, 0, GIF_A_D_REG_LABEL);
		cases++;
		if (GSSIGLBLID.LBLID == 0x33333303u && GSSIGLBLID.SIGID == 0x33333333u)
			pass++;
		else
			printf("  clear label bits: SIGLBLID is %08x%08x,"
			       " console reads 3333330333333333\n",
			       GSSIGLBLID.LBLID, GSSIGLBLID.SIGID);
	}

	/* signal.expected: the same pair on the other half. */
	{
		GSSIGLBLID.LBLID = 0x76543210u;
		GSSIGLBLID.SIGID = 0;
		/* The test acknowledges CSR's SIGNAL bit before each write, and
		 * so must this: a second SIGNAL with the bit still set takes the
		 * stall path and does not merge, which reads as the merge being
		 * wrong rather than as a missing acknowledgement. */
		gifUnit.gsSIGNAL.queued = false;
		gsCSRclear(GS_CSR_SIGNAL);
		send_ad(((u64)0xF0u << 32) | 0xF0u, 0, GIF_A_D_REG_SIGNAL);
		cases++;
		if (GSSIGLBLID.SIGID == 0x000000F0u && GSSIGLBLID.LBLID == 0x76543210u)
			pass++;
		else
			printf("  set signal bits: SIGLBLID is %08x%08x,"
			       " console reads 76543210000000f0\n",
			       GSSIGLBLID.LBLID, GSSIGLBLID.SIGID);

		GSSIGLBLID.LBLID = 0x33333333u;
		GSSIGLBLID.SIGID = 0x33333333u;
		gifUnit.gsSIGNAL.queued = false;
		gsCSRclear(GS_CSR_SIGNAL);
		send_ad(((u64)0xF0u << 32) | 0x00u, 0, GIF_A_D_REG_SIGNAL);
		cases++;
		if (GSSIGLBLID.SIGID == 0x33333303u && GSSIGLBLID.LBLID == 0x33333333u)
			pass++;
		else
			printf("  clear signal bits: SIGLBLID is %08x%08x,"
			       " console reads 3333333333333303\n",
			       GSSIGLBLID.LBLID, GSSIGLBLID.SIGID);
	}

	printf("hwgslabel: %d/%d SIGNAL and LABEL merges match the console\n",
	       pass, cases);
	return pass != cases;
}
