"""Modal dialogs (web reference §7, dialogs.js).

A small declarative dialog framework: each dialog is a DialogSpec with typed
fields; field edits land in ``app.dialog_values``; OK dispatches on the spec
id. Field defaults, ranges, and OK behavior mirror the web app at the
document-state level.
"""

from __future__ import annotations

from dataclasses import dataclass
from html import escape
from typing import TYPE_CHECKING

import affineui as ui

from . import colors
from .specs import DIALOG_BLENDS, DOC_PRESETS, PLACE_ASSETS, SHORTCUTS

if TYPE_CHECKING:
    from .app import PhotoEditApp


@dataclass(frozen=True)
class Field:
    kind: str  # section|static|text|number|select|slider|check|color|anchor|kbd|badges
    key: str = ""
    label: str = ""
    value: object = None
    options: tuple[str, ...] = ()
    lo: float = 0.0
    hi: float = 100.0
    badges: tuple[str, ...] = ()


@dataclass(frozen=True)
class DialogSpec:
    id: str
    title: str
    icon: str
    width: int
    fields: tuple[Field, ...]
    ok_label: str = "OK"


def _num(app: "PhotoEditApp", key: str, fallback: float) -> float:
    try:
        return float(str(app.dialog_values.get(key, fallback)))
    except (TypeError, ValueError):
        return fallback


def _text(app: "PhotoEditApp", key: str, fallback: str = "") -> str:
    value = app.dialog_values.get(key, fallback)
    return str(value) if value is not None else fallback


# ── Dialog constructors ──────────────────────────────────────────────────────

def open_new_doc(app: "PhotoEditApp") -> None:
    app.show_dialog(DialogSpec(
        "new_doc", "New Document", "file", 420, (
            Field("text", "name", "Name", "Untitled-1"),
            Field("select", "preset", "Preset", "Custom",
                  tuple(DOC_PRESETS.keys())),
            Field("section", label="Size & resolution"),
            Field("number", "width", "Width", 1280),
            Field("number", "height", "Height", 800),
            Field("number", "resolution", "Resolution", 72),
            Field("select", "mode", "Color Mode", "RGB/8",
                  ("RGB/8", "RGB/16", "Grayscale", "CMYK/8")),
            Field("select", "background", "Background", "White",
                  ("White", "Black", "Transparent", "Foreground")),
        ), ok_label="Create"))


def open_image_size(app: "PhotoEditApp") -> None:
    w, h = app.doc.width(), app.doc.height()
    mb = w * h * 4 / 1048576
    app.show_dialog(DialogSpec(
        "image_size", "Image Size", "aspect", 400, (
            Field("static", label=f"Current: {w} × {h}px · {mb:.2f}M"),
            Field("section", label="Dimensions"),
            Field("number", "width", "Width", w),
            Field("number", "height", "Height", h),
            Field("number", "resolution", "Resolution", 72),
            Field("check", "constrain", "Constrain", True),
            Field("check", "resample", "Resample", True),
        ), ok_label="Resize"))
    app.dialog_values["_aspect"] = w / max(1, h)


def open_canvas_size(app: "PhotoEditApp") -> None:
    w, h = app.doc.width(), app.doc.height()
    mb = w * h * 4 / 1048576
    app.show_dialog(DialogSpec(
        "canvas_size", "Canvas Size", "fit", 400, (
            Field("static", label=f"Current: {w} × {h}px · {mb:.2f}M"),
            Field("number", "width", "Width", w),
            Field("number", "height", "Height", h),
            Field("anchor", "anchor", "Anchor", "mc"),
        ), ok_label="Resize"))


def open_new_layer(app: "PhotoEditApp") -> None:
    app.show_dialog(DialogSpec(
        "new_layer", "New Layer", "plus", 380, (
            Field("text", "name", "Name", app.next_layer_name()),
            Field("select", "mode", "Mode", "Normal", DIALOG_BLENDS),
            Field("slider", "opacity", "Opacity", 100, lo=0, hi=100),
        ), ok_label="Create"))


def open_fill(app: "PhotoEditApp") -> None:
    app.show_dialog(DialogSpec(
        "fill", "Fill", "fill", 380, (
            Field("select", "contents", "Contents", "Foreground",
                  ("Foreground", "Background", "Black", "White", "50% Gray")),
            Field("section", label="Blending"),
            Field("select", "mode", "Mode", "Normal", DIALOG_BLENDS),
            Field("slider", "opacity", "Opacity", 100, lo=1, hi=100),
        ), ok_label="Fill"))


