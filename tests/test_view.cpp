#include <doctest/doctest.h>

#include "affineui/app.h"
#include "affineui/view.h"
#include "affineui_browser_server.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

void build_small_view(affineui::View& view,
                      affineui::RemotePatchQueue& patches,
                      std::string_view button_label,
                      bool checked) {
    view.begin(&patches);
    view.heading(1, "Remote-ready UI");
    view.paragraph("Same commands can inflate local DOM or browser DOM.");
    view.button(button_label, true);
    view.checkbox("Enabled", checked);
    view.slider("Gain", 0.75, 0.0, 1.0);
    view.end();
}

bool has_op(const affineui::RemotePatchQueue& queue,
            affineui::RemotePatchOp op) {
    const auto& patches = queue.patches();
    return std::any_of(patches.begin(), patches.end(),
        [&](const affineui::RemotePatch& patch) {
            return patch.op == op;
        });
}

bool has_text_patch(const affineui::RemotePatchQueue& queue,
                    std::string_view text) {
    const auto& patches = queue.patches();
    return std::any_of(patches.begin(), patches.end(),
        [&](const affineui::RemotePatch& patch) {
            return patch.op == affineui::RemotePatchOp::SetText &&
                   patch.value == text;
        });
}

std::vector<std::string> test_asset_folders() {
    return {
        std::string(AFFINEUI_TEST_SOURCE_DIR) + "/examples",
        std::string(AFFINEUI_TEST_SOURCE_DIR),
    };
}

affineui::Point find_hovered_widget(affineui::App& app,
                                    std::string_view name,
                                    int width,
                                    int height) {
    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            move.pos = {x, y};
            app.dispatch(move);
            const auto chain = app.document().hovered_info_chain();
            const bool found = std::any_of(chain.begin(), chain.end(),
                [&](const affineui::Document::HoverInfo& info) {
                    return std::any_of(info.attrs.begin(), info.attrs.end(),
                        [&](const auto& attr) {
                            return attr.first == "data-aui-name" &&
                                   attr.second == name;
                        });
                });
            if (found) return move.pos;
        }
    }
    return {-1, -1};
}

affineui::Point find_hovered_attr(affineui::App& app,
                                  std::string_view attr_name,
                                  int width,
                                  int height) {
    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            move.pos = {x, y};
            app.dispatch(move);
            const auto chain = app.document().hovered_info_chain();
            const bool found = std::any_of(chain.begin(), chain.end(),
                [&](const affineui::Document::HoverInfo& info) {
                    return std::any_of(info.attrs.begin(), info.attrs.end(),
                        [&](const auto& attr) {
                            return attr.first == attr_name;
                        });
                });
            if (found) return move.pos;
        }
    }
    return {-1, -1};
}

affineui::Point find_hovered_tag_attr(affineui::App& app,
                                      std::string_view tag,
                                      std::string_view attr_name,
                                      std::string_view attr_value,
                                      int width,
                                      int height) {
    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            move.pos = {x, y};
            app.dispatch(move);
            const auto chain = app.document().hovered_info_chain();
            const bool found = std::any_of(chain.begin(), chain.end(),
                [&](const affineui::Document::HoverInfo& info) {
                    if (info.tag != tag) return false;
                    return std::any_of(info.attrs.begin(), info.attrs.end(),
                        [&](const auto& attr) {
                            return attr.first == attr_name &&
                                   attr.second == attr_value;
                        });
                });
            if (found) return move.pos;
        }
    }
    return {-1, -1};
}

affineui::Rect hovered_class_bounds(const affineui::App& app,
                                    std::string_view cls) {
    const auto chain = app.document().hovered_info_chain();
    for (const auto& info : chain) {
        if (std::find(info.classes.begin(), info.classes.end(), cls) !=
            info.classes.end()) {
            return info.bounds;
        }
    }
    return {-1, -1, 0, 0};
}

}  // namespace

TEST_CASE("View emits remote create patches on first reconcile") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    affineui::RemotePatchQueue patches;

    build_small_view(view, patches, "Refresh", true);

    CHECK(has_op(patches, affineui::RemotePatchOp::CreateElement));
    CHECK(has_text_patch(patches, "Refresh"));
    CHECK(view.root().children.size() == 5);

    const auto html = view.to_html_document();
    CHECK(html.find("bootstrap-5.3.8.min.css") != std::string::npos);
    CHECK(html.find("Remote-ready UI") != std::string::npos);
    CHECK(html.find("Refresh") != std::string::npos);
    CHECK(html.find("form-check") != std::string::npos);
}

