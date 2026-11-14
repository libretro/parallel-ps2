/* What the stream buffer tests do, against the S_* operations the
 * includer defines (the C functions here, the C++ class when the
 * expected hash was made), on a simulated GPU: command buffers are
 * submitted with a fence counter each and complete at random later,
 * reservations of random sizes and alignments are written and committed,
 * and when one fails the command buffer is executed and waited for, as
 * the device does. The hash takes every answer, offset and wait; and no
 * reservation may overlap bytes a command buffer still in flight reads. */

#include <stdio.h>

static u64 s_current = 1;   /* the command buffer being recorded */
static u64 s_completed = 0; /* the last one the GPU finished */
static u32 s_h;
static u32 s_pick;
static int s_overlaps;
static u32 s_waits;

typedef struct region
{
	u32 offset, len;
	u64 counter;
} region;
static region s_inflight[4096];
static u32 s_ninflight;

static void mix(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		s_h ^= (v >> (i * 8)) & 0xFF;
		s_h *= 16777619u;
	}
}

static u32 pick(void)
{
	s_pick ^= s_pick << 13;
	s_pick ^= s_pick >> 17;
	s_pick ^= s_pick << 5;
	return s_pick;
}

/* the device */
static u64 dev_current(void) { return s_current; }
static u64 dev_completed(void) { return s_completed; }
static void dev_wait(u64 counter)
{
	s_waits++;
	mix(0x7A170000u ^ (u32)counter);
	if (counter >= s_current)
		s_overlaps += 1000; /* (waiting on the one being recorded never ends) */
	if (counter > s_completed)
		s_completed = counter;
}
static void submit(void)
{
	s_current++;
}
static void complete_some(void)
{
	if (s_completed + 1 < s_current)
		s_completed += 1 + pick() % (u32)(s_current - 1 - s_completed);
}

static void check_free(u32 offset, u32 len)
{
	u32 i, j = 0;
	for (i = 0; i < s_ninflight; i++)
	{
		const region* r = &s_inflight[i];
		if (r->counter <= s_completed)
			continue;
		s_inflight[j++] = *r;
		if (offset < r->offset + r->len && r->offset < offset + len)
			s_overlaps++;
	}
	s_ninflight = j;
}

static u32 run_scenario(u32 size, u32 iterations)
{
	static const u32 aligns[4] = {4, 16, 64, 256};
	u32 it;
	s_current = 1;
	s_completed = 0;
	s_h = 2166136261u;
	s_pick = 0xC0FFEE11u ^ size;
	s_overlaps = 0;
	s_ninflight = 0;
	s_waits = 0;

	mix((u32)S_CREATE(size));
	for (it = 0; it < iterations; it++)
	{
		const u32 op = pick() % 10;
		if (op < 6)
		{
			const u32 align = aligns[pick() % 4];
			const u32 bytes = 1 + pick() % (size / 3);
			int ok = S_RESERVE(bytes, align);
			mix((u32)ok);
			if (!ok)
			{
				/* execute the command buffer being recorded and wait */
				submit();
				dev_wait(s_current - 1);
				ok = S_RESERVE(bytes, align);
				mix(0x5EC0u | (u32)ok);
			}
			if (ok)
			{
				const u32 used = bytes - pick() % (bytes / 2 + 1);
				const u32 offset = S_OFFSET();
				mix(offset);
				check_free(offset, bytes);
				if (s_ninflight < 4096)
				{
					s_inflight[s_ninflight].offset  = offset;
					s_inflight[s_ninflight].len     = used;
					s_inflight[s_ninflight].counter = s_current;
					s_ninflight++;
				}
				S_COMMIT(used);
			}
		}
		else if (op < 8)
			submit();
		else
			complete_some();
	}
	mix(s_waits);
	S_DESTROY();
	return s_h;
}
