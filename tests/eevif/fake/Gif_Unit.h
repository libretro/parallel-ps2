/* The C++ VIF DMA's view of the GIF unit, for tests/eevif: the calls it
 * made on gifUnit routed to the harness's recorders, which the C build's
 * gif_unit_* calls reach too. Found ahead of the real Gif_Unit.h because
 * the old sources are compiled from this directory. */
#pragma once
#include "Gif.h"
extern int  eevif_check_paths(bool p1, bool p2, bool p3);
extern int  eevif_execute(bool isPath3, bool isResume);
extern bool eevif_can_do_path2(void);
extern bool eevif_can_do_path2hl(void);
extern bool eevif_path_is_done(int path);
extern int  eevif_path_state(int path);
struct EevifPath
{
	int idx;
	bool isDone() const { return eevif_path_is_done(idx); }
	struct StateView { int idx; operator int() const { return eevif_path_state(idx); } } state;
};
struct EevifUnit
{
	EevifPath gifPath[3];
	int checkPaths(bool p1, bool p2, bool p3) { return eevif_check_paths(p1, p2, p3); }
	int Execute(bool isPath3, bool isResume) { return eevif_execute(isPath3, isResume); }
	bool CanDoPath2() const { return eevif_can_do_path2(); }
	bool CanDoPath2HL() const { return eevif_can_do_path2hl(); }
};
extern EevifUnit gifUnit;