TEST_CASE("View emits framework-specific knob markup") {
    affineui::View decius{affineui::ViewTheme::Decius};
    decius.begin();
    auto shape = decius.knob("Shape", 0.42, 0.0, 1.0, false, "shape");
    decius.end();

    REQUIRE(shape);
    auto html = decius.to_html_fragment();
    CHECK(html.find("data-dcs-knob") != std::string::npos);
    CHECK(html.find("class=\"dcs-knob\" data-dcs-knob") !=
          std::string::npos);
    CHECK(html.find("dcs-knob__arc") != std::string::npos);
    CHECK(html.find("dcs-knob__indicator") != std::string::npos);
    CHECK(html.find("dcs-knob__cap") != std::string::npos);

    affineui::View bootstrap{affineui::ViewTheme::Bootstrap};
    bootstrap.begin();
    auto gain = bootstrap.knob("Gain", 0.5, 0.0, 1.0, false, "gain");
    bootstrap.end();

    REQUIRE(gain);
    html = bootstrap.to_html_fragment();
    CHECK(html.find("data-aui-knob") != std::string::npos);
    CHECK(html.find("class=\"aui-knob\" data-aui-name=\"gain\"") !=
          std::string::npos);
    CHECK(html.find("aui-knob__arc") != std::string::npos);
    CHECK(html.find("aui-knob__indicator") != std::string::npos);
}

TEST_CASE("View can embed trusted raw HTML fragments") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    view.heading(2, "Reference");
    view.html(R"(<div class="dcs-alert"><span data-role="raw">Decius</span></div>)",
              "raw-decius-snippet");
    view.text("<escaped>", "escaped-text");
    view.end();

    const auto html = view.to_html_fragment();
    CHECK(html.find("<div class=\"dcs-alert\"><span data-role=\"raw\">Decius</span></div>") !=
          std::string::npos);
    CHECK(html.find("&lt;escaped&gt;") != std::string::npos);
}

TEST_CASE("View framework personalities apply default and explicit selectors") {
    affineui::View decius{affineui::ViewTheme::Decius};
    decius.begin();
    auto panel = decius.panel_ref("panel");
    panel.selector(affineui::decius::selector::size,
                   affineui::decius::size::lg);
    decius.end();

    auto html = decius.to_html_document();
    CHECK(html.find("data-aui-size=\"md\"") != std::string::npos);
    CHECK(html.find("data-aui-style=\"flat\"") != std::string::npos);
    CHECK(html.find("data-dcs-style=\"flat\"") != std::string::npos);
    CHECK(html.find("data-dcs-density=\"compact\"") != std::string::npos);
    CHECK(html.find("data-aui-size=\"lg\"") != std::string::npos);
    CHECK(html.find(".aui-keycolor-swatch.is-active{border-color:var(--aui-swatch)") !=
          std::string::npos);
    CHECK(html.find("0 0 0 3px var(--aui-swatch)") == std::string::npos);

    decius.find_widget("panel").selector(affineui::decius::selector::size,
                                         "med");
    CHECK(decius.to_html_fragment().find("data-aui-size=\"md\"") !=
          std::string::npos);

    affineui::View decius_3d{affineui::ViewTheme::Decius};
    decius_3d.selector(affineui::decius::selector::style,
                       affineui::decius::style::three_d);
    decius_3d.begin();
    decius_3d.panel_ref("panel");
    decius_3d.end();
    html = decius_3d.to_html_document();
    CHECK(html.find("data-aui-style=\"3d\"") != std::string::npos);
    CHECK(html.find("data-dcs-style=\"3d\"") != std::string::npos);
    CHECK(html.find("data-dcs-style=\"flat\"") == std::string::npos);

    affineui::View bootstrap{affineui::ViewTheme::Bootstrap};
    bootstrap.begin();
    auto button = bootstrap.button("Dark", false, "dark");
    button.selector(affineui::bootstrap::selector::theme,
                    affineui::bootstrap::theme::dark);
    bootstrap.end();

    html = bootstrap.to_html_document();
    CHECK(html.find("data-aui-size=\"md\"") != std::string::npos);
    CHECK(html.find("data-bs-theme=\"dark\"") != std::string::npos);
}