def open_stroke(app: "PhotoEditApp") -> None:
    app.show_dialog(DialogSpec(
        "stroke", "Stroke", "edit", 380, (
            Field("number", "width", "Width px", 4),
            Field("color", "color", "Color", app.fg),
            Field("select", "location", "Location", "Center",
                  ("Inside", "Center", "Outside")),
            Field("slider", "opacity", "Opacity", 100, lo=1, hi=100),
        ), ok_label="Stroke"))


def open_feather(app: "PhotoEditApp") -> None:
    app.show_dialog(DialogSpec(
        "feather", "Feather Selection", "blur", 340, (
            Field("number", "radius", "Radius px", 8),
        )))


def open_export(app: "PhotoEditApp") -> None:
    app.show_dialog(DialogSpec(
        "export", "Export As", "export", 400, (
            Field("select", "format", "Format", "PNG",
                  ("PNG", "JPG", "WEBP")),
            Field("slider", "quality", "Quality", 92, lo=10, hi=100),
            Field("select", "scale", "Scale", "1×", ("1×", "2×", "0.5×")),
            Field("static",
                  label="Exports a flattened image of the document."),
        ), ok_label="Export"))


def open_shortcuts(app: "PhotoEditApp") -> None:
    app.show_dialog(DialogSpec(
        "shortcuts", "Keyboard Shortcuts", "keys", 460, (
            Field("kbd",),
        ), ok_label="Done"))


def open_place(app: "PhotoEditApp") -> None:
    app.show_dialog(DialogSpec(
        "place", "Place Embedded", "import", 380, (
            Field("static",
                  label="Places a generated asset as a new layer."),
            Field("select", "asset", "Asset", "Sun flare",
                  tuple(PLACE_ASSETS.keys())),
        ), ok_label="Place"))


def open_about(app: "PhotoEditApp", framework: bool) -> None:
    title = "About decius.css" if framework else "Decius PhotoEditor"
    blurb = ("A dense, token-driven CSS framework for pro creative tools."
             if framework else
             "A Python AffineUI recreation of the Decius photo editing "
             "sample, backed by the C++ photo document core.")
    app.show_dialog(DialogSpec(
        "about", title, "decius" if framework else "info", 420, (
            Field("static", label=blurb),
            Field("badges", badges=("decius.css 0.5.3", "IBM Plex Sans",
                                    "225 icons", "flat · comfortable")),
        ), ok_label="Close"))


def open_color_picker(app: "PhotoEditApp", target: str) -> None:
    current = app.fg if target == "fg" else app.bg
    label = "Foreground" if target == "fg" else "Background"
    app.show_dialog(DialogSpec(
        f"picker:{target}", f"{label} Color", "palette", 360, (
            Field("color", "color", label, current),
            Field("text", "hex", "Hex", current),
        )))


# Adjustment modals (§8): live-preview in the web; here the slider values are
# applied as a CSS filter on OK. (Live preview needs pointer/drag streaming —
# Phase B.)
def open_adjust(app: "PhotoEditApp", key: str, label: str) -> None:
    fields: tuple[Field, ...]
    if key == "bc":
        fields = (Field("slider", "brightness", "Brightness", 0,
                        lo=-100, hi=100),
                  Field("slider", "contrast", "Contrast", 0,
                        lo=-100, hi=100))
    elif key == "hsl":
        fields = (Field("slider", "hue", "Hue", 0, lo=-180, hi=180),
                  Field("slider", "saturation", "Saturation", 0,
                        lo=-100, hi=100),
                  Field("slider", "lightness", "Lightness", 0,
                        lo=-100, hi=100))
    elif key == "levels":
        fields = (Field("slider", "black", "Black", 0, lo=0, hi=254),
                  Field("slider", "white", "White", 255, lo=1, hi=255),
                  Field("slider", "gamma", "Gamma", 100, lo=10, hi=300))
    elif key == "vibrance":
        fields = (Field("slider", "amount", "Vibrance", 0,
                        lo=-100, hi=100),)
    elif key == "fblur":
        fields = (Field("slider", "amount", "Amount", 4, lo=0, hi=40),)
    elif key == "fnoise":
        fields = (Field("slider", "amount", "Amount", 25, lo=0, hi=100),)
    else:  # Generic fallback modal (curves/balance/mixer/photo/exposure).
        fields = (Field("slider", "amount", "Amount", 0, lo=-100, hi=100),)
    app.show_dialog(DialogSpec(f"adjust:{key}", label,
                               "color-grade", 380, fields))


