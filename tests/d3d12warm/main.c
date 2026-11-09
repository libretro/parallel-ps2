/* The D3D12 renderer's list of draw pipelines and its warm-up thread
 * (pcsx2/GS/Renderers/DX12/GSD3D12Warm.c), without a GPU: the records are
 * opaque bytes and the build callback is arithmetic.
 *
 * The list: records once each and sorted; a saved list loads back the
 * same; a file cut short, with a byte changed, of another version or
 * record size, with bytes past its records, or not a list at all is
 * refused, as is a missing one.
 *
 * The thread: every job it builds comes back once, taken while it runs,
 * in the order of the jobs (the renderer puts what it needs first first);
 * a job it declines comes back not at all; stopped part way it stops,
 * and what it finished is still there to take; in a build without
 * threads it does not start. C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GSD3D12Warm.h"

static unsigned failures;

static void check(int ok, const char *what)
{
   if (!ok)
   {
      printf("  FAIL: %s\n", what);
      failures++;
   }
}

/* ---- the list ---- */

typedef struct rec
{
   unsigned char b[24];
} rec_t;

static void rec_make(rec_t *r, unsigned v)
{
   unsigned i;
   /* the value itself in the first bytes, so no two values are alike */
   for (i = 0; i < sizeof(r->b); i++)
      r->b[i] = (unsigned char)(i * 13u + 5u);
   r->b[0] = (unsigned char)v;
   r->b[1] = (unsigned char)(v >> 8);
}

static char path_buf[1024];

static const char *path_in(const char *dir, const char *name)
{
   strcpy(path_buf, dir);
   strcat(path_buf, "/");
   strcat(path_buf, name);
   return path_buf;
}

static long file_bytes(const char *path, unsigned char **out)
{
   FILE          *fp = fopen(path, "rb");
   long           n;
   unsigned char *buf;
   if (!fp)
      return -1;
   fseek(fp, 0, SEEK_END);
   n = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   buf = (unsigned char*)malloc(n ? (size_t)n : 1);
   if (fread(buf, 1, (size_t)n, fp) != (size_t)n)
      n = -1;
   fclose(fp);
   *out = buf;
   return n;
}

static void file_put(const char *path, const unsigned char *b, long n)
{
   FILE *fp = fopen(path, "wb");
   if (!fp)
      return;
   fwrite(b, 1, (size_t)n, fp);
   fclose(fp);
}

static int list_refuses(const char *path, uint32_t version, size_t rec_size)
{
   gs_warm_list_t l;
   int            refused;
   gs_warm_list_init(&l, rec_size);
   refused = !gs_warm_list_load(&l, path, version) && l.count == 0 && l.recs == NULL;
   gs_warm_list_free(&l);
   return refused;
}

static void check_list(const char *dir)
{
   gs_warm_list_t l, m;
   rec_t          r;
   unsigned       i;
   int            sorted = 1;
   unsigned char *bytes;
   long           n;
   const char    *good;
   char           good_path[1024];

   gs_warm_list_init(&l, sizeof(rec_t));
   /* 300 records, each of 100 values three times, in no order */
   for (i = 0; i < 300; i++)
   {
      rec_make(&r, (i * 37u) % 100u);
      check(gs_warm_list_add(&l, &r), "add");
   }
   gs_warm_list_unique(&l);
   check(l.count == 100, "each record once");
   for (i = 1; i < l.count; i++)
      if (memcmp(l.recs + (i - 1) * l.rec_size, l.recs + i * l.rec_size, l.rec_size) >= 0)
         sorted = 0;
   check(sorted, "sorted");

   good = path_in(dir, "list.bin");
   strcpy(good_path, good);
   check(gs_warm_list_save(&l, good_path, 0x3A01), "save");
   gs_warm_list_init(&m, sizeof(rec_t));
   check(gs_warm_list_load(&m, good_path, 0x3A01), "load");
   check(m.count == l.count && !memcmp(m.recs, l.recs, l.count * l.rec_size), "loads back the same");
   gs_warm_list_free(&m);

   /* an empty list is a list */
   {
      gs_warm_list_t e;
      gs_warm_list_init(&e, sizeof(rec_t));
      check(gs_warm_list_save(&e, path_in(dir, "empty.bin"), 0x3A01), "save an empty list");
      check(gs_warm_list_load(&e, path_in(dir, "empty.bin"), 0x3A01) && e.count == 0, "load an empty list");
      gs_warm_list_free(&e);
   }

   n = file_bytes(good_path, &bytes);
   check(n == 24 + 100 * (long)sizeof(rec_t), "the file's length");
   if (n > 0)
   {
      unsigned char *copy = (unsigned char*)malloc((size_t)n + 16);
      const char    *bad  = path_in(dir, "bad.bin");
      char           bad_path[1024];
      strcpy(bad_path, bad);

      memcpy(copy, bytes, (size_t)n);
      file_put(bad_path, copy, n - 5);
      check(list_refuses(bad_path, 0x3A01, sizeof(rec_t)), "a list cut short is refused");

      memcpy(copy, bytes, (size_t)n);
      copy[24 + 57] ^= 0x10;
      file_put(bad_path, copy, n);
      check(list_refuses(bad_path, 0x3A01, sizeof(rec_t)), "a changed byte is refused");

      memcpy(copy, bytes, (size_t)n);
      file_put(bad_path, copy, n);
      check(list_refuses(bad_path, 0x3A02, sizeof(rec_t)), "another version is refused");
      check(list_refuses(bad_path, 0x3A01, sizeof(rec_t) + 8), "another record size is refused");

      memcpy(copy, bytes, (size_t)n);
      memset(copy + n, 0, 16);
      file_put(bad_path, copy, n + 16);
      check(list_refuses(bad_path, 0x3A01, sizeof(rec_t)), "bytes past the records are refused");

      for (i = 0; i < (unsigned)n; i++)
         copy[i] = (unsigned char)(i * 151u + 7u);
      file_put(bad_path, copy, n);
      check(list_refuses(bad_path, 0x3A01, sizeof(rec_t)), "not a list is refused");

      memcpy(copy, bytes, (size_t)n);
      copy[16] = 0xFF; /* the count, past what the file holds */
      file_put(bad_path, copy, n);
      check(list_refuses(bad_path, 0x3A01, sizeof(rec_t)), "a count the file does not hold is refused");

      free(copy);
      free(bytes);
   }
   check(list_refuses(path_in(dir, "missing.bin"), 0x3A01, sizeof(rec_t)), "a missing list");
   gs_warm_list_free(&l);
}

