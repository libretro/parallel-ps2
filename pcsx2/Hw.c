/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

#include <string.h> /* memset */

#include "Common.h"
#include "R3000A.h"
#include "IopHw.h"
#include "IopMem.h"
#include "Counters.h"
#include "Sif.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "vtlb.h"
#include "IPU/IPU_hw.h"
#include "SPU2/spu2.h"
#include "USB/USB.h"
#include "FW.h"

#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"

#include "ps2/HwInternal.h"
#include "ps2/pgifio.h"

/* The ends of the register map these handlers split on (Hw.h has the
 * whole map, for C++). */
#define HW_VIF0_START     0x10003800u
#define HW_VIF1_START     0x10003C00u
#define HW_SBUS_PS1_START 0x1000F300u
#define HW_SBUS_PS1_END   0x1000F400u

#define HW_IS_SBUS_PS1(mem) \
	(((mem) & 0x1FFFFFFF) >= HW_SBUS_PS1_START && ((mem) & 0x1FFFFFFF) < HW_SBUS_PS1_END)

static const int rdram_devices = 2;	/* put 8 for TOOL and 2 for PS2 and PSX */
static int rdram_sdevid = 0;

void hwReset(void)
{
	memset(eeHw, 0, sizeof(eeHw));

	psHu32(SBUS_F260) = 0x1D000060;

	/* i guess this is kinda a version, it's used by some bioses */
	psHu32(DMAC_ENABLEW) = 0x1201;
	psHu32(DMAC_ENABLER) = 0x1201;

	/* Sets SPU2 sample rate to PS2 standard (48KHz) whenever emulator is reset.
	 * For PSX mode sample rate setting, see the SBUS_F240 write below. */
	SPU2_Reset(false);

	sifReset();

	gsReset();
	gifHwReset();
	ipuReset();
	vif0Reset();
	vif1Reset();
	gifFifoReset();
	rcntInit();
	USBreset();
	FWreset();
}

uint intcInterrupt(void)
{
	u32 intc_stat = psHu32(INTC_STAT);
	if (intc_stat == 0)
		return 0;
	if ((intc_stat & psHu32(INTC_MASK)) == 0)
		return 0;

	if (intc_stat & 0x2)
	{
		counters[0].hold = rcntRcount(0);
		counters[1].hold = rcntRcount(1);
	}

	return 0x400;
}

uint dmacInterrupt(void)
{
	if( ((psHu16(DMAC_STAT + 2) & psHu16(DMAC_STAT)) == 0 ) &&
		( psHu16(DMAC_STAT) & 0x8000) == 0 )
		return 0;

	if (!dmacRegs.ctrl.DMAE || psHu8(DMAC_ENABLER+2) == 1)
		return 0;

	return 0x800;
}

void hwIntcIrq(int n)
{
	psHu32(INTC_STAT) |= 1<<n;
	if(psHu32(INTC_MASK) & (1<<n))cpuTestINTCInts();
}

void hwDmacIrq(int n)
{
	psHu32(DMAC_STAT) |= 1<<n;
	if(psHu16(DMAC_STAT+2) & (1<<n))cpuTestDMACInts();
}

static __fi void IntCHackCheck(void)
{
	/* Sanity check: To protect from accidentally "rewinding" the cyclecount
	 * on the few times nextBranchCycle can be behind our current cycle. */
	s64 diff = cpuRegs.nextEventCycle - cpuRegs.cycle;
	if (diff > 0 && (cpuRegs.cycle - cpuRegs.lastEventCycle) > 8) cpuRegs.cycle = cpuRegs.nextEventCycle;
}

/* --------------------------------------------------------------------------
 *  The handlers, one set per 4K page
 * --------------------------------------------------------------------------
 * Each worker below takes the page as its first argument; every per-page
 * handler (at the end) calls it with a constant, so after inlining each
 * one is the switch arm its page needs, as the templates were. The 64-
 * and 128-bit ones call back into the 32-bit ones and so are left to the
 * compiler; the FIFOs they mostly serve are not the hot path. */

static __fi mem32_t hw_read32(const u32 page, u32 mem, const bool intcstathack);
static mem64_t hw_read64(const u32 page, u32 mem);
static __fi void hw_write32(const u32 page, u32 mem, u32 value);
static void hw_write64(const u32 page, u32 mem, u64 value);
static void hw_write128(const u32 page, u32 mem, const u128* srcval);

