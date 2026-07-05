# Widget → DOM Reconciliation Architecture

Status: **DESIGN CONTRACT** — the implementation of T1 (and its follow-ups) is
built and reviewed against this document.
Revision 2, 2026-07-03: incorporates the resolutions of the 47-finding
adversarial review (4 lenses); the full ledger is Appendix A. Next gate:
the skeuomorphic-agent implementation review (Appendix B).
Source requirements: `docs/PERF_RELIABILITY_TICKETS.md` (T1 section; every block
marked "user, 2026-07-03" is a REQUIREMENT and is traced to a section here —
see the traceability table at the end of §1).
All `file:line` citations refer to the working tree **as of 2026-07-03**; a
parallel session is actively editing framework files, so line numbers may
drift — the cited identifiers are the anchor.

---

## 1. Goals, non-goals, and the mandate

From the standing mandate (PERF_RELIABILITY_TICKETS.md:3–9) and the T1 design
directives (PERF_RELIABILITY_TICKETS.md:51–63):

**Goals (normative):**

- G1 — **Always works.** A wrong DOM is never an acceptable outcome. Every
  optimization is a *provable* equivalence claim; when equivalence cannot be
  proven, the system falls to a slower always-correct path
  (PERF_RELIABILITY_TICKETS.md:60–62). The final rung is the existing
  `App::load_view` full rebootstrap (src/app/app.cpp:351–358), kept forever.
