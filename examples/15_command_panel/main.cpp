// command_panel - the smallest native demo of the command-tree widget API.
//
// This intentionally mirrors bindings/python/examples/hello_panel.py:
// create a named panel, inject children with WidgetRef::replace(), then
// inflate the View through App::load_view().

#include <affineui/affineui.h>

#include <string_view>

namespace {

affineui::ViewTheme style_from_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        std::string_view value;
        if (arg == "--style" && i + 1 < argc) {
            value = argv[++i];
        } else if (arg.starts_with("--style=")) {
            value = arg.substr(8);
        }

        if (value == "decius") {
            return affineui::ViewTheme::Decius;
        }
        if (value == "bootstrap") {
            return affineui::ViewTheme::Bootstrap;
        }
    }
    return affineui::ViewTheme::Bootstrap;
}

std::string_view style_name(affineui::ViewTheme style) {
    switch (style) {
        case affineui::ViewTheme::Decius:    return "decius";
        case affineui::ViewTheme::Bootstrap: return "bootstrap";
        case affineui::ViewTheme::Plain:     return "plain";
    }
    return "bootstrap";
}

affineui::View build_view(affineui::ViewTheme style) {
    affineui::View view{style};

    view.begin();
    auto panel = view.panel_ref("hello-panel");
    view.end();

    panel.replace([](affineui::View& v) {
        v.heading(1, "Hello from AffineUI", {}, "title");
        v.paragraph(
            "This panel is built from C++ commands and rendered by the "
            "native AffineUI engine using framework selectors.",
            {},
            "lede");
        v.button("Hello button", true, "hello-button");
        v.checkbox("Framework checkbox", true, "hello-check");
        v.slider("Framework slider", 0.65, 0.0, 1.0, "hello-slider");
    });

    return view;
}

}  // namespace

int main(int argc, char** argv) {
    const auto style = style_from_args(argc, argv);

    affineui::App::Config config;
    config.title = "AffineUI - command panel (";
    config.title += style_name(style);
    config.title += ")";
    config.width = 720;
    config.height = 420;

    affineui::App app{config};
    app.load_view(build_view(style));
    return app.run();
}
