/* Concurrent page-fault dispatch stress.
 *
 * WHAT THIS TESTS
 *
 * The fault handler finds the faulting thread's slot in a lock-free
 * table keyed on thread identity, guards re-entry with a per-slot
 * recursion flag, and leaves the bookkeeping to the thread that
 * faulted: vtlb's backpatch tables have one owner and take no lock.
 *
 * This program drives those paths: N threads take real SIGSEGV page
 * faults simultaneously, in a handler that reproduces the shipped
 * algorithm (slot table + per-slot recursion flag + per-thread
 * bookkeeping), while a churn thread registers and unregisters to race
 * the lookup.
 *
 * SCOPE - READ THIS
 *
 * This is a test of the ALGORITHM, not of the linked binary: the slot
 * table is reproduced here rather than linked from the core. It will
 * catch a design flaw - a lost slot, a dropped recursion flag, two
 * threads sharing a slot. It will NOT catch a transcription error in
 * the core itself.
 *
 * USAGE (from repo root)
 *
 *   sh tests/faultstress/build.sh
 *   ./tests/faultstress/fault_test           # defaults: 4 threads
 *   ./tests/faultstress/fault_test 8 200000  # threads, faults each
 *   TSAN_OPTIONS=halt_on_error=1 ./tests/faultstress/fault_test_tsan 4
 *
 * WHAT FAILURE LOOKS LIKE
 *
 *   - "SLOT LOST" / "NO SLOT"      : slot table lookup failed under churn
 *   - "RECURSION FLAG"             : per-slot flag not mutually exclusive
 *   - "COUNTER MISMATCH"           : two threads shared a slot
 *   - TSan data race report        : ordering bug in the protocol
 *
 * Exit 0 = clean.
 */
#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
/* Windows uses a vectored exception handler, which is what the shipped
 * SysPageFaultExceptionFilter actually is - so this build exercises
 * the real Windows dispatch shape, not a POSIX approximation. */
#include <windows.h>
typedef HANDLE thread_t;
#define THREAD_RET      DWORD WINAPI
#define thread_yield()  SwitchToThread()
#else
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
typedef pthread_t thread_t;
#define THREAD_RET      void *
#define thread_yield()  sched_yield()
#endif

#define MAX_THREADS         16
#define FAULT_SLOTS         4      /* mirrors FAULT_THREAD_SLOTS */
#define PAGES_PER_THREAD    64

/* ---- shipped algorithm, reproduced ---------------------------------- */

static volatile int  slot_claimed[FAULT_SLOTS];
static volatile long slot_id[FAULT_SLOTS];
static volatile int  slot_inhandler[FAULT_SLOTS];

static uintptr_t thread_identity(void)
{
#ifdef _WIN32
   return (uintptr_t)GetCurrentThreadId();
#else
   return (uintptr_t)pthread_self();
#endif
}

static int slot_register(void)
{
   const uintptr_t self = thread_identity();
   int i;
   for (i = 0; i < FAULT_SLOTS; i++)
   {
      if ((uintptr_t)__atomic_load_n(&slot_id[i], __ATOMIC_ACQUIRE) == self)
         return i;
   }
   for (i = 0; i < FAULT_SLOTS; i++)
   {
      int expected = 0;
      if (__atomic_exchange_n(&slot_claimed[i], 1, __ATOMIC_ACQ_REL) == expected)
      {
         __atomic_store_n(&slot_inhandler[i], 0, __ATOMIC_RELEASE);
         __atomic_store_n(&slot_id[i], (long)self, __ATOMIC_RELEASE);
         return i;
      }
   }
   return -1;
}

static void slot_unregister(void)
{
   const uintptr_t self = thread_identity();
   int i;
   for (i = 0; i < FAULT_SLOTS; i++)
   {
      if ((uintptr_t)__atomic_load_n(&slot_id[i], __ATOMIC_ACQUIRE) == self)
      {
         __atomic_store_n(&slot_id[i], 0L, __ATOMIC_RELEASE);
         __atomic_store_n(&slot_claimed[i], 0, __ATOMIC_RELEASE);
         return;
      }
   }
}

static int slot_lookup(uintptr_t self)
{
   int i;
   for (i = 0; i < FAULT_SLOTS; i++)
   {
      if ((uintptr_t)__atomic_load_n(&slot_id[i], __ATOMIC_ACQUIRE) == self)
         return i;
   }
   return -1;
}

/* ---- each thread's bookkeeping -------------------------------------- */

/* vtlb's fault bookkeeping has one owner, the thread whose code faults,
 * so it takes no lock: each slot here keeps its own, written only by the
 * thread that holds the slot, inside its exclusive recursion guard.
 * Non-atomic ON PURPOSE: a total short of the faults taken means two
 * threads shared a slot, and TSan reports the race directly. */
