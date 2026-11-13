#ifndef GLSTREAM_STUBS_H
#define GLSTREAM_STUBS_H

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The trace of every GL call since stubs_reset */
extern u32 g_trace;
extern u32 g_calls;
/* What the driver has: buffer_storage (core, ARB, EXT), the vendor */
extern int GLAD_GL_VERSION_4_4, GLAD_GL_ARB_buffer_storage, GLAD_GL_EXT_buffer_storage;
extern const char* g_vendor;
/* The next storage/data allocation fails; the next map gives NULL */
extern int g_fail_storage, g_fail_data, g_fail_map;
/* Deletes of fences go into the trace (not while a buffer is destroyed:
 * which ones it deletes is checked by the counts below) */
extern int g_trace_sync_deletes;
/* Fences made and not deleted; waits on no fence; deletes of a fence that
 * was not live; mappings and buffers still held */
extern int g_live_syncs, g_null_waits, g_bad_deletes, g_live_maps, g_live_buffers;
void stubs_reset(void);
void tr(u32 v);

#ifdef __cplusplus
}
#endif

#endif
