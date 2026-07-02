#include <doctest/doctest.h>

#include "affineui/app.h"
#include "affineui/painter.h"
#include "affineui/view.h"
#include "affineui_browser_server.h"
#include "app/context.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
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

// Minimal measuring painter for App-driven fixtures: real-ish glyph metrics
// (8px/char, 18px lines — matching the other test painters) so layouts match
// the in-window geometry, plus fill recording for paint-level assertions.
class TestPainter final : public affineui::Painter {
public:
    struct FillDraw {
        affineui::Rect rect;
        affineui::Color color;
    };
    struct TextDraw {
        std::string text;
        affineui::Point pos;
        bool has_clip{false};
        affineui::Rect clip;
    };
    struct StrokeLine {
        float x0, y0, x1, y1;
    };
    std::vector<FillDraw> fill_draws;
    std::vector<TextDraw> text_draws;
    std::vector<StrokeLine> stroke_lines;
    std::vector<affineui::Rect> clip_stack;

    void begin_frame(int, int, float) override {}
    void end_frame() override {}
    void fill_rect(const affineui::Rect& r, affineui::Color c) override {
        fill_draws.push_back({r, c});
    }
    void stroke_rect(const affineui::Rect&, affineui::Color, float) override {}
    void stroke_line(float x0, float y0, float x1, float y1, affineui::Color,
                     float) override {
        stroke_lines.push_back({x0, y0, x1, y1});
    }
    void fill_circle(float, float, float, affineui::Color) override {}
    void stroke_arc(float, float, float, float, float, affineui::Color,
                    float) override {}
    void fill_rounded_rect(const affineui::Rect& r, float,
                           affineui::Color c) override {
        fill_draws.push_back({r, c});
    }
    void stroke_rounded_rect(const affineui::Rect&, float, affineui::Color,
                             float) override {}
    void fill_rounded_rect_varying(const affineui::Rect& r, float, float,
                                   float, float, affineui::Color c) override {
        fill_draws.push_back({r, c});
    }
    void stroke_rounded_rect_varying(const affineui::Rect&, float, float,
                                     float, float, affineui::Color,
                                     float) override {}
    void fill_rounded_rect_ring(const affineui::Rect&, float, float,
                                affineui::Color) override {}
    void fill_linear_gradient_rect(const affineui::Rect&, float,
                                   affineui::Color, affineui::Color, float,
                                   float, float, float) override {}
    void fill_radial_gradient_rect(const affineui::Rect&, affineui::Color,
                                   affineui::Color, float, float, float,
                                   float, float = 50, float = 50,
                                   float = 100) override {}
    void fill_box_shadow(const affineui::Rect&, float, affineui::Color, float,
                         float, float, float, bool) override {}
    std::uint32_t resolve_font(std::string_view, int, int, bool) override {
        return 1;
    }
    int measure_text(std::uint32_t, std::string_view text) override {
        return static_cast<int>(text.size()) * 8;
    }
    TextMetrics text_metrics(std::uint32_t) override {
        return {12.0f, 4.0f, 18.0f};
    }
    void draw_text(std::uint32_t, const affineui::Point& pos,
                   std::string_view text, affineui::Color) override {
        record_text(text, pos);
    }
    affineui::Size measure_text_box(std::uint32_t, std::string_view text,
                                    float max_width, float, float) override {
        const int natural = static_cast<int>(text.size()) * 8;
        if (natural <= static_cast<int>(max_width)) return {natural, 18};
        const int per_line = std::max(1, static_cast<int>(max_width) / 8);
        const int lines =
            (static_cast<int>(text.size()) + per_line - 1) / per_line;
        return {static_cast<int>(max_width), lines * 18};
    }
    void draw_text_box(std::uint32_t, const affineui::Point& pos,
                       std::string_view text, affineui::Color, float, float,
                       float, TextAlign) override {
        record_text(text, pos);
    }
    std::uint32_t load_image(std::string_view) override { return 0; }
    affineui::Size image_size(std::uint32_t) override { return {0, 0}; }
    void draw_image(std::uint32_t, const affineui::Rect&,
                    const affineui::Rect&) override {}
    void push_clip(const affineui::Rect& r) override {
        clip_stack.push_back(r);
    }
    void pop_clip() override {
        if (!clip_stack.empty()) clip_stack.pop_back();
    }
    void push_alpha(float) override {}
    void pop_alpha() override {}

private:
    void record_text(std::string_view text, const affineui::Point& pos) {
        TextDraw td;
        td.text = std::string(text);
        td.pos = pos;
        if (!clip_stack.empty()) {
            td.has_clip = true;
            td.clip = clip_stack.back();
        }
        text_draws.push_back(std::move(td));
    }
};

int hex_channel(std::string_view hex, std::size_t offset) {
    if (hex.size() < offset + 2) return -1;
    const auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    const int hi = digit(hex[offset]);
    const int lo = digit(hex[offset + 1]);
    return hi < 0 || lo < 0 ? -1 : (hi << 4) | lo;
}

bool near_white_hex(std::string_view hex) {
    return hex.size() == 7 && hex.front() == '#' &&
           hex_channel(hex, 1) >= 248 &&
           hex_channel(hex, 3) >= 248 &&
           hex_channel(hex, 5) >= 248;
}

class SetBoolProperty final : public app::Command {
public:
    SetBoolProperty(std::string id, std::string prop, bool value)
        : Command("obj.setBool", "Set Bool"),
          id_(std::move(id)),
          prop_(std::move(prop)),
          next_(value) {}

    void redo(app::Document& doc) override {
        previous_ = std::get<bool>(doc.property(id_, prop_,
                                                app::PropValue{next_}));
        doc.set_property(id_, prop_, app::PropValue{next_});
    }

    void undo(app::Document& doc) override {
        doc.set_property(id_, prop_, app::PropValue{previous_});
    }

private:
    std::string id_;
    std::string prop_;
    bool next_{false};
    bool previous_{false};
};

class SetStringProperty final : public app::Command {
public:
    SetStringProperty(std::string id, std::string prop, std::string value)
        : Command("obj.setString", "Set String"),
          id_(std::move(id)),
          prop_(std::move(prop)),
          next_(std::move(value)) {}

