#pragma once

// Game/scene editor — a showcase built entirely on the AffineUI component API
// and the affineui_app DCC template. No raw HTML strings: the UI is declared
// with strongly-typed components and structural builders, state lives in an
// app::Context (document + selection + undo stack), and every callback is a
// safe affineui::bind() member so a destroyed controller can never crash an
// event.

// The app/DCC template (commands, undo, selection, …). It pulls in the core
// AffineUI library itself, so this is the only include the controller needs.
#include "affineui_app.h"

// The real 3D viewport (e3d engine + orbit camera + transform gizmo),
// now the shared reusable module.
#include "viewport3d.h"

namespace ge {

/// The editor controller. Trackable so UI callbacks bound to its methods turn
/// into safe no-ops if it is ever destroyed while the view is live.
class GameEditor : public affineui::Trackable {
public:
    GameEditor();

    [[nodiscard]] affineui::App::Config config();  // wires on_layout_changed → this

    /// Build the whole editor view from current state.
    [[nodiscard]] affineui::View build();

    /// Restrict the live game-editor layout to one subsystem for integration
    /// debugging. Empty/full keeps the normal demo.
    void set_debug_scope(std::string_view scope);

    /// Bind to an App and run. The App calls reload() whenever state changes.
    int run();

private:
    // ── Command / selection / state handlers (bound into the UI) ─────────
    void select_object(std::string_view id);
    void toggle_play();
    void set_tool(std::string_view tool);
    void rename_selected(std::string_view name);
    void set_roughness(std::string_view value);
    void set_tint(std::string_view value);
    void set_cast_shadows(std::string_view value);
    // Display-panel showcase controls (multi-button / checkbox / switch /
    // textarea) — kept as local UI state so the widgets are fully interactive.
    void set_blend(std::string_view value);
    void set_visible(std::string_view value);
    void set_wireframe(std::string_view value);
    void set_notes(std::string_view value);
    void undo();
    void redo();
    void duplicate_selected();
    void delete_selected();

    // ── View sections ────────────────────────────────────────────────────
    void build_menubar(affineui::View& v);
    /// The native application menu (macOS system bar). Rebuilt whenever the
    /// state it displays — the density/accent check marks — changes.
    void build_native_menu();
    [[nodiscard]] std::vector<affineui::MenuItem> build_view_menu();
    void build_outliner(affineui::View& v);
    void build_viewport(affineui::View& v);
    void build_viewport_toolbar(affineui::View& v);  // the document tab toolbar
    void build_inspector(affineui::View& v);
    void build_assets(affineui::View& v);
    void build_console(affineui::View& v);  // co-tab of Assets (tab-switch demo)
    void build_statusbar(affineui::View& v);

    void reload();

    [[nodiscard]] std::string_view selected_id() const;
    [[nodiscard]] const app::Object* selected() const;

    void load_settings();
    void save_settings();
    void save_dock_layout();  // persist dock-pane sizes after a splitter drag

    // View-menu appearance preferences: persist + rebuild with the new
    // framework selector. Spacing/sizing then comes entirely from the
    // bundle's per-density variables.
    void set_density(std::string_view density);
    void set_accent(std::string_view accent);

    app::Context    ctx_;
    affineui::App   app_;
    std::unique_ptr<viewport3d::Viewport3D> viewport_;  // real 3D scene view
    app::Preferences prefs_;   // durable: accent + density
    app::Workspace   ws_;      // ephemeral: last tool, panel layout
    bool          playing_{false};
    std::string   tool_{"select"};
    std::string   accent_{"cyan"};
    std::string   density_{"compact"};
    // Display-panel showcase state.
    std::string   blend_{"Normal"};
    bool          visible_{true};
    bool          wireframe_{false};
    std::string   notes_{"Weathered sandstone — chipped along the lower edge."};
    std::string   debug_scope_;
};

}  // namespace ge
