// command_panel - the smallest native demo of the command-tree widget API.
//
// This intentionally mirrors bindings/python/examples/hello_panel.py. The
// Python version also demonstrates rebuilding the view from callbacks; this
// C++ sample stays focused on the direct scoped declaration style.

#include <affineui/affineui.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

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

bool is_decius(affineui::ViewTheme style) {
    return style == affineui::ViewTheme::Decius;
}

void build_tabs(affineui::View& view, affineui::ViewTheme style) {
    const bool dcs = is_decius(style);
    auto controls = view.button("Controls", true, "tab-controls");
    auto fields = view.button("Fields", false, "tab-fields");
    auto list = view.button("List", false, "tab-list");
    if (dcs) {
        controls.cls("dcs-tab").attr("aria-selected", "true");
        fields.cls("dcs-tab").attr("aria-selected", "false");
        list.cls("dcs-tab").attr("aria-selected", "false");
    } else {
        controls.cls("btn btn-primary");
        fields.cls("btn btn-outline-secondary");
        list.cls("btn btn-outline-secondary");
    }
}

void build_controls(affineui::View& view) {
    view.button("Hello button", true, "hello-button")
        .on_click([] { std::puts("Hello button clicked"); });
    view.checkbox("Framework checkbox", true, "hello-check")
        .on_change([](std::string_view value) {
            std::printf("Checkbox changed: %.*s\n",
                        static_cast<int>(value.size()), value.data());
        });
    view.slider("Framework slider", 0.65, 0.0, 1.0, "hello-slider")
        .on_change([](std::string_view value) {
            std::printf("Slider changed: %.*s\n",
                        static_cast<int>(value.size()), value.data());
        });
    view.knob("Framework knob", 0.42, 0.0, 1.0, false, "hello-knob")
        .on_change([](std::string_view value) {
            std::printf("Knob changed: %.*s\n",
                        static_cast<int>(value.size()), value.data());
        });
}

void build_fields(affineui::View& view) {
    view.input("Object name", "Cylinder.042", "text", "object-name");
    view.password("Token", "secret", "token");
    view.input("Gain", "1.000", "number", "gain-field");
    view.dropdown("Mode", {"Object", "Edit", "Sculpt", "Render"},
                  "Object", "mode");
    view.button_group("Transform space", {"Local", "World", "View"},
                      "World", "space");
    view.textarea("Notes", "Dense native UI, browser semantics.", 3, "notes");
}

void build_list(affineui::View& view, affineui::ViewTheme style) {
    view.button("Append row", true, "append-row")
        .on_click([] { std::puts("Append row clicked"); });

    const auto list_classes = is_decius(style) ? "dcs-card-list" : "list-group";
    auto rows = view.container(list_classes, "event-list");
    const std::vector<std::string_view> titles{
        "Renderer warm",
        "Input route armed",
        "Patch graph clean",
    };
    for (std::size_t i = 0; i < titles.size(); ++i) {
        const bool selected = i == 0;
        auto row = view.button(titles[i], selected,
                               std::string{"log-row-"} + std::to_string(i));
        if (is_decius(style)) {
            row.cls("dcs-card dcs-card--clickable")
               .attr("aria-selected", selected ? "true" : "false");
        } else {
            row.cls(selected
                ? "list-group-item list-group-item-action active"
                : "list-group-item list-group-item-action");
        }
    }
}

affineui::View build_view(affineui::ViewTheme style) {
    affineui::View view{style};

    view.begin();
    {
        auto panel = view.panel("hello-panel");
        (void) panel;

        view.heading(1, "Hello from AffineUI", {}, "title");
        view.paragraph(
            "The same C++ command tree is rendered with Bootstrap or Decius selectors.",
            {},
            "lede");

        {
            auto tabs = view.container(is_decius(style) ? "dcs-tabs" : "btn-group",
                                       "tabs");
            (void) tabs;
            build_tabs(view, style);
        }

        {
            auto body = view.container(is_decius(style) ? "dcs-col"
                                                        : "d-flex flex-column gap-3",
                                       "tab-body");
            (void) body;
            build_controls(view);
            build_fields(view);
            build_list(view, style);
        }
    }
    view.end();

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
    config.height = 520;
    config.asset_folders = {"examples", "."};

    affineui::App app{config};
    app.load_view(build_view(style));
    return app.run();
}
