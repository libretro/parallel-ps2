/* The GS entry points the core's C units call, with C linkage. GS.cpp
 * defines them; GS/GS.h includes this for its C++ callers. */
#ifndef PCSX2_GS_ENTRY_H
#define PCSX2_GS_ENTRY_H

#include "../../common/Pcsx2Types.h"
#include "../FreezeTypes.h"

/* EmuConfig.GS.HWDownloadMode, as gs_hw_download_mode returns it. */
#define GS_HW_DOWNLOAD_ENABLED        0
#define GS_HW_DOWNLOAD_NO_READBACKS   1
#define GS_HW_DOWNLOAD_UNSYNCHRONIZED 2
#define GS_HW_DOWNLOAD_DISABLED       3

#ifdef __cplusplus
extern "C" {
#endif
void GSinit(void);
void GSshutdown(void);
void GSreset(bool hardware_reset);
void GSclose(void);
void GSInitAndReadFIFO(u8* mem, u32 size);
void GSReadLocalMemoryUnsync(u8* mem, u32 qwc, u64 BITBLITBUF, u64 TRXPOS, u64 TRXREG);
void GSgifTransfer(const u8* mem, u32 size);
void GSvsync(u32 field, bool registers_written);
int  GSfreeze(FreezeAction mode, freezeData* data);
bool GSIsHardwareRenderer(void);

/* GSopen and GSUpdateConfig on EmuConfig.GS and the frontend's context. */
void gs_open_emu(u8* basemem);
void gs_update_config_emu(void);
int  gs_hw_download_mode(void);
#ifdef __cplusplus
}
#endif

#endif
