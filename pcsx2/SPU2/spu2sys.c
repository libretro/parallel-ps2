/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* ====================================================================================== */
/*  spu2sys.cpp -- Emulation module for the SPU2 'virtual machine' */
/* ====================================================================================== */
/* This module contains (most!) stuff which is directly related to SPU2 emulation. */
/* Contents should be cross-platform compatible whenever possible. */

#include "../IopCounters.h"
#include "../IopDma.h"
#include "../IopHw.h"
#include "../R3000A.h"

#include "Dma.h"
#include "Global.h"
#include "spu2.h"

#include <stddef.h>
#include <string.h>

#include <libretro.h>

extern int16_t *retro_audio_reserve(int32_t max_samples);
extern void     retro_audio_commit(int32_t samples);

s16 spu2regs[0x010000 / sizeof(s16)];
s16 _spu2mem[0x200000 / sizeof(s16)];

V_Core Cores[2];
V_SPDIF Spdif;

StereoOut32 DCFilterIn;
StereoOut64 DCFilterOut;
u16 OutPos;
u16 InputPos;
u32 Cycles;

int PlayMode;

bool has_to_call_irq[2]     = { false, false };
bool has_to_call_irq_dma[2] = { false, false };
bool has_irq_armed          = false;

static bool psxmode = false;

/* writes a signed value to the SPU2 ram */
/* Invalidates the ADPCM cache in the process. */
static __fi void spu2M_Write(u32 addr, s16 value)
{
	/* Make sure the cache is invalidated: */
	/* (note to self : addr address WORDs, not bytes) */

	addr &= 0xfffff;
	if (addr >= SPU2_DYN_MEMLINE)
	{
		const int cacheIdx = addr / pcm_WordsPerBlock;
		pcm_cache_data[cacheIdx].Generation = 0;
	}
	*GetMemPtr(addr) = value;
}

static __fi void V_Core_DmaWrite(V_Core *c, u16 value)
{
	spu2M_Write(c->ActiveTSA, value);
	++c->ActiveTSA;
	c->ActiveTSA &= 0xfffff;
	c->TSA = c->ActiveTSA;
}

void V_Core_Init(V_Core *c, int index)
{
	uint v;
	int ci;


	/* Explicitly initializing variables instead. */
	c->Mute = false;
	c->DMABits = 0;
	c->NoiseClk = 0;
	c->NoiseCnt = 0;
	c->NoiseOut = 0;
	c->AutoDMACtrl = 0;
	c->InputDataLeft = 0;
	c->InputPosWrite = 0x100;
	c->InputDataProgress = 0;
	c->InputDataTransferred = 0;
	c->LastEffect.Left = 0;
	c->LastEffect.Right = 0;
	c->CoreEnabled = 0;
	c->AttrBit0 = 0;
	c->DmaMode = 0;
	c->DMAPtr = NULL;
	c->KeyOn = 0;
	OutPos = 0;
	DCFilterIn.Left   = 0;
	DCFilterIn.Right  = 0;
	DCFilterOut.Left  = 0;
	DCFilterOut.Right = 0;

	psxmode = false;
	c->psxSoundDataTransferControl = 0;
	c->psxSPUSTAT = 0;

	ci = c->Index = index;

	c->Regs.STATX   = 0;
	c->Regs.ATTR    = 0;
	c->ExtVol.Left  = 0x7FFF;
	c->ExtVol.Right = 0x7FFF;
	c->InpVol.Left  = 0x7FFF;
	c->InpVol.Right = 0x7FFF;
	c->FxVol.Left   = 0;
	c->FxVol.Right  = 0;
	c->MasterVol.Left.Reg_VOL = 0;
	c->MasterVol.Left.Counter = 0;
	c->MasterVol.Left.Value   = 0;
	c->MasterVol.Right.Reg_VOL = 0;
	c->MasterVol.Right.Counter = 0;
	c->MasterVol.Right.Value   = 0;

	memset(&c->DryGate, -1, sizeof(c->DryGate));
	memset(&c->WetGate, -1, sizeof(c->WetGate));
	c->DryGate.ExtL = 0;
	c->DryGate.ExtR = 0;
	if (!ci)
	{
		c->WetGate.ExtL = 0;
		c->WetGate.ExtR = 0;
	}

	c->Regs.MMIX = ci ? 0xFFC : 0xFF0; /* PS2 confirmed (f3c and f30 after BIOS ran, ffc and ff0 after sdinit) */
	c->Regs.VMIXL = 0xFFFFFF;
	c->Regs.VMIXR = 0xFFFFFF;
	c->Regs.VMIXEL = 0xFFFFFF;
	c->Regs.VMIXER = 0xFFFFFF;
	c->EffectsStartA = c ? 0xFFFF8 : 0xEFFF8;
	c->EffectsEndA = c ? 0xFFFFF : 0xEFFFF;

	c->FxEnable = false; /* Uninitialized it's 0 for both cores. Resetting libs however may set this to 0 or 1. */
	/* These are real PS2 values, mainly constant apart from a few bits: 0x3220EAA4, 0x40505E9C. */
	/* These values mean nothing.  They do not reflect the actual address the SPU2 is testing, */
	/* it would seem that reading the IRQA register returns the last written value, not the */
	/* value of the internal register.  Rewriting the registers with their current values changes */
	/* whether interrupts fire (they do while uninitialised, but do not when rewritten). */
	/* The exact boot value is unknown and probably unknowable, but it seems to be somewhere */
	/* in the input or output areas, so we're using 0x800. */
	/* F1 2005 is known to rely on an uninitialised IRQA being an address which will be hit. */
	c->IRQA = 0x800;
	c->IRQEnable = false; /* PS2 confirmed */

	for (v = 0; v < SPU2_NUM_VOICES; ++v)
	{
		c->VoiceGates[v].DryL = -1;
		c->VoiceGates[v].DryR = -1;
		c->VoiceGates[v].WetL = -1;
		c->VoiceGates[v].WetR = -1;

		c->Voices[v].Volume.Left.Reg_VOL = 0;
		c->Voices[v].Volume.Left.Counter = 0;
		c->Voices[v].Volume.Left.Value   = 0;
		c->Voices[v].Volume.Right.Reg_VOL = 0;
		c->Voices[v].Volume.Right.Counter = 0;
		c->Voices[v].Volume.Right.Value   = 0;
		c->Voices[v].SCurrent = 28;

		c->Voices[v].ADSR.Counter = 0;
		c->Voices[v].ADSR.Value = 0;
		c->Voices[v].ADSR.Phase = 0;
		c->Voices[v].Pitch = 0x3FFF;
		c->Voices[v].NextA = 0x2801;
		c->Voices[v].StartA = 0x2800;
		c->Voices[v].LoopStartA = 0x2800;
	}

	c->DMAICounter = 0;
	c->AdmaInProgress = false;

	c->Regs.STATX = 0x80;
	c->Regs.ENDX = 0xffffff; /* PS2 confirmed */

	c->RevbSampleBufPos = 0;
	memset(c->RevbDownBuf, 0, sizeof(c->RevbDownBuf));
	memset(c->RevbUpBuf, 0, sizeof(c->RevbUpBuf));
}

