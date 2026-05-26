# Conformance harness

Automated A/B testing of AffineUI's rendering against a **real browser**:
render the same test on both sides, pixel-diff them, and report *how much*
and *where* they differ. Built so many agents can each own a test in
parallel — run, see the diff, fix the lib, re-run — to drive features and
conformance into AffineUI rapidly.

## Pieces

| Part | Lang | Role |
|---|---|---|
| `tools/conformance` → `conformance_test` | C++ | **AffineUI side.** Headless render of a named test to a PPM per snapshot (offscreen D3D11 target + readback; no window). Replays interaction steps via synthetic `Ui::dispatch`. |
| `conformance/browser/shot.js` | Node + Playwright | **Browser side.** Loads the same test in real Chrome, replays the same steps, screenshots a PNG per snapshot. |
| `conformance/diff.py` | Python (Pillow+numpy) | **Differ.** Per-pixel absolute difference → metrics (changed %, mean/max delta) + a diff image (changed pixels tinted red). |
| `conformance/run.py` | Python | **Runner.** Drives both sides for a test (or all), diffs each snapshot, writes `out/report.html`, exits non-zero on failures. |

## A test

A directory `conformance/cases/<name>/` with `index.html` and an optional
`case.json`:

```json
{
  "width": 800, "height": 600, "dpi": 1.0,
  "tolerance": 2,           // ignore per-channel deltas <= this (AA noise)
  "threshold": 10.0,        // fail if > this % of pixels differ
  "steps": [
    { "snapshot": "initial" },
    { "click": [80, 136] },
    { "wait_ms": 50 },
    { "snapshot": "after-click" }
  ]
}
```

**Steps** are an ordered list of **actions** (`click [x,y]`, `hover [x,y]`,
`wait_ms`) and **`snapshot` markers**. **Both drivers self-load `case.json`**
(given `--test <name>`) and replay the identical list, emitting one image per
marker (`<test>.<driver>.<snapshot>`); the runner just orchestrates + diffs.
No marker ⇒ one snapshot named `default`.

Coordinates are **CSS pixels** so the browser and AffineUI act on the same
point. Interaction is driven by **synthetic input**: AffineUI dispatches
`MouseMove/Down/Up`; the browser uses Playwright's `page.mouse` (the standard
Chrome DevTools Protocol `Input.*` path).

`mouse_path [[x,y], ...]` replays a simple pointer trace in addition to the
basic `click`, `hover`, and `wait_ms` steps. `mouse_recording` replays the
full demo recorder format, including `move`, `down`, and `up` events:

```json
{
  "mouse_recording": [
    { "type": "move", "x": 120, "y": 80 },
    { "type": "down", "x": 120, "y": 80 },
    { "type": "up",   "x": 120, "y": 80 }
  ],
  "snapshot_prefix": "path"
}
```

If either trace step has
`"snapshot_prefix": "path"`, both drivers capture a snapshot after each point
as `path_000`, `path_001`, ...; the runner then builds browser/AffineUI/diff
filmstrips. This is the preferred way to turn an observed hover/drag repaint
bug into a deterministic regression test. Playback annotates mouse-down frames
with a transparent blue click crosshair so the filmstrip shows where the user
clicked without modifying the tested UI.

The Sokol demo adapter continuously records mouse movement in CSS pixels.
Press `R` to flush the current trace, or just exit the demo; cleanup writes
`affineui_mouse_path_<timestamp>_<n>.json` in the process working directory. Paste
that JSON object into a conformance case's `steps` array to replay the exact
movement.

### Extending the step vocabulary
The step set is intentionally **small and extensible** — we don't need every
interaction type up front; **agents add new ones as they go**. To add a step
type (named DOM interactions like `click "#id"`, `key`, `type`, `scroll`, …),
extend the dispatch in **both** `tools/conformance/main.cpp` (synthetic
`Ui::dispatch`) and `conformance/browser/shot.js` (Playwright). Unknown step
types are **skipped**, so a newer script degrades gracefully on an older
driver. *(Named/selector steps need AffineUI to expose element bounds /
hit-test; coordinates are the parity common-denominator today.)*

## Running

```
.\build.ps1 conformance              # build tool + browser deps, run ALL tests
python conformance/run.py --test X   # one test (an agent's inner loop)
python conformance/run.py --filter form
python conformance/diff.py a.png b.ppm --out d.png   # ad-hoc diff
```

Outputs land in `conformance/out/` (gitignored): `<test>.browser.<snap>.png`,
`<test>.affineui.<snap>.ppm/.png`, `<test>.diff.<snap>.png`, and
`report.html` (browser | AffineUI | diff, per snapshot, with pass/fail).

## The parallel-agent loop

Each test is isolated (named, own outputs), so agents don't collide:

1. Pick/author a test `cases/<feature>/`.
2. `python conformance/run.py --test <feature>`.
3. Open the diff — it shows exactly what AffineUI gets wrong.
4. Fix the lib; re-run until under threshold.

## Requirements

- **Node + Playwright** (`conformance/browser`; uses the system Chrome via
  `channel: chrome`, so no Chromium download). `npm install` once.
- **Python** with **Pillow + numpy** (already present here).
- **D3D11 / Windows** for `conformance_test` (offscreen readback is D3D11
  today; Metal/GL readback are TODO, mirroring the embedding backends).

## Roadmap

- **Accessibility axis (planned).** Compare the **accessibility tree**
  (roles/names/states), not just pixels: browser via Playwright
  `page.accessibility.snapshot()`; AffineUI needs an **a11y tree export**
  (build from DOM + ARIA) — a real lib feature, and the thing that also makes
  embedded AffineUI usable by screen readers / the host platform.
- Selector-based steps (needs AffineUI element-bounds / hit-test).
- Metal/GL headless readback (non-Windows).
- A larger committed test corpus (WPT-derived / CSS feature matrix).
- Tolerance/threshold tuning per test as text rendering converges.

## Known differences today

Text is the main delta: AffineUI's **baseline / line-height** and **font
rendering** diverge from Chrome (the `hello` diff shows body text doubled
vertically and the heading smeared). Layout positions and fills match.
