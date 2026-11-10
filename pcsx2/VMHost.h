/* What the C units ask of the VM manager (VMManager.cpp). C89. */
#pragma once

#include "../common/Pcsx2Defs.h"

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

#ifdef __cplusplus
}
#endif
