# Performance & reliability tickets

Standing mandate (2026-07-03): AffineUI must reconcile accurately in
near-nanoseconds-per-node with bounded allocation, hit max display
refresh (~140Hz) in real apps, and never hide relayout costs. Apps must
never hand-poke the DOM — View + reconcile is the only mutation path.
Measured baseline lives in `tests/test_reconcile_bench.cpp` (280-node
tree: 81µs/0-op clean rebuild; fresh-View+to_html 283µs — and the old
`App::load_view` reparse path dwarfs both; see memory notes).

## T1 — App/View reconcile fast path (IN FLIGHT)
`DocumentViewSink` inside document.cpp applying View mutations to the
retained DOM (remote_id→element map; attr/text via the existing
set_attribute_on_element classification; structural creates via
eventless inserts + scoped restyle à la the dock-gesture machinery;
svg-child mutations are paint-only — no blocks). View gains RawHtml
sink hooks (open_node skips the sink for RawHtml; set_text early-outs
before the sink — both must emit). App gains a persistent View +
`set_view(builder)`/`rebuild_view()`; `load_view` stays as bootstrap.
Acceptance: affine_2600 runs at display refresh during cable drags and
sequencer playback with zero HTML reparses after boot.

**Field data 2026-07-03 (DENDER, `decius_dender.exe --profile`):** on the
real 105 KB DENDER document the current set_view/rebuild_view steady
state takes **~64 s per rebuild** — including a *clean* rebuild with
zero app-state changes (which the 280-node bench does in 81 µs). For
scale: the legacy load_view reparse path is ~190 ms/interaction on the
same document (build 2.7 ms + load_view 187 ms). DENDER is fully
migrated and ready (DenderView::build_into + App::set_view wiring) but
pinned to load_view behind `kUseReconcileFastPath = false` in
dender_app.cpp until this is fixed — flip that flag for an instant
real-app repro/acceptance test.

The `--profile` diff probe decomposes the 64 s into two independent
bugs:
1. **View diff churn — attribute writes are diffed per WRITE, not per
   final value.** A no-change DENDER rebuild emits **84 patches**
   (first build: 3927). Every element that declares base classes and
   then appends a modifier via a second write (`.cls("…--has-sub")`)
   produces TWO class patches per rebuild forever — the intermediate
   value differs from last build's final, the final differs from the
   intermediate. Coalesce attr writes within a build pass and diff
   only end-of-build values; a clean rebuild must emit 0 ops on real
   apps, not just the bench (whose tree never double-writes an attr).
   **Full categorization (`decius_dender.exe --probe`, dumps all 84):
   100% are repeated same-attr writes — class ×70, data-step ×6,
   style ×6, data-dcs-select ×2; structural 0, text 0.** So the
   coalescing fix alone makes DENDER's clean rebuild exactly 0 ops.
   CAUTION for the fix: naive last-write-wins is NOT sufficient for
   `style` — the probe shows pairs like `--fill:50%` then
   `width:72px` on the same element (two different properties as
   separate full style writes); last-write-wins would DROP the fill
   var. Coalescing needs per-attribute merge semantics (style =
   declaration merge; class/list attrs = defined policy), and the
   skeuo agent should check whether today's replace-per-write
   behavior is already losing `--fill` in the final DOM (possible
   live rendering bug on sliders/combos, not just churn).
2. **DocumentViewSink charges full-document work per op:** 64 s / 84
   ops ≈ **775 ms per set_attribute("class")** — i.e. restyle/
   recollect (likely + layout) runs per mutation instead of once in
   end_view_mutations. Batch: record touched nodes during the
   mutation window, do one scoped restyle/recollect at end.

