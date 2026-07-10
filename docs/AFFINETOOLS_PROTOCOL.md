# affinetools protocol — field-level schemas

The wire/dump schema reference for the affinetools protocol
([AFFINETOOLS_DESIGN.md](AFFINETOOLS_DESIGN.md) §3; resolution item §6.5).
This file is the source of truth for every field an agent, panel, or dump
consumer may rely on. **A stage is not done until its domains' schemas are
recorded here.** Schemas are versioned; breaking a field is a version bump,
never a silent change.

Status: **protocol v0 — telemetry + log domains implemented; dom / css /
resource READ domains implemented** (file profile via R1; wire profile via
the S1 server, `src/core/tools/tools_server.cpp`, read servicing in
`src/renderer/dom/document_tools.cpp`; client reference:
`scripts/affinetools_cli.py` + `extras/tools`; enforcement:
`tests/test_telemetry.cpp` + `tests/test_tools.cpp`).

## Profiles

- **JSONL file profile** (implemented): `AFFINEUI_TELEMETRY=<path>` streams
  one JSON object per line. First line is always a `session` record. Lines
  are complete and flushed — a crash loses at most the line being written.
  `AFFINEUI_TELEMETRY_EVERY=N` samples `frame` records (writes every Nth);
  `session` and `idle` records are never sampled out.
- **Wire profile** (implemented): 4-byte **LE length prefix** + UTF-8 JSON
  document per frame, over loopback TCP. Requests
  `{"id":N,"method":"...","params":{...}}` → responses
  `{"id":N,"result":{...}}` or `{"id":N,"error":{"code":C,"message":"..."}}`;
  events are id-less `{"method":"...","params":{...}}` where `params` is
  exactly a file-profile record. **Max inbound frame 1 MiB**; a zero-length,
  oversized, or malformed frame drops the connection (fail closed — the
  prefix is untrusted input inside the probed process).

## Attach & auth

Targets opt in with `AFFINEUI_TOOLS_LISTEN=1` (ephemeral port) or `=<port>`,
or programmatically `affineui::tools_listen(port)`. Binding is loopback
only. The target writes a discovery file
`<tempdir>/affineui-tools/<pid>.json`:

```json
{"pid":18124,"port":52731,"token":"<32 hex>","exe":"C:\\...\\app.exe",
 "affineui":"0.0.1"}
```

The first message on a connection must be
`hello {token, client}` — any other method, or a wrong/missing token,
disconnects without a reply. One client at a time; extra connections are
closed on accept. The file is removed on clean shutdown; stale files from
crashed targets simply fail to connect.

### Methods (v0)

| method | params | result |
| --- | --- | --- |
| `hello` | `token` (required), `client` (informational) | `protocol` (int, 0), `affineui`, `session_id` (16 hex), `capabilities` (["telemetry","log","dom","css","resource"]), `t0_wall` |
| `ping` | — | `{}` |
| `telemetry.subscribe` | — | `{}`; `telemetry.frame` / `target.idle` events start flowing |
| `telemetry.unsubscribe` | — | `{}` |
| `log.subscribe` | — | `{}`; `log.line` events start flowing (recent buffered history first) |
| `log.unsubscribe` | — | `{}` |
| `dom.document` | — | `{root: Node}` — the `<body>` root, shallow (§dom) |
| `dom.children` | `nid` | `{nodes: [Node…], truncated?}` — tree children of `nid` |
| `dom.html` | `nid` (optional) | `{html, truncated?}` — serialized outer HTML (whole document when `nid` absent) |
| `css.box_model` | `nid` | `{laid_out, rect, visual, margin, border, padding, scroll_y, content_h}` (§css) |
| `resource.stylesheets` | — | `{sheets: [{index, origin, label, bytes}…]}` (§resource) |
| `resource.stylesheet_text` | `index` | `{index, text, truncated?}` |

**Read-command semantics.** dom/css/resource methods are queued (bounded,
32 in flight — overflow answers error `-32005 "busy"`) and serviced by the
app thread at its next frame boundary via `affineui::tools_pump()` — on
idle-short-circuited frames too, so a quiet target still answers within
one display tick. Every read is **read-only-never-relayout** (DESIGN
§2.4): geometry is the last-laid-out state; nothing mutates the document.
Errors: `-32010 "stale node"` (the nid's versioned slot no longer resolves
— refetch from the root), `-32011 "no document"`, `-32012 "no such
stylesheet"`.

### Events (v0)

