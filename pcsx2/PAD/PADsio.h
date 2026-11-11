/* What the SIO and the VM ask the pads, with C linkage for the C units. */
#pragma once

#include "../../common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

s32 PADinit(void);
void PADshutdown(void);
s32 PADopen(void);
void PADclose(void);
/* The pads' settings again from the options. */
void pad_load_config(void);

u8 PADstartPoll(int port, int slot);
u8 PADpoll(u8 value);
bool PADcomplete(void);

/* The input Input::Update staged becomes what the pads report. Only
 * while the EE is held: see PAD.cpp. */
void pad_input_latch(void);

#ifdef __cplusplus
}
#endif
