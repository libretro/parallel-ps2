/* SPU2 output, hashed.
 *
 * The mixer is deterministic: the same register writes over the same sample
 * RAM produce the same PCM, sample for sample. So a change to it is either
 * bit-exact or it is not, and this says which.
 *
 * The SPU2 is driven here the way a game drives it -- voices pointed at
 * ADPCM blocks in sample RAM, pitch, ADSR envelopes, volumes and their
 * slides, the noise generator, reverb, and the dry/wet gates -- and every
 * stereo sample Mix() emits is folded into a hash. Run it before a change
 * and after; the numbers match or the change altered what a game hears.
 *
 * The mixer has a second output a game can hear: the SPU IRQ, raised when
 * a voice or a mixer write crosses IRQA. That never reaches the PCM, so the
 * three "irq" scenarios arm it and hash which core fired on which sample
 * alongside the audio. A change to an IRQ check site moves that trace and
 * nothing else.
 *
 * The scenarios are separate hashes rather than one, so a mismatch names
 * the area that moved instead of only saying something did.
 *
 * What it catches is measured, by injecting faults into Mixer.cpp and
 * checking the hashes move. Seven of eight do: both saturation bounds, the
 * core 0 and core 1 input volumes, the core 0 master volume, a dry gate
 * reading the wrong channel, the DC filter coefficient and the final output
 * clamp.
 *
 * The eighth does not, and the reason is worth stating so nobody assumes
 * otherwise. Changing the per-voice wet clamp leaves every hash alone, even
 * moved as far as -0x4000. The clamp is reached -- the accumulator runs to
 * around -227000, thousands of samples past the bound -- but what it feeds
 * is the reverb return, and that arrives at about a hundredth of the dry
 * path: RV peaks near 383 against a dry signal at full scale. The
 * difference does not survive into 16 bits. Closing it needs a scenario
 * where the wet path carries the signal on its own, which needs reverb
 * parameters that produce steady output rather than occasional bursts.
 * Until then a change to those two lines wants checking another way.
 *
 * Note also what Init leaves shut: DryGate.ExtL and ExtR are zero on both
 * cores, and core 0 reaches the output only as core 1's Ext input. A
 * scenario that does not open that gate tests one core while appearing to
 * test two, which is why open_ext_path() exists.
 */

#include "Global.h"
#include "spu2.h"
#include "Dma.h"
#include "R3000A.h"
#include "IopCounters.h"
#include "MemoryTypes.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

/* ------------------------------------------------------------------ */
/* What the SPU2 expects the rest of the emulator to provide. None of   */
/* it participates in mixing: the IOP side raises interrupts and moves  */
/* DMA, and the audio side takes finished samples away. Here the        */
/* samples are taken by the harness instead.                            */
/* ------------------------------------------------------------------ */

alignas(__pagealignsize) u8 iopHw[Ps2MemSize::IopHardware];
IopVM_MemoryAllocMess* iopMem;
alignas(16) psxRegisters psxRegs;
psxCounter psxCounters[NUM_COUNTERS];
s32 psxNextDeltaCounter;
u32 psxNextStartCounter;
/* lClocks lives in spu2.cpp, which the harness links for SPU2write. */

extern "C" const size_t spu2_layout_probe[5];

extern "C" void psxDmaInterrupt(int) {}
extern "C" void psxDmaInterrupt2(int) {}
extern "C" void spu2Irq(void) {}
/* No i.LINK board: the output is the SPU2's own. */
extern "C" void FWmixSubboardAudio(s32*, s32*) {}

/* The sink. Mix() is called directly, so these are only reached if the
 * core tries to hand samples off on its own. */
extern "C" s16 *retro_audio_reserve(int) { static s16 buf[4096]; return buf; }
extern "C" void retro_audio_commit(int) {}

/* ------------------------------------------------------------------ */
/* A hash with no library behind it, so the number is reproducible
 * anywhere this builds. FNV-1a over the little-endian sample pairs. */
/* ------------------------------------------------------------------ */

static uint64_t hash_init(void) { return 1469598103934665603ull; }

static void hash_s16(uint64_t *h, s16 v)
{
	uint16_t u = (uint16_t)v;
	*h = (*h ^ (uint64_t)(u & 0xff))      * 1099511628211ull;
	*h = (*h ^ (uint64_t)((u >> 8) & 0xff)) * 1099511628211ull;
}

static void hash_bytes(uint64_t *h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	size_t i;
	for (i = 0; i < n; i++)
		*h = (*h ^ (uint64_t)b[i]) * 1099511628211ull;
}

/* ------------------------------------------------------------------ */
/* Deterministic setup.                                                 */
/* ------------------------------------------------------------------ */

static uint32_t rs = 0x12345678u;
static uint32_t rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
	return rs;
}

/* ADPCM blocks: 16 bytes each, a header byte pair then 14 of nibble
 * pairs. Filled with a mix of shapes so decode, loop and end flags are
 * all exercised rather than one flat tone. */
