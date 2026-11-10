// SPDX-FileCopyrightText: lrps2 arm64 port
// SPDX-License-Identifier: LGPL-3.0+
//
// arm64 EE (R5900) recompiler -- Phase C.3-2: native integer ALU translation.
//
// Builds on the C.3-1 code cache + dispatch. Each block now natively translates
// the leading run of simple integer ALU instructions to AArch64 via VIXL, then
// hands the rest of the basic block to the interpreter (eeRunBasicBlock_arm64),
// which keeps branch-delay-slot, memory, FPU/MMI/VU and exception semantics
// exact. The EE GPRs are 128-bit; integer ops touch only the low 64 bits
// (GPR.r[i].UD[0] at byte offset i*16). 32-bit MIPS ops produce a sign-extended
// 64-bit result; the D* forms are full 64-bit. Cycle accounting mirrors execI
// (cpuBlockCycles += opcode.cycles * (2 - CP0.Config[18])), summed per block via
// OpCycles() with the real per-class costs (Default 9, Branch 11, Mult 16, Div
// 112). Self-modifying code -> granular per-page (mirror-normalised) invalidation,
// as Cpu->Clear is called on EE writes.

#include "common/ScanU32.h"
#include <memmap.h>
#include "common/Pcsx2Defs.h"
#include "common/Pcsx2Types.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "Memory.h"
#include "VU.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <deque>
#include <vector>
#include <algorithm>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/mman.h>


#include "aarch64/macro-assembler-aarch64.h"

// C.30-2: native COP2 macro emission -- the vendored aVU_Macro entry points
// (aVU.cpp TU) + the AsmHelpers thread-locals its emitters write through, and
// the analysis-flag plumbing they gate on.
#include "arm64/AsmHelpers.h"
#include "arm64/aR5900Analysis.h"
#include "VUmicro.h" // _vu0FinishMicro, _vu0WaitMicro, vu0Sync
#include "../Config.h"

bool recVUMacroIsMode0(u32 op);   // aVU_Macro.inl (classify COP2 SPECIAL ALU)
bool recVUMacroEmitMode0(u32 op); // aVU_Macro.inl (emit via microVU0 single-op emitters)
extern bool g_cop2MacroFirst;     // aVU_Macro.inl (C.78: run-hoisted macro bracket --
extern bool g_cop2MacroLast;      //   first op materializes x19/x27, last op restores x19)

using namespace vixl::aarch64;

#include "arm64/aR5900Glue.h" // the interpreter half: memory paths, loop hooks

/* Interpreter ops called directly from JIT blocks (C.18): ops with
 * contained side effects (no control flow, no exception, no pc or
 * cpuBlockCycles reads) run as a plain call with cpuRegs.code staged, so
 * the block continues natively instead of handing off its whole tail.
 * Declared in R5900OpcodeTables.h. */

namespace
{
	typedef void (*BlockFn)(void);

	constexpr size_t kCodeCacheSize = 32 * 1024 * 1024;
	constexpr int    kMaxInsns      = 64;
	constexpr u32    kPageShift     = 12;

	u8*    s_code     = nullptr;
	size_t s_code_pos = 0;
	bool   s_ok       = false;

	// Block record with a snapshot of the NATIVE-range source words. The EE's
	// fault-based SMC protection must drop every block on a written page (after
	// the fault unprotects it, further writes to the page don't fault, so any
	// surviving block would go stale silently). That page-granular clear made
	// code/data-sharing pages recompile their blocks on every data write
	// (~8-12% of GT3 attract time inside vixl emission). Instead of erasing,
	// Clear marks blocks dead; the next dispatch revalidates the snapshot
	// against live memory and, when the code bytes are unchanged (the common
	// case -- the write hit data), reinstalls the existing native code and
	// re-protects the page. Bit-identical: a block is only revived if its
	// baked-in source is byte-identical.
	struct BlockRec
	{
		BlockFn fn;
		u32 end;  // Norm(one past the native-covered range); == Norm(pc) if none
		bool live;
		bool manual; // C.60: validates its source at entry; its pages are unprotected
		std::vector<u32> src; // words [Norm(pc), end) at compile time
	};

	// ---- C.60: manual pages ----
	// A page holding code is write-protected so any store to it faults and
	// invalidates the blocks compiled from it. When a page holds code AND data
	// the game keeps writing, that turns into a ping-pong: fault -> unprotect +
	// drop blocks -> next entry revives them and RE-protects -> next store
	// faults again. On GT3 two pages (0x01000, 0x81000) did this ~23k times each
	// -- 46.7k of the run's 48k protect/clear pairs -- and each cycle costs a
	// SIGSEGV plus four mprotect calls (the RAM page and its alias in the 4 GB
	// fastmem area, protect and unprotect), which is where `mprotect` got to
	// 6.2% of the EE thread.
	//
	// So once a page has been cleared often enough to prove it is not really
	// static code, stop protecting it and pay for correctness at block entry
	// instead: blocks compiled from such a page re-check their source words
	// against live memory each time they are entered, and discard themselves
	// when it changed. (Upstream does the same thing, from the first clear;
	// a threshold keeps the ~240 pages that are cleared exactly once -- overlay
	// loads -- on the cheaper protected path, paying nothing per entry.)
	constexpr u32 kManualClears = 4;
	std::vector<u32> s_page_clears; // per RAM page, sized on reserve

	// C.63 A/B toggles (cached: these are read per compiled instruction).

	inline bool PageIsManual(u32 pg)
	{
		return pg < s_page_clears.size() && s_page_clears[pg] >= kManualClears;
	}
	// Any page the block's source spans having gone manual makes the block manual:
	// none of them are protected any more, so it must validate all of its source.
	inline bool RangeIsManual(u32 ns, u32 ne)
	{
		if (ne <= ns)
			return false;
		for (u32 pg = ns >> kPageShift; pg <= (ne - 1) >> kPageShift; pg++)
			if (PageIsManual(pg))
				return true;
		return false;
	}
	std::unordered_map<u32, BlockRec>         s_blocks;
	std::unordered_map<u32, std::vector<u32>> s_page;

	// Direct-mapped block cache for the 32 MB EE RAM (the hot region): O(1) array
	// index instead of a hash lookup per block. ROM/scratchpad fall back to the
	// hash. (Foundation for cheaper block-ending; full block-linking is future.)
	constexpr u32 kRamBytes = 0x02000000;
	constexpr u32 kRamWords = kRamBytes >> 2;
	BlockFn*      s_lut      = nullptr;

	// C.40: chained blocks keep the 96-byte frame (and x19/x28) live and jump
	// straight to the successor's body; only the return to the C++ dispatcher
	// pops the frame. The chain entry point skips the prologue, whose size is
	// therefore fixed: Sub + 5*Stp + Str + exactly-4-instruction x19
	// materialization + the 5-instruction x28 (vmap base) load.
	constexpr u32 kChainEntryOffset = (1 + 5 + 1 + 4 + 5) * 4;

	// C.49: the vtlb vmap array base, pinned for the lifetime of the frame.
	// vtlbdata.vmap is a pointer allocated once from the bump allocator
	// (vtlb_Core_Alloc) and only cleared at vtlb_Core_Free, i.e. it cannot move
	// while blocks are running -- so loading it once per block entry is as
	// correct as re-loading it per access, and every inline fastmem lookup
	// saves the 4-instruction address materialization plus the indirection.
	// x28 is free: the microVU allocator only tracks w0..w26 (mVU_GPR_TOTAL),
	// helper calls preserve it (AAPCS64 callee-saved), and the EE block cache
	// owns x21..x27.
	const Register vmapbase = x28;

	inline u32  Norm(u32 a)  { return a & 0x1fffffff; }
	inline bool InRam(u32 np) { return np < kRamBytes; }
	inline void LutClearAll() { if (s_lut) memzero_pages(s_lut, (size_t)kRamWords * sizeof(BlockFn)); }

	// C.46: the whole cpuRegisters struct sits within the ldr/str immediate
	// window of the guest-reg base already pinned in x19, so reach its fields
	// through it instead of materializing a 64-bit absolute address (4
	// instructions) per access. Block tails touch pc/cycle/nextEventCycle
	// repeatedly, so this is a large part of their code size. An offset outside
	// the window would only make the MacroAssembler fall back to a scratch
	// register, so this stays correct by construction.
	inline MemOperand RegsField(const void* field)
	{
		const intptr_t off = reinterpret_cast<intptr_t>(field) -
		                     reinterpret_cast<intptr_t>(&cpuRegs.GPR.r[0]);
		return MemOperand(x19, static_cast<int64_t>(off));
	}

	bool VixlEmitSelfTest()
	{
		// Emit into a real code page: VIXL's own buffer is malloc'd on Darwin
		// (VIXL_CODE_BUFFER_MALLOC), where SetExecutable() is a no-op.
		constexpr size_t kPage = 16384;
		u8* page = (u8*)memjit_alloc(kPage);
		if (!page)
			return false;
		memjit_write_begin();
		{
			MacroAssembler masm(page, kPage, PositionDependentCode);
			masm.Add(x0, x0, 1);
			masm.Ret();
			masm.FinalizeCode();
		}
		memjit_write_end();
		memsync(page, page + 8);
		const int64_t r = reinterpret_cast<int64_t (*)(int64_t)>(page)(41);
		memjit_free(page, kPage);
		return r == 42;
	}

	// Inline vtlb fast path (Phase C.12): with the guest address in w0 (upper
	// half of x0 zero, as left by 32-bit address arithmetic), emit the vmap
	// lookup and leave the HOST pointer in x10, branching to 'slow' when the
	// entry is a handler (hw register -> take the C wrapper call instead).
	// Mirrors vtlb_memRead/Write's direct case exactly: vmv = vmap[addr>>12];
	// handler iff (s64)(vmv.value + addr) < 0; host = vmv.value + addr.
	// x10/x11 are scratch; w0/x1/q0 (address/value) are preserved.
	inline bool InlineMemEnabled()
	{
		return true;
	}
	// The inline vmap path reads x28 as the VMAP base, so it is only legal when
	// fastmem is off (in fastmem mode x28 holds the fastmem base instead). An op
	// whose guest pc is blacklisted out of fastmem therefore takes the wrapper
	// call, not this path -- those are hardware-register accesses anyway, which
	// the vmap path would have sent to the wrapper too.
	inline bool InlineVmapEnabled();

	inline void EmitVmapLookup(MacroAssembler& m, Label* slow)
	{
		m.Lsr(w11, w0, 12);
		m.Ldr(x10, MemOperand(vmapbase, x11, LSL, 3)); // vmv.value
		m.Add(x10, x10, x0);                           // + guest addr
		m.Tbnz(x10, 63, slow);                         // sign bit = handler
	}

	// ---- C.50: fastmem ----
	// The vtlb already maintains a 4 GB host area (vtlbdata.fastmem_base) in
	// which every memory-backed guest page is mapped at its own guest address --
	// it was built for the x86 rec and is kept up to date on arm64 too, we just
	// never used it. With its base pinned in x28, a load/store is ONE
	// instruction: ldr/str <data>, [x28, w<addr>, UXTW]. No vmap indirection, no
	// handler test (C.49 established that the two dependent loads, not the
	// address arithmetic, are what the old inline path costs).
	//
	// Pages that aren't memory-backed (hardware registers, unmapped vaddrs) are
	// absent from the area, so touching one SIGSEGVs. vtlb's PageFaultHandler
	// already routes such a fault here (eeFastmemFault_arm64): we rewrite the
	// faulting instruction into a branch to the slow stub emitted right next to
	// it at compile time, and remember the guest pc so later compiles of that op
	// skip fastmem. NOTHING is generated in the signal handler -- the stub
	// already exists, the patch is one store plus cache maintenance. (Stores
	// into write-protected RAM pages don't come here at all: the fault handler
	// takes its SMC branch first, exactly as it does for the old inline stores.)
	struct FmSite { uintptr_t code; uintptr_t stub; u32 pc; };
	static std::vector<FmSite> s_fm_sites;    // sorted by code address
	static std::vector<u32>    s_fm_faulting; // sorted guest pcs: never fastmem again

	// C.51: the stubs live OUT OF LINE, past the block's epilogue. The fast path
	// is then just the access itself -- no `b` over the stub, and no ~6 dead
	// instructions of wrapper call sitting in the middle of the hot instruction
	// stream. Sites of the block being emitted are collected here (absolute
	// addresses only exist after FinalizeCode) and their stubs are emitted, in
	// order, right before it.
	//   kind 0: args are already live (w0 = addr, x1 = store value)
	//   kind 1: 128-bit wrapper, needs x1 = &GPR[rt] first
	struct FmPending
	{
		size_t   site_off;   // the patchable access
		size_t   stub_off;   // filled in when the stub is emitted
		u32      pc;
		uint64_t fn;         // wrapper to call
		int      kind;
		u32      rt;
		Label*   resume;     // where the stub jumps back to (bound after the fast path)
	};
	static std::vector<FmPending> s_fm_pending;
	// Labels outlive their block's emission (VIXL Labels are neither copyable nor
	// movable, and a deque never relocates what it holds).
	static std::deque<Label> s_fm_labels;

	// Guest pc of the op being emitted (compilation is EE-thread-only, like s_rc).
	u32 s_emit_pc = 0;

	// Latched once: x28 holds the fastmem base in this mode, the vmap base
	// otherwise, and every block's prologue must agree with every other block's
	// (they inherit x28 from each other across chain hops).
	inline bool FastmemMode()
	{
		static int mode = -1;
		if (mode < 0)
			mode = (CHECK_FASTMEM && vtlbdata.fastmem_base != 0) ? 1 : 0;
		return mode != 0;
	}
	inline bool InlineVmapEnabled() { return InlineMemEnabled() && !FastmemMode(); }

	inline bool FastmemOk(u32 guest_pc)
	{
		return FastmemMode() &&
		       !std::binary_search(s_fm_faulting.begin(), s_fm_faulting.end(), guest_pc);
	}
	// Emits the patchable access as EXACTLY one instruction (ExactAssemblyScope
	// keeps VIXL from expanding it or dropping a literal pool in the middle).
	template <typename EmitAccess>
	inline void EmitFastmemAccess(MacroAssembler& m, EmitAccess emit_access)
	{
		const size_t site_off = m.GetBuffer()->GetCursorOffset();
		{
			vixl::ExactAssemblyScope scope(&m, kInstructionSize);
			emit_access();
		}
		s_fm_pending.push_back({site_off, 0, s_emit_pc, 0, 0, 0, nullptr});
	}
	// Closes the site: binds the resume point at the cursor (i.e. after the whole
	// fast path -- for LQ that includes the Str into the guest register, which a
	// patched access must skip because the 128-bit wrapper writes GPR[rt] itself)
	// and queues the out-of-line stub.
	inline void DeferFastmemStub(MacroAssembler& m, uint64_t fn, int kind, u32 rt)
	{
		s_fm_labels.emplace_back();
		Label* resume = &s_fm_labels.back();
		m.Bind(resume);
		FmPending& p = s_fm_pending.back();
		p.fn = fn; p.kind = kind; p.rt = rt; p.resume = resume;
	}
	// Emitted once per block, after the epilogue: nothing falls through into it.
	inline void EmitFastmemStubs(MacroAssembler& m)
	{
		for (FmPending& p : s_fm_pending)
		{
			p.stub_off = m.GetBuffer()->GetCursorOffset();
			if (p.kind == 1)
				m.Add(x1, x19, p.rt * 16); // x1 = &GPR[rt] (x19 = guest-reg base)
			m.Mov(x16, p.fn);
			m.Blr(x16);
			m.B(p.resume);
		}
	}

	// ---- C.27: block-local write-through GPR register cache ----
	// The low 64 bits of guest GPRs are mirrored in the callee-saved host regs
	// x21..x27 for the duration of a block: repeated reads become reg-reg Movs
	// instead of Ldrs. WRITE-THROUGH: every StoreGpr still writes cpuRegs.GPR
	// immediately, so memory stays authoritative at all times -- event tests,
	// misalign cancels (fastjmp restores callee-saved regs), interpreter
	// handoffs and the branch not-taken/taken fork need no flush logic at all.
	// The only hazard is a STALE CACHE after something writes GPR memory
	// without going through StoreGpr; those sites invalidate:
	//   MMI/PCPYH 128-bit results (rd), LQ (rt), the eeLWL/LWR/LDL/LDR helper
	//   calls (rt), and EmitInterpOpCall bodies (any reg -> invalidate all).
	// Helper C calls preserve x21..x27 (AAPCS64), so the cache survives them.
	// HI/LO live at byte offsets >= 512 and are never cached. Compilation is
	// EE-thread-only, so the compile-time state can be a plain static.
	constexpr int kCacheSlots = 7; // x21..x27
	inline Register CacheX(int slot) { return XRegister(21 + slot); }
	inline Register CacheW(int slot) { return WRegister(21 + slot); }
	struct RegCache
	{
		int8_t host_of[32];           // guest reg -> slot (-1 = uncached)
		int8_t guest_of[kCacheSlots]; // slot -> guest reg (-1 = free)
		bool   dirty[kCacheSlots];    // slot holds a value memory doesn't have yet
		u8 rr;                        // round-robin eviction cursor
		u8 pinned;                    // slots the CURRENT op holds Register handles to
		void Reset()
		{
			for (int i = 0; i < 32; i++) host_of[i] = -1;
			for (int i = 0; i < kCacheSlots; i++) { guest_of[i] = -1; dirty[i] = false; }
			rr = 0;
			pinned = 0;
		}
		void Invalidate(u32 g) // drop without write-back (memory was just overwritten)
		{
			if (g < 32 && host_of[g] >= 0)
			{
				const int s = host_of[g];
				guest_of[s] = -1; dirty[s] = false; host_of[g] = -1;
			}
		}
		// Claim a slot for guest reg g, write-back-evicting the round-robin
		// victim. Slots pinned by the current op (live Register handles from
		// SrcGpr/DstGpr) are skipped -- an op pins at most 3 of the 7 slots,
		// so the scan always terminates.
		int Alloc(MacroAssembler& m, const Register& gpr, u32 g)
		{
			int s;
			do
			{
				s = rr;
				rr = (u8)((rr + 1) % kCacheSlots);
			} while (pinned & (1u << s));
			if (guest_of[s] >= 0)
			{
				if (dirty[s]) m.Str(CacheX(s), MemOperand(gpr, (u32)guest_of[s] * 16));
				host_of[(int)guest_of[s]] = -1;
				dirty[s] = false;
			}
			guest_of[s] = (int8_t)g;
			host_of[g] = (int8_t)s;
			return s;
		}
		// Write one guest reg back to memory if dirty (mapping stays, now clean).
		void FlushReg(MacroAssembler& m, const Register& gpr, u32 g)
		{
			if (g >= 32 || host_of[g] < 0) return;
			const int s = host_of[g];
			if (!dirty[s]) return;
			m.Str(CacheX(s), MemOperand(gpr, g * 16));
			dirty[s] = false;
		}
		// Write all dirty regs back (mappings stay, now clean).
		void FlushDirty(MacroAssembler& m, const Register& gpr)
		{
			for (int s = 0; s < kCacheSlots; s++)
				if (guest_of[s] >= 0 && dirty[s])
				{
					m.Str(CacheX(s), MemOperand(gpr, (u32)guest_of[s] * 16));
					dirty[s] = false;
				}
		}
		// Emit write-backs WITHOUT touching compile-time state: for cold paths
		// (misalign -> eeCancelInstruction fastjmp) that leave the block while
		// the fall-through continuation keeps using the dirty state.
		void FlushDirtyColdPath(MacroAssembler& m, const Register& gpr)
		{
			for (int s = 0; s < kCacheSlots; s++)
				if (guest_of[s] >= 0 && dirty[s])
					m.Str(CacheX(s), MemOperand(gpr, (u32)guest_of[s] * 16));
		}
	};
	RegCache s_rc;

	// ---- C.53: out-of-line misalign cold paths ----
	// A misaligned EE load/store raises an exception: flush the dirty register
	// cache (the interpreter resumes from GPR memory) and fastjmp out through
	// eeCancelInstruction. That is ~14 instructions -- up to 7 cache stores plus
	// the 4-instruction call setup -- and it was emitted INLINE after the
	// alignment test, i.e. in the middle of the hot instruction stream, once per
	// memory op. It never executes in practice (the guest keeps its accesses
	// aligned), so park it past the epilogue with the fastmem stubs and leave
	// only `Tst; B.ne <stub>` in the stream. The cold path never returns, so it
	// needs no resume point -- but it does need the register-cache state as it
	// was AT THE ACCESS, so snapshot that (RegCache is a plain value).
	struct MaPending { Label* label; RegCache state; };
	static std::vector<MaPending> s_ma_pending;
	static std::deque<Label>      s_ma_labels;

	// C.54: per-block exit state -- the event-due stubs and the single return tail.
	struct ExitPending { Label* slow; uint64_t evt; };
	static std::vector<ExitPending> s_exit_pending;
	static std::deque<Label>        s_exit_labels;
	static Label*                   s_stale = nullptr; // C.60: manual block failed its check
	static u32                      s_stale_pc = 0;
	struct BlockRec;
	extern "C" u32 eeBlockValidate_arm64(BlockRec* r, u32 pc);
	static Label*                   s_blk_ret = nullptr;

	inline void EmitMisalignGuard(MacroAssembler& m, u32 amask)
	{
		if (!amask)
			return;
		s_ma_labels.emplace_back();
		Label* l = &s_ma_labels.back();
		m.Tst(w0, amask);
		m.B(l, ne);
		s_ma_pending.push_back({l, s_rc});
	}
	inline void EmitMisalignStubs(MacroAssembler& m, const Register& gpr)
	{
		for (MaPending& p : s_ma_pending)
		{
			m.Bind(p.label);
			p.state.FlushDirtyColdPath(m, gpr);
			m.Mov(x16, reinterpret_cast<uint64_t>(&eeCancelInstruction_arm64));
			m.Blr(x16); // fastjmps out of the block; never returns
		}
	}

	inline bool RegCacheOn()
	{
		return true;
	}

	// EE GPRs are 128-bit; integer ops use the low 64 bits at byte offset idx*16.
	// A cached guest reg reads as a reg-reg Mov; a miss loads from memory and
	// fills a slot (the extra Mov is far cheaper than the next avoided load).
	inline void LoadGpr(MacroAssembler& m, const Register& xd, const Register& gpr, u32 idx)
	{
		if (idx == 0) { m.Mov(xd, 0); return; }
		if (!RegCacheOn()) { m.Ldr(xd, MemOperand(gpr, idx * 16)); return; }
		int s = s_rc.host_of[idx];
		if (s >= 0) { m.Mov(xd, CacheX(s)); return; }
		m.Ldr(xd, MemOperand(gpr, idx * 16));
		s = s_rc.Alloc(m, gpr, idx);
		m.Mov(CacheX(s), xd);
	}

	// Dirty write-back (C.27-2): the store lands only in the cache slot; memory
	// is written at the flush points (block exits, helper calls that observe
	// GPR memory, dirty-slot eviction, and the cold misalign-cancel paths).
	inline void StoreGpr(MacroAssembler& m, const Register& xs, const Register& gpr, u32 idx)
	{
		if (!RegCacheOn() || idx == 0)
		{
			m.Str(xs, MemOperand(gpr, idx * 16));
			return;
		}
		int s = s_rc.host_of[idx];
		if (s < 0) s = s_rc.Alloc(m, gpr, idx);
		m.Mov(CacheX(s), xs);
		s_rc.dirty[s] = true;
	}

	// ---- C.27-3: direct-to-cache emission API ----
	// Converted emitters get Register handles straight into the cache slots,
	// eliminating the load-into-x0 / store-from-x0 copy dance entirely:
	//   const Register a = SrcGpr(m, gpr, rs, x0);
	//   const Register d = DstGpr(m, gpr, rd, x0);
	//   m.Add(d.W(), a.W(), ...); m.Sxtw(d, d.W());
	//   FinishDst(m, gpr, rd, d);
	// With the cache ON: Src returns the (loaded) slot register or xzr for r0,
	// Dst returns the target slot marked dirty, FinishDst is a no-op; both pin
	// their slot so a later Alloc in the same op can't evict a live handle
	// (pins are cleared at every op boundary). With the cache OFF the same
	// call sequence degenerates to exactly the pre-C.27 code: Src loads into
	// the passed scratch, Dst returns the scratch, FinishDst stores it.
	// RULES for converted emitters: acquire all Srcs before the Dst; the
	// instruction that writes Dst must be the last reader of the Srcs (Dst
	// may alias a Src when the cache is off, or when rd==rs/rt).
	inline Register SrcGpr(MacroAssembler& m, const Register& gpr, u32 idx, const Register& scratch)
	{
		// r0 materializes as Mov scratch,0 -- NOT xzr: register 31 in the Rn
		// position of immediate/extended ADD/SUB/CMP encodes SP, so handing
		// out xzr would be a silent footgun for converted emitters.
		if (idx == 0) { m.Mov(scratch, 0); return scratch; }
		if (!RegCacheOn())
		{
			m.Ldr(scratch, MemOperand(gpr, idx * 16));
			return scratch;
		}
		int s = s_rc.host_of[idx];
		if (s < 0)
		{
			s = s_rc.Alloc(m, gpr, idx);
			m.Ldr(CacheX(s), MemOperand(gpr, idx * 16));
		}
		s_rc.pinned |= (u8)(1u << s);
		return CacheX(s);
	}
	// idx must be nonzero (callers guard r0 like they always have).
	inline Register DstGpr(MacroAssembler& m, const Register& gpr, u32 idx, const Register& scratch)
	{
		if (!RegCacheOn())
			return scratch;
		int s = s_rc.host_of[idx];
		if (s < 0) s = s_rc.Alloc(m, gpr, idx); // no load: fully overwritten
		s_rc.dirty[s] = true;
		s_rc.pinned |= (u8)(1u << s);
		return CacheX(s);
	}
	inline void FinishDst(MacroAssembler& m, const Register& gpr, u32 idx, const Register& d)
	{
		if (!RegCacheOn())
			m.Str(d, MemOperand(gpr, idx * 16));
	}

	// COP1 (FPU) register sub-ops that are bit-exact and safe to translate
	// natively: the pure bit moves (MFC1/MTC1/MOV.S/NEG.S/ABS.S) plus, as of
	// C.23/C.24, the S-format arithmetic (native AArch64 FP ops with the EE's
	// fpuDouble()-clamp / checkOverflow/checkUnderflow / checkDivideByZero
	// semantics -- see EmitFpuBinaryS/EmitDivS/EmitSqrtS/EmitRsqrtS/
	// EmitMaxMinS). CVT, C.cond, MADD/MSUB/*A_S, BC1 and CFC1/CTC1 stay with
	// the interpreter (a later increment).
	//   rs=0x00 MFC1, rs=0x04 MTC1,
	//   rs=0x10 + funct {0x00 ADD.S,0x01 SUB.S,0x02 MUL.S,0x03 DIV.S,0x04 SQRT.S,
	//                     0x05 ABS.S,0x06 MOV.S,0x07 NEG.S,0x16 RSQRT.S,
	//                     0x18 ADDA.S,0x19 SUBA.S,0x1a MULA.S,
	//                     0x1c MADD.S,0x1d MSUB.S,0x1e MADDA.S,0x1f MSUBA.S,
	//                     0x28 MAX.S,0x29 MIN.S}
	bool Cop1RegSupported(u32 insn)
	{
		const u32 rs = (insn >> 21) & 31;
		if (rs == 0x00 || rs == 0x04) return true;
		if (rs == 0x02 || rs == 0x06) return true;    // CFC1/CTC1 (C.65)
		if (rs == 0x14) return (insn & 0x3f) == 0x20; // W format: CVT.S.W
		if (rs == 0x10)
		{
			const u32 funct = insn & 0x3f;
			/* Soft-float rec mode: the native S-format arithmetic mirrors the
			 * old fpuDouble interpreter, not the ps2float model, so those ops
			 * fall back to the interpreter, which is the model. The pure bit
			 * moves, the compares (equivalent integer ordering either way)
			 * and the conversions stay native. */
			if (CHECK_FPU_SOFT_REC &&
			    (funct <= 0x04 || funct == 0x16 || (funct >= 0x18 && funct <= 0x1a) ||
			     (funct >= 0x1c && funct <= 0x1f) || funct == 0x28 || funct == 0x29))
				return false;
			return funct == 0x00 || funct == 0x01 || funct == 0x02 || funct == 0x03 ||
			       funct == 0x04 || funct == 0x05 || funct == 0x06 || funct == 0x07 ||
			       funct == 0x16 || (funct >= 0x18 && funct <= 0x1a) ||
			       (funct >= 0x1c && funct <= 0x1f) || funct == 0x24 ||
			       funct == 0x28 || funct == 0x29 ||
			       funct == 0x30 || funct == 0x32 || funct == 0x34 || funct == 0x36;
		}
		return false;
	}

