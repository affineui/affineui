"""PhotoEditApp: state + wiring for the Python Decius Photo Edit sample.

Mirrors the web reference app (decius-css/samples/decius-photo) at the
click-interaction level. The document/layer/history model lives in the C++
photo core (affineui.PhotoDocument); undo/redo/history-jump genuinely restore
document state. Pointer-drag behaviors (freehand painting, marquee drag,
panel dragging) are Phase-B work and are marked where approximated.
"""

from __future__ import annotations

import re
from html import escape

import affineui as ui

from . import colors, dialogs, options, panels, stage
from .specs import (LAYER_BLENDS, MENUS, TOOL_BY_ID, TOOL_DEFAULTS,
                    tool_icon_html)
from .styles import PHOTO_CSS

_HEX_ONLY_RE = re.compile(r"#[0-9a-fA-F]{6}|#[0-9a-fA-F]{3}")

# Pseudo-random stamp anchors for click-to-paint (Phase B replaces these with
# real pointer coordinates).
_STAMP_POINTS: tuple[tuple[int, int], ...] = (
    (32, 38), (58, 30), (46, 58), (66, 52), (38, 50), (52, 42), (28, 62),
    (62, 66), (44, 34), (70, 44),
)

_ACCENTS = ("blue", "orange", "green", "purple", "teal")
_ACCENT_DOTS = {
    "blue": "#4d9fff", "orange": "#ff9f4d", "green": "#4dd97b",
    "purple": "#b48cff", "teal": "#2dd4bf",
}


def _safe_num(value: str, fallback: float) -> float:
    try:
        return float(str(value).strip().rstrip("%"))
    except (TypeError, ValueError):
        return fallback