static void fill_sample_ram(void)
{
	u32 addr;
	rs = 0x12345678u;
	memset(_spu2mem, 0, sizeof(_spu2mem));

	for (addr = 0x1000; addr < 0x80000; addr += 8)
	{
		u8 *blk = (u8 *)(_spu2mem + addr);
		int i;
		/* shift 0..12 in the low nibble, filter 0..4 in the high */
		blk[0] = (u8)(((rnd() % 5) << 4) | (rnd() % 13));
		/* loop start on some blocks, loop-end + repeat on others, so a
		 * voice started anywhere finds a terminator eventually */
		blk[1] = (u8)((addr % 0x400) == 0 ? 0x06 : ((addr % 0x200) == 0 ? 0x01 : 0x00));
		for (i = 2; i < 16; i++)
			blk[i] = (u8)rnd();
	}
}

/* Back to what a fresh boot looks like: zeroed storage, then Init.
 *
 * Two things made scenarios depend on the ones before them. V_Core_Init
 * sets fields one by one, so anything it does not name -- and the
 * emulator only ever runs it over a zeroed BSS -- carried over. And the
 * emulator never writes sample RAM behind the PCM block cache: DMA and
 * register writes invalidate the blocks they touch, so a refilled RAM
 * with a warm cache is a state it can never be in, yet fill_sample_ram
 * put the harness there before every scenario. The cache is cleared
 * here, the way a savestate thaw clears it, after RAM has been filled.
 * lClocks, Spdif and the DMA-side IRQ flags are the rest of what the
 * SPU2 units keep at file scope. */
static void reset_core(void)
{
	memset(Cores, 0, sizeof(Cores));
	memset(spu2regs, 0, sizeof(spu2regs));
	memset(&Spdif, 0, sizeof(Spdif));
	memset(&DCFilterIn, 0, sizeof(DCFilterIn));
	memset(&DCFilterOut, 0, sizeof(DCFilterOut));
	memset(pcm_cache_data, 0, pcm_BlockCount * sizeof(PcmCacheEntry));
	V_Core_Init(&Cores[0], 0);
	V_Core_Init(&Cores[1], 1);
	has_irq_armed = false;
	has_to_call_irq[0] = has_to_call_irq[1] = false;
	has_to_call_irq_dma[0] = has_to_call_irq_dma[1] = false;
	lClocks = 0;
	OutPos = 0;
	InputPos = 0;
	Cycles = 0;
	PlayMode = 0;
}

/* Core 0's output arrives at the final mix as core 1's Ext input, and Init
 * closes that gate on both cores. Left shut, nothing core 0 does -- its
 * voices, its input, its master volume -- reaches the output at all, and a
 * scenario silently tests one core instead of two. */
static void open_ext_path(void)
{
	Cores[1].DryGate.ExtL = -1;
	Cores[1].DryGate.ExtR = -1;
	Cores[1].WetGate.ExtL = -1;
	Cores[1].WetGate.ExtR = -1;
	Cores[1].ExtVol.Left  = 0x7fff;
	Cores[1].ExtVol.Right = 0x7fff;
}

/* Start a voice on a block of sample RAM with a given pitch and envelope. */
static void start_voice(int core, int v, u32 addr, u16 pitch,
                        u16 adsr1, u16 adsr2, s16 vl, s16 vr)
{
	V_Voice& vc = Cores[core].Voices[v];
	vc.StartA        = addr & ~7u;
	vc.NextA         = vc.StartA;
	vc.LoopStartA    = vc.StartA;
	vc.Pitch         = pitch;
	vc.ADSR.regADSR1 = adsr1;
	vc.ADSR.regADSR2 = adsr2;
	vc.Volume.Left.Value   = vl;
	vc.Volume.Right.Value  = vr;
	vc.Volume.Left.Enable  = 0;
	vc.Volume.Right.Enable = 0;

	/* What the KeyOn path in SPU2async does. Mix() does not run it, so
	 * the harness puts the voice in the state a key-on leaves it in. */
	vc.ADSR.Phase   = PHASE_ATTACK;
	vc.ADSR.Counter = 0;
	vc.ADSR.Value   = 0;
	ADSR_UpdateCache(&vc.ADSR);
	vc.SCurrent  = 28;
	vc.LoopMode  = 0;
	vc.SP        = -1;
	vc.LoopFlags = 0;
	vc.NextA     = vc.StartA | 1;
	vc.Prev1 = vc.Prev2 = 0;
	vc.PV1 = vc.PV2 = vc.PV3 = vc.PV4 = 0;
	Cores[core].Regs.ENDX &= ~(1u << v);
}

/* ------------------------------------------------------------------ */
/* Scenarios.                                                           */
/* ------------------------------------------------------------------ */

/* Set by run(), so a scenario can be shown to be mixing audio rather than
 * producing a very reproducible silence. */
static long   g_nonzero;
static int    g_peak;
static double g_rms;
/* The mixer's other output. An IRQ check sits at four places in the sample
 * path -- two voice-read sites, the dummy-advance site and every write
 * through spu2M_WriteFast -- and all any of them does is set
 * has_to_call_irq, which TimeUpdate consumes and which never reaches the
 * PCM. Nothing here looked at it, so a change to any of those four sites
 * passed every scenario. run() therefore folds the flags into a second
 * hash, recording which core fired on which sample, and clears them as
 * TimeUpdate does. */
static uint64_t g_irq_hash;
static long     g_irq_events;

static long   g_hi_rail;   /* samples at +0x7fff */
static long   g_lo_rail;   /* samples at -0x8000 */
static double g_dc;        /* mean output level, both channels */
static int    g_freeze_bad;/* set when a thaw did not restore the mixer */

