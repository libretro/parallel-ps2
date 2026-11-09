/* tests/eevifregs: the VU1 thread as the C++ VIF registers reached it --
 * its copy of VIF1's state, read once it has caught up, and the row and
 * column handed to it -- routed to the harness, which the C build's
 * mtvu_* calls reach too. */
#pragma once
#include "Config.h"
#include "Vif_Dma.h"
#include "VUmicro.h"
extern void eevifregs_write_row(vifStruct& v);
extern void eevifregs_write_col(vifStruct& v);
struct EevifregsVuThread
{
	alignas(16) vifStruct    vif;
	alignas(16) VIFregisters vifRegs;
	void WriteRow(vifStruct& v) { eevifregs_write_row(v); }
	void WriteCol(vifStruct& v) { eevifregs_write_col(v); }
};
extern EevifregsVuThread vu1Thread;
