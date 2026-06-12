#include "dender_view.h"

#include "dender_components.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace dender {

namespace {

double parse_double_or(std::string_view value, double fallback) noexcept {
    // strtod, not std::from_chars: Apple's libc++ leaves the
    // floating-point from_chars overload `= delete`'d.
    if (value.empty()) return fallback;
    std::string tmp(value);
    char* end = nullptr;
    errno = 0;
    double out = std::strtod(tmp.c_str(), &end);
    return (end != tmp.c_str() && errno != ERANGE) ? out : fallback;
}

std::string number(double value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

std::string value_text(const PropertyValue& value) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, double>) {
            return number(v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return v;
        } else {
            return "(" + number(v.x) + ", " + number(v.y) + ", " +
                   number(v.z) + ")";
        }
    }, value);
}

std::vector<ui::Menu> app_menus() {
    return {
        {
            "dn-menu-file",
            "File",
            {
                {"New Scene", "file", "Ctrl N", "new"},
                {"Open Scene", "folder-open", "Ctrl O", "open"},
                {"Save Scene", "save", "Ctrl S", "save", true},
            },
        },
        {
            "dn-menu-edit",
            "Edit",
            {
                {"Undo", "undo", "Ctrl Z", "undo"},
                {"Redo", "redo", "Shift Ctrl Z", "redo"},
                {"Duplicate Objects", "", "Shift D", "duplicate", true},
            },
        },
        {
            "dn-menu-render",
            "Render",
            {
                {"Render Image", "render", "F12", "render-image"},
                {"Render Animation", "", "Ctrl F12", "render-animation"},
            },
        },
        {
            "dn-menu-workspace",
            "Workspace",
            {
                {"Layout", "", "", "layout", false, true},
                {"Modeling", "", "", "modeling"},
                {"Shading", "", "", "shading"},
                {"Animation", "", "", "animation"},
            },
        },
        {
            "dn-menu-mode",
            "Mode",
            {
                {"Object Mode", "cube", "", "object-mode", false, true},
                {"Edit Mode", "mesh", "", "edit-mode"},
                {"Sculpt Mode", "", "", "sculpt-mode"},
            },
        },
        {
            "dn-menu-engine",
            "Engine",
            {
                {"Eevee", "", "", "eevee"},
                {"Workbench", "", "", "workbench"},
                {"Cycles", "", "", "cycles", false, true},
            },
        },
    };
}

void graph_row(affineui::View& view,
               std::string_view label,
               std::string_view value,
               std::string_view key) {
    auto row = view.container("dn-graph-row", std::string(key));
    ui::text_span(view, label, "dn-graph-row__label",
                  std::string(key) + "-label");
    ui::text_span(view, value, "dn-graph-row__value",
                  std::string(key) + "-value");
}

}  // namespace

DenderView::DenderView(const DenderDocument& document, DenderActions actions)
    : document_(document), actions_(std::move(actions)) {}

affineui::View DenderView::build() const {
    affineui::View view{affineui::ViewTheme::Decius};
    view.selector(affineui::decius::selector::style,
                  affineui::decius::style::flat);
    view.selector(affineui::decius::selector::density,
                  affineui::decius::density::comfortable);
    view.selector(affineui::decius::selector::accent, "orange");

    view.begin();
    {
        auto app = view.container("dn-app", "dender");
        build_topbar(view);
        build_workarea(view);
        build_statusbar(view);
        build_menus(view);
        ui::modal_panel(
            view, "preferences-modal", "Preferences",
            [](affineui::View& v) {
                v.paragraph("Viewport and input preferences would live here.",
                            "dcs-note", "preferences-note");
            },
            false);
    }
    view.end();
    return view;
}

void DenderView::build_topbar(affineui::View& view) const {
    ui::toolbar(view, "dn-topbar", "topbar", [&](affineui::View& v) {
        ui::app_title(v, "DENDER", document_);
        ui::main_menu(v, app_menus());
        ui::toolbar_separator(v, "topbar-menu-sep");
        ui::select_button(v, "Layout", "dn-menu-workspace", "workspace-select");
        ui::toolbar_spacer(v, "topbar-spacer");
        ui::icon_button(v, "save", "save-scene");
        ui::icon_button(v, "cog", "preferences");
    });
}