static uint64_t run(int samples)
{
	uint64_t h = hash_init();
	double acc = 0.0, dc = 0.0;
	int i;

	g_nonzero = 0;
	g_peak = 0;
	g_irq_hash = hash_init();
	g_irq_events = 0;
	g_hi_rail = 0;
	g_lo_rail = 0;

	for (i = 0; i < samples; i++)
	{
		s16 l = 0, r = 0;
		int a;
		int core;
		Mix(&l, &r);
		hash_s16(&h, l);
		hash_s16(&h, r);
		for (core = 0; core < 2; core++)
		{
			if (!has_to_call_irq[core])
				continue;
			has_to_call_irq[core] = false;
			g_irq_events++;
			hash_bytes(&g_irq_hash, &i, sizeof(i));
			hash_bytes(&g_irq_hash, &core, sizeof(core));
		}
		if (l || r) g_nonzero++;
		if (l ==  0x7fff || r ==  0x7fff) g_hi_rail++;
		if (l == -0x8000 || r == -0x8000) g_lo_rail++;
		a = l < 0 ? -(int)l : l; if (a > g_peak) g_peak = a;
		a = r < 0 ? -(int)r : r; if (a > g_peak) g_peak = a;
		acc += (double)l * l + (double)r * r;
		dc  += (double)l + (double)r;
	}
	g_rms = samples ? sqrt(acc / (2.0 * samples)) : 0.0;
	g_dc  = samples ? dc / (2.0 * samples) : 0.0;
	return h;
}

/* Voices at a spread of pitches and volumes, dry only. */
static uint64_t scen_voices(int samples)
{
	int v;
	reset_core();
	for (v = 0; v < 24; v++)
	{
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x90),
		            0x00ff, 0x1fc0, (s16)(0x3000 + v * 0x80), (s16)(0x3fff - v * 0x70));
		start_voice(1, v, 0x20000 + (u32)v * 0x400, (u16)(0x300 + v * 0x77),
		            0x40ff, 0x0fc0, (s16)(0x2000 + v * 0x40), (s16)(0x2fff - v * 0x30));
	}
	Cores[0].DryGate.SndL = Cores[0].DryGate.SndR = -1;
	Cores[1].DryGate.SndL = Cores[1].DryGate.SndR = -1;
	open_ext_path();
	Cores[0].MasterVol.Left.Value = Cores[0].MasterVol.Right.Value = 0x3fff;
	Cores[1].MasterVol.Left.Value = Cores[1].MasterVol.Right.Value = 0x3fff;
	return run(samples);
}

/* The same, with the wet path and reverb enabled -- the reverb buffer is
 * where the resampler and the all-pass chain live. */
static uint64_t scen_reverb(int samples)
{
	int v, c;
	reset_core();
	for (v = 0; v < 24; v++)
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x500 + v * 0x60),
		            0x10ff, 0x1fc0, 0x3000, 0x3000);
	for (c = 0; c < 2; c++)
	{
		Cores[c].DryGate.SndL = Cores[c].DryGate.SndR = -1;
		Cores[c].WetGate.SndL = Cores[c].WetGate.SndR = -1;
		Cores[c].FxEnable = 1;
		Cores[c].EffectsStartA = 0x100000 + (u32)c * 0x40000;
		Cores[c].EffectsEndA   = Cores[c].EffectsStartA + 0x3fff0;
		Cores[c].Revb.IN_COEF_L = 0x6000;
		Cores[c].Revb.IN_COEF_R = 0x5800;
		Cores[c].Revb.APF1_SIZE = 0x03c0;
		Cores[c].Revb.APF2_SIZE = 0x02b0;
		Cores[c].Revb.APF1_VOL  = 0x5000;
		Cores[c].Revb.APF2_VOL  = -0x3000;
		Cores[c].Revb.IIR_VOL   = 0x4000;
		Cores[c].Revb.WALL_VOL  = -0x2000;
		Cores[c].Revb.COMB1_VOL = 0x2000;
		Cores[c].Revb.COMB2_VOL = 0x1800;
		Cores[c].Revb.COMB3_VOL = 0x1000;
		Cores[c].Revb.COMB4_VOL = 0x0800;
		Cores[c].Revb.SAME_L_SRC = 0x0800; Cores[c].Revb.SAME_R_SRC = 0x0a00;
		Cores[c].Revb.DIFF_L_SRC = 0x0c00; Cores[c].Revb.DIFF_R_SRC = 0x0e00;
		Cores[c].Revb.SAME_L_DST = 0x1000; Cores[c].Revb.SAME_R_DST = 0x1200;
		Cores[c].Revb.DIFF_L_DST = 0x1400; Cores[c].Revb.DIFF_R_DST = 0x1600;
		Cores[c].Revb.COMB1_L_SRC = 0x1800; Cores[c].Revb.COMB1_R_SRC = 0x1a00;
		Cores[c].Revb.COMB2_L_SRC = 0x1c00; Cores[c].Revb.COMB2_R_SRC = 0x1e00;
		Cores[c].Revb.COMB3_L_SRC = 0x2000; Cores[c].Revb.COMB3_R_SRC = 0x2200;
		Cores[c].Revb.COMB4_L_SRC = 0x2400; Cores[c].Revb.COMB4_R_SRC = 0x2600;
		Cores[c].Revb.APF1_L_DST = 0x2800; Cores[c].Revb.APF1_R_DST = 0x2a00;
		Cores[c].Revb.APF2_L_DST = 0x2c00; Cores[c].Revb.APF2_R_DST = 0x2e00;
		Cores[c].FxVol.Left = Cores[c].FxVol.Right = 0x3fff;
		Cores[c].MasterVol.Left.Value = Cores[c].MasterVol.Right.Value = 0x3fff;
	}
	open_ext_path();
	return run(samples);
}

