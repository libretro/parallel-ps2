/* GSRingHeap (GS/GSRingHeap.c), the SW renderer's per-draw heap:
 * allocations keep their alignment and contents until freed, in order
 * and out of it; a heap whose old buffer frees up in time reuses it, so
 * past its working size it allocates no more; and one thread allocating
 * while another frees, as the renderer's workers do. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include "GS/GSRingHeap.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static unsigned s_rng = 1;
static unsigned next_rand(void)
{
	s_rng = s_rng * 1103515245u + 12345u;
	return s_rng >> 8;
}

typedef struct live
{
	unsigned char* p;
	size_t size;
	unsigned char fill;
} live;

static void fill(live* l)
{
	memset(l->p, l->fill, l->size);
}

static int intact(const live* l)
{
	size_t i;
	for (i = 0; i < l->size; i++)
		if (l->p[i] != l->fill)
			return 0;
	return 1;
}

/* A window of allocations freed oldest first, now and then two swapped,
 * and one in every 97 held three times as long: what the renderer's
 * workers do to it */
#define WINDOW 48
static void churn(gs_ring_heap* h, int ops, int big)
{
	live win[WINDOW], pinned;
	int n = 0, k, pin_left = 0;
	memset(&pinned, 0, sizeof(pinned));

	for (k = 0; k < ops; k++)
	{
		live l;
		size_t align = (size_t)8 << (next_rand() % 4); /* to GS_RING_MAX_ALIGN */
		l.size = 1 + next_rand() % (big ? 60000 : 3000);
		l.fill = (unsigned char)k;
		l.p = (unsigned char*)gs_ring_heap_alloc(h, l.size, align);
		CHECK(((size_t)l.p & (align - 1)) == 0);
		fill(&l);

		if (!pinned.p && k % 97 == 0)
		{
			pinned = l;
			pin_left = WINDOW * 3;
			continue;
		}
		if (n == WINDOW)
		{
			int i = (next_rand() % 8) == 0 ? 1 : 0;
			CHECK(intact(&win[i]));
			gs_ring_heap_free(win[i].p);
			memmove(&win[i], &win[i + 1], (size_t)(n - i - 1) * sizeof(win[0]));
			n--;
		}
		win[n++] = l;
		if (pinned.p && --pin_left == 0)
		{
			CHECK(intact(&pinned));
			gs_ring_heap_free(pinned.p);
			pinned.p = NULL;
		}
	}
	while (n)
	{
		CHECK(intact(&win[n - 1]));
		gs_ring_heap_free(win[--n].p);
	}
	if (pinned.p)
	{
		CHECK(intact(&pinned));
		gs_ring_heap_free(pinned.p);
	}
}

/* Draws freed promptly but for one now and then held for a few wraps of
 * the buffer: the buffer it pins is left, and taken back once it is
 * freed, instead of the heap growing until its wraps outlast the pin */
static void pinned_draws(gs_ring_heap* h)
{
	live win[4], pin;
	int n = 0, k, pin_left = 0;
	memset(&pin, 0, sizeof(pin));
	for (k = 0; k < 100000; k++)
	{
		live l;
		l.size = 256 + next_rand() % 2800;
		l.fill = (unsigned char)k;
		l.p = (unsigned char*)gs_ring_heap_alloc(h, l.size, 64);
		fill(&l);
		if (!pin.p && k % 200 == 0)
		{
			pin = l;
			pin_left = 150;
			continue;
		}
		if (n == 4)
		{
			CHECK(intact(&win[0]));
			gs_ring_heap_free(win[0].p);
			memmove(&win[0], &win[1], 3 * sizeof(win[0]));
			n--;
		}
		win[n++] = l;
		if (pin.p && --pin_left == 0)
		{
			CHECK(intact(&pin));
			gs_ring_heap_free(pin.p);
			pin.p = NULL;
		}
	}
	while (n)
		gs_ring_heap_free(win[--n].p);
	if (pin.p)
		gs_ring_heap_free(pin.p);
}

/* One thread allocating, one freeing, through a ring of pointers */
#define RING 256
typedef struct handoff
{
	live slots[RING];
	retro_atomic_size_t head, tail;
	retro_atomic_int_t done;
	int bad;
} handoff;

static void consumer(void* arg)
{
	handoff* q = (handoff*)arg;
	size_t tail = 0;
	for (;;)
	{
		size_t head = retro_atomic_load_acquire_size(&q->head);
		if (tail == head)
		{
			if (retro_atomic_load_acquire_int(&q->done)
				&& tail == retro_atomic_load_acquire_size(&q->head))
				break;
			continue;
		}
		if (!intact(&q->slots[tail % RING]))
			q->bad++;
		gs_ring_heap_free(q->slots[tail % RING].p);
		retro_atomic_store_release_size(&q->tail, ++tail);
	}
}

