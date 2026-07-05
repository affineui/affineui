#pragma once

// DENDER 3D viewport — the real, interactive, software-projected vector
// viewport drawn through the custom-paint canvas (data-aui-paint), the
// native stand-in for the web sample's three.js canvas (camera parameters
// and interaction feel per viewport.js; a true GPU engine is ticket T8).
// The declaration side emits two canvas blocks (scene + 72x72 nav gizmo);
// the registered paint handlers project and draw the scene with Painter at
// repaint time. Camera moves are GEOMETRY-ONLY: request_custom_repaint
// marks the two rects dirty — no View rebuild, no reconcile, no restyle,
// no layout. The runtime side owns the orbit camera plus the pointer
// interaction installed on App::on_event.
//
// This module owns ONLY camera state, pick geometry, and interaction
// bookkeeping; scene / selection / transform state stays in DenderDocument.
// Camera moves (orbit / pan / dolly / frame / axis-snap) are deliberately
// NOT undoable commands, and click-pick selection is routed back through
// the app so it follows the same path as outliner selection.
//
// TODO(dender): transform gizmo (spec stretch goal) — translate-mode axis
// arrows at the active object's origin with closest-point-on-axis dragging.
// Until then G/R/S set the transform mode + tool-rail state only (Phase A
// behavior) and edits go through the Inspector fields.

#include "dender_document.h"

#include <affineui/affineui.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dender {

/// Custom-paint handler names (data-aui-paint values). Shared between the
/// viewport (registers the handlers) and the components that declare the
/// canvas blocks.
inline constexpr const char* kScenePaintName = "dender.scene";
inline constexpr const char* kGizmoPaintName = "dender.gizmo";

/// Topology counts for one primitive's mesh table. `faces` counts logical
/// faces (quads + tris) — the Blender-style numbers the web overlays show
/// (Cube = verts 8 / faces 6 / tris 12).
struct MeshStats {
    int verts{0};
    int faces{0};
    int tris{0};
};
/// Stats for a mesh primitive type; null for lights / camera / empty.
[[nodiscard]] const MeshStats* mesh_stats_for(std::string_view type);

/// Live overlay / statusbar numbers: the active mesh's counts when the
/// active object is a mesh (the web boots showing the Cube's 8/6/12),
/// falling back to scene mesh totals otherwise; plus selected/total.
struct SceneStats {
    int verts{0};
    int faces{0};
    int tris{0};
    int selected{0};
    int total{0};
};
[[nodiscard]] SceneStats scene_stats(const DenderDocument& doc);

class DenderViewport {
public:
    explicit DenderViewport(const DenderDocument& doc);

    // ── Declaration (inside the dn-vp-canvas float host) ─────────────────
    /// The scene layer: a positioned wrapper div hosting the scene's
    /// custom-paint canvas block. Declare FIRST inside the canvas host so
    /// the DOM overlays (stats, tool rail, nav cluster, N-panel) stack
    /// above it. Content is drawn by the paint handler at repaint time —
    /// camera moves never touch this declaration.
    void scene_layer(affineui::View& v);

    // ── Runtime ──────────────────────────────────────────────────────────
    /// Install the pointer interaction + the scene/gizmo paint handlers.
    /// Call once.
    void attach(affineui::App& app);

    /// View rebuild request (the app's reload). Camera moves call this.
    std::function<void()> request_render;

    /// Click-pick outcome (a real click, not a drag). Empty id = clicked
    /// empty space. `shift` selects additively (web multi-select).
    std::function<void(std::string_view id, bool shift)> on_pick;

    // ── Camera commands (nav buttons / keys) — never undoable ───────────
    /// Fit the selection, or the whole scene when nothing is selected:
    /// dist = max(2, size * 1.4). Bound to F and the Camera View button.
    void frame_selected();
    /// Multiply the orbit distance (nav Zoom button: 0.8 in / 1.25 out).
    void dolly(double factor);
    /// "Move View": LMB drag pans instead of orbiting (aria-pressed).
    void toggle_pan_mode();
    [[nodiscard]] bool pan_mode() const noexcept { return pan_mode_; }
    /// Live modifier tracking — on_click callbacks carry no modifiers, so
    /// the nav Zoom button reads the last-seen shift state from here.
    [[nodiscard]] bool shift_down() const noexcept { return shift_down_; }

private:
    using Clock = std::chrono::steady_clock;

    struct GizmoNub {
        double x{0.0};  // 72x72 gizmo-local px
        double y{0.0};
        int axis{0};    // 0..5 = +X +Y +Z -X -Y -Z
    };
    struct PickFace {
        double x0, y0, x1, y1, x2, y2;
        double depth;
        int obj;
    };
    struct PickLine {
        double x0, y0, x1, y1;
        double depth;
        double tol;
        int obj;
    };

    bool handle_event(const affineui::Event& ev,
                      const std::vector<affineui::Document::HoverInfo>& chain);
    void orbit(double dx, double dy);
    void pan(double dx, double dy);
    void snap_to_axis(int axis);
    void camera_changed();
    void pick_at(double local_x, double local_y, bool shift) const;
    /// Paint handlers (registered in attach). paint_scene also refreshes
    /// canvas_rect_ and the pick geometry; paint_gizmo refreshes nubs_.
    void paint_scene(affineui::Painter& p, const affineui::Rect& r);
    void paint_gizmo(affineui::Painter& p, const affineui::Rect& r);

    const DenderDocument& doc_;
    affineui::App* app_{nullptr};

    // Orbit camera (web viewport.js: fov 38, target (0, 0.6, 0), initial
    // eye (5.2, 3.4, 6.4) → yaw/pitch/dist derived in the constructor).
    double yaw_{0.0};
    double pitch_{0.0};
    double dist_{8.7};
    Vec3 target_{0.0, 0.6, 0.0};
    bool pan_mode_{false};

    // Pick geometry captured while painting the scene (canvas-local px —
    // handle_event converts event coords through canvas_rect_).
    std::array<GizmoNub, 6> nubs_{};
    std::vector<PickFace> pick_faces_;
    std::vector<PickLine> pick_lines_;
    std::vector<std::string> pick_ids_;
    affineui::Rect canvas_rect_{};

    // Drag interaction (click-vs-drag discrimination per the web: moved
    // <= 4px, held <= 600ms, not within the 250ms post-drag suppression).
    bool dragging_{false};
    bool drag_pan_{false};
    bool drag_left_{false};
    bool drag_moved_{false};
    double down_x_{0.0};
    double down_y_{0.0};
    double last_x_{0.0};
    double last_y_{0.0};
    Clock::time_point down_time_{};
    Clock::time_point suppress_until_{};
    bool shift_down_{false};
};

}  // namespace dender
