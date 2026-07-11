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
    Axis,
    Button,
    Checkbox,
    Color,
    ColorField,
    ComponentValidity,
    Dock,
    DockCorner,
    DockHandle,
    DockLayout,
    DockLocation,
    DockPanel,
    DockPlacement,
    DomHandle,
    Document,
    DispatchResult,
    DocumentScript,
    DropPos,
    Dropdown,
    Event,
    EventType,
    Foldout,
    HoverInfo,
    IndexSelection,
    Key,
    MouseButton,
    Point,
    RemotePatch,
    RemotePatchOp,
    RemotePatchQueue,
    Rect,
    SelectMod,
    Size,
    Slider,
    TextField,
    View,
    ViewTheme,
    VirtualListOptions,
    VirtualListProvider,
    VirtualTreeProvider,
    WidgetKind,
    WidgetRef,
    __version__,
    native_backend,
    tools_active,
    tools_listen,
    tools_port,
    tools_shutdown,
    version,
)


class ElementRef:
    """Safe id-based reference to a native AffineUI document element.

    ElementRef intentionally does not expose a raw DOM pointer. Every method
    resolves through the owning Document and returns False if the element no
    longer exists, so stale refs have empty semantics instead of dangling.
    """

    def __init__(self, document: Document, elem_id: str) -> None:
        self._document = document
        self.id = elem_id

    def handle(self) -> DomHandle:
        """Return the current weak DOM handle, or an empty handle."""

        return self._document.weak_handle_for_id(self.id)

    def valid(self) -> bool:
        """Return True if this element id resolves in the current document."""

        handle = self.handle()
        return bool(handle) and self._document.weak_handle_valid(handle)

    def set_attr(self, name: str, value: str) -> bool:
        """Set an attribute if the element exists."""

        return self._document.set_attribute_by_id(self.id, name, value)

    def remove_attr(self, name: str) -> bool:
        """Remove an attribute if the element exists."""

        return self._document.remove_attribute_by_id(self.id, name)

    def set_style(self, css_text: str) -> bool:
        """Replace the inline style if the element exists."""

        return self.set_attr("style", css_text)

    def clear_style(self) -> bool:
        """Remove the inline style if the element exists."""

        return self.remove_attr("style")

    def set_text(self, text: str) -> bool:
        """Set leaf text if the element exists and can accept text."""

        return self._document.set_text_by_id(self.id, text)

    def __bool__(self) -> bool:
        return self.valid()


def element(document: Document, elem_id: str) -> ElementRef:
    """Create a safe id-based ElementRef for direct document mutation."""

    return ElementRef(document, elem_id)


def _document_element(self: Document, elem_id: str) -> ElementRef:
    return ElementRef(self, elem_id)


Document.element = _document_element


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
    "Button",
    "Checkbox",
    "Color",
    "ColorField",
    "ComponentValidity",
    "Dock",
    "DockCorner",
    "DockHandle",
    "DockLayout",
    "DockLocation",
    "DockPanel",
    "DockPlacement",
    "DomHandle",
    "Document",
    "DispatchResult",
    "DocumentScript",
    "Dropdown",
    "ElementRef",
    "Event",
    "EventType",
    "Foldout",
    "HoverInfo",
    "Key",
    "MouseButton",
    "Point",
    "RemotePatch",
    "RemotePatchOp",
    "RemotePatchQueue",
    "Rect",
    "Axis",
    "DropPos",
    "IndexSelection",
    "SelectMod",
    "Size",
    "Slider",
    "TextField",
    "View",
    "ViewTheme",
    "VirtualListOptions",
    "VirtualListProvider",
    "VirtualTreeProvider",
    "WidgetKind",
    "WidgetRef",
    "__version__",
    "bootstrap",
    "decius",
    "document",
    "element",
    "native_backend",
    "tools_active",
    "tools_listen",
    "tools_port",
    "tools_shutdown",
    "version",
]
