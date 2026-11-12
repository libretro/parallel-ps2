/* EyeToy frame and sample handoff, and the encoders behind them. */

/* The mailbox and the sample ring (pcsx2/USB/usb-eyetoy/cam-frame.h) are the */
/* lock-free handoffs between the thread that captures (the frontend's camera */
/* callback or the V4L2 thread) and the thread that runs the device. A */
/* producer and a consumer hammer each from two threads: every frame taken */
/* must be whole and never older than the one before, every sample run must */
/* be in order. Built with TSan too, since the two share nothing but the */
/* primitive. */

/* The encoders are checked by parsing what they emit: a JFIF the game's */
/* decoder would accept, with the component layout the EyeToy sends; an IPU */
/* stream with its header and end-of-sequence; the Capture Eye's 80x64 block */
/* layout, sampled every fourth pixel. */

#include "cam-frame.h"
#include "cam-jpeg.h"
#include "jo_mpeg.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int s_bad;

static void check(bool ok, const char* what)
{
	printf("  %-56s %s\n", what, ok ? "ok" : "WRONG");
	if (!ok)
		s_bad++;
}

/* ---- the frame mailbox ---- */

static const u32 MAILBOX_FRAMES = 20000;

/* A frame is its sequence number, repeated; size varies with it. */
static u32 frame_size(u32 seq)
{
	return 64 + (seq * 7919) % 4096;
}

static void* mailbox_producer(void* arg)
{
	eyetoy_frame_mailbox* m = (eyetoy_frame_mailbox*)(arg);
	for (u32 seq = 1; seq <= MAILBOX_FRAMES; seq++)
	{
		u8* w = eyetoy_mailbox_work(m);
		const u32 size = frame_size(seq);
		for (u32 i = 0; i < size; i++)
			w[i] = (u8)(seq + i);
		eyetoy_mailbox_publish(m, size, seq);
	}
	return NULL;
}

static void test_mailbox(void)
{
	eyetoy_frame_mailbox m;
	memset(&m, 0, sizeof(m));
	check(eyetoy_mailbox_init(&m), "mailbox allocates");
	check(eyetoy_mailbox_take(&m) == NULL, "mailbox starts empty");

	pthread_t t;
	pthread_create(&t, NULL, mailbox_producer, &m);

	u32 last = 0, taken = 0, torn = 0, backwards = 0;
	while (last < MAILBOX_FRAMES)
	{
		const eyetoy_frame_buffer* f = eyetoy_mailbox_take(&m);
		if (!f)
			continue;
		taken++;
		if (f->generation <= last)
			backwards++;
		if (f->size != frame_size(f->generation))
			torn++;
		else
			for (u32 i = 0; i < f->size; i++)
				if (f->data[i] != (u8)(f->generation + i))
				{
					torn++;
					break;
				}
		/* The buffer is the consumer's until the next Take: read it twice. */
		if (f->data[0] != (u8)(f->generation))
			torn++;
		last = f->generation;
	}
	pthread_join(t, NULL);

	check(torn == 0, "every frame taken is whole");
	check(backwards == 0, "frames never go backwards");
	check(last == MAILBOX_FRAMES, "the last frame published is taken");
	check(taken <= MAILBOX_FRAMES, "no frame is taken twice");
	check(eyetoy_mailbox_take(&m) == NULL, "nothing left after the last");
	printf("    %u published, %u taken\n", MAILBOX_FRAMES, taken);

	/* Reset clears what is in flight. */
	u8* w = eyetoy_mailbox_work(&m);
	w[0] = 1;
	eyetoy_mailbox_publish(&m, 1, 99);
	eyetoy_mailbox_reset(&m);
	check(eyetoy_mailbox_take(&m) == NULL, "Reset drops the frame in flight");
	eyetoy_mailbox_free(&m);
}

/* ---- the sample ring ---- */

/* Fewer than 32768 a round, so a 16-bit sample is its own sequence number */
/* with no wrap, and "later" is just "greater". */
static const u32 RING_SAMPLES = 30000;
static const u32 RING_ROUNDS = 25;
static retro_atomic_int_t s_ring_done;
static u32 s_ring_dropped;

static void* ring_producer(void* arg)
{
	eyetoy_sample_ring* r = (eyetoy_sample_ring*)(arg);
	s_ring_dropped = 0;
	for (u32 i = 0; i < RING_SAMPLES; i++)
	{
		/* A full ring drops the sample, as the frontend side does. */
		if (!eyetoy_ring_push(r, (s16)(i)))
			s_ring_dropped++;
		if ((i & 1023) == 0)
			sched_yield();
	}
	retro_atomic_store_release_int(&s_ring_done, 1);
	return NULL;
}

