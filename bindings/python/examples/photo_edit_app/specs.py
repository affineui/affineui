"""Static data specs for the Decius Photo Edit sample.

Everything in this module mirrors the web reference app
(decius-css/samples/decius-photo): tool palette, per-tool option defaults,
menu trees, swatches, adjustments, blend-mode tables, and the static panel
content (channels/comps/shortcuts).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from html import escape


# ── Tools ────────────────────────────────────────────────────────────────────

# The web uses three custom inline SVG glyphs (PS.SVG) where no di-* icon
# exists: the type "T", the shape rect, and the gradient swatch.
SVG_TYPE = (
    '<svg viewBox="0 0 16 16" width="15" height="15">'
    '<path fill="currentColor" d="M3 3h10v3.2h-1.3l-.4-1.7H8.9v9l1.7.4V15H5.4'
    'v-1.1l1.7-.4v-9H4.7l-.4 1.7H3z"/></svg>'
)
SVG_SHAPE = (
    '<svg viewBox="0 0 16 16" width="15" height="15">'
    '<rect x="2.75" y="3.75" width="10.5" height="8.5" rx="2" fill="none" '
    'stroke="currentColor" stroke-width="1.5"/></svg>'
)
SVG_GRADIENT = (
    '<svg viewBox="0 0 16 16" width="15" height="15">'
    '<defs><linearGradient id="ps-svg-grad" x1="0" y1="0" x2="1" y2="0">'
    '<stop offset="0" stop-color="currentColor"/>'
    '<stop offset="1" stop-color="currentColor" stop-opacity="0"/>'
    "</linearGradient></defs>"
    '<rect x="2" y="4" width="12" height="8" rx="1.5" '
    'fill="url(#ps-svg-grad)"/></svg>'
)


@dataclass(frozen=True)
class Tool:
    id: str
    name: str
    key: str
    icon: str  # di-* icon name, or "" when svg is set
    tip: str
    group: bool = False  # corner triangle (has hidden siblings in real PS)
    paint: bool = False
    sep_after: bool = False
    svg: str = ""  # custom inline SVG glyph


TOOLS: tuple[Tool, ...] = (
    Tool("move", "Move Tool", "V", "move",
         "Move: drag to reposition the active layer."),
    Tool("marquee", "Rectangular Marquee", "M", "marquee",
         "Marquee: drag to make a rectangular selection.", group=True),
    Tool("lasso", "Lasso Tool", "L", "lasso",
         "Lasso: drag to draw a freehand selection.", group=True),
    Tool("wand", "Object Selection", "W", "select",
         "Magic Wand: click to select a similar-color region.", group=True,
         sep_after=True),
    Tool("crop", "Crop Tool", "C", "clip",
         "Crop: drag to trim the document to a region."),
    Tool("eyedropper", "Eyedropper", "I", "eyedropper",
         "Eyedropper: click to sample a color as the foreground.",
         sep_after=True),
    Tool("brush", "Brush Tool", "B", "brush",
         "Brush: drag on the canvas to paint with the foreground color.",
         group=True, paint=True),
    Tool("pencil", "Pencil Tool", "N", "pencil",
         "Pencil: hard-edged freehand strokes.", paint=True),
    Tool("clone", "Clone Stamp", "S", "stamp",
         "Clone Stamp: Alt-click to set a source, then paint to copy pixels.",
         group=True, paint=True),
    Tool("history", "History Brush", "Y", "history-brush",
         "History Brush: paint to restore from the history source state.",
         paint=True),
    Tool("eraser", "Eraser Tool", "E", "eraser",
         "Eraser: drag to erase pixels on the active layer.", group=True,
         paint=True),
    Tool("fill", "Paint Bucket", "G", "fill",
         "Paint Bucket: click to flood-fill a region with the foreground "
         "color.", group=True),
    Tool("gradient", "Gradient Tool", "G", "",
         "Gradient: drag to draw a foreground→transparent gradient.",
         sep_after=True, svg=SVG_GRADIENT),
    Tool("dodge", "Dodge Tool", "O", "dodge",
         "Dodge: paint to lighten pixels.", group=True, paint=True),
    Tool("burn", "Burn Tool", "O", "burn",
         "Burn: paint to darken pixels.", paint=True),
    Tool("smudge", "Smudge Tool", "R", "smudge",
         "Smudge: drag to push pixels around.", paint=True),
    Tool("blur", "Blur Tool", "R", "blur",
         "Blur: paint to soften detail.", paint=True, sep_after=True),
    Tool("pen", "Pen Tool", "P", "pen",
         "Pen: click to add anchor points and build a path."),
    Tool("type", "Horizontal Type", "T", "",
         "Type: click to add a text layer.", svg=SVG_TYPE),
    Tool("shape", "Rectangle Tool", "U", "",
         "Shape: drag to draw a filled rectangle on a new layer.",
         sep_after=True, svg=SVG_SHAPE),
    Tool("hand", "Hand Tool", "H", "pan",
         "Hand: drag to pan the document."),
    Tool("zoom", "Zoom Tool", "Z", "zoom-in",
         "Zoom: click to zoom in, Alt-click to zoom out."),
)

TOOL_BY_ID: dict[str, Tool] = {tool.id: tool for tool in TOOLS}


def tool_icon_html(tool: Tool) -> str:
    if tool.svg:
        return f'<span class="ps-svg">{tool.svg}</span>'
    return f'<i class="di di-{escape(tool.icon)}"></i>'


# Per-tool options-bar state defaults (web PS.OPTIONS ranges/defaults).
TOOL_DEFAULTS: dict[str, dict[str, object]] = {
    "brush": {"size": 24, "hardness": 70, "opacity": 100, "flow": 100,
              "mode": "Normal"},
    "pencil": {"size": 4, "opacity": 100, "mode": "Normal"},
    "eraser": {"size": 30, "hardness": 50, "opacity": 100},
    "clone": {"size": 40, "hardness": 60, "opacity": 100},
    "history": {"size": 40, "opacity": 100},
    "dodge": {"size": 60, "exposure": 40},
    "burn": {"size": 60, "exposure": 40},
    "smudge": {"size": 30, "strength": 50},
    "blur": {"size": 40, "strength": 50},
    "fill": {"tolerance": 32, "opacity": 100, "contiguous": True},
    "gradient": {"opacity": 100},
    "marquee": {"mode": "New", "feather": 0},
    "lasso": {"feather": 0, "antialias": True},
    "wand": {"tolerance": 32, "contiguous": True},
    "move": {"autoselect": True},
    "crop": {"ratio": "Free"},
    "type": {"font": "IBM Plex Sans", "size": 64, "text": "Decius"},
    "shape": {"fill": "", "radius": 8},  # fill "" = current foreground
    "pen": {},
    "hand": {},
    "zoom": {},
}

# Web size-slider ranges differ per tool.
TOOL_SIZE_RANGE: dict[str, tuple[int, int]] = {
    "brush": (1, 400), "pencil": (1, 200), "eraser": (1, 400),
    "clone": (1, 400), "history": (1, 400), "dodge": (1, 400),
    "burn": (1, 400), "smudge": (1, 300), "blur": (1, 300),
}


# ── Blend modes ──────────────────────────────────────────────────────────────

# Layers-panel blend select (15 entries, web #ps-blend).
LAYER_BLENDS: tuple[str, ...] = (
    "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten",
    "Color Dodge", "Color Burn", "Hard Light", "Soft Light", "Difference",
    "Hue", "Saturation", "Color", "Luminosity",
)

# Options-bar paint-mode select (9 entries, web modeSelect).
TOOL_BLENDS: tuple[str, ...] = (
    "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten",
    "Dodge", "Burn", "Difference",
)

# Dialog blend select (13 entries, web blendOptions()).
DIALOG_BLENDS: tuple[str, ...] = (
    "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten",
    "Color Dodge", "Color Burn", "Difference", "Hue", "Saturation", "Color",
    "Luminosity",
)

# Blend name → CSS mix-blend-mode value used when compositing layer divs.
BLEND_TO_CSS: dict[str, str] = {
    "Normal": "normal", "Multiply": "multiply", "Screen": "screen",
    "Overlay": "overlay", "Darken": "darken", "Lighten": "lighten",
    "Color Dodge": "color-dodge", "Dodge": "color-dodge",
    "Color Burn": "color-burn", "Burn": "color-burn",
    "Hard Light": "hard-light", "Soft Light": "soft-light",
    "Difference": "difference", "Hue": "hue", "Saturation": "saturation",
    "Color": "color", "Luminosity": "luminosity",
}


# ── Menus ────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class MenuItem:
    action: str
    label: str
    shortcut: str = ""
    icon: str = ""
    # State key rendered as a checkmark: "rulers"|"grid"|"snap"|"panel:<id>".
    checked: str = ""


@dataclass(frozen=True)
class Menu:
    id: str
    label: str
    items: tuple[MenuItem | None, ...] = field(default_factory=tuple)


MENUS: tuple[Menu, ...] = (
    Menu("m-file", "File", (
        MenuItem("new", "New…", "Ctrl+N", "file"),
        MenuItem("open", "Open Sample…", "Ctrl+O", "folder-open"),
        MenuItem("place", "Place Embedded…", "", "import"),
        None,
        MenuItem("save", "Save", "Ctrl+S", "save"),
        MenuItem("export", "Export As…", "Shift+Ctrl+E", "export"),
        None,
        MenuItem("close", "Close", "Ctrl+W", "close"),
    )),
    Menu("m-edit", "Edit", (
        MenuItem("undo", "Undo", "Ctrl+Z", "undo"),
        MenuItem("redo", "Redo", "Shift+Ctrl+Z", "redo"),
        None,
        MenuItem("cut", "Cut", "Ctrl+X", "cut"),
        MenuItem("copy", "Copy", "Ctrl+C", "copy"),
        MenuItem("paste", "Paste", "Ctrl+V", "paste"),
        None,
        MenuItem("fill", "Fill with Foreground", "Alt+Backspace", "fill"),
        MenuItem("stroke", "Stroke…", "", "edit"),
        MenuItem("transform", "Free Transform", "Ctrl+T", "scale-corners"),
    )),
    Menu("m-image", "Image", (
        MenuItem("bc", "Brightness/Contrast…", "", "light"),
        MenuItem("hsl", "Hue/Saturation…", "Ctrl+U", "color-grade"),
        MenuItem("levels", "Levels…", "Ctrl+L", "graph"),
        MenuItem("invert", "Invert", "Ctrl+I", "mirror"),
        MenuItem("desat", "Desaturate", "Shift+Ctrl+U", "mono"),
        None,
        MenuItem("size", "Image Size…", "Alt+Ctrl+I", "aspect"),
        MenuItem("canvas", "Canvas Size…", "Alt+Ctrl+C", "fit"),
        MenuItem("flatten", "Flatten Image", "", "layers"),
    )),
    Menu("m-layer", "Layer", (
        # "lnewdlg" opens the New Layer dialog (web parity); the plain
        # "lnew" action remains the direct programmatic add used by the
        # layers-panel footer button.
        MenuItem("lnewdlg", "New Layer", "Shift+Ctrl+N", "plus"),
        MenuItem("ldup", "Duplicate Layer", "Ctrl+J", "duplicate"),
        MenuItem("ldel", "Delete Layer", "", "trash"),
        None,
        MenuItem("lgroup", "Group Layers", "Ctrl+G", "folder"),
        MenuItem("lmask", "Add Layer Mask", "", "layer-mask"),
        None,
        MenuItem("lup", "Bring Forward", "Ctrl+]", "chevron-up"),
        MenuItem("ldown", "Send Backward", "Ctrl+[", "chevron-down"),
        MenuItem("lmerge", "Merge Down", "Ctrl+E", "compress"),
    )),
    Menu("m-select", "Select", (
        MenuItem("sall", "All", "Ctrl+A", "marquee"),
        MenuItem("sdesel", "Deselect", "Ctrl+D", "close"),
        MenuItem("sinv", "Inverse", "Shift+Ctrl+I", "mirror"),
        None,
        MenuItem("sfeather", "Feather…", "Shift+F6", "blur"),
        MenuItem("sgrow", "Grow", "", "plus"),
    )),
    Menu("m-filter", "Filter", (
        MenuItem("fblur", "Gaussian Blur…", "", "blur"),
        MenuItem("fsharp", "Sharpen", "", "sharpen"),
        MenuItem("fnoise", "Add Noise…", "", "wave-noise"),
        MenuItem("fpix", "Pixelate", "", "grid"),
        None,
        MenuItem("femboss", "Emboss", "", "extrude"),
        MenuItem("ffind", "Find Edges", "", "cross-target"),
    )),
    Menu("m-view", "View", (
        MenuItem("vin", "Zoom In", "Ctrl++", "zoom-in"),
        MenuItem("vout", "Zoom Out", "Ctrl+-", "zoom-out"),
        MenuItem("vfit", "Fit on Screen", "Ctrl+0", "fit"),
        MenuItem("v100", "100%", "Ctrl+1", "cross-target"),
        None,
        MenuItem("vrulers", "Rulers", "Ctrl+R", "aspect", checked="rulers"),
        MenuItem("vgrid", "Show Grid", "Ctrl+'", "grid", checked="grid"),
        MenuItem("vsnap", "Snap", "", "magnet", checked="snap"),
    )),
    Menu("m-window", "Window", (
        MenuItem("wlayers", "Layers", "F7", "layers", checked="panel:layers"),
        MenuItem("wcolor", "Color", "F6", "palette", checked="panel:color"),
        MenuItem("whistory", "History", "", "history-brush",
                 checked="panel:history"),
        MenuItem("wadjust", "Adjustments", "", "color-grade",
                 checked="panel:adjust"),
        None,
        MenuItem("wreset", "Reset Workspace", "", "grid"),
    )),
    Menu("m-help", "Help", (
        MenuItem("habout", "About Decius PhotoEditor", "", "info"),
        MenuItem("hframework", "About decius.css", "", "decius"),
        MenuItem("hkeys", "Keyboard Shortcuts", "", "keys"),
    )),
)


# ── Palette / panels data ────────────────────────────────────────────────────

SWATCHES: tuple[str, ...] = (
    "#000000", "#404040", "#7f7f7f", "#bcbcbc", "#ffffff",
    "#ff2d2d", "#ff7a2d", "#ffd02d", "#7bd92d", "#2dd4bf",
    "#2d8cff", "#4d9fff", "#6a5cff", "#b48cff", "#ff7ab8",
    "#7a3b12", "#c0894a", "#e9cfa0", "#1f3b6e", "#0e2233",
    "#e7e9ee", "#aab0bd", "#767c8a", "#3c424f", "#181b22",
    "#5b1a1a", "#1a5b2e", "#1a3a5b", "#5b491a", "#3a1a5b",
)

# Adjustments panel: (icon, label, action) — 12 buttons, 6-col grid.
ADJUSTMENTS: tuple[tuple[str, str, str], ...] = (
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

# Channels tab rows: (label, shortcut, selected).
CHANNELS: tuple[tuple[str, str, bool], ...] = (
    ("RGB", "Ctrl+2", True),
    ("Red", "Ctrl+3", False),
    ("Green", "Ctrl+4", False),
    ("Blue", "Ctrl+5", False),
)

# Comps tab rows: (icon, label, meta, active).
COMPS: tuple[tuple[str, str, str, bool], ...] = (
    ("cross-target", "Last Document State", "", True),
    ("filmstrip", "Hero — sunrise", "V P A", False),
    ("filmstrip", "Hero — dusk", "V P A", False),
    ("filmstrip", "No title text", "V · ·", False),
)

# Keyboard Shortcuts dialog (web dlgShortcuts).
SHORTCUTS: tuple[tuple[str, str], ...] = (
    ("V", "Move"), ("M", "Marquee"), ("L", "Lasso"), ("B", "Brush"),
    ("E", "Eraser"), ("G", "Bucket"), ("S", "Clone"), ("I", "Eyedropper"),
    ("Z", "Zoom"), ("H", "Hand"), ("Ctrl+Z", "Undo"),
    ("Shift+Ctrl+Z", "Redo"), ("Ctrl+A", "Select All"),
    ("Ctrl+D", "Deselect"), ("Ctrl+0", "Fit"), ("Ctrl+1", "100%"),
    ("X", "Swap colors"), ("D", "Default colors"),
    ("[ / ]", "Layer down/up"), ("Backspace", "Clear selection"),
)

# New Document dialog presets: label → (width, height).
DOC_PRESETS: dict[str, tuple[int, int]] = {
    "Custom": (1280, 800),
    "Web 1920×1080": (1920, 1080),
    "Square 2048": (2048, 2048),
    "Postcard 1748×1240": (1748, 1240),
    "Phone 1170×2532": (1170, 2532),
}

# Place Embedded assets: label → (style, blend, opacity).
PLACE_ASSETS: dict[str, tuple[str, str, float]] = {
    "Sun flare": (
        "background:radial-gradient(circle at 32% 30%,#ffffff 0%,"
        "rgba(255,226,160,.8) 8%,rgba(255,190,110,.35) 22%,transparent 55%)",
        "Screen", 100.0,
    ),
    "Gradient map": (
        "background:linear-gradient(135deg,#1f6feb,#ff7ab8)",
        "Soft Light", 60.0,
    ),
    "Noise texture": (
        # CSS approximation of gray noise (real per-pixel noise is Phase-B).
        "background:repeating-conic-gradient(#8a8a8a 0% 25%,#767676 0% 50%);"
        "background-size:3px 3px",
        "Overlay", 40.0,
    ),
    "Vignette": (
        "background:radial-gradient(circle,rgba(0,0,0,0) 42%,"
        "rgba(0,0,0,.75) 100%)",
        "Multiply", 100.0,
    ),
}
