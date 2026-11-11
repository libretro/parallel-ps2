/* The IPU's registers and command dispatch, as pcsx2/IPU/IPU.c builds
 * them, linked whole with the real FIFOs; the decoder is a stub worker.
 *   - a reset leaves the command cleared and the worker chosen;
 *   - BCLR sets the bit pointer, ends busy and raises the IPU interrupt;
 *   - SETTH sets both thresholds, and a CTRL reset keeps them;
 *   - IDEC loads the decoder from CTRL and the command, and the worker
 *     runs later, as an event, not at once;
 *   - VDEC runs the worker at once;
 *   - reading CTRL and BP puts together their live fields.
 * C89. */
#include <stdio.h>
#include <string.h>

#include "Common.h"
#include "Hw.h"
#include "IPU/IPU.h"
#include "IPU/IPUdma.h"
#include "IPU/ipu_modes.h"
#include "SaveState.h"

PCSX2_ALIGN(__pagealignsize) u8 eeHw[PS2MEM_HARDWARE];
cpuRegisters cpuRegs;
struct ipu_decode_state ipu_decode;

static int irqs, events, worker_runs, dma_resets;

void hwIntcIrq(int n) { if (n == INTC_IPU) irqs++; }
void CPU_INT(EE_EventType n, s32 cycles)
{
	(void)cycles;
	if (n == IPU_PROCESS)
	{
		events++;
		cpuRegs.interrupt |= 1u << IPU_PROCESS;
	}
}
void ipuDmaReset(void) { dma_resets++; }
static void worker(void) { worker_runs++; ipuRegs.ctrl.BUSY = 0; }
ipu_worker_fn ipu_select_worker(void) { return worker; }
void SaveState_FreezeMem(SaveStateBase *s, void *data, int size) { (void)s; (void)data; (void)size; }
bool SaveState_FreezeTag(SaveStateBase *s, const char *src) { (void)s; (void)src; return false; }

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); fails++; } } while (0)

static u32 cmd(u32 code, u32 option) { return (code << 28) | option; }

int main(void)
{
	u32 v;

	ipuReset();
	CHECK(ipu_cmd.current == 0xffffffffu && ipu_cmd.index == 0, "a reset clears the command");
	CHECK(decoder.picture_structure == FRAME_PICTURE && dma_resets == 1, "a reset: progressive pictures, the DMA reset");

	irqs = 0;
	ipuRegs.ctrl.BUSY = 1;
	ipuWrite32(IPU_CMD, cmd(SCE_IPU_BCLR, 0x15));
	CHECK(g_BP.BP == 0x15 && !ipuRegs.ctrl.BUSY && irqs == 1, "BCLR: bit pointer, not busy, interrupt");

	ipuWrite32(IPU_CMD, cmd(SCE_IPU_SETTH, (0x34u << 16) | 0x12));
	CHECK(g_ipu_thresh[0] == 0x12 && g_ipu_thresh[1] == 0x34 && irqs == 2, "SETTH: both thresholds, interrupt");

	/* CTRL: MP1 and IVF on, then an IDEC with QSC 5 and the sign bit. */
	ipuWrite32(IPU_CTRL, (1u << 23) | (1u << 21));
	events = worker_runs = 0;
	ipuWrite32(IPU_CMD, cmd(SCE_IPU_IDEC, (5u << 16) | (1u << 25)));
	CHECK(decoder.coding_type == I_TYPE && decoder.mpeg1 == 1 && decoder.intra_vlc_format == 1,
	      "IDEC: the decoder from CTRL");
	CHECK(decoder.quantizer_scale == 5 && decoder.sgn == 1 && decoder.dcr == 1, "IDEC: the decoder from the command");
	CHECK(ipuRegs.ctrl.BUSY && events == 1 && worker_runs == 0, "IDEC: busy, the worker as an event");

	cpuRegs.interrupt = 0;
	worker_runs = 0;
	ipuWrite32(IPU_CMD, cmd(SCE_IPU_VDEC, 0));
	CHECK(worker_runs == 1, "VDEC: the worker at once");

	g_BP.BP = 0x15;
	g_BP.IFC = 3;
	g_BP.FP = 1;
	coded_block_pattern = 0x2a;
	v = ipuRead32(IPU_CTRL);
	CHECK((v & 0xf) == 3 && ((v >> 8) & 0x3f) == 0x2a, "CTRL reads IFC and CBP live");
	v = ipuRead32(IPU_BP);
	CHECK(v == (0x15u | (3u << 8) | (1u << 16)), "BP reads BP, IFC and FP");

	irqs = 0;
	ipuWrite32(IPU_CTRL, 1u << 30); /* RST */
	CHECK(g_BP.BP == 0 && ipu_cmd.current == 0xffffffffu && irqs == 1, "a CTRL reset: stream and command cleared, interrupt");
	CHECK(g_ipu_thresh[0] == 0x12 && g_ipu_thresh[1] == 0x34, "a CTRL reset keeps the SETTH thresholds");

	if (fails)
		return 1;
	printf("  ok: IPU registers and command dispatch\n");
	return 0;
}