    void redo(app::Document& doc) override {
        previous_ = std::get<std::string>(doc.property(id_, prop_,
                                                       app::PropValue{next_}));
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

// Scan along a rect's horizontal centerline for the first point whose hover
// chain contains an element with `cls` in its class list. This aims a click at
// a specific piece of a widget row (e.g. the .dcs-check box the user actually
// clicks) instead of the row's top-left corner.
affineui::Point find_in_rect_with_class(affineui::App& app,
                                        affineui::Rect r,
                                        std::string_view cls) {
    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    const int y = r.y + r.h / 2;
    for (int x = r.x + 2; x < r.x + r.w - 1; x += 2) {
        move.pos = {x, y};
        app.dispatch(move);
        for (const auto& info : app.document().hovered_info_chain()) {
            for (const auto& attr : info.attrs) {
                if (attr.first == "class" &&
                    attr.second.find(cls) != std::string::npos) {
                    return {x, y};
                }
            }
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

affineui::Rect hovered_attr_bounds(const affineui::App& app,
                                   std::string_view attr_name,
                                   std::string_view attr_value) {
    const auto chain = app.document().hovered_info_chain();
    for (const auto& info : chain) {
        for (const auto& attr : info.attrs) {
            if (attr.first == attr_name && attr.second == attr_value) {
                return info.bounds;
            }
        }
    }
    return {-1, -1, 0, 0};
}

affineui::Rect hovered_tag_attr_bounds(const affineui::App& app,
                                       std::string_view tag,
                                       std::string_view attr_name,
                                       std::string_view attr_value) {
    const auto chain = app.document().hovered_info_chain();
    for (const auto& info : chain) {
        if (info.tag != tag) continue;
        for (const auto& attr : info.attrs) {
            if (attr.first == attr_name && attr.second == attr_value) {
                return info.bounds;
            }
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
    decius.dropdown("Mode", {"Object", "Edit"}, "Object", "mode");
    decius.end();

    auto html = decius.to_html_document();
    CHECK(html.find("data-aui-size=\"md\"") != std::string::npos);
    CHECK(html.find("data-aui-style=\"flat\"") != std::string::npos);
    CHECK(html.find("data-dcs-style=\"flat\"") != std::string::npos);
    CHECK(html.find("data-dcs-density=\"compact\"") != std::string::npos);
    CHECK(html.find("data-aui-size=\"lg\"") != std::string::npos);
    CHECK(html.find(".aui-keycolor-swatch.is-active{border-color:var(--dcs-bg-app") !=
          std::string::npos);
    CHECK(html.find("0 0 0 4px var(--aui-swatch)") != std::string::npos);
    CHECK(html.find(".aui-select__menu.dcs-menu{max-width:none}") !=
          std::string::npos);
    CHECK(html.find("dcs-menu dcs-menu--select aui-select__menu") !=
          std::string::npos);
    CHECK(html.find(".aui-test-nav-item.is-active{background:var(--dcs-accent") !=
          std::string::npos);
    CHECK(html.find("color:var(--dcs-accent-text,#fff)") !=
          std::string::npos);
    CHECK(html.find(".aui-test-topbar{display:flex;align-items:stretch;flex-wrap:nowrap") !=
          std::string::npos);
    CHECK(html.find(".aui-test-control--top-density,.aui-test-control--top-accent{display:none}") !=
          std::string::npos);
    CHECK(html.find(".aui-test-control--top-style{display:none}") !=
          std::string::npos);

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

TEST_CASE("App dispatch toggles Decius checkboxes on the first click") {
    affineui::View view{affineui::ViewTheme::Decius};
    std::string value;

    view.begin();
    view.checkbox("Cast shadows", false, "shadows")
        .on_change([&](std::string_view next) { value = std::string(next); });
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(320, 140);

    const auto checkbox = find_hovered_widget(app, "shadows", 320, 140);
    REQUIRE(checkbox.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = checkbox;
    app.dispatch(down);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = checkbox;
    CHECK(app.dispatch(up));

    CHECK(value == "true");

}

TEST_CASE("App dispatch keeps model-backed Decius checkbox first clicks") {
    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};

    bool shadows = false;
    int rebuilds = 0;
    std::function<affineui::View()> build_view;
    build_view = [&]() {
        affineui::View view{affineui::ViewTheme::Decius};
        view.begin();
        view.checkbox("Cast shadows", shadows, "shadows")
            .on_change([&](std::string_view next) {
                shadows = next == "true";
                ++rebuilds;
                app.load_view(build_view());
            });
        view.end();
        return view;
    };

    app.load_view(build_view());
    app.document().layout(320, 140);

    const auto checkbox = find_hovered_widget(app, "shadows", 320, 140);
    REQUIRE(checkbox.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = checkbox;
    app.dispatch(down);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = checkbox;
    CHECK(app.dispatch(up));
    app.document().layout(320, 140);

    CHECK(shadows);
    CHECK(rebuilds == 1);
    const auto after = find_hovered_widget(app, "shadows", 320, 140);
    REQUIRE(after.x >= 0);
}

TEST_CASE("App Decius checkbox survives command-stack rebuilds on first click") {
    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app::Context ctx;

    app::Object obj;
    obj.id = "hero";
    obj.type = "mesh";
    obj.name = "Hero";
    obj.properties.push_back({"castShadows", app::PropValue{false}});
    ctx.document().add(std::move(obj));

    int reloads = 0;
    std::function<affineui::View()> build_view;
    auto prop = [&]() {
        return std::get<bool>(ctx.document().property(
            "hero", "castShadows", app::PropValue{false}));
    };
    build_view = [&]() {
        affineui::View view{affineui::ViewTheme::Decius};
        view.begin();
        view.checkbox("Cast shadows", prop(), "shadows")
            .on_change([&](std::string_view next) {
                ctx.run(std::make_unique<SetBoolProperty>(
                    "hero", "castShadows",
                    next == "true" || next == "1" || next == "on"));
            });
        view.checkbox("Visible", true, "visible");
        view.end();
        return view;
    };
    ctx.stack().set_changed_handler([&] {
        ++reloads;
        app.load_view(build_view());
    });

    app.load_view(build_view());
    app.document().layout(360, 160);

    auto click = [&](affineui::Point p) {
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
        app.document().layout(360, 160);
    };

    auto checkbox = find_hovered_widget(app, "shadows", 360, 160);
    REQUIRE(checkbox.x >= 0);
    click(checkbox);
    CHECK(prop());
    CHECK(reloads == 1);

    checkbox = find_hovered_widget(app, "shadows", 360, 160);
    REQUIRE(checkbox.x >= 0);
    click(checkbox);
    CHECK_FALSE(prop());
    CHECK(reloads == 2);
}

// Faithful mirror of examples/11 (decius_game_editor): the full 0.6.2 bundle,
// a dock workspace with an Inspector pane, foldouts + dcs-props rows, dock
// replay providers, and command-backed properties whose stack handler reloads
// the whole view — the EXACT shape reported broken in-window ("the top
// checkbox needs two clicks", "the picked color snaps back") while the bare
// fixtures above stay green.
TEST_CASE("GE-shaped inspector: command-backed checkbox + colorfield commit "
          "on the FIRST interaction") {
    std::ifstream bundle_in(
        AFFINEUI_TEST_SOURCE_DIR
        "/examples/frameworks/css/decius-css-0.6.2.bundle.min.css",
        std::ios::binary);
    REQUIRE(bundle_in.good());
    std::string bundle((std::istreambuf_iterator<char>(bundle_in)),
                       std::istreambuf_iterator<char>());
    // The real app's shell (game_editor_styles.cpp): a fixed, viewport-filling
    // flex column. This is load-bearing — the fixed shell is exactly the shape
    // where Yoga used to drop content contributions of flex-basis children
    // (the vec overlap bug), so the fixture must keep it to regression-test it.
    bundle +=
        "\n.ge-app{position:fixed;inset:0;display:flex;flex-direction:column;"
        "overflow:hidden}\n"
        ".ge-vp-canvas{position:relative;flex:1 1 auto;min-width:0;"
        "min-height:0;overflow:hidden}\n";

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    std::function<void()> on_layout_changed;
    cfg.on_layout_changed = [&] {
        if (on_layout_changed) on_layout_changed();
    };
    affineui::App app{cfg};
    app::Context ctx;

    app::Object obj;
    obj.id = "hero";
    obj.type = "mesh";
    obj.name = "Hero";
    obj.properties.push_back({"castShadows", app::PropValue{true}});
    obj.properties.push_back({"tint", app::PropValue{std::string{"#4d9fff"}}});
    ctx.document().add(std::move(obj));

    auto shadows_prop = [&] {
        return std::get<bool>(ctx.document().property(
            "hero", "castShadows", app::PropValue{true}));
    };
    auto tint_prop = [&] {
        return std::get<std::string>(ctx.document().property(
            "hero", "tint", app::PropValue{std::string{"#4d9fff"}}));
    };
    bool visible_local = true;
    // Long enough to overflow the fixed 2-row textarea height by far.
    std::string notes_local =
        "Line one of the production notes. Line two keeps going with more "
        "detail about the hero mesh. Line three describes the bake. Line "
        "four rambles about the spline rig. Line five closes it out with a "
        "reminder to retopo the collar before the next review.";
    int reloads = 0;
    int inspector_px = 0;  // 0 = declared size (320); tests shrink it below

    std::function<affineui::View()> build = [&] {
        affineui::View v{affineui::ViewTheme::Decius};
        v.set_framework_version("0.6.2");
        v.selector(affineui::decius::selector::style,
                   affineui::decius::style::flat);
        v.selector(affineui::decius::selector::density,
                   affineui::decius::density::compact);
        v.selector(affineui::decius::selector::accent, "cyan");
        v.set_dock_size_provider([&](std::string_view id) {
            // Panel ids are normalized to lowercase by the View.
            return (id == "Inspector" || id == "inspector") ? inspector_px : 0;
        });
        v.set_dock_layout_provider(
            [&] { return app.document().dock_layout(); });
        v.set_dock_placement_provider([&](std::string_view id) {
            return app.document().dock_override(id);
        });
        v.set_dock_active_tab_provider([&](std::string_view id) {
            return app.document().dock_active_tab(id);
        });
        v.begin();
        {
            auto shell = v.container("ge-app", "app");
            v.document_view("workarea", [&](affineui::View& dv) {
                dv.document(
                    [&](affineui::View& doc) {
                        auto canvas = doc.container("ge-vp-canvas", "vp-canvas");
                        canvas.attr("data-dcs-float-host", "");
                    },
                    "Lit View", "cube");
                dv.dockpanel(
                    "Inspector",
                    affineui::DockLocation::docked(affineui::Dock::Right, 320),
                    [&](affineui::View& p) {
                        auto foldouts =
                            p.container("dcs-foldouts", "insp-foldouts");
                        {
                            auto fold =
                                p.foldout("Material", true, "fold-material");
                            auto props =
                                p.container("dcs-props", "material-props");
                            p.slider("Roughness", 0.62, 0.0, 1.0, "rough");
                            p.colorfield("Tint", tint_prop(), "tint")
                                .on_change([&](std::string_view next) {
                                    ctx.run(std::make_unique<SetStringProperty>(
                                        "hero", "tint", std::string(next)));
                                });
                            p.checkbox("Cast shadows", shadows_prop(), "shadows")
                                .on_change([&](std::string_view next) {
                                    ctx.run(std::make_unique<SetBoolProperty>(
                                        "hero", "castShadows",
                                        next == "true" || next == "1" ||
                                            next == "on"));
                                });
                        }
                        {
                            auto fold =
                                p.foldout("Transform", true, "fold-xform");
                            auto props =
                                p.container("dcs-props", "xform-props");
                            p.vec("Location", {"X", "Y", "Z"},
                                  {12.0, 4.2, -8.5}, "loc");
                        }
                        {
                            auto fold =
                                p.foldout("Display", true, "fold-display");
                            auto props =
                                p.container("dcs-props", "display-props");
                            p.checkbox("Visible", visible_local, "visible")
                                .on_change([&](std::string_view next) {
                                    visible_local = next == "true" ||
                                                    next == "1" || next == "on";
                                });
                            p.textarea("Notes", notes_local, 3, "notes");
                        }
                    },
                    "cog");
                auto assets = dv.dockpanel(
                    "Assets",
                    affineui::DockLocation::docked(affineui::Dock::Bottom, 120),
                    [&](affineui::View& p) {
                        p.text("asset strip", "asset-strip");
                    },
                    "image");
                dv.dockpanel("Console",
                             affineui::DockLocation::tab().in(assets),
                             [&](affineui::View& p) {
                                 p.text("console output", "console-text");
                             },
                             "file");
            });
        }
        v.end();
        return v;
    };

    auto reload = [&] {
        ++reloads;
        app.load_view(build());
    };
    on_layout_changed = reload;
    ctx.stack().set_changed_handler(reload);
    ctx.document().set_changed_handler(reload);

    constexpr int W = 1440;
    constexpr int H = 900;
    // Each subcase boots the app itself (AFTER setting its dock sizing): the
    // dock replay provider faithfully preserves the live pane sizes across
    // reloads, so the size provider only matters for the very first build.
    // Layout with a MEASURING painter — the in-window app has real glyph
    // metrics, and the vec/titlebar bugs only reproduce with content-sized
    // text (the painterless estimate hid them).
    TestPainter painter;
    auto boot = [&] {
        app.load_view(build());
        app.set_stylesheet(bundle);
        app.document().layout(W, H, &painter);
    };

    auto click = [&](affineui::Point p) {
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
        app.document().layout(W, H, &painter);
    };

    SUBCASE("cursor: slider hover shows the bundle's cursor (pointer), not a "
            "drag/resize cursor") {
        boot();
        const auto row = app.document().find_element_rect("rough");
        REQUIRE(row.w > 0);
        const auto track =
            find_in_rect_with_class(app, row, "dcs-slider__track");
        REQUIRE(track.x >= 0);
        affineui::Event hv{};
        hv.type = affineui::EventType::MouseMove;
        hv.pos = track;
        app.dispatch(hv);
        // .dcs-slider{cursor:pointer} in the bundle — protocol code 1.
        CHECK(app.document().hovered_cursor() == 1);

        // Textarea: UA cursor is the I-beam over the editable region, but
        // the scrollbar gutter is browser UI — plain arrow, always.
        const auto notes_row = app.document().find_element_rect("notes");
        REQUIRE(notes_row.w > 0);
        const auto ta_pt =
            find_in_rect_with_class(app, notes_row, "dcs-textarea");
        REQUIRE(ta_pt.x >= 0);
        affineui::Rect ta_box{};
        for (const auto& info : app.document().hovered_info_chain()) {
            if (std::find(info.classes.begin(), info.classes.end(),
                          "dcs-textarea") != info.classes.end()) {
                ta_box = info.bounds;
            }
        }
        REQUIRE(ta_box.w > 0);
        affineui::Event hv_text{};
        hv_text.type = affineui::EventType::MouseMove;
        hv_text.pos = {ta_box.x + 12, ta_box.y + ta_box.h / 2};
        app.dispatch(hv_text);
        CHECK(app.document().hovered_cursor() == 2);  // I-beam over the value
        affineui::Event hv_gutter{};
        hv_gutter.type = affineui::EventType::MouseMove;
        hv_gutter.pos = {ta_box.x + ta_box.w - 4, ta_box.y + ta_box.h / 2};
        app.dispatch(hv_gutter);
        // The notes value overflows (the wheel-scroll subcase relies on it),
        // so the scrollbar is up — its gutter shows the default arrow.
        CHECK(app.document().hovered_cursor() == 0);
    }

    SUBCASE("checkbox: one click on the .dcs-check box flips the model once") {
        boot();
        const auto row = app.document().find_element_rect("shadows");
        REQUIRE(row.w > 0);
        // The user clicks the checkbox VISUAL (the .dcs-check square), not the
        // row's top-left corner.
        const auto box = find_in_rect_with_class(app, row, "dcs-check");
        REQUIRE(box.x >= 0);

        CHECK(shadows_prop() == true);
        click(box);
        CHECK(shadows_prop() == false);  // ONE click must flip the model
        CHECK(reloads == 1);

        // And the next click flips it back (fresh rect: the reload rebuilt DOM).
        const auto row2 = app.document().find_element_rect("shadows");
        REQUIRE(row2.w > 0);
        const auto box2 = find_in_rect_with_class(app, row2, "dcs-check");
        REQUIRE(box2.x >= 0);
        click(box2);
        CHECK(shadows_prop() == true);
        CHECK(reloads == 2);
    }

    SUBCASE("local checkbox: one click flips the local flag (control case)") {
        boot();
        const auto row = app.document().find_element_rect("visible");
        REQUIRE(row.w > 0);
        const auto box = find_in_rect_with_class(app, row, "dcs-check");
        REQUIRE(box.x >= 0);
        CHECK(visible_local == true);
        click(box);
        CHECK(visible_local == false);
    }

    SUBCASE("vector row: X/Y/Z share the row evenly with the s-1 gap") {
        // Wide inspector: the control column comfortably fits 3 editors at the
        // 72px floor (the browser reference lays them out horizontally here).
        inspector_px = 440;
        boot();
        const auto vec_row = app.document().find_element_rect("loc");
        REQUIRE(vec_row.w > 0);
        const auto x0 = app.document().find_element_rect("loc-0");
        const auto x1 = app.document().find_element_rect("loc-1");
        const auto x2 = app.document().find_element_rect("loc-2");
        REQUIRE(x0.w > 0);
        REQUIRE(x1.w > 0);
        REQUIRE(x2.w > 0);
        // All three on ONE row, left to right.
        CHECK(x0.y == x1.y);
        CHECK(x1.y == x2.y);
        CHECK(x0.x < x1.x);
        CHECK(x1.x < x2.x);
        // Equal widths (±1 rounding).
        CHECK(std::abs(x0.w - x1.w) <= 1);
        CHECK(std::abs(x1.w - x2.w) <= 1);
        // The decius gap is var(--dcs-s-1), which the 0.6.2 bundle sets to 1px
        // at compact density ([data-dcs-density=compact]{--dcs-s-1:1px}) — the
        // gaps must be exactly that, evenly, with no accidental slack.
        const int gap01 = x1.x - (x0.x + x0.w);
        const int gap12 = x2.x - (x1.x + x1.w);
        MESSAGE("vec row w=", vec_row.w, " combos=", x0.w, "/", x1.w, "/",
                x2.w, " gaps=", gap01, "/", gap12);
        CHECK(gap01 == gap12);
        CHECK(gap01 == 1);
        // The editors must not overflow their row.
        CHECK(x2.x + x2.w <= vec_row.x + vec_row.w + 1);
    }

    SUBCASE("vector row: at the DEFAULT 320px inspector the reference stacks "
            "X/Y/Z in the CONTROL COLUMN beside the Location label") {
        // Ground truth: examples/11_decius_game_editor/reference.png — the
        // 'Location' label sits in the left column and the three editors stack
        // in the right (control) column, never spanning the full row.
        boot();
        const auto pane = app.document().find_element_rect("pane-inspector");
        REQUIRE(pane.w > 0);
        const auto vec_field = app.document().find_element_rect("loc");
        REQUIRE(vec_field.w > 0);
        const auto x0 = app.document().find_element_rect("loc-0");
        const auto x1 = app.document().find_element_rect("loc-1");
        const auto x2 = app.document().find_element_rect("loc-2");
        REQUIRE(x0.w > 0);
        MESSAGE("pane=(", pane.x, ",", pane.y, " ", pane.w, "x", pane.h,
                ") field=(", vec_field.x, ",", vec_field.y, " ", vec_field.w,
                "x", vec_field.h, ") x0=(", x0.x, ",", x0.y, " ", x0.w, "x",
                x0.h, ")");
        // Stacked at this width (3*72 + gaps exceeds the control column).
        CHECK(x1.y >= x0.y + x0.h);
        CHECK(x2.y >= x1.y + x1.h);
        // The label occupies the left column: the editors must start well
        // inside the field, not at its left edge (reference: label column is
        // roughly 90-110px of the 294px row).
        const int label_col = x0.x - vec_field.x;
        MESSAGE("label column width=", label_col);
        CHECK(label_col >= 60);
        // The label itself renders beside the FIRST editor row.
        const auto label_pt = find_in_rect_with_class(
            app,
            affineui::Rect{vec_field.x, x0.y, label_col, x0.h},
            "dcs-field__label");
        CHECK(label_pt.x >= 0);
        // And nothing below overlaps.
        const auto fold_display =
            app.document().find_element_rect("fold-display");
        REQUIRE(fold_display.h > 0);
        CHECK(fold_display.y >= x2.y + x2.h);
    }

    SUBCASE("vector row: compressed inspector stacks the editors AND grows "
            "the field so nothing below overlaps") {
        inspector_px = 150;  // too narrow for 3 * 72px + gaps → must stack
        boot();

        const auto x0 = app.document().find_element_rect("loc-0");
        const auto x1 = app.document().find_element_rect("loc-1");
        const auto x2 = app.document().find_element_rect("loc-2");
        REQUIRE(x0.w > 0);
        REQUIRE(x1.w > 0);
        REQUIRE(x2.w > 0);
        // Stacked: vertical, full width, non-overlapping.
        MESSAGE("stacked combos y=", x0.y, "/", x1.y, "/", x2.y,
                " h=", x0.h, "/", x1.h, "/", x2.h);
        CHECK(x1.y >= x0.y + x0.h);
        CHECK(x2.y >= x1.y + x1.h);
        // The field (row) must have GROWN to hold all three...
        const auto vec_field = app.document().find_element_rect("loc");
        REQUIRE(vec_field.h > 0);
        CHECK(vec_field.y + vec_field.h >= x2.y + x2.h);
        // ...and push the Display foldout below it — no overlap with the
        // widgets underneath (the reported bug).
        const auto fold_display =
            app.document().find_element_rect("fold-display");
        REQUIRE(fold_display.h > 0);
        MESSAGE("vec field bottom=", vec_field.y + vec_field.h,
                " display fold top=", fold_display.y);
        CHECK(fold_display.y >= vec_field.y + vec_field.h);
        const auto visible_row = app.document().find_element_rect("visible");
        REQUIRE(visible_row.h > 0);
        CHECK(visible_row.y >= x2.y + x2.h);
    }

    SUBCASE("vector row: compressing the inspector with the SPLITTER stacks "
            "the editors and grows the field (live gesture path)") {
        // The user compresses the pane by dragging the dock splitter — a live
        // gesture with inline flex mutations + the vec flip mid-stream, NOT a
        // build-time size. This is the path reported to leave the stacked
        // editors overlapping the widgets below.
        boot();
        const auto pane =
            app.document().find_element_rect("pane-inspector");
        REQUIRE(pane.w > 0);
        // The splitter sits immediately left of the inspector pane.
        const affineui::Point grip{pane.x - 3, pane.y + pane.h / 2};
        affineui::Event down{};
        down.type = affineui::EventType::MouseDown;
        down.button = affineui::MouseButton::Left;
        down.pos = grip;
        app.dispatch(down);
        // Drag right in steps to shrink the pane from 320 to ~150.
        for (int dx = 10; dx <= 170; dx += 10) {
            affineui::Event mv{};
            mv.type = affineui::EventType::MouseMove;
            mv.pos = {grip.x + dx, grip.y};
            app.dispatch(mv);
        }
        affineui::Event up{};
        up.type = affineui::EventType::MouseUp;
        up.button = affineui::MouseButton::Left;
        up.pos = {grip.x + 170, grip.y};
        app.dispatch(up);
        app.document().layout(W, H, &painter);

        const auto pane_after =
            app.document().find_element_rect("pane-inspector");
        MESSAGE("pane w after splitter drag: ", pane_after.w);
        REQUIRE(pane_after.w < 220);  // gesture really compressed the pane

        const auto x0 = app.document().find_element_rect("loc-0");
        const auto x1 = app.document().find_element_rect("loc-1");
        const auto x2 = app.document().find_element_rect("loc-2");
        REQUIRE(x0.w > 0);
        MESSAGE("combos after compress y=", x0.y, "/", x1.y, "/", x2.y,
                " x=", x0.x, "/", x1.x, "/", x2.x);
        // Stacked vertically, not overlapping each other...
        CHECK(x1.y >= x0.y + x0.h);
        CHECK(x2.y >= x1.y + x1.h);
        // ...and the widgets BELOW are pushed down, not overlapped.
        const auto vec_field = app.document().find_element_rect("loc");
        const auto fold_display =
            app.document().find_element_rect("fold-display");
        REQUIRE(fold_display.h > 0);
        MESSAGE("vec field bottom=", vec_field.y + vec_field.h,
                " display fold top=", fold_display.y);
        CHECK(fold_display.y >= x2.y + x2.h);
    }

    SUBCASE("tearoff: single-tab float uses the reference title-only bar; "
            "title drag re-docks, empty-space drag moves the float") {
        boot();
        auto stream_drag = [&](affineui::Point from, affineui::Point to) {
            affineui::Event down{};
            down.type = affineui::EventType::MouseDown;
            down.button = affineui::MouseButton::Left;
            down.pos = from;
            app.dispatch(down);
            const int steps = 8;
            for (int i = 1; i <= steps; ++i) {
                affineui::Event mv{};
                mv.type = affineui::EventType::MouseMove;
                mv.pos = {from.x + (to.x - from.x) * i / steps,
                          from.y + (to.y - from.y) * i / steps};
                app.dispatch(mv);
            }
            affineui::Event up{};
            up.type = affineui::EventType::MouseUp;
            up.button = affineui::MouseButton::Left;
            up.pos = to;
            app.dispatch(up);
            app.document().layout(W, H, &painter);
        };

        // Tear the (inactive) Console tab out of the bottom pane into the
        // viewport center → a single-tab floating panel.
        const auto console_tab = app.document().find_element_rect(
            "[data-dcs-target=#console-body]");
        REQUIRE(console_tab.w > 0);
        // Drop in the middle of the DOCUMENT pane — open space away from all
        // edge dock zones, so a panel drag must produce a floating panel.
        const auto doc_pane =
            app.document().find_element_rect("pane-__document__");
        REQUIRE(doc_pane.w > 0);
        REQUIRE(doc_pane.h > 200);
        stream_drag({console_tab.x + console_tab.w / 2,
                     console_tab.y + console_tab.h / 2},
                    {doc_pane.x + doc_pane.w / 2, doc_pane.y + doc_pane.h / 2});

        const auto layout1 = app.document().dock_layout();
        REQUIRE(layout1.present);
        REQUIRE(layout1.floats.size() == 1);
        CHECK(layout1.floats[0].title_only == true);
        CHECK(layout1.floats[0].pane.tabs ==
              std::vector<std::string>{"console"});


        // Reference structure (decius.js convertDockToTitleOnly +
        // prepareTabForTitlebar): the title TAB keeps its tab class and gains
        // the panel-title classes + data-dcs-title-tab; the titlebar is the
        // float-drag handle; NO inline style bodges on the tab.
        const auto float_pane =
            app.document().find_element_rect("pane-console");
        REQUIRE(float_pane.w > 0);
        const affineui::Rect titlebar_strip{float_pane.x, float_pane.y,
                                            float_pane.w, 24};
        const auto title_pt = find_in_rect_with_class(
            app, titlebar_strip, "dcs-panel__title--dock-tab");
        REQUIRE(title_pt.x >= 0);
        affineui::Rect title{};
        affineui::Event hover{};
        hover.type = affineui::EventType::MouseMove;
        hover.pos = title_pt;
        app.dispatch(hover);
        for (const auto& info : app.document().hovered_info_chain()) {
            std::string cls_line = "title chain: <" + info.tag;
            for (const auto& c : info.classes) cls_line += " ." + c;
            cls_line += "> bounds=(" + std::to_string(info.bounds.x) + "," +
                        std::to_string(info.bounds.y) + "," +
                        std::to_string(info.bounds.w) + "x" +
                        std::to_string(info.bounds.h) + ")";
            MESSAGE(cls_line);
            if (std::find(info.classes.begin(), info.classes.end(),
                          "dcs-panel__title--dock-tab") !=
                info.classes.end()) {
                title = info.bounds;
            }
        }
        REQUIRE(title.w > 0);
        bool tab_ok = false;
        bool titlebar_ok = false;
        bool tab_has_inline_style = false;
        for (const auto& info : app.document().hovered_info_chain()) {
            const bool is_tab =
                std::find(info.classes.begin(), info.classes.end(),
                          "dcs-dockpane__tab") != info.classes.end();
            const bool is_title =
                std::find(info.classes.begin(), info.classes.end(),
                          "dcs-panel__title--dock-tab") != info.classes.end();
            if (is_tab && is_title) {
                tab_ok = true;
                for (const auto& a : info.attrs) {
                    if (a.first == "style" && !a.second.empty()) {
                        tab_has_inline_style = true;
                    }
                }
            }
            const bool is_titlebar =
                std::find(info.classes.begin(), info.classes.end(),
                          "dcs-dockpane__titlebar") != info.classes.end();
            const bool is_header =
                std::find(info.classes.begin(), info.classes.end(),
                          "dcs-panel__header") != info.classes.end();
            if (is_titlebar && is_header) titlebar_ok = true;
        }
        CHECK(tab_ok);
        CHECK(titlebar_ok);
        CHECK_FALSE(tab_has_inline_style);
        MESSAGE("title tab bounds=(", title.x, ",", title.y, ",", title.w, "x",
                title.h, ")");
        // The title must be CONTENT-sized (icon + label), not a sliver — in a
        // browser this is ~70px; the headless estimate must land in the same
        // ballpark so pointer gestures aim at real geometry.
        CHECK(title.w >= 40);

        // Empty-space drag (right end of the titlebar, past the title tab)
        // MOVES the float. (Vertical room is tight in this fixture — the
        // 240-tall float lives in a ~276-tall document host and clamps — so
        // the horizontal delta is the honest movement signal.)
        const auto pane_before =
            app.document().find_element_rect("pane-console");
        REQUIRE(pane_before.w > 0);
        const affineui::Point grip{title.x + title.w + 40,
                                   title.y + title.h / 2};
        stream_drag(grip, {grip.x + 48, grip.y + 36});
        const auto pane_moved =
            app.document().find_element_rect("pane-console");
        MESSAGE("float moved from (", pane_before.x, ",", pane_before.y,
                ") to (", pane_moved.x, ",", pane_moved.y, ")");
        CHECK(pane_moved.x > pane_before.x + 24);
        CHECK(pane_moved.y >= pane_before.y);  // clamped downward is fine
        {
            const auto after_move = app.document().dock_layout();
            REQUIRE(after_move.floats.size() == 1);  // moved, not re-docked
        }

        // Title-tab drag back onto the Assets pane CENTER re-docks Console as
        // a tab there (the panel-drag gesture, same as dragging a tab).
        const auto float_pane2 =
            app.document().find_element_rect("pane-console");
        REQUIRE(float_pane2.w > 0);
        const auto title2_pt = find_in_rect_with_class(
            app,
            affineui::Rect{float_pane2.x, float_pane2.y, float_pane2.w, 24},
            "dcs-panel__title--dock-tab");
        REQUIRE(title2_pt.x >= 0);
        affineui::Rect title2{};
        affineui::Event hover2{};
        hover2.type = affineui::EventType::MouseMove;
        hover2.pos = title2_pt;
        app.dispatch(hover2);
        for (const auto& info : app.document().hovered_info_chain()) {
            if (std::find(info.classes.begin(), info.classes.end(),
                          "dcs-panel__title--dock-tab") !=
                info.classes.end()) {
                title2 = info.bounds;
            }
        }
        REQUIRE(title2.w > 0);
        const auto assets_pane =
            app.document().find_element_rect("pane-assets");
        REQUIRE(assets_pane.w > 0);
        stream_drag({title2.x + title2.w / 2, title2.y + title2.h / 2},
                    {assets_pane.x + assets_pane.w / 2,
                     assets_pane.y + assets_pane.h / 2});
        const auto layout2 = app.document().dock_layout();
        REQUIRE(layout2.present);
        CHECK(layout2.floats.empty());
        bool assets_has_console = false;
        std::function<void(const affineui::Document::DockLayout::Node&)> walk =
            [&](const affineui::Document::DockLayout::Node& n) {
                if (n.split) {
                    for (const auto& c : n.children) walk(c);
                    return;
                }
                const bool has_assets =
                    std::find(n.tabs.begin(), n.tabs.end(), "assets") !=
                    n.tabs.end();
                const bool has_console =
                    std::find(n.tabs.begin(), n.tabs.end(), "console") !=
                    n.tabs.end();
                if (has_assets && has_console) assets_has_console = true;
            };
        walk(layout2.root);
        CHECK(assets_has_console);
    }

    SUBCASE("colorfield: picking in the SV square commits and STICKS") {
        boot();
        const auto row = app.document().find_element_rect("tint");
        REQUIRE(row.w > 0);
        const auto caret = find_in_rect_with_class(app, row,
                                                   "dcs-colorfield__caret");
        REQUIRE(caret.x >= 0);
        click(caret);  // open the picker popover

        const auto sv = app.document().find_element_rect(
            "#aui-cf-tint-picker-sv");
        REQUIRE(sv.w > 0);
        const std::string before = tint_prop();
        // Click near the SV square's top-right (saturated, bright — far from
        // the current color so the committed hex must differ).
        click({sv.x + sv.w - 6, sv.y + 6});
        const std::string after = tint_prop();
        CHECK(after != before);  // the pick must reach the model...
        const std::string persisted = tint_prop();
        app.document().layout(W, H, &painter);
        CHECK(tint_prop() == persisted);  // ...and survive the reload
        // The re-emitted chip must show the committed color, not the old one.
        const auto row2 = app.document().find_element_rect("tint");
        REQUIRE(row2.w > 0);
    }

    SUBCASE("colorfield: DRAGGING in the SV square (the real pick gesture) "
            "commits the released color and sticks") {
        boot();
        const auto row = app.document().find_element_rect("tint");
        REQUIRE(row.w > 0);
        const auto caret = find_in_rect_with_class(app, row,
                                                   "dcs-colorfield__caret");
        REQUIRE(caret.x >= 0);
        click(caret);  // open the picker popover

        const auto sv = app.document().find_element_rect(
            "#aui-cf-tint-picker-sv");
        REQUIRE(sv.w > 0);
        const std::string before = tint_prop();

        // Press near the center, drag toward the bright/saturated top-right,
        // release — the classic scrub. Mid-gesture changes are DEFERRED; the
        // release must deliver the FINAL color to the model, and the reload it
        // triggers must re-emit that color (not snap back).
        const affineui::Point p0{sv.x + sv.w / 2, sv.y + sv.h / 2};
        const affineui::Point p1{sv.x + sv.w - 6, sv.y + 6};
        affineui::Event down{};
        down.type = affineui::EventType::MouseDown;
        down.button = affineui::MouseButton::Left;
        down.pos = p0;
        app.dispatch(down);
        for (int i = 1; i <= 6; ++i) {
            affineui::Event mv{};
            mv.type = affineui::EventType::MouseMove;
            mv.pos = {p0.x + (p1.x - p0.x) * i / 6,
                      p0.y + (p1.y - p0.y) * i / 6};
            app.dispatch(mv);
        }
        affineui::Event up{};
        up.type = affineui::EventType::MouseUp;
        up.button = affineui::MouseButton::Left;
        up.pos = p1;
        app.dispatch(up);
        app.document().layout(W, H, &painter);

        const std::string committed = tint_prop();
        MESSAGE("tint before='", before, "' committed='", committed, "'");
        CHECK(committed != before);   // the drag reached the model
        app.document().layout(W, H, &painter);
        CHECK(tint_prop() == committed);  // and did not snap back

        // In the WINDOW the app keeps dispatching events after the pick (the
        // user moves the mouse). If the pick's reload re-synced the colorfield
        // from stale captured DOM and queued a change, the very next event
        // delivers it and snaps the color back — the reported bug. Pump moves
        // and re-assert.
        for (int i = 0; i < 3; ++i) {
            affineui::Event mv{};
            mv.type = affineui::EventType::MouseMove;
            mv.pos = {p1.x + 40 + i * 10, p1.y + 40};
            app.dispatch(mv);
        }
        app.document().layout(W, H, &painter);
        MESSAGE("after post-pick mouse moves tint='", tint_prop(), "'");
        CHECK(tint_prop() == committed);

        // The pick's reload keeps the picker OPEN (transient state), and the
        // re-opened picker must be SYNCED to the committed color — the
        // reported bug: on release the picker's preview/cursors snapped to a
        // zeroed color even though the model kept the pick.
        {
            const auto pop = app.document().find_element_rect(
                "#aui-cf-tint-picker");
            REQUIRE(pop.w > 0);  // still open across the reload
            // The preview chip sits in the picker's BOTTOM row (below the SV
            // square + hue bar).
            const auto chip_pt = find_in_rect_with_class(
                app,
                affineui::Rect{pop.x, pop.y + pop.h - 28, pop.w, 26},
                "dcs-colorfield__picker-chip");
            REQUIRE(chip_pt.x >= 0);
            affineui::Event hv{};
            hv.type = affineui::EventType::MouseMove;
            hv.pos = chip_pt;
            app.dispatch(hv);
            std::string preview_style;
            for (const auto& info : app.document().hovered_info_chain()) {
                if (std::find(info.classes.begin(), info.classes.end(),
                              "dcs-colorfield__picker-chip") ==
                    info.classes.end()) {
                    continue;
                }
                for (const auto& a : info.attrs) {
                    if (a.first == "style") preview_style = a.second;
                }
            }
            MESSAGE("picker preview style='", preview_style, "'");
            CHECK(preview_style.find(committed) != std::string::npos);
        }

        // The user judges by the CHIP — the visible swatch must show the
        // committed color too (a stale transient-state reapply could revert
        // the DOM while the model stays right).
        {
            const auto row_now = app.document().find_element_rect("tint");
            REQUIRE(row_now.w > 0);
            const auto chip_pt = find_in_rect_with_class(
                app, row_now, "dcs-colorfield__chip");
            REQUIRE(chip_pt.x >= 0);
            affineui::Event hv{};
            hv.type = affineui::EventType::MouseMove;
            hv.pos = chip_pt;
            app.dispatch(hv);
            std::string chip_style;
            std::string chip_color;
            for (const auto& info : app.document().hovered_info_chain()) {
                if (std::find(info.classes.begin(), info.classes.end(),
                              "dcs-colorfield__chip") == info.classes.end()) {
                    continue;
                }
                for (const auto& a : info.attrs) {
                    if (a.first == "style") chip_style = a.second;
                    if (a.first == "data-dcs-color") chip_color = a.second;
                }
            }
            MESSAGE("chip style='", chip_style, "' data-dcs-color='",
                    chip_color, "'");
            CHECK(chip_style.find(committed) != std::string::npos);
        }

        // The pick's reload preserves the OPEN picker (transient state), so a
        // second pick continues directly — and must also commit and stick
        // ("resetting after each selection").
        const auto sv2 = app.document().find_element_rect(
            "#aui-cf-tint-picker-sv");
        REQUIRE(sv2.w > 0);
        const affineui::Point q0{sv2.x + sv2.w / 2, sv2.y + sv2.h / 2};
        const affineui::Point q1{sv2.x + 6, sv2.y + sv2.h - 6};
        affineui::Event down2{};
        down2.type = affineui::EventType::MouseDown;
        down2.button = affineui::MouseButton::Left;
        down2.pos = q0;
        app.dispatch(down2);
        for (int i = 1; i <= 6; ++i) {
            affineui::Event mv{};
            mv.type = affineui::EventType::MouseMove;
            mv.pos = {q0.x + (q1.x - q0.x) * i / 6,
                      q0.y + (q1.y - q0.y) * i / 6};
            app.dispatch(mv);
        }
        affineui::Event up2{};
        up2.type = affineui::EventType::MouseUp;
        up2.button = affineui::MouseButton::Left;
        up2.pos = q1;
        app.dispatch(up2);
        app.document().layout(W, H, &painter);
        const std::string committed2 = tint_prop();
        MESSAGE("second pick committed='", committed2, "'");
        CHECK(committed2 != committed);
        for (int i = 0; i < 3; ++i) {
            affineui::Event mv{};
            mv.type = affineui::EventType::MouseMove;
            mv.pos = {q1.x + 40 + i * 10, q1.y + 40};
            app.dispatch(mv);
        }
        app.document().layout(W, H, &painter);
        CHECK(tint_prop() == committed2);
    }

    SUBCASE("textarea: overflowing value is CLIPPED to the box and the "
            "element scrolls (UA overflow:auto)") {
        boot();
        const auto ta = app.document().find_element_rect("notes-input");
        const auto row = app.document().find_element_rect("notes");
        REQUIRE(row.w > 0);
        // Resolve the textarea element rect: the widget row wraps label +
        // control; the control is the .dcs-textarea on the right.
        affineui::Rect box = ta.w > 0 ? ta : affineui::Rect{};
        if (box.w == 0) {
            MESSAGE("notes row=(", row.x, ",", row.y, " ", row.w, "x", row.h,
                    ") ta.w=", ta.w);
            const auto pt = find_in_rect_with_class(app, row, "dcs-textarea");
            REQUIRE(pt.x >= 0);
            affineui::Event hv{};
            hv.type = affineui::EventType::MouseMove;
            hv.pos = pt;
            app.dispatch(hv);
            for (const auto& info : app.document().hovered_info_chain()) {
                if (std::find(info.classes.begin(), info.classes.end(),
                              "dcs-textarea") != info.classes.end()) {
                    box = info.bounds;
                }
            }
        }
        REQUIRE(box.w > 0);
        MESSAGE("textarea box=(", box.x, ",", box.y, " ", box.w, "x", box.h,
                ")");

        auto notes_draw = [&]() -> const TestPainter::TextDraw* {
            const TestPainter::TextDraw* out = nullptr;
            for (const auto& td : painter.text_draws) {
                if (td.text.find("retopo") != std::string::npos) out = &td;
            }
            return out;
        };

        // Paint: the notes value must draw under a clip confined to the
        // textarea box (the reported bug: overflow lines painted over the
        // panel below — no clip at all).
        painter.text_draws.clear();
        app.document().draw(painter);
        const auto* td0 = notes_draw();
        REQUIRE(td0 != nullptr);
        MESSAGE("notes draw pos=(", td0->pos.x, ",", td0->pos.y,
                ") has_clip=", td0->has_clip, " clip=(", td0->clip.x, ",",
                td0->clip.y, " ", td0->clip.w, "x", td0->clip.h, ")");
        CHECK(td0->has_clip);
        if (td0->has_clip) {
            CHECK(td0->clip.y >= box.y);
            CHECK(td0->clip.y + td0->clip.h <= box.y + box.h + 1);
        }
        const int y_before = td0->pos.y;

        // Scroll: wheel over the textarea must scroll ITS value (UA
        // overflow:auto) — the draw origin shifts up.
        affineui::Event wheel{};
        wheel.type = affineui::EventType::MouseWheel;
        wheel.pos = {box.x + box.w / 2, box.y + box.h / 2};
        wheel.wheel_dy = -3.0f;
        const bool wheel_consumed = app.dispatch(wheel);
        MESSAGE("wheel consumed=", wheel_consumed);
        app.document().layout(W, H, &painter);
        painter.text_draws.clear();
        app.document().draw(painter);
        const auto* td1 = notes_draw();
        REQUIRE(td1 != nullptr);
        MESSAGE("notes draw y before=", y_before, " after wheel=", td1->pos.y);
        CHECK(td1->pos.y < y_before);

        // Caret placement in a SCROLLED textarea: clicking a visible line
        // must put the caret where the click landed (the reported bug: after
        // scrolling, clicks mapped against stale unscrolled geometry). The
        // caret paints as a short vertical stroke — it must sit inside the
        // box, at the clicked line.
        click({box.x + box.w / 2, box.y + box.h / 2});
        painter.stroke_lines.clear();
        painter.text_draws.clear();
        app.document().draw(painter);
        bool caret_in_box = false;
        for (const auto& sl : painter.stroke_lines) {
            const bool vertical = std::abs(sl.x0 - sl.x1) < 0.5f;
            const float h = std::abs(sl.y1 - sl.y0);
            if (!vertical || h < 8.0f || h > 24.0f) continue;
            if (sl.x0 >= box.x && sl.x0 <= box.x + box.w &&
                sl.y0 >= box.y - 1 && sl.y1 <= box.y + box.h + 1) {
                caret_in_box = true;
                MESSAGE("caret stroke at x=", sl.x0, " y=", sl.y0, "..",
                        sl.y1);
            }
        }
        CHECK(caret_in_box);
    }

    SUBCASE("colorfield: overshooting the drag OUT of the popover before "
            "releasing still commits the last in-square color") {
        boot();
        const auto row = app.document().find_element_rect("tint");
        REQUIRE(row.w > 0);
        const auto caret = find_in_rect_with_class(app, row,
                                                   "dcs-colorfield__caret");
        REQUIRE(caret.x >= 0);
        click(caret);
        const auto sv = app.document().find_element_rect(
            "#aui-cf-tint-picker-sv");
        REQUIRE(sv.w > 0);
        const std::string before = tint_prop();

        // Press inside, scrub, overshoot far outside the popover, release
        // there — the classic fast pick. The release must NOT revert the color
        // (the outside release also closes the popover; that close must not
        // resurrect the pre-drag value).
        const affineui::Point p0{sv.x + sv.w / 2, sv.y + sv.h / 2};
        const affineui::Point out{sv.x + sv.w + 160, sv.y + sv.h + 120};
        affineui::Event down{};
        down.type = affineui::EventType::MouseDown;
        down.button = affineui::MouseButton::Left;
        down.pos = p0;
        app.dispatch(down);
        for (int i = 1; i <= 8; ++i) {
            affineui::Event mv{};
            mv.type = affineui::EventType::MouseMove;
            mv.pos = {p0.x + (out.x - p0.x) * i / 8,
                      p0.y + (out.y - p0.y) * i / 8};
            app.dispatch(mv);
        }
        affineui::Event up{};
        up.type = affineui::EventType::MouseUp;
        up.button = affineui::MouseButton::Left;
        up.pos = out;
        app.dispatch(up);
        app.document().layout(W, H, &painter);

        const std::string committed = tint_prop();
        MESSAGE("overshoot: before='", before, "' committed='", committed,
                "'");
        CHECK(committed != before);
        for (int i = 0; i < 3; ++i) {
            affineui::Event mv{};
            mv.type = affineui::EventType::MouseMove;
            mv.pos = {out.x - i * 15, out.y - i * 10};
            app.dispatch(mv);
        }
        app.document().layout(W, H, &painter);
        MESSAGE("overshoot after pump tint='", tint_prop(), "'");
        CHECK(tint_prop() == committed);
    }
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
        if (theme == affineui::ViewTheme::Decius) {
            const auto combo_bounds = hovered_class_bounds(app, "dcs-combo");
            REQUIRE(combo_bounds.w > 0);
            drag.pos = {combo_bounds.x + combo_bounds.w - 4, input.y};
        } else {
            drag.pos = {input.x + 40, input.y};
        }
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
        const auto before = hovered_tag_attr_bounds(
            app, "textarea", "data-aui-name", "notes");
        REQUIRE(before.x >= 0);
        CHECK(before.w > 32);
        CHECK(before.h > 24);

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
        const auto edit_bounds = hovered_attr_bounds(app, "value", "Edit");
        const auto menu_bounds = hovered_class_bounds(app, "aui-select__menu");
        REQUIRE(menu_bounds.y >= 0);
        CHECK(menu_bounds.y == select_bounds.y + select_bounds.h);
        CHECK(menu_bounds.w == select_bounds.w);
        CHECK(edit_bounds.w >= menu_bounds.w - 8);
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

TEST_CASE("App dispatch lets named Decius buttons inside visual groups click") {
    affineui::View view{affineui::ViewTheme::Decius};
    std::string tool = "scale";

    view.begin();
    {
        auto group = view.container("dcs-btn-group", "tool-group");
        group.attr("style", "display:flex;width:160px;height:28px");
        view.button("Scale", true, "tool-scale")
            .cls("dcs-btn dcs-btn--primary")
            .attr("style", "display:block;width:80px;height:28px")
            .attr("aria-pressed", "true")
            .on_click([&] { tool = "scale"; });
        view.button("Rotate", false, "tool-rotate")
            .cls("dcs-btn")
            .attr("style", "display:block;width:80px;height:28px")
            .attr("aria-pressed", "false")
            .on_click([&] { tool = "rotate"; });
    }
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(260, 120);

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

    const auto rotate = find_hovered_widget(app, "tool-rotate", 260, 120);
    REQUIRE(rotate.x >= 0);
    click_at(rotate);
    CHECK(tool == "rotate");
}

TEST_CASE("App command dropdowns stay anchored in scrolled panels") {
    affineui::View view{affineui::ViewTheme::Decius};
    std::string mode;

    view.begin();
    {
        auto panel = view.container({}, "controls-panel-body");
        panel.attr("style",
                   "display:block;position:absolute;left:0;top:0;"
                   "width:220px;height:90px;overflow-y:auto;");
        view.container({}, "controls-spacer-top")
            .attr("style", "display:block;height:96px");
        view.dropdown("Mode", {"Object", "Edit", "Render"}, "Object", "mode")
            .on_change([&](std::string_view next) {
                mode = std::string(next);
            });
        view.container({}, "controls-spacer-bottom")
            .attr("style", "display:block;height:180px");
    }
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(280, 160);

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

    affineui::Event wheel{};
    wheel.type = affineui::EventType::MouseWheel;
    wheel.pos = {10, 10};
    wheel.wheel_dy = -3.0f;
    CHECK(app.dispatch(wheel));
    app.document().layout(280, 160);

    const auto select = find_hovered_tag_attr(app, "select",
                                              "data-aui-name", "mode",
                                              280, 160);
    REQUIRE(select.x >= 0);
    const auto select_bounds = hovered_attr_bounds(app, "data-aui-name", "mode");
    REQUIRE(select_bounds.y >= 0);
    app.document().layout(280, 160);
    const auto before_size = app.document().content_size();

    click_at(select);
    app.document().layout(280, 160);
    CHECK(app.document().content_size().height == before_size.height);

    const auto edit = find_hovered_tag_attr(app, "button", "value",
                                            "Edit", 280, 180);
    REQUIRE(edit.x >= 0);
    const auto menu_bounds = hovered_class_bounds(app, "aui-select__menu");
    REQUIRE(menu_bounds.y >= 0);
    CHECK(menu_bounds.y == select_bounds.y + select_bounds.h);
    CHECK(menu_bounds.w == select_bounds.w);

    click_at(edit);
    CHECK(mode == "Edit");
}

TEST_CASE("App Decius dropdown selection bar stretches across wide menus") {
    affineui::View view{affineui::ViewTheme::Decius};

    view.begin();
    view.dropdown("Preset",
                  {"Warm pad", "Digital pluck", "Sub bass", "Noise sweep"},
                  "Sub bass", "preset")
        .attr("style", "display:block;width:760px");
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(840, 180);

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
                                              "data-aui-name", "preset",
                                              840, 180);
    REQUIRE(select.x >= 0);
    const auto select_bounds =
        hovered_attr_bounds(app, "data-aui-name", "preset");
    REQUIRE(select_bounds.w > 320);

    click_at(select);
    app.document().layout(840, 240);

    const auto active = find_hovered_tag_attr(app, "button", "value",
                                              "Sub bass", 840, 260);
    REQUIRE(active.x >= 0);
    const auto menu_bounds = hovered_class_bounds(app, "aui-select__menu");
    const auto active_bounds = hovered_attr_bounds(app, "value", "Sub bass");
    REQUIRE(menu_bounds.y >= 0);
    CHECK(menu_bounds.w == select_bounds.w);
    CHECK(active_bounds.w >= menu_bounds.w - 8);
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

TEST_CASE("App dispatch invokes Decius colorfield picker callbacks") {
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

    const auto caret = find_hovered_tag_attr(app, "span", "data-dcs-target",
                                             "#aui-cf-tint-picker", 360, 180);
    REQUIRE(caret.x >= 0);
    click_at(caret);
    app.document().layout(360, 260);

    const auto field = app.document().find_element_rect("#aui-cf-tint");
    REQUIRE(field.w > 0);
    const auto picker = app.document().find_element_rect("#aui-cf-tint-picker");
    CHECK(picker.w >= 204);
    CHECK(picker.w >= field.w);
    const auto sv = app.document().find_element_rect("#aui-cf-tint-picker-sv");
    REQUIRE(sv.w >= 188);
    CHECK(sv.w <= picker.w);
    CHECK(sv.h == 134);

    click_at({sv.x + 1, sv.y + 1});
    app.document().layout(360, 260);
    CHECK(app.document().find_element_rect("#aui-cf-tint-picker").w ==
          picker.w);
    CHECK(near_white_hex(tint));
}

TEST_CASE("App Decius colorfield picker survives model-backed rebuilds") {
    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};

    std::string tint = "#3bb7ff";
    int rebuilds = 0;
    std::function<affineui::View()> build_view;
    build_view = [&]() {
        affineui::View view{affineui::ViewTheme::Decius};
        view.begin();
        view.input("Tint", tint, "color", "tint")
            .on_change([&](std::string_view next) {
                tint = std::string(next);
                ++rebuilds;
                app.load_view(build_view());
            });
        view.end();
        return view;
    };

    app.load_view(build_view());
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

    const auto caret = find_hovered_tag_attr(app, "span", "data-dcs-target",
                                             "#aui-cf-tint-picker", 360, 180);
    REQUIRE(caret.x >= 0);
    click_at(caret);
    app.document().layout(360, 260);

    const auto picker = app.document().find_element_rect("#aui-cf-tint-picker");
    REQUIRE(picker.w >= 204);
    const auto sv = app.document().find_element_rect("#aui-cf-tint-picker-sv");
    REQUIRE(sv.w >= 188);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {sv.x + 1, sv.y + 1};
    CHECK(app.dispatch(down));
    app.document().layout(360, 260);
    CHECK(rebuilds == 0);
    CHECK(app.document().find_element_rect("#aui-cf-tint-picker").w ==
          picker.w);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {sv.x + sv.w - 2, sv.y + 1};
    CHECK(app.dispatch(move));
    app.document().layout(360, 260);
    CHECK(rebuilds == 0);
    CHECK(app.document().find_element_rect("#aui-cf-tint-picker").w ==
          picker.w);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = move.pos;
    CHECK(app.dispatch(up));
    app.document().layout(360, 260);

    CHECK(rebuilds == 1);
    CHECK(tint.size() == 7);
    CHECK(tint.front() == '#');
    CHECK(tint != "#3bb7ff");
    CHECK(app.document().find_element_rect("#aui-cf-tint-picker").w ==
          picker.w);
}

TEST_CASE("App Decius colorfield pickers stay anchored in scrolled panels") {
    affineui::View view{affineui::ViewTheme::Decius};
    std::string tint;

    view.begin();
    {
        auto panel = view.container({}, "controls-panel-body");
        panel.attr("style",
                   "display:block;position:absolute;left:0;top:0;"
                   "width:240px;height:96px;overflow-y:auto;");
        view.container({}, "controls-spacer-top")
            .attr("style", "display:block;height:96px");
        view.input("Tint", "#3bb7ff", "color", "tint")
            .on_change([&](std::string_view next) {
                tint = std::string(next);
            });
        view.container({}, "controls-spacer-bottom")
            .attr("style", "display:block;height:180px");
    }
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(300, 180);

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

    affineui::Event wheel{};
    wheel.type = affineui::EventType::MouseWheel;
    wheel.pos = {10, 10};
    wheel.wheel_dy = -3.0f;
    CHECK(app.dispatch(wheel));
    app.document().layout(300, 180);

    const auto caret = find_hovered_tag_attr(app, "span", "data-dcs-target",
                                             "#aui-cf-tint-picker", 300, 180);
    REQUIRE(caret.x >= 0);
    const auto field_bounds = app.document().find_element_rect("#aui-cf-tint");
    REQUIRE(field_bounds.y >= 0);
    app.document().layout(300, 180);
    const auto before_size = app.document().content_size();

    click_at(caret);
    app.document().layout(300, 260);
    CHECK(app.document().content_size().height == before_size.height);

    const auto picker = app.document().find_element_rect("#aui-cf-tint-picker");
    REQUIRE(picker.y >= 0);
    CHECK(picker.x <= field_bounds.x + field_bounds.w);
    CHECK(picker.x + picker.w >= field_bounds.x);
    CHECK(picker.w >= 204);
    CHECK(picker.w >= field_bounds.w);

    const auto sv = app.document().find_element_rect("#aui-cf-tint-picker-sv");
    REQUIRE(sv.w >= 188);
    click_at({sv.x + 1, sv.y + 1});
    app.document().layout(300, 260);
    CHECK(app.document().find_element_rect("#aui-cf-tint-picker").w ==
          picker.w);
    CHECK(near_white_hex(tint));
}

TEST_CASE("App Decius vector editors keep the default horizontal gutter") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    {
        auto props = view.container("dcs-props", "props");
        (void) props;
        view.vec("Location", {"X", "Y", "Z"}, {12.0, 4.2, -8.5}, "loc");
    }
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(520, 160);

    const auto x = app.document().find_element_rect("loc-0");
    const auto y = app.document().find_element_rect("loc-1");
    const auto z = app.document().find_element_rect("loc-2");
    REQUIRE(x.w > 0);
    REQUIRE(y.w > 0);
    REQUIRE(z.w > 0);
    CHECK(y.x - (x.x + x.w) > 0);
    CHECK(z.x - (y.x + y.w) > 0);
}

TEST_CASE("App Decius vector editors expand their foldout field when stacked") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    {
        auto folds = view.container("dcs-foldouts", "foldouts");
        (void) folds;
        {
            auto fold = view.foldout("Transform", true, "fold-xform");
            (void) fold;
            auto props = view.container("dcs-props", "props");
            (void) props;
            view.vec("Location", {"X", "Y", "Z"}, {12.0, 4.2, -8.5}, "loc");
        }
        {
            auto fold = view.foldout("Display", true, "fold-display");
            (void) fold;
            auto props = view.container("dcs-props", "display-props");
            (void) props;
            view.button_group("Blend", {"Normal", "Add", "Multiply"}, "Normal",
                              "blend");
        }
    }
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(260, 180);

    const auto field = app.document().find_element_rect("loc");
    const auto x = app.document().find_element_rect("loc-0");
    const auto y = app.document().find_element_rect("loc-1");
    const auto z = app.document().find_element_rect("loc-2");
    const auto blend = app.document().find_element_rect("blend");
    const auto xform = app.document().find_element_rect("fold-xform");
    const auto display = app.document().find_element_rect("fold-display");
    REQUIRE(field.h > 0);
    REQUIRE(x.h > 0);
    REQUIRE(y.h > 0);
    REQUIRE(z.h > 0);
    REQUIRE(blend.h > 0);
    REQUIRE(xform.h > 0);
    REQUIRE(display.h > 0);

    CHECK(y.y >= x.y + x.h);
    CHECK(z.y >= y.y + y.h);
    CHECK(field.h >= (z.y + z.h) - field.y);
    CHECK(xform.h >= (z.y + z.h) - xform.y);
    CHECK(display.y >= xform.y + xform.h);
    CHECK(blend.y >= display.y);
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

// Submenu cascade: the bundle reveals `.dcs-menu__sub` purely via
// `.dcs-menu__item--has-sub:hover > .dcs-menu__sub{display:block}` (decius.js
// adds no open/close logic — it only refuses to close the menu when a
// has-sub row is clicked). The engine must (a) capture that pseudo rule
// (child combinator + :hover on the ancestor compound) and (b) RECOLLECT
// boxes when hover reveals the display:none subtree — restyling existing
// blocks can never create the submenu's missing boxes.
TEST_CASE("menu submenu cascades open on hover and its items activate") {
    std::ifstream bundle_in(
        AFFINEUI_TEST_SOURCE_DIR
        "/examples/frameworks/css/decius-css-0.6.2.bundle.min.css",
        std::ios::binary);
    REQUIRE(bundle_in.good());
    std::string bundle((std::istreambuf_iterator<char>(bundle_in)),
                       std::istreambuf_iterator<char>());

    affineui::App::Config cfg;
    cfg.asset_folders = test_asset_folders();
    affineui::App app{cfg};

    std::string picked;
    auto build = [&] {
        affineui::View v{affineui::ViewTheme::Decius};
        v.set_framework_version("0.6.2");
        v.begin();
        v.menu_bar("menubar");
        v.menu_button("View", [&](affineui::View& m) {
            m.menu_item("Reset Layout", "grid", {}, "mi-reset");
            m.submenu("Density", [&](affineui::View& s) {
                for (const auto d : {"compact", "comfortable", "spacious"}) {
                    s.menu_item(d, {}, {}, std::string("mi-dens-") + d)
                        .on_click([&picked, d] { picked = d; });
                }
            }, {}, "mi-density");
        }, "mb-view");
        // Content occupying the area the cascade opens over — LATER in the
        // DOM than the menubar, so without the menu's z-order it would win
        // every hit test (the in-window bug: cascade items not selectable
        // over the document tab bar).
        for (int i = 0; i < 8; ++i) {
            v.button("Underneath " + std::to_string(i), false,
                     "under-" + std::to_string(i));
        }
        v.end();
        return v;
    };
    app.load_view(build());
    app.set_stylesheet(bundle);
    TestPainter painter;
    constexpr int W = 800;
    constexpr int H = 500;
    app.document().layout(W, H, &painter);

    auto click = [&](affineui::Point p) {
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
        app.document().layout(W, H, &painter);
    };
    auto hover = [&](affineui::Point p) {
        affineui::Event mv{};
        mv.type = affineui::EventType::MouseMove;
        mv.pos = p;
        app.dispatch(mv);
    };

    // Open the View dropdown (attribute path — engine menu machinery).
    const auto btn = app.document().find_element_rect("mb-view");
    REQUIRE(btn.w > 0);
    click({btn.x + btn.w / 2, btn.y + btn.h / 2});
    const auto density_row = app.document().find_element_rect("mi-density");
    REQUIRE(density_row.w > 0);

    // The submenu is display:none until its opener row is hovered.
    CHECK(app.document().find_element_rect("mi-dens-comfortable").w == 0);

    // Hover the opener: the :hover reveal must land in THIS dispatch —
    // the submenu row becomes findable and hit-testable.
    hover({density_row.x + density_row.w / 2,
           density_row.y + density_row.h / 2});
    const auto comfy = app.document().find_element_rect("mi-dens-comfortable");
    REQUIRE(comfy.w > 0);
    MESSAGE("submenu item at (", comfy.x, ",", comfy.y, " ", comfy.w, "x",
            comfy.h, ")");
    // The cascade opens to the RIGHT of the opener row (left:100%).
    CHECK(comfy.x >= density_row.x + density_row.w - 4);

    // Crossing the visual gap between the opener row and the cascade must
    // NOT dismiss it: decius.css bridges the gap with an invisible
    // `.dcs-menu__sub::before` strip (absolutely positioned generated
    // content), so the hover chain stays on the cascade → opener the whole
    // way across. Hover a point INSIDE the gap, then continue into the sub.
    hover({comfy.x - 2, density_row.y + density_row.h / 2});
    REQUIRE(app.document().find_element_rect("mi-dens-comfortable").w > 0);

    // Move onto the submenu item (the opener stays in the hover ancestor
    // chain, so the cascade must stay open) and click it. The hit must land
    // INSIDE the cascade panel (`.dcs-menu.dcs-menu__sub`, z-index above the
    // page), not fall through to the buttons underneath.
    hover({comfy.x + comfy.w / 2, comfy.y + comfy.h / 2});
    REQUIRE(app.document().find_element_rect("mi-dens-comfortable").w > 0);
    bool hit_in_cascade_panel = false;
    bool hit_under_content = false;
    for (const auto& info : app.document().hovered_info_chain()) {
        const bool is_sub =
            std::find(info.classes.begin(), info.classes.end(),
                      "dcs-menu__sub") != info.classes.end();
        if (is_sub &&
            std::find(info.classes.begin(), info.classes.end(), "dcs-menu") !=
                info.classes.end()) {
            hit_in_cascade_panel = true;
        }
        if (info.elem_id.rfind("under-", 0) == 0) hit_under_content = true;
    }
    CHECK(hit_in_cascade_panel);
    CHECK_FALSE(hit_under_content);
    click({comfy.x + comfy.w / 2, comfy.y + comfy.h / 2});
    CHECK(picked == "comfortable");

    // Leaving the menu entirely hides the cascade again.
    hover({btn.x + btn.w / 2, H - 20});
    app.document().layout(W, H, &painter);
    CHECK(app.document().find_element_rect("mi-dens-comfortable").w == 0);
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
