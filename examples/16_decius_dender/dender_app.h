#pragma once

// DENDER controller — owns the affineui::App, the scene document, the undo
// stack (the affineui_app template's CommandStack driven by app-specific
// commands that edit DenderDocument), keyboard shortcuts, and the timeline
// scrub gesture. Trackable so UI callbacks bound to its methods become safe
// no-ops if it is ever destroyed while the view is live.

#include "dender_document.h"
#include "dender_viewport.h"

#include "affineui_app.h"  // app template: CommandStack, Settings, styles

#include <string>
#include <string_view>
#include <vector>

namespace dender {

/// Chrome-only state the web marks cosmetic (aria-pressed toggles, which
/// inspector/N-panel tab is showing, the active tool-rail tool). Kept out of
/// the document: none of it is scene data.
struct UiState {
    std::string active_tool{"tweak"};   // tool-rail radio (web default Tweak)
    std::string inspector_tab{"object"};
    std::string npanel_tab{"item"};
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
    [[nodiscard]] const DenderDocument& document() const noexcept {
        return document_;
    }
    /// The 3D viewport module (scene SVG layer + camera + interaction).
    [[nodiscard]] DenderViewport& viewport() noexcept { return viewport_; }
    [[nodiscard]] const UiState& ui() const noexcept { return ui_; }
    [[nodiscard]] bool can_undo() const { return stack_.can_undo(); }
    [[nodiscard]] bool can_redo() const { return stack_.can_redo(); }
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
    void set_npanel_tab(std::string_view id);    // records only, no rebuild
    void quit();

private:
    [[nodiscard]] affineui::App::Config make_config();
    /// Stylesheet + persistent-view registration shared by run()/profile().
    void boot();
    /// Rebuild the bootstrap shell (theme selectors live on <body>).
    void rebuild_shell();
    void load_settings();
    void save_settings();
    void save_dock_layout();
    void bind_keys();
    /// Low-level native events: keyboard commands + the timeline scrub drag.
    bool handle_event(const affineui::Event& ev,
                      const std::vector<affineui::Document::HoverInfo>& chain);
    bool handle_key(const affineui::Event& ev,
                    const std::vector<affineui::Document::HoverInfo>& chain);
    void run_transform_edit(int axis, std::string_view value, int channel_base,
                            std::string_view label);

    DenderDocument document_;
    UiState ui_{};
    affineui::App app_;
    DenderViewport viewport_{document_};

    // The template CommandStack is agnostic about the document it edits — its
    // Document& parameter exists for the template's property-bag model. Our
    // commands capture DenderDocument directly, so the shadow document is just
    // the stack's required plumbing.
    app::Document shadow_doc_;
    app::CommandStack stack_{shadow_doc_};

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
