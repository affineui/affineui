# Tracing & Performance Logging

Status and roadmap for AffineUI's instrumentation: what exists today, what it
caught, where it lied, and what to build next.

> **Trying to find a slowdown right now?** Don't start here — start with
> the task-oriented playbook: **[HOW_TO_PROFILE.md](HOW_TO_PROFILE.md)**
> (decision tree, which flag for which symptom, worked examples). This
> document is the *reference* for what every flag/counter/dump is; that one
> is the *guide* for using them.

**Audience & ownership.** This document is the reference for the
*instrumentation/data plane* — counters, timers, trace flags, and dump
formats. The **affinetools** devtools (element tree, performance panel,
resource views) are owned separately and are a *consumer* of this layer:
everything below is designed so a panel, a CLI dump, or an AI agent reading
stderr all see the same numbers. Nothing in this layer may depend on
affinetools; affinetools should depend only on the public structs and dump
formats described here.

---

## 1. Design principles (learned, not aspirational)

These come out of real debugging sessions (most recently the Affine 2600
"1 frame every 2–3 seconds" hunt, 2026-07-03):

1. **Zero observer effect when off.** Every trace flag is a
   `static const bool` read of `getenv` once; disabled instrumentation must
   cost nothing measurable on hot paths. Always-on counters are relaxed
   atomics or plain struct fields written where the work already happens.
2. **The overlay must never touch the document.** The perf HUD renders via
   `Renderer::draw_debug_overlay` ([renderer.h](../include/affineui/renderer.h)),
   which bypasses the DOM/layout pipeline entirely — perf instrumentation
   that reflows the thing it measures is worse than none.
3. **Wall clock is truth; per-frame duration is not.** The HUD's frame time
   comes from the frame callback cadence, so a pipeline that stalls between
   frames still reads "16.7 ms / 60 fps" while the wall clock shows one frame
   every 3 seconds. This exact blind spot hid the synth's per-step
   4-second stalls. Any fps number must be derivable from wall-clock frame
   *gaps*, not frame *durations* (roadmap item R0).
4. **stderr line protocol, greppable tags.** Traces print single lines with a
   `[tag]` prefix (`[attr]`, `[batch]`, `[rect]`, `[vec]`, `[menu]`,
   `[font]`, `[blk ...]`). This is deliberately machine-readable: an agent
   (human or AI) drives the app headless, captures stderr, and greps. Flush
   (`fflush(stderr)`) after writes on paths that may precede a hang — a
   buffered trace that dies with the process is a trace that never happened.
5. **Instrument the layer that owns the cost.** Per-attribute mutation timing
   lives in `set_attribute_on_element`, not in callers; batch-commit timing
   lives in `end_view_mutations`. When a number is surprising, the
   sub-phase breakdown must live in the same function so the two can't drift.
6. **Full compile-out is a supported configuration (decided 2026-07-03).**
   The entire subsystem — counters, HUD, traces, telemetry, spans, and the
   future affinetools protocol server — lives in the main lib behind one
   master compile switch set when the single-file lib is compiled
   (single-file convention: `#define` before include, sokol-style). When
   off, **no perf logging/monitoring code is included in the build**.
   Recommended shape: call sites are no-op macros / inline stubs so hot
   paths carry zero code, while public accessors (`Renderer::stats()`,
   `App::frame_telemetry()`) remain declared and return zeroed stubs so
   host code compiles unchanged in both configs. Per-feature defines
   (`AFFINEUI_MEM_DEBUG`, R2 alloc counters, R5 spans) nest *inside* the
   master switch; runtime env flags gate within compiled-in features.
   "Always on" elsewhere in this document means *default-on when the
   subsystem is compiled in*.
7. **Compiled-in but idle must be near-free (decided 2026-07-03).** Prebuilt
   Rust / C# / Python binding binaries ship with the perf subsystem compiled
   *in*, so leaving probes in a shipping build must carry minimal-to-no
   cost. Probe/trace macros gate on **one global bool** — a relaxed
   `std::atomic<bool>` (not `static const`: affinetools attach/detach and
   API enable flip it at runtime) — checked inline, disabled path
   predicted-not-taken. **When the gate is off, the probe's arguments are
   never computed: the branch skips over the entire call** — the macro form
   `if (aui_perf_on) { probe(expensive_args…); }` — never
   argument-then-discard. Plain counter field writes that are cheaper than
   a branch (RenderStats style, written where the work already happens)
   stay unconditional; the gate is for anything that formats, allocates,
   locks, serializes, or computes arguments. The existing
   `static const bool` getenv pattern remains fine for dev-only traces that
   never need runtime toggling.

