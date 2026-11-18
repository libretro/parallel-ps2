/* SPDX-License-Identifier: LGPL-3.0+ */
/* What GSState.cpp did with a savestate before GSStateFile.c: the write
 * (Freeze, from the data on), the read (Defrost, between its flush and
 * reset and what it derives after), and the registers made over
 * (NormalizeRestoredRegs). Kept as the reference the C is held to; the
 * renderer's hearing of the changed pages is a copy of them here. */
#include "reference.h"

template <class T>
static void WriteState(u8*& dst, T* src, size_t len = sizeof(T))
{
	memcpy(dst, src, len);
	dst += len;
}

template <class T>
static void ReadState(T* dst, u8*& src, size_t len = sizeof(T))
{
	memcpy(dst, src, len);
	src += len;
}

void RefState::Write(u8* data)
{
	const u32 version = STATE_VERSION;

	WriteState(data, &version);
	WriteState(data, &m_env.PRIM);
	WriteState(data, &m_env.PRMODECONT);
	WriteState(data, &m_env.TEXCLUT);
	WriteState(data, &m_env.SCANMSK);
	WriteState(data, &m_env.TEXA);
	WriteState(data, &m_env.FOGCOL);
	WriteState(data, &m_env.DIMX);
	WriteState(data, &m_env.DTHE);
	WriteState(data, &m_env.COLCLAMP);
	WriteState(data, &m_env.PABE);
	WriteState(data, &m_env.BITBLTBUF);
	WriteState(data, &m_env.TRXDIR);
	WriteState(data, &m_env.TRXPOS);
	WriteState(data, &m_env.TRXREG);
	WriteState(data, &m_env.TRXREG); // obsolete

	for (int i = 0; i < 2; i++)
	{
		WriteState(data, &m_env.CTXT[i].XYOFFSET);
		WriteState(data, &m_env.CTXT[i].TEX0);
		WriteState(data, &m_env.CTXT[i].TEX1);
		WriteState(data, &m_env.CTXT[i].CLAMP);
		WriteState(data, &m_env.CTXT[i].MIPTBP1);
		WriteState(data, &m_env.CTXT[i].MIPTBP2);
		WriteState(data, &m_env.CTXT[i].SCISSOR);
		WriteState(data, &m_env.CTXT[i].ALPHA);
		WriteState(data, &m_env.CTXT[i].TEST);
		WriteState(data, &m_env.CTXT[i].FBA);
		WriteState(data, &m_env.CTXT[i].FRAME);
		WriteState(data, &m_env.CTXT[i].ZBUF);
	}

	WriteState(data, &m_v.RGBAQ);
	WriteState(data, &m_v.ST);
	WriteState(data, &m_v.UV);
	WriteState(data, &m_v.FOG);
	WriteState(data, &m_v.XYZ);
	memset(data, 0, sizeof(GIFReg)); // obsolete
	data += sizeof(GIFReg);
	WriteState(data, &m_tr.x);
	WriteState(data, &m_tr.y);
	WriteState(data, m_mem.m_vm8, m_mem.m_vmsize);

	for (RefGIFPath& path : m_path)
	{
		path.tag.NREG = path.nreg;
		path.tag.NLOOP = path.nloop;
		path.tag.REGS = 0;

		for (size_t j = 0; j < C89_ARRAY_SIZE(path.regs.U8); j++)
		{
			path.tag.U32[2 + (j >> 3)] |= path.regs.U8[j] << ((j & 7) << 2);
		}

		WriteState(data, &path.tag);
		WriteState(data, &path.reg);
	}

	WriteState(data, &m_q);
}


