# affinetools — Out-of-Process DevTools for AffineUI

Design spec for the affinetools sidecar: a Chrome-DevTools-style inspector
and profiler for AffineUI apps. Status: **design v2 — revised after a
two-agent adversarial review (2026-07-03); disposition log in §8. Nothing
built.**

Companion documents:

- [TRACING_AND_PERFORMANCE_LOGGING.md](TRACING_AND_PERFORMANCE_LOGGING.md) —
  the *data plane* (counters, telemetry, spans, dumps). Everything there is
  core-library work; affinetools is strictly a consumer of it. Its design
  principles §1.6 (full compile-out) and §1.7 (near-free runtime gate, args
  never computed on the disabled path) govern all target-side code here.
- `design/affinetools/` — Claude-design HTML/JSX mockup (devtools.html). A
  *loose visual reference*. The real tool is built programmatically with
  the AffineUI widget framework — no hand-tweaked HTML. Being real
  decius.css in a browser, the mock doubles as a ground-truth A/B target.

Status legend used in tables below (a review finding was that v1 conflated
these): **✔ exists** (usable as-is in this context) · **◐ substrate**
(mechanism exists; adaptation required) · **✚ new** (must be built).

---

## 1. Decisions (settled)

1. **Out-of-process.** affinetools is a separate executable that attaches
   to a running AffineUI app. We do not modify the probed process: the only
   in-target piece is a dormant protocol server in the core library. No UI,
   no rendering target-side. Kills the observer-effect problem
   structurally, gives crash isolation (hard-to-crash extended across the
   process boundary), removes any need for multi-window UI support.
   *Honesty note (review):* the in-target server is still tools code
   running in the host — §2 and §3.1 therefore impose hard-to-crash rules
   on it (bounded allocations, max frame size, disconnect-not-assert).
2. **Data-plane first; the panel is a consumer.** Live panels, CLI dump,
   post-mortem file loading, AI-agent access, and future remote attach are
   the *same serialized protocol* with different consumers. One schema.
3. **CLI dump for AI agents is a hard requirement.** Any app with the perf
   subsystem compiled in can dump telemetry *and snapshots* to a file (env
   driven, no tools app involved). A dump is a recorded protocol session —
   including the session preamble and initial snapshots (§3.2) — and
   affinetools can open one.
4. **Perf recording must not be perturbed by the tools UI.** Rendering
   isolation is free (out-of-process). Target-side, pause/record governs
   only what is serialized and streamed; recording continues into rings.
