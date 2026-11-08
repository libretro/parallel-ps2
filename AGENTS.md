# AGENTS.md

Instructions for anyone, human or agent, changing this core. Read the
section that covers the code you touch before touching it.

## Ground rules

- **C89 for new code.** New code is C89 that MSVC compiles: `/* */`
  comments, declarations at the top of a block, no C99 or C11 in a C
  unit. C++ that new code plugs into is rewritten as C89 with it, and
  the C++ in the core is going away. A header a C unit reaches compiles
  as C89 and as C++ (`tests/cheaders`); C and C++ units agree on each
  other's symbols (`tests/clinkage`).
- **No locks.** Threads hand work over through atomics, SPSC rings and
  eventcount park/wake. A mutex added to fix a race is the wrong fix:
  give the state one owner (see Ownership).
- **Stack frames.** At most 4 KiB in the core's own code and 2 KiB in
  libretro-common (`tests/stackframe`).
- **libretro-common is a copy.** Files under
  `libretro/libretro-common/` stay plain syncs of RetroArch's
  `libretro-common/`. Adaptations go in the core's own files.
- **Every fix ships with its test.** A suite is a directory under
  `tests/` with a `build.sh`; `tests/run-all.sh` finds it, CI runs it.
  The test fails on the broken code and passes on the fix. A flaky test
  is fixed, not rerun; a slow one is made faster.
- **Build it everywhere it ships.** Touched units compile with MinGW and
  in an Apple shape as well as on Linux. A commit that adds or removes a
  source builds the Makefile and CMake to completion, and updates the
  vcxproj.
- **Optional features are neutral when off.** A setting off costs
  nothing over the code without it.

## Speed of light

Measure against what the work needs and what the hardware can do, not
against the previous build. Every byte copied, allocation made, layer
crossed and wait taken is a cost with a reason or a defect.

- **The cost model** is bytes moved, cache lines touched, instructions,
  allocations, driver calls and waits, together with the guest progress
  they bought. A copy removed on one thread and added on another, or on
  the GPU, is not removed.
- **Ownership, representation and granularity before instructions.**
  Fewer shared writers, fewer temporary forms of the same data and fewer
  redundant driver interactions first; instruction tuning last.
- **Delete by reachability.** Code nothing reaches costs
  initialisation, binary size and maintenance. Trace the configurations,
  callbacks and failure paths that could reach it, then delete it.
- **Inspect what the compiler made** where it can undo an optimisation
  (a `malloc` plus `memset` becoming `calloc`, a hidden TLS call, a
  copy loop turned back into `memcpy`).
- **Acceptance.** Exact output and preserved lifetimes first. Then the
  measured benefit. A smaller copy count, a C rewrite or a
  microbenchmark win alone does not show smoother play.
- **Stop conditions.** Keep the copy if the consumer outlives the
  borrow. Keep the barrier if its proof is missing. Keep the general
  shader if the equivalence is incomplete. Keep the fallback when the
  new method loses.

## Zero-copy and borrowed lends

Every change looks for copies to remove in the code it touches, and
every delivery says what it found and removed. A copy that stays has a
reason a reader can see.

- **One owner, the rest borrow.** Data is owned once. Everything else
  takes a pointer and a length for a bounded time. The borrow ends
  before the owner frees or reuses the memory, and a borrowed slice
  outlives every consumer, including the driver call it is passed to
  and the GPU submission that reads it.
- **The borrow contract.** Acquire a valid view and its reference;
  settle its producer; consume it within its lifetime; release it after
  the last synchronous read or the fence of the last asynchronous one.
  A consumer that stores the pointer, or a guest that may write the
  memory meanwhile, needs a snapshot or a proven copy-on-write. A
  pointer alone is not a zero-copy design: a lease, a pinned slot, a
  generation-tagged handle or a submission index makes the lifetime
  explicit.
- **Producers write where the data ends up.** A decoder writes into the
  memory the backend lends for the upload (`GSTexture::Map`), a stream
  into its stream buffer (`VKStreamBuffer`, `GLStreamBuffer`), a CDVD
  reader straight from the mapped disc image. No intermediate buffer,
  no container returned by value to be copied somewhere else, no
  pop-to-scratch stage.
