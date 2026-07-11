"""Test-session bootstrap.

Guarantees ``import photo_core`` works regardless of how the tree was built.
photo_core is the Decius Photo Edit sample's C++ raster core — a top-level
extension module built alongside affineui. A clean editable/wheel install
exposes it on the path, but a partial or edited-in-place build can leave it
importable only from the build directory. It also shares process-global
pybind11 internals with affineui's _affineui, so the two must come from the
same build. Load photo_core by file path from the SAME directory _affineui
actually loaded from, guaranteeing an ABI match, if a plain import misses.
"""

import sys
from importlib import util
from pathlib import Path


def _ensure_photo_core() -> None:
    if "photo_core" in sys.modules:
        return
    try:
        import photo_core  # noqa: F401  (already importable — done)
        return
    except ModuleNotFoundError:
        pass

    import affineui  # noqa: F401  (loads _affineui via its own locator)
    ext = sys.modules.get("affineui._affineui")

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

    # ONLY from the exact directory _affineui loaded from. The two modules
    # share process-global pybind11 state and must come from the SAME build —
    # pulling photo_core from anywhere else (e.g. the build tree while
    # _affineui came from site-packages) segfaults when a type crosses the
    # boundary. If it isn't there, let the ImportError surface.
    if ext is not None and getattr(ext, "__file__", None):
        _load_from(Path(ext.__file__).resolve().parent)


_ensure_photo_core()
