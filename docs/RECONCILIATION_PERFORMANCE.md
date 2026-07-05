# Reconciliation & Update Performance — Full Knowledge Dump

Handoff document for the agent taking over reconciliation/update
optimization. Everything below was learned by measurement on real apps
(chiefly the Affine 2600 synth, 2026-07-03 session) using the tools in
[TRACING_AND_PERFORMANCE_LOGGING.md](TRACING_AND_PERFORMANCE_LOGGING.md).
It contains: the architecture as it actually is, every measured number,
every root cause found and fixed (with mechanism), every open cost with a
concrete design sketch, and the traps — places where "obvious" fixes were
tried and were wrong.

TL;DR: **the View diff core is measured sound; do not rewrite it.** The
serious update problems come from (1) apps updating through the legacy
reparse path, (2) per-mutation restyle machinery downstream of the sink,
(3) whole-document settles for structural batches, and (4) app patterns
(raw-HTML re-emission, per-frame reconcile of animated geometry) that the
framework now has better tools for.

---

## 1. Anatomy of an update (what actually happens)

### 1.1 The fast path — `App::set_view(builder)` / `App::rebuild_view()`

```
rebuild_view()
  ├─ view.begin(sink)                sink = Document::begin_view_mutations()
  │    └─ suppresses lexbor ev_insert for the batch
  ├─ builder(view)                   app re-declares the whole tree
  │    └─ View diffs against retained WidgetNode tree
  │         └─ emits ONLY differences as sink calls:
  │              create_element / create_text / create_raw_html / remove
  │              set_text / set_raw_html / set_attribute / remove_attribute
  │                   └─ DocumentViewSink applies to the lexbor DOM
  │                        attr/text → live-mutation classification
  │                        create/remove/raw → view_structure_dirty = true
  ├─ view.end()                      tail-truncate removals
  └─ end_view_mutations()
       └─ if view_structure_dirty: dock_structure_changed()
            = whole-doc rematch + restyle + box recollect (+ layout next
              frame via content_size=0)                       [~56 ms @ 800 blocks]
```

Bootstrap (first `set_view`, or after `set_stylesheet`): build with a null
sink → `load_html(view.to_html_shell())` (head/styles/body attrs + empty
`<main>`) → replay the whole built tree through the sink
(`detail::replay_view_node`) → one structural settle. **No parse of app
content ever.**

Then per frame (renderer.cpp `prepare_frame`):

```
layout if content_size().width == 0        (the canonical needs-layout bit)
take_dirty_rects / take_paint_dirty
if anything changed → re-record display list (doc.draw into builder)
     → hash/diff vs cached list → raster changed region → composite
```

### 1.2 The legacy path — `App::load_view(view)`

`view.to_html_document()` → `load_html(...)` → **full lexbor parse, full
stylesheet re-attach (the whole framework bundle re-parses), full style
resolve, full box collect, full layout.** Hundreds of ms per call at
framework-CSS scale. Fine for a one-shot load; catastrophic as an update
mechanism. **This path was the synth's original "1 fps" — nothing else.**

**Call-site census (2026-07-03):**

| App | Pipeline |
| --- | --- |
| 17_affine_2600 | fast path only (reference implementation) |
| 16_decius_dender | **mixed** — fast path in places, `load_view` at dender_app.cpp:303/312/380/392 |
| 11_decius_game_editor | `load_view` per update (game_editor.cpp:194; interactive_tests.cpp:231 reload) |
| 15_command_panel | `load_view` (main.cpp:242) |

Migrating these is the highest-leverage fix available. Checklist per app:

1. Replace `reload() { app.load_view(build_view()); }` with a one-time
   `app.set_view([&](View& v){ build_view(v); })` and per-update
   `app.rebuild_view()`.
2. Call `app.set_stylesheet(...)` **before** the first `set_view`.
   (`set_stylesheet` re-parses the retained document from stored HTML —
   for a reconciled doc that's the empty shell; App now self-heals by
   re-bootstrapping, but the order avoids a wasted bootstrap.)
3. Builders must re-declare the FULL tree each rebuild (imm-style); the
   diff pays only for what changed.
4. Keys: give list items / conditionally-present subtrees stable keys.
   The differ keys on (parent, kind, source_location, key) — same call
   site with same key = same identity. Unstable keys = remove/create
   churn = structural batches = §3.7.
5. Audit for `set_attribute_by_id`-style DOM pokes and remove them; state
   flows through the builder. Per-frame *geometry* goes to the canvas
   (§5.3), not the builder.

