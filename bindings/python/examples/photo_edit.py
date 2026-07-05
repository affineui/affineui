"""Python Decius Photo Edit sample.

A structure/feature-parity port of the decius-css/samples/decius-photo web
app: same regions (menubar/options/float-host body/statusbar), the full
22-tool palette with per-tool options, floating Navigator / Color+Swatches /
Layers+Channels+Paths+Comps / Adjustments / History panels, all menus, and
the web's dialogs. The document/layer/history model is the C++ photo core
(affineui.PhotoDocument); undo/redo/history-jump genuinely restore state.

The implementation lives in the sibling ``photo_edit_app`` package; this
module is the entry point and keeps the public names (``PhotoEditApp``,
``PHOTO_CSS``) importable as before.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

EXAMPLE_DIR = Path(__file__).resolve().parent
if str(EXAMPLE_DIR) not in sys.path:
    sys.path.insert(0, str(EXAMPLE_DIR))

from photo_edit_app import PHOTO_CSS, PhotoEditApp  # noqa: E402

import affineui as ui  # noqa: E402

__all__ = ["PhotoEditApp", "PHOTO_CSS", "build_view", "main"]


def build_view() -> ui.View:
    return PhotoEditApp().build_view()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--headless",
        action="store_true",
        help="Build and lay out without opening a window.",
    )
    parser.add_argument(
        "--perf",
        action="store_true",
        help="Show the native performance overlay.",
    )
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
