#pragma once

// DENDER-specific markup emitters: the viewport overlay stack (stats, tool
// rail, nav cluster, N-panel), the timeline dopesheet body
// (ruler / tracks / keys / playhead), and the small Decius chrome helpers the
// chrome sections share (select buttons, pressed icon toggles, menu rows with
// trailing check marks). Purely presentational — interactive pieces emit
// stable widget keys and the view attaches the handlers.

#include "dender_document.h"

#include <affineui/affineui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace dender::ui {

/// The web sample's frame<->pixel mapping for the timeline (buildTimeline):
/// 60px label gutter, then max(3.4, width/140) px per frame.
struct TimelineScale {
    double pad{60.0};
    double ppf{3.4};

    [[nodiscard]] static TimelineScale for_width(double width) noexcept {
        TimelineScale s;
        s.ppf = std::max(3.4, width / 140.0);
        return s;
    }
    [[nodiscard]] double x(int frame) const noexcept { return pad + frame * ppf; }
    [[nodiscard]] int frame_at(double x) const noexcept {
        return static_cast<int>(std::lround((x - pad) / ppf));
    }
};

// ── Shared chrome helpers ────────────────────────────────────────────────────

void icon(affineui::View& view, std::string_view name,
          std::string_view classes = {}, std::string_view key = {});
void text_span(affineui::View& view, std::string_view text,
               std::string_view classes = {}, std::string_view key = {});

/// A boxed select-button (`dcs-select--btn`) that opens `menu_id`: optional
/// leading icon, label, caret. `tip` is the hover tooltip.
void select_button(affineui::View& view, std::string_view label,
                   std::string_view icon_name, std::string_view menu_id,
                   std::string_view key, std::string_view tip = {});

/// An icon-only ghost button with an aria-pressed state and a tooltip.
/// Returns the button ref so the caller can wire on_click.
affineui::WidgetRef icon_toggle(affineui::View& view, std::string_view icon_name,
                                bool pressed, std::string_view tip,
                                std::string_view key,
                                std::string_view extra_classes = {});

/// A menu section header (`dcs-menu__label`).
void menu_label(affineui::View& view, std::string_view text,
                std::string_view key);

/// A menu row with an optional TRAILING check mark (the web's checked-item
/// shape). Composes menu_item_custom so activation matches menu_item.
affineui::WidgetRef check_menu_item(affineui::View& view, std::string_view label,
                                    std::string_view icon_name,
                                    std::string_view shortcut, bool checked,
                                    std::string_view key);

/// A cosmetic "has submenu" row (label + caret, no nested panel) — the web's
/// dead `dcs-menu__item--has-sub` markers (Open Recent, Import, Viewpoint, …).
void submenu_stub(affineui::View& view, std::string_view label,
                  std::string_view icon_name, std::string_view key);

// ── Viewport body (the canvas host's overlay stack) ─────────────────────────
// (The scene itself is DenderViewport::scene_layer — a custom-paint canvas
// drawn by the viewport in dender_viewport.h; these are the DOM overlays
// stacked above it.)

/// Top-left stats overlay ("User Perspective" / collection | active name).
void viewport_stats(affineui::View& view, const DenderDocument& doc);

/// Bottom-left corner overlay (verts/faces/tris/objects readout, computed
/// live from the mesh tables).
void viewport_corner(affineui::View& view, const DenderDocument& doc);

/// The floating tool rail. Buttons are keyed "rail-<tool>" (tweak, cursor,
/// move, rotate, scale, transform, annotate, measure, add-cube) so the view
/// can attach handlers; `active_tool` drives the pressed state.
void tool_rail(affineui::View& view, std::string_view active_tool);

/// Top-right nav cluster: the live 72x72 axis-ball canvas (painted by
/// DenderViewport, repainted with the camera) + the 4 nav buttons, keyed
/// "nav-zoom" / "nav-move" / "nav-camera" / "nav-ortho" for wiring.
/// `move_view_pressed` reflects the Move View orbit/pan toggle.
void nav_cluster(affineui::View& view, bool move_view_pressed);

/// N-panel tab BODIES (Item / Tool / View), per the web reference (cosmetic
/// fields). The panel chrome itself is DECLARED in build_workarea as three
/// dockpanels seeded as one floating tab group — that is what lets the
/// docking system replay their placement across rebuilds.
void npanel_item_body(affineui::View& view);
void npanel_tool_body(affineui::View& view);
void npanel_view_body(affineui::View& view);

// ── Timeline dopesheet body ──────────────────────────────────────────────────

/// Ruler ticks + playback-range tint + Summary/active tracks with diamond
/// keys + the playhead. `width_px` is the pane's available width (the ruler
/// scale is resolution-dependent, like the web's clientWidth rebuild).
void timeline_body(affineui::View& view, const DenderDocument& doc,
                   int width_px);

}  // namespace dender::ui
