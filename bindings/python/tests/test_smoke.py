import sys
from pathlib import Path

import affineui as ui


def test_version_is_available():
    assert ui.version()
    assert ui.native_backend() == "sokol"


def test_headless_document_layout_smoke():
    doc = ui.document(
        "<main><h1>Hello</h1><p id='message'>Python binding</p></main>",
        "main { padding: 8px; } h1 { font-size: 24px; }",
        width=320,
        height=200,
    )

    size = doc.content_size()
    assert size.width >= 320
    assert size.height >= 200


def test_app_can_load_html_without_running_window():
    app = ui.App(title="Python Smoke")
    app.load_html("<button id='ok'>OK</button>")
    app.set_stylesheet("button { padding: 8px; }")

    doc = app.document()
    doc.layout(256, 128)
    size = doc.content_size()
    assert size.width >= 256
    assert size.height >= 128

    ok = doc.element("ok")
    assert ok
    assert ok.set_text("Ready")
    assert ok.set_attr("data-state", "ready")
    assert ok.remove_attr("data-state")
    assert ok.set_style("color: red")
    assert ok.clear_style()

    missing = doc.element("missing")
    assert not missing
    assert not missing.set_text("Ignored")
    assert not missing.set_style("display: none")


def _find_hovered_attr(app, attr_name: str, width: int, height: int):
    ev = ui.Event()
    ev.type = ui.EventType.MouseMove
    for y in range(0, height, 4):
        for x in range(0, width, 4):
            ev.pos = ui.Point(x, y)
            app.dispatch(ev)
            for info in app.document().hovered_info_chain():
                if any(name == attr_name for name, _ in info.attrs):
                    return ui.Point(x, y)
    return None


def _find_hovered_attr_value(
    app, attr_name: str, attr_value: str, width: int, height: int
):
    ev = ui.Event()
    ev.type = ui.EventType.MouseMove
    for y in range(0, height, 4):
        for x in range(0, width, 4):
            ev.pos = ui.Point(x, y)
            app.dispatch(ev)
            for info in app.document().hovered_info_chain():
                if any(
                    name == attr_name and value == attr_value
                    for name, value in info.attrs
                ):
                    return ui.Point(x, y)
    return None


def _click(app, point):
    down = ui.Event()
    down.type = ui.EventType.MouseDown
    down.button = ui.MouseButton.Left
    down.pos = point
    app.dispatch(down)

    up = ui.Event()
    up.type = ui.EventType.MouseUp
    up.button = ui.MouseButton.Left
    up.pos = point
    app.dispatch(up)


def test_view_emits_remote_patch_batches_and_html():
    view = ui.View(ui.ViewTheme.Bootstrap)
    patches = ui.RemotePatchQueue()

    view.begin(patches)
    view.heading(1, "Command API")
    view.button("Run", primary=True, key="run").on_click(lambda: None)
    view.checkbox("Enabled", True, key="enabled").on_change(lambda value: None)
    view.input("Object", "Cylinder.042", key="object")
    view.password("Token", "secret", key="token")
    view.dropdown("Mode", ["Object", "Edit"], "Object", key="mode")
    view.button_group("Space", ["Local", "World"], "World", key="space")
    view.textarea("Notes", "Dense native UI", rows=3, key="notes")
    view.slider("Gain", 0.5, key="gain").on_change(lambda value: None)
    view.knob("Shape", 0.42, key="shape").on_change(lambda value: None)
    view.end()

    assert not view.diagnostics()
    assert patches.size() > 0
    assert "create_element" in patches.to_json()
    html = view.to_html_document()
    assert "bootstrap-5.3.8.min.css" in html
    assert "Command API" in html
    assert "data-aui-knob" in html
    assert "form-control" in html
    assert "form-select" in html
    assert "btn-group" in html

    app = ui.App(title="View Smoke")
    app.load_view(view)
    app.document().layout(320, 200)
    assert app.document().content_size().width >= 320