/* Volume slides running on both the voices and the master, which is the
 * path that has to keep making progress even on a stopped voice. */
static uint64_t scen_slides(int samples)
{
	int v, c;
	reset_core();
	for (v = 0; v < 24; v++)
	{
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x40),
		            0x00ff, 0x1fc0, 0x2000, 0x2000);
		/* Reg_VOL in slide form: Enable set, with the step, shift and
		 * the decrement/exponential flags varied across the voices so
		 * every branch of the slide update is taken somewhere. */
		Cores[0].Voices[v].Volume.Left.Enable  = 1;
		Cores[0].Voices[v].Volume.Left.Step    = (u16)(v & 3);
		Cores[0].Voices[v].Volume.Left.Shift   = (u16)(8 + (v & 7));
		Cores[0].Voices[v].Volume.Left.Decr    = (u16)((v >> 2) & 1);
		Cores[0].Voices[v].Volume.Left.Exp     = (u16)((v >> 3) & 1);
		Cores[0].Voices[v].Volume.Right.Enable = 1;
		Cores[0].Voices[v].Volume.Right.Step   = (u16)((v + 1) & 3);
		Cores[0].Voices[v].Volume.Right.Shift  = (u16)(9 + (v & 5));
		Cores[0].Voices[v].Volume.Right.Decr   = (u16)((v >> 1) & 1);
		Cores[0].Voices[v].Volume.Right.Exp    = (u16)((v >> 4) & 1);
	}
	/* half the voices stopped, so the stopped-voice slide path runs too */
	for (v = 12; v < 24; v++)
		Cores[0].Voices[v].ADSR.Phase = PHASE_STOPPED;
	for (c = 0; c < 2; c++)
	{
		Cores[c].DryGate.SndL = Cores[c].DryGate.SndR = -1;
		Cores[c].MasterVol.Left.Enable  = 1;
		Cores[c].MasterVol.Left.Step    = 2;
		Cores[c].MasterVol.Left.Shift   = 10;
		Cores[c].MasterVol.Right.Enable = 1;
		Cores[c].MasterVol.Right.Step   = 3;
		Cores[c].MasterVol.Right.Shift  = 11;
		Cores[c].MasterVol.Right.Decr   = 1;
	}
	open_ext_path();
	return run(samples);
}

/* The noise generator and the gates, which are bit operations on the
 * stereo pair rather than arithmetic on it. */
static uint64_t scen_noise_gates(int samples)
{
	int v, c;
	reset_core();
	for (v = 0; v < 24; v++)
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x50),
		            0x00ff, 0x1fc0, 0x3000, 0x2800);
	Cores[0].NoiseClk = 0x1f;
	Cores[0].Regs.VMIXL = 0x00ffffff;
	Cores[0].Regs.VMIXR = 0x00f0f0f0;
	Cores[0].Regs.VMIXEL = 0x000fff00;
	Cores[0].Regs.VMIXER = 0x00ff00ff;
	for (v = 0; v < 24; v++)
		Cores[0].Voices[v].Modulated = (v & 1);
	Cores[0].VoiceGates[0].DryL = -1;
	for (c = 0; c < 2; c++)
	{
		/* asymmetric gates: left and right must not be interchangeable */
		Cores[c].DryGate.SndL = -1;
		Cores[c].DryGate.SndR = 0;
		Cores[c].DryGate.InpL = 0;
		Cores[c].DryGate.InpR = -1;
		Cores[c].DryGate.ExtL = -1;
		Cores[c].DryGate.ExtR = 0;
		Cores[c].MasterVol.Left.Value  = 0x3fff;
		Cores[c].MasterVol.Right.Value = 0x2000;
	}
	return run(samples);
}

/* The input path: ReadInput takes the ADMA areas of sample RAM through
 * InpVol, which Init leaves at zero -- so without setting it the whole
 * input side multiplies by nothing and cannot be tested. */
static uint64_t scen_input(int samples)
{
	int v, c;
	reset_core();
	for (v = 0; v < 8; v++)
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x50),
		            0x00ff, 0x1fc0, 0x2000, 0x2000);
	for (c = 0; c < 2; c++)
	{
		Cores[c].InpVol.Left  = 0x5000;
		Cores[c].InpVol.Right = 0x3800;
		Cores[c].DryGate.InpL = -1;
		Cores[c].DryGate.InpR = -1;
		Cores[c].DryGate.SndL = -1;
		Cores[c].DryGate.SndR = -1;
		Cores[c].WetGate.InpL = -1;
		Cores[c].WetGate.InpR = -1;
		Cores[c].MasterVol.Left.Value = Cores[c].MasterVol.Right.Value = 0x3fff;
	}
	open_ext_path();
	return run(samples);
}

