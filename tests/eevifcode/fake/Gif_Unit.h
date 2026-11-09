/* tests/eevifcode: the GIF unit as the C++ VIFcodes called it, routed to
 * the harness's recorders, which the C build's gif_unit_* calls reach
 * too. Found ahead of the real Gif_Unit.h because the old sources are
 * compiled from this directory. */
#pragma once
#include "Gif.h"
extern int eevifcode_check_paths(bool p1, bool p2, bool p3);
extern u32 eevifcode_transfer(u8* mem, u32 size, bool hl);
struct EevifcodeUnit
{
	int checkPaths(bool p1, bool p2, bool p3) { return eevifcode_check_paths(p1, p2, p3); }
	u32 TransferGSPacketData(GIF_TRANSFER_TYPE t, u8* mem, u32 size, bool aligned = false)
	{
		(void)aligned;
		return eevifcode_transfer(mem, size, t == GIF_TRANS_DIRECTHL);
	}
};
extern EevifcodeUnit gifUnit;