# ── Field-change hooks (web onField) ─────────────────────────────────────────

def field_changed(app: "PhotoEditApp", spec: DialogSpec, key: str) -> None:
    if spec.id == "new_doc" and key == "preset":
        preset = _text(app, "preset", "Custom")
        if preset in DOC_PRESETS and preset != "Custom":
            w, h = DOC_PRESETS[preset]
            app.dialog_values["width"] = w
            app.dialog_values["height"] = h
            app.reload()
    elif spec.id == "image_size" and key in ("width", "height"):
        if not app.dialog_values.get("constrain", True):
            return
        aspect = float(app.dialog_values.get("_aspect", 1.6)) or 1.6
        if key == "width":
            app.dialog_values["height"] = max(
                1, round(_num(app, "width", 1) / aspect))
        else:
            app.dialog_values["width"] = max(
                1, round(_num(app, "height", 1) * aspect))
        app.reload()


# ── OK dispatch ──────────────────────────────────────────────────────────────

def handle_ok(app: "PhotoEditApp", spec: DialogSpec) -> None:
    sid = spec.id
    if sid == "new_doc":
        _ok_new_doc(app)
    elif sid == "image_size":
        w = max(1, int(_num(app, "width", app.doc.width())))
        h = max(1, int(_num(app, "height", app.doc.height())))
        # Phase B: real pixel resampling; state-level resize only.
        app.doc.resize_document(w, h, "Image Size", "aspect")
        app.toast(f"Image resized to {w} × {h}px")
    elif sid == "canvas_size":
        w = max(1, int(_num(app, "width", app.doc.width())))
        h = max(1, int(_num(app, "height", app.doc.height())))
        # Phase B: anchor-relative content placement; dims-only here.
        app.doc.resize_document(w, h, "Canvas Size", "fit")
        app.toast(f"Canvas resized to {w} × {h}px "
                  f"(anchor {_text(app, 'anchor', 'mc')})")
    elif sid == "new_layer":
        app.doc.add_layer(_text(app, "name", app.next_layer_name()), "pixel",
                          "", _text(app, "mode", "Normal"),
                          _num(app, "opacity", 100), "New Layer", "plus")
        app.status = "New Layer"
    elif sid == "fill":
        _ok_fill(app)
    elif sid == "stroke":
        _ok_stroke(app)
    elif sid == "feather":
        # Matches the web: the radius is stored but never applied.
        app.sel_feather = _num(app, "radius", 8)
        app.toast(f"Feather set to {round(app.sel_feather)}px")
    elif sid == "export":
        ext = _text(app, "format", "PNG").lower()
        app.toast(f"Exported decius-photoeditor.{ext}")
    elif sid == "place":
        asset = _text(app, "asset", "Sun flare")
        style, blend, opacity = PLACE_ASSETS.get(
            asset, PLACE_ASSETS["Sun flare"])
        app.doc.add_layer(asset, "pixel", style, blend, opacity,
                          f"Place {asset}", "import")
        app.status = f"Placed {asset}"
    elif sid.startswith("picker:"):
        target = sid.split(":", 1)[1]
        color = (colors.normalize_hex(_text(app, "hex", ""))
                 or colors.normalize_hex(_text(app, "color", ""))
                 or (app.fg if target == "fg" else app.bg))
        if target == "fg":
            app.set_foreground(color, reload=False)
        else:
            app.bg = color
            app.status = f"Background {color}"
    elif sid.startswith("adjust:"):
        _ok_adjust(app, sid.split(":", 1)[1], spec.title)
    # "shortcuts" / "about" need no OK action.


def _ok_new_doc(app: "PhotoEditApp") -> None:
    name = _text(app, "name", "Untitled-1") or "Untitled-1"
    w = max(1, int(_num(app, "width", 1280)))
    h = max(1, int(_num(app, "height", 800)))
    background = _text(app, "background", "White")
    style = {
        "White": "background:#ffffff",
        "Black": "background:#000000",
        "Transparent": "",
        "Foreground": f"background:{app.fg}",
    }.get(background, "background:#ffffff")
    app.doc.new_document(w, h, style)
    app.doc_name = name
    app.color_mode = _text(app, "mode", "RGB/8")
    app.selection = None
    app.set_zoom(app.fit_zoom(), reload=False)
    app.toast(f"Created {name} — {w} × {h}px")


