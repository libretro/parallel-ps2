/* What the C units ask of the VM manager (VMManager.cpp). C89. */
#pragma once

#include "../common/Pcsx2Defs.h"

/* The stack for threads that run recompiled code: microVU recurses. */
#define VM_EMU_THREAD_STACK_SIZE (2u * 1024 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

/* VMManager::Internal::GameStartingOnCPUThread. */
void vm_game_starting_on_cpu_thread(void);
/* VMManager::Internal::IsExecutionInterrupted. */
bool vm_execution_interrupted(void);
/* The ELF to boot instead of the disc's, empty when none. */
const char* vm_elf_override(void);
/* VMManager::SetState(VMState::Stopping). */
void vm_request_stop(void);
/* VMManager::GetState() == VMState::Paused. */
bool vm_paused(void);

/* libretro/main.cpp: the frame timing changed, so retro_run reports the
 * new AV info. Set from the EE's vsync (Counters.c). */
extern bool pending_update_av_info;

/* The stack for threads that run recompiled code: microVU recurses. */
#define VM_EMU_THREAD_STACK_SIZE (2u * 1024 * 1024)

#ifdef __cplusplus
}
#endif
