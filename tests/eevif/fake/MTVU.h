/* tests/eevif: the VU1 thread as the C++ VIF DMA called it. */
#pragma once
#include "Config.h"
extern void eevif_mtvu_changes(void);
extern void eevif_mtvu_kick(void);
struct EevifVuThread
{
	void Get_MTVUChanges() { eevif_mtvu_changes(); }
	void KickPending() { eevif_mtvu_kick(); }
};
extern EevifVuThread vu1Thread;
