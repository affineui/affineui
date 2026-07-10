"""Document dock, stage, rulers, and the CSS-layer canvas renderer.

The document is rendered as a stack of styled divs — one per visible layer —
inside a transformed ``.ps-doc`` block (translate/scale, like the web's
``applyView``). Layer blend modes map to CSS ``mix-blend-mode``; layer opacity
and fill multiply into div opacity.
"""

from __future__ import annotations

from html import escape
from typing import TYPE_CHECKING

import affineui as ui

from .specs import BLEND_TO_CSS

if TYPE_CHECKING:
    from .app import PhotoEditApp

# Web ruler tick candidates (first with step*zoom >= 48px wins).
_TICK_STEPS = (1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000)


def layer_div_style(layer: ui.PhotoLayerSnapshot, z_index: int) -> str:
    """Inline style for one composited layer div."""
    alpha = (layer.opacity / 100.0) * (layer.fill / 100.0)
    parts = [f"z-index:{z_index}", f"opacity:{alpha:.3f}"]
    blend = BLEND_TO_CSS.get(layer.blend, "normal")
    if blend != "normal":
        parts.append(f"mix-blend-mode:{blend}")
    if layer.style:
        parts.append(layer.style)
    return ";".join(parts)


def render_layer_stack(app: "PhotoEditApp", v: ui.View, prefix: str) -> None:
    """Emit the visible layers, bottom→top, as absolutely-inset divs."""
    for index, layer in enumerate(app.doc.layers()):
        if not layer.visible:
            continue
        classes = "ps-layer-canvas"
        if layer.kind == "text":
            classes += " ps-layer-text"
        ref = v.container(classes=classes, key=f"{prefix}-{layer.id}",
                          build=(lambda t, layer=layer: _text_layer(t, layer))
                          if layer.kind == "text" else None)
        ref.attr("style", layer_div_style(layer, index))


def _text_layer(v: ui.View, layer: ui.PhotoLayerSnapshot) -> None:
    # Text layers carry their copy in the layer name ("T My words" for
    # tool-created layers; the sample scene title is named "DECIUS").
    text = layer.name[2:] if layer.name.startswith("T ") else layer.name
    v.html(f'<span class="ps-layer-title">{escape(text)}</span>',
           key=f"text-{layer.id}")
    if layer.id == "decius-title":
        v.html('<span class="ps-layer-subtitle">a decius.css showcase</span>',
               key=f"subtitle-{layer.id}")


def _ruler_ticks(app: "PhotoEditApp", length: int) -> str:
    step = next((s for s in _TICK_STEPS if s * app.zoom >= 48), 1000)
    px = step * app.zoom
    spans = "".join(
        f'<span style="width:{px:.1f}px">{i * step}</span>'
        for i in range(0, max(2, int(length / step) + 2))
    )
    return f'<div class="ps-ruler-ticks">{spans}</div>'


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
                    build=lambda r: r.html(
                        _ruler_ticks(app, app.doc.width())))
        v.container(classes="ps-ruler ps-ruler--v", key="ps-ruler-v",
                    build=lambda r: r.html(
                        _ruler_ticks(app, app.doc.height())))
    stage = v.container(classes="ps-stage", key="ps-stage",
                        build=lambda s: _build_doc(app, s))
    stage.attr("role", "button")
    stage.on_click(app.stage_click)
    v.container(classes="ps-stage-badge ps-stage-badge--bl",
                key="ps-ov-cursor",
                build=lambda h: h.html(
                    '<span class="dcs-badge dcs-badge--soft">0, 0 px</span>'))
    v.container(classes="ps-stage-badge ps-stage-badge--tr", key="ps-ov-doc",
                build=lambda h: h.html(
                    '<span class="dcs-badge dcs-badge--soft">'
                    f"{app.doc.width()} × {app.doc.height()}</span>"))


def _build_doc(app: "PhotoEditApp", v: ui.View) -> None:
    doc = v.container(classes="ps-doc", key="ps-doc",
                      build=lambda d: _build_doc_content(app, d))
    doc.attr("style",
             f"width:{app.doc.width()}px;height:{app.doc.height()}px;"
             f"--z:{app.zoom:.4f};--px:{app.pan_x}px;--py:{app.pan_y}px")


def _build_doc_content(app: "PhotoEditApp", v: ui.View) -> None:
    render_layer_stack(app, v, "canvas")
    if app.selection is not None:
        x, y, w, h = app.selection
        dw, dh = max(1, app.doc.width()), max(1, app.doc.height())
        marquee = v.container(classes="ps-marquee", key="ps-marquee")
        marquee.attr("style",
                     f"left:{x / dw * 100:.2f}%;top:{y / dh * 100:.2f}%;"
                     f"width:{w / dw * 100:.2f}%;height:{h / dh * 100:.2f}%;"
                     "z-index:900")
    if app.show_grid:
        v.container(classes="ps-grid-overlay",
                    key="ps-grid-overlay").attr("style", "z-index:901")
