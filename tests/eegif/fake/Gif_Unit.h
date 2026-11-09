/* tests/eegif: the GIF unit as the C++ GIF DMA called it, routed to the
 * harness's recorders, which the C build's gif_unit_* calls reach too.
 * Found ahead of the real Gif_Unit.h because the old source is compiled
 * from this directory. */
#pragma once
#include "Gif.h"
extern int  eegif_check_paths(bool p1, bool p2, bool p3);
extern int  eegif_execute(bool isPath3, bool isResume);
extern bool eegif_can_do_path2(void);
extern bool eegif_can_do_path2hl(void);
extern bool eegif_can_do_path3(void);
extern bool eegif_path3_masked(void);
extern bool eegif_signal_queued(void);
extern u32  eegif_transfer(u8* mem, u32 size);
extern void eegif_reset(bool soft);
extern bool eegif_path_is_done(int path);
extern int  eegif_path_state(int path);
extern void eegif_path_set_state(int path, int state);
struct EegifPath
{
	int idx;
	bool isDone() const { return eegif_path_is_done(idx); }
	struct StateView
	{
		int idx;
		operator int() const { return eegif_path_state(idx); }
		StateView& operator=(int v) { eegif_path_set_state(idx, v); return *this; }
	} state;
};
struct EegifSignal
{
	struct Queued { operator bool() const { return eegif_signal_queued(); } } queued;
};
struct EegifUnit
{
	EegifPath gifPath[3];
	EegifSignal gsSIGNAL;
	int checkPaths(bool p1, bool p2, bool p3) { return eegif_check_paths(p1, p2, p3); }
	int Execute(bool isPath3, bool isResume) { return eegif_execute(isPath3, isResume); }
	bool CanDoPath2() const { return eegif_can_do_path2(); }
	bool CanDoPath2HL() const { return eegif_can_do_path2hl(); }
	bool CanDoPath3() const { return eegif_can_do_path3(); }
	bool Path3Masked() const { return eegif_path3_masked(); }
	u32 TransferGSPacketData(GIF_TRANSFER_TYPE t, u8* mem, u32 size, bool aligned = false)
	{
		(void)t; (void)aligned;
		return eegif_transfer(mem, size);
	}
	void Reset(bool soft = false) { eegif_reset(soft); }
};
extern EegifUnit gifUnit;