---

## 2. Inventory — what exists today

### 2.1 Perf HUD (`AFFINEUI_PERF_OVERLAY=1`)

Read once at App startup ([app.cpp](../src/app/app.cpp) ~line 311). The
Affine 2600 example also exposes it as a `--perf` flag (pattern worth
copying into other examples). Renders a top-right panel (corner selectable
via `DebugOverlayCorner`) with:

- frame ms + fps (see the blind-spot warning above)
- framebuffer px, CSS pt size, DPI scale
- per-stage µs: `prep` / `layout` / `dl` (display-list record) and
  `rast` / `comp` (root-layer rasterize/composite), plus retained-layer size
- `ops` (cached display-list ops), `culled`, `rects` (dirty rects),
  `dirty %` (dirty area of the viewport)
- state flags (recorded / dl-changed / rasterized / partial / direct /
  reused / quiet…) and a 64-frame frame-time sparkline

Enabling the overlay also disables the idle short-circuit in `cb_frame`, so
the app renders every frame — useful for measurement, but remember it
changes idle behavior.

### 2.2 `RenderStats` (always on)

[renderer.h](../include/affineui/renderer.h) `struct RenderStats`, reachable
via `Renderer::stats()`. Lifetime counters (frames, display-list
records/replays/changes/unchanged, ops culled, root-layer
rasterizes/partials/composites/direct-composites) plus per-frame values
(stage µs for prepare/layout/record/raster/composite, cached op count,
dirty-rect count/area/percent, first & largest changed op with bounds, layer
capacity/content size, and the boolean flags the HUD renders). This struct is
the natural nucleus of the affinetools performance panel's frame feed.

### 2.3 Memory counters (`affineui::mem`)

[memory.h](../include/affineui/memory.h) / [memory.cpp](../src/memory.cpp):

- **Always on:** `mem::stats()` → `{live_bytes, live_blocks, peak_bytes,
  total_allocs, total_frees}` (relaxed atomics). `mem::report_leaks()`
  returns the live-block count for "everything freed" assertions.
- **Compile-time deep mode:** `AFFINEUI_MEM_DEBUG` adds an intrusive
  live-block list (leak report with size/tag/sequence), payload poisoning
  (0xCD fresh / 0xDD freed), double-free detection via header magic, and a
  thread-local RAII `mem::Tag` stack for attributing allocations.

**Caveat that matters:** this tracks allocations routed through the
`affineui::mem` seam (host-allocator path, lexbor's pool via
`lexbor_memory_setup`). It does **not** see global `new`/`delete` from std
containers — which is where reconcile temporaries live. The reconcile
benchmark measures those with its own global `operator new/delete` counters
(see §2.5). Closing that gap is roadmap R2.

### 2.4 Environment trace flags