/* ---- the thread ---- */

typedef struct job
{
   unsigned v;
   unsigned work;   /* how long it takes */
} job_t;

typedef struct res
{
   unsigned v;
   unsigned sum;
} res_t;

static unsigned job_sum(const job_t *j)
{
   unsigned s = j->v, i;
   for (i = 0; i < j->work; i++)
      s = s * 1103515245u + 12345u;
   return s;
}

static int build(void *ud, const void *job, void *result)
{
   const job_t *j = (const job_t*)job;
   res_t       *r = (res_t*)result;
   (void)ud;
   if (j->v % 7u == 3u)   /* some it declines */
      return 0;
   r->v   = j->v;
   r->sum = job_sum(j);
   return 1;
}

#define JOBS 2000

static void check_thread(void)
{
   job_t         *jobs  = (job_t*)malloc(JOBS * sizeof(job_t));
   unsigned char *seen  = (unsigned char*)calloc(JOBS, 1);
   gs_warm_t     *w;
   res_t          r;
   unsigned       i, got = 0, bad = 0, want = 0, last = 0, order_bad = 0;
   unsigned long  idle = 0;

   for (i = 0; i < JOBS; i++)
   {
      jobs[i].v    = i;
      jobs[i].work = 2000u + (i % 13u) * 300u;
      if (i % 7u != 3u)
         want++;
   }

   w = gs_warm_start(jobs, sizeof(job_t), JOBS, 0, sizeof(res_t), build, NULL);
#ifdef HAVE_THREADS
   check(w != NULL, "the thread starts");
   /* the jobs are its own copy */
   memset(jobs, 0xEE, JOBS * sizeof(job_t));
   /* taken while it runs, as the drawing thread does on a miss */
   /* (a result that never comes is a failure, not a hang: the thread is
    * long done once nothing has come for this many tries) */
   while (got < want && idle < 400000000ul)
   {
      if (!gs_warm_take(w, &r))
      {
         idle++;
         continue;
      }
      idle = 0;
      if (got && r.v <= last)
         order_bad++;
      last = r.v;
      if (r.v >= JOBS || seen[r.v])
         bad++;
      else
      {
         job_t j;
         j.v    = r.v;
         j.work = 2000u + (r.v % 13u) * 300u;
         seen[r.v] = 1;
         if (r.sum != job_sum(&j) || r.v % 7u == 3u)
            bad++;
      }
      got++;
   }
   gs_warm_stop(w);
   check(!gs_warm_take(w, &r), "nothing more than it built");
   check(!bad && got == want, "every job it built came back once, right");
   check(!order_bad, "in the order of the jobs");
   gs_warm_free(w);

   /* stopped part way: it stops, and what it finished stays */
   for (i = 0; i < JOBS; i++)
   {
      jobs[i].v    = i;
      jobs[i].work = 200000u;
   }
   w = gs_warm_start(jobs, sizeof(job_t), JOBS, 0, sizeof(res_t), build, NULL);
   check(w != NULL, "the thread starts again");
   while (!gs_warm_take(w, &r))
      ;
   gs_warm_stop(w);
   for (got = 1; gs_warm_take(w, &r); got++)
      ;
   check(got < want, "stopped part way");
   gs_warm_free(w);

   /* freed before it finishes, with nothing taken */
   w = gs_warm_start(jobs, sizeof(job_t), JOBS, 0, sizeof(res_t), build, NULL);
   gs_warm_free(w);
#else
   check(w == NULL, "no thread without threads");
   check(!gs_warm_take(w, &r), "nothing to take without threads");
   gs_warm_stop(w);
   gs_warm_free(w);
   (void)got; (void)bad; (void)want; (void)seen; (void)idle; (void)last; (void)order_bad;
#endif
   check(gs_warm_start(jobs, sizeof(job_t), 0, 0, sizeof(res_t), build, NULL) == NULL, "no jobs, no thread");
   free(jobs);
   free(seen);
}