static void test_ring(void)
{
	eyetoy_sample_ring* r = (eyetoy_sample_ring*)(malloc(sizeof(eyetoy_sample_ring)));
	eyetoy_ring_reset(r);
	check(eyetoy_ring_count(r) == 0 && eyetoy_ring_space(r) == EYETOY_RING_MASK, "ring starts empty");

	/* Single-threaded: fill, trim, pop. */
	for (u32 i = 0; i < 100; i++)
		eyetoy_ring_push(r, (s16)(i));
	check(eyetoy_ring_count(r) == 100, "100 pushed");
	eyetoy_ring_trim_to(r, 20);
	s16 out[64];
	u32 n = eyetoy_ring_pop(r, out, 64);
	check(n == 20 && out[0] == 80 && out[19] == 99, "TrimTo keeps the newest 20");
	check(eyetoy_ring_count(r) == 0, "popped empty");
	for (u32 i = 0; i < EYETOY_RING_SIZE + 10; i++)
		eyetoy_ring_push(r, 0);
	check(eyetoy_ring_count(r) == EYETOY_RING_MASK && eyetoy_ring_space(r) == 0, "a full ring holds SIZE-1 and drops");

	/* Two threads, several rounds: every sample is read once in order, or */
	/* was dropped by the producer on a full ring; nothing is read twice, */
	/* nothing comes back out of order, nothing is lost in between. */
	u32 disorder = 0, lost = 0, total_got = 0, total_dropped = 0;
	for (u32 round = 0; round < RING_ROUNDS; round++)
	{
		eyetoy_ring_reset(r);
		retro_atomic_int_init(&s_ring_done, 0);
		pthread_t t;
		pthread_create(&t, NULL, ring_producer, r);
		u32 got = 0;
		int last = -1;
		while (!retro_atomic_load_acquire_int(&s_ring_done) || eyetoy_ring_count(r))
		{
			n = eyetoy_ring_pop(r, out, 64);
			if (n == 0)
			{
				sched_yield();
				continue;
			}
			for (u32 i = 0; i < n; i++)
			{
				const int v = (u16)(out[i]);
				if (v <= last)
				{
					if (disorder < 3)
						printf("    disorder: %d then %d (pop %u of %u)\n", last, v, i, n);
					disorder++;
				}
				last = v;
				got++;
			}
		}
		pthread_join(t, NULL);
		if (got + s_ring_dropped != RING_SAMPLES)
			lost++;
		total_got += got;
		total_dropped += s_ring_dropped;
	}
	check(disorder == 0, "samples are read in the order pushed, none twice");
	check(lost == 0, "every sample is read or was dropped on full");
	printf("    %u rounds: %u pushed, %u read, %u dropped on full\n", RING_ROUNDS, RING_SAMPLES * RING_ROUNDS, total_got, total_dropped);
	free(r);
}

/* ---- Encoders ---- */

static u8* make_rgb(int w, int h)
{
	u8* rgb = (u8*)(malloc((size_t)(w) * h * 3));
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			u8* p = rgb + (y * w + x) * 3;
			/* Hard edges, so the MPEG escape path (|AC| > 127) is taken. */
			const u8 c = ((x / 8 + y / 8) & 1) ? 255 : 0;
			p[0] = c;
			p[1] = (u8)(x * 255 / w);
			p[2] = (u8)(255 - c);
		}
	return rgb;
}

static u16 be16(const u8* p)
{
	return (u16)((p[0] << 8) | p[1]);
}

static void test_jpeg(int w, int h)
{
	eyetoy_jpeg_tables* huff = (eyetoy_jpeg_tables*)malloc(sizeof(*huff));
	u8* rgb = make_rgb(w, h);
	u8* out = (u8*)(malloc(EYETOY_MAX_FRAME_SIZE));
	const u32 n = (eyetoy_jpeg_tables_init(huff), eyetoy_compress_jpeg(huff, out, EYETOY_MAX_FRAME_SIZE, rgb, w, h, 80));
	char what[96];
	snprintf(what, sizeof(what), "JPEG %dx%d: encodes", w, h);
	check(n > 0 && n < EYETOY_MAX_FRAME_SIZE, what);

	bool soi = n > 4 && out[0] == 0xFF && out[1] == 0xD8;
	bool eoi = n > 4 && out[n - 2] == 0xFF && out[n - 1] == 0xD9;
	bool sof = false, sos = false, dqt = false, dht = false, app0 = false;
	/* Walk the marker segments up to SOS. */
	u32 pos = 2;
	while (pos + 4 <= n && out[pos] == 0xFF)
	{
		const u8 marker = out[pos + 1];
		const u16 len = be16(out + pos + 2);
		if (marker == 0xDA)
		{
			sos = true;
			break;
		}
		if (marker == 0xE0)
			app0 = len >= 16 && !memcmp(out + pos + 4, "JFIF", 5);
		if (marker == 0xDB)
			dqt = len == 2 + 65 * 2;
		if (marker == 0xC4)
			dht = true;
		if (marker == 0xC0)
		{
			const u8* p = out + pos + 4;
			/* precision 8, height, width, 3 components: Y 2x1 table 0, Cb 1x1 table 1, Cr 1x1 table 1 */
			sof = len == 17 && p[0] == 8 && be16(p + 1) == h && be16(p + 3) == w && p[5] == 3 &&
				p[6] == 1 && p[7] == 0x21 && p[8] == 0 &&
				p[9] == 2 && p[10] == 0x11 && p[11] == 1 &&
				p[12] == 3 && p[13] == 0x11 && p[14] == 1;
		}
		pos += 2 + len;
	}
	snprintf(what, sizeof(what), "JPEG %dx%d: SOI, JFIF, DQT, SOF0 (4:2:2), DHT, SOS, EOI", w, h);
	check(soi && app0 && dqt && sof && dht && sos && eoi, what);

	/* Entropy-coded data never holds a bare 0xFF (it is stuffed), except the EOI marker. */
	bool stuffed = true;
	for (u32 i = pos + 2 + be16(out + pos + 2); i + 1 < n - 2; i++)
		if (out[i] == 0xFF && out[i + 1] != 0x00)
			stuffed = false;
	snprintf(what, sizeof(what), "JPEG %dx%d: 0xFF bytes in the scan are stuffed", w, h);
	check(stuffed, what);

	snprintf(what, sizeof(what), "JPEG %dx%d: a short buffer returns 0", w, h);
	check(eyetoy_compress_jpeg(huff, out, n / 2, rgb, w, h, 80) == 0, what);
	free(huff);
	free(out);
	free(rgb);
}