TEST_CASE("View emits framework-specific field widgets") {
    affineui::View bootstrap{affineui::ViewTheme::Bootstrap};
    bootstrap.begin();
    {
        auto panel = bootstrap.panel("panel");
        (void) panel;
        bootstrap.input("Object name", "Cylinder.042", "text", "object-name");
        bootstrap.password("Token", "secret", "token");
        bootstrap.dropdown("Mode", {"Object", "Edit"}, "Object", "mode");
        bootstrap.button_group("Space", {"Local", "World"}, "World", "space");
        bootstrap.textarea("Notes", "Dense native UI", 3, "notes");
    }
    bootstrap.end();

    CHECK(bootstrap.diagnostics().empty());
    auto html = bootstrap.to_html_fragment();
    CHECK(html.find("aui-bs-field") != std::string::npos);
    CHECK(html.find("form-control") != std::string::npos);
    CHECK(html.find("form-select") != std::string::npos);
    CHECK(html.find("btn-group") != std::string::npos);
    CHECK(html.find("Cylinder.042") != std::string::npos);

    affineui::View decius{affineui::ViewTheme::Decius};
    decius.begin();
    {
        auto panel = decius.panel("panel");
        (void) panel;
        decius.input("Object name", "Cylinder.042", "text", "object-name");
        decius.input("Gain", "1.000", "number", "gain");
        decius.input("Tint", "#4da3ff", "color", "tint");
        decius.dropdown("Mode", {"Object", "Edit"}, "Object", "mode");
        decius.button_group("Space", {"Local", "World"}, "World", "space");
    }
    decius.end();

    CHECK(decius.diagnostics().empty());
    html = decius.to_html_fragment();
    CHECK(html.find("dcs-input") != std::string::npos);
    CHECK(html.find("dcs-combo") != std::string::npos);
    CHECK(html.find("data-dcs-combo") != std::string::npos);
    CHECK(html.find("dcs-combo__fill") != std::string::npos);
    CHECK(html.find("style=\"--fill:50%\"") != std::string::npos);
    CHECK(html.find("dcs-colorfield") != std::string::npos);
    CHECK(html.find("dcs-select") != std::string::npos);
    CHECK(html.find("dcs-btn-group") != std::string::npos);
}

TEST_CASE("Stable widget refs can replace tab body content") {
    affineui::View view{affineui::ViewTheme::Bootstrap};

    view.begin();
    {
        auto panel = view.panel("panel");
        (void) panel;
        view.button("Controls", true, "tab-controls");
        view.button("Fields", false, "tab-fields");
        auto body = view.container("tab-body", "tab-body");
        (void) body;
    }
    view.end();

    auto body = view.find_widget("tab-body");
    REQUIRE(body);
    body.replace([](affineui::View& v) {
        v.checkbox("Framework checkbox", true, "hello-check");
        v.slider("Framework slider", 0.65, 0.0, 1.0, "hello-slider");
    });

    CHECK(view.find_widget("hello-check"));
    CHECK(view.find_widget("hello-slider"));

    body.replace([](affineui::View& v) {
        v.input("Object name", "Cylinder.042", "text", "object-name");
        v.dropdown("Mode", {"Object", "Edit"}, "Object", "mode");
    });

    CHECK_FALSE(view.find_widget("hello-check"));
    CHECK(view.find_widget("object-name"));
    CHECK(view.find_widget("mode"));
}

TEST_CASE("View virtual list materializes only the requested window") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    affineui::VirtualListOptions options;
    options.item_count = 100;
    options.first_item = 40;
    options.visible_items = 5;
    options.overscan = 2;
    options.item_size = 20.0;

    view.begin();
    auto list = view.virtual_list(
        "events",
        options,
        [](affineui::View& v, std::size_t index) {
            v.button("Row " + std::to_string(index), false,
                     "row-" + std::to_string(index));
        });
    view.end();

    REQUIRE(list);
    const auto html = view.to_html_fragment();
    CHECK(html.find("data-aui-widget=\"virtual-list\"") != std::string::npos);
    CHECK(html.find("height:760px") != std::string::npos);
    CHECK(html.find("height:1060px") != std::string::npos);
    CHECK(html.find("Row 37") == std::string::npos);
    CHECK(html.find("Row 38") != std::string::npos);
    CHECK(html.find("Row 46") != std::string::npos);
    CHECK(html.find("Row 47") == std::string::npos);
    CHECK(view.find_widget("row-38"));
    CHECK_FALSE(view.find_widget("row-47"));
}

