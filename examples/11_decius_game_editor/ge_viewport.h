#pragma once

// Real 3D viewport for the game editor, built on the e3d example engine
// (examples/core/3dengine). Replaces the CSS-transform "faux cube":
// the scene renders through sokol_gfx into an offscreen target that the
// custom-paint canvas composites, with orbit camera, click picking and
// a translate/rotate/scale gizmo wired into the app's selection and
// undo stack.

#include <memory>
#include <optional>
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
    struct ObjectTransform {
        e3d::Vec3 location;
        e3d::Vec3 rotation_deg;
        e3d::Vec3 scale{1.0f, 1.0f, 1.0f};
    };
    /// Current transform of a document object's 3D node (nullopt when
    /// the object has no node, e.g. groups).
    [[nodiscard]] std::optional<ObjectTransform> transform_of(
        std::string_view id) const;
    /// Set one channel (axis 0=X 1=Y 2=Z) through an undoable command.
    void set_location(std::string_view id, int axis, double value);
    void set_rotation_deg(std::string_view id, int axis, double value);
    void set_scale(std::string_view id, int axis, double value);

    static constexpr const char* kPaintName = "ge.scene";

private:
    void frame(double dt);
    void paint(affineui::Painter& p, const affineui::Rect& r);
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
