/* The SMAP's receive path, on the real net.cpp and smap.cpp.
 *
 * The host adapter's recv runs on net.cpp's RX thread and returns
 * numbered frames of every length up to the Ethernet maximum, with an
 * empty poll now and then. This thread is the EE: it ticks smap_async,
 * which hands the SMAP what it has room for, and models the IOP driver
 * through the SMAP's own registers - read a frame's buffer descriptor,
 * read the frame out of the RX FIFO, give the descriptor back, and
 * decrement the frame count - pausing every so often so the FIFO and
 * the descriptors fill and frames wait.
 *
 * Every frame must arrive once, in order, with its length and bytes
 * intact, each delivery must raise RXEND, and under TSan the RX thread
 * must touch nothing the EE reads: the SMAP's state belongs to the
 * thread that emulates it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include <libretro.h>
#include <rthreads/rthreads.h>

#define MAX_FRAME   1514
#define MIN_FRAME   60
#define TIME_LIMIT  120

/* glue.cpp */
int smaprx_rxend_irqs(void);
int smaprx_bd_count(void);
unsigned smaprx_bd_empty_flag(void);
void smaprx_open(void);
void smaprx_close(void);
void smaprx_tick(void);
unsigned smaprx_frame_count(void);
unsigned smaprx_bd_read(int bd, int word);
void smaprx_bd_write(int bd, int word, unsigned v);
unsigned smaprx_fifo_read32(void);
void smaprx_frame_dec(void);
void smaprx_ptr_write16(int which, unsigned v);
unsigned smaprx_ptr_read(int which);
void smaprx_txfifo_write32(unsigned v);
unsigned smaprx_txfifo_byte(int i);
void smaprx_rxfifo_set(int i, unsigned v);
void smaprx_dma_write(unsigned* words, int bytes);
void smaprx_dma_read(unsigned* words, int bytes);
void smaprx_tx(const unsigned char* frame, int len);
void smaprx_reset(void);
void smaprx_tx_from_here(void);
void smaprx_tx_stats(int* frames, int* resets, int* on_caller, int* out_of_order);

static void harness_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	if (level < RETRO_LOG_WARN)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

retro_log_printf_t log_cb = harness_log;

static unsigned total_frames;

static int frame_len(unsigned seq)
{
	return MIN_FRAME + (int)((seq * 97u) % (unsigned)(MAX_FRAME - MIN_FRAME + 1));
}

static unsigned char frame_byte(unsigned seq, int i)
{
	if (i < 4)
		return (unsigned char)(seq >> (8 * i));
	return (unsigned char)((seq * 31u) + (unsigned)i * 7u);
}

/* The host, on the RX thread: only it touches these. */
static unsigned host_next;
static unsigned host_polls;

int smaprx_host_recv(char* buf, int max)
{
	int len, i;
	if (host_next >= total_frames || (++host_polls % 7u) == 0)
		return 0;
	len = frame_len(host_next);
	if (len > max)
		return 0;
	for (i = 0; i < len; i++)
		buf[i] = (char)frame_byte(host_next, i);
	host_next++;
	return len;
}

static unsigned failures;

static void fail(const char* what, unsigned seq)
{
	if (failures++ < 8)
		fprintf(stderr, "  FAIL: %s (frame %u)\n", what, seq);
}

/* The FIFO pointers are registers the guest can write anything to. Every
 * access stays a word inside the 16 KB FIFO: a pointer past its end
 * wraps, and one that is not word aligned is taken down to the word. */
static void check_fifo_pointers(void)
{
	unsigned words[2];
	int i;

	smaprx_ptr_write16(0, 0xfffc);
	smaprx_txfifo_write32(0x11223344u);
	if (smaprx_txfifo_byte(0x3ffc) != 0x44 || smaprx_txfifo_byte(0x3fff) != 0x11
		|| smaprx_ptr_read(0) != 0)
		fail("TX write pointer past the FIFO wraps into it", 0);

	smaprx_ptr_write16(0, 0x3ffe);
	smaprx_txfifo_write32(0xa1b2c3d4u);
	if (smaprx_txfifo_byte(0x3ffc) != 0xd4 || smaprx_ptr_read(0) != 0)
		fail("unaligned TX write pointer is taken to its word", 0);

	for (i = 0; i < 4; i++)
		smaprx_rxfifo_set(0x3ffc + i, 0x10 + i);
	smaprx_ptr_write16(1, 0xfffe);
	if (smaprx_fifo_read32() != 0x13121110u || smaprx_ptr_read(1) != 0)
		fail("RX read pointer past the FIFO wraps into it", 0);

	words[0] = 0x01020304u;
	words[1] = 0x05060708u;
	smaprx_ptr_write16(0, 0xffff);
	smaprx_dma_write(words, 8);
	if (smaprx_txfifo_byte(0x3ffc) != 0x04 || smaprx_txfifo_byte(0) != 0x08
		|| smaprx_ptr_read(0) != 4)
		fail("TX DMA from a pointer past the FIFO stays in it", 0);

	smaprx_ptr_write16(1, 0xffff);
	smaprx_dma_read(words, 4);
	if (words[0] != 0x13121110u || smaprx_ptr_read(1) != 0)
		fail("RX DMA from a pointer past the FIFO stays in it", 0);
}

