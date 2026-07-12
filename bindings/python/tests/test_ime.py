"""IME (composition / preedit) contracts, from Python.

The composition protocol is a plain event — a UTF-8 string plus three byte
offsets — so it is fully drivable without an OS IME. That matters for the
binding specifically: a Python host that drives its own windowing (a DCC
plugin, a game engine embed) has to feed composition events in and read the
caret intent back out, and until these tests existed nothing verified the
binding could do either. The C++ core has its own IME tests; these check that
the *binding* exposes the protocol faithfully.

What still needs a real IME (and a human) is everything below the event: the
platform IMM/TSF plumbing that produces these events in the first place. See
examples/19_ime_lab and docs/IME_ARCHITECTURE.md §5.
"""

import affineui as ui


# The kanji for "Japanese" — the classic 3-codepoint, 9-byte CJK string. Byte
# offsets matter here: the composition cursor and clause range are UTF-8 BYTE
# offsets, not codepoint indices, so a multi-byte string is the only honest test.
NIHONGO = "日本語"  # 日本語
assert len(NIHONGO.encode("utf-8")) == 9


def _focused_input_app():
    """An App with a single text input, focused, caret at the end."""
    app = ui.App(title="IME", asset_folders=["examples", "."])
    app.load_html(
        "<style>body{margin:0;padding:0}"
        "input{display:block;box-sizing:border-box;width:180px;height:24px;"
        "border:1px solid #000;padding:2px 6px;font-size:12px}</style>"
        "<input type='text' value='ab'>"
    )
    doc = app.document()
    doc.layout(240, 120)

    # Click into the field, then End — same as the C++ IME test harness.
    pos = _find_input(app, 240, 120)
    assert pos is not None, "could not locate the input"

    down = ui.Event()
    down.type = ui.EventType.MouseDown
    down.button = ui.MouseButton.Left
    down.pos = pos
    app.dispatch(down)

    up = ui.Event()
    up.type = ui.EventType.MouseUp
    up.button = ui.MouseButton.Left
    up.pos = pos
    app.dispatch(up)

    end = ui.Event()
    end.type = ui.EventType.KeyDown
    end.key = ui.Key.End
    app.dispatch(end)
    return app


def _find_input(app, width: int, height: int):
    ev = ui.Event()
    ev.type = ui.EventType.MouseMove
    for y in range(0, height, 4):
        for x in range(0, width, 4):
            ev.pos = ui.Point(x, y)
            app.dispatch(ev)
            for info in app.document().hovered_info_chain():
                if info.tag == "input":
                    return ui.Point(x, y)
    return None


def _composition(text: str, cursor: int = -1, clause=None) -> ui.Event:
    ev = ui.Event()
    ev.type = ui.EventType.Composition
    ev.text = text
    ev.composition_cursor = cursor
    if clause is not None:
        ev.composition_clause_begin, ev.composition_clause_end = clause
    return ev


def _text_input(text: str) -> ui.Event:
    ev = ui.Event()
    ev.type = ui.EventType.TextInput
    ev.text = text
    return ev


def test_event_exposes_the_composition_fields():
    ev = _composition(NIHONGO, cursor=6, clause=(3, 9))
    assert ev.type == ui.EventType.Composition
    assert ev.text == NIHONGO
    assert ev.composition_cursor == 6
    assert ev.composition_clause_begin == 3
    assert ev.composition_clause_end == 9


def test_text_input_active_reports_the_focused_control():
    app = ui.App(title="IME", asset_folders=["examples", "."])
    app.load_html("<input type='text' value='ab'>")
    app.document().layout(240, 120)

    # Nothing focused yet: the IME must be told to stay off, or it will swallow
    # the host's hotkeys (see ime_lab check 8).
    assert app.document().text_input_active() is False

    app = _focused_input_app()
    assert app.document().text_input_active() is True


def test_caret_rect_is_empty_with_nothing_focused():
    app = ui.App(title="IME", asset_folders=["examples", "."])
    app.load_html("<input type='text' value='ab'>")
    app.document().layout(240, 120)
    assert app.document().caret_rect().w <= 0