**PHASE 0 STATUS (2026-07-03 evening):** (a) end-of-build attr
coalescing LANDED in View (view.h/view.cpp: attrs_before snapshot +
end() flush; style = within-pass declaration merge) — DENDER `--probe`
clean rebuild is now **0 patches** (was 84; first build 3927→3883);
(b) the §8.2/§4.1 batch settle LANDED in document.cpp
(view_batch_attr_roots; per-op raw-write+record under a view batch;
end_view_mutations: dedupe roots → subtree rematches → ONE
resolver->clear → restyle per root → reveal per root against the WARM
cache → per-root dirty rects). The old ~775 ms/op was the per-op
resolver->clear() forcing every reveal probe to COLD-resolve all
blockless (hidden-menu) children — 78 ms/op at DENDER scale;
(c) the same-value apply gate already existed in
set_attribute_on_element. (d) **Dock replay determinism FIXED** — the
2,444-patch/pass churn was `dock_node_from_layout` naming split groups
from a function-local `static int replay_split_seq++` (identity from a
synthesis counter, never stable across passes → whole dock region
re-created every rebuild → structural settle + ~600 per-op dock-engine
attr re-applies = the persistent ~45 s). Split-group identity is now
CONTENT-DERIVED (axis + child ids), per the user doctrine: identity
comes from what a node IS; unchanged arrangement → same StableId →
zero ops; genuinely changed → wholesale re-splice (cheap).
**MEASURED RESULT (`decius_dender.exe --profile`, 2026-07-03):** LIVE
probe 2,444 → **0 patches**; clean rebuild **~50,000 ms → 2.04 ms**
(Phase-0 ≤10 ms gate MET); suite 427/427 green. Remaining: camera-
change interaction = **215 ms** — the viewport svg raw-html swap sets
view_structure_dirty → full structural settle. Fix = the paint-only
lane for svg-only raw-html swaps, or migrating the DENDER viewport to
the canvas/custom-paint path ("reconcile is for state, canvas is for
geometry"); the 250 ms settle itself is T7's unindexed-cascade cost.
USER DOCTRINE ADDENDA (2026-07-03, for WIDGET_RECONCILIATION.md):
two-level hashing — IDENTITY hash (tag/selectors; changing what an
element IS = delete+recreate, expensive) separate from identity+VALUES
hash (values on an unchanged element just reset, trivial); hash only
SET/non-default attributes (one-selector-no-attrs nodes hash
trivially); canonicalization must be computed IDENTICALLY for shadow
and real DOM (one shared hash function, applied-hash stored at apply
time); xxHash64-class is sufficient (collision odds negligible +
debug oracle). Update semantics: the declared set is authoritative —
an update DELETES attributes not set in the new declaration, where
delete = UNSET back to default, exactly like never having been set.

DESIGN DIRECTIVE (user, 2026-07-03): reconcile must be a near-no-op
for unchanged parts even on huge documents — cheap modification
detection (low-cost hashing) plus optimal reconciliation. Preferred
shape: a **version stamp per widget/subtree** — every mutation
increments the widget's version; a subtree whose version matches its
DOM's last-applied version is skipped entirely (no per-node walk, no
attr compares). Applies to both bugs above: end-of-build coalescing
gives correct per-node deltas, version stamps make the common
nothing-changed case O(changed-subtrees) instead of O(document).
Correctness is absolute ("it needs to ALWAYS work"): wrong-DOM
outcomes are never acceptable; prefer a slower always-correct path.
Efficiency envelope: low CPU/memory, no base-heap allocation churn
(temp allocation pools are fine).

