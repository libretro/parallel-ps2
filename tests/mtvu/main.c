/* The VU1 worker as pcsx2/MTVU.c builds it, linked whole, with VU1, the
 * VIF unpacker and the GIF as stubs that record what reaches them.
 *
 *   - init, open, close and shutdown, twice over: the worker's
 *     eventcounts are made in retro_init and freed in retro_deinit now
 *     that no constructor makes them;
 *   - a micro-memory write reaches VU1 memory after a Clear of its
 *     range, and a data write lands, in order with a program after them;
 *   - a program runs at its address with the VIF's TOP and ITOP, and
 *     finishes its path-1 packet;
 *   - an unpack hands the unpacker the data and VIF1's registers as the
 *     EE had them;
 *   - more packets than the ring holds go through, so it wraps;
 *   - a GS SIGNAL, FINISH and LABEL from the worker reach the EE. */
#include <stdio.h>
#include <string.h>

#include "Common.h"
#include "GS.h"
#include "MTVU.h"
#include "Gif_Unit.h"
#include "Vif_Unpack.h"
#include "HotConfig.h"
#include "SaveState.h"
#include "ps2/HwInternal.h"
#include <libretro.h>

/* What the core around MTVU.c provides. */
PCSX2_ALIGN(16) u8 g_RealGSMem[PS2MEM_GS_REGS];
Gif_Unit gifUnit;
PCSX2_ALIGN(16) vifStruct vif0, vif1;
VURegs vuRegs[2];
cpuRegisters cpuRegs;
u32 emu_hot;
s32 emu_hot_ee_cycle_rate;
u32 emu_hot_ee_cycle_skip;
static void log_stub(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = log_stub;

static int irqs_gs, finished, transfers, xgkicks_finished;
static u32 vu_start_pc, vu_cleared_addr, vu_cleared_size, unpack_bytes, unpack_first;
static u16 unpack_num;
static int programs;

static void vu_set_start_pc(u32 pc) { vu_start_pc = pc; }
static void vu_execute(u32 cycles) { (void)cycles; programs++; vuRegs[1].cycle = 40; }
static void vu_clear(u32 addr, u32 size) { vu_cleared_addr = addr; vu_cleared_size = size; }
static const struct VUmicroCpu vu1_stub = { 1, 1, NULL, NULL, vu_set_start_pc, vu_execute, vu_clear, NULL };
const struct VUmicroCpu* CpuVU1 = &vu1_stub;
const struct VUmicroCpu vucpu_rec_vu1;

void dvif_unpack(int idx, const u8* data, int isFill)
{
	(void)idx; (void)isFill;
	memcpy(&unpack_first, data, 4);
	unpack_num = vu1Thread.vifRegs.num;
	unpack_bytes++;
}
void gif_path_finish_mtvu(Gif_Path* path, bool final_packet) { (void)path; if (final_packet) xgkicks_finished++; }
int  gif_unit_check_paths_queued(bool p1, bool p2, bool p3) { (void)p1; (void)p2; (void)p3; return 0; }
u32  gif_unit_transfer(GIF_TRANSFER_TYPE type, u8* pMem, u32 size) { (void)type; (void)pMem; (void)size; transfers++; return 0; }
void Gif_FinishIRQ(void) { finished++; }
void hwIntcIrq(int n) { if (n == INTC_GS) irqs_gs++; }
void cpuSetNextEventDelta(s32 delta) { (void)delta; }
s32 WorkEventCount_SpinBudget(void) { return 0; }

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); fails++; } } while (0)

