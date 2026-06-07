#include "dender_app.h"

#include "dender_styles.h"
#include "dender_view.h"

namespace dender {

DenderApp::DenderApp()
    : app_(make_config()) {}

int DenderApp::run() {
    reload();
    return app_.run();
}

void DenderApp::reload() {
    DenderView view{document_, make_actions()};
    app_.load_view(view.build());
    app_.set_stylesheet(native_css());
}

affineui::App::Config DenderApp::make_config() {
    affineui::App::Config cfg;
    cfg.title = "AffineUI - DENDER mini DCC";
    cfg.width = 1280;
    cfg.height = 820;
    cfg.clear_color = affineui::Color{31, 34, 42, 255};
    cfg.high_dpi = true;
    cfg.asset_folders = {"examples"};
    return cfg;
}

DenderActions DenderApp::make_actions() {
    DenderActions actions;
    actions.reload = [this] { reload(); };
    actions.select_node = [this](std::string_view id) {
        document_.select_node(id);
        reload();
    };
    actions.rename_selected = [this](std::string_view value) {
        document_.rename_selected(value);
    };
    actions.set_tint = [this](std::string_view value) {
        document_.set_tint(value);
    };
    actions.set_roughness = [this](double value) {
        document_.set_roughness(value);
    };
    actions.set_location_x = [this](double value) {
        document_.set_location_x(value);
    };
    actions.set_location_y = [this](double value) {
        document_.set_location_y(value);
    };
    actions.set_location_z = [this](double value) {
        document_.set_location_z(value);
    };
    actions.set_show_wireframe = [this](bool value) {
        document_.set_show_wireframe(value);
    };
    actions.reset_selection = [this] {
        document_.reset_selection();
    };
    return actions;
}

}  // namespace dender