def _ok_fill(app: "PhotoEditApp") -> None:
    contents = _text(app, "contents", "Foreground")
    color = {
        "Foreground": app.fg,
        "Background": app.bg,
        "Black": "#000000",
        "White": "#ffffff",
        "50% Gray": "#808080",
    }.get(contents, app.fg)
    # Blend-mode fills are Phase-B pixel work; opacity is honored.
    if app.doc.fill_active(color, _num(app, "opacity", 100), "Fill"):
        app.status = "Fill"
    else:
        app.status = "Layer is locked"


def _ok_stroke(app: "PhotoEditApp") -> None:
    width = max(1, int(_num(app, "width", 4)))
    color = colors.normalize_hex(_text(app, "color", app.fg)) or app.fg
    opacity = _num(app, "opacity", 100) / 100.0
    rgba = colors.hex_to_rgba(color, opacity)
    # CSS approximation: inset box-shadow strokes the layer bounds
    # (selection-rect strokes and inside/outside placement are Phase B).
    if app.doc.append_active_style(
            f"box-shadow:inset 0 0 0 {width}px {rgba}", "Stroke", "edit"):
        app.status = "Stroke"
    else:
        app.status = "Layer is locked"


def _ok_adjust(app: "PhotoEditApp", key: str, title: str) -> None:
    if key == "bc":
        b = _num(app, "brightness", 0)
        c = _num(app, "contrast", 0)
        css = f"brightness({1 + b * 0.012:.3f}) contrast({1 + c * 0.01:.3f})"
        applied = app.doc.adjust_active(css, "Brightness/Contrast", "light")
    elif key == "hsl":
        h = _num(app, "hue", 0)
        s = _num(app, "saturation", 0)
        l = _num(app, "lightness", 0)
        css = (f"hue-rotate({h:.0f}deg) saturate({1 + s * 0.01:.3f}) "
               f"brightness({1 + l * 0.01:.3f})")
        applied = app.doc.adjust_active(css, "Hue/Saturation", "color-grade")
    elif key == "levels":
        lo = _num(app, "black", 0)
        hi = max(lo + 1, _num(app, "white", 255))
        gamma = max(10.0, _num(app, "gamma", 100))
        css = (f"contrast({255 / (hi - lo):.3f}) "
               f"brightness({gamma / 100:.3f})")
        applied = app.doc.adjust_active(css, "Levels", "graph")
    elif key == "vibrance":
        css = f"saturate({1 + _num(app, 'amount', 0) * 0.01:.3f})"
        applied = app.doc.adjust_active(css, "Vibrance", "wave-sine")
    elif key == "fblur":
        amount = max(0.0, _num(app, "amount", 4))
        applied = app.doc.adjust_active(f"blur({amount:.0f}px)",
                                        "Gaussian Blur", "blur")
    elif key == "fnoise":
        # Phase B: per-pixel noise has no CSS analogue.
        app.toast("Add Noise is Phase-B pixel work")
        return
    else:
        amount = _num(app, "amount", 0)
        css = (f"brightness({1 + amount * 0.005:.3f}) "
               f"contrast({1 + amount * 0.002:.3f})")
        applied = app.doc.adjust_active(css, title, "color-grade")
    app.status = title if applied else "Layer is locked"


# ── Rendering ────────────────────────────────────────────────────────────────

def build_dialog(app: "PhotoEditApp", v: ui.View) -> None:
    spec = app.dialog
    if spec is None:
        return

    def panel(p: ui.View) -> None:
        def header(h: ui.View) -> None:
            h.html(f'<i class="di di-{escape(spec.icon)}"></i>'
                   f'<span class="dcs-panel__title">{escape(spec.title)}'
                   "</span>", key="ps-dlg-title")
            close = h.container(classes="ps-dlg-close", key="ps-dlg-close",
                                build=lambda c: c.html(
                                    '<i class="di di-close"></i>'))
            close.attr("role", "button")
            close.on_click(app.close_dialog)

        p.container(classes="dcs-panel__header", key="ps-dialog-header",
                    build=header)
        p.container(classes="ps-dlg-body", key="ps-dialog-body",
                    build=lambda b: _build_fields(app, spec, b))
        p.container(classes="ps-dialog-actions", key="ps-dialog-actions",
                    build=lambda a: (
                        a.button("Cancel",
                                 key="ps-dialog-cancel").on_click(
                            app.close_dialog),
                        a.button(spec.ok_label, primary=True,
                                 key="ps-dialog-ok").on_click(app.dialog_ok),
                    ))

    ref = v.container(classes="dcs-panel dcs-panel--floating ps-dialog",
                      key="ps-dialog", build=panel)
    ref.attr("style", f"width:{spec.width}px")


