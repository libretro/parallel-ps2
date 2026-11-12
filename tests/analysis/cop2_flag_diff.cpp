/*  Differential test: the EE COP2 flag-hack analysis, x86 vs arm64.
 *
 *  Both recompilers decide which VU0-macro instructions must compute and
 *  commit MAC / status / clip flags. The x86 side runs COP2FlagHackPass over
 *  a whole block; the arm64 side analyses a RUN of consecutive native macro
 *  ALU ops as it emits. The two were written independently, and a divergence
 *  between them is a bug in one of them.
 *
 *  This harness compiles BOTH implementations verbatim from the recompiler
 *  sources (extracted at build time by gen.py, never retyped), backs them
 *  with a synthetic instruction memory, and compares the five flag-hack bits
 *  instruction-for-instruction over generated blocks.
 *
 *  Comparison scope, and why: the arm64 analysis only ever looks at a run of
 *  native macro ALU ops, so the comparison is restricted to instructions
 *  inside such a run. The SYNC/FINISH/FLUSH bits arm64 sets are the x86
 *  COP2MicroFinishPass's job and are masked out here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int   u32;
typedef unsigned long long u64;
typedef signed int     s32;
typedef unsigned char  u8;

#define EEINST_COP2_DENORMALIZE_STATUS_FLAG 0x100
#define EEINST_COP2_NORMALIZE_STATUS_FLAG   0x200
#define EEINST_COP2_STATUS_FLAG             0x400
#define EEINST_COP2_MAC_FLAG                0x800
#define EEINST_COP2_CLIP_FLAG               0x1000
#define EEINST_COP2_SYNC_VU0                0x2000
#define EEINST_COP2_FINISH_VU0              0x4000
#define EEINST_COP2_FLUSH_VU0_REGISTERS     0x8000
#define FLAGHACK_BITS (EEINST_COP2_DENORMALIZE_STATUS_FLAG | \
                       EEINST_COP2_NORMALIZE_STATUS_FLAG   | \
                       EEINST_COP2_STATUS_FLAG             | \
                       EEINST_COP2_MAC_FLAG                | \
                       EEINST_COP2_CLIP_FLAG)

#define REG_STATUS_FLAG 16
#define REG_MAC_FLAG    17
#define REG_CLIP_FLAG   18
#define REG_FBRST       19

struct EEINST { u32 info; };

/* Synthetic instruction memory: blocks live at BASE. */
#define BASE 0x00100000u
#define MEMW 4096
static u32 g_mem[MEMW];
static u32 memRead32(u32 addr) { u32 i = (addr - BASE) >> 2; return (i < MEMW) ? g_mem[i] : 0u; }

static struct { u32 code; } cpuRegs;

#define _Opcode_ (cpuRegs.code >> 26)
#define _Rs_     ((cpuRegs.code >> 21) & 0x1f)
#define _Rt_     ((cpuRegs.code >> 16) & 0x1f)
#define _Rd_     ((cpuRegs.code >> 11) & 0x1f)
#define _Funct_  (cpuRegs.code & 0x3f)

/* ---- verbatim: cop2flags (x86 ix86-32/iR5900-32.c) ---- */
int cop2flags(u32 code)
{
	if (code >> 26 != 022)
		return 0; // not COP2
	if ((code >> 25 & 1) == 0)
		return 0; // a branch or transfer instruction

	switch (code >> 2 & 15)
	{
		case 15:
			switch (code >> 6 & 0x1f)
			{
				case 4: // ITOF*
				case 5: // FTOI*
				case 12: // MOVE MR32
				case 13: // LQI SQI LQD SQD
				case 15: // MTIR MFIR ILWR ISWR
				case 16: // RNEXT RGET RINIT RXOR
					return 0;
				case 7: // MULAq, ABS, MULAi, CLIP
					if ((code & 3) == 1) // ABS
						return 0;
					if ((code & 3) == 3) // CLIP
						return 4;
					break;
				case 11: // SUBA, MSUBA, OPMULA, NOP
					if ((code & 3) == 3) // NOP
						return 0;
					break;
				case 14: // DIV, SQRT, RSQRT, WAITQ
					if ((code & 3) == 3) // WAITQ
						return 0;
					return 1; // but different timing, ugh
				default:
					break;
			}
			break;
		case 4: // MAXbc
		case 5: // MINbc
		case 12: // IADD, ISUB, IADDI
		case 13: // IAND, IOR
		case 14: // VCALLMS, VCALLMSR
			return 0;
		case 7:
			if ((code & 1) == 1) // MAXi, MINIi
				return 0;
			break;
		case 10:
			if ((code & 3) == 3) // MAX
				return 0;
			break;
		case 11:
			if ((code & 3) == 3) // MINI
				return 0;
			break;
		default:
			break;
	}
	return 3;
}