| Flag | Where | What it prints |
| --- | --- | --- |
| `AFFINEUI_SAMPLER` | src/diag/sampler.cpp | **The "where is the time actually going" lens.** In-process stack-sampling profiler (§2.7). Dumps aggregated hot call stacks to `affineui_profile.txt` every 5 s. Symbolize offline. |
| `AFFINEUI_TRACE_JSON` | src/diag/sampler.cpp | **The "how long did each span take" lens.** Chrome-trace-format spans to `affineui_trace.json` (§2.7). Load in `chrome://tracing` / Perfetto. Spans: `frame`, `dispatch`, `layout`, `settle.global`, `settle.scoped`, `settle.attr`. |
| `AFFINEUI_PERF_OVERLAY` | app.cpp | On-screen HUD (§2.1); forces per-frame render |
| `AFFINEUI_FRAME_TRACE` | app.cpp | Per-rendered-frame stage line to stderr: `[frame] N.NN ms prep= layout= record= raster= composite= dl_changed= culled= dirty_rects= area=%  LPRZA` (flags: Layout/Paint dirty, Recorded, rasteriZed, Animations). The fast first-look at "which frames spiked and in which stage". |
| `AFFINEUI_MENU_TRACE` | document.cpp | **The mutation-cost lens.** Any `set/remove_attribute_on_element` ≥0.5 ms as `[attr] set 'class' took 33.94 ms`; sub-phase line `[attr] set 'x' root=N rematch= restyle= reveal=` when those sum ≥1 ms; `[batch] structure-changed took N ms` and `[batch] SCOPED structure roots=N rematch= recollect= total=` at settle; `[settle] rematch(all)= recollect=` split; `[rect] hidden relayout took N ms` in `find_element_rect`; menu hover-switch diagnostics |
| `AFFINEUI_SETTLE_GLOBAL` | document.cpp | **Scoped-settle bisect lever.** Forces every structural change through the OLD whole-document settle instead of the scoped path. If a style/position bug DISAPPEARS with this set, the scoped rematch is missing a subtree (positional-selector blast radius). Slow by design — diagnosis only. |
| `AFFINEUI_ATTR_CHECK` | document.cpp | **DOM-corruption sweep.** Walks every block's lexbor attr list at layout entry / batch begin+end and reports broken first/last/prev/next/owner links: `[attrcheck:where] CORRUPT block=… : reason` + a per-op chain dump; prints `swept N blocks, M corrupt` each pass. Caught the lexbor attr-steal bug (see [[lexbor-attr-steal-bug]]). |
| `AFFINEUI_VEH` | examples/16 main.cpp | **First-chance AV reporter (win32).** Prints the FIRST access violation with a raw return-address stack before the WndProc kernel-callback filter can swallow it, then terminates. Symbolize the stack offline. The only way to catch faults inside frame/event callbacks on win32. |
| `AFFINEUI_LAYOUT_DUMP` | document.cpp | Per-layout block tree: `[blk i p=parent] <tag> cls='…' bounds=x,y WxH h= minh= disp= pos= flexdir=` — one line per block, prints on every `layout()`. Watch for coordinate explosion (T6 Yoga blowup). |
| `AFFINEUI_PAINT_TRACE` | document.cpp | Paint-pass diagnostics |
| `AFFINEUI_VEC_TRACE` | document.cpp | dcs-vec stacking decisions: `[vec i] kids= min= gap= needed= avail= … -> STACKED/row` + post-toggle field style |
| `AFFINEUI_FONT_TRACE` | document.cpp | Font registration/loading: `[font] 'family' wN REGISTERED/FAILED/NOT LOADED` |
| `AFFINEUI_TEXT_TRACE` | nanovg_painter.cpp | Text raster path diagnostics |
| `AFFINEUI_DUMP_HTML` | examples 11 & 16 | Headless dump of the view's HTML **with the app's real stylesheets inlined** for browser A/B (see docs on the conformance harness). Should be promoted to a shared helper rather than per-example code |
| `AFFINEUI_TELEMETRY=<path>` | app.cpp / diag/telemetry.cpp | R1 per-frame JSONL stream: session preamble, `frame` records, ≤1 Hz `idle` heartbeats. Field schema: [AFFINETOOLS_PROTOCOL.md](AFFINETOOLS_PROTOCOL.md) |
| `AFFINEUI_TELEMETRY_EVERY=N` | diag/telemetry.cpp | Sample the telemetry stream — write every Nth `frame` record (`session`/`idle` never sampled out) |
| `AFFINEUI_MEM_DEBUG` | compile define | Deep allocator debugging (§2.3) |
| `AFFINEUI_PERF=0` | compile define | Master switch (§1.6): strips the perf/monitoring subsystem from the build; telemetry accessors become zeroed stubs |

### 2.5 Benchmarks & test-side instrumentation

- **Reconcile bench** — [tests/test_reconcile_bench.cpp](../tests/test_reconcile_bench.cpp),
  run with `affineui_tests.exe -tc="reconcile bench" -s`. Counts global
  `new/delete` and sink ops via a `CountingSink`. Reference numbers
  (RelWithDebInfo, 280-node tree): no-change rebuild **81 µs / 0 sink ops /
  ~439 allocs of temporaries**; 18-value change → **exactly 36 sink ops**;
  fresh build+serialize 283 µs.
- **Frame-budget tests** — e.g. "menubar hover-switch dispatch stays under a
  frame budget": perf assertions inside doctest. Valuable but load-sensitive
  (they flake while builds run); roadmap R6 makes budgets tolerant of
  machine noise.