#define TICKINTERVAL 768
#define SANITYINTERVAL 4800
/* TICKINTERVAL * SANITYINTERVAL = 3686400 */
#define SAMPLECOUNT 3686400 

/* The C and C++ halves share V_Core, so they have to agree on its shape --
 * bool in particular, which C89 does not have. The harness compares these
 * against what the C++ side measures. */
const size_t spu2_layout_probe[5] =
{
	sizeof(V_Core),
	sizeof(V_Voice),
	offsetof(V_Core, Voices),
	offsetof(V_Voice, Volume),
	offsetof(V_Core, Regs)
};

void TimeUpdate(u64 cClocks)
{
	int i;
	int c;
	u8 vc;
	int v;
	int max_pairs;
	int16_t *snd_buffer;
	int snd_count;
	u32 dClocks = cClocks - lClocks;

	/* Sanity Checks: */
	/*  It's not totally uncommon for the IOP's clock to jump backwards a cycle or two, and in */
	/*  such cases we just want to ignore the TimeUpdate call. */

	if (dClocks > (u32)-15)
		return;

	/*  But if for some reason our clock value seems way off base (typically due to bad dma */
	/*  timings from PCSX2), just mix out a little bit, skip the rest, and hope the ship */
	/*  "rights" itself later on. */

	if (dClocks > SAMPLECOUNT)
	{
		dClocks = SAMPLECOUNT;
		lClocks = cClocks - dClocks;
	}

	/* Mix() emits exactly one stereo sample per TICKINTERVAL of IOP
	 * clocks, so worst-case stereo samples for this call is
	 * (dClocks / TICKINTERVAL) which is bounded by SANITYINTERVAL
	 * (the dClocks cap above is SAMPLECOUNT == TICKINTERVAL *
	 * SANITYINTERVAL). Reserve room up front and write Mix's output
	 * straight into the libretro audio buffer - no per-call stack
	 * scratch (TimeUpdate is always_inline and called from many
	 * sites; a 19 KB on-stack array would balloon every caller's
	 * frame), and no intermediate memcpy. */
	max_pairs  = dClocks / TICKINTERVAL;
	snd_buffer = retro_audio_reserve(max_pairs * 2);
	snd_count  = 0;

	/* Stable for the whole batch: register writes (which set IRQEnable)
	 * are flushed through TimeUpdate before they apply, so no write lands
	 * mid-loop. Lets the sample hot path skip IRQA matching when neither
	 * core is armed (the common case). */
	has_irq_armed = Cores[0].IRQEnable || Cores[1].IRQEnable;

	/*Update Mixing Progress */
	while (dClocks >= TICKINTERVAL)
	{
		for (i = 0; i < 2; i++)
		{
			if (has_to_call_irq[i])
			{
				has_to_call_irq[i] = false;
				if (!(Spdif.Info & (4 << i)) && Cores[i].IRQEnable)
				{
					Spdif.Info |= (4 << i);
					spu2Irq();
				}
			}
		}

		dClocks -= TICKINTERVAL;
		lClocks += TICKINTERVAL;
		Cycles++;

		for (c = 0; c < 2; c++)
		{
			if (Cores[c].KeyOff)
			{
				for (vc = 0; vc < SPU2_NUM_VOICES; vc++)
				{
					if (((Cores[c].KeyOff >> vc) & 1))
						ADSR_Release(&Cores[c].Voices[vc].ADSR);
				}
				Cores[c].KeyOff = 0;
			}

			if (Cores[c].KeyOn)
			{
				Cores[c].Regs.ENDX &= ~(Cores[c].KeyOn);
				for (v = 0; v < SPU2_NUM_VOICES; v++)
				{
					if (Cores[c].KeyOn & (1 << v))
					{
						V_Voice *vc = &Cores[c].Voices[v];
						if (vc->StartA & 7)
							vc->StartA = (vc->StartA + 0xFFFF8) + 0x8;

						vc->ADSR.Phase   = PHASE_ATTACK;
						vc->ADSR.Counter = 0;
						vc->ADSR.Value   = 0;
						ADSR_UpdateCache(&vc->ADSR);

						vc->SCurrent     = 28;
						vc->LoopMode     = 0;

						/* When SP >= 0 the next sample will be grabbed, 
						 * we don't want this to happen instantly because 
						 * in the case of pitch being 0 we want to delay getting
						 * the next block header. This is a hack to work around the 
						 * fact that unlike the HW we don't update the block header 
						 * on every cycle. */
						vc->SP           = -1;

						vc->LoopFlags    = 0;
						vc->NextA        = vc->StartA | 1;
						vc->Prev1        = 0;
						vc->Prev2        = 0;

						vc->PV1          = 0;
						vc->PV2          = 0;
						vc->PV3          = 0;
						vc->PV4          = 0;
						Cores[c].KeyOn &= ~(1 << v);
					}
				}
				Cores[c].KeyOn = 0;
			}
		}
		Mix(&snd_buffer[snd_count], &snd_buffer[snd_count + 1]);
		snd_count += 2;
	}


	if (snd_count)
		retro_audio_commit(snd_count);

	/*Update DMA4 interrupt delay counter */
	if (Cores[0].DMAICounter > 0 && (psxRegs.cycle - Cores[0].LastClock) > 0)
	{
		const u32 amt = pcsx2_min_u(psxRegs.cycle - Cores[0].LastClock, (u32)Cores[0].DMAICounter);
		Cores[0].DMAICounter -= amt;
		Cores[0].LastClock = psxRegs.cycle;
		if(!Cores[0].AdmaInProgress)
			HW_DMA4_MADR += amt / 2;

		if (Cores[0].DMAICounter <= 0)
		{
			for (i = 0; i < 2; i++)
			{
				if (has_to_call_irq_dma[i])
				{
					has_to_call_irq_dma[i] = false;
					if (!(Spdif.Info & (4 << i)) && Cores[i].IRQEnable)
					{
						Spdif.Info |= (4 << i);
						spu2Irq();
					}
				}
			}

			if (((Cores[0].AutoDMACtrl & 1) != 1) && Cores[0].ReadSize)
			{
				if (Cores[0].IsDMARead)
					V_Core_FinishDMAread(&Cores[0]);
				else
					V_Core_FinishDMAwrite(&Cores[0]);
			}

			if (Cores[0].DMAICounter <= 0)
			{
				HW_DMA4_MADR = HW_DMA4_TADR;
				if (Cores[0].DmaMode)
					Cores[0].Regs.STATX |= 0x80;
				Cores[0].Regs.STATX &= ~0x400;
				Cores[0].TSA = Cores[0].ActiveTSA;
				if (HW_DMA4_CHCR & 0x01000000)
				{
					HW_DMA4_CHCR &= ~0x01000000;
					psxDmaInterrupt(4);
				}
			}
		}
		else
		{
			if (((psxCounters[6].startCycle + psxCounters[6].deltaCycles) - psxRegs.cycle) > (u32)Cores[0].DMAICounter)
			{
				psxCounters[6].startCycle  = psxRegs.cycle;
				psxCounters[6].deltaCycles = Cores[0].DMAICounter;

				psxNextDeltaCounter -= (psxRegs.cycle - psxNextStartCounter);
				psxNextStartCounter = psxRegs.cycle;
				if (psxCounters[6].deltaCycles < psxNextDeltaCounter)
					psxNextDeltaCounter = psxCounters[6].deltaCycles;
			}
		}
	}

	/*Update DMA7 interrupt delay counter */
	if (Cores[1].DMAICounter > 0 && (psxRegs.cycle - Cores[1].LastClock) > 0)
	{
		const u32 amt = pcsx2_min_u(psxRegs.cycle - Cores[1].LastClock, (u32)Cores[1].DMAICounter);
		Cores[1].DMAICounter -= amt;
		Cores[1].LastClock = psxRegs.cycle;
		if (!Cores[1].AdmaInProgress)
			HW_DMA7_MADR += amt / 2;

		if (Cores[1].DMAICounter <= 0)
		{
			for (i = 0; i < 2; i++)
			{
				if (has_to_call_irq_dma[i])
				{
					has_to_call_irq_dma[i] = false;
					if (!(Spdif.Info & (4 << i)) && Cores[i].IRQEnable)
					{
						Spdif.Info |= (4 << i);
						spu2Irq();
					}
				}
			}

			if (((Cores[1].AutoDMACtrl & 2) != 2) && Cores[1].ReadSize)
			{
				if (Cores[1].IsDMARead)
					V_Core_FinishDMAread(&Cores[1]);
				else
					V_Core_FinishDMAwrite(&Cores[1]);
			}

			if (Cores[1].DMAICounter <= 0)
			{
				HW_DMA7_MADR = HW_DMA7_TADR;
				if (Cores[1].DmaMode)
					Cores[1].Regs.STATX |= 0x80;
				Cores[1].Regs.STATX &= ~0x400;
				Cores[1].TSA = Cores[1].ActiveTSA;
				if (HW_DMA7_CHCR & 0x01000000)
				{
					HW_DMA7_CHCR &= ~0x01000000;
					psxDmaInterrupt2(0);
				}
			}
		}
		else
		{
			if (((psxCounters[6].startCycle + psxCounters[6].deltaCycles) - psxRegs.cycle) > (u32)Cores[1].DMAICounter)
			{
				psxCounters[6].startCycle  = psxRegs.cycle;
				psxCounters[6].deltaCycles = Cores[1].DMAICounter;

				psxNextDeltaCounter -= (psxRegs.cycle - psxNextStartCounter);
				psxNextStartCounter = psxRegs.cycle;
				if (psxCounters[6].deltaCycles < psxNextDeltaCounter)
					psxNextDeltaCounter = psxCounters[6].deltaCycles;
			}
		}
	}
}

