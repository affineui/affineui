"""Floating surfaces: tool strip, Navigator, Color/Swatches,
Layers/Channels/Paths/Comps, Adjustments, History, and the quick floatbar.

Positions and sizes mirror the web reference (§1 "Floating surfaces").
Channels/Paths/Comps are static content, matching the web's own stubs.
"""

from __future__ import annotations

from html import escape
from typing import TYPE_CHECKING, Callable

import affineui as ui

from . import colors
from .specs import (ADJUSTMENTS, CHANNELS, COMPS, LAYER_BLENDS, SWATCHES,
                    TOOLS, tool_icon_html)

if TYPE_CHECKING:
    import photo_core

    from .app import PhotoEditApp

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


# ── Declared floating dock panels ────────────────────────────────────────────
# Each palette is a DECLARED dockpanel seeded floating (web reference
# positions, anchored to the workarea's top-right corner) — not raw
# .dcs-panel--floating chrome. Declared panels are what the docking system
# can replay: drag one somewhere (dock it beside the stage, tear a tab off)
# and the arrangement survives every rebuild. Tab groups (Color/Swatches,
# Layers/Channels/Paths/Comps) are tab().in_(primary) co-panels; the core
# interaction layer owns tab switching and remembers the active tab.

def declare_float_panels(app: "PhotoEditApp", dv: ui.View) -> None:
    Loc, Corner = ui.DockLocation, ui.DockCorner
    if app.panels["navigator"]:
        dv.dockpanel("Navigator",
                     Loc.floating(Corner.TopRight, (12, 12), (214, 196)),
                     lambda p: _navigator_body(app, p),
                     icon="globe", key="navigator")
    if app.panels["color"]:
        color = dv.dockpanel(
            "Color", Loc.floating(Corner.TopRight, (12, 226), (268, 252)),
            lambda p: _color_tab(app, p), icon="palette", key="color")
        dv.dockpanel("Swatches", Loc.tab().in_(color),
                     lambda p: _swatches_tab(app, p), icon="grid",
                     key="swatches")
    if app.panels["layers"]:
        layers = dv.dockpanel(
            "Layers", Loc.floating(Corner.TopRight, (12, 486), (300, 230)),
            lambda p: _layers_tab(app, p), icon="layers", key="layers")
        dv.dockpanel("Channels", Loc.tab().in_(layers),
                     lambda p: _channels_tab(app, p), icon="eq",
                     key="channels")
        dv.dockpanel("Paths", Loc.tab().in_(layers),
                     lambda p: _paths_tab(app, p), icon="spline", key="paths")
        dv.dockpanel("Comps", Loc.tab().in_(layers),
                     lambda p: _comps_tab(app, p), icon="filmstrip",
                     key="comps")
    if app.panels["adjust"]:
        dv.dockpanel("Adjustments",
                     Loc.floating(Corner.TopRight, (324, 12), (236, 128)),
                     lambda p: _adjust_grid(app, p), icon="color-grade",
                     key="adjust")
    if app.panels["history"]:
        dv.dockpanel("History",
                     Loc.floating(Corner.TopRight, (324, 152), (236, 330)),
                     lambda p: _history_list(app, p), icon="history-brush",
                     key="history")


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
    bg.attr("id", "ps-chip-bg")
    bg.attr("style", f"background:{app.bg}")
    bg.attr("role", "button").attr("title", f"Background color {app.bg}")
    bg.on_click(app.open_background_picker)
    fg = v.container(classes="ps-colorchip ps-colorchip--fg", key="ps-chip-fg")
    fg.attr("id", "ps-chip-fg")
    fg.attr("style", f"background:{app.fg}")
    fg.attr("role", "button").attr("title", f"Foreground color {app.fg}")
    fg.on_click(app.open_foreground_picker)


# ── Navigator ────────────────────────────────────────────────────────────────