/* ---- verbatim: the x86 flag-hack pass (x86/iR5900Analysis.c) ---- */
void COP2FlagHackPass_Run(u32 start, u32 end, EEINST* inst_cache)
{
	int status_denormalized = 0;
	EEINST* last_status_write = NULL;
	EEINST* last_mac_write    = NULL;
	EEINST* last_clip_write   = NULL;
	u32 cfc2_pc               = start;

	EEINST* inst = inst_cache;
	for (u32 apc = start; apc < end; apc += 4, inst++)
	{
		cpuRegs.code = memRead32(apc);

		/* Catch SB/SH/SW to potential DMA->VIF0->VU0 exec.
		 * this is very unlikely in a cop2 chain. */
		if (_Opcode_ == 050 || _Opcode_ == 051 || _Opcode_ == 053)
		{
			if (last_status_write)
			{
				last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
				status_denormalized = 0;
			}
			if (last_mac_write)
				last_mac_write->info  |= EEINST_COP2_MAC_FLAG;
			if (last_clip_write)
				last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
		}
		else if (_Opcode_ == 022) /* COP2 ? */
		{
			/* Detect CTC2 Status, zero, ..., CFC2 v0, Status pattern where we need accurate sticky bits.
			 * Test case: Tekken Tag Tournament. */
			if (_Rs_ == 6 && _Rd_ == REG_STATUS_FLAG)
			{
				/* Read ahead, looking for CFC2. The scan loads each
				 * candidate into cpuRegs.code, which the _Opcode_/_Rs_/_Rd_
				 * macros below read, so restore this instruction's word
				 * before the tests that follow. */
				cfc2_pc = apc;
				for (u32 capc = apc; capc < end; capc += 4)
				{
					cpuRegs.code = memRead32(capc);
					if (_Opcode_ == 022 && _Rs_ == 2 && _Rd_ == REG_STATUS_FLAG)
					{
						cfc2_pc = capc;
						break;
					}
				}
				cpuRegs.code = memRead32(apc);
			}

			/* CFC2/CTC2 */
			if (_Rs_ == 6 || _Rs_ == 2)
			{
				switch (_Rd_)
				{
					case REG_STATUS_FLAG:
						if (last_status_write)
						{
							last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
							status_denormalized = 0;
						}
						break;
					case REG_MAC_FLAG:
						if (last_mac_write)
							last_mac_write->info  |= EEINST_COP2_MAC_FLAG;
						break;
					case REG_CLIP_FLAG:
						if (last_clip_write)
							last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
						break;
					case REG_FBRST:
						{
							/* only apply to CTC2, is FBRST readable? */
							if (_Rs_ == 2)
							{
								if (last_status_write)
								{
									last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
									status_denormalized = 0;
								}
								if (last_mac_write)
									last_mac_write->info  |= EEINST_COP2_MAC_FLAG;
								if (last_clip_write)
									last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
							}
						}
						break;
				}
			}

			/* VCALLMS, everything needs to be up to date */
			if (((cpuRegs.code >> 25 & 1) == 1) && ((cpuRegs.code >> 2 & 15) == 14))
			{
				if (last_status_write)
				{
					last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
					status_denormalized = 0;
				}
				if (last_mac_write)
					last_mac_write->info  |= EEINST_COP2_MAC_FLAG;
				if (last_clip_write)
					last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
			}

			/* 1 - status, 2 - mac, 3 - clip */
			{
				const int flags = cop2flags(cpuRegs.code);
				if (flags != 0)
				{
					/* STATUS */
					if (flags & 1)
					{
						if (!status_denormalized)
						{
							inst->info |= EEINST_COP2_DENORMALIZE_STATUS_FLAG;
							status_denormalized = 1;
						}

						/* If we're still behind the next CFC2 after the sticky bits got cleared,
						 * we need to update flags.
						 * Also do this if we're a vsqrt/vrsqrt/vdiv,
						 * these update status unconditionally. */
						{
							const u32 sub_opcode = (cpuRegs.code & 3) | ((cpuRegs.code >> 4) & 0x7c);
							if (apc < cfc2_pc || (_Rs_ >= 020 && _Funct_ >= 074
										&& sub_opcode >= 070 && sub_opcode <= 072))
								inst->info |= EEINST_COP2_STATUS_FLAG;
						}

						last_status_write = inst;
					}

					/* MAC */
					if (flags & 2)
						last_mac_write = inst;

					/* CLIP */
					if (flags & 4)
					{
						/* We don't track the clip flag yet..
						 * but it's unlikely that we'll have
						 * more than 4 clip flags in a row,
						 * because that would be pointless? */
						inst->info |= EEINST_COP2_CLIP_FLAG;
						last_clip_write = inst;
					}
				}
			}
		}
	}

	if (last_status_write)
	{
		last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
		status_denormalized      = 0;
	}
	if (last_mac_write)
		last_mac_write->info  |= EEINST_COP2_MAC_FLAG;
	if (last_clip_write)
		last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
}