TEST_CASE("App dispatch invokes command button callbacks") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    int clicks = 0;

    view.begin();
    view.button("Run", true, "run").on_click([&] { ++clicks; });
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(240, 120);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    bool found = false;
    for (int y = 0; y < 120 && !found; y += 4) {
        for (int x = 0; x < 240 && !found; x += 4) {
            move.pos = {x, y};
            app.dispatch(move);
            const auto chain = app.document().hovered_info_chain();
            found = std::any_of(chain.begin(), chain.end(),
                [](const affineui::Document::HoverInfo& info) {
                    return info.tag == "button";
                });
        }
    }
    REQUIRE(found);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = move.pos;
    app.dispatch(down);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = move.pos;
    CHECK(app.dispatch(up));
    CHECK(clicks == 1);
}

TEST_CASE("App dispatch invokes command widget change callbacks") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    std::string value;

    view.begin();
    view.checkbox("Enabled", false, "enabled")
        .on_change([&](std::string_view next) { value = std::string(next); });
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(260, 120);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    bool found = false;
    for (int y = 0; y < 120 && !found; y += 4) {
        for (int x = 0; x < 260 && !found; x += 4) {
            move.pos = {x, y};
            app.dispatch(move);
            const auto chain = app.document().hovered_info_chain();
            found = std::any_of(chain.begin(), chain.end(),
                [](const affineui::Document::HoverInfo& info) {
                    return std::any_of(info.attrs.begin(), info.attrs.end(),
                        [](const auto& attr) {
                            return attr.first == "data-aui-name" &&
                                   attr.second == "enabled";
                        });
                });
        }
    }
    REQUIRE(found);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = move.pos;
    app.dispatch(down);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = move.pos;
    CHECK(app.dispatch(up));
    CHECK(value == "true");
}

TEST_CASE("App dispatch invokes command knob change callbacks") {
    for (const auto theme :
         {affineui::ViewTheme::Bootstrap, affineui::ViewTheme::Decius}) {
        affineui::View view{theme};
        std::string value;

        view.begin();
        view.knob("Shape", 0.42, 0.0, 1.0, false, "shape")
            .on_change([&](std::string_view next) {
                value = std::string(next);
            });
        view.end();

        affineui::App::Config cfg;
        cfg.asset_folders = test_asset_folders();
        affineui::App app{cfg};
        app.load_view(view);
        app.document().layout(320, 200);

        const auto knob = theme == affineui::ViewTheme::Bootstrap
            ? find_hovered_attr(app, "data-aui-knob", 320, 200)
            : find_hovered_attr(app, "data-dcs-knob", 320, 200);
        REQUIRE(knob.x >= 0);

        affineui::Event down{};
        down.type = affineui::EventType::MouseDown;
        down.button = affineui::MouseButton::Left;
        down.pos = knob;
        app.dispatch(down);

        affineui::Event drag{};
        drag.type = affineui::EventType::MouseMove;
        drag.pos = {knob.x, knob.y - 36};
        CHECK(app.dispatch(drag));

        affineui::Event up{};
        up.type = affineui::EventType::MouseUp;
        up.button = affineui::MouseButton::Left;
        up.pos = drag.pos;
        app.dispatch(up);

        REQUIRE_FALSE(value.empty());
        CHECK(std::stod(value) > 0.42);
    }
}

TEST_CASE("App dispatch invokes command text input change callbacks") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    std::string value;

    view.begin();
    view.input("Name", "Ada", "text", "name")
        .on_change([&](std::string_view next) { value = std::string(next); });
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(320, 160);

    const auto input = find_hovered_tag_attr(app, "input", "data-aui-name",
                                             "name", 320, 160);
    REQUIRE(input.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = input;
    app.dispatch(down);

    affineui::Event end{};
    end.type = affineui::EventType::KeyDown;
    end.key = affineui::Key::End;
    app.dispatch(end);

    affineui::Event text{};
    text.type = affineui::EventType::TextInput;
    text.text = "X";
    CHECK(app.dispatch(text));
    CHECK(value == "AdaX");
}