	// Is this a Cop1RegSupported() instruction specifically one of the S-format
	// arithmetic/compare/convert ops (everything except the plain bit moves).
	bool Cop1ArithSupported(u32 insn)
	{
		const u32 rs = (insn >> 21) & 31;
		if (rs == 0x14) return true; // CVT.S.W
		if (rs != 0x10) return false;
		const u32 funct = insn & 0x3f;
		return funct == 0x00 || funct == 0x01 || funct == 0x02 || funct == 0x03 ||
		       funct == 0x04 || funct == 0x16 || (funct >= 0x18 && funct <= 0x1a) ||
		       (funct >= 0x1c && funct <= 0x1f) || funct == 0x24 ||
		       funct == 0x28 || funct == 0x29 ||
		       funct == 0x30 || funct == 0x32 || funct == 0x34 || funct == 0x36;
	}

	// ---- C.23: native S-format ADD/SUB/MUL (bit-exact port of pcsx2/FPU.c) ----
	// The interpreter's ADD_S/SUB_S/MUL_S are, e.g.:
	//   _FdValf_ = fpuDouble(Fs) + fpuDouble(Ft);
	//   if (checkOverflow(_FdValUl_, O|SO)) return;
	//   checkUnderflow(_FdValUl_, U|SU);
	// where fpuDouble() clamps a raw single INPUT (denormal/zero -> signed zero;
	// inf/nan -> signed posFmax 0x7f7fffff; else passthrough) and the "+"/"-"/"*"
	// is a real IEEE single-precision op (fpuDouble returns a `float`, despite the
	// name). This is NOT the x86 recompiler's double-promotion "full" mode (see
	// armsx2-pcsx2's aR5900FPU.c emitToPS2FPUFullCore) -- that targets a
	// different ground truth (the x86 JIT) with extra magnitude-preserving range.
	// Our target is the plain interpreter, so the natural translation is: clamp
	// each operand's raw bits, do the arithmetic natively in AArch64 single
	// precision (Sn regs -- exactly matches host `float op float`, no guard-bit
	// masking trickery needed since we never promote to double), then clamp the
	// raw result bits the same way checkOverflow/checkUnderflow do. Verified safe
	// against host FPCR: EE blocks always run with FPCR left at its process
	// default (bitmask 0, no flush-to-zero) -- only VU1 briefly swaps FPCR
	// (RAII-restored), so denormal results here are never
	// silently flushed by the host before we can inspect them.

	// Clamp a raw single bit pattern per fpuDouble(). wtmp is scratch.
	inline void EmitFpuClampBits(MacroAssembler& m, const Register& wd, const Register& wtmp)
	{
		Label notZero, done;
		m.And(wtmp, wd, 0x7f800000);      // exponent field
		m.Cbnz(wtmp, &notZero);
		m.And(wd, wd, 0x80000000);        // exp==0 (zero/denormal) -> signed zero
		m.B(&done);
		m.Bind(&notZero);
		m.Cmp(wtmp, 0x7f800000);
		m.B(&done, ne);                   // normal finite -> untouched
		m.And(wtmp, wd, 0x80000000);
		m.Orr(wd, wtmp, 0x7f7fffff);      // exp==0xff -> signed posFmax
		m.Bind(&done);
	}

	// checkOverflow(wd, O|SO) then, only if no overflow, checkUnderflow(wd, U|SU) --
	// exact mirror of pcsx2/FPU.c for the ADD_S/SUB_S/MUL_S family. wd holds the
	// raw result bits (updated in place with the clamped value); xfbase must hold
	// &fpuRegs. Scratch: w2, w3.
	inline void EmitFpuOverflowUnderflow(MacroAssembler& m, const Register& wd, const Register& xfbase)
	{
		Label overflow, notDenormal, done;

		m.And(w2, wd, 0x7fffffff);
		m.Cmp(w2, 0x7f800000);
		m.B(&overflow, eq);

		// no overflow: checkOverflow's else-branch clears FPUflagO only.
		m.Ldr(w3, MemOperand(xfbase, 252));
		m.Bic(w3, w3, 0x8000);
		m.Str(w3, MemOperand(xfbase, 252));

		// checkUnderflow: exp==0 && mantissa!=0 (true denormal) -> flush + U|SU;
		// else clear U only (exact zero is NOT underflow).
		m.And(w2, wd, 0x7f800000);
		m.Cbnz(w2, &notDenormal);
		m.And(w2, wd, 0x7fffff);
		m.Cbz(w2, &notDenormal);
		m.And(wd, wd, 0x80000000);        // flush to signed zero
		m.Ldr(w3, MemOperand(xfbase, 252));
		m.Orr(w3, w3, 0x4000);
		m.Orr(w3, w3, 0x8);
		m.Str(w3, MemOperand(xfbase, 252));
		m.B(&done);

		m.Bind(&notDenormal);
		m.Ldr(w3, MemOperand(xfbase, 252));
		m.Bic(w3, w3, 0x4000);
		m.Str(w3, MemOperand(xfbase, 252));
		m.B(&done);

		m.Bind(&overflow);
		m.And(w3, wd, 0x80000000);
		m.Orr(wd, w3, 0x7f7fffff);        // signed posFmax
		m.Ldr(w2, MemOperand(xfbase, 252));
		m.Orr(w2, w2, 0x8000);
		m.Orr(w2, w2, 0x10);
		m.Str(w2, MemOperand(xfbase, 252));

		m.Bind(&done);
	}

	// The ACC accumulator lives right after fpr[32] (128 B) + fprc[32] (128 B).
	constexpr u32 kFpuAcc = 256;

	// op: 0 add, 1 sub, 2 mul (the low bits of both the ADD/SUB/MUL.S funct
	// 0x00/0x01/0x02 and the ADDA/SUBA/MULA.S funct 0x18/0x19/0x1a). x1 must
	// hold &fpuRegs (caller already loaded it -- see EmitCop1Reg). Result ->
	// fpuRegs byte offset dst_off (fd*4, or kFpuAcc for the A forms; the
	// interpreter's ADDA/SUBA/MULA are line-identical to ADD/SUB/MUL apart
	// from the destination, and none of them touch ACCflag -- that field is
	// x86-recompiler-internal).
	void EmitFpuBinaryS(MacroAssembler& m, const Register& xfbase, u32 op, u32 dst_off, u32 fs, u32 ft)
	{
		m.Ldr(w0, MemOperand(xfbase, fs * 4));
		EmitFpuClampBits(m, w0, w4);
		m.Fmov(s0, w0);
		m.Ldr(w0, MemOperand(xfbase, ft * 4));
		EmitFpuClampBits(m, w0, w4);
		m.Fmov(s1, w0);
		switch (op)
		{
			case 0:  m.Fadd(s0, s0, s1); break;
			case 1:  m.Fsub(s0, s0, s1); break;
			default: m.Fmul(s0, s0, s1); break; // 2
		}
		m.Fmov(w0, s0);
		EmitFpuOverflowUnderflow(m, w0, xfbase);
		m.Str(w0, MemOperand(xfbase, dst_off));
	}


	// Exact ADD.S/SUB.S (and the A forms): the proven double pipeline from
	// the x86 iFPUd path, expressed in GPRs with one exact double Fadd.
	// Mask the smaller-exponent operand's low (|d|-1) bits (drop it at
	// |d| >= 25), convert each operand to an extended-range double by
	// integer bit construction - the PS2 treats exponent 255 as ordinary
	// numbers, so cvt-style conversion is wrong for them - add exactly in
	// double (a sum of two 24-bit mantissas within 25 exponents needs at
	// most 49 bits, so the Fadd never rounds and FPCR cannot matter), and
	// chop-pack manually with the model's ranges: overflow above exponent
	// 255 clamps to the true MAX 0x7fffffff with O|SO, underflow below
	// exponent 1 flushes to signed zero with U|SU. Byte-exact against the
	// ps2float model over 4.4 million vectors in the C model of this
	// sequence; the single-precision path it replaces measured 38.5%
	// wrong, exponent-255 operands and guard-bit discard both.
	// Scratch: w0/w2-w7 (+ x aliases), d0/d1.
	// Core of the exact pipeline: operands already loaded, a in w0, b in w2.
	void EmitFpuAddSubExactCore(MacroAssembler& m, const Register& xfbase, bool issub, u32 dst_off)
	{
		Label mask_done, big_a, big_b, chk_neg;
		Label a_zero, a_done, b_zero, b_done;
		Label zero_res, of, uf, done;

		// signed exponent difference
		m.Ubfx(w3, w0, 23, 8);
		m.Ubfx(w4, w2, 23, 8);
		m.Sub(w3, w3, w4);

		m.Cmp(w3, 25);
		m.B(&big_a, ge);                  // d >= 25: b -> sign only
		m.Cmp(w3, 1);
		m.B(&chk_neg, lt);
		m.Sub(w4, w3, 1);                 // 1..24: b &= ~0 << (d-1)
		m.Mov(w5, 0xffffffff);
		m.Lsl(w5, w5, w4);
		m.And(w2, w2, w5);
		m.B(&mask_done);
		m.Bind(&chk_neg);
		m.Cbz(w3, &mask_done);            // d == 0: no mask
		m.Cmp(w3, -24);
		m.B(&big_b, lt);                  // d <= -25: a -> sign only
		m.Neg(w4, w3);                    // -24..-1: a &= ~0 << (-d-1)
		m.Sub(w4, w4, 1);
		m.Mov(w5, 0xffffffff);
		m.Lsl(w5, w5, w4);
		m.And(w0, w0, w5);
		m.B(&mask_done);
		m.Bind(&big_a);
		m.And(w2, w2, 0x80000000);
		m.B(&mask_done);
		m.Bind(&big_b);
		m.And(w0, w0, 0x80000000);
		m.Bind(&mask_done);

		// a -> d0 with PS2-normal semantics (denormal -> signed zero)
		m.Ubfx(w4, w0, 23, 8);
		m.Cbz(w4, &a_zero);
		m.Add(w4, w4, 896);
		m.Lsl(x4, x4, 52);
		m.And(w6, w0, 0x7fffff);
		m.Lsl(x6, x6, 29);
		m.Orr(x6, x6, x4);
		m.Lsr(w5, w0, 31);
		m.Lsl(x5, x5, 63);
		m.Orr(x6, x6, x5);
		m.B(&a_done);
		m.Bind(&a_zero);
		m.Lsr(w5, w0, 31);
		m.Lsl(x6, x5, 63);
		m.Bind(&a_done);
		m.Fmov(d0, x6);

		// b -> d1
		m.Ubfx(w4, w2, 23, 8);
		m.Cbz(w4, &b_zero);
		m.Add(w4, w4, 896);
		m.Lsl(x4, x4, 52);
		m.And(w6, w2, 0x7fffff);
		m.Lsl(x6, x6, 29);
		m.Orr(x6, x6, x4);
		m.Lsr(w5, w2, 31);
		m.Lsl(x5, x5, 63);
		m.Orr(x6, x6, x5);
		m.B(&b_done);
		m.Bind(&b_zero);
		m.Lsr(w5, w2, 31);
		m.Lsl(x6, x5, 63);
		m.Bind(&b_done);
		m.Fmov(d1, x6);

		if (issub)
			m.Fsub(d0, d0, d1);
		else
			m.Fadd(d0, d0, d1);

		// chop-pack to PS2 single
		m.Fmov(x6, d0);
		m.And(x7, x6, 0x7fffffffffffffffull);
		m.Lsr(x5, x6, 63);
		m.Lsl(w5, w5, 31);                // sign << 31
		m.Cbz(x7, &zero_res);             // exact zero
		m.Lsr(x4, x6, 52);
		m.And(w4, w4, 0x7ff);
		m.Sub(w4, w4, 896);               // PS2 exponent
		m.Cmp(w4, 255);
		m.B(&of, gt);
		m.Cmp(w4, 1);
		m.B(&uf, lt);
		m.Lsr(x6, x6, 29);
		m.And(w6, w6, 0x7fffff);
		m.Orr(w0, w5, w6);
		m.Orr(w0, w0, Operand(w4, LSL, 23));
		m.Ldr(w3, MemOperand(xfbase, 252)); // clear O, clear U
		m.Bic(w3, w3, 0x8000);
		m.Bic(w3, w3, 0x4000);
		m.Str(w3, MemOperand(xfbase, 252));
		m.B(&done);

		m.Bind(&zero_res);
		m.Mov(w0, w5);
		m.Ldr(w3, MemOperand(xfbase, 252));
		m.Bic(w3, w3, 0x8000);
		m.Bic(w3, w3, 0x4000);
		m.Str(w3, MemOperand(xfbase, 252));
		m.B(&done);

		m.Bind(&of);
		m.Orr(w0, w5, 0x7fffffff);        // true PS2 MAX, exponent 255
		m.Ldr(w3, MemOperand(xfbase, 252));
		m.Orr(w3, w3, 0x8000);
		m.Orr(w3, w3, 0x10);
		m.Str(w3, MemOperand(xfbase, 252));
		m.B(&done);

		m.Bind(&uf);
		m.Mov(w0, w5);
		m.Ldr(w3, MemOperand(xfbase, 252));
		m.Orr(w3, w3, 0x4000);
		m.Orr(w3, w3, 0x8);
		m.Str(w3, MemOperand(xfbase, 252));

		m.Bind(&done);
		m.Str(w0, MemOperand(xfbase, dst_off));
	}

	void EmitFpuAddSubExact(MacroAssembler& m, const Register& xfbase, bool issub, u32 dst_off, u32 fs, u32 ft)
	{
		m.Ldr(w0, MemOperand(xfbase, fs * 4)); // a
		m.Ldr(w2, MemOperand(xfbase, ft * 4)); // b
		EmitFpuAddSubExactCore(m, xfbase, issub, dst_off);
	}

	// funct 0x1c MADD.S / 0x1d MSUB.S: temp = fpuDouble(Fs)*fpuDouble(Ft);
	// fd = fpuDouble(ACC) +/- fpuDouble(temp.UL) -- BOTH the intermediate
	// product and ACC are re-clamped through fpuDouble before the add/sub
	// (pcsx2/FPU.c MADD_S/MSUB_S). Then the usual O|SO / U|SU result check.
	// There is NO fused multiply-add on the EE -- the product is a rounded
	// single, so native Fmul + Fadd (not Fmadd) is the exact translation.
	// The accumulate stage runs the exact double pipeline: ACC enters raw
	// (the core handles denormals and exponent-255 the way the model does,
	// where the old fpuDouble clamp destroyed 255-exponent accumulators),
	// the single-precision product enters fpuDouble-clamped as before. The
	// remaining divergence from the model is the product itself: the exact
	// multiply needs the Booth tree, and until then a product that
	// overflows clamps like the old interpreter instead of forcing the
	// mul-signed MAX the model's exception ladder produces.
	void EmitFpuMaddS(MacroAssembler& m, const Register& xfbase, u32 fd, u32 fs, u32 ft, bool sub)
	{
		m.Ldr(w0, MemOperand(xfbase, fs * 4));
		EmitFpuClampBits(m, w0, w4);
		m.Fmov(s0, w0);
		m.Ldr(w0, MemOperand(xfbase, ft * 4));
		EmitFpuClampBits(m, w0, w4);
		m.Fmov(s1, w0);
		m.Fmul(s0, s0, s1);
		m.Fmov(w2, s0);
		EmitFpuClampBits(m, w2, w4);   // fpuDouble(temp.UL)
		m.Ldr(w0, MemOperand(xfbase, kFpuAcc)); // raw ACC: the core handles it
		EmitFpuAddSubExactCore(m, xfbase, sub, fd * 4);
	}

	// funct 0x1e MADDA.S / 0x1f MSUBA.S. The old interpreter read raw ACC
	// and the raw IEEE product here, unlike MADD - that asymmetry died when
	// the interpreter moved to the soft model, where the A forms are the
	// same fused chain with the accumulator as destination. Same shape as
	// EmitFpuMaddS: clamped single product, raw ACC, exact-core accumulate.
	void EmitFpuMaddaS(MacroAssembler& m, const Register& xfbase, u32 fs, u32 ft, bool sub)
	{
		m.Ldr(w0, MemOperand(xfbase, fs * 4));
		EmitFpuClampBits(m, w0, w4);
		m.Fmov(s0, w0);
		m.Ldr(w0, MemOperand(xfbase, ft * 4));
		EmitFpuClampBits(m, w0, w4);
		m.Fmov(s1, w0);
		m.Fmul(s0, s0, s1);
		m.Fmov(w2, s0);
		EmitFpuClampBits(m, w2, w4);
		m.Ldr(w0, MemOperand(xfbase, kFpuAcc));
		EmitFpuAddSubExactCore(m, xfbase, sub, kFpuAcc);
	}

	// checkOverflow(wd,0) + checkUnderflow(wd,0): DIV.S/SQRT.S/RSQRT.S pass
	// cFlagsToSet=0 to both checks (pcsx2/FPU.c), so the raw result bits get
	// value-clamped exactly like EmitFpuOverflowUnderflow but FCR31 is NEVER
	// touched (neither set nor cleared) -- a real ground-truth quirk, not an
	// oversight, so no fbase param here.
	inline void EmitFpuClampOutValueOnly(MacroAssembler& m, const Register& wd)
	{
		Label overflow, done;
		m.And(w2, wd, 0x7fffffff);
		m.Cmp(w2, 0x7f800000);
		m.B(&overflow, eq);
		m.And(w2, wd, 0x7f800000);
		m.Cbnz(w2, &done);
		m.And(w2, wd, 0x7fffff);
		m.Cbz(w2, &done);
		m.And(wd, wd, 0x80000000);
		m.B(&done);
		m.Bind(&overflow);
		m.And(w3, wd, 0x80000000);
		m.Orr(wd, w3, 0x7f7fffff);
		m.Bind(&done);
	}

	// funct 0x03 DIV.S: checkDivideByZero(fd, Ft, Fs, D|SD, I|SI) first -- if Ft's
	// raw exponent field is 0 (zero/denormal): set D|SD, or I|SI instead if Fs's
	// exponent field is ALSO 0 (0/0); fd = sign(Ft^Fs) | posFmax; EARLY RETURN, no
	// overflow/underflow check at all. Otherwise fd = fpuDouble(Fs)/fpuDouble(Ft),
	// then checkOverflow(0)/checkUnderflow(0) (value-clamp only, no flags).
	void EmitDivS(MacroAssembler& m, const Register& xfbase, u32 fd, u32 fs, u32 ft)
	{
		Label divByZero, done;

		m.Ldr(w5, MemOperand(xfbase, ft * 4)); // raw Ft: divisor sign + zero-check
		m.And(w2, w5, 0x7f800000);
		m.Cbz(w2, &divByZero);

		m.Ldr(w0, MemOperand(xfbase, fs * 4));
		EmitFpuClampBits(m, w0, w4);
		m.Fmov(s0, w0);
		m.Mov(w0, w5);
		EmitFpuClampBits(m, w0, w4);
		m.Fmov(s1, w0);
		m.Fdiv(s0, s0, s1);
		m.Fmov(w0, s0);
		EmitFpuClampOutValueOnly(m, w0);
		m.Str(w0, MemOperand(xfbase, fd * 4));
		m.B(&done);

		m.Bind(&divByZero);
		{
			Label zeroFs, flagsDone;
			m.Ldr(w6, MemOperand(xfbase, fs * 4)); // raw Fs: 0/0 check + result sign
			m.Ldr(w3, MemOperand(xfbase, 252));
			m.And(w2, w6, 0x7f800000);
			m.Cbz(w2, &zeroFs);
			m.Orr(w3, w3, 0x10000); // D
			m.Orr(w3, w3, 0x20);    // SD
			m.B(&flagsDone);
			m.Bind(&zeroFs);
			m.Orr(w3, w3, 0x20000); // I
			m.Orr(w3, w3, 0x40);    // SI
			m.Bind(&flagsDone);
			m.Str(w3, MemOperand(xfbase, 252));
			m.Eor(w0, w5, w6);
			m.And(w0, w0, 0x80000000);
			m.Orr(w0, w0, 0x7f7fffff);
			m.Str(w0, MemOperand(xfbase, fd * 4));
		}

		m.Bind(&done);
	}

	// funct 0x04 SQRT.S: single operand in the Ft field (not Fs -- matches the
	// x86 recompiler's XMMINFO_READT, a real EE quirk vs standard MIPS unary
	// COP1 encoding). Unconditionally clears I|D, then: Ft exponent-field==0 ->
	// fd = signed zero (Ft's sign, no extra flags); Ft negative -> sets I|SI,
	// fd = +sqrt(|fpuDouble(Ft)|) (positive magnitude only -- sqrt of a negative
	// does NOT reapply the sign); Ft positive -> fd = sqrt(fpuDouble(Ft)). No
	// overflow/underflow clamp at all in any path (ground truth has none).
	void EmitSqrtS(MacroAssembler& m, const Register& xfbase, u32 fd, u32 ft)
	{
		Label isZero, isNeg, done;

		m.Ldr(w3, MemOperand(xfbase, 252));
		m.Bic(w3, w3, 0x30000); // clear I|D unconditionally
		m.Str(w3, MemOperand(xfbase, 252));

		m.Ldr(w0, MemOperand(xfbase, ft * 4));
		m.And(w2, w0, 0x7f800000);
		m.Cbz(w2, &isZero);
		m.Tst(w0, 0x80000000);
		m.B(&isNeg, ne);

		// positive, nonzero-exp
		EmitFpuClampBits(m, w0, w4);
		m.Fmov(s0, w0);
		m.Fsqrt(s0, s0);
		m.Fmov(w0, s0);
		m.Str(w0, MemOperand(xfbase, fd * 4));
		m.B(&done);

		m.Bind(&isNeg);
		m.Ldr(w3, MemOperand(xfbase, 252));
		m.Orr(w3, w3, 0x20000); // I
		m.Orr(w3, w3, 0x40);    // SI
		m.Str(w3, MemOperand(xfbase, 252));
		EmitFpuClampBits(m, w0, w4);
		m.And(w0, w0, 0x7fffffff); // fabs
		m.Fmov(s0, w0);
		m.Fsqrt(s0, s0);
		m.Fmov(w0, s0);
		m.Str(w0, MemOperand(xfbase, fd * 4));
		m.B(&done);

		m.Bind(&isZero);
		m.And(w0, w0, 0x80000000);
		m.Str(w0, MemOperand(xfbase, fd * 4));

		m.Bind(&done);
	}

	// funct 0x16 RSQRT.S: fd = Fs / rsqrt-source(Ft). Unconditionally clears D|I,
	// then: Ft exponent-field==0 -> sets D|SD, fd = sign(Ft raw) | posFmax
	// (NOT xor'd with Fs, unlike DIV.S's inline zero-check -- a genuine
	// asymmetry in the ground truth), EARLY RETURN (no overflow/underflow).
	// Ft negative -> sets I|SI, divisor = fpuDouble(sqrt(|fpuDouble(Ft)|)) (the
	// sqrt RESULT is re-clamped through fpuDouble before dividing). Ft positive
	// -> divisor = sqrt(fpuDouble(Ft)) (sqrt result NOT re-clamped -- another
	// real asymmetry vs the negative path; replicate both exactly, do not
	// "simplify" to match). Both non-early-return paths fall through to
	// checkOverflow(0)/checkUnderflow(0) (value-clamp only, no flags).
	void EmitRsqrtS(MacroAssembler& m, const Register& xfbase, u32 fd, u32 fs, u32 ft)
	{
		Label isZero, isNeg, compute, done;

		m.Ldr(w3, MemOperand(xfbase, 252));
		m.Bic(w3, w3, 0x30000); // clear D|I unconditionally
		m.Str(w3, MemOperand(xfbase, 252));

		m.Ldr(w5, MemOperand(xfbase, ft * 4)); // raw Ft
		m.And(w2, w5, 0x7f800000);
		m.Cbz(w2, &isZero);
		m.Tst(w5, 0x80000000);
		m.B(&isNeg, ne);

		// positive, nonzero-exp: s1 = sqrt(fpuDouble(Ft)), NOT re-clamped
		m.Mov(w0, w5);
		EmitFpuClampBits(m, w0, w4);
		m.Fmov(s1, w0);
		m.Fsqrt(s1, s1);
		m.B(&compute);

		m.Bind(&isNeg);
		m.Ldr(w3, MemOperand(xfbase, 252));
		m.Orr(w3, w3, 0x20000); // I
		m.Orr(w3, w3, 0x40);    // SI
		m.Str(w3, MemOperand(xfbase, 252));
		m.Mov(w0, w5);
		EmitFpuClampBits(m, w0, w4);
		m.And(w0, w0, 0x7fffffff); // fabs
		m.Fmov(s1, w0);
		m.Fsqrt(s1, s1);
		m.Fmov(w0, s1);
		EmitFpuClampBits(m, w0, w4); // re-clamp temp.UL before dividing
		m.Fmov(s1, w0);
		m.B(&compute);

		m.Bind(&compute);
		m.Ldr(w0, MemOperand(xfbase, fs * 4));
		EmitFpuClampBits(m, w0, w4);
		m.Fmov(s0, w0);
		m.Fdiv(s0, s0, s1);
		m.Fmov(w0, s0);
		EmitFpuClampOutValueOnly(m, w0);
		m.Str(w0, MemOperand(xfbase, fd * 4));
		m.B(&done);

		m.Bind(&isZero);
		m.Ldr(w3, MemOperand(xfbase, 252));
		m.Orr(w3, w3, 0x10000); // D
		m.Orr(w3, w3, 0x20);    // SD
		m.Str(w3, MemOperand(xfbase, 252));
		m.And(w0, w5, 0x80000000);
		m.Orr(w0, w0, 0x7f7fffff);
		m.Str(w0, MemOperand(xfbase, fd * 4));

		m.Bind(&done);
	}

	// funct 0x24 CVT.W.S: if the exponent field of the RAW single (no fpuDouble
	// clamp!) is <= 0x4E800000, fd = (s32)Fs.f (C cast = truncate toward zero,
	// exactly Fcvtzs); else out-of-range: positive -> 0x7fffffff, negative ->
	// 0x80000000. Denormals/zero have exp 0 -> in-range -> truncate to 0.
	void EmitCvtW(MacroAssembler& m, const Register& xfbase, u32 fd, u32 fs)
	{
		Label outOfRange, done;
		m.Ldr(w0, MemOperand(xfbase, fs * 4));
		m.And(w2, w0, 0x7f800000);
		m.Mov(w3, 0x4E800000);
		m.Cmp(w2, w3);
		m.B(&outOfRange, hi);
		m.Fmov(s0, w0);
		m.Fcvtzs(w2, s0);
		m.Str(w2, MemOperand(xfbase, fd * 4));
		m.B(&done);
		m.Bind(&outOfRange);
		m.Mov(w2, 0x7fffffff);
		m.Mov(w3, 0x80000000);
		m.Tst(w0, 0x80000000);
		m.Csel(w2, w3, w2, ne);
		m.Str(w2, MemOperand(xfbase, fd * 4));
		m.Bind(&done);
	}

	// rs=0x14 (W format) funct 0x20 CVT.S.W: fd = (float)Fs.SL -- plain signed
	// int32 -> single conversion (Scvtf, round-to-nearest = host default FPCR,
	// which EE blocks always run under). No flags, no clamps.
	void EmitCvtS(MacroAssembler& m, const Register& xfbase, u32 fd, u32 fs)
	{
		m.Ldr(w0, MemOperand(xfbase, fs * 4));
		m.Scvtf(s0, w0);
		m.Fmov(w0, s0);
		m.Str(w0, MemOperand(xfbase, fd * 4));
	}

	// funct 0x30 C.F / 0x32 C.EQ / 0x34 C.LT / 0x36 C.LE: the compares are
	// the interpreter's fpuCompareFull total order - flush denormals to
	// zero, fold the non-sign bits of negatives, then one signed integer
	// compare - so no clamps and no host FP. The clamped Fcmp this
	// replaces collapsed distinct same-signed exponent-255 values, which
	// are ordinary numbers to the PS2, onto one clamp value and misordered
	// them. C.F unconditionally clears the C flag (0x00800000).
	void EmitFpuCmpS(MacroAssembler& m, const Register& xfbase, u32 funct, u32 fs, u32 ft)
	{
		if (funct == 0x30) // C.F: clear C only
		{
			m.Ldr(w2, MemOperand(xfbase, 252));
			m.Bic(w2, w2, 0x00800000);
			m.Str(w2, MemOperand(xfbase, 252));
			return;
		}
		m.Ldr(w0, MemOperand(xfbase, fs * 4));
		m.Ldr(w2, MemOperand(xfbase, ft * 4));
		// flush denormals: exponent field zero -> +0
		m.Tst(w0, 0x7f800000);
		m.Csel(w0, wzr, w0, eq);
		m.Tst(w2, 0x7f800000);
		m.Csel(w2, wzr, w2, eq);
		// sign-fold onto two's-complement order
		m.Asr(w4, w0, 31);
		m.Lsr(w4, w4, 1);
		m.Eor(w0, w0, w4);
		m.Asr(w4, w2, 31);
		m.Lsr(w4, w4, 1);
		m.Eor(w2, w2, w4);
		m.Cmp(w0, w2);
		m.Cset(w0, funct == 0x32 ? eq : (funct == 0x34 ? lt : le));
		m.Ldr(w2, MemOperand(xfbase, 252));
		m.Bic(w2, w2, 0x00800000);
		m.Orr(w2, w2, Operand(w0, LSL, 23));
		m.Str(w2, MemOperand(xfbase, 252));
	}