def _build_fields(app: "PhotoEditApp", spec: DialogSpec, v: ui.View) -> None:
    for index, field in enumerate(spec.fields):
        key = field.key or f"f{index}"
        if field.kind == "section":
            v.paragraph(field.label, classes="ps-dlg-sec",
                        key=f"dlg-sec-{index}")
        elif field.kind == "static":
            v.paragraph(field.label, classes="ps-dlg-static",
                        key=f"dlg-static-{index}")
        elif field.kind == "text":
            v.input(field.label, _text(app, key, str(field.value or "")),
                    key=f"dlg-{key}").on_change(
                lambda text, key=key: app.set_dialog_value(key, text))
        elif field.kind == "number":
            v.input(field.label, str(app.dialog_values.get(key, field.value)),
                    type="number", key=f"dlg-{key}").on_change(
                lambda text, key=key, spec=spec:
                app.set_dialog_value(key, text, numeric=True, spec=spec))
        elif field.kind == "select":
            v.dropdown(field.label, list(field.options),
                       _text(app, key, str(field.value or "")),
                       key=f"dlg-{key}").on_change(
                lambda text, key=key, spec=spec:
                app.set_dialog_value(key, text, spec=spec))
        elif field.kind == "slider":
            v.slider(field.label,
                     float(app.dialog_values.get(key, field.value) or 0),
                     field.lo, field.hi, key=f"dlg-{key}").on_change(
                lambda text, key=key: app.set_dialog_value(
                    key, text, numeric=True))
        elif field.kind == "check":
            v.checkbox(field.label,
                       bool(app.dialog_values.get(key, field.value)),
                       key=f"dlg-{key}").on_change(
                lambda text, key=key: app.set_dialog_value(
                    key, text.strip().lower() in ("true", "1", "on",
                                                  "checked")))
        elif field.kind == "color":
            v.color_field(field.label,
                          _text(app, key, str(field.value or "#000000")),
                          key=f"dlg-{key}").on_change(
                lambda text, key=key: app.set_dialog_value(key, text))
        elif field.kind == "anchor":
            _build_anchor_grid(app, v, key)
        elif field.kind == "kbd":
            _build_kbd_list(v)
        elif field.kind == "badges":
            badges = "".join(
                f'<span class="dcs-badge">{escape(b)}</span>'
                for b in field.badges)
            v.html(f'<div class="ps-badge-row">{badges}</div>',
                   key=f"dlg-badges-{index}")


def _build_anchor_grid(app: "PhotoEditApp", v: ui.View, key: str) -> None:
    selected = _text(app, key, "mc")

    def row(r: ui.View) -> None:
        r.paragraph("Anchor", classes="ps-anchor-label", key="dlg-anchor-lbl")

        def grid(g: ui.View) -> None:
            for anchor in ("tl", "tc", "tr", "ml", "mc", "mr",
                           "bl", "bc", "br"):
                cell = g.container(classes="ps-anchor",
                                   key=f"dlg-anchor-{anchor}")
                cell.attr("role", "button")
                cell.attr("aria-pressed",
                          "true" if anchor == selected else "false")
                cell.on_click(lambda anchor=anchor, key=key:
                              app.set_dialog_value(key, anchor,
                                                   reload=True))

        r.container(classes="ps-anchor-grid", key="dlg-anchor-grid",
                    build=grid)

    v.container(classes="ps-anchor-row", key="dlg-anchor-row", build=row)


def _build_kbd_list(v: ui.View) -> None:
    def rows(k: ui.View) -> None:
        for key, label in SHORTCUTS:
            k.html(f"<p><kbd>{escape(key)}</kbd>"
                   f"<span>{escape(label)}</span></p>",
                   key=f"shortcut-{key}")

    v.container(classes="ps-kbd-list", key="ps-kbd-list", build=rows)
