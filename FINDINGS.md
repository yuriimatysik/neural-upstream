# What we learned taking DLSS Neural Rendering apart

Notes from getting NR to run upstream of the upscaler, building our own runtime
for Ada, and measuring where the time actually goes. Written down because the
dead ends cost more than the answers did.

Hardware: RTX 4070 Ti (Ada, `sm_89`). Titles: GTA V Enhanced, Bright Memory:
Infinite benchmark.

---

## 1. The add-on

### Hook the evaluate wherever it lives, not where you expect it

`NVSDK_NGX_D3D12_EvaluateFeature` is exported by the driver's `_nvngx.dll`, but
also by any NGX proxy (OptiScaler and friends) and by other modules that wrap it.
In a game without native DLSS the proxy answers the calls while the driver module
stays loaded and idle — so there is no way to tell the live provider apart by
inspection.

Hook every module that exports it. Guard against re-entry, because a proxy
forwards to the driver and one game call then enters the hooks twice.

**What not to do:** hook one candidate and migrate on a frame counter if nothing
arrives. That cannot distinguish *wrong module* from *the player is still in the
menu*, and it abandons a correct hook. This broke a working setup.

### The cadence must be anchored to something the game owns

Counting evaluates does not work. Deciding "every Nth frame" from a call counter
put the work on the wrong frame after any alt-tab, because losing or gaining one
evaluate flips the parity — and the counter was incremented from ~7 threads
without atomics.

Anchor on the DLSS jitter: it comes from the game, is identical within a frame,
and changes on every new one. Claim the frame with one `InterlockedExchange` so
exactly one thread is the frame's first evaluate.

### Skipped frames: reuse the effect, not the image

Feeding the previous frame's result while the motion vectors are current is what
ghosting is made of. Store what the network *changed* (`neural - proxy`), follow
it along the motion vectors to where each pixel was, and apply it to the current
frame. Sample the delta bilinearly — point sampling rounds subpixel motion to
whole texels and re-introduces the very misalignment the reprojection exists to
fix.

Reject the delta where depth says the surface underneath changed. Stash the
capture-time depth in the delta texture's spare alpha channel: no extra texture,
no extra pass.

---

## 2. Rebuilding the NR runtime for Ada

The shipped `nvngx_dlssnr.dll` targets Blackwell (`sm_120`). It can be rebuilt
for other architectures because **the fatbinary carries PTX**, not just cubins.

### Anatomy

| section | contents | changed by community builds |
| --- | --- | --- |
| `.text` | CPU code | a handful of bytes |
| `.data` | 15 fatbins, kernels + PTX | ~90% |
| `.rsrc` | 147 MB of weights | **never** |

Four independent builds all had byte-identical weights. Nobody quantises or
retrains anything; they recompile kernels.

### The pipeline

1. Find `0xBA55ED50` fatbin headers in `.data`. The entry header is 64 bytes:
   arch at offset 28, payload size at 8, compressed size at 16.
2. PTX payloads are **zstd**-compressed. 35 MB of readable PTX comes out.
3. Rewrite what Ada lacks (below), assemble with `ptxas -arch=sm_89`.
4. Rebuild each fatbin in place, padding to the original span.
5. Patch the arch dispatch: the original computes `lea ecx,[rax-0x140]` then
   switches on it; a single-arch build replaces those six bytes with a `jmp` past
   the switch.

### What Ada does not have, and what to do about it

| Hopper / Blackwell | replacement for `sm_89` |
| --- | --- |
| `cp.async.bulk` (TMA) | per-lane `cp.async.ca.shared.global` 16 B, or plain `ld.global.v4` + `st.shared.v4` |
| `mbarrier.expect_tx`, transaction counting | delete it |
| `mbarrier.try_wait` | **`bar.sync`** |
| `elect.sync` | let every lane participate, or test `%laneid == 0` |
| `red.global.v4.f16x2` | four scalar `red.global.f16x2` |
| `min.relu.s32` | `min.s32` then `max.s32 ..., 0` |
| `fence.release.gpu` | `fence.acq_rel.gpu` |

**The one that matters.** Transaction barriers do not translate. They count
*bytes moved by the TMA engine*, not thread arrivals, and Ada has no such
counter. Initialised with 0 they hang; with 1 they fail; `.noComplete` deadlocks;
removing the wait outright leaves a race. The answer is to delete the mbarrier
machinery entirely and put a block-wide `bar.sync` where the wait was —
`cp.async.wait_group` only guarantees the *issuing thread's* copies, and a
cooperative copy needs the cross-thread guarantee the mbarrier used to provide.

**The subtle one.** In some kernels the copy source is a *generic* address with
no `cvta.to.global`. TMA accepts generic addresses; `cp.async.*.global` does not,
and faults. Always convert.

