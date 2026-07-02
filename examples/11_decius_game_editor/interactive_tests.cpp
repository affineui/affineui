#include "interactive_tests.h"

#include "game_editor_styles.h"

#include "affineui_app.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ge {
namespace {

constexpr const char* kDeciusVersion = "0.6.2";

bool truthy(std::string_view value) {
    return value == "true" || value == "1" || value == "on";
}

class SetBoolProperty final : public app::Command {
public:
    SetBoolProperty(std::string id, std::string prop, bool value)
        : Command("obj.setBool", "Set Bool"),
          id_(std::move(id)),
          prop_(std::move(prop)),
          next_(value) {}

    void redo(app::Document& doc) override {
        previous_ = std::get<bool>(
            doc.property(id_, prop_, app::PropValue{next_}));
        doc.set_property(id_, prop_, app::PropValue{next_});
    }

    void undo(app::Document& doc) override {
        doc.set_property(id_, prop_, app::PropValue{previous_});
    }

private:
    std::string id_;
    std::string prop_;
    bool        next_{false};
    bool        previous_{false};
};

class SetStringProperty final : public app::Command {
public:
    SetStringProperty(std::string id, std::string prop, std::string value)
        : Command("obj.setString", "Set String"),
          id_(std::move(id)),
          prop_(std::move(prop)),
          next_(std::move(value)) {}

    void redo(app::Document& doc) override {
        previous_ = std::get<std::string>(
            doc.property(id_, prop_, app::PropValue{next_}));
        doc.set_property(id_, prop_, app::PropValue{next_});
    }

    void undo(app::Document& doc) override {
        doc.set_property(id_, prop_, app::PropValue{previous_});
    }

private:
    std::string id_;
    std::string prop_;
    std::string next_;
    std::string previous_;
};

affineui::App::Config lab_config(std::string_view title,
                                 std::function<void()> on_layout_changed) {
    affineui::App::Config cfg;
    cfg.title = "AffineUI Decius interactive test - " + std::string(title);
    cfg.width = 780;
    cfg.height = 620;
    cfg.clear_color = affineui::Color{0x14, 0x16, 0x1c, 0xff};
    cfg.high_dpi = true;
    cfg.asset_folders = {".", "examples"};
    cfg.on_layout_changed = std::move(on_layout_changed);
    return cfg;
}

void install_decius_styles(affineui::App& app) {
    std::string base;
    std::string css = app::read_framework_bundle(
        affineui::ViewTheme::Decius, kDeciusVersion, base);
    css += "\n";
    css += native_css();
    css += R"CSS(
.ge-lab-shell{position:fixed;inset:0;display:flex;flex-direction:column;background:var(--dcs-bg-app);color:var(--dcs-text);overflow:hidden}
.ge-lab-body{flex:1;min-height:0;display:flex;gap:12px;padding:12px;overflow:auto;align-items:flex-start}
.ge-lab-panel{width:360px;height:auto;min-height:auto;align-self:flex-start;overflow:visible}
.ge-lab-panel-wide{width:520px;height:auto;min-height:auto;align-self:flex-start;overflow:visible}
.ge-lab-panel-xl{width:820px;height:auto;min-height:auto;align-self:flex-start;overflow:visible}
.ge-lab-panel>.dcs-panel__body,
.ge-lab-panel-wide>.dcs-panel__body,
.ge-lab-panel-xl>.dcs-panel__body{flex:0 0 auto;min-height:auto;overflow:visible}
.ge-lab-tree-panel{width:520px;height:320px;min-height:320px;overflow:hidden}
.ge-lab-tree-panel>.dcs-panel__body{flex:1 1 auto;min-height:0;padding:0;overflow:auto}
.ge-lab-tree{display:flex;flex-direction:column;min-height:100%;width:100%;padding:6px 0}
.ge-lab-note{font-size:var(--dcs-fs-xs);color:var(--dcs-text-mute);padding:6px 0 0}
)CSS";
    app.set_stylesheet(css, base);
}

void setup_decius_view(affineui::View& v, std::string_view accent = "cyan") {
    v.set_framework_version(kDeciusVersion);
    v.selector(affineui::decius::selector::style,
               affineui::decius::style::flat);
    v.selector(affineui::decius::selector::density,
               affineui::decius::density::compact);
    v.selector(affineui::decius::selector::accent, accent);
}