- **No slurps.** Files are mapped, or read in place through
  libretro-common's filestream/VFS (`vfs_hybrid`). A file read whole
  into a fresh allocation, to be parsed or compared, is a bug when it
  could be mapped or streamed. A parser borrows the mapped bytes for as
  long as its tree lives.
- **Copies that must stay say why.** Some copies are what keeps the
  emulation correct, and stay, with a comment naming the reason:
  - guest memory the GPU reads later is snapshotted when the GS thread
    takes it, as the EE and IOP go on writing it (a texture, a CLUT, a
    vertex buffer, a packet). A bounded snapshot reused while unchanged
    is often faster than fencing every guest write: make it small, do
    not make it go away;
  - data a driver or an API hands over only for the length of a call;
  - a buffer that keeps a failed or cancelled read from changing its
    destination.
- **Say exactly what "zero-copy" removed.** No CPU pixel copy, no
  CPU/GPU round trip, a borrowed GPU image and shared memory are
  different claims. Name the duplicate that went and the work that
  stays (the GPU may still copy, the driver may still stage).
- **Write the final representation once.** Byte swapping, format
  conversion, decompression and guest-visible snapshots are work the
  emulation needs: fuse them into the write to the final destination
  rather than adding a stage.
- **Reuse storage with a defensible bound.** Capacity comes from the
  most that can be live at once, the guest's address space or bounded
  in-flight work, never from a number chosen to hide an allocation.
  Reset counts rather than clear payloads, do not clear bytes about to
  be overwritten, and define what happens when the pool is exhausted.
  A pool that grows on exhaustion still allocates.
- **Three ledgers for a resource.** Content (which producer wrote the
  newest version, covering which bytes), lifetime (which owner, command
  buffer, submission or frontend ticket can still reach it) and cost
  (bytes, passes, driver calls, allocations, waits). Draw the graph of
  an expensive transition, remove the edges whose consumer can use an
  existing version, and only then schedule what remains better.
- **Validate first, mutate second.** Reject bad offsets, counts and
  overflowing ranges before touching a destination. An empty span is
  zero bytes, never one, and never a pointer formed past the end.
- **Measure it.** Bytes copied and allocations made are work results:
  count them where they happen, and say what changed. They are not
  frame-time results until timing shows it.

## Ownership and threads

- **Change ownership, not the lock.** One thread owns a structure's
  mutation; others send it requests and read immutable publications.
  SPSC where there is one producer and one consumer, MPSC where there
  really are several.
- **Publication is a lifetime contract.** Fill every field, then
  release-publish; acquire before reading. A job published to a worker
  is self-contained: never a pointer to a stack temporary, never moved
  after its interior pointers are set. Completion is the last access to
  a request: the caller may reuse its storage at once.
- **Parking has two checks.** Check the predicate, prepare the
  eventcount wait, check again, then commit or cancel. A one-check sleep
  loses wakeups. Parking an idle worker is fine; busy-spinning one is
  not.
- **Name progress precisely.** An atomic fast path is not lock-freedom.
  A CAS loop waiting for an owner-held bit is a lock. Finite polling
  before parking is a latency policy.
- **Shutdown and failure paths are part of the design.** Stop the
  producers, cancel queued work, finish active work, drain the results,
  release the references, and only then destroy device objects. Join
  workers even when the feature they serve was off.
- **The frontend owns pacing.** One frame per `retro_run`
  (`tests/framepace`), the input of a run in its picture
  (`tests/latency`). No wall clock decides emulated progress, and a
  faster `retro_run` that advances less of the machine is not faster.

## Shader compilation and stutter

The target is the console's own behaviour: a game never waits for a
shader. Starting a game for the first time, with no shader cache and no
pipeline list, plays the same as a run with full ones. Nobody should be
able to see a shader stutter or a pause. Shader compilation gets better
with every commit, and never worse.

PSO here means a complete pipeline (`VkPipeline`, a D3D12 PSO, a linked
GL program). SPIR-V words, a shader module, a driver cache blob and a
ready pipeline are different things: a warmed shader alone does not make
a first draw free of a hitch.

