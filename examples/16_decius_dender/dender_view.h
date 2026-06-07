#pragma once

#include "dender_document.h"

#include <affineui/affineui.h>

#include <functional>
#include <string_view>

namespace dender {

struct DenderActions {
    std::function<void()> reload;
    std::function<void(std::string_view)> select_node;
    std::function<void(std::string_view)> rename_selected;
    std::function<void(std::string_view)> set_tint;
    std::function<void(double)> set_roughness;
    std::function<void(double)> set_location_x;
    std::function<void(double)> set_location_y;
    std::function<void(double)> set_location_z;
    std::function<void(bool)> set_show_wireframe;
    std::function<void()> reset_selection;
};

class DenderView {
public:
    DenderView(const DenderDocument& document, DenderActions actions);

    [[nodiscard]] affineui::View build() const;

private:
    void build_topbar(affineui::View& view) const;
    void build_workarea(affineui::View& view) const;
    void build_outliner(affineui::View& view) const;
    void build_viewport(affineui::View& view) const;
    void build_inspector(affineui::View& view) const;
    void build_timeline(affineui::View& view) const;
    void build_statusbar(affineui::View& view) const;
    void build_menus(affineui::View& view) const;
    void build_graph_readout(affineui::View& view) const;

    const DenderDocument& document_;
    DenderActions actions_;
};

}  // namespace dender
