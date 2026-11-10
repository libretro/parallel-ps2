/* The settings the EE's hot paths test (every guest memory access, every
 * interpreted branch, every event test), as one word instead of a call or
 * a walk into EmuConfig. emu_config_sync_hot (Pcsx2Config.cpp) rebuilds
 * them from EmuConfig: CPU thread start takes the defaults,
 * VMManager::LoadSettings, the one place EmuConfig's CPU, speedhack and
 * gamefix settings are applied, ends with it, and so does vtlb's fastmem
 * fallback. tests/hotconfig keeps it that way. C89. */
#pragma once

#include "../common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMU_HOT_EEREC       (1u << 0) /* Cpu.Recompiler.EnableEE */
#define EMU_HOT_EECACHE     (1u << 1) /* Cpu.Recompiler.EnableEECache */
#define EMU_HOT_FASTMEM     (1u << 2) /* Cpu.Recompiler.EnableFastmem */
#define EMU_HOT_WAITLOOP    (1u << 3) /* Speedhacks.WaitLoop */
#define EMU_HOT_GOEMON_TLB  (1u << 4) /* Gamefixes.GoemonTlbHack */
#define EMU_HOT_INSTANT_DMA (1u << 5) /* Gamefixes.InstantDMAHack */
#define EMU_HOT_EE_TIMING   (1u << 6) /* Gamefixes.EETimingHack */

extern u32 emu_hot;
/* Speedhacks.EECycleRate and EECycleSkip. */
extern s32 emu_hot_ee_cycle_rate;
extern u32 emu_hot_ee_cycle_skip;

void emu_config_sync_hot(void);
/* vtlb.c, when the fastmem area cannot be had: turns it off and resyncs. */
void emu_config_disable_fastmem(void);

#ifdef __cplusplus
}
#endif
