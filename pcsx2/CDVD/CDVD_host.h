/* What the drive asks of the emulator around it, from C: defined in the
 * C++ units named. C89. */
#pragma once

#include "../../common/Pcsx2Defs.h"
#include "../VMHost.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The fast-boot option (libretro/main.cpp). */
bool host_option_fast_boot(void);

#ifdef __cplusplus
}
#endif
