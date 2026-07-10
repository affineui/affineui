#pragma once

// DENDER view — declares the whole UI from document + ui state each rebuild:
// topbar (logo / menubar / pickers), the declarative dock workarea (viewport
// document pane, Outliner + Inspector right column, Timeline bottom),
// statusbar, the shared popup menus, and the theme-tweaks popover. All
// handlers bind back into DenderApp.

#include "dender_app.h"

#include <affineui/affineui.h>

#include <string_view>

namespace dender {

class DenderView {
public:
    explicit DenderView(DenderApp& app);

    [[nodiscard]] affineui::View build() const;

    /// Declare the UI into a caller-owned View (the App::set_view persistent
    /// reconcile path — the App brackets this with begin(sink)/end()).
    void build_into(affineui::View& v) const;

private:
    void build_topbar(affineui::View& v) const;
    void build_workarea(affineui::View& v) const;

    void build_viewport_toolbar(affineui::View& v) const;
    void build_viewport_body(affineui::View& v) const;

    void build_outliner_toolbar(affineui::View& v) const;
    void build_outliner_body(affineui::View& v) const;

    void build_inspector_toolbar(affineui::View& v) const;
    void build_inspector_body(affineui::View& v) const;
    void build_inspector_object_tab(affineui::View& v) const;
    void build_inspector_static_tab(affineui::View& v,
                                    std::string_view id) const;
    /// Live Location/Rotation/Scale vecs over the active object's e3d node,
    /// shared by the main inspector and the Item N-panel. `key_prefix`
    /// disambiguates widget names between the two hosts.
    void build_transform_vecs(affineui::View& v, std::string_view id,
                              std::string_view key_prefix) const;
    /// The Item N-panel mini-inspector: live Transform + base (unscaled)
    /// Dimensions.
    void build_npanel_item_body(affineui::View& v) const;

    void build_timeline_toolbar(affineui::View& v) const;
    void build_timeline_body(affineui::View& v) const;

    void build_statusbar(affineui::View& v) const;
    void build_shared_menus(affineui::View& v) const;
    void build_tweaks_popover(affineui::View& v) const;

    DenderApp& app_;
    const app::Document& doc_;
    const UiState& ui_;
};

}  // namespace dender