/* Freeze in the middle of a run, scribble over everything the mixer
 * reads, thaw, and carry on. The second half has to hash the same as an
 * uninterrupted run: a savestate that does not restore a field leaves the
 * mixer running on the scribble. The DMA pointers are the interesting
 * part, since they are turned into offsets on the way out and back into
 * pointers on the way in. */
static uint64_t scen_freeze(int samples)
{
	uint64_t plain, restored;
	void *blk = malloc((size_t)SPU2Savestate_SizeIt() + 64);
	struct SPU2Savestate_DataBlock *spud;
	int half = samples / 2, v;

	/* an uninterrupted run, for the second half's hash to match */
	reset_core();
	for (v = 0; v < 8; v++)
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x90),
		            0x00ff, 0x1fc0, 0x3000, 0x3000);
	Cores[0].DryGate.SndL = Cores[0].DryGate.SndR = -1;
	Cores[0].MasterVol.Left.Value = Cores[0].MasterVol.Right.Value = 0x3fff;
	Cores[1].MasterVol.Left.Value = Cores[1].MasterVol.Right.Value = 0x3fff;
	open_ext_path();
	run(half);
	plain = run(samples - half);

	/* the same, with a freeze/thaw across the seam */
	fill_sample_ram();
	reset_core();
	for (v = 0; v < 8; v++)
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x90),
		            0x00ff, 0x1fc0, 0x3000, 0x3000);
	Cores[0].DryGate.SndL = Cores[0].DryGate.SndR = -1;
	Cores[0].MasterVol.Left.Value = Cores[0].MasterVol.Right.Value = 0x3fff;
	Cores[1].MasterVol.Left.Value = Cores[1].MasterVol.Right.Value = 0x3fff;
	open_ext_path();
	run(half);

	spud = (struct SPU2Savestate_DataBlock *)
	       (((uintptr_t)blk + 63) & ~(uintptr_t)63);
	SPU2Savestate_FreezeIt(spud);

	memset(Cores, 0x5a, sizeof(Cores));
	memset(spu2regs, 0x5a, sizeof(spu2regs));
	memset(&Spdif, 0x5a, sizeof(Spdif));
	OutPos = 0x1234; InputPos = 0x4321; Cycles = 0xdeadbeef; PlayMode = 7;

	if (SPU2Savestate_ThawIt(spud) != 0)
		printf("  (thaw refused the block)\n");
	restored = run(samples - half);
	free(blk);

	g_freeze_bad = (restored != plain);
	return restored;
}

/* A steady offset on the input, which the DC blocker at the end of Mix()
 * exists to take back out. The input sits at +0x4000 for three quarters of
 * each period and -0x4000 for the rest, so it carries a large positive
 * offset while still moving enough to be audible. A filter that works
 * leaves the output hovering around zero; one whose state is quantised at
 * the output step feeds its own rounding error back through a pole of
 * 0.995 and parks the output about a hundred steps off instead. Hence the
 * mean-output guard below, which is what this scenario is really for. */
static uint64_t scen_dcblock(int samples)
{
	int c, i;
	reset_core();
	for (i = 0; i < 0x400; i++)
	{
		s16 v = (s16)(((i & 0xff) < 0xc0) ? 0x2000 : -0x2000);
		_spu2mem[0x2000 + i] = v;
		_spu2mem[0x2200 + i] = v;
		_spu2mem[0x2400 + i] = v;
		_spu2mem[0x2600 + i] = v;
	}
	for (c = 0; c < 2; c++)
	{
		/* Kept well clear of the rails: the output clamp is asymmetric
		 * and would put an offset back in that has nothing to do with
		 * the filter. */
		Cores[c].InpVol.Left  = 0x4000;
		Cores[c].InpVol.Right = 0x4000;
		Cores[c].DryGate.InpL = -1;
		Cores[c].DryGate.InpR = -1;
		Cores[c].MasterVol.Left.Value = Cores[c].MasterVol.Right.Value = 0x4000;
	}
	open_ext_path();
	return run(samples);
}

/* Every address in the register window, driven through the dispatch table
 * four times with different values, hashing the whole of both cores and the
 * register file after each pass. This is what holds the table's 0x400
 * entries to their handlers: an entry pointed at the wrong handler, or a
 * handler handed the wrong core or register, shows up here and nowhere
 * else. The sweep is followed by an ordinary mix so the scenario still has
 * to produce audio. */
static uint64_t scen_regwrite(int samples)
{
	static const u16 vals[4] = { 0x0000, 0xffff, 0x5a5a, 0x1234 };
	uint64_t h = hash_init();
	int p, v;

	memset(Cores, 0, sizeof(Cores));
	reset_core();

	for (p = 0; p < 4; p++)
	{
		u32 a;
		for (a = 0; a < 0x800; a += 2)
			SPU2write(0x1f900000u | a, vals[p]);
		hash_bytes(&h, Cores, sizeof(Cores));
		hash_bytes(&h, spu2regs, sizeof(spu2regs));
		hash_bytes(&h, &Spdif, sizeof(Spdif));
	}

	fill_sample_ram();
	reset_core();
	for (v = 0; v < 8; v++)
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x90),
		            0x00ff, 0x1fc0, 0x3000, 0x3000);
	Cores[0].DryGate.SndL = Cores[0].DryGate.SndR = -1;
	Cores[0].MasterVol.Left.Value = Cores[0].MasterVol.Right.Value = 0x3fff;
	Cores[1].MasterVol.Left.Value = Cores[1].MasterVol.Right.Value = 0x3fff;
	open_ext_path();
	return h ^ run(samples);
}