- **RecordingPainter** ([tests/test_document.cpp](../tests/test_document.cpp))
  now records `fill_path`/`stroke_path` (paint, endpoints) in addition to
  rects/arcs/text — tests can assert on the vector-path pipeline the same
  way they assert on classic draws.

### 2.6 Field results — what this kit has actually caught

Worth recording because it validates the *shape* of the kit (env flag +
stderr line + headless run):

- `[attr] set 'class' took 33.94 ms` × ~117/step → found the whole-document
  generated-content recollect (fixed by token-precise dependency checks) and
  the per-mutation stylesheet dependency scan (fixed with
  `attr_subtree_local_cache`).
- The phase line (`rematch= restyle= reveal=` all <1 ms while the total was
  33 ms) proved the cost was *outside* the measured phases — bisecting by
  measurement, not by guesswork.
- `AFFINEUI_LAYOUT_DUMP` showed styled, sane blocks while the paint was
  wrong → pointed at draw/restyle rather than layout, which led to the
  anonymous-block `var()` inheritance bug in `parent_resolved`.
- The HUD's `dirty 0.00%` at "60 fps" while the sequencer was supposedly
  playing was the tell that frames weren't actually flowing (blind spot #3).

### 2.7 The stack sampler & Chrome trace (`src/diag/sampler.cpp`)

> **For the how-to — the decision tree, worked examples, and the workflow —
> see [HOW_TO_PROFILE.md](HOW_TO_PROFILE.md).** This section is the
> reference: what the two tools are, where their output goes, and the
> implementation constraints that keep them correct.

Added 2026-07-03; the two tools that turned the "app feels slow" hunt from
guesswork into evidence. Stage timers (`AFFINEUI_MENU_TRACE`,
`AFFINEUI_FRAME_TRACE`) tell you *which stage* is slow; these tell you
*which call path* (sampler) and *which span* (trace).

**Why in-process (no external profiler).** The dev box has no VTune / no
`perf` / no debugger installed, and a windowed-subsystem exe can't be driven
by most sampling tools anyway. Both live in the framework (`src/diag/`),
started from `App::run`, so every `affineui::App` gets them free. Do NOT
reimplement per-example (an earlier attempt put the sampler in
`examples/16/main.cpp` and was correctly rejected — wrong layer).

**`AFFINEUI_SAMPLER=1` → `affineui_profile.txt`.** A watchdog thread samples
the UI thread at ~1 kHz (`SuspendThread` → `GetThreadContext` →
`RtlVirtualUnwind`, the OS x64 unwinder — exact frames, no frame-pointers).
Keeps only exe-relative RVAs, aggregates identical stacks, appends the top
~25 every 5 s (survives a kill). Correctness constraints: **no heap alloc
while the target is suspended** (it may hold the CRT heap lock →
deadlock); `ResumeThread` before bookkeeping; a `suspend failures` counter
in each dump header. Cost: one thread in a `Sleep(1)` loop; the UI thread is
blocked only for the microsecond suspend window. Symbolize with `tools/symprof`
(dbghelp; `symprof <exe> <profile> [top-N]`) — resolves against the exe's
PDB, prints top stacks with file:line plus a flat self-time ranking.
llvm-symbolizer is the manual fallback (RVAs are image-relative; add the x64
base `0x140000000`). **The profile is only valid against the exact exe that
produced it** — a stale dump symbolizes to plausible-but-wrong functions;
the sampler deletes the old file on start to enforce this.

**`AFFINEUI_TRACE_JSON=1` → `affineui_trace.json`.** Chrome-trace complete
events (`"ph":"X"`), one per closed span, flushed immediately (valid after a
kill; `chrome://tracing` and Perfetto accept an unterminated array). Spans
are RAII: `#include "internal/diag.h"` then
`detail::TraceSpan span("name");` — near-zero cost when off. Placed today:
`frame`, `dispatch`, `layout`, `settle.global` / `settle.scoped` /
`settle.attr`. Add more wherever a hot span needs decomposing.

