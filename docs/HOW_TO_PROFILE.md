# How to Profile AffineUI

A task-oriented playbook: **the app feels slow / hitches / spikes — now
what?** Open this, follow the decision tree, find the cause, fix it.

This is the *guide*. The *inventory* of every flag and counter lives in
[TRACING_AND_PERFORMANCE_LOGGING.md](TRACING_AND_PERFORMANCE_LOGGING.md);
the *budget* every interaction must hit is
[WIDGET_RECONCILIATION.md §1a](WIDGET_RECONCILIATION.md). Read those when
you need reference detail. Read this when you need to *find a spike*.

---

## The one rule that governs everything

**1 ms is ~5 million CPU operations per core.** A UI update touches a
handful of widgets. So a slow interaction is essentially NEVER honest work
— it is an accidental algorithm: a linear scan called in a loop, an event
flood multiplying passes, a whole-document rematch for a one-node change, a
per-element allocation. **Every AffineUI perf bug found to date was one of
these, not "the CPU is too slow."** Do not optimize honest work until the
profiler proves it *is* honest work at the expected scale. The Qt precedent
holds: pages of 10⁴–10⁵ widgets reconciled with insignificant cost — only
drawing was ever slow.

Corollary: **profile spikes, not averages.** An average over all frames is
useless — a session that is 95% idle at 0.02 ms and 5% stalled at 200 ms
*averages* to something that looks fine. The stalls are the bug. Every tool
below is aimed at isolating the long frames.

---

## Step 0: Reproduce it under measurement

Windowed builds take no stdin, so drive them one of two ways:

- **By hand** — launch, perform the slow action, close. Fine for a first look.
- **Synthetic input** — `tools/drive-input/drive-input.ps1` posts real
  `WM_MOUSEMOVE`/button messages into the window via win32 `PostMessage`
  (finds it by pid, or title fallback), exposing `MoveTo`/`Down`/`Up`/
  `Click`/`Drag` helpers to a `-Script` workload file. Use this when a run
  must be *reproducible* (comparing before/after a fix). A ready DENDER
  workload (hover sweep, orbit drag, picks, panel clicks, both splitter
  drags) is `tools/drive-input/dender-workload.ps1`:

  ```powershell
  $p = Start-Process ".\decius_dender.exe" -PassThru   # env flags set first
  Start-Sleep 5                                          # let it boot
  tools\drive-input\drive-input.ps1 -ProcId $p.Id `
    -Title "AffineUI - DENDER mini DCC" `
    -Script tools\drive-input\dender-workload.ps1
  ```

Always turn on tracing for the repro:

```powershell
cd affineui\build\ninja\examples\decius_dender
$env:AFFINEUI_FRAME_TRACE="1"; $env:AFFINEUI_SAMPLER="1"; $env:AFFINEUI_TRACE_JSON="1"
.\decius_dender.exe          # do the slow thing for 10–20 s, then close
```

All three flags compose; one run gives you all three views. (You can also
turn on the on-screen HUD with `AFFINEUI_PERF_OVERLAY=1` for a live fps/ms
readout while you drive.)

---

## The decision tree

```
App is slow.
│
├─ Is it slow when IDLE (nothing happening)?
│    → BUG. Idle must be ~0.02 ms/frame, fully quiet.
│      Check the dirty-gating: something is marking dirty every frame.
│      AFFINEUI_FRAME_TRACE shows nonzero frames with dirty_rects=0.
│
├─ Slow only DURING an interaction (click / drag / hover / resize)?
│    → Step 1: which STAGE?  (AFFINEUI_FRAME_TRACE)
│    → Step 2: which CALL PATH inside that stage?  (AFFINEUI_SAMPLER + symprof)
│    → Step 3: confirm the fix by re-measuring the same driven repro.
│
├─ A specific gesture MULTIPLIES cost (drag gets slower the longer you hold)?
│    → Event flood. One expensive frame is queuing more events, each paying
│      full cost. Look for uncoalesced event handling (see "Event floods").
│
└─ UI VANISHES / renders wrong (not a crash — process still alive)?
     → Not a perf bug per se. AFFINEUI_LAYOUT_DUMP: look for coordinate
       explosion (Yoga blowup → content laid out ~10⁹ px offscreen).
       AFFINEUI_ATTR_CHECK: look for DOM corruption. AFFINEUI_VEH=1:
       catch a swallowed access violation (win32 eats faults in WndProc).
```

---

## Step 1 — Which stage? (`AFFINEUI_FRAME_TRACE=1`)

Prints one line per rendered frame:

