"""Floating surfaces: tool strip, Navigator, Color/Swatches,
Layers/Channels/Paths/Comps, Adjustments, History, and the quick floatbar.

Positions and sizes mirror the web reference (§1 "Floating surfaces").
Channels/Paths/Comps are static content, matching the web's own stubs.
"""

from __future__ import annotations

from html import escape
from typing import TYPE_CHECKING, Callable

import affineui as ui

from . import colors, stage
from .specs import (ADJUSTMENTS, CHANNELS, COMPS, LAYER_BLENDS, SWATCHES,
                    TOOLS, tool_icon_html)

if TYPE_CHECKING:
    from .app import PhotoEditApp

Build = Callable[[ui.View], None]


def _icon_button(v: ui.View, key: str, icon_html: str, title: str,
                 on_click: Callable[[], None] | None = None,
                 pressed: bool | None = None,
                 extra_classes: str = "") -> ui.WidgetRef:
    classes = "dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost"
    if extra_classes:
        classes += " " + extra_classes
    ref = v.container(classes=classes, key=key,
                      build=lambda h: h.html(icon_html))
    ref.attr("role", "button").attr("title", title)
    if pressed is not None:
        ref.attr("aria-pressed", "true" if pressed else "false")
    if on_click is not None:
        ref.on_click(on_click)
    return ref


def _di(icon: str) -> str:
    return f'<i class="di di-{escape(icon)}"></i>'


# ── Floating panel shells ────────────────────────────────────────────────────

def floating_panel(v: ui.View, key: str, style: str, header: Build,
                   body: Build) -> None:
    def panel(p: ui.View) -> None:
        head = p.container(classes="dcs-panel__header", key=f"{key}-header",
                           build=header)
        head.attr("data-dcs-drag-handle", "")
        p.container(classes="dcs-panel__body", key=f"{key}-body", build=body)

    ref = v.container(
        classes="dcs-panel dcs-panel--floating ps-floating ps-float-panel",
        key=key, build=panel)
    ref.attr("style", style)


def title_header(title: str, icon: str, key: str) -> Build:
    def build(h: ui.View) -> None:
        h.container(classes="ps-panel-title", key=f"{key}-title",
                    build=lambda s: s.html(
                        f"{_di(icon)}<span>{escape(title)}</span>"))
        h.container(classes="dcs-panel__tools", key=f"{key}-tools")

    return build


def tabs_header(app: "PhotoEditApp", key: str,
                tabs: tuple[tuple[str, str, str], ...], selected: str,
                on_select: Callable[[str], None]) -> Build:
    def build(h: ui.View) -> None:
        def build_tabs(strip: ui.View) -> None:
            for tab_id, label, icon in tabs:
                tab = strip.container(
                    classes="ps-ptab", key=f"{key}-tab-{tab_id}",
                    build=lambda t, label=label, icon=icon: t.html(
                        f"{_di(icon)}<span>{escape(label)}</span>"))
                tab.attr("role", "tab")
                tab.attr("aria-selected",
                         "true" if tab_id == selected else "false")
                tab.on_click(lambda tab_id=tab_id: on_select(tab_id))

        h.container(classes="ps-ptabs", key=f"{key}-tabs", build=build_tabs)

    return build


# ── Tool strip ───────────────────────────────────────────────────────────────

def build_toolstrip(app: "PhotoEditApp", v: ui.View) -> None:
    v.container(classes="dcs-grip", key="ps-tools-grip").attr(
        "data-dcs-drag-handle", "")

    def grid(g: ui.View) -> None:
        for tool in TOOLS:
            ref = g.container(classes="ps-tool", key=f"tool-{tool.id}",
                              build=lambda h, tool=tool: h.html(
                                  tool_icon_html(tool)))
            ref.attr("role", "button")
            ref.attr("title", f"{tool.name}  ({tool.key})")
            ref.attr("aria-pressed",
                     "true" if app.tool == tool.id else "false")
            ref.attr("data-group", "true" if tool.group else "false")
            ref.on_click(lambda tool_id=tool.id: app.set_tool(tool_id))
            if tool.sep_after:
                g.container(classes="ps-toolsep", key=f"sep-{tool.id}")

    v.container(classes="ps-toolgrid", key="ps-tools", build=grid)
    v.container(classes="ps-colorchips", key="ps-colorchips",
                build=lambda c: _build_color_chips(app, c))
    qm = v.container(classes="ps-tool", key="ps-quickmask",
                     build=lambda h: h.html(_di("layer-mask")))
    qm.attr("role", "button")
    qm.attr("title", "Edit in Quick Mask Mode (Q)")
    qm.attr("aria-pressed", "true" if app.quickmask else "false")
    qm.on_click(app.toggle_quickmask)