def _navigator_body(app: "PhotoEditApp", v: ui.View) -> None:
    def body(p: ui.View) -> None:
        p.container(classes="ps-nav-thumb", key="ps-nav-thumb",
                    build=lambda n: _nav_thumb(app, n))

        # Web-parity zoom row: a dcs-row (align-items:center) with a muted
        # zoom-out icon, a growing dcs-slider, a zoom-in icon, and a
        # fixed-width percentage readout. The icons are clickable steppers
        # (an enhancement over the web's static icons).
        def zoom_row(row: ui.View) -> None:
            out = row.container(classes="ps-nav-zbtn", key="ps-nav-zoom-out",
                                build=lambda h: h.html(_di("zoom-out")))
            out.attr("role", "button").attr("title", "Zoom out")
            out.on_click(lambda: app.zoom_step(1 / 1.4))
            row.slider("", app.zoom * 100, 5, 800,
                       key="ps-nav-zoom").cls("dcs-slider ps-grow").on_change(
                lambda text: app.set_zoom_percent_text(text))
            zin = row.container(classes="ps-nav-zbtn", key="ps-nav-zoom-in",
                                build=lambda h: h.html(_di("zoom-in")))
            zin.attr("role", "button").attr("title", "Zoom in")
            zin.on_click(lambda: app.zoom_step(1.4))
            pct = row.container(classes="ps-nav-pct ps-prop-val",
                                key="ps-nav-pct",
                                build=lambda h: h.html(
                                    f"{round(app.zoom * 100)}%"))
            pct.attr("id", "ps-nav-pct").attr("style", "width:42px")

        p.container(classes="ps-nav-zoomrow dcs-row", key="ps-nav-zoomrow",
                    build=zoom_row)

    v.container(classes="ps-nav-body", key="ps-nav-panel", build=body)


def _nav_thumb(app: "PhotoEditApp", v: ui.View) -> None:
    # The raster core paints the live composite (and the viewport
    # rectangle) into this canvas each frame. Click/drag-to-pan routes
    # through App.on_event (web panels.js Navigator behavior).
    v.canvas("ps-nav", classes="ps-nav-canvas", key="ps-nav-canvas")


# ── Color / Swatches ─────────────────────────────────────────────────────────

