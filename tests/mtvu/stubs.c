/* Fault handler registration: the worker registers, nothing faults here. */
#include <faulthandler.h>
bool retro_faulthandler_register_thread(void) { return true; }
void retro_faulthandler_unregister_thread(void) { }

#include "Common.h"
#include "SaveState.h"

PCSX2_ALIGN(__pagealignsize) u8 eeHw[PS2MEM_HARDWARE];

/* Instant VU1 here, so no event is ever scheduled. */
void CPU_INT(EE_EventType n, s32 ecycle) { (void)n; (void)ecycle; }

/* Savestates are not exercised. */
void SaveState_FreezeMem(SaveStateBase *s, void *data, int size) { (void)s; (void)data; (void)size; }
bool SaveState_FreezeTag(SaveStateBase *s, const char *src) { (void)s; (void)src; return false; }