5. **Gating.** Master compile switch (single-file convention: `#define`
   before include) — off means *no* perf/monitoring code in the build.
   Compiled in (always true for prebuilt Rust/C#/Python binding binaries),
   idle cost is one predicted global-bool branch per probe site, and the
   branch **skips the entire call — probe arguments are never computed on
   the disabled path**. Trivial variable-only writes (counter bumps,
   resets) may stay unconditional where a store is cheaper than a branch.
   *Refinement (review):* the fast gate is a single "any consumer active"
   bool; a per-domain consumer mask is checked only on the slow path and
   is **latched once per frame** by the app thread so every frame is
   atomically instrumented-or-not (no torn spans on detach). Rings are
   inert until a consumer subscribes; detach must not disable an
   env-driven JSONL dump (independent consumers, one mask).
6. **Packaging.** `affineui_tools.h` (+ `.cpp` if size demands) is a
   single-file sidecar library beside `affineui.h` — panels, protocol
   client, dump loader, embedded stylesheet/assets (bin2c). `affinetools.exe`
   is a thin shell around it. Developed multi-file, amalgamated for
   distribution, oriented single-file from the start.

---

## 2. Architecture

```
┌─────────────────── target process ────────────────────────┐
│  app ──> AffineUI core                                     │
│           ├─ instrumentation (TRACING doc)                 │
│           └─ tools server [master #ifdef + runtime gate]   │
│               ├─ server thread (created by tools_listen;   │
│               │   BLOCKED in accept/read when idle)        │
│               └─ frame-boundary pump on the app thread     │
└───────────────┼────────────────────────────────────────────┘
                │  length-prefixed JSON, loopback transport
┌───────────────┼────────────────────────────────────────────┐
│  affinetools.exe (an AffineUI app itself)                  │
│   protocol client ──> client-side model ──> View panels    │
└─────────────────────────────────────────────────────────────┘
        ▲                          ▲
   dump files (same           AI agents / scripts
   protocol, JSONL)           (same protocol)
```

### 2.1 Thread model (normative)

- `affineui::tools_listen()` creates **one server thread**. It blocks in
  accept/read when idle (the honest statement of "dormant"). It owns the
  socket, parses inbound JSON into **typed command structs**, and hands
  them to the app thread via a bounded SPSC queue (acquire/release).
  Queue full → immediate error response, command dropped. Shutdown:
  App/Document teardown closes the socket and joins the thread *first*.
- The app thread services commands in `affineui::tools_pump()` at the
  frame boundary. `App` calls it automatically inside `cb_frame`;
  **embedded hosts** (engine embedding, `render_to`-driven — which
  includes the prebuilt binding binaries this feature targets) call
  `tools_pump()` themselves once per frame on their UI thread. This is
  part of the embedding contract (EMBEDDING_DESIGN.md gets a section).
- Outbound: the app thread writes **fixed-size binary records** into
  fixed-capacity synchronized rings; the server thread drains rings and
  does all JSON formatting and socket writes **off the app thread**. The
  server thread never reads live structs (`Renderer::stats()`,
  `mem::stats()` internals, the DOM) — only rings and mailboxes populated
  at the frame boundary. (Review: v1's "drained off-thread" over plain
  structs was a data race; this is the fix.)

### 2.2 Backpressure (normative invariants)

- The app thread **never blocks on the transport** and **never allocates
  proportionally to client slowness**.
- Per-stream overflow policy: `telemetry` / `perf.spans` / `log` rings are
  **drop-oldest** with a `dropped` counter surfaced in the next event.
  `dom.patch` overflow **bumps the revision epoch**; the client detects
  the gap (seq discontinuity) and re-snapshots affected subtrees.
- Target-side budget while attached: app-thread tools cost **< 100 µs per
  frame at 140 Hz** under normal panel load. *Measured 2026-07-04:*
  **0.016 µs median** per frame (assembly + ring push; bench asserted in
  tests/test_tools.cpp), formatting 1.3 µs/record on the server thread.

### 2.3 Idle, wake, and honesty about observer effect

- The idle short-circuit in `cb_frame` emits a low-rate **heartbeat**
  (`target.idle`, ~1 Hz, includes skipped-present count per R0) so a quiet
  target is distinguishable from a hung one — the exact blind spot
  TRACING §1.3 documents, not reintroduced over the wire.
- Pending commands set a wake flag → the app schedules a frame; the
  command-latency contract is "next frame or heartbeat-interval,
  whichever is sooner." A throttled/minimized target reports
  `target.throttled` status rather than silently stalling.
- **Disclosed deviation:** an active attach with subscriptions or overlay
  forces per-frame rendering, exactly as `AFFINEUI_PERF_OVERLAY` does
  today (app.cpp idle gate). Zero *timing* distortion of measured phases
  remains the requirement; idle *behavior* changes while attached, and the
  status bar says so.

### 2.4 Queries must never mutate

- All geometry/style queries are **read-only-never-relayout**: they return
  last-laid-out values plus a `dirty` flag and never trigger the hidden
  relayout documented for `find_element_rect` (TRACING §2.4 `[rect]`).
  The next natural frame refreshes values.
- `input.pick(x,y)` uses a **pure hit test** (point → element chain, no
  synthetic MouseMove, no `:hover` restyle) — a small new Document API;
  `hovered_info_chain()` reads current state and is not it (review B9).

### 2.5 Node identity

`DomHandle{document_id, node_slot, generation}` (types.h) serializes as
`"nid":[doc,slot,gen]`. Stale nids resolve to nothing target-side (the
versioned-slot check). The client's mirror is guarded by the **session
id** (new hello field) — a target restart on the same port can never alias
into a stale mirror — plus per-event `seq`/`revision` (§3.2).

---

## 3. Protocol

### 3.1 Transport

Pluggable `Transport` interface; v1 is **loopback TCP with a mandatory
session token** (named pipe with user-only ACL is the recorded Windows
upgrade path if warranted).

- **Zero-config open (decided 2026-07-04, implemented):** every App-based
  app binds **F12 / Ctrl+Shift+I** by default (`Config::devtools_hotkey`,
  vetoable; compiled out with `AFFINEUI_PERF=0`) →
  `affineui::tools_open_devtools()`: starts the server on demand and
  launches the affinetools viewer attached to this pid, with no setup.
  Viewer lookup: `AFFINEUI_TOOLS_EXE` override → beside the app's exe →
  the build tree (walking up to `tools/affinetools/`) → PATH. Debounced;
  the one-client policy makes repeat presses harmless.
- *Listening* otherwise defaults **off**. Enable via
  `AFFINEUI_TOOLS_LISTEN=1` (or `=<port>`) or
  `affineui::tools_listen(port)`. Binds `127.0.0.1`. Default port is
  **0 (ephemeral)**; the chosen port plus a random per-session token and
  `{pid, exe, affineui_version}` are written to a discovery file under the
  user's temp dir (`affineui-tools/<pid>.json`, user-ACL'd). This solves
  multi-target discovery and port collisions in v1 (review A12), and the
  token answers loopback-on-multi-user-machines (review A6): `hello`
  without the correct token → disconnect. Embedders get a **host veto**
  (`Config` flag / compile define) that hard-disables env activation —
  game hosts will not ship an env-activatable input-injection port.
