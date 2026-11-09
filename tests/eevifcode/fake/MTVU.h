/* tests/eevifcode: the VU1 thread as the C++ VIFcodes called it, routed
 * to the harness's recorders, which the C build's mtvu_write_col/row
 * reach too. */
#pragma once
#include "Config.h"
#include "Vif_Dma.h"
extern void eevifcode_write_col(void);
extern void eevifcode_write_row(void);
struct EevifcodeVuThread
{
	void WriteCol(vifStruct&) { eevifcode_write_col(); }
	void WriteRow(vifStruct&) { eevifcode_write_row(); }
};
extern EevifcodeVuThread vu1Thread;