static void test_mpeg(void)
{
	const int w = 320, h = 240;
	u8* rgb = make_rgb(w, h);
	u8* out = (u8*)(malloc(EYETOY_MAX_FRAME_SIZE));
	eyetoy_frame_encoder* enc = eyetoy_encoder_create();
	const u32 n = eyetoy_encode_frame(enc, out, rgb, w, h, EYETOY_FORMAT_MPEG);
	check(n > 16 && n < EYETOY_MAX_FRAME_SIZE, "MPEG 320x240: encodes");
	/* "ipum", zeros, width and height little-endian. */
	check(!memcmp(out, "ipum\0\0\0\0", 8) && out[8] == (w & 0xFF) && out[9] == (w >> 8) &&
			  out[10] == (h & 0xFF) && out[11] == (h >> 8),
		"MPEG 320x240: IPU header");
	check(out[n - 4] == 0x00 && out[n - 3] == 0x00 && out[n - 2] == 0x01 && out[n - 1] == 0xB0,
		"MPEG 320x240: ends on end-of-sequence");
	/* Deterministic: the same picture encodes to the same bytes. */
	u8* out2 = (u8*)(malloc(EYETOY_MAX_FRAME_SIZE));
	const u32 n2 = eyetoy_encode_frame(enc, out2, rgb, w, h, EYETOY_FORMAT_MPEG);
	check(n2 == n && !memcmp(out, out2, n), "MPEG 320x240: deterministic");
	free(out2);
	eyetoy_encoder_free(enc);
	free(out);
	free(rgb);
}

static void test_yuv400(void)
{
	const int w = 320, h = 240;
	u8* rgb = (u8*)(malloc((size_t)(w) * h * 3));
	/* Grey = (x/4 + y/4 * 80) & 0xFF: every sampled pixel is its own index. */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			const u8 g = (u8)((x / 4 + (y / 4) * 80) & 0xFF);
			u8* p = rgb + (y * w + x) * 3;
			p[0] = p[1] = p[2] = g;
		}
	u8* out = (u8*)(malloc(EYETOY_MAX_FRAME_SIZE));
	eyetoy_frame_encoder* enc = eyetoy_encoder_create();
	const u32 n = eyetoy_encode_frame(enc, out, rgb, w, h, EYETOY_FORMAT_YUV400);
	check(n == 80 * 64, "YUV400: 80x64 bytes");
	bool layout = true;
	u32 pos = 0;
	for (int my = 0; my < 8 && layout; my++)
		for (int mx = 0; mx < 10 && layout; mx++)
			for (int y = 0; y < 8 && layout; y++)
				for (int x = 0; x < 8; x++, pos++)
				{
					const int sx = 8 * mx + x, sy = 8 * my + y;
					/* In the picture the grey comes back through the float luma sum, */
					/* which can land one below; past it, 0x01. */
					const int expect = (4 * sy < h) ? ((sx + sy * 80) & 0xFF) : 0x01;
					const int diff = (int)(out[pos]) - expect;
					if (diff < -1 || diff > 0 || (4 * sy >= h && diff != 0))
					{
						layout = false;
						break;
					}
				}
	check(layout, "YUV400: 8x8 blocks of every fourth pixel, 0x01 past the picture");
	eyetoy_encoder_free(enc);
	free(out);
	free(rgb);
}

int main(void)
{
	printf("== FrameMailbox ==\n");
	test_mailbox();
	printf("== SampleRing ==\n");
	test_ring();
	printf("== encoders ==\n");
	test_jpeg(320, 240);
	test_jpeg(100, 60);
	test_mpeg();
	test_yuv400();
	printf(s_bad ? "FAIL\n" : "PASS\n");
	return s_bad ? 1 : 0;
}