- G2 — **Do not reconcile what didn't change.** A no-change frame is one
  branch (the root's dirty flags); a clean rebuild emits **0** sink ops on
  real apps, not just benches (PERF_RELIABILITY_TICKETS.md:42–44).
- G3 — **What does reconcile is cheap.** Near-nanoseconds-per-node reconcile
  cost, low CPU and memory, **zero base-heap allocation churn** in steady
  state; chained-block temporary pools/arenas are the sanctioned allocation
  mechanism (PERF_RELIABILITY_TICKETS.md:62–63, 104–110; T2 at :238–244).
- G4 — **140 Hz on real apps.** ~7 ms total frame budget at max refresh;
  the reconcile stages must leave the renderer most of it. Acceptance
  fixtures: DENDER `--profile` (~4 ms/interaction target, §13) and
  affine_2600 cable drags (PERF_RELIABILITY_TICKETS.md:20–22).
- G5 — **Apps never hand-poke the DOM.** View + reconcile is the only
  app-facing mutation path (PERF_RELIABILITY_TICKETS.md:5–6). The *core
  interaction layer* is not an app: it mutates the real DOM under an explicit
  ownership contract (§9).

**Non-goals:**

- Not a second renderer. Stage 4 (dirty-rect renderer, mutation
  classification, idle short-circuit) exists and is out of scope; §8 defines
  only the stage 3→4 bridge.
- Not a new DOM. lexbor remains the retained document; the shadow structure
  is an emission/diff artifact, never something apps traverse.
- Not a general-purpose React clone: no portals, no context propagation, no
  async rendering. One thread, one document, synchronous frame phases (§10).

**Measured baseline this design must beat** (PERF_RELIABILITY_TICKETS.md:23–49,
re-measured 2026-07-03 via `decius_dender.exe --profile`,
examples/16_decius_dender/dender_app.cpp:332–468):

| Path | Cost on real 105 KB DENDER document |
|---|---|
| Legacy `App::load_view` (reparse) | ~190 ms/interaction (build 2.7 ms + load_view 187 ms) |
| In-flight `set_view`/`rebuild_view` steady state | **~64 s**/rebuild — even a *clean* rebuild |
| Diff probe (no-change rebuild) | 84 patches emitted (first build: 3927) |
| Implied document-sink cost | 64 s / 84 ops ≈ **775 ms per `set_attribute("class")`** |
| 280-node bench (tests/test_reconcile_bench.cpp) | 81 µs / 0 ops clean — failed to predict the above (§14) |

The two decomposed bugs (per-WRITE diffing instead of end-of-build
coalescing, §5.2; per-op full-document restyle instead of batch-end scoped
work, §8.2) are fixed *by construction* in this architecture, not patched.

**Requirement traceability** (ticket block → section):

| Ticket block (PERF_RELIABILITY_TICKETS.md) | Section here |
|---|---|
| Standing mandate :3–9 | §1 |
| Canonical 4-stage pipeline :206–223 | §2 |
| Dirty flags in the widget tree :140–183 | §3.4 |
| Render purity / non-reentrancy :162–177 | §3.6, §4.1 |
| Granularity — dynamic, not guessed :185–204 | §3, §4.2, §5.5 |
| Shadow DOM model / open storage trade :102–115, :225–236 | §4.3 |
| Design directive (versions, coalescing) :51–63 | §3.3, §5.2 |
| Incremental DOM hashing :80–100 | §6 |
| Visibility-scoped reconcile :65–78 | §7 |
| Reliability spine :116–138 | §10 |
| Field data / bugs :23–49 | §5.2, §8.2, §12 |
| Widget boundary definition :232–236 | §3.1 |
| Memory mandate :62–63, T2 :238–244 | §11 |

---

## 1a. Normative per-stage interaction budget (USER-SET, 2026-07-03)

The acceptance spec for one UI interaction (a click that updates a
panel), dictated per stage. These are HARD targets, not aspirations —
"any spike over our desired framerate is a bug":

| # | Stage | Budget | Meaning |
|---|-------|--------|---------|
| 1 | Update creates a subtree | **3–10 µs** | builder re-emits + reconcile splices the changed widget's subtree |
| 2 | Container div sizes changed in reconcile | **5 µs** | size/style attr diffs applied |
| 3 | Minimal style update (no styles changed) | **1–2 µs** | hash-gated: unchanged identity ⇒ NO rematch, NO resolve, NO cache clear |
| 4 | Minimal layout update (frames only) | **4 µs** | incremental layout: only dirty nodes recompute (retained layout tree) |
| 5 | Repaint, changed sections only | **≤ 0.1 ms** | partial raster of dirty rects; the largest single term |

Total: an interaction is a **~0.1 ms** event, paint-dominated. The
corollaries: most changes require NO style recomputation at all (§5/§6
two-level hashing is the gate); layout must be incremental (a retained
tree with dirty marking, not a per-pass rebuild); paint must be partial
(dirty-rect raster, not full-window re-record). Anything the pipeline
does that is not on this table (whole-document rematch, full recollect,
full relayout, full re-raster) is a scoping bug against this spec.

---

## 2. The four-stage pipeline

The organizing spine (verbatim requirement, PERF_RELIABILITY_TICKETS.md:206–223).
Each stage has its own **skip gate**; a frame in which nothing happened costs
one branch at stage 1 and the existing idle short-circuit at stage 4.

```
   app state change
        │
  ┌─────▼─────────────────────────────────────────────────────────┐
  │ S1 UPDATE      widget.invalidate(): SELF-DIRTY, version++,     │
  │                DESCENDANT-DIRTY up-walk (early-out)            │
  │   skip gate:   no state change → root flags clear → done       │
  ├────────────────────────────────────────────────────────────────┤
  │ S2 RENDER      flagged widgets re-emit THEIR OWN shadow dom    │
  │                into emit-only output; children = mount points  │
  │   skip gate:   SELF-DIRTY only; clean widgets retain shadow    │
  ├────────────────────────────────────────────────────────────────┤
  │ S3 RECONCILE   coalesced FINAL values diffed against real DOM  │
  │                inside one transactional sink window            │
  │   skip gate:   visibility → flags → versions → hashes (§5.4)   │
  ├────────────────────────────────────────────────────────────────┤
  │ S4 UI UPDATE   sink classification (paint/restyle/layout)      │
  │                drives small-rect renderer updates              │
  │   skip gate:   existing idle short-circuit + dirty rects       │
  └────────────────────────────────────────────────────────────────┘
```

**Stage 4 already exists** and is not redesigned here:

- Idle short-circuit: `cb_frame` returns before touching the renderer when
  `!dirty && !animations_active && !viewport_changed && settle_frames<=0`
  (src/app/app.cpp:724–729).
- Mutation classification: `set_attribute_on_element` classifies
  selector-affecting vs paint-only attributes, picks a scoped dirty root, and
  produces dirty rects via `mark_live_mutation_dirty`
  (src/dom/document.cpp:10025–10156).
- Paint-only fast paths: svg-child geometry, custom-paint repaint requests
  (src/dom/document.cpp:18513–18527).

What this design owes stage 4 is the **3→4 bridge contract** (§8): the sink
receives only final-value ops, classifies each, records a touched set, and
performs *one* scoped restyle + *one* recollect + at most one deferred layout
at the end of the mutation window — never per op.

**Frame order** (requirement, PERF_RELIABILITY_TICKETS.md:174–177):
`events → update/render → reconcile → paint`, each phase closed before the
next begins. Event dispatch never runs inside a render pass; render never
runs inside the sink window; the sink window is closed before
`Renderer::render_to` (src/app/app.cpp:772). Today's `cb_frame` already runs
frame callbacks before rendering (src/app/app.cpp:695–703); the update/render
and reconcile phases slot between event dispatch and `render_to`.

**Pipeline driver and scheduling (normative).** The engine owns scheduling;
apps never call a rebuild to make changes visible:

- `WidgetHandle::invalidate()` — and every coalesced `WidgetRef` write
  (§5.2.1) — marks the widget tree AND sets an App-level "view dirty"
  condition that wakes the frame. The S1–S3 phases run in `cb_frame`
  **before** the idle short-circuit (src/app/app.cpp:724–729); the stage-1
  gate (root flags clear) is the first check, so the do-nothing frame stays
  one branch. Placing the phases *after* the short-circuit would be a
  guessable-wrong implementation: an invalidate from a timer would set root
  flags and the UI would never update.
- A reconcile that applied any op (or produced dirty rects) sets the
  renderer-dirty condition the short-circuit consumes, so `render_to`
  actually runs; a 0-op reconcile leaves it untouched.
- `App::rebuild_view()` remains public but becomes sugar for invalidating
  the **root widget** (compat for existing samples and bindings);
  `set_view` still installs the builder and bootstraps.
- Invalidations from timers or other threads latch the same dirty condition;
  backends that sleep between frames must treat it as a wake source (same
  contract as animation wakes today).

---

## 3. The widget tree

### 3.1 What a widget boundary is (open decision → recommendation)

Today the samples render the whole app as **one root-level build function**
(`DenderView{*this}.build_into(v)`, examples/16_decius_dender/dender_app.cpp:299–300),
so the only "widget" is the entire document. The ticket requires a defined
boundary: *"a keyed subtree with a version and a render/build closure — the
SAME primitive the visibility-scoped-build item needs"*
(PERF_RELIABILITY_TICKETS.md:232–236).

**Options:**

1. **Keyed View scopes as-is.** Every `View::element(tag, classes, key)`
   scope (include/affineui/view.h:558–561) becomes a potential boundary.
   * Pro: zero new API. Con: no version, no closure to re-run in isolation
   (the builder is one imperative function — you cannot re-execute "just this
   scope"), no place to hang flags/hash/segment. Fails the deferred-build
   requirement of §7 outright.
2. **components.h wrappers grown up.** Typed components
   (include/affineui/components.h:51–79) gain versions and render closures.
   * Pro: user-facing surface already exists. Con: components are *views over
   a node* by design ("a typed component is just a view over a node",
   components.h:29–31); making them own render closures inverts their
   contract, and plain containers/panels (most of a real layout) are not
   components.
3. **An explicit `widget(key, state, render)` primitive.** A new View entry
   point that registers a keyed subtree with a render closure the *engine*
   can re-invoke independently.

**RECOMMENDED: option 3**, with option 2 layered on top (typed components may
*wrap* a widget exactly as they wrap a WidgetRef today). Rationale:

- It is the only option that supports **independent re-render**: stage 2
  requires re-running one widget's emission without re-running its parent's
  builder code. That needs a stored closure per boundary.
- It is the **same primitive visibility scoping needs** (§7): a hidden tab is
  a widget whose closure simply hasn't been invoked yet — the deferred
  builder closure and the widget render closure are one thing. Precedent
  in-tree: `set_dock_active_tab_provider` already emits inactive tab bodies
  as empty placeholders built on first reveal (include/affineui/view.h:803–807).
- It gives version stamps, dirty flags, hashes, node counts, and shadow
  segments a natural home (the widget slot, §3.2) without bloating every
  `WidgetNode` (include/affineui/view.h:306–322).

**API sketch (normative shape, names non-binding):**

```cpp
// In a build function or another widget's render:
v.widget("scene-panel", [this](WidgetOut& out) {   // render closure, §4.1
    /* emit THIS widget's dom; child widgets appear as out.mount(...) */
});

// From app code / event handlers (never inside render):
WidgetHandle h = v.widget_handle("scene-panel");
h.invalidate();                    // SELF-DIRTY + version++ + up-walk
h.invalidate_children();           // parent-controlled force-children (§4.2)
```

Granularity is **dynamic, not guessed** (PERF_RELIABILITY_TICKETS.md:185–204):
apps may declare one widget or hundreds; the engine coalesces/escalates at
run time (§5.5). Nothing in the engine assumes a fixed panel-sized segment —
that is the Qt lesson the ticket records.

Non-widget content (plain scopes between widget boundaries) belongs to the
nearest enclosing widget; the root build function is implicitly the **root
widget**, so a program with zero `widget()` calls degenerates to today's
whole-app-one-region behavior — correct, just coarse.

### 3.2 Widget state model

- **App-owned state.** Widget state lives in the application (documents,
  selection, camera…), not in the engine. The engine stores per-widget
  *bookkeeping* only, in a **widget slot** keyed by the widget's StableId:

  ```
  WidgetSlot {
      StableId        id;               // §5.1 identity (globally scoped key — survives reparenting)
      uint64_t        version;          // ++ on every invalidate (S1)
      uint64_t        applied_version;  // last version reconciled into DOM
      uint8_t         flags;            // SELF_DIRTY | SHADOW_DIRTY | DESC_DIRTY | HIDDEN | …
      RenderFn        render;           // the closure (re-bind rule §4.2; inline storage, no heap)
      ShadowSegment*  shadow;           // retained emission (§4.3)
      RefOverlay*     overlay;          // durable post-declaration ref writes (§5.2.1)
      uint32_t        node_count;       // cached, for escalation (§5.5)
      Hash128         subtree_hash;     // cached Merkle hash + dirty bit (§6)
      WidgetSlot*     parent; …children // the widget tree (sparse over the dom)
  }
  ```

  Slot decisions (normative):
  - **Versions are 64-bit.** A 32-bit counter invalidated per event at
    ~140 Hz wraps in ~355 days, and §6.4 explicitly reasons about apps
    running for months; a wrap to `applied_version` while hidden would
    silently skip the reveal reconcile. 8 extra bytes fit the ≤128 B slot
    budget (§11). Belt-and-suspenders: the reveal check also honors flags
    held at the boundary (§7), which cannot wrap.
  - **`RenderFn` is not `std::function`.** Closure storage is fixed-size
    inline (SBO with a compile-time capture-size assert) or a segment-arena
    thunk — re-binding a closure is a copy, never a base-heap allocation
    (a parent with 30 mounted children re-rendering at 140 Hz must not
    allocate 30 `std::function`s per frame against the §11 gates).
  - **Child slots are stored contiguously per parent**, so the stage-2
    fanout scan (§3.4) is cache-linear.
- **Snapshot at render.** The render closure captures what it reads; the
  engine passes it nothing mutable (§3.6). If an app wants engine-managed
  state cells (React-`useState`-like), that is a future sidecar — the core
  contract is only: *state changed ⇒ call `invalidate()`*.
- The widget tree is **sparse**: it parallels the shadow/DOM tree but has a
  node only at declared boundaries. Depth is small (≪ DOM depth), so up-walks
  are cheap.

### 3.3 Per-widget version stamps

Requirement (PERF_RELIABILITY_TICKETS.md:53–59): every mutation increments
the widget's `version`; a subtree whose `version == applied_version` is
skipped entirely — no per-node walk, no attr compares. `applied_version` is
written **only** when the sink window that applied the widget's delta commits
(§8.1); a widget that changed while hidden keeps `version != applied_version`
and therefore reconciles exactly once on reveal (§7). Versions are
writer-side and O(1); they compose with hashes, which are structural and
survive full rebuilds with no shared version lineage (§6).

**Atomicity invariant (normative).** Setting a dirty flag (SELF-DIRTY or
SHADOW-DIRTY, §5.2.1) and `version++` are ONE operation: every mutation
entry point — `invalidate()`, `invalidate_children()`, coalesced
`WidgetRef` writes, interaction-layer Mode-R commits (§9) — sets the flag
AND bumps the version together. No path does one without the other.
Corollary: **versions confirm skips, they never veto a flagged visit** — a
set flag always causes the visit regardless of version equality, and the
inconsistent state (flag set, `version == applied_version`) is a debug
assert (corruption signal), never a skip.

### 3.4 SELF-DIRTY / DESCENDANT-DIRTY flags

The concrete stage-1 mechanism — no searching, no side queues
(PERF_RELIABILITY_TICKETS.md:140–160):

- `invalidate()` sets SELF-DIRTY on the widget, `version++`, then walks UP
  the widget-parent chain setting DESCENDANT-DIRTY, **early-outing at the
  first ancestor that already has it**. Amortized near-O(1), worst case
  O(widget-tree depth).
- The stage-2 descent starts at the root and **follows flags only**:
  - neither flag → skip the whole subtree. The root's flags ARE the global
    "anything dirty?" check; the true do-nothing frame is one branch.
  - DESCENDANT-DIRTY only → descend only into flagged children.
  - SELF-DIRTY → this is a **region root**: run its render (§4). Multiple
    self-dirty widgets under one ancestor are discovered on the same descent;
    that is where the coalescing/escalation decision is made (§5.5).
  - SHADOW-DIRTY only (§5.2.1) → diff the widget's retained segment (with
    overlay applied) against the DOM **without** running the render closure.
- **Descent cost is O(fanout) per flagged spine node, accepted with an
  explicit bound:** DESCENDANT-DIRTY says "some child is dirty," not which,
  so each flagged spine node scans its widget children. Child slots are
  contiguous (§3.2) so the scan is cache-linear, and the T5 guardrail
  (virtualize above N unvirtualized children) bounds fanout in practice. If
  profiling shows wide flat widget trees paying for this, the specified
  upgrade is an intrusive dirty-child linked list per parent maintained by
  the up-walk (O(1) insert, true O(dirty·depth) descent) — an
  implementation swap, not a contract change.
- **Clearing:** the pass clears flags as it processes — but a widget's
  flags are cleared only **after** its render closure returns successfully
  (§8.1 failure semantics). The walk visits exactly the flagged spines, so
  clearing during the walk is complete; steady state returns to all-clean.
- **Clearing across hidden boundaries (both guesses lose without this
  rule):** ancestors' DESCENDANT-DIRTY IS cleared once every reachable
  flagged child is processed or **parked** at a hidden boundary — the
  boundary widget holds its own flags while hidden. (Not clearing would
  keep the whole app off the do-nothing fast path for as long as any
  hidden widget holds dirt.) Reveal re-marks the ancestor chain
  (invalidate-style up-walk from the boundary, §7) so the next descent can
  find it again. `invalidate()` on a widget under an already-parked hidden
  boundary early-outs at the boundary (its DESC-DIRTY persists), so hidden
  churn costs O(depth-to-boundary) once and ~O(1) after.
- **Stale-flag benignity** (requirement, PERF_RELIABILITY_TICKETS.md:178–180):
  a widget removed while dirty may leave DESCENDANT-DIRTY on ancestors. The
  next descent finds no flagged child and clears it — never a crash, never
  an error. Flags are hints for *where to look*, never proofs; skipping is
  justified by versions/hashes, and *extra* flags only cost a wasted descent.
- **Visibility boundary:** a hidden/obscured widget keeps its flags; the
  descent stops at the boundary and the pending dirt is processed on reveal
  (§7; PERF_RELIABILITY_TICKETS.md:181–183).

### 3.5 Non-reentrancy

**RENDER CALLS USER CODE, THEREFORE UPDATE IS NON-REENTRANT** (user,
emphatic; PERF_RELIABILITY_TICKETS.md:162–163). One update/render pass runs
at a time; `invalidate()` from *outside* the pass (event handlers, timers)
marks flags for the *next* frame. `invalidate()` reached from *inside* a
render is a blocked mutation (§3.6). Event dispatch never runs inside a
render pass (frame order, §2).

### 3.6 WHEN RENDERING, MUTATION IS BLOCKED

Impossible **by construction**, with one engine gate as defense-in-depth
(requirement, PERF_RELIABILITY_TICKETS.md:163–174):

1. **Type-system wall.** The render closure receives (a) only `const`
   views of widget state it captured, and (b) an **emit-only** output object
   (§4.1) whose entire surface is element/attr/text emission and child
   mounting. No `View&`, no `WidgetRef`, no handles, no state setters are in
   scope. You cannot express a mutation in a well-typed render body.
2. **Engine gate.** One gate condition — `in_render_ OR view_batch_active`
   — covering **both stage 2 and the open stage-3 sink window**, checked by
   **every mutation entry point**: `WidgetHandle::invalidate`,
   all `WidgetRef` mutators (which already route through
   `View::can_mutate_children` — precedent at src/app/view.cpp:4176–4183,
   which today blocks structural `WidgetRef` mutation during a build and
   logs `"Illegal WidgetRef::… during view generation"`), `App::rebuild_view`,
   and the Document's app-facing mutation APIs. A blocked attempt **does not
   take effect**: debug = assert; release = blocked + loud diagnostic
   (`View::diagnostics()` + stderr). Hard-to-crash: never corrupt the pass,
   never crash the app.

**Blocked-call observables (per API, so callers know exactly what
happened):**

- Blocked `invalidate()` is **not lost**: it latches a deferred re-mark
  that is applied when the pass closes — the change lands next frame. (An
  invalidate is idempotent intent; deferring it is always correct.)
- Blocked `WidgetRef` writes and state setters are **dropped** (debug =
  assert, release = no-op + diagnostic); value-returning APIs return their
  failure-indicating defaults. They are not deferred because a write's
  value may depend on state the pass is currently projecting — deferring
  would smuggle mutation into the pass by another name.
- **No user callbacks fire inside stage 3.** Removing elements inside the
  sink window can trigger destroy/focus-loss notifications (the lexbor
  fork's `event_destroy` patches, weak-callback notify): these are queued
  and dispatched in the **next event phase**, never synchronously inside
  the window.

**Handler registration is emission, not mutation.** Declaring an event
handler inside a render closure (`ElemScope::on_click(...)`, §4.1) is part
of declaring the widget — exactly like an attribute — and is therefore
legal under the gate. Handlers are stored per-widget in the slot (§4.1),
not in view-global vectors.

**Purity AND determinism (normative).** A render closure must be a
deterministic pure function of the state it reads: **same state ⇒
byte-identical emission.** It must not sample clocks, frame counters, RNG,
or cross-thread mutable state (affine_2600's audio engine position
included). Time-varying content flows through `invalidate()`d state — a
frame callback updates app state and invalidates; render never samples
time itself. This is load-bearing, not stylistic: the oracle re-runs
builds and compares hashes (§6.5), VERIFY_SKIPS re-renders "clean" widgets
(§10.1), and escalation equivalence (§5.5) all assume it. The type wall
cannot enforce determinism (a lambda can capture `this` and read a clock),
so violations are *detected*: the oracle diverges, and the ladder's
terminal nondeterminism classification (§10.4) names the widget instead of
looping.

Render is thereby a **pure projection**: widget state → shadow dom. Every
equivalence claim in this design — versions, hashes, the oracle — rests on
that purity; this is why the gate is architecture, not hygiene.

---

## 4. Render: emission into shadow storage

### 4.1 The emit-only output object

`WidgetOut` (name non-binding) is the *only* thing a render closure touches:

```cpp
class WidgetOut {                     // NO mutation of engine/app state.
public:
    ElemScope element(std::string_view tag, std::string_view classes = {},
                      std::string_view key = {});      // RAII like View::Scope
    void attr(std::string_view name, std::string_view value);
    void text(std::string_view value);
    void raw_html(std::string_view markup, std::string_view key = {});
    // Child MOUNT-POINT splicing: reference a child widget WITHOUT rendering
    // it. The child's retained shadow segment is spliced here; if the child
    // is dirty it renders on its own visit. `force` = the parent-controlled
    // invalidation switch (§4.2). Binding rule: §4.2.
    void mount(std::string_view child_key, WidgetRenderFn render,
               MountFlags flags = MountFlags::None /* | ForceChildren */);
};
// ElemScope subsumes the WidgetRef chaining surface AS EMISSION (§3.6):
//   scope.on_click(fn); scope.on_change(fn); scope.attr(...); ...
// Handler binding declares the widget's behavior, exactly like an attr.
```

Semantics:

- **Forward-only emission, "like serializing to a file"**
  (PERF_RELIABILITY_TICKETS.md:104–110). Emitted nodes and strings are
  bump-allocated into chained-block storage (§4.3, §11): stable addresses,
  no realloc invalidation, no base-heap churn. There is no "go back and edit
  an earlier element" API — a later write to the *open* element's attrs is
  legal (that's how builder helpers append class modifiers) and is coalesced
  at element close (§5.2), but closed elements are immutable.
- **Event handlers are per-widget slot state, not view-global vectors.**
  Today handler registration mutates View-level vectors
  (`click_handlers_`/`change_handlers_`, src/app/view.cpp:4091–4110) and
  `App::rebuild_view` harvests ALL bindings wholesale every rebuild
  (src/app/app.cpp:428–429) — under per-widget partial renders that would
  be an O(total-handlers) `std::function` vector copy per frame (base-heap
  churn, G3 violation) with no story for widgets that didn't render.
  Contract: handlers bound during a widget's render are stored in that
  widget's slot, replaced atomically when the widget re-renders, freed when
  it is removed; dispatch resolves target → owning slot → handler by
  StableId. The whole-view harvest is deleted.
- The convenience builders (`View::button`, `dock_panel`, `tree_row`, … —
  include/affineui/view.h:584 ff.) are re-expressed over `WidgetOut`
  emission. **Honest migration statement** (the earlier "app-visible
  signatures do not change" was overstated): builder *names and arguments*
  do not change, and `.on_click/.on_change/.attr` chaining compiles
  unchanged — but inside a render closure the returned handle is an
  emit-only `ElemScope`, not a live `WidgetRef`. Code that stores the
  return value and mutates it *later, outside the build* keeps working via
  the §5.2.1 durability contract. The per-pattern migration table lives in
  §13.

### 4.2 Containment default + parent-controlled force-children

Requirement (PERF_RELIABILITY_TICKETS.md:199–204, 210–214):

- **Default: rendering a widget does NOT re-render its children.** Child
  widgets are **stable mount points** inside the parent's emission; an
  untouched child's retained shadow subtree is *referenced*, not re-emitted.
  Containment is the default, not an opt-in memo.
- **Parent-controlled invalidation:** the parent KNOWS when its re-render
  invalidates its children (data flows down; structural change). It says so
  explicitly — `MountFlags::ForceChildren` on the mount, or
  `WidgetHandle::invalidate_children()` from update code — which sets
  SELF-DIRTY on each mounted child (and recurses only if those also force).
- **Mount closure binding (normative):** `mount()` **re-binds** the child
  slot's closure on *every* parent render that reaches the mount (the
  parameter is `render`, not `render_if_new` — "may be re-bound" is not a
  contract). A child render that runs between parent renders uses the
  last-bound closure. Consequently closures must read changing data through
  **stable references** (app model + child key), never by-value snapshots
  of data that changes independently of the parent: a stale by-value
  capture renders stale data when the child is invalidated in isolation,
  and the oracle/`AFFINEUI_VERIFY_SKIPS` report it as divergence rather
  than silently showing it. This convention is documented loudly in the
  widget() API docs. Re-bind is a copy into inline/arena storage (§3.2),
  never an allocation.
- A mount whose `child_key` did not exist last frame creates the child slot
  and renders it. A retained child not mounted this pass is **removed by
  default** (its segment freed, its DOM subtree removed via the sink, its
  slot and name registrations unregistered — the existing `unregister_tree`
  discipline, src/app/view.cpp:4152–4174).
- **Recycling override (normative — virtual lists always, any container by
  opt-in):** remove-on-unmount is the *default*, and a container marked
  recycling overrides it — otherwise every virtual-list scroll tick would
  unmount ~overscan widgets (segment free, DOM destroy, StableId map erase)
  and mount as many new ones at 140 Hz, guaranteed churn that contradicts
  T5's "zero create/remove churn while scrolling." A recycling container
  parks unmounted children in a **bounded per-container recycle pool**:
  slot + segment retained keyed by stable item id; DOM subtree
  detached-but-kept, or **rebound** to an incoming row (true recycling:
  same DOM nodes, new attrs/text via the ordinary diff). LRU cap; eviction
  applies the default free rule. Recycled-row rebinds are attr/text diffs
  on existing nodes and therefore do **not** set the §8.2 structural flag —
  scrolling must not trigger recollects.

### 4.3 THE STORAGE COMPARISON (open trade → recommendation)

The ticket's explicitly open trade (PERF_RELIABILITY_TICKETS.md:225–236):
*"it's not clear temporary shadow dom that you throw away beats a retained
shadow dom."* Three candidate designs, compared honestly:

| Axis | (A) Retained shadow tree, general-heap (today's persistent `View`: `WidgetNode` with `std::string`/`std::vector`, include/affineui/view.h:306–322) | (B) Temporary per-pass emission, thrown away after diff | (C) Retained **per-widget segments** in chained-block arenas |
|---|---|---|---|
| Memory between frames | Full tree resident; ~heavy (per-node vectors + N strings; heap metadata per allocation) | Near zero (only real DOM + slot headers) | Full tree resident but packed (bump-allocated, no per-string heap headers); measured bytes/widget budget §11 |
| Re-emit cost for a **clean** widget | Zero — but only with versions/flags; today's code re-*walks* everything (§12) | **Must re-emit everything every pass** to have something to diff — violates the shadow-retention requirement ("widgets not touched RETAIN their shadow shape — no re-render, no re-emit, no re-diff", PERF_RELIABILITY_TICKETS.md:111–115) unless hybridized back toward retention | Zero: the segment persists untouched; skip = pointer comparison |
| Re-emit cost for a **dirty** widget | In-place node reuse, but per-attr `std::string` compare/assign churn (src/app/view.cpp:4040–4052) | Fast bump emission | Fast bump emission of a fresh segment; old segment's blocks recycled O(1) |
| Pointer/id stability | Bad: `parent->children` is `std::vector<WidgetNode>` — growth reallocates and invalidates node pointers (why `WidgetRef` re-finds by id/name, src/app/view.cpp:4239–4253) | N/A (nothing persists) | Good: nodes never move within a segment; cross-segment references go through slot handles, never raw node pointers |
| Fragmentation | General-heap fragmentation over hours (soak risk) | None | Bounded: segments are whole chains of size-classed blocks (or single-region slabs) returned to a free list; a re-rendering widget cannot fragment *shared* storage because segments never interleave storage units — the no-interleave invariant holds at block **or slab** granularity (§11), which is what keeps the O(1) whole-chain free story while avoiding a per-segment 64 KiB floor |
| Interaction with hashing/versions | Hash/versions must live beside nodes (bloat) or in a side map (lookup cost) | Hashes must be recomputed every pass (defeats incremental hashing, §6) | Hash + version live in the slot header; per-node cached hashes live in the packed segment nodes — cheap and cache-friendly |
| Diff input | Diff happens *during* build (the per-write bug class, §5.2) | Natural end-of-build diff | Natural end-of-render diff: new segment vs previous segment |

**RECOMMENDATION: (C) — a retained shadow tree whose storage unit is the
per-widget segment, bump-allocated in chained blocks.** It is the only point
in the space that satisfies all three hard requirements simultaneously:
shadow retention for clean widgets (:111–115), forward-only chained-block
emission (:104–110), and no base-heap churn (:62–63). (A) is the current
code's position and is kept as the *semantic* model (the segment tree
behaves exactly like today's `WidgetNode` tree for diffing and `WidgetRef`
resolution); (B) is rejected as a whole-system answer but survives locally:
the *dirty* widget's new segment is exactly a "temporary shadow" until the
diff commits, after which it *becomes* the retained segment and the old one
is discarded — the trade is thus resolved per-widget, not globally.

Fallback position if (C)'s implementation risk bites: (A) plus versions/
flags/coalescing meets every *correctness* requirement and the DENDER
acceptance number; (C) is then the T2 memory-discipline milestone (§13
phase 4). This is recorded as an open question (§15.1).

---

## 5. Reconcile

### 5.1 Identity and keying

Current rules (kept, with additions):

- **StableId** = FNV-1a over (parent id, kind, key) — or (parent id, kind,
  source file/line/column) when no key is given
  (src/app/view.cpp:33–48). Identity is therefore stable across rebuilds for
  keyed nodes and for unkeyed nodes emitted from the same call site.
- **Keys vs positional:** today, matching is *positional with identity
  verification*: a child matches only if the node at the same index has the
  same id, kind, and tag (src/app/view.cpp:3961–3964); any mismatch
  truncates and recreates the entire remaining tail
  (src/app/view.cpp:3967–3973, and at close: 4020–4031, 1486–1497).
  **New requirement:** within a dirty region the diff performs **keyed
  matching** (§5.3) so reordering K of N children costs O(K) moves, not
  O(N) remove+creates. Outside dirty regions no matching runs at all.
- **Duplicate policy:** a duplicate key among siblings gets an index-mixed
  fallback id (`make_duplicate_stable_id`, src/app/view.cpp:50–52, applied
  at 3949–3956) plus a `"Duplicate widget id"` diagnostic
  (src/app/view.cpp:4138–4141). This stays: duplicates are *legal but
  positionally unstable* — they may churn on reorder, and the diagnostic is
  the contract that the app should fix its keys. The reconciler must remain
  correct (never crash, never alias two DOM nodes to one id) in their
  presence.
  **The same policy now covers unkeyed same-call-site siblings** — the more
  common case, which today escapes it: the duplicate scan is guarded by
  `if (!key.empty())` (src/app/view.cpp:3949–3956) and `remote_id` derives
  from the id (:3978), so `for (item : items) v.element("div")` aliases N
  elements to ONE StableId and ONE remote_id. Normative: the index-mixed
  fallback applies to **every** same-call-site sibling after the first
  (accepting positional instability, same as keyed duplicates), with the
  same diagnostic. The §5.3 old-id index and the sink's remote_id map
  **require unique-per-parent ids**; for any parent still containing
  duplicates the reconciler falls back to purely positional matching
  (never the index) — correct, unstable, diagnosed.
- **Dock-resolver determinism:** dockables may be declared in any order; the
  resolver produces a deterministic layout from explicit parent+side
  declarations (include/affineui/view.h:752–758), and a provided live layout
  replays deterministically (view.h:795–801). Keys for dock-emitted DOM
  derive from pane/panel ids, never from declaration order — required so
  dock DOM ids are stable across rebuilds regardless of code motion.
- **Widget slot identity is parent-INDEPENDENT (decided).** A widget's key
  is mandatory (no source-location fallback) and registers in a **global
  named scope** (the existing `widget_names_` discipline) — it is NOT mixed
  with the parent id chain. Rationale: element-level StableId mixes the
  parent id, so a parent-mixed widget id would change when a dock panel is
  dragged from the left container to the right, destroying the slot,
  segment, version lineage, and DOM subtree — losing focus/scroll/transient
  state on every dock rearrangement and defeating §5.3's
  move-preserves-state goal exactly where it matters. Instead the slot
  survives reparenting and reconcile emits a **cross-parent move** of its
  mounted DOM subtree (§5.3 step 4). Element-level StableIds *inside* a
  segment remain parent-mixed as today; only the widget boundary is
  globally keyed. Recreate-on-reparent is explicitly rejected.

### 5.2 END-OF-BUILD COALESCING — the diff's canonical input

**This fixes measured bug #1** (84 phantom patches,
PERF_RELIABILITY_TICKETS.md:36–44). Today `View::set_attr` emits to the sink
on **every write** whose value differs from the node's current in-memory
value (src/app/view.cpp:4040–4052, emission at :4051). The killer sequence,
per rebuild, forever:

1. `open_node` re-applies the declared base classes
   (src/app/view.cpp:4000) — differs from last build's *final* value
   (base + modifier) → patch #1;
2. the builder's later `.cls("…--has-sub")` write appends the modifier —
   differs from the intermediate → patch #2.

**Contract:** the sink never sees intermediate values. Attribute (and text)
writes during a build/render pass mutate only the shadow node; the diff and
its sink emission run at **segment (region) close, top-down** — never at
node close, never during the build. The parenthetical "(or node close)" in
an earlier draft hid a correctness choice: elements close children-first
(RAII scopes), so per-node-close emission would emit a child's
`create_element` before its parent's — a `RemotePatchQueue` consumer
(Python `View.begin(queue)`) would receive a patch referencing a
`parent_id` that does not exist yet, and `RemotePatch::CreateElement`
carries **no attributes** (src/app/view.cpp:1154–1167), so final attrs must
*follow* the create as separate patches — ordering is load-bearing, not
cosmetic. **Normative sink ordering:** creates are emitted parent-first,
each immediately followed by its final attrs/text, then its children; every
op is position-valid at emission time; a node created and removed within
one pass emits nothing. A §14 parity test replays a RemotePatchQueue into a
scratch client model and asserts it equals the shadow. The diff compares
each node's **final** attribute set against the previously applied set,
emitting per-attr deltas (set/remove) only for real end-state differences.
A clean rebuild emits **0 ops by construction** — there is no code path
that can emit an intermediate value.

This also covers:

- `open_node`'s unconditional class reset + framework attr adjustment
  (src/app/view.cpp:4000–4010) — becomes ordinary shadow writes, invisible
  to the sink unless the final value changed.
- **WidgetRef post-declaration writes** — see the durability contract,
  §5.2.1. Summary: ref writes coalesce into the shadow, set
  **SHADOW-DIRTY** (diff-only — *not* SELF-DIRTY), survive later
  re-renders via a per-widget overlay, and are oracle-visible.
- Selector/framework recipe writes (`set_selector`,
  src/app/view.cpp:4054–4071) — same rule, they are just writes.

### 5.2.1 WidgetRef post-declaration writes — the durability contract

The naive rule an earlier draft stated ("coalesce into the shadow and mark
the owning widget SELF-DIRTY") **self-destructs**: SELF-DIRTY re-runs the
render closure (§3.4), which regenerates the segment as a pure projection
of app state (§3.6) and silently discards any write whose value is not
derivable from that state — which is the entire point of the imperative
`WidgetRef` surface. And had the write survived, the fresh-build oracle
(§6.5) could never reproduce it and would report divergence forever. Yet
`ref.set_text/attr/cls` after declaration is the dominant pattern (~78
uses across 17 example files; the primary Python-binding mutation surface),
so it must be durable. (Also for the record: the premise that these writes
reach the document today via `mutation_sink_` describes dead code —
`set_mutation_sink` is declared, include/affineui/view.h:528, but never
called anywhere in the repo; outside a build, ref writes currently reach
no sink at all. Recorded in §12#15.)

**Normative semantics:**

1. A ref write outside render/build coalesces into the retained shadow
   node AND is recorded in the widget's **overlay** — a small per-widget
   table keyed by (element StableId, attr-or-text), arena-stored in the
   slot (§3.2).
2. It sets **SHADOW-DIRTY**: "the retained segment changed; diff it
   against the DOM *without* re-running the render closure." The up-walk
   and `version++` happen exactly as for SELF-DIRTY (§3.3 atomicity).
3. When the widget later re-renders for any reason (SELF-DIRTY), the
   closure produces the fresh emission, then the **overlay is re-applied
   on top before diffing** — ref writes survive re-renders. Precedence
   when both flags are set in one frame: render first, overlay replay
   second. One ordering, no races.
4. An overlay entry is **retired** when the closure's fresh emission
   itself declares the same (element, attr) with any value — re-declaring
   reclaims ownership (last declaration wins; a debug diagnostic notes the
   retirement so "why did my ref write disappear" is answerable).
5. The oracle's fresh-build side applies the same overlay after building
   (**fresh build + overlay replay**), so surviving ref writes are
   equivalence-visible, never a permanent divergence.
6. **Timing (decided; formerly an open question):** ref writes apply at
   the next reconcile, which the write itself schedules (§2 pipeline
   driver) — same-frame when written during event dispatch. No immediate
   per-write mini-window: one mutation path. If 140 Hz scheduling ever
   shows visible `on_change` feedback lag, an immediate coalesced
   mini-window is the recorded fallback (measured, not assumed).

**`WidgetRef::append/replace` (structural post-declaration mutation) are
redefined as widget sugar:** `append(build)` declares an **anonymous child
widget** whose render closure is the passed build lambda — slot-backed,
versioned, durable, oracle-visible: exactly the §3.1 primitive.
`replace(build)` re-binds that widget's closure and invalidates it.
Today's implementation is a **live fast-path correctness bug**, not just a
design gap: `build_children` runs with `sink_ = nullptr`
(src/app/view.cpp:4193–4196) while `open_node` emits creates only when a
sink is present (:3990), so appended children never reach any sink while
their attr writes would route to a sink that has never seen those elements
(§12#15).

### 5.3 The diff algorithm for a dirty region

Input: the region root's new emission (segment) and its previous applied
shadow. Per node, one pass, with precise cursor discipline (an implementer
must not have to guess — the earlier draft under-specified backward moves
and claimed-node handling):

1. **Keyed matching with a claimed set.** An in-order cursor over old
   children (the existing `WidgetNode::cursor` discipline,
   include/affineui/view.h:317–321) plus a **claimed bitmap** over them
   (frame-arena):
   - `new[i].id == old[cursor].id` and unclaimed → match; claim; advance
     both cursors.
   - Otherwise look the id up in a per-parent old-id index (built lazily,
     only for parents that mismatch — the common clean prefix pays
     nothing; built only for parents with unique-per-parent ids, §5.1).
     Found — **earlier or later**, unclaimed → emit **move** (real-DOM
     `insert_before`, preserving interaction/focus state on the moved
     subtree); claim it. A lookup hit does **not** advance the cursor, and
     the cursor **skips already-claimed entries** when it advances — this
     is what makes backward moves ([A,B]→[B,A]) come out right: the cursor
     skips the claimed A when it reaches it.
   - Not found → **create** (subtree emit at the current position).
   - Old ids never claimed by the end → **remove**.
   The algorithm is greedy, not LIS-minimal: extra moves are cost-only,
   never correctness. **Unkeyed and text nodes never enter the id index**;
   they match positionally within the gaps between keyed matches (a keyed
   element list with interleaved unkeyed label/text nodes is well-defined).
2. Matched nodes: compare tag/kind (mismatch ⇒ replace — EXCEPT a focused
   text control is never replaced for a value-only difference, §9), then
   final attrs (sorted-merge of old/new attr lists → per-attr set/remove,
   honoring gesture-held exclusions and class-token merge, §9), then text.
3. **Hash gate — cached-vs-cached only.** The descend-skip on
   `subtree_hash(new) == subtree_hash(applied)` applies only where **both
   sides hold cached hashes**: mount splices (retained child segments),
   retained segments compared across a covering re-render, raw-html
   fingerprints (§8.4). Freshly emitted children have no cached hash —
   computing one is a bottom-up O(N) canonical walk (sort attrs, apply
   exclusions, hash text) that cannot early-out, roughly doubling
   dirty-path cost in the dominant really-changed case (per-frame
   animation, drag feedback) for payoff only in the rare
   spurious-invalidate case. Fresh emissions therefore get a **direct
   structural compare with early-out**. New-segment hashes are computed
   lazily at/after commit (when they become the cached side for next time)
   or on oracle demand; release-mode hashing budget: **~zero hash work on
   frames where the oracle does not run**.
4. **Cross-parent widget moves:** a widget slot whose mount moved to a
   different parent (§5.1 parent-independent identity) emits one move of
   its mounted real-DOM subtree; its retained segment is spliced, not
   re-rendered.

**Insertion positioning is reference-node based, not index based
(normative).** The local sink positions every create/move as
`insert_before(real node of the next shadow sibling, else append)`. Index
arithmetic is wrong in any parent that also contains real-DOM children the
shadow never emitted (interaction-created nodes: open layers, tearoff
chrome — §9): each foreign sibling shifts every shadow index by one.
Reference-node insertion is immune by construction. `RemotePatch` keeps
`index` purely as the **wire serialization** for remote clients, whose
mirrored document contains no foreign siblings by definition; the local
`DocumentViewSink` must never position by index. Today
`DocumentViewSink::create_element` ignores `index` and always appends
(src/dom/document.cpp:18260–18287) — sound only under the truncate-tail
scheme; it gains insert-before positioning plus
`move(node, parent, before)`; `RemotePatch` gains a `Move` op for parity
(§12).

### 5.4 The skip lattice

Ordered cheapest-first; each layer is a *justified* claim (§10.1):

1. **Visibility** (§7): obscured/offscreen widget → not even built. Claim
   justified by: it cannot be seen; correctness deferred to reveal via
   versions.
2. **Dirty flags** (§3.4): no flag → no visit. Justification: flags are a
   conservative over-approximation — every mutation path sets them (the
   in-render gate §3.6 closes the back door), so no-flag ⇒ no mutation.
3. **Versions** (§3.3): `version == applied_version` **and flags clear** →
   skip subtree without walking. Justification: writer-side counter,
   incremented atomically with the flags by the only mutation entry points.
   Versions **confirm** a skip that the flag layer already permits; they
   never veto a flagged visit (§3.3 — flag set + version equal is a
   debug-asserted corruption state, not a skip).
4. **Hashes** (§6): `subtree_hash(new) == subtree_hash(applied)` → skip
   even though versions differ (e.g. a rebuild wrote identical values, or a
   full re-render rung produced an equivalent tree). Fast-path on top of
   identity/version checks, with the collision policy of §6.4.

### 5.5 Escalation cost model (coalescing dirty widgets)

Requirement: dirty-rect merging on the tree, with a *specified, measurable*
knob (PERF_RELIABILITY_TICKETS.md:195–198). When the stage-2 descent finds
K SELF-DIRTY widgets under one ancestor A, it may either render each
fine-grained region, or escalate to rendering A once (with ForceChildren).

**Heuristic (normative — escalation must be COST-MONOTONE: it may never
make a frame more expensive than the fine renders it replaces):**

- **Necessary condition** to escalate to ancestor A:

  ```
  node_count(A)  <=  C_esc * sum(node_count(d) for d in dirty_under_A)
  ```

  with `C_esc ≈ 2` (tunable). Escalation is a bet that one covering render
  is cheaper; this bound caps the worst case at a small constant factor,
  never O(document). A bare `K >= K_max` trigger is **forbidden**: 17
  scattered one-node widgets under the root (status cells, VU meters, the
  affine_2600 sequencer-playback LEDs — the acceptance fixture itself)
  must render as 17 fine regions, not escalate to a root ForceChildren
  full re-render every frame.
- **Sufficient condition:** `sum(node_count(dirty_under_A)) >=
  rho * node_count(A)` (initial `rho = 0.5`) — the dirty set already
  covers most of A.
- `K >= K_max` (initial 16) only triggers a **search** for the cheapest
  covering ancestor satisfying the necessary condition; if none exists,
  the K fine renders simply proceed. They already share one sink window,
  so the per-region fixed overhead is one segment header + one diff
  context — the §14 bench records that overhead so `K_max` can be
  justified with a number or deleted.
- **Hysteresis:** escalate at `K_hi`, de-escalate at `K_lo < K_hi`
  (initial 16/8), so a workload oscillating around the boundary does not
  alternate fine/covering frames (which would free and re-emit every child
  segment on alternate frames — block churn plus frame-pacing jitter).
- **Recursion guard:** escalation decisions at deeper ancestors feed the
  counts their parents see, which can amplify level by level (up to 2× per
  level in a chain of containers each ~2× their child). The necessary
  condition is therefore always evaluated against the **original dirty
  set's** node sum, not escalated intermediate sums — total amplification
  is capped at `C_esc`.
- `node_count` is the cached per-segment count (§3.2, maintained at each
  render — no walk). For **never-rendered** widgets (hidden/deferred) it
  is unknown, not zero: they are excluded from sums, and an ancestor
  containing one is ineligible for escalation (a placeholder guess would
  feed garbage into the formula; reveal bursts render fine-grained once
  and are measured thereafter).

Constants are tunable and **must be measured**: the bench fixture (§14)
sweeps K and subtree sizes and records the crossover where one covering
render beats K fine renders (emission cost is ~linear in node count, so
the model is a two-parameter fit; the tickets' 81 µs/280 nodes and
DENDER's 2.7 ms/~3900 nodes give the initial slope) — and includes the
scattered-17×1-node case asserting the root is NOT rendered.

**Expected frequency (user directive, 2026-07-03): large covering
escalations should be VERY RARE** — the legitimate case is "every panel
changed at once" (theme switch, workspace switch, document load). And
even then, an over-broad re-render must not amplify downstream cost:
re-emission and re-diff are the only prices paid, because coalesced
final-value diffing plus the §8.2 same-value apply gate guarantee the
document and renderer see only the true deltas (the DIRT-NEVER-AMPLIFIES
invariant). Escalation trades emission cost against per-region overhead;
it never trades away downstream cleanliness.
Escalation never changes semantics — rendering A with ForceChildren
produces the same final DOM as K fine renders (purity + determinism,
§3.6); it is purely a cost decision, which is what makes it safe to get
"wrong" in cost, never in output.

---

## 6. Incremental DOM hashing

Requirement block: PERF_RELIABILITY_TICKETS.md:80–100.

### 6.1 Merkle subtree hashes

Every shadow node carries `subtree_hash = H(own_content, child_hashes…)` and
a cached `own_hash`. Comparing two subtrees (or two document roots) is one
128-bit compare — identical or different, no walk.

### 6.2 Canonical hash input

`own_hash` = H over, in order:

1. node type tag (element / text / raw-html);
2. lowercase tag name;
3. attributes as `(name, value)` pairs **sorted by name** (attr order is not
   semantic), after applying the **exclusion list** below;
4. text content (text nodes; element inline text);
5. for raw-html nodes: the raw markup string (this *is* the fingerprint —
   the sink re-parses only when it changes, §8.4).

`subtree_hash` = H(own_hash, ordered child `subtree_hash`es). **Child order
is semantic** and is hashed via order, never via commutative mixing.

**EXCLUSIONS** (not hashed, not diffed — kept deliberately small, because
every exclusion widens the "reconcile won't fix it" surface):

- **Transient user state:** focus, scroll offsets, caret/selection — none of
  these live in DOM attributes in AffineUI (they are Document-side state,
  cf. `TransientState` open-layer capture,
  include/affineui/document.h:328–341), but any future attr-reflected form
  is excluded by rule.
- **Gesture-held attributes only** (§9 — ownership is TEMPORAL, not a
  static list): an attribute is excluded from hash and diff **only while a
  live gesture holds its element** — e.g. inline `style` `flex-basis` on a
  dock pane *during* a splitter drag (src/dom/document.cpp:10666–10679),
  inline left/top on a float *during* its drag, drag-scrub
  `data-value`/`aria-valuenow` *before* commit. Outside a gesture the same
  attribute is ordinary Mode-R state: emitted by the build from providers,
  diffed, hashed. In particular **settled dock sizes and `hidden` on tab
  bodies are Mode R and ARE hashed** — an earlier draft statically excluded
  them here, flatly contradicting §9's Mode-R classification; a static
  exclusion would make programmatic pane-size changes and workspace
  restores silently unreconcilable (wrong DOM, unhealable by the ladder).
- **Interaction-owned class TOKENS** (§9): `class` is hashed as the
  **declared token set** — tokens matching the interaction-owned patterns
  (`dcs-*--active`, `dcs-*--drop-*`, …) are stripped by the canonicalizer,
  mirroring the diff's token-merge rule, so a menu being open or a row
  being drop-highlighted never perturbs hashes.
- **Bookkeeping attributes:** engine-internal markers that cannot affect
  rendering semantics (e.g. transient trace/debug attrs). `data-aui-name`
  IS hashed (it is selector-visible and app-declared), and so is
  `aria-expanded` (selector-visible, cf. the :9190 comment in
  document.cpp); truly inert bookkeeping must be nominated explicitly to
  the exclusion list — default is *included*.

### 6.3 Dirty-up-chain invalidation + lazy recompute

- A shadow mutation marks the node's hash DIRTY and walks up the ancestor
  chain marking hash-dirty, **early-out at an already-dirty ancestor**
  (identical shape to the flag up-walk, §3.4 — the two walks are one walk).
- Recompute is **incremental and lazy**: on demand (a skip check or the
  oracle), a dirty node recombines its cached `own_hash` (recomputed only if
  its own content changed) with its children's **cached** subtree hashes.
  Neither the whole tree nor untouched siblings are ever re-hashed.
- **Fresh emissions are not hashed during the build.** During a render
  every node is "a mutation"; charging the dirty-up-walk plus recompute per
  re-render would tax the hot path with no release-mode consumer. New
  segments get hashes lazily at/after commit or on oracle demand (§5.3
  step 3); the maintenance rule above applies to *retained* segments
  mutated by overlay writes (§5.2.1).

### 6.4 Hash width + collision policy

A collision would silently skip a real change — the **forbidden outcome**
(G1). Policy, all three layers together:

1. **Width: 128-bit** (xxh3-128/rapidhash-class, fixed seed per process).
   Birthday bound at 128 bits across even 10^9 subtree compares is ~10^-20 —
   below hardware error rates. 64-bit is NOT sufficient as a skip
   justification on its own (10^7 subtrees ⇒ ~10^-5 lifetime collision odds;
   an app that runs for months crosses the comfort line).
2. **Hash equality is a fast-path, not the sole authority, where cheap:**
   within a diff (§5.3 step 3) hash-equal is accepted; in debug builds a
   configurable fraction (default 1%) of hash-equal skips are re-verified by
   deep structural compare, and **any** mismatch is a hard assert.
3. **The oracle** (§6.5, §10.2) runs continuously in debug — a collision
   that slipped a change through is caught by the next full-build oracle
   compare with probability 1−2^-128 per check, i.e. detected in practice.

### 6.5 The O(1) equivalence oracle

`hash(real-dom projection) == hash(fresh full build)`:

- the "real-dom projection" hash is the root `subtree_hash` of the *applied*
  shadow tree (which mirrors the real DOM by the transactional contract
  §8.1);
- the "fresh build" hash is computed by running the full build with sink =
  none into a scratch arena (this is exactly today's bootstrap-style build,
  src/app/app.cpp:401–403), **replaying overlays** (§5.2.1), and hashing it.

**The lexbor side is verified too — first-class, not "on demand."**
Hash-vs-hash compares two shadow-side artifacts and therefore cannot see
**sink bugs** (an op applied to the wrong element via an aliased remote_id,
a mislanded move, a missed remove) — precisely the component where both
measured bugs lived. Debug mode therefore runs a **shadow ≡ lexbor deep
verify** at two cadences: (a) **continuous over the touched set** after
every window — cheap, O(ops), only mutated subtrees; (b) **full-document
sampled** — every N frames and after every structural window. Its cost on
DENDER (~3,900 nodes) is measured and published alongside the 2.7 ms
fresh-build figure (§14).

**Debug oracle schedule (140 Hz reality):** run the full fresh-build check
after every reconcile *while frame time permits*; during sustained gesture
streams degrade (flag-controlled) to settle-triggered + every-Nth-frame
sampling, so debug builds stay interactive — a 2.7 ms build per frame is a
third of the 7 ms budget on DENDER and scales linearly (a 50k-node T4
document would make continuous mode a slideshow, violating the project's
verify-in-window rule). Fuzz/CI suites always run continuous mode. The
bench records the cost formula (build slope × nodes + hash slope × nodes)
so the throttle threshold is measured, not guessed.

Release: sampled (e.g. every N seconds or on-demand via perf HUD).
Divergence ⇒ degradation ladder (§10.4) — including the terminal
nondeterministic-render classification so a divergence that reproduces
immediately after rung 3 cannot loop — loud log.

---

## 7. Visibility scoping

Requirement: PERF_RELIABILITY_TICKETS.md:65–78. Content that cannot be seen
is neither **built** nor reconciled — skipping the diff alone is not enough;
the declaration lambdas must not run.

- **Obscured tabs/pages/panels** (inactive dock tabs, unselected inspector
  sheets, closed panels): the widget at the visibility boundary holds a
  **deferred builder closure + placeholder node**. Precedent in-tree, kept
  and generalized: `set_dock_active_tab_provider` emits inactive tab bodies
  as empty hidden placeholders whose content builds on first selection
  (include/affineui/view.h:803–807).
- **Offscreen virtual list/tree items** (ties into T5,
  PERF_RELIABILITY_TICKETS.md:265–278): `View::virtual_list` already takes a
  per-item build closure and a window
  (`first_item/visible_items/overscan`, include/affineui/view.h:192–199,
  623–627); each *item* is a widget whose closure runs only inside the
  window. Row recycling (T5) reuses item widget slots + segments by stable
  item id.
- **Mechanics:** the boundary widget carries a HIDDEN state bit. While
  hidden: `invalidate()` on it or its descendants sets flags and bumps
  versions normally, but the stage-2 descent **stops at the boundary**
  (flags/versions are *held*, PERF_RELIABILITY_TICKETS.md:181–183) and
  ancestors' DESC-DIRTY is cleared with the boundary *parked* (§3.4 —
  otherwise hidden churn would keep the whole app off the do-nothing fast
  path). On reveal: the boundary clears HIDDEN, **re-marks the ancestor
  chain** (invalidate-style up-walk) so the next descent reaches it, and is
  treated as SELF-DIRTY if `version != applied_version` **or flags are held
  at the boundary** (the flag is the non-wrapping backstop; versions are
  64-bit anyway, §3.2) — or if never built. It reconciles **exactly once**
  — versioning supplies the reveal-correctness story
  (PERF_RELIABILITY_TICKETS.md:75–77).
- **Hide semantics for interaction state (normative):** hiding a boundary
  **blurs** any focus inside it — with an app-observable blur/commit for
  text controls per the §9 text-control rule (uncommitted text commits;
  never silently lingers in an invisible field) — and **cancels or
  completes any gesture** anchored inside it (pointer capture must not keep
  writing to invisible DOM). **Reveal never restores focus implicitly** —
  an implicit restore would be a surprising focus steal.
- **Who decides visibility:** the engine, from state it already owns — the
  dock active-tab provider, the virtual-list window, `hidden`/`display:none`
  status of the placeholder's real DOM node. Apps never toggle visibility by
  poking the DOM (G5); they change state (select tab, scroll) and the
  interaction layer / providers translate (§9).
- The oracle (§6.5) treats a hidden placeholder as equivalent to its
  unbuilt content *by definition*: the fresh-build comparison also builds
  placeholders for hidden regions (same providers), so the claim "hidden
  content may be stale" is scoped and explicit, not a hole in the oracle.

---

## 8. Sink contract (the stage 3→4 bridge)

### 8.1 Transactional mutation window

- `Document::begin_view_mutations()` … `end_view_mutations()` is the window
  (src/dom/document.cpp:18457–18502). All stage-3 output for a frame is
  applied inside **one** window.
- **Transactionality** (requirement, PERF_RELIABILITY_TICKETS.md:127–129):
  an exception mid-render or mid-reconcile must leave the previous
  consistent DOM, or trigger the ladder (§10.4). Concretely:
  - stage 2 (render) failures leave the DOM untouched (emission targets a
    fresh segment; previous shadow and DOM persist) but must **not lose the
    pending change** — "discard, log, done" alone would drop it forever,
    because the pass clears flags and nothing would ever revisit the
    widget. Normative: a widget's flags are cleared only **after** its
    closure returns successfully (§3.4); on a throw, SELF-DIRTY is re-set
    (it stays set) and `version != applied_version` persists, so the next
    frame retries. After `N_render_fail = 3` consecutive failures of the
    same widget, escalate to a ladder rung (§10.4: region treatment —
    render a placeholder + loud diagnostic) instead of retrying forever.
    **Sibling widgets rendered in the same pass still commit** — partial
    commit across *widgets* is the accepted semantics (each widget's
    segment is its own unit); the resulting cross-widget state (new
    selection applied, failing inspector stale) is transient by
    construction — healed by the retry/escalation — and loudly logged on
    the first failing frame;
  - stage 3 (apply) failures are NOT unwindable op-by-op against lexbor;
    the contract is therefore **abort-to-rung**: catch at the window
    boundary, mark the window poisoned, and rebootstrap (rung 3) — never
    fail forward into a half-applied state. `applied_version` and applied
    hashes are committed only on successful `end_view_mutations`, so a
    poisoned window leaves the bookkeeping claiming (correctly) that the
    widgets are still pending.
- Nested/overlapping windows are forbidden (single-threaded frame phases,
  §2); `begin_view_mutations` while a window is active is a debug assert.
- **No user code runs inside the window.** Element removal inside stage 3
  can trigger destroy/focus-loss notifications (the lexbor fork's
  `event_destroy` patches; weak-callback destroy notify): these are
  **queued to the next event phase**, never dispatched synchronously
  inside the window — so no callback can observe or mutate a half-applied
  frame. The mutation gate covers the window
  (`in_render_ OR view_batch_active`, §3.6), with the blocked-call
  observables specified there (deferred invalidate latch; dropped ref
  writes + diagnostic).

### 8.2 Batching contract — the 775 ms/op bug class, forbidden

**This fixes measured bug #2** (PERF_RELIABILITY_TICKETS.md:45–49). Today the
window suppresses lexbor's eager per-insert selector matching
(src/dom/document.cpp:18468–18472) and defers ONE `dock_structure_changed`
to the end for *structural* dirt (src/dom/document.cpp:18488–18500) — but
attribute/text ops still charge full live-mutation machinery **per op**:
`DocumentViewSink::set_attribute` → `set_attribute_on_element`
(src/dom/document.cpp:18401–18411 → 10025–10156), which per selector-
affecting attr does a stylesheet rematch + subtree restyle + reveal check —
≈775 ms each on the 105 KB DENDER document, ×84 phantom ops = the 64 s.

**Contract (normative):**

- Inside the window, **per-op full-document (or large-subtree) restyle,
  rematch, recollect, and layout are FORBIDDEN.** Ops perform only the raw
  DOM write plus O(1) bookkeeping.
- **Same-value writes are no-ops at the apply gate (user directive,
  2026-07-03).** Every op first compares against the element's current
  value (attr string, text content, style property after merge): equal →
  the op is dropped on the spot — no DOM write, no touched-set entry, no
  classification, no dirty bits (style/layout/paint untouched); cost is
  one compare. This is the last wall of the **DIRT-NEVER-AMPLIFIES
  invariant**: every stage may only *narrow* the change set
  (visibility → dirty flags → versions/hashes → coalesced diff → apply
  gate), never widen it. The steady-state diff already emits only real
  deltas, but the gate is what makes upstream over-approximation *safe*
  rather than merely rare: an escalated/ForceChildren re-render, a ladder
  rung-2/3 full re-render whose diff baseline was discarded, or a
  localized region rebootstrap all re-emit broadly — yet what reaches the
  document, and therefore the renderer, stays proportional to the TRUE
  change. A frame that re-rendered half the app but changed one label
  produces one attribute write and one repaint rect.
- Each op is **classified on receipt** (reusing the existing classification:
  `attribute_can_affect_selector_matching`, subject-confined rematch,
  paint-only svg-child rules — src/dom/document.cpp:10040–10090) and its
  target recorded in a **touched set** (dirty-root block indices +
  per-element rematch list + structural flag), with old visual rects
  captured for stage-4 dirty rectangles.
- `end_view_mutations` then performs, in order, **at most once each**:
  1. one scoped stylesheet **rematch** over the touched-set cover (or the
     per-element rematch list when every touched attr is subject-confined);
  2. one scoped **restyle** over the merged dirty roots;
  3. one **scoped recollect** iff structure changed or a hidden subtree was
     revealed (the existing reveal check, src/dom/document.cpp:10126–10131,
     moves to batch end) — scoping is a normative deliverable, see below;
  4. at most one **deferred layout** iff any op classified as
     layout-affecting (T3's layout-lock policy can veto/defer further,
     PERF_RELIABILITY_TICKETS.md:249–252);
  5. dirty-rect emission to the renderer from the captured old rects + new
     rects.
- The existing per-op path (`set_attribute_by_id` etc.) remains for
  *non-window* mutations (interaction layer live drags), where one-op-one-
  update is exactly right.

**Scoped recollect is a normative deliverable of this design — the real
fix for bug #2, not an optimization.** "One recollect per window" bounds
the COUNT, not the COST: the only recollect that exists today,
`recollect_blocks_from_current_dom` (src/dom/document.cpp:9261–9318),
clears **all** blocks, resets the style store, and re-resolves the entire
document body-down — unconditionally O(document) with full selector
resolution. Under that primitive, every frame containing *any* structural
op (a virtual-list window shift, a menu opening during a drag, a row
insert, a tab reveal) pays full-document restyle+collect — tens of ms on
DENDER-scale documents against a 7 ms budget, making the §14
"settle ≤ 2 ms" budget and phase 3's structural-op-per-frame 140 Hz
workloads unreachable while stages 1–3 are O(changed). Requirements:

- Recollect is **scoped to the touched dirty roots**: patch `impl.blocks`
  and the style store for the mutated subtrees only, preserving block
  indices or using a block-index remap — the same way restyle is scoped.
  (Feasibility of preserve-vs-remap is a named skeuo-review question,
  Appendix B.)
- The sink distinguishes **"structure changed inside an already-collected
  subtree"** from "anything structural": recycled-row rebinds (§4.2) and
  attr-only frames never set the structural flag at all.
- Until the scoped version lands: measure one full recollect on DENDER and
  on a 10k-node T4 fixture, publish the numbers, and **phase 3's
  "structural op per frame at 140 Hz" acceptance is BLOCKED on scoped
  recollect** (§13).

**Transient interaction state survives the window (normative).** Today's
recollect calls `reset_dynamic_block_state`, which sets
`hovered_idx = active_idx = focused_idx = -1`
(src/dom/document.cpp:9251–9259, invoked at :9325; `dock_structure_changed`
also resets at :14230–14231). Pseudo-class state bits and scroll are
already snapshot/replayed **by element** (:9265–9286) — but focus is not,
and key routing gates on `focused_idx` (document.cpp:7894): under this
design a structural reconcile during typing would silently kill the caret
and eat keystrokes mid-word. Contract:

- The window snapshots interaction state **by element** (versioned
  `DomHandle` weak slots — never block indices) and restores it after the
  batch-end recollect: focus/caret/selection/hover, exactly as scroll and
  state bits are handled today (:9265–9286 is the in-tree pattern to
  extend). Preferred endgame: migrate `focused_idx`/`hovered_idx` to weak
  handles so recollect renumbering is a non-event.
- **Animation/transition continuity:** animated and transition style state
  is keyed by element and **survives the recollect** (extend the same
  per-element snapshot/replay). A transition may reset only when the
  reconcile actually changed that element's relevant property. (Today
  `style_store.reset()` drops ComputedStyle/AnimatedStyle wholesale and
  recounts candidates from scratch, :9287, :9319–9323; under this design
  structural reconciles happen at interaction rate, so that would visibly
  restart or snap document-wide transitions every frame — for elements the
  diff never touched.)
- §14 fixtures: "type in a TextField while a background widget reconciles
  structurally every frame" and "run a transition while an unrelated
  widget reconciles structurally" both assert continuity.

**Layout timing is pinned (closes the T3 hidden-relayout hole inside this
contract).** The at-most-one deferred layout runs **at
`end_view_mutations`, before any geometry consumer can observe the
document**. Every geometry read between window close and paint —
`find_element_rect` and `dispatch()` hit-testing, which both carry the
hidden full-relayout contract today (src/dom/document.cpp:16773–16796) —
routes through **one shared ensure-layout** that (a) satisfies the
deferral if still pending, (b) is counted in the perf-HUD layout counters,
(c) is subject to the T3 layout-lock veto, and (d) marks the deferred
layout done so stage 4 never runs it twice. Without this, a popover opened
in the window and placed via `find_element_rect` before paint pays a
hidden full layout mid-frame *plus* the deferred one — two uncounted
full layouts per frame. §14 asserts **at most one layout per frame**
across the scripted interaction suites, counter-gated.

### 8.3 Mutation classification feeding the renderer

Unchanged in kind from today (this is stage 4's existing input): each
applied delta is classified paint-only / restyle / layout;
`mark_live_mutation_dirty` produces small rects
(src/dom/document.cpp:10141–10155). The window aggregates rects instead of
emitting per op. Svg-child mutations remain paint-only, no blocks
(PERF_RELIABILITY_TICKETS.md:15–16).

### 8.4 Raw-html nodes

`View::html` markup (include/affineui/view.h:581–583; sink hooks
view.h:394–402) is opaque to the shadow diff. Its **fingerprint is its
markup-string hash** (§6.2): unchanged hash ⇒ no sink call, the parsed
fragment group persists (src/dom/document.cpp:18303–18350); changed hash ⇒
`set_raw_html` re-parses the fragment inside the window (counts as
structural dirt). Raw-html inside an otherwise-clean widget cannot force a
re-render: the string lives in the widget's segment and is hash-compared
like everything else.

---

## 9. Interaction-layer ownership of real-DOM state (DECIDED — temporal ownership)

**Reality check — the core interaction layer mutates the real DOM directly
today** (by design: it is the C++ port of decius.js, living in
src/dom/document.cpp, driven by `data-dcs-*`):

| Interaction | Real-DOM state written | Citation |
|---|---|---|
| Dock splitter drag | inline `style` `flex:0 0 Npx` on prev/next panes | src/dom/document.cpp:10666–10679 |
| Tab switch | `hidden` toggled on tab bodies; active classes on tabs | dock machinery, document.cpp (data-dcs tab handling) |
| Menus / popovers | open-layer elements + inline placement styles | `TransientState::Layer{id, style, base_style, …}`, include/affineui/document.h:328–338 |
| Floating panel / tearoff drags | inline `left/top` (+ size) on floats; structural dock surgery | `tear_off_panel`, src/dom/document.cpp:14242 ff.; dock surgery + `dock_structure_changed` :14222–14236 |
| Drag-scrub combos, faders, knobs | `data-value` / `aria-valuenow` / value attrs | src/dom/document.cpp:10499–10581 |
| Focus / hover | Document-side indices (`hovered_idx` etc.), not attrs | src/dom/document.cpp:14230–14233 |
| Text input editing | live value/caret/selection overlays keyed by node; display prefers overlay over declared value | src/dom/document.cpp:699–701, 5225–5231, 5257–5263 |
| Tree/menu transient classes | `--open`/`--active`/`--drop-*`/`--stacked` class **tokens** on app-declared elements | src/dom/document.cpp:15275, 11802, 15302–15313, 11277 |
| Misc transient stores | `user_textarea_sizes`, `dcs_select_anchors`, per-element scroll offsets, `text_selection_drag_idx` | src/dom/document.cpp:768–769 ff. |

**The provider round-trip precedent** (already shipped): dock placement,
sizes, live layout, and active tab are read back from the Document and fed
into the next build via `set_dock_size_provider` /
`set_dock_placement_provider` / `set_dock_layout_provider` /
`set_dock_active_tab_provider` (include/affineui/view.h:784–807), so the
declaration re-emits the *current* arrangement and a rebuild never fights a
drag. `load_html` likewise capture/restores open-layer transient state
across the bootstrap (src/app/app.cpp:341–346).

**General ownership rule (normative — DECIDED, formerly "proposed"):**
every piece of real-DOM state has exactly one writer **at a time**.
Ownership is a per-element, per-attribute **state machine**, not a static
table — a static attr-level registry cannot express the reality its own
commit-point paragraph describes ("live = X, settled = R"), and either
static reading ships a concrete failure: statically-X means a declared
`data-value`/pane-size change on a matched node is silently never emitted
(wrong DOM, hash-excluded so unhealable); statically-R means a re-render
landing mid-drag re-emits the settled pre-drag value and snaps the
splitter back under the user's pointer.

- **Mode R — round-tripped (settled state).** The interaction layer writes
  the DOM *and* publishes to Document state; a provider feeds it into the
  build so declared == real by the time reconcile runs. Reconcile diffs it
  normally; **hashes include it**. This is the settled mode for dock
  sizes/placement/layout/**active tab (including `hidden` on tab
  bodies)**, and future: foldout/tree expanded state, selection where
  app-meaningful. Providers read **live** Document state at build time (as
  the dock providers do today), so even a build that lands mid-gesture
  emits current values — the gesture-held exclusion below is
  belt-and-suspenders, not the only defense.
- **Mode X — gesture-held (live state).** An attribute is X **only while
  the owning behavior holds a live gesture on that element**. The
  interaction layer stamps a **gesture-held bit + owner mask** on the
  element's bookkeeping at gesture begin and clears it at commit/cancel;
  the diff's attr merge and the hash canonicalizer test that bit — an
  **O(1) flag test at apply time, never a selector match** in the diff hot
  loop (a "attr name + behavior selector" lookup would cost tens of
  thousands of selector matches on a rung-2 full diff). While held:
  reconcile neither emits, diffs, nor removes the held attrs; hashes
  exclude them (§6.2). Instances: splitter-drag flex-basis, float-drag
  left/top, scrub values before commit, menu/popover placement styles.
- **The commit point (normative — the "live = X, settled = R" template,
  now with its missing final step):** on gesture commit the interaction
  layer (1) publishes the settled value to Document state / the provider,
  (2) clears the gesture-held bit, and (3) **invalidates the owning
  widget** (SELF-DIRTY + version++, §3.3 atomicity). The commit IS a state
  change. The re-render re-reads the provider and emits values that
  already match the real DOM — a 0-op diff — and the applied shadow
  re-converges before any oracle check. Without step (3) there is no
  "next rebuild" (nothing else is dirty), the shadow stays stale, and the
  continuous oracle reports a **false divergence + ladder drop after every
  splitter release and tab switch**, destroying the divergence==bug
  promise. The earlier draft rule "interaction mutations do NOT touch
  widget versions" is hereby **scoped to gesture-held Mode-X writes
  only**; Mode-R commits always invalidate.

**`style` ownership is property-level, not attr-level.** Exclusion applies
to a fixed owned-property set per behavior (`flex-basis` for splitters;
`left/top/width/height` for float drags) — never to the whole `style`
attribute, or app-declared pane styles (border, padding) would be silently
unreconcilable. The diff merges style as parsed properties **only for
nodes carrying the gesture-held bit**; every other node compares the
string — parse cost confined to the handful of gesture-held elements.

**`class` ownership is token-level.** The interaction layer owns
individual class TOKENS on elements whose `class` the app also declares
(`dcs-tree__chevron--open` :15275/:15209, `dcs-menu__item--active`
:11802/:13040, `dcs-tree__row--drop-*` :15302–15313/:15429,
`dcs-vec--stacked` :11277). `class` cannot be excluded wholesale — it is
the app's primary declared surface. Registry entries for `class` are
**token patterns**; the diff's class compare operates on token sets:
applied value = declared tokens ∪ interaction-owned tokens currently
present on the real node; the hash canonicalizer strips the same patterns
(§6.2). Without this, any scene-panel re-render (e.g. renaming an object)
would collapse every user-expanded tree row and wipe `--drop-*` indicators
mid-drag. **Tree/foldout OPEN state is additionally promoted to a Mode-R
provider before the Phase-1 flip** (§13 acceptance): expansion is
user-meaningful settled state that must survive rebuilds and be
declarable; token-merge remains for genuinely transient tokens
(`--active`, `--drop-*`).

**Structural ownership (tearoff / drag-to-dock / open layers)** — the
registry covers attributes, but the interaction layer also RESTRUCTURES
the real DOM (`tear_off_panel` + float machinery,
src/dom/document.cpp:13886–13989, :14222 ff.), creating wrapper elements
the sink has no remote_id for and reparenting view-declared panels. The
diff transforms *shadow* → *declared* and silently assumes real == shadow;
structural interaction writes break that premise, and a later reconcile
would double-apply structure (a second float container, a ghost empty
wrapper). Rules:

- **Open layers (menus, popovers, drag ghosts) live OUTSIDE the reconciled
  tree by definition:** engine-created, engine-destroyed, never emitted by
  a build, never diffed or hashed; reference-node insertion (§5.3) makes
  reconcile immune to their presence as foreign siblings.
- **Structural gesture commits follow the attr commit contract** plus one
  addition — the commit must leave shadow and real DOM *reconvergeable*.
  Preferred (matches the dock direction): the settled structure is fully
  provider-described — **declarative-docking D3 float/tearoff providers
  are a Phase-1 dependency for tearoff-enabled apps** (§13) — and the
  commit invalidates the dock widget AND **discards its retained shadow
  segment**, forcing the region through a render+diff whose structural ops
  resolve by **remote-id adoption**: dock keys derive from pane/panel ids
  (§5.1), so "create float container F / move panel A into F" maps onto
  the already-existing interaction-created nodes, adopting them into the
  remote_id map; a structural op whose target state already holds becomes
  a no-op. Fallback until adoption lands: the region takes localized
  rung-3 treatment (region rebootstrap against the real DOM). **Never**
  diff old-shadow→declared and apply it blind to an already-moved real
  DOM.
- §14 gains the fuzz case: tearoff + unrelated invalidate + reconcile,
  oracle- and deep-verify-checked.

**Text controls (normative — absent from the earlier draft entirely).**
Uncommitted typing and carets live in `live_text_values`/`live_text_carets`
keyed by node (src/dom/document.cpp:699–700); the DISPLAY value prefers
the overlay over the declared attr (:5225–5231, :5257–5263). Rules:

- A text control's value is **gesture-held (Mode X) while the element is
  focused/live**. A declared value arriving mid-edit is **deferred**:
  recorded as the pending declared value, applied on blur/commit. (The
  React alternative — declared wins immediately, overlay dropped — is
  rejected: DCC users must not lose an in-progress rename to a background
  update. One rule, stated once.)
- Commit publishes through `on_change` and invalidates the owning widget
  (standard commit contract), so declared/shadow/real converge.
- Reconcile must **never replace a focused text control node** when only
  its value differs (§5.3 step 2); a structural replace of a focused
  control blurs-with-commit first.
- **IME composition is reserved now as gesture-held state:** a future
  composition buffer blocks value application until `compositionend` — the
  slot in this model exists so the engine addition lands inside the
  contract, not around it.

**Gesture references are weak (hard-to-crash invariant).** Every
interaction-layer reference to a real-DOM node MUST be a versioned
`DomHandle` weak slot. Today `TreeDrag.tree/row/select_box/target` hold
raw `lxb_dom_element_t*` (src/dom/document.cpp:615–629, set at :15427,
:17286–17288) that the destroy hooks do **not** clear (:763–784 clears
live-text maps, textarea sizes, select anchors, DomHandle slots — not
`tree_drag`): a reconcile that removes/replaces a dragged row or drop
target (duplicate-key churn and tag-mismatch⇒replace are both legal)
leaves a dangling pointer, and the next mouse-move is a use-after-free.
Additionally the sink's remove path defines gesture semantics: removing a
gesture-held node **cancels the gesture** (the existing
`cancel_dcs_tree_drag`-style cleanup) — reconcile never defers to a
gesture for structural correctness; the gesture yields, safely. The §14
fuzzer runs mutation scripts **while synthetic gestures are in flight**
(the earlier plan mutated only a quiescent document).

**Completeness audit is a Phase-1 exit criterion.** The table above is
illustrative, not exhaustive. Phase 1 does not flip until every
`data-dcs-*` behavior and every DocumentImpl transient store
(`live_text_*`, `user_textarea_sizes`, `dcs_select_anchors`, per-element
scroll offsets, `text_selection_drag_idx`, chevron-open tokens,
`aria-expanded`, …) has an audit row assigning **R / gesture-held-X** with
its commit point and its survives-move / survives-replace answer. This
audit is also the evidence the remaining registry-form open question
(§15) needs.

Consequences and guards:

- Reconcile can never clobber a drag (gesture-held exclusion) and never
  thrashes settled dock state (Mode R agrees by round-trip; providers read
  live state at build time).
- An app declaring a currently-gesture-held attr is a diagnostic (two
  writers); the declared value is deferred to gesture end (the
  text-control rule, generalized).
- Gesture-held exclusions + class-token patterns are the *only* hash
  exclusions besides bookkeeping (§6.2); each is **temporally scoped**, so
  the "reconcile won't fix it" surface exists only while a finger is down.
- The oracle compares hashes that exclude gesture-held state on both
  sides, and Mode-R commits invalidate owners — continuous verification
  stays green across interaction. §14 asserts it: a scripted
  tab-switch/splitter-drag/tree-drag/tearoff session with **zero rung
  drops and zero divergence events**.

The remaining open question (§15) covers only the **registration
mechanism** (static table vs per-behavior C++ registration vs
personality-scoped registries), not the semantics above.

---

## 10. Reliability spine

Verbatim requirements (PERF_RELIABILITY_TICKETS.md:116–138) — this is
architecture, not a test plan bolted on.

### 10.1 Justified skips

Every skip is a checkable claim, never a hope: visibility (claim: cannot be
seen; §7), flags (claim: no mutation path ran; §3.4 + the §3.6 gate),
versions (claim: no invalidate since apply; §3.3), hashes (claim: 128-bit
structural equality; §6.4). Debug builds can verify any skip: a
`AFFINEUI_VERIFY_SKIPS` mode re-renders sampled "clean" widgets into a
scratch arena and hash-compares against their retained segments; a mismatch
is a purity violation (someone mutated state without `invalidate()`) and
asserts with the widget key. The optimizer is only ever allowed to elide
work it can prove equivalent.

### 10.2 Cheap divergence detection

The O(1) oracle (§6.5): continuous in debug (after every reconcile, with
the gesture-stream throttle schedule of §6.5), sampled/on-demand in release
(perf-HUD trigger + periodic) — **plus** the first-class shadow ≡ lexbor
deep verify (§6.5: continuous over the touched set, sampled full-document),
which is what catches sink bugs that hash-vs-hash structurally cannot see.
Silent corruption — the worst failure mode — becomes a detected event
(PERF_RELIABILITY_TICKETS.md:122–126). Detection output: the first diverging
subtree (walk down the hash mismatch — O(depth·branching), only on failure),
logged with widget key, versions, and both hashes.

### 10.3 Transactional windows

§8.1. Exceptions at any phase boundary leave either the previous consistent
DOM (render-phase failure) or trigger rung 3 (apply-phase failure). Never
fail forward.

### 10.4 The degradation ladder

Always ends in correct; the system picks the cheapest rung it can **prove**
correct, so reliability is never traded for speed
(PERF_RELIABILITY_TICKETS.md:129–135):

| Rung | Action | Trigger |
|---|---|---|
| 0 | Skip (justified, §10.1) | version/hash/flag proof holds |
| 1 | Region re-render + reconcile | SELF-DIRTY region; a *localized* detected divergence (re-render the diverging widget with ForceChildren); `N_render_fail` consecutive render-closure failures of one widget (§8.1 — placeholder + loud diagnostic); a structural interaction commit without provider adoption (§9 fallback: region rebootstrap) |
| 2 | Full re-render + reconcile | root ForceChildren: every widget re-emits, diff heals any shadow-vs-DOM drift that rung 1 couldn't localize |
| 3 | Full `load_view` rebootstrap | poisoned window (§8.1), rung-2 oracle failure, or any unrecoverable sink error. The existing path: shell + replay (src/app/app.cpp:397–416) or classic `load_view` (:351–358), kept forever as the escape hatch |

Any detected inconsistency self-heals by dropping down a rung and **logging
loudly** (stderr + `View::diagnostics()` + perf HUD counter; release builds
too — a silent self-heal is a masked bug). Repeated rung-3 drops within a
short window escalate the logging to once-per-cause with a rate limit,
never to silence.

**Terminal state — nondeterministic render (the ladder must not loop).**
Rate-limiting the *logging* of repeated rung-3 drops does not break the
loop itself: a render closure that samples a clock (elapsed-time label,
frame counter) makes every oracle check diverge, and without a terminal
state the ladder rebootstraps (~190 ms) **every frame, forever**. Rule: if
rung 3 completes and the very next oracle check diverges again **with no
intervening `invalidate()`**, the cause is by elimination a
nondeterministic render closure (§3.6 violation), not DOM corruption.
Classification: `nondeterministic-render`; action: localize via the
first-diverging-subtree walk (§10.2), disable the oracle for that widget
(or the app if unlocalizable), surface a **distinct fatal-class
diagnostic** naming the widget, and keep rendering. This terminal state
exits the loop; it is the only condition under which the oracle is ever
switched off, and it is never silent.

### 10.5 Frame order

`events → update/render → reconcile → paint`, each phase closed before the
next (§2, §3.5). Enforced by phase flags in debug (`in_event_dispatch_`,
`in_render_`, `view_batch_active` — the last already exists,
src/dom/document.cpp:18465–18466).

---

## 11. Memory discipline

Requirements: no base-heap allocation churn; chained-block temp pools are
fine (PERF_RELIABILITY_TICKETS.md:62–63); T2 targets
(PERF_RELIABILITY_TICKETS.md:238–244). Measured today: ~1.6 heap
allocs/node/rebuild from builder temporaries (`std::to_string`,
concatenation, key strings).

- **Arena design:** one block allocator with **size-classed blocks**
  (256 B / 4 KiB / 64 KiB — class chosen by a segment's first-emission
  size, promoted on overflow) from a process-wide free list. A fixed
  64 KiB-only block would put a 64 KiB floor under every widget segment;
  combined with "segments never interleave blocks" and the design's own
  push toward fine granularity (every virtual-list row is a widget, §7),
  1,000 one-row widgets would cost ≥64 MB of shadow — 300× the stated
  budget. **Expected bytes for a 1-node widget:** one 256 B block + slot
  header ≤ 128 B ⇒ ≲ 400 B. Small segments may alternatively pack into
  **slabs owned by ONE parent region**, freed as a unit when that region
  re-renders — the no-interleave invariant holds at slab granularity, so
  cross-widget fragmentation stays impossible and the O(1) whole-chain
  free survives. The ≤2×-HTML budget below is computed **including
  per-segment block floors** — that accounting is precisely what forces
  the size classes. Three arena roles:
  1. **Segment arenas** (§4.3): per-widget retained chains; freed as whole
     chains to the free list on re-render/removal — O(1) reset, stable
     addresses while live.
  2. **Frame scratch arena:** diff working state (old-id indexes §5.3,
     touched sets §8.2, oracle fresh-build §6.5); reset once per frame,
     O(1).
  3. **String storage:** attr names/values and text are bump-allocated
     `string_view`s into the owning segment; attr *names* intern into a
     process-wide table (the name set is tiny and hot). `std::to_string`
     temporaries are replaced by arena `format_to` helpers
     (string_view-first setter paths, T2).
- **Small-size cases:** attr lists as inline arrays (SSO ≤ 8 attrs) inside
  segment nodes; StableId maps reused across frames (T2:243–244).
- **Measurable budgets (bench-gated, §14):**
  - ≤ **0.2 allocs/node** steady-state rebuild (T2 target; today 1.6);
    global-new counted exactly as the bench does now
    (tests/test_reconcile_bench.cpp:42–54).
  - **0 base-heap allocs** for a clean frame (skip lattice touches only
    slot headers).
  - Bytes/widget budget: slot header ≤ 128 B; shadow overhead target
    ≤ 2× the serialized-HTML size of the widget's markup (measured on
    DENDER: 105 KB HTML ⇒ ≤ ~210 KB segment bytes + slots; recorded, then
    ratcheted).
  - Free-list high-water mark reported in the perf HUD; **free-list decay:**
    blocks above high-water × 1.5 hysteresis are returned to the OS heap
    after ~300 idle frames, so a one-time heavy view (large dialog,
    transient escalation doubling a subtree) does not pin its peak
    resident forever. Soak (§14) gates on **RSS**, not just free-list
    count.

---

## 12. Current-code gap analysis

Today's T1 implementation vs this design (all citations 2026-07-03):

| # | Area | Today | Design | Gap class |
|---|---|---|---|---|
| 1 | Diff timing | Per-WRITE sink emission in `View::set_attr` (src/app/view.cpp:4040–4052) → **measured bug #1**: 84 phantom patches/clean DENDER rebuild (PERF_RELIABILITY_TICKETS.md:36–44) | End-of-build coalescing; diff compares final values only (§5.2) | Correctness-of-cost; fix by construction |
| 2 | Sink batching | Window suppresses `ev_insert` + one `dock_structure_changed` for structure (src/dom/document.cpp:18465–18500) but attr/text ops charge full rematch/restyle/reveal per op (:18401–18411 → 10025–10156) → **measured bug #2**: ≈775 ms/op | Touched-set + one scoped rematch/restyle/recollect/layout at `end_view_mutations` (§8.2) | Perf; contract violation today |
| 3 | Widget granularity | One root build function; whole-tree walk every rebuild (src/app/app.cpp:417–426) | Widget slots, flags, versions, containment (§3, §4.2) | Missing primitive |
| 4 | Shadow storage | Retained persistent `View` of heap-backed `WidgetNode`s diffed in place — "one point in this space, not the decided answer" (PERF_RELIABILITY_TICKETS.md:230–232) | Retained per-widget segments in chained blocks (§4.3), heap model as fallback | Open trade, recommendation made |
| 5 | Keyed matching | Positional match else truncate-tail remove+create (src/app/view.cpp:3961–3973); sink appends only, ignores `index` (src/dom/document.cpp:18260–18287) | In-order cursor + keyed matching + moves (§5.3) | Perf + state preservation on reorder |
| 6 | Hashing / versions / oracle | None | §3.3, §6 | Missing |
| 7 | Visibility scoping | Dock hidden-tab lazy placeholders only (include/affineui/view.h:803–807); `virtual_list` options exist but not wired to scroll (T5) | General boundary widgets, deferred closures (§7) | Partial precedent |
| 8 | Document/body attrs | `document_attrs_` only reach HTML serialization (src/app/view.cpp:752–776, include/affineui/view.h:993); reconcile never diffs them — **theme selector changes require a shell rebuild** via `set_stylesheet` re-bootstrap (examples/16_decius_dender/dender_app.cpp:316–330, src/app/app.cpp:437–448) | **DECIDED: sink op** (`set_document_attribute` — browser/remote mode needs the patch anyway). The diff runs over the **RESOLVED attr set** (`body_attrs()` output: explicit `document_attrs_` + framework `adjust_document_attrs` + `data-aui-framework/-version` derived from theme+version, src/app/view.cpp:752–778) — diffing the raw vector alone would miss theme/framework-version changes. `<head>` link changes remain shell-rebuild-only (table below) | Known T1 follow-up |
| 9 | RemotePatchSink parity | Same per-write emission (bug #1 reproduces in the diff probe via `RemotePatchQueue`, dender_app.cpp:347–372); `RemotePatch` has `index` but no Move op (include/affineui/view.h:334–351); no transaction framing | Coalescing benefits it automatically (it sits behind the same sink interface, view.h:405–428); add `Move` + begin/end markers (§5.3, §8.1) | Parity contract |
| 10 | Python bindings / standalone View | `View.begin(RemotePatchQueue)` exposed (bindings/python/src/affineui_py.cpp:530–532); patches stream to JSON; no App/Document/frame loop in that mode | **DECIDED: the widget model lives in View, not App.** Widget slots, versions, flags, visibility, and the stage-1/2 descent are View-owned; App contributes only scheduling (§2) and the document sink. A standalone View + RemotePatchQueue therefore hosts the identical widget model — no third "legacy remote" path to drift. Coalescing lands *below* the sink interface so remote consumers get final values; widget primitive + scheduling need pybind surface (GIL-safe callbacks per binding conventions) | Binding follow-through |
| 11 | Bench fidelity | 280-node bench: 81 µs/0 ops clean — **failed to predict** both real-app bugs because its tree never double-writes an attr and its `CountingSink` charges nothing per op (tests/test_reconcile_bench.cpp:62–88, 195–249; PERF_RELIABILITY_TICKETS.md:42–44) | Real-app-shaped fixture + document-sink-cost model (§14) | Test gap |
| 12 | Purity gate | `can_mutate_children` blocks structural WidgetRef mutation during build only (src/app/view.cpp:4176–4183) | Full emit-only render API + engine-wide in-render gate (§3.6) | Partial |
| 13 | Bootstrap / fallback | Shell + replay bootstrap works (`to_html_shell`, include/affineui/view.h:933–937; replay src/app/app.cpp:397–416); `load_view` intact (:351–358) | Kept verbatim as rung 3 (§10.4) and initial mount (§13) | None — keep |
| 14 | Interaction ownership | De-facto correct for dock via providers (include/affineui/view.h:784–807); no general rule; nothing excluded from (future) hashes | Temporal ownership state machine, gesture-held bits, token/property-level rules, commit-invalidates-owner (§9) | Decided |
| 15 | WidgetRef mutation path | `set_mutation_sink` is declared but **never called** anywhere in the repo (include/affineui/view.h:528) — outside a build, ref writes reach no sink today; `append/replace` run `build_children` with `sink_ = nullptr` (src/app/view.cpp:4193–4196) while `open_node` emits only when a sink is present (:3990) — appended children silently never reach the document on the fast path | Overlay + SHADOW-DIRTY durability contract; `append/replace` as anonymous child widgets (§5.2.1) | **Live correctness bug today**, not just a design gap |
| 16 | Transient interaction state vs batching | Batch-end recollect resets `focused_idx`/hover (src/dom/document.cpp:9251–9259 via :9325); gesture structs hold raw element pointers not cleared by destroy hooks (:615–629 vs :763–784); animated style state dropped by `style_store.reset()` (:9287) | Element-keyed snapshot/restore across the window + weak `DomHandle` gesture refs + animation continuity (§8.2, §9) | Correctness bug the moment reconciles run at interaction rate |
| 17 | C ABI (`affineui_c` → Rust/C#) | No widget surface; docs/LANGUAGE_BINDINGS.md predates this design | `affineui_widget(key, render_fn, user_data)`, `affineui_invalidate(handle)`, handler callbacks — C function pointer + `user_data`, honoring the `user_free` exactly-once contract | Binding follow-through (spec row added; implementation with phase 4) |

**Shell-rebuild-only inputs vs reconciled inputs** (closes the #8
ambiguity — document-attr diffing does NOT eliminate every rebootstrap
class, and an implementer must not believe otherwise):

| Input | Path |
|---|---|
| Body/document attrs, incl. framework-derived (`data-aui-framework`, `data-aui-version`, theme selectors) | **Reconciled**: diff of the resolved attr set via the `set_document_attribute` sink op |
| Framework version → `<head>` theme `<link>` (theme_link, src/app/view.cpp:742–750) | **Shell rebuild** (`set_stylesheet` re-bootstrap) — documented, rare, rung-3-class by design |
| User stylesheet / UA style replacement | **Shell rebuild** — documented, rare |

---

## 13. Migration & rollout

`load_view` **stays forever**: initial mount bootstrap (shell + replay,
src/app/app.cpp:397–416) and the ladder's final rung (§10.4). Every phase
below keeps the previous behavior behind the same App API
(`set_view`/`rebuild_view`, src/app/app.cpp:384–432); no sample rewrites
until **phase 1's** widget adoption (DenderView).

**Interim mandate statement (owning the gap explicitly):** an app with
zero `widget()` calls has the root build function as its one widget, so
*any* invalidate re-runs the full builder — on DENDER that is 2.7 ms and
~3,900 nodes × ~1.6 base-heap allocs/node ≈ 6k allocations per
interaction, at up to 140 Hz. That is a sustained violation of mandate G3
for all root-widget apps (the 17 non-DENDER examples, and any binding user
who doesn't adopt `widget()`) until phase 4's arenas AND per-app adoption
land. This is by design, not an accident: the fix for those apps is
adoption, not engine work, and correctness is unaffected. To keep the gap
honest, the **alloc-count gate for the dirty-widget path is pulled forward
into Phase 1 acceptance** (it needs no arenas to measure — it measures
that a single-widget invalidate allocates proportionally to that widget,
not the document); the full ≤0.2 allocs/node + 0-alloc-clean gates remain
phase 4.

**Phase 0 — kill the two measured bugs (no new primitives).**
End-of-build coalescing in View (§5.2, segment-close top-down emission) +
batched sink contract (§8.2). Because this phase already **changes the
observable patch stream** for existing RemotePatchQueue consumers (84→0
phantom patches; attrs emitted after creates instead of interleaved), the
JSON protocol gains its **begin/end transaction frame markers and the
`Move` op now** — additive, a no-op for old clients, and it avoids a
second protocol break later (decided; formerly open).
*Acceptance:* DENDER diff probe = **0 patches** on clean rebuild (first
build stays ~3927); `rebuild_view` clean ≤ 10 ms and camera-change
interaction ≤ ~15 ms on the 105 KB document (`decius_dender.exe --profile`,
dender_app.cpp:332–468); bench B unchanged (0 ops, tests/
test_reconcile_bench.cpp:247–248); RemotePatchQueue replay-parity test
green (§5.2).

**Phase 1 — flip DENDER.**
Widget slots + flags + versions + containment mounts + in-render gate
(§3, §4.1–4.2), diff over dirty regions with keyed matching +
reference-node insertion (§5.3), document-attr diffing over the resolved
set (§12#8), the §5.2.1 overlay/SHADOW-DIRTY path, transient-state
snapshot/restore across the window (§8.2), weak-handle gesture refs +
commit-invalidates-owner (§9). Flip `kUseReconcileFastPath = true`
(dender_app.cpp:28) — this is the real-app acceptance test.
*Phase-1 dependencies from §9 (DENDER hits them immediately — scene tree,
dock, text fields):* tree/foldout open-state Mode-R provider; the
ownership completeness audit table; D3 float/tearoff providers (or the
region-rebootstrap fallback) for tearoff-enabled apps; text-control
deferred-value rule.
*Acceptance:* DENDER ~**4 ms/interaction** (2.7 ms build — dropping toward
build-of-dirty-widgets-only as DenderView adopts `widget()` — + reconcile +
patch); zero HTML reparses after boot; `rebuild_shell` hack
(dender_app.cpp:316–330) deleted; type-while-reconciling and
tree-expansion-survives-re-render fixtures green; dirty-widget alloc gate
(interim statement above) green.

**Phase 2 — reliability spine.**
Incremental hashing + oracle (§6), skip verification, transactional
windows + degradation ladder wiring (§10), exception injection tests (§14).
*Acceptance:* debug-mode continuous oracle green through the full DENDER +
examples interaction suite; injected faults at every phase boundary
self-heal to the correct DOM with loud logs.

**Phase 3 — visibility + 140 Hz.**
Visibility boundaries (§7), virtual-list wiring to scroll with recycle
pools (§4.2, T5 handshake), escalation model tuning (§5.5), **scoped
recollect** (§8.2 — the structural-op-per-frame acceptance below is
BLOCKED on it). affine_2600 acceptance: cable drags and sequencer playback
at display refresh (~140 Hz) with zero reparses
(PERF_RELIABILITY_TICKETS.md:20–22).
*Acceptance:* affine_2600 frame time ≤ 7 ms sustained during drags AND
during sequencer playback (the scattered-small-dirty-widgets case — §5.5's
escalation must not degrade it); DENDER inactive-tab switch cost
independent of hidden-tab content size; virtual-list scroll at 140 Hz with
zero recollects and zero create/remove churn.

**Phase 4 — memory discipline.**
Segment arenas + string interning + SSO attrs (§4.3 recommendation, §11);
RemotePatch `Move`/transaction framing + Python surface (§12#9–10).
*Acceptance:* alloc gates (≤0.2 allocs/node dirty rebuild, 0 allocs clean
frame), bytes/widget budget recorded and ratcheted, soak flat.

Order rationale: correctness-of-cost first (phase 0 is measurable this
week and unblocks the flag flip), primitives second, proofs third,
scale/memory last — each phase leaves the system strictly more correct and
never depends on a later phase for safety (the ladder exists from phase 2;
before that, `kUseReconcileFastPath` and `load_view` are the manual ladder).

---

## 14. Test & verification plan

Follows from the reliability spine (PERF_RELIABILITY_TICKETS.md:136–138).

- **Real-app-shaped fixture.** Extend tests/test_reconcile_bench.cpp with a
  fixture that reproduces what the 280-node bench missed
  (§12#11): (a) base-classes-then-modifier **double attr writes** (the
  `.cls("…--has-sub")` pattern, PERF_RELIABILITY_TICKETS.md:38–41), (b) a
  cost-charging sink that models per-op restyle work (so per-op batching
  regressions show up as time, not just op counts), (c) ~4000 nodes
  (DENDER scale), (d) raw-html islands, (e) dock-provider round-trips.
- **Clean rebuild MUST be 0 ops** — on the real-shaped fixture and on DENDER
  `--profile` (diff probe, dender_app.cpp:347–372), not only the synth bench
  (existing check tests/test_reconcile_bench.cpp:247).
- **Per-op cost budgets:** window-applied attr op ≤ 5 µs bookkeeping;
  `end_view_mutations` scoped settle ≤ 2 ms on DENDER-scale documents for a
  single-widget dirty region; assert via the `[batch]`/`[attr]` trace
  timers already present (src/dom/document.cpp:18492–18499, 10133–10140).
- **Property/fuzz vs the oracle:** generate a random widget tree + random
  mutation scripts (attr add/remove/change incl. duplicates of the
  double-write pattern, text, reorder incl. backward moves, insert/remove,
  hide/reveal, interaction-owned attr writes, overlay ref writes); after
  every reconcile assert `hash(applied) == hash(fresh build + overlay)`
  and (sampled) deep-compare shadow ≡ lexbor DOM. **Mutation scripts also
  run WHILE synthetic gestures are in flight** (tree drag, splitter drag,
  focused text edit) — not only against a quiescent document — asserting
  no UAF (ASAN), gestures cancel cleanly when their nodes are removed, and
  gesture-held state is never clobbered. Includes the tearoff case:
  tearoff + unrelated invalidate + reconcile, oracle- and
  deep-verify-checked. Seeded, shrinking, run in CI; long-run mode in soak.
- **Interaction-continuity fixtures** (§8.2, §9): (a) type in a TextField
  while a background widget reconciles structurally every frame — focus,
  caret, and uncommitted text survive; (b) CSS transition running while an
  unrelated widget reconciles structurally — no restart/snap; (c) scripted
  tab-switch + splitter-drag + tree-drag + tearoff session — **zero oracle
  divergence events, zero rung drops** (Mode-R commits re-converge the
  shadow); (d) expanded tree rows survive a scene-panel re-render.
- **Escalation benches** (§5.5): K×subtree-size crossover sweep;
  per-region fixed-overhead measurement (justifies or deletes `K_max`);
  the **scattered case** — 17 one-node dirty widgets under a 10k-node root
  must NOT render the root; boundary-oscillation case exercising
  hysteresis.
- **Ordering/parity:** replay a RemotePatchQueue stream into a scratch
  client model; assert it equals the shadow (parent-first creates, attrs
  follow their create, positions valid at emission — §5.2).
- **Layout discipline:** at most **one layout per frame** across the
  scripted interaction suites, gated on the perf-HUD layout counter
  (§8.2 ensure-layout contract); recollect count AND recollect scope
  (touched-subtree node count vs document node count) reported per frame.
- **Exception injection at every phase boundary:** throwing render
  closures, sink ops that throw at op k (parameterized), OOM simulation in
  arenas; assert previous-DOM-or-rebootstrap, never half-applied (§8.1),
  and that the ladder logs.
- **Alloc-count regression gates:** the existing global-new counter
  (tests/test_reconcile_bench.cpp:27–54) gates ≤0.2 allocs/node dirty,
  0 allocs clean frame; CI fails on exceed (T2,
  PERF_RELIABILITY_TICKETS.md:242–243).
- **Skip verification mode** (§10.1) run over the examples' scripted
  interaction suites in CI debug jobs.
- **Soak:** hours-long DENDER + affine_2600 scripted interaction runs with
  leak/handle tracking and free-list high-water monitoring (T4,
  PERF_RELIABILITY_TICKETS.md:259–262); oracle sampled throughout.
- **Windowed verification:** per the project rule, interaction/visual
  milestones (phase 1 flip, phase 3 140 Hz) are not "done" until verified
  in the windowed app by a user tester — unit tests and headless numbers
  are necessary, not sufficient.

---

## 15. Open questions

Only what truly remains undecided. Items decided during the review
revision (document-attr diffing → sink op; remote transaction framing →
Phase 0; WidgetRef write timing → §5.2.1; widget identity → parent-
independent; static-vs-temporal ownership → temporal state machine) have
moved into their sections and are recorded in Appendix A.

1. **Segment storage vs heap-model retained tree (final call).** §4.3
   recommends per-widget chained-block segments (C); fallback is today's
   heap model (A) + versions. *Deciding evidence:* phase-1 DENDER numbers
   with (A) — if allocs/node and rebuild time already meet gates,
   (C) can slip to phase 4 or be narrowed to string storage only; plus
   measured bytes/widget and free-list fragmentation from a (C) prototype
   under the fuzz workload.
2. **Widget state cells.** Does the engine ever own state (React-style
   hooks), or stay app-state + `invalidate()` forever (§3.2)? *Options:*
   sidecar state-cell library vs core primitive vs never. *Evidence:*
   whether the Python/tri-language bindings need engine-held state to make
   `build=` closures ergonomic without leaking C++ object lifetimes.
3. **Escalation constants** `rho`, `C_esc`, `K_hi/K_lo` (§5.5) — and
   whether `K_max`-triggered covering-search earns its keep at all.
   *Evidence:* bench sweep of K×subtree-size crossover + per-region fixed
   overhead on DENDER-scale fixtures; re-fit after segment arenas land
   (emission slope changes).
4. **Hash width final call** (§6.4): 128-bit recommended; is 64-bit + debug
   full-verify acceptable for memory-constrained embedding hosts?
   *Options:* 128 everywhere / 64 + verify / per-build-config. *Evidence:*
   measured hash throughput + segment memory delta on 50k-node T4 stress
   docs; embedding-partner memory ceilings.
5. **Real-DOM moves in lexbor.** §5.3 assumes `insert_before`-based moves
   are cheaper and state-preserving vs remove+create (focus, open layers,
   scroll). *Evidence:* microbench of lexbor node moves + restyle cost vs
   recreate on DENDER-scale lists; whether any lexbor bookkeeping breaks on
   reparenting (style attach, event targets). Also a skeuo-review item
   (Appendix B).
6. **Ownership REGISTRATION mechanism** (§9 — semantics are decided;
   only the plumbing is open): static table in the interaction layer,
   `data-aui-owned` marker attr, or per-behavior C++ registration?
   *Evidence:* the Phase-1 completeness audit's row count (§9); whether
   personalities (Bootstrap vs Decius) need different registries
   (personality-mapping tiers say probably yes → registration API over
   static table).
7. **Oracle scope for hidden content** (§7): fresh-build oracle builds
   placeholders for hidden regions (recommended), or optionally force-build
   hidden content in a deep-verify mode? *Evidence:* whether hidden-region
   bugs (stale content on reveal) escape the version mechanism in fuzz —
   if yes, add the deep mode as a nightly job.
8. **Scoped recollect strategy** (§8.2): patch blocks in place preserving
   indices, or rebuild touched subranges with a block-index remap table?
   *Options + deciding evidence:* skeuo review of block-index consumers
   (Appendix B item 2) + a prototype on the T4 10k-node fixture; whichever
   meets "settle ≤ 2 ms for a single-widget region" with the least
   invasive change wins.
9. **Recycle pool sizing** (§4.2): per-container LRU cap (fixed count vs
   proportional to window size) and eviction cadence. *Evidence:* T5
   100k-row scroll bench memory ceiling vs re-render cost of a cold row.

---

## A. Review findings and resolutions

The 2026-07-03 adversarial review ran four lenses (correctness,
performance, interaction-state, api-contract) and produced 47 findings.
Every finding is listed; none dropped. **ACCEPTED** = the spec changed
(section cited). **ANSWERED** = a question resolved inline. Nothing was
rejected outright; two half-rejections are flagged where a finding's
*suggestion* lost to a competing finding's.

### Correctness lens (C1–C14)

| # | Sev | Finding | Resolution |
|---|---|---|---|
| C1 | blocker | Structural interaction surgery (tearoff) desyncs shadow from real DOM; next reconcile double-applies structure | ACCEPTED — §9 structural ownership: commit invalidates owner + discards its shadow segment; structural ops resolve via remote-id adoption (dock keys from pane/panel ids) or localized region rebootstrap; never blind-apply. Fuzz case added (§14) |
| C2 | blocker | Mode R vs X contradiction on the same attrs; static per-attr registry can't express live-vs-settled | ACCEPTED — §9 rewritten as a temporal per-element/per-attr state machine (gesture-held bit); §6.2 exclusions rewritten gesture-scoped; contradictory static entries deleted |
| C3 | blocker | WidgetRef writes silently discarded by the re-render their own SELF-DIRTY triggers | ACCEPTED — §5.2.1: distinct SHADOW-DIRTY (diff without closure), per-widget overlay re-applied after renders, retirement-on-redeclare diagnostic, oracle replays overlays |
| C4 | major | Version check can veto a legitimately flagged visit; flag-without-version++ paths silently skipped | ACCEPTED — §3.3 atomicity invariant (flag+version one op; versions confirm, never veto; inconsistent state debug-asserted); §5.4 layer 3 reworded |
| C5 | major | Throwing render closure drops the change forever; no rung, no retry; torn cross-widget state | ACCEPTED — §8.1: flags cleared only after successful closure return; SELF-DIRTY re-set on throw; N_render_fail=3 escalates to rung 1 (placeholder + diagnostic, §10.4 trigger added); sibling partial-commit declared the semantics |
| C6 | major | Render determinism load-bearing but never required; nondeterminism = permanent rung-3 loop | ACCEPTED — §3.6 normative determinism (same state ⇒ identical emission; time via invalidated state); §10.4 terminal `nondeterministic-render` classification exits the loop |
| C7 | major | Unkeyed same-call-site siblings share one StableId AND remote_id (dup guard keyed-only, view.cpp:3949) | ACCEPTED — §5.1: index-mixed fallback for every same-call-site sibling after the first + diagnostic; id index/remote map require unique-per-parent ids; positional fallback for parents with duplicates |
| C8 | major | Mode-R commits bump no version → stale shadow → oracle false-diverges after every drag | ACCEPTED — §9 commit contract step (3): every Mode-R commit invalidates the owning widget; "never touch versions" scoped to gesture-held X only |
| C9 | minor | Fate of ancestor DESC-DIRTY above a hidden boundary unspecified; both guesses lose | ACCEPTED — §3.4/§7: clear-with-park, reveal re-marks the chain, invalidate under a parked boundary early-outs |
| C10 | minor | Keyed-matching cursor discipline underspecified (backward moves, claimed nodes, interleaved text) | ACCEPTED — §5.3: claimed bitmap, cursor skips claimed, lookup hits don't advance, greedy-not-LIS noted, text/unkeyed positional in gaps |
| C11 | minor | uint32 version can wrap to applied_version while hidden | ACCEPTED — §3.2 uint64 versions + §7 flag backstop on reveal |
| C12 | question | Can user code run inside the sink window; what do blocked callers observe? | ANSWERED — §3.6/§8.1: no callbacks in stage 3 (destroy/focus notifications queued to next event phase); gate = in_render_ OR view_batch_active; per-API observables (invalidate deferred-latched, ref writes dropped + diagnostic) |
| C13 | question | Continuous oracle never hashes the lexbor DOM — sink bugs escape | ANSWERED — §6.5/§10.2: shadow≡lexbor deep verify promoted to first-class: continuous over touched set per window, sampled full-document; cost measured in §14 |
| C14 | question | Is widget identity parent-independent? Reparent destroys the widget | ANSWERED/DECIDED — §5.1: widget keys are globally scoped; slot survives reparenting; §5.3 step 4 cross-parent DOM move; recreate-on-reparent rejected |

### Performance lens (P1–P12)

| # | Sev | Finding | Resolution |
|---|---|---|---|
| P1 | blocker | Bare `K >= K_max` escalates scattered small widgets to O(document) root re-render per frame | ACCEPTED — §5.5 cost-monotone rule: node_count(A) ≤ C_esc·Σdirty is NECESSARY; K_max only triggers a covering search; hysteresis K_hi/K_lo; never-rendered widgets excluded from sums; scattered-17× bench added |
| P2 | blocker | "One recollect per window" bounds count not cost — the only recollect is full-document (document.cpp:9261) | ACCEPTED — §8.2: scoped recollect is a normative deliverable; structural-inside-collected-subtree distinguished; recycled rows never structural; phase-3 acceptance BLOCKED on it; full-recollect cost measured and published meanwhile |
| P3 | major | Mode-R commits never bump versions → oracle false divergence + rung drops per gesture | ACCEPTED — same resolution as C8/I5 (§9 commit contract; §14 zero-divergence interaction suite) |
| P4 | major | Deferred layout + find_element_rect hidden relayout = double layout outside the contract | ACCEPTED — §8.2: deferred layout runs at end_view_mutations before any geometry consumer; single shared ensure-layout (HUD-counted, T3-vetoable, marks deferral done); §14 one-layout-per-frame gate |
| P5 | major | Remove-on-unmount contradicts row recycling — scroll churns slots/segments/DOM at 140 Hz | ACCEPTED — §4.2 recycling override: bounded per-container pool, slot+segment retained by item id, DOM rebound, LRU eviction; default remains remove-on-unmount |
| P6 | major | 64 KiB block floor × no-interleave blows the bytes budget at fine granularity; no free-list trim | ACCEPTED — §11 size-classed blocks (256 B/4 KiB/64 KiB) or single-region slabs; 1-node-widget bytes stated (≲400 B); budget computed incl. floors; free-list decay + RSS-gated soak; §4.3 cell aligned |
| P7 | minor | Hash-compare on fresh emissions costs O(N) walk where structural compare early-outs | ACCEPTED — §5.3 step 3: hash gate cached-vs-cached only; fresh emissions structural-compare; lazy post-commit hashing; ~zero hash work on non-oracle frames (§6.3 note) |
| P8 | major | Exclusion granularity/lookup unspecified: name-level style exclusion = wrong DOM; selector match in hot loop | ACCEPTED — §9: ownership stamped at gesture begin (O(1) bit test at apply); style property-level with fixed owned sets; hidden-on-tab-body resolved to Mode R (deleted from §6.2) |
| P9 | minor | Descent is O(fanout) per spine node, not O(changed) | ACCEPTED — §3.4: bounded scan declared acceptable (contiguous slots, T5 virtualization guardrail); intrusive dirty-child list specified as the upgrade if profiling demands |
| P10 | question | Where do the phases hook into cb_frame's idle short-circuit; does invalidate() wake the frame? | ANSWERED — §2 pipeline driver: phases before the short-circuit; stage-1 gate first; reconcile output feeds renderer-dirty; wake-source contract |
| P11 | question | Is a continuous debug oracle viable during 140 Hz gestures / T4 docs? | ANSWERED — §6.5 schedule: continuous while frame time permits, settle-triggered + sampled during gesture streams (flag-controlled), CI always continuous, cost formula benched |
| P12 | minor | RenderFn re-bind per build = std::function heap churn against alloc gates | ACCEPTED (storage) — §3.2: inline SBO/arena closure storage, re-bind is a copy. The suggestion's no-re-bind-for-existing-slots half is REJECTED: A9 made always-re-bind normative (stale-capture correctness beats saving a copy); churn is eliminated by storage instead |

### Interaction-state lens (I1–I10)

| # | Sev | Finding | Resolution |
|---|---|---|---|
| I1 | blocker | Batch-end recollect destroys focus/caret/hover document-wide (reset_dynamic_block_state, :9251) | ACCEPTED — §8.2: element-keyed (DomHandle weak slot) snapshot/restore across the window incl. focus/caret/selection/hover; preferred migration of focused_idx/hovered_idx to weak handles; type-while-reconciling fixture (§14, Phase-1 acceptance) |
| I2 | blocker | Static attr registry can't express live=X/settled=R; §6.2 vs §9 contradict on dock sizes and `hidden` | ACCEPTED — same as C2: §9 temporal state machine; §6.2 rewritten; contradictions deleted |
| I3 | blocker | Interaction owns class TOKENS, not the attr — final-value class diff wipes open/active/drop state | ACCEPTED — §9 token-level ownership (pattern registry, token-set merge in diff, matching hash canonicalization §6.2) AND tree/foldout open state promoted to a Mode-R provider before the Phase-1 flip (§13) |
| I4 | blocker | Gesture state holds raw element pointers not cleared by destroy hooks → UAF on next mouse move | ACCEPTED — §9: versioned DomHandle weak slots mandatory for all gesture refs; sink remove cancels gestures on held nodes; §14 fuzz runs mutations during in-flight gestures under ASAN |
| I5 | major | Mode-R commits don't invalidate the owner; "next rebuild" no longer exists → oracle false-positives per tab switch | ACCEPTED — §9 commit contract; §14 scripted interaction suite asserts zero divergence events/rung drops |
| I6 | major | Index-based moves wrong in parents with interaction-created children; tearoff structure has no rule | ACCEPTED — §5.3 reference-node insertion normative (index = wire serialization only); §9 structural ownership paragraph (providers/adoption or region rebootstrap; D3 as Phase-1 dependency for tearoff apps) |
| I7 | major | Recollect discards animated style state → transitions restart/snap on every structural reconcile | ACCEPTED — §8.2 animation-continuity clause (element-keyed survival; reset only when the element's relevant property changed); §14 fixture |
| I8 | major | No controlled/uncontrolled rule for text inputs; replace destroys uncommitted typing; IME unreserved | ACCEPTED — §9 text-control rules: value gesture-held while focused, declared value DEFERRED to blur/commit (one rule, React-style declared-wins rejected), commit publishes + invalidates, never replace focused control on value-only diff, IME composition reserved as gesture-held |
| I9 | minor | Hide/reveal policy for focus and in-flight gestures inside hidden subtree unstated | ACCEPTED — §7: hide blurs (with observable commit) and cancels/completes anchored gestures; reveal never restores focus implicitly |
| I10 | question | Is the Mode-X enumeration complete? Six+ transient stores unlisted | ANSWERED — §9 completeness audit (every data-dcs-* behavior + every DocumentImpl transient store, with commit point and survives-move/replace) is a Phase-1 exit criterion (§13); table rows added for text/class-token/misc stores |

### API-contract lens (A1–A11)

| # | Sev | Finding | Resolution |
|---|---|---|---|
| A1 | blocker | WidgetRef writes erased by their own re-render and invisible to the oracle; mutation_sink_ is dead code | ACCEPTED — §5.2.1 overlay contract (option (a) of the suggestion): overlay keyed by (StableId, attr), re-applied post-render, oracle = fresh build + overlay replay, retire-on-redeclare; SHADOW-DIRTY added; dead-code fact recorded (§12#15) |
| A2 | blocker | Registry is attribute-only; structural surgery breaks shadow≡real with no version bump | ACCEPTED — §9 structural ownership (commit invalidates + segment discard + remote-id adoption/no-op for already-satisfied ops; open layers declared outside the reconciled tree); "never touch versions" scoped to gesture-held attrs |
| A3 | major | Emit-only WidgetOut has no handler surface; bans WidgetRef; "signatures do not change" false; whole-view handler harvest | ACCEPTED — §4.1: ElemScope subsumes chaining as emission (on_click/on_change legal under the gate, §3.6); per-widget handler storage in slots, dispatch via StableId, harvest deleted; honest migration statement replaces the overclaim |
| A4 | major | Create-emission ordering ambiguous; node-close option breaks RemotePatchQueue consumers | ACCEPTED — §5.2: segment-close top-down emission normative; node-close option deleted; parent-first/attrs-follow ordering clause; §14 replay-parity test |
| A5 | major | Dock-pane `style` simultaneously Mode X and the Mode R carrier; static registry can't express both | ACCEPTED — same as C2/P8: gesture-scoped exclusion + providers read live state at build time (belt-and-suspenders) |
| A6 | major | WidgetRef::append/replace unspecified; today's implementation already desyncs the fast path | ACCEPTED — §5.2.1: redefined as anonymous child widgets (option (a)); live-bug status recorded in §12#15 |
| A7 | question | Who runs stages 1–3; does invalidate() schedule a frame? | ANSWERED — §2 pipeline driver (engine-owned scheduling; rebuild_view = root invalidate, kept for compat) |
| A8 | question | Standalone View + RemotePatchQueue widget model? Protocol versioned across Phase 0? C ABI? | ANSWERED — §12#10 DECIDED: widget model is View-owned, App only schedules (no third path); §13 Phase 0 ships begin/end markers + Move op with the first protocol-visible change; §12#17 C-ABI row added |
| A9 | question | Is mount's closure re-bound when the parent re-renders over an existing child? | ANSWERED — §4.2 normative: always re-bound on parent render; stable-reference capture convention documented; oracle/VERIFY_SKIPS named as the stale-capture detector |
| A10 | minor | document_attrs_ diffing misses framework-derived attrs and <head> changes | ACCEPTED — §12#8: diff the RESOLVED attr set; decided as a sink op; shell-rebuild-only-inputs table added after §12 |
| A11 | minor | Root-widget apps violate G3 through Phase 3; phase reference wrong | ACCEPTED — §13 interim mandate statement; dirty-widget alloc gate pulled into Phase 1; phase-2→phase-1 reference fixed |

---

## B. Questions for the skeuomorphic-agent review

The skeuo agent owns the current T1 implementation (src/app/view.cpp,
src/app/app.cpp, src/dom/document.cpp) and reviews this document before
implementation. Items where its implementation knowledge should **confirm
or veto**:

1. **Per-op sink cost.** §8.2 budgets ≤ 5 µs bookkeeping per window op and
   ≤ 2 ms scoped settle for a single-widget region on DENDER. Given
   `set_attribute_on_element`'s classification path (:10025–10156), is
   classification-on-receipt separable from its restyle side effects
   without duplicating the classifier?
2. **Scoped recollect feasibility (§8.2, open question 8).** Can
   `impl.blocks` + style store be patched per-subtree with block-index
   preservation, or do consumers assume dense/ordered indices (hit-testing,
   focus routing, renderer display lists) forcing a remap table? What is
   the realistic cost of one full `recollect_blocks_from_current_dom` on
   DENDER today?
3. **SuppressDomStyleAttach limits.** The window already suppresses eager
   per-insert selector matching (:18468–18472). Does deferring ALL
   rematch/restyle to batch end interact badly with anything that reads
   style mid-window (reveal checks, pseudo state bits, dock machinery)?
4. **Transient-state snapshot/replay (§8.2).** Confirm the :9265–9286
   per-element snapshot pattern extends cleanly to focused_idx/caret/
   selection and AnimatedStyle entries — and whether migrating
   `focused_idx`/`hovered_idx` to DomHandle weak slots is tractable or
   collides with block-index-based key routing (:7894).
5. **Reference-node insertion (§5.3).** Any lexbor constraint on
   `insert_before` with foreign siblings present — especially around
   raw-html fragment groups (:18303–18350) and eventless inserts?
6. **Remote-id adoption for tearoff (§9).** Can stable ids for
   interaction-created float wrappers be derived from pane/panel ids with
   the current `tear_off_panel` code (:13886–13989), or is the localized
   region-rebootstrap fallback the realistic Phase-1 answer?
7. **Dock resolver determinism incl. floats.** Can D3 float/tearoff state
   be fully provider-described so declared == real after a tearoff commit
   (§9), and does the resolver replay a floating arrangement
   deterministically?
8. **Document-attr sink op (§12#8).** Any obstacle to
   `set_document_attribute` inside the window — body-attr selector
   classification, restyle scope = whole document by definition?
9. **Gesture-held bit storage (§9).** Cheapest home for the per-element
   gesture-held owner mask: element bookkeeping, block flags, or a side
   map — given the diff needs an O(1) test per matched node?
10. **Ensure-layout unification (§8.2).** Does routing
    `find_element_rect`/dispatch geometry through one shared ensure-layout
    conflict with the existing hidden-relayout contract (:16773–16796),
    and can that layout be scoped rather than full-document?
11. **lexbor node moves (§15.5).** Does reparenting break any lexbor
    bookkeeping (style attach, event targets, fragment groups)? Cost of
    move vs remove+create at DENDER scale?
12. **cb_frame ordering (§2).** Confirm S1–S3 can run before the idle
    short-circuit (:724–729) without breaking settle_frames/animation
    accounting, and that reconcile output can feed `impl_->dirty` cleanly.
13. **Escalation slope (§5.5).** Confirm emission cost is ~linear in node
    count in the current builder, and measure the per-region fixed
    overhead that would justify (or delete) `K_max`.
14. **Class-token enumeration (§9).** Can the interaction layer enumerate
    its owned token patterns per behavior exhaustively (the §9 audit), or
    are tokens constructed dynamically anywhere (string-built modifiers)
    that a pattern registry would miss?