app::Object lab_object() {
    app::Object obj;
    obj.id = "hero";
    obj.type = "mesh";
    obj.name = "Hero";
    obj.properties.push_back({"tint", app::PropValue{std::string{"#4d9fff"}}});
    obj.properties.push_back({"castShadows", app::PropValue{false}});
    return obj;
}

struct LabCase {
    std::string_view name;
    std::string_view view;
    std::string_view automated_test;
};

const std::vector<LabCase>& lab_cases() {
    static const std::vector<LabCase> cases{
        {"checkbox", "checkbox",
         "App Decius checkbox survives command-stack rebuilds on first click"},
        {"checkbox-command-rebuild", "checkbox",
         "App Decius checkbox survives command-stack rebuilds on first click"},
        {"dropdown", "dropdown",
         "App Decius dropdown selection bar stretches across wide menus"},
        {"dropdown-wide-menu", "dropdown",
         "App Decius dropdown selection bar stretches across wide menus"},
        {"color", "color-command",
         "App Decius colorfield picker survives model-backed rebuilds"},
        {"color-command", "color-command",
         "App Decius colorfield picker survives model-backed rebuilds"},
        {"color-command-rebuild", "color-command",
         "App Decius colorfield picker survives model-backed rebuilds"},
        {"color-direct", "color-direct",
         "App dispatch invokes Decius colorfield picker callbacks"},
        {"color-callbacks", "color-direct",
         "App dispatch invokes Decius colorfield picker callbacks"},
        {"color-scrolled", "color-scrolled",
         "App Decius colorfield pickers stay anchored in scrolled panels"},
        {"vector", "vector",
         "App Decius vector editors keep the default horizontal gutter"},
        {"vector-gutter", "vector",
         "App Decius vector editors keep the default horizontal gutter"},
        {"vector-stacked", "vector",
         "App Decius vector editors expand their foldout field when stacked"},
        {"tree", "tree",
         "UiControls script reorders Decius tree rows by drag/drop"},
        {"tree-drag-drop", "tree",
         "UiControls script reorders Decius tree rows by drag/drop"},
        {"tearout", "tearout",
         "UiControls: View panel tearoff uses a default size inside the workspace float host"},
        {"tearout-single-title", "tearout",
         "UiControls: View panel tearoff uses a default size inside the workspace float host"},
    };
    return cases;
}

std::string_view lab_view_for(std::string_view name) {
    for (const LabCase& lab : lab_cases()) {
        if (lab.name == name) return lab.view;
    }
    return name;
}

class LabController : public affineui::Trackable {
public:
    explicit LabController(std::string scenario)
        : requested_(std::move(scenario)),
          scenario_(lab_view_for(requested_)),
          app_(lab_config(requested_, [this] { reload(); })) {
        ctx_.document().add(lab_object());
        ctx_.stack().set_changed_handler([this] { reload(); });
    }

    int run() {
        reload();
        install_decius_styles(app_);
        return app_.run();
    }

private:
    affineui::View make_view() {
        affineui::View v{affineui::ViewTheme::Decius};
        setup_decius_view(v);
        v.begin();
        {
            auto shell = v.container("ge-lab-shell", "lab-shell");
            build_menu(v);
            auto body = v.container("ge-lab-body", "lab-body");
            if (scenario_ == "color" || scenario_ == "color-command") {
                build_color(v, true);
            } else if (scenario_ == "color-direct") {
                build_color(v, false);
            } else if (scenario_ == "color-scrolled") {
                build_color_scrolled(v);
            } else if (scenario_ == "dropdown") {
                build_dropdown(v);
            } else if (scenario_ == "vector") {
                build_vector(v);
            } else if (scenario_ == "tree") {
                build_tree(v);
            } else if (scenario_ == "tearout") {
                build_tearout(v);
            } else if (scenario_ == "checkbox") {
                build_checkbox(v);
            } else {
                build_unknown(v);
            }
        }
        v.end();
        return v;
    }

    void reload() { app_.load_view(make_view()); }

    void build_menu(affineui::View& v) {
        auto bar = v.menu_bar("lab-menubar");
        v.menu_brand("Interactive Tests", "decius", "lab-brand");
        v.menu_button("Cases", [&](affineui::View& m) {
            for (std::string_view name :
                 {"checkbox", "dropdown", "color", "color-direct",
                  "color-scrolled", "vector", "tree", "tearout"}) {
                m.menu_item(std::string(name), {}, {}, "case-" + std::string(name));
            }
        }, "lab-cases");
        v.menu_spacer("lab-spacer");
        v.menu_meta("--lab=" + requested_, "lab-meta");
    }