**Synthetic input** for reproducible runs: `tools/drive-input/drive-input.ps1`
posts `WM_MOUSEMOVE`/button messages into the window (win32 `PostMessage`),
exposing `MoveTo`/`Down`/`Up`/`Click`/`Drag` to a `-Script` workload
(`tools/drive-input/dender-workload.ps1` for DENDER). See
[HOW_TO_PROFILE.md](HOW_TO_PROFILE.md) § "Step 0". (Gotcha: `LP` is a
PowerShell alias for `Out-Printer` — name helper functions carefully.)

---

## 3. Gaps

Several original gaps were closed by the 2026-07-03 sampler + Chrome trace
(§2.7): the sampler gives the "no really, where is the CPU" answer that no
counter can, and the Chrome trace gives a real span/flame view. What the
new tools do NOT do: attribute allocations, surface GPU stats, or run in a
CI-ingestible always-on stream. Remaining:

1. **No wall-clock frame-gap metric in the HUD.** `AFFINEUI_FRAME_TRACE`
   now prints per-frame stage ms to stderr, and the Chrome trace's `frame`
   span captures wall time — but the on-screen HUD still shows render ms,
   not the true present-to-present gap (see §1.3). Cheap to add (R0).
2. **No always-on machine-ingestible telemetry stream.** The Chrome trace
   (§2.7) is JSON and pandas/Perfetto-ingestible, but it is opt-in per run
   and span-shaped, not a per-frame JSONL row with the full `RenderStats`.
   No snapshot format for affinetools yet.
3. **Allocation visibility is split.** `mem::stats()` misses global
   new/delete; the global-new counters exist only inside one benchmark.
   No per-subsystem attribution at runtime (reconcile vs layout vs paint).
   *(The sampler catches alloc-heavy paths indirectly — `operator new` on a
   hot stack is a data-locality smell — but does not count bytes.)*
4. **No GPU-side numbers.** Draw calls, buffer appends, pipeline switches,
   texture binds — sokol_gfx ships `sg_frame_stats` /
   `sg_enable_frame_stats()` (already in the vendored copy) and we don't
   read it. Batching/atlasing quality is currently anecdotal.
5. **No reconcile/mutation counters surfaced.** Sink ops per batch, restyle
   scope sizes, rematch element counts, recollect occurrences — these were
   the story of every recent perf bug and none are visible outside
   `AFFINEUI_MENU_TRACE` prose.
6. **Span coverage is coarse.** `detail::TraceSpan` (§2.7) gives real
   nested spans now, but only ~6 are placed (frame/dispatch/layout/settle*).
   A 6 ms layout frame still can't be decomposed into yoga-pass → measure
   calls without adding spans (trivial now — one line each).
7. **`AFFINEUI_DUMP_HTML` is per-example code**, not a core facility.

---

## 4. Roadmap

Ordered; each item is independently landable. Division of labor: everything
here is data-plane work in the core library; the affinetools agent consumes
it (their plan stages S0→S4 start from exactly this dump/snapshot layer —
data-plane first, panel as consumer, CLI dump usable by AI agents, zero
observer effect on the paused UI).

### R0 — Wall-clock truth in the HUD ✅ *landed 2026-07-04*

- `cb_frame` measures the wall-clock gap at callback **entry**
  (steady_clock); the HUD sparkline + fps now derive from gaps, not
  durations, and a `gap^ <max over refresh window>  skip <count>` line
  distinguishes "quiet" from "stalled" at a glance. Skipped presents
  (idle short-circuits) are counted on the short-circuit path.

### R1 — Unified per-frame telemetry + JSONL dump ✅ *landed 2026-07-04*

- `FrameTelemetry` ([telemetry.h](../include/affineui/telemetry.h)),
  assembled in `cb_frame`: wall-clock times (gap, cb duration, skipped
  presents), `RenderStats` per-frame fields, `mem::stats()` deltas. R4
  mutation counters join it when they land.
- `AFFINEUI_TELEMETRY=<path>` streams JSONL (session preamble line first;
  `idle` heartbeats at ≤1 Hz while short-circuiting);
  `AFFINEUI_TELEMETRY_EVERY=N` samples frame records. **Field-level schema
  lives in [AFFINETOOLS_PROTOCOL.md](AFFINETOOLS_PROTOCOL.md)** — the
  wire protocol carries these same objects, so that file is the contract;
  `tests/test_telemetry.cpp` enforces it.
