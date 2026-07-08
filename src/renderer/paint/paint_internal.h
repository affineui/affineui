#pragma once

#include "affineui/painter.h"

#include <memory>

// Forward declaration so callers don't have to drag nanovg.h in.
struct NVGcontext;

namespace affineui::detail {

/// Construct a Painter that draws into the given NanoVG context.
/// The painter holds a non-owning pointer to `vg`; the caller is
/// responsible for creating it (via `nvgCreateGL3`) and destroying it
/// (via `nvgDeleteGL3`) around the painter's lifetime.
std::unique_ptr<Painter> make_nanovg_painter(NVGcontext* vg);

/// Register a font with the painter's NanoVG context. Returns the
/// internal font handle (suitable for `Painter::resolve_font` result)
/// or 0 on failure.
std::uint32_t register_font_file(NVGcontext* vg,
                                 const char* family,
                                 const char* ttf_path);

/// Try to load a default sans-serif from a list of platform-typical
/// font paths. Returns the family name that succeeded, or empty
/// string view on miss. Mutates the NanoVG context as a side effect.
std::string_view register_default_font(NVGcontext* vg);

}  // namespace affineui::detail