static __fi void UpdateSpdifMode(void)
{
	if (Spdif.Out & 0x4) /* use 24/32bit PCM data streaming */
	{
		PlayMode = 8;
		return;
	}

	if (Spdif.Out & SPDIF_OUT_BYPASS)
	{
		PlayMode = 2;
		if (!(Spdif.Mode & SPDIF_MODE_BYPASS_BITSTREAM))
			PlayMode = 4; /*bitstream bypass */
	}
	else
	{
		PlayMode = 0; /*normal processing */
		if (Spdif.Out & SPDIF_OUT_PCM)
			PlayMode = 1;
	}
}

#define map_spu1to2(addr) ((addr) * 4 + ((addr) >= 0x200 ? 0xc0000 : 0))
#define map_spu2to1(addr) (((addr) - ((addr) >= 0xc0000 ? 0xc0000 : 0)) / 4)

void V_Core_WriteRegPS1(V_Core *c, u32 mem, u16 value)
{
	const u32 reg = mem & 0xffff;

	if ((reg >= 0x1c00) && (reg < 0x1d80))
	{
		/*voice values */
		u8 voice = ((reg - 0x1c00) >> 4);
		const u8 vval = reg & 0xf;
		switch (vval)
		{
			case 0x0: /*VOLL (Volume L) */
				c->Voices[voice].Volume.Left.Reg_VOL = value;
				if (!c->Voices[voice].Volume.Left.Enable)
					c->Voices[voice].Volume.Left.Value = (s16)(value << 1);
				break;
			case 0x2: /*VOLR (Volume R) */
				c->Voices[voice].Volume.Right.Reg_VOL = value;
				if (!c->Voices[voice].Volume.Right.Enable)
					c->Voices[voice].Volume.Right.Value = (s16)(value << 1);
				break;
			case 0x4:
				c->Voices[voice].Pitch = value;
				break;
			case 0x6:
				c->Voices[voice].StartA = map_spu1to2(value);
				break;

			case 0x8: /* ADSR1 (Envelope) */
				c->Voices[voice].ADSR.regADSR1 = value;
				ADSR_UpdateCache(&c->Voices[voice].ADSR);
				break;

			case 0xa: /* ADSR2 (Envelope) */
				c->Voices[voice].ADSR.regADSR2 = value;
				ADSR_UpdateCache(&c->Voices[voice].ADSR);
				break;
			case 0xc: /* Voice 0..23 ADSR Current Volume */
				/* not commonly set by games */
				c->Voices[voice].ADSR.Value = value;
				break;
			case 0xe:
				c->Voices[voice].LoopStartA = map_spu1to2(value);
				break;
			default:
				break;
		}
	}

	else
		switch (reg)
		{
			case 0x1d80: /*         Mainvolume left */
				c->MasterVol.Left.Reg_VOL = value;
				if (!c->MasterVol.Left.Enable)
					c->MasterVol.Left.Value = (s16)(value << 1);
				break;

			case 0x1d82: /*         Mainvolume right */
				c->MasterVol.Right.Reg_VOL = value;
				if (!c->MasterVol.Right.Enable)
					c->MasterVol.Right.Value = (s16)(value << 1);
				break;

			case 0x1d84: /*         Reverberation depth left */
				c->FxVol.Left = (s16)value;
				break;

			case 0x1d86: /*         Reverberation depth right */
				c->FxVol.Right = (s16)value;
				break;

			case 0x1d88: /*         Voice ON  (0-15) */
				tbl_reg_writes[((REG_S_KON) & 0x7ff) / 2](tbl_reg_args[((REG_S_KON) & 0x7ff) / 2], value);
				break;
			case 0x1d8a: /*         Voice ON  (16-23) */
				tbl_reg_writes[((REG_S_KON + 2) & 0x7ff) / 2](tbl_reg_args[((REG_S_KON + 2) & 0x7ff) / 2], value);
				break;

			case 0x1d8c: /*         Voice OFF (0-15) */
				tbl_reg_writes[((REG_S_KOFF) & 0x7ff) / 2](tbl_reg_args[((REG_S_KOFF) & 0x7ff) / 2], value);
				break;
			case 0x1d8e: /*         Voice OFF (16-23) */
				tbl_reg_writes[((REG_S_KOFF + 2) & 0x7ff) / 2](tbl_reg_args[((REG_S_KOFF + 2) & 0x7ff) / 2], value);
				break;

			case 0x1d90: /*         Channel FM (pitch lfo) mode (0-15) */
				tbl_reg_writes[((REG_S_PMON) & 0x7ff) / 2](tbl_reg_args[((REG_S_PMON) & 0x7ff) / 2], value);
				break;

			case 0x1d92: /*         Channel FM (pitch lfo) mode (16-23) */
				tbl_reg_writes[((REG_S_PMON + 2) & 0x7ff) / 2](tbl_reg_args[((REG_S_PMON + 2) & 0x7ff) / 2], value);
				break;


			case 0x1d94: /*         Channel Noise mode (0-15) */
				tbl_reg_writes[((REG_S_NON) & 0x7ff) / 2](tbl_reg_args[((REG_S_NON) & 0x7ff) / 2], value);
				break;

			case 0x1d96: /*         Channel Noise mode (16-23) */
				tbl_reg_writes[((REG_S_NON + 2) & 0x7ff) / 2](tbl_reg_args[((REG_S_NON + 2) & 0x7ff) / 2], value);
				break;

			case 0x1d98: /*         1F801D98h - Voice 0..23 Reverb mode aka Echo On (EON) (R/W) */
				tbl_reg_writes[((REG_S_VMIXEL) & 0x7ff) / 2](tbl_reg_args[((REG_S_VMIXEL) & 0x7ff) / 2], value);
				tbl_reg_writes[((REG_S_VMIXER) & 0x7ff) / 2](tbl_reg_args[((REG_S_VMIXER) & 0x7ff) / 2], value);
				break;

			case 0x1d9a: /*         1F801D98h + 2 - Voice 0..23 Reverb mode aka Echo On (EON) (R/W) */
				tbl_reg_writes[((REG_S_VMIXEL + 2) & 0x7ff) / 2](tbl_reg_args[((REG_S_VMIXEL + 2) & 0x7ff) / 2], value);
				tbl_reg_writes[((REG_S_VMIXER + 2) & 0x7ff) / 2](tbl_reg_args[((REG_S_VMIXER + 2) & 0x7ff) / 2], value);
				break;

			case 0x1d9c: /* Voice 0..15 ON/OFF (status) (ENDX) (R); writeable but hw overrides it shortly after */
			case 0x1d9e: /* Voice 15..23 ON/OFF (status) (ENDX) (R); writeable but hw overrides it shortly after */
				break;

			case 0x1da2: /*         Reverb work area start */
				c->EffectsStartA = map_spu1to2(value);
				break;

			case 0x1da4:
				c->IRQA = map_spu1to2(value);
				break;

			case 0x1da6:
				c->TSA = map_spu1to2(value);
				break;

			case 0x1da8: /* Spu Write to Memory */
				Cores[0].ActiveTSA = Cores[0].TSA;
				if (Cores[0].IRQEnable && (Cores[0].IRQA <= Cores[0].ActiveTSA))
				{
					has_to_call_irq[0] = true;
					spu2Irq();
				}
				V_Core_DmaWrite(c, value);
				break;

			case 0x1daa:
				tbl_reg_writes[((REG_C_ATTR) & 0x7ff) / 2](tbl_reg_args[((REG_C_ATTR) & 0x7ff) / 2], value);
				break;

			case 0x1dac: /* 1F801DACh - Sound RAM Data Transfer Control (should be 0004h) */
				c->psxSoundDataTransferControl = value;
				break;

			case 0x1dae: /* 1F801DAEh - SPU Status Register (SPUSTAT) (R) */
						 /* The SPUSTAT register should be treated read-only (writing is possible in so far that the written */
						 /* value can be read-back for a short moment, however, thereafter the hardware is overwriting that value). */
						 /*Regs.STATX = value; */
			case 0x1DB0: /* 1F801DB0h 4  CD Volume Left/Right */
			case 0x1DB2:
			case 0x1DB4: /* 1F801DB4h 4  Extern Volume Left / Right */
			case 0x1DB6:
			case 0x1DB8: /* 1F801DB8h 4  Current Main Volume Left/Right */
			case 0x1DBA:
			case 0x1DBC: /* 1F801DBCh 4  Unknown? (R/W) */
			case 0x1DBE:
				break;

			case 0x1DC0:
				c->Revb.APF1_SIZE = value * 4;
				break;
			case 0x1DC2:
				c->Revb.APF2_SIZE = value * 4;
				break;
			case 0x1DC4:
				c->Revb.IIR_VOL = value;
				break;
			case 0x1DC6:
				c->Revb.COMB1_VOL = value;
				break;
			case 0x1DC8:
				c->Revb.COMB2_VOL = value;
				break;
			case 0x1DCA:
				c->Revb.COMB3_VOL = value;
				break;
			case 0x1DCC:
				c->Revb.COMB4_VOL = value;
				break;
			case 0x1DCE:
				c->Revb.WALL_VOL = value;
				break;
			case 0x1DD0:
				c->Revb.APF1_VOL = value;
				break;
			case 0x1DD2:
				c->Revb.APF2_VOL = value;
				break;
			case 0x1DD4:
				c->Revb.SAME_L_DST = value * 4;
				break;
			case 0x1DD6:
				c->Revb.SAME_R_DST = value * 4;
				break;
			case 0x1DD8:
				c->Revb.COMB1_L_SRC = value * 4;
				break;
			case 0x1DDA:
				c->Revb.COMB1_R_SRC = value * 4;
				break;
			case 0x1DDC:
				c->Revb.COMB2_L_SRC = value * 4;
				break;
			case 0x1DDE:
				c->Revb.COMB2_R_SRC = value * 4;
				break;
			case 0x1DE0:
				c->Revb.SAME_L_SRC = value * 4;
				break;
			case 0x1DE2:
				c->Revb.SAME_R_SRC = value * 4;
				break;
			case 0x1DE4:
				c->Revb.DIFF_L_DST = value * 4;
				break;
			case 0x1DE6:
				c->Revb.DIFF_R_DST = value * 4;
				break;
			case 0x1DE8:
				c->Revb.COMB3_L_SRC = value * 4;
				break;
			case 0x1DEA:
				c->Revb.COMB3_R_SRC = value * 4;
				break;
			case 0x1DEC:
				c->Revb.COMB4_L_SRC = value * 4;
				break;
			case 0x1DEE:
				c->Revb.COMB4_R_SRC = value * 4;
				break;
			case 0x1DF0:
				c->Revb.DIFF_L_SRC = value * 4;
				break; /* DIFF_R_SRC and DIFF_L_SRC supposedly swapped on SPU2 */
			case 0x1DF2:
				c->Revb.DIFF_R_SRC = value * 4;
				break; /* but I don't believe it! (games in psxmode sound better unswapped) */
			case 0x1DF4:
				c->Revb.APF1_L_DST = value * 4;
				break;
			case 0x1DF6:
				c->Revb.APF1_R_DST = value * 4;
				break;
			case 0x1DF8:
				c->Revb.APF2_L_DST = value * 4;
				break;
			case 0x1DFA:
				c->Revb.APF2_R_DST = value * 4;
				break;
			case 0x1DFC:
				c->Revb.IN_COEF_L = value;
				break;
			case 0x1DFE:
				c->Revb.IN_COEF_R = value;
				break;
		}

	spu2Ru16(mem) = value;
}