```
[frame]  236.31 ms  prep=226.0 layout=222.2 record=3.9 raster=6.7 composite=2.7  dl_changed=0 dirty_rects=20 area=25%  LPRZ-
```

Read the stage columns of the *spiking* frames (ignore the 0.02 ms ones):

| Column | Stage | If it's the big one… |
|---|---|---|
| `prep` / `layout` | style settle + Yoga layout | go to Step 2; likely a whole-document rematch/recollect/relayout |
| `record` | display-list build | go to Step 2; likely full re-record instead of reuse |
| `raster` | rasterization | full-window raster instead of dirty-rect (partial raster gated off today) |
| `composite` | layer composite | compositor path |

`dirty_rects` / `area=%` tell you how much the frame *claimed* changed. A
one-button click showing `area=100%` is a scoping bug — it should be a
button-sized rect.

`AFFINEUI_MENU_TRACE=1` adds the mutation-cost lens on top: any
attribute/settle taking >0.5 ms prints with its sub-phase split
(`rematch= restyle= reveal=`, or `[batch] SCOPED structure roots=N …`).
That often names the stage *and* the operation in one line.

---

## Step 2 — Which call path? (`AFFINEUI_SAMPLER=1` + `symprof`)

This is the tool that answers **"no really, where is the CPU"** — the one
to reach for whenever a fix is being justified by *reasoning* instead of
*measurement*.

A watchdog thread samples the UI thread at ~1 kHz and appends aggregated
hot stacks to `affineui_profile.txt` every 5 s (survives a kill). Then
symbolize with `symprof`:

```powershell
$exe  = "build\ninja\examples\decius_dender\decius_dender.exe"
$prof = "build\ninja\examples\decius_dender\affineui_profile.txt"
build\ninja\tools\symprof.exe $exe $prof 20     # top-20 stacks + self-time ranking
```

**Reading it:**

- A healthy session is **dominated by the idle present-wait**
  (`_sapp_win32_frame` → `App::run`). ~70%+ there = good.
- **Any app-code stack with a large share is the spike.** That is the thing
  to fix. Follow the stack down to the deepest AffineUI frame — that's where
  the cost lives.
- `operator new` / `_Emplace_reallocate` / `std::vector` on a hot stack =
  **allocation churn**, a data-locality violation (§ "The allocation smell").

