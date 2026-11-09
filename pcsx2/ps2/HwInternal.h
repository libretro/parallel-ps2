/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "../Hw.h"

#include "../../common/SingleRegisterTypes.h"

/* What the EE hardware register handlers (Hw.c) and the DMAC (Dmac.c)
 * reach in the units that are still C++, with C linkage. */
#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 *  Hardware FIFOs (128 bit access only!), FiFo.cpp
 * --------------------------------------------------------------------------
 * VIF0   -- 0x10004000 -- eeHw[0x4000]
 * VIF1   -- 0x10005000 -- eeHw[0x5000]
 * GIF    -- 0x10006000 -- eeHw[0x6000]
 * IPUout -- 0x10007000 -- eeHw[0x7000]
 * IPUin  -- 0x10007010 -- eeHw[0x7010] */
extern void ReadFIFO_VIF1(mem128_t* out);
extern void ReadFIFO_IPUout(mem128_t* out);

extern void WriteFIFO_VIF0(const mem128_t* value);
extern void WriteFIFO_VIF1(const mem128_t* value);
extern void WriteFIFO_GIF(const mem128_t* value);
extern void WriteFIFO_IPUin(const mem128_t* value);

/* Gif.c: the GIF's registers on page 3, its DMA, and its reset. The
 * read and write return like the other units': the value read, and false
 * when the write was taken care of. */
extern u32  gifRead32(u32 mem);
extern bool gifWrite32(u32 mem, u32 value);
extern void gifInterrupt(void);
extern void dmaGIF(void);
extern void gifHwReset(void);  /* the GIF unit and its paths */
extern void gifFifoReset(void); /* the 16-quadword FIFO */

/* GS.cpp: the GS's part of hwReset. */
extern void gsReset(void);

/* MTGS.cpp: a GS download into EE memory (VIF1 reversed), done once the
 * GS thread has caught up. */
extern void mtgs_init_and_read_fifo(u8* mem, u32 qwc);

/* MTVU.cpp: the VU1 thread's results taken in, and its pending unpack
 * notifications flushed. */
extern void mtvu_get_changes(void);
extern void mtvu_kick_pending(void);

/* Pcsx2Config.cpp: the gamefixes the handlers check. */
extern bool emu_config_ophflag_hack(void);
extern bool emu_config_giffifo_hack(void);
extern bool emu_config_dmabusy_hack(void);
extern bool emu_config_viffifo_hack(void);
extern bool emu_config_vif1stall_hack(void);
extern bool emu_config_vusync_hack(void);
extern bool emu_config_instant_vu1(void);

#ifdef __cplusplus
}
#endif