u16 V_Core_ReadRegPS1(V_Core *c, u32 mem)
{
	u16 value = spu2Ru16(mem);

	const u32 reg = mem & 0xffff;

	if ((reg >= 0x1c00) && (reg < 0x1d80))
	{
		/*voice values */
		const u8 voice = ((reg - 0x1c00) >> 4);
		const u8 vval = reg & 0xf;
		switch (vval)
		{
			case 0x0: /*VOLL (Volume L) */
				return c->Voices[voice].Volume.Left.Reg_VOL;
			case 0x2: /*VOLR (Volume R) */
				return c->Voices[voice].Volume.Right.Reg_VOL;
			case 0x4:
				return c->Voices[voice].Pitch;
			case 0x6:
				return map_spu2to1(c->Voices[voice].StartA);
			case 0x8:
				return c->Voices[voice].ADSR.regADSR1;
			case 0xa:
				return c->Voices[voice].ADSR.regADSR2;
			case 0xc: /* Voice 0..23 ADSR Current Volume */
				return c->Voices[voice].ADSR.Value;
			case 0xe:
				return map_spu2to1(c->Voices[voice].LoopStartA);
			default:
				break;
		}
	}
	else
		switch (reg)
		{
			case 0x1d80:
				return c->MasterVol.Left.Value;
			case 0x1d82:
				return c->MasterVol.Right.Value;
			case 0x1d84:
				return c->FxVol.Left;
			case 0x1d86:
				return c->FxVol.Right;
			case 0x1d88:
			case 0x1d8a:
			case 0x1d8c:
			case 0x1d8e:
				return 0;
			case 0x1d90:
				return (c->Regs.PMON & 0xFFFF);
			case 0x1d92:
				return (c->Regs.PMON >> 16);
			case 0x1d94:
				return (c->Regs.NON & 0xFFFF);
			case 0x1d96:
				return (c->Regs.NON >> 16);
			case 0x1d98:
				return (c->Regs.VMIXEL & 0xFFFF);
			case 0x1d9a:
				return (c->Regs.VMIXEL >> 16);
			case 0x1d9c:
				return c->Regs.ENDX & 0xFFFF;
			case 0x1d9e:
				return c->Regs.ENDX >> 16;
			case 0x1da2:
				return map_spu2to1(c->EffectsStartA);
			case 0x1da4:
				return map_spu2to1(c->IRQA);
			case 0x1da6:
				return map_spu2to1(c->TSA);
			case 0x1da8:
				c->ActiveTSA = c->TSA;
				return V_Core_DmaRead(c);
			case 0x1daa:
				return Cores[0].Regs.ATTR;
			case 0x1dac: /* 1F801DACh - Sound RAM Data Transfer Control (should be 0004h) */
				return c->psxSoundDataTransferControl;
			case 0x1dae:
				return Cores[0].Regs.STATX;
		}

	return value;
}