int main(int argc, char** argv)
{
	static unsigned char frame[MAX_FRAME + 4];
	const unsigned empty = smaprx_bd_empty_flag();
	const int bd_count = smaprx_bd_count();
	unsigned expect = 0;
	unsigned long iter = 0;
	int bd = 0;
	time_t start;

	total_frames = (argc > 1) ? (unsigned)atoi(argv[1]) : 20000u;

	smaprx_open();
	start = time(NULL);

	while (expect < total_frames && failures == 0)
	{
		smaprx_tick();
		iter++;

		/* The driver is busy elsewhere: the FIFO and the descriptors
		 * fill, and frames wait in the ring. */
		if ((iter % 64u) < 12u)
		{
			sthread_yield();
			continue;
		}

		while (smaprx_frame_count() > 0 && failures == 0)
		{
			unsigned ctrl = smaprx_bd_read(bd, 0);
			int len = (int)smaprx_bd_read(bd, 2);
			int w, i;

			if (ctrl & empty)
			{
				fail("frame counted but its descriptor is still empty", expect);
				break;
			}
			if (len != frame_len(expect))
			{
				fail("frame arrived with the wrong length", expect);
				break;
			}
			for (w = 0; w < (len + 3) / 4; w++)
			{
				unsigned v = smaprx_fifo_read32();
				memcpy(frame + w * 4, &v, 4);
			}
			for (i = 0; i < len; i++)
			{
				if (frame[i] != frame_byte(expect, i))
				{
					fail("frame arrived out of order or changed", expect);
					break;
				}
			}
			smaprx_bd_write(bd, 0, empty);
			smaprx_frame_dec();
			bd = (bd + 1) % bd_count;
			expect++;
		}

		if (time(NULL) - start > TIME_LIMIT)
		{
			fail("timed out waiting for frames", expect);
			break;
		}
		sthread_yield();
	}

	/* The other way: what the PS2 sends reaches the adapter on the
	 * network thread, in order, and an adapter reset with it. */
	{
		enum { TX_FRAMES = 2000 };
		unsigned char out[64];
		int frames = 0, resets = 0, on_caller = 0, out_of_order = 0;
		unsigned i;
		time_t tx_start;

		smaprx_tx_from_here();
		memset(out, 0, sizeof(out));
		for (i = 0; i < TX_FRAMES; i++)
		{
			out[0] = (unsigned char)i;
			smaprx_tx(out, (int)sizeof(out));
			if (i == TX_FRAMES / 2)
				smaprx_reset();
		}
		tx_start = time(NULL);
		for (;;)
		{
			smaprx_tx_stats(&frames, &resets, &on_caller, &out_of_order);
			if ((frames == TX_FRAMES && resets == 1) || time(NULL) - tx_start > TIME_LIMIT)
				break;
			sthread_yield();
		}
		if (frames != TX_FRAMES || resets != 1)
			fail("sent frames or the reset never reached the adapter", (unsigned)frames);
		if (on_caller)
			fail("the adapter was entered on the sending thread, not the network thread", (unsigned)on_caller);
		if (out_of_order)
			fail("sent frames reached the adapter out of order", (unsigned)out_of_order);
		if (!failures)
			printf("  ok: %d frames and a reset sent through the network thread, in order\n", frames);
	}

	smaprx_close();
	check_fifo_pointers();

	if (smaprx_rxend_irqs() == 0)
		fail("no RXEND was raised", expect);

	if (failures)
	{
		fprintf(stderr, "  %u failures, %u of %u frames received\n", failures, expect, total_frames);
		return 1;
	}
	printf("  ok: %u frames received in order and intact, %d RXEND interrupts\n",
		expect, smaprx_rxend_irqs());
	return 0;
}
