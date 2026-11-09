/* See GSD3D12Warm.h. C89. */
#include <stdlib.h>
#include <string.h>

#include <encodings/crc32.h>
#include <streams/file_stream.h>

#ifdef HAVE_THREADS
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <rthreads/rthreads.h>
#endif

#include "GSD3D12Warm.h"

#define GS_WARM_MAGIC      "GSPLIST1"
#define GS_WARM_HEADER     (8 + 4 * 4)

/* ---- the list --------------------------------------------------------- */

void gs_warm_list_init(gs_warm_list_t *l, size_t rec_size)
{
   l->recs     = NULL;
   l->rec_size = rec_size;
   l->count    = 0;
   l->cap      = 0;
}

void gs_warm_list_free(gs_warm_list_t *l)
{
   free(l->recs);
   gs_warm_list_init(l, l->rec_size);
}

bool gs_warm_list_add(gs_warm_list_t *l, const void *rec)
{
   if (l->count == l->cap)
   {
      const size_t   cap  = l->cap ? l->cap * 2 : 64;
      unsigned char *recs = (unsigned char*)realloc(l->recs, cap * l->rec_size);
      if (!recs)
         return false;
      l->recs = recs;
      l->cap  = cap;
   }
   memcpy(l->recs + l->count * l->rec_size, rec, l->rec_size);
   l->count++;
   return true;
}

/* qsort has no context argument: the size of the records being sorted
 * is handed over here, by the one thread that sorts. */
static size_t gs_warm_sort_size;

static int gs_warm_cmp(const void *a, const void *b)
{
   return memcmp(a, b, gs_warm_sort_size);
}

void gs_warm_list_unique(gs_warm_list_t *l)
{
   size_t r, w;

   if (l->count < 2)
      return;
   gs_warm_sort_size = l->rec_size;
   qsort(l->recs, l->count, l->rec_size, gs_warm_cmp);
   for (r = 1, w = 1; r < l->count; r++)
   {
      unsigned char *rec = l->recs + r * l->rec_size;
      if (memcmp(rec, l->recs + (w - 1) * l->rec_size, l->rec_size))
      {
         if (w != r)
            memcpy(l->recs + w * l->rec_size, rec, l->rec_size);
         w++;
      }
   }
   l->count = w;
}