/* COP2MicroFinishPass_Run is not part of this comparison (it decides
 * SYNC/FINISH/FLUSH, which the arm64 builder handles elsewhere) and is
 * omitted from the harness. */

/* ---- verbatim: the arm64 mode-0 classifier (arm64/aVU_Macro.inl) ----
 * The emitter function pointers are reduced to a boolean; the classification
 * logic -- which op words are native macro ALU ops -- is untouched. */
static bool cop2Mode0Emitter(u32 op)
{
	const u32 funct = op & 0x3f;
	if (funct >= 0x3c) // SPECIAL2 sub-table (x86: recCOP2_SPEC2)
	{
		switch ((op & 3) | ((op >> 4) & 0x7c))
		{
			// ADDA family (mode 0x110, M5.2 commit 1)
			case 0x00: return true;  // ADDAx
			case 0x01: return true;  // ADDAy
			case 0x02: return true;  // ADDAz
			case 0x03: return true;  // ADDAw
			case 0x22: return true;  // ADDAi
			case 0x28: return true;   // ADDA
			// SUBA family (mode 0x110, M5.2 commit 2)
			case 0x04: return true;  // SUBAx
			case 0x05: return true;  // SUBAy
			case 0x06: return true;  // SUBAz
			case 0x07: return true;  // SUBAw
			case 0x26: return true;  // SUBAi
			case 0x2c: return true;   // SUBA
			// MULA family (mode 0x110, M5.2 commit 3)
			case 0x18: return true;  // MULAx
			case 0x19: return true;  // MULAy
			case 0x1a: return true;  // MULAz
			case 0x1b: return true;  // MULAw
			case 0x1e: return true;  // MULAi
			case 0x2a: return true;   // MULA
			// MADDA family (mode 0x110, M5.2 commit 4)
			case 0x08: return true; // MADDAx
			case 0x09: return true; // MADDAy
			case 0x0a: return true; // MADDAz
			case 0x0b: return true; // MADDAw
			case 0x23: return true; // MADDAi
			case 0x29: return true;  // MADDA
			// MSUBA family (mode 0x110, M5.2 commit 5)
			case 0x0c: return true; // MSUBAx
			case 0x0d: return true; // MSUBAy
			case 0x0e: return true; // MSUBAz
			case 0x0f: return true; // MSUBAw
			case 0x27: return true; // MSUBAi
			case 0x2d: return true;  // MSUBA
			case 0x2e: return true; // OPMULA (mode 0x110, M5.2 commit 6)
			// *q ACC forms (mode 0x111, M5.3 commit 1)
			case 0x1c: return true;  // MULAq
			case 0x20: return true;  // ADDAq
			case 0x21: return true; // MADDAq
			case 0x24: return true;  // SUBAq
			case 0x25: return true; // MSUBAq
			// ITOF/FTOI/ABS/MOVE/MR32 (Mode-0, M5.1)
			case 0x10: return true;  // ITOF0
			case 0x11: return true;  // ITOF4
			case 0x12: return true; // ITOF12
			case 0x13: return true; // ITOF15
			case 0x14: return true;  // FTOI0
			case 0x15: return true;  // FTOI4
			case 0x16: return true; // FTOI12
			case 0x17: return true; // FTOI15
			case 0x1d: return true;    // ABS
			case 0x1f: return true;   // CLIP (mode 0x108, M5.4)
			case 0x30: return true;   // MOVE
			case 0x31: return true;   // MR32
			// VI load/store (M5.4; LQI/SQI/LQD/SQD mode 0x104/0x100/0x104/0x100,
			// ILWR 0x104, ISWR 0x100)
			case 0x34: return true;    // LQI
			case 0x35: return true;    // SQI
			case 0x36: return true;    // LQD
			case 0x37: return true;    // SQD
			case 0x3c: return true;   // MTIR (mode 0x104, M5.4)
			case 0x3d: return true;   // MFIR (mode 0x104, M5.4)
			case 0x3e: return true;   // ILWR
			case 0x3f: return true;   // ISWR
			// DIV/SQRT/RSQRT (mode 0x112, M5.3 commit 2)
			case 0x38: return true;    // DIV
			case 0x39: return true;   // SQRT
			case 0x3a: return true;  // RSQRT
			case 0x3b: return true;  // WAITQ (empty, M5.3 commit 3)
			case 0x2f: return true;    // NOP   (empty, M5.3 commit 3)
			// RNG (M5.4; RGET/RNEXT mode 0x104, RINIT/RXOR mode 0x100)
			case 0x40: return true;  // RNEXT
			case 0x41: return true;   // RGET
			case 0x42: return true;  // RINIT
			case 0x43: return true;   // RXOR
			default: return false;
		}
	}
	// SPECIAL1 ops (x86: recCOP2SPECIAL1t, dispatched by funct = op & 0x3f). The
	// flag-free MAX*/MINI* family is Mode-0 (M5.1); the ADD/SUB/MUL/MADD/MSUB/OPMSUB
	// families are flag ops (mode 0x110, M5.2); the *q forms are mode 0x111 (M5.3);
	// the VI integer ALU (IADD/ISUB/IADDI/IAND/IOR) is mode 0x104 (M5.4). CALLMS/CALLMSR
	// (funct 0x38/0x39) are deliberately NOT here (M5.5): x86 emits them via
	// INTERPRETATE_COP2_FUNC, not a native macro, so they fall through to nullptr and the
	// EE rec runs them on the inline interpreter (with a matching cycle commit). See
	// recCop2IsCallms in aR5900.cpp.
	switch (funct)
	{
		// ADD family (mode 0x110, M5.2 commit 1)
		case 0x00: return true;  // ADDx
		case 0x01: return true;  // ADDy
		case 0x02: return true;  // ADDz
		case 0x03: return true;  // ADDw
		case 0x22: return true;  // ADDi
		case 0x28: return true;   // ADD
		// SUB family (mode 0x110, M5.2 commit 2)
		case 0x04: return true;  // SUBx
		case 0x05: return true;  // SUBy
		case 0x06: return true;  // SUBz
		case 0x07: return true;  // SUBw
		case 0x26: return true;  // SUBi
		case 0x2c: return true;   // SUB
		// MUL family (mode 0x110, M5.2 commit 3)
		case 0x18: return true;  // MULx
		case 0x19: return true;  // MULy
		case 0x1a: return true;  // MULz
		case 0x1b: return true;  // MULw
		case 0x1e: return true;  // MULi
		case 0x2a: return true;   // MUL
		// MADD family (mode 0x110, M5.2 commit 4)
		case 0x08: return true; // MADDx
		case 0x09: return true; // MADDy
		case 0x0a: return true; // MADDz
		case 0x0b: return true; // MADDw
		case 0x23: return true; // MADDi
		case 0x29: return true;  // MADD
		// MSUB family (mode 0x110, M5.2 commit 5)
		case 0x0c: return true; // MSUBx
		case 0x0d: return true; // MSUBy
		case 0x0e: return true; // MSUBz
		case 0x0f: return true; // MSUBw
		case 0x27: return true; // MSUBi
		case 0x2d: return true;  // MSUB
		case 0x2e: return true; // OPMSUB (mode 0x110, M5.2 commit 6)
		// *q ALU forms (mode 0x111, M5.3 commit 1)
		case 0x1c: return true;  // MULq
		case 0x20: return true;  // ADDq
		case 0x21: return true; // MADDq
		case 0x24: return true;  // SUBq
		case 0x25: return true; // MSUBq
		// MAX/MINI family (Mode-0, M5.1)
		case 0x10: return true;  // MAXx
		case 0x11: return true;  // MAXy
		case 0x12: return true;  // MAXz
		case 0x13: return true;  // MAXw
		case 0x14: return true; // MINIx
		case 0x15: return true; // MINIy
		case 0x16: return true; // MINIz
		case 0x17: return true; // MINIw
		case 0x1d: return true;  // MAXi
		case 0x1f: return true; // MINIi
		case 0x2b: return true;   // MAX
		case 0x2f: return true;  // MINI
		// VI integer ALU (mode 0x104, M5.4)
		case 0x30: return true;  // IADD
		case 0x31: return true;  // ISUB
		case 0x32: return true; // IADDI
		case 0x34: return true;  // IAND
		case 0x35: return true;   // IOR
		default: return false;
	}
}