/* FIFOs are the only "legal" 128 bit registers, so they are handled first.
 * All other registers fall back on the 64-bit handler (and from there all
 * non-IPU reads fall back to the 32-bit handler). */
static void hw_read128(const u32 page, u32 mem, u128* out)
{
	u32 part[4];
	u64 v64;

	/* Zero-init: a 128-bit read of a page-0x0F register that is neither in
	 * the SBUS_PS1 range nor at 0x1000f3e0 fills nothing in below. */
	memset(out, 0, sizeof(*out));

	switch (page)
	{
		case 0x05:
			ReadFIFO_VIF1(out);
			break;

		case 0x07:
			if (mem & 0x10)
				break; /* IPUin is write-only */
			ReadFIFO_IPUout(out);
			break;

		case 0x04:
		case 0x06:
			/* VIF0 and GIF are write-only.
			 * [Ps2Confirm] Reads from these FIFOs (and IPUin) do one of the following:
			 * return zero, leave contents of the dest register unchanged, or in some
			 * indeterminate state.  The actual behavior probably isn't important. */
			break;

		case 0x0F:
			/* TODO/FIXME: PSX mode: this is new */
			if (HW_IS_SBUS_PS1(mem))
			{
				PGIFrQword((mem & 0x1FFFFFFF), out);
				break;
			}

			/* WARNING: this code is never executed anymore due to previous condition.
			 * It requires investigation of what to do. */
			if (mem == 0x1000f3E0)
			{
				ReadFifoSingleWord();
				part[0] = psHu32(0x1000f3E0);
				ReadFifoSingleWord();
				part[1] = psHu32(0x1000f3E0);
				ReadFifoSingleWord();
				part[2] = psHu32(0x1000f3E0);
				ReadFifoSingleWord();
				part[3] = psHu32(0x1000f3E0);
				memcpy(out, part, 16);
			}
			break;

		default:
			v64 = hw_read64(page, mem);
			out->_u64[0] = v64;
			out->_u64[1] = v64;
			break;
	}
}

static __fi mem32_t hw_read32(const u32 page, u32 mem, const bool intcstathack)
{
	PCSX2_ALIGN(16) u128 out128;
	u32 sif2fifosize;
	int ret;

	switch( page )
	{
		case 0x00:
		case 0x01:	return rcntRead32( mem );

		case 0x02:	return ipuRead32( mem );

		case 0x03:
			if (mem >= HW_VIF0_START)
			{
				if(mem >= HW_VIF1_START)
					return vif1Read32(mem);
				return vif0Read32(mem);
			}
			return gifRead32(mem);

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
			/* [Ps2Confirm] Reading from FIFOs using non-128 bit reads is a complete mystery.
			 * No game is known to attempt such a thing (yay!), so probably nothing for us to
			 * worry about.  Chances are, though, doing so is "legal" and yields some sort
			 * of reproducible behavior.  Candidate for real hardware testing.
			 * Current assumption: Reads 128 bits and discards the unused portion. */
			hw_read128(page, mem & ~0x0f, &out128);
			return out128._u32[(mem >> 2) & 0x3];

		case 0x0f:
			/* INTC_STAT shortcut for heavy spinning.
			 * Performance Note: Visual Studio handles this best if we just manually check for it here,
			 * outside the context of the switch statement below.  This is likely fixed by PGO also,
			 * but it's an easy enough conditional to account for anyways. */
			if (mem == INTC_STAT)
			{
				/* Disable INTC hack when in PS1 mode as it seems to break games. */
				if (intcstathack && !(psxHu32(HW_ICFG) & (1 << 3)))
					IntCHackCheck();
				return psHu32(INTC_STAT);
			}

			/* todo: psx mode: this is new */
			if (HW_IS_SBUS_PS1(mem))
				return PGIFr((mem & 0x1FFFFFFF));

			/* WARNING: this code is never executed anymore due to previous condition.
			 * It requires investigation of what to do. */
			if ((mem & 0x1000ff00) == 0x1000f300)
			{
				sif2fifosize = pcsx2_min_i(sif2.fifo.size, 7);

				switch (mem & 0xf0)
				{
					case 0x00:
						return psxHu32(0x1f801814);
					case 0x80:
						ret = psHu32(mem) | (sif2fifosize << 16);
						if (sif2.fifo.size > 0)
							ret |= 0x80000000;
						return ret;
					case 0xc0:
						ReadFifoSingleWord();
						return psHu32(mem);
					case 0xe0:
						if (sif2.fifo.size > 0)
						{
							ReadFifoSingleWord();
							return psHu32(mem);
						}
						break;
				}
				return 0;
			}
			switch( mem )
			{
				case SIO_ISR:

				case 0x1000f410:
				case MCH_RICM:
					return 0;

				case SBUS_F240:
					return psHu32(SBUS_F240) | 0xF0000102;
				case SBUS_F260:
					return psHu32(SBUS_F260);
				case MCH_DRD:
					if( !((psHu32(MCH_RICM) >> 6) & 0xF) )
					{
						switch ((psHu32(MCH_RICM)>>16) & 0xFFF)
						{
							/* MCH_RICM: x:4|SA:12|x:5|SDEV:1|SOP:4|SBC:1|SDEV:5 */

							case 0x21: /* INIT */
								if (rdram_sdevid < rdram_devices)
								{
									rdram_sdevid++;
									return 0x1F;
								}
								return 0;

							case 0x23: /* CNFGA */
								return 0x0D0D;	/* PVER=3 | MVER=16 | DBL=1 | REFBIT=5 */

							case 0x24: /* CNFGB */
								/* 0x0110 for PSX  SVER=0 | CORG=8(5x9x7) | SPT=1 | DEVTYP=0 | BYTE=0 */
								return 0x0090;	/* SVER=0 | CORG=4(5x9x6) | SPT=1 | DEVTYP=0 | BYTE=0 */

							case 0x40: /* DEVID */
								return psHu32(MCH_RICM) & 0x1F;	/* =SDEV */
						}
					}
					return 0;
			}
			break;
		default: break;
	}
	/* Hack for Transformers and Test Drive Unlimited to simulate filling the VIF FIFO
	 * It actually stalls VIF a few QW before the end of the transfer, so we need to pretend its all gone
	 * else itll take aaaaaaaaages to boot. */
	if(mem == (D1_CHCR + 0x10) && emu_config_viffifo_hack())
		return psHu32(mem) + (vif1ch.qwc * 16);

	return psHu32(mem);
}

