"""PhotoEditApp: state + wiring for the Python Decius Photo Edit sample.

Mirrors the web reference app (decius-css/samples/decius-photo) at the
full interaction level. The document/layer/history model AND the pixels
live in the C++ raster core (photo_core.PhotoDocument): brush strokes,
selections, adjustments with live preview, filters, crop/resize, and
pixel-snapshot history are all real. The core also owns the render path
(custom-paint canvas for the stage, navigator, and layer thumbnails), so
Python only routes pointer/keyboard input and drives document ops —
paint.js's pointer routing lives in ``_on_event`` below.
"""

from __future__ import annotations

from html import escape

import affineui as ui
import photo_core

from . import colors, dialogs, options, panels, stage
from .specs import (BLEND_TO_CANVAS, CANVAS_TO_BLEND, MENUS, TOOL_BY_ID,
                    TOOL_DEFAULTS, TOOLS, tool_icon_html)
from .styles import PHOTO_CSS, read_decius_bundle


def _framework_asset_roots() -> list[str]:
    """Absolute asset roots for the resource loader.

    The framework's shipped assets (frameworks/fonts/*, frameworks/css/*)
    live under the repo's examples/ dir. Relative url()s like
    "frameworks/fonts/decius-icons.ttf" are looked up under each root, so
    the root must be the directory that CONTAINS frameworks/. Walk up from
    this file until that directory is found; fall back to the cwd. Absolute
    so it works regardless of the launch directory.
    """
    from pathlib import Path

    here = Path(__file__).resolve()
    roots: list[str] = []
    for parent in here.parents:
        if (parent / "frameworks" / "css").is_dir():
            roots.append(str(parent))
            break
        if (parent / "examples" / "frameworks" / "css").is_dir():
            roots.append(str(parent / "examples"))
            break
    roots.append(str(Path.cwd()))  # last-resort for relative launches
    return roots


_ACCENTS = ("blue", "orange", "green", "purple", "teal")
_ACCENT_DOTS = {
    "blue": "#4d9fff", "orange": "#ff9f4d", "green": "#4dd97b",
    "purple": "#b48cff", "teal": "#2dd4bf",
}

# Web per-tool CSS cursors, restricted to what the native renderer maps.
_TOOL_CURSORS = {
    "move": "move", "hand": "move", "type": "text", "zoom": "crosshair",
}

# First tool wins for shared shortcut letters (web TOOLS.find semantics).
_KEY_TO_TOOL: dict[str, str] = {}
for _tool in TOOLS:
    _KEY_TO_TOOL.setdefault(_tool.key.upper(), _tool.id)

_PAINT_TOOLS = frozenset(
    tool.id for tool in TOOLS if tool.paint)
_RECT_DRAG_TOOLS = frozenset(
    ("marquee", "lasso", "crop", "shape", "gradient"))


def _safe_num(value: str, fallback: float) -> float:
    try:
        return float(str(value).strip().rstrip("%"))
    except (TypeError, ValueError):
        return fallback


