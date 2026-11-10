/* Granite's ownership check (util/owned.hpp), the thing that proves the
 * device's state has one owner at a time:
 *   - the owner's nested entries pass;
 *   - a handoff to another thread, after the first has left, passes;
 *   - a second thread entering while the first is inside aborts
 *     (run as "overlap"; the build script expects it to fail). */
#define GRANITE_OWNERSHIP_CHECK 1
#include "owned.hpp"
#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <stdio.h>
#include <string.h>

static retro_atomic_int_t s_inside, s_go;

static void nested(int n)
{
	OWNED();
	if (n)
		nested(n - 1);
}

static void enter_and_hold(void *)
{
	OWNED();
	retro_atomic_store_release_int(&s_inside, 1);
	while (!retro_atomic_load_acquire_int(&s_go))
		sthread_yield();
}

static void enter_once(void *)
{
	nested(3);
}

int main(int argc, char **argv)
{
	sthread_t *t;
	retro_atomic_int_init(&s_inside, 0);
	retro_atomic_int_init(&s_go, 0);

	if (argc > 1 && !strcmp(argv[1], "overlap"))
	{
		t = sthread_create(enter_and_hold, nullptr);
		while (!retro_atomic_load_acquire_int(&s_inside))
			sthread_yield();
		nested(0); /* aborts: the other thread is inside */
		retro_atomic_store_release_int(&s_go, 1);
		sthread_join(t);
		return 0;
	}

	nested(5);
	t = sthread_create(enter_once, nullptr);
	sthread_join(t);
	nested(2);
	printf("  ok: nested entries and a handoff pass\n");
	return 0;
}
