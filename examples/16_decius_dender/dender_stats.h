#pragma once

// DENDER mesh-topology statistics — the Verts / Faces / Tris numbers the web
// overlays and statusbar show. These are the Blender-style logical counts for
// each primitive type (Cube = 8 / 6 / 12), independent of the GPU viewport's
// own tessellation. A small standalone helper (the software-projection
// viewport that once computed these has been replaced by the shared
// viewport3d::Viewport3D).

#include "app/document.h"

#include <functional>
#include <string_view>

namespace dender {

/// Topology counts for one primitive's mesh table. `faces` counts logical
/// faces (quads + tris) — the Blender-style numbers the web shows.
struct MeshStats {
    int verts{0};
    int faces{0};
    int tris{0};
};
/// Stats for a mesh primitive type; null for lights / camera / empty.
[[nodiscard]] const MeshStats* mesh_stats_for(std::string_view type);

/// Live overlay / statusbar numbers: the active mesh's counts when the active
/// object is a mesh (the web boots showing the Cube's 8/6/12), falling back to
/// scene mesh totals otherwise; plus selected/total.
struct SceneStats {
    int verts{0};
    int faces{0};
    int tris{0};
    int selected{0};
    int total{0};
};
/// `active_id` is the selection's active object; `is_selected(id)` reports
/// selection membership (active + multi).
[[nodiscard]] SceneStats scene_stats(
    const app::Document& doc, std::string_view active_id,
    const std::function<bool(std::string_view)>& is_selected);

}  // namespace dender