class PhotoEditApp:
    def __init__(self) -> None:
        self.app: ui.App | None = None
        self.doc = photo_core.PhotoDocument(1280, 800)
        self.doc.load_sample_scene()  # web-parity boot scene

        self.doc_name = "Untitled-1"
        self.color_mode = "RGB/8"
        self.resolution = 72

        self.tool = "brush"  # web boots with the brush tool
        self.tool_options: dict[str, dict[str, object]] = {
            tool_id: dict(defaults)
            for tool_id, defaults in TOOL_DEFAULTS.items()
        }

        self.fg = "#1f6feb"  # web boot colors
        self.bg = "#ffffff"

        self.sel_feather = 0.0
        self.quickmask = False
        self.show_grid = False
        self.show_rulers = True
        self.snap = True
        self.density = "comfortable"
        self.visual_style = "flat"
        self.accent = "blue"
        self.tweaks_open = False
        # Decius framework bundle, read once and cached; the stylesheet is
        # installed once (not re-parsed on every reload — see reload()).
        self._decius_css: str | None = None
        self._decius_base: str = ""
        self._stylesheet_installed = False
        # True once set_view() has installed the reconcile builder; reload()
        # then reconciles (rebuild_view) instead of reparsing (load_view).
        self._view_installed = False

        self.status = TOOL_BY_ID[self.tool].tip
        self.panels = {"navigator": True, "color": True, "layers": True,
                       "adjust": True, "history": True}
        self._dock_reset_pending = False
        self.lock_flags = {"transparency": False, "image": False,
                           "position": False}
        self.layer_filtering = False
        self.renaming_layer_id: int | None = None

        self.dialog: dialogs.DialogSpec | None = None
        self.dialog_values: dict[str, object] = {}

        # pointer-routing state (paint.js `drag`)
        self._drag: dict[str, object] | None = None
        self._space_down = False
        self._needs_reload = False
        self._boot_synced = False
        # Last stage-canvas screen bounds (x,y,w,h) seen from the hover
        # chain — a reload-proof fallback for pointer-in-stage gating when
        # the core's live stage rect isn't available yet.
        self._last_stage_bounds: tuple[int, int, int, int] | None = None

    # ── View assembly ───────────────────────────────────────────────────────

    @property
    def tool_spec(self):
        return TOOL_BY_ID[self.tool]

    @property
    def hsv(self) -> tuple[float, float, float]:
        return colors.hex_to_hsv(self.fg)

    @property
    def zoom(self) -> float:
        return self.doc.zoom()

    def tool_cursor(self) -> str:
        return _TOOL_CURSORS.get(self.tool, "crosshair")

    def _populate_view(self, view: ui.View) -> None:
        """Configure + build the whole UI into a GIVEN view (begin..end).

        This is the set_view() builder body: the App owns a persistent View
        and hands it here on every rebuild so only the diff reaches the
        document (the reconcile fast path). It is also reused by build_view()
        for the legacy load_view path. Selectors and the dock-layout/placement
        providers are re-applied each call because the App reuses one View
        across rebuilds — a stale provider from a prior build would otherwise
        linger.
        """
        view.selector(ui.decius.selector.style, self.visual_style)
        view.selector(ui.decius.selector.density, self.density)
        view.selector(ui.decius.selector.accent, self.accent)
        # Replay the LIVE dock arrangement (dragged palettes, docked splits,
        # tearoffs) instead of the declared seed on every rebuild; placement
        # overrides cover panels the user re-placed while their group was
        # hidden. Without these providers each reload would snap every
        # palette back to its seed position.
        # After Reset Workspace, skip the providers for ONE rebuild so the
        # declared seed layout wins over the live arrangement.
        if self._dock_reset_pending:
            self._dock_reset_pending = False
        elif self.app is not None:
            doc = self.app.document()
            view.set_dock_layout_provider(doc.dock_layout)
            overrides = dict(doc.dock_overrides())
            view.set_dock_placement_provider(
                lambda pid: overrides.get(pid, ui.DockPlacement()))
        view.begin()
        view.container(classes="ps-app", key="ps-app", build=self._build_app)
        if self.dialog is not None:
            view.container(classes="ps-dialog-backdrop",
                           key="ps-dialog-backdrop",
                           build=lambda v: dialogs.build_dialog(self, v))
        view.end()

    def build_view(self) -> ui.View:
        # Legacy one-shot path (load_view): build a fresh View. The live app
        # runs through set_view()/_populate_view() instead.
        view = ui.View(ui.ViewTheme.Decius)
        self._populate_view(view)
        return view

    def reload(self) -> None:
        # State changed → re-render. When a set_view() builder is installed
        # (the live app), reconcile synchronously via rebuild_view(): it
        # re-runs _populate_view() into the App's persistent View and pushes
        # only the diff to the document — the cheap paint-only mutation path,
        # NOT the ~74ms serialize+reparse that load_view() pays. Synchronous
        # (not just invalidate()) so headless tests, which have no frame loop,
        # still see a rebuilt DOM immediately after reload(). Interactive
        # per-frame coalescing rides invalidate() from the drag paths.
        if self.app is None:
            return
        self._needs_reload = False
        self._ensure_stylesheet()
        if self._view_installed:
            self.app.rebuild_view()
        else:
            self.app.load_view(self.build_view())

    def _ensure_stylesheet(self) -> None:
        # The stylesheet (Decius framework bundle + PHOTO_CSS) never changes
        # between reloads, so parse and install it ONCE. Doing it on every
        # reload re-parsed the whole bundle each time — ~70ms — which made
        # every selection/history click feel laggy. The Decius view theme
        # only emits .dcs-*/.di-* class names; the bundle that styles them
        # (and the di icon font) is a separate sheet, handed the bundle's
        # directory as the base URL so its relative url(../fonts/…) resolve
        # like a <link>ed sheet.
        if self._stylesheet_installed:
            return
        if self._decius_css is None:
            self._decius_css, self._decius_base = read_decius_bundle()
            if not self._decius_css:
                print("[photo_edit] WARNING: Decius framework bundle not "
                      "found — UI will be unstyled. Run from the repo or "
                      "ship frameworks/ beside the app.")
        combined = (self._decius_css + "\n" + PHOTO_CSS
                    if self._decius_css else PHOTO_CSS)
        self.app.set_stylesheet(combined, self._decius_base or None)
        self._stylesheet_installed = True

    def launch(self, high_dpi: bool, perf: bool, headless: bool) -> None:
        asset_roots = _framework_asset_roots()
        self.app = ui.App(
            title="Decius Photo Edit (Python)",
            width=1280,
            height=820,
            asset_folders=asset_roots,
            high_dpi=high_dpi,
            perf_overlay=perf,
        )
        # The raster core owns the whole canvas render path (stage,
        # navigator, per-layer thumbnails) via custom-paint handlers.
        self.doc.attach(self.app)
        self.app.on_event(self._on_event)
        self.app.on_frame(self._on_frame)
        # Install the stylesheet ONCE before the builder: set_stylesheet
        # re-bootstraps an already-installed view, so doing it first avoids a
        # redundant rebuild. Then hand the App the reconcile builder — every
        # later reload() reconciles the diff instead of reparsing the DOM.
        self._ensure_stylesheet()
        self.app.set_view(self._populate_view)
        self._view_installed = True
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
        # The docking workspace: the stage is the declared document (the
        # dock tree's center), every palette a DECLARED dockpanel seeded
        # floating (panels.declare_float_panels). The dock-layout provider
        # wired in build_view() replays the user's arrangement — dragged
        # palette positions, docked splits, and torn-off tabs all survive
        # rebuilds. The tool strip and quick floatbar are floating toolbars
        # (not dockables) and stay raw overlays.
        def workarea(dv: ui.View) -> None:
            dv.document(lambda d: stage.build_document_body(self, d),
                        title=self.title_text(), icon="image")
            panels.declare_float_panels(self, dv)

        v.document_view("ps-workarea", workarea)
        # data-dcs-drag makes the floating toolbar movable by its grip
        # (a [data-dcs-drag-handle] inside); without it the core's float-drag
        # never arms and the toolbar can't be moved.
        strip = v.container(
            classes="dcs-toolbar dcs-toolbar--v dcs-toolbar--floating "
                    "ps-toolstrip",
            key="ps-toolstrip",
            build=lambda t: panels.build_toolstrip(self, t))
        strip.attr("data-dcs-drag", "")
        panels.build_floatbar(self, v)

    # ── Derived text ────────────────────────────────────────────────────────

    def title_text(self) -> str:
        return (f"{self.doc_name} @ {round(self.zoom * 100)}% "
                f"({self.color_mode})")

    def doc_size_text(self) -> str:
        mb = self.doc.width() * self.doc.height() * 4 / 1048576
        return f"{mb:.2f}M / {mb * max(1, len(self.doc.layers())):.1f}M"

    # ── Status / toast ──────────────────────────────────────────────────────

    def toast(self, message: str) -> None:
        self.status = message
        self.reload()

    # ── Pointer routing (paint.js pointer section) ──────────────────────────

    def _on_frame(self, dt: float) -> None:
        # One-shot: the core auto-fits the view on its first stage paint;
        # sync the zoom readouts (rulers/statusbar/title) right after.
        if not self._boot_synced and self.doc.stage_rect() is not None:
            self._boot_synced = True
            self.reload()
            return
        # Wheel zoom/pan has no release event to reload on; refresh the
        # readouts next frame. Drags defer to their mouse-up reload.
        if self._needs_reload and self._drag is None:
            self.reload()

    def _on_event(self, ev: ui.Event, hover) -> bool:
        et = ev.type
        if et == ui.EventType.MouseDown:
            return self._mouse_down(ev, hover)
        if et == ui.EventType.MouseMove:
            return self._mouse_move(ev, hover)
        if et == ui.EventType.MouseUp:
            return self._mouse_up(ev)
        if et == ui.EventType.MouseWheel:
            return self._wheel(ev, hover)
        if et == ui.EventType.KeyDown:
            return self._key_down(ev)
        if et == ui.EventType.KeyUp:
            if ev.key == ui.Key.Space:
                self._space_down = False
            return False
        return False

    def _over_stage(self, hover) -> bool:
        for info in hover:
            if info.valid and "ps-stage-canvas" in info.classes:
                b = info.bounds
                self._last_stage_bounds = (b.x, b.y, b.w, b.h)
                return True
        return False

    @staticmethod
    def _hover_bounds(hover, cls: str):
        for info in hover:
            if info.valid and cls in info.classes:
                return info.bounds
        return None

    @staticmethod
    def _hover_attr(hover, cls: str, attr: str):
        for info in hover:
            if info.valid and cls in info.classes:
                for name, value in info.attrs:
                    if name == attr:
                        return value
        return None

    def _doc_point(self, ev: ui.Event) -> tuple[float, float]:
        return self.doc.screen_to_doc(float(ev.pos.x), float(ev.pos.y))

    def _capture(self) -> None:
        if self.app is not None:
            self.app.capture_pointer()

    def _mouse_down(self, ev: ui.Event, hover) -> bool:
        if self.dialog is not None:
            return False
        # Snappy activation: layer selection and history jumps fire on
        # mousedown, not the framework's mouseup on_click. The row carries
        # its id/index as a data attribute so the router can act immediately.
        lid = self._hover_attr(hover, "ps-layer", "data-layer-id")
        if lid is not None:
            self.set_layer(int(lid))
            return True
        hidx = self._hover_attr(hover, "ps-history-item", "data-history-index")
        if hidx is not None:
            self.jump_history(int(hidx))
            return True
        if self._panel_drag_down(ev, hover):
            return True
        if not self._over_stage(hover):
            return False
        x, y = self._doc_point(ev)
        tool = "hand" if (self._space_down or self.tool == "hand") \
            else self.tool

        if tool == "hand":
            self._drag = {"kind": "pan", "sx": ev.pos.x, "sy": ev.pos.y,
                          "px": self.doc.pan_x(), "py": self.doc.pan_y()}
            self._capture()
            return True
        if tool == "zoom":
            factor = (1 / 1.4) if ev.alt else 1.4
            self.doc.set_zoom_at(self.zoom * factor, float(ev.pos.x),
                                 float(ev.pos.y))
            self.reload()
            return True
        if tool == "eyedropper":
            color = self.doc.pick_color(x, y)
            if color:
                self.set_foreground(color)
            return True
        if tool == "fill":
            opts = self.tool_options["fill"]
            ok = self.doc.fill_at(
                x, y, self.fg,
                tolerance=_safe_num(str(opts.get("tolerance", 32)), 32),
                contiguous=bool(opts.get("contiguous", True)),
                opacity=_safe_num(str(opts.get("opacity", 100)), 100) / 100)
            self.status = "Paint Bucket" if ok else "Layer is locked"
            self.reload()
            return True
        if tool == "wand":
            opts = self.tool_options["wand"]
            rect = self.doc.wand_select(
                x, y,
                tolerance=_safe_num(str(opts.get("tolerance", 32)), 32),
                contiguous=bool(opts.get("contiguous", True)))
            if rect is not None:
                self.status = f"Selection: {rect[2]} × {rect[3]} px"
            self.reload()
            return True
        if tool == "type":
            opts = self.tool_options["type"]
            text = str(opts.get("text", "Decius")) or "Decius"
            size = _safe_num(str(opts.get("size", 64)), 64)
            self.doc.place_type(x, y, text, size=size, color=self.fg,
                                bold=True)
            self.status = f"Type: {text}"
            self.reload()
            return True
        if tool == "pen":
            self.doc.pen_add_point(x, y)
            return True
        if tool == "clone" and ev.alt:
            self.doc.set_clone_source(x, y)
            self.toast("Clone source set ✓")
            return True
        if tool == "move":
            if self.doc.begin_move():
                self._drag = {"kind": "move", "sx": x, "sy": y}
                self._capture()
            else:
                self.toast("Layer is locked")
            return True
        if tool in _RECT_DRAG_TOOLS:
            self._drag = {"kind": tool, "ax": x, "ay": y, "bx": x, "by": y,
                          "pts": [(x, y)]}
            self._capture()
            self._preview_rect(x, y, x, y)
            return True
        if tool in _PAINT_TOOLS:
            if self._begin_paint_stroke(tool, x, y):
                self._drag = {"kind": "stroke"}
                self._capture()
            return True
        return False

    # ── Panel pointer behaviors (web initPicker / Navigator pan) ────────────

    def _panel_drag_down(self, ev: ui.Event, hover) -> bool:
        sv = self._hover_bounds(hover, "ps-sv")
        if sv is not None:
            h, _, _ = self.hsv
            self._drag = {"kind": "sv", "bounds": sv, "h": h}
            self._capture()
            self._sv_pick(ev)
            return True
        hue = self._hover_bounds(hover, "ps-hue")
        if hue is not None:
            _, s, v = self.hsv
            self._drag = {"kind": "hue", "bounds": hue, "s": s, "v": v}
            self._capture()
            self._hue_pick(ev)
            return True
        nav = self._hover_bounds(hover, "ps-nav-canvas")
        if nav is not None:
            self._drag = {"kind": "navpan", "bounds": nav}
            self._capture()
            self._nav_pan(ev)
            return True
        return False

    def _sv_pick(self, ev: ui.Event) -> None:
        drag = self._drag
        b = drag["bounds"]
        s = min(max((ev.pos.x - b.x) / max(1, b.w), 0.0), 1.0)
        v = min(max(1.0 - (ev.pos.y - b.y) / max(1, b.h), 0.0), 1.0)
        self.fg = colors.hsv_to_hex(drag["h"], s, v)
        self._sync_picker_dom(drag["h"], s, v)

    def _hue_pick(self, ev: ui.Event) -> None:
        drag = self._drag
        b = drag["bounds"]
        h = min(max((ev.pos.x - b.x) / max(1, b.w), 0.0), 1.0) * 360.0
        drag["h"] = h
        self.fg = colors.hsv_to_hex(h, drag["s"], drag["v"])
        self._sync_picker_dom(h, drag["s"], drag["v"])

    def _sync_picker_dom(self, h: float, s: float, v: float) -> None:
        # Live DOM updates while dragging (dot positions, chips, SV hue);
        # the full view reload happens on release.
        if self.app is None:
            return
        doc = self.app.document()
        doc.set_attribute_by_id(
            "ps-sv-dot", "style",
            f"left:{s * 100:.1f}%;top:{(1 - v) * 100:.1f}%")
        doc.set_attribute_by_id("ps-hue-dot", "style",
                                f"left:{h / 360 * 100:.1f}%")
        doc.set_attribute_by_id(
            "ps-sv", "style",
            "background:linear-gradient(to top,#000,transparent),"
            f"linear-gradient(to right,#fff,hsl({h:.0f},100%,50%))")
        doc.set_attribute_by_id("ps-chip-fg", "style",
                                f"background:{self.fg}")

    def _nav_pan(self, ev: ui.Event) -> None:
        # panels.js centerOnDoc: the clicked navigator point becomes the
        # stage center.
        b = self._drag["bounds"]
        dw, dh = max(1, self.doc.width()), max(1, self.doc.height())
        s = min(b.w / dw, b.h / dh)
        if s <= 0:
            return
        tw, th = dw * s, dh * s
        ox, oy = (b.w - tw) / 2, (b.h - th) / 2
        dx = min(max((ev.pos.x - b.x - ox) / s, 0.0), float(dw))
        dy = min(max((ev.pos.y - b.y - oy) / s, 0.0), float(dh))
        z = self.zoom
        self.doc.set_pan((dw / 2 - dx) * z, (dh / 2 - dy) * z)
        self._sync_overlays()

    def _begin_paint_stroke(self, tool: str, x: float, y: float) -> bool:
        opts = self.tool_options.get(tool, {})
        size = _safe_num(str(opts.get("size", 24)), 24)
        hardness = _safe_num(str(opts.get("hardness", 70)), 70) / 100
        if tool in ("dodge", "burn"):
            opacity = _safe_num(str(opts.get("exposure", 40)), 40) / 100
        elif tool in ("smudge", "blur"):
            opacity = _safe_num(str(opts.get("strength", 50)), 50) / 100
        else:
            opacity = _safe_num(str(opts.get("opacity", 100)), 100) / 100
        flow = _safe_num(str(opts.get("flow", 100)), 100) / 100
        ok = self.doc.begin_stroke(tool, x, y, size=size, hardness=hardness,
                                   opacity=opacity, flow=flow, color=self.fg)
        if not ok:
            if tool == "clone" and not self.doc.has_clone_source():
                self.toast("Set a source first (Alt-click).")
            else:
                self.toast("Layer is locked")
        return ok

    def _mouse_move(self, ev: ui.Event, hover) -> bool:
        drag = self._drag
        if drag is None or self._over_stage(hover):
            self._update_cursor_readout(ev)
        if drag is None:
            return False
        kind = drag["kind"]
        if kind == "sv":
            self._sv_pick(ev)
            return True
        if kind == "hue":
            self._hue_pick(ev)
            return True
        if kind == "navpan":
            self._nav_pan(ev)
            self._needs_reload = True
            return True
        if kind == "pan":
            self.doc.set_pan(drag["px"] + (ev.pos.x - drag["sx"]),
                             drag["py"] + (ev.pos.y - drag["sy"]))
            self._sync_overlays()
            self._needs_reload = True
            return True
        x, y = self._doc_point(ev)
        if kind == "stroke":
            self.doc.stroke_to(x, y)
            return True
        if kind == "move":
            self.doc.move_to(x - drag["sx"], y - drag["sy"])
            return True
        # rect-drag family: update the live preview rect
        drag["bx"], drag["by"] = x, y
        if kind == "lasso":
            drag["pts"].append((x, y))
            xs = [p[0] for p in drag["pts"]]
            ys = [p[1] for p in drag["pts"]]
            self._preview_rect(min(xs), min(ys), max(xs), max(ys))
        else:
            self._preview_rect(drag["ax"], drag["ay"], x, y)
        return True

    def _mouse_up(self, ev: ui.Event) -> bool:
        drag = self._drag
        if drag is None:
            return False
        self._drag = None
        if self.app is not None:
            self.app.release_pointer()
        kind = drag["kind"]
        if kind in ("pan", "navpan"):
            self.reload()
            return True
        if kind in ("sv", "hue"):
            self.status = f"Foreground {self.fg}"
            self.reload()
            return True
        if kind == "stroke":
            self.doc.end_stroke()
            self.reload()
            return True
        if kind == "move":
            self.doc.end_move()
            self.status = "Move Layer"
            self.reload()
            return True
        ax, ay = drag["ax"], drag["ay"]
        bx, by = drag["bx"], drag["by"]
        x, y = min(ax, bx), min(ay, by)
        w, h = abs(bx - ax), abs(by - ay)
        if kind in ("marquee", "lasso"):
            if kind == "lasso":
                xs = [p[0] for p in drag["pts"]]
                ys = [p[1] for p in drag["pts"]]
                x, y = min(xs), min(ys)
                w, h = max(xs) - x, max(ys) - y
            if w > 1 and h > 1:
                self.doc.set_selection(round(x), round(y), round(w),
                                       round(h))
                self.status = f"Selection: {round(w)} × {round(h)} px"
            else:
                self.doc.clear_selection()
            self.reload()
            return True
        if kind == "crop":
            if w > 4 and h > 4:
                self.doc.crop_to(round(x), round(y), round(w), round(h))
                self.status = f"Cropped to {round(w)} × {round(h)}px"
            self.reload()
            return True
        if kind == "shape":
            opts = self.tool_options["shape"]
            fill = str(opts.get("fill", "") or self.fg)
            radius = _safe_num(str(opts.get("radius", 8)), 8)
            if self.doc.draw_shape(ax, ay, bx, by, fill, radius):
                self.status = "Rectangle"
            else:
                self.status = "Layer is locked"
            self.reload()
            return True
        if kind == "gradient":
            opacity = _safe_num(
                str(self.tool_options["gradient"].get("opacity", 100)),
                100) / 100
            if self.doc.apply_gradient(ax, ay, bx, by, self.fg, opacity):
                self.status = "Gradient"
            else:
                self.status = "Layer is locked"
            self.reload()
            return True
        return True

    def _pointer_in_stage(self, ev: ui.Event) -> bool:
        # Geometry check that survives a DOM reload — unlike the hover chain,
        # which goes stale right after reload() until the next mouse-move.
        # A wheel burst reloads between events, so hover-based gating would
        # drop every wheel after the first (zoom appears to move one step
        # then stall). Prefer the core's live stage rect; fall back to the
        # last stage bounds seen from the hover chain.
        rect = self.doc.stage_rect() or self._last_stage_bounds
        if rect is None:
            return False
        x, y = float(ev.pos.x), float(ev.pos.y)
        return (rect[0] <= x < rect[0] + rect[2]
                and rect[1] <= y < rect[1] + rect[3])

    def _wheel(self, ev: ui.Event, hover) -> bool:
        if self.dialog is not None:
            return False
        if not (self._over_stage(hover) or self._pointer_in_stage(ev)):
            return False
        if ev.ctrl or ev.super:
            factor = 1.12 if ev.wheel_dy > 0 else 0.89
            self.doc.set_zoom_at(self.zoom * factor, float(ev.pos.x),
                                 float(ev.pos.y))
        else:
            self.doc.set_pan(self.doc.pan_x() + ev.wheel_dx * 32,
                             self.doc.pan_y() + ev.wheel_dy * 32)
        # Update the zoom readouts in place (statusbar/title/rulers/nav) so a
        # wheel burst stays snappy without a full DOM reload — reloading
        # between wheel events staled the hover chain and dropped every wheel
        # after the first. A final reload still lands via _needs_reload once
        # wheeling settles.
        self._sync_overlays()
        self._sync_zoom_readouts()
        self._needs_reload = True
        return True

    def _sync_zoom_readouts(self) -> None:
        # Live-update the visible zoom % (navigator) during a wheel burst;
        # the statusbar field and rulers refresh on the settle reload.
        if self.app is None:
            return
        self.app.document().set_text_by_id(
            "ps-nav-pct", f"{round(self.zoom * 100)}%")

    # ── Keyboard (web wireKeys) ─────────────────────────────────────────────

    def _key_down(self, ev: ui.Event) -> bool:
        if self.app is not None and \
                self.app.document().text_editing_active():
            return False
        key = ev.key
        cmd = ev.ctrl or ev.super
        K = ui.Key
        if key == K.Escape and self.dialog is not None:
            self.close_dialog()
            return True
        if key == K.Space:
            if not self._space_down:
                self._space_down = True
            return True
        if cmd and key == K.Z:
            self.redo() if ev.shift else self.undo()
            return True
        if cmd and key == K.Y:
            self.redo()
            return True
        if cmd and key == K.A:
            self.select_all()
            return True
        if cmd and key == K.D:
            self.deselect()
            return True
        if cmd and key == K.Equal:
            self.zoom_step(1.4)
            return True
        if cmd and key == K.Minus:
            self.zoom_step(1 / 1.4)
            return True
        if cmd and key == K.Digit0:
            self.fit_to_screen()
            return True
        if cmd and key == K.Digit1:
            self.set_zoom(1.0)
            return True
        if cmd and key == K.S:
            self.toast(f"Saved {self.doc_name}.psd")
            return True
        if cmd:
            return False
        if key == K.X:
            self.swap_colors()
            return True
        if key == K.D:
            self.reset_colors()
            return True
        if key == K.BracketLeft:
            self.move_layer(-1)
            return True
        if key == K.BracketRight:
            self.move_layer(1)
            return True
        if key in (K.Delete, K.Backspace) and self.doc.has_selection():
            if self.doc.delete_selection_pixels():
                self.status = "Clear"
            self.reload()
            return True
        # tool shortcuts (single letters, first-match like the web)
        name = getattr(key, "name", "")
        if len(name) == 1 and name in _KEY_TO_TOOL:
            self.set_tool(_KEY_TO_TOOL[name])
            return True
        return False

    # ── Live overlay updates during drags (no view reload) ──────────────────

    def _sync_overlays(self) -> None:
        if self.app is None:
            return
        self.app.document().set_attribute_by_id(
            "ps-marquee", "style", stage.marquee_style(self))

    def _preview_rect(self, ax: float, ay: float, bx: float,
                      by: float) -> None:
        if self.app is None:
            return
        rect = self.doc.stage_rect()
        if rect is None:
            return
        ox, oy = self.doc.doc_origin()
        z = self.zoom
        x, y = min(ax, bx), min(ay, by)
        w, h = abs(bx - ax), abs(by - ay)
        style = (f"left:{ox - rect[0] + x * z:.1f}px;"
                 f"top:{oy - rect[1] + y * z:.1f}px;"
                 f"width:{w * z:.1f}px;height:{h * z:.1f}px")
        self.app.document().set_attribute_by_id("ps-marquee", "style", style)

    def _update_cursor_readout(self, ev: ui.Event) -> None:
        if self.app is None or self.doc.stage_rect() is None:
            return
        x, y = self._doc_point(ev)
        self.app.document().set_text_by_id(
            "ps-cursor-text", f"{round(x)}, {round(y)} px")

    # ── Tools ───────────────────────────────────────────────────────────────

    def set_tool(self, tool_id: str) -> None:
        if tool_id not in TOOL_BY_ID:
            return
        if self.tool == "pen" and tool_id != "pen":
            self.doc.pen_clear()
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

    @property
    def clone_source_set(self) -> bool:
        return self.doc.has_clone_source()

    def toggle_quickmask(self) -> None:
        self.quickmask = not self.quickmask
        self.status = ("Quick Mask on" if self.quickmask
                       else "Quick Mask off")
        self.reload()

    def apply_crop(self) -> None:
        if self.doc.has_selection():
            sel = self.doc.selection()
            if self.doc.apply_crop():
                self.toast(f"Cropped to {sel[2]} × {sel[3]}px")
                return
        self.toast("Crop: drag a region first (or make a selection)")

    # ── Layers ──────────────────────────────────────────────────────────────

    def current_layer(self) -> photo_core.PhotoLayer:
        return self.doc.active_layer()

    def current_blend_name(self) -> str:
        return CANVAS_TO_BLEND.get(self.current_layer().blend, "Normal")

    def set_layer(self, layer_id: int) -> None:
        if self.renaming_layer_id and self.renaming_layer_id != layer_id:
            self.renaming_layer_id = None
        if self.doc.set_active_id(layer_id):
            self.status = f"Selected layer: {self.current_layer().name}"
            self.reload()

    def set_layer_opacity(self, value: str) -> None:
        if self.doc.set_active_opacity(_safe_num(value, 100.0) / 100.0):
            self.doc.snapshot("Layer Opacity", "opacity")
            self.status = "Layer Opacity"
        self.reload()

    def set_layer_fill(self, value: str) -> None:
        if self.doc.set_active_fill(_safe_num(value, 100.0) / 100.0):
            self.doc.snapshot("Layer Fill", "opacity")
            self.status = "Layer Fill"
        self.reload()

    def set_layer_blend(self, value: str) -> None:
        blend = BLEND_TO_CANVAS.get(value)
        if blend and self.doc.set_active_blend(blend):
            self.status = f"Blend: {value}"
        self.reload()

    def toggle_layer_visible(self, layer_id: int) -> None:
        if self.doc.toggle_visible(layer_id):
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
        if self.doc.duplicate_active():
            self.status = "Duplicate Layer"
        self.reload()

    def delete_layer(self) -> None:
        if self.doc.delete_active():
            self.status = "Delete Layer"
        else:
            self.status = "Cannot delete the last layer"
        self.reload()

    def move_layer(self, direction: int) -> None:
        if self.doc.move_active(direction):
            self.status = "Reorder Layers"
        self.reload()

    def merge_down(self) -> None:
        if self.doc.merge_down():
            self.status = "Merge Down"
        else:
            self.status = "Cannot merge (bottom layer)"
        self.reload()

    def begin_rename(self, layer_id: int) -> None:
        self.renaming_layer_id = layer_id
        self.reload()

    def commit_rename(self, layer_id: int, name: str) -> None:
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
        self.doc.jump_to(index)
        entries = self.doc.history_entries()
        current = self.doc.history_index()
        if 0 <= current < len(entries):
            self.status = entries[current].name
        self.reload()

    # ── Zoom / view ─────────────────────────────────────────────────────────

    def set_zoom(self, zoom: float, reload: bool = True) -> None:
        self.doc.set_zoom(zoom)
        self.status = f"Zoom {round(self.zoom * 100)}%"
        if reload:
            self.reload()

    def zoom_step(self, factor: float) -> None:
        self.set_zoom(self.zoom * factor)

    def fit_to_screen(self) -> None:
        self.doc.fit_to_screen()
        self.status = f"Zoom {round(self.zoom * 100)}%"
        self.reload()

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
        if self.doc.previewing():
            self.doc.cancel_preview()  # adjustment modal Cancel/×
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
            "invert": ("invert", "Invert", "mirror"),
            "desat": ("desat", "Desaturate", "mono"),
            "threshold": ("threshold", "Threshold", "filter-lp"),
        }
        if action in direct:
            kind, label, icon = direct[action]
            applied = self.doc.apply_adjust(kind, 0, label, icon)
            self.status = label if applied else "Layer is locked"
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
        self.doc.select_all()
        self.toast(f"Selection: {self.doc.width()} × {self.doc.height()} px")

    def deselect(self) -> None:
        self.doc.clear_selection()
        self.toast("Deselect")

    # ── File ────────────────────────────────────────────────────────────────

    def open_sample(self) -> None:
        self.doc.load_sample_scene()
        self.doc.fit_to_screen()
        self.toast("Sample scene loaded")

    def export_document(self, fmt: str, scale: float) -> None:
        from pathlib import Path

        path = Path.cwd() / "decius-photoeditor.png"
        opaque = fmt.upper() != "PNG"  # JPG/WEBP-style opaque backdrop
        if self.doc.export_png(str(path), scale, opaque):
            self.toast(f"Exported {path.name} ({scale:g}×)")
        else:
            self.toast("Export failed")

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
            "lnew": self.add_layer,
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
            # Filter (web ui.js PS.filter: fblur is modal, the rest are
            # immediate pixel ops)
            "fblur": lambda: dialogs.open_adjust(self, "fblur",
                                                 "Gaussian Blur"),
            "fsharp": lambda: self._apply_filter("sharpen", 0, "Sharpen",
                                                 "sharpen"),
            "fnoise": lambda: self._apply_filter("noise", 90, "Add Noise",
                                                 "wave-noise"),
            "fpix": lambda: self._apply_filter("pixelate", 10, "Pixelate",
                                               "grid"),
            "femboss": lambda: self._apply_filter("emboss", 0, "Emboss",
                                                  "extrude"),
            "ffind": lambda: self._apply_filter("findedges", 0,
                                                "Find Edges",
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

    def _flatten(self) -> None:
        if self.doc.flatten():
            self.status = "Flatten Image"
        self.reload()

    def _apply_filter(self, kind: str, amount: float, label: str,
                      icon: str) -> None:
        if self.doc.apply_adjust(kind, amount, label, icon):
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
        # Drop the live dock state (tearoffs, drag-to-dock overrides,
        # remembered tabs) and rebuild once from the declared seed.
        if self.app is not None:
            self.app.document().reset_dock_state()
        self._dock_reset_pending = True
        self.fit_to_screen()