### 1.3 The escape hatch — custom paint (canvas)

`View::canvas(name)` emits `<div data-aui-paint="name">`;
`App::set_custom_paint(name, fn)` registers a handler invoked during that
block's paint with the active `Painter` + border-box rect;
`App::request_custom_repaint(name)` = dirty rect + paint_dirty only —
**no restyle, no layout, no reconcile**. Because the display list records
by replaying document paint through a Painter, handler output becomes
ordinary hashed/diffed ops. Use it for anything whose *geometry* changes
per frame (cables, meters, scopes, drag previews). Reconcile is for state;
canvas is for geometry. Cable dragging via canvas: 7.7 ms/frame vs 23.8
via DOM mutations — and the DOM version also fought every problem in §3.

## 2. Measured baselines — the numbers to protect

Run: `affineui_tests.exe -tc="reconcile bench" -s` (RelWithDebInfo).
280-node tree:

| Scenario | Result |
| --- | --- |
| No-change rebuild | **81 µs, 0 sink ops**, ~439 temp allocs |
| 18-value change | 83 µs, **exactly 36 sink ops** |
| Fresh build + to_html | 283 µs, ~1863 allocs |

Real-app (synth, ~800 blocks, 1180×790 @1.25 dpi, decius bundle):

| Operation | Cost |
| --- | --- |
| Attr-only batch op (class write, post-fixes) | sub-ms (0.3–1 ms) |
| Sequencer step (~100 attr ops + settle) | ~2–3 ms total frame work |
| Structural batch settle (`dock_structure_changed`) | ~56 ms |
| Full frame, playing | 6.6 ms (150 fps) |
| Full frame, cable drag (canvas) | 7.7 ms (129 fps) |
| Legacy `load_view` update | ~1000+ ms (the original 1 fps) |

Any core change should re-run the bench and update this table. The
per-node cost is ~290 ns including builder temporaries; ~1.6 heap
allocs/node of *temporaries* (§4.4).

## 3. Root causes found & fixed (mechanism + location)

These are all in the working tree as of 2026-07-03. **Verify they're
present before re-diagnosing** — several produce identical symptoms to
"reconciliation is broken."

### 3.1 Generated-content recollect on every class write (~33 ms each)
Symptom: `[attr] set 'class' took 33 ms` × ~117/step → 4 s/frame.
Mechanism: `generated_content_depends_on_attribute` treated any class
write on any element that *could* match a `::before/::after` rule
(modulo the attribute) as "generated content may change" →
`recollect_blocks_from_current_dom` (full box rebuild). With a framework
bundle full of class-gated pseudo rules, that's essentially every class
write. Fix: the check now takes old/new attribute values; for `class` a
rule counts only if one of the *changed tokens* appears in its compounds
(`compound_generated_dependency_toggled`). History: at HEAD the check had
no element filter at all (recollected even more, but also masked 3.3);
the element filter was added for menu-lag; token precision completes it.

### 3.2 Whole-stylesheet dependency scan per mutation
`stylesheet_dependencies_stay_in_mutated_subtree` walked every rule of
every sheet per selector-affecting attr write to decide subtree-local vs
parent-escalated rematch. Result depends only on (sheets, attr name) —
now cached in `DocumentImpl::attr_subtree_local_cache` (mutable map;
cleared at both `attach_stylesheet` sites and document teardown).

### 3.3 Restyle losing inherited CSS variables under anonymous boxes
Symptom: after a live `aria-checked` flip, the checkbox subtree painted
unstyled (no background, icon black) — looked exactly like "reconcile
dropped my styles." Mechanism: `parent_resolved` walked the *block* tree;
anonymous/synthetic blocks carry no `custom_props`, so a subtree restyle
under an inline run resolved every `var()` against an empty set, and the
first restyled element then *stored* its var-less custom_props, poisoning
its children. Collect never hit this because it resolves along the DOM
chain. Fix: `parent_resolved` hops past blocks with no element. Latent at
HEAD (aria writes always recollected there); exposed when recollects got
rarer. **Lesson: restyle and collect must resolve through identical
inheritance chains — audit any new restyle path against this.**

