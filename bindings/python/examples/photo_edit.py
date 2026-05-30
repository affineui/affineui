"""Python Decius Photo Edit sample.

This mirrors the sibling decius-css/samples/decius-photo app shape while keeping
the UI, callbacks, and view generation in Python. The document/layer model uses
the AffineUI C++ photo core through the Python binding.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from html import escape
from typing import Callable

import affineui as ui


@dataclass
class Tool:
    id: str
    name: str
    key: str
    icon: str
    tip: str
    group: bool = False
    paint: bool = False
    sep_after: bool = False


TOOLS: tuple[Tool, ...] = (
    Tool("move", "Move Tool", "V", "move", "Move: drag to reposition the active layer."),
    Tool("marquee", "Rectangular Marquee", "M", "marquee", "Marquee: drag to make a rectangular selection.", True),
    Tool("lasso", "Lasso Tool", "L", "lasso", "Lasso: drag to draw a freehand selection.", True),
    Tool("wand", "Object Selection", "W", "select", "Magic Wand: click to select a similar-color region.", True, sep_after=True),
    Tool("crop", "Crop Tool", "C", "clip", "Crop: drag to trim the document to a region."),
    Tool("eyedropper", "Eyedropper", "I", "eyedropper", "Eyedropper: sample a color as the foreground.", sep_after=True),
    Tool("brush", "Brush Tool", "B", "brush", "Brush: drag on the canvas to paint with the foreground color.", True, True),
    Tool("pencil", "Pencil Tool", "N", "pencil", "Pencil: hard-edged freehand strokes.", False, True),
    Tool("clone", "Clone Stamp", "S", "stamp", "Clone Stamp: copy pixels from a sampled source.", True, True),
    Tool("history", "History Brush", "Y", "history-brush", "History Brush: restore from the history source.", False, True),
    Tool("eraser", "Eraser Tool", "E", "eraser", "Eraser: drag to erase pixels on the active layer.", True, True),
    Tool("fill", "Paint Bucket", "G", "fill", "Paint Bucket: fill a region with the foreground color.", True),
    Tool("gradient", "Gradient Tool", "G", "gain", "Gradient: draw a foreground to transparent gradient.", sep_after=True),
    Tool("dodge", "Dodge Tool", "O", "dodge", "Dodge: paint to lighten pixels.", True, True),
    Tool("burn", "Burn Tool", "O", "burn", "Burn: paint to darken pixels.", False, True),
    Tool("smudge", "Smudge Tool", "R", "smudge", "Smudge: drag to push pixels around.", False, True),
    Tool("blur", "Blur Tool", "R", "blur", "Blur: paint to soften detail.", False, True, sep_after=True),
    Tool("pen", "Pen Tool", "P", "pen", "Pen: place anchor points and build a path."),
    Tool("type", "Horizontal Type", "T", "pencil", "Type: click to add a text layer."),
    Tool("shape", "Rectangle Tool", "U", "grid", "Shape: draw a filled rectangle on a new layer.", sep_after=True),
    Tool("hand", "Hand Tool", "H", "pan", "Hand: drag the document to pan."),
    Tool("zoom", "Zoom Tool", "Z", "zoom-in", "Zoom: click to zoom in; Alt-click to zoom out."),
)


MENU_ORDER: tuple[tuple[str, str], ...] = (
    ("m-file", "File"),
    ("m-edit", "Edit"),
    ("m-image", "Image"),
    ("m-layer", "Layer"),
    ("m-select", "Select"),
    ("m-filter", "Filter"),
    ("m-view", "View"),
    ("m-window", "Window"),
    ("m-help", "Help"),
)


MenuItem = tuple[str, str, str, str]


MENUS: dict[str, tuple[MenuItem | None, ...]] = {
    "m-file": (
        ("new", "New...", "Ctrl+N", "file"),
        ("open", "Open Sample...", "Ctrl+O", "folder-open"),
        ("place", "Place Embedded...", "", "import"),
        None,
        ("save", "Save", "Ctrl+S", "save"),
        ("export", "Export As...", "Shift+Ctrl+E", "export"),
        None,
        ("close", "Close", "Ctrl+W", "close"),
    ),
    "m-edit": (
        ("undo", "Undo", "Ctrl+Z", "undo"),
        ("redo", "Redo", "Shift+Ctrl+Z", "redo"),
        None,
        ("cut", "Cut", "Ctrl+X", "cut"),
        ("copy", "Copy", "Ctrl+C", "copy"),
        ("paste", "Paste", "Ctrl+V", "paste"),
        None,
        ("fill", "Fill with Foreground", "Alt+Backspace", "fill"),
        ("stroke", "Stroke...", "", "edit"),
        ("transform", "Free Transform", "Ctrl+T", "scale-corners"),
    ),
    "m-image": (
        ("bc", "Brightness / Contrast...", "", "light"),
        ("hsl", "Hue / Saturation...", "Ctrl+U", "color-grade"),
        ("levels", "Levels...", "Ctrl+L", "graph"),
        ("invert", "Invert", "Ctrl+I", "mirror"),
        ("desat", "Desaturate", "Shift+Ctrl+U", "mono"),
        None,
        ("size", "Image Size...", "Alt+Ctrl+I", "aspect"),
        ("canvas", "Canvas Size...", "Alt+Ctrl+C", "fit"),
        ("flatten", "Flatten Image", "", "layers"),
    ),
    "m-layer": (
        ("lnew", "New Layer", "Shift+Ctrl+N", "plus"),
        ("ldup", "Duplicate Layer", "Ctrl+J", "duplicate"),
        ("ldel", "Delete Layer", "", "trash"),
        None,
        ("lgroup", "Group Layers", "Ctrl+G", "folder"),
        ("lmask", "Add Layer Mask", "", "layer-mask"),
        None,
        ("lup", "Bring Forward", "Ctrl+]", "chevron-up"),
        ("ldown", "Send Backward", "Ctrl+[", "chevron-down"),
        ("lmerge", "Merge Down", "Ctrl+E", "compress"),
    ),
    "m-select": (
        ("sall", "All", "Ctrl+A", "marquee"),
        ("sdesel", "Deselect", "Ctrl+D", "close"),
        ("sinv", "Inverse", "Shift+Ctrl+I", "mirror"),
        None,
        ("sfeather", "Feather...", "Shift+F6", "blur"),
        ("sgrow", "Grow", "", "plus"),
    ),
    "m-filter": (
        ("fblur", "Gaussian Blur...", "", "blur"),
        ("fsharp", "Sharpen", "", "sharpen"),
        ("fnoise", "Add Noise...", "", "wave-noise"),
        ("fpix", "Pixelate", "", "grid"),
        None,
        ("femboss", "Emboss", "", "extrude"),
        ("ffind", "Find Edges", "", "cross-target"),
    ),
    "m-view": (
        ("vin", "Zoom In", "Ctrl++", "zoom-in"),
        ("vout", "Zoom Out", "Ctrl+-", "zoom-out"),
        ("vfit", "Fit on Screen", "Ctrl+0", "fit"),
        ("v100", "100%", "Ctrl+1", "cross-target"),
        None,
        ("vrulers", "Rulers", "Ctrl+R", "aspect"),
        ("vgrid", "Show Grid", "Ctrl+'", "grid"),
        ("vsnap", "Snap", "", "magnet"),
    ),
    "m-window": (
        ("wlayers", "Layers", "F7", "layers"),
        ("wcolor", "Color", "F6", "palette"),
        ("whistory", "History", "", "history-brush"),
        ("wadjust", "Adjustments", "", "color-grade"),
        None,
        ("wreset", "Reset Workspace", "", "grid"),
    ),
    "m-help": (
        ("habout", "About Decius Photo Edit", "", "info"),
        ("hframework", "About decius.css", "", "decius"),
        ("hkeys", "Keyboard Shortcuts", "", "keys"),
    ),
}


SWATCHES = (
    "#000000", "#404040", "#7f7f7f", "#bcbcbc", "#ffffff",
    "#ff2d2d", "#ff7a2d", "#ffd02d", "#7bd92d", "#2dd4bf",
    "#2d8cff", "#4d9fff", "#6a5cff", "#b48cff", "#ff7ab8",
    "#7a3b12", "#c0894a", "#e9cfa0", "#1f3b6e", "#0e2233",
    "#e7e9ee", "#aab0bd", "#767c8a", "#3c424f", "#181b22",
    "#5b1a1a", "#1a5b2e", "#1a3a5b", "#5b491a", "#3a1a5b",
)


ADJUSTMENTS = (
    ("light", "Brightness/Contrast", "bc"),
    ("color-grade", "Hue/Saturation", "hsl"),
    ("graph", "Levels", "levels"),
    ("curve", "Curves", "curves"),
    ("mono", "Black & White", "desat"),
    ("palette", "Color Balance", "balance"),
    ("mirror", "Invert", "invert"),
    ("eq", "Channel Mixer", "mixer"),
    ("droplet", "Photo Filter", "photo"),
    ("wave-sine", "Vibrance", "vibrance"),
    ("filter-lp", "Threshold", "threshold"),
    ("gain", "Exposure", "exposure"),
)


BLENDS = (
    "Normal",
    "Multiply",
    "Screen",
    "Overlay",
    "Darken",
    "Lighten",
    "Color Dodge",
    "Color Burn",
    "Difference",
)


PHOTO_CSS = r"""
.aui-root>.ps-app{margin:-24px;height:100vh;min-height:100vh;width:calc(100% + 48px)}
.ps-app{display:flex;flex-direction:column;min-height:100vh;background:var(--dcs-bg-app,#1f222a);color:var(--dcs-text,#e7e9ee);overflow:hidden}
.ps-menubar{flex:0 0 auto;min-height:var(--dcs-h-lg,32px);height:var(--dcs-h-lg,32px);overflow:visible}
.ps-brand{display:inline-flex;align-items:center;align-self:stretch;gap:7px;padding:0 12px;margin:0 8px 0 0;background:#0d0f14;color:#e7e9ee;line-height:1}
.ps-brand__mark{color:var(--dcs-accent,#4f86d6);font-size:14px}.ps-brand__name{font-weight:700;font-size:12px;white-space:nowrap}
.ps-doc-name{color:var(--dcs-text-mute,#8c93a3);font-family:var(--dcs-font-mono,monospace);font-size:12px}
.ps-options{display:flex;align-items:center;gap:9px;height:var(--dcs-h-xl,38px);padding:0 10px;background:var(--dcs-surface-1,#252a34);border-bottom:1px solid var(--dcs-line,#343946);overflow:hidden;white-space:nowrap;flex:0 0 auto}
.ps-tool-glyph{display:inline-flex;align-items:center;justify-content:center;width:26px;height:26px;border-radius:3px;background:var(--dcs-well,#171a21);color:var(--dcs-accent,#4f86d6);font-size:16px;flex:0 0 auto}
.ps-tool-name{font-weight:700;font-size:13px}.ps-opt-slot{display:flex;align-items:center;gap:8px;min-width:0;flex:1 1 auto;overflow:hidden}
.ps-opt-slot>.dcs-field{height:28px;min-height:28px;min-width:148px}.ps-opt-slot>.dcs-field>.dcs-field__label{flex:0 0 auto;font-size:11px}
.ps-opt-slot>.dcs-field>.dcs-slider{min-width:78px}.ps-opt-slot>.dcs-field>.aui-select{min-width:120px}.ps-opt-note{margin:0;color:var(--dcs-text-dim,#a6adbb);font-size:12px}
.ps-body{position:relative;flex:1 1 auto;min-height:0;overflow:hidden;background:var(--dcs-stage,#151820)}
.ps-doc-dock{position:absolute;inset:0}.ps-doc-dock>.dcs-dockpane{position:absolute;inset:0;display:flex;flex-direction:column}
.ps-stagewrap{position:absolute;inset:0;display:flex;background:var(--dcs-stage,#151820);overflow:hidden}
.ps-ruler-corner{position:absolute;left:0;top:0;width:18px;height:18px;background:var(--dcs-surface-1,#252a34);border-right:1px solid var(--dcs-line,#343946);border-bottom:1px solid var(--dcs-line,#343946);z-index:2}
.ps-ruler{position:absolute;background:var(--dcs-surface-1,#252a34);color:var(--dcs-text-mute,#8c93a3);font:8px var(--dcs-font-mono,monospace);overflow:hidden;z-index:1}
.ps-ruler--h{left:18px;right:0;top:0;height:18px;border-bottom:1px solid var(--dcs-line,#343946)}
.ps-ruler--v{left:0;top:18px;bottom:0;width:18px;border-right:1px solid var(--dcs-line,#343946)}
.ps-ruler-ticks{display:flex;height:100%;align-items:flex-end;gap:28px;padding-left:9px}.ps-ruler-ticks span{border-left:1px solid var(--dcs-line-strong,#4b5262);height:8px;padding-left:2px}
.ps-ruler--v .ps-ruler-ticks{flex-direction:column;align-items:flex-start;gap:28px;padding-top:9px;padding-left:0}.ps-ruler--v span{border-left:0;border-top:1px solid var(--dcs-line-strong,#4b5262);width:8px;height:auto;padding-top:2px}
.ps-stage{position:absolute;left:18px;right:0;top:18px;bottom:0;overflow:hidden;cursor:crosshair}
.ps-doc{position:absolute;left:50%;top:50%;width:640px;height:400px;transform:translate(-50%,-50%) translate(var(--px,0),var(--py,0)) scale(var(--z,.67));transform-origin:center;box-shadow:0 0 0 1px #0008,0 24px 60px rgba(0,0,0,.45);background-image:repeating-conic-gradient(#cfcfcf 0 90deg,#fff 90deg 180deg);background-size:16px 16px;overflow:hidden}
.ps-layer-canvas{position:absolute;inset:0}.ps-layer-text{display:flex;align-items:center;justify-content:center;color:#fff;font-size:58px;font-weight:800;text-shadow:0 3px 12px #0008;letter-spacing:0}.ps-layer-vignette{box-shadow:inset 0 0 160px rgba(0,0,0,.58)}
.ps-marquee{position:absolute;left:21%;top:19%;width:47%;height:54%;outline:1px dashed #fff;box-shadow:0 0 0 1px #000;pointer-events:none}
.ps-grid-overlay{position:absolute;inset:0;background-image:linear-gradient(rgba(255,255,255,.12) 1px,transparent 1px),linear-gradient(90deg,rgba(255,255,255,.12) 1px,transparent 1px);background-size:64px 64px;pointer-events:none;opacity:.5}
.ps-stage-badge{position:absolute;z-index:5}.ps-stage-badge--bl{left:8px;bottom:8px}.ps-stage-badge--tr{right:8px;top:8px}
.ps-toolstrip{position:absolute;left:12px;top:12px;display:flex;flex-direction:column;align-items:center;gap:2px;max-height:calc(100% - 24px);overflow:auto;z-index:15}
.ps-toolgrid{display:flex;flex-wrap:wrap;width:69px;gap:1px}.ps-tool{position:relative;width:34px;height:34px;display:flex;align-items:center;justify-content:center;border-radius:3px;color:var(--dcs-text-dim,#a6adbb);cursor:pointer;border:1px solid transparent;font-size:17px}
.ps-tool:hover{background:var(--dcs-surface-2,#303642);color:var(--dcs-text,#e7e9ee)}.ps-tool[aria-pressed=true]{background:var(--dcs-accent-dim,#263f64);color:var(--dcs-accent-hi,#b9d5ff);border-color:var(--dcs-accent-lo,#3b6ba8)}
.ps-tool[data-group=true]::after{content:"";position:absolute;right:3px;bottom:3px;border-left:4px solid transparent;border-bottom:4px solid var(--dcs-text-mute,#8c93a3)}
.ps-toolsep{width:28px;height:1px;background:var(--dcs-line-soft,#303642);margin:5px auto;flex:0 0 auto}
.ps-colorchips{position:relative;width:38px;height:38px;margin:8px 0 2px}.ps-colorchip{position:absolute;width:24px;height:24px;border:1px solid var(--dcs-line-strong,#4b5262);border-radius:3px;box-shadow:0 3px 8px rgba(0,0,0,.32);cursor:pointer}
.ps-colorchip--bg{right:0;bottom:0;z-index:1}.ps-colorchip--fg{left:0;top:0;z-index:2}.ps-chip-mini{position:absolute;color:var(--dcs-text-mute,#8c93a3);font-size:11px;cursor:pointer;line-height:1}.ps-swap{right:0;top:-2px}.ps-reset{left:-1px;bottom:-1px}
.ps-floating{position:absolute;z-index:12}.ps-float-panel{display:flex;flex-direction:column;min-height:90px}.ps-float-panel>.dcs-panel__body{flex:1 1 auto;min-height:0;overflow:auto}
.ps-panel-title{display:flex;align-items:center;gap:7px;min-width:0}.ps-panel-title span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.ps-colorpanel{display:flex;flex-direction:column;gap:8px;padding:10px}.ps-sv{height:120px;border-radius:3px;border:1px solid var(--dcs-line,#343946);position:relative;background:linear-gradient(to top,#000,transparent),linear-gradient(to right,#fff,var(--ps-fg,#000))}
.ps-sv-dot{position:absolute;left:64%;top:34%;width:12px;height:12px;border-radius:50%;border:2px solid #fff;box-shadow:0 0 0 1px #0009;transform:translate(-50%,-50%)}.ps-hue{height:14px;border-radius:3px;border:1px solid var(--dcs-line,#343946);background:linear-gradient(90deg,red,#ff0,lime,cyan,blue,magenta,red);position:relative}.ps-hue-dot{position:absolute;left:58%;top:50%;width:5px;height:20px;border-radius:2px;background:#fff;box-shadow:0 0 0 1px #0009;transform:translate(-50%,-50%)}
.ps-swatches{display:flex;flex-wrap:wrap;gap:3px}.ps-swatch-chip{width:20px;height:20px;border-radius:3px;border:1px solid var(--dcs-line,#343946);cursor:pointer}.ps-swatch-chip:hover{outline:1px solid var(--dcs-accent,#4f86d6);outline-offset:1px}
.ps-nav-thumb{position:relative;height:116px;border:1px solid var(--dcs-line,#343946);border-radius:3px;overflow:hidden;background-image:repeating-conic-gradient(#cfcfcf 0 90deg,#fff 90deg 180deg);background-size:10px 10px}.ps-nav-mini{position:absolute;left:12px;right:12px;top:18px;bottom:18px;background:linear-gradient(135deg,#f08c3c 0%,#e26b9c 42%,#7654d9 100%)}.ps-nav-view{position:absolute;left:36px;top:26px;width:92px;height:58px;border:2px solid var(--dcs-danger,#ff5b6a);box-shadow:0 0 0 1px #0008}
.ps-layer-filter,.ps-layer-bo,.ps-layer-lock-row,.ps-layer-footer{display:flex;align-items:center;gap:3px;padding:6px 8px;border-bottom:1px solid var(--dcs-line-soft,#303642)}.ps-layer-footer{border-top:1px solid var(--dcs-line,#343946);border-bottom:0;margin-top:auto}
.ps-layers{display:flex;flex-direction:column;min-height:0;height:100%}.ps-layer-list{overflow:auto;min-height:112px}.ps-layer{display:flex;align-items:center;gap:8px;padding:6px 8px;border-bottom:1px solid var(--dcs-line,#343946);cursor:pointer;min-height:48px}
.ps-layer:hover{background:var(--dcs-surface-1,#252a34)}.ps-layer.is-active{background:var(--dcs-accent-dim,#263f64);box-shadow:inset 2px 0 0 var(--dcs-accent,#4f86d6)}.ps-layer-eye{width:18px;text-align:center;color:var(--dcs-text-dim,#a6adbb);font-size:13px}.ps-layer-eye.is-off{opacity:.35}
.ps-layer-thumb{width:34px;height:34px;flex:none;border:1px solid var(--dcs-line-strong,#4b5262);border-radius:3px;background-image:repeating-conic-gradient(#cfcfcf 0 90deg,#fff 90deg 180deg);background-size:8px 8px;overflow:hidden}.ps-layer-thumb-fill{width:100%;height:100%}
.ps-layer-name{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:12px}.ps-layer-lock{color:var(--dcs-text-mute,#8c93a3);font-size:12px;flex:none}.ps-amt-label{font-size:11px;color:var(--dcs-text-dim,#a6adbb);white-space:nowrap}.ps-amt{font:12px var(--dcs-font-mono,monospace);min-width:38px;text-align:right}
.ps-adjust-grid{display:flex;flex-wrap:wrap;gap:5px;padding:10px}.ps-adjust{width:34px;height:34px}.ps-history-item{display:flex;align-items:center;gap:8px;padding:7px 9px;border-bottom:1px solid var(--dcs-line,#343946);color:var(--dcs-text-dim,#a6adbb);cursor:pointer}.ps-history-item.is-current{background:var(--dcs-accent-dim,#263f64);color:var(--dcs-text,#e7e9ee)}
.ps-floatbar{position:absolute;left:50%;bottom:18px;transform:translateX(-50%);display:flex;align-items:center;gap:2px;z-index:16}.ps-statusbar{display:flex;align-items:center;gap:8px;min-height:28px;padding:0 8px;background:var(--dcs-surface-1,#252a34);border-top:1px solid var(--dcs-line,#343946);font-size:12px;flex:0 0 auto}.ps-statusbar p{margin:0}.ps-status-spacer{flex:1}
.ps-dialog-backdrop{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;background:rgba(0,0,0,.45);z-index:500}.ps-dialog{width:420px;max-width:calc(100vw - 48px)}.ps-dialog .dcs-panel__body{display:flex;flex-direction:column;gap:10px;padding:14px}.ps-dialog-actions{display:flex;justify-content:flex-end;gap:8px;padding:10px 14px;border-top:1px solid var(--dcs-line,#343946)}.ps-dialog-copy{margin:0;color:var(--dcs-text-dim,#a6adbb);line-height:1.45}.ps-kbd-list{display:flex;flex-wrap:wrap;gap:6px 16px}.ps-kbd-list p{width:178px;margin:0;display:flex;justify-content:space-between;color:var(--dcs-text-dim,#a6adbb)}
"""


def _tool(tool_id: str) -> Tool:
    return next(tool for tool in TOOLS if tool.id == tool_id)


def _safe_num(value: str, fallback: float) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return fallback


def _clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


class PhotoEditApp:
    def __init__(self) -> None:
        self.app: ui.App | None = None
        self.tool = "brush"
        self.fg = "#000000"
        self.bg = "#ffffff"
        self.zoom = 0.67
        self.pan_x = 0
        self.pan_y = 0
        self.quickmask = False
        self.show_grid = False
        self.show_rulers = True
        self.snap = True
        self.density = "comfortable"
        self.visual_style = "flat"
        self.accent = "blue"
        self.status = "Brush: drag on the canvas to paint with the foreground color."
        self.dialog: tuple[str, str] | None = None
        self.panels = {
            "navigator": True,
            "color": True,
            "layers": True,
            "adjust": True,
            "history": True,
        }
        self.doc = ui.PhotoDocument(1280, 800)

    def build_view(self) -> ui.View:
        view = ui.View(ui.ViewTheme.Decius)
        view.selector(ui.decius.selector.style, self.visual_style)
        view.selector(ui.decius.selector.density, self.density)
        view.selector(ui.decius.selector.accent, self.accent)
        view.begin()
        view.container(classes="ps-app", key="ps-app", build=self._build_app)
        if self.dialog is not None:
            view.container(classes="ps-dialog-backdrop", key="ps-dialog-backdrop", build=self._build_dialog)
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
        self.reload()
        if headless:
            self.app.document().layout(1280, 820)
            size = self.app.document().content_size()
            print(f"laid out {size.width}x{size.height}")
            return
        self.app.launch(native=True)

    def snapshot(self, label: str) -> None:
        self.doc.snapshot(label)
        self.status = label

    def set_tool(self, tool_id: str) -> None:
        if tool_id not in {tool.id for tool in TOOLS}:
            return
        self.tool = tool_id
        self.status = _tool(tool_id).tip
        self.reload()

    def set_layer(self, layer_id: str) -> None:
        if self.doc.set_active_layer(layer_id):
            self.status = f"Selected layer: {self.current_layer().name}"
            self.reload()

    def current_layer(self) -> ui.PhotoLayerSnapshot:
        return self.doc.active_layer()

    def set_layer_opacity(self, value: str) -> None:
        self.doc.set_active_opacity(_clamp(_safe_num(value, 100.0), 0.0, 100.0))
        self.status = "Layer Opacity"
        self.reload()

    def set_layer_blend(self, value: str) -> None:
        if value in BLENDS:
            self.doc.set_active_blend(value)
            self.status = f"Blend: {value}"
            self.reload()

    def toggle_layer_visible(self, layer_id: str) -> None:
        self.doc.toggle_layer_visible(layer_id)
        self.status = "Layer Visibility"
        self.reload()

    def add_layer(self, name: str | None = None) -> None:
        self.doc.add_layer(name or "")
        self.status = "New Layer"
        self.reload()

    def duplicate_layer(self) -> None:
        if self.doc.duplicate_active_layer():
            self.status = "Duplicate Layer"
        self.reload()

    def delete_layer(self) -> None:
        if self.doc.delete_active_layer():
            self.status = "Delete Layer"
        self.reload()

    def set_zoom(self, zoom: float) -> None:
        self.zoom = _clamp(zoom, 0.05, 8.0)
        self.status = f"Zoom {round(self.zoom * 100)}%"
        self.reload()

    def select_history(self, index: int) -> None:
        self.doc.select_history(index)
        history = self.doc.history()
        current = self.doc.history_index()
        if 0 <= current < len(history):
            self.status = history[current]
        self.reload()

    def set_foreground(self, color: str) -> None:
        self.fg = color
        self.status = f"Foreground {color}"
        self.reload()

    def swap_colors(self) -> None:
        self.fg, self.bg = self.bg, self.fg
        self.status = "Swapped foreground/background"
        self.reload()

    def reset_colors(self) -> None:
        self.fg = "#000000"
        self.bg = "#ffffff"
        self.status = "Default foreground/background"
        self.reload()

    def toggle_panel(self, key: str) -> None:
        self.panels[key] = not self.panels[key]
        self.reload()

    def open_dialog(self, title: str, body: str) -> None:
        self.dialog = (title, body)
        self.reload()

    def close_dialog(self) -> None:
        self.dialog = None
        self.reload()

    def dialog_ok(self) -> None:
        if self.dialog:
            self.snapshot(self.dialog[0])
        self.dialog = None
        self.reload()

    def menu_action(self, action: str) -> None:
        dialogs = {
            "new": ("New Document", "Choose a preset, dimensions, color mode, and background contents."),
            "open": ("Open Sample", "Load one of the bundled sample plates into the current document."),
            "place": ("Place Embedded", "Place an embedded asset as a new layer."),
            "export": ("Export As", "Choose format, scale, and metadata options for export."),
            "stroke": ("Stroke", "Stroke the current selection with foreground color."),
            "bc": ("Brightness / Contrast", "Preview brightness and contrast on the active layer."),
            "hsl": ("Hue / Saturation", "Adjust hue, saturation, and lightness."),
            "levels": ("Levels", "Set input shadows, midtones, and highlights."),
            "size": ("Image Size", "Resize the image dimensions and resampling mode."),
            "canvas": ("Canvas Size", "Change canvas bounds and anchor the existing artwork."),
            "sfeather": ("Feather Selection", "Set the feather radius for the active selection."),
            "fblur": ("Gaussian Blur", "Blur the current layer or selected pixels."),
            "fnoise": ("Add Noise", "Apply monochromatic or color noise to the selection."),
            "habout": ("About Decius Photo Edit", "A Python AffineUI recreation of the Decius photo editing sample backed by the C++ photo core."),
            "hframework": ("About decius.css", "A dense, token-driven CSS framework for pro creative tools."),
            "hkeys": ("Keyboard Shortcuts", "V Move, M Marquee, B Brush, E Eraser, G Fill, Z Zoom, X Swap colors, D Default colors."),
        }
        if action in dialogs:
            self.open_dialog(*dialogs[action])
            return
        if action == "lnew":
            self.add_layer()
        elif action == "ldup":
            self.duplicate_layer()
        elif action == "ldel":
            self.delete_layer()
        elif action == "undo":
            self.status = self.doc.undo()
            self.reload()
        elif action == "redo":
            self.status = self.doc.redo()
            self.reload()
        elif action == "fill":
            self.doc.fill_active(self.fg)
            self.status = "Fill with Foreground"
            self.reload()
        elif action == "invert":
            self.doc.invert_active()
            self.status = "Invert"
            self.reload()
        elif action == "desat":
            self.doc.desaturate_active()
            self.status = "Desaturate"
            self.reload()
        elif action == "flatten":
            self.doc.flatten()
            self.status = "Flatten Image"
            self.reload()
        elif action == "vin":
            self.set_zoom(self.zoom * 1.4)
        elif action == "vout":
            self.set_zoom(self.zoom / 1.4)
        elif action == "vfit":
            self.set_zoom(0.67)
        elif action == "v100":
            self.set_zoom(1.0)
        elif action == "vrulers":
            self.show_rulers = not self.show_rulers
            self.reload()
        elif action == "vgrid":
            self.show_grid = not self.show_grid
            self.reload()
        elif action == "vsnap":
            self.snap = not self.snap
            self.reload()
        elif action == "wlayers":
            self.toggle_panel("layers")
        elif action == "wcolor":
            self.toggle_panel("color")
        elif action == "whistory":
            self.toggle_panel("history")
        elif action == "wadjust":
            self.toggle_panel("adjust")
        elif action == "wreset":
            for key in self.panels:
                self.panels[key] = True
            self.zoom = 0.67
            self.pan_x = 0
            self.pan_y = 0
            self.reload()
        else:
            self.snapshot(action.replace("-", " ").title())
            self.reload()

    def _build_app(self, v: ui.View) -> None:
        v.container(classes="dcs-menubar ps-menubar", key="ps-menubar", build=self._build_menubar)
        v.container(classes="ps-options", key="ps-options", build=self._build_options)
        v.container(classes="ps-body", key="ps-body", build=self._build_body)
        v.container(classes="dcs-statusbar ps-statusbar", key="ps-statusbar", build=self._build_statusbar)
        v.container(classes="ps-menu-host", key="ps-menus", build=self._build_menus)

    def _build_menubar(self, v: ui.View) -> None:
        brand = v.container(classes="ps-brand", key="ps-brand", build=lambda b: b.html('<i class="di di-decius ps-brand__mark"></i><span class="ps-brand__name">Decius Photo</span>'))
        brand.attr("role", "button").on_click(lambda: self.open_dialog("About Decius Photo Edit", "A Python clone of the Decius photo sample UI backed by a C++ photo core."))
        for menu_id, label in MENU_ORDER:
            trigger = v.button(label, key=f"trigger-{menu_id}")
            trigger.cls("dcs-menubar__item")
            trigger.attr("data-dcs-toggle", "menu")
            trigger.attr("data-dcs-target", f"#{menu_id}")
            trigger.attr("aria-expanded", "false")
        v.container(classes="dcs-menubar__spacer", key="ps-menu-spacer")
        v.container(
            classes="dcs-menubar__meta ps-doc-name",
            key="ps-doc-name",
            build=lambda m: m.html(f"<span>Untitled-1 @ {round(self.zoom * 100)}% (RGB/8)</span>"),
        )
        settings = v.button("Settings", key="ps-settings")
        settings.cls("dcs-btn dcs-btn--icon dcs-btn--ghost ps-settings")
        settings.on_click(lambda: self.open_dialog("Theme Tweaks", "Density, style, and accent controls are implemented in Python state."))

    def _build_options(self, v: ui.View) -> None:
        tool = _tool(self.tool)
        v.container(classes="ps-tool-glyph", key="ps-opt-glyph", build=lambda h: h.html(f'<i class="di di-{escape(tool.icon)}"></i>'))
        v.container(classes="ps-tool-name", key="ps-opt-name", build=lambda h: h.html(escape(tool.name)))
        v.container(classes="dcs-divider dcs-divider--v", key="ps-opt-divider-a")
        v.container(classes="ps-opt-slot", key="ps-opt-slot", build=self._build_tool_options)
        v.container(classes="dcs-divider dcs-divider--v", key="ps-opt-divider-b")
        fit = v.button("Fit", key="ps-btn-reset-view")
        fit.cls("dcs-btn dcs-btn--ghost dcs-btn--sm")
        fit.on_click(lambda: self.set_zoom(0.67))

    def _build_tool_options(self, v: ui.View) -> None:
        if self.tool in {"brush", "pencil", "eraser", "clone", "history", "dodge", "burn", "smudge", "blur"}:
            v.slider("Size", 24 if self.tool == "brush" else 40, 1, 400, key="ps-o-size")
            v.slider("Opacity", self.current_layer().opacity, 1, 100, key="ps-o-opacity").on_change(self.set_layer_opacity)
            v.dropdown("Mode", BLENDS, self.current_layer().blend, key="ps-o-mode").on_change(self.set_layer_blend)
        elif self.tool in {"marquee", "lasso", "wand"}:
            v.input("Feather", "0", type="number", key="ps-o-feather")
            v.checkbox("Anti-alias", True, key="ps-o-aa")
            if self.tool == "wand":
                v.slider("Tolerance", 32, 0, 128, key="ps-o-tolerance")
        elif self.tool == "crop":
            v.dropdown("Ratio", ["Free", "1:1", "4:3", "16:9"], "Free", key="ps-o-ratio")
            v.button("Apply", primary=True, key="ps-crop-apply").on_click(lambda: self.snapshot("Crop"))
        elif self.tool == "type":
            v.dropdown("Font", ["IBM Plex Sans", "JetBrains Mono", "Georgia"], "IBM Plex Sans", key="ps-type-font")
            v.input("Size", "64", type="number", key="ps-type-size")
            v.input("Text", "Decius", key="ps-type-text")
        elif self.tool == "zoom":
            v.button("100%", key="ps-zoom-100").on_click(lambda: self.set_zoom(1.0))
            v.button("Fit Screen", key="ps-zoom-fit").on_click(lambda: self.set_zoom(0.67))
        else:
            v.paragraph(_tool(self.tool).tip, classes="ps-opt-note", key="ps-opt-note")

    def _build_body(self, v: ui.View) -> None:
        v.container(classes="dcs-dock ps-doc-dock", key="ps-doc-dock", build=self._build_document_dock)
        v.container(classes="dcs-toolbar dcs-toolbar--v dcs-toolbar--floating ps-toolstrip", key="ps-toolstrip", build=self._build_toolstrip)
        if self.panels["navigator"]:
            self._floating_panel(v, "ps-navigator", "Navigator", "globe", "right:12px;top:12px;width:214px", self._build_navigator)
        if self.panels["color"]:
            self._floating_panel(v, "panel-color", "Color", "palette", "right:12px;top:226px;width:268px", self._build_color_panel)
        if self.panels["layers"]:
            self._floating_panel(v, "panel-layers", "Layers", "layers", "right:12px;top:486px;bottom:12px;width:300px", self._build_layers_panel)
        if self.panels["adjust"]:
            self._floating_panel(v, "panel-adjust", "Adjustments", "color-grade", "right:324px;top:12px;width:236px", self._build_adjustments)
        if self.panels["history"]:
            self._floating_panel(v, "panel-history", "History", "history-brush", "right:324px;top:152px;bottom:12px;width:236px", self._build_history)
        v.container(classes="dcs-toolbar dcs-toolbar--floating ps-floatbar", key="ps-floatbar", build=self._build_floatbar)

    def _floating_panel(
        self,
        v: ui.View,
        key: str,
        title: str,
        icon: str,
        style: str,
        build: Callable[[ui.View], None],
    ) -> None:
        def panel(p: ui.View) -> None:
            def header(h: ui.View) -> None:
                h.container(classes="ps-panel-title", key=f"{key}-title", build=lambda s: s.html(f'<i class="di di-{escape(icon)}"></i><span>{escape(title)}</span>'))
                h.container(classes="dcs-panel__tools", key=f"{key}-tools")

            p.container(classes="dcs-panel__header", key=f"{key}-header", build=header).attr("data-dcs-drag-handle", "")
            p.container(classes="dcs-panel__body", key=f"{key}-body", build=build)

        ref = v.container(classes="dcs-panel dcs-panel--floating ps-floating ps-float-panel", key=key, build=panel)
        ref.attr("style", style)

    def _build_document_dock(self, v: ui.View) -> None:
        def pane(p: ui.View) -> None:
            def tabbar(t: ui.View) -> None:
                t.container(classes="dcs-dockpane__tabs", key="ps-doc-tabs", build=lambda tabs: tabs.html(f'<div class="dcs-dockpane__tab" aria-selected="true"><i class="di di-image"></i> Untitled-1 @ {round(self.zoom * 100)}% (RGB/8)</div>'))
                t.container(classes="dcs-dockpane__toolbars", key="ps-doc-toolbars")

            p.container(classes="dcs-dockpane__tabbar", key="ps-doc-tabbar", build=tabbar)
            p.container(classes="dcs-dockpane__body", key="ps-doc-pane-body", build=self._build_stagewrap)

        v.container(classes="dcs-dockpane dcs-dockpane--center", key="ps-doc-center", build=pane)

    def _build_stagewrap(self, v: ui.View) -> None:
        v.container(classes="ps-stagewrap", key="ps-stagewrap", build=self._build_stage)

    def _build_stage(self, v: ui.View) -> None:
        v.container(classes="ps-ruler-corner", key="ps-ruler-corner")
        if self.show_rulers:
            v.container(classes="ps-ruler ps-ruler--h", key="ps-ruler-h", build=lambda r: r.html(self._ruler_ticks()))
            v.container(classes="ps-ruler ps-ruler--v", key="ps-ruler-v", build=lambda r: r.html(self._ruler_ticks()))
        stage = v.container(classes="ps-stage", key="ps-stage", build=self._build_document)
        stage.attr("role", "button")
        stage.on_click(lambda: self.snapshot(f"{_tool(self.tool).name} Action"))
        v.container(classes="ps-stage-badge ps-stage-badge--bl", key="ps-ov-cursor", build=lambda h: h.html('<span class="dcs-badge dcs-badge--soft">0, 0 px</span>'))
        v.container(classes="ps-stage-badge ps-stage-badge--tr", key="ps-ov-doc", build=lambda h: h.html(f'<span class="dcs-badge dcs-badge--soft">{self.doc.width()} x {self.doc.height()}</span>'))

    def _ruler_ticks(self) -> str:
        ticks = "".join(f"<span>{i}</span>" for i in range(0, 1300, 200))
        return f'<div class="ps-ruler-ticks">{ticks}</div>'

    def _build_document(self, v: ui.View) -> None:
        style = f"--z:{self.zoom};--px:{self.pan_x}px;--py:{self.pan_y}px"
        doc = v.container(classes="ps-doc", key="ps-doc", build=self._build_layers_on_canvas)
        doc.attr("style", style)

    def _build_layers_on_canvas(self, v: ui.View) -> None:
        for index, layer in enumerate(self.doc.layers()):
            if not layer.visible:
                continue
            classes = "ps-layer-canvas"
            if layer.id == "title":
                classes += " ps-layer-text"
            if layer.id == "vignette":
                classes += " ps-layer-vignette"
            opacity = layer.opacity / 100.0
            ref = v.container(classes=classes, key=f"canvas-{layer.id}", build=lambda h, layer=layer: h.html("Decius" if layer.id == "title" else ""))
            ref.attr("style", f"z-index:{index};opacity:{opacity:.2f};{layer.style}")
        if self.tool in {"marquee", "lasso", "crop"}:
            v.container(classes="ps-marquee", key="ps-marquee")
        if self.show_grid:
            v.container(classes="ps-grid-overlay", key="ps-grid-overlay")

    def _build_toolstrip(self, v: ui.View) -> None:
        v.container(classes="dcs-grip", key="ps-tools-grip").attr("data-dcs-drag-handle", "")
        def grid(g: ui.View) -> None:
            for tool in TOOLS:
                self._tool_button(g, tool)
                if tool.sep_after:
                    g.container(classes="ps-toolsep", key=f"sep-{tool.id}")
        v.container(classes="ps-toolgrid", key="ps-tools", build=grid)
        v.container(classes="ps-colorchips", key="ps-colorchips", build=self._build_color_chips)
        qm = v.container(classes="ps-tool", key="ps-quickmask", build=lambda h: h.html('<i class="di di-layer-mask"></i>'))
        qm.attr("role", "button").attr("title", "Edit in Quick Mask Mode (Q)")
        qm.attr("aria-pressed", "true" if self.quickmask else "false")
        qm.on_click(lambda: self._toggle_quickmask())

    def _tool_button(self, v: ui.View, tool: Tool) -> None:
        ref = v.container(classes="ps-tool", key=f"tool-{tool.id}", build=lambda h: h.html(f'<i class="di di-{escape(tool.icon)}"></i>'))
        ref.attr("role", "button")
        ref.attr("title", f"{tool.name} ({tool.key})")
        ref.attr("aria-pressed", "true" if self.tool == tool.id else "false")
        ref.attr("data-group", "true" if tool.group else "false")
        ref.on_click(lambda tool_id=tool.id: self.set_tool(tool_id))

    def _toggle_quickmask(self) -> None:
        self.quickmask = not self.quickmask
        self.status = "Quick Mask on" if self.quickmask else "Quick Mask off"
        self.reload()

    def _build_color_chips(self, v: ui.View) -> None:
        reset = v.container(classes="ps-chip-mini ps-reset", key="ps-color-reset", build=lambda h: h.html("D"))
        reset.attr("role", "button").on_click(self.reset_colors)
        swap = v.container(classes="ps-chip-mini ps-swap", key="ps-color-swap", build=lambda h: h.html("X"))
        swap.attr("role", "button").on_click(self.swap_colors)
        bg = v.container(classes="ps-colorchip ps-colorchip--bg", key="ps-chip-bg")
        bg.attr("style", f"background:{self.bg}")
        bg.attr("role", "button").on_click(lambda: self.open_dialog("Background Color", f"Current background: {self.bg}"))
        fg = v.container(classes="ps-colorchip ps-colorchip--fg", key="ps-chip-fg")
        fg.attr("style", f"background:{self.fg}")
        fg.attr("role", "button").on_click(lambda: self.open_dialog("Foreground Color", f"Current foreground: {self.fg}"))

    def _build_navigator(self, v: ui.View) -> None:
        v.container(classes="ps-colorpanel", key="ps-nav-panel", build=lambda p: (
            p.container(classes="ps-nav-thumb", key="ps-nav-thumb", build=lambda n: (
                n.container(classes="ps-nav-mini", key="ps-nav-mini"),
                n.container(classes="ps-nav-view", key="ps-nav-view"),
            )),
            p.slider("Zoom", self.zoom * 100, 5, 800, key="ps-nav-zoom").on_change(lambda value: self.set_zoom(_safe_num(value, 67.0) / 100.0)),
        ))

    def _build_color_panel(self, v: ui.View) -> None:
        v.container(classes="ps-colorpanel", key="ps-colorpanel", build=lambda p: (
            p.container(classes="ps-sv", key="ps-sv", build=lambda s: s.container(classes="ps-sv-dot", key="ps-sv-dot")).attr("style", f"--ps-fg:{self.fg}"),
            p.container(classes="ps-hue", key="ps-hue", build=lambda h: h.container(classes="ps-hue-dot", key="ps-hue-dot")),
            p.input("Hex", self.fg, key="ps-hex").on_change(self.set_foreground),
            p.container(classes="ps-swatches", key="ps-swatches", build=self._build_swatches),
        ))

    def _build_swatches(self, v: ui.View) -> None:
        for color in SWATCHES:
            swatch = v.container(classes="ps-swatch-chip", key=f"swatch-{color[1:]}")
            swatch.attr("style", f"background:{color}")
            swatch.attr("role", "button")
            swatch.attr("title", color)
            swatch.on_click(lambda color=color: self.set_foreground(color))

    def _build_layers_panel(self, v: ui.View) -> None:
        v.container(classes="ps-layers", key="ps-layers", build=lambda p: (
            p.container(classes="ps-layer-filter", key="ps-layer-filter", build=self._build_layer_filter),
            p.container(classes="ps-layer-bo", key="ps-layer-blend-opacity", build=self._build_layer_blend_opacity),
            p.container(classes="ps-layer-lock-row", key="ps-layer-lock-row", build=self._build_layer_locks),
            p.container(classes="ps-layer-list", key="ps-layer-list", build=self._build_layer_rows),
            p.container(classes="ps-layer-footer", key="ps-layer-footer", build=self._build_layer_footer),
        ))

    def _build_layer_filter(self, v: ui.View) -> None:
        v.dropdown("Kind", ["All", "Pixel", "Adjustment", "Type", "Shape"], "All", key="ps-layer-kind")
        for label, icon in (("Pixel", "image"), ("Adjust", "color-grade"), ("Type", "pencil"), ("Shape", "grid")):
            v.button(label, key=f"ps-filter-{label.lower()}").cls("dcs-btn dcs-btn--icon dcs-btn--sm").attr("title", label)

    def _build_layer_blend_opacity(self, v: ui.View) -> None:
        layer = self.current_layer()
        v.dropdown("Blend", BLENDS, layer.blend, key="ps-blend").on_change(self.set_layer_blend)
        v.slider("Opacity", layer.opacity, 0, 100, key="ps-opacity").on_change(self.set_layer_opacity)

    def _build_layer_locks(self, v: ui.View) -> None:
        v.container(classes="ps-amt-label", key="ps-lock-label", build=lambda h: h.html("Lock:"))
        for key, icon in (("transparency", "grid"), ("image", "brush"), ("position", "move"), ("all", "lock")):
            b = v.button(key, key=f"ps-lock-{key}")
            b.cls("dcs-btn dcs-btn--icon dcs-btn--sm")
            b.attr("title", f"Lock {key}")
            b.attr("aria-pressed", "true" if self.current_layer().locked and key == "all" else "false")
            if key == "all":
                b.on_click(lambda: self._toggle_lock())
        v.container(classes="ps-amt-label", key="ps-fill-label", build=lambda h: h.html("Fill:"))
        v.container(classes="ps-amt", key="ps-fill-amt", build=lambda h: h.html(f"{round(self.current_layer().fill)}%"))

    def _toggle_lock(self) -> None:
        self.doc.set_active_locked(not self.current_layer().locked)
        self.status = "Layer Lock"
        self.reload()

    def _build_layer_rows(self, v: ui.View) -> None:
        for layer in reversed(self.doc.layers()):
            self._layer_row(v, layer)

    def _layer_row(self, v: ui.View, layer: ui.PhotoLayerSnapshot) -> None:
        def row(r: ui.View) -> None:
            eye = r.container(classes="ps-layer-eye" + ("" if layer.visible else " is-off"), key=f"eye-{layer.id}", build=lambda h: h.html("o" if layer.visible else "-"))
            eye.attr("role", "button").on_click(lambda layer_id=layer.id: self.toggle_layer_visible(layer_id))
            r.container(classes="ps-layer-thumb", key=f"thumb-{layer.id}", build=lambda h: h.container(classes="ps-layer-thumb-fill", key=f"thumb-fill-{layer.id}").attr("style", layer.style))
            r.container(classes="ps-layer-name", key=f"name-{layer.id}", build=lambda h: h.html(escape(layer.name)))
            r.container(classes="ps-layer-lock", key=f"lock-{layer.id}", build=lambda h: h.html("lock" if layer.locked else ""))

        is_active = layer.id == self.doc.active_layer_id()
        cls = "ps-layer" + (" is-active" if is_active else "")
        ref = v.container(classes=cls, key=f"layer-{layer.id}", build=row)
        ref.attr("role", "button")
        ref.attr("aria-selected", "true" if is_active else "false")
        ref.on_click(lambda layer_id=layer.id: self.set_layer(layer_id))

    def _build_layer_footer(self, v: ui.View) -> None:
        for key, label, cb in (
            ("link", "Link", lambda: self.snapshot("Linked Layers")),
            ("fx", "fx", lambda: self.open_dialog("Layer Style", "Blending options and effects live here.")),
            ("mask", "Mask", lambda: self.snapshot("Layer Mask")),
            ("adjust", "Adjust", lambda: self.open_dialog("New Adjustment", "Create a fill or adjustment layer.")),
            ("group", "Group", lambda: self.snapshot("New Group")),
            ("new", "New", self.add_layer),
            ("dup", "Duplicate", self.duplicate_layer),
            ("del", "Delete", self.delete_layer),
        ):
            b = v.button(label, key=f"ps-l-{key}")
            b.cls("dcs-btn dcs-btn--icon dcs-btn--sm")
            b.attr("title", label)
            b.on_click(cb)

    def _build_adjustments(self, v: ui.View) -> None:
        def grid(g: ui.View) -> None:
            for icon, label, action in ADJUSTMENTS:
                b = g.container(classes="dcs-btn dcs-btn--icon dcs-btn--sm ps-adjust", key=f"adj-{action}", build=lambda h, icon=icon: h.html(f'<i class="di di-{escape(icon)}"></i>'))
                b.attr("role", "button").attr("title", label)
                b.on_click(lambda action=action: self.menu_action(action))

        v.container(classes="ps-adjust-grid", key="ps-adjust-grid", build=grid)

    def _build_history(self, v: ui.View) -> None:
        history_index = self.doc.history_index()
        for index, label in enumerate(self.doc.history()):
            item = v.container(
                classes="ps-history-item" + (" is-current" if index == history_index else ""),
                key=f"history-{index}",
                build=lambda h, label=label: h.html(f'<i class="di di-history-brush"></i><span>{escape(label)}</span>'),
            )
            item.attr("role", "button")
            item.on_click(lambda index=index: self.select_history(index))

    def _build_floatbar(self, v: ui.View) -> None:
        for key, label, cb in (
            ("undo", "Undo", lambda: self.menu_action("undo")),
            ("redo", "Redo", lambda: self.menu_action("redo")),
            ("zoomout", "-", lambda: self.set_zoom(self.zoom / 1.4)),
            ("fit", "Fit", lambda: self.set_zoom(0.67)),
            ("zoomin", "+", lambda: self.set_zoom(self.zoom * 1.4)),
        ):
            b = v.button(label, key=f"float-{key}")
            b.cls("dcs-btn dcs-btn--icon dcs-btn--sm")
            b.on_click(cb)

    def _build_statusbar(self, v: ui.View) -> None:
        v.input("", f"{round(self.zoom * 100)}%", key="ps-zoom-field").on_change(lambda value: self.set_zoom(_safe_num(value.rstrip("%"), 67.0) / 100.0))
        v.container(classes="dcs-statusbar__sep", key="ps-status-sep-a")
        v.container(classes="dcs-statusbar__item", key="ps-doc-size", build=lambda h: h.html("Doc: 3.91M / 11.7M"))
        v.container(classes="dcs-statusbar__sep", key="ps-status-sep-b")
        v.container(classes="dcs-statusbar__item", key="ps-status-tip", build=lambda h: h.html(escape(self.status)))
        v.container(classes="ps-status-spacer", key="ps-status-spacer")
        v.container(classes="dcs-statusbar__item dcs-statusbar__item--ok", key="ps-status-ready", build=lambda h: h.html('<i class="di di-check-circle"></i> Ready'))
        v.container(classes="dcs-statusbar__sep", key="ps-status-sep-c")
        v.container(classes="dcs-statusbar__item", key="ps-status-mode", build=lambda h: h.html("RGB/8 - 72 ppi"))

    def _build_menus(self, v: ui.View) -> None:
        for menu_id, _label in MENU_ORDER:
            def build_menu(menu_view: ui.View, menu_id: str = menu_id) -> None:
                for index, item in enumerate(MENUS[menu_id]):
                    if item is None:
                        menu_view.container(classes="dcs-menu__sep", key=f"{menu_id}-sep-{index}")
                        continue
                    action, label, shortcut, icon = item
                    html = (
                        f'<span class="dcs-menu__icon"><i class="di di-{escape(icon)}"></i></span>'
                        f'<span class="dcs-menu__label">{escape(label)}</span>'
                        f'<span class="dcs-menu__shortcut">{escape(shortcut)}</span>'
                    )
                    row = menu_view.container(classes="dcs-menu__item", key=f"{menu_id}-{action}", build=lambda h, html=html: h.html(html))
                    row.attr("role", "menuitem")
                    row.attr("data-dcs-value", action)

            menu = v.container(classes="dcs-menu", key=menu_id, build=build_menu)
            menu.attr("id", menu_id)
            menu.attr("hidden", "")
            menu.on_change(lambda action: self.menu_action(action))

    def _build_dialog(self, v: ui.View) -> None:
        title, body = self.dialog or ("Dialog", "")

        def panel(p: ui.View) -> None:
            p.container(
                classes="dcs-panel__header",
                key="ps-dialog-header",
                build=lambda h: h.html(f'<span class="dcs-panel__title">{escape(title)}</span>'),
            )

            def dialog_body(b: ui.View) -> None:
                b.paragraph(body, classes="ps-dialog-copy", key="ps-dialog-copy")
                if title == "Keyboard Shortcuts":
                    b.container(classes="ps-kbd-list", key="ps-kbd-list", build=self._build_shortcuts)
                elif title == "Theme Tweaks":
                    b.button_group("Density", ["compact", "comfortable", "spacious"], self.density, key="ps-theme-density").on_change(self._set_density)
                    b.button_group("Style", ["flat", "3d"], self.visual_style, key="ps-theme-style").on_change(self._set_style)
                    b.button_group("Accent", ["blue", "orange", "green", "purple", "teal"], self.accent, key="ps-theme-accent").on_change(self._set_accent)

            p.container(classes="dcs-panel__body", key="ps-dialog-body", build=dialog_body)
            p.container(classes="ps-dialog-actions", key="ps-dialog-actions", build=lambda a: (
                a.button("Cancel", key="ps-dialog-cancel").on_click(self.close_dialog),
                a.button("OK", primary=True, key="ps-dialog-ok").on_click(self.dialog_ok),
            ))

        v.container(classes="dcs-panel dcs-panel--floating ps-dialog", key="ps-dialog", build=panel)

    def _build_shortcuts(self, v: ui.View) -> None:
        for key, label in (
            ("V", "Move"),
            ("M", "Marquee"),
            ("B", "Brush"),
            ("E", "Eraser"),
            ("G", "Fill"),
            ("Z", "Zoom"),
            ("Ctrl+Z", "Undo"),
            ("Ctrl+0", "Fit on screen"),
        ):
            v.html(f"<p><kbd>{escape(key)}</kbd><span>{escape(label)}</span></p>", key=f"shortcut-{key}")

    def _set_density(self, value: str) -> None:
        if value in {"compact", "comfortable", "spacious"}:
            self.density = value
            self.reload()

    def _set_style(self, value: str) -> None:
        if value in {"flat", "3d"}:
            self.visual_style = value
            self.reload()

    def _set_accent(self, value: str) -> None:
        if value in {"blue", "orange", "green", "purple", "teal"}:
            self.accent = value
            self.reload()


def build_view() -> ui.View:
    return PhotoEditApp().build_view()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--headless", action="store_true", help="Build and lay out without opening a window.")
    parser.add_argument("--perf", action="store_true", help="Show the native performance overlay.")
    parser.add_argument(
        "--dpi",
        choices=("retina", "normal"),
        default="retina",
        help="Native window DPI mode.",
    )
    args = parser.parse_args()

    PhotoEditApp().launch(
        high_dpi=args.dpi == "retina",
        perf=args.perf,
        headless=args.headless,
    )


if __name__ == "__main__":
    main()