/* The narrow accesses are lanes of the word, little-endian as the EE is. */
static __fi mem8_t hw_read8(const u32 page, u32 mem)
{
	const u32 ret32 = hw_read32(page, mem & ~0x03, false);
	return (mem8_t)(ret32 >> ((mem & 0x03) * 8));
}

static __fi mem16_t hw_read16(const u32 page, u32 mem, const bool intcstathack)
{
	const u32 ret32 = hw_read32(page, mem & ~0x03, intcstathack);
	return (mem16_t)(ret32 >> ((mem & 0x02) * 8));
}

static mem64_t hw_read64(const u32 page, u32 mem)
{
	PCSX2_ALIGN(16) u128 full;
	u32 lo, hi;

	switch (page)
	{
		case 0x02:
			return ipuRead64(mem);

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
			/* [Ps2Confirm] Reading from FIFOs using non-128 bit reads is a complete mystery.
			 * Current assumption: Reads 128 bits and discards the unused portion. */
			hw_read128(page, mem & ~0x0f, &full);
			return full._u64[(mem >> 3) & 0x1];

		case 0x0F:
			if (mem == 0x1000f3E0)
			{
				ReadFifoSingleWord();
				lo = psHu32(0x1000f3E0);
				ReadFifoSingleWord();
				hi = psHu32(0x1000f3E0);
				return (u64)lo | ((u64)hi << 32);
			}
			break;
		default: break;
	}

	return (u64)hw_read32(page, mem, false);
}

/* Shift the middle 8 bits (bits 4-12) into the lower 8 bits.
 * This helps the compiler optimize the switch statement into a lookup table. :) */
#define HELPSWITCH(m) (((m)>>4) & 0xff)

/* FIFOs are the only "legal" 128 bit registers.  Handle them first.
 * all other registers fall back on the 64-bit handler (and from there
 * most of them fall back to the 32-bit handler). */
static void hw_write128(const u32 page, u32 mem, const u128* srcval)
{
	switch (page)
	{
		case 0x04:
			WriteFIFO_VIF0(srcval);
			break;

		case 0x05:
			WriteFIFO_VIF1(srcval);
			break;

		case 0x06:
			WriteFIFO_GIF(srcval);
			break;

		case 0x07:
			if (mem & 0x10)
				WriteFIFO_IPUin(srcval);
			break;

		case 0x0F:
			/* todo: psx mode: this is new */
			if (HW_IS_SBUS_PS1(mem))
			{
				PGIFwQword((mem & 0x1FFFFFFF), (void*)srcval);
				return;
			}
			/* fallthrough */
			hw_write64(page, mem, srcval->_u64[0]);
			break;
		default:
			hw_write64(page, mem, srcval->_u64[0]);
			break;
	}
}