static bool recVUMacroIsMode0(u32 op) { return cop2Mode0Emitter(op); }

/* ---- verbatim: the arm64 run analysis (arm64/recR5900_arm64.cpp) ---- */
static bool InRam(u32 a) { return a >= BASE && a < BASE + MEMW * 4; }
static u32  Norm(u32 a)  { return a; }
	constexpr int kCop2RunMax = 64;
	EEINST s_cop2Run[kCop2RunMax];
	u32    s_cop2RunStart = 1, s_cop2RunEnd = 0; // empty
	int    s_emit_budget  = 1; // insns the current block may still emit (set by the builder)
	inline void Cop2RunInvalidate() { s_cop2RunStart = 1; s_cop2RunEnd = 0; }


	inline bool Cop2NativeMacroAlu(u32 insn)
	{
		if ((insn >> 26) != 0x12 || !((insn >> 25) & 1))
			return false;
		if (((insn >> 2) & 15) == 14)
			return false; // VCALLMS/VCALLMSR
		const u32 sub = (insn & 3) | ((insn >> 4) & 0x7c);
		if ((insn & 0x3f) >= 0x3c && sub == 0x1f)
			return false; // VCLIP (interp-pinned, C.30-4)
		// C.78: VNOP/VWAITQ have EMPTY emitters (recVNOP/recVWAITQ are {}), so
		// they never call setupMacroOp/endMacroOp. A run-hoisted bracket whose
		// LAST op is one of them never emits the x19 restore -- the rest of the
		// block then runs with x19 still pointing at vuRegs[0] (GT3's VU0 -> GS
		// upload loop ends `...VSQI, VSQI, VNOP, VNOP`: the following branch
		// read its GPRs from VF registers and stored pc into VI space -- instant
		// wedge). Keep them out of runs entirely; they still emit through the
		// per-op path (first = last = true), where no bracket means no restore
		// is needed.
		if ((insn & 0x3f) >= 0x3c && (sub == 0x2f || sub == 0x3b))
			return false; // VNOP/VWAITQ (bracket-less empty emitters)
		return recVUMacroIsMode0(insn);
	}

	void Cop2AnalyzeRun(u32 pc)
	{
		int n = 0;
		u32 p = pc;
		while (n < kCop2RunMax && n < s_emit_budget && Cop2NativeMacroAlu(memRead32(p)))
		{
			s_cop2Run[n].info = EEINST_COP2_SYNC_VU0 | EEINST_COP2_FINISH_VU0 |
			                    EEINST_COP2_FLUSH_VU0_REGISTERS;
			n++; p += 4;
		}
		s_cop2RunStart = pc;
		s_cop2RunEnd   = p;
		if (!n)
			return;

		// Tekken pattern (x86 m_cfc2_pc): CTC2 to REG_STATUS right before the
		// run, or a CFC2 from REG_STATUS right after it -> keep every status
		// write live so the sticky bits accumulate accurately. The peeks stay
		// inside RAM: one word past either end of a block could leave the
		// mapped range, and a compile-time vtlb miss is not an option.
		const auto is_status_move = [](u32 i, u32 rs) {
			return (i >> 26) == 0x12 && ((i >> 21) & 31) == rs && ((i >> 11) & 31) == REG_STATUS_FLAG;
		};

		// x86's COP2FlagHackPass tracks a HORIZON, not adjacency: a CTC2 to
		// REG_STATUS starts it and it runs to the matching CFC2 from
		// REG_STATUS, so every status write in between stays live even when
		// other instructions (a store, a NOP, an interp-pinned VU op) split
		// the macro ops into several runs. Peeking only one word either side
		// of the run misses that, and the sticky bits the CFC2 reads never
		// accumulate. Scan instead, bounded, and stop at whichever comes
		// first. The scans stay inside mapped RAM: a compile-time vtlb miss
		// is not an option.
		const int kHorizonScan = 64;
		bool all_status = false;
		{
			int k;
			for (k = 1; k <= kHorizonScan; k++)   // backwards: CTC2 Status?
			{
				const u32 a = Norm(pc) - (u32)k * 4;
				u32 w;
				if (!InRam(a))
					break;
				w = memRead32(pc - (u32)k * 4);
				if (is_status_move(w, 2))         // an earlier CFC2 closed it
					break;
				if (is_status_move(w, 6)) { all_status = true; break; }
			}
			for (k = 0; !all_status && k < kHorizonScan; k++) // forwards: CFC2 Status?
			{
				const u32 a = Norm(p) + (u32)k * 4;
				u32 w;
				if (!InRam(a))
					break;
				w = memRead32(p + (u32)k * 4);
				if (is_status_move(w, 2)) { all_status = true; break; }
				if (is_status_move(w, 6))         // a later CTC2 reopens it
					break;
			}
		}

		bool denormalized = false;
		int last_status = -1, last_mac = -1;
		for (int i = 0; i < n; i++)
		{
			const u32 insn = memRead32(pc + i * 4);
			const int f = cop2flags(insn);
			if (f & 1)
			{
				if (!denormalized)
				{
					s_cop2Run[i].info |= EEINST_COP2_DENORMALIZE_STATUS_FLAG;
					denormalized = true;
				}
				// x86: DIV/SQRT/RSQRT update status unconditionally.
				const u32 sub = (insn & 3) | ((insn >> 4) & 0x7c);
				if (all_status || (((insn >> 21) & 31) >= 0x10 && (insn & 0x3f) >= 0x3c && sub >= 0x38 && sub <= 0x3a))
					s_cop2Run[i].info |= EEINST_COP2_STATUS_FLAG;
				last_status = i;
			}
			if (f & 2)
				last_mac = i;
			if (f & 4)
				s_cop2Run[i].info |= EEINST_COP2_CLIP_FLAG;
		}
		if (last_status >= 0)
			s_cop2Run[last_status].info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
		if (last_mac >= 0)
			s_cop2Run[last_mac].info |= EEINST_COP2_MAC_FLAG;
	}