    void panel(affineui::View& v,
               std::string_view title,
               std::function<void(affineui::View&)> build,
               bool wide = false) {
        auto p = v.container(
            wide ? "dcs-panel dcs-panel--bordered ge-lab-panel-wide"
                 : "dcs-panel dcs-panel--bordered ge-lab-panel",
            "panel-" + std::string(title));
        {
            auto header = v.container("dcs-panel__header", "hdr-" + std::string(title));
            v.text(std::string(title), "title-" + std::string(title));
        }
        {
            auto body = v.container("dcs-panel__body", "body-" + std::string(title));
            build(v);
        }
    }

    std::string command_tint() const {
        return std::get<std::string>(ctx_.document().property(
            "hero", "tint", app::PropValue{std::string{"#4d9fff"}}));
    }

    bool command_shadows() const {
        return std::get<bool>(ctx_.document().property(
            "hero", "castShadows", app::PropValue{false}));
    }

    void build_color(affineui::View& v, bool command_backed) {
        panel(v, command_backed ? "Color command" : "Color direct",
              [&](affineui::View& p) {
                  auto props = p.container("dcs-props", "props-color");
                  const std::string value =
                      command_backed ? command_tint() : direct_tint_;
                  p.input("Tint", value, "color", "tint")
                      .on_change([this, command_backed](std::string_view next) {
                          if (command_backed) {
                              ctx_.run(std::make_unique<SetStringProperty>(
                                  "hero", "tint", std::string(next)));
                          } else {
                              direct_tint_ = std::string(next);
                              reload();
                          }
                      });
                  p.text("Model value: " + value, "color-value")
                      .cls("ge-lab-note");
              });
    }

    void build_color_scrolled(affineui::View& v) {
        panel(v, "Color scrolled", [&](affineui::View& p) {
            auto scroll = p.container("dcs-props", "props-color-scroll");
            scroll.attr("style", "height:96px;overflow-y:auto");
            p.container_ref({}, "color-scroll-top")
                .attr("style", "display:block;height:96px");
            p.input("Tint", direct_tint_, "color", "tint")
                .on_change([this](std::string_view next) {
                    direct_tint_ = std::string(next);
                    reload();
                });
            p.container_ref({}, "color-scroll-bottom")
                .attr("style", "display:block;height:180px");
        });
    }

    void build_dropdown(affineui::View& v) {
        auto p = v.container("dcs-panel dcs-panel--bordered ge-lab-panel-xl",
                             "panel-dropdown");
        {
            auto header = v.container("dcs-panel__header", "hdr-dropdown");
            v.text("Dropdown wide menu", "title-dropdown");
        }
        {
            auto body = v.container("dcs-panel__body", "body-dropdown");
            auto props = v.container("dcs-props", "props-dropdown");
            v.dropdown("Preset",
                       {"Warm pad", "Digital pluck", "Sub bass", "Noise sweep"},
                       dropdown_value_, "preset")
                .attr("style", "display:block;width:760px")
                .on_change([this](std::string_view next) {
                    dropdown_value_ = std::string(next);
                    reload();
                });
            v.text("Model value: " + dropdown_value_, "dropdown-value")
                .cls("ge-lab-note");
        }
    }

    void build_vector(affineui::View& v) {
        panel(v, "Vector", [&](affineui::View& p) {
            auto foldouts = p.container("dcs-foldouts", "foldouts");
            {
                auto fold = p.foldout("Transform", true, "fold-xform");
                auto props = p.container("dcs-props", "xform-props");
                p.vec("Location", {"X", "Y", "Z"}, {12.0, 4.2, -8.5}, "loc");
            }
            {
                auto fold = p.foldout("Display", true, "fold-display");
                auto props = p.container("dcs-props", "display-props");
                p.button_group("Blend", {"Normal", "Add", "Multiply"},
                               "Normal", "blend");
            }
        });
    }