static void threaded(void)
{
	static handoff q;
	gs_ring_heap h;
	sthread_t* t;
	size_t head = 0;
	int k;

	memset(&q, 0, sizeof(q));
	gs_ring_heap_init(&h, 4000);
	t = sthread_create(consumer, &q);
	CHECK(t != NULL);
	if (!t)
		return;
	for (k = 0; k < 200000; k++)
	{
		live* l;
		while (head - retro_atomic_load_acquire_size(&q.tail) == RING)
			;
		l = &q.slots[head % RING];
		l->size = 1 + next_rand() % 4000;
		l->fill = (unsigned char)(k * 7);
		l->p = (unsigned char*)gs_ring_heap_alloc(&h, l->size, 16);
		fill(l);
		retro_atomic_store_release_size(&q.head, ++head);
	}
	retro_atomic_store_release_int(&q.done, 1);
	sthread_join(t);
	CHECK(q.bad == 0);
	gs_ring_heap_deinit(&h);
}

int main(void)
{
	gs_ring_heap h;
	size_t warm;

	/* Small allocations, then ones that outgrow the buffers made for them */
	gs_ring_heap_init(&h, 3000);
	churn(&h, 20000, 0);
	churn(&h, 20000, 1);

	/* Grown to its working size, it takes its old buffer again */
	warm = h.made;
	churn(&h, 200000, 1);
	if (h.made != warm)
		printf("  buffers: %u after warming up, %u after\n", (unsigned)warm, (unsigned)h.made);
	CHECK(h.made == warm);
	CHECK(h.made <= 12);
	gs_ring_heap_deinit(&h);

	/* The two buffers made with the heap, taking turns: none made after */
	gs_ring_heap_init(&h, 3056);
	CHECK(h.made == 2);
	pinned_draws(&h);
	if (h.made != 2)
		printf("  buffers: %u for pinned draws\n", (unsigned)h.made);
	CHECK(h.made == 2);
	gs_ring_heap_deinit(&h);

	/* Draws of up to 400 KiB, two alive at once: the buffers made for
	 * them hold them, and none is made after */
	gs_ring_heap_init(&h, 400 * 1024);
	{
		live big[2];
		int k;
		memset(big, 0, sizeof(big));
		for (k = 0; k < 2000; k++)
		{
			live* l = &big[k & 1];
			if (l->p)
			{
				CHECK(intact(l));
				gs_ring_heap_free(l->p);
			}
			l->size = 64 * 1024 + next_rand() % (336 * 1024);
			l->fill = (unsigned char)k;
			l->p    = (unsigned char*)gs_ring_heap_alloc(&h, l->size, 64);
			fill(l);
		}
		for (k = 0; k < 2; k++)
		{
			CHECK(intact(&big[k]));
			gs_ring_heap_free(big[k].p);
		}
	}
	if (h.made != 2)
		printf("  buffers: %u for big draws\n", (unsigned)h.made);
	CHECK(h.made == 2);
	gs_ring_heap_deinit(&h);

	/* The largest allocation exactly, either side of where the buffers
	 * double (its two prefixes taking it over half of one, or to exactly
	 * half): still in the buffers made for it */
	{
		static const size_t sizes[3] = {512 * 1024, 512 * 1024 - 2 * sizeof(size_t) + 4, 512 * 1024 - 2 * sizeof(size_t)};
		void* p[2];
		int i, k;
		for (i = 0; i < 3; i++)
		{
			gs_ring_heap_init(&h, sizes[i]);
			p[0] = p[1] = NULL;
			for (k = 0; k < 16; k++)
			{
				if (p[k & 1])
					gs_ring_heap_free(p[k & 1]);
				p[k & 1] = gs_ring_heap_alloc(&h, sizes[i], 64);
				CHECK(((size_t)p[k & 1] & 63) == 0);
			}
			gs_ring_heap_free(p[0]);
			gs_ring_heap_free(p[1]);
			if (h.made != 2)
				printf("  buffers: %u for %u-byte draws\n", (unsigned)h.made, (unsigned)sizes[i]);
			CHECK(h.made == 2);
			gs_ring_heap_deinit(&h);
		}
	}

	threaded();

	if (fails)
	{
		printf("ringheap: %d failed\n", fails);
		return 1;
	}
	printf("ringheap: ok\n");
	return 0;
}
