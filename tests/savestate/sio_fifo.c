/* sio2Freeze's FIFO serialisation (pcsx2/SioFifo.c), round-tripped.
 *
 * The state carries a SioFifo as a u32 count followed by its bytes in
 * front-to-back order. The load side sizes the fifo first and reads
 * straight into its storage; that is only the same thing if a cleared
 * fifo pushed count times lays them out contiguously from data[0], which
 * is what this checks against the fifo itself.
 *
 * The cases that matter are the ones where head is not 0: a fifo that has
 * been partly drained stores its live bytes at an offset, and the save side
 * writes from data + head while the load side writes back to data + 0. */
#include <stdio.h>
#include <stdlib.h>

#include "SioFifo.h"
#include "SioTypes.h"
#include "SaveState.h"

/* What Sio.h holds a loaded state to, restated so the harness does not
 * pull the whole SIO in. */
#define SIO2_SEND3_COUNT 16
#define sio_port_slot_ok(port, slot) ((size_t)(port) < SIO_PORTS && (size_t)(slot) < SIO_SLOTS)
#define sio2_send3_ok(position, length) \
	((size_t)(position) <= SIO2_SEND3_COUNT && (size_t)(length) <= SEND3_COMMAND_LENGTH_MASK)

static long checks;
static long failures;

static void expect(int cond, const char* what)
{
	checks++;
	if (!cond)
	{
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

/* A fifo holding live bytes with drained already popped off the front, so
 * head lands where a half-used fifo would leave it. */
static void build(SioFifo* q, int drained, int live, u8 seed)
{
	int i;
	sio_fifo_clear(q);
	for (i = 0; i < drained + live; i++)
		sio_fifo_push(q, (u8)(seed + i));
	for (i = 0; i < drained; i++)
		sio_fifo_pop(q);
}

static void round_trip(int drained, int live)
{
	SioFifo out = { 0 }, in = { 0 };
	SaveStateBase w, r;
	int i;

	build(&out, drained, live, 0x40);
	expect((int)sio_fifo_size(&out) == live, "the fifo holds what was put in it");
	/* Something else in the destination, so a load that does nothing
	 * shows. */
	build(&in, 3, 11, 0x90);

	SaveState_Init(&w, NULL, 0, 0, true);
	sio_fifo_freeze(&w, &out);
	SaveState_Init(&r, w.memory, w.memory_size, w.memory_cap, false);
	sio_fifo_freeze(&r, &in);

	expect((int)sio_fifo_size(&in) == live, "the count came back");
	expect(r.idx == w.idx, "both passes covered the same bytes");
	for (i = 0; i < live; i++)
	{
		if (in.data[in.head + i] != out.data[out.head + i])
		{
			printf("  FAIL: byte %d of a %d-deep fifo (head %d)\n", i, live, (int)out.head);
			failures++;
			break;
		}
	}
	checks++;

	free(w.memory);
	sio_fifo_free(&out);
	sio_fifo_free(&in);
}

/* A count larger than the block has left is refused before the fifo grows
 * for it. */
static void huge_count(void)
{
	SioFifo q = { 0 };
	SaveStateBase r;
	u8 block[8] = { 0xff, 0xff, 0xff, 0xff, 1, 2, 3, 4 };
	SaveState_Init(&r, block, sizeof(block), sizeof(block), false);
	sio_fifo_freeze(&r, &q);
	expect(r.error && sio_fifo_size(&q) == 0 && q.capacity == 0, "a count past the block is refused, nothing grown");
	sio_fifo_free(&q);
}

int main(void)
{
	int drained, live;

	/* Across a growth step (capacity starts at 64 and doubles) and on both
	 * sides of it, at every head offset that fits. */
	for (live = 0; live <= 200; live += live < 8 ? 1 : 17)
		for (drained = 0; drained <= 70; drained += drained < 4 ? 1 : 23)
			round_trip(drained, live);
	huge_count();

	/* The fields a loaded state may not take out of range: the port and
	 * slot that index the memory cards, and the SEND3 position and command
	 * length. Sixteen is all descriptors read, seventeen is past them. */
	expect(sio_port_slot_ok(0, 0) && sio_port_slot_ok(1, 3), "port 0..1, slot 0..3 load");
	expect(!sio_port_slot_ok(2, 0), "port 2 is refused");
	expect(!sio_port_slot_ok(0, 4), "slot 4 is refused");
	expect(!sio_port_slot_ok(255, 200), "a port and slot from nowhere are refused");
	expect(sio2_send3_ok(0, 0) && sio2_send3_ok(SIO2_SEND3_COUNT, SEND3_COMMAND_LENGTH_MASK),
		"send3 position 0..16 and a length the field holds load");
	expect(!sio2_send3_ok(SIO2_SEND3_COUNT + 1, 0), "send3 position 17 is refused");
	expect(!sio2_send3_ok(0, (size_t)SEND3_COMMAND_LENGTH_MASK + 1), "a longer command is refused");

	printf("%s: sio fifo, %ld checks, %ld failures\n", failures ? "FAIL" : "PASS", checks, failures);
	return failures != 0;
}