/* ------------------------------------------------------------------ */
/*  Block generation                                                    */
/* ------------------------------------------------------------------ */

/* A pool of real COP2 macro ALU encodings plus the ops that bracket or
 * break a run. Words are built from the fields both analyses decode. */
static u32 mk_cop2_special(u32 funct, u32 fd, u32 fs, u32 ft)
{
	return (022u << 26) | (1u << 25) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}
static u32 mk_cop2_special2(u32 sub, u32 fd, u32 fs, u32 ft)
{
	/* sub = (op & 3) | ((op >> 4) & 0x7c) -> funct low 2 bits, and bits 6..10 */
	const u32 funct = 0x3cu | (sub & 3u);
	return (022u << 26) | (1u << 25) | (ft << 16) | (fs << 11) | (fd << 6) |
	       ((sub & 0x7cu) << 4) | funct;
}
static u32 mk_ctc2_status(void) { return (022u << 26) | (6u << 21) | (2u << 16) | (REG_STATUS_FLAG << 11); }
static u32 mk_cfc2_status(void) { return (022u << 26) | (2u << 21) | (2u << 16) | (REG_STATUS_FLAG << 11); }
static u32 mk_ctc2_fbrst(void)  { return (022u << 26) | (2u << 21) | (2u << 16) | (REG_FBRST << 11); }
static u32 mk_sw(void)          { return (053u << 26) | (1u << 21) | (2u << 16) | 0x10u; }
static u32 mk_nop(void)         { return 0u; }
static u32 mk_vcallms(void)     { return (022u << 26) | (1u << 25) | (14u << 2) | 0x3cu; }