### 3.4 Inline `style` dead inside view batches
`begin_view_mutations` nulls lexbor's `ev_insert` for the batch — which
is also lexbor's *inline-style parser* (the attr-node insert event only
does work for `style`). Elements that acquire `style="` inside a batch
never got declarations parsed → e.g. the cable overlay's
`position:absolute` ignored, wrapper laid out as a flex sliver. Fix:
`parse_inline_style_attr` / `parse_inline_styles_deep` called from
`DocumentViewSink::create_element`, `set_attribute` (fresh style attr
only — `ev_set_value` still covers value changes on existing attrs), and
`set_raw_html` fragments.

### 3.5 Renderer relayouted every frame under sustained repaint
renderer.cpp `prepare_frame` used
`layout_dirty = content_size().width != viewport_w`. Any content
legitimately 1px wider than the viewport → **full layout every rendered
frame** (9 ms/frame at synth scale), visible only under continuous
repaints (canvas animation, drags) because idle frames short-circuit.
Layout thrash also oscillated canvas bounds → cache invalidation storms
downstream. Fix: `layout_dirty = content_size().width == 0` — the
document's canonical needs-layout signal (same bit `mark_live_mutation_dirty`
sets and dispatch's `ensure_interaction_layout` reads). Viewport/dpi
changes are separately handled by `viewport_changed`.

### 3.6 Geometry queries returned zeros after structural batches
`find_element_rect` read block bounds directly; after a structural settle
(recollect resets `content_size` and zeroes bounds until the next layout)
every rect was 0×0, so anything computing overlay geometry from element
rects silently produced nothing (cables never appeared). Fix:
`find_element_rect` now runs the same hidden painterless relayout as
`dispatch()` when `content_size().width == 0` (the user explicitly
blessed hidden relayout; reparse must stay manual). It's logically-const
lazy evaluation of retained state.

### 3.7 Stylesheet swap wiped the reconciled document
`Document::set_user_stylesheet` re-parses from `impl_->html` — which for
a reconciled document is the empty bootstrap shell; calling it after
`set_view` erased everything (looked like the reconciler ate the DOM).
`App::set_stylesheet` now re-bootstraps the view when one is live; apps
should still order stylesheet-then-view.

### 3.8 Unnamed widget refs were write-only no-ops
`View::ref_for_node` returned an empty `WidgetRef` for keyless nodes, so
builder chains like `hw_label(v,...).attr(...)` silently did nothing
(missing titles/labels everywhere — again looked like diff breakage).
Now every node gets a live id-addressed ref; `find_widget("")` explicitly
returns empty (empty name ≠ wildcard). Contract is pinned by the
"keyless widgets are write-only declarations" test.

### 3.9a FIXED 2026-07-04: generated-content matcher heap churn
`element_matches_compound` materialized tag/id strings, a class-token
vector, and a vector of ALL attributes as string pairs — per rule ×
element, inside every `collect_blocks`/recollect. Profiler (sampler +
`tools/symprof`): ~15% of ALL CPU during knob drags was these
temporaries (`operator new` under `element_attrs`/`split_classes`).
Now matches directly against lexbor storage via `attr_view`/`tag_view`/
`class_tokens_contain` (document.cpp) — recollect 35–43 ms → ~13 ms on
the synth. Every structural settle, menu reveal, and startup benefits.