#define LISTED 300
#define ROOM   8

/* Past the list: jobs the drawing thread queues, a bounded number whose
 * results are not taken yet; one it declines gives its room back. */
static void check_queue(void)
{
   job_t     *jobs = (job_t*)malloc(LISTED * sizeof(job_t));
   gs_warm_t *w;
   res_t      r;
   job_t      j;
   unsigned   i, got, bad = 0, queued, want, next;
   unsigned long idle;

   for (i = 0; i < LISTED; i++)
   {
      jobs[i].v    = i;
      jobs[i].work = 5000u;
   }
   w = gs_warm_start(jobs, sizeof(job_t), LISTED, ROOM, sizeof(res_t), build, NULL);
#ifdef HAVE_THREADS
   check(w != NULL, "the thread starts with room to queue");
   /* the first part of the list, all of it built once this returns */
   gs_warm_wait(w, 100);
   for (got = 0; got < 100 && gs_warm_take(w, &r); )
   {
      if (r.v >= LISTED || r.sum != job_sum(&jobs[r.v]))
         bad++;
      if (r.v < 100)
         got++;
   }
   check(!bad, "the waited-for part came back right");
   check(got == 100 - 100 / 7 - (100 % 7 > 3), "all of the waited-for part was there");
   gs_warm_wait(w, LISTED);
   while (gs_warm_take(w, &r))
      ;

   /* the room: no more than ROOM at once */
   j.work = 3000u;
   for (queued = 0, next = 1000; queued < ROOM + 4; queued++, next++)
   {
      j.v = next;
      if (!gs_warm_queue(w, &j))
         break;
   }
   check(queued == ROOM, "as many queued as there is room for");
   /* taken back, declined ones included, the room comes back */
   want = 0;
   for (i = 1000; i < next; i++)
      if (i % 7u != 3u)
         want++;
   for (got = 0, idle = 0; got < want && idle < 400000000ul; )
   {
      if (!gs_warm_take(w, &r))
      {
         idle++;
         continue;
      }
      idle = 0;
      j.v = r.v;
      if (r.v < 1000 || r.v >= next || r.sum != job_sum(&j))
         bad++;
      got++;
   }
   check(!bad && got == want, "every queued job it built came back, right");
   /* (a declined one's record may still be in flight: drain them) */
   for (idle = 0, queued = 0; queued < ROOM && idle < 400000000ul; idle++)
   {
      j.v = next;
      if (gs_warm_queue(w, &j))
      {
         queued++;
         next++;
         continue;
      }
      while (gs_warm_take(w, &r))
         ;
   }
   check(queued == ROOM, "the room came back, declined ones too");
   /* freed with queued jobs not taken back */
   gs_warm_free(w);

   /* no list, only queued jobs */
   w = gs_warm_start(NULL, sizeof(job_t), 0, ROOM, sizeof(res_t), build, NULL);
   check(w != NULL, "a thread for queued jobs alone");
   gs_warm_wait(w, 5);
   j.v = 4; j.work = 10u;
   check(gs_warm_queue(w, &j), "queued with no list");
   for (idle = 0; !gs_warm_take(w, &r) && idle < 400000000ul; idle++)
      ;
   check(r.v == 4 && r.sum == job_sum(&j), "and built");
   /* stopped while it waits for a job */
   gs_warm_free(w);
#else
   check(w == NULL, "no thread without threads");
   j.v = 1; j.work = 1u;
   check(!gs_warm_queue(w, &j), "nothing queued without threads");
   gs_warm_wait(w, LISTED);
   gs_warm_free(w);
   (void)r; (void)i; (void)got; (void)bad; (void)queued; (void)want; (void)next; (void)idle;
#endif
   free(jobs);
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "usage: d3d12warm_test <scratch dir>\n");
      return 2;
   }
   check_list(argv[1]);
   check_thread();
   check_queue();
   if (failures)
   {
      printf("d3d12warm: FAILED (%u)\n", failures);
      return 1;
   }
   printf("d3d12warm: ok\n");
   return 0;
}
