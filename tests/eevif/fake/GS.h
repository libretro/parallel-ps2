/* tests/eevif: MTGS as the C++ VIF1 download called it. */
#pragma once
#include "Pcsx2Types.h"
extern void eevif_read_fifo(u8* mem, u32 qwc);
namespace MTGS { static inline void InitAndReadFIFO(u8* mem, u32 qwc) { eevif_read_fifo(mem, qwc); } }