static void RegWrite_VoiceParams(u32 arg, u16 value)
{
	const int core  = arg >> 12;
	const int voice = (arg >> 4) & 31;
	const int param = (arg >> 1) & 7;

	V_Voice *thisvoice = &Cores[core].Voices[voice];

	switch (param)
	{
		case 0: /*VOLL (Volume L) */
			thisvoice->Volume.Left.Reg_VOL = value;
			if (!thisvoice->Volume.Left.Enable)
				thisvoice->Volume.Left.Value = (s16)(value << 1);
			break;
		
		case 1: /*VOLR (Volume R) */
			thisvoice->Volume.Right.Reg_VOL = value;
			if (!thisvoice->Volume.Right.Enable)
				thisvoice->Volume.Right.Value = (s16)(value << 1);
			break;

		case 2:
			thisvoice->Pitch = value;
			break;

		case 3: /* ADSR1 (Envelope) */
			thisvoice->ADSR.regADSR1 = value;
			ADSR_UpdateCache(&thisvoice->ADSR);
			break;

		case 4: /* ADSR2 (Envelope) */
			thisvoice->ADSR.regADSR2 = value;
			ADSR_UpdateCache(&thisvoice->ADSR);
			break;

			/* REG_VP_ENVX, REG_VP_VOLXL and REG_VP_VOLXR are all writable, only ENVX has any effect when written to. */
			/* Colin McRae Rally 2005 triggers case 5 (ADSR), but it doesn't produce issues enabled or disabled. */

		case 5:
			thisvoice->ADSR.Value = value;
			break;
		case 6:
		case 7:
		default:
			break;
	}
}

