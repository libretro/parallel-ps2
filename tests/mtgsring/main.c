/* The GS command ring as pcsx2/MTGS.c builds it, linked whole, with the
 * GS entry points recording what reaches them.
 *
 *   - a GIF packet and a FIFO read reach the GS in order, with the FIFO
 *     read's pointer and size intact through the 16-byte record;
 *   - a wait from a thread that is not the producer drains the ring
 *     itself, and a vsync it drains is kept for retro_run, not presented;
 *   - mtgs_present_pending presents it once;
 *   - unsynchronized downloads read GS memory directly and queue nothing;
 *   - a GS reset reaches the GS with its flag. */
#include <stdio.h>
#include <string.h>

#include "Common.h"
#include "GS.h"
#include "GS/GSEntry.h"
#include "Gif_Unit.h"
#include "Vif_Dma.h"
#include "VUmicro.h"
#include "MTGSOwner.h"
#include "PAD/PADsio.h"
#include "../libretro/audio_queue.h"
#include <libretro.h>
#include <rthreads/retro_asym_eventcount.h>

/* What the core around MTGS.c provides. */
PCSX2_ALIGN(16) u8 g_RealGSMem[PS2MEM_GS_REGS];
retro_atomic_int_t s_GSRegistersWritten;
Gif_Unit gifUnit;
PCSX2_ALIGN(64) Gif_Path_ReadAmount gifPathReadAmount[3];
PCSX2_ALIGN(16) vifStruct vif0, vif1;
static retro_asym_eventcount_t ec_xgkick, ec_p1;

static void log_stub(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = log_stub;

struct retro_asym_eventcount* mtvu_ec_xgkick(void)      { return &ec_xgkick; }
struct retro_asym_eventcount* mtvu_ec_p1_progress(void) { return &ec_p1; }
u32       gif_path_pending_packets(Gif_Path* path)      { (void)path; return 0; }
GS_Packet gif_path_get_packet_mtvu(Gif_Path* path)      { GS_Packet p; (void)path; memset(&p, 0, sizeof(p)); return p; }
void      gif_path_pop_packet_mtvu(Gif_Path* path)      { (void)path; }
uint32_t  retro_audio_mark(void)                        { return 77; }
void      pad_input_latch(void)                         { }

/* The GS, as a log of calls. */
static char trace[512];
static int  hw_mode;
static const u8* fifo_mem;
static u32  fifo_qwc;
static u32  vsyncs, vsync_field;

static void note(const char* s) { strcat(trace, s); }

void GSreset(bool hw)                         { note(hw ? "reset(1) " : "reset(0) "); }
void GSclose(void)                            { note("close "); }
void GSInitAndReadFIFO(u8* mem, u32 size)     { fifo_mem = mem; fifo_qwc = size; note("fifo "); }
void GSReadLocalMemoryUnsync(u8* mem, u32 qwc, u64 a, u64 b, u64 c)
{
	(void)a; (void)b; (void)c;
	fifo_mem = mem; fifo_qwc = qwc; note("unsync ");
}
void GSgifTransfer(const u8* mem, u32 size)
{
	char s[32];
	sprintf(s, "gif(%d,%u) ", (int)(mem - (const u8*)gifUnit.gifPath[2].buffer), (unsigned)size);
	note(s);
}
void GSvsync(u32 field, bool rw)              { (void)rw; vsyncs++; vsync_field = field; note("vsync "); }
int  GSfreeze(FreezeAction m, freezeData* d)  { (void)m; (void)d; return 0; }
bool GSIsHardwareRenderer(void)               { return true; }
void gs_open_emu(u8* basemem)                 { (void)basemem; note("open "); }
void gs_update_config_emu(void)               { }
int  gs_hw_download_mode(void)                { return hw_mode; }

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); fails++; } } while (0)

int main(void)
{
	static u8 path_buf[4096];
	static u8 fifo[64];
	GS_Packet pk;

	retro_asym_eventcount_init(&ec_xgkick);
	retro_asym_eventcount_init(&ec_p1);
	gifUnit.gifPath[2].buffer = path_buf;

	mtgs_try_open_gs();
	CHECK(mtgs_is_open(), "open");
	/* As retro_run on another thread would leave it: this thread is not
	 * the one that renders, and there is no producer. */
	mtgs_wait_for_close();
	trace[0] = 0;

	memset(&pk, 0, sizeof(pk));
	pk.offset = 32;
	pk.size   = 64;
	Gif_AddCompletedGSPacket(&pk, GIF_PATH_3);
	mtgs_init_and_read_fifo(fifo, 3);
	CHECK(!strcmp(trace, "gif(32,4) fifo "), "the packet, then the FIFO read");
	CHECK(fifo_mem == fifo && fifo_qwc == 3, "the FIFO read's pointer and size through the record");
	CHECK(retro_atomic_load_acquire_int(&gifPathReadAmount[GIF_PATH_3].v) == 0, "readAmount back to zero");

	trace[0] = 0;
	PS2MEM_GS[0x1000] = 0; /* CSR.FIELD clear: field 1 */
	mtgs_post_vsync_start();
	CHECK(!strcmp(trace, ""), "a drained vsync is not presented");
	CHECK(mtgs_pending.pending && mtgs_pending.field == 1 && mtgs_pending.audio_mark == 77, "a drained vsync is kept");
	CHECK(mtgs_vsync_drained, "and marked drained");
	CHECK(mtgs_present_pending() == 1 && vsyncs == 1 && vsync_field == 1, "presented once by retro_run");
	CHECK(mtgs_present_pending() == 0 && vsyncs == 1, "and only once");

	trace[0] = 0;
	hw_mode = GS_HW_DOWNLOAD_UNSYNCHRONIZED;
	mtgs_init_and_read_fifo(fifo, 2);
	CHECK(!strcmp(trace, "unsync ") && fifo_qwc == 2, "unsynchronized: read directly, nothing queued");
	hw_mode = GS_HW_DOWNLOAD_ENABLED;

	trace[0] = 0;
	mtgs_reset_gs(true);
	mtgs_wait_gs(false);
	CHECK(!strcmp(trace, "reset(1) "), "a hardware reset reaches the GS");

	trace[0] = 0;
	mtgs_close_gs();
	CHECK(!mtgs_is_open() && !strcmp(trace, "close "), "close");

	if (fails)
		return 1;
	printf("  ok: GS command ring\n");
	return 0;
}