def test_python_knob_callback_survives_headless_dispatch():
    for theme, attr_name in (
        (ui.ViewTheme.Bootstrap, "data-aui-knob"),
        (ui.ViewTheme.Decius, "data-dcs-knob"),
    ):
        changes = []
        view = ui.View(theme)
        view.begin()
        view.knob("Shape", 0.42, key="shape").on_change(changes.append)
        view.end()

        app = ui.App(title="Knob Smoke", asset_folders=["examples", "."])
        app.load_view(view)
        app.document().layout(360, 240)

        knob = _find_hovered_attr(app, attr_name, 360, 240)
        assert knob is not None

        down = ui.Event()
        down.type = ui.EventType.MouseDown
        down.button = ui.MouseButton.Left
        down.pos = knob
        app.dispatch(down)

        drag = ui.Event()
        drag.type = ui.EventType.MouseMove
        drag.pos = ui.Point(knob.x, knob.y - 36)
        app.dispatch(drag)

        up = ui.Event()
        up.type = ui.EventType.MouseUp
        up.button = ui.MouseButton.Left
        up.pos = drag.pos
        app.dispatch(up)

        assert changes
        assert float(changes[-1]) > 0.42


def test_view_panel_uses_framework_selectors():
    bootstrap = ui.View(ui.ViewTheme.Bootstrap)
    bootstrap.begin()
    bootstrap.panel(
        key="panel",
        build=lambda v: v.input("Object", "Cylinder.042", key="object"),
    )
    bootstrap.end()
    bootstrap_html = bootstrap.to_html_document()
    assert not bootstrap.diagnostics()
    assert "bootstrap-5.3.8.min.css" in bootstrap_html
    assert "card shadow-sm" in bootstrap_html
    assert "aui-bs-form" in bootstrap_html
    assert "aui-bs-field" in bootstrap_html
    assert "form-control" in bootstrap_html
    assert 'data-aui-size="md"' in bootstrap_html

    decius = ui.View(ui.ViewTheme.Decius)
    decius.begin()
    decius.panel(
        key="panel",
        build=lambda v: v.input("Object", "Cylinder.042", key="object"),
    ).selector(ui.decius.selector.size, ui.decius.size.lg)
    decius.end()
    decius_html = decius.to_html_document()
    assert not decius.diagnostics()
    assert "decius-css-0.5.2.bundle.min.css" in decius_html
    assert "dcs-panel dcs-panel--bordered" in decius_html
    assert "dcs-input" in decius_html
    assert 'data-aui-style="flat"' in decius_html
    assert 'data-dcs-style="flat"' in decius_html
    assert 'data-aui-size="lg"' in decius_html

    decius.selector(ui.decius.selector.style, ui.decius.style.three_d)
    decius_html = decius.to_html_document()
    assert 'data-aui-style="3d"' in decius_html
    assert 'data-dcs-style="3d"' in decius_html


def test_named_widget_refs_are_safe_and_recoverable():
    view = ui.View(ui.ViewTheme.Bootstrap)
    patches = ui.RemotePatchQueue()

    view.begin(patches)
    run = view.button("Run", primary=True, key="run-button")
    view.end()

    assert run
    assert run.name() == "run-button"
    assert view.find_widget("run-button").id() == run.id()

    run.text("Stop")
    assert "Stop" in view.to_html_fragment()

    view.begin(patches)
    view.paragraph("No button")
    view.end()

    assert not run
    run.text("Ignored")
    assert "Ignored" not in view.to_html_fragment()

    view.clear()
    view.begin(patches)
    rebuilt = view.button("Run again", primary=True, key="run-button")
    view.end()

    assert run
    assert rebuilt.id() == run.id()
    assert "Run again" in view.to_html_fragment()


def test_keyless_widgets_are_write_only_declarations():
    view = ui.View(ui.ViewTheme.Bootstrap)
    patches = ui.RemotePatchQueue()

    view.begin(patches)
    unnamed = view.button("Write only")
    view.end()

    assert not unnamed
    assert not view.find_widget("Write only")
    assert "Write only" in view.to_html_fragment()


def test_duplicate_widget_ids_are_diagnostics_not_corruption():
    view = ui.View(ui.ViewTheme.Bootstrap)
    patches = ui.RemotePatchQueue()

    view.begin(patches)
    view.button("First", key="dup")
    view.button("Second", key="dup")
    view.end()

    assert view.diagnostics()
    assert "dup" in view.diagnostics()[0]
    assert view.find_widget("dup")


def test_widget_ref_append_and_replace_children():
    view = ui.View(ui.ViewTheme.Bootstrap)
    patches = ui.RemotePatchQueue()

    view.begin(patches)
    panel = view.container(classes="panel", key="panel")
    view.end()

    assert panel
    panel.append(lambda v: v.paragraph("First", key="first"))
    panel.append(lambda v: v.button("Second", key="second"))
    assert view.find_widget("first")
    assert view.find_widget("second")

    panel.replace(lambda v: v.heading(2, "Replacement", key="replacement"))
    assert not view.find_widget("second")
    assert view.find_widget("replacement")


