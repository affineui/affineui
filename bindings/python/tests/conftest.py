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
    if ext is None or not getattr(ext, "__file__", None):
        return  # let the real ImportError surface in the test module
    ext_dir = Path(ext.__file__).resolve().parent
    for cand in sorted(ext_dir.glob("photo_core*"),
                       key=lambda p: p.stat().st_mtime, reverse=True):
        if cand.suffix.lower() not in (".pyd", ".so"):
            continue
        spec = util.spec_from_file_location("photo_core", cand)
        if spec is None or spec.loader is None:
            continue
        module = util.module_from_spec(spec)
        sys.modules["photo_core"] = module
        spec.loader.exec_module(module)
        return


_ensure_photo_core()