static void RegWrite_VoiceAddr(u32 arg, u16 value)
{
	const int core    = arg >> 12;
	const int voice   = (arg >> 4) & 31;
	const int address = (arg >> 1) & 7;

	V_Voice *thisvoice = &Cores[core].Voices[voice];

	switch (address)
	{
		case 0: /* SSA (Waveform Start Addr) (hiword, 4 bits only) */
			thisvoice->StartA = ((u32)(value & 0x0F) << 16) | (thisvoice->StartA & 0xFFF8);
			break;

		case 1: /* SSA (loword) */
			thisvoice->StartA = (thisvoice->StartA & 0x0F0000) | (value & 0xFFF8);
			break;

		case 2:
			thisvoice->LoopMode = 1;
			thisvoice->LoopStartA = ((u32)(value & 0x0F) << 16) | (thisvoice->LoopStartA & 0xFFF8);
			break;

		case 3:
			thisvoice->LoopMode = 1;
			thisvoice->LoopStartA = (thisvoice->LoopStartA & 0x0F0000) | (value & 0xFFF8);
			break;



		case 4:
			/* NAX is confirmed to be writable on hardware (decoder will start decoding at new location).
			 *
			 * Example games:
			 * FlatOut
			 * Soul Reaver 2
			 * Wallace And Gromit: Curse Of The Were-Rabbit. */
			thisvoice->NextA = ((u32)(value & 0x0F) << 16) | (thisvoice->NextA & 0xFFF8) | 1;
			thisvoice->SCurrent = 28;
			break;

		case 5:
			thisvoice->NextA = (thisvoice->NextA & 0x0F0000) | (value & 0xFFF8) | 1;
			thisvoice->SCurrent = 28;
			break;
	}
}