/* Driven hard enough to sit on the saturation boundaries. Without this the
 * clamps are never reached -- the other scenarios peak at about half scale
 * -- and a change to how the result is saturated passes unnoticed, which is
 * the first thing a vectorised clamp would get wrong. */
static uint64_t scen_clipping(int samples)
{
	int v, c;
	reset_core();
	for (v = 0; v < 24; v++)
	{
		/* full volume, sustain held at maximum so every voice keeps
		 * contributing rather than decaying out of the clip region */
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x800 + v * 0x40),
		            0x00ff, 0x00c0, 0x7fff, 0x7fff);
		start_voice(1, v, 0x20000 + (u32)v * 0x400, (u16)(0x800 + v * 0x37),
		            0x00ff, 0x00c0, 0x7fff, 0x7fff);
	}
	for (c = 0; c < 2; c++)
	{
		Cores[c].DryGate.SndL = Cores[c].DryGate.SndR = -1;
		Cores[c].WetGate.SndL = Cores[c].WetGate.SndR = -1;
		Cores[c].MasterVol.Left.Value = Cores[c].MasterVol.Right.Value = 0x7fff;
		Cores[c].Regs.VMIXL = Cores[c].Regs.VMIXR = 0x00ffffff;
		Cores[c].Regs.VMIXEL = Cores[c].Regs.VMIXER = 0x00ffffff;
		/* the wet path loud as well, since its clamps are separate */
		Cores[c].FxEnable      = 1;
		Cores[c].EffectsStartA = 0x100000 + (u32)c * 0x40000;
		Cores[c].EffectsEndA   = Cores[c].EffectsStartA + 0x3fff0;
		Cores[c].FxVol.Left    = Cores[c].FxVol.Right = 0x7fff;
		Cores[c].Revb.IN_COEF_L = 0x7fff;
		Cores[c].Revb.IN_COEF_R = -0x7fff;
		Cores[c].Revb.APF1_SIZE = 0x03c0;
		Cores[c].Revb.APF2_SIZE = 0x02b0;
		Cores[c].Revb.APF1_VOL  = 0x7fff;
		Cores[c].Revb.APF2_VOL  = -0x7fff;
		Cores[c].Revb.IIR_VOL   = 0x7fff;
		Cores[c].Revb.WALL_VOL  = -0x7fff;
		Cores[c].Revb.COMB1_VOL = 0x7fff;
		Cores[c].Revb.COMB2_VOL = -0x7fff;
		Cores[c].Revb.COMB3_VOL = 0x7fff;
		Cores[c].Revb.COMB4_VOL = -0x7fff;
		Cores[c].Revb.SAME_L_SRC = 0x0800; Cores[c].Revb.SAME_R_SRC = 0x0a00;
		Cores[c].Revb.DIFF_L_SRC = 0x0c00; Cores[c].Revb.DIFF_R_SRC = 0x0e00;
		Cores[c].Revb.SAME_L_DST = 0x1000; Cores[c].Revb.SAME_R_DST = 0x1200;
		Cores[c].Revb.DIFF_L_DST = 0x1400; Cores[c].Revb.DIFF_R_DST = 0x1600;
		Cores[c].Revb.COMB1_L_SRC = 0x1800; Cores[c].Revb.COMB1_R_SRC = 0x1a00;
		Cores[c].Revb.COMB2_L_SRC = 0x1c00; Cores[c].Revb.COMB2_R_SRC = 0x1e00;
		Cores[c].Revb.COMB3_L_SRC = 0x2000; Cores[c].Revb.COMB3_R_SRC = 0x2200;
		Cores[c].Revb.COMB4_L_SRC = 0x2400; Cores[c].Revb.COMB4_R_SRC = 0x2600;
		Cores[c].Revb.APF1_L_DST = 0x2800; Cores[c].Revb.APF1_R_DST = 0x2a00;
		Cores[c].Revb.APF2_L_DST = 0x2c00; Cores[c].Revb.APF2_R_DST = 0x2e00;
	}
	open_ext_path();
	return run(samples);
}

/* The same voices as scen_voices, with IRQA armed on one core or both.
 * The addresses are picked to land in both windows the mixer checks: the
 * voices sweep sample RAM from 0x1000 upward, and spu2M_WriteFast walks
 * 0x400 + OutPos, 0x1000 + OutPos and their neighbours once per sample. So
 * the read-side checks and the write-side one both fire, and the trace
 * says which core, on which sample.
 *
 * has_irq_armed is what TimeUpdate computes at the top of a batch; the
 * harness calls Mix() directly, so it sets it the same way. */
enum { ARM_CORE0 = 1, ARM_CORE1 = 2 };