/* The EE's SIO transmit FIFO: a byte at a time, gathered into lines. */
static void hw_sio_tx(u8 value)
{
	static bool included_newline = false;
	static char sio_buffer[1024];
	static int sio_count;

	if (value == '\r')
	{
		included_newline        = true;
		sio_buffer[sio_count++] = '\n';
	}
	else if (!included_newline || (value != '\n'))
	{
		included_newline        = false;
		sio_buffer[sio_count++] = value;
	}

	if ((sio_count == C89_ARRAY_SIZE(sio_buffer)-1) || (sio_count != 0 && sio_buffer[sio_count-1] == '\n'))
	{
		sio_buffer[sio_count]   = 0;
		sio_count               = 0;
	}
}

static __fi void hw_write8(const u32 page, u32 mem, u8 value)
{
	u32 merged;

	if (mem == SIO_TXFIFO)
	{
		hw_sio_tx(value);
		return;
	}

	switch(mem & ~3)
	{
		case DMAC_STAT:
		case INTC_STAT:
		case INTC_MASK:
		case DMAC_FAKESTAT:
			hw_write32(page, mem & ~3, (u32)value << (mem & 3) * 8);
			break;
		default:
			merged = hw_read32(page, mem & ~0x03, false);
			merged &= ~(0xffu << ((mem & 0x3) * 8));
			merged |= (u32)value << ((mem & 0x3) * 8);

			hw_write32(page, mem & ~0x03, merged);
			break;
	}
}

static __fi void hw_write32(const u32 page, u32 mem, u32 value)
{
	/* Notes:
	 * All unknown registers on the EE are "reserved" as discarded writes and indeterminate
	 * reads.  Bus error is only generated for registers outside the first 16k of mapped
	 * register space (which is handled by the VTLB mapping, so no need for checks here). */
	PCSX2_ALIGN(16) u128 zerofill;
	u32 cycle;

	switch (page)
	{
		case 0x00:
		case 0x01:
			if (!rcntWrite32(mem, &value))
				return;
			break;
		case 0x02:
			if (!ipuWrite32(mem, value))
				return;
			break;
		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
			/* [Ps2Confirm] Direct FIFO read/write behavior.  We need to create a test that writes
			 * data to one of the FIFOs and determine the result.  I'm not quite sure offhand a good
			 * way to do that --air
			 * Current assumption is that 32-bit and 64-bit writes likely do 128-bit zero-filled
			 * writes (upper 96 bits are 0, lower 32 bits are effective). */
			memset(&zerofill, 0, sizeof(zerofill));
			zerofill._u32[(mem >> 2) & 0x03] = value;
			hw_write128(page, mem & ~0x0f, &zerofill);
			return;

		case 0x03:
			if (mem >= HW_VIF0_START)
			{
				if(mem >= HW_VIF1_START)
				{
					if (!vif1Write32(mem, value)) return;
				}
				else
				{
					if (!vif0Write32(mem, value)) return;
				}
			}
			else if (!gifWrite32(mem, value))
				return;
			break;

		case 0x08:
		case 0x09:
		case 0x0a:
		case 0x0b:
		case 0x0c:
		case 0x0d:
		case 0x0e:
			if (!dmacWrite32(mem, value))
				return;
			break;

		case 0x0f:
			switch( HELPSWITCH(mem) )
			{
				case HELPSWITCH(INTC_STAT):
					psHu32(INTC_STAT) &= ~value;
					return;

				case HELPSWITCH(INTC_MASK):
					psHu32(INTC_MASK) ^= (u16)value;
					cpuTestINTCInts();
					return;

				case HELPSWITCH(SIO_TXFIFO):
					/* [Ps2Confirm] What happens when we write 32bit
					 * values to SIO_TXFIFO?
					 * If it works like the IOP, then all 32bits are
					 * written to the FIFO in  order.
					 * PCSX2 up to this point simply ignored
					 * non-8bit writes to this port. */
					hw_sio_tx((u8)value);
					hw_sio_tx((u8)(value >> 8));
					hw_sio_tx((u8)(value >> 16));
					hw_sio_tx((u8)(value >> 24));
					return;

				case HELPSWITCH(SBUS_F220):
					psHu32(mem) |= value;
					return;

				case HELPSWITCH(SBUS_F230):
					psHu32(mem) &= ~value;
					return;

				case HELPSWITCH(SBUS_F240) :
					if (value & (1 << 19))
					{
						cycle = psxRegs.cycle;
						psxReset();
						PSXCLK =  33868800;
						SPU2_Reset(true);
						setPs1CDVDSpeed(cdvd.Speed);
						psxHu32(0x1f801450) = 0x8;
						psxHu32(0x1f801078) = 1;
						psxRegs.cycle = cycle;
					}
					if(!(value & 0x100))
						psHu32(mem) &= ~0x100;
					else
						psHu32(mem) |= 0x100;
					return;

				case HELPSWITCH(SBUS_F260):
					psHu32(mem) = value;
					return;

				case HELPSWITCH(MCH_RICM): /* MCH_RICM: x:4|SA:12|x:5|SDEV:1|SOP:4|SBC:1|SDEV:5 */
					if ((((value >> 16) & 0xFFF) == 0x21) && (((value >> 6) & 0xF) == 1) && (((psHu32(0xf440) >> 7) & 1) == 0)) /* INIT & SRP=0 */
						rdram_sdevid = 0;	/* if SIO repeater is cleared, reset sdevid */
					psHu32(mem) = value & ~0x80000000;	/* kill the busy bit */
					return;

				case HELPSWITCH(SBUS_F200):
				case HELPSWITCH(MCH_DRD):
					break;

				case HELPSWITCH(DMAC_ENABLEW):
					if (!dmacWrite32(DMAC_ENABLEW, value)) return;
					break;

				default:
					/* TODO: psx add the real address in a sbus HELPSWITCH */
					if (HW_IS_SBUS_PS1(mem))
					{
						PGIFw((mem & 0x1FFFFFFF), value);
						return;
					}
			}
			break;
	}

	psHu32(mem) = value;
}

