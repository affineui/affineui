"""Options bar: per-tool controls (web reference §2, PS.OPTIONS).

Each tool renders the exact control set of the web app. Values persist in
``app.tool_options[tool_id]`` so switching tools round-trips state.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

import affineui as ui

from .specs import TOOL_BLENDS, TOOL_SIZE_RANGE

if TYPE_CHECKING:
    from .app import PhotoEditApp


def _slider(app: "PhotoEditApp", v: ui.View, opt: str, label: str,
            lo: float, hi: float) -> None:
    value = float(app.tool_option(opt, lo))
    v.slider(label, value, lo, hi, key=f"ps-o-{opt}").on_change(
        lambda text, opt=opt: app.set_tool_option(opt, text, numeric=True)
    )


def _number(app: "PhotoEditApp", v: ui.View, opt: str, label: str) -> None:
    value = app.tool_option(opt, 0)
    ref = v.input(label, str(value), type="number", key=f"ps-o-{opt}-n")
    ref.cls("ps-opt-num")
    ref.on_change(
        lambda text, opt=opt: app.set_tool_option(opt, text, numeric=True)
    )


def _check(app: "PhotoEditApp", v: ui.View, opt: str, label: str) -> None:
    checked = bool(app.tool_option(opt, False))
    v.checkbox(label, checked, key=f"ps-o-{opt}").on_change(
        lambda text, opt=opt: app.set_tool_option(
            opt, text.strip().lower() in ("true", "1", "on", "checked"))
    )


def _select(app: "PhotoEditApp", v: ui.View, opt: str, label: str,
            options: tuple[str, ...]) -> None:
    selected = str(app.tool_option(opt, options[0]))
    v.dropdown(label, list(options), selected, key=f"ps-o-{opt}").on_change(
        lambda text, opt=opt: app.set_tool_option(opt, text)
    )


def _badge(v: ui.View, text: str, key: str) -> None:
    v.html(f'<span class="dcs-badge dcs-badge--soft">{text}</span>', key=key)


def _size_row(app: "PhotoEditApp", v: ui.View) -> None:
    lo, hi = TOOL_SIZE_RANGE.get(app.tool, (1, 400))
    _slider(app, v, "size", "Size", lo, hi)
    _number(app, v, "size", "")


def build(app: "PhotoEditApp", v: ui.View) -> None:
    tool = app.tool
    if tool == "brush":
        _size_row(app, v)
        _slider(app, v, "hardness", "Hardness", 0, 100)
        _slider(app, v, "opacity", "Opacity", 1, 100)
        _slider(app, v, "flow", "Flow", 1, 100)
        _select(app, v, "mode", "Mode", TOOL_BLENDS)
    elif tool == "pencil":
        _size_row(app, v)
        _slider(app, v, "opacity", "Opacity", 1, 100)
        _select(app, v, "mode", "Mode", TOOL_BLENDS)
    elif tool == "eraser":
        _size_row(app, v)
        _slider(app, v, "hardness", "Hardness", 0, 100)
        _slider(app, v, "opacity", "Opacity", 1, 100)
    elif tool == "clone":
        _size_row(app, v)
        _slider(app, v, "hardness", "Hardness", 0, 100)
        _slider(app, v, "opacity", "Opacity", 1, 100)
        _badge(v, "Source set ✓" if app.clone_source_set
               else "Alt-click to set source", "ps-clone-state")
    elif tool == "history":
        _size_row(app, v)
        _slider(app, v, "opacity", "Opacity", 1, 100)
    elif tool in ("dodge", "burn"):
        _size_row(app, v)
        _slider(app, v, "exposure", "Exposure", 1, 100)
    elif tool in ("smudge", "blur"):
        _size_row(app, v)
        _slider(app, v, "strength", "Strength", 1, 100)
    elif tool == "fill":
        _slider(app, v, "tolerance", "Tolerance", 0, 128)
        _slider(app, v, "opacity", "Opacity", 1, 100)
        _check(app, v, "contiguous", "Contiguous")
    elif tool == "gradient":
        _slider(app, v, "opacity", "Opacity", 1, 100)
        _badge(v, "Foreground → Transparent", "ps-gradient-note")
    elif tool == "marquee":
        selected = str(app.tool_option("mode", "New"))
        v.button_group("", ["New", "Add", "Subtract"], selected,
                       key="ps-marquee-mode").on_change(
            lambda text: app.set_tool_option("mode", text))
        _number(app, v, "feather", "Feather")
        v.text("px", key="ps-feather-px")
    elif tool == "lasso":
        _number(app, v, "feather", "Feather")
        v.text("px", key="ps-feather-px")
        _check(app, v, "antialias", "Anti-alias")
    elif tool == "wand":
        _slider(app, v, "tolerance", "Tolerance", 0, 128)
        _check(app, v, "contiguous", "Contiguous")
    elif tool == "move":
        _check(app, v, "autoselect", "Auto-Select")
        _badge(v, "Drag to move layer pixels", "ps-move-note")
    elif tool == "crop":
        _select(app, v, "ratio", "Ratio", ("Free", "1:1", "4:3", "16:9"))
        apply_btn = v.button("Apply", primary=True, key="ps-crop-apply")
        apply_btn.cls("dcs-btn--sm")
        apply_btn.on_click(app.apply_crop)
    elif tool == "type":
        _select(app, v, "font", "Font",
                ("IBM Plex Sans", "JetBrains Mono", "Georgia"))
        _number(app, v, "size", "Size")
        v.input("Text", str(app.tool_option("text", "Decius")),
                key="ps-type-text").on_change(
            lambda text: app.set_tool_option("text", text))
    elif tool == "zoom":
        for key, label, cb in (
            ("ps-zoom-in", "+", lambda: app.zoom_step(1.4)),
            ("ps-zoom-out", "−", lambda: app.zoom_step(1 / 1.4)),
            ("ps-zoom-100", "100%", lambda: app.set_zoom(1.0)),
            ("ps-zoom-fit", "Fit Screen", app.fit_to_screen),
        ):
            v.button(label, key=key).cls("dcs-btn--sm").on_click(cb)
    elif tool == "shape":
        fill = str(app.tool_option("fill", "") or app.fg)
        v.color_field("Fill", fill, key="ps-shape-chip").on_change(
            lambda text: app.set_tool_option("fill", text))
        _number(app, v, "radius", "Radius")
    elif tool == "pen":
        _badge(v, "Click to place anchor points · demo path preview",
               "ps-pen-note")
    elif tool == "hand":
        _badge(v, "Drag the document to pan · scroll to pan · "
               "Space + drag anytime", "ps-hand-note")
    else:
        v.paragraph(app.tool_spec.tip, classes="ps-opt-note",
                    key="ps-opt-note")
