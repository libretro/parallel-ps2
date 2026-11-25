# LRPS2

PlayStation 2 emulation core for [libretro](https://www.libretro.com/) (RetroArch),
based on PCSX2. This fork's headline work is a native **AArch64 (arm64) port**:
upstream PCSX2 emulation relies on x86-64 recompilers, which do not exist on ARM —
this tree adds arm64 recompilers for the EE and IOP plus the supporting
infrastructure to run correctly (and reasonably fast) on 64-bit ARM Linux devices.

## Status (arm64)

Verified booting to real in-game content (Mega Man X7 gameplay, Gran Turismo 3
intro/FMV) on a Snapdragon-class device (Adreno 618, 4K-page Linux, glibc
RetroArch flatpak + musl test host).

**Profile from a save state, not from a boot.** A headless run can only reach
content the emulator walks to by itself, and for Gran Turismo 3 that is its
opening movie -- 9000 frames (150 s of emulated time) with Start auto-pressed
still land in the intro. Profiles taken that way put the IPU (`IPUWorker` +
`yuv2rgb`, ~14 %) on top and leave the MTVU thread at 0.2 % of CPU, i.e. they
measure the video decoder rather than the game, and they sent two optimisation
attempts down blind alleys before that was noticed. Resuming from an in-race
save state instead:

| Thread | CPU | Inside it |
|---|---|---|
| EE | 41 % | 50 % in its own JIT, 43 % native C++ (SPU `Mix` 8.9 %, `memcpy` 4.0 %, event test 2.1 %), 7 % IOP JIT |
| GS | 37 % | Vulkan renderer |
| MTVU | 23 % | 75 % inside microVU1's generated code |

No IPU at all, and the event test that looked like a 6-7 % problem is 2.3 %.
Any performance claim here should say which of the two it came from.

Three findings that came out of getting that profile:

* **Loading a save state used to drop the EE onto the interpreter for the rest
  of the session** (3.5x slower: GT3 in-race 92 s -> 26 s per 600 frames). The
  interpreter's execute loop always restarted in its boot stage, whose
  "interpret until the pc reaches EELOAD" condition can never be satisfied
  mid-game, so the JIT stage was never reached. Fixed: the loop resumes in
  GAME_RUNNING when `g_GameStarted` (which is part of the save state) says the
  entry point is behind us.
* **The EE thread is the critical path, and it is not waiting.** Wall-clock
  timing of the sync points (`LRPS2_SYNC_STATS=1`) puts the EE's blocking at
  0.97 s on the GS and 0.00 s on MTVU over a 600-frame in-race run. Letting the
  EE run frames ahead of the GS, or turning MTVU off, both measure as nothing.
  (The sampling profiler's cross-thread percentages are undercounts -- SIGPROF
  does not queue -- so only within-thread splits are trustworthy.)
* **The EE thread is not instruction-count-bound either.** The in-race handoff
  work (C.63-C.65) removed 63 % of the interpreter handoffs, and the code-quality
  work (C.66-C.67) shrank the hottest block 35 % (3068 -> 1984 host bytes:
  dead COP2 flag pipelines dropped, unaligned pairs fused to single accesses) --
  and wall time did not move for any of them. On this core (A76/A55) the OoO
  front end absorbs the removed instructions; what remains is bound by memory
  and dependency chains, so further emitted-code polishing has a low ceiling.
  These changes are kept for coverage, interpreter-fidelity, code density and
  x86 parity, not for speed.
* **The remaining native-C++ "plumbing" on the EE thread is protocol cost, not
  waste.** Single-level caller attribution for leaf symbols
  (`LRPS2_PROF_LR=memcpy,memcmp,__aarch64_` -- the SIGPROF handler records LR
  next to PC, and for a leaf function LR is the caller) pins it down: `memcpy`
  (4 %) is 59 % the MTGS ring write (`WRITERING_DMA`) and 31 % the MTVU ring
  write (`VU_Thread::VifUnpack`); the outlined atomics (2.2 %) are 84 % the
  `WorkSema::NotifyOfWork` fetch_add, one per VIF unpack packet; `memcmp`
  (1.2 %) is entirely the C.60 block-revalidation check. The WorkSema RMW in
  particular must stay *per notify*: an RMW always observes the latest state,
  which is what makes the sleep/wake handshake sound -- the tempting
  load-then-skip variant can read a stale RUNNING_N while the worker is going
  to sleep and miss the wakeup. What C.80 does instead is issue **fewer
  notifies**: a VIF unpack packet only publishes its ring data (the release
  store of the write pointer stays per-packet, so an awake worker drains it
  immediately) and defers the wakeup RMW to the next flush point -- any other
  MTVU command (they all notify), `WaitVU` (mandatory: `WaitForEmpty` trusts
  the sema state machine), `Close`, or the VIF1 DMA/MFIFO end-of-transfer
  tails (latency bound). In-race GT3 that removes **82 %** of the notifies
  (3.9M unpacks -> 0.7M notifies per 600 frames) and the fetch_add leaves the
  EE-thread profile (2.2 % -> below threshold); wall time is flat, as with
  every plumbing change since C.49 (`LRPS2_NO_VIF_LAZYKICK=1` restores the
  per-packet notify, `LRPS2_MTVU_STATS=1` reports the deferred/issued counts).
  An SPU worker thread was measured and rejected: both test titles run with an
  SPU IRQ armed 93-98 % of all sample ticks and GT3's in-race IRQs are 100 %
  mixer-voice IRQA crossings, discovered tick-exactly during mixing -- async
  mixing cannot deliver them at the bit-identical IOP cycle, and the only
  sound design (speculative mixing with rollback) is out of proportion to the
  ~5-6 % ceiling (`LRPS2_SPU_SYNC_STATS=1` collects the evidence).

Overall recompiler-suite progress: roughly **~88 %** (weighted by remaining
work; dynamic instruction coverage on the tested titles is much higher — the
vast majority of EE+IOP instructions already execute natively, and the
remaining interpreter handoffs are mostly untranslatable exceptions). Opcode
coverage is essentially done; the current work is the quality of the emitted
code, guided by the in-tree sampling profiler (`LRPS2_PROF=1`), which buckets
PC samples by JIT code cache and resolves the rest against the core's symbol
table.

### EE (R5900) JIT — ~89 %

Native ALU, loads/stores incl. LQ/SQ (inline vtlb fast path) + unaligned family, branches + likely branches + BC0x/BC1x, block linking/chaining, MULT/DIV/MADD + HI/LO (both pipelines), **complete COP1 (FPU)** — data moves + all S-format arithmetic + CVT + C.cond, interpreter-exact clamp/flag semantics, **CFC1/CTC1 native** (C.65: `fs` is a compile-time constant, so CFC1 specializes to FCR31 load / revision constant / zero, and CTC1 to a non-FCR31 register is a nop), bit-exact MMI subset → NEON, MFSA/MTSA(B/H), kernel idle-loop skip, block revalidation cache, write-back GPR register cache with direct-to-cache emission, event tests gated on `cycle >= nextEventCycle` in block tails (the upstream-rec dispatcher rule; ~23 % faster wall clock on GT3, `LRPS2_NO_EVTGATE=1` restores every-branch tests), default-rate cycle bookkeeping inlined into block tails (`LRPS2_NO_INLINE_UPD=1` restores the helper call), block chaining keeps the frame live across hops (no per-block frame pop/push or x19 rematerialization; same on the IOP side). ADDI/DADDI (overflow trap dropped, exactly like the x86 rec), COP0 EI/DI, LQC2/SQC2 (128-bit VU0-register load/store, native since C.59: vu0Sync plus a 128-bit access straight into vuRegs[0].VF[ft], so the EE register cache survives an op MMX7 runs ~827K times; `LRPS2_NO_EE_VUQMEM=1` restores the interpreter call). **Memory goes through the vtlb fastmem area** (C.50): the 4 GB host mapping in which every memory-backed guest page sits at its own guest address, so a load/store is one instruction (`ldr/str <data>, [x28, w<addr>, UXTW]`) with no vmap indirection. Hardware registers and unmapped pages are absent from the area, so touching one faults; the faulting instruction is rewritten into a branch to a slow stub emitted next to it at compile time, and the guest pc is blacklisted so later compiles of that op skip fastmem (`LRPS2_NO_FASTMEM=1` restores the old inline vmap path, `LRPS2_FASTMEM_LOG=1` traces the patches). Emitted-code quality: cpuRegs fields are reached through the x19 guest-reg base instead of a 4-instruction absolute address per access (C.46); load/store displacements fold into the address add (C.48); the vmap/fastmem base is pinned in x28 for the life of the block frame (C.49); the backpatch stubs (C.51), the misalign cancel paths (C.53), the event-due tails and the frame pop/ret (C.54) all live out of line past the epilogue, so the hot instruction stream carries no cold code; the chain epilogue folds the mirror mask and RAM range check into one test (C.52); an exit whose successor is known at compile time chains through a constant LUT slot with no pc read at all, on the event gate's not-due path where pc provably has not been redirected (C.54); the tail's cycle bookkeeping and cycle update are fused into one sequence (C.55); the FP loads/stores use fastmem too (C.56); and ADDIU/DADDIU fold their immediate into the add (C.57); the canonical unaligned pairs (LDL/LDR, LWL/LWR, SDL/SDR, SWL/SWR -- adjacent, left-then-right, matching registers and offsets) fuse into ONE host unaligned access through fastmem instead of two helper calls each, with a fault stub that replays the exact two-access guest-order semantics for hw pages (C.67, `LRPS2_NO_EE_UPAIR=1` restores the helpers). **Handoff coverage (C.63-C.65), measured from an in-race GT3 save state rather than an intro movie:** ADD/SUB (trap dropped like ADDI), PLZCW (= `CLS` per 32-bit half), PREVH (= `REV64 .8H`) and CFC1/CTC1 are native, and a **likely branch is no longer refused by an untranslatable delay slot** -- GT3 is full of `beql ..., panic; break` asserts whose slot only runs when taken, i.e. never; the not-taken path is emitted natively and the taken path hands control to the interpreter at the branch (C.64). **ERET is native too (C.77)**: the interrupt/exception return was the single biggest remaining breaker (196k per 600 in-race frames -- every interrupt return broke its block); it is a branch with no delay slot (pc from ErrorEPC/EPC by Status.ERL, clear that bit, pull nextEventCycle), and its tail deliberately does NO cycle flush and NO event test, mirroring the interpreter-handoff runner it replaces (a plain `do execI while (!branch2)` loop) -- the next block's gated tail delivers the pending interrupt; adding a flush+test here instead shifted MMX7's FMV frame phase (`LRPS2_NO_EE_ERET=1` restores the handoff). **The remaining MMI stragglers are native too (C.79)**: PPAC5 (per-word RGB1555 pack → NEON shift/mask), and the full-128-bit HI/LO moves PMFHI/PMFLO (`rd <- HI/LO`) and PMTHI/PMTLO (`HI/LO <- rs`), all bit-exact (`LRPS2_NO_EE_MMIHL=1` restores the handoff). With those gone the only EE ops that still break a block are SYSCALL and the TLB/exception path — everything else GT3 and MMX7 execute is translated. (PMFHL/PMTHL stay with the interpreter deliberately: they never fire in the reference titles, so a native path could not be golden-verified.) Interpreter handoffs in-race: 887k -> 136k breakers per 600 frames (-85 % from the C.62 start), and what remains is SYSCALL (123k, exception path). Translating PLZCW also FIXED a timing deviation: a block that breaks at an untranslatable op flushes its cycle count there, a rounding point the interpreter does not have -- with PLZCW native, MMX7's frame-3000 framebuffer now matches the pure interpreter (88190948), which the old JIT did not. The bit-exactness reference for MMX7 moved accordingly. `LRPS2_DUMP_RANGE=lo:hi` dumps the guest ops a pc range compiles from with their translatability, and `LRPS2_DUMP_HOST=<pc>` writes a block's emitted host code out for `objdump -D -b binary -m aarch64`. Missing: SYSCALL/TLB (interpreter forever); PMFHL/PMTHL (never fire in the reference titles)

### IOP (R3000A) JIT — ~98 %

Native ALU incl. the trapping forms ADDI/ADD/SUB (this interpreter drops the overflow trap, so they are the U ops — same call the EE made in C.42) and RFE (pure Status bit shuffle) (C.68, `LRPS2_NO_IOP_C68=1` restores the interpreter), aligned + unaligned (LWL/LWR/SWL/SWR) loads/stores, branches + delay slots, block linking, MULT/DIV + HI/LO moves, COP0 moves (MFC0/CFC0/MTC0/CTC0), write-back GPR register cache (w22–w27, `LRPS2_NO_IOP_REGCACHE=1` to disable), inline RAM fastmem for loads (`LRPS2_NO_IOP_FASTMEM=1` to disable; the remaining helper-call traffic is genuine HW-register polling), event tests gated on `cycle >= iopNextEventCycle` in branch tails (`LRPS2_NO_IOP_EVTGATE=1` to disable); J native (only the module-import stub form `j ...; li $zero, fn` stays interpreted for the HLE hook — and that hook's resolution is cached per stub pc (C.71): the up-to-0x2000-byte backward magic scan, the `std::string` heap allocation and the library-name compare chain ran on EVERY stub execution, millions of times a minute; now a hit revalidates its inputs with three RAM reads (`LRPS2_NO_IOP_JSTUB_CACHE=1` restores the uncached path — `iopMemReadString` left the profile, ~1.5-2 % of the EE thread). A straight-line run longer than the block cap chains to the next block instead of handing its fully-translatable tail to the interpreter (C.69, the EE's C.44 rule; `LRPS2_NO_IOP_CAP_CHAIN=1` to disable), and an interpreter-tail block also covers its breaker word for SMC invalidation, so code loaded OVER what was data at compile time (IRX module loads) recompiles instead of re-handing off forever (C.70 — one stale MMX7 block was re-interpreting `lw ra` thousands of times a run). **Handoff coverage is closed:** `LRPS2_IOP_HANDOFF_STATS=1` (histogram of interpreted ops, EE-style) shows exactly two breaker classes left on both test titles — the J import stub (HLE hook, by design) and SYSCALL (exception, interpreter forever); GTE/COP2 is PS1-only and never fires. The kernel idle spin (`j <self>` + nop) is fast-forwarded to the next event or the end of the cycle budget rather than emulated an iteration at a time (C.47, `LRPS2_NO_IOP_IDLE=1` to disable) — it was 18 % of all JIT time; psxRegs fields go through the x19 base (C.46)

### VU1 recompiler — ~70 %

**microVU1 (armsx2 aVU transplant) is the default provider**: native AArch64 VU codegen, MVU_DIFF-verified register-exact against the interpreter on both test titles, byte-identical framebuffers through 12000-frame runs. Instant-VU1 and MTVU are back to default-on with it. **In-race profile facts (GT3 save state)**: EE never waits on MTVU (SyncStats 0.00 s) so VU1 speed does not gate wall time; mvu1 is ~75 % of the MTVU thread and FLAT (150 blocks, hottest 5.6 % of JIT) — no per-block target exists, only systematic emitter quality. First such passes done: C.72 folds the page offset of absolute-address accesses (flag spills, clamp constants) into the load/store instead of a separate `add` after `adrp` (`armAbsMemOperand`), and C.73 replaces the naive copy+4×Ins `mVUshufflePS` permute with the cheap NEON form where one exists (Dup/Rev64/Ext/identity; 2-lane swaps insert only the moved lanes) — and C.74 pins `&mVU` in x27 (`RVUMVU`, micro mode only — macro mode's x27 belongs to the EE rec) with the hot per-op scalars moved ahead of the ~290KB `prog` member so they encode as scaled immediates, while `&mVU.regs()` fields ride the existing x19 — and C.75 embeds copies of the emit-constant tables (clamp bounds, FTOI/ITOF scales, EFU polynomials) in `microVU` itself so the per-clamp constant loads are one `[x27 + imm]` instruction — the hottest block dropped 1776 → 1292 host bytes (−27 %), adrp count 54 → 9, and the register-exact MVU_DIFF shadow run from the in-race save state produces a byte-identical divergence log to the pre-C.72 baseline (the 9 logged programs are the known pre-existing interp-vs-microVU last-bit FP differences). LRPS2_PROF now attributes mvu0/mvu1 samples per guest block and `LRPS2_DUMP_HOST_MVU=0x<pc>` dumps a block's host code. Fallbacks: `LRPS2_VU1REC_PAIR=1` (interp-pair rec), `LRPS2_NO_VU1REC=1` (interpreter; both restore conservative non-instant/opt-in-MTVU behavior)

### VU0 / COP2 — ~78 %

**microVU0 runs VU0 micro programs** (VCALLMS) natively; **COP2 macro ALU ops emit native NEON directly into EE JIT blocks** (aVU_Macro single-op emitters) with **real flag liveness** (C.66): the analysis runs over a run of consecutive macro ALU ops, capped at the block end, and only the last writer of each flag category computes it -- the same flag-hack the x86 rec ships by default (its `COP2FlagHackPass`, here run-local because this builder interleaves analysis with emission). Dead intermediate MAC/status pipelines (~40 host instructions per op) vanish: the hottest in-race GT3 block, VU0-macro matrix code, dropped 3068 -> 2004 host bytes. Wall-time flat (OoO absorbed the ALU); the win is code size, I-cache and x86 parity. `LRPS2_NO_COP2_LIVENESS=1` restores all-live; BC2 branches native; **the COP2 transfers (QMFC2/CFC2/QMTC2/CTC2) are native too** (C.58) -- the interlock bit is known at compile time, so the `_vu0FinishMicro`/`_vu0WaitMicro` call is only emitted when set, and CFC2's REG_R quirk (writes UL[0], leaves UL[1]) is reproduced exactly. CTC2 to FBRST/CMSAR1 (VU reset / VU1 microprogram kick) and CALLMS/CALLMSR stay on the interpreter call, faithful to x86. **C.78 macro emission quality**, on the EE critical path (VU0 macro runs in the EE thread): the COP2 transfer bodies and the conservative VU0-busy check address vuRegs through an adrp+pageoff fold instead of a 4-instruction absolute materialization each (`LRPS2_NO_COP2_ABSFOLD=1` restores them); the per-op macro bracket -- VU0-busy check, x19 repoint to &vuRegs[0], x19 restore to &cpuRegs (~11 instructions per op) -- is **hoisted to once per run** of consecutive native macro ALU ops (the C.66 liveness run): between the ops of a run nothing else is emitted, events only fire at block tails, and no run op can start a VU0 microprogram, so the bases stay valid across the run's whole extent (`LRPS2_NO_COP2_RUNHOIST=1` restores per-op brackets); and **x27 = &microVU0 is materialized inside the bracket**, so mvuAbsMem's one-instruction `[x27 + imm]` addressing of the mVU scalars and the C.75 embedded constant tables now applies in macro mode too (`LRPS2_NO_COP2_MX27=1` restores the adrp fold). FOOTGUN captured in the run predicate: VNOP/VWAITQ have EMPTY emitters (no setupMacroOp/endMacroOp), so a hoisted bracket ending on one never emits its x19 restore -- GT3's VU0->GS upload loop ends `VSQI, VSQI, VNOP, VNOP` and the following branch then read its GPRs out of VF registers (instant wedge); they are excluded from runs and emit through the per-op path. Hottest in-race block (VU0-macro matrix code) 1860 -> 1540 host bytes (-17 %); wall flat (C.49 lesson: OoO absorbs it), the win is code size, I-cache and parity. `LRPS2_NO_VU0REC=1` / `LRPS2_NO_EE_COP2MACRO=1` / `LRPS2_NO_EE_COP2XFER=1` fall back

### VIF unpack dynarec — ~100 %

NEON unpack kernels (armsx2 transplant); portable C fallback (`LRPS2_NO_VIF_DYNAREC=1`)

### SMC / overlays

Compiled pages are write-protected; faults invalidate stale blocks (vtlb `mmap_MarkCountedRamPage` flow). A page the game keeps *writing* would otherwise ping-pong forever — fault, unprotect, drop blocks, revive them, re-protect, fault again — at a SIGSEGV plus four `mprotect` calls a cycle (the RAM page and its alias in the fastmem area). After `kManualClears` clears a page is therefore left writable and its blocks check their own source words at entry instead (C.60), which is where the chained entry point sits, so a chained jump validates too; the check is handed its block record at compile time rather than looking itself up (C.61). On GT3 that took the run from 48006 protects / 47803 clears to 251 / 49 — two pages had been ping-ponging ~23k times each — and `mprotect` left the profile entirely. `LRPS2_NO_EE_MANUAL=1` restores protect-on-every-revive, `LRPS2_SMC_STATS=1` dumps the per-page protect/clear counts

### MTVU (VU1 thread)

**Default-on** with microVU1 (≥3 cores; `LRPS2_NO_MTVU=1` disables). Partial-packet flush protocol prevents the continuous-microprogram livelock; with an interp-style VU1 provider MTVU stays opt-in (`LRPS2_MTVU=1`). **Lazy VIF-unpack kick (C.80)**: an unpack packet publishes its ring data but defers the WorkSema wakeup RMW to the next flush point (any other MTVU command, `WaitVU`, `Close`, or the VIF1 DMA/MFIFO end-of-transfer tails) — in-race GT3 82 % of the notifies disappear (`LRPS2_NO_VIF_LAZYKICK=1` restores per-packet notify, `LRPS2_MTVU_STATS=1` reports the counts)

### GS renderer

Standard **Vulkan** renderer (`pcsx2_renderer = "Vulkan"`). paraLLEl-GS requires GPU features (8/16-bit storage, small subgroups) that e.g. Adreno 618 lacks

### MTGS

Works (GS thread active)


Correctness methodology: headless libretro harness runs are compared for
byte-identical framebuffer output against the interpreter (`LRPS2_NO_EEREC=1`)
and the mem-interpreted JIT (`LRPS2_NO_EE_MEM=1`) baselines; JIT-vs-baseline
lockstep is verified with per-frame RAM+scratchpad hashes, per-op GPR-hash
traces, and interrupt/syscall vector logs (see debug toggles below).

## Float accuracy: the ps2float work

The PS2's FPU and VUs are not IEEE-754: no NaN or infinity, a wider effective
range at the top of the exponent scale, truncating (chop) arithmetic, an adder
that discards low bits of the smaller operand by exponent distance, and a
multiplier built from a truncated Booth/carry-save tree whose last-bit
behaviour differs from a correctly rounded multiply. Upstream approximates
this with clamping hacks and per-game fixes. This tree models it.

### The model

`pcsx2/ps2float.c/.h` is a C89, dependency-free, bit-exact software model of
the PS2 float pipeline (add, sub, mul, madd chains, min/max, compares,
conversions), validated against the reference implementation from PCSX2's own
research (31.9 million cross-checked cases). Everything below is proven
against it; "byte-exact" in this section always means byte-exact against this
model, which is byte-exact against hardware for the covered operations.

### What is exact, where

* **EE FPU (COP1), interpreter:** fully soft, unconditionally.
* **EE FPU, x86 and arm64 recompilers:** ADD/SUB byte-exact through an
  extended-range double-precision pipeline; the MUL/MADD family through the
  same pipeline (near-exact; last-ulp divergences only); MAX/MIN and all
  compares as integer operations (exact, and faster than the clamped
  sequences they replaced). A `pcsx2_fpu_softfloat` option routes the
  remainder through the model for full exactness at interpreter cost.
* **VU interpreters:** fully soft, unconditionally.
* **VU recompilers (x86 mVU and arm64 aVU), add/sub:** the hardware adder's
  exponent-distance operand masking, emitted inline. Reduces mismatch against
  the model from 44.9 % of random cases to 0.79 % (the remainder is the
  exponent-255 range single precision cannot represent). This replaced the
  old VuAddSub gamefix outright; Tri-Ace titles need the option on.
* **VU recompilers, multiply:** a full emitted model of the truncated Booth
  tree, byte-exact including the discarded-carry correction, behind a guard
  that runs the tree only when the correction could matter.

### Options (all runtime, per-game)

| Option | Default | Cost (Tekken Tag, 4K, uncapped) |
|---|---|---|
| VU Accurate Add/Sub | on | ~14 fps of ~176 (was ~30 before the optimisation arc) |
| EE Accurate FPU Arithmetic | on | small |
| VU Exact Multiply | off | large (~60 fps); the full hardware multiplier is expensive by nature |

### The optimisation arc, and how it is proven

The masking and the multiplier went through several emission generations, each
required to be bit-identical to the last. The standard that emerged, after an
early rework passed every review and still broke rendering in the wild: an
**emitted-bytes differential harness** assembles both generations with the
real emitter, executes them as machine code under the VU's rounding mode
(chop, denormals-are-zero, flush-to-zero) across register patterns including
xmm15 operands, and compares every result lane. Nothing lands on intrinsics
proofs or code review alone. Under that standard:

* Add/sub mask: two-sided 38-op form -> fused single-keep 33-op form (the
  algebra proven over 80.7 M lanes; the differential also caught a real
  distance-24 boundary bug in the shipping version) -> 20-op AVX2 form with
  per-lane variable shifts -> 15-op AVX-512 ternary-logic form. Dispatched at
  recompile time by CPU capability; SSE remains the floor.
* Exact multiply: the slow path became a register-preserving emitted tree
  stub (no flush at call sites); the guard gained oracle-proven exclusions --
  zero operands, and any multiplier whose recoded mantissa has sixteen or
  more trailing zeros (the sharp soundness boundary; fifteen is measurably
  unsound) -- taking the game-data trip rate from 99.8 % to 51 %. The stub
  also has an AVX-512 ternary-logic build (a third smaller). vf0-sourced
  broadcast add/subs (the canonical VU register move) emit as the proven
  identity instead of mask-plus-add.
* The emitter grew VEX and minimal EVEX layers (vpsllvd, vpternlogd, blendv,
  the shift-immediate forms) -- each encoding executed in the differential
  before any consumer shipped.

#
### The EE approximation ceiling, measured

The EE FPU ladder ends at softfloat for a reason that is now
quantified rather than assumed. Against the ps2float model on thirty
million random operand pairs: the recompiled divide under host
nearest rounding is already exact for 81.7 percent of operands, with
every miss exactly one ulp; the model itself is the truncated
quotient or the truncated quotient plus one, nothing else, with the
plus-one carrying no closed form (the predicate search's best simple
classifier reached 78.1 percent - host rounding already beats
anything cheaper than the recurrence). A truncation-corrected divide
was designed and measured at 61.8 percent before a line was emitted:
it would have been a twenty-point regression. Square root has the
same structure from the other side - the model is never above host
nearest, 73.8 percent exact as emitted, 76.2 under truncation, the
remainder being its own trajectory class. Multiplies sit at 99.94
percent in the double tier and adds are exact. The conclusion these
numbers force: the approximate tiers are at their practical ceiling,
every remaining bit of EE exactness costs the recurrence, and that
is precisely what the softfloat option is.

## Measured and rejected (this arc)

* A GPR-based scalar masking path: broke rendering through a mechanism that
  survived three targeted hunts unidentified; the emission paths now use no
  GPRs at all, and that empirical boundary is the rule.
* An AVX2 three-operand rebuild of the multiply fast path: proven
  byte-identical over 24 M executed quads, measured at parity-to-slower --
  the out-of-order engine absorbs the SSE copies; 22 % fewer bytes bought
  nothing.
* Fusing the MADD/MSUB exponent hand-off between multiply and add stages:
  priced out on paper -- reproducing the pack's overflow/underflow selects in
  the exponent domain costs more than the extraction it saves.
* A vf0-multiplier specialisation aimed by a static instruction census
  was reverted as dynamically cold, then re-landed when a dynamic
  instruction census (one emitted counter per block, execution counts
  times compile-time mix) showed the class at roughly a tenth of
  executed multiplies - the static text and the executed histogram
  disagree on this title by an order of magnitude in both directions.
  Which led to:

### Microcode auditing

Static instruction counts mislead: Tekken Tag's VU1 program text is 61-83 %
add/sub, but its measured option costs prove execution is multiply-dominated
-- the per-vertex MSUB transform chains dwarf the per-object move-heavy code.
The tree carries tooling (developed against a RetroArch save state: RZIP
decompression, freeze-layout walking, a VU1 disassembler built from the
interpreter's own opcode tables) to extract and census a game's actual
microcode before optimising for it.

## Building

CMake options common to all builds:

```
-DLIBRETRO=ON -DDISABLE_ADVANCE_SIMD=ON -DUSE_VULKAN=ON -DUSE_OPENGL=ON
```

The build produces `pcsx2_libretro.so`; install it into RetroArch's `cores/`
directory. A PS2 BIOS image is required in `<system>/pcsx2/bios/`.

Notes for arm64:
- Build against the same libc your frontend uses (a musl-built core will not
  load into a glibc RetroArch flatpak; build inside the matching SDK).
- 4K host pages are assumed on non-Apple aarch64 (16K remains for Apple
  Silicon); the vtlb page-protection code depends on this matching the kernel.

## Debug/diagnostic environment toggles (arm64, all off by default)

Bisection switches:

| Variable | Effect |
|---|---|
| `LRPS2_NO_EEREC=1` | EE runs on the pure interpreter |
| `LRPS2_NO_IOPREC=1` | IOP runs on the pure interpreter |
| `LRPS2_NO_EE_MEM/LOAD/STORE/BRANCH/MMI/MULDIV/COP1/LD64=1` | Route the given EE op class to the interpreter |
| `LRPS2_NO_EE_MMIHL=1` | Route PPAC5 + PMFHI/PMFLO/PMTHI/PMTLO (C.79) to the interpreter |
| `LRPS2_NO_EE_FPU_ARITH=1` | Route COP1 S-format arithmetic to the interpreter (data moves stay native) |
| `LRPS2_NO_EE_DIVS/SQRTS/RSQRTS=1` | Route just DIV.S / SQRT.S / RSQRT.S to the interpreter |
| `LRPS2_EE_SPLIT_MEM=1` | End the block after every native mem op |
| `LRPS2_NO_INLINE_MEM=1` | Disable the inline vtlb fast path (wrapper calls only) |
| `LRPS2_NO_EE_MANUAL=1` | Re-protect a code page on every revive instead of letting it go manual (C.60) |
| `LRPS2_MTVU=1` | Opt in to the MTVU (VU1 worker thread) |
| `LRPS2_VU1_INSTANT=1` | Restore instant VU1 (off on arm64; see VU0/VU1 row) |
| `LRPS2_MTVU_LOG=1` | MTVU worker/GS packet + per-path GS byte counters |

Tracing/logging:

| Variable | Effect |
|---|---|
| `LRPS2_RAMCRC=1` | Per-frame FNV hash of EE RAM + scratchpad (+ cycle) |
| `LRPS2_DUMP=<path>` + `LRPS2_DUMP_FRAME=<n>` | Full RAM dump at frame n |
| `LRPS2_TRACE=<path>` + `LRPS2_TRACE_LO/HI=<hex>` (+`_STEP`, `_FRAME`) | Binary (pc, GPR-hash) execution trace |
| `LRPS2_EXCLOG=<path>` | COP0 state at every 0x180/0x200 vector entry (+ syscall number/args) |
| `LRPS2_EVTLOG=<path>` + `LRPS2_EVT_LO/HI=<frame>` | Log every EE event test |
| `LRPS2_WLOG=1` + `LRPS2_WLO/WHI=<hex>` + `LRPS2_WFRAME=<n>` | Watch EE memory accesses in a physical range |
| `LRPS2_IPU_LOG_FRAME=<n>` | Log IPU FDEC results + IPU1 DMA feed from frame n |
| `LRPS2_FAULT_LOG=1` | Log vtlb page-fault handler activity; on an unhandled fault, locate the faulting pc's JIT block (guest pc, emitted-code hexdump, guest MIPS source) |
| `LRPS2_HANDOFF_STATS=1` | Histogram of ops handed to the interpreter + first-op breaker table |
| `LRPS2_JIT_STATS=1` | JIT compile statistics (e.g. likely-branch counts) |
| `LRPS2_SMC_STATS=1` | Per-page write-protect/clear counts (which pages ping-pong between code and data) |
| `LRPS2_EVT_STATS=1` | EE event-test rate: entries, how many run the IOP or the interrupt scan, and which clamp set `nextEventCycle` |
| `LRPS2_IOP_HANDOFF_STATS=1` (+`LRPS2_IOP_HANDOFF_PC=<key>`) | IOP-side interpreted-op histogram + first-op breaker table (+ pinpoint the pcs behind one breaker key) |
| `LRPS2_PROF=1` (+`_HZ`, `_MAX`) | In-tree sampling profiler: per-thread CPU-time SIGPROF sampling, bucketed by JIT code cache, symbolized against `.symtab`, with per-guest-block hot lists |
| `LRPS2_PROF_LR=sub1,sub2` | Caller attribution for leaf symbols (memcpy/memcmp/`__aarch64_*`): samples matching a substring get their LR symbolized into a per-thread caller histogram |
| `LRPS2_SYNC_STATS=1` | Wall-clock blocking at the EE↔GS/MTVU sync points (who actually waits on whom) |
| `LRPS2_SPU_STATS=1` / `LRPS2_SPU_MUTE=1` | SPU voice-shape statistics / mute the mixer to re-measure its wall-time ceiling |
| `LRPS2_SPU_SYNC_STATS=1` | SPU worker-thread feasibility: register read/write/DMA rates, armed-IRQ tick fraction, and which site (mixer/reverb/ADMA-input/DMA/register) each IRQA match came from |
| `LRPS2_MTVU_STATS=1` | C.80 lazy-kick effect: VIF unpack packets deferred vs MTVU notifies issued |
| `LRPS2_DUMP_HOST=<pc>` / `LRPS2_DUMP_HOST_IOP=<pc>` / `LRPS2_DUMP_HOST_MVU=<pc>` | Write a compiled block's host code (+ guest words) for offline `objdump -D -b binary -m aarch64` |
| `MVU_DIFF=1` | microVU1-vs-interpreter register-exact shadow differential (needs instant VU1 — even an empty `LRPS2_NO_VU1_INSTANT=` disables it and poisons the log) |

## Measured and rejected

Kept here because each looks obviously worth doing until you measure it. The
common thread: on this core (A76/A55) scalar integer ALU work is close to free,
so anything that trades it for wider-but-longer-latency code, or for fewer
instructions at the cost of a memory access, tends to lose.

| Idea | Why it was dropped |
|---|---|
| NEON `IDCT_Block` (IPU) | Written and **bit-exact on 602k blocks** — but slower at every block density: 169 vs 73 ns/block for the full vector version, and even a hybrid keeping the scalar row pass (whose DC shortcut skips most rows of a real MPEG block) with a NEON column pass that needs no transpose is 78 vs 74. The scalar code is not naive; it is well matched to the data. `yuv2rgb` already has a NEON path, so IPU is closed |
| SPU mixer micro-optimisation | The mixer is the biggest native C++ item on the EE thread in-game (`Mix` 7.6 %, reverb 1.2 %), but every cheap lever measures empty: the ceiling is ~6 % wall (muting the whole mixer: 17 -> 16 s medians); 47.1 of 48 voices are genuinely active in GT3 (engine sounds on every car) and none are silent, so the stopped-voice and silence shortcuts have nothing to skip; noise/modulation/volume-slides are 0.0 per sample; `TimeUpdate` mixes 1.01 samples per call, so a voice-outer reorder has no batch; and all 46 sustain voices have a nonzero envelope step, so an exact static-sustain ADSR skip has zero candidates. What remains is the price of 47 live voices at 48 kHz in already-tight scalar code (3-cache-line voice layout, prefetch, cached ADSR phases). The only lever left is an SPU worker thread -- a synchronisation project (games read ENDX/IRQA against mixing progress), not an optimisation. `LRPS2_SPU_STATS=1` dumps the voice-shape numbers, `LRPS2_SPU_MUTE=1` re-measures the ceiling |
| SPU mixer IRQ-mode specialisation | The IRQ machinery is 93 of `Mix`'s 169 global accesses: the armed flag read 13 times, and four nested `IRQEnable`/`IRQA` checks against both cores, once per voice per sample. Compiling one copy of the sample path per arm state (none / core 0 / core 1 / both -- the state is fixed for a `TimeUpdate` batch) folds all of it: `Mix` 524 -> 419 memory ops and 68 -> 50 stack accesses in the single-core copies, `V_Core_Mix` 144 -> 60, PCM and IRQ trace bit-identical. And the clock does not move: 240-253 ns per `Mix` on both trees, every mode within 2 % and inside run-to-run noise, for +15 KB of `.text`. The checks it removes are L1-hit loads and predicted branches that were never on the dependency chain, so the out-of-order core was already hiding them; the mixer is bound by its interpolation and envelope arithmetic, not by load count. The same lesson from the other side: outlining `MixVoice` cut the unit's memory ops 28 % and ran 15 % slower, because 48 calls per sample are on the chain. Memory-op count predicts time here only when the ops removed were feeding something. The IRQ-trace lane in `tests/spu2` is what survives, since the four check sites had no coverage before it |
| Coarsening the EE↔IOP slice | The event test runs 102M times on a 3000-frame MMX7 run and is 5–7 % of the EE thread, but it is not slow — it is asked for that often. The 48-cycle re-test at the end of the test wins the clamp 663 times out of 76M; sweeping it 48→384 changed neither the call count, nor the output (bit-identical), nor the time. `CPU_INT`/`cpuTestINTCInts` keep pulling `nextEventCycle` back to 4–8 cycles, so the rate is the density of the DMA/INT schedule. Thinning that moves interrupt delivery, for a ceiling measured at 2–4 % (inside the noise) |

## Project Details

Upstream PCSX2: https://pcsx2.net/ — this core inherits its GPL/LGPL licensing;
see the source headers. The arm64 recompilers emit through `pcsx2/arm64/a64emit.c`,
an AArch64 code emitter in C89.