static void run_once(int round)
{
	static u8 micro[256];
	u32 data[4] = { 0xCAFEF00D, 2, 3, 4 };
	int i;
	u8 ad[16];

	mtvu_init();
	mtvu_reset();
	mtvu_open();
	CHECK(mtvu_is_open() && mtvu_thread(), "open");

	for (i = 0; i < (int)sizeof(micro); i++)
		micro[i] = (u8)(i ^ round);
	mtvu_write_micro_mem(0x100, micro, sizeof(micro));
	mtvu_write_data_mem(0x40, data, sizeof(data));
	mtvu_execute_vu(0x12, 7, 9, 0);
	mtvu_wait_vu();
	CHECK(vu_cleared_addr == 0x100 && vu_cleared_size == sizeof(micro), "micro write clears its range");
	CHECK(!memcmp(&vuRegs[1].Micro[0x100], micro, sizeof(micro)), "micro write lands");
	CHECK(!memcmp(&vuRegs[1].Mem[0x40], data, sizeof(data)), "data write lands");
	CHECK(vu_start_pc == (0x12 << 3) && vu1Thread.vifRegs.top == 7 && vu1Thread.vifRegs.itop == 9, "the program's address, TOP and ITOP");
	CHECK(xgkicks_finished > 0 && transfers > 0, "the program finishes its path-1 packet");

	vif1Regs.num = 33;
	vif1Regs.cycle.wl = 1;
	vif1Regs.cycle.cl = 1;
	unpack_bytes = 0;
	mtvu_vif_unpack((const u8*)data, sizeof(data));
	mtvu_wait_vu();
	CHECK(unpack_bytes == 1 && unpack_first == 0xCAFEF00D && unpack_num == 33, "an unpack sees its data and VIF1's registers");

	/* VU1's whole data memory a packet, 16 KB against a 16 MB ring:
	 * 2048 of them wrap it twice. */
	{
		static u8 block[0x4000];
		programs = 0;
		for (i = 0; i < 2048; i++)
		{
			memset(block, i, sizeof(block));
			mtvu_write_data_mem(0, block, sizeof(block));
			mtvu_execute_vu(0, 0, 0, 0);
		}
		mtvu_wait_vu();
		CHECK(programs == 2048, "every program through a wrapping ring");
		CHECK(vuRegs[1].Mem[0] == (u8)2047 && vuRegs[1].Mem[0x3FFF] == (u8)2047, "the last write is the one in memory");
	}

	/* The worker's A+D writes, as Gif_Unit.c hands them over. */
	memset(ad, 0, sizeof(ad));
	ad[0] = 0x05; ad[4] = 0x0F; ad[8] = 0x62; /* LABEL 5, mask 0xF */
	Gif_HandlerAD_MTVU(ad);
	ad[8] = 0x61;                             /* FINISH */
	Gif_HandlerAD_MTVU(ad);
	ad[0] = 0x03; ad[4] = 0xFF; ad[8] = 0x60; /* SIGNAL 3, mask 0xFF */
	Gif_HandlerAD_MTVU(ad);
	irqs_gs = finished = 0;
	GSCSRr = 0; /* SIGNAL clear, so the signal is taken rather than queued */
	mtvu_get_changes();
	CHECK(GSSIGLBLID.LBLID == 5, "LABEL reaches the register");
	CHECK(finished == 1, "FINISH reaches the GIF");
	CHECK(GSSIGLBLID.SIGID == 3 && irqs_gs == 1, "SIGNAL reaches the register and raises the GS interrupt");
	CHECK(retro_atomic_load_acquire_int(&vu1Thread.mtvuInterrupts) == 0, "and every flag is taken");

	mtvu_close();
	CHECK(!mtvu_is_open(), "close");
	mtvu_shutdown();
}

int main(void)
{
	static u8 vu1_mem[0x4000], vu1_micro[0x4000];
	vuRegs[1].Mem   = vu1_mem;
	vuRegs[1].Micro = vu1_micro;
	emu_hot         = EMU_HOT_VU1_INSTANT; /* no VU1 busy events to schedule */
	run_once(0);
	run_once(1); /* retro_deinit then retro_init again */
	if (fails)
		return 1;
	printf("  ok: VU1 worker\n");
	return 0;
}