	// funct 0x28 MAX.S / 0x29 MIN.S: pure signed-int32 comparison on the raw
	// bits (pcsx2/FPU.c fp_max/fp_min) -- NOT a float compare: no fpuDouble
	// clamp, no overflow/underflow. Two negative operands invert the natural
	// signed-int order vs float magnitude, hence the bothNeg swap. Clears O|U
	// (same bits ABS_S/NEG_S clear). NOTE: xfbase is x1, so w1 must NOT be
	// used as a value register here -- the C.24 crash was exactly that
	// (`Ldr(w1, [x1, ...])` zeroed the base, sending the result store to
	// ~NULL); values live in w0/w2..w5 instead.
	void EmitMaxMinS(MacroAssembler& m, const Register& xfbase, u32 fd, u32 fs, u32 ft, bool isMax)
	{
		m.Ldr(w0, MemOperand(xfbase, fs * 4));
		m.Ldr(w2, MemOperand(xfbase, ft * 4));
		m.Cmp(w0, w2);           // signed s32 compare
		m.Csel(w3, w0, w2, gt);  // w3 = max(fs,ft)
		m.Csel(w4, w0, w2, lt);  // w4 = min(fs,ft)
		m.And(w5, w0, w2);
		m.Tst(w5, 0x80000000);   // both negative?
		if (isMax) m.Csel(w0, w4, w3, ne); // bothNeg -> min, else -> max
		else       m.Csel(w0, w3, w4, ne); // bothNeg -> max, else -> min
		m.Str(w0, MemOperand(xfbase, fd * 4));
		m.Ldr(w2, MemOperand(xfbase, 252));
		m.Bic(w2, w2, 0xC000); // clear O|U
		m.Str(w2, MemOperand(xfbase, 252));
	}

	// fpuRegisters layout: FPRreg fpr[32] (4 bytes each) at offset 0; u32 fprc[32]
	// at offset 128 -> FCR31 (the status/cause reg) at offset 128 + 31*4 = 252.
	void EmitCop1Reg(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		const u32 rs    = (insn >> 21) & 31;
		const u32 rt    = (insn >> 16) & 31;   // GPR for MFC1/MTC1
		const u32 fs    = (insn >> 11) & 31;
		const u32 fd    = (insn >>  6) & 31;
		const u32 funct = insn & 0x3f;
		const uint64_t fbase = reinterpret_cast<uint64_t>(&fpuRegs);

		if (rs == 0x00) // MFC1 rt, fs : GPR[rt] = sign_extend_32_to_64(fpr[fs].UL)
		{
			if (rt) { m.Mov(x1, fbase); m.Ldr(w0, MemOperand(x1, fs * 4)); m.Sxtw(x0, w0); StoreGpr(m, x0, gpr, rt); }
			return;
		}
		if (rs == 0x04) // MTC1 rt, fs : fpr[fs].UL = GPR[rt].UL[0]
		{
			m.Mov(x1, fbase); LoadGpr(m, x0, gpr, rt); m.Str(w0, MemOperand(x1, fs * 4));
			return;
		}
		// C.65: the control-register moves. fs is a compile-time constant, so
		// CFC1 specializes to one of two loads, and a CTC1 to any register but
		// FCR31 is a nop. fprc[31] sits at fpuRegs byte offset 252 (fpr[32]
		// first, 4 bytes each) and fprc[0] at 128 -- same base as the BC1 test.
		//
		// Only FCR0 and FCR31 exist and the hardware decodes one bit of the
		// register number, so 0-15 read as FCR0 and 16-31 as FCR31. FCR31 reads
		// drop the always-zero bits and set the always-one ones. This used to
		// mirror FPU.c's interpreter, which returned a literal 0x2E00 for
		// FCR0, zero for everything but 0 and 31, and an unmasked FCR31 -- all
		// three wrong against ps2autotests tests/cpu/ee_fpu/fcr.expected, which
		// prints the whole file after reset. FPU.c is fixed; this follows it
		// so the three engines agree.
		if (rs == 0x02) // CFC1 rt, fs
		{
			if (rt)
			{
				m.Mov(x1, fbase);
				if (fs & 0x10)
				{
					m.Ldr(w0, MemOperand(x1, 252));
					m.And(w0, w0, 0x0083c078);
					m.Orr(w0, w0, 0x01000001);
					m.Sxtw(x0, w0);
				}
				else
				{
					m.Ldr(w0, MemOperand(x1, 128));
					m.Sxtw(x0, w0);
				}
				StoreGpr(m, x0, gpr, rt);
			}
			return;
		}
		if (rs == 0x06) // CTC1 rt, fs : only FCR31 is writable
		{
			if (fs == 31) { m.Mov(x1, fbase); LoadGpr(m, x0, gpr, rt); m.Str(w0, MemOperand(x1, 252)); }
			return;
		}
		if (rs == 0x14) // W format: CVT.S.W only
		{
			m.Mov(x1, fbase);
			EmitCvtS(m, x1, fd, fs);
			return;
		}
		// rs == 0x10 single-precision ops
		const u32 ft = (insn >> 16) & 31;
		m.Mov(x1, fbase);
		if (funct == 0x00 || funct == 0x01 || funct == 0x02) // ADD.S/SUB.S/MUL.S
		{
			if (funct <= 1)
				EmitFpuAddSubExact(m, x1, funct == 1, fd * 4, fs, ft);
			else
				EmitFpuBinaryS(m, x1, funct, fd * 4, fs, ft);
			return;
		}
		if (funct == 0x18 || funct == 0x19 || funct == 0x1a) // ADDA.S/SUBA.S/MULA.S
		{
			if ((funct & 3) <= 1)
				EmitFpuAddSubExact(m, x1, (funct & 3) == 1, kFpuAcc, fs, ft);
			else
				EmitFpuBinaryS(m, x1, funct & 3, kFpuAcc, fs, ft);
			return;
		}
		if (funct == 0x03) { EmitDivS(m, x1, fd, fs, ft); return; }   // DIV.S
		if (funct == 0x04) { EmitSqrtS(m, x1, fd, ft); return; }     // SQRT.S (source is Ft, not Fs)
		if (funct == 0x16) { EmitRsqrtS(m, x1, fd, fs, ft); return; } // RSQRT.S
		if (funct == 0x1c) { EmitFpuMaddS(m, x1, fd, fs, ft, false); return; } // MADD.S
		if (funct == 0x1d) { EmitFpuMaddS(m, x1, fd, fs, ft, true); return; }  // MSUB.S
		if (funct == 0x1e) { EmitFpuMaddaS(m, x1, fs, ft, false); return; }    // MADDA.S
		if (funct == 0x1f) { EmitFpuMaddaS(m, x1, fs, ft, true); return; }     // MSUBA.S
		if (funct == 0x24) { EmitCvtW(m, x1, fd, fs); return; }               // CVT.W.S
		if (funct == 0x28) { EmitMaxMinS(m, x1, fd, fs, ft, true); return; }  // MAX.S
		if (funct == 0x29) { EmitMaxMinS(m, x1, fd, fs, ft, false); return; } // MIN.S
		if (funct == 0x30 || funct == 0x32 || funct == 0x34 || funct == 0x36) // C.F/C.EQ/C.LT/C.LE
		{
			EmitFpuCmpS(m, x1, funct, fs, ft);
			return;
		}
		m.Ldr(w0, MemOperand(x1, fs * 4));
		if (funct == 0x06) { m.Str(w0, MemOperand(x1, fd * 4)); return; } // MOV.S
		if (funct == 0x05) m.And(w0, w0, 0x7fffffff);                     // ABS.S
		else               m.Eor(w0, w0, 0x80000000);                    // NEG.S
		m.Str(w0, MemOperand(x1, fd * 4));
		// clear the O|U cause flags in FCR31 (ABS.S/NEG.S do clearFPUFlags(O|U))
		m.Ldr(w0, MemOperand(x1, 252));
		m.Bic(w0, w0, 0xC000);
		m.Str(w0, MemOperand(x1, 252));
	}

	// ---- MMI (opcode 0x1C): 128-bit parallel integer ops -> ARM NEON ----
	// The EE GPRs are 128 bits; MMI does packed byte/half/word arithmetic over the
	// whole register, which maps 1:1 onto AArch64 NEON Q-registers. We translate
	// only the bit-exact, side-effect-free ops (parallel add/sub incl. saturating,
	// logical, signed compares, signed max/min, the EXTL/EXTU interleaves, the
	// immediate shifts, the PPAC* even-element packs and PCPYLD/PCPYUD). PMFHL/
	// PMTHL, the HI/LO multiply-divide ops, variable shifts, PMADD, the remaining
	// shuffles (PINTH/PEXEH/PREVH/PCPYH...), etc. stay with the interpreter.
	enum { M_ADD, M_SUB, M_SQADD, M_UQADD, M_SQSUB, M_UQSUB, M_AND, M_ORR, M_EOR,
	       M_NOR, M_CMGT, M_CMEQ, M_SMAX, M_SMIN, M_ZIP1, M_ZIP2,
	       M_SLLI, M_SRLI, M_SRAI, // immediate shifts (fmt 1=.8H sa&0xf, 2=.4S sa)
	       M_PACK,                 // PPACB/H/W: even elements, rt -> low, rs -> high
	       M_CPYLD, M_CPYUD,       // PCPYLD/PCPYUD (64-bit lane zips)
	       M_REVH };               // PREVH: reverse the halfwords within each 64-bit half

	// Decode an MMI instruction to (action, lane fmt: 0=16B,1=8H,2=4S). Returns the
	// action or -1 if not natively supported. funct selects the sub-class
	// (MMI0=0x08, MMI1=0x28, MMI2=0x09, MMI3=0x29); (insn>>6)&0x1f the op.
	int MmiAction(u32 insn, int* fmt)
	{
		const u32 funct = insn & 0x3f;
		const u32 sub   = (insn >> 6) & 0x1f;
		switch (funct)
		{
			// top-level MMI functs: immediate shifts (sub = shift amount)
			case 0x34: *fmt = 1; return M_SLLI; // PSLLH
			case 0x36: *fmt = 1; return M_SRLI; // PSRLH
			case 0x37: *fmt = 1; return M_SRAI; // PSRAH
			case 0x3c: *fmt = 2; return M_SLLI; // PSLLW
			case 0x3e: *fmt = 2; return M_SRLI; // PSRLW
			case 0x3f: *fmt = 2; return M_SRAI; // PSRAW
			case 0x08: // MMI0
				switch (sub)
				{
					case 0x00: *fmt = 2; return M_ADD;   // PADDW
					case 0x01: *fmt = 2; return M_SUB;   // PSUBW
					case 0x02: *fmt = 2; return M_CMGT;  // PCGTW
					case 0x03: *fmt = 2; return M_SMAX;  // PMAXW
					case 0x04: *fmt = 1; return M_ADD;   // PADDH
					case 0x05: *fmt = 1; return M_SUB;   // PSUBH
					case 0x06: *fmt = 1; return M_CMGT;  // PCGTH
					case 0x07: *fmt = 1; return M_SMAX;  // PMAXH
					case 0x08: *fmt = 0; return M_ADD;   // PADDB
					case 0x09: *fmt = 0; return M_SUB;   // PSUBB
					case 0x0a: *fmt = 0; return M_CMGT;  // PCGTB
					case 0x10: *fmt = 2; return M_SQADD; // PADDSW
					case 0x11: *fmt = 2; return M_SQSUB; // PSUBSW
					case 0x12: *fmt = 2; return M_ZIP1;  // PEXTLW
					case 0x13: *fmt = 2; return M_PACK;  // PPACW
					case 0x14: *fmt = 1; return M_SQADD; // PADDSH
					case 0x15: *fmt = 1; return M_SQSUB; // PSUBSH
					case 0x16: *fmt = 1; return M_ZIP1;  // PEXTLH
					case 0x17: *fmt = 1; return M_PACK;  // PPACH
					case 0x18: *fmt = 0; return M_SQADD; // PADDSB
					case 0x19: *fmt = 0; return M_SQSUB; // PSUBSB
					case 0x1a: *fmt = 0; return M_ZIP1;  // PEXTLB
					case 0x1b: *fmt = 0; return M_PACK;  // PPACB
				}
				return -1;
			case 0x28: // MMI1
				switch (sub)
				{
					case 0x02: *fmt = 2; return M_CMEQ;  // PCEQW
					case 0x03: *fmt = 2; return M_SMIN;  // PMINW
					case 0x06: *fmt = 1; return M_CMEQ;  // PCEQH
					case 0x07: *fmt = 1; return M_SMIN;  // PMINH
					case 0x0a: *fmt = 0; return M_CMEQ;  // PCEQB
					case 0x10: *fmt = 2; return M_UQADD; // PADDUW
					case 0x11: *fmt = 2; return M_UQSUB; // PSUBUW
					case 0x12: *fmt = 2; return M_ZIP2;  // PEXTUW
					case 0x14: *fmt = 1; return M_UQADD; // PADDUH
					case 0x15: *fmt = 1; return M_UQSUB; // PSUBUH
					case 0x16: *fmt = 1; return M_ZIP2;  // PEXTUH
					case 0x18: *fmt = 0; return M_UQADD; // PADDUB
					case 0x19: *fmt = 0; return M_UQSUB; // PSUBUB
					case 0x1a: *fmt = 0; return M_ZIP2;  // PEXTUB
				}
				return -1;
			case 0x09: // MMI2
				if (sub == 0x0e) { *fmt = 3; return M_CPYLD; } // PCPYLD
				if (sub == 0x12) { *fmt = 0; return M_AND; }   // PAND
				if (sub == 0x13) { *fmt = 0; return M_EOR; }   // PXOR
				if (sub == 0x1b) { *fmt = 1; return M_REVH; }  // PREVH (C.63)
				return -1;
			case 0x29: // MMI3
				if (sub == 0x0e) { *fmt = 3; return M_CPYUD; } // PCPYUD
				if (sub == 0x12) { *fmt = 0; return M_ORR; }   // POR
				if (sub == 0x13) { *fmt = 0; return M_NOR; }   // PNOR
				return -1;
		}
		return -1;
	}

	bool MmiSupported(u32 insn) { int f; return MmiAction(insn, &f) >= 0; }

	// Load a full 128-bit GPR into a NEON Q-register (r0 reads as all-zero).
	void LoadQ(MacroAssembler& m, const VRegister& q, const Register& gpr, u32 idx)
	{
		if (idx == 0) m.Movi(q.V2D(), (uint64_t)0);
		else          m.Ldr(q, MemOperand(gpr, idx * 16));
	}

	void EmitMmi(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		int fmt;
		const int act = MmiAction(insn, &fmt);
		const u32 rd = (insn >> 11) & 31, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
		if (!rd) return; // result discarded (r0 stays zero)

		s_rc.FlushReg(m, gpr, rs); // 128-bit sources are read from memory
		s_rc.FlushReg(m, gpr, rt);
		LoadQ(m, q1, gpr, rs); // rs -> v1
		LoadQ(m, q2, gpr, rt); // rt -> v2
		VRegister d, n, o;     // n = rs(v1), o = rt(v2)
		switch (fmt)
		{
			case 0:  d = v0.V16B(); n = v1.V16B(); o = v2.V16B(); break;
			case 1:  d = v0.V8H();  n = v1.V8H();  o = v2.V8H();  break;
			case 2:  d = v0.V4S();  n = v1.V4S();  o = v2.V4S();  break;
			default: d = v0.V2D();  n = v1.V2D();  o = v2.V2D();  break;
		}
		// Immediate shift amount: PS*H uses sa & 0xf, PS*W the full 5-bit sa.
		const int shamt = (int)((insn >> 6) & (fmt == 1 ? 0xf : 0x1f));
		switch (act)
		{
			case M_ADD:   m.Add(d, n, o);   break;
			case M_SUB:   m.Sub(d, n, o);   break;
			case M_SQADD: m.Sqadd(d, n, o); break;
			case M_UQADD: m.Uqadd(d, n, o); break;
			case M_SQSUB: m.Sqsub(d, n, o); break;
			case M_UQSUB: m.Uqsub(d, n, o); break;
			case M_AND:   m.And(d, n, o);   break;
			case M_ORR:   m.Orr(d, n, o);   break;
			case M_EOR:   m.Eor(d, n, o);   break;
			case M_NOR:   m.Orr(d, n, o); m.Mvn(d, d); break;
			case M_CMGT:  m.Cmgt(d, n, o);  break;
			case M_CMEQ:  m.Cmeq(d, n, o);  break;
			case M_SMAX:  m.Smax(d, n, o);  break;
			case M_SMIN:  m.Smin(d, n, o);  break;
			case M_ZIP1:  m.Zip1(d, o, n);  break; // PEXTL: interleave rt then rs
			case M_ZIP2:  m.Zip2(d, o, n);  break; // PEXTU
			// NEON USHR/SSHR immediates encode 1..lanebits only; shift 0 is a copy.
			case M_SLLI:  m.Shl(d, o, shamt); break;                                      // PSLLH/PSLLW
			case M_SRLI:  if (shamt) m.Ushr(d, o, shamt); else m.Mov(v0.V16B(), v2.V16B()); break; // PSRLH/PSRLW
			case M_SRAI:  if (shamt) m.Sshr(d, o, shamt); else m.Mov(v0.V16B(), v2.V16B()); break; // PSRAH/PSRAW
			case M_PACK:  m.Uzp1(d, o, n);  break; // PPAC*: rt evens -> low, rs evens -> high
			case M_CPYLD: m.Zip1(d, o, n);  break; // PCPYLD: rd = {rt.UD[0], rs.UD[0]}
			case M_CPYUD: m.Zip2(d, n, o);  break; // PCPYUD: rd = {rs.UD[1], rt.UD[1]}
			// PREVH: rd.US[0..3] = rt.US[3..0], rd.US[4..7] = rt.US[7..4] -- exactly
			// what REV64 does on a .8H view (reverse halfwords within each 64 bits).
			case M_REVH:  m.Rev64(d, o);    break;
		}
		m.Str(q0, MemOperand(gpr, rd * 16));
		s_rc.Invalidate(rd); // 128-bit write bypasses StoreGpr
	}

	// HI/LO live right after the 32 128-bit GPRs: HI at byte offset 32*16, LO
	// at 32*16+16. Both are full 128-bit (cpuRegisters: GPR_reg HI, LO).
	constexpr u32 kHI = 32 * 16;
	constexpr u32 kLO = kHI + 16;

	// C.79: the remaining EE MMI ops that actually break blocks in-race (GT3):
	// PPAC5 (per-word RGB1555 pack) and the full-128-bit HI/LO moves PMFHI/PMFLO
	// (rd <- HI/LO) and PMTHI/PMTLO (HI/LO <- rs). HI/LO are 128-bit and live at
	// kHI/kLO right after the 32 GPRs (see EmitMulDiv). All bit-exact, no side
	// effects. PMFHL/PMTHL stay with the interpreter -- they never fire in the
	// reference games, so a native path couldn't be golden-verified.
	enum { HL_NONE, HL_PMFHI, HL_PMFLO, HL_PMTHI, HL_PMTLO, HL_PPAC5 };
	int MmiHiLoAction(u32 insn)
	{
		const u32 funct = insn & 0x3f, sub = (insn >> 6) & 0x1f;
		if (funct == 0x09) { if (sub == 0x08) return HL_PMFHI; if (sub == 0x09) return HL_PMFLO; } // MMI2
		if (funct == 0x29) { if (sub == 0x08) return HL_PMTHI; if (sub == 0x09) return HL_PMTLO; } // MMI3
		if (funct == 0x08 && sub == 0x1f) return HL_PPAC5;                                         // MMI0
		return HL_NONE;
	}

	// Element permutes, the saturating absolutes, the variable word shifts,
	// PADSBH and PEXT5.
	//
	// The permutes are pure data movement: each destination element is one
	// source element, so they are emitted as word or halfword loads followed
	// by stores. Every load is issued before any store, which is what makes
	// rd aliasing rs or rt safe -- the interpreter reads its sources into
	// locals for the same reason.
	//
	// PABSW/PABSH are the saturating absolute value, clamping 0x80000000 to
	// 0x7fffffff and 0x8000 to 0x7fff, which is precisely what SQABS does.
	enum { MP_NONE, MP_PEXEW, MP_PEXCW, MP_PROT3W, MP_PEXEH, MP_PEXCH,
	       MP_PINTH, MP_PINTEH, MP_PABSW, MP_PABSH, MP_PEXT5, MP_PADSBH,
	       MP_PSLLVW, MP_PSRLVW, MP_PSRAVW };
	int MmiPermAction(u32 insn)
	{
		const u32 funct = insn & 0x3f, sub = (insn >> 6) & 0x1f;
		if (funct == 0x08 && sub == 0x1e) return MP_PEXT5; // MMI0
		if (funct == 0x28) // MMI1
		{
			if (sub == 0x01) return MP_PABSW;
			if (sub == 0x04) return MP_PADSBH;
			if (sub == 0x05) return MP_PABSH;
		}
		if (funct == 0x09) // MMI2
		{
			if (sub == 0x02) return MP_PSLLVW;
			if (sub == 0x03) return MP_PSRLVW;
			if (sub == 0x0a) return MP_PINTH;
			if (sub == 0x1a) return MP_PEXEH;
			if (sub == 0x1e) return MP_PEXEW;
			if (sub == 0x1f) return MP_PROT3W;
		}
		if (funct == 0x29) // MMI3
		{
			if (sub == 0x03) return MP_PSRAVW;
			if (sub == 0x0a) return MP_PINTEH;
			if (sub == 0x1a) return MP_PEXCH;
			if (sub == 0x1e) return MP_PEXCW;
		}
		return MP_NONE;
	}

	void EmitMmiPerm(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		const int act = MmiPermAction(insn);
		const u32 rd = (insn >> 11) & 31, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
		int i;

		if (!rd)
			return;

		// PSLLVW/PSRLVW/PSRAVW shift words 0 and 2 of rt by the low five bits
		// of the matching word of rs, each 32-bit result sign-extended into a
		// 64-bit lane of rd. A 32-bit variable shift on AArch64 already takes
		// its amount modulo 32, but the mask is emitted anyway so the code
		// says what the interpreter says.
		if (act == MP_PSLLVW || act == MP_PSRLVW || act == MP_PSRAVW)
		{
			s_rc.FlushReg(m, gpr, rs);
			s_rc.FlushReg(m, gpr, rt);
			for (i = 0; i < 2; i++)
			{
				const u32 word = (u32)(i * 2);
				m.Ldr(w0, MemOperand(gpr, rt * 16 + word * 4));
				m.Ldr(w1, MemOperand(gpr, rs * 16 + word * 4));
				m.And(w1, w1, 0x1f);
				if      (act == MP_PSLLVW) m.Lsl(w2, w0, w1);
				else if (act == MP_PSRLVW) m.Lsr(w2, w0, w1);
				else                       m.Asr(w2, w0, w1);
				m.Sxtw(x2, w2);
				m.Str(x2, MemOperand(gpr, rd * 16 + (u32)(i * 8)));
			}
			s_rc.Invalidate(rd);
			return;
		}

		// PADSBH subtracts in the lower four halfwords and adds in the upper
		// four, both wrapping (the saturating forms are PSUBSH/PADDSH).
		if (act == MP_PADSBH)
		{
			s_rc.FlushReg(m, gpr, rs);
			s_rc.FlushReg(m, gpr, rt);
			LoadQ(m, q1, gpr, rs);
			LoadQ(m, q2, gpr, rt);
			m.Sub(v3.V8H(), v1.V8H(), v2.V8H());
			m.Add(v4.V8H(), v1.V8H(), v2.V8H());
			m.Ins(v3.V2D(), 1, v4.V2D(), 1); // keep sub low, add high
			m.Str(q3, MemOperand(gpr, rd * 16));
			s_rc.Invalidate(rd);
			return;
		}

		// PEXT5 is the inverse of PPAC5 above: unpack RGB1555 out of each
		// word, the same four fields shifted the other way.
		if (act == MP_PEXT5)
		{
			s_rc.FlushReg(m, gpr, rt);
			LoadQ(m, q1, gpr, rt);
			m.Movi(v4.V4S(), 0x1f);
			m.Shl(v5.V4S(), v4.V4S(), 5);   // 0x3e0
			m.Shl(v6.V4S(), v4.V4S(), 10);  // 0x7c00
			m.Movi(v7.V4S(), 0x80, LSL, 8); // 0x8000
			m.And(v0.V16B(), v1.V16B(), v4.V16B()); m.Shl(v0.V4S(), v0.V4S(), 3);
			m.And(v2.V16B(), v1.V16B(), v5.V16B()); m.Shl(v2.V4S(), v2.V4S(), 6);
			m.Orr(v0.V16B(), v0.V16B(), v2.V16B());
			m.And(v2.V16B(), v1.V16B(), v6.V16B()); m.Shl(v2.V4S(), v2.V4S(), 9);
			m.Orr(v0.V16B(), v0.V16B(), v2.V16B());
			m.And(v2.V16B(), v1.V16B(), v7.V16B()); m.Shl(v2.V4S(), v2.V4S(), 16);
			m.Orr(v0.V16B(), v0.V16B(), v2.V16B());
			m.Str(q0, MemOperand(gpr, rd * 16));
			s_rc.Invalidate(rd);
			return;
		}

		if (act == MP_PABSW || act == MP_PABSH)
		{
			s_rc.FlushReg(m, gpr, rt);
			LoadQ(m, q1, gpr, rt);
			if (act == MP_PABSW) m.Sqabs(v0.V4S(), v1.V4S());
			else                 m.Sqabs(v0.V8H(), v1.V8H());
			m.Str(q0, MemOperand(gpr, rd * 16));
			s_rc.Invalidate(rd); // 128-bit write bypasses StoreGpr
			return;
		}

		// Source element index per destination element, matching MMI.c.
		// Two-source forms take rs elements as indices 8..15 (halfwords) so
		// the tables stay uniform.
		static const int kPexew[4]  = { 2, 1, 0, 3 };
		static const int kPexcw[4]  = { 0, 2, 1, 3 };
		static const int kProt3w[4] = { 1, 2, 0, 3 };
		static const int kPexeh[8]  = { 2, 1, 0, 3, 6, 5, 4, 7 };
		static const int kPexch[8]  = { 0, 2, 1, 3, 4, 6, 5, 7 };
		static const int kPinth[8]  = { 0, 8 + 4, 1, 8 + 5, 2, 8 + 6, 3, 8 + 7 };
		static const int kPinteh[8] = { 0, 8 + 0, 2, 8 + 2, 4, 8 + 4, 6, 8 + 6 };

		const bool     word  = (act == MP_PEXEW || act == MP_PEXCW || act == MP_PROT3W);
		const bool     two   = (act == MP_PINTH || act == MP_PINTEH);
		const int      count = word ? 4 : 8;
		const int      size  = word ? 4 : 2;
		const int*     idx   = (act == MP_PEXEW)  ? kPexew  :
		                       (act == MP_PEXCW)  ? kPexcw  :
		                       (act == MP_PROT3W) ? kProt3w :
		                       (act == MP_PEXEH)  ? kPexeh  :
		                       (act == MP_PEXCH)  ? kPexch  :
		                       (act == MP_PINTH)  ? kPinth  : kPinteh;
		// w0..w7 hold the gathered elements; count is 4 or 8, so they fit.
		const Register src[8] = { w0, w1, w2, w3, w4, w5, w6, w7 };

		s_rc.FlushReg(m, gpr, rt);
		if (two)
			s_rc.FlushReg(m, gpr, rs);

		for (i = 0; i < count; i++)
		{
			const int  e    = idx[i];
			const bool from = (e >= 8);            // two-source: 8.. selects rs
			const u32  base = (from ? rs : rt) * 16;
			const u32  off  = base + (u32)((from ? e - 8 : e) * size);
			if (word) m.Ldr(src[i], MemOperand(gpr, off));
			else      m.Ldrh(src[i], MemOperand(gpr, off));
		}
		for (i = 0; i < count; i++)
		{
			const u32 off = rd * 16 + (u32)(i * size);
			if (word) m.Str(src[i], MemOperand(gpr, off));
			else      m.Strh(src[i], MemOperand(gpr, off));
		}
		s_rc.Invalidate(rd); // 128-bit write bypasses StoreGpr
	}