void RefState::Read(u8* data)
{
	u32 version;

	ReadState(&version, data);


	ReadState(&m_env.PRIM, data);

	if (version <= 6)
		data += sizeof(GIFRegPRMODE);

	ReadState(&m_env.PRMODECONT, data);
	ReadState(&m_env.TEXCLUT, data);
	ReadState(&m_env.SCANMSK, data);
	ReadState(&m_env.TEXA, data);
	ReadState(&m_env.FOGCOL, data);
	ReadState(&m_env.DIMX, data);
	ReadState(&m_env.DTHE, data);
	ReadState(&m_env.COLCLAMP, data);
	ReadState(&m_env.PABE, data);
	ReadState(&m_env.BITBLTBUF, data);
	ReadState(&m_env.TRXDIR, data);
	ReadState(&m_env.TRXPOS, data);
	ReadState(&m_env.TRXREG, data);
	ReadState(&m_env.TRXREG, data); // obsolete
	// Technically this value ought to be saved like m_tr.x/y (break
	// compatibility) but so far only a single game (Motocross Mania) really
	// depends on this value (i.e != BITBLTBUF) Savestates are likely done at
	// VSYNC, so not in the middle of a texture transfer, therefore register
	// will be set again properly
	m_tr.m_blit = m_env.BITBLTBUF;

	for (int i = 0; i < 2; i++)
	{
		ReadState(&m_env.CTXT[i].XYOFFSET, data);
		ReadState(&m_env.CTXT[i].TEX0, data);
		ReadState(&m_env.CTXT[i].TEX1, data);

		if (version <= 6)
			data += sizeof(GIFRegTEX2);

		ReadState(&m_env.CTXT[i].CLAMP, data);
		ReadState(&m_env.CTXT[i].MIPTBP1, data);
		ReadState(&m_env.CTXT[i].MIPTBP2, data);
		ReadState(&m_env.CTXT[i].SCISSOR, data);
		ReadState(&m_env.CTXT[i].ALPHA, data);
		ReadState(&m_env.CTXT[i].TEST, data);
		ReadState(&m_env.CTXT[i].FBA, data);
		ReadState(&m_env.CTXT[i].FRAME, data);
		ReadState(&m_env.CTXT[i].ZBUF, data);

		if (version <= 4)
			data += sizeof(u32) * 7; // skip
	}

	ReadState(&m_v.RGBAQ, data);
	ReadState(&m_v.ST, data);
	ReadState(&m_v.UV, data);
	ReadState(&m_v.FOG, data);
	ReadState(&m_v.XYZ, data);
	data += sizeof(GIFReg); // obsolite
	ReadState(&m_tr.x, data);
	ReadState(&m_tr.y, data);
	/* A page at a time: one that already holds what the state does is
	 * left alone, and the renderer hears which ones changed. */
	{
		u32 changed[GS_MAX_PAGES / 32];
		u32 page;

		memset(changed, 0, sizeof(changed));
		for (page = 0; page < GS_MAX_PAGES; page++)
		{
			u8* const dst = m_mem.m_vm8 + page * GS_PAGE_SIZE;
			if (memcmp(dst, data, GS_PAGE_SIZE) != 0)
			{
				memcpy(dst, data, GS_PAGE_SIZE);
				changed[page >> 5] |= 1u << (page & 31);
			}
			data += GS_PAGE_SIZE;
		}
		memcpy(m_changed, changed, sizeof(changed));
	}

	m_tr.total = 0; // TODO: restore transfer state

	for (RefGIFPath& path : m_path)
	{
		ReadState(&path.tag, data);
		ReadState(&path.reg, data);

		path.SetTag(&path.tag); // expand regs

		/* SetTag leaves an idle path's registers as they were, and Freeze
		 * writes them back: an idle path takes the state's too, so the
		 * state saved after a load is the state that was loaded. */
		if (path.nloop == 0)
		{
			const GSVector4i v = GSVector4i::loadl(&path.tag.REGS);
			path.nreg = (path.tag.U32[1] & 0xf0000000) ? (path.tag.U32[1] >> 28) : 16;
			path.regs = v.upl8(v >> 4) & GSVector4i::x0f(path.nreg);
		}
	}

	ReadState(&m_q, data);
}

/* A Q that a restore brought back, made into one the handlers could have
 * produced: a zero becomes the substitute that handler uses, and a NaN
 * becomes FLT_MAX. The zero test is on the bit pattern, not the value,
 * because that is what the handlers do -- so a negative zero is left alone
 * here exactly as it is there. */
static float ScrubRestoredQ(float q, float zero_substitute)
{
	u32 bits;
	memcpy(&bits, &q, sizeof(bits));
	if (bits == 0)
		return zero_substitute;
	if (q != q)
		return FLT_MAX;
	return q;
}

/* Every GS register a game writes goes through a GIFRegHandler, and a good
 * many of those handlers do more than store the value: they mask a field to
 * its real width, clamp it to a range the rest of the emulator relies on,
 * substitute a value a particular game depends on, or maintain an invariant
 * between two registers. Defrost restores registers with a raw copy, so none
 * of that happens, and every one of those guarantees is silently void for the
 * rest of the session.
 *
 * That is not a theoretical gap. The ALPHA clamp below is the only thing
 * keeping a blend index inside an 81-entry table, and the two Q scrubs exist
 * because named games emit a zero or a NaN there; a state file put all three
 * straight back. Two registers were being re-normalised by hand here, which
 * is how the ALPHA one came to be noticed and the other nine did not.
 *
 * So when a handler transforms a value, this has to transform it the same
 * way, and a new handler that does needs a line here. What it deliberately
 * does NOT do is improve on the handlers: the Q scrubs compare the bit
 * pattern rather than the float, so a negative zero passes through both here
 * and there, and that stays true until someone changes the handler. */