static __fi void hw_write16(const u32 page, u32 mem, u16 value)
{
	u32 merged;

	switch(mem & ~3)
	{
		case DMAC_STAT:
		case INTC_STAT:
		case INTC_MASK:
		case DMAC_FAKESTAT:
			hw_write32(page, mem & ~3, (u32)value << (mem & 3) * 8);
			break;
		default:
			merged = hw_read32(page, mem & ~0x03, false);
			merged &= ~(0xffffu << ((mem & 0x2) * 8));
			merged |= (u32)value << ((mem & 0x2) * 8);

			hw_write32(page, mem & ~0x03, merged);
			break;
	}
}

static void hw_write64(const u32 page, u32 mem, u64 value)
{
	/* * Only the IPU has true 64 bit registers.
	 * * FIFOs have 128 bit registers that are probably zero-fill.
	 * * All other registers likely disregard the upper 32-bits and simply act as normal
	 *   32-bit writes. */
	PCSX2_ALIGN(16) u128 zerofill;

	switch (page)
	{
		case 0x02:
			if (!ipuWrite64(mem, value))
				return;
			memcpy(&eeHw[(mem) & 0xffff], &value, sizeof(value));
			break;
		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
			memset(&zerofill, 0, sizeof(zerofill));
			zerofill._u64[(mem >> 3) & 0x01] = value;
			hw_write128(page, mem & ~0x0f, &zerofill);
			break;

		default:
			/* disregard everything except the lower 32 bits.
			 * ... and skip the 64 bit writeback since the 32-bit one will suffice. */
			hw_write32(page, mem, (u32)value);
			break;
	}
}

/* The handler set of one page, as the vtlb calls it. */
#if PCSX2_MINGW_R128_BY_PTR
#define HW_WRITE128_HANDLER(p) \
	static void hwWrite128_##p(u32 mem, const r128* srcval) \
	{ \
		PCSX2_ALIGN(16) u128 v = r128_to_u128(r128_load(srcval)); \
		hw_write128(p, mem, &v); \
	}
#else
#define HW_WRITE128_HANDLER(p) \
	static void TAKES_R128 hwWrite128_##p(u32 mem, r128 srcval) \
	{ \
		PCSX2_ALIGN(16) u128 v = r128_to_u128(srcval); \
		hw_write128(p, mem, &v); \
	}
#endif

