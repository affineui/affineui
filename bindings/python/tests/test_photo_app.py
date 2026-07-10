"""End-to-end interaction tests for the Photo Edit sample app.

Builds the real PhotoEditApp headlessly, lays it out, then drives its
App.on_event pointer routing with synthetic events — brush drags, marquee
selections, wheel zoom — and asserts the raster core changed the way the
web reference would.
"""

import sys
from pathlib import Path

import pytest

import affineui as ui

EXAMPLES = Path(__file__).resolve().parents[1] / "examples"


@pytest.fixture()
def photo():
    sys.path.insert(0, str(EXAMPLES))
    try:
        from photo_edit import PhotoEditApp
    finally:
        sys.path.remove(str(EXAMPLES))

    sample = PhotoEditApp()
    app = ui.App(title="Photo App Events", width=1280, height=820,
                 asset_folders=[str(EXAMPLES)], high_dpi=False)
    sample.app = app
    sample.doc.attach(app)
    app.on_event(sample._on_event)
    sample.reload()
    app.document().layout(1280, 820)
    yield sample, app
    sample.doc.detach()


def _move(app, x, y):
    ev = ui.Event()
    ev.type = ui.EventType.MouseMove
    ev.pos = ui.Point(int(x), int(y))
    app.dispatch(ev)


def _button(app, kind, x, y, alt=False, ctrl=False):
    ev = ui.Event()
    ev.type = kind
    ev.button = ui.MouseButton.Left
    ev.pos = ui.Point(int(x), int(y))
    ev.alt = alt
    ev.ctrl = ctrl
    app.dispatch(ev)


def _key(app, key, ctrl=False, shift=False):
    ev = ui.Event()
    ev.type = ui.EventType.KeyDown
    ev.key = key
    ev.ctrl = ctrl
    ev.shift = shift
    app.dispatch(ev)
    up = ui.Event()
    up.type = ui.EventType.KeyUp
    up.key = key
    app.dispatch(up)


def _find_stage_point(app):
    ev = ui.Event()
    ev.type = ui.EventType.MouseMove
    for y in range(80, 780, 16):
        for x in range(120, 1000, 16):
            ev.pos = ui.Point(x, y)
            app.dispatch(ev)
            for info in app.document().hovered_info_chain():
                if info.valid and "ps-stage-canvas" in info.classes:
                    return x, y
    return None


def _drag(app, x0, y0, x1, y1, steps=6):
    _move(app, x0, y0)
    _button(app, ui.EventType.MouseDown, x0, y0)
    for i in range(1, steps + 1):
        _move(app, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)
    _button(app, ui.EventType.MouseUp, x1, y1)


def test_brush_drag_paints_and_snapshots(photo):
    sample, app = photo
    point = _find_stage_point(app)
    assert point is not None, "stage canvas not hit-testable"
    x, y = point
    sample.set_tool("brush")
    rev = sample.doc.revision()
    _drag(app, x, y, x + 60, y + 20)
    assert sample.doc.revision() > rev
    names = [h.name for h in sample.doc.history_entries()]
    assert names[-1] == "Brush"
    assert not sample.doc.stroking()


def test_marquee_drag_selects(photo):
    sample, app = photo
    x, y = _find_stage_point(app)
    sample.set_tool("marquee")
    _drag(app, x, y, x + 80, y + 50)
    sel = sample.doc.selection()
    assert sel is not None
    assert sel[2] > 0 and sel[3] > 0


def test_move_tool_shifts_layer(photo):
    sample, app = photo
    x, y = _find_stage_point(app)
    # paint something on the (unlocked) top layer first
    sample.set_tool("brush")
    _drag(app, x, y, x + 10, y)
    sample.set_tool("move")
    rev = sample.doc.revision()
    _drag(app, x, y, x + 40, y + 30)
    assert sample.doc.revision() > rev
    assert [h.name for h in sample.doc.history_entries()][-1] == \
        "Move Layer"


def test_ctrl_wheel_zooms(photo):
    sample, app = photo
    x, y = _find_stage_point(app)
    before = sample.doc.zoom()
    ev = ui.Event()
    ev.type = ui.EventType.MouseWheel
    ev.pos = ui.Point(x, y)
    ev.ctrl = True
    ev.wheel_dy = 1.0
    app.dispatch(ev)
    assert sample.doc.zoom() > before


def test_keyboard_shortcuts(photo):
    sample, app = photo
    # tool shortcuts
    _key(app, ui.Key.E)
    assert sample.tool == "eraser"
    _key(app, ui.Key.B)
    assert sample.tool == "brush"
    # swap / reset colors
    sample.fg, sample.bg = "#112233", "#445566"
    _key(app, ui.Key.X)
    assert (sample.fg, sample.bg) == ("#445566", "#112233")
    _key(app, ui.Key.D)
    assert (sample.fg, sample.bg) == ("#000000", "#ffffff")
    # select all / deselect
    _key(app, ui.Key.A, ctrl=True)
    assert sample.doc.has_selection()
    _key(app, ui.Key.D, ctrl=True)
    assert not sample.doc.has_selection()
    # undo/redo round-trip through ctrl+z / ctrl+shift+z
    sample.doc.add_layer("Z Test")
    count = len(sample.doc.layers())
    _key(app, ui.Key.Z, ctrl=True)
    assert len(sample.doc.layers()) == count - 1
    _key(app, ui.Key.Z, ctrl=True, shift=True)
    assert len(sample.doc.layers()) == count


