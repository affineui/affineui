"""Python bindings for AffineUI."""

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
    WidgetRef,
    __version__,
    native_backend,
    version,
)


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
    "WidgetRef",
    "__version__",
    "document",
    "native_backend",
    "version",
]