static void RegWrite_Core(u32 arg, u16 value)
{
	int i;
	uint vc;
	uint v;
	u32 vx;
	const int omem = arg & 0xfff;
	const int core = arg >> 12;
	V_Core *thiscore = &Cores[core];

	switch (omem)
	{
		case REG__1AC:
			/* ---------------------------------------------------------------------------- */
			/* 0x1ac / 0x5ac : direct-write to DMA address : special register (undocumented) */
			/* ---------------------------------------------------------------------------- */
			/* On the GS, DMAs are actually pushed through a hardware register.  Chances are the */
			/* SPU works the same way, and "technically" *all* DMA data actually passes through */
			/* the HW registers at 0x1ac (core0) and 0x5ac (core1).  We handle normal DMAs in */
			/* optimized block copy fashion elsewhere, but some games will write this register */
			/* directly, so handle those here: */

			/* Performance Note: The PS2 Bios uses this extensively right before booting games, */
			/* causing massive slowdown if we don't shortcut it here. */
			thiscore->ActiveTSA = thiscore->TSA;
			for (i = 0; i < 2; i++)
			{
				if (Cores[i].IRQEnable && (Cores[i].IRQA == thiscore->ActiveTSA))
					{ has_to_call_irq[i] = true; }
			}
			V_Core_DmaWrite(thiscore, value);
			break;

		case REG_C_ATTR:
		{
			bool irqe = thiscore->IRQEnable;
			u8 oldDmaMode = thiscore->DmaMode;

			thiscore->AttrBit0 = (value >> 0) & 0x01;  /*1 bit */
			thiscore->DMABits = (value >> 1) & 0x07;   /*3 bits */
			thiscore->DmaMode = (value >> 4) & 0x03;   /*2 bit (not necessary, we get the direction from the iop) */
			thiscore->IRQEnable = (value >> 6) & 0x01; /*1 bit */
			thiscore->FxEnable = (value >> 7) & 0x01;  /*1 bit */
			thiscore->NoiseClk = (value >> 8) & 0x3f;  /*6 bits */
			thiscore->Mute = 0;
			/* no clue */
			thiscore->Regs.ATTR = value & 0xffff;

			if (!thiscore->DmaMode && !(thiscore->Regs.STATX & 0x400))
				thiscore->Regs.STATX &= ~0x80;
			else if(!oldDmaMode && thiscore->DmaMode)
				thiscore->Regs.STATX |= 0x80;

			thiscore->ActiveTSA = thiscore->TSA;

			if (thiscore->IRQEnable != irqe)
			{
				if (!thiscore->IRQEnable)
					Spdif.Info &= ~(4 << thiscore->Index);
			}
		}
		break;

		case REG_S_PMON:
			for (vc = 1; vc < 16; ++vc)
				thiscore->Voices[vc].Modulated = (value >> vc) & 1;
			thiscore->Regs.PMON = (thiscore->Regs.PMON & 0xffff0000u) | value;
			break;

		case (REG_S_PMON + 2):
			for (vc = 0; vc < 8; ++vc)
				thiscore->Voices[vc + 16].Modulated = (value >> vc) & 1;
			thiscore->Regs.PMON = (thiscore->Regs.PMON & 0x0000ffffu) | ((u32)value << 16);
			break;

		case REG_S_NON:
			for (vc = 0; vc < 16; ++vc)
				thiscore->Voices[vc].Noise = (value >> vc) & 1;
			thiscore->Regs.NON = (thiscore->Regs.NON & 0xffff0000u) | value;
			break;

		case (REG_S_NON + 2):
			for (vc = 0; vc < 8; ++vc)
				thiscore->Voices[vc + 16].Noise = (value >> vc) & 1;
			thiscore->Regs.NON = (thiscore->Regs.NON & 0x0000ffffu) | ((u32)value << 16);
			break;

		case REG_S_VMIXL:
			{
				const u32 result = thiscore->Regs.VMIXL;
				thiscore->Regs.VMIXL = (thiscore->Regs.VMIXL & 0xffff0000u) | value;
				if (result == thiscore->Regs.VMIXL)
					break;
				for (vc = 0, vx = 1; vc < 16; ++vc, vx <<= 1)
					thiscore->VoiceGates[vc].DryL = (value & vx) ? -1 : 0;
			}
			break;

		case (REG_S_VMIXL + 2):
			{
				const u32 result = thiscore->Regs.VMIXL;
				thiscore->Regs.VMIXL = (thiscore->Regs.VMIXL & 0x0000ffffu) | ((u32)value << 16);
				if (result == thiscore->Regs.VMIXL)
					break;
				for (vc = 16, vx = 1; vc < 24; ++vc, vx <<= 1)
					thiscore->VoiceGates[vc].DryL = (value & vx) ? -1 : 0;
			}
			break;

		case REG_S_VMIXEL:
			{
				const u32 result = thiscore->Regs.VMIXEL;
				thiscore->Regs.VMIXEL = (thiscore->Regs.VMIXEL & 0xffff0000u) | value;
				if (result == thiscore->Regs.VMIXEL)
					break;
				for (vc = 0, vx = 1; vc < 16; ++vc, vx <<= 1)
					thiscore->VoiceGates[vc].WetL = (value & vx) ? -1 : 0;
			}
			break;

		case (REG_S_VMIXEL + 2):
			{
				const u32 result = thiscore->Regs.VMIXEL;
				thiscore->Regs.VMIXEL = (thiscore->Regs.VMIXEL & 0x0000ffffu) | ((u32)value << 16);
				if (result == thiscore->Regs.VMIXEL)
					break;
				for (vc = 16, vx = 1; vc < 24; ++vc, vx <<= 1)
					thiscore->VoiceGates[vc].WetL = (value & vx) ? -1 : 0;
			}
			break;

		case REG_S_VMIXR:
			{
				const u32 result = thiscore->Regs.VMIXR;
				thiscore->Regs.VMIXR = (thiscore->Regs.VMIXR & 0xffff0000u) | value;
				if (result == thiscore->Regs.VMIXR)
					break;
				for (vc = 0, vx = 1; vc < 16; ++vc, vx <<= 1)
					thiscore->VoiceGates[vc].DryR = (value & vx) ? -1 : 0;
			}
			break;

		case (REG_S_VMIXR + 2):
			{
				const u32 result = thiscore->Regs.VMIXR;
				thiscore->Regs.VMIXR = (thiscore->Regs.VMIXR & 0x0000ffffu) | ((u32)value << 16);
				if (result == thiscore->Regs.VMIXR)
					break;
				for (vc = 16, vx = 1; vc < 24; ++vc, vx <<= 1)
					thiscore->VoiceGates[vc].DryR = (value & vx) ? -1 : 0;
			}
			break;

		case REG_S_VMIXER:
			{
				const u32 result = thiscore->Regs.VMIXER;
				thiscore->Regs.VMIXER = (thiscore->Regs.VMIXER & 0xffff0000u) | value;
				if (result == thiscore->Regs.VMIXER)
					break;
				for (vc = 0, vx = 1; vc < 16; ++vc, vx <<= 1)
					thiscore->VoiceGates[vc].WetR = (value & vx) ? -1 : 0;
			}
			break;

		case (REG_S_VMIXER + 2):
			{
				const u32 result = thiscore->Regs.VMIXER;
				thiscore->Regs.VMIXER = (thiscore->Regs.VMIXER & 0x0000ffffu) | ((u32)value << 16);
				if (result == thiscore->Regs.VMIXER)
					break;
				for (vc = 16, vx = 1; vc < 24; ++vc, vx <<= 1)
					thiscore->VoiceGates[vc].WetR = (value & vx) ? -1 : 0;
			}
			break;

		case REG_P_MMIX:
		{
			/* Each MMIX gate is assigned either 0 or 0xffffffff depending on the status */
			/* of the MMIX bits.  I use -1 below as a shorthand for 0xffffffff. :) */

			const int vx = value & ((core == 0) ? 0xFF0 : 0xFFF);
			thiscore->WetGate.ExtR = (vx & 0x001) ? -1 : 0;
			thiscore->WetGate.ExtL = (vx & 0x002) ? -1 : 0;
			thiscore->DryGate.ExtR = (vx & 0x004) ? -1 : 0;
			thiscore->DryGate.ExtL = (vx & 0x008) ? -1 : 0;
			thiscore->WetGate.InpR = (vx & 0x010) ? -1 : 0;
			thiscore->WetGate.InpL = (vx & 0x020) ? -1 : 0;
			thiscore->DryGate.InpR = (vx & 0x040) ? -1 : 0;
			thiscore->DryGate.InpL = (vx & 0x080) ? -1 : 0;
			thiscore->WetGate.SndR = (vx & 0x100) ? -1 : 0;
			thiscore->WetGate.SndL = (vx & 0x200) ? -1 : 0;
			thiscore->DryGate.SndR = (vx & 0x400) ? -1 : 0;
			thiscore->DryGate.SndL = (vx & 0x800) ? -1 : 0;
			thiscore->Regs.MMIX = value;
		}
		break;

		case REG_S_ENDX:
			thiscore->Regs.ENDX &= 0xff0000;
			break;

		case (REG_S_ENDX + 2):
			thiscore->Regs.ENDX &= 0xffff;
			break;

		case REG_S_ADMAS:
			/* hack for ps1driver which writes -1 (and never turns the adma off after psxlogo). */
			/* adma isn't available in psx mode either */
			if (value == 32767)
			{
				psxmode = true;
				Cores[1].FxEnable = 0;
				Cores[1].EffectsStartA = 0x7FFF8; /* park core1 effect area in inaccessible mem */
				Cores[1].EffectsEndA = 0x7FFFF;
				for (v = 0; v < 24; ++v)
				{
					Cores[1].Voices[v].Volume.Left.Reg_VOL  = 0;
					Cores[1].Voices[v].Volume.Left.Counter  = 0;
					Cores[1].Voices[v].Volume.Left.Value    = 0;
					Cores[1].Voices[v].Volume.Right.Reg_VOL = 0;
					Cores[1].Voices[v].Volume.Right.Counter = 0;
					Cores[1].Voices[v].Volume.Right.Value   = 0;
					Cores[1].Voices[v].SCurrent = 28;

					Cores[1].Voices[v].ADSR.Value = 0;
					Cores[1].Voices[v].ADSR.Phase = 0;
					Cores[1].Voices[v].Pitch = 0x0;
					Cores[1].Voices[v].NextA = 0x6FFFF;
					Cores[1].Voices[v].StartA = 0x6FFFF;
					Cores[1].Voices[v].LoopStartA = 0x6FFFF;
					Cores[1].Voices[v].Modulated = 0;
				}
				return;
			}
			thiscore->AutoDMACtrl = value;
			if (!(value & 0x3) && thiscore->AdmaInProgress)
			{
				/* Kill the current transfer so it doesn't continue */
				thiscore->AdmaInProgress = 0;
				thiscore->InputDataLeft = 0;
				thiscore->DMAICounter = 0;
				thiscore->InputDataTransferred = 0;

				/* Not accurate behaviour but shouldn't hurt for now, need to run some tests */
				/* to see why Prince of Persia Warrior Within buzzes when going in to the map */
				/* since it starts an ADMA of music, then kills ADMA and input DMA */
				/* without disabling ADMA read mode or clearing the buffer. */
				for (i = 0; i < 0x200; i++)
				{
					GetMemPtr(0x2000 + (thiscore->Index << 10))[i] = 0;
					GetMemPtr(0x2200 + (thiscore->Index << 10))[i] = 0;
				}
			}
			break;

		default:
		{
			const int addr = omem | ((core == 1) ? 0x400 : 0);
			*(regtable[addr >> 1]) = value;
		}
		break;
	}
}