TEST_CASE("App dispatch supports numeric input horizontal drag") {
    for (const auto theme :
         {affineui::ViewTheme::Bootstrap, affineui::ViewTheme::Decius}) {
        affineui::View view{theme};
        std::string value;

        view.begin();
        view.input("Gain", "1.0", "number", "gain")
            .on_change([&](std::string_view next) { value = std::string(next); });
        view.end();

        affineui::App::Config cfg;
        cfg.asset_folders = test_asset_folders();
        affineui::App app{cfg};
        app.load_view(view);
        app.document().layout(320, 160);

        const auto input = find_hovered_tag_attr(app, "input", "data-aui-name",
                                                 "gain", 320, 160);
        REQUIRE(input.x >= 0);
        CHECK(app.document().hovered_cursor() == 6);

        affineui::Event down{};
        down.type = affineui::EventType::MouseDown;
        down.button = affineui::MouseButton::Left;
        down.pos = input;
        app.dispatch(down);

        affineui::Event drag{};
        drag.type = affineui::EventType::MouseMove;
        drag.pos = {input.x + 40, input.y};
        CHECK(app.dispatch(drag));

        affineui::Event up{};
        up.type = affineui::EventType::MouseUp;
        up.button = affineui::MouseButton::Left;
        up.pos = drag.pos;
        app.dispatch(up);

        REQUIRE_FALSE(value.empty());
        CHECK(std::stod(value) > 1.0);
    }
}

TEST_CASE("App dispatch edits and resizes command textareas") {
    for (const auto theme :
         {affineui::ViewTheme::Bootstrap, affineui::ViewTheme::Decius}) {
        affineui::View view{theme};
        std::string value;

        view.begin();
        view.textarea("Notes", "alpha\nomega", 3, "notes")
            .on_change([&](std::string_view next) { value = std::string(next); });
        view.end();

        affineui::App::Config cfg;
        cfg.asset_folders = test_asset_folders();
        affineui::App app{cfg};
        app.load_view(view);
        app.document().layout(420, 240);

        const auto textarea = find_hovered_tag_attr(app, "textarea",
                                                    "data-aui-name", "notes",
                                                    420, 240);
        REQUIRE(textarea.x >= 0);
        const auto before = app.document().hovered_info().bounds;

        affineui::Event down{};
        down.type = affineui::EventType::MouseDown;
        down.button = affineui::MouseButton::Left;
        down.pos = {before.x + 8, before.y + 8};
        app.dispatch(down);

        affineui::Event end{};
        end.type = affineui::EventType::KeyDown;
        end.key = affineui::Key::End;
        app.dispatch(end);

        affineui::Event text{};
        text.type = affineui::EventType::TextInput;
        text.text = "!";
        CHECK(app.dispatch(text));
        CHECK(value.find('!') != std::string::npos);

        affineui::Event hover{};
        hover.type = affineui::EventType::MouseMove;
        hover.pos = {before.x + before.w - 2, before.y + before.h - 2};
        app.dispatch(hover);
        CHECK(app.document().hovered_cursor() == 4);

        down.pos = hover.pos;
        app.dispatch(down);

        affineui::Event drag{};
        drag.type = affineui::EventType::MouseMove;
        drag.pos = {hover.pos.x + 24, hover.pos.y + 20};
        CHECK(app.dispatch(drag));
    }
}

TEST_CASE("App dispatch defers numeric input edit mode until click release") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    std::string value;

    view.begin();
    view.input("Gain", "1.0", "number", "gain")
        .on_change([&](std::string_view next) { value = std::string(next); });
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(320, 160);

    const auto input = find_hovered_tag_attr(app, "input", "data-aui-name",
                                             "gain", 320, 160);
    REQUIRE(input.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = input;
    app.dispatch(down);
    CHECK(value.empty());

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = input;
    app.dispatch(up);
    CHECK(value.empty());

    affineui::Event select_all{};
    select_all.type = affineui::EventType::KeyDown;
    select_all.key = affineui::Key::A;
    select_all.ctrl = true;
    app.dispatch(select_all);

    affineui::Event text{};
    text.type = affineui::EventType::TextInput;
    text.text = "2.5";
    CHECK(app.dispatch(text));
    CHECK(value == "2.5");
}

