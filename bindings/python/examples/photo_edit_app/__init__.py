"""Modular Python port of the Decius Photo Edit sample.

Modules:
    specs    — static data (tools, menus, blends, swatches, panel content)
    colors   — hex/RGB/HSV math
    styles   — app stylesheet (PHOTO_CSS)
    options  — per-tool options-bar builders
    stage    — document dock, rulers, CSS-layer canvas
    panels   — tool strip + floating panels
    dialogs  — modal dialog framework + all dialogs
    app      — PhotoEditApp state and wiring
"""


def _ensure_photo_core() -> None:
    """Make ``import photo_core`` work no matter how the tree was built.

    ``photo_core`` is this sample's C++ raster core — a top-level extension
    module built alongside affineui. A scikit-build *editable* install exposes
    it via its finder, but a plain build (or a partial/edited-in-place tree)
    may leave it importable only from the build directory. Rather than depend
    on install state, locate the built .pyd and load it directly.

    CRITICAL: photo_core and affineui's _affineui are two pybind11 modules that
    share process-global pybind internals — they MUST come from the same build
    (mismatched builds crash when a type crosses the boundary, e.g. attach()).
    So we load photo_core from the SAME directory _affineui actually loaded
    from, guaranteeing an ABI match. A normal ``import photo_core`` that
    already resolves to that same build short-circuits this.
    """
    import sys
    if "photo_core" in sys.modules:
        return

    from importlib import util
    from pathlib import Path

    # The directory _affineui was actually loaded from is the source of truth
    # for which build is live; photo_core must match it byte-for-byte-compatibly.
    import affineui  # noqa: F401  (triggers affineui's own extension location)
    affineui_ext = sys.modules.get("affineui._affineui")
    ext_dir = None
    if affineui_ext is not None and getattr(affineui_ext, "__file__", None):
        ext_dir = Path(affineui_ext.__file__).resolve().parent

    def _load_from(directory: Path) -> bool:
        if not directory or not directory.is_dir():
            return False
        for cand in sorted(directory.glob("photo_core*"),
                           key=lambda p: p.stat().st_mtime, reverse=True):
            if cand.suffix.lower() not in (".pyd", ".so"):
                continue
            spec = util.spec_from_file_location("photo_core", cand)
            if spec is None or spec.loader is None:
                continue
            module = util.module_from_spec(spec)
            sys.modules["photo_core"] = module
            spec.loader.exec_module(module)
            return True
        return False

    # Search, in order: (1) the exact dir _affineui loaded from — the surest
    # ABI match; (2) the editable build tree's config dirs; (3) a plain import.
    # …/photo_edit_app → …/examples → …/bindings/python
    py_root = Path(__file__).resolve().parents[2]
    search_dirs = [ext_dir] if ext_dir is not None else []
    build = py_root / "build"
    if build.is_dir():
        for cfg in ("Release", "RelWithDebInfo", "Debug"):
            search_dirs.extend(build.glob(f"*/{cfg}"))
        search_dirs.extend(build.glob("*"))  # per-config-less generators
    for d in search_dirs:
        if _load_from(d):
            return
    try:
        import photo_core  # noqa: F401
        return
    except ModuleNotFoundError:
        pass
    raise ModuleNotFoundError(
        "No module named 'photo_core'. Build it with:\n"
        "    cd bindings/python && pip install -e . --no-build-isolation\n"
        "(the Decius Photo Edit sample's C++ raster core).")


_ensure_photo_core()

from .app import PhotoEditApp
from .styles import PHOTO_CSS

__all__ = ["PhotoEditApp", "PHOTO_CSS"]
