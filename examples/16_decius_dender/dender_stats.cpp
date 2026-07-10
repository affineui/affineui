#include "dender_stats.h"

namespace dender {

const MeshStats* mesh_stats_for(std::string_view type) {
    // Blender-style logical topology counts, matching the tessellation the
    // original software viewport used (Cube 8/6/12, etc.). Precomputed so the
    // overlays need no geometry.
    static const MeshStats kCube{8, 6, 12};
    static const MeshStats kUvSphere{178, 192, 352};
    static const MeshStats kIcosphere{42, 80, 80};
    static const MeshStats kCylinder{34, 48, 64};
    static const MeshStats kCone{18, 32, 32};
    static const MeshStats kTorus{240, 240, 480};
    static const MeshStats kPlane{4, 1, 2};
    if (type == "Cube") return &kCube;
    if (type == "UV Sphere") return &kUvSphere;
    if (type == "Icosphere") return &kIcosphere;
    if (type == "Cylinder") return &kCylinder;
    if (type == "Cone") return &kCone;
    if (type == "Torus") return &kTorus;
    if (type == "Plane") return &kPlane;
    return nullptr;
}

SceneStats scene_stats(const app::Document& doc, std::string_view active_id,
                       const std::function<bool(std::string_view)>& is_selected) {
    SceneStats st;
    st.total = static_cast<int>(doc.objects().size());
    for (const auto& obj : doc.objects()) {
        if (is_selected(obj.id)) ++st.selected;
    }
    const app::Object* active =
        active_id.empty() ? nullptr : doc.find(active_id);
    if (const MeshStats* ms =
            active != nullptr ? mesh_stats_for(active->type) : nullptr) {
        st.verts = ms->verts;
        st.faces = ms->faces;
        st.tris = ms->tris;
        return st;
    }
    for (const auto& obj : doc.objects()) {  // fall back to scene totals
        if (const MeshStats* ms = mesh_stats_for(obj.type)) {
            st.verts += ms->verts;
            st.faces += ms->faces;
            st.tris += ms->tris;
        }
    }
    return st;
}

}  // namespace dender
