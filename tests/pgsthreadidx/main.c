/* paraLLEl-GS's command-pool thread indices, on the real thread_id.cpp.
 *
 * The device keeps one command pool per thread index, and a thread's index
 * is handed out on its first request. Each device - one per session, and
 * a core image can run many sessions - has pools of its own, so:
 *   - within a device, the device thread holds 0 and every other recording
 *     thread a distinct index of its own, never 0;
 *   - a later device hands its indices out again from 1, to new threads and
 *     to threads kept from the session before alike, so session after
 *     session no thread is ever pushed onto pool 0.
 * C89. */
#include <stdio.h>

#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include <retro_atomic.h>

#define COUNT    4 /* GSRendererPGS's PGS_THREAD_INDEX_COUNT */
#define SESSIONS 6

void     tid_set_count(unsigned count);
void     tid_register(unsigned index);
unsigned tid_get(void);

static unsigned s_got[COUNT];

static void ask(void *slot)
{
	*(unsigned*)slot = tid_get();
}

/* A thread kept across sessions: asks once per session when told to.
 * The handshake is two counters and an eventcount; each side waits for
 * the other's counter to move. */
static retro_eventcount_t s_ec;
static retro_atomic_int_t s_round, s_answered, s_quit;
static unsigned           s_kept_index;

/* Park until *v holds `want`, or the quit flag is up when `quit_too`. */
static void wait_for(retro_atomic_int_t *v, int want, int quit_too)
{
	for (;;)
	{
		int key;
		if (retro_atomic_load_acquire_int(v) == want
		 || (quit_too && retro_atomic_load_acquire_int(&s_quit)))
			return;
		key = retro_eventcount_prepare_wait(&s_ec);
		if (retro_atomic_load_acquire_int(v) == want
		 || (quit_too && retro_atomic_load_acquire_int(&s_quit)))
		{
			retro_eventcount_cancel_wait(&s_ec);
			return;
		}
		retro_eventcount_commit_wait(&s_ec, key);
	}
}

static void kept_thread(void *unused)
{
	int seen = 0;
	(void)unused;
	for (;;)
	{
		wait_for(&s_round, seen + 1, 1);
		if (retro_atomic_load_acquire_int(&s_quit))
			break;
		seen++;
		s_kept_index = tid_get();
		retro_atomic_store_release_int(&s_answered, seen);
		retro_eventcount_notify(&s_ec);
	}
}

int main(void)
{
	sthread_t *kept, *t[COUNT];
	int session, i, j, failures = 0;

	retro_eventcount_init(&s_ec);
	kept = sthread_create(kept_thread, NULL);

	for (session = 1; session <= SESSIONS; session++)
	{
		unsigned seen[COUNT + 1];
		int n = 0;

		/* The device is made: the count, then its own thread takes 0. */
		tid_set_count(COUNT);
		tid_register(0);
		seen[n++] = tid_get();

		/* The thread kept from the last session records again. */
		retro_atomic_store_release_int(&s_round, session);
		retro_eventcount_notify(&s_ec);
		wait_for(&s_answered, session, 0);
		seen[n++] = s_kept_index;

		/* And new threads, up to the pools there are. */
		for (i = 0; i < COUNT - 2; i++)
			t[i] = sthread_create(ask, &s_got[i]);
		for (i = 0; i < COUNT - 2; i++)
		{
			sthread_join(t[i]);
			seen[n++] = s_got[i];
		}

		if (seen[0] != 0)
		{
			printf("  FAIL: session %d: the device thread holds %u, not 0\n", session, seen[0]);
			failures++;
		}
		for (i = 1; i < n; i++)
		{
			if (seen[i] == 0 || seen[i] >= COUNT)
			{
				printf("  FAIL: session %d: a recording thread was given index %u\n", session, seen[i]);
				failures++;
			}
			for (j = 0; j < i; j++)
				if (seen[i] == seen[j])
				{
					printf("  FAIL: session %d: two threads share index %u\n", session, seen[i]);
					failures++;
				}
		}
	}

	retro_atomic_store_release_int(&s_quit, 1);
	retro_eventcount_notify(&s_ec);
	sthread_join(kept);
	retro_eventcount_free(&s_ec);

	printf(failures ? "thread indices: FAILED (%d)\n" : "thread indices: ok, %d sessions\n",
			failures ? failures : SESSIONS);
	return failures != 0;
}