static u32 rnd_state = 12345;
static u32 rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }

/* Representative macro ALU ops: MUL/ADD/SUB/MADD families (flags 3),
 * DIV/SQRT/RSQRT (status-unconditional), CLIP (flags 4), and the
 * flagless integer ops (IADD/IAND) that cop2flags returns 0 for. */
static u32 gen_macro_op(void)
{
	switch (rnd() % 9)
	{
		case 0: return mk_cop2_special(0x00, 1, 2, 3);           /* ADDx  */
		case 1: return mk_cop2_special(0x28, 1, 2, 3);           /* ADD   */
		case 2: return mk_cop2_special(0x18, 1, 2, 3);           /* MULq? */
		case 3: return mk_cop2_special(0x2a, 1, 2, 3);           /* MUL   */
		case 4: return mk_cop2_special2(0x38, 0, 2, 3);          /* DIV   */
		case 5: return mk_cop2_special2(0x39, 0, 2, 3);          /* SQRT  */
		case 6: return mk_cop2_special2(0x3a, 0, 2, 3);          /* RSQRT */
		/* VCLIP is deliberately not in the arm64 native set (interp-pinned),
		 * so it ends a run rather than living inside one; it belongs in the
		 * breaker pool, not here. */
		case 7: return mk_cop2_special(0x30, 1, 2, 3);           /* IADD  */
		default: return mk_cop2_special(0x08, 1, 2, 3);          /* MADDx */
	}
}