| event | params |
| --- | --- |
| `telemetry.frame` | a `frame` record (below) per presented frame |
| `target.idle` | an `idle` record — ≤1 Hz heartbeat while the target idle-short-circuits |
| `telemetry.dropped` | `{count}` — records lost to ring overflow while the reader was slow (DESIGN §2.2 drop-oldest; the app thread never blocks) |
| `log.line` | a `log` record (below) — one AffineUI diagnostic line, frame-stamped |
| `log.dropped` | `{count}` — log lines lost to the budgeted ring (1024 lines) |

## `log` — one diagnostic line (`log.line`)

AffineUI's own warnings/errors/parse diagnostics (the `affineui::log`
facility). Each is stamped with the presented-frame index current at
emission, so the panel can align a line with the performance graph and
select that frame on click. Budget: the target keeps the most recent 1024
lines; overflow is reported via `log.dropped`.

```json
{"level":"warn","frame":412,"t_ms":6883.21,"text":"font 'x' NOT LOADED"}
```

| field | type | meaning |
| --- | --- | --- |
| `level` | string | `debug` / `info` / `warn` / `error` |
| `frame` | u64 | presented-frame index at emission (0 = pre-first-frame) |
| `t_ms` | ms | time since session `t0` |
| `text` | string | the diagnostic message (JSON-escaped) |

## Conventions

- All timestamps are milliseconds relative to the session origin `t0`
  (steady clock). The `session` record anchors `t0` to wall clock.
- Times: `double` ms with 2-decimal precision. Durations in µs are
  integers. Sizes in bytes are integers.
- Fields may be *added* within a schema version; consumers must ignore
  unknown fields. Removal/renaming/retyping bumps `v`.

---

## `session` — preamble record / future `hello` payload

First line of every dump; sent on every future connection.

```json
{"v":1,"type":"session","schema":"telemetry/1","affineui":"0.0.1",
 "platform":"windows","pid":18124,"t0_wall":"2026-07-04T19:22:31Z"}
```

| field | type | meaning |
| --- | --- | --- |
| `v` | int | record schema version (this table: 1) |
| `type` | string | `"session"` |
| `schema` | string | dump content schema id, `"telemetry/1"` |
| `affineui` | string | library version (`AFFINEUI_VERSION_STRING`) |
| `platform` | string | `windows` / `macos` / `linux` / `web` / `unknown` |
| `pid` | int | process id (discovery correlation) |
| `t0_wall` | string | UTC ISO-8601 wall-clock anchor for `t_ms` origin |

Reserved for the wire `hello` result (same object + these): `protocol`
(int), `session_id` (string), `capabilities` (string array).

## `frame` — per presented frame (`telemetry.frame`)

Assembled in `cb_frame` after `render_to`; also readable in-process via
`App::frame_telemetry()`. One record per *presented* frame — idle
short-circuited callbacks do not emit `frame` records (see `idle`).

```json
{"v":1,"type":"frame","frame":412,"t_ms":6883.21,"gap_ms":6.94,
 "cb_ms":1.41,"skipped":3,"fb":[1280,720],"dpi":1.25,
 "stage_us":{"prep":10,"layout":120,"dl":80,"rast":300,"comp":90},
 "ops":{"cached":412,"culled":10,"changed":3,"rects":2,"dirty_pct":3.40},
 "flags":{"rec":1,"dl":1,"rast":1,"partial":0,"direct":0,"reused":0,"anim":0},
 "mem":{"live":9123456,"blocks":1284,"allocs":439,"frees":420}}
```

| field | type | source | meaning |
| --- | --- | --- | --- |
| `v` | int | — | frame schema version (this table: 1) |
| `type` | string | — | `"frame"` |
| `frame` | u64 | counter | presented-frame index (monotonic, 1-based) |
| `t_ms` | ms | steady clock | time since session `t0` at callback entry |
| `gap_ms` | ms | steady clock | **wall-clock gap** since the previous frame callback entry (R0 — the truth metric; a stalled pipeline shows here even when durations look healthy) |
| `cb_ms` | ms | steady clock | this callback's duration (event work + render + present handoff) |
| `skipped` | u64 | counter | idle short-circuits since the previous presented frame |
| `fb` | [int,int] | swapchain | framebuffer size, physical px |
| `dpi` | float | platform | pixels-per-point scale |
| `stage_us.prep` | µs | RenderStats | document prepare |
| `stage_us.layout` | µs | RenderStats | layout pass |
| `stage_us.dl` | µs | RenderStats | display-list record |
| `stage_us.rast` | µs | RenderStats | root-layer rasterize |
| `stage_us.comp` | µs | RenderStats | composite |
| `ops.cached` | u32 | RenderStats | cached display-list ops |
| `ops.culled` | u32 | RenderStats | ops culled this frame |
| `ops.changed` | u32 | RenderStats | display-list diff changed ops |
| `ops.rects` | u32 | RenderStats | dirty rects |
| `ops.dirty_pct` | float | RenderStats | dirty area, % of viewport (2 dp) |
| `flags.rec` | 0/1 | RenderStats | document paint recorded this frame |
| `flags.dl` | 0/1 | RenderStats | display list changed |
| `flags.rast` | 0/1 | RenderStats | root layer rasterized |
| `flags.partial` | 0/1 | RenderStats | partial rasterize |
| `flags.direct` | 0/1 | RenderStats | direct composite |
| `flags.reused` | 0/1 | RenderStats | root layer reused |
| `flags.anim` | 0/1 | RenderStats | animations active |
| `mem.live` | bytes | mem::stats | live payload bytes (affineui::mem seam — global new/delete not included until R2) |
| `mem.blocks` | u64 | mem::stats | live block count |
| `mem.allocs` | u32 | mem::stats Δ | allocations since previous frame record |
| `mem.frees` | u32 | mem::stats Δ | frees since previous frame record |