class PhotoEditApp:
    def __init__(self) -> None:
        self.app: ui.App | None = None
        self.doc = ui.PhotoDocument(1280, 800)
        self.doc.load_sample_scene()  # web-parity boot scene

        self.doc_name = "Untitled-1"
        self.color_mode = "RGB/8"
        self.resolution = 72

        self.tool = "brush"  # web boots with the brush tool
        self.tool_options: dict[str, dict[str, object]] = {
            tool_id: dict(defaults)
            for tool_id, defaults in TOOL_DEFAULTS.items()
        }
        self.clone_source_set = False

        self.fg = "#1f6feb"  # web boot colors
        self.bg = "#ffffff"

        self.zoom = 0.67
        self.pan_x = 0
        self.pan_y = 0
        self.selection: tuple[int, int, int, int] | None = None
        self.sel_feather = 0.0

        self.quickmask = False
        self.show_grid = False
        self.show_rulers = True
        self.snap = True
        self.density = "comfortable"
        self.visual_style = "flat"
        self.accent = "blue"
        self.tweaks_open = False

        self.status = TOOL_BY_ID[self.tool].tip
        self.panels = {"navigator": True, "color": True, "layers": True,
                       "adjust": True, "history": True}
        self.color_tab = "color"
        self.layers_tab = "layers"
        self.lock_flags = {"transparency": False, "image": False,
                           "position": False}
        self.layer_filtering = False
        self.renaming_layer_id: str | None = None

        self.dialog: dialogs.DialogSpec | None = None
        self.dialog_values: dict[str, object] = {}

        self._stamp_index = 0

    # ── View assembly ───────────────────────────────────────────────────────

    @property
    def tool_spec(self):
        return TOOL_BY_ID[self.tool]

    @property
    def hsv(self) -> tuple[float, float, float]:
        return colors.hex_to_hsv(self.fg)

    def build_view(self) -> ui.View:
        view = ui.View(ui.ViewTheme.Decius)
        view.selector(ui.decius.selector.style, self.visual_style)
        view.selector(ui.decius.selector.density, self.density)
        view.selector(ui.decius.selector.accent, self.accent)
        view.begin()
        view.container(classes="ps-app", key="ps-app", build=self._build_app)
        if self.dialog is not None:
            view.container(classes="ps-dialog-backdrop",
                           key="ps-dialog-backdrop",
                           build=lambda v: dialogs.build_dialog(self, v))
        view.end()
        return view

    def reload(self) -> None:
        if self.app is None:
            return
        self.app.load_view(self.build_view())
        self.app.set_stylesheet(PHOTO_CSS)

    def launch(self, high_dpi: bool, perf: bool, headless: bool) -> None:
        self.app = ui.App(
            title="Decius Photo Edit (Python)",
            width=1280,
            height=820,
            asset_folders=["examples"],
            high_dpi=high_dpi,
            perf_overlay=perf,
        )
        self.zoom = self.fit_zoom()
        self.reload()
        if headless:
            self.app.document().layout(1280, 820)
            size = self.app.document().content_size()
            print(f"laid out {size.width}x{size.height}")
            return
        self.app.launch(native=True)

    def _build_app(self, v: ui.View) -> None:
        v.container(classes="dcs-menubar ps-menubar", key="ps-menubar",
                    build=self._build_menubar)
        v.container(classes="ps-options", key="ps-options",
                    build=self._build_options_bar)
        v.container(classes="ps-body", key="ps-body", build=self._build_body)
        v.container(classes="dcs-statusbar ps-statusbar", key="ps-statusbar",
                    build=self._build_statusbar)
        v.container(classes="ps-menu-host", key="ps-menus",
                    build=self._build_menus)
        if self.tweaks_open:
            v.container(classes="dcs-panel dcs-panel--floating ps-tweaks",
                        key="ps-tweaks", build=self._build_tweaks)

    # ── Menubar / menus ─────────────────────────────────────────────────────

    def _build_menubar(self, v: ui.View) -> None:
        v.container(classes="ps-brand", key="ps-brand",
                    build=lambda b: b.html(
                        '<i class="di di-decius ps-brand__mark"></i>'
                        '<span class="ps-brand__name">Decius&nbsp;Photo'
                        "</span>"))
        for menu in MENUS:
            trigger = v.button(menu.label, key=f"trigger-{menu.id}")
            trigger.cls("dcs-menubar__item")
            trigger.attr("data-dcs-toggle", "menu")
            trigger.attr("data-dcs-target", f"#{menu.id}")
            trigger.attr("aria-expanded", "false")
        v.container(classes="dcs-menubar__spacer", key="ps-menu-spacer")
        v.container(classes="dcs-menubar__meta ps-doc-name",
                    key="ps-doc-name",
                    build=lambda m: m.html(
                        f"<span>{escape(self.title_text())}</span>"))
        v.container(classes="dcs-divider dcs-divider--v",
                    key="ps-menubar-divider")
        cog = v.container(
            classes="dcs-btn dcs-btn--icon dcs-btn--ghost ps-settings",
            key="ps-settings", build=lambda h: h.html(
                '<i class="di di-cog"></i>'))
        cog.attr("role", "button").attr("title", "Theme tweaks")
        cog.on_click(self.toggle_tweaks)

    def _menu_checked(self, state_key: str) -> bool:
        if state_key == "rulers":
            return self.show_rulers
        if state_key == "grid":
            return self.show_grid
        if state_key == "snap":
            return self.snap
        if state_key.startswith("panel:"):
            return self.panels.get(state_key.split(":", 1)[1], False)
        return False

    def _build_menus(self, v: ui.View) -> None:
        for menu in MENUS:
            def build_menu(menu_view: ui.View, menu=menu) -> None:
                for index, item in enumerate(menu.items):
                    if item is None:
                        menu_view.container(classes="dcs-menu__sep",
                                            key=f"{menu.id}-sep-{index}")
                        continue
                    checked = bool(item.checked
                                   and self._menu_checked(item.checked))
                    html = (
                        '<span class="dcs-menu__icon">'
                        f'<i class="di di-{escape(item.icon)}"></i></span>'
                        f'<span class="dcs-menu__label">{escape(item.label)}'
                        "</span>"
                        '<span class="dcs-menu__shortcut">'
                        f"{escape(item.shortcut)}</span>"
                    )
                    row = menu_view.container(
                        classes="dcs-menu__item"
                        + (" dcs-menu__item--checked" if checked else ""),
                        key=f"{menu.id}-{item.action}",
                        build=lambda h, html=html: h.html(html))
                    row.attr("role", "menuitem")
                    row.attr("data-dcs-value", item.action)

            ref = v.container(classes="dcs-menu", key=menu.id,
                              build=build_menu)
            ref.attr("id", menu.id)
            ref.attr("hidden", "")
            ref.on_change(self.menu_action)

    def _build_tweaks(self, v: ui.View) -> None:
        def body(p: ui.View) -> None:
            p.button_group("Density", ["Compact", "Comfortable", "Spacious"],
                           self.density.title(),
                           key="tweak-density").on_change(self.set_density)
            p.button_group("Style", ["Flat", "3D"],
                           "3D" if self.visual_style == "3d" else "Flat",
                           key="tweak-style").on_change(self.set_style)

            def dots(row: ui.View) -> None:
                for accent in _ACCENTS:
                    dot = row.container(
                        classes="ps-accent-dot"
                        + (" is-active" if accent == self.accent else ""),
                        key=f"tweak-accent-{accent}")
                    dot.attr("style", f"background:{_ACCENT_DOTS[accent]}")
                    dot.attr("role", "button").attr("title",
                                                    accent.title())
                    dot.on_click(lambda accent=accent:
                                 self.set_accent(accent))

            p.paragraph("Accent", classes="ps-amt-label", key="tweak-accent-l")
            p.container(classes="ps-accent-dots", key="tweak-accents",
                        build=dots)

        v.container(classes="dcs-panel__header", key="ps-tweaks-header",
                    build=lambda h: h.html(
                        '<span class="dcs-panel__title">Theme tweaks'
                        "</span>"))
        v.container(classes="dcs-panel__body", key="ps-tweaks-body",
                    build=body)

    # ── Options bar / statusbar ─────────────────────────────────────────────

    def _build_options_bar(self, v: ui.View) -> None:
        tool = self.tool_spec
        v.container(classes="ps-tool-glyph", key="ps-opt-glyph",
                    build=lambda h: h.html(tool_icon_html(tool)))
        v.container(classes="ps-tool-name", key="ps-opt-name",
                    build=lambda h: h.html(escape(tool.name)))
        v.container(classes="dcs-divider dcs-divider--v",
                    key="ps-opt-divider-a")
        v.container(classes="ps-opt-slot", key="ps-opt-slot",
                    build=lambda slot: options.build(self, slot))
        v.container(classes="dcs-divider dcs-divider--v",
                    key="ps-opt-divider-b")
        fit = v.button("Fit", key="ps-btn-reset-view")
        fit.cls("dcs-btn dcs-btn--ghost dcs-btn--sm")
        fit.on_click(self.fit_to_screen)

    def _build_statusbar(self, v: ui.View) -> None:
        zoom_field = v.input("", f"{round(self.zoom * 100)}%",
                             key="ps-zoom-field")
        zoom_field.cls("ps-zoom-wrap")
        zoom_field.on_change(self.set_zoom_percent_text)
        v.container(classes="dcs-statusbar__sep", key="ps-status-sep-a")
        v.container(classes="dcs-statusbar__item", key="ps-st-doc",
                    build=lambda h: h.html(f"Doc: {self.doc_size_text()}"))
        v.container(classes="dcs-statusbar__sep", key="ps-status-sep-b")
        v.container(classes="dcs-statusbar__item", key="ps-st-tip",
                    build=lambda h: h.html(escape(self.status)))
        v.container(classes="ps-status-spacer", key="ps-status-spacer")
        v.container(classes="dcs-statusbar__item dcs-statusbar__item--ok",
                    key="ps-status-ready",
                    build=lambda h: h.html(
                        '<i class="di di-check-circle"></i> Ready'))
        v.container(classes="dcs-statusbar__sep", key="ps-status-sep-c")
        v.container(classes="dcs-statusbar__item", key="ps-status-mode",
                    build=lambda h: h.html(
                        f"{escape(self.color_mode)} · "
                        f"{self.resolution} ppi"))

    # ── Body ────────────────────────────────────────────────────────────────

    def _build_body(self, v: ui.View) -> None:
        v.container(classes="dcs-dock ps-doc-dock", key="ps-doc-dock",
                    build=lambda d: stage.build_document_dock(self, d))
        v.container(
            classes="dcs-toolbar dcs-toolbar--v dcs-toolbar--floating "
                    "ps-toolstrip",
            key="ps-toolstrip",
            build=lambda t: panels.build_toolstrip(self, t))
        if self.panels["navigator"]:
            panels.build_navigator(self, v)
        if self.panels["color"]:
            panels.build_color_panel(self, v)
        if self.panels["layers"]:
            panels.build_layers_panel(self, v)
        if self.panels["adjust"]:
            panels.build_adjustments(self, v)
        if self.panels["history"]:
            panels.build_history(self, v)
        panels.build_floatbar(self, v)

    # ── Derived text ────────────────────────────────────────────────────────

    def title_text(self) -> str:
        return (f"{self.doc_name} @ {round(self.zoom * 100)}% "
                f"({self.color_mode})")

    def doc_size_text(self) -> str:
        mb = self.doc.width() * self.doc.height() * 4 / 1048576
        return f"{mb:.2f}M / {mb * max(1, len(self.doc.layers())):.1f}M"

    def stage_size(self) -> tuple[float, float]:
        width, height = 1280.0, 820.0
        if self.app is not None:
            try:
                size = self.app.window_size()
                if size.width > 0 and size.height > 0:
                    width, height = float(size.width), float(size.height)
            except Exception:
                pass
        # menubar + options bar + statusbar + ruler gutters.
        return (max(200.0, width - 18), max(200.0, height - 32 - 38 - 28 - 18))

    def fit_zoom(self) -> float:
        stage_w, stage_h = self.stage_size()
        z = min((stage_w - 96) / max(1, self.doc.width()),
                (stage_h - 96) / max(1, self.doc.height()))
        return max(0.05, min(16.0, z))

    # ── Status / toast ──────────────────────────────────────────────────────

    def toast(self, message: str) -> None:
        self.status = message
        self.reload()

    # ── Tools ───────────────────────────────────────────────────────────────

    def set_tool(self, tool_id: str) -> None:
        if tool_id not in TOOL_BY_ID:
            return
        self.tool = tool_id
        self.status = TOOL_BY_ID[tool_id].tip
        self.reload()

    def tool_option(self, key: str, fallback: object = None) -> object:
        return self.tool_options.get(self.tool, {}).get(key, fallback)

    def set_tool_option(self, key: str, value: object,
                        numeric: bool = False) -> None:
        opts = self.tool_options.setdefault(self.tool, {})
        if numeric:
            number = _safe_num(str(value), float(opts.get(key, 0) or 0))
            value = int(number) if number == int(number) else number
        opts[key] = value
        self.reload()

    def toggle_quickmask(self) -> None:
        self.quickmask = not self.quickmask
        self.status = ("Quick Mask on" if self.quickmask
                       else "Quick Mask off")
        self.reload()

    # ── Stage interaction (click-level; pointer drags are Phase B) ─────────

    def stage_click(self) -> None:
        tool = self.tool
        name = self.tool_spec.name
        if tool in ("brush", "pencil", "dodge", "burn"):
            self._paint_stamp(tool)
        elif tool == "fill":
            opacity = _safe_num(str(self.tool_option("opacity", 100)), 100)
            if self.doc.fill_active(self.fg, opacity, "Paint Bucket"):
                self.status = "Paint Bucket"
            else:
                self.status = "Layer is locked"
            self.reload()
        elif tool == "gradient":
            opacity = _safe_num(str(self.tool_option("opacity", 100)), 100)
            top = colors.hex_to_rgba(self.fg, opacity / 100.0)
            bottom = colors.hex_to_rgba(self.fg, 0.0)
            if self.doc.set_active_style(
                    f"background:linear-gradient(180deg,{top},{bottom})",
                    "Gradient", "fill"):
                self.status = "Gradient"
            else:
                self.status = "Layer is locked"
            self.reload()
        elif tool == "eyedropper":
            self._sample_foreground()
        elif tool in ("marquee", "lasso", "wand"):
            self._click_selection(tool)
        elif tool == "type":
            self._place_type_layer()
        elif tool == "shape":
            self._place_shape_layer()
        elif tool == "zoom":
            self.zoom_step(1.4)
        elif tool == "crop":
            self.toast("Crop: choose a ratio, then press Apply "
                       "in the options bar")
        elif tool == "clone":
            self.clone_source_set = True
            self.toast("Clone source set ✓ (drag painting is Phase B)")
        elif tool == "pen":
            self.toast("Pen: anchor added — path preview is Phase B")
        elif tool in ("eraser", "history", "smudge", "blur"):
            self.toast(f"{name}: pointer-drag painting arrives in Phase B")
        else:  # move / hand
            self.status = self.tool_spec.tip
            self.reload()

    def _paint_stamp(self, tool: str) -> None:
        # Phase B: real pointer coordinates + drag strokes. Until then a
        # click stamps one dab at a rotating canvas anchor.
        x, y = _STAMP_POINTS[self._stamp_index % len(_STAMP_POINTS)]
        self._stamp_index += 1
        size = max(24.0, _safe_num(str(self.tool_option("size", 24)), 24) * 2)
        if tool == "dodge":
            color, alpha = "#ffffff", 0.35
        elif tool == "burn":
            color, alpha = "#000000", 0.35
        else:
            opacity = _safe_num(str(self.tool_option("opacity", 100)), 100)
            color, alpha = self.fg, opacity / 100.0
        hardness = (100.0 if tool == "pencil" else
                    _safe_num(str(self.tool_option("hardness", 70)), 70))
        inner = colors.hex_to_rgba(color, alpha)
        edge = colors.hex_to_rgba(color, 0.0)
        stamp = (f"radial-gradient(circle {size:.0f}px at {x}% {y}%,"
                 f"{inner} 0%,{inner} {hardness:.0f}%,{edge} 100%)")
        if self.doc.paint_active(stamp, self.tool_spec.name,
                                 self.tool_spec.icon or "brush"):
            self.status = self.tool_spec.name
        else:
            self.status = "Layer is locked"
        self.reload()

    def _sample_foreground(self) -> None:
        for layer in reversed(self.doc.layers()):
            if not layer.visible:
                continue
            match = _HEX_ONLY_RE.search(layer.style)
            if match:
                color = colors.normalize_hex(match.group(0))
                if color:
                    self.fg = color
                    self.toast(f"Sampled {color}")
                    return
        self.toast("Nothing to sample")

    def _click_selection(self, tool: str) -> None:
        w, h = self.doc.width(), self.doc.height()
        if tool == "wand":
            rect = (round(w * 0.30), round(h * 0.24), round(w * 0.34),
                    round(h * 0.36))
        elif tool == "lasso":
            rect = (round(w * 0.18), round(h * 0.22), round(w * 0.5),
                    round(h * 0.46))
        else:
            rect = (round(w * 0.21), round(h * 0.19), round(w * 0.47),
                    round(h * 0.54))
        self.selection = rect
        self.toast(f"Selection: {rect[2]} × {rect[3]} px")

    def _place_type_layer(self) -> None:
        opts = self.tool_options["type"]
        text = str(opts.get("text", "Decius")) or "Decius"
        size = int(_safe_num(str(opts.get("size", 64)), 64))
        font = str(opts.get("font", "IBM Plex Sans"))
        style = (f"background:transparent;color:{self.fg};"
                 f"font-size:{size}px;font-family:'{font}'")
        self.doc.add_layer(f"T {text}", "text", style, "Normal", 100.0,
                           "Type", "pencil")
        self.status = "Type"
        self.reload()

    def _place_shape_layer(self) -> None:
        fill = str(self.tool_option("fill", "") or self.fg)
        # Phase B: corner radius needs a real rect (border-radius does not
        # clip a background layer).
        style = (f"background:linear-gradient({fill},{fill});"
                 "background-size:42% 34%;background-position:center;"
                 "background-repeat:no-repeat")
        self.doc.add_layer("Rectangle", "pixel", style, "Normal", 100.0,
                           "Rectangle", "grid")
        self.status = "Rectangle"
        self.reload()

    def apply_crop(self) -> None:
        if self.selection is not None:
            _, _, w, h = self.selection
            if self.doc.resize_document(max(4, w), max(4, h), "Crop",
                                        "clip"):
                self.selection = None
                self.set_zoom(self.fit_zoom(), reload=False)
                self.toast(f"Cropped to {w} × {h}px")
                return
        ratio = str(self.tool_option("ratio", "Free"))
        ratios = {"1:1": 1.0, "4:3": 4 / 3, "16:9": 16 / 9}
        if ratio in ratios:
            w, h = self.doc.width(), self.doc.height()
            target = ratios[ratio]
            if w / max(1, h) > target:
                w = round(h * target)
            else:
                h = round(w / target)
            if self.doc.resize_document(max(4, w), max(4, h), "Crop",
                                        "clip"):
                self.toast(f"Cropped to {w} × {h}px")
                return
        self.toast("Crop: make a selection first (drag crop is Phase B)")

    # ── Layers ──────────────────────────────────────────────────────────────

    def current_layer(self) -> ui.PhotoLayerSnapshot:
        return self.doc.active_layer()

    def set_layer(self, layer_id: str) -> None:
        if self.renaming_layer_id and self.renaming_layer_id != layer_id:
            self.renaming_layer_id = None
        if self.doc.set_active_layer(layer_id):
            self.status = f"Selected layer: {self.current_layer().name}"
            self.reload()

    def set_layer_opacity(self, value: str) -> None:
        if self.doc.set_active_opacity(_safe_num(value, 100.0)):
            self.status = "Layer Opacity"
        self.reload()

    def set_layer_fill(self, value: str) -> None:
        if self.doc.set_active_fill_amount(_safe_num(value, 100.0)):
            self.status = "Layer Fill"
        self.reload()

    def set_layer_blend(self, value: str) -> None:
        if value in LAYER_BLENDS and self.doc.set_active_blend(value):
            self.status = f"Blend: {value}"
        self.reload()

    def toggle_layer_visible(self, layer_id: str) -> None:
        if self.doc.toggle_layer_visible(layer_id):
            self.status = "Layer Visibility"
        self.reload()

    def toggle_layer_lock(self) -> None:
        locked = self.current_layer().locked
        if self.doc.set_active_locked(not locked):
            self.status = "Layer unlocked" if locked else "Layer locked"
        self.reload()

    def toggle_lock_flag(self, flag: str) -> None:
        # Visual-only pressed state (web parity: only "lock all" is real).
        if flag in self.lock_flags:
            self.lock_flags[flag] = not self.lock_flags[flag]
            self.reload()

    def toggle_layer_filtering(self) -> None:
        self.layer_filtering = not self.layer_filtering
        self.reload()

    def add_layer(self) -> None:
        self.doc.add_layer()
        self.status = "New Layer"
        self.reload()

    def duplicate_layer(self) -> None:
        if self.doc.duplicate_active_layer():
            self.status = "Duplicate Layer"
        self.reload()

    def delete_layer(self) -> None:
        if self.doc.delete_active_layer():
            self.status = "Delete Layer"
        else:
            self.status = "Cannot delete (locked or last layer)"
        self.reload()

    def move_layer(self, direction: int) -> None:
        if self.doc.move_active(direction):
            self.status = "Reorder Layers"
        self.reload()

    def merge_down(self) -> None:
        if self.doc.merge_down():
            self.status = "Merge Down"
        else:
            self.status = "Cannot merge (locked or bottom layer)"
        self.reload()

    def begin_rename(self, layer_id: str) -> None:
        self.renaming_layer_id = layer_id
        self.reload()

    def commit_rename(self, layer_id: str, name: str) -> None:
        self.renaming_layer_id = None
        if self.doc.rename_layer(layer_id, name):
            self.status = "Rename Layer"
        self.reload()

    def next_layer_name(self) -> str:
        return f"Layer {len(self.doc.layers()) + 1}"

    # ── History ─────────────────────────────────────────────────────────────

    def undo(self) -> None:
        label = self.doc.undo()
        self.status = f"Undo → {label}" if label else "Nothing to undo"
        self.reload()

    def redo(self) -> None:
        label = self.doc.redo()
        self.status = f"Redo → {label}" if label else "Nothing to redo"
        self.reload()

    def jump_history(self, index: int) -> None:
        self.doc.select_history(index)
        entries = self.doc.history()
        current = self.doc.history_index()
        if 0 <= current < len(entries):
            self.status = entries[current]
        self.reload()

    # ── Zoom / view ─────────────────────────────────────────────────────────

    def set_zoom(self, zoom: float, reload: bool = True) -> None:
        self.zoom = max(0.05, min(16.0, zoom))
        self.status = f"Zoom {round(self.zoom * 100)}%"
        if reload:
            self.reload()

    def zoom_step(self, factor: float) -> None:
        self.set_zoom(self.zoom * factor)

    def fit_to_screen(self) -> None:
        self.pan_x = 0
        self.pan_y = 0
        self.set_zoom(self.fit_zoom())

    def set_zoom_percent_text(self, text: str) -> None:
        self.set_zoom(_safe_num(text, self.zoom * 100) / 100.0)

    # ── Colors ──────────────────────────────────────────────────────────────

    def set_foreground(self, color: str, reload: bool = True) -> None:
        normalized = colors.normalize_hex(color)
        self.fg = normalized or color
        self.status = f"Foreground {self.fg}"
        if reload:
            self.reload()

    def set_foreground_hex(self, text: str) -> None:
        normalized = colors.normalize_hex(text)
        if normalized:
            self.set_foreground(normalized)
        else:
            self.toast(f"Invalid hex: {text}")

    def set_foreground_channel(self, channel: str, text: str) -> None:
        r, g, b = colors.hex_to_rgb(self.fg)
        value = int(max(0, min(255, _safe_num(text, 0))))
        channels = {"r": r, "g": g, "b": b}
        channels[channel] = value
        self.set_foreground(
            colors.rgb_to_hex(channels["r"], channels["g"], channels["b"]))

    def swap_colors(self) -> None:
        self.fg, self.bg = self.bg, self.fg
        self.status = "Swapped foreground/background"
        self.reload()

    def reset_colors(self) -> None:
        self.fg = "#000000"
        self.bg = "#ffffff"
        self.status = "Default foreground/background"
        self.reload()

    def open_foreground_picker(self) -> None:
        dialogs.open_color_picker(self, "fg")

    def open_background_picker(self) -> None:
        dialogs.open_color_picker(self, "bg")

    # ── Panels / tabs / theme ───────────────────────────────────────────────

    def toggle_panel(self, key: str) -> None:
        if key in self.panels:
            self.panels[key] = not self.panels[key]
            self.reload()

    def set_color_tab(self, tab: str) -> None:
        if tab in ("color", "swatches"):
            self.color_tab = tab
            self.reload()

    def set_layers_tab(self, tab: str) -> None:
        if tab in ("layers", "channels", "paths", "comps"):
            self.layers_tab = tab
            self.reload()

    def toggle_tweaks(self) -> None:
        self.tweaks_open = not self.tweaks_open
        self.reload()

    def set_density(self, value: str) -> None:
        normalized = value.strip().lower()
        if normalized in ("compact", "comfortable", "spacious"):
            self.density = normalized
            self.reload()

    def set_style(self, value: str) -> None:
        self.visual_style = "3d" if value.strip().lower() == "3d" else "flat"
        self.reload()

    def set_accent(self, value: str) -> None:
        if value in _ACCENTS:
            self.accent = value
            self.reload()

    # ── Dialogs ─────────────────────────────────────────────────────────────

    def show_dialog(self, spec: dialogs.DialogSpec) -> None:
        self.dialog = spec
        self.dialog_values = {
            field.key: field.value
            for field in spec.fields if field.key
        }
        self.reload()

    def set_dialog_value(self, key: str, value: object,
                         numeric: bool = False, reload: bool = False,
                         spec: dialogs.DialogSpec | None = None) -> None:
        if numeric:
            value = _safe_num(str(value),
                              _safe_num(str(self.dialog_values.get(key, 0)),
                                        0))
        self.dialog_values[key] = value
        if spec is not None:
            dialogs.field_changed(self, spec, key)
        if reload:
            self.reload()

    def close_dialog(self) -> None:
        self.dialog = None
        self.reload()

    def dialog_ok(self) -> None:
        spec = self.dialog
        self.dialog = None
        if spec is not None:
            dialogs.handle_ok(self, spec)
        self.reload()

    # ── Adjustments ─────────────────────────────────────────────────────────

    def adjustment_action(self, action: str) -> None:
        direct = {
            "invert": (self.doc.invert_active, "Invert"),
            "desat": (self.doc.desaturate_active, "Desaturate"),
        }
        if action in direct:
            op, label = direct[action]
            self.status = label if op() else "Layer is locked"
            self.reload()
            return
        if action == "threshold":
            applied = self.doc.adjust_active("grayscale(1) contrast(12)",
                                             "Threshold", "filter-lp")
            self.status = "Threshold" if applied else "Layer is locked"
            self.reload()
            return
        titles = {
            "bc": "Brightness/Contrast", "hsl": "Hue/Saturation",
            "levels": "Levels", "vibrance": "Vibrance", "curves": "Curves",
            "balance": "Color Balance", "mixer": "Channel Mixer",
            "photo": "Photo Filter", "exposure": "Exposure",
        }
        dialogs.open_adjust(self, action, titles.get(action, action.title()))

    # ── Selection ───────────────────────────────────────────────────────────

    def select_all(self) -> None:
        self.selection = (0, 0, self.doc.width(), self.doc.height())
        self.toast(f"Selection: {self.doc.width()} × {self.doc.height()} px")

    def deselect(self) -> None:
        self.selection = None
        self.toast("Deselect")

    # ── File ────────────────────────────────────────────────────────────────

    def open_sample(self) -> None:
        self.doc.load_sample_scene()
        self.selection = None
        self.fit_to_screen()
        self.toast("Sample scene loaded")

    # ── Menu dispatch (web reference §3 handler column) ────────────────────

    def menu_action(self, action: str) -> None:
        handlers = {
            # File
            "new": lambda: dialogs.open_new_doc(self),
            "open": self.open_sample,
            "place": lambda: dialogs.open_place(self),
            "save": lambda: self.toast(f"Saved {self.doc_name}.psd"),
            "export": lambda: dialogs.open_export(self),
            "close": lambda: self.toast("Close — the demo document "
                                        "stays open"),
            # Edit
            "undo": self.undo,
            "redo": self.redo,
            "cut": lambda: self.toast("Cut"),
            "copy": lambda: self.toast("Copy"),
            "paste": lambda: self.toast("Paste"),
            "fill": lambda: dialogs.open_fill(self),
            "stroke": lambda: dialogs.open_stroke(self),
            "transform": lambda: self.toast("Free Transform"),
            # Image
            "bc": lambda: self.adjustment_action("bc"),
            "hsl": lambda: self.adjustment_action("hsl"),
            "levels": lambda: self.adjustment_action("levels"),
            "invert": lambda: self.adjustment_action("invert"),
            "desat": lambda: self.adjustment_action("desat"),
            "size": lambda: dialogs.open_image_size(self),
            "canvas": lambda: dialogs.open_canvas_size(self),
            "flatten": self._flatten,
            # Layer ("lnew" stays the direct programmatic add; the menu item
            # uses "lnewdlg" → New Layer dialog, matching the web).
            "lnewdlg": lambda: dialogs.open_new_layer(self),
            "lnew": self._add_layer_direct,
            "ldup": self.duplicate_layer,
            "ldel": self.delete_layer,
            "lgroup": lambda: self.toast("Group Layers"),
            "lmask": lambda: self.toast("Add Layer Mask"),
            "lup": lambda: self.move_layer(1),
            "ldown": lambda: self.move_layer(-1),
            "lmerge": self.merge_down,
            # Select
            "sall": self.select_all,
            "sdesel": self.deselect,
            "sinv": lambda: self.toast("Inverse"),
            "sfeather": lambda: dialogs.open_feather(self),
            "sgrow": lambda: self.toast("Grow"),
            # Filter
            "fblur": lambda: dialogs.open_adjust(self, "fblur",
                                                 "Gaussian Blur"),
            "fsharp": lambda: self._apply_filter(
                "contrast(1.4) saturate(1.1)", "Sharpen", "sharpen"),
            "fnoise": lambda: dialogs.open_adjust(self, "fnoise",
                                                  "Add Noise"),
            "fpix": lambda: self.toast("Pixelate is Phase-B pixel work"),
            "femboss": lambda: self._apply_filter(
                "grayscale(1) contrast(2)", "Emboss", "extrude"),
            "ffind": lambda: self._apply_filter(
                "grayscale(1) invert(1) contrast(2.5)", "Find Edges",
                "cross-target"),
            # View
            "vin": lambda: self.zoom_step(1.4),
            "vout": lambda: self.zoom_step(1 / 1.4),
            "vfit": self.fit_to_screen,
            "v100": lambda: self.set_zoom(1.0),
            "vrulers": self._toggle_rulers,
            "vgrid": self._toggle_grid,
            "vsnap": self._toggle_snap,
            # Window
            "wlayers": lambda: self.toggle_panel("layers"),
            "wcolor": lambda: self.toggle_panel("color"),
            "whistory": lambda: self.toggle_panel("history"),
            "wadjust": lambda: self.toggle_panel("adjust"),
            "wreset": self._reset_workspace,
            # Help
            "habout": lambda: dialogs.open_about(self, False),
            "hframework": lambda: dialogs.open_about(self, True),
            "hkeys": lambda: dialogs.open_shortcuts(self),
        }
        handler = handlers.get(action)
        if handler is not None:
            handler()
        else:
            # Web fallback: unhandled menu values just toast.
            self.toast(action.replace("-", " ").title())

    def _add_layer_direct(self) -> None:
        self.doc.add_layer()
        self.status = "New Layer"
        self.reload()

    def _flatten(self) -> None:
        if self.doc.flatten():
            self.status = "Flatten Image"
        self.reload()

    def _apply_filter(self, css: str, label: str, icon: str) -> None:
        if self.doc.adjust_active(css, label, icon):
            self.status = label
        else:
            self.status = "Layer is locked"
        self.reload()

    def _toggle_rulers(self) -> None:
        self.show_rulers = not self.show_rulers
        self.reload()

    def _toggle_grid(self) -> None:
        self.show_grid = not self.show_grid
        self.reload()

    def _toggle_snap(self) -> None:
        self.snap = not self.snap
        self.reload()

    def _reset_workspace(self) -> None:
        for key in self.panels:
            self.panels[key] = True
        self.color_tab = "color"
        self.layers_tab = "layers"
        self.fit_to_screen()
