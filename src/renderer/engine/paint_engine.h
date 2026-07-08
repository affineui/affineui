#pragma once

// `PaintEngine` — emits display lists from a laid-out box tree.
//
// Inputs:
//   • BoxTree (from LayoutEngine)
//   • per-element ComputedStyle
//
// Output:
//   • per-Layer DisplayList (lives on the Layer)
//
// Walks the box tree in tree order, partitions boxes into layers
// (based on ComputedStyle::requires_own_layer), and emits the layer's
// display list as it walks. The walk is paint-dirty-aware: clean
// subtrees keep their cached display list.

#include "renderer/engine/box.h"
#include "renderer/engine/layer.h"

#include <memory>

namespace affineui {

class StyleEngine;

struct PaintStats {
    std::uint32_t boxes_visited{0};
    std::uint32_t boxes_painted{0};
    std::uint32_t layers_touched{0};
    std::uint32_t layers_unchanged_by_hash{0};   // see DisplayList::same_pixels_as
    std::uint32_t ops_emitted{0};
    double        cpu_time_ms{0.0};
};

class PaintEngine {
public:
    PaintEngine();
    ~PaintEngine();

    PaintEngine(const PaintEngine&)            = delete;
    PaintEngine& operator=(const PaintEngine&) = delete;
    PaintEngine(PaintEngine&&) noexcept;
    PaintEngine& operator=(PaintEngine&&) noexcept;

    void attach(BoxTree& boxes, StyleEngine& style, LayerTree& layers);

    /// Re-emit display lists for paint-dirty boxes. Cleans the
    /// BF_PaintDirty bit as it goes; sets LF_RasterizeDirty on any
    /// layer whose display list rolling hash changed.
    PaintStats paint_pass();

    /// Force-paint a single box subtree. Useful for tests and the
    /// "promote during idle" use case.
    void paint_subtree(BoxId root);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace affineui