def _build_color_chips(app: "PhotoEditApp", v: ui.View) -> None:
    reset = v.container(classes="ps-chip-mini ps-reset", key="ps-color-reset",
                        build=lambda h: h.html("⬚"))
    reset.attr("role", "button").attr("title", "Default colors (D)")
    reset.on_click(app.reset_colors)
    swap = v.container(classes="ps-chip-mini ps-swap", key="ps-color-swap",
                       build=lambda h: h.html("⇄"))
    swap.attr("role", "button").attr("title", "Swap colors (X)")
    swap.on_click(app.swap_colors)
    bg = v.container(classes="ps-colorchip ps-colorchip--bg", key="ps-chip-bg")
    bg.attr("style", f"background:{app.bg}")
    bg.attr("role", "button").attr("title", f"Background color {app.bg}")
    bg.on_click(app.open_background_picker)
    fg = v.container(classes="ps-colorchip ps-colorchip--fg", key="ps-chip-fg")
    fg.attr("style", f"background:{app.fg}")
    fg.attr("role", "button").attr("title", f"Foreground color {app.fg}")
    fg.on_click(app.open_foreground_picker)


# ── Navigator ────────────────────────────────────────────────────────────────

def build_navigator(app: "PhotoEditApp", v: ui.View) -> None:
    floating_panel(v, "ps-navigator", "right:12px;top:12px;width:214px",
                   title_header("Navigator", "globe", "ps-navigator"),
                   lambda b: _navigator_body(app, b))


def _navigator_body(app: "PhotoEditApp", v: ui.View) -> None:
    def body(p: ui.View) -> None:
        p.container(classes="ps-nav-thumb", key="ps-nav-thumb",
                    build=lambda n: _nav_thumb(app, n))

        def zoom_row(row: ui.View) -> None:
            out = row.container(classes="ps-nav-zbtn", key="ps-nav-zoom-out",
                                build=lambda h: h.html(_di("zoom-out")))
            out.attr("role", "button")
            out.on_click(lambda: app.zoom_step(1 / 1.4))
            row.slider("", app.zoom * 100, 5, 800,
                       key="ps-nav-zoom").on_change(
                lambda text: app.set_zoom_percent_text(text))
            zin = row.container(classes="ps-nav-zbtn", key="ps-nav-zoom-in",
                                build=lambda h: h.html(_di("zoom-in")))
            zin.attr("role", "button")
            zin.on_click(lambda: app.zoom_step(1.4))
            row.container(classes="ps-nav-pct", key="ps-nav-pct",
                          build=lambda h: h.html(
                              f"{round(app.zoom * 100)}%"))

        p.container(classes="ps-nav-zoomrow", key="ps-nav-zoomrow",
                    build=zoom_row)

    v.container(classes="ps-nav-body", key="ps-nav-panel", build=body)


def _nav_thumb(app: "PhotoEditApp", v: ui.View) -> None:
    doc_w, doc_h = max(1, app.doc.width()), max(1, app.doc.height())
    box_w, box_h = 174.0, 100.0  # thumb interior minus margins
    scale = min(box_w / doc_w, box_h / doc_h)
    nav_w, nav_h = doc_w * scale, doc_h * scale

    def mini_doc(m: ui.View) -> None:
        inner = m.container(classes="ps-nav-scale", key="ps-nav-scale",
                            build=lambda s: stage.render_layer_stack(
                                app, s, "nav"))
        inner.attr("style",
                   f"position:absolute;left:0;top:0;width:{doc_w}px;"
                   f"height:{doc_h}px;transform:scale({scale:.5f});"
                   "transform-origin:0 0")

    doc_box = v.container(classes="ps-nav-doc", key="ps-nav-doc",
                          build=mini_doc)
    doc_box.attr("style",
                 f"left:50%;top:50%;width:{nav_w:.1f}px;"
                 f"height:{nav_h:.1f}px;margin-left:{-nav_w / 2:.1f}px;"
                 f"margin-top:{-nav_h / 2:.1f}px")

    # Red viewport rectangle: visible fraction of the doc at current zoom.
    stage_w, stage_h = app.stage_size()
    fx = min(1.0, stage_w / (doc_w * app.zoom))
    fy = min(1.0, stage_h / (doc_h * app.zoom))
    cx = min(max(0.5 - app.pan_x / (app.zoom * doc_w), fx / 2), 1 - fx / 2)
    cy = min(max(0.5 - app.pan_y / (app.zoom * doc_h), fy / 2), 1 - fy / 2)
    view = v.container(classes="ps-nav-view", key="ps-nav-view")
    view.attr("style",
              f"left:calc(50% + {((cx - fx / 2) - 0.5) * nav_w:.1f}px);"
              f"top:calc(50% + {((cy - fy / 2) - 0.5) * nav_h:.1f}px);"
              f"width:{fx * nav_w:.1f}px;height:{fy * nav_h:.1f}px")