VISIBILITY-SCOPED RECONCILE (user, 2026-07-03 — must be part of this
architecture, proven in the user's prior reconciliation system):
don't even ATTEMPT to reconcile content that cannot be seen —
(a) **list/tree items outside the viewport** (requires the
virtual-list/tree system, ties into T5), and
(b) **pages/tabs/panels fully obscured** by other pages/tabs
(inactive dock tabs, unselected inspector sheets, closed panels).
Implication: the win must include skipping the BUILD, not just the
diff — obscured/offscreen declaration lambdas shouldn't run at all
(deferred builder closure + placeholder node). Versioning supplies
the reveal-correctness story: a subtree that changed while hidden has
version ≠ applied-version, so it reconciles exactly once when it
becomes visible. Precedent already in-tree: declarative dock keeps
hidden tab placeholders and builds them lazily on first reveal.

INCREMENTAL DOM HASHING (user, 2026-07-03 — from their prior system,
part of this architecture): every node carries a subtree hash
(Merkle-style: own canonical content combined with children's subtree
hashes). Comparing two DOM roots is then an O(1) hash compare —
identical or different, no walk. Mutations only mark the hash DIRTY
up the ancestor chain (early-out at an already-dirty ancestor);
recompute is incremental and lazy — a dirty node re-combines its
cached own-content hash with the children's cached subtree hashes, so
neither the whole tree nor untouched individual elements are ever
re-hashed. Complements version stamps: versions track "did this
widget change since last apply" (writer-side, cheap), hashes give
structural equivalence of arbitrary subtrees (works even across a
full rebuild with no shared version lineage). Also makes the
"ALWAYS works" oracle cheap enough for debug builds: after any
reconcile, hash(document) == hash(fresh build) as a continuous
assert. Doc must pin down: canonical hash input (tag, sorted attrs,
text, child order — EXCLUDING transient user state like focus/
scroll/caret and bookkeeping attrs), hash width vs collision policy
(a collision would silently skip a real change — wide hash +
debug-mode full verify, or hash-equality only as a fast-path on top
of identity/version checks).

SHADOW DOM MODEL (user, 2026-07-03 — from their prior system). The
SETTLED invariants:
- A widget's "render" takes a DOM-OUTPUT OBJECT (a view) and writes
  dom elements into it; that emitted output is what reconciles
  against the real DOM. Emission is forward-only, "like serializing
  to a file" — in the prior system it landed in chained-block
  temporary memory (bump-alloc nodes+strings, stable addresses, no
  realloc invalidation, no base-heap churn — T2 as design, not
  afterthought).
- Render runs ONLY WHEN NEEDED (version/dirty gate): widgets not
  touched RETAIN their shadow shape — no re-render, no re-emit, no
  re-diff. Per-widget render granularity is the unit of incremental
  work; "clean" is free because the cached shadow subtree simply
  persists.
RELIABILITY SPINE (user, 2026-07-03: "rock solid reliable" — this is
architecture, not a test plan bolted on):
1. Every skip must be JUSTIFIED, not hoped: a version match or hash
   match is a checkable claim. Debug builds can verify any skip;
   the optimizer is only ever allowed to elide work it can prove
   equivalent.
2. Divergence must be DETECTABLE cheaply: the incremental hash gives
   an O(1) oracle — hash(real dom) vs hash(full fresh build) — run
   continuously in debug, sampled or on-demand in release. Silent
   corruption (the worst failure mode) becomes a detected event.
3. Mutation window is TRANSACTIONAL: an exception mid-render or
   mid-reconcile leaves the previous consistent DOM (or triggers 4);
   never fail forward into a half-applied state.
4. A DEGRADATION LADDER that always ends in correct: skip →
   region re-render+reconcile → full re-render+reconcile → full
   rebootstrap (the load_view path, kept forever as the escape
   hatch). Any detected inconsistency self-heals by dropping down a
   rung and logging loudly. The system picks the cheapest rung it
   can prove correct — reliability is not traded for speed because
   speed is only ever claimed where equivalence is established.
5. Test plan follows from 1–4: property/fuzz random mutations vs the
   oracle, exception injection at every stage boundary, alloc-count
   gates, real-app fixtures (DENDER --profile), soak runs.

DIRTY FLAGS IN THE WIDGET TREE (user, 2026-07-03 — the concrete
mechanism for the UPDATE stage; no searching, no side queues):
every widget node carries two flags —
- SELF-DIRTY: "I am dirty; re-render/re-reconcile me."
- DESCENDANT-DIRTY: "one of my children (transitively) is dirty."
Semantics:
- State change on a widget sets its SELF-DIRTY and walks UP the
  ancestor chain setting DESCENDANT-DIRTY, EARLY-OUT at the first
  ancestor that already has it — mutations amortize to near-O(1),
  worst case O(depth).
- The update/render pass starts at the root and follows flags only:
  neither flag → skip the whole subtree (the root's flags ARE the
  global "anything dirty?" check — the true do-nothing frame is one
  branch); DESCENDANT-DIRTY only → descend only into flagged
  children; SELF-DIRTY → this is a region root: run its render.
  Multiple self-dirty nodes under one ancestor are discovered on the
  same descent — that's where the coalescing/escalation decision is
  made.
- Update/render CLEARS the flags as it processes (the walk visits
  exactly the flagged spines, so clearing during the walk is
  complete; steady state returns to all-clean).
Rules the doc must pin down:
- RENDER CALLS USER CODE, THEREFORE UPDATE IS NON-REENTRANT (user,
  emphatic), and mutating the widget tree during render must be
  IMPOSSIBLE BY CONSTRUCTION, not merely detected: the render API
  hands user code only a CONST view of widget state and an EMIT-ONLY
  output object (elements/attrs/text — no tree handles, no
  state-setter surface in scope). The type system is the first wall;
  the second is one engine-level gate: WHEN RENDERING, MUTATION IS
  BLOCKED (user, verbatim) — a single in-render flag checked by
  every mutation entry point; blocked attempts do not take effect
  (debug = assert, release = blocked + loud diagnostic;
  hard-to-crash: never corrupt the pass, never crash the app). Render is a pure projection of
  widget state → shadow dom; every equivalence claim (versions,
  hashes, the oracle) rests on that purity. Corollary: event
  dispatch never runs inside a render pass — frame order is events →
  update/render → reconcile → paint, each phase closed before the
  next.
- A widget removed while dirty may leave a stale DESCENDANT-DIRTY on
  ancestors — must be benign (next descent finds nothing; never
  crash).
- A hidden/obscured subtree keeps its flags but the descent stops at
  the visibility boundary; the pending dirt is processed on reveal
  (composes with versions).

GRANULARITY — DYNAMIC, NOT GUESSED (user, 2026-07-03, Qt lesson):
the prior system's tracked region was the Qt PANEL — any change
re-emitted that panel's whole dom in one (incredibly optimized) go
and reconciled it in one go. Panel granularity avoided whole-app
redraws, but a panel that grew very complicated began to STALL:
fixed structural segments are a guess that eventually loses. Our
system should NOT pick granular segments up front — do INCREMENTAL
shadow-dom updates instead:
- A dirty widget marks itself AND its children: its subtree is the
  re-render candidate region.
- Multiple dirty widgets MAY coalesce — escalate to re-rendering a
  covering subtree when that beats K separate fine-grained updates
  (dirty-rect merging, but on the tree; the escalation cost model is
  a design knob the doc must specify).
- Rendering a widget does NOT automatically re-render its children —
  EXCEPT when the parent says so: the parent KNOWS whether its
  re-render invalidates its children (data flows down, structural
  change) and the render API needs that explicit force-children
  switch. Default = children's retained shadow subtrees are reused
  as mount points.

CANONICAL PIPELINE (user, 2026-07-03 — the doc should be organized
around these four stages, each with its own skip gate):
1. widget state change → UPDATE (dirty-mark the widget; version++)
2. update → REDRAW the widget's dom — and ONLY its own: a parent's
   redraw MAY cause child widgets to redraw but usually does NOT.
   Children are stable mount points inside the parent's emission;
   an untouched child's retained shadow subtree is referenced, not
   re-emitted. (Containment is the default, not an opt-in memo.)
3. RECONCILE → the new dirty dom reconciled with the real dom
   optimally (coalesced final values, versions/hashes skip the
   clean parts).
4. UI UPDATE → the real-dom change drives an optimal renderer
   update: mutation classification (paint-only vs restyle vs
   layout) feeds small-rect repaints.
Stage 4 largely EXISTS today (dirty-rect renderer, paint-only
classification, idle short-circuit in cb_frame). Stages 1–3 are this
design's subject; the 3→4 bridge is the sink's mutation
classification and batching contract.

The OPEN TRADE the doc must compare honestly (user: "it's not clear
temporary shadow dom that you throw away beats a retained shadow
dom"): throwaway/per-pass shadow storage VS a retained shadow tree —
memory footprint between frames, re-emit cost for dirty widgets,
pointer/id stability, fragmentation when dirty widgets re-render into
shared blocks, interaction with hashing/versions. Current T1 code is
a retained persistent View diffed in place; that's one point in this
space, not the decided answer. Also required: define what a WIDGET
BOUNDARY is in AffineUI terms (a keyed subtree with a version and a
render/build closure — note this is the SAME primitive the
visibility-scoped-build item needs), since today the samples render
the whole app as one root-level build function.

## T2 — Reconcile hardening (allocation discipline)
Builder temporaries cost ~1.6 heap allocs/node/rebuild (std::to_string,
concatenation, key strings). Add a frame arena / temp block allocator
for the build pass, small-buffer attr storage, and string_view-first
setter paths. Target ≤0.2 allocs/node steady-state, enforced by the
bench (fail CI when exceeded). Also: attr vector small-size
optimization, StableId map reuse.

## T3 — Layout discipline ("re-layout can be hidden")
Audit every hidden relayout trigger: `find_element_rect` forcing layout
on dirty docs, attribute classification that over-invalidates, style
attr writes that should be paint-only. Add:
- a **layout-lock / may-relayout policy callback** (app can veto or
  defer layout during drags/animation; deferred layouts coalesce),
- per-frame layout/cascade counters in the perf HUD,
- a debug mode that asserts/logs on unexpected relayout inside a
  locked region.

## T4 — Deep perf + stability pass on large layouts
Synthetic stress suite: 10k/50k-node documents, deep nesting, wide
flex rows, thousands-of-rows tables, mixed inline SVG; measure
cascade/layout/display-list/raster scaling curves and memory ceilings.
Soak tests (hours-long runs, leak/handle tracking), extend the ASAN
gesture fuzzer to cover reconcile mutations, conformance A/B against
Chrome on big pages. Output: documented budgets ("N nodes → X ms
layout") and regression gates in CI.

## T5 — Virtualization & scroll areas (TABLE STAKES — before users)
Retained-DOM web-style UI without windowing is a foot-gun: naive users
will render 10k-row lists and blame the framework. Needed soon:
- **ScrollArea** container: independent scroll offset, clipping,
  hit-testing, wheel/drag/scrollbar UI, nested scroll.
- **Windowed list box**: `View::virtual_list` exists
  (VirtualListOptions: first_item/visible_items/overscan) but must be
  wired end-to-end to real scroll events, with **row recycling**
  (stable ids reused, zero create/remove churn while scrolling) and
  variable-height rows (item_sizes is already in the options struct).
- Virtualized tree view + table (DCC inspectors need both).
- Guardrails: docs + a runtime diagnostic when a container renders >N
  unvirtualized children ("did you mean virtual_list?").
Acceptance: 100k-row list scrolls at display refresh with flat memory.

## T12 — Window renders BLANK after a minor resize (affinetools; OPEN, user-reported 2026-07-04)

**Symptom:** the affinetools viewer window renders correctly on first
show, then goes fully blank (just the `--dcs-bg` fill) after any small
OS resize (repro: grow the frame +40×30 px). Persistent — stays blank
until… (recovery unknown; further resizes don't fix it). User confirms
this is a real, repeatable bug, not a capture artifact.

**What it is NOT (eliminated by controlled A/B, each technique added to
the *working* command_panel example without reproducing the blank):**
- NOT the frametime **canvas** custom-paint — blanks on the Elements tab
  (no canvas) too.
- NOT **position:fixed;inset:0** — blanks with a normal-flow `height:100%`
  or `height:100vh` flex column too.
- NOT **set_view / the reconcile fast path** — command_panel routed
  through `set_view` (AFT_SET_VIEW probe) resizes perfectly.
- NOT **rebuild-every-frame from on_frame** — command_panel with
  set_view + a per-frame `rebuild_view()` loop resizes perfectly.
- NOT a **layout collapse** — a headless repro (test_view.cpp
  "set_view: full-height column reflows to a grown viewport") shows the
  column DOES reflow to the grown viewport with non-zero rects (it is
  ~20px short of filling height — a separate minor `height:100%`-chain
  rounding issue, filed inline, but NOT the blank).

**What the render trace showed (AFFINEUI_RESIZE_TRACE, since removed):**
the resize frame takes `direct_live_resize` (draws direct to swapchain,
invalidates the retained layer), the next frame re-RASTERs the layer at
the new size, then COMPOSITE-ONLY frames follow — i.e. the render branch
sequence is *correct* and the layer IS re-rastered, yet the composited
pixels are blank. So the defect is either (a) the re-raster replays into
the layer FBO but produces empty pixels, or (b) the composite samples the
layer with wrong UVs after a within-rounded-capacity content resize.

**Landed hardening (correct regardless of T12):** `ensure_root_layer`
now invalidates the retained layer (`layer.valid=false`) whenever the
content size changes within the rounded texture capacity and the partial
exposed-strip path is off — previously it silently kept `valid=true` and
only composited, so a grown viewport sampled a region of the texture that
was never drawn. This made the branch sequence correct but did NOT fix
the blank, so the root cause is deeper (suspect (a) above — the raster
into the resized FBO).

**Remaining unique factor to probe next:** the ONLY thing left that
distinguishes affinetools from every working A/B is the background socket
**reader thread** mutating shared model state under a mutex while the main
thread reads it during build_view / paint. Next step: run affinetools
with the reader thread inert (detached / no target) and confirm whether a
quiet viewer still blanks on resize — if yes, the thread is exonerated and
the cause is in the specific DOM/CSS; if no, it's a threading/timing
interaction with the resize relayout.

**Workaround for now:** none landed; the tool is usable as long as its
window isn't resized. Do not ship affinetools to users until T12 is
fixed.

## T11 — Text-update overdraw ✅ FIXED 2026-07-04 (same day)

**Root cause (not the renderer):** `set_text_on_element` wrote the new
text into the ELEMENT's block while the text actually painted from an
anonymous `#text` child block (collect_blocks creates one, under a
synthetic line-box run, whenever the element was collected with
children — e.g. every View-bootstrap-replayed paragraph). The stale
child run kept painting beneath the parent's new copy forever — exactly
one stale generation, the bootstrap-era string. Both raster paths and
the display-list diff were innocent (verified: full clear + re-raster
per update; ops stable; headless repro drew both strings at the same
position through TestPainter).
**Fix:** `set_text_on_element` routes the new text into the lone
`#text` descendant block when that's the shape (clearing the element
block's copy so it can never double-paint); mixed-content subtrees keep
the previous behavior. Regression test: "set_view: text-only rebuild
replaces the painted run (T11)" in tests/test_view.cpp. Live-verified:
affinetools' 1 Hz counter runs pixel-clean.

Found by affinetools' own live window (its first framework catch): a
paragraph whose TEXT changes each second via App::rebuild_view (the
reconcile set_text paint-only path) renders the new string composited
over the previous one — the old glyph pixels are never cleared, so two+
generations of text stack up ("target idle - NNN presents skipped" and
the status-bar line, both visibly doubled; screenshots captured off the
running window, persistent across frames, not a capture race).
Repro: `tools/affinetools` attached to any idle target — the idle
counter line updates ~1 Hz and corrupts immediately. Suspects, in
order: (1) partial rasterize applies the changed text op's dirty rect
but doesn't clear/redraw the BACKGROUND under it before compositing
glyphs (blend-over-stale); (2) the display-list diff's changed-op
bounds use the new run's bounds only, missing the old run's wider
extent (RenderStats carries first/largest old+new bounds — verify the
union actually feeds the dirty region); (3) same-key set_text marking
paint-dirty without marking the block's raster region stale at all.
Acceptance: affinetools idle view runs for minutes with pixel-clean
text; add a RecordingPainter/goldens test for a text-only reconcile
update inside a retained layer.

## T10 — DENDER windowed bug batch (USER-REPORTED with screenshots, 2026-07-03 late)

1. **Panel text clipping not respected** — inspector content (Parent /
   Collection row, Start/End fields) bleeds past the pane bottom into
   the timeline region. Paint is not clipping children to the overflow
   container's box. Audit Document::draw's push_clip coverage for
   overflow:auto/hidden blocks — text runs (and likely all ops) of
   descendants must clip to the scroll container. Broad visual
   correctness; affects every pane.
2. **Tearout drag from EMPTY tabbar area jumps panel to window left** —
   float-drag start from the tabbar's empty space records a bad grab
   anchor (offset defaults to ~0 → panel snaps to origin-at-cursor,
   reads as "jumped to left edge"). Look at data-dcs-drag press
   handling: anchor must be cursor-minus-panel-origin at press,
   whichever child was hit.
3. **Semi-transparent nav buttons floating mid-viewport** — the
   nav-cluster button column (zoom/move/camera/ortho) rendered detached
   near viewport center = position styles NOT applied (unstyled static
   flow + ghost button look). PRIME SUSPECT: scoped-settle rematch gap —
   a created subtree whose selector matches depend on ancestors/
   siblings outside the recorded root. BISECT: run with
   AFFINEUI_SETTLE_GLOBAL=1 (forces old global settle; added 2026-07-03)
   — if the buttons position correctly, the scoped rematch needs the
   positional-selector blast-radius widening (rematch created subtrees +
   parent's direct children element-local).
4. **Window resize seconds-slow; sustained resize (2–3 s+) makes UI
   "disappear permanently"** — TWO parts. (a) Slowness: WM_SIZE arrives
   per mouse move in the win32 modal sizing loop and each Resize
   dispatch paid full relayout + full text re-measure — FIXED
   2026-07-03: resize events now coalesce to ≤1/frame (app.cpp, same
   pattern as mouse moves). (b) Disappearance: NO crash dump exists →
   process alive, rendering wrong. PRIME SUSPECT: T6 Yoga blowup
   (flex-column + overflow:auto in anchored absolute panel → heights
   ~2^30) triggered at some intermediate width and STICKING — content
   laid out ~10^9 px away looks like a vanished UI. Repro next session
   with AFFINEUI_FRAME_TRACE + AFFINEUI_LAYOUT_DUMP while resizing;
   check content_size/root bounds for explosion. Root-layer GPU realloc
   checked and is NOT the leak (grow-only capacity, full destroy chain).
5. **Moving a panel (timeline at least) DUPLICATES it** — two panes of
   the same type appear. Dock override → View re-emits the pane at its
   new location with a location-derived key while the OLD pane's remove
   never lands (sink map miss / key mismatch on re-parent?). Check pane
   identity: pane keys must be PANEL-identity-stable across placement
   (content-derived dock ids changed 2026-07-03 — the duplicate may be
   a survivor of id-scheme mismatch between seed and override paths).
   Same bug class as the adversarial review's dock-determinism blocker.
6. **Theme-tweaks popover clipped on right edge** (Density/Style/Accent
   rows cut off) — popover width vs content: likely fixed menu width +
   unwrapped rows; minor layout/CSS in the tweaks menu markup.

Landed with this batch: resize coalescing (4a) + AFFINEUI_SETTLE_GLOBAL
bisect lever (3).

## T9 — Interaction budget vs measured: the remaining gap (2026-07-03 evening)

USER-SET per-stage budget (normative, WIDGET_RECONCILIATION.md §1a):
subtree create 3–10 µs · reconcile size diffs 5 µs · no-change style
update 1–2 µs · frames-only layout 4 µs · partial repaint ≤0.1 ms.
Interaction total ≈ 0.1 ms.

LANDED TODAY (each verified by profiler/trace, 427/427 green):
- O(1) StyleStore::element_of (was 60% of session CPU via the hover
  path's per-event linear scans).
- Mouse-move coalescing in App (event-flood death spiral: 868 layouts /
  5.5 s in one session → 39; moves now ≤1 dispatch per frame).
- Scoped structural settle (sink records block roots; settle rematches
  only those subtrees; resolver cache stays warm; per-op invalidation
  on destroy — global settle only for scope-less changes).
- ALL batched attr ops defer to the settle (style writes had still been
  paying per-op restyle inside the window — unsafe AND slow).
- Lone-text set_text = LOCAL block text refresh + remeasure (was: every
  label/value change ⇒ full structural settle).
- dcs-vec layout-time toggles: lightweight path + 8 px unstack
  hysteresis (splitter threshold crossings paid toggle+relayout cycles).
- request_custom_repaint matches cached block attrs (was quadratic).
- Diag substrate: AFFINEUI_SAMPLER (1 kHz stack profiler → 
  affineui_profile.txt, symbolize via llvm-symbolizer) +
  AFFINEUI_TRACE_JSON (chrome://tracing spans: frame/dispatch/layout/
  settle.*). SPIKE-focused analysis only — averages hide the bugs.

REMAINING vs budget (trace: click-class dispatch still 130–200 ms):
| Budget line | Measured today | Fix (in order) |
|---|---|---|
| style no-change 1–2 µs | rematch(root) 40–70 ms | Phase 1 hashing gates + rematch ONLY created subtrees (+sibling blast radius for positional selectors); T7 rule index inside scopes |
| subtree create 3–10 µs | recollect(full doc) 80–140 ms | B2 scoped recollect (block-vector splice + index remap) — THE big rock; block attr/string interning kills the alloc churn (profiler: operator new + string-map emplace in resolve, cascade.cpp:3343) |
| frames-only layout 4 µs | full Yoga rebuild ~10 ms/pass | retained Yoga tree with dirty marking (yoga_adapter) |
| partial repaint ≤0.1 ms | full re-record+raster ~10–15 ms | enable partial root raster (machinery exists, gated on exact dirty coverage) + display-list reuse; THEN per-pane cached layers (M2 compositor — layer.cpp/composite.cpp exist, out of build pending sokol_gfx render-target port): splitter drags become pane BLITS (translation free), only width-reflowed panes re-raster. USER field data 2026-07-03: left splitter (all panes move) hitches to ~100 fps, inspector-bottom splitter (small region) much less — geometry change area drives paint, layers break that coupling |
| reconcile diffs 5 µs | ~µs (SOUND already) | Phase 1 widget dirty-flags so builders skip clean subtrees (clean rebuild 0.89 ms → ns-class) |

USER-SESSION PROFILE 2026-07-03 22:23 (47,951 samples, 42% idle; of the
interactive remainder): ~40% = per-dirty-frame render prep — full Yoga
tree rebuild (layout_blocks_with_yoga) + full display-list re-record
(Document::draw → DisplayListBuilder::end_frame, effective_transform_for,
DisplayList::finalize_hash) with operator-new churn threaded throughout
(vector reallocs per frame — data-locality violation); ~25% = dispatch —
hit_test_blocks_impl linear block scan per pointer frame (candidate:
spatial index / top-down early-out, minor vs paint) + hover/splitter
restyle spine. Confirms T9 items 3+4 are the whole remaining story.

ALLOCATION-CHURN FINDING (symprof, matched profile 2026-07-04): the
recollect/collect_blocks path rebuilds a `vector<pair<string,string>>` of
EVERY element's attributes PER ELEMENT during generated-content selector
matching (element_attrs @document.cpp:1124 ← element_matches_compound ←
generated_rule_matches ← append_generated_content_for_element ←
collect_blocks). Pure per-element heap alloc on the settle hot path — the
data-locality violation the budget doctrine forbids. FIX: pass a view /
cache the attr list instead of copying per match; part of B2 (scoped
recollect must not re-collect the whole doc anyway, but the per-element
copy is a separate, independently-fixable alloc bug).

## T8 — DENDER viewport → threepp GPU engine (kill the SVG stand-in)
Filed 2026-07-03. USER DIRECTIVES (verbatim intent): "don't use SVG for
dynamic painted elements", "why would we be drawing the 3d view with
svg? Why not with 3D?", "Just use threepp for the engine."

Why SVG existed: Phase B ported the web demo's three.js canvas as
software-projection → inline SVG because it needed zero new framework
surface (dender_viewport.h calls itself "the native stand-in for the
web sample's three.js canvas"). It predates the custom-paint canvas.
Cost: every camera move is a full View rebuild (~2.8 ms) + SVG
fragment destroy/reparse (~5.7 ms) = the measured 8.6 ms — DOM work
for content that is not DOM.

Architecture (right layer, no bodges):
- **Engine**: threepp (github.com/markaren/threepp, MIT, C++ port of
  three.js). Maximum parity with the ground-truth web demo — the
  reference viewport.js IS three.js, so scene graph, camera params
  (fov 38, eye/target), materials, and Raycaster picking port 1:1.
- **Interop**: threepp renders OpenGL. The app stays sokol/D3D11 (the
  backend decision is untouched): threepp renders OFFSCREEN in its own
  hidden GL context (GLFW invisible window), RGBA readback (PBO) →
  core dynamic-image → drawn by the viewport's `data-aui-paint` canvas
  block. This is demo-honest AND doubles as the render-to-texture
  engine-interop proof (a host engine bringing its own API is exactly
  the embedding story). Zero-copy native-handle path (sg_image
  injection / nvsgCreateImageFromHandle — already used by the
  root-layer compositor) is the later same-API optimization.
- **Core addition needed** (small, backend-agnostic): dynamic image on
  Painter — `create_image_rgba(w,h,px)` / `update_image_rgba(handle,px)`
  (NanoVGPainter → nvgCreateImageRGBA/nvgUpdateImage; headless stubs).
- **UI side**: viewport block becomes `View::canvas("dender.viewport")`;
  paint handler draws the engine texture. Camera moves = engine
  re-render + `request_custom_repaint` — NO View rebuild, NO reconcile,
  NO restyle/layout. The find_element_rect mid-build measuring (the P4
  crash trigger) disappears: the paint handler receives the rect.
- **Picking**: threepp Raycaster (same as viewport.js), replacing the
  hand-projected pick_faces_/pick_lines_ tables.
- **Nav gizmo** (72×72, camera-dependent): second canvas or drawn by
  the same handler — must also leave the reconcile path.
- Headless AFFINEUI_DUMP_HTML: viewport region renders empty (as does
  the web demo's canvas in a static dump) — the SVG generator and its
  Fnv fingerprint memoization retire entirely.

Staging: (1) feasibility probe — threepp FetchContent under
MSVC/ninja + hidden-context offscreen render + readback PNG, outside
the repo; (2) core dynamic-image API + canvas block wiring (camera →
repaint only); (3) threepp scene port of viewport.js (meshes, grid,
helpers, matcap/flat shading, outlines) + Raycaster picking;
(4) profile gate: camera change ≤ engine-render + repaint-rect cost,
0 View patches, 0 restyle roots.

## T7 — Style-resolution scaling: rule indexing + style sharing
Filed 2026-07-03 (user: 250 ms for a DENDER-scale settle "is billions
of CPU operations — doesn't make sense"). The arithmetic confirms an
algorithmic hole, not honest work: ~4k elements × full decius bundle
(thousands of rules) with no rule index = tens of millions of selector
evaluations per full rematch/restyle, and the resolver cache is
frequently cold (per-op clears pre-batching; settle paths). This layer
caps everything above it: structural settle ~250 ms, legacy load_view
~190 ms (parse is ~5 ms, cascade is the rest), reveal probes 78 ms/op
pre-batching — all the same root. Standard fixes, in order of value:
(1) **rule index** — bucket rules by rightmost compound (class/id/tag)
so each element tests only plausibly-matching rules (browsers' core
trick; typically 100–1000× fewer evaluations); (2) **ancestor bloom
filter** for fast descendant-combinator rejection; (3) **style
sharing** — identical siblings (same tag/classes/attrs, no pseudo
divergence) share one resolved style (huge on menus/lists/trees);
(4) keep the resolver cache WARM across batches (invalidate scoped,
never blanket-clear). Budget: full-document restyle ≤ 10 ms at 5k
nodes; settle ≤ 15 ms. This may live partly in the lexbor fork
([[affineui_lexbor]] patches) — cascade internals are theirs.

## T6 — Layout blowup: flex-column + overflow:auto inside anchored absolute panel
Found 2026-07-03 during the PhotoEdit sample rebuild. A
`flex-direction:column` body with `overflow:auto` inside an absolutely
positioned panel anchored with both `top` and `bottom` makes layout
explode: content height resolves to ~2^30 px (observed 1059922332),
likely a Yoga overflow/undefined-constraint path. Reproduced and
bisected headlessly. The sample dodges it with an equivalent
block-body + `height:100%` child — see the NOTE at
`bindings/python/examples/photo_edit_app/styles.py:78` (delete that
comment once fixed). Needs a minimal repro in tests/ + fix in the
affineui_yoga fork (preserve as a fork patch per the vendoring
convention).