void DenderView::build_workarea(affineui::View& view) const {
    ui::docking_container(view, "workarea", [&](affineui::View& v) {
        ui::docking_row(v, "main-row", [&](affineui::View& row) {
            build_outliner(row);
            ui::docking_splitter(row, "split-left-center");
            build_viewport(row);
            ui::docking_splitter(row, "split-center-right");
            build_inspector(row);
        });
        ui::docking_splitter(v, "split-main-timeline", true);
        build_timeline(v);
    });
}

void DenderView::build_outliner(affineui::View& view) const {
    ui::docking_panel(
        view,
        {"outliner", "dn-outliner", "Outliner", "folder-open",
         "dn-outliner-body"},
        [](affineui::View& v) {
            auto search = v.element_ref("input", "dcs-input", "outliner-search");
            search.attr("value", "")
                  .attr("placeholder", "Search")
                  .attr("style", "max-width:128px");
            ui::icon_button(v, "eq", "outliner-filter");
        },
        [&](affineui::View& v) {
            ui::tree_panel(v, document_, actions_.select_node);
            ui::list_panel(v, "geometry-stack", "Geometry Stack",
                           {"Mesh Primitive", "Bevel Node", "Weighted Normals"});
        });
}

void DenderView::build_viewport(affineui::View& view) const {
    ui::docking_panel(
        view,
        {"viewport", "dcs-dockpane--center dn-viewport", "3D Viewport",
         "cube", "dn-vp-body"},
        [](affineui::View& v) {
            ui::select_button(v, "Object Mode", "dn-menu-mode", "mode-select");
            ui::toolbar_separator(v, "viewport-mode-sep");
            ui::menu_button(v, "View", "dn-menu-render", "viewport-view-menu");
            ui::menu_button(v, "Select", "dn-menu-edit", "viewport-select-menu");
            ui::menu_button(v, "Add", "dn-menu-file", "viewport-add-menu");
            ui::toolbar_spacer(v, "viewport-toolbar-spacer");
            ui::icon_button(v, "gizmo", "viewport-gizmo", true);
            ui::icon_button(v, "view-bbox", "viewport-bbox", true);
            ui::select_button(v, "Cycles", "dn-menu-engine", "engine-select");
        },
        [&](affineui::View& v) {
            ui::document_panel(v, document_);
            ui::notification(v, {
                "graph-notification",
                "info",
                "Dependency Graph",
                "Transform and material properties are evaluated into viewport state.",
            });
        });
}