- Framing: 4-byte LE length + UTF-8 JSON. **Max frame 1 MiB**; violation
  or malformed JSON → drop connection, never assert, never large-alloc
  (the length prefix is untrusted input inside the probed process —
  hard-to-crash applies; review A9).
- Target-side command parsing needs a JSON *reader* in core: **decided —
  vendor a minimal reader in core behind the master switch**, shared by
  the tools lib (closes v1 open question §6.2). Parsing happens on the
  server thread into typed commands; the app thread never parses JSON.
- Shared-memory ring remains a *future optional fast lane* for bulk
  payloads only. Note from review (A10): the scarce resource is not socket
  bandwidth but **serialization time in the target** — hence §2.1's
  binary-records-in, JSON-out-off-thread rule and the §2.2 budget.

### 3.2 Message model

JSON-RPC-shaped, CDP-flavored:

```jsonc
{ "id": 7, "method": "dom.children", "params": { "nid": [1,482,3] } }
{ "id": 7, "result": { "revision": 91, … } }
{ "method": "dom.patch", "params": { "seq": 4021, "revision": 92, … } }
```

- Handshake: `hello {protocol, token, client}` →
  `{protocol, affineui_version, session_id, capabilities, t0}` where `t0`
  anchors the steady-clock timebase to wall clock. All wire timestamps are
  relative to session `t0` (fixes dump/Perfetto correlation; review A13).
- **Ordering & coherence (review A5/B2):** all dom responses *and* events
  are serialized at the frame boundary in mutation order (single writer);
  every dom event carries a monotonic `seq`, and both patches and
  `dom.children` responses carry the document `revision`, so
  snapshot-vs-stream races are resolvable client-side. Gap in `seq` (ring
  overflow) → client re-snapshots.
- **Dump = recorded session.** Every dump file begins with a session
  preamble line (the hello payload + platform + DPI + wall-clock start);
  a dom-recording dump includes the initial snapshot. `AFFINEUI_TELEMETRY=
  <path>` (R1) is the telemetry-only profile of the same format.
- Capabilities gate optional domains (and are used honestly: e.g. `mem`
  snapshots only when the target was built with `AFFINEUI_MEM_DEBUG`;
  the panel says "rebuild with MEM_DEBUG for snapshots" instead of
  showing empty data — review A14).

### 3.3 The DOM mutation observer (the load-bearing new piece)

The adversarial review's central finding (A1/B2, confirmed against
view.h): **`RemotePatchSink` cannot source `dom.patch`.** It observes only
View-reconciler mutations — the interaction layer (data-dcs-* handlers,
dock drag surgery, menu/popover toggles, document scripts) mutates the
Document below the View and would silently desync the mirror; its patches
are keyed by View string ids, not nids; and it skips raw-HTML subtrees by
design. The op *vocabulary* survives; the mechanism must be built:

- A core **document mutation observer**: nid-keyed events
  `{create_element, create_text, move, set_text, set_attribute,
  remove_attribute, remove, subtree_replaced}` with **sibling-anchor
  inserts** (not indices) and a `move` op (reorders must not destroy
  client tree state), emitted from the document mutation seam
  (`set_attribute_on_element`, structural commit in `end_view_mutations`,
  parse-in-place paths emit `subtree_replaced` → client refetches).
  This is the same seam `AFFINEUI_MENU_TRACE` already instruments —
  "instrument the layer that owns the cost" (TRACING §1.5).
- **Audit requirement:** every mutation path must route through the
  observer; if the audit finds bypass paths, the fallback is a
  lexbor-level observer in the affineui_lexbor fork (precedent: the
  `event_destroy` patch lives at that layer).
- The observer increments the document `revision`; it is entirely inside
  the master switch and per-domain mask (zero cost unless a dom
  subscriber exists).

### 3.4 Domains

| Domain | Methods / events | Status / backing |
| --- | --- | --- |
| `telemetry` | `subscribe/unsubscribe`; ev `telemetry.frame`, `target.idle` | ◐ R1 `FrameTelemetry` (RenderStats ✔ + mem::stats ✔ + R4) |
| `dom` reads | `document` (shallow), `children(nid)` (lazy), `attributes`, `html(nid)`, `ancestors(nid)`, `query(selector)→[nid+rect]`, `search(text|selector)→[nid]` (target-side — lazy clients can't search client-side; review B8) | ◐ walk + `find_element_rect` matcher as the selector seed; search ✚ |
| `dom` writes | `set_attribute`, `remove_attribute`, `set_text` (nid-keyed) | ◐ thin wrappers over `set_attribute_by_id`-class mutators (document.h:201-210) — **live editing is table stakes, not a luxury** (review B1); S2a |
| `dom` events | `dom.patch` (seq + revision) | ✚ §3.3 observer |
| `css` | `computed(nid|selector)`, `box_model(nid)`, `layout(nid)` (Yoga in/out, dirty) | ✚ serialization layer — `ComputedStyle` is `detail::`, deliberately unexported (review B3); public read-only serializer to build, honoring §2.4 |
| `css` (S2b) | `matched(nid)` (rules, specificity, origin, winner), `override` (devtools-owned override sheet w/ per-declaration disable), `force_state(nid, pseudo)` | ✚ gated on the cascade investigation (§6.1) — provenance retention and the override mechanism are the *same* cascade-layer design task |
| `view` | `tree` (component kinds, widget keys, `source_location`, local_dom nids), `node_for(nid)` reverse lookup | ◐ **provenance — the killer feature for a programmatic framework** (review B4): View builders already capture `std::source_location` and `WidgetNode` carries `widget_name` + DomHandle; verify retention through reconcile, serialize at frame boundary. S2a |
| `overlay` | `highlight(nid|rect)` (box-model tinting), `clear` | ✚ new debug-draw primitive — `draw_debug_overlay` is the *text HUD* only (review B3); build a rect/tint layer on the same DOM-bypassing path (TRACING §1.2 principle transfers, the capability is new) |
| `perf` | `spans.subscribe` (live span *timeline strip*), `export.perfetto` | ◐ R5 ring. **Flame contradiction resolved** (review B6): TRACING R5 says don't build a flame viewer — adopted. In-tool = lightweight last-N-frames span strip; deep flame analysis = Perfetto export. The mock's flame panel is superseded |
| `mem` | `stats`; `snapshot`/`diff` capability-gated on `AFFINEUI_MEM_DEBUG` | ◐ memory.h ✔ / gaps per TRACING §2.3 + R2 (global-new blind spot stated in panel until R2) |
| `resource` | `fonts`, `images`, `stylesheets`, `gpu` | ✚ registries + R3 |
| `input` | `click/drag/key/type(selector…)`, `pick(x,y)` | ◐ UiScript's *selector/anchor/drag-interpolation logic* is reusable; the dispatch path is **new**: an app-thread injection queue at the frame boundary, drags spread across N frames with completion events, ordered against real input, DPI-converted, **no forced relayout** (UiScript is an in-process test harness and does not transplant — reviews A15/B9) |
| `page` | `screenshot` (PNG over socket; slow is fine) | ✚ — without pixels an AI agent can't close a visual-debugging loop (review B7) |
| `log` | `subscribe` → `{tag, level, text, t}` | ◐ framework diagnostics only: `View::diagnostics()` + the `[tag]` trace-line sinks get a structured mirror (review B12). App logging stays out of scope — apps own their logging |
| `objects` *(future)* | enumerate/get/set reflectables via `ObjectClass` | ◐ object.h reflection exists and is wire-friendly (bool/double/string); deliberate S4+ differentiator (app-state inspection à la GammaRay), recorded so its omission is a decision, not a miss (review B11) |

---

## 4. The affinetools app

A standalone AffineUI application built with the programmatic View /
component framework — deliberately the framework's hardest dogfood.

**Data flow.** The protocol client maintains a client-side model (partial
mirror). Panels are pure View functions over it; updates re-reconcile.
"Paused" = stop applying model updates → zero reconcile ops.
**Named dependency (review B13):** the panel-refresh claim (~81 µs/280
nodes) is the View *diff*; the current `App::load_view` still reparses.
The **persistent-View reconcile fast path (DocumentViewSink + App fast
path)** is a prerequisite for S2's live panels and is tracked as such —
affinetools must not jank while measuring jank.

**Corrected component inventory** (review B3 — v1 overclaimed):

| Panel | Exists ✔ | Substrate ◐ / Needs building ✚ |
| --- | --- | --- |
| Shell | DockPanel ✔, status bar CSS ✔ | ✚ **Tabs** component (none exists outside dock panes; Elements' Styles/Computed/Layout pane needs it too) |
| Elements | Foldout ✔, splitters ✔ | ✚ **Tree component** — the Tree>TreeItem-with-lazy-`on_open` design is *decided, not built* (components.h has no Tree; view.h has flat `tree_row` only). Lazy `on_open` → `dom.children` fetch. ✚ box-model diagram (trivial) |
| Performance | Buttons/toggles ✔ | ◐ VirtualList builder exists but exposes **no scroll callback** — fetch-on-scroll needs one (small core addition). ✚ **canvas component** (below) for the frametime timeline + span strip |
| Memory | table CSS ✔ | ✚ **Table** component: VirtualList-backed, column model, sortable, thousands of rows — a component, not a wrapper |
| Sources (S4) | — | ✚ code/text viewer (mono, line numbers, gutter marks, selection/copy, h-scroll) |

**Canvas component contract** (must be specced before S3 — review B6):
built on `set_custom_paint`/`request_custom_repaint` ✔, but the *event*
side is the real design: `on_pointer(local x, y, phase)`, `on_wheel`,
cursor hints, and the discipline "paint reads state; events mutate state;
`request_custom_repaint` schedules" — tooltips render as DOM overlays
mutated outside the paint handler (custom paint must not touch the
Document, per its own contract).

**Graduation criteria** for new widgets (review B10): a widget moves from
`affineui_tools` to core when it is used by ≥2 surfaces, has a
personality-mapper recipe (or a recorded Decius-only decision), and has
doctest coverage. Tabs, Table, Tree, and the canvas component are all
expected to graduate quickly; no tools-only half-widget lingers (no-bodges).

---

## 5. Staging

- **S0 — data plane** *(tracing agent, core)*: TRACING R0–R6. R1 defines
  the telemetry payload; R5 feeds the span strip; R4 is the "why was this
  frame slow" row.
- **S1 — spine**. *Status 2026-07-04 (late): **S1 COMPLETE except the
  DocumentViewSink fast path** (reconcile workstream). Spine verified
  end-to-end; §6 items 1/2/4/7/8 all resolved; the `affineui_tools`
  sidecar lib (extras/tools: discovery + protocol client + live model +
  dump summaries) and the `affinetools` viewer (tools/affinetools:
  decius shell, tab strip, live telemetry readout, `--open` dump
  summaries) are BUILT and verified: F12 injected into an unconfigured
  example spawned the viewer, which attached and rendered live data
  (screenshot-verified). First dogfood catch: text-update overdraw →
  PERF_RELIABILITY_TICKETS.md T11.* *Entry criterion: §6 item 1 (R1
  schema agreed).*
  Transport + server thread + typed-command pump + hello (token, session,
  capabilities) + telemetry subscribe + heartbeat; `affineui_tools` shell
  attaching, live status bar; **checked-in `scripts/affinetools_cli.py`**
  (attach → command → JSON → exit — the agent recipe, shipped not
  promised); dump replay; `AFFINETOOLS_PROTOCOL.md` created (§6 item 5).
  *Run during S1:* the cascade investigation (§6 item 2), the
  `source_location` check (§6 item 4), and the DocumentViewSink fast-path
  work. *Exit criteria: §6 item 7 (budget measured) and item 8 (binding
  wrappers) done.*
- **S2a — Elements without the cascade question** (review B5 split).
  *First task: §6 item 3 (mutation-path audit) — before any dom.patch
  code.* DOM mutation observer + revision/seq machinery; Tree + Tabs
  components; lazy tree + `dom.search`/`query`/reveal-in-tree; computed
  styles + box model + Yoga view (read-only serializers); `view`
  provenance domain + badges; overlay highlight primitive +
  pure-hit-test pick; **dom write methods** (attribute/text editing in
  the panel).
- **S2b — Styles proper**. *Entry criterion: §6 item 2 resolved.* Matched
  rules w/ winner + specificity + origin; `css.override` (per-declaration
  toggle/edit); `force_state`.
- **S3 — Performance panel**. *Entry criterion: §6 item 6 (canvas
  contract written).* Canvas component, frametime timeline + span strip,
  pause/inspect, Perfetto export, `page.screenshot`, `log` drawer.
- **S4 — the rest**: Memory (stats now, snapshot/diff behind capability,
  R2 histograms when they land), Resources, Sources + layout watches,
  full remote-drive `input` UI, `objects` domain exploration.

---

## 6. Open items — resolution schedule

Every unresolved item is deliberate: it either needs information only an
investigation or implementation can produce, or it isn't load-bearing
until a later stage. Each has a **gate** — the point where enough
information exists *and* the answer becomes needed. An item unresolved at
its gate **blocks that stage**; nothing on this list may slip silently.
The gates are wired into §5 as entry/exit criteria.

| # | Item | What resolves it | Gate (resolve by) | Blocks | Answer recorded in |
|---|------|------------------|-------------------|--------|--------------------|
| 1 | ✅ **RESOLVED 2026-07-04** — R1 `FrameTelemetry` wire schema: defined in [AFFINETOOLS_PROTOCOL.md](AFFINETOOLS_PROTOCOL.md) (`session`/`frame`/`idle`, telemetry/1) and implemented (telemetry.h + diag/telemetry.cpp + `App::frame_telemetry()` + `AFFINEUI_TELEMETRY` sink; enforced by tests/test_telemetry.cpp). S1 entry criterion met | — | — | S1 unblocked | done |
| 2 | ✅ **RESOLVED 2026-07-04** (investigation, 2026-07-04): AffineUI's resolver discards provenance (`cascade.cpp` walk drops the weak chain, folds winners into flat `ResolvedStyle`), **but lexbor's matched store retains everything** — per-property AVL with winning declaration + specificity + a specificity-sorted loser chain (`html/style.h` weak list), and every declaration back-links to its style rule + selector (`declr→rule.parent→parent`). **Retention mode = LOW effort** (~100-150 lines: walk `with_weak=true` behind an attach flag, record `{declr,spec,is_weak}`, group by source rule, bypass `LexborResolver::cache_` for inspected elements; the `:hover` overlay path at document.cpp:8586 must also be covered). **Override = devtools-owned stylesheet appended last** in `document→css.stylesheets`, per-declaration disable by re-emitting the sheet minus that declaration (cascade-layer; `apply_decl_list` rule-mask is the lighter alternative). **Hard limit:** the parser never stamps `rule.begin` → **file:line of rules is NOT available**; S2b ships selector-text + specificity + origin-index provenance, and a small fork change (stamp `rule.begin` in `lxb_css_stylesheet_qualified_rule_end`) is the follow-up that unlocks `decius.css:412` display. Note: `src/engine/style_engine.cpp` is fully stubbed — the live cascade is `LexborResolver`, not `StyleEngine` | — | — | S2b unblocked | done (this row + §3.4) |
| 3 | **Mutation-path audit** — do all Document mutations route through the §3.3 seam, or is the lexbor-level fallback needed? | Audit of every mutation path in document.cpp incl. raw-HTML parse-in-place | **First task of S2a**, before any dom.patch code | dom.patch implementation layer | §3.3 amended with the audit result |
| 4 | ✅ **RESOLVED 2026-07-04**: was NOT retained (only hashed into StableId); fields added — `WidgetNode::src_file/src_line` populated in `View::open_node` (file_name() is a static literal, so retention is pointer+int, no allocation). `view.tree` provenance data now exists on every node | — | — | S2a `view` domain unblocked | done |
| 5 | **Field-level message schemas** — this spec names methods and semantics, not every JSON field | Each domain's implementation pins its schema in a versioned protocol reference, agent-consumable | **As each domain lands**: S1 hello/telemetry/log-of-session; S2a dom/view/overlay/input; S2b css; S3 perf/page/log; S4 mem/resource | Per-domain: agent consumers + dump-format stability | `docs/AFFINETOOLS_PROTOCOL.md` — created in S1 with the first two domains, one section per domain thereafter; a stage is not done until its domains' schemas are in this file |
| 6 | **Canvas component event contract** (`on_pointer`/`on_wheel`/cursor hints; paint-reads-state, events-mutate-state, `request_custom_repaint` schedules) | S2a panel-building experience + a written contract | **S3 entry criterion** | S3 charts | §4 revised + a component design note beside components.h |
| 7 | ✅ **RESOLVED 2026-07-04 — measured**: app-thread cost/frame while attached (telemetry assembly + ring push) = **0.016 µs median** (micro-bench in tests/test_tools.cpp, asserted <100 µs every run); JSON formatting = 1.3 µs/record on the *server* thread per §2.1. Live corroboration: 653-frame capture showed cb_ms ≈ 0.06–0.1 ms *total* callback incl. render with a subscriber attached | — | — | S1 exit criterion met | done (+§2.2) |
| 8 | ✅ **RESOLVED 2026-07-04 — verified live**: C ABI `affineui_tools_listen/active/port/shutdown` (c_api.h §affinetools); pybind `affineui.tools_*` module functions + `App.frame_telemetry()` dict (keys mirror the telemetry.frame schema) + `.pyi` stubs + package re-exports. Verified: Python process `tools_listen()` → CLI attach + ping 0.1 ms → clean shutdown. Note: Ui-facade / `render_to` hosts can *listen* but don't yet *feed* telemetry (frame assembly is App-only) — that's the `tools_pump()` embed-contract work, tracked for S2a | — | — | S1 exit criterion met | done (LANGUAGE_BINDINGS.md entry pending that doc's next revision) |
| 9 | **Multi-document surface** (`documents.list`, per-document subscribe) — `DomHandle.document_id` already scopes nids | A real multi-Document app existing | **Deferred — gate on existence**; re-check at each stage boundary | Nothing today | §3.4 |
| 10 | **Remote attach + pipe/ACL transport upgrade** | A real console/devkit embedding case + a threat story | **Deferred — gate on use case**; §3.1's token/discovery was chosen to extend, not rework | Nothing today | §3.1 |

*(Closed in v2: JSON reader — vendored in core behind the master switch;
port discovery — port-0 + temp-dir session file.)*

---

## 7. Review disposition (2026-07-03)

Two independent adversarial reviews (systems/protocol lens; product/
tooling lens) produced 28 findings. Accepted in full: backpressure +
thread-ownership model (§2.1–2.2), idle heartbeat/wake + disclosed
observer deviation (§2.3), never-relayout queries + pure hit test (§2.4),
seq/revision/session coherence (§3.2), token + port-0 discovery + host
veto + max-frame/hard-to-crash parsing (§3.1), DOM mutation observer
replacing the RemotePatch ✔ claim (§3.3), dom writes / css override /
force-state (§3.4, §6.1), `view` provenance domain, `dom.search` +
selector one-shots + `page.screenshot` + CLI script (agent loop), honest
component inventory + canvas contract + Table scope + graduation criteria
(§4), S2a/S2b split + fast-path dependency (§5), mem capability gating,
log domain (framework diagnostics only), objects domain as recorded
future. Modified rather than adopted verbatim: transport stays loopback
TCP + token for v1 (named pipe + ACL recorded as the Windows upgrade
path, not the default — one transport everywhere beats two in v1);
in-tool flame graph *cut* in favor of span strip + Perfetto (resolves the
cross-doc contradiction in TRACING R5's favor); `log` domain scoped to
framework diagnostics only (apps own their logging — user decision
upstream of this spec).
