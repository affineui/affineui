# AffineUI Python

Initial proof-of-concept Python bindings for AffineUI.

The package builds a small `pybind11` extension over the C++ core. The
first slice exposes:

- `affineui.version()`
- `affineui.native_backend()` (`"sokol"` for this package)
- `affineui.Color`, `Size`, and `Rect`
- headless `affineui.Document`
- `affineui.App` construction, HTML/CSS loading, native `run()`, and
  native `launch()`

The intended production model is:

- the C++ core owns `App`, rendering, widget behavior, and callback lists;
- the C++ core keeps its two-file zero-dependency SDK shape; pybind11 and
  scikit-build are Python packaging tools, not AffineUI runtime/core
  dependencies;
- Python exposes a Pythonic package on top of that core;
- bound Python methods are mapped to weak-method callbacks in the Python
  layer before they enter the shared C++ callback substrate.

This proof of concept links the modular CMake target because that is the
fastest development path. The binding architecture must also support
building against the generated `affineui.h` / `affineui.cpp`
amalgamation for embedders who want the same two-file core in a Python
extension.

The native wheel backend is sokol (`sokol_app` + `sokol_gfx`) compiled
into `_affineui`. SDL is not a Python package dependency; it remains an
optional C++ embedding adapter. The graphics backend is still selected
by AffineUI's normal platform logic (`d3d11` on Windows, Metal on macOS,
GL where appropriate).

## Install from the repository

```powershell
python -m pip install ./bindings/python
```

For editable development:

```powershell
python -m pip install -e ./bindings/python
```

## Smoke test

```python
import affineui as ui

doc = ui.document(
    "<main><h1>Hello from Python</h1><p id='msg'>AffineUI</p></main>",
    "main { padding: 16px; } h1 { font-size: 32px; }",
)
print(ui.version(), doc.content_size())
```

Native window smoke:

```python
import affineui as ui

app = ui.App("AffineUI Python")
app.load_html("<main><h1>Hello</h1><p>Native sokol window.</p></main>")
app.launch(native=True)
```
