/* SPDX-FileCopyrightText: lrps2 arm64 port
 * SPDX-License-Identifier: LGPL-3.0+
 *
 * arm64 IOP (R3000A) recompiler -- Phase C.2b-4: branches + delay slots.
 *
 * Builds on the C.2b-1..3 code cache, dispatch, ALU and load/store translation.
 * Blocks now also translate MIPS branches/jumps natively, each ending the block:
 * the (always-executed) delay slot is emitted inline, the condition is evaluated
 * from the pre-delay-slot register values, and psxRegs.pc is set to the taken
 * target or the fall-through (bpc+8). iopEventTest() is called only on the taken
 * path, matching the interpreter's doBranch(). Link (JAL/JALR/B*AL) writes pc+8
 * before the delay slot. J (0x02) is native since C.38 except the module-import
 * stub form (delay slot `li $zero, fn`), which stays on the interpreter for the
 * HLE hook; any opcode not covered by EmitSimple/EmitBranch hands the rest of
 * the basic block to the interpreter.
 *
 * Self-modifying code is handled by granular per-4KiB-page, mirror-normalised
 * invalidation in recClearIOP (psxCpu->Clear is called on every IOP write). */

#include "R3000A.h"
#include <memmap.h>
#include "common/Pcsx2Defs.h"
#include "IopMem.h"
#include "IopHw.h"
#include "IopDma.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "arm64/a64emit.h"
#include "arm64/aBlocks.h"

extern void iopRunBasicBlock_arm64(void);

typedef void (*BlockFn)(void);

#define kCodeCacheSize (16 * 1024 * 1024)
#define kMaxInsns      64
#define kPageShift     12

static u8*    s_code     = NULL;
static size_t s_code_pos = 0;
static bool   s_ok       = false;

static INLINE u32 Norm(u32 a) { return a & 0x1fffffff; }

/* Direct-mapped block cache for the 2 MB IOP RAM (hot region) + asm block-linking
 * (block-to-block tail-chaining), mirroring the EE recompiler. ROM/scratchpad
 * fall back to the hash. */
#define kRamBytes 0x00200000u

/* C.41: chained blocks keep the 80-byte frame (and x19) live and enter the
 * successor's body directly; only the return to recExecuteBlock pops the
 * frame. Prologue size is fixed: Sub + 5*Stp + exactly-4-instruction x19
 * materialization. */
#define kChainEntryOffset ((1 + 5 + 4) * 4)
#define kRamWords (kRamBytes >> 2)
static BlockFn* s_lut = NULL;
static INLINE bool InRam(u32 np) { return np < kRamBytes; }
static INLINE void LutClearAll(void) { if (s_lut) memzero_pages(s_lut, (size_t)kRamWords * sizeof(BlockFn)); }

/* Word-granular "native code covers this RAM word" bitmap (64KB). Every IOP
 * RAM store lands in recClearIOP, so the common case (word with no compiled
 * code on it) must be a single bit test, not a hash lookup. Bits can go
 * stale after a block is erased; the slow path self-cleans them. */
static u8 s_covered[kRamWords >> 3];
static INLINE void CoverRange(u32 ns, u32 ne)
{
	u32 w;
	for (w = ns >> 2; w < ((ne + 3) >> 2); w++)
		s_covered[w >> 3] |= 1u << (w & 7);
}

/* Block index (aBlocks.h), sized here once so compiling allocates nothing.
 * end = Norm(one past the last byte covered by NATIVE code), plus the
 * breaker word for interpreter-tail blocks (C.70): the tail itself reads
 * live memory, but the DECISION to hand off was made from the breaker word's
 * compile-time value, so a write to it must also invalidate the block.
 * Running out of records or links flushes the cache like running out of
 * code space does. */
#define kMaxBlocks  32768
#define kHashSize   8192
#define kMaxLinks   (4 * kMaxBlocks)
#define kPages      (0x20000000u >> kPageShift)
#define kBlockPages 2 /* pages one block's range can span */

static a64_block     s_recs[kMaxBlocks];
static a64_page_link s_links[kMaxLinks];
static u32           s_hash[kHashSize];
static u32           s_page_head[kPages];
static a64_blocks    s_blocks = {
	s_recs, s_links, s_hash, s_page_head, NULL,
	kMaxBlocks, kMaxLinks, kHashSize - 1, kPages, 0
};

/* Index a compiled block on the pages of its range. */
static void BlockAdd(BlockFn fn, u32 pc, u32 end)
{
	a64_block* r = a64_blocks_get(&s_blocks, pc);
	r->fn  = fn;
	r->end = end;
	if (end > Norm(pc))
	{
		u32 pg;
		for (pg = Norm(pc) >> kPageShift; pg <= (end - 1) >> kPageShift; pg++)
			a64_blocks_list(&s_blocks, pg, pc);
	}
}

static bool EmitSelfTest(void)
{
	u8* page;
	/* Emit into a real code page and run it: the code cache's W^X
	 * switching works on this host. */
	const size_t kPage = 16384;
	int64_t r;
	page = (u8*)memjit_alloc(kPage);
	if (!page)
		return false;
	memjit_write_begin();
	{
		a64_emit masm_e;
		a64_emit* const masm = &masm_e;
		a64_init(masm, page, kPage);
		a64_add_imm(masm, A64_X(0), A64_X(0), 1);
		a64_ret(masm);
	}
	memjit_write_end();
	memsync(page, page + 8);
	r = ((int64_t (*)(int64_t))page)(41);
	memjit_free(page, kPage);
	return r == 42;
}

/* ---- C.32: block-local write-back GPR register cache (the C.27 pattern
 * from the EE rec, sized for the IOP). Guest GPRs are 32-bit, mirrored in
 * the callee-saved w22..w27 (x20/x21 stay reserved for the branch
 * condition/jr-target snapshots that live across the delay slot). Helper
 * C calls (iopMemRead/Write, iopEventTest) preserve x22..x27 (AAPCS64)
 * and never touch psxRegs.GPR, so there are no invalidation sites at all:
 * the only discipline is a write-back flush at every block exit (the
 * branch tail before StorePC/eventtest/epilogue -- the conditional form
 * computes pc with Csel and shares one exit, so there is no fork to
 * snapshot -- and the interpreter-handoff tail, whose callee does read
 * and write GPR memory). */