- `App::frame_telemetry()` exposes the same record programmatically — HUD,
  dump, and (future) panel all read one source. The subsystem honors §1.6:
  compiled out entirely under `AFFINEUI_PERF=0` (accessors become zeroed
  stubs; verified config), and the sink gates on one relaxed atomic
  (§1.7) so unsinked frames never format or allocate.

### R2 — Allocation attribution

- Promote the benchmark's global `operator new/delete` counters into an
  opt-in dev-build hook (compile define, e.g. `AFFINEUI_NEW_COUNTERS`) with
  per-scope RAII counters: `reconcile`, `restyle`, `layout`, `paint` scopes
  wrap their phases and report allocs/frees/bytes per frame into
  `FrameTelemetry`.
- Longer term this is the measurement side of the T2 arena work: the target
  is *zero* steady-state allocations in a no-change rebuild (currently ~1.6
  allocs/node of temporaries), and the counter is how we hold the line.

### R3 — GPU / batching stats

- `sg_enable_frame_stats()` behind the perf overlay / telemetry flags; fold
  `sg_frame_stats` (draw calls, apply_bindings, apply_pipeline, buffer
  update/append bytes) into `FrameTelemetry` and a second HUD row.
- Add NanoVG-level counters where sokol can't see the semantics: fill vs
  stroke path counts, gradient-LUT cache hits/misses, vertex-buffer
  high-water mark (the silent-blank-frame class of bug becomes a visible
  counter), glyph atlas occupancy/evictions.

### R4 — Mutation & reconcile counters

- Per view batch: sink ops by kind (create/remove/attr/text/raw), whether
  the batch went structural (`dock_structure_changed`), restyle scope
  (elements rematched, blocks restyled), recollect occurrences, and the
  batch-commit time already traced under `AFFINEUI_MENU_TRACE`.
- Surfaced in `FrameTelemetry` + a HUD line. This is the "why was this frame
  slow" row for the perf panel: every recent regression (33 ms class writes,
  per-step recollects) would have been one glance at these counters.

### R5 — Structured spans (flame data)

- A tiny span API (`AUI_TRACE_SPAN("layout/yoga")`) compiled out by default;
  when enabled, emits begin/end pairs into a ring buffer dumped as Chrome
  `chrome://tracing` / Perfetto JSON (`AFFINEUI_TRACE_OUT=<path>`).
  Buckets stay authoritative; spans are the drill-down. Perfetto format
  because the tooling already exists — we should not build a flame viewer.

### R6 — Test & harness hardening

- Make frame-budget tests measure against a machine-relative baseline (or
  median-of-N) so they don't flake under build load.
- Promote `AFFINEUI_DUMP_HTML` from per-example code into an App-level
  facility (it must keep inlining the real stylesheets — see
  `headless-dump-must-match-runtime-css`).
- A `scripts/telemetry_report.py` starter that ingests the R1 JSONL and
  prints p50/p95/max frame gap, stage breakdown, alloc rates — the "run
  python on it" workflow, checked in next to the schema.

### Conventions for new instrumentation (apply to all of the above)

- Env prefix `AFFINEUI_`, one `static const bool` read, zero cost when off.
- stderr lines: `[tag] key=value …`, one line per event, `fflush` on paths
  that can precede a hang.
- Numbers that appear in the HUD must come from the same struct the dump
  writes (no parallel bookkeeping).
- Timers use `std::chrono::steady_clock`; report ms with two decimals, µs as
  integers.

---

## 5. Quick reference — driving it headless

```powershell
# HUD on screen
$env:AFFINEUI_PERF_OVERLAY = "1"; .\affine_2600.exe     # or: affine_2600 --perf

# capture mutation costs from a headless run
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = ".\affine_2600.exe"; $psi.UseShellExecute = $false
$psi.RedirectStandardError = $true
$psi.EnvironmentVariables["AFFINEUI_MENU_TRACE"] = "1"
# … start, read stderr, grep '\[attr\]|\[batch\]|\[rect\]'

# block tree per layout
AFFINEUI_LAYOUT_DUMP=1   # bounds/display per block, printed on every layout()

# reconcile perf + alloc baseline
affineui_tests.exe -tc="reconcile bench" -s
```

When stderr is redirected to a pipe, drain it continuously — a killed
process loses buffered output, and zombie `link.exe`/app processes from
interrupted runs will hold `.ilk`/`.pdb`/exe locks on the next build.
