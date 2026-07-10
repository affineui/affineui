#pragma once

// Real 3D viewport for the game editor, built on the e3d example engine
// (examples/core/3dengine). Replaces the CSS-transform "faux cube":
// the scene renders through sokol_gfx into an offscreen target that the
// custom-paint canvas composites, with orbit camera, click picking and
// a translate/rotate/scale gizmo wired into the app's selection and
// undo stack.

#include <array>
#include <memory>
#include <string>
#include <unordered_map>

#include "affineui_app.h"
#include "e3d.h"

namespace ge {

class GeViewport : public affineui::Trackable {
public:
    GeViewport();
    ~GeViewport() override;

    /// Register the paint handler, the per-frame tick, and remember the
    /// context whose document/selection drive the scene. Call once,
    /// before the first frame. The App must outlive this viewport.
    void attach(affineui::App& app, app::Context& ctx);

    /// Declare the canvas block (inside the viewport pane container).
    void build(affineui::View& v);

    /// Route a document event (called from the app's on_event handler).
    /// Returns true when the viewport consumed it.
    bool handle_event(
        const affineui::Event& ev,
        const std::vector<affineui::Document::HoverInfo>& chain);

    /// Tool rail / keyboard: "select", "move", "rotate", "scale".
    void set_tool(std::string_view tool);

    /// Reconcile 3D nodes with the app document's object list (adds and
    /// removes; existing nodes keep their transforms).
    void sync_document();
    /// Point the selection box (and, for gizmo tools, the gizmo) at the
    /// app selection.
    void sync_selection();

    // ── Inspector bridge ────────────────────────────────────────────
    /// The 3D node mirroring a document object (nullptr when the object
    /// has no node, e.g. groups). Inspect it through its reflection
    /// class: `get_class(*node)` — see e3d_scene.h.
    [[nodiscard]] e3d::Object3D* node_of(std::string_view id) const;
    /// Set one reflected property (by its ObjectClass name, e.g.
    /// "position.x", "visible") through an undoable command.
    void set_node_property(std::string_view id, std::string_view prop,
                           const affineui::PropertyValue& value);

    static constexpr const char* kPaintName = "ge.scene";
    static constexpr const char* kNavPaintName = "ge.navball";

private:
    void frame(double dt);
    void paint(affineui::Painter& p, const affineui::Rect& r);
    void paint_navball(affineui::Painter& p, const affineui::Rect& r);
    void snap_camera_to_axis(int axis);
    void mark_dirty();
    e3d::Vec2 to_ndc(double x, double y) const;
    e3d::ObjectPtr make_node(const app::Object& obj, std::size_t index);
    void pick(double mx, double my, bool additive);
    void push_transform_command();
    void apply_transform_command(const e3d::ObjectPtr& node,
                                 const e3d::Vec3& old_p,
                                 const e3d::Quat& old_q,
                                 const e3d::Vec3& old_s);

    affineui::App* app_{nullptr};
    app::Context*  ctx_{nullptr};

    std::shared_ptr<e3d::Scene>             scene_;
    std::shared_ptr<e3d::PerspectiveCamera> camera_;
    std::unique_ptr<e3d::Renderer>          renderer_;
    std::unique_ptr<e3d::OrbitControls>     orbit_;
    std::unique_ptr<e3d::TransformControls> gizmo_;
    std::shared_ptr<e3d::SelectionBox>      selection_box_;
    e3d::ObjectPtr                          selected_node_;

    // Document-object nodes by app object id (grid/lights/floor are not
    // in here and are never pickable).
    std::unordered_map<std::string, e3d::ObjectPtr> nodes_;

    affineui::Rect canvas_rect_{};
    bool           dirty_{true};

    // Navigation gizmo (axis ball): nub centers in the web gizmo's
    // 72x72 local space, refreshed by paint_navball for click
    // hit-testing (the web's snap-to-axis nubs).
    struct NavNub {
        double x{0.0}, y{0.0};
        int    axis{0};  // 0..5 = +X +Y +Z -X -Y -Z
    };
    std::array<NavNub, 6> nav_nubs_{};
    affineui::Rect        nav_rect_{};

    // Camera drag / click-vs-drag state.
    bool   cam_dragging_{false};
    bool   gizmo_dragging_{false};
    bool   drag_moved_{false};
    bool   drag_left_{false};
    double down_x_{0.0}, down_y_{0.0};

    // Gizmo drag start pose (for the undo command).
    e3d::Vec3  start_position_;
    e3d::Quat  start_quaternion_;
    e3d::Vec3  start_scale_{1.0f, 1.0f, 1.0f};
    std::string tool_{"select"};
};

}  // namespace ge