static void arm_irq(unsigned mode, u32 irqa0, u32 irqa1)
{
	Cores[0].IRQEnable = (mode & ARM_CORE0) != 0;
	Cores[1].IRQEnable = (mode & ARM_CORE1) != 0;
	Cores[0].IRQA = irqa0;
	Cores[1].IRQA = irqa1;
	has_irq_armed = mode != 0;
}

static uint64_t scen_irq_with(int samples, unsigned mode)
{
	int v;
	reset_core();
	for (v = 0; v < 24; v++)
	{
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x90),
		            0x00ff, 0x1fc0, (s16)(0x3000 + v * 0x80), (s16)(0x3fff - v * 0x70));
		start_voice(1, v, 0x20000 + (u32)v * 0x400, (u16)(0x300 + v * 0x77),
		            0x40ff, 0x0fc0, (s16)(0x2000 + v * 0x40), (s16)(0x2fff - v * 0x30));
	}
	Cores[0].DryGate.SndL = Cores[0].DryGate.SndR = -1;
	Cores[1].DryGate.SndL = Cores[1].DryGate.SndR = -1;
	open_ext_path();
	Cores[0].MasterVol.Left.Value = Cores[0].MasterVol.Right.Value = 0x3fff;
	Cores[1].MasterVol.Left.Value = Cores[1].MasterVol.Right.Value = 0x3fff;
	arm_irq(mode, 0x1080, 0x20c8);
	return run(samples);
}

static uint64_t scen_irq_core0(int samples) { return scen_irq_with(samples, ARM_CORE0); }
static uint64_t scen_irq_core1(int samples) { return scen_irq_with(samples, ARM_CORE1); }
static uint64_t scen_irq_both(int samples)  { return scen_irq_with(samples, ARM_CORE0 | ARM_CORE1); }

/* ------------------------------------------------------------------ */

struct Scenario
{
	const char *name;
	uint64_t (*fn)(int);
	uint64_t expect;
	/* Expected IRQ trace, and the least number of events per 48000
	 * samples that makes the scenario worth anything. Zero means the
	 * scenario arms nothing and must raise nothing -- if it starts
	 * raising, it stopped being the scenario it was pinned as. */
	uint64_t irq_expect;
	long     irq_min;
};

