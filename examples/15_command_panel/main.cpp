// command_panel - the smallest native demo of the command-tree widget API.
//
// This intentionally mirrors bindings/python/examples/hello_panel.py. The
// Python version also demonstrates rebuilding the view from callbacks; this
// C++ sample stays focused on the direct scoped declaration style.

#include <affineui/affineui.h>
#include <affineui/log.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
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

bool high_dpi_from_args(int argc, char** argv) {
    bool high_dpi = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        std::string_view value;
        if (arg == "--dpi" && i + 1 < argc) {
            value = argv[++i];
        } else if (arg.starts_with("--dpi=")) {
            value = arg.substr(6);
        }

        if (value == "retina" || value == "high" || value == "hidpi") {
            high_dpi = true;
        } else if (value == "normal" || value == "1x") {
            high_dpi = false;
        }
    }
    return high_dpi;
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
        controls.cls("nav-link active");
        fields.cls("nav-link");
        list.cls("nav-link");
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
    view.input("Object name", "Cylinder.042", "text", "object-name")
        .on_change([](std::string_view value) {
            std::printf("Name changed: %.*s\n",
                        static_cast<int>(value.size()), value.data());
        });
    view.password("Token", "secret", "token")
        .on_change([](std::string_view value) {
            std::printf("Token changed: %.*s\n",
                        static_cast<int>(value.size()), value.data());
        });
    view.input("Gain", "1.000", "number", "gain-field")
        .on_change([](std::string_view value) {
            std::printf("Gain changed: %.*s\n",
                        static_cast<int>(value.size()), value.data());
        });
    view.slider("Rotation", 45.0, -180.0, 180.0, "rotation-degrees")
        .on_change([](std::string_view value) {
            std::printf("Rotation changed: %.*s\n",
                        static_cast<int>(value.size()), value.data());
        });
    view.dropdown("Mode", {"Object", "Edit", "Sculpt", "Render"},
                  "Object", "mode")
        .on_change([](std::string_view value) {
            std::printf("Mode changed: %.*s\n",
                        static_cast<int>(value.size()), value.data());
        });
    view.button_group("Transform space", {"Local", "World", "View"},
                      "World", "space")
        .on_change([](std::string_view value) {
            std::printf("Space changed: %.*s\n",
                        static_cast<int>(value.size()), value.data());
        });
    view.textarea("Notes", "Dense native UI, browser semantics.", 3, "notes")
        .on_change([](std::string_view value) {
            std::printf("Notes changed: %.*s\n",
                        static_cast<int>(value.size()), value.data());
        });
}

void build_list(affineui::View& view, affineui::ViewTheme style) {
    view.button("Append row", true, "append-row")
        .on_click([] { std::puts("Append row clicked"); });

    const auto list_classes = is_decius(style)
        ? affineui::decius::class_name::list
        : affineui::bootstrap::class_name::list;
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
            row.cls(affineui::decius::class_name::list_item)
               .attr("aria-selected", selected ? "true" : "false");
        } else {
            row.cls(selected
                ? "list-group-item list-group-item-action active"
                : "list-group-item list-group-item-action");
        }
    }
}

void build_view_into(affineui::View& view, affineui::ViewTheme style) {
    view.set_theme(style);
    {
        auto panel = view.panel("hello-panel");
        (void) panel;

        view.heading(1, "Hello from AffineUI", {}, "title");
        view.paragraph(
            "The same C++ command tree is rendered with Bootstrap or Decius selectors.",
            {},
            "lede");

        {
            auto tabs = view.container(is_decius(style) ? "dcs-tabs" : "nav nav-tabs",
                                       "tabs");
            (void) tabs;
            build_tabs(view, style);
        }

        {
            auto controls = view.container(
                is_decius(style) ? affineui::decius::class_name::form
                                 : affineui::bootstrap::class_name::form,
                "controls-body");
            (void) controls;
            build_controls(view);
        }

        {
            auto fields = view.container(
                is_decius(style) ? affineui::decius::class_name::props
                                 : affineui::bootstrap::class_name::props,
                "fields-body");
            (void) fields;
            build_fields(view);
        }

        {
            auto list = view.container(
                is_decius(style) ? affineui::decius::class_name::column
                                 : affineui::bootstrap::class_name::column,
                "list-body");
            (void) list;
            build_list(view, style);
        }
    }
}

affineui::View build_view(affineui::ViewTheme style) {
    affineui::View view{style};
    view.begin();
    build_view_into(view, style);
    view.end();
    return view;
}

}  // namespace

int main(int argc, char** argv) {
    const auto style = style_from_args(argc, argv);
    const bool high_dpi = high_dpi_from_args(argc, argv);

    affineui::App::Config config;
    config.title = "AffineUI - command panel (";
    config.title += style_name(style);
    config.title += ")";
    config.width = 720;
    config.height = 520;
    config.asset_folders = {"examples", "."};
    config.high_dpi = high_dpi;

    affineui::App app{config};
    app.load_view(build_view(style));
    // AFT_LOG_DEMO=1: emit a periodic diagnostic through affineui::log so a
    // developer can watch the affinetools log panel (frame-stamped lines,
    // click-to-select). Off unless the env var is set — the app is silent
    // by default. Handy for demoing/verifying the tools; costs nothing when
    // unset.
    if (std::getenv("AFT_LOG_DEMO") != nullptr) {
        auto counter = std::make_shared<int>(0);
        auto accum = std::make_shared<double>(0.0);
        app.on_frame([counter, accum](double dt) {
            *accum += dt;
            if (*accum >= 0.4) {
                *accum = 0.0;
                ++*counter;
                affineui::LogLevel lvl =
                    (*counter % 7 == 0)   ? affineui::LogLevel::error
                    : (*counter % 3 == 0) ? affineui::LogLevel::warn
                                          : affineui::LogLevel::info;
                affineui::log(lvl, "demo diagnostic #" +
                                       std::to_string(*counter));
            }
        });
    }
    return app.run();
}