#define kIopCacheSlots 6 /* w22..w27 */
static INLINE a64r IopCacheW(int slot) { return A64_W(22 + slot); }
typedef struct IopRegCache
{
	int8_t host_of[32];
	int8_t guest_of[kIopCacheSlots];
	bool   dirty[kIopCacheSlots];
	u8 rr;
} IopRegCache;
static IopRegCache s_irc;
static void IrcReset(void)
{
	int i;
	for (i = 0; i < 32; i++) s_irc.host_of[i] = -1;
	for (i = 0; i < kIopCacheSlots; i++) { s_irc.guest_of[i] = -1; s_irc.dirty[i] = false; }
	s_irc.rr = 0;
}
static int IrcAlloc(a64_emit* m, a64r gpr, u32 g)
{
	const int s = s_irc.rr;
	s_irc.rr = (u8)((s_irc.rr + 1) % kIopCacheSlots);
	if (s_irc.guest_of[s] >= 0)
	{
		if (s_irc.dirty[s]) a64_str(m, IopCacheW(s), a64_mem_off(gpr, (u32)s_irc.guest_of[s] * 4));
		s_irc.host_of[(int)s_irc.guest_of[s]] = -1;
		s_irc.dirty[s] = false;
	}
	s_irc.guest_of[s] = (int8_t)g;
	s_irc.host_of[g] = (int8_t)s;
	return s;
}
static void IrcFlushDirty(a64_emit* m, a64r gpr)
{
	int s;
	for (s = 0; s < kIopCacheSlots; s++)
		if (s_irc.guest_of[s] >= 0 && s_irc.dirty[s])
		{
			a64_str(m, IopCacheW(s), a64_mem_off(gpr, (u32)s_irc.guest_of[s] * 4));
			s_irc.dirty[s] = false;
		}
}
static INLINE void LoadGpr(a64_emit* m, a64r wd, a64r gpr, u32 idx)
{
	int s;
	if (idx == 0) { a64_mov_imm(m, wd, 0); return; }
	s = s_irc.host_of[idx];
	if (s >= 0) { a64_mov(m, wd, IopCacheW(s)); return; }
	a64_ldr(m, wd, a64_mem_off(gpr, idx * 4));
	s = IrcAlloc(m, gpr, idx);
	a64_mov(m, IopCacheW(s), wd);
}
static INLINE void StoreGpr(a64_emit* m, a64r ws, a64r gpr, u32 idx)
{
	int s;
	if (idx == 0)
	{
		a64_str(m, ws, a64_mem_off(gpr, idx * 4));
		return;
	}
	s = s_irc.host_of[idx];
	if (s < 0) s = IrcAlloc(m, gpr, idx);
	a64_mov(m, IopCacheW(s), ws);
	s_irc.dirty[s] = true;
}

/* C.68: ADDI/ADD/SUB (psxADDI/psxADD/psxSUB drop the integer-overflow trap
 * and are byte-for-byte the U forms -- same call as EE C.42) and RFE (the
 * Status bit shuffle of psxRFE, then iopTestIntc like rpsxRFE so an
 * interrupt pending behind IEc is raised at the next branch). Checked in
 * BOTH EmitSimple and IsTranslatable so block building and delay-slot
 * inlining stay consistent.
 * Translate one side-effect-light instruction (ALU + aligned load/store) to
 * native AArch64. Returns false (emitting nothing) for control flow and
 * anything not covered. gpr (x19) is callee-saved so it survives mem helpers. */