def _color_tab(app: "PhotoEditApp", v: ui.View) -> None:
    def body(p: ui.View) -> None:
        h, s, val = app.hsv
        hue_hex = colors.hsv_to_hex(h, 1.0, 1.0)
        # SV square + hue bar use the framework's own dcs-color-square /
        # dcs-hue-bar (which carry the correct gradients — the square's
        # white→hue ramp reads --hue, the bar is the rainbow); we only add
        # ps-* hooks so App.on_event can hit-test them. Pointer-precise
        # picking routes through App.on_event (web initPicker drag), which
        # updates the dots/chips live via DOM style mutations.
        def sv_body(box: ui.View) -> None:
            dot = box.container(classes="ps-sv-dot", key="ps-sv-dot")
            dot.attr("id", "ps-sv-dot")
            dot.attr("style", f"left:{s * 100:.1f}%;"
                              f"top:{(1 - val) * 100:.1f}%")

        sv = p.container(classes="dcs-color-square ps-sv", key="ps-sv",
                         build=sv_body)
        sv.attr("id", "ps-sv")
        sv.attr("style", f"--hue:{hue_hex}")

        def hue_body(bar: ui.View) -> None:
            dot = bar.container(classes="ps-hue-dot", key="ps-hue-dot")
            dot.attr("id", "ps-hue-dot")
            dot.attr("style", f"left:{h / 360 * 100:.1f}%")

        hue = p.container(classes="dcs-hue-bar ps-hue", key="ps-hue",
                          build=hue_body)
        hue.attr("id", "ps-hue")
        hexfield = p.input("Hex", app.fg, key="ps-hex")
        hexfield.cls("ps-hex-field")
        hexfield.on_change(app.set_foreground_hex)

        # R/G/B use the framework's own labeled text field (v.input →
        # dcs-field > dcs-field__label + dcs-input, the web's structure),
        # styled as a row with tabular-num digits. A number input would
        # instead build the heavy dcs-combo spinbutton, which is the wrong
        # control here.
        def rgb_row(row: ui.View) -> None:
            r, g, b = colors.hex_to_rgb(app.fg)
            for channel, value in (("r", r), ("g", g), ("b", b)):
                field = row.input(channel.upper(), str(value),
                                  key=f"ps-{channel}")
                field.cls("dcs-field dcs-field--row ps-rgb-field")
                field.on_change(
                    lambda text, channel=channel:
                    app.set_foreground_channel(channel, text))

        p.container(classes="ps-rgb-row dcs-row", key="ps-rgb-row",
                    build=rgb_row)

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
    blend = v.dropdown("", list(LAYER_BLENDS), app.current_blend_name(),
                       key="ps-blend")
    blend.cls("ps-blend-select")
    blend.on_change(app.set_layer_blend)
    v.container(classes="ps-row-spacer", key="ps-bo-spacer")
    v.container(classes="ps-amt-label", key="ps-op-label",
                build=lambda h: h.html("Opacity:"))
    v.input("", f"{round(layer.opacity * 100)}", type="number",
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
    v.input("", f"{round(layer.fill * 100)}", type="number",
            key="ps-fill-amt").cls("ps-amt-field").on_change(
        app.set_layer_fill)


def _layer_rows(app: "PhotoEditApp", v: ui.View) -> None:
    for layer in reversed(app.doc.layers()):
        _layer_row(app, v, layer)


def _layer_row(app: "PhotoEditApp", v: ui.View,
               layer: "photo_core.PhotoLayer") -> None:
    is_active = layer.id == app.doc.active_id()

    def row(r: ui.View) -> None:
        eye = r.container(
            classes="ps-layer-eye" + ("" if layer.visible else " is-off"),
            key=f"eye-{layer.id}",
            build=lambda h: h.html(
                _di("eye") if layer.visible else _di("eye-off")))
        eye.attr("role", "button").attr("title", "Toggle visibility")
        eye.on_click(lambda: app.toggle_layer_visible(layer.id))

        # Live pixel thumbnail, painted by the raster core.
        r.container(classes="ps-layer-thumb", key=f"thumb-{layer.id}",
                    build=lambda t: t.canvas(
                        app.doc.thumb_paint_name(layer.id),
                        classes="ps-layer-thumb-fill",
                        key=f"thumb-canvas-{layer.id}"))

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
    # Selection fires on mousedown via App.on_event (see app._mouse_down);
    # the id rides in a data attribute the router reads. No on_click — that
    # would be a redundant mouseup activation.
    ref.attr("data-layer-id", str(layer.id))


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

def _adjust_grid(app: "PhotoEditApp", v: ui.View) -> None:
    def grid(g: ui.View) -> None:
        for icon, label, action in ADJUSTMENTS:
            b = g.container(classes="ps-adjust", key=f"adj-{action}",
                            build=lambda h, icon=icon: h.html(_di(icon)))
            b.attr("role", "button").attr("title", label)
            b.on_click(lambda action=action: app.adjustment_action(action))

    v.container(classes="ps-adjust-grid", key="ps-adjust-grid", build=grid)


# ── History ──────────────────────────────────────────────────────────────────

def _history_list(app: "PhotoEditApp", v: ui.View) -> None:
    current = app.doc.history_index()
    for index, entry in enumerate(app.doc.history_entries()):
        classes = "ps-history-item"
        if index == current:
            classes += " is-current"
        elif index > current:
            classes += " is-future"
        item = v.container(
            classes=classes, key=f"history-{index}",
            build=lambda h, label=entry.name, icon=entry.icon: h.html(
                f"{_di(icon)}<span>{escape(label)}</span>"))
        item.attr("role", "button")
        # Jump on mousedown via App.on_event (see app._mouse_down) for snap.
        item.attr("data-history-index", str(index))


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