# ── Color / Swatches ─────────────────────────────────────────────────────────

def build_color_panel(app: "PhotoEditApp", v: ui.View) -> None:
    tabs = (("color", "Color", "palette"), ("swatches", "Swatches", "grid"))
    floating_panel(
        v, "panel-color", "right:12px;top:226px;width:268px",
        tabs_header(app, "panel-color", tabs, app.color_tab,
                    app.set_color_tab),
        lambda b: (_color_tab(app, b) if app.color_tab == "color"
                   else _swatches_tab(app, b)))


def _color_tab(app: "PhotoEditApp", v: ui.View) -> None:
    def body(p: ui.View) -> None:
        h, s, val = app.hsv
        # SV square + hue bar reflect the foreground; pointer-precise picking
        # inside the square is Phase-B (no pointer coordinates on click). The
        # hex + RGB fields below are the committing editors.
        sv = p.container(classes="ps-sv", key="ps-sv",
                         build=lambda box: box.container(
                             classes="ps-sv-dot", key="ps-sv-dot").attr(
                             "style",
                             f"left:{s * 100:.1f}%;"
                             f"top:{(1 - val) * 100:.1f}%"))
        sv.attr("style",
                "background:linear-gradient(to top,#000,transparent),"
                f"linear-gradient(to right,#fff,hsl({h:.0f},100%,50%))")
        p.container(classes="ps-hue", key="ps-hue",
                    build=lambda bar: bar.container(
                        classes="ps-hue-dot", key="ps-hue-dot").attr(
                        "style", f"left:{h / 360 * 100:.1f}%"))
        hexfield = p.input("Hex", app.fg, key="ps-hex")
        hexfield.cls("ps-hex-field")
        hexfield.on_change(app.set_foreground_hex)

        def rgb_row(row: ui.View) -> None:
            r, g, b = colors.hex_to_rgb(app.fg)
            for channel, value in (("r", r), ("g", g), ("b", b)):
                row.input(channel.upper(), str(value), type="number",
                          key=f"ps-{channel}").on_change(
                    lambda text, channel=channel:
                    app.set_foreground_channel(channel, text))

        p.container(classes="ps-rgb-row", key="ps-rgb-row", build=rgb_row)

    v.container(classes="ps-colorpanel", key="ps-colorpanel", build=body)


def _swatches_tab(app: "PhotoEditApp", v: ui.View) -> None:
    # Web: left-click sets foreground, right-click background. The binding
    # has no context-click event, so background changes go through the bg
    # chip's picker dialog instead.
    def grid(g: ui.View) -> None:
        for color in SWATCHES:
            chip = g.container(classes="ps-swatch-chip",
                               key=f"swatch-{color[1:]}")
            chip.attr("style", f"background:{color}")
            chip.attr("role", "button").attr("title", color)
            chip.on_click(lambda color=color: app.set_foreground(color))

    v.container(classes="ps-swatches", key="ps-swatches", build=grid)


# ── Layers / Channels / Paths / Comps ────────────────────────────────────────

def build_layers_panel(app: "PhotoEditApp", v: ui.View) -> None:
    tabs = (("layers", "Layers", "layers"), ("channels", "Channels", "eq"),
            ("paths", "Paths", "spline"), ("comps", "Comps", "filmstrip"))
    builders = {
        "layers": _layers_tab,
        "channels": _channels_tab,
        "paths": _paths_tab,
        "comps": _comps_tab,
    }
    floating_panel(
        v, "panel-layers", "right:12px;top:486px;bottom:12px;width:300px",
        tabs_header(app, "panel-layers", tabs, app.layers_tab,
                    app.set_layers_tab),
        lambda b: builders[app.layers_tab](app, b))