static bool EmitSimple(a64_emit* m, a64r gpr, u32 insn)
{
	u32 zimm;
	const u32 op = insn >> 26;
	const u32 rs = (insn >> 21) & 31, rt = (insn >> 16) & 31, rd = (insn >> 11) & 31, sa = (insn >> 6) & 31;
	const u32 funct = insn & 0x3f;
	const int32_t simm = (int16_t)(insn & 0xffff);
	zimm = insn & 0xffff;

	switch (op)
	{
		case 0x00:
			switch (funct)
			{
				case 0x00: if (rd) { LoadGpr(m, A64_W(0), gpr, rt); a64_lsl_imm(m, A64_W(0), A64_W(0), sa); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x02: if (rd) { LoadGpr(m, A64_W(0), gpr, rt); a64_lsr_imm(m, A64_W(0), A64_W(0), sa); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x03: if (rd) { LoadGpr(m, A64_W(0), gpr, rt); a64_asr_imm(m, A64_W(0), A64_W(0), sa); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x04: if (rd) { LoadGpr(m, A64_W(0), gpr, rt); LoadGpr(m, A64_W(1), gpr, rs); a64_lsl(m, A64_W(0), A64_W(0), A64_W(1)); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x06: if (rd) { LoadGpr(m, A64_W(0), gpr, rt); LoadGpr(m, A64_W(1), gpr, rs); a64_lsr(m, A64_W(0), A64_W(0), A64_W(1)); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x07: if (rd) { LoadGpr(m, A64_W(0), gpr, rt); LoadGpr(m, A64_W(1), gpr, rs); a64_asr(m, A64_W(0), A64_W(0), A64_W(1)); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x20: PCSX2_FALLTHROUGH; /* ADD == ADDU (C.68) */
				case 0x21: if (rd) { LoadGpr(m, A64_W(0), gpr, rs); LoadGpr(m, A64_W(1), gpr, rt); a64_add(m, A64_W(0), A64_W(0), A64_W(1)); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x22: PCSX2_FALLTHROUGH; /* SUB == SUBU (C.68) */
				case 0x23: if (rd) { LoadGpr(m, A64_W(0), gpr, rs); LoadGpr(m, A64_W(1), gpr, rt); a64_sub(m, A64_W(0), A64_W(0), A64_W(1)); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x24: if (rd) { LoadGpr(m, A64_W(0), gpr, rs); LoadGpr(m, A64_W(1), gpr, rt); a64_and(m, A64_W(0), A64_W(0), A64_W(1)); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x25: if (rd) { LoadGpr(m, A64_W(0), gpr, rs); LoadGpr(m, A64_W(1), gpr, rt); a64_orr(m, A64_W(0), A64_W(0), A64_W(1)); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x26: if (rd) { LoadGpr(m, A64_W(0), gpr, rs); LoadGpr(m, A64_W(1), gpr, rt); a64_eor(m, A64_W(0), A64_W(0), A64_W(1)); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x27: if (rd) { LoadGpr(m, A64_W(0), gpr, rs); LoadGpr(m, A64_W(1), gpr, rt); a64_orr(m, A64_W(0), A64_W(0), A64_W(1)); a64_mvn(m, A64_W(0), A64_W(0)); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x2a: if (rd) { LoadGpr(m, A64_W(0), gpr, rs); LoadGpr(m, A64_W(1), gpr, rt); a64_cmp(m, A64_W(0), A64_W(1)); a64_cset(m, A64_W(0), A64_LT); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				case 0x2b: if (rd) { LoadGpr(m, A64_W(0), gpr, rs); LoadGpr(m, A64_W(1), gpr, rt); a64_cmp(m, A64_W(0), A64_W(1)); a64_cset(m, A64_W(0), A64_LO); StoreGpr(m, A64_W(0), gpr, rd); } return true;
				/* ---- C.33: HI/LO pipeline. hi=GPR.r[32], lo=GPR.r[33]
				 * (offsets 128/132); accessed directly, never through the
				 * register cache (host_of[] only spans guest 0..31). */
				case 0x10: if (rd) { a64_ldr(m, A64_W(0), a64_mem_off(gpr, 32 * 4)); StoreGpr(m, A64_W(0), gpr, rd); } return true; /* MFHI */
				case 0x11: { LoadGpr(m, A64_W(0), gpr, rs); a64_str(m, A64_W(0), a64_mem_off(gpr, 32 * 4)); } return true; /* MTHI */
				case 0x12: if (rd) { a64_ldr(m, A64_W(0), a64_mem_off(gpr, 33 * 4)); StoreGpr(m, A64_W(0), gpr, rd); } return true; /* MFLO */
				case 0x13: { LoadGpr(m, A64_W(0), gpr, rs); a64_str(m, A64_W(0), a64_mem_off(gpr, 33 * 4)); } return true; /* MTLO */
				case 0x18: /* MULT */
				case 0x19: /* MULTU */
				{
					LoadGpr(m, A64_W(0), gpr, rs); LoadGpr(m, A64_W(1), gpr, rt);
					if (funct == 0x18) a64_smull(m, A64_X(0), A64_W(0), A64_W(1));
					else               a64_umull(m, A64_X(0), A64_W(0), A64_W(1));
					a64_str(m, A64_W(0), a64_mem_off(gpr, 33 * 4)); /* lo */
					a64_lsr_imm(m, A64_X(0), A64_X(0), 32);
					a64_str(m, A64_W(0), a64_mem_off(gpr, 32 * 4)); /* hi */
					return true;
				}
				case 0x1a: /* DIV (psxDIV semantics) */
				{
					LoadGpr(m, A64_W(0), gpr, rs); LoadGpr(m, A64_W(1), gpr, rt);
					a64_sdiv(m, A64_W(2), A64_W(0), A64_W(1)); /* rt==0 -> 0 (fixed below); INT_MIN/-1 -> INT_MIN (= psxDIV's x86-overflow lo) */
					a64_msub(m, A64_W(3), A64_W(2), A64_W(1), A64_W(0)); /* hi = rs - lo*rt; rt==0 -> rs, overflow -> 0: both match psxDIV */
					a64_cmp_imm(m, A64_W(0), 0);
					a64_mov_imm(m, A64_W(4), 1);
					a64_mov_imm(m, A64_W(5), -1);
					a64_csel(m, A64_W(4), A64_W(4), A64_W(5), A64_LT); /* div-by-0 lo: rs<0 ? 1 : 0xFFFFFFFF */
					a64_cmp_imm(m, A64_W(1), 0);
					a64_csel(m, A64_W(2), A64_W(2), A64_W(4), A64_NE);
					a64_str(m, A64_W(2), a64_mem_off(gpr, 33 * 4)); /* lo */
					a64_str(m, A64_W(3), a64_mem_off(gpr, 32 * 4)); /* hi */
					return true;
				}
				case 0x1b: /* DIVU (psxDIVU semantics) */
				{
					LoadGpr(m, A64_W(0), gpr, rs); LoadGpr(m, A64_W(1), gpr, rt);
					a64_udiv(m, A64_W(2), A64_W(0), A64_W(1));
					a64_msub(m, A64_W(3), A64_W(2), A64_W(1), A64_W(0)); /* rt==0 -> hi=rs */
					a64_cmp_imm(m, A64_W(1), 0);
					a64_csinv(m, A64_W(2), A64_W(2), A64_WZR, A64_NE); /* rt==0 -> lo=0xFFFFFFFF */
					a64_str(m, A64_W(2), a64_mem_off(gpr, 33 * 4)); /* lo */
					a64_str(m, A64_W(3), a64_mem_off(gpr, 32 * 4)); /* hi */
					return true;
				}
				default: return false;
			}
		case 0x08: PCSX2_FALLTHROUGH; /* ADDI == ADDIU (C.68) */
		case 0x09: if (rt) { LoadGpr(m, A64_W(0), gpr, rs); a64_add_imm(m, A64_W(0), A64_W(0), simm); StoreGpr(m, A64_W(0), gpr, rt); } return true;
		case 0x0a: if (rt) { LoadGpr(m, A64_W(0), gpr, rs); a64_mov_imm(m, A64_W(1), (u32)simm); a64_cmp(m, A64_W(0), A64_W(1)); a64_cset(m, A64_W(0), A64_LT); StoreGpr(m, A64_W(0), gpr, rt); } return true;
		case 0x0b: if (rt) { LoadGpr(m, A64_W(0), gpr, rs); a64_mov_imm(m, A64_W(1), (u32)simm); a64_cmp(m, A64_W(0), A64_W(1)); a64_cset(m, A64_W(0), A64_LO); StoreGpr(m, A64_W(0), gpr, rt); } return true;
		case 0x0c: if (rt) { LoadGpr(m, A64_W(0), gpr, rs); a64_mov_imm(m, A64_W(1), zimm); a64_and(m, A64_W(0), A64_W(0), A64_W(1)); StoreGpr(m, A64_W(0), gpr, rt); } return true;
		case 0x0d: if (rt) { LoadGpr(m, A64_W(0), gpr, rs); a64_mov_imm(m, A64_W(1), zimm); a64_orr(m, A64_W(0), A64_W(0), A64_W(1)); StoreGpr(m, A64_W(0), gpr, rt); } return true;
		case 0x0e: if (rt) { LoadGpr(m, A64_W(0), gpr, rs); a64_mov_imm(m, A64_W(1), zimm); a64_eor(m, A64_W(0), A64_W(0), A64_W(1)); StoreGpr(m, A64_W(0), gpr, rt); } return true;
		case 0x0f: if (rt) { a64_mov_imm(m, A64_W(0), zimm << 16); StoreGpr(m, A64_W(0), gpr, rt); } return true;

		case 0x20: case 0x24: case 0x21: case 0x25: case 0x23:
		{
			bool fast;
			uint64_t fn;
			a64_label slow = A64_LABEL_INIT, done = A64_LABEL_INIT;
			LoadGpr(m, A64_W(0), gpr, rs); a64_add_imm(m, A64_W(0), A64_W(0), simm);
			fn = (op == 0x23) ? ((uint64_t)(&iopMemRead32))
			            : (op == 0x21 || op == 0x25) ? ((uint64_t)(&iopMemRead16))
			            : ((uint64_t)(&iopMemRead8));
			/* C.36 fastmem: IOP main RAM is the first 8MB of physical
			 * space (2MB mirrored 4x into one contiguous host buffer),
			 * and iopMemRead* only does LUT bookkeeping to reach it — the
			 * top runtime hotspot after the C.35 event gate (iopMemRead32
			 * 6.8% self). Inline the RAM read and keep the helper call
			 * for everything else (HW/SIF/ROM/unmapped). iopMem->Main is
			 * stable for the process lifetime, so it embeds as a code
			 * constant. The read happens even for rt==0 (IO side effects
			 * on the slow path), matching the interpreter. */
			fast = iopMem != NULL;
			if (fast)
			{
				a64_and_imm(m, A64_W(1), A64_W(0), 0x1fffffff); /* physical address */
				a64_cmp_imm(m, A64_W(1), 0x00800000);
				a64_b_cond(m, &slow, A64_HS);
				a64_and_imm(m, A64_W(1), A64_W(1), 0x1fffff); /* 2MB mirror fold */
				a64_mov_imm(m, A64_X(2), ((uint64_t)(iopMem->Main)));
				switch (op)
				{
					case 0x20: a64_ldrsb(m, A64_W(0), a64_mem_reg(A64_X(2), A64_X(1), 0)); break;
					case 0x24: a64_ldrb(m, A64_W(0), a64_mem_reg(A64_X(2), A64_X(1), 0)); break;
					case 0x21: a64_ldrsh(m, A64_W(0), a64_mem_reg(A64_X(2), A64_X(1), 0)); break;
					case 0x25: a64_ldrh(m, A64_W(0), a64_mem_reg(A64_X(2), A64_X(1), 0)); break;
					default:   a64_ldr(m, A64_W(0), a64_mem_reg(A64_X(2), A64_X(1), 0)); break;
				}
				a64_b(m, &done);
				a64_bind(m, &slow);
			}
			a64_mov_imm(m, A64_X(16), fn); a64_blr(m, A64_X(16));
			switch (op) /* helpers return u8/u16; normalize like the fast path */
			{
				case 0x20: a64_sxtb(m, A64_W(0), A64_W(0)); break;
				case 0x24: a64_uxtb(m, A64_W(0), A64_W(0)); break;
				case 0x21: a64_sxth(m, A64_W(0), A64_W(0)); break;
				case 0x25: a64_uxth(m, A64_W(0), A64_W(0)); break;
				default: break;
			}
			if (fast)
				a64_bind(m, &done);
			if (rt)
				StoreGpr(m, A64_W(0), gpr, rt);
			return true;
		}
		case 0x28: case 0x29: case 0x2b:
		{
			uint64_t fn;
			LoadGpr(m, A64_W(0), gpr, rs); a64_add_imm(m, A64_W(0), A64_W(0), simm);
			LoadGpr(m, A64_W(1), gpr, rt);
#if defined(__APPLE__)
			/* iopMemWrite8/16 take u8/u16. Apple's arm64 ABI makes the caller
			 * extend narrow arguments to 32 bits and the callee relies on it,
			 * so pass the value already truncated. AAPCS64 elsewhere has the
			 * callee extend, so the plain register is correct there as is. */
			if (op == 0x28) a64_uxtb(m, A64_W(1), A64_W(1));
			else if (op == 0x29) a64_uxth(m, A64_W(1), A64_W(1));
#endif
			fn = (op == 0x2b) ? ((uint64_t)(&iopMemWrite32))
			            : (op == 0x29) ? ((uint64_t)(&iopMemWrite16))
			            : ((uint64_t)(&iopMemWrite8));
			a64_mov_imm(m, A64_X(16), fn); a64_blr(m, A64_X(16));
			return true;
		}

		/* ---- C.34: unaligned LWL/LWR (psxLWL/psxLWR semantics). The
		 * aligned-word read happens even for rt==0 (IO side effects),
		 * matching the interpreter. shift = (vaddr&3)*8 and old-rt are
		 * recomputed/reloaded AFTER the helper call (rs and the register
		 * cache survive it; rt isn't written until the very end, so the
		 * rs==rt aliasing case is safe). All shift amounts are <=24, so
		 * LSLV/LSRV mod-32 semantics never bite. */
		case 0x22: case 0x26:
		{
			LoadGpr(m, A64_W(0), gpr, rs); a64_add_imm(m, A64_W(0), A64_W(0), simm);
			a64_and_imm(m, A64_W(0), A64_W(0), 0xfffffffc);
			a64_mov_imm(m, A64_X(16), ((uint64_t)(&iopMemRead32))); a64_blr(m, A64_X(16));
			if (!rt) return true;
			LoadGpr(m, A64_W(1), gpr, rs); a64_add_imm(m, A64_W(1), A64_W(1), simm);
			a64_and_imm(m, A64_W(1), A64_W(1), 3); a64_lsl_imm(m, A64_W(1), A64_W(1), 3); /* shift */
			LoadGpr(m, A64_W(2), gpr, rt); /* old rt */
			if (op == 0x22) /* LWL: rt = (rt & (0x00ffffff >> shift)) | (mem << (24-shift)) */
			{
				a64_mov_imm(m, A64_W(3), 0x00ffffffu); a64_lsr(m, A64_W(3), A64_W(3), A64_W(1)); a64_and(m, A64_W(2), A64_W(2), A64_W(3));
				a64_mov_imm(m, A64_W(4), 24); a64_sub(m, A64_W(4), A64_W(4), A64_W(1)); a64_lsl(m, A64_W(0), A64_W(0), A64_W(4));
			}
			else /* LWR: rt = (rt & (0xffffff00 << (24-shift))) | (mem >> shift) */
			{
				a64_mov_imm(m, A64_W(3), 0xffffff00u); a64_mov_imm(m, A64_W(4), 24); a64_sub(m, A64_W(4), A64_W(4), A64_W(1));
				a64_lsl(m, A64_W(3), A64_W(3), A64_W(4)); a64_and(m, A64_W(2), A64_W(2), A64_W(3));
				a64_lsr(m, A64_W(0), A64_W(0), A64_W(1));
			}
			a64_orr(m, A64_W(0), A64_W(0), A64_W(2));
			StoreGpr(m, A64_W(0), gpr, rt);
			return true;
		}

		/* ---- C.34: unaligned SWL/SWR (read-modify-write of the aligned
		 * word, psxSWL/psxSWR semantics; two helper calls). */
		case 0x2a: case 0x2e:
		{
			LoadGpr(m, A64_W(0), gpr, rs); a64_add_imm(m, A64_W(0), A64_W(0), simm);
			a64_and_imm(m, A64_W(0), A64_W(0), 0xfffffffc);
			a64_mov_imm(m, A64_X(16), ((uint64_t)(&iopMemRead32))); a64_blr(m, A64_X(16));
			LoadGpr(m, A64_W(1), gpr, rs); a64_add_imm(m, A64_W(1), A64_W(1), simm);
			a64_and_imm(m, A64_W(1), A64_W(1), 3); a64_lsl_imm(m, A64_W(1), A64_W(1), 3); /* shift */
			LoadGpr(m, A64_W(2), gpr, rt);
			if (op == 0x2a) /* SWL: mem' = (rt >> (24-shift)) | (mem & (0xffffff00 << shift)) */
			{
				a64_mov_imm(m, A64_W(3), 24); a64_sub(m, A64_W(3), A64_W(3), A64_W(1)); a64_lsr(m, A64_W(2), A64_W(2), A64_W(3));
				a64_mov_imm(m, A64_W(4), 0xffffff00u); a64_lsl(m, A64_W(4), A64_W(4), A64_W(1)); a64_and(m, A64_W(0), A64_W(0), A64_W(4));
			}
			else /* SWR: mem' = (rt << shift) | (mem & (0x00ffffff >> (24-shift))) */
			{
				a64_lsl(m, A64_W(2), A64_W(2), A64_W(1));
				a64_mov_imm(m, A64_W(3), 24); a64_sub(m, A64_W(3), A64_W(3), A64_W(1));
				a64_mov_imm(m, A64_W(4), 0x00ffffffu); a64_lsr(m, A64_W(4), A64_W(4), A64_W(3)); a64_and(m, A64_W(0), A64_W(0), A64_W(4));
			}
			a64_orr(m, A64_W(1), A64_W(0), A64_W(2)); /* value */
			LoadGpr(m, A64_W(0), gpr, rs); a64_add_imm(m, A64_W(0), A64_W(0), simm);
			a64_and_imm(m, A64_W(0), A64_W(0), 0xfffffffc);
			a64_mov_imm(m, A64_X(16), ((uint64_t)(&iopMemWrite32))); a64_blr(m, A64_X(16));
			return true;
		}

		/* ---- C.34: COP0 moves. psxMFC0/CFC0/MTC0/CTC0 are plain copies
		 * against psxRegs.CP0.r[rd] (offset 136 + rd*4 from the GPR base;
		 * no side effects anywhere in this interpreter, not even for
		 * Status/Cause). RFE (rs=0x10) is handled below. */
		case 0x10:
		{
			const u32 rs_field = rs;
			if (rs_field == 0x00 || rs_field == 0x02) /* MFC0/CFC0 */
			{
				if (rt) { a64_ldr(m, A64_W(0), a64_mem_off(gpr, 136 + rd * 4)); StoreGpr(m, A64_W(0), gpr, rt); }
				return true;
			}
			if (rs_field == 0x04 || rs_field == 0x06) /* MTC0/CTC0 */
			{
				LoadGpr(m, A64_W(0), gpr, rt);
				a64_str(m, A64_W(0), a64_mem_off(gpr, 136 + rd * 4));
				return true;
			}
			/* C.68: RFE. psxCOP0 dispatches on the rs field alone (psxCP0[16]
			 * = psxRFE for any funct), so match the same way:
			 * Status = (Status & ~0xf) | ((Status & 0x3c) >> 2), Status = CP0.r[12]. */
			if (rs_field == 0x10)
			{
				a64_ldr(m, A64_W(0), a64_mem_off(gpr, 136 + 12 * 4));
				a64_and_imm(m, A64_W(1), A64_W(0), 0x3c);
				a64_and_imm(m, A64_W(0), A64_W(0), 0xfffffff0);
				a64_orr_shift(m, A64_W(0), A64_W(0), A64_W(1), A64_LSR, 2);
				a64_str(m, A64_W(0), a64_mem_off(gpr, 136 + 12 * 4));
				/* An interrupt left pending while IEc was clear is raised
				 * now, as rpsxRFE does: iopTestIntc pulls the next event
				 * test to within two cycles. */
				a64_mov_imm(m, A64_X(16), ((uint64_t)(&iopTestIntc)));
				a64_blr(m, A64_X(16));
				return true;
			}
			return false;
		}
		default: return false;
	}
}

/* True iff EmitSimple would translate this instruction (no emission). Used to
 * decide whether a branch's delay slot can be inlined. */
static bool IsTranslatable(u32 insn)
{
	const u32 op = insn >> 26, funct = insn & 0x3f;
	if (op == 0x00)
	{
		switch (funct)
		{
			case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:
			case 0x21: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
			case 0x2a: case 0x2b:
			case 0x10: case 0x11: case 0x12: case 0x13: /* C.33 HI/LO moves */
			case 0x18: case 0x19: case 0x1a: case 0x1b: /* C.33 mult/div */
				return true;
			case 0x20: case 0x22: return true; /* C.68 ADD/SUB (no trap) */
			default: return false;
		}
	}
	if (op == 0x10) /* C.34 COP0 moves + C.68 RFE */
	{
		const u32 rs_field = (insn >> 21) & 31;
		if (rs_field == 0x10)
			return true;
		return rs_field == 0x00 || rs_field == 0x02 || rs_field == 0x04 || rs_field == 0x06;
	}
	switch (op)
	{
		case 0x08: return true; /* C.68 ADDI (no trap) */
		case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
		case 0x20: case 0x21: case 0x23: case 0x24: case 0x25:
		case 0x28: case 0x29: case 0x2b:
		case 0x22: case 0x26: case 0x2a: case 0x2e: return true; /* C.34 LWL/LWR/SWL/SWR */
		default: return false;
	}
}

/* C.46: every psxRegisters field lives a short, constant distance from the
 * guest-reg base already pinned in x19, so reach them through it instead of
 * materializing a 64-bit absolute address (4 instructions) per access. The
 * block tail touches pc/cycle/iopCycleEE/iopNextEventCycle several times
 * each, so this is most of the tail's code size. Offsets are well inside the
 * ldr/str immediate window; if one ever isn't, the emitter falls back
 * to a scratch register on its own, so this stays correct by construction. */
static INLINE a64_mem RegsField(const void* field)
{
	const intptr_t off = ((intptr_t)(field)) -
	                     ((intptr_t)(&psxRegs.GPR.r[0]));
	return a64_mem_off(A64_X(19), ((int64_t)(off)));
}

/* cycle += count ; iopCycleEE -= (ICFG&8 ? 9 : 8) * count */
static void EmitCycleBookkeeping(a64_emit* m, int count)
{
	a64_ldr(m, A64_W(0), RegsField(&psxRegs.cycle)); a64_add_imm(m, A64_W(0), A64_W(0), count); a64_str(m, A64_W(0), RegsField(&psxRegs.cycle));
	a64_mov_imm(m, A64_X(10), ((uint64_t)(&psxHu32(HW_ICFG))));
	a64_ldr(m, A64_W(1), a64_mem_off(A64_X(10), 0));
	a64_mov_imm(m, A64_W(2), 9 * count); a64_mov_imm(m, A64_W(3), 8 * count);
	a64_tst_imm(m, A64_W(1), 8);
	a64_csel(m, A64_W(2), A64_W(2), A64_W(3), A64_NE);
	a64_ldr(m, A64_W(0), RegsField(&psxRegs.iopCycleEE)); a64_sub(m, A64_W(0), A64_W(0), A64_W(2)); a64_str(m, A64_W(0), RegsField(&psxRegs.iopCycleEE));
}

/* Restore the frame, then tail-chain straight to the next block via the RAM LUT
 * (staying in JIT code). IOP has a cycle budget, so only chain while
 * iopCycleEE > 0; otherwise (budget spent / LUT miss / non-RAM pc) return to
 * recExecuteBlock. x30 propagates so the final ret unwinds to the dispatcher. */
static void EmitEpilogue(a64_emit* m)
{
	/* C.41: dispatch FIRST with the frame (and x19) still live; the
	 * successor's chain entry skips its prologue. Only the return to
	 * recExecuteBlock pops the frame. */
	a64_label ret_path = A64_LABEL_INIT;
	a64_ldr(m, A64_W(0), RegsField(&psxRegs.iopCycleEE));
	a64_cmp_imm(m, A64_W(0), 0);
	a64_b_cond(m, &ret_path, A64_LE); /* budget exhausted -> return to dispatcher */
	a64_ldr(m, A64_W(1), RegsField(&psxRegs.pc));
	a64_and_imm(m, A64_W(2), A64_W(1), 0x1fffffff);
	a64_mov_imm(m, A64_W(3), kRamBytes);
	a64_cmp(m, A64_W(2), A64_W(3));
	a64_b_cond(m, &ret_path, A64_HS); /* not RAM -> return */
	a64_mov_imm(m, A64_X(0), ((uint64_t)(&s_lut)));
	a64_ldr(m, A64_X(0), a64_mem_off(A64_X(0), 0));
	a64_lsr_imm(m, A64_W(2), A64_W(2), 2);
	a64_ldr(m, A64_X(3), a64_mem_ext(A64_X(0), A64_W(2), A64_UXTW, 3));
	a64_cbz(m, A64_X(3), &ret_path);
	a64_add_imm(m, A64_X(3), A64_X(3), kChainEntryOffset); /* skip the successor's prologue */
	a64_br(m, A64_X(3));
	a64_bind(m, &ret_path);
	a64_ldp(m, A64_X(19), A64_X(20), a64_mem_off(A64_SP, 0));
	a64_ldp(m, A64_X(21), A64_X(30), a64_mem_off(A64_SP, 16));
	a64_ldp(m, A64_X(22), A64_X(23), a64_mem_off(A64_SP, 32));
	a64_ldp(m, A64_X(24), A64_X(25), a64_mem_off(A64_SP, 48));
	a64_ldp(m, A64_X(26), A64_X(27), a64_mem_off(A64_SP, 64));
	a64_add_imm(m, A64_SP, A64_SP, 80);
	a64_ret(m);
}

static INLINE void StorePC(a64_emit* m, a64r wsrc)
{
	a64_str(m, wsrc, RegsField(&psxRegs.pc));
}

/* C.37: gate the event test on cycle >= iopNextEventCycle, like the
 * upstream x86 IOP rec dispatcher (the interpreter tests on every
 * taken branch; mirroring that made iopEventTest 5.9% of CPU time).
 * The slice stays bounded by iopCycleEE, and the EE side's
 * _cpuEventTest_Shared still calls iopEventTest unconditionally. */
static void EmitIopEventTest(a64_emit* m)
{
	a64_label skip = A64_LABEL_INIT;
	a64_ldr(m, A64_W(0), RegsField(&psxRegs.cycle));
	a64_ldr(m, A64_W(1), RegsField(&psxRegs.iopNextEventCycle));
	a64_subs(m, A64_W(0), A64_W(0), A64_W(1)); /* (s32)(cycle - iopNextEventCycle) */
	a64_b_cond(m, &skip, A64_MI);     /* not due yet */
	a64_mov_imm(m, A64_X(16), (uint64_t)&iopEventTest);
	a64_blr(m, A64_X(16));
	a64_bind(m, &skip);
}

/* Translate a branch/jump (ending the block). Returns false, emitting nothing,
 * if it can't be handled (then the caller hands the rest to the interpreter). */
static bool EmitBranch(a64_emit* m, a64r gpr, u32 bpc, u32 insn, int n_leading)
{
	bool uncond = false, is_jr = false, two = false, one = false;
	int link;
	unsigned cond;
	u32 tconst;
	u32 ds;
	const u32 op = insn >> 26, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31, funct = insn & 0x3f;
	const int32_t simm = (int16_t)(insn & 0xffff);

	link = -1;
	cond = A64_AL;
	tconst = 0;

	if      (op == 0x00 && funct == 0x08) { uncond = true; is_jr = true; }
	else if (op == 0x00 && funct == 0x09) { uncond = true; is_jr = true; link = (int)((insn >> 11) & 31); }
	else if (op == 0x03) { uncond = true; link = 31; tconst = (((bpc + 4) & 0xf0000000) | ((insn & 0x3ffffff) << 2)); } /* JAL */
	else if (op == 0x02) { uncond = true; tconst = (((bpc + 4) & 0xf0000000) | ((insn & 0x3ffffff) << 2)); } /* J (C.38; import-stub form filtered below) */
	else if (op == 0x04) { two = true; cond = A64_EQ; tconst = bpc + 4 + simm * 4; } /* BEQ */
	else if (op == 0x05) { two = true; cond = A64_NE; tconst = bpc + 4 + simm * 4; } /* BNE */
	else if (op == 0x06) { one = true; cond = A64_LE; tconst = bpc + 4 + simm * 4; } /* BLEZ */
	else if (op == 0x07) { one = true; cond = A64_GT; tconst = bpc + 4 + simm * 4; } /* BGTZ */
	else if (op == 0x01)
	{
		const u32 t = bpc + 4 + simm * 4;
		if      (rt == 0x00) { one = true; cond = A64_LT; tconst = t; } /* BLTZ */
		else if (rt == 0x01) { one = true; cond = A64_GE; tconst = t; } /* BGEZ */
		else if (rt == 0x10) { one = true; cond = A64_LT; tconst = t; link = 31; } /* BLTZAL */
		else if (rt == 0x11) { one = true; cond = A64_GE; tconst = t; link = 31; } /* BGEZAL */
		else return false;
	}
	else return false;

	ds = iopMemRead32(bpc + 4);
	if (!IsTranslatable(ds))
		return false;

	/* C.38: J was pinned to the interpreter wholesale for the IOP
	 * module-import HLE, which dragged every J-heavy loop through the
	 * interpreter (~9% of CPU time in the handoff cluster). The hook only
	 * fires when the delay slot is the import-stub marker `li $zero, fn`
	 * (psxJ checks `delayslot >> 16 == 0x2400`), and the delay slot is
	 * compile-time known here — stores still go through iopMemWrite, so a
	 * stub patched in later faults the page and recompiles. Only the
	 * stub form stays on the interpreter. */
	if (op == 0x02 && (ds >> 16) == 0x2400)
		return false;

	/* C.47: the IOP kernel idle loop is a block that is nothing but `j <self>`
	 * with a nop delay slot, spinning until an interrupt arrives. It was the
	 * single hottest block in the emulator (18% of all JIT time on MMX7):
	 * every two guest cycles paid a full block tail plus a chain hop. The
	 * loop has no side effects, so instead of running it an iteration at a
	 * time, compute how many iterations would elapse before the next IOP
	 * event or the end of the cycle budget, whichever comes first, and apply
	 * them in one step. cycle and iopCycleEE land on exactly the values the
	 * spin would have produced when the event test next fires, so the
	 * emulated timing is unchanged -- this is not a speedhack. */
	if (op == 0x02 && n_leading == 0 && ds == 0 && tconst == bpc)
	{
		/* k = EE cycles charged per iteration = (ICFG & 8 ? 9 : 8) * 2 insns */
		a64_mov_imm(m, A64_X(10), ((uint64_t)(&psxHu32(HW_ICFG))));
		a64_ldr(m, A64_W(5), a64_mem_off(A64_X(10), 0));
		a64_mov_imm(m, A64_W(6), 9 * 2); a64_mov_imm(m, A64_W(7), 8 * 2);
		a64_tst_imm(m, A64_W(5), 8);
		a64_csel(m, A64_W(5), A64_W(6), A64_W(7), A64_NE); /* w5 = k */

		a64_ldr(m, A64_W(0), RegsField(&psxRegs.cycle)); /* w0 = cycle */
		a64_ldr(m, A64_W(1), RegsField(&psxRegs.iopNextEventCycle)); /* w1 = next event */
		a64_ldr(m, A64_W(4), RegsField(&psxRegs.iopCycleEE)); /* w4 = budget (> 0 on entry) */

		/* iterations until the event gate fires: ceil((s32)(next - cycle) / 2),
		 * which is <= 0 when the event is already due. */
		a64_sub(m, A64_W(6), A64_W(1), A64_W(0));
		a64_add_imm(m, A64_W(6), A64_W(6), 1);
		a64_asr_imm(m, A64_W(6), A64_W(6), 1);

		/* iterations until the budget is spent: ceil(budget / k). The epilogue
		 * stops chaining once iopCycleEE <= 0, so this is where the spin ends. */
		a64_add(m, A64_W(7), A64_W(4), A64_W(5));
		a64_sub_imm(m, A64_W(7), A64_W(7), 1);
		a64_udiv(m, A64_W(7), A64_W(7), A64_W(5));

		/* n = max(min(n_evt, n_bud), 1): always make at least one iteration of
		 * progress, so a due-but-not-yet-tested event cannot livelock the loop. */
		a64_cmp(m, A64_W(6), A64_W(7));
		a64_csel(m, A64_W(2), A64_W(6), A64_W(7), A64_LT);
		a64_mov_imm(m, A64_W(3), 1);
		a64_cmp_imm(m, A64_W(2), 1);
		a64_csel(m, A64_W(2), A64_W(2), A64_W(3), A64_GT);

		a64_add_shift(m, A64_W(0), A64_W(0), A64_W(2), A64_LSL, 1); /* cycle += 2n */
		a64_str(m, A64_W(0), RegsField(&psxRegs.cycle));
		a64_mul(m, A64_W(3), A64_W(2), A64_W(5)); /* k * n */
		a64_sub(m, A64_W(4), A64_W(4), A64_W(3)); /* iopCycleEE -= k*n */
		a64_str(m, A64_W(4), RegsField(&psxRegs.iopCycleEE));

		a64_mov_imm(m, A64_W(0), tconst);
		StorePC(m, A64_W(0));
		EmitIopEventTest(m);
		EmitEpilogue(m);
		return true;
	}

	/* JR/JALR target: read before the link is written -- `jalr rd, rs`
	 * with rd == rs jumps to the old rs (psxJALR) */
	if (is_jr) { LoadGpr(m, A64_W(21), gpr, rs); }
	/* link (before the delay slot, unconditionally); the BxxZAL condition
	 * below reads rs after it, as psxBGEZAL/psxBLTZAL do */
	if (link > 0) { a64_mov_imm(m, A64_W(0), bpc + 8); StoreGpr(m, A64_W(0), gpr, (u32)link); }
	/* snapshot condition operands before the delay slot can clobber them */
	if (two)   { LoadGpr(m, A64_W(20), gpr, rs); LoadGpr(m, A64_W(21), gpr, rt); }
	else if (one) { LoadGpr(m, A64_W(20), gpr, rs); }
	/* the always-executed delay slot */
	EmitSimple(m, gpr, ds);
	/* time: n_leading native ops + the branch + the delay slot */
	EmitCycleBookkeeping(m, n_leading + 2);
	/* Block exit: the chained successor reads GPR MEMORY; nothing after
	 * this point (Csel/StorePC/iopEventTest/epilogue) touches guest GPRs,
	 * and both conditional outcomes share this single exit path. */
	IrcFlushDirty(m, gpr);

	if (uncond)
	{
		if (is_jr) StorePC(m, A64_W(21));
		else { a64_mov_imm(m, A64_W(0), tconst); StorePC(m, A64_W(0)); }
		EmitIopEventTest(m);
	}
	else
	{
		a64_label not_taken = A64_LABEL_INIT;
		if (two) a64_cmp(m, A64_W(20), A64_W(21));
		else     a64_cmp_imm(m, A64_W(20), 0);
		a64_mov_imm(m, A64_W(0), tconst); a64_mov_imm(m, A64_W(1), bpc + 8);
		a64_csel(m, A64_W(0), A64_W(0), A64_W(1), cond);
		StorePC(m, A64_W(0));
		a64_b_cond(m, &not_taken, A64_INVERT(cond));
		EmitIopEventTest(m);
		a64_bind(m, &not_taken);
	}
	EmitEpilogue(m);
	return true;
}

static BlockFn CompileBlock(u32 pc)
{
	u8* start;
	a64_emit masm_e;
	a64_emit* masm;
	a64r gpr;
	u32 p;
	int n;
	bool done = false, branch_end = false;
	size_t sz;
	BlockFn fn;
	u32 ns, ne;
	u32 cov;
	if (s_code_pos + 8192 > kCodeCacheSize || !a64_blocks_room(&s_blocks, 0, kBlockPages))
	{
		a64_blocks_clear(&s_blocks);
		LutClearAll();
		memset(s_covered, 0, sizeof(s_covered));
		s_code_pos = 0;
	}

	start = s_code + s_code_pos;
	memjit_write_begin(); /* W^X: code cache writable on this thread until the block is emitted */
	masm = &masm_e;
	a64_init(masm, start, kCodeCacheSize - s_code_pos);
	IrcReset(); /* fresh per-block register-cache state (C.32) */

	gpr = A64_X(19);
	/* Uniform 80-byte frame across all blocks (tail-chaining: any block's
	 * epilogue must match any successor's prologue). x22..x27 = C.32 cache. */
	a64_sub_imm(masm, A64_SP, A64_SP, 80);
	a64_stp(masm, A64_X(19), A64_X(20), a64_mem_off(A64_SP, 0));
	a64_stp(masm, A64_X(21), A64_X(30), a64_mem_off(A64_SP, 16));
	a64_stp(masm, A64_X(22), A64_X(23), a64_mem_off(A64_SP, 32));
	a64_stp(masm, A64_X(24), A64_X(25), a64_mem_off(A64_SP, 48));
	a64_stp(masm, A64_X(26), A64_X(27), a64_mem_off(A64_SP, 64));
	{
		/* Exactly 4 instructions so kChainEntryOffset stays constant. */
		const uint64_t gv = (uint64_t)&psxRegs.GPR.r[0];
		a64_movz(masm, gpr, gv & 0xffff, -1);
		a64_movk(masm, gpr, (gv >> 16) & 0xffff, 16);
		a64_movk(masm, gpr, (gv >> 32) & 0xffff, 32);
		a64_movk(masm, gpr, (gv >> 48) & 0xffff, 48);
	}

	p = pc;
	n = 0;
	while (n < kMaxInsns)
	{
		const u32 insn = iopMemRead32(p);
		if (EmitSimple(masm, gpr, insn)) { p += 4; n++; continue; }
		if (EmitBranch(masm, gpr, p, insn, n)) { done = branch_end = true; break; } /* emits bookkeeping + pc + epilogue */
		break; /* unsupported -> interpreter handles the rest of the block */
	}

	/* C.69: block ended only because n hit kMaxInsns with every op translated
	 * and no branch yet (n == kMaxInsns <=> cap: a break always leaves
	 * n <= kMaxInsns-1 since the body only runs while n < kMaxInsns). Mirror
	 * the EE's C.44 cap-chain: end the block cleanly at p and chain, instead
	 * of handing a fully-translatable tail to the interpreter (LW/ADDIU
	 * sitting at position 64 were the residual IOP handoff breakers). */
	if (!done && n == kMaxInsns)
	{
		a64_mov_imm(masm, A64_W(0), p);
		StorePC(masm, A64_W(0));
		EmitCycleBookkeeping(masm, n);
		IrcFlushDirty(masm, gpr);
		EmitEpilogue(masm);
		done = true;
	}

	if (!done)
	{
		if (n > 0)
		{
			a64_ldr(masm, A64_W(0), RegsField(&psxRegs.pc)); a64_add_imm(masm, A64_W(0), A64_W(0), 4 * n); a64_str(masm, A64_W(0), RegsField(&psxRegs.pc));
			EmitCycleBookkeeping(masm, n);
		}
		/* The interpreter tail reads AND writes guest GPR memory. */
		IrcFlushDirty(masm, gpr);
		a64_mov_imm(masm, A64_X(16), ((uint64_t)(&iopRunBasicBlock_arm64)));
		a64_blr(masm, A64_X(16));
		IrcReset();
		EmitEpilogue(masm);
	}

	memjit_write_end();
	sz = a64_offset(masm);
	memsync(((void*)(start)), (u8*)(((void*)(start))) + (sz));
	s_code_pos += (sz + 15) & ~(size_t)15;

	fn = ((BlockFn)(start));
	/* Native code covers the leading run; a translated branch also bakes the
	 * branch + delay slot (p still points at the branch instruction there).
	 * A cap-chained block (C.69) covers exactly [pc, p) -- no branch baked. */
	ns = Norm(pc);
	ne = Norm(branch_end ? p + 8 : p);
	/* C.70: an interpreter-tail block's shape is decided by what the breaker
	 * word held at COMPILE time. If the guest later rewrites that word (IRX
	 * module load landing over what was data), the block must recompile to
	 * pick up the now-translatable code -- so cover the breaker word too.
	 * (MMX7: a block at 0x16480 compiled before the module loaded kept
	 * re-handing `lw ra` to the interpreter forever.) */
	cov = (!done && InRam(ne)) ? ne + 4 : ne;

	BlockAdd(fn, pc, cov);
	if (InRam(ns)) s_lut[ns >> 2] = fn;
	if (cov > ns && InRam(cov - 1))
		CoverRange(ns, cov);
	return fn;
}

static INLINE BlockFn BlockForPC(u32 pc)
{
	const u32 np = Norm(pc);
	if (InRam(np))
	{
		BlockFn f = s_lut[np >> 2];
		return f ? f : CompileBlock(pc);
	}
	{
		const a64_block* r = a64_blocks_find(&s_blocks, pc);
		return r ? r->fn : CompileBlock(pc);
	}
}

static void recReserve(void)
{
	s_ok = EmitSelfTest();
	if (!s_code)
	{
		s_code = (u8*)memjit_alloc(kCodeCacheSize);
	}
	if (!s_lut)
	{
		s_lut = (BlockFn*)mmap(NULL, (size_t)kRamWords * sizeof(BlockFn), PROT_READ | PROT_WRITE,
		                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (s_lut == MAP_FAILED) s_lut = NULL;
	}
	s_ok = s_ok && s_code && s_lut;
	log_cb(RETRO_LOG_INFO, "arm64 IOP rec (C.2b-4): %s.\n", s_ok ? "native ALU+mem+branch JIT active" : "FAILED -> interpreter fallback");
}

static void recResetIOP(void)
{
	a64_blocks_clear(&s_blocks);
	LutClearAll();
	memset(s_covered, 0, sizeof(s_covered));
	s_code_pos = 0;
}

/* Erase only the blocks whose NATIVE range [Norm(pc), end) intersects the
 * (inclusive) byte range [a0, a1]. Page lists keep the survivors; entries for
 * blocks already erased through a neighbouring page are dropped as they are met. */
static void EraseBlocksInRange(u32 a0, u32 a1)
{
	u32 pg;
	for (pg = a0 >> kPageShift; pg <= a1 >> kPageShift; pg++)
	{
		u32* link = &s_page_head[pg];
		while (*link)
		{
			const u32 spc = s_links[*link - 1].pc;
			const a64_block* r = a64_blocks_find(&s_blocks, spc);
			const u32 ns = Norm(spc);
			if (r && ns <= a1 && r->end > a0)
			{
				a64_blocks_erase(&s_blocks, spc);
				if (InRam(ns)) s_lut[ns >> 2] = NULL;
				r = NULL;
			}
			if (!r)
				a64_blocks_unlink(&s_blocks, link); /* erased, here or through another page */
			else
				link = &s_links[*link - 1].next;
		}
	}
}

static void recClearIOP(u32 addr, u32 size)
{
	const u32 a0 = Norm(addr);
	const u32 a1 = Norm(addr + (size ? size : 1) * 4 - 1);
	if (a0 <= a1 && a1 < kRamBytes)
	{
		u32 w;
		/* RAM fast path (every IOP store lands here, typically 1 word with no
		 * code on it). Ranges are whole words: callers pass addr&~3 + word
		 * counts, and block bounds are 4-aligned, so word-level tests match
		 * the byte-level intersection exactly. */
		bool covered = false;
		for (w = a0 >> 2; w <= (a1 >> 2) && !covered; w++)
			covered = (s_covered[w >> 3] >> (w & 7)) & 1;
		if (!covered)
			return;
		EraseBlocksInRange(a0, a1);
		/* Nothing overlaps [a0, a1] anymore -- clear the bits so the next
		 * store to these words takes the fast path again (self-cleans bits
		 * left stale by blocks erased through other words). */
		for (w = a0 >> 2; w <= (a1 >> 2); w++)
			s_covered[w >> 3] &= ~(1u << (w & 7));
		return;
	}
	if (!s_blocks.nrecs)
		return;
	EraseBlocksInRange(a0, a1);
}

static s32 recExecuteBlock(s32 eeCycles)
{
	/* If the recompiler failed to initialize, run the IOP interpreter. */
	if (!s_ok)
		return psxInt.ExecuteBlock(eeCycles);

	psxRegs.iopBreak   = 0;
	psxRegs.iopCycleEE = eeCycles;
	while (psxRegs.iopCycleEE > 0)
		BlockForPC(psxRegs.pc)();
	return psxRegs.iopBreak + psxRegs.iopCycleEE;
}

static void recShutdown(void)
{
	a64_blocks_clear(&s_blocks);
	memset(s_covered, 0, sizeof(s_covered));
	if (s_code) { memjit_free(s_code, kCodeCacheSize); s_code = NULL; }
	s_code_pos = 0;
}

R3000Acpu psxRec = {
	recReserve,
	recResetIOP,
	recExecuteBlock,
	recClearIOP,
	recShutdown,
};
