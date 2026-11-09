/* tests/eevifunpack: the VU1 thread as the C++ unpackers reached it --
 * its copy of VIF1's state, which the loop writes when the thread runs
 * VIF1, and the hand-off of an UNPACK's data -- routed to the harness,
 * which the C build's mtvu_vif* calls reach too. */
#pragma once
#include "Config.h"
#include "Vif_Dma.h"
extern void eevifunpack_mtvu_unpack(const u8* data, u32 size);
struct EevifunpackVuThread
{
	alignas(16) vifStruct    vif;
	alignas(16) VIFregisters vifRegs;
	void VifUnpack(vifStruct&, VIFregisters&, const u8* data, u32 size) { eevifunpack_mtvu_unpack(data, size); }
};
extern EevifunpackVuThread vu1Thread;