def _layers_tab(app: "PhotoEditApp", v: ui.View) -> None:
    def body(p: ui.View) -> None:
        p.container(classes="ps-layer-filter", key="ps-layer-filter",
                    build=lambda f: _layer_filter_bar(app, f))
        p.container(classes="ps-layer-bo", key="ps-layer-bo",
                    build=lambda f: _layer_blend_row(app, f))
        p.container(classes="ps-layer-lock-row", key="ps-layer-lock-row",
                    build=lambda f: _layer_lock_row(app, f))
        p.container(classes="ps-layer-list", key="ps-layer-list",
                    build=lambda rows: _layer_rows(app, rows))
        p.container(classes="ps-layer-footer", key="ps-layer-footer",
                    build=lambda f: _layer_footer(app, f))

    v.container(classes="ps-layers", key="ps-layers", build=body)


def _layer_filter_bar(app: "PhotoEditApp", v: ui.View) -> None:
    # Visual-only filter bar, like the web (buttons don't filter yet there
    # either).
    v.dropdown("", ["Kind", "Name", "Effect", "Mode", "Attribute", "Color"],
               "Kind", key="ps-layer-kind").cls("ps-layer-kind")
    _icon_button(v, "ps-lf-pixel", _di("image"), "Pixel layers")
    _icon_button(v, "ps-lf-adjust", _di("color-grade"), "Adjustment layers")
    _icon_button(v, "ps-lf-type", '<span class="ps-tcap">T</span>',
                 "Type layers")
    _icon_button(v, "ps-lf-shape", _di("poly"), "Shape layers")
    _icon_button(v, "ps-lf-smart", _di("cube"), "Smart objects")
    v.container(classes="ps-row-spacer", key="ps-lf-spacer")
    _icon_button(v, "ps-filter-toggle", _di("bolt"), "Toggle filtering",
                 on_click=app.toggle_layer_filtering,
                 pressed=app.layer_filtering)


def _layer_blend_row(app: "PhotoEditApp", v: ui.View) -> None:
    layer = app.current_layer()
    blend = v.dropdown("", list(LAYER_BLENDS),
                       layer.blend if layer.blend in LAYER_BLENDS
                       else "Normal", key="ps-blend")
    blend.cls("ps-blend-select")
    blend.on_change(app.set_layer_blend)
    v.container(classes="ps-row-spacer", key="ps-bo-spacer")
    v.container(classes="ps-amt-label", key="ps-op-label",
                build=lambda h: h.html("Opacity:"))
    v.input("", f"{round(layer.opacity)}", type="number",
            key="ps-op-amt").cls("ps-amt-field").on_change(
        app.set_layer_opacity)


def _layer_lock_row(app: "PhotoEditApp", v: ui.View) -> None:
    layer = app.current_layer()
    v.container(classes="ps-amt-label", key="ps-lock-label",
                build=lambda h: h.html("Lock:"))
    # Only "all" is real layer state (web parity); the other three are
    # pressed-state visuals.
    for flag, icon, title in (("transparency", "grid", "Lock transparency"),
                              ("image", "brush", "Lock image"),
                              ("position", "move", "Lock position")):
        _icon_button(v, f"ps-lock-{flag}", _di(icon), title,
                     on_click=lambda flag=flag: app.toggle_lock_flag(flag),
                     pressed=app.lock_flags[flag])
    _icon_button(v, "ps-lock-all", _di("lock"), "Lock all",
                 on_click=app.toggle_layer_lock, pressed=layer.locked)
    v.container(classes="ps-row-spacer", key="ps-lock-spacer")
    v.container(classes="ps-amt-label", key="ps-fill-label",
                build=lambda h: h.html("Fill:"))
    v.input("", f"{round(layer.fill)}", type="number",
            key="ps-fill-amt").cls("ps-amt-field").on_change(
        app.set_layer_fill)


def _layer_rows(app: "PhotoEditApp", v: ui.View) -> None:
    for layer in reversed(app.doc.layers()):
        _layer_row(app, v, layer)


