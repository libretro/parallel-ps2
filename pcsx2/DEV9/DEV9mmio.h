/* The DEV9 register handlers the EE's vtlb maps over 0x14000000 (DEV9.cpp).
 * C, so Memory.c reaches them; DEV9.h includes this. */
#pragma once

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

u8 DEV9read8(u32 addr);
u16 DEV9read16(u32 addr);
u32 DEV9read32(u32 addr);
void DEV9write8(u32 addr, u8 value);
void DEV9write16(u32 addr, u16 value);
void DEV9write32(u32 addr, u32 value);

struct Pcsx2Config;
s32 DEV9init(void);
void DEV9close(void);
s32 DEV9open(void);
void DEV9shutdown(void);
/* What changed in the DEV9 settings applied, against the old ones. */
void DEV9CheckChanges(const struct Pcsx2Config* old);

#ifdef __cplusplus
}
#endif
