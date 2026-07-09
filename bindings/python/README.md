# AffineUI for Python

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_dender.png" width="720" alt="AffineUI running the Dender 3D-print slicer — the default Decius CSS look">

<img src="https://raw.githubusercontent.com/benjcooley/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI rendering a Bootstrap dashboard">

**A small, HTML5-compliant, GPU-accelerated UI renderer with an integrated
component framework — a native replacement for Electron and Qt, for Python.**

Think Gradio, but native: describe your UI as a tree of typed components,
get a real HTML/CSS renderer with a browser-quality cascade, animations,
hit-testing, and 120 Hz paint — all in one binary, no browser embedded, no
JavaScript runtime, no Electron. Bootstrap and Tailwind-style stylesheets
render out of the box; the bundled default is
[Decius CSS](https://deciuscss.com).

**Status:** alpha. Broad standards coverage, real UIs run today, but
expect bugs and expect APIs to move.

---

## Install

```bash
pip install affineui
```

Prebuilt wheels ship for CPython 3.9 – 3.13 on Linux (`manylinux_2_28`
x86_64), macOS (universal2 — Intel + Apple Silicon), and Windows AMD64.

For a source install from a clone of the [main repo](https://github.com/benjcooley/affineui):

```bash
pip install ./bindings/python           # regular install
pip install -e ./bindings/python        # editable / dev
```

---

## Hello, world

### Component API (recommended)

```python
import affineui as ui

view = ui.View(ui.ViewTheme.Decius)
view.begin()
view.heading(1, "Hello from Python")
view.paragraph("AffineUI — native HTML/CSS, no browser, no JS.")
view.button("Click me", key="go").on_click(lambda: print("clicked!"))
view.end()

app = ui.App(title="Hello", width=720, height=480)
app.load_view(view)
raise SystemExit(app.run())
```

### Raw HTML

```python
import affineui as ui

app = ui.App(title="Hello", width=720, height=480)
app.load_html("""
  <main style="padding: 24px; font-family: sans-serif;">
    <h1>Hello from Python</h1>
    <p>AffineUI is alive.</p>
    <button>Click me</button>
  </main>
""")
raise SystemExit(app.run())
```

### Headless (no window — for CI and tests)

```python
import affineui as ui

doc = ui.document(
    "<main><h1>Hello</h1><p>Layout without a window.</p></main>",
    "main { padding: 16px; } h1 { font-size: 32px; }",
    width=640, height=360,
)
print(ui.version(), doc.content_size())
```

---

## A modest app

A live-updating counter — state lives in Python; the reconciler diffs
your view and patches the DOM in place. CSS hover/focus/animation keeps
running between updates.

```python
import affineui as ui

count = 0
app = ui.App(title="Counter", width=480, height=240)

def build_view() -> ui.View:
    view = ui.View(ui.ViewTheme.Decius)
    view.begin()
    view.heading(1, f"Count: {count}")
    view.button("Increment", key="inc").on_click(bump)
    view.button("Reset",     key="reset").on_click(reset)
    view.end()
    return view

def bump():
    global count
    count += 1
    app.load_view(build_view())

def reset():
    global count
    count = 0
    app.load_view(build_view())

app.load_view(build_view())
raise SystemExit(app.run())
```

The main repo's [`bindings/python/examples/`](https://github.com/benjcooley/affineui/tree/main/bindings/python/examples)
ships larger runnable programs — a component gallery, a photo editor,
and a full Bootstrap-styled panel demo.

---

## Loading design-system CSS

Decius CSS is the default when you use the component API. To render your
own HTML with a framework stylesheet, tell the app where its asset
folder is:

```python
app = ui.App(
    title="Bootstrap demo",
    width=1280,
    height=720,
    asset_folders=["examples"],          # so href="frameworks/..." resolves
)
app.load_html("""
  <link rel="stylesheet" href="frameworks/css/bootstrap-5.3.8.min.css">
  <div class="container py-4">
    <button class="btn btn-primary">Native Bootstrap</button>
  </div>
""")
raise SystemExit(app.run())
```

Bootstrap 4.6 / 5.3, Tailwind-style utility classes, and Ant-style
component markup all render — this is a real CSS engine, not an
HTML-shaped layout solver.

---

## What AffineUI is *not*

- **Not a browser.** No navigation, no `fetch`, no cookies.
- **Not a nerfed HTML5.** Missing non-esoteric features are bugs.
- **Not a security sandbox.** Never feed it untrusted HTML/CSS/JS.
- **Not an everything-included framework.** No video decode, no audio,
  no 3D — those are your app's job.
- **Not (yet) a JS runtime.** JS + React support is on the roadmap as
  an opt-in extension.

See the [main README](https://github.com/benjcooley/affineui) for the
full picture, embedding notes, C++ / Rust / C# APIs, and the demo
gallery.

---

## Links

- **Repository:** <https://github.com/benjcooley/affineui>
- **Docs:** <https://github.com/benjcooley/affineui/tree/main/docs>
- **Issues:** <https://github.com/benjcooley/affineui/issues>
- **License:** MIT