void RefState::NormalizeRestoredRegs()
{
	for (u32 i = 0; i < 2; i++)
	{
		GSDrawingContext& ctx = m_env.CTXT[i];

		// GIFRegHandlerXYOFFSET: OFX/OFY are declared u32, and the header
		// states the handler is what keeps the pad bits clear.
		ctx.XYOFFSET.OFX &= 0xffff;
		ctx.XYOFFSET.OFY &= 0xffff;

		// GIFRegHandlerALPHA: clamped to 2 because GSRendererHW builds a
		// blend index as ((A * 3 + B) * 3 + C) * 3 + D against an 81-entry
		// table. These are 2-bit fields, so a state holding 3 in each
		// reaches 120 and reads thirty-nine entries past the end.
		ctx.ALPHA.A = pcsx2_min_i(ctx.ALPHA.A, 2U);
		ctx.ALPHA.B = pcsx2_min_i(ctx.ALPHA.B, 2U);
		ctx.ALPHA.C = pcsx2_min_i(ctx.ALPHA.C, 2U);
		ctx.ALPHA.D = pcsx2_min_i(ctx.ALPHA.D, 2U);

		// GIFRegHandlerFRAME: FBW is clamped to 32. The field is 6 bits, so
		// a state can ask for 63, and FRAME.FBW * 64 is a target width the
		// texture cache allocates from.
		ctx.FRAME.FBW = (32U < ctx.FRAME.FBW) ? 32U : ctx.FRAME.FBW;

		// GIFRegHandlerFRAME: the three formats that address only part of a
		// 32-bit pixel are rewritten to PSMCT32 with a mask that spares the
		// rest. Berserk depends on the first of them. A saved FRAME should
		// already hold the substituted value, so this is a no-op for any
		// state the handler produced.
		switch (ctx.FRAME.PSM)
		{
			case PSMT8H:
				ctx.FRAME.PSM   = PSMCT32;
				ctx.FRAME.FBMSK = 0x00FFFFFF;
				break;
			case PSMT4HH:
				ctx.FRAME.PSM   = PSMCT32;
				ctx.FRAME.FBMSK = 0x0FFFFFFF;
				break;
			case PSMT4HL:
				ctx.FRAME.PSM   = PSMCT32;
				ctx.FRAME.FBMSK = 0xF0FFFFFF;
				break;
			default:
				break;
		}

		// GIFRegHandlerFRAME and GIFRegHandlerZBUF both maintain this: on
		// hardware, a Z-format FRAME forces the Z buffer to colour
		// swizzling. Powerdrome clears Z by relying on it. Each handler
		// re-establishes it on every write to either register, so the pair
		// is never seen uncoupled -- except after a restore, which brings
		// them back independently and then derives offset.zb and
		// offset.fzb4 from the mismatch.
		if ((ctx.FRAME.PSM & 0x30) == 0x30)
			ctx.ZBUF.PSM &= ~0x30;
		else
			ctx.ZBUF.PSM |= 0x30;

		// ApplyTEX0: CPSM is restricted to 1010b. Defrost calls ApplyTEX0
		// for the active context only, and after m_prev_env is taken, so
		// neither copy of the other context was ever masked.
		ctx.TEX0.CPSM &= 0xa;
	}

	// GIFPackedRegHandlerUV and GIFRegHandlerUV: UV is 10.4 fixed point in
	// 14 bits. GSRegs.h states these handlers are what keeps the pad bits
	// clear, and a raw restore falsifies that.
	m_v.UV &= 0x3fff3fff;

	// GIFRegHandlerRGBAQ and GIFPackedRegHandlerSTQ scrub Q on the way in,
	// both for named games: Silent Hill emits a NaN, which breaks the
	// min/max in GSVertexTrace, and Vexx emits a zero, which becomes a
	// division by zero further down. A state file reintroduced both, past
	// everything downstream that was written to assume they were gone.
	// Substituting on the bit pattern rather than the value, as the two
	// handlers do.
	m_v.RGBAQ.Q = ScrubRestoredQ(m_v.RGBAQ.Q, 1.0f);
	m_q         = ScrubRestoredQ(m_q, FLT_MIN);

	// GIFRegHandlerSCANMSK latches this when the register arrives; Reset
	// cleared it and the raw restore of SCANMSK does not set it again.
	if (m_env.SCANMSK.MSK & 2)
		m_scanmask_used = 2;
}

//