	// The halfword multiply-accumulates: PMADDH (MMI2 0x10), PHMADH (0x11),
	// PMSUBH (0x14) and PHMSBH (0x15).
	//
	// PMADDH and PMSUBH accumulate eight halfword products into HI and LO on
	// the same scatter PMULTH uses: products 0,1,4,5 to LO words 0..3 and
	// 2,3,6,7 to HI words 0..3, each added to or subtracted from what is
	// already in that word.
	//
	// PHMADH and PHMSBH are horizontal and do not accumulate at all. For each
	// pair they compute the product of the upper halfword, then add or
	// subtract the product of the lower one, writing the combined value to
	// the even word and the upper product alone to the odd word. PHMSBH
	// stores that second value complemented -- undocumented, but what the
	// hardware does and what MMI.c reproduces.
	//
	// All four then fill rd, when written, from LO word 0, HI word 0, LO word
	// 2 and HI word 2, read back after the products are in place, exactly as
	// the interpreter assigns them.
	enum { MH_NONE, MH_PMADDH, MH_PMSUBH, MH_PHMADH, MH_PHMSBH };
	int MmiHmaccAction(u32 insn)
	{
		const u32 funct = insn & 0x3f, sub = (insn >> 6) & 0x1f;
		if (funct != 0x09) // all four are MMI2
			return MH_NONE;
		if (sub == 0x10) return MH_PMADDH;
		if (sub == 0x11) return MH_PHMADH;
		if (sub == 0x14) return MH_PMSUBH;
		if (sub == 0x15) return MH_PHMSBH;
		return MH_NONE;
	}

	void EmitMmiHmacc(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		const int  act  = MmiHmaccAction(insn);
		const u32  rd   = (insn >> 11) & 31, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
		const bool add  = (act == MH_PMADDH || act == MH_PHMADH);
		const bool horiz = (act == MH_PHMADH || act == MH_PHMSBH);
		int        i;

		s_rc.FlushReg(m, gpr, rs);
		s_rc.FlushReg(m, gpr, rt);

		if (!horiz)
		{
			static const int kToLo[8] = { 1, 1, 0, 0, 1, 1, 0, 0 };
			static const int kWord[8] = { 0, 1, 0, 1, 2, 3, 2, 3 };
			for (i = 0; i < 8; i++)
			{
				const u32 dest = (kToLo[i] ? kLO : kHI) + (u32)(kWord[i] * 4);
				m.Ldrsh(w0, MemOperand(gpr, rs * 16 + (u32)(i * 2)));
				m.Ldrsh(w1, MemOperand(gpr, rt * 16 + (u32)(i * 2)));
				m.Mul(w2, w0, w1);
				m.Ldr(w3, MemOperand(gpr, dest));
				if (add) m.Add(w2, w3, w2);
				else     m.Sub(w2, w3, w2);
				m.Str(w2, MemOperand(gpr, dest));
			}
		}
		else
		{
			// (destination, halfword) pairs: LO from 0, HI from 2, LO from 4,
			// HI from 6, filling words 0/1 then 2/3 of each.
			static const int kToLo[4] = { 1, 0, 1, 0 };
			static const int kDd[4]   = { 0, 0, 2, 2 };
			static const int kN[4]    = { 0, 2, 4, 6 };
			for (i = 0; i < 4; i++)
			{
				const u32 base = (kToLo[i] ? kLO : kHI);
				m.Ldrsh(w0, MemOperand(gpr, rs * 16 + (u32)((kN[i] + 1) * 2)));
				m.Ldrsh(w1, MemOperand(gpr, rt * 16 + (u32)((kN[i] + 1) * 2)));
				m.Mul(w2, w0, w1);                      // firsttemp
				m.Ldrsh(w0, MemOperand(gpr, rs * 16 + (u32)(kN[i] * 2)));
				m.Ldrsh(w1, MemOperand(gpr, rt * 16 + (u32)(kN[i] * 2)));
				m.Mul(w3, w0, w1);
				if (add) m.Add(w1, w2, w3);
				else     m.Sub(w1, w2, w3);
				m.Str(w1, MemOperand(gpr, base + (u32)(kDd[i] * 4)));
				if (!add)
					m.Mvn(w2, w2);                      // ~firsttemp
				m.Str(w2, MemOperand(gpr, base + (u32)((kDd[i] + 1) * 4)));
			}
		}

		if (rd)
		{
			m.Ldr(w0, MemOperand(gpr, kLO + 0));
			m.Ldr(w1, MemOperand(gpr, kHI + 0));
			m.Ldr(w2, MemOperand(gpr, kLO + 8));
			m.Ldr(w3, MemOperand(gpr, kHI + 8));
			m.Str(w0, MemOperand(gpr, rd * 16 + 0));
			m.Str(w1, MemOperand(gpr, rd * 16 + 4));
			m.Str(w2, MemOperand(gpr, rd * 16 + 8));
			m.Str(w3, MemOperand(gpr, rd * 16 + 12));
			s_rc.Invalidate(rd);
		}
	}

	// The word-lane multiply-accumulates: PMADDW (MMI2 0x00), PMSUBW
	// (MMI2 0x04) and PMADDUW (MMI3 0x00). Unlike the PMULT forms these read
	// HI and LO back and combine with what is already there.
	//
	// All three treat the lane's accumulator as one 64-bit quantity spanning
	// LO and HI rather than two independent halves, so the carry out of the
	// low word reaches the high one, and both destinations are re-derived
	// from that single result. PMADDW and PMSUBW previously reproduced two
	// claimed errata here -- a divide by 0xffffffff standing in for >> 32 and
	// a conditional +0x70000000 on lane 0 -- which the ps2autotests console
	// captures do not support (30/32 and 28/32 against them, versus 32/32
	// for this form), and which the x86 recompiler never implemented either.
	enum { MA_NONE, MA_PMADDW, MA_PMSUBW, MA_PMADDUW };
	int MmiMaccAction(u32 insn)
	{
		const u32 funct = insn & 0x3f, sub = (insn >> 6) & 0x1f;
		if (funct == 0x09 && sub == 0x00) return MA_PMADDW;  // MMI2
		if (funct == 0x09 && sub == 0x04) return MA_PMSUBW;  // MMI2
		if (funct == 0x29 && sub == 0x00) return MA_PMADDUW; // MMI3
		return MA_NONE;
	}

	void EmitMmiMacc(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		const int  act  = MmiMaccAction(insn);
		const u32  rd   = (insn >> 11) & 31, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
		const bool madd = (act == MA_PMADDW);
		int        i;

		s_rc.FlushReg(m, gpr, rs);
		s_rc.FlushReg(m, gpr, rt);

		for (i = 0; i < 2; i++)
		{
			const u32 ss   = (u32)(i * 2);      // source word
			const u32 lane = (u32)(i * 8);      // destination 64-bit lane

			if (act == MA_PMADDUW)
			{
				// tempu = (LO.UL[ss] | HI.UL[ss] << 32) + rs.UL[ss] * rt.UL[ss]
				m.Ldr(w0, MemOperand(gpr, rs * 16 + ss * 4));
				m.Ldr(w1, MemOperand(gpr, rt * 16 + ss * 4));
				m.Umull(x2, w0, w1);
				m.Ldr(w3, MemOperand(gpr, kLO + ss * 4));
				m.Ldr(w4, MemOperand(gpr, kHI + ss * 4));
				m.Orr(x3, x3, Operand(x4, LSL, 32));
				m.Add(x2, x2, x3);
				m.Sxtw(x5, w2);                     // LO = sign-extended low
				m.Lsr(x6, x2, 32); m.Sxtw(x6, w6);  // HI = sign-extended high
				m.Str(x5, MemOperand(gpr, kLO + lane));
				m.Str(x6, MemOperand(gpr, kHI + lane));
				if (rd)
					m.Str(x2, MemOperand(gpr, rd * 16 + lane)); // rd = whole sum
				continue;
			}

			// product = (s64)rs.SL[ss] * rt.SL[ss]
			m.Ldr(w0, MemOperand(gpr, rs * 16 + ss * 4));
			m.Ldr(w1, MemOperand(gpr, rt * 16 + ss * 4));
			m.Smull(x2, w0, w1);
			// acc = LO.UL[ss] | HI.UL[ss] << 32, as one 64-bit value
			m.Ldr(w3, MemOperand(gpr, kLO + ss * 4));
			m.Ldr(w4, MemOperand(gpr, kHI + ss * 4));
			m.Orr(x3, x3, Operand(x4, LSL, 32));
			if (madd) m.Add(x2, x3, x2);
			else      m.Sub(x2, x3, x2);
			m.Sxtw(x5, w2);                      // LO = sign-extended low
			m.Lsr(x6, x2, 32); m.Sxtw(x6, w6);   // HI = sign-extended high
			m.Str(x5, MemOperand(gpr, kLO + lane));
			m.Str(x6, MemOperand(gpr, kHI + lane));
			if (rd)
			{
				// rd word dd*2 takes LO's low word, dd*2+1 takes HI's,
				// which together are the 64-bit result.
				m.Str(x2, MemOperand(gpr, rd * 16 + lane));
			}
		}
		if (rd)
			s_rc.Invalidate(rd);
	}

	// The parallel multiplies that only write: PMULTW and PMULTUW (MMI2 0x0c,
	// MMI3 0x0c) and PMULTH (MMI2 0x1c). The multiply-accumulate forms are a
	// different problem -- they read HI/LO back -- and stay with the
	// interpreter.
	//
	// PMULTW multiplies words 0 and 2 signed into 64-bit products; PMULTUW is
	// the same unsigned. Both split the product into LO and HI as two
	// sign-extended halves and, when rd is written, put the whole 64-bit
	// product in the matching lane of rd.
	//
	// PMULTH multiplies all eight halfword pairs into 32-bit products and
	// scatters them: products 0,1,4,5 to LO words 0,1,2,3 and products
	// 2,3,6,7 to HI words 0,1,2,3. rd, when written, takes products 0,2,4,6.
	// Every product is computed before anything is stored, so rd aliasing rs
	// or rt behaves as the interpreter does.
	enum { MM_NONE, MM_PMULTW, MM_PMULTUW, MM_PMULTH };
	int MmiMulAction(u32 insn)
	{
		const u32 funct = insn & 0x3f, sub = (insn >> 6) & 0x1f;
		if (funct == 0x09 && sub == 0x0c) return MM_PMULTW;  // MMI2
		if (funct == 0x09 && sub == 0x1c) return MM_PMULTH;  // MMI2
		if (funct == 0x29 && sub == 0x0c) return MM_PMULTUW; // MMI3
		return MM_NONE;
	}

	void EmitMmiMul(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		const int act = MmiMulAction(insn);
		const u32 rd = (insn >> 11) & 31, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
		int i;

		s_rc.FlushReg(m, gpr, rs);
		s_rc.FlushReg(m, gpr, rt);

		if (act == MM_PMULTH)
		{
			// Products 0,1,4,5 land in LO words 0,1,2,3 and products 2,3,6,7
			// in HI words 0,1,2,3. Each is stored as it is computed; only
			// LO/HI are touched in this pass, so rd aliasing rs or rt cannot
			// disturb a source that has not been read yet.
			static const int kToLo[8] = { 1, 1, 0, 0, 1, 1, 0, 0 };
			static const int kWord[8] = { 0, 1, 0, 1, 2, 3, 2, 3 };

			for (i = 0; i < 8; i++)
			{
				m.Ldrsh(w0, MemOperand(gpr, rs * 16 + (u32)(i * 2)));
				m.Ldrsh(w1, MemOperand(gpr, rt * 16 + (u32)(i * 2)));
				m.Mul(w2, w0, w1);
				m.Str(w2, MemOperand(gpr,
					(kToLo[i] ? kLO : kHI) + (u32)(kWord[i] * 4)));
			}
			if (rd)
			{
				// rd takes LO word 0, HI word 0, LO word 2, HI word 2, read
				// back after every product is in place exactly as the
				// interpreter assigns them.
				m.Ldr(w0, MemOperand(gpr, kLO + 0));
				m.Ldr(w1, MemOperand(gpr, kHI + 0));
				m.Ldr(w2, MemOperand(gpr, kLO + 8));
				m.Ldr(w3, MemOperand(gpr, kHI + 8));
				m.Str(w0, MemOperand(gpr, rd * 16 + 0));
				m.Str(w1, MemOperand(gpr, rd * 16 + 4));
				m.Str(w2, MemOperand(gpr, rd * 16 + 8));
				m.Str(w3, MemOperand(gpr, rd * 16 + 12));
				s_rc.Invalidate(rd);
			}
			return;
		}

		for (i = 0; i < 2; i++)
		{
			const u32 word = (u32)(i * 2);
			const u32 lane = (u32)(i * 8);
			m.Ldr(w0, MemOperand(gpr, rs * 16 + word * 4));
			m.Ldr(w1, MemOperand(gpr, rt * 16 + word * 4));
			if (act == MM_PMULTW) m.Smull(x2, w0, w1);
			else                  m.Umull(x2, w0, w1);
			m.Sxtw(x3, w2);                      // LO = sign-extended low half
			m.Lsr(x4, x2, 32); m.Sxtw(x4, w4);   // HI = sign-extended high half
			m.Str(x3, MemOperand(gpr, kLO + lane));
			m.Str(x4, MemOperand(gpr, kHI + lane));
			if (rd)
				m.Str(x2, MemOperand(gpr, rd * 16 + lane)); // whole product
		}
		if (rd)
			s_rc.Invalidate(rd);
	}

	// The parallel divides: PDIVW (MMI2 sub 0x0d) and PDIVBW (MMI3 sub 0x1d)
	// signed, PDIVUW (MMI3 sub 0x0d) unsigned. Two 32-bit lanes each except
	// PDIVBW, which has four. Without these a block containing one runs
	// entirely on the interpreter.
	//
	// Both mirror MMI.c _PDIVW/_PDIVBW, including the two results the EE
	// defines where hardware division would otherwise misbehave: INT_MIN / -1
	// gives INT_MIN remainder 0, and division by zero gives a quotient of
	// (dividend < 0) ? 1 : -1 with the dividend as the remainder. SDIV traps
	// on neither case, but it answers 0 and INT_MIN, so the guards are still
	// needed to match.
	//
	// PDIVW divides word ss of rs by word ss of rt for ss = 0 and 2, writing
	// the sign-extended results to the two 64-bit lanes of LO and HI. PDIVBW
	// divides each of the four words of rs by halfword 0 of rt -- one signed
	// divisor shared by every lane -- writing four 32-bit lanes.
	enum { MD_NONE, MD_PDIVW, MD_PDIVBW, MD_PDIVUW };
	int MmiDivAction(u32 insn)
	{
		const u32 funct = insn & 0x3f, sub = (insn >> 6) & 0x1f;
		if (funct == 0x09 && sub == 0x0d) return MD_PDIVW;  // MMI2
		if (funct == 0x29 && sub == 0x1d) return MD_PDIVBW; // MMI3
		if (funct == 0x29 && sub == 0x0d) return MD_PDIVUW; // MMI3
		return MD_NONE;
	}

	void EmitMmiDiv(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		const int  act   = MmiDivAction(insn);
		const u32  rs    = (insn >> 21) & 31, rt = (insn >> 16) & 31;
		const bool bw    = (act == MD_PDIVBW);
		const bool uw    = (act == MD_PDIVUW);
		const int  lanes = bw ? 4 : 2;
		int        i;

		// Both operands are read straight out of memory, so any cached low
		// half has to be in memory first (as PCPYH and PPAC5 do above).
		s_rc.FlushReg(m, gpr, rs);
		s_rc.FlushReg(m, gpr, rt);

		// PDIVBW's divisor is halfword 0 of rt, sign-extended once and reused
		// by every lane. Reading it signed also makes the -1 test below work
		// on the same register the division uses.
		if (bw)
			m.Ldrsh(w1, MemOperand(gpr, rt * 16));

		for (i = 0; i < lanes; i++)
		{
			Label zero, normal, store;
			const int word = bw ? i : i * 2;                 // word index within rs
			const u32 loo  = kLO + (u32)(bw ? i * 4 : i * 8);
			const u32 hio  = kHI + (u32)(bw ? i * 4 : i * 8);

			m.Ldr(w0, MemOperand(gpr, rs * 16 + word * 4));
			if (!bw)
				m.Ldr(w1, MemOperand(gpr, rt * 16 + word * 4));

			if (uw)
			{
				// PDIVUW is unsigned and has no overflow case: the only
				// result the divider would get wrong is division by zero,
				// which the EE defines as quotient -1 remainder dividend.
				m.Cbz(w1, &zero);
				m.Udiv(w2, w0, w1); m.Msub(w3, w2, w1, w0);
				m.B(&store);
				m.Bind(&zero);
				m.Mov(w2, -1); m.Mov(w3, w0);
				m.Bind(&store);
				m.Sxtw(x2, w2); m.Sxtw(x3, w3);
				m.Str(x2, MemOperand(gpr, loo));
				m.Str(x3, MemOperand(gpr, hio));
				continue;
			}

			m.Cbz(w1, &zero);                                       // divisor == 0
			m.Mov(w5, 0x80000000); m.Cmp(w0, w5); m.B(&normal, ne); // dividend == INT_MIN
			m.Cmn(w1, 1); m.B(&normal, ne);                         // && divisor == -1
			m.Mov(w2, 0x80000000); m.Mov(w3, wzr); m.B(&store);     // -> INT_MIN rem 0

			m.Bind(&normal);
			m.Sdiv(w2, w0, w1); m.Msub(w3, w2, w1, w0);             // q, rem = rs - q*rt
			m.B(&store);

			m.Bind(&zero);
			m.Mov(w6, 1); m.Mov(w7, -1); m.Cmp(w0, 0);
			m.Csel(w2, w6, w7, lt);                                 // q = (rs < 0) ? 1 : -1
			m.Mov(w3, w0);                                          // rem = rs

			m.Bind(&store);
			if (bw)
			{
				m.Str(w2, MemOperand(gpr, loo));
				m.Str(w3, MemOperand(gpr, hio));
			}
			else
			{
				m.Sxtw(x2, w2); m.Sxtw(x3, w3);
				m.Str(x2, MemOperand(gpr, loo));
				m.Str(x3, MemOperand(gpr, hio));
			}
		}
	}

	void EmitMmiHiLo(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		const int act = MmiHiLoAction(insn);
		const u32 rd = (insn >> 11) & 31, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
		switch (act)
		{
			case HL_PMFHI: case HL_PMFLO: // rd = HI / LO (full 128 bits)
				if (!rd) return;
				m.Ldr(q0, MemOperand(gpr, act == HL_PMFHI ? kHI : kLO));
				m.Str(q0, MemOperand(gpr, rd * 16));
				s_rc.Invalidate(rd); // 128-bit write bypasses StoreGpr
				return;

			case HL_PMTHI: case HL_PMTLO: // HI / LO = rs (full 128 bits)
				s_rc.FlushReg(m, gpr, rs); // rs low half may be dirty in the cache
				LoadQ(m, q0, gpr, rs);     // rs (r0 reads as zero)
				m.Str(q0, MemOperand(gpr, act == HL_PMTHI ? kHI : kLO));
				return;

			case HL_PPAC5: // rd.UL[n] = RGB1555 pack of rt.UL[n], for each of the 4 words
			{
				if (!rd) return;
				s_rc.FlushReg(m, gpr, rt);
				LoadQ(m, q1, gpr, rt); // rt -> v1
				// Field masks, splat across the 4 words (matches the interpreter's
				// shifts/masks exactly: 0x1f, 0x3e0, 0x7c00, 0x8000).
				m.Movi(v4.V4S(), 0x1f);
				m.Shl(v5.V4S(), v4.V4S(), 5);   // 0x3e0
				m.Shl(v6.V4S(), v4.V4S(), 10);  // 0x7c00
				m.Movi(v7.V4S(), 0x80, LSL, 8); // 0x8000
				m.Ushr(v0.V4S(), v1.V4S(), 3);  m.And(v0.V16B(), v0.V16B(), v4.V16B());
				m.Ushr(v2.V4S(), v1.V4S(), 6);  m.And(v2.V16B(), v2.V16B(), v5.V16B()); m.Orr(v0.V16B(), v0.V16B(), v2.V16B());
				m.Ushr(v2.V4S(), v1.V4S(), 9);  m.And(v2.V16B(), v2.V16B(), v6.V16B()); m.Orr(v0.V16B(), v0.V16B(), v2.V16B());
				m.Ushr(v2.V4S(), v1.V4S(), 16); m.And(v2.V16B(), v2.V16B(), v7.V16B()); m.Orr(v0.V16B(), v0.V16B(), v2.V16B());
				m.Str(q0, MemOperand(gpr, rd * 16));
				s_rc.Invalidate(rd);
				return;
			}
		}
	}

	// ---- Integer mult/div + HI/LO moves (opcode 0x00) ----
	// All bit-exact, side-effect-free integer ops on the low 32 bits, with the
	// 64-bit HI/LO results stored sign-extended, matching the interpreter exactly
	// (R5900OpcodeImpl.c MULT/MULTU/DIV/DIVU). HI/LO layout: see kHI/kLO above
	// (the mult/div path uses only their low 64 bits).

	// Load the low 32 bits of a GPR into a w-register (r0 reads as zero).
	inline void LoadW(MacroAssembler& m, const Register& wd, const Register& gpr, u32 idx)
	{
		if (idx == 0) { m.Mov(wd, 0); return; }
		if (RegCacheOn())
		{
			// A cached 64-bit value's W view is exactly the guest low 32 bits;
			// on a miss just load (no fill -- we only have half the value).
			const int s = s_rc.host_of[idx];
			if (s >= 0) { m.Mov(wd, CacheW(s)); return; }
		}
		m.Ldr(wd, MemOperand(gpr, idx * 16));
	}

	// off = 0 for the SPECIAL (pipeline 0) forms, 8 for the MMI pipeline-1
	// forms (MULT1/DIV1/MFHI1/...): HI1/LO1 are the upper 64 bits of HI/LO,
	// and the funct encodings line up exactly between the two op spaces.
	void EmitMulDiv(MacroAssembler& m, const Register& gpr, u32 funct, u32 rs, u32 rt, u32 rd, u32 off = 0)
	{
		const u32 kHIo = kHI + off, kLOo = kLO + off;
		switch (funct)
		{
			case 0x10: if (rd) { m.Ldr(x0, MemOperand(gpr, kHIo)); StoreGpr(m, x0, gpr, rd); } return; // MFHI
			case 0x12: if (rd) { m.Ldr(x0, MemOperand(gpr, kLOo)); StoreGpr(m, x0, gpr, rd); } return; // MFLO
			case 0x11: LoadGpr(m, x0, gpr, rs); m.Str(x0, MemOperand(gpr, kHIo)); return;              // MTHI
			case 0x13: LoadGpr(m, x0, gpr, rs); m.Str(x0, MemOperand(gpr, kLOo)); return;              // MTLO

			case 0x18: // MULT (signed)
			case 0x19: // MULTU (unsigned)
			{
				LoadW(m, w0, gpr, rs); LoadW(m, w1, gpr, rt);
				if (funct == 0x18) m.Smull(x0, w0, w1); else m.Umull(x0, w0, w1);
				m.Sxtw(x2, w0);                // LO = (s32)(res & 0xffffffff)
				m.Lsr(x3, x0, 32); m.Sxtw(x3, w3); // HI = (s32)(res >> 32)
				m.Str(x2, MemOperand(gpr, kLOo));
				m.Str(x3, MemOperand(gpr, kHIo));
				if (rd) StoreGpr(m, x2, gpr, rd); // rd = LO.UD[0]
				return;
			}
			case 0x1a: // DIV (signed)
			{
				LoadW(m, w0, gpr, rs); LoadW(m, w1, gpr, rt);
				Label zero, normal, store;
				m.Cbz(w1, &zero);
				m.Mov(w5, 0x80000000); m.Cmp(w0, w5); m.B(&normal, ne); // overflow: rs==INT_MIN
				m.Cmn(w1, 1); m.B(&normal, ne);                         // && rt==-1
				m.Mov(x2, (uint64_t)(int64_t)(s32)0x80000000); m.Mov(x3, 0); m.B(&store);
				m.Bind(&normal);
				m.Sdiv(w2, w0, w1); m.Msub(w4, w2, w1, w0);             // q, rem=rs-q*rt
				m.Sxtw(x2, w2); m.Sxtw(x3, w4); m.B(&store);
				m.Bind(&zero);                                          // div by 0
				m.Mov(w6, 1); m.Mov(w7, -1); m.Cmp(w0, 0); m.Csel(w2, w6, w7, lt); // LO=(rs<0)?1:-1
				m.Sxtw(x2, w2); m.Sxtw(x3, w0);                        // HI=sext(rs)
				m.Bind(&store);
				m.Str(x2, MemOperand(gpr, kLOo)); m.Str(x3, MemOperand(gpr, kHIo));
				return;
			}
			case 0x1b: // DIVU (unsigned)
			{
				LoadW(m, w0, gpr, rs); LoadW(m, w1, gpr, rt);
				Label zero, store;
				m.Cbz(w1, &zero);
				m.Udiv(w2, w0, w1); m.Msub(w4, w2, w1, w0);
				m.Sxtw(x2, w2); m.Sxtw(x3, w4); m.B(&store);
				m.Bind(&zero);
				m.Mov(x2, (uint64_t)-1); m.Sxtw(x3, w0);              // LO=-1, HI=sext(rs)
				m.Bind(&store);
				m.Str(x2, MemOperand(gpr, kLOo)); m.Str(x3, MemOperand(gpr, kHIo));
				return;
			}
		}
	}

	// MADD/MADDU (+ pipeline-1 MADD1/MADDU1, off=8): multiply-accumulate into
	// the 64-bit {HI.low32, LO.low32} accumulator, split back sign-extended.
	// Mirrors MMI.c MADD: temp = (u64)LO.UL[0] | ((u64)HI.UL[0]<<32) + rs*rt;
	// LO.SD = sext(temp[31:0]); HI.SD = sext(temp[63:32]); rd = LO.SD[0].
	void EmitMadd(MacroAssembler& m, const Register& gpr, bool is_unsigned, u32 rs, u32 rt, u32 rd, u32 off = 0)
	{
		const u32 kHIo = kHI + off, kLOo = kLO + off;
		m.Ldr(w0, MemOperand(gpr, kLOo));            // acc[31:0]  = LO.UL[0] (zero-ext)
		m.Ldr(w1, MemOperand(gpr, kHIo));            // acc[63:32] = HI.UL[0]
		m.Orr(x0, x0, Operand(x1, LSL, 32));         // acc
		LoadW(m, w2, gpr, rs); LoadW(m, w3, gpr, rt);
		if (is_unsigned) m.Umull(x2, w2, w3); else m.Smull(x2, w2, w3);
		m.Add(x0, x0, x2);                           // temp = acc + rs*rt
		m.Sxtw(x1, w0);                              // LO = sext(temp[31:0])
		m.Lsr(x2, x0, 32); m.Sxtw(x2, w2);           // HI = sext(temp[63:32])
		m.Str(x1, MemOperand(gpr, kLOo));
		m.Str(x2, MemOperand(gpr, kHIo));
		if (rd) StoreGpr(m, x1, gpr, rd);            // rd = LO.SD[0]
	}

