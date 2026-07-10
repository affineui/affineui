#pragma once

// Reusable DCC 3D viewport, built on the e3d example engine
// (examples/core/3dengine) and the affineui_app template. The scene renders
// through sokol_gfx into an offscreen target that a custom-paint canvas
// composites, with an orbit camera, click picking and a translate/rotate/
// scale gizmo wired into an app::Context's selection and undo stack.
//
// This is the generalized form of the game editor's original GeViewport:
// the couplings a host hard-codes (custom-paint handler names, the canvas
// class handle_event claims, and how a document object maps to an e3d node)
// are parameterized through a Config so a second host (DENDER) can reuse the
// same viewport with its own DOM classes and primitive catalog. The Config
// defaults reproduce the game editor's exact values, so a host that passes
// an empty Config gets identical behavior.

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "affineui_app.h"
#include "e3d.h"

namespace viewport3d {

class Viewport3D : public affineui::Trackable {
public:
    /// Factory turning a document object into its 3D node (or nullptr for
    /// objects that live only in the outliner, e.g. groups). Injected via
    /// Config so each host maps its own object types / primitive catalog.
    /// `index` is the object's position in the document's object list —
    /// hosts can use it for deterministic default placement.
    using NodeFactory =
        std::function<e3d::ObjectPtr(const app::Object& obj, std::size_t index)>;

    /// Host-specific couplings. Defaults match the game editor exactly, so a
    /// host passing `{}` keeps today's game-editor behavior.
    struct Config {
        std::string scene_paint  = "ge.scene";     // scene canvas paint name
        std::string nav_paint    = "ge.navball";   // nav axis-ball paint name
        std::string canvas_class = "ge-vp-canvas"; // host class handle_event claims
        std::string stats_class  = "ge-vp-stats";  // overlay that declines events
        // Classes/keys emitted on the two canvas ELEMENTS build() declares.
        // (canvas_class above is the host's float-host CONTAINER, emitted by
        // the host and only read back in handle_event — not put on a canvas.)
        std::string scene_canvas_class = "ge-vp-3dcanvas";
        std::string scene_canvas_key   = "ge-vp-3d";
        std::string nav_canvas_class   = "ge-vp-navball";
        std::string nav_canvas_key     = "ge-vp-nav";
        // Document-object -> e3d node mapping. Empty = the built-in 3-type
        // fallback (mesh / light / spline) the game editor originally used.
        NodeFactory make_node{};
    };

    // Two constructors instead of a defaulted argument: a `Config{}` default
    // ARGUMENT would reference Config's default member initializers before
    // this enclosing class is complete, which GCC rejects (MSVC accepts). A
    // separate no-arg ctor that constructs the Config in its own body avoids
    // the parse-order problem on both compilers.
    Viewport3D() : Viewport3D(Config{}) {}
    explicit Viewport3D(Config config);
    ~Viewport3D() override;

    /// Install/replace the node factory after construction (alternative to
    /// setting it in the Config).
    void set_node_factory(NodeFactory factory) {
        cfg_.make_node = std::move(factory);
    }

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

    // ── Camera ops (nav buttons / keys) — never undoable ────────────────
    /// Multiply the orbit distance (zoom): factor < 1 dollies in.
    void dolly(double factor);
    /// Fit the camera to the selected node's bounds (or a default framing
    /// of the current selection box when nothing definite is selected).
    void frame_selected();
    /// Flip the orbit left-button between Rotate and Pan ("Move View").
    void toggle_pan_mode();
    [[nodiscard]] bool pan_mode() const noexcept { return pan_mode_; }
    /// Snap the camera down a world axis: 0..5 = +X +Y +Z -X -Y -Z.
    void snap_to_axis(int axis);

    // ── Inspector bridge ────────────────────────────────────────────
    /// The 3D node mirroring a document object (nullptr when the object
    /// has no node, e.g. groups). Inspect it through its reflection
    /// class: `get_class(*node)` — see e3d_scene.h.
    [[nodiscard]] e3d::Object3D* node_of(std::string_view id) const;

    /// The object's BASE (unscaled) dimensions: the size of its mesh
    /// geometry's bounding box, ignoring the node's scale transform.
    /// {0,0,0} when the object has no node or no mesh geometry.
    [[nodiscard]] e3d::Vec3 base_dimensions(std::string_view id) const;

    /// Swap the object's mesh geometry (a mesh REGENERATION — e.g. the Item
    /// panel's Dimensions edit rebuilds the primitive at a new size). The
    /// caller owns type-specific regeneration (it knows a box from a sphere);
    /// this just installs the new geometry so the renderer re-uploads it.
    /// No-op if the object has no mesh node.
    void set_node_geometry(std::string_view id, e3d::GeometryPtr geometry);
    /// Live-preview one reflected property (by its ObjectClass name,
    /// e.g. "position.x") during a continuous gesture: applies the
    /// value and remembers the pre-gesture value, but pushes NO undo
    /// entry — commit_node_property() ends the gesture.
    void preview_node_property(std::string_view id, std::string_view prop,
                               const affineui::PropertyValue& value);
    /// Commit a reflected property edit as one undoable command whose
    /// undo restores the pre-gesture value (works with or without
    /// preceding previews).
    void commit_node_property(std::string_view id, std::string_view prop,
                              const affineui::PropertyValue& value);

    /// Live material hooks (the inspector's Material foldout): applied
    /// directly to the node's mesh material — persistence stays in the
    /// app document's properties, re-applied by sync_document().
    void set_material_tint(std::string_view id, std::string_view hex);
    void set_material_roughness(std::string_view id, double roughness);

    /// Fired whenever a control mutates a node's transform (the gizmo
    /// drag path — three.js objectChange), with the document object id.
    /// The inspector listens to track edits live.
    std::function<void(const std::string& id)> on_node_changed;

private:
    void frame(double dt);
    void paint(affineui::Painter& p, const affineui::Rect& r);
    void paint_navball(affineui::Painter& p, const affineui::Rect& r);
    void snap_camera_to_axis(int axis);
    void mark_dirty();
    e3d::Vec2 to_ndc(double x, double y) const;
    e3d::ObjectPtr make_node(const app::Object& obj, std::size_t index);
    e3d::ObjectPtr make_default_node(const app::Object& obj, std::size_t index);
    void pick(double mx, double my, bool additive);
    void push_transform_command();
    void apply_transform_command(const e3d::ObjectPtr& node,
                                 const e3d::Vec3& old_p,
                                 const e3d::Quat& old_q,
                                 const e3d::Vec3& old_s);

    Config         cfg_;
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
    // "Move View": orbit left-button flips between Rotate and Pan.
    bool           pan_mode_{false};

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

    // Pre-gesture values captured by preview_node_property, keyed
    // "<id>\x1f<prop>", consumed by commit_node_property's undo.
    std::unordered_map<std::string, affineui::PropertyValue> gesture_old_;
};

}  // namespace viewport3d