def _layer_row(app: "PhotoEditApp", v: ui.View,
               layer: ui.PhotoLayerSnapshot) -> None:
    is_active = layer.id == app.doc.active_layer_id()

    def row(r: ui.View) -> None:
        eye = r.container(
            classes="ps-layer-eye" + ("" if layer.visible else " is-off"),
            key=f"eye-{layer.id}",
            build=lambda h: h.html(
                _di("eye") if layer.visible else _di("eye-off")))
        eye.attr("role", "button").attr("title", "Toggle visibility")
        eye.on_click(lambda: app.toggle_layer_visible(layer.id))

        def thumb(t: ui.View) -> None:
            if layer.kind == "text":
                t.html('<span class="ps-tcap" style="position:absolute;'
                       'inset:0;display:flex;align-items:center;'
                       'justify-content:center;color:#333">T</span>',
                       key=f"thumb-t-{layer.id}")
            else:
                t.container(classes="ps-layer-thumb-fill",
                            key=f"thumb-fill-{layer.id}").attr(
                    "style", layer.style)

        r.container(classes="ps-layer-thumb", key=f"thumb-{layer.id}",
                    build=thumb)

        if app.renaming_layer_id == layer.id:
            rename = r.input("", layer.name, key=f"rename-{layer.id}")
            rename.cls("ps-layer-rename")
            rename.on_change(lambda text: app.commit_rename(layer.id, text))
        else:
            r.container(classes="ps-layer-name", key=f"name-{layer.id}",
                        build=lambda h: h.html(escape(layer.name)))

        if layer.locked:
            r.container(classes="ps-layer-lock", key=f"lock-{layer.id}",
                        build=lambda h: h.html(_di("lock")))

        if is_active and not layer.locked:
            # Click-level affordances standing in for drag-reorder and
            # double-click rename (no DnD / dblclick in the binding yet).
            def actions(a: ui.View) -> None:
                _icon_button(a, f"up-{layer.id}", _di("chevron-up"),
                             "Bring forward",
                             on_click=lambda: app.move_layer(1))
                _icon_button(a, f"down-{layer.id}", _di("chevron-down"),
                             "Send backward",
                             on_click=lambda: app.move_layer(-1))
                _icon_button(a, f"ren-{layer.id}", _di("edit"), "Rename",
                             on_click=lambda: app.begin_rename(layer.id))

            r.container(classes="ps-layer-actions",
                        key=f"actions-{layer.id}", build=actions)

    ref = v.container(classes="ps-layer" + (" is-active" if is_active else ""),
                      key=f"layer-{layer.id}", build=row)
    ref.attr("role", "button")
    ref.attr("aria-selected", "true" if is_active else "false")
    ref.on_click(lambda: app.set_layer(layer.id))


def _layer_footer(app: "PhotoEditApp", v: ui.View) -> None:
    _icon_button(v, "ps-l-link", _di("link"), "Link layers",
                 on_click=lambda: app.toast("Link layers"))
    _icon_button(v, "ps-l-fx", '<span class="ps-fx">fx</span>', "Layer style",
                 on_click=lambda: app.toast("Layer style"))
    _icon_button(v, "ps-l-mask", _di("layer-mask"), "Add layer mask",
                 on_click=lambda: app.toast("Add layer mask"))
    _icon_button(v, "ps-l-adjust", _di("color-grade"),
                 "New adjustment layer",
                 on_click=lambda: app.adjustment_action("hsl"))
    _icon_button(v, "ps-l-group", _di("folder"), "New group",
                 on_click=lambda: app.toast("New group"))
    v.container(classes="ps-row-spacer", key="ps-lf-footer-spacer")
    _icon_button(v, "ps-l-new", _di("plus"), "New layer",
                 on_click=app.add_layer)
    _icon_button(v, "ps-l-dup", _di("duplicate"), "Duplicate layer",
                 on_click=app.duplicate_layer)
    _icon_button(v, "ps-l-del", _di("trash"), "Delete layer",
                 on_click=app.delete_layer)


def _channels_tab(app: "PhotoEditApp", v: ui.View) -> None:
    for label, shortcut, selected in CHANNELS:
        v.container(
            classes="ps-list-row" + (" is-active" if selected else ""),
            key=f"channel-{label.lower()}",
            build=lambda h, label=label, shortcut=shortcut: h.html(
                f"{_di('eq')}<span>{escape(label)}</span>"
                f'<span class="ps-row-meta">{escape(shortcut)}</span>'))