**CRITICAL — match the binary.** RVAs are positions in one specific build.
Symbolizing a stale profile against a relinked exe gives plausible but
WRONG functions (dbghelp returns the nearest symbol before each moved
address — you'll see nonsense like `cursor_in_content_area → rfind → main`).
Regenerate the profile after every rebuild. The sampler deletes the old
profile on start to enforce this; symprof warns if the PDB doesn't load,
but it *cannot* detect a build mismatch. When symbols look absurd, suspect
a stale dump first.

---

## Step 3 — Which span, and how long? (`AFFINEUI_TRACE_JSON=1`)

For per-interaction timing and a flame view. Emits Chrome-trace spans to
`affineui_trace.json` (flushed per span, valid even after a kill). Spans
today: `frame`, `dispatch`, `layout`, `settle.global` / `settle.scoped` /
`settle.attr`.

Load in `chrome://tracing` or Perfetto for the flame view, or grep the
worst spans without a browser:

```powershell
Get-Content affineui_trace.json |
  Where-Object { $_ -match '"name":"([^"]+)".*"dur":([0-9]+)' } |
  ForEach-Object { [pscustomobject]@{ name=$Matches[1]; ms=[math]::Round([long]$Matches[2]/1000,1) } } |
  Sort-Object ms -Descending | Select-Object -First 20
```

Need a finer breakdown of a hot span? Adding one is a single line — put it
right where you want to measure:

```cpp
#include "internal/diag.h"
...
detail::TraceSpan span("layout.yoga_pass");   // near-zero cost when the flag is off
```

Then re-run. Nest them freely; a 6 ms layout frame decomposes into
sub-spans the moment you place them.

---

## Common shapes and their fixes

These are the categories every AffineUI perf bug has fallen into. Match
the profile to one:

### The quadratic-in-disguise
A cheap-looking function (`element_of`, a hit-test, a lookup) that is
correct in isolation but called **inside a loop over all N blocks, per
event** → O(N²) per interaction. The profiler shows one function at a huge
share with a shallow stack. *Found: `StyleStore::element_of` linear scan =
60% of a session; it was called per-block per-mouse-move.* Fix: make the
inner lookup O(1) (index/array), or hoist it out of the loop.

### Event floods (multiplying cost)
A gesture gets **slower the longer you hold it**. One slow frame lets input
events (mouse moves, WM_SIZE during a resize) queue up; each queued event
pays full cost (hover update, relayout) synchronously, which pushes the
next frame later, which queues more. *Found: 868 relayouts / 5.5 s from
uncoalesced mouse moves; seconds-slow resize from per-WM_SIZE relayout.*
Fix: **coalesce to at most one dispatch per frame** (`cb_frame` flushes a
single pending move/resize; presses flush any pending move first to keep
ordering). Look here whenever cost scales with gesture *duration*.

### Whole-document work for a local change
A one-node change triggers a full-document rematch, recollect, relayout, or
re-raster. The `MENU_TRACE` line says `roots=1` but the time is 100+ ms, or
`FRAME_TRACE` shows `area=100%` for a tiny edit. *Found: every structural
change cleared the whole style cache and rematched all ~2000 elements.*
Fix: **scope to what changed** — record the changed subtree roots, rematch/
restyle/recollect only those, keep caches warm. Whole-document work on an
incremental change is a scoping bug, full stop. (Bisect lever:
`AFFINEUI_SETTLE_GLOBAL=1` forces the old whole-document path — if a
*correctness* bug disappears with it set, the scoped path is missing a
subtree.)

### The allocation smell (data locality)
`operator new` / `vector` reallocation / `unordered_map<string,string>`
emplace shows up *inside* a hot reconcile/style/layout stack. This is the
data-locality violation the budget doctrine forbids: per-node heap churn
where the design calls for sequential arena/SoA walks. *Found: `collect_blocks`
rebuilds a `vector<pair<string,string>>` of every element's attributes per
element during selector matching.* Fix: pass a view, cache the structure,
or intern the strings — never allocate per node on the hot path.

### Honest-but-unbatched work
Occasionally the work is real (e.g. a full panel rebuild) but done with the
wrong granularity — rebuilding 2000 Yoga nodes to move one splitter, or
re-recording the whole display list to repaint one pane. Fix: retained
structures with dirty marking (retained Yoga tree, display-list reuse,
per-pane cached layers) so the incremental change does incremental work.
This is the last tier — only reach for it after the profiler shows the
cheaper bugs above are gone.

---

## When the UI vanishes or renders wrong (alive, not crashed)

Not a spike — a correctness failure that *looks* like the app died. No
crash dump exists (`%LOCALAPPDATA%\CrashDumps` empty for the run) ⇒ the
process is alive, rendering wrong.

- **`AFFINEUI_LAYOUT_DUMP=1`** — one line per block with bounds. Scan for
  coordinate explosion: heights/positions in the millions or ~2³⁰. That's
  the Yoga blowup (flex-column + overflow:auto in an anchored-absolute
  panel) — content laid out a billion pixels offscreen reads as a blank UI.
- **`AFFINEUI_ATTR_CHECK=1`** — sweeps every block's lexbor attribute list
  at layout/batch boundaries and reports broken links (`CORRUPT block=…`).
  This caught the lexbor attr-steal memory-corruption bug.
- **`AFFINEUI_VEH=1`** (win32) — prints the FIRST access violation with a
  raw stack before the WndProc kernel-callback filter swallows it, then
  terminates. The *only* way to see faults inside frame/event callbacks on
  win32; symbolize the printed stack with symprof/llvm-symbolizer.

---

## Crash dumps (when it does die)

WER writes minidumps to `%LOCALAPPDATA%\CrashDumps\<exe>.<pid>.dmp`. With no
debugger installed, parse them with the Python `minidump` package
(`pip install minidump`): read the exception record (code + faulting
address), the fault-time register context, and scan the faulting thread's
stack memory for return addresses that fall in the exe's module range —
each is a frame. Symbolize those RVAs with `symprof` or llvm-symbolizer at
`0x140000000 + RVA`. This is how the lexbor attr-steal AV was root-caused.
(For a live fault inside a callback, `AFFINEUI_VEH=1` is faster — it prints
the stack directly, no dump parsing.)

---

## The discipline

1. **Measure before fixing.** If you're about to justify a fix by
   reasoning, run the sampler first. Every time this was skipped, the guess
   was wrong.
2. **Profile spikes, not averages.**
3. **Match the binary to the profile.** Regenerate after every rebuild.
4. **A whole-document pass for a local change is a bug, not a slow path.**
5. **`operator new` on a hot stack is a smell,** not a cost of doing
   business.
6. **Confirm in the window.** Unit tests and headless numbers do not
   measure felt latency — the user's in-window test is the real
   acceptance. Re-run the same driven repro after the fix and compare.