TEST_CASE("App dispatch invokes command dropdown and button-group callbacks") {
    for (const auto theme :
         {affineui::ViewTheme::Bootstrap, affineui::ViewTheme::Decius}) {
        affineui::View view{theme};
        std::string mode;
        std::string space;

        view.begin();
        view.dropdown("Mode", {"Object", "Edit", "Render"}, "Object", "mode")
            .on_change([&](std::string_view next) {
                mode = std::string(next);
            });
        view.button_group("Space", {"Local", "World"}, "World", "space")
            .on_change([&](std::string_view next) {
                space = std::string(next);
            });
        view.end();

        affineui::App::Config cfg;
        cfg.asset_folders = test_asset_folders();
        affineui::App app{cfg};
        app.load_view(view);
        app.document().layout(420, 420);

        auto click_at = [&](affineui::Point p) {
            affineui::Event down{};
            down.type = affineui::EventType::MouseDown;
            down.button = affineui::MouseButton::Left;
            down.pos = p;
            app.dispatch(down);
            affineui::Event up{};
            up.type = affineui::EventType::MouseUp;
            up.button = affineui::MouseButton::Left;
            up.pos = p;
            app.dispatch(up);
        };

        const auto select = find_hovered_tag_attr(app, "select",
                                                  "data-aui-name", "mode",
                                                  420, 420);
        REQUIRE(select.x >= 0);
        const auto select_bounds = app.document().hovered_info().bounds;
        click_at(select);
        app.document().layout(420, 420);
        const auto reopened_select = find_hovered_tag_attr(app, "select",
                                                           "data-aui-name",
                                                           "mode", 420, 420);
        REQUIRE(reopened_select.x >= 0);
        CHECK(app.document().hovered_info().bounds.y == select_bounds.y);
        CHECK(app.document().hovered_info().bounds.h == select_bounds.h);

        const auto edit = find_hovered_tag_attr(app, "button", "value",
                                                "Edit", 420, 420);
        REQUIRE(edit.x >= 0);
        const auto menu_bounds = hovered_class_bounds(app, "aui-select__menu");
        REQUIRE(menu_bounds.y >= 0);
        CHECK(menu_bounds.y == select_bounds.y + select_bounds.h);
        click_at(edit);
        CHECK(mode == "Edit");
        app.document().layout(420, 420);

        const auto local = find_hovered_tag_attr(app, "button", "value",
                                                 "Local", 420, 420);
        REQUIRE(local.x >= 0);
        click_at(local);
        CHECK(space == "Local");
    }
}

TEST_CASE("App load_view preserves named scroll panels across control reloads") {
    auto make_view = [](bool checked) {
        affineui::View view{affineui::ViewTheme::Decius};
        view.begin();
        {
            auto panel = view.container({}, "controls-panel-body");
            panel.attr("style",
                       "display:block;position:absolute;left:0;top:0;"
                       "width:180px;height:80px;overflow-y:auto;");
            view.container({}, "controls-spacer-top")
                .attr("style", "display:block;height:96px");
            view.checkbox("Loop Selection", checked, "controls-loop")
                .on_change([](std::string_view) {});
            view.container({}, "controls-spacer-bottom")
                .attr("style", "display:block;height:180px");
        }
        view.end();
        return view;
    };

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(make_view(false));
    app.document().layout(240, 120);

    affineui::Event wheel{};
    wheel.type = affineui::EventType::MouseWheel;
    wheel.pos = {10, 10};
    wheel.wheel_dy = -3.0f;
    CHECK(app.dispatch(wheel));
    app.document().layout(240, 120);

    const auto before = find_hovered_widget(app, "controls-loop", 240, 120);
    REQUIRE(before.x >= 0);

    app.load_view(make_view(true));
    app.document().layout(240, 120);

    const auto after = find_hovered_widget(app, "controls-loop", 240, 120);
    REQUIRE(after.x >= 0);
    CHECK(after.y >= before.y - 4);
    CHECK(after.y <= before.y + 4);
}

