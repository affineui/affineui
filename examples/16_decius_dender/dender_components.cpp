#include "dender_components.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace dender::ui {

namespace {

std::string pct(double value) {
    std::ostringstream out;
    out << value << "%";
    return out.str();
}

std::string key_with(std::string_view prefix, std::string_view suffix) {
    std::string out{prefix};
    out += "-";
    out += suffix;
    return out;
}

std::string classes(std::string_view base, std::string_view extra) {
    std::string out{base};
    if (!extra.empty()) {
        if (!out.empty()) out += ' ';
        out += extra;
    }
    return out;
}

void button_base(affineui::View& view,
                 std::string_view classes,
                 std::string_view key,
                 const BuildFn& build) {
    auto button = view.element("button", classes, key);
    button.attr("type", "button");
    if (build) build(view);
}

}  // namespace

void icon(affineui::View& view,
          std::string_view name,
          std::string_view classes,
          std::string_view key) {
    auto node = view.element_ref("i", "di di-" + std::string(name), key);
    if (!classes.empty()) {
        node.cls("di di-" + std::string(name) + " " + std::string(classes));
    }
}

void text_span(affineui::View& view,
               std::string_view text,
               std::string_view classes,
               std::string_view key) {
    auto span = view.element_ref("span", classes, key);
    span.text(text);
}

void app_title(affineui::View& view,
               std::string_view app_name,
               const DenderDocument& document) {
    auto title = view.container("dn-app-title", "app-title");
    {
        auto logo = view.container("dn-logo", "app-title-logo");
        icon(view, "decius", "dn-logo__mark", "app-logo-mark");
        text_span(view, app_name, "dn-logo__name", "app-logo-name");
    }
    text_span(view, document.title(), "dn-document-title", "document-title");
    if (document.dirty()) {
        text_span(view, "modified", "dn-document-title__dirty", "document-dirty");
    }
}

void main_menu(affineui::View& view, const std::vector<Menu>& menus) {
    auto nav = view.element("nav", "dcs-menubar", "main-menu");
    for (const auto& menu : menus) {
        menu_button(view, menu.label, menu.id, key_with("main-menu", menu.id));
    }
}

void menu_popup(affineui::View& view, const Menu& menu) {
    auto popup = view.container("dcs-menu", menu.id);
    popup.attr("id", menu.id).attr("hidden", "");
    if (!menu.label.empty()) {
        auto label = view.container_ref("dcs-menu__label", key_with(menu.id, "label"));
        label.text(menu.label);
    }
    for (std::size_t i = 0; i < menu.items.size(); ++i) {
        const auto& item = menu.items[i];
        if (item.separator_before) {
            view.container_ref("dcs-menu__sep",
                               key_with(menu.id, "sep-" + std::to_string(i)));
        }

        auto row = view.element(
            "button",
            "dcs-menu__item",
            key_with(menu.id, "item-" + std::to_string(i)));
        row.attr("type", "button");
        if (!item.value.empty()) row.attr("data-dcs-value", item.value);

        {
            auto icon_slot = view.container(
                "dcs-menu__icon",
                key_with(menu.id, "icon-" + std::to_string(i)));
            if (!item.icon.empty()) {
                icon(view, item.icon, {},
                     key_with(menu.id, "icon-glyph-" + std::to_string(i)));
            }
        }

        text_span(view, item.label, "dcs-menu__label-text",
                  key_with(menu.id, "text-" + std::to_string(i)));
        if (!item.shortcut.empty()) {
            text_span(view, item.shortcut, "dcs-menu__shortcut",
                      key_with(menu.id, "shortcut-" + std::to_string(i)));
        }
        if (item.checked) {
            auto check = view.container("dcs-menu__check",
                                        key_with(menu.id, "check-" + std::to_string(i)));
            icon(view, "check", {}, key_with(menu.id, "check-icon-" + std::to_string(i)));
        }
    }
}

void toolbar(affineui::View& view,
             std::string_view toolbar_classes,
             std::string_view key,
             const BuildFn& build) {
    auto bar = view.container(classes("dcs-toolbar", toolbar_classes), key);
    if (build) build(view);
}

void toolbar_separator(affineui::View& view, std::string_view key) {
    view.element_ref("span", "dcs-toolbar__sep", key);
}