int main(int argc, char **argv)
{
	/* Samples per scenario. 48000 is a second of audio at the SPU2's
	 * output rate, which is long enough for envelopes to run their
	 * phases and for the reverb buffer to wrap. */
	int samples = argc > 1 ? atoi(argv[1]) : 48000;
	int print   = (argc > 2 && strcmp(argv[2], "--print") == 0);
	int verbose = (argc > 2 && strcmp(argv[2], "--verbose") == 0);
	int isolate = (argc > 2 && strcmp(argv[2], "--isolation") == 0);
	int i, bad = 0;
	long want_irq;

	/* Pinned from the tree as it stands, at the default 48000 samples.
	 * A change to the mixer that is meant to be bit-exact leaves every one
	 * of these alone. Re-pin with --print only when the output is meant to
	 * change, and say in the commit why. Each is the scenario's hash on
	 * its own -- --isolation checks that the order they run in cannot
	 * change any of them. */
	static Scenario scen[] = {
		{ "voices",      scen_voices,      0x38f961780490c5b4ull },
		{ "reverb",      scen_reverb,      0x9339279599345708ull },
		{ "slides",      scen_slides,      0x9a97813bad76deb3ull },
		{ "noise+gates", scen_noise_gates, 0x0703290860af6ecfull },
		{ "clipping",    scen_clipping,    0x425be0726858fb85ull },
		{ "input",       scen_input,       0xe777b67d31162c20ull },
		{ "regwrite",    scen_regwrite,    0x86cda105ef4d970dull },
		{ "freeze",      scen_freeze,      0xa8a0426ca14c9f41ull },
		{ "dcblock",     scen_dcblock,     0xa91c0091521349d7ull },
		{ "irq core0",   scen_irq_core0,   0x38f961780490c5b4ull, 0xf0e4300c7c41c785ull, 64 },
		{ "irq core1",   scen_irq_core1,   0x38f961780490c5b4ull, 0x9452aceb8e669e32ull, 64 },
		{ "irq both",    scen_irq_both,    0x38f961780490c5b4ull, 0x5b85ae91eea842a0ull, 128 },
	};

	/* The DSP is compiled as C and this harness as C++; if the two
	 * disagree about V_Core -- bool's size is the likely way -- every
	 * offset after it is wrong and the hashes would be meaningless. */
	{
		const size_t mine[5] = {
			sizeof(V_Core), sizeof(V_Voice), offsetof(V_Core, Voices),
			offsetof(V_Voice, Volume), offsetof(V_Core, Regs)
		};
		int k;
		for (k = 0; k < 5; k++)
		{
			if (mine[k] != spu2_layout_probe[k])
			{
				printf("  LAYOUT MISMATCH at %d: C++ %zu, C %zu\n",
				       k, mine[k], spu2_layout_probe[k]);
				return 1;
			}
		}
	}

	fill_sample_ram();

	/* A pin is only worth what the scenario behind it is worth, and a
	 * scenario that reads state left by the one before it is a different
	 * scenario in every order it is run in. So each one has to hash the
	 * same alone, and after every other one. Three of the twelve did not,
	 * until reset_core cleared the block cache; one of them then found a
	 * savestate bug that the order it had been running in had hidden.
	 * Quadratic in scenarios, so it is its own lane. */
	if (isolate)
	{
		const int n = (int)(sizeof(scen) / sizeof(scen[0]));
		int j;
		for (i = 0; i < n; i++)
		{
			uint64_t alone, alone_irq, after, after_irq;
			fill_sample_ram();
			alone = scen[i].fn(samples);
			alone_irq = g_irq_hash;
			for (j = 0; j < n; j++)
			{
				if (j == i)
					continue;
				fill_sample_ram();
				scen[j].fn(samples);
				fill_sample_ram();
				after = scen[i].fn(samples);
				after_irq = g_irq_hash;
				if (after != alone || after_irq != alone_irq)
				{
					printf("  %-12s after %-12s %016llx, alone %016llx\n",
					       scen[i].name, scen[j].name,
					       (unsigned long long)after,
					       (unsigned long long)alone);
					bad++;
				}
			}
		}
		printf("%s: SPU2 PCM, %d scenarios, %d order-dependent\n",
		       bad ? "FAIL" : "PASS", n, bad);
		return bad != 0;
	}

	for (i = 0; i < (int)(sizeof(scen) / sizeof(scen[0])); i++)
	{
		uint64_t h;
		fill_sample_ram();
		h = scen[i].fn(samples);

		/* A scenario that emits nothing would hash very consistently and
		 * test nothing, so each one has to be audible before its hash
		 * means anything. */
		if (strcmp(scen[i].name, "clipping") == 0 &&
		    (g_hi_rail < 64 || g_lo_rail < 64))
		{
			printf("  %-12s NOT SATURATING: rails +%ld/-%ld -- the clamps are untested\n",
			       scen[i].name, g_hi_rail, g_lo_rail);
			bad++;
			continue;
		}
		if (strcmp(scen[i].name, "freeze") == 0 && g_freeze_bad)
		{
			printf("  %-12s the mixer does not resume where it froze\n",
			       scen[i].name);
			bad++;
			continue;
		}
		if (strcmp(scen[i].name, "dcblock") == 0 && fabs(g_dc) > 8.0)
		{
			printf("  %-12s DC %+.1f -- the blocker is leaving an offset behind\n",
			       scen[i].name, g_dc);
			bad++;
			continue;
		}
		if (g_nonzero < samples / 4 || g_peak < 256)
		{
			printf("  %-12s SILENT: %ld/%d non-zero, peak %d -- scenario is not mixing\n",
			       scen[i].name, g_nonzero, samples, g_peak);
			bad++;
			continue;
		}
		/* An IRQ scenario whose addresses stopped being crossed hashes
		 * a clean trace of nothing, which would pass forever. */
		want_irq = scen[i].irq_min * (long)samples / 48000;
		if (scen[i].irq_min && want_irq < 1)
			want_irq = 1;
		if (g_irq_events < want_irq)
		{
			printf("  %-12s %ld IRQ events, wanted at least %ld -- "
			       "the armed addresses are not being reached\n",
			       scen[i].name, g_irq_events, want_irq);
			bad++;
			continue;
		}
		if (scen[i].irq_min == 0 && g_irq_events != 0)
		{
			printf("  %-12s raised %ld IRQs with nothing armed\n",
			       scen[i].name, g_irq_events);
			bad++;
			continue;
		}
		if (print)
			printf("  { \"%s\", %016llx, %016llx },\n", scen[i].name,
			       (unsigned long long)h,
			       (unsigned long long)g_irq_hash);
		else if (scen[i].expect == 0)
			printf("  %-12s %016llx  (unpinned)%s%016llx\n", scen[i].name,
			       (unsigned long long)h,
			       scen[i].irq_min ? ", IRQ " : "",
			       (unsigned long long)(scen[i].irq_min ? g_irq_hash : 0));
		else if (h != scen[i].expect)
		{
			printf("  %-12s %016llx  EXPECTED %016llx\n", scen[i].name,
			       (unsigned long long)h, (unsigned long long)scen[i].expect);
			bad++;
		}
		else if (scen[i].irq_min && scen[i].irq_expect
		      && g_irq_hash != scen[i].irq_expect)
		{
			printf("  %-12s %016llx  ok, but IRQ trace %016llx EXPECTED %016llx\n",
			       scen[i].name, (unsigned long long)h,
			       (unsigned long long)g_irq_hash,
			       (unsigned long long)scen[i].irq_expect);
			bad++;
		}
		else if (scen[i].irq_min)
			printf("  %-12s %016llx  ok, %ld IRQs %016llx\n", scen[i].name,
			       (unsigned long long)h, g_irq_events,
			       (unsigned long long)g_irq_hash);
		else
			printf("  %-12s %016llx  ok\n", scen[i].name,
			       (unsigned long long)h);

		if (!print && verbose)
			printf("               %ld/%d non-zero, peak %d, rms %.0f, rails +%ld/-%ld, dc %+.1f\n",
			       g_nonzero, samples, g_peak, g_rms, g_hi_rail, g_lo_rail, g_dc);
	}

	if (!print)
		printf("%s: SPU2 PCM, %d scenarios, %d mismatches\n",
		       bad ? "FAIL" : "PASS",
		       (int)(sizeof(scen) / sizeof(scen[0])), bad);
	return bad != 0;
}