Where the renderers stand: Vulkan builds the utility pipelines and the
listed TFX pipelines on a warm-up thread, and draws a key it has not
built with a dynamic sibling while its own pipeline is built ahead;
D3D12 warms from the pipeline list on a thread (`GSD3D12Warm`);
paraLLEl-GS builds its pipelines ahead on its own threads. A key no
sibling covers is still built on the draw path. That is the gap the
invariants below close; nothing may widen it.

### Invariants

1. **Nothing new compiles on the draw path.** After boot, no shader,
   pipeline or program is created on the GS thread, the EE thread or
   inside `retro_run`, and nothing there waits for a compile. A code
   path that adds one is a bug, even when it only runs the first time a
   state is seen. Moving an existing one off the draw path is the
   direction of every change here.
2. **Every draw has a correct pipeline ready.** A draw with no
   specialised pipeline yet uses one that renders the same result: the
   dynamic sibling, with what was baked supplied through push constants,
   uniforms or dynamic state. Never a skipped draw, a placeholder or a
   guessed approximation. Before a key loses a field to dynamic state,
   show that the dynamic version renders the same.
3. **A miss is a state machine.** Unseen, queued, words ready, pipeline
   queued, pipeline landed. The draw takes the best ready step: the
   specialised pipeline, else a ready sibling while the specialisation is
   queued, and only with neither a synchronous build. Count each step.
4. **Workers never hold up a draw.** Compile queues are bounded, and
   admission counts results not yet drained as well as work not yet
   done. A full queue defers speculative work and the draw goes ahead
   with its ready pipeline. Finished work is adopted on the thread that
   owns the cache; a worker result that lost the race to a draw-path
   build is destroyed, not swapped in. Raising queue sizes or worker
   counts because utilisation looks low is not a fix.
5. **The cache is never what removes a stutter.** Invariants 1 and 2
   hold with "Disable Shader Cache" on and with no pipeline list. With
   the cache on:
   - nothing reads or writes the disk on the GS or EE thread, or in
     `retro_run`;
   - recorded keys, shader binaries and driver pipeline data collect in
     memory and are written by a worker at shutdown or on pause;
   - cache files live in the frontend's system directory;
   - a rejected or mismatched driver blob is a recoverable miss: start
     from an empty cache.
6. **Cache the right representation.** The pipeline list holds keys,
   independent of the driver, and survives a driver update; a driver
   pipeline cache or a GL program binary is tied to its build, vendor
   and version and is never reused across devices on the strength of a
   portable key. A list written at the end of a short session merges
   with the old one rather than replacing it.
7. **Zero-copy in the shader path.** Each shader's words are owned once
   and handed over without a copy.
   - The Vulkan shaders are SPIR-V built into the core
     (`VKShaders.inc`); a draw's interface is settled in those words
     (`VKSpirvPatch`), and no compiler runs at run time. Keep it so.
   - Cache files are mapped, or read in place through filestream/VFS;
     validated slices go straight to the driver (`vkCreatePipelineCache`
     initial data, program binaries) and are unmapped after the call
     that consumed them.
   - No slurps, no byte-by-byte copies, no wrapper allocated only to be
     discarded. Shader text and IR are built in fixed or reused storage,
     with no formatted temporaries and no heap allocation during play.
8. **Stable identity.** Keys are compared byte for byte:
   - zero the padding before hashing;
   - zero the fields a shader does not read (`VKPipelineKey.h`), and
     audit every field through key construction, pipeline creation and
     dynamic-state emission before removing one: omitting state a
     pipeline consumes is a rendering bug;
   - include the backend, the target (SPIR-V version, shader model, GLSL
     dialect), the render pass and the host configuration;
   - never key on a pointer, an address or a module handle.
   Changing what a key means, even when its size stays the same, bumps
   the list and cache version in the same commit. Version by meaning,
   not by byte count. A corrupt or mismatched file is a recoverable
   miss.

### Draw-side overhead