void toolbar_spacer(affineui::View& view, std::string_view key) {
    view.element_ref("span", "dcs-toolbar__spacer", key);
}

void icon_button(affineui::View& view,
                 std::string_view icon_name,
                 std::string_view key,
                 bool pressed,
                 std::string_view extra_classes) {
    button_base(view, classes("dcs-btn dcs-btn--icon dcs-btn--ghost", extra_classes),
                key, [&](affineui::View& v) {
        icon(v, icon_name, {}, key_with(key, "icon"));
    });
    if (pressed) view.find_widget(key).attr("aria-pressed", "true");
}

void menu_button(affineui::View& view,
                 std::string_view label,
                 std::string_view menu_id,
                 std::string_view key) {
    button_base(view, "dcs-menubar__item", key, [&](affineui::View& v) {
        v.text(label, key_with(key, "text"));
    });
    view.find_widget(key)
        .attr("data-dcs-toggle", "menu")
        .attr("data-dcs-target", "#" + std::string(menu_id));
}

void select_button(affineui::View& view,
                   std::string_view label,
                   std::string_view menu_id,
                   std::string_view key) {
    button_base(view, "dcs-select dcs-select--btn", key, [&](affineui::View& v) {
        text_span(v, label, "dcs-select__label", key_with(key, "label"));
        auto caret = v.container("dcs-select__caret", key_with(key, "caret"));
        icon(v, "chevron-down", {}, key_with(key, "caret-icon"));
    });
    view.find_widget(key)
        .attr("data-dcs-toggle", "menu")
        .attr("data-dcs-target", "#" + std::string(menu_id));
}

void docking_container(affineui::View& view,
                       std::string_view key,
                       const BuildFn& build) {
    auto dock = view.container("dcs-dock dcs-dock--v dn-workarea", key);
    if (build) build(view);
}

void docking_row(affineui::View& view,
                 std::string_view key,
                 const BuildFn& build) {
    auto row = view.container("dn-mainrow", key);
    if (build) build(view);
}

void docking_panel(affineui::View& view,
                   const DockingPanel& panel,
                   const BuildFn& toolbar_build,
                   const BuildFn& body_build) {
    auto pane = view.container(classes("dcs-dockpane", panel.classes), panel.key);
    {
        auto tabbar = view.container("dcs-dockpane__tabbar",
                                     key_with(panel.key, "tabbar"));
        {
            auto tabs = view.container("dcs-dockpane__tabs",
                                       key_with(panel.key, "tabs"));
            auto tab = view.element("button", "dcs-dockpane__tab",
                                    key_with(panel.key, "tab"));
            tab.attr("type", "button")
               .attr("aria-selected", "true")
               .attr("data-dcs-target", "#" + panel.tabpanel_id);
            if (!panel.icon.empty()) {
                icon(view, panel.icon, {}, key_with(panel.key, "tab-icon"));
                view.text(" ", key_with(panel.key, "tab-gap"));
            }
            view.text(panel.title, key_with(panel.key, "tab-title"));
        }
        if (toolbar_build) {
            auto toolbars = view.container("dcs-dockpane__toolbars",
                                           key_with(panel.key, "toolbars"));
            docking_panel_toolbar(view, toolbar_build);
        }
    }
    {
        auto body = view.container("dcs-dockpane__body", panel.tabpanel_id);
        body.attr("id", panel.tabpanel_id);
        if (body_build) body_build(view);
    }
}

void docking_panel_toolbar(affineui::View& view, const BuildFn& build) {
    auto bar = view.container("dcs-dockpane__toolbar", "dock-panel-toolbar");
    if (build) build(view);
}

void docking_splitter(affineui::View& view,
                      std::string_view key,
                      bool horizontal) {
    auto splitter = view.container_ref(
        horizontal ? "dcs-splitter dcs-splitter--h" : "dcs-splitter",
        key);
    splitter.attr("data-dcs-splitter", horizontal ? "h" : "");
}

void tree_panel(affineui::View& view,
                const DenderDocument& document,
                const std::function<void(std::string_view)>& on_select) {
    auto tree = view.container("dcs-tree", "outliner-tree");
    tree.attr("data-dcs-select", "multi");

    for (const auto& node : document.scene_nodes()) {
        const bool selected = document.selected_node_id() == node.id;
        auto row = view.button(node.label, selected, "outliner-" + node.id);
        row.cls("dcs-tree__row")
           .attr("style", "width:100%;--depth:" + std::to_string(node.depth))
           .attr("aria-selected", selected ? "true" : "false");
        if (node.branch) row.attr("aria-expanded", "true");
        row.on_click([on_select, id = node.id] {
            if (on_select) on_select(id);
        });
    }
}

