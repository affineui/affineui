"""Color math helpers (hex/RGB/HSV) mirroring the web app's ui.js utilities."""

from __future__ import annotations

import colorsys
import re

_HEX_RE = re.compile(r"^#?([0-9a-fA-F]{3}|[0-9a-fA-F]{6})$")
_CSS_COLOR_RE = re.compile(r"#[0-9a-fA-F]{6}|#[0-9a-fA-F]{3}|rgba?\([^)]*\)")


def normalize_hex(value: str) -> str | None:
    """Return canonical '#rrggbb' for a 3/6-digit hex string, else None."""
    match = _HEX_RE.match(value.strip())
    if match is None:
        return None
    digits = match.group(1).lower()
    if len(digits) == 3:
        digits = "".join(c * 2 for c in digits)
    return "#" + digits


def hex_to_rgb(value: str) -> tuple[int, int, int]:
    normalized = normalize_hex(value) or "#000000"
    return (int(normalized[1:3], 16), int(normalized[3:5], 16),
            int(normalized[5:7], 16))


def rgb_to_hex(r: int, g: int, b: int) -> str:
    clamp = lambda c: max(0, min(255, int(round(c))))  # noqa: E731
    return f"#{clamp(r):02x}{clamp(g):02x}{clamp(b):02x}"


def hex_to_rgba(value: str, alpha: float) -> str:
    r, g, b = hex_to_rgb(value)
    return f"rgba({r},{g},{b},{max(0.0, min(1.0, alpha)):.3f})"


def hex_to_hsv(value: str) -> tuple[float, float, float]:
    """Return (h 0..360, s 0..1, v 0..1)."""
    r, g, b = hex_to_rgb(value)
    h, s, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
    return (h * 360.0, s, v)


def hsv_to_hex(h: float, s: float, v: float) -> str:
    r, g, b = colorsys.hsv_to_rgb((h % 360.0) / 360.0, max(0.0, min(1.0, s)),
                                  max(0.0, min(1.0, v)))
    return rgb_to_hex(r * 255.0, g * 255.0, b * 255.0)


def first_color_in(css: str) -> str | None:
    """First color literal in a CSS string (eyedropper approximation)."""
    match = _CSS_COLOR_RE.search(css)
    return match.group(0) if match is not None else None