### Traps

- `ptxas` 12.9 silently accepts `fence.release.gpu` and a trailing NUL byte;
  13.3 rejects both. Use the newest assembler available — it is a free
  correctness check. (PTX 9.4 needs newer than 13.3; 9.3 assembles fine.)
- Copy sizes are not uniform. One module mixed 512- and 1024-byte copies, and a
  transform hardcoded to 512 moved half the data on 72 of them.
- Community builds are not authoritative. Four existed, all different, none
  demonstrably NVIDIA's own.

---

## 3. Measuring

### Build tooling that answers questions offline

- **Load test** — `cuModuleLoadData` on each cubin catches invalid or
  ISA-incompatible output in a second, without launching a game.
- **Kernel diff** — compare kernel symbol sets between builds. Ours and the
  reference matched 74/74, which located a bug to *behaviour* rather than
  compilation.
- **Pattern sandbox** — a tiny PTX kernel running the exact translated sequence
  against known data. This is what proved `cp.async.mbarrier.arrive.noinc` faults
  and `bar.sync` works, in seconds instead of game restarts.
- **`nvdisasm`** — read the SASS of a build that works. One look at *zero*
  `ARRIVES` and 209 `BAR.SYNC` answered a design question that six failed
  attempts had not.

Pip has `nvidia-cuda-nvcc`, `nvidia-cuda-nvdisasm` and `nvidia-cuda-cuobjdump`;
no full toolkit needed. `compute-sanitizer` from pip does **not** work — it
cannot instrument.

Note that NGX does not go through `nvcuda.dll`; the driver runs these kernels on
an internal path. So `cuLaunchKernel` cannot be hooked to capture real launch
parameters, and CUDA-level profilers are unlikely to see the kernels at all.

### Validate the instrument before trusting it

Synthetic kernel arguments make results non-deterministic: the same reference
kernel returned `ILLEGAL_ADDRESS` four times and `OK` once across five runs.
Divergences from a single pass are noise. Repeat, and act only on stable signals.

### Use a control variable

GPU clocks ramp *during* a run. The colour encode does not depend on the NR
build, so its measured time reveals the GPU's state: samples at `encode=0.014`
are comparable to each other, samples at `encode=0.008` are effectively a
different machine. Comparing run averages without this produced a confident and
wrong conclusion.

---

## 4. Where the time actually goes

GPU timestamps around each stage. RTX 4070 Ti, 1280x720 → 2560x1440:

```
colour encode    0.014 ms
the network      3.27  ms      99% of the cost, about a third of a 100 fps frame
colour decode    0.017 ms
```

Static instruction mix of the network's SASS:

```
integer arithmetic   39.2%   (IMAD alone: 19,104)
data movement        11.1%
control / sync        7.7%
global memory         6.1%
float arithmetic      4.4%
tensor (HMMA)         4.2%
shared memory         3.4%
```

The kernels are dominated by **address arithmetic**, not by tensor maths and not
by memory traffic. That explains every failed optimisation below.

### Four levers, all measured, all null or negative

| change | result |
| --- | --- |
| `cp.async` `.ca` → `.cg` (bypass L1) | within noise |
| `.maxnreg` 168 → 128 (double occupancy) | ~10% slower, then a TDR |
| `.maxnreg` 168 → 200 (zero spill) | ~3% slower |
| `cp.async` → plain `ld` / `st` | within noise |

NVIDIA's `.maxnreg 168` is well chosen for Ada too: the 32 bytes of spill cost
less than the occupancy given up, which also implies the kernels use blocks
smaller than 256 threads.

The FP8-versus-FP16 premise did not survive contact with data either. A
multi-architecture build using FP16 approximation measured the same as native FP8
on Ada.

### Build comparison

One run each, Bright Memory: Infinite, stdev ≈ 20:

| build | avg | median | 1% low |
| --- | --- | --- | --- |
| ours, `.cg`, maxnreg 168 | 100.7 | 98.3 | 70.1 |
| ours, maxnreg 128 | 100.0 | 98.1 | 70.9 |
| multi-arch FP16 | 99.4 | 97.6 | 69.5 |
| community `sm_89` | 99.1 | 97.5 | 68.4 |
| ours, maxnreg 200 | 97.8 | 95.7 | 64.2 |

Everything except the last sits inside 1.6%, which is inside the noise. Ours
never measured worse; that is as strong a claim as one run each supports.

---

## 5. Method, in hindsight

- **Get the disassembler early.** One failed package search sent this work back
  to guessing for hours. The answer was in the binary the whole time.
- **A crash is one bit of information.** Bisecting by swapping modules turned
  "it crashes" into "module 5, the attention kernels" and then into a specific
  missing `cvta`.