- **Compare in the destination.** Uniform and constant blocks compare
  and update in their packed form; no second copy of the values exists
  to compare against.
- **Reuse immutable snapshots.** An unchanged constant block reuses its
  staged copy within the same staging generation; a reset, a load, a
  rotation or a reallocation invalidates it. Never overwrite a snapshot
  an in-flight draw can read.
- **Descriptors.** Reuse a set when its contents are unchanged and move
  dynamic offsets; write a fresh set when anything changed. A set an
  in-flight command buffer recorded is never patched in place.
- **Clear and draw are separate states.** A clear never reuses a draw
  pipeline or inherits its dynamic state, nor the other way round.

### Regressions to guard

Eviction or worker landing never leaves a draw bound to a recycled
entry. A device or renderer change quiesces the workers before the
words, modules, libraries, pipeline cache and device are released. A
savestate load or reset invalidates staging generations and binding
state. Geometry-shader and every pipeline-library variant are tested,
not only vertex and fragment.

### The gate: every commit, measured

Any commit that touches `pcsx2/GS/Renderers`, the shaders, the pipeline
keys, the warm-up threads, or the cache and file I/O behind them runs,
before delivery, `tests/vkpipekey`, `tests/vkspirv`, `tests/vkpipewarm`,
`tests/d3d12warm`, `tests/pgspipelines` and `tests/swdraw` as well as
`tests/run-all.sh`, and reports the renderer's pipeline counters (built
on the render thread, built ahead, handed over, drawn with a sibling)
for:

1. cold: no cache and no pipeline list;
2. the pipeline list only;
3. everything warm;
4. a savestate loaded into a fresh process.

Report the results in the delivery summary, not in the commit message.

- **Hard gates.** These don't depend on the machine, so they must not
  get worse: pipelines built on the render thread after boot; draws
  waiting for a compile; distinct pipelines and shaders built for the
  same cases; heap allocations during play attributed to the renderer;
  pictures identical to the reference, with no validation errors.
- **Timed gates.** Compared with the parent commit on the same machine:
  boot to first frame, warm-up time, worker pipeline creation time, and
  `retro_run` p50/p99/max with the count of runs over the frame budget.
  A regression beyond the run-to-run spread blocks the commit.
- **Cold must match warm.** The cold run's p99 and max `retro_run` stay
  within the warm run's spread. A gap is a stutter to fix.
- **Synthetic cases only.** Test cases are generated or synthetic
  (`tests/swdraw`'s packets, synthetic BIOS images). No game names and
  no disc images, in files, comments or messages.

### Measuring honestly

- **Clean runs.** Time on an idle machine, with nothing else building,
  and alternate parent and candidate (ABBA). Timing runs take no
  screenshots, picture hashes, readbacks or GPU query polling; picture
  checks are separate runs. Report medians, p99 and max with frame
  numbers, never a single run, and never a best of several.
- **Record the controls.** Revisions, driver, GPU, OS, renderer, the
  extensions actually enabled (dynamic state, pipeline libraries),
  compiler flags, affinity, warm-up and timer resolution go with every
  result. Keep caches and lists of private copies; never empty the
  frontend's system directory to make a cold run.
- **Explain hitches with counters.** Don't diagnose a hitch by how it
  looks. A long frame is a compile only when the compile counters put
  one at that frame; readbacks, guest CPU work and I/O look the same.
  Time driver calls with CPU time, completed GPU work with retired
  timestamps.
- **Counters cost nothing in release builds.** They exist only in the
  profiler build (`make PROFILER=1`). Each is one atomic add where the
  event happens. No logging on paced paths, no wall clocks, no extra
  work. Cycle counts use `cpu_features_get_perf_counter()`.
- **Equal fixtures.** Instrument both sides the same way and read the
  generated code before believing a small difference. Use a monotonic
  high-resolution clock.
- **Count useful work.** Count new pictures and emulated progress too.
  A run that repeats frames or skips guest work only looks faster.
- **Keep work and time apart.** Fewer compiles, fewer bytes copied and
  fewer allocations are work results. They are not frame-time results
  until timing shows it, and a mixed timing result stays mixed.
