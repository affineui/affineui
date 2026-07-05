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

from .app import PhotoEditApp
from .styles import PHOTO_CSS

__all__ = ["PhotoEditApp", "PHOTO_CSS"]
