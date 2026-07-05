#include "dender_components.h"

#include "dender_viewport.h"  // live scene stats for the corner overlay

#include <cstdio>

namespace dender::ui {

using affineui::View;
using affineui::WidgetRef;

namespace {

std::string key_with(std::string_view prefix, std::string_view suffix) {
    std::string out{prefix};
    out += '-';
    out += suffix;
    return out;
}

std::string px(double value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1fpx", value);
    return buf;
}

}  // namespace

void icon(View& view, std::string_view name, std::string_view classes,
          std::string_view key) {
    std::string cls = "di di-" + std::string(name);
    if (!classes.empty()) {
        cls += ' ';
        cls += classes;
    }
    view.element_ref("i", cls, key);
}

void text_span(View& view, std::string_view text, std::string_view classes,
               std::string_view key) {
    view.element("span", classes, key).text(text);
}

void select_button(View& view, std::string_view label,
                   std::string_view icon_name, std::string_view menu_id,
                   std::string_view key, std::string_view tip) {
    auto btn = view.element("button", "dcs-select dcs-select--btn", key);
    btn.attr("type", "button")
        .attr("data-dcs-toggle", "menu")
        .attr("data-dcs-target", "#" + std::string(menu_id));
    if (!tip.empty()) btn.attr("data-dcs-tip", tip);
    if (!icon_name.empty()) {
        icon(view, icon_name, "dcs-select__icon", key_with(key, "icon"));
    }
    text_span(view, label, "dcs-select__label", key_with(key, "label"));
    {
        auto caret = view.container("dcs-select__caret", key_with(key, "caret"));
        icon(view, "chevron-down", {}, key_with(key, "caret-icon"));
    }
}

WidgetRef icon_toggle(View& view, std::string_view icon_name, bool pressed,
                      std::string_view tip, std::string_view key,
                      std::string_view extra_classes) {
    auto btn = view.icon_button(icon_name, key);
    if (!extra_classes.empty()) {
        btn.cls("dcs-btn dcs-btn--icon dcs-btn--ghost " +
                std::string(extra_classes));
    }
    btn.attr("aria-pressed", pressed ? "true" : "false");
    if (!tip.empty()) btn.attr("data-dcs-tip", tip);
    return btn;
}

void menu_label(View& view, std::string_view text, std::string_view key) {
    view.element(std::string_view{"div"}, "dcs-menu__label", key).text(text);
}

WidgetRef check_menu_item(View& view, std::string_view label,
                          std::string_view icon_name, std::string_view shortcut,
                          bool checked, std::string_view key) {
    auto item = view.menu_item_custom(key);
    auto ref = item.ref();
    {
        // The 16px icon gutter every desktop-menu row reserves.
        auto gutter = view.container("dcs-menu__icon", key_with(key, "icon"));
        if (!icon_name.empty()) {
            icon(view, icon_name, {}, key_with(key, "icon-glyph"));
        }
    }
    view.element("span", "dcs-menu__label-text", key_with(key, "label"))
        .text(label);
    if (!shortcut.empty()) {
        view.element("span", "dcs-menu__shortcut", key_with(key, "shortcut"))
            .text(shortcut);
    }
    if (checked) {
        // Trailing check mark (the web's checked-item shape).
        auto slot = view.container("dcs-menu__icon", key_with(key, "check"));
        icon(view, "check", {}, key_with(key, "check-glyph"));
    }
    return ref;
}

void submenu_stub(View& view, std::string_view label,
                  std::string_view icon_name, std::string_view key) {
    auto item = view.menu_item_custom(key);
    item.cls("dcs-menu__item dcs-menu__item--has-sub");
    {
        auto gutter = view.container("dcs-menu__icon", key_with(key, "icon"));
        if (!icon_name.empty()) {
            icon(view, icon_name, {}, key_with(key, "icon-glyph"));
        }
    }
    view.element("span", "dcs-menu__label-text", key_with(key, "label"))
        .text(label);
    {
        auto caret = view.container("dcs-menu__caret", key_with(key, "caret"));
        icon(view, "chevron-right", {}, key_with(key, "caret-glyph"));
    }
}

// ── Viewport overlay stack ───────────────────────────────────────────────────

void viewport_stats(View& view, const DenderDocument& doc) {
    auto stats = view.container("dn-vp-stats", "vp-stats");
    view.element("b", {}, "vp-stats-camera").text("User Perspective");
    view.element_ref("br", {}, "vp-stats-break");
    const SceneObject* active = doc.active();
    view.text("(1) Collection | " +
                  (active != nullptr ? active->name : std::string{"\xE2\x80\x94"}),
              "vp-stats-selection");
}

void viewport_corner(View& view, const DenderDocument& doc) {
    const SceneStats stats = scene_stats(doc);
    auto corner = view.container("dn-vp-corner", "vp-corner");
    view.text("Verts " + std::to_string(stats.verts) + " | Faces " +
                  std::to_string(stats.faces) + " | Tris " +
                  std::to_string(stats.tris) + " | Objects " +
                  std::to_string(stats.selected) + "/" +
                  std::to_string(stats.total),
              "vp-corner-text");
}

void tool_rail(View& view, std::string_view active_tool) {
    affineui::FloatingToolbarOptions opts;
    opts.vertical = true;
    opts.small = true;
    opts.drag_bounds = ".dn-vp-canvas";
    opts.position = "left:8px;top:8px";
    auto rail = view.floating_toolbar(opts, "vp-toolrail");
    rail.cls("dcs-toolbar dcs-toolbar--v dcs-toolbar--sm dcs-toolbar--floating "
             "dn-toolrail");

    struct Tool {
        const char* id;
        const char* icon;
        const char* tip;
        bool divider_before;
        bool radio;
    };
    static constexpr Tool kTools[] = {
        {"tweak", "cross-target", "Tweak", false, true},
        {"cursor", "pen", "Cursor", false, true},
        {"move", "move", "Move", true, true},
        {"rotate", "rotate", "Rotate", false, true},
        {"scale", "scale", "Scale", false, true},
        {"transform", "axes", "Transform", false, true},
        {"annotate", "pen", "Annotate", true, true},
        {"measure", "graph", "Measure", false, true},
        {"add-cube", "plus", "Add Cube", false, false},
    };
    for (const Tool& t : kTools) {
        if (t.divider_before) {
            view.container_ref("dcs-divider",
                               key_with("rail-div", t.id));
        }
        auto btn = icon_toggle(view, t.icon, t.radio && active_tool == t.id,
                               t.tip, key_with("rail", t.id), "dcs-btn--sm");
        if (t.radio) btn.attr("data-dcs-radio", "tool");
    }
}

void nav_cluster(View& view, bool move_view_pressed) {
    auto cluster = view.container("dn-navcluster", "vp-navcluster");
    {
        // Live axis nav ball: a custom-paint canvas the viewport redraws
        // on camera moves (web buildGizmo). Clicks on nubs are resolved by
        // DenderViewport from the gizmo bounds.
        auto gizmo = view.container("dn-gizmo", "vp-gizmo");
        gizmo.attr("data-dcs-tip", "Viewpoint");
        gizmo.attr("data-aui-name", "dn-vp-gizmo");
        auto canvas = view.canvas(kGizmoPaintName, {}, "vp-gizmo-canvas");
        canvas.attr("style",
                    "position:absolute;inset:0;width:100%;height:100%");
    }
    {
        auto btns = view.container("dn-navbtns", "vp-navbtns");
        struct Nav { const char* icon; const char* tip; const char* key;
                     bool pressed; };
        const Nav kNav[] = {
            {"search", "Zoom", "nav-zoom", false},
            {"move", "Move View", "nav-move", move_view_pressed},
            {"camera", "Camera View", "nav-camera", false},
            {"grid", "Toggle Perspective/Ortho", "nav-ortho", false},
        };
        for (const Nav& n : kNav) {
            icon_toggle(view, n.icon, n.pressed, n.tip, n.key, "dcs-btn--sm");
        }
    }
}

namespace {

// A labeled combo row for the N-panel (field label + bare drag-scrub combo).
void combo_field(View& view, std::string_view label, std::string_view tag,
                 double value, double step, std::string_view key) {
    auto field = view.container("dcs-field", key);
    text_span(view, label, "dcs-field__label", key_with(key, "label"));
    view.combo(tag, value, step, key_with(key, "combo"));
}

}  // namespace

void npanel(View& view, const DenderDocument& doc, std::string_view active_tab) {
    (void) doc;  // cosmetic per the web sample: fields show the static values
    auto panel =
        view.container("dcs-panel dcs-panel--floating dn-npanel", "vp-npanel");
    panel.attr("data-dcs-drag", "").attr("data-dcs-drag-bounds", ".dn-vp-canvas");
    auto pane = view.container("dcs-dockpane", "npanel-pane");
    {
        auto tabbar = view.container("dcs-dockpane__tabbar", "npanel-tabbar");
        tabbar.attr("data-dcs-drag-handle", "");
        auto tabs = view.container("dcs-dockpane__tabs", "npanel-tabs");
        struct Tab { const char* id; const char* icon; const char* label; };
        static constexpr Tab kTabs[] = {
            {"item", "cube", "Item"},
            {"tool", "axes", "Tool"},
            {"view", "camera", "View"},
        };
        for (const Tab& t : kTabs) {
            auto tab = view.element("button", "dcs-dockpane__tab",
                                    key_with("npanel-tab", t.id));
            tab.attr("type", "button")
                .attr("aria-selected", active_tab == t.id ? "true" : "false")
                .attr("data-dcs-target", "#npanel-" + std::string(t.id));
            icon(view, t.icon, {}, key_with("npanel-tab-icon", t.id));
            view.element("span", "dcs-dockpane__tab-label",
                         key_with("npanel-tab-label", t.id))
                .text(t.label);
        }
    }
    auto body = view.container("dcs-dockpane__body", "npanel-body");
    {
        auto item = view.container({}, "npanel-item");
        item.attr("id", "npanel-item").attr("data-dcs-tabpanel", "");
        if (active_tab != "item") item.attr("hidden", "");
        {
            auto fold = view.foldout("Transform", true, "npanel-fold-xform");
            auto props = view.container("dcs-props", "npanel-xform-props");
            view.vec("Location", {"X", "Y", "Z"}, {0.0, 0.0, 0.0}, "np-loc");
            view.vec("Dimensions", {"X", "Y", "Z"}, {2.0, 2.0, 2.0}, "np-dim");
        }
        {
            auto fold = view.foldout("View", true, "npanel-fold-view");
            auto props = view.container("dcs-props", "npanel-view-props");
            combo_field(view, "Focal", "Lens", 50.0, 0.5, "np-focal");
            combo_field(view, "Clip Start", {}, 0.1, 0.01, "np-clip");
            view.checkbox("Lock to Object", false, "np-lock");
            view.container_ref("dcs-divider", "np-view-div");
            view.button_group("Transform", {"Local", "Global"}, "Global",
                              "np-orient");
        }
    }
    {
        auto tool = view.container({}, "npanel-tool");
        tool.attr("id", "npanel-tool").attr("data-dcs-tabpanel", "");
        if (active_tab != "tool") tool.attr("hidden", "");
    }
    {
        auto vw = view.container({}, "npanel-view");
        vw.attr("id", "npanel-view").attr("data-dcs-tabpanel", "");
        if (active_tab != "view") vw.attr("hidden", "");
    }
}

// ── Timeline dopesheet body ──────────────────────────────────────────────────

namespace {

void timeline_track(View& view, std::string_view label, bool accent,
                    const Timeline& tl, const TimelineScale& scale,
                    double max_x, std::string_view key) {
    auto track = view.container("dn-tl-track", key);
    auto lbl = view.element("span", "dn-tl-track__label", key_with(key, "label"));
    lbl.text(label);
    if (accent) lbl.attr("style", "color:var(--dcs-accent)");
    int i = 0;
    for (const int frame : tl.keys) {
        const double x = scale.x(frame);
        if (x > max_x) continue;
        view.element_ref("span", "dn-key",
                         key_with(key, "key-" + std::to_string(i++)))
            .attr("style", "left:" + px(x));
    }
}

}  // namespace

void timeline_body(View& view, const DenderDocument& doc, int width_px) {
    const Timeline& tl = doc.timeline();
    // Before the first frame the window reports a degenerate size; lay the
    // ruler out for a plausible width and let the resize reload correct it.
    const double width = width_px >= 320 ? width_px : 1280.0;
    const auto scale = TimelineScale::for_width(width);
    const double max_x = width - 4.0;

    auto body = view.container("dn-timeline-body", "tl-body");

    // Playback-range tint behind ruler + tracks.
    {
        const double x0 = std::clamp(scale.x(tl.start), 0.0, max_x);
        const double x1 = std::clamp(scale.x(tl.end), x0, max_x);
        view.container_ref("dn-tl-range", "tl-range")
            .attr("style", "left:" + px(x0) + ";width:" + px(x1 - x0));
    }

    // Ruler: minor tick every 5 frames, major (with a frame label) every 10.
    {
        auto ruler = view.container("dn-tl-ruler", "tl-ruler");
        for (int f = 0;; f += 5) {
            const double x = scale.x(f);
            if (x > max_x) break;
            const bool major = f % 10 == 0;
            auto tick = view.container(
                major ? "dn-tl-tick dn-tl-tick--major" : "dn-tl-tick",
                "tl-tick-" + std::to_string(f));
            tick.attr("style", "left:" + px(x));
            if (major) {
                view.element("span", {}, "tl-tick-label-" + std::to_string(f))
                    .text(std::to_string(f));
            }
        }
    }

    // Tracks: Summary + the active object (the web's static "Cube" row).
    {
        auto tracks = view.container("dn-tl-tracks", "tl-tracks");
        const SceneObject* active = doc.active();
        timeline_track(view, "Summary", false, tl, scale, max_x, "tl-track-sum");
        timeline_track(view,
                       active != nullptr ? std::string_view{active->name}
                                         : std::string_view{"\xE2\x80\x94"},
                       true, tl, scale, max_x, "tl-track-obj");
    }

    // Playhead + frame flag.
    {
        auto playhead = view.container("dn-playhead", "tl-playhead");
        playhead.attr("style", "left:" + px(scale.x(tl.frame)));
        view.element("div", "dn-playhead__flag", "tl-playhead-flag")
            .text(std::to_string(tl.frame));
    }
}

}  // namespace dender::ui