	// Per-opcode cycle cost, mirroring R5900OpcodeTables.c exactly (Cycles::*):
	// Default=9, Branch=11, CopDefault=7, Mult=2*8=16, Div=14*8=112, Load=14,
	// Store=14, MMI_Default=14. Used to accumulate cpuBlockCycles accurately
	// (cpuBlockCycles += cost * (2 - CP0.Config[18]), summed over the block's ops).
	// NOTE: loads/stores/COP1-moves/MMI must NOT fall through to Default(9) --
	// undercounting their cost makes the JIT's cpuRegs.cycle lag the interpreter's,
	// so scheduled vsync/timer/DMA interrupts fire at different instruction
	// boundaries. That timing drift is what made MMX7 stall after the logo (the EE
	// idle loop 0x00081fc0 spins waiting for an interrupt that the JIT delivered
	// late relative to a NO_EE_MEM reference).
	int OpCycles(u32 insn)
	{
		const u32 op = insn >> 26, funct = insn & 0x3f;
		switch (op)
		{
			case 0x00: // SPECIAL
				if (funct == 0x18 || funct == 0x19) return 16;  // MULT/MULTU
				if (funct == 0x1a || funct == 0x1b) return 112; // DIV/DIVU
				return 9;                                       // ALU/shift/JR/JALR/MFHI...
			case 0x01:                                          // REGIMM
				// MTSAB/MTSAH are Default; the rest (BLTZ/BGEZ/B*ZAL/likely) Branch.
				if (((insn >> 16) & 31) == 0x18 || ((insn >> 16) & 31) == 0x19) return 9;
				return 11;                                      // Branch
			case 0x04: case 0x05: case 0x06: case 0x07:         // BEQ/BNE/BLEZ/BGTZ
			case 0x14: case 0x15: case 0x16: case 0x17:         // BEQL/BNEL/BLEZL/BGTZL
				return 11;                                      // Branch
			case 0x10:                                          // COP0
				if (((insn >> 21) & 31) == 0x08) return 11;     // BC0x (Branch)
				return 7;                                       // MFC0/MTC0 (CopDefault)
			case 0x11:                                          // COP1
				// Per R5900OpcodeTables.c: BC1F/T(L) = Branch(11); MUL/MULA/
				// MADD/MSUB/MADDA/MSUBA.S = FPU_Mult(4*8=32), DIV.S/SQRT.S =
				// 6*8=48, RSQRT.S = 8*8=64; everything else we translate (MFC1/
				// MTC1/MOV/NEG/ABS/ADD/SUB/ADDA/SUBA/MAX/MIN/CVT/C.cond) is
				// CopDefault(7).
				if (((insn >> 21) & 31) == 0x08) return 11;     // BC1x (Branch)
				if (((insn >> 21) & 31) == 0x10)
				{
					switch (insn & 0x3f)
					{
						case 0x02: case 0x1a:            // MUL.S/MULA.S
						case 0x1c: case 0x1d:            // MADD.S/MSUB.S
						case 0x1e: case 0x1f: return 32; // MADDA.S/MSUBA.S
						case 0x03: case 0x04: return 48; // DIV.S/SQRT.S
						case 0x16: return 64;            // RSQRT.S
					}
				}
				return 7;                                       // CopDefault
			case 0x1c:                                          // MMI
				if (funct == 0x18 || funct == 0x19) return 16;  // MULT1/MULTU1 (Mult)
				if (funct == 0x1a || funct == 0x1b) return 112; // DIV1/DIVU1 (Div)
				if (funct >= 0x10 && funct <= 0x13) return 9;   // MFHI1/MTHI1/MFLO1/MTLO1 (Default)
				if (funct == 0x00 || funct == 0x01 || funct == 0x20 || funct == 0x21) return 16; // MADD/MADDU(1) (Mult)
				return 14;                                      // MMI_Default
			case 0x1a: case 0x1b:                               // LDL/LDR
			case 0x1e:                                          // LQ
			case 0x20: case 0x21: case 0x22: case 0x23:         // LB/LH/LWL/LW
			case 0x24: case 0x25: case 0x26: case 0x27:         // LBU/LHU/LWR/LWU
			case 0x31:                                          // LWC1
			case 0x36:                                          // LQC2
			case 0x37:                                          // LD
				return 14;                                      // Load
			case 0x1f:                                          // SQ
			case 0x28: case 0x29: case 0x2a: case 0x2b:         // SB/SH/SWL/SW
			case 0x2c: case 0x2d: case 0x2e:                    // SDL/SDR/SWR
			case 0x39:                                          // SWC1
			case 0x3e:                                          // SQC2
			case 0x3f:                                          // SD
				return 14;                                      // Store
			default:
				return 9;                                       // Default (addi/ori/lui/J/JAL/daddi...)
		}
	}

	// Translate one simple integer ALU op (32- and 64-bit forms). Returns false,
	// emitting nothing, for control flow / memory / FPU / MMI / anything else.
	// C.18: execute an interpreter op implementation from inside the block.
	// Only for ops with contained side effects: no control flow, no exception,
	// and no reads of cpuRegs.pc or cpuBlockCycles (MFC0's Count uses
	// cpuRegs.cycle, which advances at branch boundaries identically in both
	// execution modes, so mid-block reads match the interpreter handoff).
	void EmitInterpOpCall(MacroAssembler& m, const Register& gpr, u32 insn, void (*fn)())
	{
		// The interp op body reads guest GPRs from memory (MTC0 rt, QFSRV
		// rs/rt) -- write the dirty cache back first...
		s_rc.FlushDirty(m, gpr);
		m.Mov(w0, insn);
		m.Str(w0, RegsField(&cpuRegs.code));
		m.Mov(x16, reinterpret_cast<uint64_t>(fn));
		m.Blr(x16);
		// ...and it may also write any guest GPR directly (MFC0 rt, QFSRV rd):
		// drop the whole cache afterwards.
		s_rc.Reset();
	}

	// C.78: adrp+pageoff fold for absolute data addresses -- the EE-rec twin of
	// AsmHelpers' armAbsMemOperand, which reads the armAsm thread-locals and is
	// only valid during the COP2-macro assembler handoff. Blocks are emitted in
	// place (masm is constructed over s_code + s_code_pos), so the current host
	// address is the buffer start plus the cursor. Falls back to the full
	// materialization when the page displacement does not encode or the page
	// offset is not size-aligned.
	MemOperand AbsMem(MacroAssembler& m, const Register& scratch, const void* addr, unsigned size)
	{
		const uintptr_t cur = reinterpret_cast<uintptr_t>(m.GetBuffer()->GetStartAddress<u8*>()) +
		                      m.GetBuffer()->GetCursorOffset();
		const s64 page_disp = (static_cast<s64>(reinterpret_cast<uintptr_t>(addr) & ~uintptr_t(0xFFF)) -
		                       static_cast<s64>(cur & ~uintptr_t(0xFFF))) >> 12;
		const u32 page_off = static_cast<u32>(reinterpret_cast<uintptr_t>(addr) & 0xFFFu);
		if (vixl::IsInt21(page_disp) && size && (page_off % size) == 0)
		{
			{
				vixl::aarch64::SingleEmissionCheckScope guard(&m);
				m.adrp(scratch, page_disp);
			}
			return MemOperand(scratch, page_off);
		}
		m.Mov(scratch, reinterpret_cast<uint64_t>(addr));
		return MemOperand(scratch);
	}

	// C.30-2: native COP2 macro emission. The vendored aVU_Macro emitters gate
	// flag maintenance on g_pCurInstInfo's M1 analysis bits; we run no analysis
	// pass, so a static "everything live" record keeps every gate conservative
	// (always denormalize/update/normalize the flags -- correct, just not
	// minimal). The emitters write through the AsmHelpers thread-locals, so we
	// hand them OUR in-flight assembler for the duration of the op: armAsmPtr
	// must be the block buffer's start so armGetCurrentCodePointer() (used for
	// ADRP/call displacement math) computes true host addresses.
	EEINST s_cop2AllLive; // .info set on first use

} // pause the anonymous namespace: cop2flags has a global declaration

// C.66: verbatim copy of x86 ix86-32/iR5900-32.cpp cop2flags() -- that TU is not
// in the arm64 build. Returns a bitmask of the flags a COP2 macro op WRITES:
// 1 = status, 2 = MAC, 4 = clip; 0 for non-flag ops, branches and transfers.
// (Declared in arm64/aR5900Analysis.h.)
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

namespace {
	// ---- C.66: run-local COP2 flag liveness ----
	// The aVU macro emitters already gate every piece of flag maintenance on
	// g_pCurInstInfo's analysis bits (CHECK_VU_FLAGHACK is default-on), but we
	// fed them a static "everything live" record, so every VMUL/VMADD in GT3's
	// VU0-macro matrix code carried the full MAC+status pipeline -- fcmeq/sshr/
	// addv/fmov plus the sticky-fold, ~40 host instructions per op, most of the
	// hottest block's bytes. The x86 rec computes real liveness with
	// COP2FlagHackPass over the whole block; our builder interleaves analysis
	// and emission, so the block end isn't known up front. Instead the analysis
	// runs over a RUN: the maximal sequence of consecutive native macro ALU ops
	// starting at the op being emitted, capped by the block's remaining
	// instruction budget (a run never crosses the block end). Anything else --
	// CFC2/CTC2, transfers, stores, branches, CALLMS, a skipped op -- ends the
	// run, and the last writer of each flag category before it commits, exactly
	// like the x86 pass commits at reads/hazards/block end:
	//   * status: first writer DENORMALIZEs into gprF0, dead intermediate
	//     writers skip flag work entirely (their sticky contribution is the
	//     documented flag-hack trade), the last writer computes + NORMALIZEs.
	//     All writers stay live when the run is bracketed by the Tekken-style
	//     CTC2-to-STATUS ... CFC2-from-STATUS pattern (sticky accuracy), and
	//     DIV/SQRT/RSQRT compute unconditionally, both as in x86.
	//   * MAC: only the last writer computes (MAC is last-op state, not sticky).
	//   * clip: every writer computes (x86 does not track clip liveness either).
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

	// ---- C.67: fused unaligned pairs ----
	// The canonical MIPS unaligned idioms are two-op sequences that fully
	// overwrite their target: LDL rt,off+7(b); LDR rt,off(b) is an unaligned
	// 64-bit load from b+off, and LWL/LWR, SDL/SDR, SWL/SWR likewise. Each half
	// used to cost a full helper call (address materialization + blr + the
	// helper's masked merge) -- GT3's hot in-race block opens with two such
	// pairs. ARM does unaligned accesses natively on normal memory, and the
	// fastmem area lays guest pages out contiguously at their guest addresses,
	// so the pair fuses into the same one-instruction patchable access the
	// aligned ops use (crossing a guest page stays contiguous by construction;
	// an unmapped/hw page faults into the stub, whose wrapper replays the exact
	// two-access guest-order semantics). Only the canonical left-then-right
	// order fuses; loads additionally require rt != rs (the left op would
	// clobber the base mid-pair) and rt != 0 (the helpers' discard semantics).
	// The vmap path never fuses: its one-page lookup cannot vouch for host
	// contiguity across a guest page boundary.
	struct UPair { bool load, is64; u32 rt, rs; s32 off; };
	inline bool UnalignedPair(u32 a, u32 b, UPair* out)
	{
		const u32 opa = a >> 26, opb = b >> 26;
		const u32 rta = (a >> 16) & 31, rtb = (b >> 16) & 31;
		const u32 rsa = (a >> 21) & 31, rsb = (b >> 21) & 31;
		const s32 offa = (int16_t)(a & 0xffff), offb = (int16_t)(b & 0xffff);
		if (rta != rtb || rsa != rsb)
			return false;
		bool load, is64;
		if      (opa == 0x1a && opb == 0x1b) { load = true;  is64 = true;  if (offa != offb + 7) return false; }
		else if (opa == 0x22 && opb == 0x26) { load = true;  is64 = false; if (offa != offb + 3) return false; }
		else if (opa == 0x2c && opb == 0x2d) { load = false; is64 = true;  if (offa != offb + 7) return false; }
		else if (opa == 0x2a && opb == 0x2e) { load = false; is64 = false; if (offa != offb + 3) return false; }
		else return false;
		if (load && (rta == 0 || rta == rsa))
			return false;
		*out = {load, is64, rta, rsa, offb};
		return true;
	}

	void EmitUnalignedPair(MacroAssembler& m, const Register& gpr, const UPair& u)
	{
		{ const Register a = SrcGpr(m, gpr, u.rs, x0); m.Add(w0, a.W(), u.off); }
		if (u.load)
		{
			const uint64_t fn = u.is64 ? reinterpret_cast<uint64_t>(&eeReadU64_arm64)
			                           : reinterpret_cast<uint64_t>(&eeReadU32_arm64);
			if (FastmemOk(s_emit_pc))
			{
				EmitFastmemAccess(m, [&] {
					if (u.is64) m.ldr(x0, MemOperand(vmapbase, w0, UXTW));
					else        m.ldr(w0, MemOperand(vmapbase, w0, UXTW));
				});
				DeferFastmemStub(m, fn, 0, 0);
			}
			else { m.Mov(x16, fn); m.Blr(x16); }
			if (!u.is64)
				m.Sxtw(x0, w0); // LWL/LWR pairs sign-extend, exactly like LW
			StoreGpr(m, x0, gpr, u.rt);
		}
		else
		{
			LoadGpr(m, x1, gpr, u.rt);
			const uint64_t fn = u.is64 ? reinterpret_cast<uint64_t>(&eeWriteU64_arm64)
			                           : reinterpret_cast<uint64_t>(&eeWriteU32_arm64);
			if (FastmemOk(s_emit_pc))
			{
				EmitFastmemAccess(m, [&] {
					if (u.is64) m.str(x1, MemOperand(vmapbase, w0, UXTW));
					else        m.str(w1, MemOperand(vmapbase, w0, UXTW));
				});
				DeferFastmemStub(m, fn, 0, 0);
			}
			else { m.Mov(x16, fn); m.Blr(x16); }
		}
	}


	// C.78: compile-time state of the run-hoisted macro bracket: true while the
	// emitted code has x19 == &vuRegs[0] (and x27 == &microVU0) left over from
	// a previous macro op of the same run whose endMacroOp skipped the restore.
	bool s_x19HeldVu = false;

	// C.78: force-close a held bracket -- repoint x19 back at cpuRegs so the
	// code emitted next (interp call, block tail) sees the EE base again. Cold
	// path: only reachable if an op inside an analyzed run refuses native
	// emission, which the shared predicate (Cop2NativeMacroAlu vs the gates
	// below) is supposed to rule out. Invalidate the run so the next macro op
	// re-analyzes and re-opens a fresh bracket (with its VU0 sync).
	void Cop2CloseHold(MacroAssembler& m, const Register& gpr)
	{
		if (!s_x19HeldVu)
			return;
		m.Mov(gpr, reinterpret_cast<uint64_t>(&cpuRegs));
		s_x19HeldVu = false;
		Cop2RunInvalidate();
	}

	bool EmitCop2Macro(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		if (!recVUMacroIsMode0(insn))
			{ Cop2CloseHold(m, gpr); return false; } // CALLMS/CALLMSR/unknown -> C.29-1 interp call
		// VCLIP stays on the interpreter call PERMANENTLY (C.30-4 root cause,
		// GT3 black road): the interpreter's macro path keeps the live clip
		// shift register in VU->clipflag and commits it to VI[REG_CLIP_FLAG]
		// through the FMAC pipeline -- i.e. VI lags the live value by one op.
		// GT3 reads the flag via CFC2 tens of thousands of times per race and
		// depends on that delayed view; the native emitter's immediate
		// read-shift-write on VI hands CFC2 a value shifted 6 bits too far
		// and the road-chunk culling goes wrong. A native version would have
		// to replicate the delayed commit (VI = old clipflag; clipflag = new)
		// -- not worth it for an op this rare next to the ALU stream.
		if ((insn & 0x3f) >= 0x3c && ((insn & 3) | ((insn >> 4) & 0x7c)) == 0x1f)
			{ Cop2CloseHold(m, gpr); return false; }

		s_cop2AllLive.info = EEINST_COP2_DENORMALIZE_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG |
		                     EEINST_COP2_STATUS_FLAG | EEINST_COP2_MAC_FLAG | EEINST_COP2_CLIP_FLAG |
		                     EEINST_COP2_SYNC_VU0 | EEINST_COP2_FINISH_VU0 | EEINST_COP2_FLUSH_VU0_REGISTERS;
		EEINST* saved_inst = g_pCurInstInfo;
		// C.66: real per-op liveness from the run analysis; the static all-live
		// record only remains for an
		// op the analyzer would not include (should not happen -- the same
		// predicate guards both). C.78: the analysis moved ahead of the VU0
		// sync + cache retire so the run boundaries can gate them too.
		g_pCurInstInfo = &s_cop2AllLive;
		bool in_run = false;
		if (s_emit_pc < s_cop2RunStart || s_emit_pc >= s_cop2RunEnd)
			Cop2AnalyzeRun(s_emit_pc);
		if (s_emit_pc >= s_cop2RunStart && s_emit_pc < s_cop2RunEnd)
		{
			g_pCurInstInfo = &s_cop2Run[(s_emit_pc - s_cop2RunStart) >> 2];
			in_run = true;
		}
		// C.78: hoist the per-op bracket overhead to once per run. First op of
		// a run: VU0 sync + x19/x27 materialization (setupMacroOp reads
		// g_cop2MacroFirst); ops after it skip both -- between the ops of a run
		// nothing else is emitted, no event test runs (events fire at block
		// tails), and no run op can start a VU0 microprogram (CALLMS and CTC2
		// CMSAR1 are excluded from runs by the analyzer), so VU0 stays idle and
		// x19/x27 stay valid for the run's whole emitted extent. The last op
		// restores x19 in endMacroOp (g_cop2MacroLast).
		const bool hoist = in_run;
		const bool first = !hoist || !s_x19HeldVu;
		const bool last  = !hoist || (s_emit_pc + 4 >= s_cop2RunEnd);
		g_cop2MacroFirst = first;
		g_cop2MacroLast  = last;

		// The macro emitters clobber gprF0 (w23 -- one of OUR cache slots) and
		// run their own VI-GPR allocator (macro mode excludes x19-x26; x27 is
		// untracked but our cache is dead after this anyway): retire the guest
		// GPR cache exactly like an interp-call would. Mid-run this emits
		// nothing (the first op already retired it and nothing refilled it).
		s_rc.FlushDirty(m, gpr);
		s_rc.Reset();

		// Conservative VU0 sync (their aR5900 does this analysis-driven): a
		// macro op reads/writes VU0 state, so any in-flight VU0 microprogram
		// must finish first. The interpreter ops self-sync the same way.
		// C.78: once per run (see above).
		if (first)
		{
			Label vu0_idle;
			m.Ldr(w0, AbsMem(m, x10, &vuRegs[0].VI[REG_VPU_STAT].UL, 4)); // C.78: adrp fold
			m.Tbz(w0, 0, &vu0_idle); // VPU_STAT bit0 = VU0 running
			m.Mov(x16, reinterpret_cast<uint64_t>(&_vu0FinishMicro));
			m.Blr(x16);
			m.Bind(&vu0_idle);
		}
		cpuRegs.code = insn; // emit-time input: the emitters' _Fs_/_Ft_/... macros read it

		MacroAssembler* saved_asm = armAsm;
		u8* saved_ptr = armAsmPtr;
		size_t saved_cap = armAsmCapacity;
		armAsm = &m;
		armAsmPtr = m.GetBuffer()->GetStartAddress<u8*>();
		armAsmCapacity = m.GetBuffer()->GetCapacity();

		const bool ok = recVUMacroEmitMode0(insn);

		armAsm = saved_asm;
		armAsmPtr = saved_ptr;
		armAsmCapacity = saved_cap;
		g_pCurInstInfo = saved_inst;
		// C.78: a non-last run op skipped its endMacroOp restore -- record that
		// the emitted stream is mid-bracket. A dispatch miss (should not happen
		// after recVUMacroIsMode0) emits nothing, so a bracket held from a
		// previous op must be closed before the interp-call fallback.
		if (ok)
			s_x19HeldVu = hoist && !last;
		else
			Cop2CloseHold(m, gpr);
		return ok;
	}

	// ---- C.58: native COP2 transfers (QMFC2 / CFC2 / QMTC2 / CTC2) ----
	//
	// Once the VU0 syncs are done these are plain register moves, but they were
	// still going out through EmitInterpOpCall, which flushes the GPR register
	// cache before the call and drops the whole thing afterwards -- expensive in
	// a COP2-heavy block. Emit them natively instead, mirroring VU0.c exactly:
	//
	//   * every one of them calls vu0Sync() first;
	//   * the interlock bit is cpuRegs.code & 1, which we know while COMPILING,
	//     so the _vu0FinishMicro/_vu0WaitMicro call is only emitted when it is
	//     actually set (it usually isn't);
	//   * CFC2's REG_R case writes only UL[0] and leaves UL[1] alone, while the
	//     general case sign-extends into UL[1] -- i.e. a 64-bit signed store. The
	//     upper 64 bits of the 128-bit GPR are untouched in both, so the write is
	//     a read-modify-write of the low half, not a StoreGpr;
	//   * CTC2 to FBRST or CMSAR1 resets a VU / kicks off a VU1 microprogram, and
	//     MAC_FLAG/TPC/VPU_STAT are read-only. The side-effect pair stays on the
	//     interpreter call (return false); the read-only ones become no-ops (but
	//     still sync).
	//
	// vu0Sync() reads cpuRegs.cycle, which inside a JIT block only advances at
	// block boundaries -- exactly as it did through the interpreter call, so this
	// changes nothing about VU0 timing (same argument as MFC0 Count, C.18).
	// Must agree with EmitCop2Transfer exactly: IsTranslatable and the emitter
	// have to make the same call, or the block builder and the delay-slot path
	// disagree about where a block ends.
	inline bool Cop2XferSupported(u32 insn)
	{
		const u32 rs = (insn >> 21) & 31;
		const u32 fs = (insn >> 11) & 31;
		if (rs != 0x01 && rs != 0x02 && rs != 0x05 && rs != 0x06)
			return false; // CALLMS/CALLMSR and friends stay interpreted
		// CTC2 to FBRST resets a VU and to CMSAR1 kicks off a VU1 microprogram:
		// side effects not worth open-coding.
		if (rs == 0x06 && fs != 0 && (fs == REG_FBRST || fs == REG_CMSAR1))
			return false;
		return true;
	}

	bool EmitCop2Transfer(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		if (!Cop2XferSupported(insn))
			return false;

		const u32 rs = (insn >> 21) & 31; // COP2 sub-op
		const u32 rt = (insn >> 16) & 31;
		const u32 fs = (insn >> 11) & 31; // rd slot
		const bool interlock = (insn & 1) != 0;

		const bool is_read = (rs == 0x01 || rs == 0x02); // QMFC2 / CFC2

		m.Mov(x16, reinterpret_cast<uint64_t>(&vu0Sync));
		m.Blr(x16);
		if (interlock)
		{
			m.Mov(x16, is_read ? reinterpret_cast<uint64_t>(&_vu0FinishMicro)
			                   : reinterpret_cast<uint64_t>(&_vu0WaitMicro));
			m.Blr(x16);
		}

		switch (rs)
		{
			case 0x01: // QMFC2 rt, fs: GPR[rt].UQ = VF[fs].UQ
				if (!rt) return true;
				m.Ldr(q0, AbsMem(m, x10, &vuRegs[0].VF[fs], 16)); // C.78: adrp fold
				m.Str(q0, MemOperand(gpr, rt * 16));
				s_rc.Invalidate(rt); // 128-bit write bypasses StoreGpr
				return true;

			case 0x05: // QMTC2 rt, fs: VF[fs].UQ = GPR[rt].UQ  (fs == 0 is a no-op)
				if (!fs) return true;
				s_rc.FlushReg(m, gpr, rt); // the 128-bit source is read from memory
				m.Ldr(q0, MemOperand(gpr, rt * 16));
				m.Str(q0, AbsMem(m, x10, &vuRegs[0].VF[fs], 16)); // C.78: adrp fold
				return true;

			case 0x02: // CFC2 rt, fs
			{
				if (!rt) return true;
				m.Ldr(w0, AbsMem(m, x10, &vuRegs[0].VI[fs], 4)); // C.78: adrp fold
				s_rc.FlushReg(m, gpr, rt); // read-modify-write of GPR memory below
				if (fs == REG_R)
				{
					// UL[0] = VI[R] & 0x7fffff; UL[1] untouched.
					m.And(w0, w0, 0x7fffff);
					m.Ldr(x1, MemOperand(gpr, rt * 16));
					m.Bfi(x1, x0, 0, 32);
					m.Str(x1, MemOperand(gpr, rt * 16));
				}
				else
				{
					// UL[0] = VI[fs]; UL[1] = sign of it -- a 64-bit signed store.
					m.Sxtw(x0, w0);
					m.Str(x0, MemOperand(gpr, rt * 16));
				}
				s_rc.Invalidate(rt);
				return true;
			}

			case 0x06: // CTC2 rt, fs
			{
				if (!fs) return true;
				if (fs == REG_MAC_FLAG || fs == REG_TPC || fs == REG_VPU_STAT)
					return true; // read-only: the syncs above are the whole op
				const Register src = SrcGpr(m, gpr, rt, x0);
				const MemOperand vi = AbsMem(m, x10, &vuRegs[0].VI[fs], 4); // C.78: adrp fold
				if (fs == REG_R)
				{
					m.And(w1, src.W(), 0x7fffff);
					m.Orr(w1, w1, 0x3f800000);
					m.Str(w1, vi);
				}
				else
				{
					m.Str(src.W(), vi);
				}
				return true;
			}
		}
		return false;
	}

	// QFSRV is MMI1 (funct 0x28) sub 0x1b -- NOT MMI3 (0x29), whose sub 0x1b is
	// PCPYH. The first C.18 cut called QFSRV() on PCPYH instructions (the
	// GT3-attract breaker at funct=29/sub=1b) and broke MMX7's post-logo
	// progression -- same shifted-table bug class as the C.10 DADDU miscompile.
	inline bool IsQfsrv(u32 insn) { return (insn & 0x3f) == 0x28 && ((insn >> 6) & 0x1f) == 0x1b; }
	inline bool IsPcpyh(u32 insn) { return (insn & 0x3f) == 0x29 && ((insn >> 6) & 0x1f) == 0x1b; }