static u32 gen_breaker(void)
{
	switch (rnd() % 7)
	{
		case 6: return mk_cop2_special2(0x1f, 0, 2, 3);          /* VCLIP */
		case 0: return mk_ctc2_status();
		case 1: return mk_cfc2_status();
		case 2: return mk_ctc2_fbrst();
		case 3: return mk_sw();
		case 4: return mk_vcallms();
		default: return mk_nop();
	}
}

/* ------------------------------------------------------------------ */

static int failures = 0, compared = 0, runs_seen = 0;
static int div_policy = 0, div_other = 0, div_arm_superset = 0, div_x86_extra = 0;
static int cur_has_cfc2_after = 0, cur_prev_ctc2 = 0;

static void check_block(const u32* words, int n, const char* what)
{
	EEINST x86_inst[MEMW];
	int i;

	if (n <= 0 || n >= MEMW) return;
	memset(g_mem, 0, sizeof(g_mem));
	for (i = 0; i < n; i++) g_mem[i] = words[i];
	memset(x86_inst, 0, sizeof(EEINST) * (size_t)n);

	COP2FlagHackPass_Run(BASE, BASE + (u32)n * 4, x86_inst);

	/* For every position that starts a native macro ALU run, ask the arm64
	 * analysis and compare the flag-hack bits over the run. */
	for (i = 0; i < n; i++)
	{
		int len, k;
		if (!Cop2NativeMacroAlu(g_mem[i])) continue;
		if (i > 0 && Cop2NativeMacroAlu(g_mem[i - 1])) continue; /* run start only */

		{
			int q;
			cur_prev_ctc2 = (i > 0 && (g_mem[i-1] >> 26) == 022u &&
			                 ((g_mem[i-1] >> 21) & 31) == 6u &&
			                 ((g_mem[i-1] >> 11) & 31) == REG_STATUS_FLAG);
			cur_has_cfc2_after = 0;
			for (q = i; q < n; q++)
				if ((g_mem[q] >> 26) == 022u && ((g_mem[q] >> 21) & 31) == 2u &&
				    ((g_mem[q] >> 11) & 31) == REG_STATUS_FLAG)
				{ cur_has_cfc2_after = 1; break; }
		}
		s_emit_budget = n - i;
		Cop2RunInvalidate();
		Cop2AnalyzeRun(BASE + (u32)i * 4);
		len = (int)((s_cop2RunEnd - s_cop2RunStart) / 4);
		if (len <= 0) continue;
		runs_seen++;

		for (k = 0; k < len; k++)
		{
			const u32 a = x86_inst[i + k].info      & FLAGHACK_BITS;
			const u32 b = s_cop2Run[k].info         & FLAGHACK_BITS;
			compared++;
			if (a != b)
			{
				/* Known policy difference, not a bug in either side: when a
				 * run is preceded by CTC2-to-Status but NO CFC2-from-Status
				 * follows it, arm64 keeps every status write live (its
				 * bracket test accepts the leading CTC2 alone) while x86
				 * only does so up to its cfc2_pc horizon, which never
				 * advanced. arm64 is the conservative side: extra flag
				 * work, never less. */
				if ((a ^ b) == EEINST_COP2_STATUS_FLAG && (b & EEINST_COP2_STATUS_FLAG) &&
				    cur_prev_ctc2 && !cur_has_cfc2_after)
				{
					div_policy++;
					continue;
				}
				if ((a & ~b) == 0)
				{
					/* arm64 marks a superset: its denormalize/normalize
					 * bracket spans a RUN, x86's spans the whole BLOCK, so
					 * arm64 re-brackets at every run boundary. Extra flag
					 * work, never less -- safe by construction. */
					div_arm_superset++;
					continue;
				}
				div_x86_extra++;
				failures++;
				if (failures <= 8)
					fprintf(stderr,
						"DIVERGE %-18s run@%d len=%d idx=%d insn=%08x  x86=%04x arm64=%04x (xor %04x)\n",
						what, i, len, k, g_mem[i + k], a, b, a ^ b);
			}
		}
	}
}

