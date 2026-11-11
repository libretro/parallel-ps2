/* pcsx2/EmuOptions.c, the real unit built as C89: the core options'
 * defaults, the sanity pass over the clamp modes, every game fix through
 * set, get and its name, and the speed hacks' names, setter, clamp and
 * comparison. The options are one word of bits each, as the C++ config
 * and the hot settings sync read them. */
#include <stdio.h>
#include <string.h>

#include "EmuOptions.h"

static int failures;
static void check(int ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

int main(void)
{
	EmuRecompilerOptions rec;
	EmuGamefixOptions fix;
	EmuSpeedhackOptions a, b;
	enum SpeedHack hack;
	u32 i;
	int all = 1;

	check(sizeof(rec) == 4 && sizeof(fix) == 4, "the recompiler and game fix options are one word each");

	emu_recompiler_defaults(&rec);
	check(rec.EnableEE && rec.EnableIOP && rec.EnableVU0 && rec.EnableVU1 && rec.EnableFastmem
		&& !rec.EnableEECache && rec.EnableFpuAccurateArith && !rec.EnableFpuSoftFloat,
		"recompiler defaults: every rec and fastmem on, the EE cache off");
	check(rec.vu0Overflow && !rec.vu0ExtraOverflow && rec.vu1Overflow && rec.fpuOverflow
		&& !rec.fpuExtraOverflow && !rec.fpuFullMode, "clamping defaults to standard overflow");

	/* Extra without normal, and full without extra, cannot be. */
	rec.fpuOverflow = false;
	rec.fpuExtraOverflow = true;
	rec.vu1Overflow = true;
	rec.vu1ExtraOverflow = true;
	rec.vu1SignOverflow = true;
	rec.vu0Overflow = false;
	rec.vu0ExtraOverflow = false;
	rec.vu0SignOverflow = true;
	emu_recompiler_sanitize(&rec);
	check(rec.fpuOverflow && !rec.fpuExtraOverflow, "FPU extra without normal goes back to the default");
	check(rec.vu0Overflow && !rec.vu0SignOverflow, "VU0 sign without extra goes back to the default");
	check(rec.vu1Overflow && rec.vu1ExtraOverflow && rec.vu1SignOverflow, "a full VU1 chain is kept");

	emu_gamefix_clear_all(&fix);
	for (i = 0; i < GamefixId_COUNT; i++)
	{
		const u32 before = fix.bitset;
		emu_gamefix_set(&fix, (enum GamefixId)i, true);
		all &= emu_gamefix_get(&fix, (enum GamefixId)i) && fix.bitset != before
			&& emu_gamefix_name(i) && emu_gamefix_name(i)[0];
	}
	check(all, "every game fix sets its own bit and has a name");
	check(emu_gamefix_name(GamefixId_COUNT) == NULL, "no name past the last game fix");
	check(fix.XgKickHack && fix.VuAddSubHack && !strcmp(emu_gamefix_name(Fix_XGKick), "XGKick"),
		"the fixes land on the bits the core reads");
	emu_gamefix_set(&fix, Fix_XGKick, false);
	check(!emu_gamefix_get(&fix, Fix_XGKick) && emu_gamefix_get(&fix, Fix_VuAddSub), "clearing one leaves the rest");

	emu_speedhack_defaults(&a);
	check(a.WaitLoop && a.IntcStat && a.vuFlagHack && a.vu1Instant && !a.vuThread && !a.fastCDVD
		&& a.EECycleRate == 0 && a.EECycleSkip == 0, "speed hack defaults: the recommended ones on");
	b = a;
	check(emu_speedhack_equal(&a, &b), "a copy is equal");
	emu_speedhack_set(&b, SPEEDHACK_EE_CYCLE_RATE, 99);
	check(b.EECycleRate == EMU_EE_CYCLE_RATE_MAX && !emu_speedhack_equal(&a, &b), "the cycle rate clamps, and counts");
	emu_speedhack_set(&b, SPEEDHACK_EE_CYCLE_RATE, -99);
	check(b.EECycleRate == EMU_EE_CYCLE_RATE_MIN, "at both ends");
	emu_speedhack_set(&b, SPEEDHACK_MTVU, 1);
	check(b.vuThread, "mtvu sets the VU thread");
	emu_speedhack_clear_all(&b);
	check(b.bitset == 0 && b.EECycleRate == 0, "clear all");

	all = 1;
	for (i = 0; i < SPEEDHACK_COUNT; i++)
	{
		const char* name = emu_speedhack_name((enum SpeedHack)i);
		all &= emu_speedhack_parse_name(name, strlen(name), &hack) && hack == (enum SpeedHack)i;
	}
	check(all, "every speed hack's name parses back to it");
	check(!emu_speedhack_parse_name("mtvux", 5, &hack) && !emu_speedhack_parse_name("mtv", 3, &hack)
		&& emu_speedhack_parse_name("mtvux", 4, &hack) && hack == SPEEDHACK_MTVU,
		"a name is its whole run, no more and no less");

	printf(failures ? "emuoptions: %d failure(s)\n" : "emuoptions: ok\n", failures);
	return failures != 0;
}