void DenderView::build_inspector(affineui::View& view) const {
    const auto location = document_.location();
    ui::docking_panel(
        view,
        {"inspector", "dn-inspector", "Inspector", "cog",
         "dn-inspector-body"},
        {},
        [&](affineui::View& v) {
            v.heading(2, document_.selected_object(), "dcs-panel__title",
                      "inspector-title");
            v.input("Name", document_.selected_object(), "text", "object-name")
                .on_change([actions = actions_](std::string_view value) {
                    if (actions.rename_selected) actions.rename_selected(value);
                    if (actions.reload) actions.reload();
                });
            v.input("Tint", document_.tint(), "color", "object-tint")
                .on_change([actions = actions_](std::string_view value) {
                    if (actions.set_tint) actions.set_tint(value);
                    if (actions.reload) actions.reload();
                });
            v.slider("Roughness", document_.roughness(), 0.0, 1.0, "roughness")
                .on_change([actions = actions_](std::string_view value) {
                    if (actions.set_roughness) {
                        actions.set_roughness(parse_double_or(value, 0.42));
                    }
                    if (actions.reload) actions.reload();
                });
            v.input("Location X", number(location.x), "number", "loc-x")
                .on_change([actions = actions_](std::string_view value) {
                    if (actions.set_location_x) {
                        actions.set_location_x(parse_double_or(value, 0.0));
                    }
                    if (actions.reload) actions.reload();
                });
            v.input("Location Y", number(location.y), "number", "loc-y")
                .on_change([actions = actions_](std::string_view value) {
                    if (actions.set_location_y) {
                        actions.set_location_y(parse_double_or(value, 0.0));
                    }
                    if (actions.reload) actions.reload();
                });
            v.input("Location Z", number(location.z), "number", "loc-z")
                .on_change([actions = actions_](std::string_view value) {
                    if (actions.set_location_z) {
                        actions.set_location_z(parse_double_or(value, 0.0));
                    }
                    if (actions.reload) actions.reload();
                });
            v.dropdown("Modifier", {"Subdivision", "Bevel", "Mirror", "Solidify"},
                       "Subdivision", "modifier");
            v.button_group("Space", {"Local", "Global", "View"}, "Global",
                           "space");
            v.checkbox("Show wireframe overlay", document_.show_wireframe(),
                       "show-wire")
                .on_change([actions = actions_](std::string_view value) {
                    if (actions.set_show_wireframe) {
                        actions.set_show_wireframe(parse_bool(value));
                    }
                    if (actions.reload) actions.reload();
                });
            {
                auto row = v.container("dcs-btn-row", "inspector-actions");
                v.button("Apply", true, "apply").on_click([] {
                    std::puts("Apply clicked");
                });
                v.button("Reset", false, "reset").on_click([actions = actions_] {
                    if (actions.reset_selection) actions.reset_selection();
                    if (actions.reload) actions.reload();
                });
            }
            build_graph_readout(v);
        });
}

void DenderView::build_timeline(affineui::View& view) const {
    ui::docking_panel(
        view,
        {"timeline", "dn-timeline", "Timeline", "timeline",
         "dn-timeline-body"},
        [](affineui::View& v) {
            ui::icon_button(v, "play", "timeline-play");
            ui::icon_button(v, "keyframe", "timeline-keyframe");
        },
        [&](affineui::View& v) {
            auto track = v.container("dn-timeline-track", "timeline-track");
            track.attr("data-dcs-tabpanel", "");
            v.container_ref("dn-timeline-ruler", "timeline-ruler")
                .text("1  24  48  72  96  120");
            v.container_ref("dn-playhead", "timeline-playhead");
            for (std::size_t i = 0; i < document_.timeline_keys().size(); ++i) {
                const auto& key = document_.timeline_keys()[i];
                v.element_ref("span", "dn-key",
                              "timeline-key-" + std::to_string(i))
                    .attr("style", "left:" + number(key.position_percent) + "%");
            }
        });
}

void DenderView::build_statusbar(affineui::View& view) const {
    auto status = view.container("dcs-statusbar", "statusbar");
    {
        auto ready = view.element("span",
                                  "dcs-statusbar__item dcs-statusbar__item--ok",
                                  "status-ready");
        ui::icon(view, "check-circle", {}, "status-ready-icon");
        view.text(" Ready", "status-ready-text");
    }
    ui::text_span(view, "native C++ mini DCC", "dcs-statusbar__item",
                  "status-mode");
    view.element_ref("span", "dcs-statusbar__spacer", "status-spacer");
    ui::text_span(view, "document/view + dependency graph",
                  "dcs-statusbar__item dcs-statusbar__item--accent",
                  "status-features");
}

void DenderView::build_menus(affineui::View& view) const {
    for (const auto& menu : app_menus()) {
        ui::menu_popup(view, menu);
    }
}

void DenderView::build_graph_readout(affineui::View& view) const {
    auto readout = view.container("dn-graph-readout", "graph-readout");
    for (const auto& property : document_.graph().properties()) {
        if (property.input) continue;
        const auto* value = document_.graph().value(property.id);
        if (!value) continue;
        graph_row(view, property.label, value_text(*value),
                  "graph-" + property.id);
    }
}

}  // namespace dender
