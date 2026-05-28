"""Python bindings for AffineUI."""

from importlib import machinery, util
from pathlib import Path
import sys


def _prefer_local_extension() -> None:
    module_name = __name__ + "._affineui"
    if module_name in sys.modules:
        return

    package_dir = Path(__file__).resolve().parent
    candidates = []
    for suffix in machinery.EXTENSION_SUFFIXES:
        candidate = package_dir / ("_affineui" + suffix)
        if candidate.exists():
            candidates.append(candidate)

    editable_build = package_dir.parent.parent / "build"
    if editable_build.exists():
        candidates.extend(editable_build.glob("*/Release/_affineui*.pyd"))
        candidates.extend(editable_build.glob("*/Debug/_affineui*.pyd"))
        candidates.extend(editable_build.glob("*/RelWithDebInfo/_affineui*.pyd"))

    candidates = sorted(
        set(candidates),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    for candidate in candidates:
        spec = util.spec_from_file_location(module_name, candidate)
        if spec is None or spec.loader is None:
            continue
        module = util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
        return


_prefer_local_extension()

from ._affineui import (
    App,
    Color,
    Document,
    RemotePatch,
    RemotePatchOp,
    RemotePatchQueue,
    Rect,
    Size,
    View,
    ViewTheme,
    VirtualListOptions,
    WidgetRef,
    __version__,
    native_backend,
    version,
)


class bootstrap:
    """Bootstrap command-tree selector constants."""

    class selector:
        size = "size"
        theme = "theme"

    class size:
        sm = "sm"
        med = "md"
        medium = "md"
        md = "md"
        lg = "lg"

    class theme:
        light = "light"
        dark = "dark"

    class class_name:
        form = "aui-bs-form"
        props = "aui-bs-props"
        column = "aui-bs-col"
        row = "aui-bs-row"
        note = "form-text"
        button_row = "aui-bs-btn-row"
        list = "list-group"
        list_item = "list-group-item"
        tree = "list-group"
        tree_row = "list-group-item"
        table = "table"


class decius:
    """Decius command-tree selector constants."""

    class selector:
        size = "size"
        style = "style"
        density = "density"
        accent = "accent"
        radius = "radius"
        dark = "dark"

    class size:
        sm = "sm"
        med = "md"
        medium = "md"
        md = "md"
        lg = "lg"

    class style:
        flat = "flat"
        three_d = "3d"

    class density:
        compact = "compact"
        comfortable = "comfortable"
        spacious = "spacious"

    class class_name:
        form = "dcs-form"
        props = "dcs-props"
        column = "dcs-col"
        row = "dcs-row"
        note = "dcs-note"
        button_row = "dcs-btn-row"
        list = "dcs-list"
        list_item = "dcs-list__item"
        tree = "dcs-tree"
        tree_row = "dcs-tree__row"
        table = "dcs-table"


def document(html: str = "", css: str = "", *, width: int = 1024, height: int = 0) -> Document:
    """Create a headless document and optionally lay it out.

    This helper is intentionally small. It gives tests and Python users a
    quick way to exercise the retained HTML/CSS pipeline before opening a
    native window.
    """

    doc = Document()
    if css:
        doc.set_user_stylesheet(css)
    if html:
        doc.set_html(html)
    if width > 0:
        doc.layout(width, height)
    return doc


__all__ = [
    "App",
    "Color",
    "Document",
    "RemotePatch",
    "RemotePatchOp",
    "RemotePatchQueue",
    "Rect",
    "Size",
    "View",
    "ViewTheme",
    "VirtualListOptions",
    "WidgetRef",
    "__version__",
    "bootstrap",
    "decius",
    "document",
    "native_backend",
    "version",
]