void list_panel(affineui::View& view,
                std::string_view key,
                std::string_view title,
                const std::vector<std::string>& rows) {
    auto panel = view.container("dn-list-panel", key);
    view.heading(3, title, "dn-list-panel__title", key_with(key, "title"));
    auto list = view.container("dcs-list", key_with(key, "list"));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto row = view.button(rows[i], i == 0,
                               key_with(key, "row-" + std::to_string(i)));
        row.cls("dcs-list__item")
           .attr("aria-selected", i == 0 ? "true" : "false");
    }
}

void document_panel(affineui::View& view,
                    const DenderDocument& document) {
    const auto evaluated = document.evaluate_scene();
    auto canvas = view.container("dn-vp-canvas", "document-canvas");
    canvas.attr("data-dcs-tabpanel", "")
          .attr("data-dcs-float-host", "");

    view.container_ref("dn-vp-grid", "viewport-grid");
    if (evaluated.show_wireframe) {
        view.container_ref("dn-vp-wire", "viewport-wire-overlay");
    }
    auto cube = view.container_ref("dn-cube", "viewport-cube");
    cube.attr("style", evaluated.cube_style);

    {
        auto stats = view.container("dn-vp-stats", "viewport-stats");
        view.element_ref("b", {}, "viewport-camera-label").text("User Perspective");
        view.element_ref("br", {}, "viewport-stats-break");
        view.text("(1) Collection | " + std::string(document.selected_object()),
                  "viewport-selection-label");
    }

    {
        auto corner = view.container("dn-vp-corner", "viewport-corner");
        view.text("Verts 8 | Faces 6 | Tris 12 | Objects 3/3 | Eval clean",
                  "viewport-mesh-stats");
    }

    toolbar(view, "dcs-toolbar--v dcs-toolbar--sm dcs-toolbar--floating dn-toolrail",
            "viewport-toolrail", [&](affineui::View& v) {
        v.element_ref("span", "dcs-grip dcs-grip--h", "toolrail-grip")
            .attr("data-dcs-drag-handle", "");
        icon_button(v, "cross-target", "tool-select", true, "dcs-btn--sm");
        icon_button(v, "move", "tool-move", false, "dcs-btn--sm");
        icon_button(v, "rotate", "tool-rotate", false, "dcs-btn--sm");
        icon_button(v, "scale", "tool-scale", false, "dcs-btn--sm");
    });
    view.find_widget("viewport-toolrail").attr("data-dcs-drag-bounds", ".dn-vp-canvas");

    auto panel = view.container("dcs-panel dcs-panel--floating dn-npanel",
                                "viewport-n-panel");
    panel.attr("data-dcs-drag-bounds", ".dn-vp-canvas");
    docking_panel(
        view,
        {"n-panel", "", "Item", "", "dn-npanel-item"},
        {},
        [&](affineui::View& v) {
            auto props = v.container("dcs-props", "n-panel-props");
            v.input("Lens", "50", "number", "camera-lens");
            v.input("Clip", "0.1", "number", "camera-clip");
            v.input("Cube X", pct(evaluated.cube_left_percent), "text",
                    "computed-cube-x");
        });
}

void modal_panel(affineui::View& view,
                 std::string_view key,
                 std::string_view title,
                 const BuildFn& body_build,
                 bool open) {
    auto modal = view.container("dcs-modal dn-modal", key);
    if (!open) modal.attr("hidden", "");
    auto panel = view.container("dcs-panel dn-modal__panel", key_with(key, "panel"));
    view.heading(2, title, "dcs-panel__title", key_with(key, "title"));
    if (body_build) body_build(view);
}

void notification(affineui::View& view, const Notification& note) {
    auto toast = view.container(std::string{"dn-notification dn-notification--"} +
                                    note.tone,
                                note.key);
    view.heading(3, note.title, "dn-notification__title",
                 key_with(note.key, "title"));
    view.paragraph(note.body, "dn-notification__body", key_with(note.key, "body"));
}

}  // namespace dender::ui