def _paths_tab(app: "PhotoEditApp", v: ui.View) -> None:
    v.paragraph("No paths. Use the Pen tool to create a work path.",
                classes="ps-panel-note", key="ps-paths-note")


def _comps_tab(app: "PhotoEditApp", v: ui.View) -> None:
    for icon, label, meta, active in COMPS:
        meta_html = (f'<span class="ps-row-meta">{escape(meta)}</span>'
                     if meta else "")
        v.container(
            classes="ps-list-row" + (" is-active" if active else ""),
            key=f"comp-{label.lower().replace(' ', '-')}",
            build=lambda h, icon=icon, label=label, meta_html=meta_html:
            h.html(f"{_di(icon)}<span>{escape(label)}</span>{meta_html}"))

    def footer(f: ui.View) -> None:
        _icon_button(f, "ps-comp-apply", _di("check"), "Apply comp",
                     on_click=lambda: app.toast("Apply comp"))
        _icon_button(f, "ps-comp-update", _di("redo"), "Update comp",
                     on_click=lambda: app.toast("Update comp"))
        f.container(classes="ps-row-spacer", key="ps-comp-spacer")
        _icon_button(f, "ps-comp-new", _di("plus"), "New comp",
                     on_click=lambda: app.toast("New comp"))
        _icon_button(f, "ps-comp-del", _di("trash"), "Delete comp",
                     on_click=lambda: app.toast("Delete comp"))

    v.container(classes="ps-layer-footer", key="ps-comps-footer",
                build=footer)


# ── Adjustments ──────────────────────────────────────────────────────────────

def build_adjustments(app: "PhotoEditApp", v: ui.View) -> None:
    floating_panel(v, "panel-adjust", "right:324px;top:12px;width:236px",
                   title_header("Adjustments", "color-grade", "panel-adjust"),
                   lambda b: _adjust_grid(app, b))


def _adjust_grid(app: "PhotoEditApp", v: ui.View) -> None:
    def grid(g: ui.View) -> None:
        for icon, label, action in ADJUSTMENTS:
            b = g.container(classes="ps-adjust", key=f"adj-{action}",
                            build=lambda h, icon=icon: h.html(_di(icon)))
            b.attr("role", "button").attr("title", label)
            b.on_click(lambda action=action: app.adjustment_action(action))

    v.container(classes="ps-adjust-grid", key="ps-adjust-grid", build=grid)


# ── History ──────────────────────────────────────────────────────────────────

def build_history(app: "PhotoEditApp", v: ui.View) -> None:
    floating_panel(v, "panel-history",
                   "right:324px;top:152px;bottom:12px;width:236px",
                   title_header("History", "history-brush", "panel-history"),
                   lambda b: _history_list(app, b))


def _history_list(app: "PhotoEditApp", v: ui.View) -> None:
    current = app.doc.history_index()
    for index, (label, icon) in enumerate(app.doc.history_entries()):
        classes = "ps-history-item"
        if index == current:
            classes += " is-current"
        elif index > current:
            classes += " is-future"
        item = v.container(classes=classes, key=f"history-{index}",
                           build=lambda h, label=label, icon=icon: h.html(
                               f"{_di(icon)}<span>{escape(label)}</span>"))
        item.attr("role", "button")
        item.on_click(lambda index=index: app.jump_history(index))


# ── Quick floatbar ───────────────────────────────────────────────────────────

def build_floatbar(app: "PhotoEditApp", v: ui.View) -> None:
    def bar(b: ui.View) -> None:
        _icon_button(b, "float-undo", _di("undo"), "Undo",
                     on_click=app.undo)
        _icon_button(b, "float-redo", _di("redo"), "Redo",
                     on_click=app.redo)
        b.container(classes="dcs-divider dcs-divider--v", key="float-sep")
        _icon_button(b, "float-zoom-out", _di("zoom-out"), "Zoom out",
                     on_click=lambda: app.zoom_step(1 / 1.4))
        _icon_button(b, "float-fit", _di("fit"), "Fit on screen",
                     on_click=app.fit_to_screen)
        _icon_button(b, "float-zoom-in", _di("zoom-in"), "Zoom in",
                     on_click=lambda: app.zoom_step(1.4))

    v.container(classes="dcs-toolbar dcs-toolbar--floating ps-floatbar",
                key="ps-floatbar", build=bar)
