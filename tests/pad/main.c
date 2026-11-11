/* pcsx2/PAD/PAD.c, the real unit: its save state block, and what a
 * state's bus query is let do. A query is restored as it was saved, and
 * PADpoll indexes the pads by its port and slot and writes a vibration
 * toggle's bytes up to its byte count -- so a state with a slot past the
 * fourth, or a toggle longer than its nine bytes, wrote past the pads.
 * Such a state is refused, with nothing taken from it. */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <libretro.h>

#include "PAD/PAD.h"
#include "Config.h"
#include "USB/USB.h"

/* What PAD.c reaches outside itself. */
Pcsx2Config EmuConfig;
static bool env(unsigned cmd, void* data) { (void)cmd; (void)data; return false; }
retro_environment_t environ_cb = env;
void USBSetPortDevice(unsigned port, int device, unsigned input_port) { (void)port; (void)device; (void)input_port; }
void sioConvertPadToPortAndSlot(u32 index, u32* port, u32* slot)
{
	if (index > 4) { *port = 1; *slot = index - 4; }
	else if (index > 1) { *port = 0; *slot = index - 1; }
	else { *port = index; *slot = 0; }
}
u32 sioConvertPortAndSlotToPad(u32 port, u32 slot)
{
	if (slot == 0)
		return port;
	return port == 0 ? slot + 1 : slot + 4;
}

static int failures;
static void check(int ok, const char* what)
{
	printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

/* A saved state, the query changed by poke, loaded back. */
static PadFullFreezeData s_state;
static s32 load_with(void (*poke)(QueryInfo*))
{
	freezeData fd;
	PadFullFreezeData copy = s_state;
	poke(&copy.query);
	fd.size = sizeof(copy);
	fd.data = (u8*)&copy;
	return PADfreeze(FREEZE_LOAD, &fd);
}
static void nothing(QueryInfo* q) { (void)q; }
static void slot7(QueryInfo* q) { q->slot = 7; q->numBytes = 9; q->lastByte = 0; q->queryDone = 0; }
static void port2(QueryInfo* q) { q->port = 2; }
static void long_reply(QueryInfo* q) { q->numBytes = 200; }
static void long_toggle(QueryInfo* q) { q->currentCommand = 0x4D; q->numBytes = 42; q->lastByte = 2; q->queryDone = 0; }
static void toggle(QueryInfo* q) { q->currentCommand = 0x4D; q->numBytes = 9; q->lastByte = 2; q->queryDone = 0; }

int main(void)
{
	static PadFullFreezeData before, after;
	freezeData fd;
	u32 i;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("pad\n");
	PADinit();
	pad_load_config();

	fd.size = 0;
	fd.data = NULL;
	PADfreeze(FREEZE_SIZE, &fd);
	/* 8 + 2 + 8 pads of 20 + a query of 48: the layout states keep */
	check(fd.size == 218 && sizeof(PadFullFreezeData) == 218, "the block is the 218 bytes states have held");

	fd.size = sizeof(s_state);
	fd.data = (u8*)&s_state;
	check(PADfreeze(FREEZE_SAVE, &fd) == 0, "a state saves");
	check(load_with(nothing) == 0, "and loads back");
	check(load_with(toggle) == 0, "a vibration toggle mid-reply loads");
	/* the toggle runs to its end: bytes 3..8, the vibrate bytes 1..6 */
	for (i = 0; i < 7; i++)
		PADpoll((u8)i);
	fd.data = (u8*)&before;
	PADfreeze(FREEZE_SAVE, &fd);
	check(load_with(slot7) != 0, "a query on a fifth slot is refused");
	check(load_with(port2) != 0, "a query on a third port is refused");
	check(load_with(long_reply) != 0, "a reply longer than its buffer is refused");
	check(load_with(long_toggle) != 0, "a vibration toggle past its nine bytes is refused");
	fd.data = (u8*)&after;
	PADfreeze(FREEZE_SAVE, &fd);
	check(!memcmp(&before.query, &after.query, sizeof(before.query)), "a refused state leaves the bus as it was");
	/* Polled as a game would after the loads: in bounds whatever was
	 * offered (the sanitizer lanes see any write past the pads). */
	for (i = 0; i < 64; i++)
		PADpoll(0x4D);

	printf(failures ? "pad: FAILED\n" : "pad: ok\n");
	return failures ? 1 : 0;
}