- **Test both directions.** Declaring a parameter optimal after only lowering it
  was wrong twice over: the reasoning was unsupported, and the comparison used
  runs that were not comparable.
- **Pick a benchmark that costs a minute.** Swapping a AAA game for a 2.5 GB
  standalone benchmark changed which experiments were worth running at all.

---

## 6. Coexisting with frame generation

Everything above was measured with the add-on alone. Running it underneath DLSS
Multi Frame Generation at 3x and 4x broke it in ways none of the image code
explained, and the search cost a day. What follows is the short version.

### The artefact was frame pacing, and the cadence caused it

The network costs **4.8 ms of an 8.9 ms rendered frame** — 54% of the budget. At
cadence 2 that lands on every other frame, so the rendered interval alternates
**8.9 / 13.9 ms**. DLSS-G places its generated frames inside that interval and
cannot pace through a 56% swing, which is why the artefact grew with both the
multiplier and the cadence, and why Quality was the only mode that worked: same
total cost, but uniform.

### What it was not, each ruled out with evidence

| theory | how it died |
| --- | --- |
| our `sm_89` kernels | a third-party multi-arch build shows the same artefact |
| a failing evaluate | `erfail=0 grfail=0` — not one failure in a whole session |
| what the add-on draws | `EffectStrength=0` — same work, no visual effect, artefact unchanged |
| the shader's compositing | four separate image fixes moved nothing |
| Streamline's descriptor heap | the same wrap rate at 2x, which NVIDIA ships |

`will skip the present` in `sl.log` is **not** an artefact: it fires whenever FG
is switched off, which is teardown. Reading it as a failure cost hours.

### Two real bugs found on the way

- **The descriptor ring was eight slots.** Three dispatches a frame with the CPU
  two or three frames ahead of the GPU left ~2.7 frames of headroom, so slots were
  being recycled while still in flight. It had been that way from the first day and
  is the most likely cause of artefacts blamed on other things. Now 64.
- **43% of evaluates were handed the raw colour.** The game issues more than one
  evaluate per frame and only the first claims it; the rest took the skipped-frame
  path and threw the enhancement away, even though `final_tex` already held that
  very frame. Whether the effect was seen depended on which evaluate reached the
  screen.

### The instrument kept becoming the experiment

Twice. `ring_dump` wrote 1024 lines through a logger that locks and flushes per
message, on the present thread — pressing the key *produced* the artefact it was
meant to catch. Later `prof_report` was wired to Map and Unmap a readback buffer
every present. A stall on the present thread is visible on screen; anything
measuring from there has to cost nothing. All of it now hangs off `Diagnostics`,
off by default.

And the profiler had never worked at all: `prof_report()` was defined and never
called, *and* `g_prof.freq` was declared and never assigned, so it returned on its
first line regardless. Every cost figure quoted before this point came from
nowhere.

### Three more levers, all null or negative

| change | result |
| --- | --- |
| `DLSSNR.ScalingRatio` below 1.0 | inert, like `Hint.Render.Preset`; cost unchanged |
| reprojection scaled by the delta's age | shifting under a turning camera — the multiple assumes constant velocity |
| advecting the delta one frame per frame | worse still, though it extrapolates nothing |

The second and third attack the same thing from opposite directions and both
made it worse, which says the residual flicker at cadence > 1 is not about *where*
the delta is placed. It is the cost of reusing an effect across frames at all.
That also retires the async-queue argument for fixing it: that design reprojects
identically.

### What did work

- **Build every frame the same way** (`UniformDelta`). Above cadence 1 the image
  used to alternate between the network's output developed directly and the stored
  effect reprojected — two different paths, one flickering against the other. Now
  the fresh result only ever arrives as the next frame's delta, and what varies is
  the delta's age rather than which path drew the frame.
- **Clip both paths or neither.** `CSApplyDelta` clamped its reconstructed value
  and `CSDecode` did not, so the two disagreed on highlights — on HDR content, a
  flicker on exactly the pixels the eye tracks.
- **Degrade, do not cliff.** A pixel whose history cannot be reprojected used to
  fall back to the game's own colour: full effect one frame, none the next, on
  exactly the pixels motion uncovers.

### Still open

- A rare black flash that only appears with the add-on enabled. A 120 ms stall
  does not reproduce it, it does not correlate with the >100 ms frames Streamline
  reports, and a session with identical settings can show none at all.
- The 4.8 ms itself. Two routes remain: run the network below render resolution
  (less work, moderate risk) or move it to a second queue (same work, off the
  critical path, and a synchronisation bug there hangs the GPU). Stage one of the
  second route is done and validated — the network reads private copies of Color,
  Depth and MVec correctly, and those copies cost 0.07 ms.
