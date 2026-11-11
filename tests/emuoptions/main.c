/* pcsx2/EmuOptions.c, the real unit built as C89: the core options'
 * defaults, the sanity pass over the clamp modes, every game fix through
 * set, get and its name, and the speed hacks' names, setter, clamp and
 * comparison. The options are one word of bits each, as the C++ config
 * and the hot settings sync read them. The float control register's
 * rounding modes read back as set, x86's and AArch64's encodings alike. */
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

	{
		EmuCpuOptions c, d;
		emu_cpu_defaults(&c);
		d = c;
		check(fpcr_round_mode(c.FPUFPCR) == FPROUND_CHOP && fpcr_round_mode(c.VU0FPCR) == FPROUND_CHOP
			&& fpcr_round_mode(c.FPUDivFPCR) == FPROUND_NEAREST,
			"the units chop, division rounds to nearest");
		check(c.FPUFPCR.bitmask == fpcr_emu_default().bitmask
			&& c.FPUDivFPCR.bitmask == fpcr_set_round_mode(fpcr_emu_default(), FPROUND_NEAREST).bitmask,
			"which is the emulator's float control, division's mode apart");
		check(emu_cpu_equal(&c, &d) && !emu_cpu_recompilers_changed(&c, &d), "a copy is equal");
		d.FPUDivFPCR = fpcr_set_round_mode(d.FPUDivFPCR, FPROUND_CHOP);
		check(!emu_cpu_equal(&c, &d) && !emu_cpu_recompilers_changed(&c, &d),
			"division's mode counts as a change, though no recompiler changes");
		d = c;
		d.Recompiler.EnableVU1 = false;
		check(!emu_cpu_equal(&c, &d) && emu_cpu_recompilers_changed(&c, &d), "a recompiler off does");
		d = c;
		d.Recompiler.EnableFastmem = false;
		check(!emu_cpu_equal(&c, &d) && !emu_cpu_recompilers_changed(&c, &d), "fastmem is not a recompiler");
	}

	{
		EmuDev9Options c, d;
		emu_dev9_defaults(&c);
		check(!c.EthEnable && c.EthApi == EMU_NETAPI_UNSET && !c.EthDevice[0] && c.AutoMask && c.AutoGateway
			&& c.ModeDNS1 == EMU_DNS_AUTO && c.ModeDNS2 == EMU_DNS_AUTO && !c.HddEnable
			&& !strcmp(c.HddFile, "DEV9hdd.raw") && !c.HddIdFile[0]
			&& c.HddSizeSectors == 40u * (1024 * 1024 * 1024 / 512),
			"DEV9 defaults: no network, automatic addressing, a 40 GB DEV9hdd.raw, both off");
		d = c;
		check(emu_dev9_equal(&c, &d), "a copy is equal");
		d.Mask[3] = 255;
		check(!emu_dev9_equal(&c, &d), "the subnet mask counts");
		d = c;
		d.HddIdFile[0] = 'x';
		d.HddIdFile[1] = '\0';
		check(!emu_dev9_equal(&c, &d), "so does the drive's security block");
		/* Past the terminator does not. */
		d = c;
		d.HddFile[sizeof(d.HddFile) - 1] = 'x';
		check(emu_dev9_equal(&c, &d), "what follows a path's terminator does not");
	}

	{
		EmuGSOptions g, h;
		emu_gs_defaults(&g);
		check(g.PCRTCAntiBlur && g.Mipmap && g.Renderer == GS_RENDERER_AUTO && g.UpscaleMultiplier == 1.0f
			&& g.InterlaceMode == GS_INTERLACE_AUTOMATIC && g.TriFilter == GS_TRIFILTER_AUTOMATIC
			&& g.AccurateBlendingUnit == GS_BLEND_BASIC && g.HWMipmapMode == GS_MIPMAP_UNCLAMPED
			&& g.Dithering == 3 && g.GetSkipCountFunctionId == -1 && g.OverrideTextureBarriers == -1
			&& g.SWExtraThreads == 2 && g.SWExtraThreadsHeight == 4
			&& g.FramerateNTSC == EMU_GS_DEFAULT_FRAME_RATE_NTSC && g.FrameratePAL == EMU_GS_DEFAULT_FRAME_RATE_PAL
			&& !g.ManualUserHacks && !g.UserHacks_AlignSpriteX && g.UserHacks_HalfPixelOffset == GS_HPO_OFF,
			"GS defaults: automatic renderer at native resolution, no user hacks");
		check(emu_gs_hardware_renderer(&g), "the automatic renderer counts as hardware");
		h = g;
		check(emu_gs_equal(&g, &h), "a copy is equal");
		h.FrameratePAL = 50.5f;
		check(!emu_gs_equal(&g, &h), "the frame rate counts");
		h = g;
		h.PGSSuperSampleTextures = 1;
		check(!emu_gs_equal(&g, &h), "so does a paraLLEl-GS setting");
		h.Renderer = GS_RENDERER_SW;
		check(!emu_gs_hardware_renderer(&h), "the software renderer is not hardware");

		/* Manual hacks off: every user hack goes, the native scaling the
		 * user set stays. */
		h = g;
		h.UserHacks_AlignSpriteX = true;
		h.UserHacks_TCOffsetX = 5;
		h.SkipDrawEnd = 3;
		h.UserHacks_NativeScaling = GS_NATIVE_SCALING_OFF;
		h.NativeScalingSet = true;
		emu_gs_mask_user_hacks(&h);
		check(!h.UserHacks_AlignSpriteX && h.UserHacks_TCOffsetX == 0 && h.SkipDrawEnd == 0
			&& h.UserHacks_NativeScaling == GS_NATIVE_SCALING_OFF,
			"manual hacks off: the hacks are masked, a native scaling the user set is kept");
		h.ManualUserHacks = true;
		h.UserHacks_AlignSpriteX = true;
		h.SkipDrawEnd = 3;
		emu_gs_mask_user_hacks(&h);
		check(h.UserHacks_AlignSpriteX && h.SkipDrawEnd == 3, "manual hacks on: they stay");
		/* Upscaling hacks need upscaling and manual hacks both. */
		emu_gs_mask_upscaling_hacks(&h);
		check(!h.UserHacks_AlignSpriteX && h.SkipDrawEnd == 3, "at native resolution the upscaling hacks go, the rest stay");
		h.UpscaleMultiplier = 2.0f;
		h.UserHacks_AlignSpriteX = true;
		emu_gs_mask_upscaling_hacks(&h);
		check(h.UserHacks_AlignSpriteX, "upscaled with manual hacks: they stay");
	}

	/* Every rounding mode reads back as set, on either architecture. */
	{
		FPControlRegister r = fpcr_emu_default();
		int m, ok = 1;
		for (m = 0; m < FPROUND_COUNT; m++)
		{
			const FPControlRegister s = fpcr_set_round_mode(r, (enum FPRoundMode)m);
			ok &= fpcr_round_mode(s) == (enum FPRoundMode)m
				&& fpcr_set_round_mode(s, fpcr_round_mode(r)).bitmask == r.bitmask;
		}
		check(ok, "every rounding mode reads back, touching only the rounding bits");
	}

	/* And on the hardware: each mode, made current, rounds a third the
	 * way it says. On AArch64 this is what catches the two infinities
	 * swapped, which reading a mode back cannot. */
	{
		const FPControlRegister saved = fpcr_get_current();
		float pos[FPROUND_COUNT], neg[FPROUND_COUNT];
		int m, back = 1;
		for (m = 0; m < FPROUND_COUNT; m++)
		{
			volatile float one = 1.0f, three = 3.0f;
			fpcr_set_current(fpcr_set_round_mode(fpcr_default(), (enum FPRoundMode)m));
			back &= fpcr_round_mode(fpcr_get_current()) == (enum FPRoundMode)m;
			pos[m] = one / three;
			neg[m] = -one / three;
		}
		fpcr_set_current(saved);
		check(back, "each mode made current reads back from the hardware");
		check(pos[FPROUND_POSITIVE_INFINITY] > pos[FPROUND_NEGATIVE_INFINITY]
			&& neg[FPROUND_POSITIVE_INFINITY] > neg[FPROUND_NEGATIVE_INFINITY],
			"toward +inf rounds a third up, toward -inf down");
		check(pos[FPROUND_CHOP] == pos[FPROUND_NEGATIVE_INFINITY] && neg[FPROUND_CHOP] == neg[FPROUND_POSITIVE_INFINITY],
			"chop rounds toward zero on both sides");
		check(pos[FPROUND_NEAREST] == pos[FPROUND_POSITIVE_INFINITY], "nearest rounds a third up, to 0x3eaaaaab");
	}

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