TEST_CASE("App dispatch invokes Decius colorfield menu callbacks") {
    affineui::View view{affineui::ViewTheme::Decius};
    std::string tint;

    view.begin();
    view.input("Tint", "#3bb7ff", "color", "tint")
        .on_change([&](std::string_view next) { tint = std::string(next); });
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(360, 180);

    auto click_at = [&](affineui::Point p) {
        affineui::Event down{};
        down.type = affineui::EventType::MouseDown;
        down.button = affineui::MouseButton::Left;
        down.pos = p;
        app.dispatch(down);
        affineui::Event up{};
        up.type = affineui::EventType::MouseUp;
        up.button = affineui::MouseButton::Left;
        up.pos = p;
        app.dispatch(up);
    };

    const auto field = find_hovered_widget(app, "tint", 360, 180);
    REQUIRE(field.x >= 0);
    click_at(field);
    app.document().layout(360, 180);

    const auto green = find_hovered_tag_attr(app, "button", "data-dcs-value",
                                             "#3dd68a", 360, 220);
    REQUIRE(green.x >= 0);
    click_at(green);
    CHECK(tint == "#3dd68a");
}

TEST_CASE("View reconcile reuses nodes and emits property patches") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    affineui::RemotePatchQueue first;
    build_small_view(view, first, "Refresh", true);

    const auto button_id = view.root().children[2].remote_id;
    REQUIRE_FALSE(button_id.empty());
    REQUIRE(view.find_remote(button_id) != nullptr);

    affineui::RemotePatchQueue second;
    build_small_view(view, second, "Export", false);

    CHECK_FALSE(has_op(second, affineui::RemotePatchOp::CreateElement));
    CHECK(has_text_patch(second, "Export"));
    CHECK(has_op(second, affineui::RemotePatchOp::RemoveAttribute));
    CHECK(view.root().children[2].remote_id == button_id);
    CHECK(view.find_remote(button_id)->text == "Export");
}

TEST_CASE("View named widget refs resolve to persistent tree items") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    affineui::RemotePatchQueue patches;

    view.begin(&patches);
    auto first = view.button("First", true, "main-action");
    view.end();

    REQUIRE(first);
    CHECK(first.name() == "main-action");
    auto lookup = view.find_widget("main-action");
    REQUIRE(lookup);
    CHECK(lookup.id() == first.id());

    affineui::WidgetRef copied;
    copied = lookup;
    REQUIRE(copied);
    CHECK(copied.id() == lookup.id());

    lookup.text("Second");
    CHECK(view.find_widget("main-action").node()->text == "Second");

    view.begin(&patches);
    auto second = view.button("Refresh label", true, "main-action");
    view.end();

    REQUIRE(second);
    CHECK(second.id() == first.id());
    CHECK(view.find_widget("main-action").node()->text == "Refresh label");
}

TEST_CASE("View keyless widgets are write-only declarations") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    affineui::RemotePatchQueue patches;

    view.begin(&patches);
    auto unnamed = view.button("Write only");
    view.end();

    CHECK_FALSE(unnamed);
    CHECK_FALSE(view.find_widget(""));
    CHECK_FALSE(view.find_widget("Write only"));
    CHECK(view.to_html_fragment().find("Write only") != std::string::npos);
}

TEST_CASE("View named widget refs become empty when refresh removes the widget") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    affineui::RemotePatchQueue patches;

    view.begin(&patches);
    auto button = view.button("Transient", false, "transient");
    view.end();
    REQUIRE(button);

    view.begin(&patches);
    view.paragraph("No button here");
    view.end();

    CHECK_FALSE(button);
    CHECK_FALSE(view.find_widget("transient"));
}

TEST_CASE("View named refs recover after a subtree is rebuilt") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    affineui::RemotePatchQueue patches;

    view.begin(&patches);
    auto button = view.button("Before", true, "recoverable");
    view.end();
    REQUIRE(button);
    const auto first_id = button.id();

    view.clear();
    view.begin(&patches);
    auto rebuilt = view.button("After", true, "recoverable");
    view.end();

    REQUIRE(rebuilt);
    CHECK(button);
    CHECK(button.id() == rebuilt.id());
    CHECK(button.id() == first_id);
    CHECK(button.node()->text == "After");
}

TEST_CASE("View records diagnostics for duplicate explicit widget ids") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    affineui::RemotePatchQueue patches;

    view.begin(&patches);
    view.button("First", true, "duplicate");
    view.button("Second", false, "duplicate");
    view.end();

    REQUIRE_FALSE(view.diagnostics().empty());
    CHECK(view.diagnostics().front().find("duplicate") != std::string::npos);
    CHECK(view.find_widget("duplicate").node()->text == "Second");
}