	bool EmitSimple(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		s_rc.pinned = 0; // new op: previous op's Register handles are dead
		const u32 op = insn >> 26;
		const u32 rs = (insn >> 21) & 31, rt = (insn >> 16) & 31, rd = (insn >> 11) & 31, sa = (insn >> 6) & 31;
		const u32 funct = insn & 0x3f;
		const int32_t simm = (int16_t)(insn & 0xffff);
		const uint64_t simm64 = (uint64_t)(int64_t)simm;
		const u32 zimm = insn & 0xffff;

		switch (op)
		{
			case 0x00:
				switch (funct)
				{
					// 32-bit shifts -> sign-extend to 64 (direct-to-cache, C.27-3)
					case 0x00: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), d = DstGpr(m, gpr, rd, x0); m.Lsl(d.W(), a.W(), sa); m.Sxtw(d, d.W()); FinishDst(m, gpr, rd, d); } return true; // SLL
					case 0x02: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), d = DstGpr(m, gpr, rd, x0); m.Lsr(d.W(), a.W(), sa); m.Sxtw(d, d.W()); FinishDst(m, gpr, rd, d); } return true; // SRL
					case 0x03: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), d = DstGpr(m, gpr, rd, x0); m.Asr(d.W(), a.W(), sa); m.Sxtw(d, d.W()); FinishDst(m, gpr, rd, d); } return true; // SRA
					case 0x04: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), b = SrcGpr(m, gpr, rs, x1), d = DstGpr(m, gpr, rd, x0); m.Lsl(d.W(), a.W(), b.W()); m.Sxtw(d, d.W()); FinishDst(m, gpr, rd, d); } return true; // SLLV
					case 0x06: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), b = SrcGpr(m, gpr, rs, x1), d = DstGpr(m, gpr, rd, x0); m.Lsr(d.W(), a.W(), b.W()); m.Sxtw(d, d.W()); FinishDst(m, gpr, rd, d); } return true; // SRLV
					case 0x07: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), b = SrcGpr(m, gpr, rs, x1), d = DstGpr(m, gpr, rd, x0); m.Asr(d.W(), a.W(), b.W()); m.Sxtw(d, d.W()); FinishDst(m, gpr, rd, d); } return true; // SRAV
					// 32-bit add/sub -> sign-extend to 64
					// ADD/SUB trap on signed overflow; the x86 rec does not emulate that
					// exception and neither do we for ADDI/DADDI, so they are ADDU/SUBU.
					case 0x20:
					case 0x21: if (rd) { const Register a = SrcGpr(m, gpr, rs, x0), b = SrcGpr(m, gpr, rt, x1), d = DstGpr(m, gpr, rd, x0); m.Add(d.W(), a.W(), b.W()); m.Sxtw(d, d.W()); FinishDst(m, gpr, rd, d); } return true; // ADD/ADDU
					case 0x22:
					case 0x23: if (rd) { const Register a = SrcGpr(m, gpr, rs, x0), b = SrcGpr(m, gpr, rt, x1), d = DstGpr(m, gpr, rd, x0); m.Sub(d.W(), a.W(), b.W()); m.Sxtw(d, d.W()); FinishDst(m, gpr, rd, d); } return true; // SUB/SUBU
					// 64-bit logic / set / add / sub
					case 0x24: if (rd) { const Register a = SrcGpr(m, gpr, rs, x0), b = SrcGpr(m, gpr, rt, x1), d = DstGpr(m, gpr, rd, x0); m.And(d, a, b); FinishDst(m, gpr, rd, d); } return true; // AND
					case 0x25: if (rd) { const Register a = SrcGpr(m, gpr, rs, x0), b = SrcGpr(m, gpr, rt, x1), d = DstGpr(m, gpr, rd, x0); m.Orr(d, a, b); FinishDst(m, gpr, rd, d); } return true; // OR
					case 0x26: if (rd) { const Register a = SrcGpr(m, gpr, rs, x0), b = SrcGpr(m, gpr, rt, x1), d = DstGpr(m, gpr, rd, x0); m.Eor(d, a, b); FinishDst(m, gpr, rd, d); } return true; // XOR
					case 0x27: if (rd) { const Register a = SrcGpr(m, gpr, rs, x0), b = SrcGpr(m, gpr, rt, x1), d = DstGpr(m, gpr, rd, x0); m.Orr(d, a, b); m.Mvn(d, d); FinishDst(m, gpr, rd, d); } return true; // NOR
					case 0x2a: if (rd) { const Register a = SrcGpr(m, gpr, rs, x0), b = SrcGpr(m, gpr, rt, x1), d = DstGpr(m, gpr, rd, x0); m.Cmp(a, b); m.Cset(d, lt); FinishDst(m, gpr, rd, d); } return true; // SLT
					case 0x2b: if (rd) { const Register a = SrcGpr(m, gpr, rs, x0), b = SrcGpr(m, gpr, rt, x1), d = DstGpr(m, gpr, rd, x0); m.Cmp(a, b); m.Cset(d, lo); FinishDst(m, gpr, rd, d); } return true; // SLTU
					// funct map: 0x2c=DADD 0x2d=DADDU 0x2e=DSUB 0x2f=DSUBU (the
					// interpreter's DADD/DSUB don't trap on overflow, so both
					// pairs emit the same Add/Sub). 0x2d used to emit Sub --
					// every real DADDU rd,rs,rt computed rs-rt; it survived boot
					// because `move rd,rs` == `daddu rd,rs,$zero` where rs-0 is
					// accidentally correct (MMX7 post-logo stall, C.9).
					case 0x2c:
					case 0x2d: if (rd) { const Register a = SrcGpr(m, gpr, rs, x0), b = SrcGpr(m, gpr, rt, x1), d = DstGpr(m, gpr, rd, x0); m.Add(d, a, b); FinishDst(m, gpr, rd, d); } return true; // DADD/DADDU
					case 0x2e:
					case 0x2f: if (rd) { const Register a = SrcGpr(m, gpr, rs, x0), b = SrcGpr(m, gpr, rt, x1), d = DstGpr(m, gpr, rd, x0); m.Sub(d, a, b); FinishDst(m, gpr, rd, d); } return true; // DSUB/DSUBU
					// 64-bit shifts (variable)
					case 0x14: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), b = SrcGpr(m, gpr, rs, x1), d = DstGpr(m, gpr, rd, x0); m.Lsl(d, a, b); FinishDst(m, gpr, rd, d); } return true; // DSLLV
					case 0x16: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), b = SrcGpr(m, gpr, rs, x1), d = DstGpr(m, gpr, rd, x0); m.Lsr(d, a, b); FinishDst(m, gpr, rd, d); } return true; // DSRLV
					case 0x17: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), b = SrcGpr(m, gpr, rs, x1), d = DstGpr(m, gpr, rd, x0); m.Asr(d, a, b); FinishDst(m, gpr, rd, d); } return true; // DSRAV
					// 64-bit shifts (immediate)
					case 0x38: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), d = DstGpr(m, gpr, rd, x0); m.Lsl(d, a, sa);      FinishDst(m, gpr, rd, d); } return true; // DSLL
					case 0x3a: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), d = DstGpr(m, gpr, rd, x0); m.Lsr(d, a, sa);      FinishDst(m, gpr, rd, d); } return true; // DSRL
					case 0x3b: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), d = DstGpr(m, gpr, rd, x0); m.Asr(d, a, sa);      FinishDst(m, gpr, rd, d); } return true; // DSRA
					case 0x3c: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), d = DstGpr(m, gpr, rd, x0); m.Lsl(d, a, sa + 32); FinishDst(m, gpr, rd, d); } return true; // DSLL32
					case 0x3e: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), d = DstGpr(m, gpr, rd, x0); m.Lsr(d, a, sa + 32); FinishDst(m, gpr, rd, d); } return true; // DSRL32
					case 0x3f: if (rd) { const Register a = SrcGpr(m, gpr, rt, x0), d = DstGpr(m, gpr, rd, x0); m.Asr(d, a, sa + 32); FinishDst(m, gpr, rd, d); } return true; // DSRA32
					// HI/LO moves + integer mult/div (bit-exact, sign-extended results)
					case 0x10: case 0x11: case 0x12: case 0x13: // MFHI/MTHI/MFLO/MTLO
					case 0x18: case 0x19: case 0x1a: case 0x1b: // MULT/MULTU/DIV/DIVU
						EmitMulDiv(m, gpr, funct, rs, rt, rd); return true;
					// SYNC is architecturally a barrier; the interpreter's SYNC()
					// body is empty, so it translates to pure cycle bookkeeping.
					case 0x0f: return true;
					// MFSA/MTSA: the shift-amount register (also written by
					// MTSAB/MTSAH, read by QFSRV). MFSA zero-extends sa into
					// rd (interp: GPR[rd].UD[0] = (u64)cpuRegs.sa).
					case 0x28:
						if (rd)
						{
							const Register d = DstGpr(m, gpr, rd, x0);
							m.Ldr(d.W(), RegsField(&cpuRegs.sa)); // 32-bit load zero-extends
							FinishDst(m, gpr, rd, d);
						}
						return true;
					case 0x29:
						// MTSA: sa is four bits (a byte count for QFSRV), so
						// mask like recMTSA and the interpreter do.
						LoadGpr(m, x0, gpr, rs);
						m.And(w0, w0, 0xf);
						m.Str(w0, RegsField(&cpuRegs.sa));
						return true;
					// MOVZ/MOVN: rd = rs when rt ==/!= 0 (full 64-bit compare),
					// rd unchanged otherwise.
					case 0x0a:
					case 0x0b:
						if (rd)
						{
							const Register a = SrcGpr(m, gpr, rs, x0);
							const Register b = SrcGpr(m, gpr, rt, x1);
							const Register dold = SrcGpr(m, gpr, rd, x2); // OLD rd value
							const Register d = DstGpr(m, gpr, rd, x2);    // same slot / same scratch
							m.Cmp(b, 0);
							m.Csel(d, a, dold, funct == 0x0a ? eq : ne);
							FinishDst(m, gpr, rd, d);
						}
						return true;
					default: return false;
				}
			// ADDI/DADDI (0x08/0x18): the R5900's overflow trap is dropped, exactly
			// like the upstream x86 recompiler (recADDIU just calls recADDI, which
			// never checks overflow -- no game relies on the exception). So they are
			// emitted identically to ADDIU/DADDIU. C.42.
			case 0x08: // ADDI
			// C.57: fold the immediate into the add (VIXL materializes a scratch
			// itself if it doesn't fit an add/sub imm, i.e. never worse), and skip
			// the add entirely when rs is r0 -- `li` is everywhere and cost three
			// instructions: a zero into a scratch, the immediate into another, then
			// the add. Safe for the same reason as C.48: SrcGpr(r0) hands back a
			// scratch holding zero, never xzr (xzr in the Rn slot of an immediate
			// add encodes SP).
			case 0x09: if (rt) {
				// NB: the source must be claimed BEFORE the destination -- for the
				// very common rs == rt form, DstGpr would otherwise hand back a
				// slot it never loaded and SrcGpr would read garbage out of it.
				if (!rs) { const Register d = DstGpr(m, gpr, rt, x0); m.Mov(d, static_cast<int64_t>(simm)); FinishDst(m, gpr, rt, d); }
				else { const Register a = SrcGpr(m, gpr, rs, x0), d = DstGpr(m, gpr, rt, x0); m.Add(d.W(), a.W(), simm); m.Sxtw(d, d.W()); FinishDst(m, gpr, rt, d); }
				} return true; // ADDIU
			case 0x18: // DADDI
			case 0x19: if (rt) { // C.57 (same source-before-destination rule as ADDIU)
				if (!rs) { const Register d = DstGpr(m, gpr, rt, x0); m.Mov(d, simm64); FinishDst(m, gpr, rt, d); }
				else { const Register a = SrcGpr(m, gpr, rs, x0), d = DstGpr(m, gpr, rt, x0); m.Add(d, a, simm64); FinishDst(m, gpr, rt, d); }
				} return true; // DADDIU
			case 0x0a: if (rt) { const Register a = SrcGpr(m, gpr, rs, x0), d = DstGpr(m, gpr, rt, x0); m.Mov(x1, simm64); m.Cmp(a, x1); m.Cset(d, lt); FinishDst(m, gpr, rt, d); } return true; // SLTI
			case 0x0b: if (rt) { const Register a = SrcGpr(m, gpr, rs, x0), d = DstGpr(m, gpr, rt, x0); m.Mov(x1, simm64); m.Cmp(a, x1); m.Cset(d, lo); FinishDst(m, gpr, rt, d); } return true; // SLTIU
			case 0x0c: if (rt) { const Register a = SrcGpr(m, gpr, rs, x0), d = DstGpr(m, gpr, rt, x0); m.Mov(x1, (uint64_t)zimm); m.And(d, a, x1); FinishDst(m, gpr, rt, d); } return true; // ANDI
			case 0x0d: if (rt) { const Register a = SrcGpr(m, gpr, rs, x0), d = DstGpr(m, gpr, rt, x0); m.Mov(x1, (uint64_t)zimm); m.Orr(d, a, x1); FinishDst(m, gpr, rt, d); } return true; // ORI
			case 0x0e: if (rt) { const Register a = SrcGpr(m, gpr, rs, x0), d = DstGpr(m, gpr, rt, x0); m.Mov(x1, (uint64_t)zimm); m.Eor(d, a, x1); FinishDst(m, gpr, rt, d); } return true; // XORI
			case 0x0f: if (rt) { const Register d = DstGpr(m, gpr, rt, x0); m.Mov(d.W(), zimm << 16); m.Sxtw(d, d.W()); FinishDst(m, gpr, rt, d); } return true; // LUI

			// REGIMM (0x01): only MTSAB/MTSAH here -- the branches (BLTZ/BGEZ/
			// B*ZAL/likely) are handled by EmitBranch. MTSAB/MTSAH write the
			// shift-amount register cpuRegs.sa (read by QFSRV). rt selects the op.
			case 0x01:
			{
				const u32 rtf = (insn >> 16) & 31;
				if (rtf == 0x18) // MTSAB: sa = (rs.UL[0] & 0xF) ^ (imm & 0xF)
				{
					LoadGpr(m, x0, gpr, rs); m.And(w0, w0, 0xF);
					m.Eor(w0, w0, (u32)(zimm & 0xF));
					m.Str(w0, RegsField(&cpuRegs.sa));
					return true;
				}
				if (rtf == 0x19) // MTSAH: sa = ((rs.UL[0] & 0x7) ^ (imm & 0x7)) << 1
				{
					LoadGpr(m, x0, gpr, rs); m.And(w0, w0, 0x7);
					m.Eor(w0, w0, (u32)(zimm & 0x7));
					m.Lsl(w0, w0, 1);
					m.Str(w0, RegsField(&cpuRegs.sa));
					return true;
				}
				return false; // branches -> EmitBranch
			}

			// Loads (LB/LBU/LH/LHU/LW/LWU/LD). Address = rs.UL[0] + simm (32-bit).
			// The interpreter cancels (fastjmp) on a misaligned LH/LW/LD, so do the
			// same; reads run even when rt==0 (side effects). Memory goes through
			// the EE vtlb wrappers. gpr (x19) is callee-saved across the calls.
			case 0x20: case 0x24: case 0x21: case 0x25: case 0x23: case 0x27: case 0x37:
			{
				{ const Register a = SrcGpr(m, gpr, rs, x0); m.Add(w0, a.W(), simm); }
				const u32 amask = (op == 0x37) ? 7 : (op == 0x23 || op == 0x27) ? 3 : (op == 0x21 || op == 0x25) ? 1 : 0;
				EmitMisalignGuard(m, amask); // C.53: cold path lives out of line
				uint64_t fn = (op == 0x37) ? reinterpret_cast<uint64_t>(&eeRead64_arm64)
				            : (op == 0x23 || op == 0x27) ? reinterpret_cast<uint64_t>(&eeRead32_arm64)
				            : (op == 0x21 || op == 0x25) ? reinterpret_cast<uint64_t>(&eeRead16_arm64)
				            : reinterpret_cast<uint64_t>(&eeRead8_arm64);
				if (FastmemOk(s_emit_pc))
				{
					// One-instruction access; the stub after it is dead code
					// until a fault patches the access into `b stub`. Ldrb/Ldrh/
					// Ldr w0 zero-extend into x0 exactly like the u32-returning
					// wrappers, so the value semantics are unchanged.
					EmitFastmemAccess(m, [&] {
						switch (op)
						{
							case 0x20: case 0x24: m.ldrb(w0, MemOperand(vmapbase, w0, UXTW)); break; // LB/LBU
							case 0x21: case 0x25: m.ldrh(w0, MemOperand(vmapbase, w0, UXTW)); break; // LH/LHU
							case 0x23: case 0x27: m.ldr (w0, MemOperand(vmapbase, w0, UXTW)); break; // LW/LWU
							default:              m.ldr (x0, MemOperand(vmapbase, w0, UXTW)); break; // LD
						}
					});
					DeferFastmemStub(m, fn, 0, 0);
				}
				else if (InlineVmapEnabled())
				{
					// Inline vtlb direct case; handlers (hw regs) fall back to
					// the wrapper call. Same value semantics: Ldrb/Ldrh/Ldr w0
					// zero-extend into x0 exactly like the u32-returning
					// wrappers.
					Label slow, done;
					EmitVmapLookup(m, &slow);
					switch (op)
					{
						case 0x20: case 0x24: m.Ldrb(w0, MemOperand(x10)); break; // LB/LBU
						case 0x21: case 0x25: m.Ldrh(w0, MemOperand(x10)); break; // LH/LHU
						case 0x23: case 0x27: m.Ldr (w0, MemOperand(x10)); break; // LW/LWU
						default:              m.Ldr (x0, MemOperand(x10)); break; // LD
					}
					m.B(&done);
					m.Bind(&slow);
					m.Mov(x16, fn); m.Blr(x16);
					m.Bind(&done);
				}
				else { m.Mov(x16, fn); m.Blr(x16); } // result in x0 (u32 returns are zero-extended in x0)
				if (rt)
				{
					switch (op)
					{
						case 0x20: m.Sxtb(x0, w0); break; // LB
						case 0x21: m.Sxth(x0, w0); break; // LH
						case 0x23: m.Sxtw(x0, w0); break; // LW
						default: break;                   // LBU/LHU/LWU (zero-extended), LD (full 64)
					}
					StoreGpr(m, x0, gpr, rt);
				}
				return true;
			}
			// Unaligned load family (LWL/LWR/LDL/LDR): merge with the existing rt
			// contents at the aligned address, no misalign exception -- a plain
			// helper call that mirrors the interpreter op exactly (C.16). These
			// were the top handoff cause in GT3's attract (LDL+LDR alone ~865M
			// interpreted ops / 10500 frames).
			case 0x22: case 0x26: case 0x1a: case 0x1b:
			{
				{ const Register a = SrcGpr(m, gpr, rs, x0); m.Add(w0, a.W(), simm); }
				m.Mov(w1, rt);
				s_rc.FlushReg(m, gpr, rt); // the helper READS GPR[rt] for the merge
				const uint64_t fn = (op == 0x22) ? reinterpret_cast<uint64_t>(&eeLWL_arm64)
				                  : (op == 0x26) ? reinterpret_cast<uint64_t>(&eeLWR_arm64)
				                  : (op == 0x1a) ? reinterpret_cast<uint64_t>(&eeLDL_arm64)
				                  :                reinterpret_cast<uint64_t>(&eeLDR_arm64);
				m.Mov(x16, fn); m.Blr(x16);
				s_rc.Invalidate(rt); // ...and then writes it directly
				return true;
			}
			// Unaligned store family (SWL/SWR/SDL/SDR): read-modify-write of the
			// aligned word/dword, same helper-call pattern.
			case 0x2a: case 0x2e: case 0x2c: case 0x2d:
			{
				{ const Register a = SrcGpr(m, gpr, rs, x0); m.Add(w0, a.W(), simm); }
				m.Mov(w1, rt);
				s_rc.FlushReg(m, gpr, rt); // the helper reads GPR[rt] from memory
				const uint64_t fn = (op == 0x2a) ? reinterpret_cast<uint64_t>(&eeSWL_arm64)
				                  : (op == 0x2e) ? reinterpret_cast<uint64_t>(&eeSWR_arm64)
				                  : (op == 0x2c) ? reinterpret_cast<uint64_t>(&eeSDL_arm64)
				                  :                reinterpret_cast<uint64_t>(&eeSDR_arm64);
				m.Mov(x16, fn); m.Blr(x16);
				return true;
			}
			// Stores (SB/SH/SW/SD).
			case 0x28: case 0x29: case 0x2b: case 0x3f:
			{
				{ const Register a = SrcGpr(m, gpr, rs, x0); m.Add(w0, a.W(), simm); }
				const u32 amask = (op == 0x3f) ? 7 : (op == 0x2b) ? 3 : (op == 0x29) ? 1 : 0;
				EmitMisalignGuard(m, amask); // C.53: cold path lives out of line
				LoadGpr(m, x1, gpr, rt); // value (low bits used by the wrapper)
				uint64_t fn = (op == 0x3f) ? reinterpret_cast<uint64_t>(&eeWrite64_arm64)
				            : (op == 0x2b) ? reinterpret_cast<uint64_t>(&eeWrite32_arm64)
				            : (op == 0x29) ? reinterpret_cast<uint64_t>(&eeWrite16_arm64)
				            : reinterpret_cast<uint64_t>(&eeWrite8_arm64);
				if (FastmemOk(s_emit_pc))
				{
					// A store into a write-protected RAM page (one holding
					// compiled blocks) SIGSEGVs into the fault handler's SMC
					// branch, which unprotects the page and drops the stale
					// blocks before the kernel retries this store -- it never
					// reaches the backpatch path, exactly like the inline
					// stores below.
					EmitFastmemAccess(m, [&] {
						switch (op)
						{
							case 0x28: m.strb(w1, MemOperand(vmapbase, w0, UXTW)); break; // SB
							case 0x29: m.strh(w1, MemOperand(vmapbase, w0, UXTW)); break; // SH
							case 0x2b: m.str (w1, MemOperand(vmapbase, w0, UXTW)); break; // SW
							default:   m.str (x1, MemOperand(vmapbase, w0, UXTW)); break; // SD
						}
					});
					DeferFastmemStub(m, fn, 0, 0);
				}
				else if (InlineVmapEnabled())
				{
					// Inline direct store. A store into a write-protected RAM
					// page (holding compiled blocks) SIGSEGVs -> vtlb
					// PageFaultHandler -> mmap_ClearCpuBlock unprotects and
					// drops the stale blocks -> the kernel retries this Str;
					// identical flow to interpreter stores.
					Label slow, done;
					EmitVmapLookup(m, &slow);
					switch (op)
					{
						case 0x28: m.Strb(w1, MemOperand(x10)); break; // SB
						case 0x29: m.Strh(w1, MemOperand(x10)); break; // SH
						case 0x2b: m.Str (w1, MemOperand(x10)); break; // SW
						default:   m.Str (x1, MemOperand(x10)); break; // SD
					}
					m.B(&done);
					m.Bind(&slow);
					m.Mov(x16, fn); m.Blr(x16);
					m.Bind(&done);
				}
				else { m.Mov(x16, fn); m.Blr(x16); }
				return true;
			}
			// PREF (0x33): the interpreter's PREF() body is empty (like SYNC),
			// so it translates to pure cycle bookkeeping (Default=9).
			case 0x33: return true;

			// LQ/SQ (128-bit; the hardware silently aligns -- addr & ~0xf, no
			// misalign exception). Previously untranslated: every LQ/SQ ended
			// the block with an interpreter handoff, and they are everywhere in
			// compiler-generated 128-bit copies.
			case 0x1e: // LQ
			{
				if (!rt) return false; // rt==0: rare, leave to interp
				{ const Register a = SrcGpr(m, gpr, rs, x0); m.Add(w0, a.W(), simm); }
				m.And(w0, w0, 0xfffffff0);
				Label slow, done;
				if (FastmemOk(s_emit_pc))
				{
					// Only the fastmem load can fault; patching it into `b stub`
					// also skips the Str below, and the stub's wrapper writes
					// GPR[rt] itself (x1 = &GPR[rt]), so the result lands in the
					// same place either way -- which is why the resume point is
					// AFTER the Str.
					EmitFastmemAccess(m, [&] { m.ldr(q0, MemOperand(vmapbase, w0, UXTW)); });
					m.Str(q0, MemOperand(gpr, rt * 16));
					DeferFastmemStub(m, reinterpret_cast<uint64_t>(&eeRead128_arm64), 1, rt);
				}
				else
				{
					if (InlineVmapEnabled())
					{
						EmitVmapLookup(m, &slow);
						m.Ldr(q0, MemOperand(x10));
						m.Str(q0, MemOperand(gpr, rt * 16));
						m.B(&done);
						m.Bind(&slow);
					}
					m.Add(x1, gpr, rt * 16);
					m.Mov(x16, reinterpret_cast<uint64_t>(&eeRead128_arm64)); m.Blr(x16);
					m.Bind(&done);
				}
				s_rc.Invalidate(rt); // 128-bit write bypasses StoreGpr (both paths)
				return true;
			}
			case 0x1f: // SQ
			{
				{ const Register a = SrcGpr(m, gpr, rs, x0); m.Add(w0, a.W(), simm); }
				m.And(w0, w0, 0xfffffff0);
				s_rc.FlushReg(m, gpr, rt); // the 128-bit source is read from memory
				Label slow, done;
				if (FastmemOk(s_emit_pc))
				{
					// The GPR-side load can't fault; the fastmem store is the
					// patch site (the wrapper re-reads GPR[rt] via x1).
					m.Ldr(q0, MemOperand(gpr, rt * 16)); // GPR.r[0] is kept zero
					EmitFastmemAccess(m, [&] { m.str(q0, MemOperand(vmapbase, w0, UXTW)); });
					DeferFastmemStub(m, reinterpret_cast<uint64_t>(&eeWrite128_arm64), 1, rt);
				}
				else
				{
					if (InlineVmapEnabled())
					{
						EmitVmapLookup(m, &slow);
						m.Ldr(q0, MemOperand(gpr, rt * 16)); // GPR.r[0] is kept zero
						m.Str(q0, MemOperand(x10));
						m.B(&done);
						m.Bind(&slow);
					}
					m.Add(x1, gpr, rt * 16);
					m.Mov(x16, reinterpret_cast<uint64_t>(&eeWrite128_arm64)); m.Blr(x16);
					m.Bind(&done);
				}
				return true;
			}

			// COP1. Cop1RegSupported decides: the register moves
			// (MFC1/MTC1/CFC1/CTC1), the S-format arithmetic and compares,
			// and the conversions are all emitted here. BC1 is declined by
			// that predicate but is not a handoff -- it is a branch, and
			// EmitBranch picks it up below alongside BC0x and BC2x.
			//
			// The one conditional is soft-float rec mode: the native
			// arithmetic mirrors the old fpuDouble interpreter rather than
			// the ps2float model, so with EnableFpuSoftFloat set the
			// arithmetic falls back and only the bit moves, compares and
			// conversions stay native. That setting is off by default, so
			// the arithmetic is normally emitted.
			case 0x11:
				if (!Cop1RegSupported(insn)) return false;
				EmitCop1Reg(m, gpr, insn);
				return true;

			// MMI (128-bit parallel integer) -> NEON, for the bit-exact subset.
			case 0x1c:
				// Pipeline-1 mult/div + HI1/LO1 moves: the funct encodings match
				// the SPECIAL forms exactly, only HI/LO shift up by 8 bytes.
				// MADD/MADDU (funct 0x00/0x01) are pipeline 0; MADD1/MADDU1
				// (0x20/0x21) pipeline 1.
				switch (funct)
				{
					case 0x10: case 0x11: case 0x12: case 0x13: // MFHI1/MTHI1/MFLO1/MTLO1
					case 0x18: case 0x19: case 0x1a: case 0x1b: // MULT1/MULTU1/DIV1/DIVU1
						EmitMulDiv(m, gpr, funct, rs, rt, rd, 8);
						return true;
					case 0x00: case 0x01: // MADD/MADDU
						EmitMadd(m, gpr, funct == 0x01, rs, rt, rd, 0);
						return true;
					case 0x20: case 0x21: // MADD1/MADDU1
						EmitMadd(m, gpr, funct == 0x21, rs, rt, rd, 8);
						return true;
				}
				// C.63: PLZCW -- rd.UL[0..1] = count_leading_sign_bits(rs.SL[0..1]) - 1,
				// which is exactly what AArch64's CLS computes (it excludes the sign
				// bit). Only the low 64 bits of rd are written, like the interpreter.
				if ((insn & 0x3f) == 0x04)
				{
					const u32 rd = (insn >> 11) & 31, rs = (insn >> 21) & 31;
					if (rd)
					{
						const Register a = SrcGpr(m, gpr, rs, x0);
						m.Cls(w1, a.W());        // low word
						m.Lsr(x2, a, 32);
						m.Cls(w2, w2);           // high word
						m.Bfi(x1, x2, 32, 32);   // rd = lo | (hi << 32)
						const Register d = DstGpr(m, gpr, rd, x0);
						m.Mov(d, x1);
						FinishDst(m, gpr, rd, d);
					}
					return true;
				}
				if (MmiSupported(insn)) { EmitMmi(m, gpr, insn); return true; }
				if (MmiHiLoAction(insn)) { EmitMmiHiLo(m, gpr, insn); return true; } // C.79
				if (MmiDivAction(insn)) { EmitMmiDiv(m, gpr, insn); return true; }
				if (MmiPermAction(insn)) { EmitMmiPerm(m, gpr, insn); return true; }
				if (MmiMulAction(insn)) { EmitMmiMul(m, gpr, insn); return true; }
				if (MmiMaccAction(insn)) { EmitMmiMacc(m, gpr, insn); return true; }
				if (MmiHmaccAction(insn)) { EmitMmiHmacc(m, gpr, insn); return true; }
				// PCPYH: broadcast halfword 0 of each 64-bit half of rt across
				// that half. Scalar: (u16)half * 0x0001000100010001.
				if (IsPcpyh(insn))
				{
					if (rd)
					{
						s_rc.FlushReg(m, gpr, rt); // low half is read from memory
						m.Ldrh(w0, MemOperand(gpr, rt * 16));     // rt.US[0] (r0 memory kept zero)
						m.Ldrh(w1, MemOperand(gpr, rt * 16 + 8)); // rt.US[4]
						m.Mov(x2, 0x0001000100010001ull);
						m.Mul(x0, x0, x2);
						m.Mul(x1, x1, x2);
						m.Str(x0, MemOperand(gpr, rd * 16));
						m.Str(x1, MemOperand(gpr, rd * 16 + 8));
						s_rc.Invalidate(rd); // direct write bypasses StoreGpr
					}
					return true;
				}
				// QFSRV (quadword funnel shift by the SA register): interpreter
				// implementation called inline (C.18).
				if (IsQfsrv(insn))
				{
					EmitInterpOpCall(m, gpr, insn, &intQFSRV);
					return true;
				}
				return false;

			// CACHE (0x2f): emulated D$ maintenance, contained side effects ->
			// COP2 / VU0 macro mode (0x12), C.29-1: everything except the BC2
			// branches (rs=0x08, control flow -> EmitBranch) executes via the
			// interpreter's top-level COP2() dispatcher called inline, so VU0
			// macro ALU ops / VCALLMS(R) / Q(M)FC2/CTC2/CFC2 no longer BREAK
			// the block into an interpreter tail. All contained: no EE pc use,
			// no exceptions; VU0 timing reads cpuRegs.cycle which only
			// advances at branch boundaries -- identical in both execution
			// modes (same argument as MFC0 Count, C.18). EmitInterpOpCall's
			// FlushDirty-before covers QMTC2/CTC2 reading GPR memory and its
			// cache Reset-after covers QMFC2/CFC2 writing it. The whole COP2
			// opcode space is charged Default(9) by R5900OpcodeTables (the
			// class table is disabled upstream), which OpCycles already does.
			case 0x12:
				if (rs == 0x08) return false; // BC2F/T(L) -> EmitBranch
				// C.30-2: SPECIAL1/2 ALU ops emit natively via the microVU0
				// single-op emitters.
				if (rs >= 0x10 && EmitCop2Macro(m, gpr, insn))
					return true;
				// C.58: the COP2 transfers (QMFC2/CFC2/QMTC2/CTC2) are register
				// moves once the VU0 syncs are out of the way -- no reason to pay
				// an interpreter call, which also has to flush and then drop the
				// whole GPR register cache.
				if (EmitCop2Transfer(m, gpr, insn))
					return true;
				EmitInterpOpCall(m, gpr, insn, &intCOP2);
				return true;

			// interpreter implementation called inline (C.18). Top GT3 attract
			// block-breaker (19.4M handoffs / 10500 frames).
			case 0x2f:
				EmitInterpOpCall(m, gpr, insn, &intCACHE);
				return true;

			// COP0 (0x10): MFC0/MTC0 via inline interpreter call (C.18) -- MFC0
			// Count reads cpuRegs.cycle which only advances at branch boundaries,
			// identical in both execution modes; MTC0 Status/Config side effects
			// live inside the op. BC0x (branches) and CO/TLB/ERET stay handoffs.
			case 0x10:
				if (rs == 0x00)
				{
					// MFC0 rt, rd. rd is a compile-time constant, so most of
					// COP0.c's switch collapses to a load here rather than a
					// call into the interpreter:
					//
					//   24        reads nothing at all, a plain nop
					//   12        Status, masked to its implemented bits
					//   default   the register, sign-extended
					//
					// 9 (Count) and 25 (PERF) keep the call. Count has to
					// advance cpuRegs.cycle first and does it even when rt is
					// zero, and PERF runs COP0_UpdatePCCR, so neither is a
					// load -- emitting them would mean duplicating side
					// effects that already exist in one place.
					const u32 rd_ = (insn >> 11) & 31;
					// COP0.c returns early when rt is zero unless rd is 9,
					// the one register read for its side effect rather than
					// its value, so nothing at all happens here either.
					if (!rt && rd_ != 9)
						return true;
					if (rd_ == 9 || rd_ == 25)
					{
						EmitInterpOpCall(m, gpr, insn, &intMFC0);
						return true;
					}
					if (rd_ == 24)
						return true;              // reads nothing
					m.Ldr(w0, RegsField(&cpuRegs.CP0.r[rd_]));
					if (rd_ == 12)
						m.And(w0, w0, 0xf0c79c1f);
					m.Sxtw(x0, w0);
					StoreGpr(m, x0, gpr, rt);
					return true;
				}
				if (rs == 0x04) { EmitInterpOpCall(m, gpr, insn, &intMTC0); return true; }
				// CO-format EI/DI (funct 0x38/0x39), C.43: both just conditionally
				// toggle the COP0 Status EIE bit -- no memory, no control flow. EI
				// also lowers nextEventCycle, which the next branch's event gate
				// (C.35) picks up, matching the interpreter (whose EI does the same
				// and only checks events at the next branch). Unlike the x86 rec,
				// the block is NOT force-ended after EI: the interpreter doesn't
				// either. DI here is immediate (the interpreter's DI is too; the
				// x86 rec's one-instruction delay is a rec-only quirk). ERET/TLB
				// (control flow / TLB state) stay interpreter handoffs.
				if (rs == 0x10 && funct == 0x38) { EmitInterpOpCall(m, gpr, insn, &intEI); return true; }
				if (rs == 0x10 && funct == 0x39) { EmitInterpOpCall(m, gpr, insn, &intDI); return true; }
				return false;

			// LWC1 ft, off(base): fpr[ft].UL = read32(addr). Unlike integer LW, a
			// misaligned address is *silently skipped* (no exception/cancel) -- match
			// the interpreter's `if (addr & 3) return;`.
			case 0x31:
			{
				{ const Register a = SrcGpr(m, gpr, rs, x0); m.Add(w0, a.W(), simm); }
				Label skip;
				m.Tst(w0, 3); m.B(&skip, ne);
				// C.56: the FP loads/stores were the last memory ops still paying a
				// full wrapper call (a 4-instruction address materialization plus
				// the blr) on every access -- they never even used the old inline
				// vmap path. Give them fastmem like the integer ones: the access is
				// one instruction, and a fault patches it into the stub that does
				// the call.
				if (FastmemOk(s_emit_pc))
				{
					EmitFastmemAccess(m, [&] { m.ldr(w0, MemOperand(vmapbase, w0, UXTW)); });
					DeferFastmemStub(m, reinterpret_cast<uint64_t>(&eeRead32_arm64), 0, 0);
				}
				else { m.Mov(x16, reinterpret_cast<uint64_t>(&eeRead32_arm64)); m.Blr(x16); }
				m.Mov(x1, reinterpret_cast<uint64_t>(&fpuRegs)); m.Str(w0, MemOperand(x1, rt * 4));
				m.Bind(&skip);
				return true;
			}
			// SWC1 ft, off(base): write32(addr, fpr[ft].UL); misalign silently skipped.
			case 0x39:
			{
				{ const Register a = SrcGpr(m, gpr, rs, x0); m.Add(w0, a.W(), simm); }
				Label skip;
				m.Tst(w0, 3); m.B(&skip, ne);
				m.Mov(x1, reinterpret_cast<uint64_t>(&fpuRegs)); m.Ldr(w1, MemOperand(x1, rt * 4));
				if (FastmemOk(s_emit_pc)) // C.56
				{
					EmitFastmemAccess(m, [&] { m.str(w1, MemOperand(vmapbase, w0, UXTW)); });
					DeferFastmemStub(m, reinterpret_cast<uint64_t>(&eeWrite32_arm64), 0, 0);
				}
				else { m.Mov(x16, reinterpret_cast<uint64_t>(&eeWrite32_arm64)); m.Blr(x16); }
				m.Bind(&skip);
				return true;
			}

			// LQC2/SQC2 (0x36/0x3e): 128-bit VU0 register load/store. Both start
			// with vu0Sync() (a VU0 state-machine catch-up that early-outs when
			// VU0 is idle) then a 128-bit mem access to/from vuRegs[0].VF[ft]. The
			// vu0Sync + alignment-exact memRead128/memWrite128 semantics are
			// delicate, so these route through the inline interpreter-call path
			// (like MFC0/EI) rather than a hand-emitted body -- the win is that the
			// block no longer BREAKS here (top non-exception handoff cause in MMX7:
			// LQC2 ~827K, SQC2 ~100K). C.45.
			// C.59: emitted natively now. The op is vu0Sync() + a 128-bit access
			// between memory and vuRegs[0].VF[ft], and the destination is a VU
			// register, NOT a GPR -- so unlike the interpreter-call path this
			// leaves the EE register cache alone instead of flushing it and then
			// dropping it wholesale, on an op MMX7 executes ~827K times.
			// Faithful to VU0.c: the address is NOT 16-byte aligned here (LQ/SQ
			// mask it, LQC2/SQC2 hand the raw address to memRead128/memWrite128),
			// and LQC2 with ft == 0 still performs the read -- the load can have
			// side effects -- it just throws the value away.
			case 0x36: // LQC2
			{
				static u128 s_lqc2_sink; // ft == 0: the read happens, the value doesn't land
				m.Mov(x16, reinterpret_cast<uint64_t>(&vu0Sync)); m.Blr(x16);
				{ const Register a = SrcGpr(m, gpr, rs, x0); m.Add(w0, a.W(), simm); }
				m.Mov(x1, rt ? reinterpret_cast<uint64_t>(&vuRegs[0].VF[rt])
				             : reinterpret_cast<uint64_t>(&s_lqc2_sink));
				m.Mov(x16, reinterpret_cast<uint64_t>(&eeRead128_arm64)); m.Blr(x16);
				return true;
			}
			case 0x3e: // SQC2
			{
				m.Mov(x16, reinterpret_cast<uint64_t>(&vu0Sync)); m.Blr(x16);
				{ const Register a = SrcGpr(m, gpr, rs, x0); m.Add(w0, a.W(), simm); }
				m.Mov(x1, reinterpret_cast<uint64_t>(&vuRegs[0].VF[rt])); // VF[0] is the (0,0,0,1) constant
				m.Mov(x16, reinterpret_cast<uint64_t>(&eeWrite128_arm64)); m.Blr(x16);
				return true;
			}
			default: return false;
		}
	}

	// Block epilogue with tail-chaining: restore the frame, then dispatch directly
	// to the next block (cpuRegs.pc) via the RAM LUT -- staying in JIT code instead
	// of returning to the C++ GAME_RUNNING loop. On a LUT miss (uncompiled block or
	// non-RAM pc) it returns to the dispatcher, which compiles it and re-enters.
	// x30 (the original return address into eeJitRunBlock) propagates through the
	// chain, so the final `ret` returns to the C++ caller.
	void EmitChainEpilogue(MacroAssembler& m)
	{
		// C.40: try to chain FIRST, with the frame (and x19) still live --
		// the successor's chain entry skips its prologue, so back-to-back
		// blocks pay no frame pop/push or x19 rematerialization.
		// C.52: the pc still has to be READ here -- an event test in the tail can
		// redirect it (an interrupt/exception sets pc to the handler), and a
		// chain that jumped to the compile-time successor instead would never
		// deliver those. What can go is the arithmetic around it:
		//   * mask + RAM range check collapse into one Tst. np = pc & 0x1fffffff
		//     is in RAM (< 32 MB) exactly when pc has no bits set in 0x1e000000.
		//   * the index is then a plain bitfield extract of pc (bits 2..24).
		//   * s_lut never moves after eeJitReserve, so materialize its VALUE
		//     instead of the address of the pointer variable: same 4-instruction
		//     movz/movk chain, but the load of the pointer goes away. (A literal
		//     -pool load of the base was tried instead and is SLOWER: it trades
		//     4 cheap ALU ops for a data-cache access, the same trap as C.49.)
		// 15 instructions and 3 loads -> 12 instructions and 2 loads, on every
		// block exit -- the hot EE loops are many tiny blocks, so their tails are
		// a real part of what they execute.
		m.Ldr(w1, RegsField(&cpuRegs.pc));      // pc (may have been redirected by the event test)
		m.Tst(w1, 0x1e000000);                  // outside the 32 MB RAM window?
		m.B(s_blk_ret, ne);                     // -> return to the dispatcher
		m.Mov(x0, reinterpret_cast<uint64_t>(s_lut)); // the LUT base itself, materialized
		m.Ubfx(w2, w1, 2, 23);                  // idx = (pc & 0x01ffffff) >> 2
		m.Ldr(x3, MemOperand(x0, w2, UXTW, 3)); // fn = s_lut[idx]
		m.Cbz(x3, s_blk_ret);                   // uncompiled -> return
		m.Add(x3, x3, kChainEntryOffset);       // skip the successor's prologue
		m.Br(x3);                               // tail-jump to next block's body
	}

	// C.54: the frame pop + ret, emitted ONCE per block in the stub area (every
	// exit used to carry its own copy: 8 instructions each, inline, cold).
	void EmitFrameRet(MacroAssembler& m)
	{
		m.Ldp(x19, x20, MemOperand(sp, 0));
		m.Ldp(x21, x30, MemOperand(sp, 16));
		m.Ldp(x22, x23, MemOperand(sp, 32));
		m.Ldp(x24, x25, MemOperand(sp, 48));
		m.Ldp(x26, x27, MemOperand(sp, 64));
		m.Ldr(x28, MemOperand(sp, 80));
		m.Add(sp, sp, 96);
		m.Ret();
	}

	// C.54: exit to a compile-time-known successor.
	//
	// cpuRegs.pc can only differ from that successor if the tail's event test
	// actually RAN -- an exception redirects pc to its handler (C.52 learned this
	// the hard way: chaining blindly to the assumed successor never delivers
	// interrupts and the EE just spins). But the test is GATED on cycle >=
	// nextEventCycle, so on the gate's not-due path -- the overwhelmingly common
	// one -- pc IS the constant. Then the LUT slot address is a compile-time
	// constant too: no pc load, no mirror mask, no range check, no index
	// computation. The due path (call the event test, then chain generically off
	// the possibly-redirected pc) is deferred to the block's stub area, so the
	// hot stream carries none of it -- same reasoning as C.51/C.53.
	void EmitKnownExit(MacroAssembler& m, u32 target_pc, uint64_t evt)
	{
		const u32 np = Norm(target_pc);
		// A target outside RAM has no LUT slot -> fall back to the generic tail.
		if (!s_lut || !InRam(np))
		{
			if (evt) { m.Mov(x16, evt); m.Blr(x16); }
			EmitChainEpilogue(m);
			return;
		}
		if (evt)
		{
			s_exit_labels.emplace_back();
			Label* slow = &s_exit_labels.back();
			m.Ldr(x0, RegsField(&cpuRegs.cycle));
			m.Ldr(x1, RegsField(&cpuRegs.nextEventCycle));
			m.Subs(x0, x0, x1);   // (s64)(cycle - nextEventCycle)
			m.B(slow, pl);        // event due -> out-of-line stub
			s_exit_pending.push_back({slow, evt});
		}
		// Not due (or no test at all): pc == target_pc, guaranteed.
		m.Mov(x3, reinterpret_cast<uint64_t>(&s_lut[np >> 2])); // the LUT slot itself
		m.Ldr(x3, MemOperand(x3));
		m.Cbz(x3, s_blk_ret);             // uncompiled -> return to the dispatcher
		m.Add(x3, x3, kChainEntryOffset); // skip the successor's prologue
		m.Br(x3);
	}
	// Deferred: the event-due tails, then the block's single return path.
	void EmitExitStubs(MacroAssembler& m)
	{
		for (ExitPending& e : s_exit_pending)
		{
			m.Bind(e.slow);
			m.Mov(x16, e.evt);
			m.Blr(x16);           // may redirect pc (exception/interrupt)
			EmitChainEpilogue(m); // ... so chain off the pc it left behind
		}
		// C.60: the manual block's source changed under it. It has just been
		// discarded, so hand the guest pc (this block's, we are at its entry)
		// back to the dispatcher, which compiles it again from live memory.
		if (s_stale)
		{
			m.Bind(s_stale);
			m.Mov(w0, s_stale_pc);
			m.Str(w0, RegsField(&cpuRegs.pc));
			m.B(s_blk_ret);
		}
		m.Bind(s_blk_ret);
		EmitFrameRet(m);
	}

	bool IsTranslatable(u32 insn)
	{
		const u32 op = insn >> 26, funct = insn & 0x3f;
		if (op == 0x00)
		{
			switch (funct)
			{
				case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:
				case 0x21: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
				case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
				case 0x14: case 0x16: case 0x17: case 0x38: case 0x3a: case 0x3b:
				case 0x3c: case 0x3e: case 0x3f:
					return true;
				case 0x10: case 0x11: case 0x12: case 0x13: // MFHI/MTHI/MFLO/MTLO
				case 0x18: case 0x19: case 0x1a: case 0x1b: // MULT/MULTU/DIV/DIVU
					return true;
				case 0x20: case 0x22:                       // ADD/SUB (C.63, trap dropped)
					return true;
				case 0x0f: return true;                     // SYNC (empty body)
				case 0x0a: case 0x0b: return true;          // MOVZ/MOVN
				case 0x28: case 0x29: return true;          // MFSA/MTSA
				default: return false;
			}
		}
		if (op == 0x11)
			return Cop1RegSupported(insn);
		if (op == 0x1c)
		{
			const u32 f = insn & 0x3f;
			if ((f >= 0x10 && f <= 0x13) || (f >= 0x18 && f <= 0x1b) // pipeline-1 muldiv
				|| f == 0x00 || f == 0x01 || f == 0x20 || f == 0x21) // MADD/MADDU(1)
				return true;
			if (f == 0x04)             // PLZCW (C.63)
				return true;
			return MmiSupported(insn) || MmiHiLoAction(insn) || IsPcpyh(insn) || // C.79
				MmiDivAction(insn) || MmiPermAction(insn) ||
				MmiMulAction(insn) || MmiMaccAction(insn) ||
				MmiHmaccAction(insn) || IsQfsrv(insn);
		}
		if (op == 0x01) // REGIMM: MTSAB/MTSAH only (branches handled elsewhere)
		{
			const u32 rtf = (insn >> 16) & 31;
			return rtf == 0x18 || rtf == 0x19;
		}
		if (op == 0x12) // COP2: all but BC2 (branches -> EmitBranch)
		{
			if (((insn >> 21) & 31) == 0x08) return false;
			if (Cop2XferSupported(insn)) return true; // C.58: native
			return true;
		}
	if (op == 0x2f) return true; // CACHE
		if (op == 0x10) // COP0: MFC0/MTC0 + CO-format EI/DI
		{
			const u32 crs = (insn >> 21) & 31, cf = insn & 0x3f;
			return crs == 0x00 || crs == 0x04 || (crs == 0x10 && (cf == 0x38 || cf == 0x39));
		}
		switch (op)
		{
			case 0x08: case 0x18: // ADDI/DADDI (overflow trap dropped, like the x86 rec)
			case 0x09: case 0x19: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
			case 0x33: // PREF (empty body)
				return true;
			case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27:
			case 0x22: case 0x26: case 0x1a: case 0x1b: // LWL/LWR/LDL/LDR
				return true;
			case 0x2a: case 0x2e: case 0x2c: case 0x2d: // SWL/SWR/SDL/SDR
				return true;
			case 0x1e: return ((insn >> 16) & 31) != 0; // LQ (rt==0 -> interp)
			case 0x36: case 0x3e: return true; // LQC2/SQC2: native (C.59)
			case 0x37: return true; // LD
			case 0x28: case 0x29: case 0x2b:
				return true;
			case 0x1f: return true; // SQ
			case 0x3f: return true; // SD
			case 0x31: case 0x39: return true; // LWC1/SWC1 are COP1 moves
			default: return false;
		}
	}

	// cyc = raw sum of the block's per-op cycle costs (see OpCycles); the
	// (2 - CP0.Config[18]) factor is applied here, matching execI.
	//
	// C.55: cpuBlockCycles and the cycle-rate speedhack are ordinary globals,
	// far from cpuRegs (367 KB and 26 MB in the image), so the x19-base trick of
	// C.46 cannot reach them -- their addresses have to be materialized. What
	// CAN go is doing it twice: a branch exit ran this and then the cycle update,
	// each with its own 4-instruction movz/movk chain for &cpuBlockCycles, and
	// each round-tripping the value through memory. Fuse them: one
	// materialization, one load, one store, and the non-default-rate path (which
	// calls the helper) is a stub, not inline.
	void EmitTailCycles(MacroAssembler& m, int cyc, bool with_update)
	{
		const uint64_t upd = reinterpret_cast<uint64_t>(&eeUpdateCycles_arm64);
		m.Mov(x10, reinterpret_cast<uint64_t>(&cpuBlockCycles)); // once, for both halves
		m.Ldr(w1, RegsField(&cpuRegs.CP0.n.Config));
		m.Mov(w2, cyc); m.Mov(w3, cyc * 2);
		m.Tst(w1, 1u << 18);
		m.Csel(w2, w2, w3, ne);
		m.Ldr(w0, MemOperand(x10));
		m.Add(w0, w0, w2); // cpuBlockCycles += cyc * (2 - CP0.Config[18])

		if (!with_update)
		{
			m.Str(w0, MemOperand(x10));
			if (with_update) { m.Mov(x16, upd); m.Blr(x16); }
			return;
		}
		// Default cycle rate: cycle += max(1, cpuBlockCycles >> 3); bc &= 7.
		Label slow, done;
		m.Mov(x11, reinterpret_cast<uint64_t>(&EmuConfig.Speedhacks.EECycleRate));
		m.Ldrsb(w1, MemOperand(x11));
		m.Cbnz(w1, &slow);
		m.Lsr(w2, w0, 3);
		m.Cmp(w2, 0);
		m.Csinc(w2, w2, wzr, ne); // max(1, bc >> 3)
		// cpuRegs.cycle is 64 bits wide; the add has to carry out of the
		// low word or the clock steps back 2^32 cycles when it crosses it.
		m.Ldr(x3, RegsField(&cpuRegs.cycle));
		m.Add(x3, x3, Operand(w2, UXTW));
		m.Str(x3, RegsField(&cpuRegs.cycle));
		m.And(w0, w0, 7);
		m.Str(w0, MemOperand(x10));
		m.B(&done);
		m.Bind(&slow);
		m.Str(w0, MemOperand(x10)); // the helper reads cpuBlockCycles from memory
		m.Mov(x16, upd);
		m.Blr(x16);
		m.Bind(&done);
	}

	void EmitCycleBookkeeping(MacroAssembler& m, int cyc) { EmitTailCycles(m, cyc, false); }

	inline void StorePC(MacroAssembler& m, const Register& wsrc)
	{
		m.Str(wsrc, RegsField(&cpuRegs.pc));
	}
	inline void StorePCImm(MacroAssembler& m, u32 v) { m.Mov(w0, v); StorePC(m, w0); }


	// Translate a branch/jump (ends the block via the chaining epilogue). EE: the
	// delay slot runs only when taken; intEventTest() runs on both paths; not-taken
	// continues at bpc+4. Likely branches / Goemon-HLE JAL / untranslatable delay
	// slot -> false (interpreter finishes the basic block).
	bool EmitBranch(MacroAssembler& m, const Register& gpr, u32 bpc, u32 insn, int cyc_leading)
	{
		s_rc.pinned = 0; // new op: previous op's Register handles are dead
		const u32 op = insn >> 26, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31, funct = insn & 0x3f;
		const int32_t simm = (int16_t)(insn & 0xffff);

		bool uncond = false, is_jr = false, two = false, one = false, likely = false;
		bool bc0 = false; // condition = CPCOND0() (DMAC all-clear test), via helper call
		bool bc1 = false; // condition = FCR31 C flag (0x00800000), tested inline
		bool bc2 = false; // condition = CP2COND (VPU_STAT VU0-busy bit 0x100), tested inline
		int  link = -1;
		Condition cond = al;
		u32  tconst = 0;
		const u32 jtarget = (((bpc + 4) & 0xf0000000) | ((insn & 0x3ffffff) << 2));

		if      (op == 0x00 && funct == 0x08) { uncond = true; is_jr = true; }
		else if (op == 0x00 && funct == 0x09) { uncond = true; is_jr = true; link = (int)((insn >> 11) & 31); }
		else if (op == 0x02) { uncond = true; tconst = jtarget; }
		else if (op == 0x03) { uncond = true; link = 31; tconst = jtarget;
			if (jtarget == 0x3563b8 || jtarget == 0x35d628 || jtarget == 0x35c118) return false; }
		else if (op == 0x04) { two = true; cond = eq; tconst = bpc + 4 + simm * 4; }
		else if (op == 0x05) { two = true; cond = ne; tconst = bpc + 4 + simm * 4; }
		else if (op == 0x06) { one = true; cond = le; tconst = bpc + 4 + simm * 4; }
		else if (op == 0x07) { one = true; cond = gt; tconst = bpc + 4 + simm * 4; }
		// Likely branches: taken == normal (delay slot inline); not-taken SKIPS
		// the delay slot (continue at bpc+8) and, like the interpreter's
		// BEQL/BNEL/BLEZL/BGTZL/BLTZL/BGEZL, calls intEventTest there.
		else if (op == 0x14) { two = true; cond = eq; tconst = bpc + 4 + simm * 4; likely = true; }
		else if (op == 0x15) { two = true; cond = ne; tconst = bpc + 4 + simm * 4; likely = true; }
		else if (op == 0x16) { one = true; cond = le; tconst = bpc + 4 + simm * 4; likely = true; }
		else if (op == 0x17) { one = true; cond = gt; tconst = bpc + 4 + simm * 4; likely = true; }
		else if (op == 0x01)
		{
			const u32 t = bpc + 4 + simm * 4;
			if      (rt == 0x00) { one = true; cond = lt; tconst = t; }
			else if (rt == 0x01) { one = true; cond = ge; tconst = t; }
			else if (rt == 0x02) { one = true; cond = lt; tconst = t; likely = true; } // BLTZL
			else if (rt == 0x03) { one = true; cond = ge; tconst = t; likely = true; } // BGEZL
			else if (rt == 0x10) { one = true; cond = lt; tconst = t; link = 31; }
			else if (rt == 0x11) { one = true; cond = ge; tconst = t; link = 31; }
			else return false;
		}
		// BC0F/BC0T(+L): branch on CPCOND0 == 0/1 (COP0.c: DMAC CIS|~CPC
		// all-clear). Taken/not-taken semantics match the BLEZ class (interp
		// BC0F does nothing on not-taken; the likely forms skip the delay
		// slot + event test like the other likely branches).
		else if (op == 0x10 && rs == 0x08)
		{
			const u32 t = bpc + 4 + simm * 4;
			if      (rt == 0x00) { bc0 = true; cond = eq; tconst = t; }                // BC0F
			else if (rt == 0x01) { bc0 = true; cond = ne; tconst = t; }                // BC0T
			else if (rt == 0x02) { bc0 = true; cond = eq; tconst = t; likely = true; } // BC0FL
			else if (rt == 0x03) { bc0 = true; cond = ne; tconst = t; likely = true; } // BC0TL
			else return false;
		}
		// BC1F/BC1T(+L): branch on the FCR31 C flag (0x00800000) clear/set.
		// Same not-taken semantics as BC0x: nothing on the non-likely forms,
		// and the likely forms skip the delay slot WITHOUT an event test
		// (FPU.c's BC1L macro only does `cpuRegs.pc += 4`).
		else if (op == 0x11 && rs == 0x08)
		{
			const u32 t = bpc + 4 + simm * 4;
			if      (rt == 0x00) { bc1 = true; cond = eq; tconst = t; }                // BC1F
			else if (rt == 0x01) { bc1 = true; cond = ne; tconst = t; }                // BC1T
			else if (rt == 0x02) { bc1 = true; cond = eq; tconst = t; likely = true; } // BC1FL
			else if (rt == 0x03) { bc1 = true; cond = ne; tconst = t; likely = true; } // BC1TL
			else return false;
		}
		// BC2F/BC2T(+L): branch on CP2COND = VPU_STAT's VU0-busy bit (0x100).
		// Same not-taken semantics as BC0x/BC1x (COP2.cpp: BC2xL else pc+=4,
		// no event test). NOTE the whole COP2 space is charged Default(9),
		// not Branch(11) -- the upstream opcode table has no COP2 subclass.
		else if (op == 0x12 && rs == 0x08)
		{
			const u32 t = bpc + 4 + simm * 4;
			if      (rt == 0x00) { bc2 = true; cond = eq; tconst = t; }                // BC2F
			else if (rt == 0x01) { bc2 = true; cond = ne; tconst = t; }                // BC2T
			else if (rt == 0x02) { bc2 = true; cond = eq; tconst = t; likely = true; } // BC2FL
			else if (rt == 0x03) { bc2 = true; cond = ne; tconst = t; likely = true; } // BC2TL
			else return false;
		}
		else return false;

		const u32 ds = memRead32(bpc + 4);
		// C.64: a LIKELY branch executes its delay slot only when taken. The slots
		// that were refusing translation here are BREAK -- GT3 is full of asserts
		// shaped `beql <bad>, <expected>, panic; break` -- so the slot never runs,
		// yet its presence used to send the branch, and with it the rest of the
		// block, to the interpreter: 127k handoffs per 600 in-race frames on a path
		// that never fires. Emit the not-taken path (the only one that happens)
		// natively and hand the taken path back to the interpreter AT THE BRANCH,
		// which then executes branch + slot exactly as before. Plain conditionals
		// only: the COP condition forms and the linking forms keep the old bail.
		const bool ds_interp = !IsTranslatable(ds);
		if (ds_interp && !(likely && (two || one) && !bc0 && !bc1 && !bc2 && link < 0))
			return false;

		// Taken/uncond runs the branch + its delay slot inline; not-taken runs only
		// the branch (the delay slot at bpc+4 is executed by the continuation).
		const int cyc_taken = cyc_leading + OpCycles(insn) + OpCycles(ds);
		const int cyc_nt    = cyc_leading + OpCycles(insn);

		const uint64_t evt = reinterpret_cast<uint64_t>(&eeEventTest_arm64);

		// C.35: gate the event test on cycle >= nextEventCycle, the upstream
		// x86 recompiler's dispatcher rule. The interpreter tests on EVERY
		// taken branch, and mirroring that costs ~28% of CPU time in
		// _cpuEventTest_Shared/iopEventTest bookkeeping (GT3 perf profile).
		// This intentionally trades interpreter bit-identity (events fire at
		// the next due branch, not the current one) for upstream-rec timing.
		const auto EmitEventTest = [&m, evt]()
		{
			Label skip;
			m.Ldr(x0, RegsField(&cpuRegs.cycle));
			m.Ldr(x1, RegsField(&cpuRegs.nextEventCycle));
			m.Subs(x0, x0, x1); // (s64)(cycle - nextEventCycle)
			m.B(&skip, mi);     // not due yet
			m.Mov(x16, evt);
			m.Blr(x16);
			m.Bind(&skip);
		};

		// C.39: inline the default-cycle-rate body of intUpdateCPUCycles
		// (cycle += max(1, cpuBlockCycles >> 3); cpuBlockCycles &= 7) into the
		// block tail — it runs on every block exit and the call was 4.3% of
		// CPU time. Any non-zero EECycleRate falls back to the helper, checked
		// at runtime so settings changes need no block flush.

		// WaitLoop speedhack for the EE kernel idle loop at 0x81fc0 (6 nops +
		// `beq zero,zero,-6`): on the taken path, after the cycle flush and
		// before the event test, cycle = max(cycle, nextEventCycle) -- the
		// x86 recompiler's rule. The next event then fires immediately instead
		// of burning host time emulating millions of idle iterations. An event
		// that is already overdue leaves the clock alone: the clock never runs
		// backwards, since the counters derive their counts from the distance
		// between it and the cycle they last synced at.
		const bool idle_skip = EmuConfig.Speedhacks.WaitLoop && !is_jr
			&& ((tconst & 0x1fffffff) == 0x00081fc0);
		const auto EmitIdleSkip = [&m]()
		{
			m.Ldr(x0, RegsField(&cpuRegs.cycle));
			m.Ldr(x1, RegsField(&cpuRegs.nextEventCycle));
			m.Cmp(x1, x0);
			m.Csel(x0, x1, x0, gt); // (s64)(nextEventCycle - cycle) > 0
			m.Str(x0, RegsField(&cpuRegs.cycle));
		};

		// JR/JALR target first: `jalr rd, rs` with rd == rs jumps to the old
		// rs (JALR() in Interpreter.c reads it before _SetLink).
		if (is_jr) LoadGpr(m, x20, gpr, rs);
		// Link value is ZERO-extended to match the interpreter's _SetLink
		// (R5900.h: UD[0] = u32 pc+4) -- NOT sign-extended as real MIPS64 would.
		// Kernel return addresses (0x8xxxxxxx) otherwise get 0xffffffff upper
		// halves that the interpreter path never produces.
		if (link > 0) { m.Mov(w0, bpc + 8); StoreGpr(m, x0, gpr, (u32)link); }

		if (uncond)
		{
			s_emit_pc = bpc + 4; // delay slot
			Cop2RunInvalidate(); s_emit_budget = 1; // C.66
			EmitSimple(m, gpr, ds);
			if (is_jr) StorePC(m, w20); else StorePCImm(m, tconst);
			s_rc.FlushDirty(m, gpr); // block exit: the next block reads GPR memory
			EmitTailCycles(m, cyc_taken, true); // C.55 (bookkeeping + update, fused)
			if (idle_skip) EmitIdleSkip();
			if (is_jr) { EmitEventTest(); EmitChainEpilogue(m); }
			else       EmitKnownExit(m, tconst, evt); // C.54
			return true;
		}

		if (bc0)
		{
			// CPCOND0 is a plain int() reading dmacRegs; call it directly.
			m.Mov(x16, reinterpret_cast<uint64_t>(&intCPCOND0));
			m.Blr(x16);
			m.Cmp(w0, 0); // eq -> CPCOND0()==0 (BC0F taken), ne -> ==1 (BC0T taken)
		}
		else if (bc1)
		{
			// FCR31 lives at fpuRegs.fprc[31] (byte offset 252).
			m.Mov(x10, reinterpret_cast<uint64_t>(&fpuRegs));
			m.Ldr(w0, MemOperand(x10, 252));
			m.Tst(w0, 0x00800000); // eq -> C clear (BC1F taken), ne -> C set (BC1T taken)
		}
		else if (bc2)
		{
			m.Mov(x10, reinterpret_cast<uint64_t>(&vuRegs[0].VI[REG_VPU_STAT].UL));
			m.Ldr(w0, MemOperand(x10));
			m.Tst(w0, 0x100); // eq -> VU0 idle (BC2F taken), ne -> busy (BC2T taken)
		}
		else
		{
			const Register a = SrcGpr(m, gpr, rs, x0);
			// C.57: beqz/bnez (rt == r0) is the common form -- compare against the
			// immediate zero instead of materializing a zero into a scratch first.
			if (two && rt) { const Register b = SrcGpr(m, gpr, rt, x1); m.Cmp(a, b); }
			else m.Cmp(a, 0);
		}
		// The taken/not-taken paths fork the compile-time cache state: the
		// delay slot is only EXECUTED on the taken path, so its cache fills,
		// stores and evictions must not leak into the not-taken emission.
		// Snapshot here, emit the taken path (mutating), then restore for the
		// not-taken path. Both paths flush before their chain epilogue.
		const RegCache fork_state = s_rc;
		Label not_taken;
		m.B(&not_taken, InvertCondition(cond));
		if (ds_interp)
		{
			// C.64: hand the taken path to the INTERPRETER at the branch itself --
			// not to a JIT block at bpc, which would start with this same branch and
			// chain straight back here. It re-executes branch + slot exactly as a
			// normal block break does, and only the ops BEFORE the branch are
			// accounted here (the branch's own cycles are the interpreter's).
			StorePCImm(m, bpc);
			EmitCycleBookkeeping(m, cyc_leading);
			s_rc.FlushDirty(m, gpr);
			m.Mov(x16, reinterpret_cast<uint64_t>(&eeRunBasicBlock_arm64));
			m.Blr(x16);
			s_rc.Reset();
			EmitChainEpilogue(m); // continue from wherever the interpreter left pc
		}
		else
		{
			s_emit_pc = bpc + 4; // delay slot
			Cop2RunInvalidate(); s_emit_budget = 1; // C.66
			EmitSimple(m, gpr, ds);
			StorePCImm(m, tconst);
			s_rc.FlushDirty(m, gpr); // block exit (taken)
			EmitTailCycles(m, cyc_taken, true); // C.55 (bookkeeping + update, fused)
			if (idle_skip) EmitIdleSkip();
			EmitKnownExit(m, tconst, evt); // C.54
		}
		s_rc = fork_state;
		m.Bind(&not_taken);
		EmitCycleBookkeeping(m, cyc_nt);
		// Likely branches skip their delay slot when not taken.
		StorePCImm(m, bpc + (likely ? 8 : 4));
		s_rc.FlushDirty(m, gpr); // block exit (not taken)
		// Mirror the interpreter EXACTLY: of the non-likely conditionals, only
		// BEQ/BNE call intEventTest() on the not-taken path (BGEZ/BGTZ/BLEZ/BLTZ
		// and the AL forms do nothing there -- see Interpreter.c); the likely
		// branches BEQL/BNEL/BLEZL/BGTZL/BLTZL/BGEZL do. EXCEPTION: BC0FL/BC0TL
		// and BC1FL/BC1TL are likely (skip the delay slot) but do NOT call
		// intEventTest on not-taken -- COP0.c's BC0xL and FPU.c's BC1L
		// macro only do `cpuRegs.pc += 4`. Always
		// WITHOUT intUpdateCPUCycles (no cpuBlockCycles flush). Deviating in
		// either direction (extra/missing test points, or flushing first) shifts
		// interrupt delivery (EPC) inside wait loops relative to the interpreter,
		// which cascades into different kernel-scheduler decisions (MMX7 FMV
		// loader wedge).
		const bool nt_evt = (op == 0x04 || op == 0x05 || (likely && !bc0 && !bc1 && !bc2));
		EmitKnownExit(m, bpc + (likely ? 8 : 4), nt_evt ? evt : 0); // C.54
		return true;
	}

	// C.77: ERET, the interrupt/exception return (COP0 CO space, funct 0x18),
	// was the single biggest remaining interpreter handoff: every interrupt
	// return broke its block (196k breakers per 600 in-race GT3 frames, 59% of
	// all remaining breakers, dragging the handler tails through the
	// interpreter with them). It is a branch with NO delay slot: pc <-
	// ErrorEPC/EPC selected by Status.ERL, that bit (ERL, else EXL) clears,
	// and nextEventCycle pulls to within 4 cycles -- COP0.c's ERET does the
	// pull BEFORE the branch flushes cpuBlockCycles into cpuRegs.cycle, so the
	// pull here reads the PRE-flush cycle to match the interpreter handoff
	// exactly. Then the usual taken-branch tail: fused cycle flush, gated
	// event test (this test is where an interrupt left pending while EXL
	// masked it fires -- pc must already hold the return address), chain off
	// cpuRegs.pc like JR. TLBR/TLBWI/TLBP and the rest of the CO space stay
	// interpreter handoffs (rare, TLB state machine). intSetBranch() is
	// interpreter-loop bookkeeping and has no JIT equivalent to mirror.
	bool EmitEret(MacroAssembler& m, const Register& gpr, u32 insn, int cyc_leading)
	{
		if ((insn >> 26) != 0x10 || ((insn >> 21) & 31) != 0x10 || (insn & 0x3f) != 0x18)
			return false;
		s_rc.pinned = 0; // new op: previous op's Register handles are dead
		m.Ldr(w0, RegsField(&cpuRegs.CP0.n.Status));
		m.Ldr(w1, RegsField(&cpuRegs.CP0.n.ErrorEPC));
		m.Ldr(w2, RegsField(&cpuRegs.CP0.n.EPC));
		m.Tst(w0, 1u << 2); // Status.ERL
		m.Csel(w1, w1, w2, ne); // pc = ERL ? ErrorEPC : EPC
		StorePC(m, w1);
		m.Bic(w2, w0, 1u << 2); // Status & ~ERL
		m.Bic(w3, w0, 1u << 1); // Status & ~EXL
		m.Csel(w0, w2, w3, ne);
		m.Str(w0, RegsField(&cpuRegs.CP0.n.Status));
		// if ((int)(nextEventCycle - cycle) > 4) nextEventCycle = cycle + 4
		Label no_pull;
		m.Ldr(x0, RegsField(&cpuRegs.cycle));
		m.Ldr(x1, RegsField(&cpuRegs.nextEventCycle));
		m.Sub(w2, w1, w0);
		m.Cmp(w2, 4);
		m.B(&no_pull, le);
		m.Add(x0, x0, 4);
		m.Str(x0, RegsField(&cpuRegs.nextEventCycle));
		m.Bind(&no_pull);
		s_rc.FlushDirty(m, gpr); // block exit: the next block reads GPR memory
		// NO cycle flush and NO event test here -- mirror the interpreter
		// handoff this replaces exactly: eeRunBasicBlock_arm64 is a plain
		// `do execI while (!branch2)` loop, and ERET (unlike the branch ops,
		// which go through intDoBranch) only sets pc and the branch flags, so
		// the old path returned to the chain with cpuBlockCycles carrying the
		// eret's cycles and cpuRegs.cycle still stale; the NEXT block's tail
		// does the flush and its (C.35-gated) event test, and the pulled
		// nextEventCycle makes that gate fire within a few cycles. Doing the
		// flush + an event test here instead looks more faithful but shifts
		// interrupt delivery by one block boundary, which snapped MMX7's FMV
		// to a different frame phase (109232045 vs the 111415017 golden).
		EmitCycleBookkeeping(m, cyc_leading + OpCycles(insn));
		EmitChainEpilogue(m); // continue from EPC (the event test runs at the next tail)
		return true;
	}

	// Register a block on its source pages (dedup: revive cycles re-push after
	// eeJitClear erased only the WRITTEN page's list -- a spanning block's entry
	// on the other page survives) and write-protect them so ANY host write
	// (interpreter store, JIT wrapper store, SIF/IPU DMA memcpy, overlay load)
	// faults -> vtlb PageFaultHandler -> mmap_ClearCpuBlock -> Cpu->Clear ->
	// eeJitClear_arm64 drops the page's blocks. Without this nothing ever
	// invalidates EE blocks on self-modifying code / game overlay loads
	// (MMX7 loads an overlay ~frame 111 and later runs STALE translated
	// code at 0x105138 -> its FMV loader wedges -> post-logo black stall).
	void RegisterPages(u32 pc, u32 ns, u32 ne)
	{
		if (ne <= ns)
			return; // no native-covered bytes -> content-independent block
		for (u32 pg = ns >> kPageShift; pg <= (ne - 1) >> kPageShift; pg++)
		{
			auto& vec = s_page[pg];
			if (!ScanContainsU32(vec.data(), vec.size(), pc))
				vec.push_back(pc);
			// C.60: a manual page is deliberately left writable -- protecting it
			// again is what the ping-pong was. Its blocks validate at entry.
			if (InRam(pg << kPageShift) && !PageIsManual(pg))
				mmap_MarkCountedRamPage(pg << kPageShift);
		}
	}


	BlockFn CompileBlock(u32 pc)
	{
		if (s_code_pos + 8192 > kCodeCacheSize)
		{
			s_blocks.clear();
			s_page.clear();
			LutClearAll();
			s_code_pos = 0;
			s_fm_sites.clear(); // C.50: the code they point at gets overwritten
			s_fm_pending.clear();
		}

		u8* start = s_code + s_code_pos;
		// C.60: decided before emission (the entry check is part of the code we are
		// about to emit) from the pages the block can possibly cover; the exact
		// range is re-checked once it is known, below.
		const u32 startpg = Norm(pc) >> kPageShift;
		const bool manual = PageIsManual(startpg) || PageIsManual(startpg + 1);
		// C.61: a manual block's check needs its record's address while it is being
		// emitted, so the (empty) record goes in first and is filled in below.
		BlockRec* rec_ptr = nullptr;
		if (manual)
		{
			BlockRec& r = s_blocks[pc];
			r = BlockRec{nullptr, Norm(pc), true, true, {}};
			rec_ptr = &r;
		}
		s_fm_pending.clear(); // C.50/C.51: per-block site state
		s_fm_labels.clear();
		s_ma_pending.clear(); // C.53
		s_ma_labels.clear();
		s_exit_pending.clear(); // C.54
		s_exit_labels.clear();
		s_exit_labels.emplace_back();
		s_blk_ret = &s_exit_labels.front(); // the block's one return tail
		memjit_write_begin(); // W^X: code cache writable on this thread until FinalizeCode
		MacroAssembler masm(start, kCodeCacheSize - s_code_pos, PositionDependentCode);
		// The aVU macro emitters (C.30-2) hold RSCRATCHADDR (x17) and q31 across
		// vixl macro expansions -- keep the assembler from synthesizing into them
		// (mirrors armStartBlock in AsmHelpers.cpp).
		masm.GetScratchRegisterList()->Remove(17);
		masm.GetScratchVRegisterList()->Remove(31);

		const Register gpr = x19;
		s_rc.Reset(); // fresh per-block register-cache state (C.27)
		// Uniform 96-byte frame across ALL blocks (the chain epilogue of any
		// block must match any successor's prologue): x19/x20 (gpr base, jr
		// target), x21..x27 (GPR register cache), x28 (vmap base), x30 (chain
		// return).
		masm.Sub(sp, sp, 96);
		masm.Stp(x19, x20, MemOperand(sp, 0));
		masm.Stp(x21, x30, MemOperand(sp, 16));
		masm.Stp(x22, x23, MemOperand(sp, 32));
		masm.Stp(x24, x25, MemOperand(sp, 48));
		masm.Stp(x26, x27, MemOperand(sp, 64));
		masm.Str(x28, MemOperand(sp, 80));
		{
			// Exactly 4 instructions so kChainEntryOffset stays constant
			// (VIXL's Mov would shrink the sequence for compressible values).
			const uint64_t gv = reinterpret_cast<uint64_t>(&cpuRegs.GPR.r[0]);
			vixl::ExactAssemblyScope scope(&masm, 4 * kInstructionSize);
			masm.movz(gpr, gv & 0xffff);
			masm.movk(gpr, (gv >> 16) & 0xffff, 16);
			masm.movk(gpr, (gv >> 32) & 0xffff, 32);
			masm.movk(gpr, (gv >> 48) & 0xffff, 48);
		}
		{
			// x28 = the fastmem area base (C.50) or, when fastmem is off, the
			// vtlb vmap array base (C.49) -- in both cases the VALUE of the
			// field, not its address. 4 fixed instructions for the address plus
			// the load, so the prologue size stays constant for
			// kChainEntryOffset. Every block agrees on which of the two it is
			// (FastmemMode() is latched), which is what lets chained blocks
			// inherit x28 without reloading it.
			const uint64_t vv = FastmemMode()
				? reinterpret_cast<uint64_t>(&vtlbdata.fastmem_base)
				: reinterpret_cast<uint64_t>(&vtlbdata.vmap);
			vixl::ExactAssemblyScope scope(&masm, 5 * kInstructionSize);
			masm.movz(vmapbase, vv & 0xffff);
			masm.movk(vmapbase, (vv >> 16) & 0xffff, 16);
			masm.movk(vmapbase, (vv >> 32) & 0xffff, 32);
			masm.movk(vmapbase, (vv >> 48) & 0xffff, 48);
			masm.ldr(vmapbase, MemOperand(vmapbase));
		}

		// C.60: a block on a manual page is not protected by anything -- check its
		// source before running it. This sits at kChainEntryOffset (the prologue
		// above has a fixed size), so the chained entry from another block runs it
		// as well. x30 is clobbered by the call but the frame already holds it.
		s_stale = nullptr;
		if (manual)
		{
			s_stale_pc = pc;
			masm.Mov(x0, reinterpret_cast<uint64_t>(rec_ptr)); // C.61: no lookup
			masm.Mov(w1, pc);
			masm.Mov(x16, reinterpret_cast<uint64_t>(&eeBlockValidate_arm64));
			masm.Blr(x16);
			s_exit_labels.emplace_back();
			s_stale = &s_exit_labels.back();
			masm.Cbz(w0, s_stale); // source changed -> discarded, back to dispatcher
		}

		int n = 0;
		u32 p = pc; // guest pc cursor advanced across the translated run
		int cyc = 0; // raw sum of the leading translated ops' cycle costs
		bool done = false;
		bool branch_end = false; // ended via EmitBranch: branch + delay slot baked
		Cop2RunInvalidate(); // C.66: liveness never crosses a block boundary
		s_x19HeldVu = false; // C.78: a bracket never outlives its block (every run's last op closes it)
		while (n < kMaxInsns)
		{
			s_emit_budget = (int)(kMaxInsns - n); // C.66: a COP2 run may not outlive the block
			const u32 insn = memRead32(p);
			s_emit_pc = p; // C.50: guest pc of the op being emitted (fastmem site bookkeeping)
			// C.67: fuse the canonical unaligned pair into one host access. Both
			// ops are consumed; the pair never straddles the block cap, and the
			// second op is read from live memory exactly as EmitSimple would.
			if (n + 2 <= kMaxInsns)
			{
				UPair up;
				if (UnalignedPair(insn, memRead32(p + 4), &up))
				{
					EmitUnalignedPair(masm, gpr, up);
					cyc += OpCycles(insn) + OpCycles(memRead32(p + 4));
					p += 8; n += 2;
					continue;
				}
			}
			if (EmitSimple(masm, gpr, insn))
			{
				cyc += OpCycles(insn); p += 4; n++;
				continue;
			}
			if (EmitBranch(masm, gpr, p, insn, cyc)) { done = true; branch_end = true; break; } // emits bookkeeping + pc + eventtest + chain epilogue
			// C.77: ERET ends the block with no delay slot -- advance p past it so
			// page coverage (ne = Norm(p)) includes the eret word itself.
			if (EmitEret(masm, gpr, insn, cyc)) { p += 4; n++; done = true; break; }
			break; // unsupported -> interpreter finishes the basic block
		}

		// C.44: the block-length cap (n == kMaxInsns) was reached with every op
		// translated and no branch yet -- i.e. a straight-line run longer than
		// kMaxInsns (unrolled copy/vector setup, common in MMX7). Don't hand the
		// fully-translatable tail to the interpreter; end the block cleanly at p
		// and let the chain continue translating from there. This was the #1
		// "breaker" in the handoff stats (LD/SD/etc. sitting at position 64).
		if (!done && n == kMaxInsns)
		{
			StorePCImm(masm, p);
			EmitCycleBookkeeping(masm, cyc);
			s_rc.FlushDirty(masm, gpr);
			EmitKnownExit(masm, p, 0); // C.54
			done = true;
		}

		if (!done)
		{
			if (n > 0)
			{
				masm.Ldr(w0, RegsField(&cpuRegs.pc)); masm.Add(w0, w0, 4 * n); masm.Str(w0, RegsField(&cpuRegs.pc));
				EmitCycleBookkeeping(masm, cyc);
			}
			// The interpreter tail reads AND writes guest GPR memory: flush the
			// dirty cache before the call, and treat everything as unknown after
			// (the block ends right away, but keep the invariant explicit).
			s_rc.FlushDirty(masm, gpr);
			masm.Mov(x16, reinterpret_cast<uint64_t>(&eeRunBasicBlock_arm64));
			masm.Blr(x16);
			s_rc.Reset();
			EmitChainEpilogue(masm);
		}

		EmitFastmemStubs(masm);       // C.51: out of line, past the epilogue -- nothing falls into it
		EmitMisalignStubs(masm, gpr); // C.53: same, for the misalign cancels
		EmitExitStubs(masm);          // C.54: event-due tails + the block's single return path
		masm.FinalizeCode();
		memjit_write_end();

		const size_t sz = masm.GetSizeOfCodeGenerated();
		memsync(reinterpret_cast<void*>(start), (u8*)(reinterpret_cast<void*>(start)) + (sz));

		// C.50: buffer offsets -> absolute addresses. Blocks are emitted at
		// increasing s_code_pos and offsets grow within a block, so appending
		// keeps s_fm_sites sorted by code address; the only thing that breaks
		// that ordering is a code-cache wrap, which clears the list.
		for (const FmPending& fp : s_fm_pending)
			s_fm_sites.push_back({reinterpret_cast<uintptr_t>(start) + fp.site_off,
			                      reinterpret_cast<uintptr_t>(start) + fp.stub_off, fp.pc});
		s_fm_pending.clear();
		s_fm_labels.clear(); // the labels are resolved; the block is finalized
		s_ma_pending.clear();
		s_ma_labels.clear();
		s_exit_pending.clear();
		s_exit_labels.clear();
		s_blk_ret = nullptr;
		s_code_pos += (sz + 15) & ~size_t(15);

		BlockFn fn = reinterpret_cast<BlockFn>(start);
		// Native code covers the leading translated run; a translated branch also
		// bakes the branch + delay slot (p still points at the branch there).
		const u32 ns = Norm(pc), ne = Norm(branch_end ? p + 8 : p);
		if (!manual && RangeIsManual(ns, ne))
			return fn; // ran once, never cached: recompiled fresh on the next entry
		BlockRec rec{fn, ne, true, manual, {}};
		if (ne > ns)
		{
			rec.src.resize((ne - ns) >> 2);
			for (u32 q = 0; q < (u32)rec.src.size(); q++)
				rec.src[q] = memRead32(pc + q * 4);
		}

		s_blocks[pc] = std::move(rec); // overwrites a dead record after SMC
		if (InRam(ns)) s_lut[ns >> 2] = fn;
		RegisterPages(pc, ns, ne);
		return fn;
	}

	// Revive a dead block if its baked-in source words are unchanged in live
	// memory (the page-clearing write hit data, not this code). Reinstalls the
	// existing native code and re-protects the page; returns nullptr when the
	// source really changed (caller erases + recompiles).
	BlockFn Revive(u32 pc, BlockRec& r)
	{
		// C.60: the page went manual under it -- recompile so it gets its check.
		if (!r.manual && RangeIsManual(Norm(pc), r.end))
			return nullptr;
		for (u32 q = 0; q < (u32)r.src.size(); q++)
			if (memRead32(pc + q * 4) != r.src[q])
				return nullptr;
		r.live = true;
		const u32 ns = Norm(pc);
		if (InRam(ns)) s_lut[ns >> 2] = r.fn;
		RegisterPages(pc, ns, r.end);
		return r.fn;
	}

	// C.60: entry check for a block compiled from a manual (unprotected) page.
	// Emitted at kChainEntryOffset, so BOTH the dispatcher's call and a chained
	// jump from another block run it. Returns 0 when the source words changed --
	// the block is discarded here, and its code branches to the stale tail,
	// which hands the pc back to the dispatcher for a fresh compile.
	// C.61: the record is reached through the pointer baked into the block at
	// compile time. Looking it up by pc (an unordered_map probe on every single
	// block entry, chained ones included) was 2.1% of the EE thread on its own --
	// more than the comparison it exists to perform. References into an
	// unordered_map are stable across inserts, so the pointer stays good for the
	// life of the block, and the block is erased only from here or from a clear.
	extern "C" u32 eeBlockValidate_arm64(BlockRec* r, u32 pc)
	{
		const u32 np = Norm(pc);
		const size_t bytes = r->src.size() * sizeof(u32);
		bool same;
		if (InRam(np) && eeMem)
			same = memcmp(eeMem->Main + np, r->src.data(), bytes) == 0;
		else
		{
			same = true;
			for (u32 q = 0; same && q < (u32)r->src.size(); q++)
				same = memRead32(pc + q * 4) == r->src[q];
		}
		if (same)
			return 1;
		if (InRam(np))
			s_lut[np >> 2] = nullptr;
		s_blocks.erase(pc);
		return 0;
	}

	inline BlockFn BlockForPC(u32 pc)
	{
		const u32 np = Norm(pc);
		if (InRam(np))
		{
			BlockFn f = s_lut[np >> 2];
			if (f)
				return f;
			auto it = s_blocks.find(pc);
			if (it != s_blocks.end())
			{
				if (BlockFn g = Revive(pc, it->second))
					return g;
				s_blocks.erase(it); // source changed -> compile fresh
			}
			return CompileBlock(pc);
		}
		auto it = s_blocks.find(pc);
		return (it != s_blocks.end() && it->second.live) ? it->second.fn : CompileBlock(pc);
	}
}

