/* What the SIO asks the pads, with C linkage for the C units. */
#pragma once

#include "../../common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

u8 PADstartPoll(int port, int slot);
u8 PADpoll(u8 value);
bool PADcomplete(void);

#ifdef __cplusplus
}
#endif