#define HW_PAGE_HANDLERS(p) \
	static mem8_t  hwRead8_##p (u32 mem) { return hw_read8(p, mem); } \
	static mem16_t hwRead16_##p(u32 mem) { return hw_read16(p, mem, false); } \
	static mem32_t hwRead32_##p(u32 mem) { return hw_read32(p, mem, false); } \
	static mem64_t hwRead64_##p(u32 mem) { return hw_read64(p, mem); } \
	static RETURNS_R128 hwRead128_##p(u32 mem) \
	{ \
		PCSX2_ALIGN(16) u128 v; \
		hw_read128(p, mem, &v); \
		return r128_load(&v); \
	} \
	static void hwWrite8_##p (u32 mem, mem8_t value)  { hw_write8(p, mem, value); } \
	static void hwWrite16_##p(u32 mem, mem16_t value) { hw_write16(p, mem, value); } \
	static void hwWrite32_##p(u32 mem, mem32_t value) { hw_write32(p, mem, value); } \
	static void hwWrite64_##p(u32 mem, mem64_t value) { hw_write64(p, mem, value); } \
	HW_WRITE128_HANDLER(p)

HW_PAGE_HANDLERS(0x00)
HW_PAGE_HANDLERS(0x01)
HW_PAGE_HANDLERS(0x02)
HW_PAGE_HANDLERS(0x03)
HW_PAGE_HANDLERS(0x04)
HW_PAGE_HANDLERS(0x05)
HW_PAGE_HANDLERS(0x06)
HW_PAGE_HANDLERS(0x07)
HW_PAGE_HANDLERS(0x08)
HW_PAGE_HANDLERS(0x09)
HW_PAGE_HANDLERS(0x0a)
HW_PAGE_HANDLERS(0x0b)
HW_PAGE_HANDLERS(0x0c)
HW_PAGE_HANDLERS(0x0d)
HW_PAGE_HANDLERS(0x0e)
HW_PAGE_HANDLERS(0x0f)

/* INTC_STAT reads with the fast-forward to the next event. */
static mem16_t hwRead16_page_0F_INTC_HACK(u32 mem) { return hw_read16(0x0f, mem, true); }
static mem32_t hwRead32_page_0F_INTC_HACK(u32 mem) { return hw_read32(0x0f, mem, true); }

#define HW_HANDLERS(p) \
	hwRead8_##p,  hwRead16_##p,  hwRead32_##p,  hwRead64_##p,  hwRead128_##p, \
	hwWrite8_##p, hwWrite16_##p, hwWrite32_##p, hwWrite64_##p, hwWrite128_##p

u32 hwRegisterPageHandlers(u32 page)
{
	switch (page)
	{
		case 0x0: return vtlb_RegisterHandler(HW_HANDLERS(0x00));
		case 0x1: return vtlb_RegisterHandler(HW_HANDLERS(0x01));
		case 0x2: return vtlb_RegisterHandler(HW_HANDLERS(0x02));
		case 0x3: return vtlb_RegisterHandler(HW_HANDLERS(0x03));
		case 0x4: return vtlb_RegisterHandler(HW_HANDLERS(0x04));
		case 0x5: return vtlb_RegisterHandler(HW_HANDLERS(0x05));
		case 0x6: return vtlb_RegisterHandler(HW_HANDLERS(0x06));
		case 0x7: return vtlb_RegisterHandler(HW_HANDLERS(0x07));
		case 0x8: return vtlb_RegisterHandler(HW_HANDLERS(0x08));
		case 0x9: return vtlb_RegisterHandler(HW_HANDLERS(0x09));
		case 0xa: return vtlb_RegisterHandler(HW_HANDLERS(0x0a));
		case 0xb: return vtlb_RegisterHandler(HW_HANDLERS(0x0b));
		case 0xc: return vtlb_RegisterHandler(HW_HANDLERS(0x0c));
		case 0xd: return vtlb_RegisterHandler(HW_HANDLERS(0x0d));
		case 0xe: return vtlb_RegisterHandler(HW_HANDLERS(0x0e));
		default:  return vtlb_NewHandler();
	}
}

void hwAssignPage0FHandlers(u32 handler, bool intc_stat_hack)
{
	vtlb_ReassignHandler(handler,
		hwRead8_0x0f,
		intc_stat_hack ? hwRead16_page_0F_INTC_HACK : hwRead16_0x0f,
		intc_stat_hack ? hwRead32_page_0F_INTC_HACK : hwRead32_0x0f,
		hwRead64_0x0f, hwRead128_0x0f,
		hwWrite8_0x0f, hwWrite16_0x0f, hwWrite32_0x0f, hwWrite64_0x0f, hwWrite128_0x0f);
}