TEST_CASE("WidgetRef can append and replace child declarations") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    affineui::RemotePatchQueue patches;

    view.begin(&patches);
    auto panel_scope = view.container("panel", "panel");
    auto panel = panel_scope.ref();
    view.end();

    REQUIRE(panel);
    panel.append([](affineui::View& v) {
        v.paragraph("First child", {}, "first");
    });
    panel.append([](affineui::View& v) {
        v.button("Second child", false, "second");
    });
    REQUIRE(panel.node() != nullptr);
    CHECK(panel.node()->children.size() == 2);
    CHECK(view.find_widget("second"));

    panel.replace([](affineui::View& v) {
        v.heading(2, "Replacement", {}, "replacement");
    });
    REQUIRE(panel.node() != nullptr);
    CHECK(panel.node()->children.size() == 1);
    CHECK_FALSE(view.find_widget("second"));
    CHECK(view.find_widget("replacement"));
}

TEST_CASE("WidgetRef child mutation is illegal during view generation") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    affineui::RemotePatchQueue patches;

    view.begin(&patches);
    auto panel = view.container_ref("panel", "panel");
    panel.append([](affineui::View& v) {
        v.paragraph("Forbidden");
    });
    view.end();

    REQUIRE_FALSE(view.diagnostics().empty());
    CHECK(view.diagnostics().front().find("append") != std::string::npos);
    CHECK(panel);
    REQUIRE(panel.node() != nullptr);
    CHECK(panel.node()->children.empty());
}

TEST_CASE("App can load a command view through the compatibility bridge") {
    affineui::View view{affineui::ViewTheme::Plain};
    affineui::RemotePatchQueue patches;
    build_small_view(view, patches, "Launch", true);

    affineui::App app;
    app.load_view(view);
    app.document().layout(480, 320);
    const auto size = app.document().content_size();
    CHECK(size.width >= 480);
    CHECK(size.height >= 320);
}

TEST_CASE("Document weak DOM handles invalidate when the document is replaced") {
    affineui::Document document;
    document.set_html(R"(<main><button id="run">Run</button></main>)");

    const auto handle = document.weak_handle_for_id("run");
    REQUIRE(handle);
    CHECK(document.weak_handle_valid(handle));

    document.set_html(R"(<main><button id="stop">Stop</button></main>)");
    CHECK_FALSE(document.weak_handle_valid(handle));
}

TEST_CASE("browser asset store serves boot page and bridge script") {
    const auto assets = affineui::browser::default_assets("Test App");

    const auto* index = assets.find("/");
    REQUIRE(index != nullptr);
    CHECK(index->content_type.find("text/html") != std::string::npos);
    CHECK(index->body.find("Test App") != std::string::npos);
    CHECK(index->body.find("/affineui.js") != std::string::npos);

    const auto* bridge = assets.find("/affineui.js");
    REQUIRE(bridge != nullptr);
    CHECK(bridge->body.find("affineuiApplyPatches") != std::string::npos);
    CHECK(bridge->body.find("EventSource") != std::string::npos);
    CHECK(bridge->body.find("/events") != std::string::npos);
}

TEST_CASE("browser transport core serves assets, queues events, and formats SSE patches") {
    auto assets = affineui::browser::default_assets("Transport");
    affineui::browser::TransportCore transport{std::move(assets)};

    auto index = transport.handle_request({"GET", "/", {}});
    CHECK(index.status == 200);
    CHECK(index.content_type.find("text/html") != std::string::npos);

    affineui::RemotePatchQueue patches;
    affineui::View view{affineui::ViewTheme::Bootstrap};
    build_small_view(view, patches, "Sync", true);
    transport.enqueue_patches_json(patches.to_json());

    auto stream = transport.handle_request({"GET", "/stream", {}});
    CHECK(stream.status == 200);
    CHECK(stream.keep_open);
    CHECK(stream.content_type.find("text/event-stream") != std::string::npos);
    CHECK(stream.body.find("event: patches") != std::string::npos);
    CHECK(stream.body.find("create_element") != std::string::npos);

    auto event_response = transport.handle_request({
        "POST",
        "/events",
        R"({"type":"click","id":"aui-test"})",
    });
    CHECK(event_response.status == 202);
    auto events = transport.take_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].find("aui-test") != std::string::npos);
}