def test_eyedropper_sets_foreground(photo):
    sample, app = photo
    x, y = _find_stage_point(app)
    sample.set_tool("eyedropper")
    _move(app, x, y)
    _button(app, ui.EventType.MouseDown, x, y)
    _button(app, ui.EventType.MouseUp, x, y)
    assert sample.fg.startswith("#") and len(sample.fg) == 7


def test_adjust_modal_preview_commit(photo):
    sample, app = photo
    # open Brightness/Contrast via the adjustments panel action
    sample.adjustment_action("bc")
    assert sample.dialog is not None
    assert sample.doc.previewing()
    sample.set_dialog_value("brightness", 60, numeric=True,
                            spec=sample.dialog)
    sample.dialog_ok()
    assert not sample.doc.previewing()
    assert [h.name for h in sample.doc.history_entries()][-1] == \
        "Brightness/Contrast"


def _find_class_point(app, cls):
    ev = ui.Event()
    ev.type = ui.EventType.MouseMove
    for y in range(0, 820, 8):
        for x in range(0, 1280, 8):
            ev.pos = ui.Point(x, y)
            app.dispatch(ev)
            for info in app.document().hovered_info_chain():
                if info.valid and cls in info.classes:
                    return x, y, info.bounds
    return None


def test_sv_square_picks_color(photo):
    sample, app = photo
    found = _find_class_point(app, "ps-sv")
    assert found is not None, "color panel SV square not hit-testable"
    _, _, bounds = found
    # click the bottom-left corner: s≈0, v≈0 → black-ish
    x = bounds.x + 1
    y = bounds.y + bounds.h - 1
    _move(app, x, y)
    _button(app, ui.EventType.MouseDown, x, y)
    _button(app, ui.EventType.MouseUp, x, y)
    r, g, b = (int(sample.fg[i:i + 2], 16) for i in (1, 3, 5))
    assert max(r, g, b) < 32, sample.fg


def test_navigator_click_pans(photo):
    sample, app = photo
    found = _find_class_point(app, "ps-nav-canvas")
    assert found is not None, "navigator canvas not hit-testable"
    _, _, bounds = found
    before = (sample.doc.pan_x(), sample.doc.pan_y())
    # click near the left edge of the navigator → pan towards doc left
    x = bounds.x + 2
    y = bounds.y + bounds.h // 2
    _move(app, x, y)
    _button(app, ui.EventType.MouseDown, x, y)
    _button(app, ui.EventType.MouseUp, x, y)
    assert (sample.doc.pan_x(), sample.doc.pan_y()) != before


def test_adjust_modal_cancel_restores(photo):
    sample, app = photo
    entries_before = len(sample.doc.history_entries())
    sample.adjustment_action("hsl")
    sample.set_dialog_value("hue", 120, numeric=True, spec=sample.dialog)
    sample.close_dialog()  # cancel
    assert not sample.doc.previewing()
    assert len(sample.doc.history_entries()) == entries_before


def test_layer_selection_fires_on_mousedown(photo):
    sample, app = photo
    # Add a layer so there is at least one non-active row to click.
    sample.doc.add_layer("Row Test")
    active_before = sample.doc.active_id()
    sample.reload()
    app.document().layout(1280, 820)

    # Find any layer row whose data-layer-id differs from the active one.
    ev = ui.Event(); ev.type = ui.EventType.MouseMove
    hit = target = None
    for y in range(0, 820, 6):
        for x in range(0, 1280, 6):
            ev.pos = ui.Point(x, y); app.dispatch(ev)
            for info in app.document().hovered_info_chain():
                if info.valid and "ps-layer" in info.classes:
                    for name, value in info.attrs:
                        if name == "data-layer-id" \
                                and int(value) != active_before:
                            hit, target = (x, y), int(value)
            if hit:
                break
        if hit:
            break
    assert hit is not None, "a non-active layer row was not hit-testable"
    x, y = hit
    _move(app, x, y)
    # MouseDown ALONE must select (snappy) — no MouseUp yet.
    _button(app, ui.EventType.MouseDown, x, y)
    assert sample.doc.active_id() == target, "selection did not fire on down"


def test_history_jump_fires_on_mousedown(photo):
    sample, app = photo
    # Generate a couple of history entries, then jump to an earlier one.
    sample.doc.add_layer("A")
    sample.doc.add_layer("B")
    sample.reload()
    app.document().layout(1280, 820)
    before_index = sample.doc.history_index()
    assert before_index > 0

    ev = ui.Event(); ev.type = ui.EventType.MouseMove
    hit = target = None
    for y in range(0, 820, 6):
        for x in range(0, 1280, 6):
            ev.pos = ui.Point(x, y); app.dispatch(ev)
            for info in app.document().hovered_info_chain():
                if info.valid and "ps-history-item" in info.classes:
                    for name, value in info.attrs:
                        if name == "data-history-index" and int(value) == 0:
                            hit, target = (x, y), 0
            if hit:
                break
        if hit:
            break
    assert hit is not None, "history item not hit-testable"
    x, y = hit
    _move(app, x, y)
    _button(app, ui.EventType.MouseDown, x, y)  # no MouseUp
    assert sample.doc.history_index() == target, \
        "history jump did not fire on down"