static uint32_t gs_warm_rd32(const unsigned char *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void gs_warm_wr32(unsigned char *p, uint32_t v)
{
   p[0] = (unsigned char)v;
   p[1] = (unsigned char)(v >> 8);
   p[2] = (unsigned char)(v >> 16);
   p[3] = (unsigned char)(v >> 24);
}

bool gs_warm_list_load(gs_warm_list_t *l, const char *path, uint32_t version)
{
   void          *buf = NULL;
   int64_t        len = 0;
   const unsigned char *p;
   uint32_t       count;
   bool           ok  = false;

   gs_warm_list_free(l);
   if (!path || !l->rec_size || !filestream_read_file(path, &buf, &len) || !buf)
      return false;
   p = (const unsigned char*)buf;
   if (len < GS_WARM_HEADER || memcmp(p, GS_WARM_MAGIC, 8)
         || gs_warm_rd32(p + 8) != version
         || gs_warm_rd32(p + 12) != (uint32_t)l->rec_size)
      goto end;
   count = gs_warm_rd32(p + 16);
   /* (the count is checked against the length before it is multiplied) */
   if ((uint64_t)(len - GS_WARM_HEADER) % l->rec_size
         || (uint64_t)(len - GS_WARM_HEADER) / l->rec_size != count)
      goto end;
   if (encoding_crc32(0, p + GS_WARM_HEADER, (size_t)count * l->rec_size)
         != gs_warm_rd32(p + 20))
      goto end;
   if (count)
   {
      if (!(l->recs = (unsigned char*)malloc((size_t)count * l->rec_size)))
         goto end;
      memcpy(l->recs, p + GS_WARM_HEADER, (size_t)count * l->rec_size);
      l->count = l->cap = count;
   }
   ok = true;
end:
   free(buf);
   return ok;
}

bool gs_warm_list_save(const gs_warm_list_t *l, const char *path, uint32_t version)
{
   const size_t   body = l->count * l->rec_size;
   unsigned char *buf;
   bool           ok;

   if (!path || (uint64_t)l->count > 0xFFFFFFFFu)
      return false;
   if (!(buf = (unsigned char*)malloc(GS_WARM_HEADER + body)))
      return false;
   memcpy(buf, GS_WARM_MAGIC, 8);
   gs_warm_wr32(buf + 8,  version);
   gs_warm_wr32(buf + 12, (uint32_t)l->rec_size);
   gs_warm_wr32(buf + 16, (uint32_t)l->count);
   gs_warm_wr32(buf + 20, encoding_crc32(0, l->recs, body));
   if (body)
      memcpy(buf + GS_WARM_HEADER, l->recs, body);
   ok = filestream_write_file_atomic(path, buf, (int64_t)(GS_WARM_HEADER + body));
   free(buf);
   return ok;
}

/* ---- the warm-up thread ---------------------------------------------- */

#ifdef HAVE_THREADS
#include <rthreads/retro_eventcount.h>

/* A result in the queue: a header (aligned for any result), then it. */
#define GS_WARM_REC_HEAD 16
#define GS_WARM_BUILT    1u
#define GS_WARM_QUEUED   2u

struct gs_warm
{
   unsigned char     *jobs;
   unsigned char     *scratch;   /* the thread's result in the making */
   unsigned char     *taken;     /* the drawing thread's: a header, or a result it drops */
   unsigned char     *job;       /* the thread's queued job in hand */
   gs_warm_build_t    build;
   void              *ud;
   sthread_t         *thread;
   size_t             job_size;
   size_t             count;
   size_t             queued;    /* room for queued jobs not taken back */
   size_t             in_flight; /* the drawing thread's */
   size_t             result_size;
   retro_spsc_t       results;
   retro_spsc_t       queue;
   retro_eventcount_t wake;      /* a queued job, or stop */
   retro_eventcount_t done_ev;
   retro_atomic_size_t done;     /* of the list */
   retro_atomic_int_t stop;
};

static void gs_warm_put(gs_warm_t *w, unsigned flags)
{
   memset(w->scratch, 0, GS_WARM_REC_HEAD);
   w->scratch[0] = (unsigned char)flags;
   /* (room for every one was made at the start) */
   retro_spsc_write(&w->results, w->scratch, GS_WARM_REC_HEAD + w->result_size);
}

static void gs_warm_thread(void *data)
{
   gs_warm_t *w = (gs_warm_t*)data;
   size_t     i;

   for (i = 0; i < w->count; i++)
   {
      if (retro_atomic_load_acquire_int(&w->stop))
         break;
      if (w->build(w->ud, w->jobs + i * w->job_size, w->scratch + GS_WARM_REC_HEAD))
         gs_warm_put(w, GS_WARM_BUILT);
      retro_atomic_store_release_size(&w->done, i + 1);
      retro_eventcount_notify(&w->done_ev);
   }
   retro_atomic_store_release_size(&w->done, w->count);
   retro_eventcount_notify(&w->done_ev);

   while (w->queued && !retro_atomic_load_acquire_int(&w->stop))
   {
      if (retro_spsc_read_avail(&w->queue) < w->job_size)
      {
         int key = retro_eventcount_prepare_wait(&w->wake);
         if (retro_spsc_read_avail(&w->queue) >= w->job_size
               || retro_atomic_load_acquire_int(&w->stop))
            retro_eventcount_cancel_wait(&w->wake);
         else
            retro_eventcount_commit_wait(&w->wake, key);
         continue;
      }
      retro_spsc_read(&w->queue, w->job, w->job_size);
      /* (one it could not build comes back too: its room is given back) */
      gs_warm_put(w, GS_WARM_QUEUED
            | (w->build(w->ud, w->job, w->scratch + GS_WARM_REC_HEAD) ? GS_WARM_BUILT : 0u));
   }
}

gs_warm_t *gs_warm_start(const void *jobs, size_t job_size, size_t count,
      size_t queued, size_t result_size, gs_warm_build_t build, void *ud)
{
   gs_warm_t *w;
   size_t     rec = GS_WARM_REC_HEAD + result_size;

   if ((!jobs && count) || (!count && !queued) || !job_size || !result_size || !build)
      return NULL;
   if (!(w = (gs_warm_t*)calloc(1, sizeof(*w))))
      return NULL;
   w->jobs    = (unsigned char*)malloc(count ? count * job_size : 1);
   w->scratch = (unsigned char*)malloc(rec);
   w->taken   = (unsigned char*)malloc(rec);
   w->job     = (unsigned char*)malloc(job_size);
   if (!w->jobs || !w->scratch || !w->taken || !w->job)
      goto fail;
   if (!retro_spsc_init(&w->results, (count + queued) * rec))
      goto fail;
   if (!retro_spsc_init(&w->queue, (queued ? queued : 1) * job_size))
      goto fail_results;
   if (!retro_eventcount_init(&w->wake))
      goto fail_queue;
   if (!retro_eventcount_init(&w->done_ev))
      goto fail_wake;
   if (count)
      memcpy(w->jobs, jobs, count * job_size);
   w->job_size    = job_size;
   w->count       = count;
   w->queued      = queued;
   w->result_size = result_size;
   w->build       = build;
   w->ud          = ud;
   retro_atomic_size_init(&w->done, 0);
   retro_atomic_int_init(&w->stop, 0);
   if ((w->thread = sthread_create(gs_warm_thread, w)))
      return w;

   retro_eventcount_free(&w->done_ev);
fail_wake:
   retro_eventcount_free(&w->wake);
fail_queue:
   retro_spsc_free(&w->queue);
fail_results:
   retro_spsc_free(&w->results);
fail:
   free(w->jobs);
   free(w->scratch);
   free(w->taken);
   free(w->job);
   free(w);
   return NULL;
}

bool gs_warm_queue(gs_warm_t *w, const void *job)
{
   if (!w || !w->thread || w->in_flight >= w->queued
         || retro_spsc_write_avail(&w->queue) < w->job_size)
      return false;
   retro_spsc_write(&w->queue, job, w->job_size);
   w->in_flight++;
   retro_eventcount_notify(&w->wake);
   return true;
}

void gs_warm_wait(gs_warm_t *w, size_t n)
{
   if (!w || !w->thread)
      return;
   if (n > w->count)
      n = w->count;
   while (retro_atomic_load_acquire_size(&w->done) < n)
   {
      int key = retro_eventcount_prepare_wait(&w->done_ev);
      if (retro_atomic_load_acquire_size(&w->done) >= n)
         retro_eventcount_cancel_wait(&w->done_ev);
      else
         retro_eventcount_commit_wait(&w->done_ev, key);
   }
}

bool gs_warm_take(gs_warm_t *w, void *result)
{
   size_t rec;
   if (!w)
      return false;
   rec = GS_WARM_REC_HEAD + w->result_size;
   while (retro_spsc_read_avail(&w->results) >= rec)
   {
      /* the header, then the result straight into the caller's */
      retro_spsc_read(&w->results, w->taken, GS_WARM_REC_HEAD);
      if (w->taken[0] & GS_WARM_QUEUED)
         w->in_flight--;
      if (w->taken[0] & GS_WARM_BUILT)
      {
         retro_spsc_read(&w->results, result, w->result_size);
         return true;
      }
      retro_spsc_read(&w->results, w->taken, w->result_size);
   }
   return false;
}

void gs_warm_stop(gs_warm_t *w)
{
   if (!w || !w->thread)
      return;
   retro_atomic_store_release_int(&w->stop, 1);
   retro_eventcount_notify(&w->wake);
   sthread_join(w->thread);
   w->thread = NULL;
}

void gs_warm_free(gs_warm_t *w)
{
   if (!w)
      return;
   gs_warm_stop(w);
   retro_eventcount_free(&w->done_ev);
   retro_eventcount_free(&w->wake);
   retro_spsc_free(&w->queue);
   retro_spsc_free(&w->results);
   free(w->jobs);
   free(w->scratch);
   free(w->taken);
   free(w->job);
   free(w);
}

#else

gs_warm_t *gs_warm_start(const void *jobs, size_t job_size, size_t count,
      size_t queued, size_t result_size, gs_warm_build_t build, void *ud)
{
   (void)jobs; (void)job_size; (void)count; (void)queued; (void)result_size;
   (void)build; (void)ud;
   return NULL;
}

bool gs_warm_queue(gs_warm_t *w, const void *job) { (void)w; (void)job; return false; }
void gs_warm_wait(gs_warm_t *w, size_t n) { (void)w; (void)n; }
bool gs_warm_take(gs_warm_t *w, void *result) { (void)w; (void)result; return false; }
void gs_warm_stop(gs_warm_t *w) { (void)w; }
void gs_warm_free(gs_warm_t *w) { (void)w; }

#endif
