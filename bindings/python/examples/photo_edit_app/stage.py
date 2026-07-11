"""Document dock, stage, and rulers.

The document itself is a custom-paint canvas ("ps-stage"): the C++ raster
core draws the composited pixels (with zoom/pan applied) directly through
the renderer's Painter each frame — no Python in the render path. This
module builds the retained DOM around it: rulers whose ticks track the
core's view transform, the marching-ants marquee overlay, the grid
overlay, and the cursor/doc-size badges.

During pointer drags the canvas repaints via request_custom_repaint and
only the marquee/badge elements are mutated in place; the full view
reload happens on discrete state changes.
"""

from __future__ import annotations

import math
from html import escape
from typing import TYPE_CHECKING

import affineui as ui

if TYPE_CHECKING:
    from .app import PhotoEditApp

# Web ruler tick candidates (first with step*zoom >= 48px wins).
_TICK_STEPS = (1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000)

# Stage-local offset of the stage box inside the stagewrap (ruler gutters).
RULER_GUTTER = 18


def marquee_style(app: "PhotoEditApp") -> str:
    """Inline style for the marquee overlay, in stage-local pixels."""
    sel = app.doc.selection()
    if sel is None:
        return "display:none"
    rect = app.doc.stage_rect()
    if rect is None:
        return "display:none"
    ox, oy = app.doc.doc_origin()
    z = app.doc.zoom()
    x, y, w, h = sel
    left = ox - rect[0] + x * z
    top = oy - rect[1] + y * z
    return (f"left:{left:.1f}px;top:{top:.1f}px;"
            f"width:{w * z:.1f}px;height:{h * z:.1f}px")


def grid_style(app: "PhotoEditApp") -> str:
    """Inline style placing the grid overlay over the document rect."""
    rect = app.doc.stage_rect()
    if rect is None:
        return "display:none"
    ox, oy = app.doc.doc_origin()
    z = app.doc.zoom()
    return (f"left:{ox - rect[0]:.1f}px;top:{oy - rect[1]:.1f}px;"
            f"width:{app.doc.width() * z:.1f}px;"
            f"height:{app.doc.height() * z:.1f}px;z-index:901")


def _ruler_ticks(app: "PhotoEditApp", horizontal: bool) -> str:
    """Absolutely-positioned tick spans tracking the core view transform.

    `origin` is where document coordinate 0 falls in ruler-local pixels, so
    every tick is `origin + d * zoom` — the ticks slide and re-space as the
    core's pan/zoom changes (the view rebuild that redraws them runs each
    frame of a pan drag; see PhotoEditApp._on_frame).

    The ruler measures the whole visible STAGE, not just the image: document
    coordinates run negative left of / above the image and past its far edge,
    exactly like the web reference. Clamping ticks to 0..doc_len left the
    ruler blank wherever the canvas showed empty stage — which, zoomed in, is
    most of it.
    """
    z = max(1e-6, app.doc.zoom())
    step = next((s for s in _TICK_STEPS if s * z >= 48), 1000)
    rect = app.doc.stage_rect()
    if rect is None:
        origin = 0.0
        length = 4000.0
    else:
        ox, oy = app.doc.doc_origin()
        origin = (ox - rect[0]) if horizontal else (oy - rect[1])
        length = float(rect[2] if horizontal else rect[3])
    # First tick at or before the ruler's left/top edge, snapped to `step`.
    first = math.floor(-origin / z / step) * step
    spans = []
    d = first
    while True:
        sx = origin + d * z
        if sx > length:
            break
        if sx >= 0:
            edge = "left" if horizontal else "top"
            spans.append(f'<span style="{edge}:{sx:.1f}px">{d}</span>')
        d += step
    return f'<div class="ps-ruler-ticks">{"".join(spans)}</div>'


def build_document_body(app: "PhotoEditApp", v: ui.View) -> None:
    """The document pane's CONTENT. The pane/tab chrome itself is declared
    via View.document() inside the workarea's document_view — that is what
    makes the stage the dock tree's center that palettes can dock around."""
    v.container(classes="ps-stagewrap", key="ps-stagewrap",
                build=lambda s: _build_stage(app, s))


def _build_stage(app: "PhotoEditApp", v: ui.View) -> None:
    v.container(classes="ps-ruler-corner", key="ps-ruler-corner")
    if app.show_rulers:
        v.container(classes="ps-ruler ps-ruler--h", key="ps-ruler-h",
                    build=lambda r: r.html(_ruler_ticks(app, True)))
        v.container(classes="ps-ruler ps-ruler--v", key="ps-ruler-v",
                    build=lambda r: r.html(_ruler_ticks(app, False)))
    stage = v.container(classes="ps-stage", key="ps-stage",
                        build=lambda s: _build_stage_content(app, s))
    stage.attr("style", f"cursor:{app.tool_cursor()}")
    v.container(
        classes="ps-stage-badge ps-stage-badge--bl", key="ps-ov-cursor",
        build=lambda h: h.html(
            '<span id="ps-cursor-text" class="dcs-badge dcs-badge--soft">'
            "0, 0 px</span>"))
    v.container(
        classes="ps-stage-badge ps-stage-badge--tr", key="ps-ov-doc",
        build=lambda h: h.html(
            '<span class="dcs-badge dcs-badge--soft">'
            f"{app.doc.width()} × {app.doc.height()}</span>"))


def _build_stage_content(app: "PhotoEditApp", v: ui.View) -> None:
    # The raster core paints the whole document view into this element.
    v.canvas("ps-stage", classes="ps-stage-canvas", key="ps-stage-canvas")
    marquee = v.container(classes="ps-marquee", key="ps-marquee")
    marquee.attr("id", "ps-marquee")
    marquee.attr("style", marquee_style(app))
    if app.show_grid:
        grid = v.container(classes="ps-grid-overlay", key="ps-grid-overlay")
        grid.attr("style", grid_style(app))
