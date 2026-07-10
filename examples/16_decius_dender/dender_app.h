#pragma once

// DENDER controller — "the editor in Blender clothing". Owns the affineui::App
// and an app::Context (the shared DCC hub: Document + Selection + CommandStack +
// CommandRegistry), the shared reusable 3D viewport (viewport3d::Viewport3D),
// keyboard shortcuts, and the timeline scrub gesture. All DENDER-specific chrome
// state (transform mode, shading, timeline, tool rail, inspector tabs) stays
// local here — only the general spine lives in app::. Trackable so UI callbacks
// bound to its methods become safe no-ops if it is ever destroyed while the view
// is live.

#include "dender_scene.h"

#include "affineui_app.h"  // app template: Context, Settings, styles
#include "viewport3d.h"    // the shared reusable 3D viewport

#include "e3d.h"  // e3d::ObjectPtr / Euler / Vec3 for the node factory

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dender {

/// DENDER-local viewport chrome state, none of which is scene data (mode/
/// shading are cosmetic vs the real GPU viewport; the timeline is a pure
/// DENDER concept). Kept off the shared app:: hub.
enum class TransformMode { Translate, Rotate, Scale };
enum class Shading { Wireframe, Solid, Texture, Rendered };

// Timeline lives in dender_scene.h (shared with the presentational
// components without pulling in the viewport / app headers).

/// Chrome-only state the web marks cosmetic (aria-pressed toggles, which
/// inspector/N-panel tab is showing, the active tool-rail tool). Kept out of
/// the document: none of it is scene data.
struct UiState {
    std::string active_tool{"tweak"};   // tool-rail radio (web default Tweak)
    std::string inspector_tab{"object"};
    bool snapping{false};
    bool proportional{false};
    bool show_gizmo{true};
    bool show_overlays{true};
    bool xray{false};
    bool auto_key{false};
};

class DenderApp : public affineui::Trackable {
public:
    DenderApp();

    int run();
    /// Headless timing of the reload path (--profile): view rebuild vs
    /// load_view (serialize+reparse) vs forced layout, per interaction.
    int profile(int iterations);
    void reload();

    /// Build the current view (also the headless AFFINEUI_DUMP_HTML path).
    [[nodiscard]] affineui::View build_view();

    // ── State the view renders from ──────────────────────────────────────
    [[nodiscard]] app::Context& ctx() noexcept { return ctx_; }
    [[nodiscard]] const app::Context& ctx() const noexcept { return ctx_; }
    /// The shared 3D viewport module.
    [[nodiscard]] viewport3d::Viewport3D& viewport() noexcept {
        return *viewport_;
    }
    [[nodiscard]] const UiState& ui() const noexcept { return ui_; }
    [[nodiscard]] const Timeline& timeline() const noexcept { return timeline_; }
    [[nodiscard]] Shading shading() const noexcept { return shading_; }
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] std::string_view accent() const noexcept { return accent_; }
    [[nodiscard]] std::string_view density() const noexcept { return density_; }
    [[nodiscard]] std::string_view style() const noexcept { return style_; }
    [[nodiscard]] bool can_undo() const { return ctx_.stack().can_undo(); }
    [[nodiscard]] bool can_redo() const { return ctx_.stack().can_redo(); }
    [[nodiscard]] int view_width() const;  // logical px (timeline ruler scale)

    // Dock persistence plumbing the view wires into its providers.
    [[nodiscard]] int workspace_panel_size(std::string_view id) const;
    [[nodiscard]] affineui::Document::DockLayout dock_layout() const;
    [[nodiscard]] affineui::Document::DockPlacement dock_override(
        std::string_view id) const;
    [[nodiscard]] std::string dock_active_tab(std::string_view id) const;

    // ── Handlers (bound into the view) ───────────────────────────────────
    void select_object(std::string_view id);
    /// Viewport click-pick: empty id = clicked empty space. Same selection
    /// path as outliner clicks (selection never rides the undo stack).
    void pick_object(std::string_view id, bool shift);
    void rename_active(std::string_view name);            // undoable
    void set_location(int axis, std::string_view value);  // undoable, coalesced
    void set_rotation_deg(int axis, std::string_view value);
    void set_scale(int axis, std::string_view value);
    /// Item-panel Dimensions edit: rebuild the active object's mesh at the new
    /// base size (a mesh REGENERATION, not a scale). preview_dimension swaps
    /// the mesh live during a scrub (no undo entry); commit_dimension pushes
    /// ONE undoable resize at the gesture's end.
    void preview_dimension(int axis, std::string_view value);
    void commit_dimension(int axis, std::string_view value);
    void add_object(std::string_view type);                // undoable, selects
    void delete_active();                                  // undoable
    void duplicate_active();                               // undoable, selects
    void undo();
    void redo();

    void set_tool(std::string_view tool);      // rail radio (+ transform mode)
    void set_shading(std::string_view mode);   // "wire"/"solid"/"tex"/"render"
    void toggle_play();
    void set_frame(std::string_view value);    // combo edits (cross-validated)
    void set_start(std::string_view value);
    void set_end(std::string_view value);

    void set_accent(std::string_view accent);    // theme tweaks popover
    void set_density(std::string_view density);
    void set_style(std::string_view style);

    void toggle_flag(std::string_view which);    // snapping/gizmo/… toggles
    void set_inspector_tab(std::string_view id); // rail tab (rebuild keeps it)
    void quit();

private:
    [[nodiscard]] affineui::App::Config make_config();
    /// Stylesheet + persistent-view registration shared by run()/profile().
    void boot();
    /// Rebuild the bootstrap shell (theme selectors live on <body>).
    void rebuild_shell();
    void seed_scene();  // the web sample's initial Cube / Light / Camera
    void load_settings();
    void save_settings();
    void save_dock_layout();
    void bind_keys();
    /// Low-level native events: keyboard commands + the timeline scrub drag.
    bool handle_event(const affineui::Event& ev,
                      const std::vector<affineui::Document::HoverInfo>& chain);
    bool handle_key(const affineui::Event& ev,
                    const std::vector<affineui::Document::HoverInfo>& chain);
    void run_transform_edit(std::string_view prop, std::string_view value,
                            bool degrees, std::string_view label);
    /// Regenerate the object's mesh geometry at its base dimensions with one
    /// axis replaced (shared by preview_/commit_dimension).
    [[nodiscard]] e3d::GeometryPtr regen_dimension(std::string_view id,
                                                   int axis,
                                                   std::string_view value);

    /// DENDER catalog type -> e3d node (the viewport's make_node factory).
    /// Reads spawn from the catalog, or a pending override (duplicate x+0.5 /
    /// restored transform) when one is queued for this object id.
    [[nodiscard]] e3d::ObjectPtr make_scene_node(const app::Object& obj);

    /// Transform queued for the next node the factory builds for an id.
    struct NodeSpawn {
        Vec3 position{};        // DENDER Vec3 (doubles)
        e3d::Vec3 rotation{};   // radians XYZ Euler
        e3d::Vec3 scale{1.0f, 1.0f, 1.0f};
    };

    // ── The shared DCC hub ───────────────────────────────────────────────
    app::Context ctx_;
    UiState ui_{};
    affineui::App app_;
    std::unique_ptr<viewport3d::Viewport3D> viewport_;

    // ── DENDER-local chrome state (never in the shared hub) ──────────────
    TransformMode mode_{TransformMode::Translate};
    Shading shading_{Shading::Solid};
    Timeline timeline_{};
    bool playing_{false};
    int id_counter_{0};  // monotonic obj_N minting (web viewport.js ids)

    // Transforms queued for nodes the factory has not built yet (duplicate
    // x+0.5). Keyed by document object id, consumed once by make_scene_node.
    std::unordered_map<std::string, NodeSpawn> pending_spawns_;

    // Pre-gesture mesh geometry captured on the first Dimensions preview of a
    // scrub, so commit_dimension can push ONE undoable resize (old -> new).
    e3d::GeometryPtr dim_gesture_geometry_;
    std::string      dim_gesture_id_;

    // Theme state moved off the document onto the controller (game_editor
    // pattern).
    std::string accent_{"orange"};
    std::string density_{"comfortable"};
    std::string style_{"flat"};

    app::Preferences prefs_;  // durable: accent / density / style
    app::Workspace ws_;       // ephemeral: dock pane sizes

    // Kept so rebuild_shell() can re-apply the sheet (shell re-bootstrap).
    std::string stylesheet_;
    std::string stylesheet_base_;

    affineui::Keymap keymap_;

    // Timeline scrub gesture (pointer-captured drag on the dopesheet body).
    bool scrubbing_{false};
    affineui::Rect scrub_bounds_{};
};

}  // namespace dender
