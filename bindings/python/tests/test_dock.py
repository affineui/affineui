"""Declarative docking from Python.

Mirrors the Rust (tests/dock.rs) and C# (tests/Dock) suites. Focuses on the
SAVE half of size persistence — Document.dock_pane_sizes() — which was the gap:
the binding could feed sizes back IN (set_dock_size_provider) but had no way to
read them back OUT, so a user's splitter drags were unpersistable.
"""

import affineui as ui


def _workspace_app():
    app = ui.App(title="Dock", width=1280, height=820)
    view = ui.View(ui.ViewTheme.Decius)

    def ws(v):
        v.document(lambda v: v.heading(1, "Viewport"), title="Scene", icon="cube")
        v.dockpanel(
            "Outliner",
            ui.DockLocation.docked(ui.Dock.Left).sized(280),
            lambda v: v.heading(2, "Objects"),
            icon="list",
            key="outliner",
        )
        v.dockpanel(
            "Inspector",
            ui.DockLocation.docked(ui.Dock.Right).sized(320),
            lambda v: v.heading(2, "Properties"),
            icon="sliders",
            key="inspector",
        )

    view.document_view("ws", ws)
    app.load_view(view)
    return app


def test_dock_pane_sizes_round_trip():
    # The SAVE half. dock_pane_sizes() reads the live flex-basis from the DOM,
    # so this needs a materialized App/Document — a View-only test cannot see it,
    # which is exactly why the gap went unnoticed.
    app = _workspace_app()
    sizes = dict(app.document().dock_pane_sizes())
    assert sizes, "dock_pane_sizes() empty — the SAVE half of persistence is broken"

    def find(key):
        return next((px for pane, px in sizes.items() if key in pane), None)

    assert find("outliner") == 280, sizes
    assert find("inspector") == 320, sizes
    # The flexible center/document pane has no fixed basis and is omitted.
    assert find("scene") is None and find("cube") is None, sizes