/* The minimal case delta-debugged out of the random corpus: a CTC2-to-Status
 * and its matching CFC2-from-Status separated from the macro run by one
 * instruction on each side. x86 keeps every status write live across the
 * whole CTC2..CFC2 horizon; arm64 only inspects the instruction immediately
 * before the run and immediately after it, so it drops the MUL's status
 * contribution and the CFC2 reads sticky bits that never accumulated. */
static void known_case(void)
{
	u32 w[6];
	w[0] = mk_ctc2_status();
	w[1] = mk_nop();
	w[2] = mk_cop2_special(0x2a, 1, 2, 3); /* MUL   : flags 3 */
	w[3] = mk_cop2_special2(0x3a, 0, 2, 3); /* RSQRT: flags 1 */
	w[4] = mk_ctc2_status();
	w[5] = mk_cfc2_status();
	check_block(w, 6, "known-horizon-case");
}

int main(void)
{
	u32 w[64];
	int iter;

	known_case();

	/* 1. Pure runs of macro ALU ops, every length. */
	for (int len = 1; len <= 24; len++)
	{
		for (iter = 0; iter < 200; iter++)
		{
			for (int i = 0; i < len; i++) w[i] = gen_macro_op();
			check_block(w, len, "pure-run");
		}
	}

	/* 2. Runs bracketed by CTC2 Status / CFC2 Status (the Tekken pattern). */
	for (int len = 1; len <= 16; len++)
	{
		for (iter = 0; iter < 200; iter++)
		{
			int n = 0;
			w[n++] = mk_ctc2_status();
			for (int i = 0; i < len; i++) w[n++] = gen_macro_op();
			if (iter & 1) w[n++] = mk_cfc2_status();
			check_block(w, n, "ctc2-bracket");
		}
	}

	/* 3. Mixed blocks: runs separated by arbitrary breakers. */
	for (iter = 0; iter < 4000; iter++)
	{
		int n = 0;
		while (n < 40)
		{
			int rl = 1 + (int)(rnd() % 6);
			for (int i = 0; i < rl && n < 40; i++) w[n++] = gen_macro_op();
			if (n < 40) w[n++] = gen_breaker();
		}
		check_block(w, n, "mixed");
	}

	printf("runs analysed: %d, instructions compared: %d\n", runs_seen, compared);
	printf("known policy differences (arm64 conservative, CTC2 without a following CFC2): %d\n", div_policy);
	printf("arm64-superset differences (per-run vs per-block bracket): %d\n", div_arm_superset);
	printf("UNEXPLAINED (x86 marks a flag arm64 does not): %d\n", div_x86_extra);
	return failures ? 1 : 0;
}
