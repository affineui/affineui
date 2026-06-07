#pragma once

#include "dender_document.h"

#include <affineui/affineui.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dender::ui {

using BuildFn = std::function<void(affineui::View&)>;

struct MenuItem {
    std::string label;
    std::string icon;
    std::string shortcut;
    std::string value;
    bool separator_before{false};
    bool checked{false};
};

struct Menu {
    std::string id;
    std::string label;
    std::vector<MenuItem> items;
};

struct DockingPanel {
    std::string key;
    std::string classes;
    std::string title;
    std::string icon;
    std::string tabpanel_id;
};

struct Notification {
    std::string key;
    std::string tone;
    std::string title;
    std::string body;
};

void icon(affineui::View& view,
          std::string_view name,
          std::string_view classes = {},
          std::string_view key = {});
void text_span(affineui::View& view,
               std::string_view text,
               std::string_view classes = {},
               std::string_view key = {});

void app_title(affineui::View& view,
               std::string_view app_name,
               const DenderDocument& document);
void main_menu(affineui::View& view, const std::vector<Menu>& menus);
void menu_popup(affineui::View& view, const Menu& menu);
void toolbar(affineui::View& view,
             std::string_view classes,
             std::string_view key,
             const BuildFn& build);
void toolbar_separator(affineui::View& view, std::string_view key);
void toolbar_spacer(affineui::View& view, std::string_view key);
void icon_button(affineui::View& view,
                 std::string_view icon_name,
                 std::string_view key,
                 bool pressed = false,
                 std::string_view extra_classes = {});
void menu_button(affineui::View& view,
                 std::string_view label,
                 std::string_view menu_id,
                 std::string_view key);
void select_button(affineui::View& view,
                   std::string_view label,
                   std::string_view menu_id,
                   std::string_view key);

void docking_container(affineui::View& view,
                       std::string_view key,
                       const BuildFn& build);
void docking_row(affineui::View& view,
                 std::string_view key,
                 const BuildFn& build);
void docking_panel(affineui::View& view,
                   const DockingPanel& panel,
                   const BuildFn& toolbar_build,
                   const BuildFn& body_build);
void docking_panel_toolbar(affineui::View& view, const BuildFn& build);
void docking_splitter(affineui::View& view,
                      std::string_view key,
                      bool horizontal = false);

void tree_panel(affineui::View& view,
                const DenderDocument& document,
                const std::function<void(std::string_view)>& on_select);
void list_panel(affineui::View& view,
                std::string_view key,
                std::string_view title,
                const std::vector<std::string>& rows);
void document_panel(affineui::View& view,
                    const DenderDocument& document);
void modal_panel(affineui::View& view,
                 std::string_view key,
                 std::string_view title,
                 const BuildFn& body_build,
                 bool open = false);
void notification(affineui::View& view, const Notification& note);

}  // namespace dender::ui