void eeJitReserve_arm64(void)
{
	s_ok = VixlEmitSelfTest();
	if (!s_code)
	{
		s_code = (u8*)memjit_alloc(kCodeCacheSize);
	}
	if (!s_lut)
	{
		s_lut = (BlockFn*)mmap(nullptr, (size_t)kRamWords * sizeof(BlockFn), PROT_READ | PROT_WRITE,
		                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (s_lut == MAP_FAILED) s_lut = nullptr;
	}
	s_page_clears.assign(kRamBytes >> kPageShift, 0); // C.60
	s_ok = s_ok && s_code && s_lut;
	log_cb(RETRO_LOG_INFO, "arm64 EE rec (C.7): %s.\n", s_ok ? "native ALU+mem+branch+FPU-mov+MMI+muldiv JIT (block-linking)" : "FAILED");
}

void eeJitReset_arm64(void)
{
	s_blocks.clear();
	s_page.clear();
	LutClearAll();
	s_code_pos = 0;
	// C.50: the code these sites point at is about to be overwritten. Faulting
	// pcs are kept -- they are guest addresses, still valid, and re-learning
	// them would mean re-taking every fault.
	s_fm_sites.clear();
	s_fm_pending.clear();
}

void eeJitClear_arm64(u32 addr, u32 size)
{
	// The clear MUST cover every block on the touched pages: the fault handler
	// unprotects the whole page, so later writes to it don't fault and any
	// surviving block would go stale silently. Blocks are marked dead (records
	// and native code kept); the next dispatch revalidates the source snapshot
	// and revives the block without recompiling when the code bytes survived.
	if (s_blocks.empty())
		return;
	const u32 a0 = Norm(addr);
	const u32 a1 = Norm(addr + (size ? size : 1) * 4 - 1);
	for (u32 pg = a0 >> kPageShift; pg <= a1 >> kPageShift; pg++)
	{
		auto it = s_page.find(pg);
		if (it == s_page.end())
			continue;
		if (pg < s_page_clears.size())
			s_page_clears[pg]++; // C.60: enough of these and the page goes manual
		for (u32 spc : it->second)
		{
			auto bit = s_blocks.find(spc);
			if (bit != s_blocks.end())
				bit->second.live = false;
			const u32 np = Norm(spc);
			if (InRam(np)) s_lut[np >> 2] = nullptr;
		}
		s_page.erase(it);
	}
}

void eeJitShutdown_arm64(void)
{
	s_blocks.clear();
	s_page.clear();
	if (s_code) { memjit_free(s_code, kCodeCacheSize); s_code = nullptr; }
	if (s_lut) { munmap(s_lut, (size_t)kRamWords * sizeof(BlockFn)); s_lut = nullptr; }
	s_code_pos = 0;
}

// C.50: a fastmem access touched a page the vtlb doesn't back with memory (a
// hardware register, or an unmapped vaddr). Rewrite the faulting instruction
// into a branch to the slow stub that was emitted next to it, so the kernel's
// retry of the same pc takes the wrapper call instead, and blacklist the guest
// pc so later compiles of that op don't use fastmem at all.
//
// Runs on the faulting (EE) thread from the SIGSEGV handler. It only reads a
// sorted vector that is mutated by the same thread while COMPILING -- never
// while executing, which is the only time a fault can arrive -- and writes 4
// bytes of RWX code cache. Returns false for any fault we don't own, which
// lets the handler fall through to its default (abort) path.
extern "C" bool eeFastmemFault_arm64(uintptr_t code_address)
{
	size_t lo = 0, hi = s_fm_sites.size();
	while (lo < hi)
	{
		const size_t mid = lo + ((hi - lo) >> 1);
		if (s_fm_sites[mid].code < code_address) lo = mid + 1; else hi = mid;
	}
	if (lo >= s_fm_sites.size() || s_fm_sites[lo].code != code_address)
		return false;

	const FmSite site = s_fm_sites[lo];
	const ptrdiff_t delta = (ptrdiff_t)(site.stub - site.code) >> 2; // in instructions
	if (delta < -(1 << 25) || delta >= (1 << 25))
		return false; // out of B range -- impossible within one block, but don't corrupt code

	u32* const insn = reinterpret_cast<u32*>(site.code);
	memjit_write_begin();
	*insn = 0x14000000u | (static_cast<u32>(delta) & 0x03ffffffu); // B <stub>
	memjit_write_end();
	memsync(insn, (u8*)(insn) + (sizeof(u32)));

	const auto it = std::lower_bound(s_fm_faulting.begin(), s_fm_faulting.end(), site.pc);
	if (it == s_fm_faulting.end() || *it != site.pc)
		s_fm_faulting.insert(it, site.pc);

	return true;
}


void eeJitRunBlock_arm64(void)
{

	BlockForPC(cpuRegs.pc)();
}