### 3.9b FIXED 2026-07-04: blockless view-built `<svg>` = permanent "reveal"
The hidden-subtree reveal detectors skipped SVG-NAMESPACE children, but
view/sink-built svg (knob rings/arcs, LCD digits) is HTML-namespace.
Such svg never collects boxes (it paints via `paint_direct_child_svgs`),
so the detector saw a "visible element with no block" forever → EVERY
attr write near such a widget escalated to a full recollect. Measured:
**42 full recollects in one knob drag** (each 13–43 ms — the "knobs are
huge perf drops" report). Fixed with a `tag_view(child) == "svg"` skip
in all three detectors → 2 per drag (the genuine first-touch). Trace:
`AFFINEUI_MENU_TRACE=1` now prints `[reveal] blockless child <tag>` when
a reveal recollect fires — if that line repeats during interaction,
something in this class is back.

### 3.9c FIXED 2026-07-04: knobs are painter-drawn (no on-the-fly SVG)
User decree: "SVG → static, paint → dynamic." The core knob's ring +
value arc + indicator were an inline `<svg>` whose `d`/`--angle` were
rebuilt as strings and written to the DOM on every move, then reparsed
by the SVG painter each frame. Now the engine paints them from
`data-min/max/value` + `data-bipolar` at block-paint time — same UA-chrome
tier as checkbox/radio/switch (document.cpp `Document::draw`, arc via
`append_arc_cubics` → `stroke_path`). A knob move is now ONE `data-value`
write (not selector-affecting in practice) + the value-label text; the
View builder emits no knob SVG. `update_live_control_value`'s knob branch
shrank to the value label. Result: 0 recollects, 0 SVG path-string
build/parse per move.

### 3.9d FIXED 2026-07-04: SVG parse-once cache (static art)
`render_svg_element_tree` reparsed every `d`/shape/gradient string on
every paint — and because the renderer full-layer-rerasters on any DL
change, a knob drag reparsed ~500 path strings/frame (jack sockets, LCD
digits, chevrons). Fix: the SVG path parse now produces LOCAL (viewBox)
coordinates, cached per element on `DocumentImpl::svg_path_cache` keyed
on element pointer + a hash of the geometry attributes; the per-paint
work is `transform_local_cmds` (a matrix-multiply per point, no parsing).
A geometry-attr write busts the entry via hash mismatch, so it stays
correct through mutation with no explicit invalidation. Cleared on
document replace; 20k-entry leak backstop. **Profiler after both fixes:
95.3% idle present-wait during a triple-knob drag; `build_svg_path_data`
and the generated-content matcher heap churn are GONE from the hot
samples.** The remaining `raster` cost is GPU full-window rasterization
(`nvg__tesselateBezier`), which only the composited-layer / partial-raster
work (§4) shrinks — a per-path tessellation cache inside the painter is
the next lever if raster stays hot.

### 3.9 Retained hidden blocks (context, not a bug)
Collect *keeps* `display:none` subtrees as blocks (Yoga gets DisplayNone;
`Document::draw` has a none-subtree suppression prepass) so menu/popover
reveals are pure restyles in both directions — previously every reveal
was a triple full box rebuild (~50 ms/hop, dropped menubar sweeps). Any
reconcile work must preserve this: **hiding/revealing must not rebuild
boxes.** Corollary: block counts include hidden subtrees; don't "optimize"
by skipping them at collect.

## 4. Open costs, ranked, with designs

### 4.1 Batch-deferred restyle (biggest designed win)
Today each selector-affecting attr write inside a batch pays:
block-index lookup (linear scan) + scoped lexbor rematch +
`resolver->clear()` (**wipes the whole style cache**) + subtree restyle +
reveal check + dirty-rect math ≈ 0.3–1 ms. A ~100-op batch = tens of ms
and N full cache wipes.

Design: while `view_batch_active`, `DocumentViewSink::set_attribute` /
`remove_attribute` for selector-affecting names should only (a) write the
raw lxb attr (keeping 3.4's inline-style handling), (b) refresh block
metadata, (c) accumulate a dirty-root set (same escalation rule as today:
target block, or parent when deps escape the subtree — the cached scan
from 3.2 answers that). `end_view_mutations` then, when NOT structural
(structural settle supersedes): one rematch over the union of roots → one
`resolver->clear()` → one restyle pass over the roots → one reveal check
per root → dirty rects. Non-selector attrs (svg `d`, `style`) keep the
existing immediate cheap path — canvas/fader hot paths depend on it.

Interactions to respect: the generated-content check (3.1) needs old/new
values per write — evaluate it eagerly per-op (it's cheap now) and let a
positive result mark the batch structural, exactly as today. The reveal
check semantics ("mutation un-hid a subtree with no boxes") must run
after the deferred restyle, per root. Mid-batch geometry reads
(`find_element_rect`) may now see pre-restyle state — acceptable; the
canvas moved app geometry reads to paint time, and dispatch never runs
mid-batch.

### 4.2 Element→block index map
`block_index_for_exact_element` / `block_index_for_element_or_ancestor`
are linear scans over ~all blocks, run per mutation, per hidden-relayout
check, per `find_element_rect`. Design: `unordered_map<lxb_dom_element_t*,
int>` rebuilt at collect (collect already touches every block), updated
on recollect; invalidate with the blocks vector. Makes every mutation
cheaper and `find_element_rect` O(walk-free). (App-side mitigation
already exists where it mattered: PatchBay caches jack centers.)

### 4.3 Scoped structural settle
`dock_structure_changed` = whole-document rematch + restyle + recollect
for ANY structural op. Correct and simple; ~56 ms at 800 blocks; will not
scale to DCC documents. Design direction: collect the affected subtree(s)
only and splice into the block vector. Hard parts (why it hasn't been
done): blocks are a DFS-ordered vector — splicing shifts indices, so
parent_idx links, hovered/active/focused indices, pending dirty roots,
and any cached indices need fixup or generation-checking; synthetic
inline-run blocks regenerate with their parent; paint order derives from
the vector. A generation counter on blocks + index-free handles
(ElementId already exists and is versioned) is the likely shape. Interim
mitigation that works today: **keep per-update batches attr/text-only**
(see 4.6/5.2 — most structural churn is app-side and avoidable).

### 4.4 Builder temporaries (T2 arena)
~1.6 allocs/node/rebuild of short-lived strings/vectors (attr strings,
class concatenation, key strings). Bounded, but at DCC scale (10k nodes ×
60 Hz) it's real. Design: per-rebuild bump arena on the View for node
scratch; SSO-friendly key building; intern repeated class strings.
Target: **zero steady-state allocations for a no-change rebuild** —
the bench's alloc counter is the regression guard.

### 4.5 Keyed reorder support (real reconciler-core gap — the only one)
The differ is truncate-tail: children are matched positionally within a
parent; a *reorder* of keyed siblings produces remove+create churn (=
structural batch) rather than moves. Sample apps with sortable lists /
tab reorder will hit this. Design: within a parent, when positional match
fails, look up the incoming (kind, source_location, key) in the remaining
old children (the `widget_names_`-style map generalizes); emit a `move`
sink op (new; DocumentViewSink implements as lxb node re-insert, marks
structural — but a *move* settle could be far cheaper than
remove+create since styles/boxes can travel). Even without a cheaper
settle, preserving node identity keeps focus/scroll/interaction state.

### 4.6 Sink-op telemetry (do this first — it's diagnostic leverage)
Count ops by kind per batch + whether the batch went structural + settle
time; surface via `FrameTelemetry` (tracing doc R1/R4). Every recent
"reconciliation is broken" turned out to be *visible in one glance* at
this data. Cheap to add in `DocumentViewSink` + `end_view_mutations`.

## 5. Skeuomorphic apps specifically (user suspicion: confirmed)

Fewer widgets does NOT mean cheap updates — the skeuo kit concentrates
exactly the expensive patterns:

- **`lcd()` re-emits raw HTML whenever its value changes** — **FIXED
  2026-07-04** with direction (a): digits are now retained elements
  (`div > svg > 7 <path>`s, positional glyph keys), so a ticking display
  diffs to `fill` attribute writes on the flipped segments only.
  Measured on the synth's CLOCK RATE knob (which refreshes the tempo
  LCD per move via `bind_value_reload`): **was ~35–41 ms structural
  settle per move (knob felt like 26 fps mud), now ~2.1 ms attr settle
  steady-state**. The old text stands as the cautionary tale; canvas
  digits (direction b) remain the upgrade if LCDs ever animate per frame.
- **NEW FINDING (2026-07-04, for whoever owns §4.3): the scoped
  structural settle's recollect is still near-full-document cost.**
  `[batch] SCOPED structure roots=1 … recollect=35–43 ms` on the synth
  (~1600 blocks) — one root should not cost a whole-doc recollect.
  Related: the **first drag-touch of any knob pays ~40 ms once** —
  `[attr] set 'data-value' … reveal=40.7` — because the engine's live
  knob update creates the arc/indicator elements on first touch and the
  reveal path escalates to `recollect_blocks_from_current_dom`. Steady
  drag after that is clean. Scoping the reveal recollect (or pre-creating
  the arc) kills the hitch.
- **`led_meter`** is already element-based: value changes = ~20 class
  toggles + data-value per meter. Sub-ms each post-3.1/3.2, but a batch of
  meters × 60 Hz wants 4.1. If meters ever animate at frame rate, move
  them to canvas.
- **`lit_button`** class toggles: fine (attr-only).
- **`jack()` art swaps raw SVG on patched/unpatched** — structural, but
  only on topology changes (rare, user-initiated). Acceptable.
- **Cables/drag** — already canvas; zero reconcile involvement.
- The synth (`examples/17_affine_2600` + `extras/skeuo`) is the reference:
  150 fps playing / 129 fps dragging. A slow skeuo app should be diffed
  against its update pattern before touching core.

## 6. Diagnosis recipe (10 minutes, in order)

1. **Grep the app for `load_view`** in the update path → migrate (§1.2).
   This is the fix most of the time.
2. `AFFINEUI_PERF_OVERLAY=1`: check `layout` and `dl` per frame. Remember
   the fps number lies under stalls (it's frame-duration, not wall-clock
   gap) — if the app "runs 60 fps" but feels frozen, believe the feel.
3. `AFFINEUI_MENU_TRACE=1`, drive one update, read stderr:
   - `[attr] set 'class' took ~33 ms` → fixes 3.1/3.2 missing from build.
   - many sub-ms `[attr]` lines → §4.1 territory (batch-deferred restyle).
   - `[batch] structure-changed took N ms` **on every update** → the app
     emits structural ops per update: raw-HTML re-emission (LCD!),
     unstable keys, or conditional subtrees without keys. Fix the app
     pattern first; §4.3 second.
   - repeated `[rect] hidden relayout took N ms` → something reads
     geometry after every structural batch; expected once per batch, a
     storm means the batch pattern is wrong.
4. `AFFINEUI_LAYOUT_DUMP=1` if styling looks wrong post-mutation (3.3-class
   bugs): blocks present with sane bounds but wrong paint → restyle/vars;
   blocks missing → collect; bounds zero → layout ordering.
5. Only then consider core work, in this order: 4.6 (visibility) → 4.1 →
   4.2 → 4.5 → 4.3 → 4.4.

## 7. Traps — things that look wrong but aren't (don't re-fix)

- **Lexbor's per-attr events are style-only.** `ev_insert`/`ev_set_value`
  on attr nodes parse inline `style` and nothing else. A suppression
  guard around class/aria writes was built, measured to save nothing, and
  removed — don't re-add it. (Element-insert events DO cost; that's why
  batches suppress `ev_insert`, with 3.4's compensation.)
- **The hidden relayout in `find_element_rect`/`dispatch` is by design**
  (user decision: relayout may hide, reparse must not). Don't "clean it
  up" into an explicit-layout requirement.
- **Canvas repaints re-record the whole display list** (~2.5 ms at synth
  scale). This is currently fine and correct (ops re-hash, diff drives
  the raster region). The upgrade path is a separate composited layer for
  volatile surfaces (compositor work, T3) — not per-block DL caching,
  which was considered and adds diff complexity for less win.
- **`content_size == 0` is THE needs-layout contract** shared by
  document mutations, dispatch, `find_element_rect`, and now the
  renderer. Anything introducing a second convention will reintroduce
  3.5-class thrash.
- **`paint_dirty` is a sledgehammer** ("style state suspect — rebuild
  display list, full repaint"). Prefer dirty rects; `request_custom_repaint`
  shows the light-touch pattern.
- **Perf-budget tests flake under machine load** (parallel builds). A
  failing budget test on a busy box is not a regression signal by itself.
- The **HUD fps lies under stalls** (frame-duration based). Wall-clock
  frame gaps are tracing-doc R0; until then, trust wall time.

## 8. Invariants & tests that pin this behavior

- `reconcile bench` (tests/test_reconcile_bench.cpp): op-exactness +
  alloc counts — the §2 table.
- "View keyless widgets are write-only declarations": unnamed refs live,
  `find_widget("")` empty (3.8).
- "real Decius checkbox generated icon updates after live aria mutation" +
  "live Decius checkbox state survives ordinary viewport relayout":
  live-mutation restyle correctness incl. vars-under-anon (3.1/3.3).
- "menubar hover-switch dispatch stays under a frame budget": guards 3.9's
  reveal-without-rebuild.
- Suite: `.\build.ps1 test` — 429/429 green as of this dump. Keep it that
  way per change, not per milestone; three of today's fixes were found
  because tests pinned behavior that "harmless" perf work had broken.

## 9. Coordination

- Tickets T2 (arena), T3 (layout-lock/hidden-relayout audit + volatile
  layer), T4 (large-layout stress) in
  [PERF_RELIABILITY_TICKETS.md](PERF_RELIABILITY_TICKETS.md) — keep in sync.
- Instrumentation/data-plane work (frame telemetry, sink-op counters) has
  its own roadmap in the tracing doc; the affinetools agent consumes it.
- §4.1's implementation site (`DocumentViewSink::set_attribute`,
  `end_view_mutations`) is within ~200 lines of 3.1/3.2/3.4's code in
  document.cpp — read those fixes before restructuring, they encode
  correctness the hard way.
- The custom-paint canvas and the skeuo kit are the reference consumers of
  the batch API; `examples/17_affine_2600` is the app to keep at 150 fps
  as the canary while changing any of this.