def test_caret_rect_needs_a_painted_layout():
    """KNOWN LIMITATION, asserted so it cannot regress silently.

    caret_rect() is "as of the last layout/paint": it returns {} unless a
    layout has run WITH a measurer — `if (last_measurer == nullptr) return {}`
    in document_text.cpp. The measurer is only ever set by the 3-arg
    Document::layout(w, h, painter), and the renderer's frame path does supply
    it (renderer.cpp), so a *windowed* app gets a real caret rect.

    But Python's Document.layout binding takes only (width, height) and never
    passes a painter (bindings/python/src/affineui_py.cpp), so in a purely
    headless document the caret rect stays empty even with a focused, actively
    composing control. A Python host driving its own windowing therefore cannot
    anchor the OS candidate window from a headless layout — it has to go
    through App's frame path.

    That is a real gap in the binding's embedder story. Pinning it here keeps it
    visible until the binding exposes a layout-with-painter (or an App-level
    caret query).
    """
    app = _focused_input_app()
    assert app.document().text_input_active() is True

    # Focused and composing — and still empty, purely for want of a measurer.
    app.dispatch(_composition(NIHONGO, cursor=0))
    assert app.document().caret_rect().w <= 0


def _focused_view_app():
    """Same as _focused_input_app, but built through the View API so we can
    observe commits the way a real Python host does — via on_change."""
    changes = []
    view = ui.View(ui.ViewTheme.Decius)
    view.begin()
    view.input("Name", "ab", key="field").on_change(changes.append)
    view.end()

    app = ui.App(title="IME", asset_folders=["examples", "."])
    app.load_view(view)
    app.document().layout(360, 200)

    pos = _find_input(app, 360, 200)
    assert pos is not None, "could not locate the input"
    for kind in (ui.EventType.MouseDown, ui.EventType.MouseUp):
        ev = ui.Event()
        ev.type = kind
        ev.button = ui.MouseButton.Left
        ev.pos = pos
        app.dispatch(ev)
    end = ui.Event()
    end.type = ui.EventType.KeyDown
    end.key = ui.Key.End
    app.dispatch(end)
    return app, changes


def test_preedit_does_not_fire_a_change():
    """The preedit is spliced into a DERIVED display string. It must never
    reach the control's value, so a host's on_change must not see it — that is
    the whole point of composition vs. committed text."""
    app, changes = _focused_view_app()
    app.dispatch(_composition(NIHONGO))
    assert changes == []

    # Cancelling (empty composition) likewise commits nothing.
    app.dispatch(_composition(""))
    assert changes == []


def test_commit_fires_a_change_with_the_committed_text():
    app, changes = _focused_view_app()
    app.dispatch(_composition(NIHONGO))
    app.dispatch(_text_input(NIHONGO))

    assert changes, "commit did not reach the host's on_change"
    assert NIHONGO in changes[-1]


def test_composition_cursor_out_of_range_is_survivable():
    """Byte 1 is mid-codepoint (日 is 3 bytes), byte 99 is past the end, and -1
    means "no cursor reported". The engine clamps to UTF-8 boundaries on
    dispatch rather than slicing a codepoint in half, so none of these may
    throw or commit anything."""
    app, changes = _focused_view_app()

    app.dispatch(_composition(NIHONGO, cursor=1))
    app.dispatch(_composition(NIHONGO, cursor=99))
    app.dispatch(_composition(NIHONGO, cursor=-1))

    assert app.document().text_input_active() is True
    assert changes == []


def test_clause_range_round_trips_through_dispatch():
    """The clause range marks the segment the IME is actively converting — it
    is what drives the thicker underline. Out-of-range clauses must be clamped,
    not fatal."""
    app, changes = _focused_view_app()

    app.dispatch(_composition(NIHONGO, cursor=9, clause=(3, 9)))
    app.dispatch(_composition(NIHONGO, cursor=9, clause=(0, 999)))
    app.dispatch(_composition(NIHONGO, cursor=9, clause=(9, 3)))  # inverted

    assert app.document().text_input_active() is True
    assert changes == []


def test_focus_loss_clears_the_composition():
    """Clicking away while composing must not leave a dangling preedit — and
    must not silently commit it into the value either."""
    app, changes = _focused_view_app()
    app.dispatch(_composition(NIHONGO))

    blur = ui.Event()
    blur.type = ui.EventType.MouseDown
    blur.button = ui.MouseButton.Left
    blur.pos = ui.Point(2, 2)  # background, not the field
    app.dispatch(blur)

    assert app.document().text_input_active() is False
    # The point of the test: losing focus must DROP the preedit, not quietly
    # commit it. Without this the assertion above would pass even if the
    # composition text had been written into the value on the way out.
    assert changes == []