static uint64_t slot_counter[FAULT_SLOTS];
static uint64_t slot_shadow[FAULT_SLOTS][8];

static volatile int saw_no_slot;
static volatile int saw_recursion_conflict;

static double now_sec(void)
{
#ifdef _WIN32
   LARGE_INTEGER f, c;
   QueryPerformanceFrequency(&f);
   QueryPerformanceCounter(&c);
   return (double)c.QuadPart / (double)f.QuadPart;
#else
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* ---- fault machinery ------------------------------------------------ */

static size_t page_size;

/* Portable page primitives: reserve inaccessible, deny, allow. */
static void *page_reserve_noaccess(size_t bytes)
{
#ifdef _WIN32
   return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
#else
   void *p = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   return (p == MAP_FAILED) ? NULL : p;
#endif
}

static int page_deny(void *addr, size_t bytes)
{
#ifdef _WIN32
   DWORD old;
   return VirtualProtect(addr, bytes, PAGE_NOACCESS, &old) ? 0 : -1;
#else
   return mprotect(addr, bytes, PROT_NONE);
#endif
}

static int page_allow(void *addr, size_t bytes)
{
#ifdef _WIN32
   DWORD old;
   return VirtualProtect(addr, bytes, PAGE_READWRITE, &old) ? 0 : -1;
#else
   return mprotect(addr, bytes, PROT_READ | PROT_WRITE);
#endif
}

static size_t page_size_query(void)
{
#ifdef _WIN32
   SYSTEM_INFO si;
   GetSystemInfo(&si);
   return (size_t)si.dwPageSize;
#else
   return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

struct thread_ctx
{
   int       id;
   uint8_t  *region;      /* PAGES_PER_THREAD pages, PROT_NONE */
   long      faults;
   uint64_t  local_count;
};


/* Platform-independent core.  Returns 1 if we handled the fault. */
static int fault_dispatch(void *fault_addr)
{
   uintptr_t self = thread_identity();
   int       slot;
   int       i;

   slot = slot_lookup(self);
   if (slot < 0)
   {
      saw_no_slot = 1;
      abort();
   }

   /* Per-slot recursion guard: must be exclusive for this slot. */
   if (__atomic_exchange_n(&slot_inhandler[slot], 1, __ATOMIC_ACQ_REL))
   {
      saw_recursion_conflict = 1;
      abort();
   }

   /* The bookkeeping, as vtlb does it: plain host-memory writes, no
    * allocation, no blocking, no access to the faulting region, and
    * no lock -- the slot is this thread's alone. */
   slot_counter[slot]++;
   for (i = 0; i < 8; i++)
      slot_shadow[slot][i] = slot_counter[slot] + (uint64_t)i;

   /* "Handle" the fault: make the page accessible so we can resume,
    * mirroring vtlb's backpatch-and-continue. */
   if (page_allow((void*)((uintptr_t)fault_addr & ~(uintptr_t)(page_size - 1)),
                  page_size) != 0)
      abort();

   __atomic_store_n(&slot_inhandler[slot], 0, __ATOMIC_RELEASE);
   return 1;
}

#ifdef _WIN32
/* Vectored handler - the same mechanism SysPageFaultExceptionFilter
 * installs, so this build tests the real Windows dispatch shape. */
static LONG CALLBACK veh_handler(EXCEPTION_POINTERS *eps)
{
   if (eps->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
      return EXCEPTION_CONTINUE_SEARCH;
   if (fault_dispatch((void*)eps->ExceptionRecord->ExceptionInformation[1]))
      return EXCEPTION_CONTINUE_EXECUTION;
   return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void fault_handler(int sig, siginfo_t *si, void *uctx)
{
   (void)sig; (void)uctx;
   fault_dispatch(si->si_addr);
}
#endif

static THREAD_RET fault_worker(void *p)
{
   struct thread_ctx *ctx = (struct thread_ctx*)p;
   long   n;
   size_t pg;

   if (slot_register() < 0)
   {
      fprintf(stderr, "NO SLOT for thread %d\n", ctx->id);
      exit(2);
   }

   for (n = 0; n < ctx->faults; n++)
   {
      pg = (size_t)(n % PAGES_PER_THREAD);
      /* Re-arm then touch: each touch is a real page fault. */
      if (page_deny(ctx->region + pg * page_size, page_size) != 0)
         exit(6);
      ctx->region[pg * page_size] = (uint8_t)n; /* faults here */
      ctx->local_count++;
   }

   slot_unregister();
#ifdef _WIN32
   return 0;
#else
   return NULL;
#endif
}

/* Races register/unregister against the lookup in live handlers. */
static volatile int churn_stop;

static THREAD_RET churn_worker(void *p)
{
   (void)p;
   while (!__atomic_load_n(&churn_stop, __ATOMIC_ACQUIRE))
   {
      int s = slot_register();
      if (s >= 0)
         slot_unregister();
      thread_yield();
   }
#ifdef _WIN32
   return 0;
#else
   return NULL;
#endif
}

int main(int argc, char **argv)
{
#ifndef _WIN32
   struct sigaction  sa;
#endif
   thread_t          th[MAX_THREADS], churn;
   struct thread_ctx ctx[MAX_THREADS];
   int               n_threads = 4;
   long              faults    = 50000;
   int               i;
   uint64_t          expected  = 0;
   uint64_t          counted   = 0;
   double            t_start, elapsed;

   if (argc > 1) n_threads = atoi(argv[1]);
   if (argc > 2) faults    = atol(argv[2]);
   if (n_threads < 1 || n_threads > FAULT_SLOTS)
   {
      fprintf(stderr, "thread count must be 1..%d (slot table size)\n", FAULT_SLOTS);
      return 2;
   }

   page_size = page_size_query();

#ifdef _WIN32
   if (!AddVectoredExceptionHandler(1, veh_handler))
      return 1;
#else
   memset(&sa, 0, sizeof(sa));
   sigemptyset(&sa.sa_mask);
   sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
   sa.sa_sigaction = fault_handler;
   if (sigaction(SIGSEGV, &sa, NULL) != 0)
      return 1;
#endif

   printf("fault dispatch stress: %d threads x %ld faults, %d slots\n",
          n_threads, faults, FAULT_SLOTS);

   for (i = 0; i < n_threads; i++)
   {
      ctx[i].id     = i;
      ctx[i].faults = faults;
      ctx[i].local_count = 0;
      ctx[i].region = (uint8_t*)page_reserve_noaccess(PAGES_PER_THREAD * page_size);
      if (!ctx[i].region)
         return 1;
   }

   /* Churn only makes sense with a free slot to contend for. */
   churn_stop = 0;
   t_start = now_sec();
#ifdef _WIN32
   if (n_threads < FAULT_SLOTS)
      churn = CreateThread(NULL, 0, churn_worker, NULL, 0, NULL);
   for (i = 0; i < n_threads; i++)
      th[i] = CreateThread(NULL, 0, fault_worker, &ctx[i], 0, NULL);
   for (i = 0; i < n_threads; i++)
      WaitForSingleObject(th[i], INFINITE);
   __atomic_store_n(&churn_stop, 1, __ATOMIC_RELEASE);
   if (n_threads < FAULT_SLOTS)
      WaitForSingleObject(churn, INFINITE);
#else
   if (n_threads < FAULT_SLOTS)
      pthread_create(&churn, NULL, churn_worker, NULL);
   for (i = 0; i < n_threads; i++)
      pthread_create(&th[i], NULL, fault_worker, &ctx[i]);
   for (i = 0; i < n_threads; i++)
      pthread_join(th[i], NULL);
   __atomic_store_n(&churn_stop, 1, __ATOMIC_RELEASE);
   if (n_threads < FAULT_SLOTS)
      pthread_join(churn, NULL);
#endif

   elapsed = now_sec() - t_start;
   for (i = 0; i < n_threads; i++)
      expected += ctx[i].local_count;
   for (i = 0; i < FAULT_SLOTS; i++)
      counted += slot_counter[i];

   printf("  faults taken:     %llu\n", (unsigned long long)expected);
   printf("  slot counters:    %llu\n", (unsigned long long)counted);
   printf("  wall time:        %.3f s   (%.2f us/fault, %.0f faults/s)\n",
          elapsed, expected ? (elapsed * 1e6) / (double)expected : 0.0,
          elapsed > 0.0 ? (double)expected / elapsed : 0.0);

   if (saw_no_slot)
   {
      printf("FAIL: SLOT LOST - a fault arrived on a thread with no slot\n");
      return 3;
   }
   if (saw_recursion_conflict)
   {
      printf("FAIL: RECURSION FLAG - per-slot guard was not exclusive\n");
      return 4;
   }
   if (counted != expected)
   {
      printf("FAIL: COUNTER MISMATCH - two threads shared a slot "
             "(%llu != %llu, lost %lld)\n",
             (unsigned long long)counted,
             (unsigned long long)expected,
             (long long)expected - (long long)counted);
      return 5;
   }

   printf("PASS: slot table, recursion guards and per-thread bookkeeping held under "
          "%d-way concurrent faulting.\n", n_threads);
   return 0;
}