## `idle` — heartbeat (`target.idle`)

Emitted from the idle short-circuit path at most once per second while a
sink/subscriber is active, so "quiet" and "hung" are distinguishable
(DESIGN §2.3; TRACING blind spot §1.3).

```json
{"v":1,"type":"idle","t_ms":9120.50,"skipped":978}
```

| field | type | meaning |
| --- | --- | --- |
| `t_ms` | ms | time since session `t0` |
| `skipped` | u64 | idle short-circuits since the last presented frame |

---

## `dom` — DOM reads (v0)

Node identity is the wire **nid**: `[document_id, node_slot, generation]`
— the versioned-slot `DomHandle` (DESIGN §2.5). Handles stay valid across
mutations that keep the node alive; removal/destruction bumps the slot
generation and further requests answer `-32010`. There is **no live
`dom.patch` stream yet** (that requires the §3.3 mutation observer and its
audit) — clients poll/refetch on demand; a stale-nid error is the signal
to re-snapshot.

**`Node`** — one element or text node, shallow:

```json
{"nid":[1,4,1],"tag":"div","children":12,"rect":[0,36,439,20],
 "attrs":[["id","root"],["class","dcs-panel"]]}
{"nid":[1,9,1],"tag":"#text","children":0,"text":"Hello"}
```

| field | type | meaning |
| --- | --- | --- |
| `nid` | [u32,u32,u32] | versioned weak node id |
| `tag` | string | element tag, or `"#text"` for a text node |
| `children` | int | tree children (elements + non-whitespace text) |
| `rect` | [x,y,w,h] | last-laid-out border box, **visual** space (scroll/transform applied); zeros when the node has no box |
| `attrs` | [[name,value]…] | element attributes, document order (elements only) |
| `text` | string | text-node content, ≤160 bytes UTF-8-safe preview (text nodes only) |

Pure-whitespace text nodes are omitted. `dom.children` caps at 512 nodes
per reply (`truncated: true`); `dom.html` caps at 512 KiB.

## `css.box_model` — last-laid-out box numbers (v0)

```json
{"laid_out":true,"rect":[0,36,439,20],"visual":[0,36,439,20],
 "margin":[0,0,0,0],"border":[1,1,1,1],"padding":[8,16,8,16],
 "scroll_y":0,"content_h":0}
```

| field | type | meaning |
| --- | --- | --- |
| `laid_out` | bool | false = no box (display:none subtree, `<head>` …); other fields absent |
| `rect` | [x,y,w,h] | border box, document space |
| `visual` | [x,y,w,h] | border box, visual space |
| `margin` / `border` / `padding` | [t,r,b,l] | used values, px (`border` is the *used* width — 0 when the side's style is none) |
| `scroll_y` / `content_h` | px | scroll state for scrollable blocks |

## `resource` — stylesheet sources (v0)

`resource.stylesheets` enumerates in cascade order: author `<style>` /
`<link rel=stylesheet>` sheets in document order, then the user stylesheet
(framework bundle / app theme) last. `origin` ∈ `"style" | "link" |
"user"`; `label` is the href (or a positional label); `bytes` is `-1` for
a `<link>` until its text is fetched. `resource.stylesheet_text` resolves
`<link>` bodies through the document's resource loader and caps replies at
768 KiB (`truncated: true`).

---

## Planned domains (schemas land with their stage)

`dom.patch` events + dom writes / `view` / `overlay` / `input` (S2a
remainder — dom.patch is gated on the §3.3 mutation-path audit) ·
`css.matched` / overrides (S2b) · `perf` / `page` (S3) · `mem` snapshots /
`resource.textures`+`fonts`+`gpu` (S4, needs the R3 resource registries) —
see DESIGN §3.4 for method semantics. Each gets its own section here when
implemented.
