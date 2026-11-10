# What the tests under tests/ cover

Run everything with `sh tests/run-all.sh`. Six of the suites score the
emulator against console captures and want a checkout to compare against:

    PS2AUTOTESTS=/path/to/ps2autotests   ee, fpu, iop, mmi, vif, vu
    PS1TESTS=/path/to/ps1-tests          iop, for the GTE and MDEC oracles

        git clone https://github.com/unknownbrackets/ps2autotests
        git clone https://github.com/JaCzekanski/ps1-tests

Without them those suites skip rather than fail, so the script is still
worth running with neither set.

`SANITIZER=undefined sh tests/run-all.sh` builds under UBSan, and
everything that links emulator code is clean under it. Two suites ignore
the variable on purpose: `faultstress` and `spsc` are concurrency tests
and build with `-fsanitize=thread` unconditionally, since that is what
they are for. `fpaudit` honours it but drops it for `jit_stage`, which
emits and runs machine code of its own that ASan's shadow mapping does
not survive.

Do a clean build between sanitizers with different define sets rather
than reusing objects.

## The two kinds of harness

Most ops are scored twice, and the pair is deliberate.

A **transcribing** harness copies the semantics out of the emulator and
scores the copy against the capture. It is cheap, has no link
dependencies, and catches a wrong answer.

A **real-code** harness links the emulator's own translation unit and
calls the ops through their real entry points. It catches what a
transcription cannot: drift between the copy and the code, undefined
behaviour under a sanitizer, and mistakes in the parts a transcription
quietly reimplements correctly.

Keeping both is what found the DIV_S bug -- `hwfpu.c` scored 36 of 36 on
a block where `hwrealfpu.cpp` scored 27, and only one of them could be
right. Where the two disagree, that disagreement is the finding.

## Coverage

| Suite | Harness | Cases | Through |
|---|---|---|---|
| mmi | hwreal.cpp | 2690 across 101 ops | MMI.c |
| mmi | hwelem.c, hwpmfhl.c, hwsa.c | 1933 | transcription |
| mmi | hwscore.c | 428, not printed as a count | transcription |
| ee | hwrealee.cpp | 944 across 56 ops | R5900OpcodeImpl.c |
| ee | hwalu, hwload, hwstore, hwmuldiv, hwbranch | 1186 | transcription |
| ee | hwspr.cpp | 448 | SPR.cpp |
| ee | hwperf.c | 85 | transcription |
| ee | hwcpcond.cpp | 6 | COP0.c |
| ee | hwgslabel.cpp | 4 | Gif_Unit.c |
| fpu | hwrealfpu.cpp | 939 across 25 ops | FPU.c |
| fpu | hwfpu.c, hwfcr.c, hwbranch.c | 595 | ps2float.c + transcription |
| iop | hwrealiop.cpp | 563 across 33 ops | R3000AOpcodeTables.c |
| iop | hwalu, hwlsu, hwmuldiv, hwbranch | 722 | transcription |
| iop | hwgtefuzz.cpp | 1100 | IopGte.cpp |
| vu | hwrealvu.cpp | 479 across 28 ops | VUops.cpp, VU0.c |
| vu | hwclip, hwrandom, hwint, hwefu, hwfdiv, hwctc2 | 295 | transcription |
| vif | hwunpack.cpp, hwcycle.cpp | 42 formats + 16 write modes + 4 layout + 342 write cycles | Vif_Unpack.c |
| emitter | hwas.cpp | 9213 | c89ops.h vs GNU as |

## What is deliberately not scored

Each of these was looked at and left, for a reason worth knowing before
looking again.

**Needs the emulator running, not a function of inputs.** The `kernel/`
tree (24 captures: threads, semaphores, event flags), `dma/dmac/tagintr`,
`dma/vif/basic` and `mskpath3`, `memory/ee/default`, and the IOP libc
captures, which run real IRX code rather than anything this tree
implements. `dma/dmac/tagintr` got as far as a written scorer: driving the
tag chain needs the real `hwDmacSrcChain`, and linking `Hw.cpp` for it
pulls in sixty more symbols for four cases.

**Needs instruction sequencing.** `ee/branchdelay`, `ee/lsudelay`,
`iop/branchdelay`, `iop/hilodelay`, `iop/lsudelay`,
`vu/lower/fdivdelay`. These report what an instruction *inside* a delay
slot observes. The EE branchdelay properties were verified by reading
instead, which is how the IOP JALR bug was found.

**Would be circular.** `dma/spr/normal` -- its two claims are SADR
masking to 0x3FF0 and MADR dropping bit 31, both in `Dmac.cpp`'s write
path. Reaching that path needs the hardware register file; restating the
masks in a test would compare two constants transcribed by hand, and
pass whatever the emulator did.

**No oracle in the capture.** `cpu/cop` and `gpu/gp0-e1` in ps1-tests
print pass/fail summaries rather than values, so there is nothing to
score against.

**Implemented but deferred.** The PS1 MDEC status register: the oracle
exists in `tests/iop/hwmdec.cpp` and scores 0 of 1289, wired as a report
rather than a gate. Making it pass means restructuring `rl2blk` to expose
block-level state, which is a real change rather than a test.

**Not reachable by any harness here.** The arm64 EE recompiler decodes
with nested switches rather than tables -- about 190 case labels -- so
the dispatch audit in `tests/ee/tabaudit.c` and `tests/iop/tabaudit.c`
cannot reach it. A misplaced case there is the same class of bug as a
swapped table entry and nothing would catch it.

## Known gaps inside passing suites

A clean score measures what the capture asks, not what the instruction
does. These are the places where those differ, found by mutating the
emulator and seeing what still passed:

- **CLIP** (vu): the fourteen cases pin the comparison but not the
  six-bit shift that accumulates the flag across calls, and no operand
  has a denormal w. Two thirds of the op is unexercised.
- **VIF column masking**: the `stmod` scenarios use a mask that selects
  row and data only, so the `MaskCol` arm never runs.
- **`SLTIU` and the immediate split** (ee, iop): no line in either
  capture uses a negative or high-bit immediate, so sign- and
  zero-extension are indistinguishable. Both suites now carry cases
  derived from the MIPS definition instead, marked as such.
- **`TQWC == 0`** (spr): no interleave case leaves it zero on its own.
- **VU EFU accuracy** (vu): scored as floors, not targets, and the floors
  depend on `vu0ExtraOverflow`, which defaults off. `hwefu.c` hardcodes
  the clamp and so models the other configuration; that is why its EATAN
  score differs from `hwrealvu`'s.

## The traps this tree has hit

Recorded because each cost real time and each looks like an emulator bug
first.

Silent under-coverage is the recurring one: a block whose name or operand
shape stops matching the capture contributes nothing and reports success.
Three blocks were absent that way -- `mult` and `multu` in the EE
harness, `pcpyh` and `prevh` in the MMI one -- and none was found by a
failing test. The harnesses now compare what they parsed against the
number of lines each block has, and a mismatch is a failure.

The others, in short: named constants missing from a table drop lines
without failing; the variable shifts print value-first, not source-first,
which has caught four separate harnesses; and several captures seed a
register differently from their neighbours, so a value that looks like a
result may be a source or a leftover.