static void RegWrite_CoreExt(u32 arg, u16 value)
{
	const int addr = arg & 0xfff;
	const int core = arg >> 12;
	V_Core *thiscore = &Cores[core];

	switch (addr)
	{
			/* Master Volume Address Write! */

		case REG_P_MVOLL:
			thiscore->MasterVol.Left.Reg_VOL = value;
			if (!thiscore->MasterVol.Left.Enable)
				thiscore->MasterVol.Left.Value = (s16)(value << 1);
			break;
		case REG_P_MVOLR:
			thiscore->MasterVol.Right.Reg_VOL = value;
			if (!thiscore->MasterVol.Right.Enable)
				thiscore->MasterVol.Right.Value = (s16)(value << 1);
			break;

		case REG_P_EVOLL:
			thiscore->FxVol.Left = (s16)(value);
			break;

		case REG_P_EVOLR:
			thiscore->FxVol.Right = (s16)(value);
			break;

		case REG_P_AVOLL:
			thiscore->ExtVol.Left = (s16)(value);
			break;

		case REG_P_AVOLR:
			thiscore->ExtVol.Right = (s16)(value);
			break;

		case REG_P_BVOLL:
			thiscore->InpVol.Left = (s16)(value);
			break;

		case REG_P_BVOLR:
			thiscore->InpVol.Right = (s16)(value);
			break;

			/* MVOLX has been confirmed to not be allowed to be written to, so cases have been added as a no-op. */
			/* Tokyo Xtreme Racer Zero triggers this code, caused left side volume to be reduced. */

		case REG_P_MVOLXL:
		case REG_P_MVOLXR:
			break;

		default:
		{
			const int raddr = addr + ((core == 1) ? 0x28 : 0);
			*(regtable[raddr >> 1]) = value;
		}
		break;
	}
}


static void RegWrite_SPDIF(u32 addr, u16 value)
{
	*(regtable[addr >> 1]) = value;
	UpdateSpdifMode();
}

static void RegWrite_Raw(u32 addr, u16 value)
{
	*(regtable[addr >> 1]) = value;
}

static void RegWrite_Null(u32 addr, u16 value)
{
	(void)addr;
	(void)value;
}

/* -------------------------------------------------------------------------------------- */
/*  tbl_reg_writes  - Register Write Function Invocation LUT */
/* -------------------------------------------------------------------------------------- */

#define VoiceParamsSet(core, voice)                                   \
	PARAMS(core, voice, 0), PARAMS(core, voice, 1),                   \
		PARAMS(core, voice, 2), PARAMS(core, voice, 3),               \
		PARAMS(core, voice, 4), PARAMS(core, voice, 5),               \
		PARAMS(core, voice, 6), PARAMS(core, voice, 7)

#define VoiceParamsCore(core)                                                                                   \
	VoiceParamsSet(core, 0), VoiceParamsSet(core, 1), VoiceParamsSet(core, 2), VoiceParamsSet(core, 3),         \
		VoiceParamsSet(core, 4), VoiceParamsSet(core, 5), VoiceParamsSet(core, 6), VoiceParamsSet(core, 7),     \
		VoiceParamsSet(core, 8), VoiceParamsSet(core, 9), VoiceParamsSet(core, 10), VoiceParamsSet(core, 11),   \
		VoiceParamsSet(core, 12), VoiceParamsSet(core, 13), VoiceParamsSet(core, 14), VoiceParamsSet(core, 15), \
		VoiceParamsSet(core, 16), VoiceParamsSet(core, 17), VoiceParamsSet(core, 18), VoiceParamsSet(core, 19), \
		VoiceParamsSet(core, 20), VoiceParamsSet(core, 21), VoiceParamsSet(core, 22), VoiceParamsSet(core, 23)

#define VoiceAddrSet(core, voice)                     \
	VADDR(core, voice, 0), VADDR(core, voice, 1),     \
		VADDR(core, voice, 2), VADDR(core, voice, 3), \
		VADDR(core, voice, 4), VADDR(core, voice, 5)

#define CoreParamsPair(core, omem) \
	RCORE(core, omem), RCORE(core, ((omem) + 2))

/* Pass one: which handler. */
#define PARAMS(c, v, p) RegWrite_VoiceParams
#define VADDR(c, v, a)  RegWrite_VoiceAddr
#define RCORE(c, a)     RegWrite_Core
#define RCEXT(c, a)     RegWrite_CoreExt
#define RSPDIF(a)       RegWrite_SPDIF
#define REGRAW(a)       RegWrite_Raw
#define RNULL           RegWrite_Null
#define RNULLPTR        NULL

RegWriteHandler* const tbl_reg_writes[0x401] =
{
#include "reg_write_table.h"
};

#undef PARAMS
#undef VADDR
#undef RCORE
#undef RCEXT
#undef RSPDIF
#undef REGRAW
#undef RNULL
#undef RNULLPTR

/* Pass two: what to hand it. Core index in bit 12, the core-relative
 * register address below, so a handler recovers exactly the constants the
 * table entry named. */
#define PARAMS(c, v, p) (u16)(((c) << 12) | ((v) << 4) | ((p) << 1))
#define VADDR(c, v, a)  (u16)(((c) << 12) | ((v) << 4) | ((a) << 1))
#define RCORE(c, a)     (u16)(((c) << 12) | (a))
#define RCEXT(c, a)     (u16)(((c) << 12) | (a))
#define RSPDIF(a)       (u16)(a)
#define REGRAW(a)       (u16)(a)
#define RNULL           0
#define RNULLPTR        0

const u16 tbl_reg_args[0x401] =
{
#include "reg_write_table.h"
};

