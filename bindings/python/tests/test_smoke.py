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


def test_view_emits_remote_patch_batches_and_html():
    view = ui.View(ui.ViewTheme.Bootstrap)
    patches = ui.RemotePatchQueue()

    view.begin(patches)
    view.heading(1, "Command API")
    view.button("Run", primary=True)
    view.checkbox("Enabled", True)
    view.slider("Gain", 0.5)
    view.end()

    assert patches.size() > 0
    assert "create_element" in patches.to_json()
    html = view.to_html_document()
    assert "bootstrap-5.3.8.min.css" in html
    assert "Command API" in html

    app = ui.App(title="View Smoke")
    app.load_view(view)
    app.document().layout(320, 200)
    assert app.document().content_size().width >= 320


def test_view_panel_uses_framework_selectors():
    bootstrap = ui.View(ui.ViewTheme.Bootstrap)
    bootstrap.begin()
    bootstrap.panel(key="panel")
    bootstrap.end()
    bootstrap_html = bootstrap.to_html_document()
    assert "bootstrap-5.3.8.min.css" in bootstrap_html
    assert "container py-4" in bootstrap_html

    decius = ui.View(ui.ViewTheme.Decius)
    decius.begin()
    decius.panel(key="panel")
    decius.end()
    decius_html = decius.to_html_document()
    assert "decius-css-0.4.1.bundle.min.css" in decius_html
    assert "dcs-panel dcs-panel--bordered" in decius_html


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


def test_append_is_illegal_during_generation():
    view = ui.View(ui.ViewTheme.Bootstrap)
    patches = ui.RemotePatchQueue()

    view.begin(patches)
    panel = view.container(classes="panel", key="panel")
    panel.append(lambda v: v.paragraph("Forbidden"))
    view.end()

    assert view.diagnostics()
    assert "append" in view.diagnostics()[0]