def test_stable_ref_replaces_tab_body_content():
    view = ui.View(ui.ViewTheme.Bootstrap)
    patches = ui.RemotePatchQueue()

    view.begin(patches)
    view.panel(
        key="panel",
        build=lambda v: (
            v.button("Controls", primary=True, key="tab-controls"),
            v.button("Fields", key="tab-fields"),
            v.container(classes="tab-body", key="tab-body"),
        ),
    )
    view.end()

    body = view.find_widget("tab-body")
    assert body
    body.replace(
        lambda v: (
            v.checkbox("Framework checkbox", True, key="hello-check"),
            v.slider("Framework slider", 0.65, key="hello-slider"),
        )
    )
    assert view.find_widget("hello-check")
    assert view.find_widget("hello-slider")

    body.replace(
        lambda v: (
            v.input("Object", "Cylinder.042", key="object"),
            v.dropdown("Mode", ["Object", "Edit"], "Object", key="mode"),
        )
    )
    assert not view.find_widget("hello-check")
    assert view.find_widget("object")
    assert view.find_widget("mode")


def test_virtual_list_materializes_visible_window():
    view = ui.View(ui.ViewTheme.Bootstrap)

    view.begin()
    view.virtual_list(
        key="events",
        item_count=100,
        first_item=40,
        visible_items=5,
        overscan=2,
        item_size=20.0,
        build=lambda v, index: v.button(f"Row {index}", key=f"row-{index}"),
    )
    view.end()

    html = view.to_html_fragment()
    assert 'data-aui-widget="virtual-list"' in html
    assert "height:760px" in html
    assert "height:1060px" in html
    assert "Row 37" not in html
    assert "Row 38" in html
    assert "Row 46" in html
    assert "Row 47" not in html
    assert view.find_widget("row-38")
    assert not view.find_widget("row-47")


def test_append_is_illegal_during_generation():
    view = ui.View(ui.ViewTheme.Bootstrap)
    patches = ui.RemotePatchQueue()

    view.begin(patches)
    panel = view.container(classes="panel", key="panel")
    panel.append(lambda v: v.paragraph("Forbidden"))
    view.end()

    assert view.diagnostics()
    assert "append" in view.diagnostics()[0]


def test_photo_document_core_layer_history_operations():
    doc = ui.PhotoDocument(640, 480)

    assert doc.width() == 640
    assert doc.height() == 480
    assert doc.active_layer_id() == "retouch"
    assert doc.active_layer().name == "Hero retouch"
    assert doc.history()[doc.history_index()] == "Layer Opacity"

    new_id = doc.add_layer("Paint test")
    assert doc.active_layer_id() == new_id
    assert doc.active_layer().name == "Paint test"
    assert doc.history()[doc.history_index()] == "New Layer"

    assert doc.set_active_opacity(42)
    assert doc.active_layer().opacity == 42
    assert doc.set_active_blend("Multiply")
    assert doc.active_layer().blend == "Multiply"

    assert doc.toggle_layer_visible(new_id)
    assert doc.active_layer().visible is False

    copy_id = doc.duplicate_active_layer()
    assert copy_id
    assert doc.active_layer().name.endswith(" copy")

    assert doc.delete_active_layer()
    assert doc.active_layer_id() == new_id
    assert doc.undo() == "Duplicate Layer"
    assert doc.redo() == "Delete Layer"


def test_photo_edit_sample_uses_cpp_core_and_is_clickable():
    examples = Path(__file__).resolve().parents[1] / "examples"
    sys.path.insert(0, str(examples))
    try:
        from photo_edit import PHOTO_CSS, PhotoEditApp
    finally:
        sys.path.remove(str(examples))

    sample = PhotoEditApp()
    assert isinstance(sample.doc, ui.PhotoDocument)
    app = ui.App(
        title="Photo Edit Smoke",
        width=1280,
        height=820,
        asset_folders=["examples", "."],
        high_dpi=False,
    )
    sample.app = app
    sample.reload()
    app.set_stylesheet(PHOTO_CSS)
    app.document().layout(1280, 820)

    assert app.document().content_size().width >= 1280
    eraser = _find_hovered_attr_value(
        app, "data-aui-name", "tool-eraser", 1280, 820
    )
    assert eraser is not None

    _click(app, eraser)
    assert sample.tool == "eraser"

    sample.menu_action("lnew")
    assert sample.current_layer().name.startswith("Layer")
    assert sample.doc.history()[sample.doc.history_index()] == "New Layer"
