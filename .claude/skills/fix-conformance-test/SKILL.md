---
name: fix-conformance-test
description: >
  Fix an AffineUI visual-conformance gap the RIGHT way. Use when a conformance
  test (conformance/cases/<name>) diffs too high against the real browser. Run
  the A/B, find the root cause, and fix it properly — extend lexbor in-tree for
  missing CSS, or add real engine facilities. No bodging; halt on architectural
  questions.
---

# Fixing an AffineUI conformance gap

You make AffineUI render a conformance test match Chrome by fixing the
**library**, not by patching the test or hacking around a missing feature.

## Non-negotiable rules

1. **No bodging. Ever.** A fix is code that works correctly with the library's
   architecture and is consistent, maintainable, and well-crafted. No
   workarounds, no shortcuts, no ad-hoc re-parsing.
2. **Missing CSS ⇒ extend lexbor.** If a CSS property/shorthand/value doesn't
   work, it's because lexbor is incomplete. Add it to lexbor (now an in-tree
   subtree at `external/lexbor`). NEVER hand-parse CSS in AffineUI to dodge
   lexbor (e.g. the legacy `RuleFill` side-table in `src/dom/document.cpp` is
   debt to migrate INTO lexbor, not a pattern to copy).
3. **HALT on architectural questions.** If a proper fix needs a new subsystem,
   a cross-cutting change, a non-obvious design decision, or a large lexbor
   change — **stop and ask the human.** Do not improvise or "make it up as you
   go." Improvising is what creates bodges.
4. **Minimal + no regressions.** Scope the change to the actual cause; re-run
   sibling tests to confirm you didn't break them.

## The loop

```
# one-time: MSVC env (PowerShell)
$vcvars="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && set" | %{ if($_-match'^([^=]+)=(.*)$'){Set-Item "env:$($matches[1])" $matches[2]} }
$env:Path += ';'+[Environment]::GetEnvironmentVariable('Path','User')   # python/node

cmake --preset ninja
cmake --build build/ninja --target conformance_test     # rebuilds lexbor_static + lib

# full A/B for one test (browser + AffineUI + diff + report):
python conformance/run.py --test <name>
#   (worktree without browser deps: `npm install` in conformance/browser once,
#    or diff your render against the browser PNG already in the main checkout's
#    conformance/out/<name>.browser.<snap>.png via conformance/diff.py)
```

### ALWAYS visually inspect the images — never trust `pct_changed` alone

After every render you MUST open and actually LOOK at all three images with the
Read tool:
- `conformance/out/<name>.browser.<snap>.png`  (Chrome — the truth)
- `conformance/out/<name>.affineui.<snap>.png`  (ours; convert a `.ppm`:
  `python -c "from PIL import Image;Image.open('x.ppm').save('x.png')"`)
- `conformance/out/<name>.diff.<snap>.png`       (red = changed)

`pct_changed` is necessary but NOT sufficient — it routinely HIDES real bugs:
- A clearly-wrong single element is a small fraction of a mostly-white canvas
  (e.g. `border-radius:50%` rendering a hard **square** scored only ~2%; a
  heading rendering **non-bold** scored ~6%). Both look "passing" by number,
  obviously broken by eye.
- Localized wrong **color**, wrong **font weight/size**, wrong **layout**
  (stacked vs inline), missing/extra elements — all easy to miss numerically.
- In the diff, distinguish **thin glyph-edge red** (irreducible NanoVG-vs-Skia
  text AA — a real ~6% floor on dense text) from **solid blocks / shifted
  shapes / wrong fills** (real bugs to fix). Same % can be either.

So: compare browser vs ours side by side, read the diff, and name the concrete
defect ("subtitle is black, should be gray"; "links stack, should be inline")
before changing code. Iterate fix → rebuild → re-render → re-LOOK until the
render matches Chrome (not merely until the number drops). A test can be at/under
threshold and still be visibly wrong — fix what you SEE, then let the number
confirm.

## Where things live (find the right layer)

| Symptom | Layer | Files |
|---|---|---|
| CSS property ignored / wrong value | **lexbor** (parse) then **cascade** | `external/lexbor/source/lexbor/css/…`, `src/style/cascade.cpp` |
| value parsed but not applied | **computed style** + consumer | `src/internal/computed_style.h`, `src/style/computed.cpp` |
| wrong size/position/flow | **layout** | `src/layout/*.cpp`, `src/layout/yoga_adapter.cpp`, `src/engine/layout_engine.cpp` |
| wrong fill/border/gradient/shadow | **paint** | `src/paint/nanovg_painter.cpp`, `src/engine/paint_engine.cpp` |
| wrong text metrics/baseline/wrap | **text/inline** | `src/text/*`, `src/layout/inline.cpp` |

## Extending lexbor (the common case)

`external/lexbor` is the **full affineui_lexbor fork as a git subtree** — the
canonical, editable lexbor, with the `utils/` generator and decl tests. Read
`external/lexbor/PATCHES.md`: `border-radius`, `background`, `box-shadow`,
`gap`, `row/column-gap` were all added there properly (property table + value
parser + serializer + a declaration test). Follow that pattern to add a
property/shorthand/value. After it parses, you'll have `LXB_CSS_PROPERTY_<NAME>`
/ value enums — map them in `src/style/cascade.cpp` → computed style → the
consuming layer. Rebuild (`--target conformance_test` rebuilds lexbor_static).
Push lexbor changes back to the fork at integration: `git subtree push
--prefix=external/lexbor <fork> affineui`.

If the property already parses in lexbor (e.g. `box-sizing`), you just wire the
cascade → computed → layout/paint — no lexbor change needed.

## Adding an engine facility (when a proper fix needs one)

Build it properly, matching existing patterns: e.g. a new computed-style field
(`computed_style.h`, packed like its neighbors) ← set in `cascade.cpp` ←
consumed in `yoga_adapter.cpp`/`engine` (layout) or `nanovg_painter.cpp`
(paint). If the facility is large or cross-cutting → **HALT and ask.**

## Finishing

- Re-run the target test + 1–2 related siblings; confirm no regression.
- Commit with a clear message: `fix(<area>): <what> — <test> conformance`.
- Report: before→after `pct_changed`, files+functions changed, root cause, and
  any lexbor changes (so they get pushed to the fork).
