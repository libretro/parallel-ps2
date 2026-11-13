#ifndef GLPROGRAM_STUBS_H
#define GLPROGRAM_STUBS_H

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The trace of every GL call and log line since stubs_reset, and how many
 * GL calls that was */
extern u32 g_trace;
extern u32 g_calls;
extern int g_echo; /* print what is logged */
void stubs_reset(void);
void tr(u32 v);

#ifdef __cplusplus
}
#endif

#endif
