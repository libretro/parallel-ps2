/* Pipelines built ahead of their first draw, on a thread of their own,
 * from a list of the ones earlier sessions drew with. C89.
 *
 * The list holds the keys pipelines are built from, records of a fixed
 * size the caller defines; nothing in it depends on the driver, so a
 * driver update that throws away the driver's own pipeline cache leaves
 * it standing. The warm-up thread is handed the list as it was loaded,
 * builds each through the caller's callback and returns what it built
 * through a lock-free queue made room for all of them up front. Past the
 * list it builds what the drawing thread queues for it, a bounded number
 * at a time, and parks when there is nothing. The thread that draws
 * takes from that queue when it misses a pipeline, and draws with one
 * it has meanwhile. */
#ifndef GS_D3D12_WARM_H
#define GS_D3D12_WARM_H

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* ---- the list --------------------------------------------------------- */

typedef struct gs_warm_list
{
   unsigned char *recs;
   size_t         rec_size;
   size_t         count;
   size_t         cap;
} gs_warm_list_t;

void gs_warm_list_init(gs_warm_list_t *l, size_t rec_size);
void gs_warm_list_free(gs_warm_list_t *l);
/* Appends a copy of @rec; false when there is no memory for it. */
bool gs_warm_list_add(gs_warm_list_t *l, const void *rec);
/* Sorted, each record once. */
void gs_warm_list_unique(gs_warm_list_t *l);

/* The file: an 8-byte magic, then the version, the record size, the
 * record count and the CRC-32 of the records, each a 32-bit
 * little-endian word, then the records. A file that is missing, of
 * another version or record size, cut short or longer than its count
 * says, or whose records fail the CRC is refused and the list left
 * empty: false. */
bool gs_warm_list_load(gs_warm_list_t *l, const char *path, uint32_t version);
/* Written to a file beside @path and moved over it, so a reader never
 * sees half of one. */
bool gs_warm_list_save(const gs_warm_list_t *l, const char *path, uint32_t version);

/* ---- the warm-up thread ---------------------------------------------- */

/* Builds @job into @result; nonzero when there is a result to return. */
typedef int (*gs_warm_build_t)(void *ud, const void *job, void *result);

typedef struct gs_warm gs_warm_t;

/* Starts the thread on a copy of @count jobs of @job_size bytes, with
 * room for @queued queued jobs whose results are not taken yet. NULL
 * when there are neither, no memory or no thread - and always in a
 * build without threads: the caller then builds everything as it is
 * drawn. */
gs_warm_t *gs_warm_start(const void *jobs, size_t job_size, size_t count,
      size_t queued, size_t result_size, gs_warm_build_t build, void *ud);
/* The drawing thread: a job for after the list. False, and nothing
 * queued, when the room is taken (results not taken back count) or
 * there is no thread. Never waits. */
bool gs_warm_queue(gs_warm_t *w, const void *job);
/* Until the list's first @n jobs are done. For device creation only. */
void gs_warm_wait(gs_warm_t *w, size_t n);
/* The drawing thread: one finished result, or false when there is none
 * yet. Never waits. */
bool gs_warm_take(gs_warm_t *w, void *result);
/* Asks the thread to stop after the job in hand and joins it. Results it
 * finished stay to be taken. */
void gs_warm_stop(gs_warm_t *w);
/* Stops it if it runs; the caller has taken (and released) every result
 * it wants first. */
void gs_warm_free(gs_warm_t *w);

RETRO_END_DECLS

#endif