    void build_tree(affineui::View& v) {
        auto p = v.container("dcs-panel dcs-panel--bordered ge-lab-tree-panel",
                             "panel-tree");
        {
            auto header = v.container("dcs-panel__header", "hdr-tree");
            v.text("Tree", "title-tree");
        }
        {
            auto body = v.container("dcs-panel__body", "body-tree");
            auto tree = v.tree("scene-tree");
            tree.cls("ge-lab-tree");
            struct Row {
                const char* id;
                const char* label;
                const char* icon;
                int depth;
            };
            for (const Row& row :
                 {Row{"world", "WorldRoot", "cube", 0},
                  Row{"hero", "Hero_mesh_high", "sphere", 1},
                  Row{"key_light", "KeyLight", "light", 1},
                  Row{"spline", "SplineRig", "spline", 1}}) {
                affineui::TreeRowOptions opts;
                opts.depth = row.depth;
                opts.selected = selected_row_ == row.id;
                opts.icon = row.icon;
                opts.expandable = row.depth == 0;
                opts.expanded = true;
                v.tree_row(row.label, opts, "row-" + std::string(row.id))
                    .on_click([this, id = std::string(row.id)] {
                        selected_row_ = id;
                        reload();
                    });
            }
        }
    }

    void build_tearout(affineui::View& v) {
        auto host = v.container("ge-lab-panel-wide", "tearout-host");
        host.attr("style", "height:460px;width:620px;min-height:0");
        v.document_view("workarea", [&](affineui::View& dv) {
            dv.document([&](affineui::View& doc) {
                  auto canvas = doc.container("ge-vp-canvas", "vp-canvas");
                  canvas.attr("data-dcs-float-host", "");
                  doc.container_ref("ge-vp-grid", "vp-grid");
              }, "Lit View", "cube");
            auto assets = dv.dockpanel(
                "Assets", affineui::DockLocation::docked(affineui::Dock::Bottom, 150),
                [&](affineui::View& p) {
                    auto strip = p.container("ge-asset-strip", "asset-strip");
                    p.text("Drag the Console tab out, then inspect its title.",
                           "tearout-help");
                },
                "image");
            dv.dockpanel("Console", affineui::DockLocation::tab().in(assets),
                         [&](affineui::View& p) {
                             p.text("Console output", "console-text");
                         },
                         "file");
        });
    }

    void build_checkbox(affineui::View& v) {
        panel(v, "Checkbox command", [&](affineui::View& p) {
            auto props = p.container("dcs-props", "props-check");
            p.checkbox("Cast shadows", command_shadows(), "shadows")
                .on_change([this](std::string_view next) {
                    ctx_.run(std::make_unique<SetBoolProperty>(
                        "hero", "castShadows", truthy(next)));
                });
            p.checkbox("Visible", true, "visible");
            p.text(command_shadows() ? "Model value: true"
                                     : "Model value: false",
                   "shadow-value")
                .cls("ge-lab-note");
        });
    }

    void build_unknown(affineui::View& v) {
        panel(v, "Unknown", [&](affineui::View& p) {
            p.text("Unknown --lab scenario: " + scenario_, "unknown");
        });
    }

    std::string requested_;
    std::string scenario_;
    affineui::App app_;
    app::Context ctx_;
    std::string direct_tint_{"#4d9fff"};
    std::string dropdown_value_{"Sub bass"};
    std::string selected_row_{"hero"};
};

const std::vector<std::string_view>& scenario_names() {
    static const std::vector<std::string_view> names{
        "checkbox", "dropdown", "color", "color-direct", "color-scrolled",
        "vector", "tree", "tearout",
    };
    return names;
}

}  // namespace

bool is_interactive_test(std::string_view name) {
    for (const LabCase& lab : lab_cases()) {
        if (name == lab.name) return true;
    }
    return false;
}

int run_interactive_test(std::string_view name) {
    if (!is_interactive_test(name)) {
        print_interactive_test_usage();
        return 2;
    }
    LabController lab{std::string(name)};
    return lab.run();
}

void print_interactive_test_usage() {
    std::fprintf(stderr, "Interactive Decius lab scenarios:\n");
    for (auto name : scenario_names()) {
        std::fprintf(stderr, "  --lab=%.*s\n",
                     static_cast<int>(name.size()), name.data());
    }
    std::fprintf(stderr, "Synthetic-test aliases:\n");
    for (const LabCase& lab : lab_cases()) {
        std::fprintf(stderr, "  --lab=%.*s  ->  %.*s\n",
                     static_cast<int>(lab.name.size()), lab.name.data(),
                     static_cast<int>(lab.automated_test.size()),
                     lab.automated_test.data());
    }
    std::fprintf(stderr, "Game editor isolation scopes:\n");
    std::fprintf(stderr, "  --game-scope=inspector\n");
    std::fprintf(stderr, "  --game-scope=tree\n");
    std::fprintf(stderr, "  --game-scope=tearout\n");
}

}  // namespace ge
