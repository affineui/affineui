#include <doctest/doctest.h>

#include "affineui/document.h"
#include "affineui/painter.h"
#include "affineui/ui.h"
#include "decius_interactions.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

class RecordingPainter final : public affineui::Painter {
public:
    struct TextDraw {
        std::string text;
        affineui::Point pos;
        affineui::Color color;
        float max_width{0.0f};
        TextAlign align{TextAlign::Left};
        float alpha{1.0f};
    };
    struct FillDraw {
        affineui::Rect rect;
        affineui::Color color;
        affineui::Mat2x3 transform;
    };
    struct RoundedFillDraw {
        affineui::Rect rect;
        float tl{0.0f};
        float tr{0.0f};
        float br{0.0f};
        float bl{0.0f};
        affineui::Color color;
    };
    struct ArcDraw {
        float cx{0.0f};
        float cy{0.0f};
        float radius{0.0f};
        float start_deg{0.0f};
        float end_deg{0.0f};
        affineui::Color color;
        float thickness{0.0f};
    };
    struct LinearGradientDraw {
        affineui::Rect rect;
        affineui::Color stop0;
        affineui::Color stop1;
        float tl{0.0f};
        float tr{0.0f};
        float br{0.0f};
        float bl{0.0f};
    };
    struct StrokeLineDraw {
        float x0{0.0f};
        float y0{0.0f};
        float x1{0.0f};
        float y1{0.0f};
        affineui::Color color;
        float thickness{0.0f};
    };
    struct BorderRingDraw {
        affineui::Rect rect;
        float radius{0.0f};
        float thickness{0.0f};
        affineui::Color color;
    };
    struct ShadowDraw {
        affineui::Rect rect;
        float radius{0.0f};
        affineui::Color color;
        float offset_x{0.0f};
        float offset_y{0.0f};
        float blur{0.0f};
        float spread{0.0f};
        bool inset{false};
    };

    std::vector<affineui::Color> fill_colors;
    std::vector<FillDraw> fill_draws;
    std::vector<RoundedFillDraw> rounded_fill_draws;
    std::vector<ArcDraw> arc_draws;
    std::vector<LinearGradientDraw> linear_gradient_draws;
    std::vector<StrokeLineDraw> stroke_line_draws;
    std::vector<BorderRingDraw> border_ring_draws;
    std::vector<affineui::Color> stroke_colors;
    std::vector<std::string> font_requests;
    std::vector<std::string> text_runs;
    std::vector<TextDraw> text_draws;
    std::vector<std::string> image_urls;
    std::vector<affineui::Rect> image_draws;
    std::vector<affineui::Rect> clip_rects;
    std::vector<bool> shadow_insets;
    std::vector<ShadowDraw> shadow_draws;
    std::vector<float> alpha_stack;
    std::vector<affineui::Mat2x3> transform_stack;
    affineui::Mat2x3 current_transform{};
    float current_alpha{1.0f};

    void begin_frame(int, int, float) override {}
    void end_frame() override {}
    void fill_rect(const affineui::Rect& rect, affineui::Color color) override {
        fill_colors.push_back(color);
        fill_draws.push_back({rect, color, current_transform});
    }
    void stroke_rect(const affineui::Rect&, affineui::Color color, float) override {
        stroke_colors.push_back(color);
    }
    void stroke_line(float x0, float y0, float x1, float y1,
                     affineui::Color color, float thickness) override {
        stroke_colors.push_back(color);
        stroke_line_draws.push_back({x0, y0, x1, y1, color, thickness});
    }
    void fill_circle(float, float, float, affineui::Color color) override {
        fill_colors.push_back(color);
    }
    void stroke_arc(float cx, float cy, float radius, float start_deg,
                    float end_deg, affineui::Color color, float thickness) override {
        stroke_colors.push_back(color);
        arc_draws.push_back({cx, cy, radius, start_deg, end_deg, color,
                             thickness});
    }
    void fill_rounded_rect(const affineui::Rect& rect, float radius,
                           affineui::Color color) override {
        fill_colors.push_back(color);
        rounded_fill_draws.push_back({rect, radius, radius, radius, radius, color});
    }
    void stroke_rounded_rect(const affineui::Rect&, float, affineui::Color color, float) override {
        stroke_colors.push_back(color);
    }
    void fill_rounded_rect_varying(const affineui::Rect& rect, float tl, float tr,
                                   float br, float bl,
                                   affineui::Color color) override {
        fill_colors.push_back(color);
        rounded_fill_draws.push_back({rect, tl, tr, br, bl, color});
    }
    void stroke_rounded_rect_varying(const affineui::Rect&, float, float, float, float,
                                     affineui::Color color, float) override {
        stroke_colors.push_back(color);
    }
    void fill_rounded_rect_ring(const affineui::Rect& rect, float radius,
                                float thickness, affineui::Color color) override {
        stroke_colors.push_back(color);
        border_ring_draws.push_back({rect, radius, thickness, color});
    }
    void fill_linear_gradient_rect(const affineui::Rect& rect, float,
                                   affineui::Color c0, affineui::Color c1,
                                   float tl, float tr, float br, float bl) override {
        fill_colors.push_back(c0);
        linear_gradient_draws.push_back({rect, c0, c1, tl, tr, br, bl});
    }
    void fill_radial_gradient_rect(const affineui::Rect&,
                                   affineui::Color c0, affineui::Color,
                                   float, float, float, float,
                                   float = 50, float = 50, float = 100) override {
        fill_colors.push_back(c0);
    }
    void fill_box_shadow(const affineui::Rect& rect, float radius,
                         affineui::Color c, float offset_x, float offset_y,
                         float blur, float spread, bool inset) override {
        fill_colors.push_back(c);
        shadow_insets.push_back(inset);
        shadow_draws.push_back(
            {rect, radius, c, offset_x, offset_y, blur, spread, inset});
    }
    std::uint32_t resolve_font(std::string_view family, int, int, bool) override {
        font_requests.emplace_back(family);
        return 1;
    }
    int measure_text(std::uint32_t, std::string_view text) override {
        return static_cast<int>(text.size()) * 8;
    }
    TextMetrics text_metrics(std::uint32_t) override { return {12.0f, 4.0f, 18.0f}; }
    void draw_text(std::uint32_t, const affineui::Point& pos,
                   std::string_view text, affineui::Color color) override {
        text_runs.emplace_back(text);
        text_draws.push_back({std::string(text), pos, color, 0.0f,
                              TextAlign::Left, current_alpha});
    }
    affineui::Size measure_text_box(std::uint32_t, std::string_view text,
                                    float max_width, float, float) override {
        const int natural = static_cast<int>(text.size()) * 8;
        return {natural < static_cast<int>(max_width)
                    ? natural
                    : static_cast<int>(max_width),
                18};
    }
    void draw_text_box(std::uint32_t, const affineui::Point& pos,
                       std::string_view text, affineui::Color color,
                       float max_width,
                       float, float, TextAlign align) override {
        text_runs.emplace_back(text);
        text_draws.push_back({std::string(text), pos, color, max_width, align,
                              current_alpha});
    }
    std::uint32_t load_image(std::string_view url) override {
        image_urls.emplace_back(url);
        return url.empty() ? 0u : 7u;
    }
    affineui::Size image_size(std::uint32_t image) override {
        return image == 0 ? affineui::Size{} : affineui::Size{64, 32};
    }
    void draw_image(std::uint32_t, const affineui::Rect& dst,
                    const affineui::Rect&) override {
        image_draws.push_back(dst);
    }
    void push_clip(const affineui::Rect& rect) override { clip_rects.push_back(rect); }
    void pop_clip() override {}
    void push_alpha(float alpha) override {
        alpha_stack.push_back(current_alpha);
        current_alpha *= alpha;
    }
    void pop_alpha() override {
        if (alpha_stack.empty()) return;
        current_alpha = alpha_stack.back();
        alpha_stack.pop_back();
    }
    void push_transform(const affineui::Mat2x3& transform) override {
        transform_stack.push_back(current_transform);
        current_transform = current_transform.then(transform);
    }
    void pop_transform() override {
        if (transform_stack.empty()) return;
        current_transform = transform_stack.back();
        transform_stack.pop_back();
    }
};

bool same_color(affineui::Color a, affineui::Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool saw_stroke(const RecordingPainter& painter, affineui::Color color) {
    for (const auto& stroke : painter.stroke_colors) {
        if (same_color(stroke, color)) return true;
    }
    return false;
}

bool saw_fill(const RecordingPainter& painter, affineui::Color color) {
    for (const auto& fill : painter.fill_colors) {
        if (same_color(fill, color)) return true;
    }
    return false;
}

const RecordingPainter::TextDraw* find_text_draw(
    const RecordingPainter& painter,
    std::string_view text) {
    for (const auto& draw : painter.text_draws) {
        if (draw.text == text) return &draw;
    }
    return nullptr;
}

const RecordingPainter::FillDraw* find_fill_draw(
    const RecordingPainter& painter,
    affineui::Color color) {
    for (const auto& draw : painter.fill_draws) {
        if (same_color(draw.color, color)) return &draw;
    }
    return nullptr;
}

const RecordingPainter::RoundedFillDraw* find_rounded_fill_draw(
    const RecordingPainter& painter,
    affineui::Color color,
    int width = -1,
    int height = -1) {
    for (const auto& draw : painter.rounded_fill_draws) {
        if (!same_color(draw.color, color)) continue;
        if (width >= 0 && draw.rect.w != width) continue;
        if (height >= 0 && draw.rect.h != height) continue;
        return &draw;
    }
    return nullptr;
}

const RecordingPainter::BorderRingDraw* find_border_ring_left_of_text(
    const RecordingPainter& painter,
    std::string_view text,
    int max_gap = 28) {
    const auto* label = find_text_draw(painter, text);
    if (!label) return nullptr;
    for (const auto& ring : painter.border_ring_draws) {
        const int ring_right = ring.rect.x + ring.rect.w;
        if (ring_right > label->pos.x) continue;
        if (label->pos.x - ring_right > max_gap) continue;
        const int center_y = ring.rect.y + ring.rect.h / 2;
        if (center_y < label->pos.y - 12 || center_y > label->pos.y + 22)
            continue;
        return &ring;
    }
    return nullptr;
}

affineui::Point find_hovered_tag(affineui::Document& doc, std::string_view tag) {
    for (int y = 0; y < 120; y += 2) {
        for (int x = 0; x < 320; x += 2) {
            affineui::Event move{};
            move.type = affineui::EventType::MouseMove;
            move.pos = {x, y};
            doc.dispatch(move);
            if (doc.hovered_info().tag == tag) {
                return {x, y};
            }
        }
    }
    return {-1, -1};
}

affineui::Point find_hovered_button(affineui::Document& doc) {
    return find_hovered_tag(doc, "button");
}

affineui::Point find_hovered_id(affineui::Document& doc,
                                std::string_view elem_id,
                                int width,
                                int height) {
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            affineui::Event move{};
            move.type = affineui::EventType::MouseMove;
            move.pos = {x, y};
            doc.dispatch(move);
            if (doc.hovered_info().elem_id == elem_id) {
                return {x, y};
            }
        }
    }
    return {-1, -1};
}

affineui::Point find_hovered_chain_id(affineui::Document& doc,
                                      std::string_view elem_id,
                                      int width,
                                      int height) {
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            affineui::Event move{};
            move.type = affineui::EventType::MouseMove;
            move.pos = {x, y};
            doc.dispatch(move);
            const auto chain = doc.hovered_info_chain();
            const bool found = std::any_of(
                chain.begin(), chain.end(),
                [&](const affineui::Document::HoverInfo& info) {
                    return info.elem_id == elem_id;
                });
            if (found) return {x, y};
        }
    }
    return {-1, -1};
}

affineui::Point find_hovered_attr(affineui::Document& doc,
                                  std::string_view name,
                                  std::string_view value,
                                  int width,
                                  int height) {
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            affineui::Event move{};
            move.type = affineui::EventType::MouseMove;
            move.pos = {x, y};
            doc.dispatch(move);
            const auto chain = doc.hovered_info_chain();
            for (const auto& info : chain) {
                for (const auto& attr : info.attrs) {
                    if (attr.first == name && attr.second == value) {
                        return {x, y};
                    }
                }
            }
        }
    }
    return {-1, -1};
}

std::string hovered_attr_for_id(affineui::Document& doc,
                                std::string_view elem_id,
                                std::string_view name) {
    const auto chain = doc.hovered_info_chain();
    for (const auto& info : chain) {
        if (info.elem_id != elem_id) continue;
        for (const auto& attr : info.attrs) {
            if (attr.first == name) return attr.second;
        }
    }
    return {};
}

std::string read_test_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("default-constructed document has zero content size") {
    affineui::Document doc;
    auto sz = doc.content_size();
    CHECK(sz.width == 0);
    CHECK(sz.height == 0);
}

TEST_CASE("set_html accepts a string without crashing") {
    affineui::Document doc;
    doc.set_html("<h1>Hi</h1>");
    CHECK(true);
}

TEST_CASE("UiControls script opt-in owns default widget behavior") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        label { display: block; width: 160px; height: 32px; }
        input { display: inline-block; width: 20px; height: 20px; }
        </style>
        <label id="wrap" data-aui-widget="checkbox">
            <input id="check" type="checkbox"><span>Enabled</span>
        </label>
    )HTML");
    doc.layout(220, 80, &painter);

    const auto p = find_hovered_id(doc, "check", 220, 80);
    REQUIRE(p.x >= 0);

    auto click = [&] {
        affineui::Event down{};
        down.type = affineui::EventType::MouseDown;
        down.button = affineui::MouseButton::Left;
        down.pos = p;
        doc.dispatch(down);

        affineui::Event up{};
        up.type = affineui::EventType::MouseUp;
        up.button = affineui::MouseButton::Left;
        up.pos = p;
        doc.dispatch(up);
    };
    auto hovered_has_checked = [&] {
        const auto chain = doc.hovered_info_chain();
        for (const auto& info : chain) {
            if (info.elem_id != "check") continue;
            return std::any_of(info.attrs.begin(), info.attrs.end(),
                [](const auto& attr) {
                    return attr.first == "checked";
                });
        }
        return false;
    };

    click();
    CHECK_FALSE(hovered_has_checked());

    doc.attach_script(affineui::DocumentScript::UiControls);
    click();
    CHECK(hovered_has_checked());

    doc.detach_script(affineui::DocumentScript::UiControls);
    click();
    CHECK(hovered_has_checked());
}

TEST_CASE("UiControls script updates range input and Decius knob markup") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #gain { display: block; width: 200px; height: 24px; margin: 8px; }
        #shape { display: block; width: 80px; height: 80px; margin: 8px; }
        </style>
        <input id="gain" type="range" min="0" max="1" value="0">
        <div id="shape" data-dcs-knob data-min="0" data-max="1"
             data-value="0.25" value="0.25">
            <svg><path class="dcs-knob__arc"></path></svg>
            <div class="dcs-knob__indicator" style="--angle:-67.5deg"></div>
            <div class="dcs-knob__value">0.25</div>
        </div>
    )HTML");
    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.layout(260, 160, &painter);

    auto gain = find_hovered_id(doc, "gain", 260, 160);
    REQUIRE(gain.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = gain;
    doc.dispatch(down);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {gain.x + 140, gain.y};
    doc.dispatch(move);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = move.pos;
    doc.dispatch(up);
    CHECK(hovered_attr_for_id(doc, "gain", "value") != "0");

    auto shape = find_hovered_chain_id(doc, "shape", 260, 160);
    REQUIRE(shape.x >= 0);

    down.pos = shape;
    doc.dispatch(down);
    move.pos = {shape.x, shape.y - 40};
    doc.dispatch(move);
    up.pos = move.pos;
    doc.dispatch(up);
    shape = find_hovered_chain_id(doc, "shape", 260, 160);
    REQUIRE(shape.x >= 0);
    CHECK(hovered_attr_for_id(doc, "shape", "value") != "0.25");
}

TEST_CASE("UiControls script emits named button activations") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        button { display: block; width: 120px; height: 36px; }
        </style>
        <button data-aui-name="run">Run</button>
    )HTML");
    doc.layout(180, 80, &painter);

    const auto p = find_hovered_attr(doc, "data-aui-name", "run", 180, 80);
    REQUIRE(p.x >= 0);

    auto click = [&] {
        affineui::Event down{};
        down.type = affineui::EventType::MouseDown;
        down.button = affineui::MouseButton::Left;
        down.pos = p;
        doc.dispatch(down);

        affineui::Event up{};
        up.type = affineui::EventType::MouseUp;
        up.button = affineui::MouseButton::Left;
        up.pos = p;
        doc.dispatch(up);
    };

    click();
    CHECK(doc.take_activated_widgets().empty());

    doc.attach_script(affineui::DocumentScript::UiControls);
    click();
    const auto activations = doc.take_activated_widgets();
    REQUIRE(activations.size() == 1);
    CHECK(activations[0] == "run");
}

TEST_CASE("UiControls script emits named widget changes") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .check { display: block; width: 160px; height: 32px; }
        input { width: 20px; height: 20px; }
        </style>
        <label class="check" data-aui-widget="checkbox" data-aui-name="enabled">
            <input type="checkbox"> Enabled
        </label>
    )HTML");
    doc.layout(220, 80, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    const auto p = find_hovered_attr(doc, "data-aui-name", "enabled", 220, 80);
    REQUIRE(p.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = p;
    doc.dispatch(down);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = p;
    doc.dispatch(up);

    const auto changes = doc.take_widget_changes();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].name == "enabled");
    CHECK(changes[0].value == "true");
}

TEST_CASE("Ui on_click matches hovered ancestors") {
    affineui::Ui ui;
    RecordingPainter painter;
    bool clicked = false;

    ui.html(R"HTML(
        <style>
            body { margin: 0; padding: 0; }
            button { display: block; width: 120px; height: 40px; padding: 0; }
            span { display: block; width: 80px; height: 24px; }
        </style>
        <button id="outer"><span>Inner</span></button>
    )HTML");
    ui.document().layout(200, 80, &painter);
    ui.on_click("#outer", [&] { clicked = true; });

    const auto p = find_hovered_tag(ui.document(), "span");
    REQUIRE(p.x >= 0);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = p;

    CHECK(ui.dispatch(up));
    CHECK(clicked);
}

TEST_CASE("Ui on_click routes through transformed hit testing") {
    affineui::Ui ui;
    RecordingPainter painter;
    int selected = -1;

    ui.html(R"HTML(
        <style>
            html, body { margin: 0; }
            .stage { position: relative; width: 200px; height: 120px; }
            .clip {
                position: absolute;
                left: 0;
                top: 0;
                width: 100px;
                height: 30px;
                background: #2f86ee;
            }
            .lane {
                position: absolute;
                left: 0;
                top: 0;
                width: 100px;
                height: 30px;
                transform: translateY(50px);
            }
        </style>
        <div class="stage">
            <div id="top" class="clip"></div>
            <div class="lane"><div id="bottom" class="clip"></div></div>
        </div>
    )HTML");
    ui.document().layout(200, 120, &painter);
    ui.on_click("#top", [&] { selected = 0; });
    ui.on_click("#bottom", [&] { selected = 1; });

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = {10, 60};

    CHECK(ui.dispatch(up));
    CHECK(selected == 1);
}

TEST_CASE("captured pointer moves bypass DOM hover restyle") {
    affineui::Ui ui;
    RecordingPainter painter;
    int captured_moves = 0;

    ui.html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .box { display: inline-block; width: 50px; height: 40px; }
        #left { background: #222; }
        #right { background: #444; }
        #right:hover { background: #3dd68a; }
        </style>
        <div id="left" class="box"></div><div id="right" class="box"></div>
    )HTML");
    ui.document().layout(140, 60, &painter);
    ui.on_event([&](const affineui::Event& e,
                    const std::vector<affineui::Document::HoverInfo>& chain) {
        if (e.type == affineui::EventType::MouseDown) {
            const bool on_left = std::any_of(
                chain.begin(), chain.end(),
                [](const affineui::Document::HoverInfo& info) {
                    return info.elem_id == "left";
                });
            if (on_left) {
                ui.capture_pointer();
                return true;
            }
        }
        if (e.type == affineui::EventType::MouseMove &&
            ui.pointer_captured()) {
            ++captured_moves;
            return true;
        }
        if (e.type == affineui::EventType::MouseUp &&
            ui.pointer_captured()) {
            ui.release_pointer();
            return true;
        }
        return false;
    });

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {10, 10};
    CHECK_FALSE(ui.dispatch(move));
    CHECK(ui.document().hovered_info().elem_id == "left");

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {10, 10};
    CHECK(ui.dispatch(down));
    CHECK(ui.pointer_captured());
    (void)ui.document().take_dirty_rects();

    move.pos = {70, 10};
    CHECK(ui.dispatch(move));
    CHECK(captured_moves == 1);
    CHECK(ui.document().hovered_info().elem_id == "left");
    CHECK(ui.document().take_dirty_rects().empty());

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = {70, 10};
    CHECK(ui.dispatch(up));
    CHECK_FALSE(ui.pointer_captured());
}

TEST_CASE("set_html clears stale hover identity") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #old { display: block; width: 80px; height: 32px; background: #222; }
        </style>
        <div id="old"></div>
    )HTML");
    doc.layout(120, 80, &painter);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {8, 8};
    doc.dispatch(move);
    CHECK(doc.hovered_info().valid);

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #new { display: block; width: 80px; height: 32px; background: #333; }
        </style>
        <div id="new"></div>
    )HTML");

    CHECK_FALSE(doc.hovered_info().valid);
}

TEST_CASE("Decius checkbox rerender preserves control box and generated check") {
    affineui::Ui ui;
    RecordingPainter painter;
    bool checked = false;

    const auto render = [&] {
        std::string checked_attr = checked ? " aria-checked=\"true\"" : "";
        return R"HTML(
            <style>
            body { margin: 0; padding: 0; font-size: 16px; line-height: 1; }
            .dcs-check {
              display: inline-flex;
              align-items: center;
              gap: 6px;
              color: #dce6ff;
              cursor: pointer;
            }
            .dcs-check__box {
              width: 14px;
              height: 14px;
              display: flex;
              align-items: center;
              justify-content: center;
              background: #20232b;
              color: transparent;
              border: 1px solid #4b5568;
            }
            .dcs-check[aria-checked=true] .dcs-check__box {
              background: #3dd68a;
              color: #0a1220;
              border-color: #3dd68a;
            }
            .di { font-family: decius-icons; display: inline-block; color: inherit; }
            .di-check::before { content: "\e01b"; }
            </style>
            <div id="sync" class="dcs-check")HTML" + checked_attr + R"HTML(>
              <div class="dcs-check__box"><i class="di di-check"></i></div>
              <span>Hard sync</span>
            </div>
        )HTML";
    };

    auto rerender = [&] {
        ui.html(render());
        ui.mark_dirty();
    };
    rerender();
    ui.on_click("#sync", [&] {
        checked = !checked;
        rerender();
    });

    ui.document().layout(180, 60, &painter);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {7, 7};
    ui.dispatch(move);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {7, 7};
    ui.dispatch(down);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = {7, 7};
    ui.dispatch(up);

    REQUIRE(checked);
    ui.document().layout(180, 60, &painter);

    painter.fill_draws.clear();
    painter.fill_colors.clear();
    painter.rounded_fill_draws.clear();
    painter.text_draws.clear();
    painter.text_runs.clear();
    ui.document().draw(painter);

    const auto checked_fill = affineui::Color::rgb(0x3d, 0xd6, 0x8a);
    CHECK((saw_fill(painter, checked_fill) ||
           find_rounded_fill_draw(painter, checked_fill) != nullptr));

    const auto* icon = find_text_draw(painter, "\xEE\x80\x9B");
    REQUIRE(icon != nullptr);
    CHECK(same_color(icon->color, affineui::Color::rgb(0x0a, 0x12, 0x20)));
}

TEST_CASE("real Decius checkbox remains visible after rerender and hover restyle") {
    affineui::Ui ui;
    RecordingPainter painter;
    bool checked = false;

    const auto examples_root =
        std::filesystem::path{AFFINEUI_TEST_SOURCE_DIR} / "examples";
    ui.document().set_resource_loader(
        [examples_root](std::string_view url) -> std::string {
            const std::filesystem::path rel{std::string(url)};
            return read_test_file(examples_root / rel);
        });

    const auto render = [&] {
        std::string checked_attr = checked ? " aria-checked=\"true\"" : "";
        return R"HTML(
            <!doctype html><html><head>
            <link rel="stylesheet" href="frameworks/css/decius-css-0.5.2.bundle.min.css">
            <style>body{margin:0;padding:0;background:#101219}</style>
            </head><body class="dcs" data-dcs-density="comfortable" data-dcs-accent="green">
              <div id="sync" class="dcs-check")HTML" + checked_attr + R"HTML(>
                <div class="dcs-check__box"><i class="di di-check"></i></div>
                <span>Hard sync</span>
              </div>
            </body></html>
        )HTML";
    };

    auto rerender = [&] {
        ui.html(render());
        ui.mark_dirty();
    };
    rerender();
    ui.on_click("#sync", [&] {
        checked = !checked;
        rerender();
    });

    ui.document().layout(220, 80, &painter);
    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {8, 8};
    ui.dispatch(move);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = move.pos;
    ui.dispatch(down);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = move.pos;
    ui.dispatch(up);
    REQUIRE(checked);

    ui.document().layout(220, 80, &painter);
    ui.dispatch(move);

    painter.fill_draws.clear();
    painter.fill_colors.clear();
    painter.rounded_fill_draws.clear();
    painter.text_draws.clear();
    painter.text_runs.clear();
    ui.document().draw(painter);

    const auto checked_fill = affineui::Color::rgb(0x3d, 0xd6, 0x8a);
    CHECK((saw_fill(painter, checked_fill) ||
           find_rounded_fill_draw(painter, checked_fill) != nullptr));
    const auto* icon = find_text_draw(painter, "\xEE\x80\x9B");
    REQUIRE(icon != nullptr);
    CHECK(same_color(icon->color, affineui::Color::rgb(0x0a, 0x12, 0x20)));
    CHECK(find_text_draw(painter, "Hard sync") != nullptr);
}

TEST_CASE("real Decius checkbox generated icon updates after live aria mutation") {
    affineui::Ui ui;
    RecordingPainter painter;

    const auto examples_root =
        std::filesystem::path{AFFINEUI_TEST_SOURCE_DIR} / "examples";
    ui.document().set_resource_loader(
        [examples_root](std::string_view url) -> std::string {
            const std::filesystem::path rel{std::string(url)};
            return read_test_file(examples_root / rel);
        });

    ui.html(R"HTML(
        <!doctype html><html><head>
        <link rel="stylesheet" href="frameworks/css/decius-css-0.5.2.bundle.min.css">
        <style>body{margin:0;padding:0;background:#101219}</style>
        </head><body class="dcs" data-dcs-density="comfortable" data-dcs-accent="green">
          <div id="sync" class="dcs-check">
            <div class="dcs-check__box"><i class="di di-check"></i></div>
            <span>Hard sync</span>
          </div>
        </body></html>
    )HTML");

    ui.document().layout(220, 80, &painter);
    REQUIRE(ui.set_attr("sync", "aria-checked", "true"));
    ui.document().layout(220, 80, &painter);

    painter.fill_draws.clear();
    painter.fill_colors.clear();
    painter.rounded_fill_draws.clear();
    painter.text_draws.clear();
    painter.text_runs.clear();
    ui.document().draw(painter);

    const auto checked_fill = affineui::Color::rgb(0x3d, 0xd6, 0x8a);
    CHECK((saw_fill(painter, checked_fill) ||
           find_rounded_fill_draw(painter, checked_fill) != nullptr));
    const auto* icon = find_text_draw(painter, "\xEE\x80\x9B");
    REQUIRE(icon != nullptr);
    CHECK(same_color(icon->color, affineui::Color::rgb(0x0a, 0x12, 0x20)));
    CHECK(find_text_draw(painter, "Hard sync") != nullptr);
}

TEST_CASE("live selector attribute mutation rematches cascade") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .row { display: flex; gap: 8px; }
        .check { display: inline-flex; align-items: center; gap: 6px; }
        .box {
          width: 14px;
          height: 14px;
          background: #20232b;
          border: 1px solid #5d6577;
        }
        .check[aria-checked=true] .box {
          background: #3dd68a;
          border-color: #3dd68a;
        }
        </style>
        <div class="row">
          <div id="sync" class="check"><div class="box"></div><span>Hard sync</span></div>
          <div>Other control</div>
        </div>
    )HTML");
    doc.layout(260, 80, &painter);
    (void)doc.take_paint_dirty();
    (void)doc.take_dirty_rects();

    REQUIRE(doc.set_attribute_by_id("sync", "aria-checked", "true"));
    CHECK_FALSE(doc.take_paint_dirty());
    CHECK_FALSE(doc.take_dirty_rects().empty());
    doc.layout(260, 80, &painter);

    painter.fill_draws.clear();
    painter.fill_colors.clear();
    painter.rounded_fill_draws.clear();
    doc.draw(painter);

    CHECK((saw_fill(painter, affineui::Color::rgb(0x3d, 0xd6, 0x8a)) ||
           find_rounded_fill_draw(
               painter, affineui::Color::rgb(0x3d, 0xd6, 0x8a)) != nullptr));

    (void)doc.take_paint_dirty();
    (void)doc.take_dirty_rects();
    REQUIRE(doc.remove_attribute_by_id("sync", "aria-checked"));
    CHECK_FALSE(doc.take_paint_dirty());
    CHECK_FALSE(doc.take_dirty_rects().empty());
    doc.layout(260, 80, &painter);

    painter.fill_draws.clear();
    painter.fill_colors.clear();
    painter.rounded_fill_draws.clear();
    doc.draw(painter);

    CHECK_FALSE(saw_fill(painter, affineui::Color::rgb(0x3d, 0xd6, 0x8a)));
}

TEST_CASE("no-op live attribute mutations do not dirty document") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>html, body { margin: 0; padding: 0; }</style>
        <div id="sync" aria-checked="true">Hard sync</div>
    )HTML");
    doc.layout(180, 60, &painter);
    (void)doc.take_paint_dirty();
    (void)doc.take_dirty_rects();

    CHECK_FALSE(doc.set_attribute_by_id("sync", "aria-checked", "true"));
    CHECK_FALSE(doc.take_paint_dirty());
    CHECK(doc.take_dirty_rects().empty());

    CHECK_FALSE(doc.remove_attribute_by_id("sync", "data-missing"));
    CHECK_FALSE(doc.take_paint_dirty());
    CHECK(doc.take_dirty_rects().empty());

    REQUIRE(doc.remove_attribute_by_id("sync", "aria-checked"));
    (void)doc.take_dirty_rects();
    CHECK_FALSE(doc.remove_attribute_by_id("sync", "aria-checked"));
    CHECK_FALSE(doc.take_paint_dirty());
    CHECK(doc.take_dirty_rects().empty());
}

TEST_CASE("live mutation dirty rects include transformed visual bounds") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        #track { position: relative; width: 100px; height: 100px; }
        #thumb {
          position: absolute;
          left: 50%;
          top: 50%;
          width: 20px;
          height: 10px;
          transform: translate(-50%, -50%);
          background: #fff;
        }
        </style>
        <div id="track"><div id="thumb"></div></div>
    )HTML");
    doc.layout(140, 120, &painter);
    (void)doc.take_paint_dirty();
    (void)doc.take_dirty_rects();

    REQUIRE(doc.set_attribute_by_id(
        "thumb", "style",
        "position:absolute;left:50%;top:70%;width:20px;height:10px;"
        "transform:translate(-50%,-50%);background:#fff"));

    auto dirty = doc.take_dirty_rects();
    REQUIRE_FALSE(dirty.empty());
    affineui::Rect bounds{};
    for (const auto& r : dirty) {
        if (bounds.w <= 0 || bounds.h <= 0) {
            bounds = r;
        } else {
            const int x0 = std::min(bounds.x, r.x);
            const int y0 = std::min(bounds.y, r.y);
            const int x1 = std::max(bounds.x + bounds.w, r.x + r.w);
            const int y1 = std::max(bounds.y + bounds.h, r.y + r.h);
            bounds = {x0, y0, x1 - x0, y1 - y0};
        }
    }

    CHECK(bounds.x <= 40);
    CHECK(bounds.y <= 45);
    CHECK(bounds.x + bounds.w >= 60);
    CHECK(bounds.y + bounds.h >= 76);
}

TEST_CASE("selector attribute mutations avoid dirtying unrelated siblings") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .panel { display: flex; gap: 10px; width: 520px; height: 32px; }
        .check { display: inline-flex; align-items: center; gap: 6px; }
        .box { width: 14px; height: 14px; background: #20232b; }
        .check[aria-checked=true] .box { background: #3dd68a; }
        .sibling { width: 430px; height: 32px; background: #444; }
        </style>
        <div class="panel">
          <div id="sync" class="check"><div class="box"></div><span>Sync</span></div>
          <div class="sibling"></div>
        </div>
    )HTML");
    doc.layout(560, 80, &painter);
    (void)doc.take_paint_dirty();
    (void)doc.take_dirty_rects();

    REQUIRE(doc.set_attribute_by_id("sync", "aria-checked", "true"));
    auto dirty = doc.take_dirty_rects();
    REQUIRE_FALSE(dirty.empty());
    affineui::Rect bounds{};
    for (const auto& r : dirty) {
        if (bounds.w <= 0 || bounds.h <= 0) {
            bounds = r;
        } else {
            const int x0 = std::min(bounds.x, r.x);
            const int y0 = std::min(bounds.y, r.y);
            const int x1 = std::max(bounds.x + bounds.w, r.x + r.w);
            const int y1 = std::max(bounds.y + bounds.h, r.y + r.h);
            bounds = {x0, y0, x1 - x0, y1 - y0};
        }
    }

    CHECK(bounds.w < 120);
}

TEST_CASE("sibling selector attribute mutations keep parent-scope invalidation") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .panel { display: flex; gap: 10px; width: 520px; height: 32px; }
        .check { width: 50px; height: 32px; }
        .sibling { width: 430px; height: 32px; background: #444; }
        .check[aria-checked=true] + .sibling { background: #3dd68a; }
        </style>
        <div class="panel">
          <div id="sync" class="check"></div>
          <div class="sibling"></div>
        </div>
    )HTML");
    doc.layout(560, 80, &painter);
    (void)doc.take_paint_dirty();
    (void)doc.take_dirty_rects();

    REQUIRE(doc.set_attribute_by_id("sync", "aria-checked", "true"));
    auto dirty = doc.take_dirty_rects();
    REQUIRE_FALSE(dirty.empty());
    affineui::Rect bounds{};
    for (const auto& r : dirty) {
        if (bounds.w <= 0 || bounds.h <= 0) {
            bounds = r;
        } else {
            const int x0 = std::min(bounds.x, r.x);
            const int y0 = std::min(bounds.y, r.y);
            const int x1 = std::max(bounds.x + bounds.w, r.x + r.w);
            const int y1 = std::max(bounds.y + bounds.h, r.y + r.h);
            bounds = {x0, y0, x1 - x0, y1 - y0};
        }
    }
    CHECK(bounds.w >= 490);

    doc.layout(560, 80, &painter);
    painter.fill_colors.clear();
    doc.draw(painter);
    CHECK(saw_fill(painter, affineui::Color::rgb(0x3d, 0xd6, 0x8a)));
}

TEST_CASE("real Decius checkbox survives unrelated live control mutation") {
    affineui::Ui ui;
    RecordingPainter painter;
    bool checked = false;

    const auto examples_root =
        std::filesystem::path{AFFINEUI_TEST_SOURCE_DIR} / "examples";
    ui.document().set_resource_loader(
        [examples_root](std::string_view url) -> std::string {
            const std::filesystem::path rel{std::string(url)};
            return read_test_file(examples_root / rel);
        });

    const auto render = [&] {
        std::string checked_attr = checked ? " aria-checked=\"true\"" : "";
        return R"HTML(
            <!doctype html><html><head>
            <link rel="stylesheet" href="frameworks/css/decius-css-0.5.2.bundle.min.css">
            <style>body{margin:0;padding:0;background:#101219}.row{display:flex;gap:18px}</style>
            </head><body class="dcs" data-dcs-density="comfortable" data-dcs-accent="green">
              <div class="row">
                <div id="sync" class="dcs-check")HTML" + checked_attr + R"HTML(>
                  <div class="dcs-check__box"><i class="di di-check"></i></div>
                  <span>Hard sync</span>
                </div>
                <div id="gain" data-dcs-slider data-min="0" data-max="1" data-value=".4" class="dcs-slider">
                  <div class="dcs-slider__track">
                    <div id="gain__fill" class="dcs-slider__fill" style="width:40%"></div>
                    <div id="gain__thumb" class="dcs-slider__thumb" style="left:40%"></div>
                  </div>
                </div>
                <div id="shape" data-dcs-knob data-min="0" data-max="1" data-value=".4" class="dcs-knob">
                  <svg class="dcs-knob__ring" viewBox="0 0 24 24">
                    <path id="shape__arc" class="dcs-knob__arc" d="M 4.58 4.58 A 10.5 10.5 0 0 1 12 22.5" fill="none" stroke="var(--dcs-accent)" stroke-width="1.75" stroke-linecap="round"></path>
                  </svg>
                  <div class="dcs-knob__cap"></div>
                  <div id="shape__indicator" class="dcs-knob__indicator" style="--angle:-27deg"></div>
                  <div class="dcs-knob__label">Shape</div>
                  <div id="shape__value" class="dcs-knob__value">0.40</div>
                </div>
              </div>
            </body></html>
        )HTML";
    };

    auto rerender = [&] {
        ui.html(render());
        ui.mark_dirty();
    };
    rerender();
    ui.on_click("#sync", [&] {
        checked = !checked;
        rerender();
    });

    ui.document().layout(420, 100, &painter);
    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {8, 8};
    ui.dispatch(move);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = move.pos;
    ui.dispatch(down);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = move.pos;
    ui.dispatch(up);
    REQUIRE(checked);

    ui.document().layout(420, 100, &painter);
    REQUIRE(ui.set_attr("gain__fill", "style", "width:68%"));
    REQUIRE(ui.set_attr("gain__thumb", "style", "left:68%"));
    REQUIRE(ui.set_attr("shape__indicator", "style", "--angle:48deg"));
    REQUIRE(ui.set_text("shape__value", "0.68"));
    ui.document().layout(420, 100, &painter);

    painter.fill_draws.clear();
    painter.fill_colors.clear();
    painter.rounded_fill_draws.clear();
    painter.text_draws.clear();
    painter.text_runs.clear();
    ui.document().draw(painter);

    const auto checked_fill = affineui::Color::rgb(0x3d, 0xd6, 0x8a);
    CHECK((saw_fill(painter, checked_fill) ||
           find_rounded_fill_draw(painter, checked_fill) != nullptr));
    const auto* icon = find_text_draw(painter, "\xEE\x80\x9B");
    REQUIRE(icon != nullptr);
    CHECK(same_color(icon->color, affineui::Color::rgb(0x0a, 0x12, 0x20)));
    CHECK(find_text_draw(painter, "Hard sync") != nullptr);
}

TEST_CASE("dark synth checkbox survives real page live control interactions") {
    affineui::Ui ui;
    RecordingPainter painter;

    const auto examples_root =
        std::filesystem::path{AFFINEUI_TEST_SOURCE_DIR} / "examples";
    ui.document().set_resource_loader(
        [examples_root](std::string_view url) -> std::string {
            const std::filesystem::path rel{std::string(url)};
            return read_test_file(examples_root / "13_decius_synth_dark" / rel);
        });

    std::string html = read_test_file(examples_root /
                                      "13_decius_synth_dark" / "index.html");
    REQUIRE_FALSE(html.empty());
    const std::string unchecked = R"HTML(<div id="sync" class="dcs-check">)HTML";
    const auto pos = html.find(unchecked);
    REQUIRE(pos != std::string::npos);
    html.replace(pos, unchecked.size(),
                 R"HTML(<div id="sync" class="dcs-check" aria-checked="true">)HTML");
    ui.html(html);

    constexpr int w = 1280;
    constexpr int h = 860;
    ui.document().layout(w, h, &painter);

    const affineui::Point sub = find_hovered_id(ui.document(), "sub", w, h);
    REQUIRE(sub.x >= 0);
    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = sub;
    ui.dispatch(down);

    affineui::Event drag{};
    drag.type = affineui::EventType::MouseMove;
    drag.pos = {sub.x + 48, sub.y};
    ui.dispatch(drag);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = drag.pos;
    ui.dispatch(up);

    REQUIRE(ui.set_attr("shape__indicator", "style", "--angle:48deg"));
    REQUIRE(ui.set_text("shape__value", "0.68"));
    ui.document().set_animation_time_for_testing(0.35);
    ui.document().layout(w, h, &painter);

    painter.fill_draws.clear();
    painter.fill_colors.clear();
    painter.rounded_fill_draws.clear();
    painter.text_draws.clear();
    painter.text_runs.clear();
    ui.document().draw(painter);

    const auto checked_fill = affineui::Color::rgb(0x3d, 0xd6, 0x8a);
    CHECK((saw_fill(painter, checked_fill) ||
           find_rounded_fill_draw(painter, checked_fill) != nullptr));
    const auto* icon = find_text_draw(painter, "\xEE\x80\x9B");
    REQUIRE(icon != nullptr);
    CHECK(same_color(icon->color, affineui::Color::rgb(0x0a, 0x12, 0x20)));
    CHECK(find_text_draw(painter, "Hard sync") != nullptr);
}

TEST_CASE("dark synth unchecked checkbox keeps its box after hover leave and drag") {
    affineui::Ui ui;
    RecordingPainter painter;

    const auto examples_root =
        std::filesystem::path{AFFINEUI_TEST_SOURCE_DIR} / "examples";
    ui.document().set_resource_loader(
        [examples_root](std::string_view url) -> std::string {
            const std::filesystem::path rel{std::string(url)};
            return read_test_file(examples_root / "13_decius_synth_dark" / rel);
        });

    const std::string html = read_test_file(
        examples_root / "13_decius_synth_dark" / "index.html");
    REQUIRE_FALSE(html.empty());
    ui.html(html);

    constexpr int w = 1280;
    constexpr int h = 860;
    ui.document().layout(w, h, &painter);

    const affineui::Point sync = find_hovered_id(ui.document(), "sync", w, h);
    REQUIRE(sync.x >= 0);
    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = sync;
    ui.dispatch(move);

    const affineui::Point sub = find_hovered_id(ui.document(), "sub", w, h);
    REQUIRE(sub.x >= 0);
    move.pos = sub;
    ui.dispatch(move);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = sub;
    ui.dispatch(down);

    affineui::Event drag{};
    drag.type = affineui::EventType::MouseMove;
    drag.pos = {sub.x + 48, sub.y};
    ui.dispatch(drag);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = drag.pos;
    ui.dispatch(up);

    ui.document().layout(w, h, &painter);
    painter.fill_draws.clear();
    painter.fill_colors.clear();
    painter.rounded_fill_draws.clear();
    painter.border_ring_draws.clear();
    painter.stroke_colors.clear();
    painter.text_draws.clear();
    painter.text_runs.clear();
    ui.document().draw(painter);

    const auto* box_ring = find_border_ring_left_of_text(painter, "Hard sync");
    REQUIRE(box_ring != nullptr);
    CHECK(box_ring->rect.w == 14);
    CHECK(box_ring->rect.h == 14);
    CHECK(same_color(box_ring->color,
                     affineui::Color::rgb(0x5d, 0x65, 0x77)));
}

TEST_CASE("dark synth C++ value interactions keep checked checkbox visible") {
    namespace dcs = demo::decius;

    affineui::Ui ui;
    RecordingPainter painter;

    struct State {
        bool sync{false};
        bool armed{true};
        std::unordered_map<std::string, float> values{
            {"shape", 0.66f},
            {"sub", 0.54f},
            {"fader-a", 0.28f},
        };
    } state;

    const auto examples_root =
        std::filesystem::path{AFFINEUI_TEST_SOURCE_DIR} / "examples";
    ui.document().set_resource_loader(
        [examples_root](std::string_view url) -> std::string {
            const std::filesystem::path rel{std::string(url)};
            return read_test_file(examples_root / rel);
        });

    auto value = [&](std::string_view id) {
        const auto it = state.values.find(std::string(id));
        return it == state.values.end() ? 0.0f : it->second;
    };

    const auto render = [&] {
        std::ostringstream h;
        h << R"HTML(
            <!doctype html><html><head><meta charset="utf-8">
            <link rel="stylesheet" href="frameworks/css/decius-css-0.5.2.bundle.min.css">
            <style>
            body{margin:0;background:#101219}
            .desk{padding:18px;background:#101219}
            .module{display:flex;align-items:center;gap:22px;background:#20232b;padding:16px;border-radius:5px}
            .slider-wrap{width:220px}
            </style></head>
            <body class="dcs" data-dcs-density="comfortable" data-dcs-accent="green">
            <div class="desk"><div class="module">
        )HTML"
          << dcs::check("Hard sync", state.sync, false, "sync")
          << R"HTML(<div class="slider-wrap">)HTML"
          << dcs::slider(0, 1, value("sub"), false, true, "sub")
          << R"HTML(</div>)HTML"
          << dcs::fader(value("fader-a"), true, "fader-a")
          << dcs::knob(0, 1, value("shape"), "Shape", false, 72, "shape")
          << dcs::toggle("Voice armed", state.armed, "armed")
          << R"HTML(</div></div></body></html>)HTML";
        return h.str();
    };

    auto rerender = [&] {
        ui.html(render());
        ui.mark_dirty();
    };
    rerender();

    ui.on_click("#sync", [&] {
        state.sync = !state.sync;
        rerender();
    });
    ui.on_click("#armed", [&] {
        state.armed = !state.armed;
        rerender();
    });
    dcs::install_value_interactions(ui, {
        [&](std::string_view id) { return value(id); },
        [&](std::string_view id, float v) {
            state.values[std::string(id)] = v;
        },
        rerender,
    });

    constexpr int w = 520;
    constexpr int h = 180;
    auto send_mouse = [&](affineui::EventType type,
                          affineui::Point pos,
                          affineui::MouseButton button =
                              affineui::MouseButton::Left) {
        affineui::Event ev{};
        ev.type = type;
        ev.pos = pos;
        ev.button = button;
        ui.dispatch(ev);
        ui.document().layout(w, h, &painter);
    };

    ui.document().layout(w, h, &painter);
    const affineui::Point sync = find_hovered_id(ui.document(), "sync", w, h);
    REQUIRE(sync.x >= 0);
    send_mouse(affineui::EventType::MouseMove, sync);
    send_mouse(affineui::EventType::MouseDown, sync,
               affineui::MouseButton::Left);
    send_mouse(affineui::EventType::MouseUp, sync,
               affineui::MouseButton::Left);
    REQUIRE(state.sync);

    const affineui::Point sub = find_hovered_id(ui.document(), "sub", w, h);
    REQUIRE(sub.x >= 0);
    send_mouse(affineui::EventType::MouseMove, sub);
    send_mouse(affineui::EventType::MouseDown, sub,
               affineui::MouseButton::Left);
    send_mouse(affineui::EventType::MouseMove, {sub.x + 74, sub.y});
    send_mouse(affineui::EventType::MouseUp, {sub.x + 74, sub.y},
               affineui::MouseButton::Left);

    const affineui::Point fader =
        find_hovered_id(ui.document(), "fader-a", w, h);
    REQUIRE(fader.x >= 0);
    send_mouse(affineui::EventType::MouseMove, fader);
    send_mouse(affineui::EventType::MouseDown, fader,
               affineui::MouseButton::Left);
    const float fader_after_press = state.values["fader-a"];
    send_mouse(affineui::EventType::MouseMove, {fader.x, fader.y + 38});
    send_mouse(affineui::EventType::MouseUp, {fader.x, fader.y + 38},
               affineui::MouseButton::Left);
    CHECK(state.values["fader-a"] < fader_after_press);
    const float fader_after_down_drag = state.values["fader-a"];
    send_mouse(affineui::EventType::MouseMove, fader);
    send_mouse(affineui::EventType::MouseDown, fader,
               affineui::MouseButton::Left);
    send_mouse(affineui::EventType::MouseMove, {fader.x, fader.y - 38});
    send_mouse(affineui::EventType::MouseUp, {fader.x, fader.y - 38},
               affineui::MouseButton::Left);
    CHECK(state.values["fader-a"] > fader_after_down_drag);

    const affineui::Point shape =
        find_hovered_id(ui.document(), "shape", w, h);
    REQUIRE(shape.x >= 0);
    send_mouse(affineui::EventType::MouseMove, shape);
    send_mouse(affineui::EventType::MouseDown, shape,
               affineui::MouseButton::Left);
    send_mouse(affineui::EventType::MouseMove, {shape.x, shape.y - 36});
    send_mouse(affineui::EventType::MouseUp, {shape.x, shape.y - 36},
               affineui::MouseButton::Left);

    painter.fill_draws.clear();
    painter.fill_colors.clear();
    painter.rounded_fill_draws.clear();
    painter.border_ring_draws.clear();
    painter.stroke_colors.clear();
    painter.text_draws.clear();
    painter.text_runs.clear();
    ui.document().draw(painter);

    const auto checked_fill = affineui::Color::rgb(0x3d, 0xd6, 0x8a);
    CHECK((saw_fill(painter, checked_fill) ||
           find_rounded_fill_draw(painter, checked_fill) != nullptr));
    const auto* icon = find_text_draw(painter, "\xEE\x80\x9B");
    REQUIRE(icon != nullptr);
    CHECK(same_color(icon->color, affineui::Color::rgb(0x0a, 0x12, 0x20)));
    CHECK(find_text_draw(painter, "Hard sync") != nullptr);
}

TEST_CASE("live Decius checkbox state survives ordinary viewport relayout") {
    namespace dcs = demo::decius;

    affineui::Ui ui;
    RecordingPainter painter;

    const auto examples_root =
        std::filesystem::path{AFFINEUI_TEST_SOURCE_DIR} / "examples";
    ui.document().set_resource_loader(
        [examples_root](std::string_view url) -> std::string {
            const std::filesystem::path rel{std::string(url)};
            return read_test_file(examples_root / rel);
        });

    std::ostringstream h;
    h << R"HTML(
        <!doctype html><html><head><meta charset="utf-8">
        <link rel="stylesheet" href="frameworks/css/decius-css-0.5.2.bundle.min.css">
        <style>
        body{margin:0;background:#101219}
        .desk{padding:18px;background:#101219}
        </style></head>
        <body class="dcs" data-dcs-density="comfortable" data-dcs-accent="green">
        <div class="desk">
    )HTML"
      << dcs::check("Hard sync", false, false, "sync")
      << R"HTML(</div></body></html>)HTML";

    ui.html(h.str());
    ui.document().layout(520, 160, &painter);
    REQUIRE(ui.set_attr("sync", "aria-checked", "true"));

    // Resizing inside the same media-query set must not reparse from the
    // original HTML string, or live DOM mutations disappear.
    ui.document().layout(560, 160, &painter);

    painter.fill_draws.clear();
    painter.fill_colors.clear();
    painter.rounded_fill_draws.clear();
    painter.border_ring_draws.clear();
    painter.stroke_colors.clear();
    painter.text_draws.clear();
    painter.text_runs.clear();
    ui.document().draw(painter);

    const auto checked_fill = affineui::Color::rgb(0x3d, 0xd6, 0x8a);
    CHECK((saw_fill(painter, checked_fill) ||
           find_rounded_fill_draw(painter, checked_fill) != nullptr));
    const auto* icon = find_text_draw(painter, "\xEE\x80\x9B");
    REQUIRE(icon != nullptr);
    CHECK(same_color(icon->color, affineui::Color::rgb(0x0a, 0x12, 0x20)));
}

TEST_CASE("game editor inspector keeps Decius check and switch interactive across rerenders") {
    namespace dcs = demo::decius;

    affineui::Ui ui;
    RecordingPainter painter;

    struct State {
        int selected{1};
        bool cast_shadows{true};
        bool gpu_skinning{true};
    } state;

    const auto examples_root =
        std::filesystem::path{AFFINEUI_TEST_SOURCE_DIR} / "examples";
    ui.document().set_resource_loader(
        [examples_root](std::string_view url) -> std::string {
            const std::filesystem::path rel{std::string(url)};
            return read_test_file(examples_root / rel);
        });

    const auto render = [&] {
        const char* selected_name = state.selected == 0 ? "WorldRoot"
                                  : state.selected == 1 ? "Hero_mesh_high"
                                                        : "KeyLight";
        std::ostringstream h;
        h << R"HTML(
            <!doctype html><html><head><meta charset="utf-8">
            <link rel="stylesheet" href="frameworks/css/decius-css-0.5.2.bundle.min.css">
            <style>
            body{margin:0;background:#14161c}
            .app{height:100vh;background:var(--dcs-bg-app);display:flex;gap:1px}
            .left{flex:0 0 260px}.right{flex:0 0 340px}.main{flex:1;min-width:0}
            .prop-row{display:flex;align-items:center;gap:8px;margin-bottom:8px}
            .prop-row span{flex:0 0 92px;color:var(--dcs-text-mute);font-size:var(--dcs-fs-xs);text-transform:uppercase;letter-spacing:.08em}
            .prop-row .dcs-input{flex:1}
            </style></head>
            <body class="dcs" data-dcs-density="compact" data-dcs-style="3d">
            <div class="app">
              <aside class="left dcs-dockpane"><div class="dcs-dockpane__body">
                <div class="dcs-tree">
                  <div id="object-0" class="dcs-tree__row")HTML"
          << (state.selected == 0 ? " aria-selected=\"true\"" : "")
          << R"HTML(><span class="dcs-tree__label">WorldRoot</span></div>
                  <div id="object-1" class="dcs-tree__row")HTML"
          << (state.selected == 1 ? " aria-selected=\"true\"" : "")
          << R"HTML(><span class="dcs-tree__label">Hero_mesh_high</span></div>
                  <div id="object-2" class="dcs-tree__row")HTML"
          << (state.selected == 2 ? " aria-selected=\"true\"" : "")
          << R"HTML(><span class="dcs-tree__label">KeyLight</span></div>
                </div>
              </div></aside>
              <main class="main dcs-dockpane"><div class="dcs-dockpane__body"></div></main>
              <aside class="right dcs-dockpane"><div class="dcs-dockpane__body" style="padding:12px">
        )HTML"
          << "<h3 style=\"margin:0 0 12px;font-size:16px\">"
          << selected_name << "</h3>"
          << "<div class=\"prop-row\"><span>Name</span><input class=\"dcs-input\" value=\""
          << selected_name << "\"></div>"
          << "<div style=\"margin:14px 0\">"
          << dcs::slider(0, 1, .62f, false, true, "roughness")
          << "</div>"
          << dcs::check("Cast shadows", state.cast_shadows, false,
                        "cast-shadows")
          << dcs::toggle("GPU skinning", state.gpu_skinning,
                         "gpu-skinning")
          << R"HTML(
              </div></aside>
            </div></body></html>
        )HTML";
        return h.str();
    };

    auto rerender = [&] {
        ui.html(render());
        ui.mark_dirty();
    };
    rerender();

    ui.on_click("#cast-shadows", [&] {
        state.cast_shadows = !state.cast_shadows;
        if (!dcs::set_checked(ui, "cast-shadows", state.cast_shadows)) {
            rerender();
        }
    });
    ui.on_click("#gpu-skinning", [&] {
        state.gpu_skinning = !state.gpu_skinning;
        if (!dcs::set_checked(ui, "gpu-skinning", state.gpu_skinning)) {
            rerender();
        }
    });
    ui.on_click("#object-0", [&] { state.selected = 0; rerender(); });

    constexpr int w = 940;
    constexpr int h = 420;
    auto send_mouse = [&](affineui::EventType type,
                          affineui::Point pos,
                          affineui::MouseButton button =
                              affineui::MouseButton::Left) {
        affineui::Event ev{};
        ev.type = type;
        ev.pos = pos;
        ev.button = button;
        ui.dispatch(ev);
        ui.document().layout(w, h, &painter);
    };

    ui.document().layout(w, h, &painter);
    const affineui::Point object =
        find_hovered_chain_id(ui.document(), "object-0", w, h);
    const affineui::Point check =
        find_hovered_chain_id(ui.document(), "cast-shadows", w, h);
    const affineui::Point toggle =
        find_hovered_chain_id(ui.document(), "gpu-skinning", w, h);
    REQUIRE(object.x >= 0);
    REQUIRE(check.x >= 0);
    REQUIRE(toggle.x >= 0);

    send_mouse(affineui::EventType::MouseMove, object);
    send_mouse(affineui::EventType::MouseDown, object);
    send_mouse(affineui::EventType::MouseUp, object);
    REQUIRE(state.selected == 0);

    painter.fill_draws.clear();
    painter.fill_colors.clear();
    painter.rounded_fill_draws.clear();
    painter.text_draws.clear();
    painter.text_runs.clear();
    ui.document().draw(painter);

    const auto* box_ring = find_border_ring_left_of_text(painter, "Cast shadows");
    REQUIRE(box_ring != nullptr);
    CHECK(box_ring->rect.w == 14);
    CHECK(box_ring->rect.h == 14);
    REQUIRE(find_text_draw(painter, "\xEE\x80\x9B") != nullptr);
    CHECK(find_text_draw(painter, "Cast shadows") != nullptr);

    send_mouse(affineui::EventType::MouseMove, check);
    send_mouse(affineui::EventType::MouseDown, check);
    send_mouse(affineui::EventType::MouseUp, check);
    CHECK_FALSE(state.cast_shadows);

    send_mouse(affineui::EventType::MouseMove, toggle);
    send_mouse(affineui::EventType::MouseDown, toggle);
    send_mouse(affineui::EventType::MouseUp, toggle);
    CHECK_FALSE(state.gpu_skinning);
}

TEST_CASE("common named HTML entities decode in compact entity mode") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>body { margin: 0; padding: 0; } p { margin: 0; }</style>
        <p>&amp; &lt; &gt; &quot; &apos; &nbsp; &times; &mdash;</p>
    )HTML");
    doc.layout(640, 0, &painter);
    doc.draw(painter);

    std::string expected = "& < > \" ' ";
    expected += "\xC2\xA0";
    expected += " ";
    expected += "\xC3\x97";
    expected += " ";
    expected += "\xE2\x80\x94";

    bool saw_expected = false;
    for (const auto& text : painter.text_runs) {
        if (text == expected) saw_expected = true;
    }
    CHECK(saw_expected);
}

TEST_CASE("user stylesheet round-trips through set_user_stylesheet") {
    affineui::Document doc;
    doc.set_user_stylesheet("body { color: red; }");
    CHECK(true);
}

TEST_CASE("linked stylesheet is loaded through resource loader") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_resource_loader([](std::string_view url) -> std::string {
        if (url == "app.css") {
            return "button { border: 1px solid #123456; }";
        }
        return {};
    });
    doc.set_html(R"HTML(
        <link rel="stylesheet" href="app.css">
        <button>Loaded CSS</button>
    )HTML");
    doc.layout(320, 0, &painter);

    painter.stroke_colors.clear();
    doc.draw(painter);
    CHECK(saw_stroke(painter, affineui::Color::rgb(0x12, 0x34, 0x56)));
}

TEST_CASE("uniform solid border paints the CSS border area") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .box {
            box-sizing: border-box;
            width: 14px;
            height: 14px;
            background: #4d9fff;
            border: 1px solid #2f86ee;
            border-radius: 2px;
        }
        </style>
        <div class="box"></div>
    )HTML");
    doc.layout(64, 0, &painter);
    doc.draw(painter);

    REQUIRE_FALSE(painter.border_ring_draws.empty());
    const auto& ring = painter.border_ring_draws.back();
    CHECK(ring.rect.x == 0);
    CHECK(ring.rect.y == 0);
    CHECK(ring.rect.w == 14);
    CHECK(ring.rect.h == 14);
    CHECK(ring.radius == doctest::Approx(2.0f));
    CHECK(ring.thickness == doctest::Approx(1.0f));
    CHECK(same_color(ring.color, affineui::Color::rgb(0x2f, 0x86, 0xee)));
}

TEST_CASE("flex-centered slider track paints at explicit height") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .slider {
          position: relative;
          display: flex;
          align-items: center;
          width: 212px;
          height: 22px;
        }
        .track {
          position: relative;
          width: 100%;
          height: 4px;
          background: #111111;
          border-radius: 999px;
        }
        .fill {
          position: absolute;
          top: 0;
          bottom: 0;
          left: 0;
          width: 68%;
          background: #4d9fff;
          border-radius: 999px;
        }
        </style>
        <div class="slider"><div class="track"><div class="fill"></div></div></div>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    bool saw_blue_fill_at_track_height = false;
    for (const auto& draw : painter.rounded_fill_draws) {
        if (same_color(draw.color, affineui::Color::rgb(0x4d, 0x9f, 0xff)) &&
            draw.rect.h == 4) {
            saw_blue_fill_at_track_height = true;
        }
    }
    CHECK(saw_blue_fill_at_track_height);
}

TEST_CASE("Decius property slider fill stays constrained to track height") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs, .dcs * { box-sizing: border-box; }
        .dcs-field {
          display: flex;
          align-items: center;
          gap: 6px;
          min-height: 24px;
        }
        .dcs-field__label {
          flex: 0 0 96px;
        }
        .dcs-props {
          display: flex;
          flex-direction: column;
          gap: 6px;
        }
        .dcs-props > .dcs-field {
          height: 22px;
          min-height: 22px;
          justify-content: space-between;
        }
        .dcs-props > .dcs-field > .dcs-slider {
          flex: 1;
          min-width: 0;
          height: 22px;
        }
        .dcs-slider {
          position: relative;
          display: flex;
          align-items: center;
          height: 24px;
          width: 100%;
          min-width: 80px;
        }
        .dcs-slider__track {
          position: relative;
          height: 4px;
          width: 100%;
          background: #20232b;
          border: 1px solid #14161c;
          border-radius: 999px;
        }
        .dcs-slider__fill {
          position: absolute;
          top: 0;
          bottom: 0;
          left: 0;
          width: 68%;
          background: linear-gradient(90deg, #2f86ee, #4d9fff);
          border-radius: 999px;
        }
        </style>
        <div class="dcs">
          <div class="dcs-props">
            <div class="dcs-field">
              <span class="dcs-field__label">Mix</span>
              <div class="dcs-slider">
                <div class="dcs-slider__track">
                  <div class="dcs-slider__fill"></div>
                </div>
              </div>
            </div>
          </div>
        </div>
    )HTML");
    doc.layout(360, 0, &painter);
    doc.draw(painter);

    bool saw_gradient_at_track_height = false;
    bool saw_track_at_track_height = false;
    for (const auto& draw : painter.rounded_fill_draws) {
        if (same_color(draw.color, affineui::Color::rgb(0x20, 0x23, 0x2b))) {
            CAPTURE(draw.rect.x);
            CAPTURE(draw.rect.y);
            CAPTURE(draw.rect.w);
            CAPTURE(draw.rect.h);
            CHECK(draw.rect.h == 4);
            saw_track_at_track_height = true;
        }
    }
    CHECK(saw_track_at_track_height);
    for (const auto& draw : painter.linear_gradient_draws) {
        if (same_color(draw.stop0, affineui::Color::rgb(0x2f, 0x86, 0xee))) {
            CAPTURE(draw.rect.x);
            CAPTURE(draw.rect.y);
            CAPTURE(draw.rect.w);
            CAPTURE(draw.rect.h);
            CHECK(draw.rect.h == 2);
            saw_gradient_at_track_height = true;
        }
    }
    CHECK(saw_gradient_at_track_height);
}

TEST_CASE("transparent side on circular border paints the missing quadrant") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .spin {
          display: block;
          width: 32px;
          height: 32px;
          --w: .25em;
          color: #0d6efd;
          border: var(--w) solid currentColor;
          border-right-color: transparent;
          border-radius: 50%;
        }
        </style>
        <div class="spin"></div>
    )HTML");
    doc.layout(120, 0, &painter);
    doc.draw(painter);

    REQUIRE(painter.arc_draws.size() == 3);
    auto has_arc = [&](float start, float end) {
        for (const auto& arc : painter.arc_draws) {
            if (arc.start_deg == doctest::Approx(start) &&
                arc.end_deg == doctest::Approx(end)) {
                CHECK(same_color(arc.color, affineui::Color::rgb(0x0D, 0x6E, 0xFD)));
                return true;
            }
        }
        return false;
    };

    CHECK(has_arc(-45.0f, 45.0f));
    CHECK(has_arc(135.0f, 225.0f));
    CHECK(has_arc(225.0f, 315.0f));
}

TEST_CASE("inline svg circular arc path paints with viewBox scaling and vars") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        :root { --dcs-accent: #4d9fff; }
        body { margin: 0; padding: 0; }
        .knob { display: block; width: 56px; height: 56px; }
        </style>
        <div class="knob">
          <svg viewBox="0 0 24 24">
            <path d="M 4.57537879754125 19.42462120245875 A 10.5 10.5 0 1 1 19.42462120245875 19.42462120245875"
                  fill="none" stroke="rgba(255,255,255,.08)" stroke-width="1.5" stroke-linecap="round"></path>
            <path d="M 4.57537879754125 19.42462120245875 A 10.5 10.5 0 0 1 4.123833768880174 5.056225414101656"
                  fill="none" stroke="var(--dcs-accent)" stroke-width="1.75" stroke-linecap="round"></path>
          </svg>
        </div>
    )HTML");
    doc.layout(80, 0, &painter);
    doc.draw(painter);

    bool saw_accent_arc = false;
    for (const auto& arc : painter.arc_draws) {
        if (same_color(arc.color, affineui::Color::rgb(0x4d, 0x9f, 0xff))) {
            saw_accent_arc = true;
            CHECK(arc.cx == doctest::Approx(28.0f));
            CHECK(arc.cy == doctest::Approx(28.0f));
            CHECK(arc.radius == doctest::Approx(24.5f));
            CHECK(arc.start_deg == doctest::Approx(-135.0f));
            CHECK(arc.end_deg == doctest::Approx(-48.6f).epsilon(0.01));
        }
    }
    CHECK(saw_accent_arc);
}

TEST_CASE("img element loads and draws through painter") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        img { display: block; }
        </style>
        <img src="picture.png">
    )HTML");
    doc.layout(320, 0, &painter);

    painter.image_urls.clear();
    painter.image_draws.clear();
    doc.draw(painter);

    REQUIRE(!painter.image_urls.empty());
    CHECK(painter.image_urls.back() == "picture.png");
    REQUIRE(painter.image_draws.size() == 1);
    CHECK(painter.image_draws.back().w == 64);
    CHECK(painter.image_draws.back().h == 32);
}

TEST_CASE("collapsed whitespace between inline-block siblings is rendered") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>body { margin: 0; padding: 0; } button { display: inline-block; }</style>
        <button>A</button> <button>B</button>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    bool saw_space = false;
    for (const auto& text : painter.text_runs) {
        if (text == " ") saw_space = true;
    }
    CHECK(saw_space);
}

TEST_CASE("mixed inline button text does not center into following badge") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        button { display: inline-block; text-align: center; padding: 0; }
        span { display: inline-block; padding: 0; }
        </style>
        <button>Notifications <span>4</span></button>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    const auto* label = find_text_draw(painter, "Notifications");
    const auto* badge = find_text_draw(painter, "4");
    REQUIRE(label != nullptr);
    REQUIRE(badge != nullptr);

    const int label_w = static_cast<int>(std::string_view("Notifications").size()) * 8;
    CHECK(label->max_width <= static_cast<float>(label_w + 4));
    CHECK(label->pos.x + label_w <= badge->pos.x);
}

TEST_CASE("right-aligned input text uses its own content box") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .field {
          display: flex;
          align-items: center;
          width: 394px;
          height: 22px;
        }
        .label { flex: 0 0 96px; }
        input {
          flex: 1;
          min-width: 0;
          box-sizing: border-box;
          height: 22px;
          border: 1px solid #000;
          padding: 0 6px;
          text-align: right;
          font-size: 12px;
          line-height: 1;
        }
        </style>
        <label class="field"><span class="label">Scale</span><input value="1.000"></label>
    )HTML");
    doc.layout(480, 0, &painter);
    doc.draw(painter);

    const auto* value = find_text_draw(painter, "1.000");
    REQUIRE(value != nullptr);

    // The input occupies x=96..394. With border-box sizing, 1px border and
    // 6px left/right padding, its content box starts at x=103 and is 284px
    // wide. The old fallback aligned against the whole field row, passing
    // x=0 and a ~394px line box to the painter. A later paint-only wrap
    // slack bug widened the right-aligned line box by 4px and pushed the
    // glyphs right, so keep the painted width exact for aligned controls.
    CHECK(value->pos.x == 103);
    CHECK(value->max_width == 284.0f);
    CHECK(value->align == affineui::Painter::TextAlign::Right);
}

TEST_CASE("native select text uses platform inner inset") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        select {
          display: flex;
          align-items: center;
          box-sizing: border-box;
          width: 292px;
          height: 22px;
          border: 1px solid #000;
          padding: 0 6px;
          font-size: 12px;
          line-height: normal;
        }
        </style>
        <select><option>Object</option></select>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    const auto* value = find_text_draw(painter, "Object");
    REQUIRE(value != nullptr);

    CHECK(value->pos.x == 12);
    CHECK(value->max_width == 273.0f);
}

TEST_CASE("textarea text uses native edit viewport top inset") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        textarea {
          display: flex;
          box-sizing: border-box;
          width: 292px;
          height: 22px;
          border: 1px solid #000;
          padding: 6px;
          font-size: 12px;
          line-height: 1.45;
        }
        </style>
        <textarea>Dense native UI, browser semantics.</textarea>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    const auto* value =
        find_text_draw(painter, "Dense native UI, browser semantics.");
    REQUIRE(value != nullptr);

    CHECK(value->pos.x == 7);
    CHECK(value->pos.y == 12);
}

TEST_CASE("single-line flex text honors align-items center") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .box {
          display: flex;
          align-items: center;
          width: 120px;
          height: 22px;
          font-size: 12px;
          line-height: 1;
        }
        </style>
        <div class="box">Value</div>
    )HTML");
    doc.layout(200, 0, &painter);
    doc.draw(painter);

    const auto* value = find_text_draw(painter, "Value");
    REQUIRE(value != nullptr);
    CHECK(value->pos.y > 0);
}

TEST_CASE("native select indicator uses inherited color and native inset") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        select {
          display: block;
          box-sizing: border-box;
          width: 292px;
          height: 22px;
          color: #e7e9ee;
          background: #20232b;
          border: 1px solid #14161c;
        }
        </style>
        <select><option>Object</option></select>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    const auto chev = affineui::Color::rgb(0xe7, 0xe9, 0xee);
    std::vector<RecordingPainter::StrokeLineDraw> lines;
    for (const auto& line : painter.stroke_line_draws) {
        if (same_color(line.color, chev)) lines.push_back(line);
    }

    REQUIRE(lines.size() == 2);
    CHECK(lines[0].x1 == doctest::Approx(282.25f));
    CHECK(lines[1].x0 == doctest::Approx(282.25f));
    CHECK(lines[0].x0 == doctest::Approx(278.5f));
    CHECK(lines[1].x1 == doctest::Approx(286.0f));
    CHECK(lines[0].thickness == doctest::Approx(1.35f));
}

TEST_CASE("bootstrap form-select indicator follows SVG background placement") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .form-select {
          display: block;
          box-sizing: border-box;
          width: 480px;
          height: 40px;
          color: #212529;
          background: #fff;
          border: 1px solid #dee2e6;
        }
        </style>
        <select class="form-select"><option>Open this select menu</option></select>
    )HTML");
    doc.layout(520, 0, &painter);
    doc.draw(painter);

    const auto chev = affineui::Color::rgb(0x34, 0x3a, 0x40);
    std::vector<RecordingPainter::StrokeLineDraw> lines;
    for (const auto& line : painter.stroke_line_draws) {
        if (same_color(line.color, chev)) lines.push_back(line);
    }

    REQUIRE(lines.size() == 2);
    CHECK(lines[0].x1 == doctest::Approx(459.5f));
    CHECK(lines[1].x0 == doctest::Approx(459.5f));
    CHECK(lines[0].x0 == doctest::Approx(455.5f));
    CHECK(lines[1].x1 == doctest::Approx(463.5f));
    CHECK(lines[0].thickness == doctest::Approx(1.25f));
}

TEST_CASE("listbox selects do not draw a dropdown indicator") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        select {
          display: block;
          width: 120px;
          height: 80px;
          color: #e7e9ee;
          border: 1px solid #14161c;
        }
        </style>
        <select multiple><option>Object</option></select>
    )HTML");
    doc.layout(160, 0, &painter);
    doc.draw(painter);

    CHECK_FALSE(saw_stroke(painter, affineui::Color::rgb(0xe7, 0xe9, 0xee)));
}

TEST_CASE("generated before content participates in breadcrumb inline flow") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; font-size: 16px; line-height: 1; }
        ol { display: flex; list-style: none; margin: 0; padding: 0; --gap: .5rem; }
        .item + .item { padding-left: var(--gap); }
        .item + .item::before {
            content: var(--divider, "/");
            padding-right: var(--gap);
            color: rgba(33, 37, 41, .75);
        }
        </style>
        <ol><li class="item">Home</li><li class="item">Library</li></ol>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    const auto* slash = find_text_draw(painter, "/");
    const auto* library = find_text_draw(painter, "Library");
    REQUIRE(slash != nullptr);
    REQUIRE(library != nullptr);

    CHECK(slash->pos.x < library->pos.x);
    CHECK(library->pos.x >= slash->pos.x + 16);
}

TEST_CASE("empty positioned generated content paints a box") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .host {
          position: relative;
          display: flex;
          align-items: center;
          width: 120px;
          height: 32px;
          padding: 0 10px;
          border: 1px solid #3a3f4c;
          overflow: hidden;
          background: #20232b;
        }
        .host::before,
        .host::after {
          content: "";
          position: absolute;
          left: 0;
          right: 0;
          height: 1px;
        }
        .host::before {
          top: 12px;
          background: #4d9fff;
        }
        .host::after {
          bottom: 10px;
          background: #ef6b6b;
        }
        </style>
        <div class="host"></div>
    )HTML");
    doc.layout(180, 0, &painter);
    doc.draw(painter);

    const auto* guide = find_fill_draw(
        painter, affineui::Color::rgb(0x4d, 0x9f, 0xff));
    REQUIRE(guide != nullptr);
    CHECK(guide->rect.w == 140);
    CHECK(guide->rect.h == 1);

    const auto* lower = find_fill_draw(
        painter, affineui::Color::rgb(0xef, 0x6b, 0x6b));
    REQUIRE(lower != nullptr);
    CHECK(lower->rect.w == 140);
    CHECK(lower->rect.h == 1);
}

TEST_CASE("Decius empty generated control details paint") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .row { display: flex; align-items: center; gap: 12px; }
        .dcs-switch {
          position: relative;
          width: 28px;
          height: 16px;
          background: #20232b;
          border-radius: 999px;
        }
        .dcs-switch:after {
          content: "";
          position: absolute;
          top: 1px;
          left: 1px;
          width: 12px;
          height: 12px;
          background: #767c8a;
          border-radius: 50%;
        }
        .dcs-switch[aria-checked=true] {
          background: #4d9fff;
        }
        .dcs-switch[aria-checked=true]:after {
          transform: translateX(12px);
          background: #0a1220;
        }
        .dcs-badge {
          display: inline-flex;
          align-items: center;
          gap: 4px;
          color: #aab0bd;
        }
        .dcs-badge--dot:before {
          content: "";
          width: 6px;
          height: 6px;
          border-radius: 50%;
          background: currentColor;
          opacity: .85;
        }
        .dcs-radio .dcs-check__box {
          position: relative;
          width: 14px;
          height: 14px;
          border-radius: 50%;
          background: #4d9fff;
        }
        .dcs-radio[aria-checked=true] .dcs-check__box:after {
          content: "";
          width: 6px;
          height: 6px;
          border-radius: 50%;
          background: #0a1220;
        }
        </style>
        <div class="row">
          <div class="dcs-switch" aria-checked="true"></div>
          <span class="dcs-badge dcs-badge--dot">armed</span>
          <div class="dcs-radio" aria-checked="true"><div class="dcs-check__box"></div></div>
        </div>
    )HTML");
    doc.layout(240, 0, &painter);
    doc.draw(painter);

    const auto* switch_thumb = find_rounded_fill_draw(
        painter, affineui::Color::rgb(0x0a, 0x12, 0x20), 12, 12);
    REQUIRE(switch_thumb != nullptr);
    CHECK(switch_thumb->tl == doctest::Approx(6.0f));

    const auto* radio_dot = find_rounded_fill_draw(
        painter, affineui::Color::rgb(0x0a, 0x12, 0x20), 6, 6);
    REQUIRE(radio_dot != nullptr);
    CHECK(radio_dot->tl == doctest::Approx(3.0f));

    const auto* badge_dot = find_rounded_fill_draw(
        painter, affineui::Color::rgb(0xaa, 0xb0, 0xbd), 6, 6);
    REQUIRE(badge_dot != nullptr);
    CHECK(badge_dot->tl == doctest::Approx(3.0f));
}

TEST_CASE("generated pseudo-elements match body ancestor attributes") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .switch {
          position: relative;
          display: block;
          width: 28px;
          height: 16px;
        }
        .switch::after {
          content: "";
          position: absolute;
          left: 1px;
          top: 1px;
          width: 12px;
          height: 12px;
          background: #111111;
          border-radius: 50%;
        }
        [data-mode="3d"] .switch[aria-checked="true"]::after {
          background: #eeeeee;
        }
        </style>
        <body data-mode="3d">
          <div class="switch" aria-checked="true"></div>
        </body>
    )HTML");
    doc.layout(100, 0, &painter);
    doc.draw(painter);

    CHECK(saw_fill(painter, affineui::Color::rgb(0xee, 0xee, 0xee)));
    CHECK_FALSE(saw_fill(painter, affineui::Color::rgb(0x11, 0x11, 0x11)));
}

TEST_CASE("linked stylesheet empty generated content keeps parsed declarations") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_resource_loader([](std::string_view url) -> std::string {
        if (url == "decius.css") {
            return R"CSS(
                @font-face { font-family: ignored; src: url(ignored.woff); }
                .dcs-switch {
                  position: relative;
                  width: 28px;
                  height: 16px;
                  background: #20232b;
                  border-radius: 999px;
                }
                .dcs-switch:after {
                  content: "";
                  position: absolute;
                  top: 1px;
                  left: 1px;
                  width: 12px;
                  height: 12px;
                  background: #767c8a;
                  border-radius: 50%;
                }
                .dcs-switch[aria-checked=true]:after {
                  transform: translateX(12px);
                  background: #0a1220;
                }
            )CSS";
        }
        return {};
    });
    doc.set_html(R"HTML(
        <link rel="stylesheet" href="decius.css">
        <div class="dcs-switch" aria-checked="true"></div>
    )HTML");
    doc.layout(120, 0, &painter);
    doc.draw(painter);

    const auto* thumb = find_rounded_fill_draw(
        painter, affineui::Color::rgb(0x0a, 0x12, 0x20), 12, 12);
    REQUIRE(thumb != nullptr);
    CHECK(thumb->tl == doctest::Approx(6.0f));
}

TEST_CASE("generated content decodes CSS string escapes") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; font-size: 16px; line-height: 1; }
        .icon::before { content: "\e02b"; }
        </style>
        <span class="icon"></span>
    )HTML");
    doc.layout(160, 0, &painter);
    doc.draw(painter);

    const auto* icon = find_text_draw(painter, "\xEE\x80\xAB");
    REQUIRE(icon != nullptr);
    CHECK(find_text_draw(painter, "\\e02b") == nullptr);
}

TEST_CASE("inline generated icon content inherits recovered font family") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; font-size: 16px; line-height: 1; }
        .di { font-family: decius-icons; display: inline-block; }
        .di-check::before { content: "\e01b"; }
        </style>
        <span class="di di-check"></span>
    )HTML");
    doc.layout(160, 0, &painter);
    doc.draw(painter);

    const auto* icon = find_text_draw(painter, "\xEE\x80\x9B");
    REQUIRE(icon != nullptr);

    bool requested_icon_font = false;
    for (const auto& family : painter.font_requests) {
        if (family == "decius-icons") {
            requested_icon_font = true;
            break;
        }
    }
    CHECK(requested_icon_font);
}

TEST_CASE("inline-block generated content contributes flex item width") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; font-size: 16px; line-height: 1; }
        .strip { display: flex; gap: 14px; }
        .icon { display: inline-block; }
        .one::before { content: "A"; }
        .two::before { content: "B"; }
        </style>
        <div class="strip"><i class="icon one"></i><i class="icon two"></i></div>
    )HTML");
    doc.layout(160, 0, &painter);
    doc.draw(painter);

    const auto* a = find_text_draw(painter, "A");
    const auto* b = find_text_draw(painter, "B");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    CHECK(b->pos.x >= a->pos.x + 22);
}

TEST_CASE("generated inline-block icon is centered by flex parent") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; font-size: 16px; line-height: 1; }
        .box {
            display: flex;
            align-items: center;
            justify-content: center;
            width: 14px;
            height: 14px;
        }
        .icon { display: inline-block; background: #ff0000; }
        .icon::before { content: "X"; }
        </style>
        <div class="box"><i class="icon"></i></div>
    )HTML");
    doc.layout(80, 40, &painter);
    doc.draw(painter);

    const auto* icon_text = find_text_draw(painter, "X");
    REQUIRE(icon_text != nullptr);
    CHECK(icon_text->pos.x == 3);

    const auto* icon_box = find_fill_draw(
        painter, affineui::Color::rgb(0xff, 0x00, 0x00));
    REQUIRE(icon_box != nullptr);
    CHECK(icon_box->rect.x == 3);
    CHECK(icon_box->rect.w == 8);
}

TEST_CASE("empty generated content paints an absolutely positioned box") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .host { position: relative; width: 28px; height: 16px; }
        .host::after {
            content: "";
            position: absolute;
            top: 1px;
            left: 1px;
            width: 12px;
            height: 12px;
            background: #ff0000;
            border-radius: 50%;
        }
        </style>
        <div class="host"></div>
    )HTML");
    doc.layout(160, 0, &painter);
    doc.draw(painter);

    bool saw_thumb = false;
    for (const auto& draw : painter.fill_draws) {
        if (same_color(draw.color, affineui::Color::rgb(0xff, 0x00, 0x00)) &&
            draw.rect.x == 1 && draw.rect.y == 1 &&
            draw.rect.w == 12 && draw.rect.h == 12) {
            saw_thumb = true;
        }
    }
    for (const auto& draw : painter.rounded_fill_draws) {
        if (same_color(draw.color, affineui::Color::rgb(0xff, 0x00, 0x00)) &&
            draw.rect.x == 1 && draw.rect.y == 1 &&
            draw.rect.w == 12 && draw.rect.h == 12) {
            saw_thumb = true;
        }
    }
    CHECK(saw_thumb);
}

TEST_CASE("empty generated content honors percentage inset and subpixel translate") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .host { position: relative; width: 22px; height: 8px; }
        .host::after {
            content: "";
            position: absolute;
            left: 3px;
            right: 3px;
            top: 50%;
            height: 1px;
            background: #4d9fff;
            transform: translateY(-.5px);
        }
        </style>
        <div class="host"></div>
    )HTML");
    doc.layout(80, 40, &painter);
    doc.draw(painter);

    const RecordingPainter::FillDraw* rule = nullptr;
    for (const auto& draw : painter.fill_draws) {
        if (same_color(draw.color, affineui::Color::rgb(0x4d, 0x9f, 0xff))) {
            rule = &draw;
            break;
        }
    }

    REQUIRE(rule != nullptr);
    CHECK(rule->rect.x == 3);
    CHECK(rule->rect.y == 4);
    CHECK(rule->rect.w == 16);
    CHECK(rule->rect.h == 1);
    CHECK(rule->transform.ty == doctest::Approx(-0.5f));
}

TEST_CASE("fractional layout position does not synthesize a paint transform") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .host {
            position: relative;
            box-sizing: border-box;
            width: 28px;
            height: 140px;
            border: 1px solid transparent;
        }
        .thumb {
            position: absolute;
            box-sizing: border-box;
            top: 24%;
            width: 22px;
            height: 12px;
            background: #ff0000;
        }
        </style>
        <div class="host"><div class="thumb"></div></div>
    )HTML");
    doc.layout(80, 160, &painter);
    doc.draw(painter);

    const auto* thumb = find_fill_draw(
        painter, affineui::Color::rgb(0xff, 0x00, 0x00));
    REQUIRE(thumb != nullptr);
    CHECK(thumb->rect.y == 34);
    CHECK(thumb->transform.tx == doctest::Approx(0.0f));
    CHECK(thumb->transform.ty == doctest::Approx(0.0f));
}

TEST_CASE("grid container blockifies and stretches inline-block buttons") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; font-size: 16px; line-height: 1.5; }
        .grid { display: grid; width: 256px; gap: 8px; }
        button {
            display: inline-block;
            text-align: center;
            padding: 6px 12px;
            border: 1px solid;
        }
        </style>
        <div class="grid">
            <button>Block A</button>
            <button>Block B</button>
        </div>
    )HTML");
    doc.layout(400, 0, &painter);
    doc.draw(painter);

    const auto* a = find_text_draw(painter, "Block A");
    const auto* b = find_text_draw(painter, "Block B");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    CHECK(a->max_width > 180.0f);
    CHECK(b->max_width > 180.0f);
    CHECK(b->pos.y > a->pos.y);
}

TEST_CASE("HTML5 aside participates as a block-level box") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        main { display: flex; width: 180px; height: 40px; }
        aside { flex: 0 0 80px; height: 40px; background: #123456; }
        section { flex: 1; height: 40px; background: #abcdef; }
        </style>
        <main><aside></aside><section></section></main>
    )HTML");
    doc.layout(200, 0, &painter);

    const auto aside_pos = find_hovered_tag(doc, "aside");
    CHECK(aside_pos.x >= 0);
}

TEST_CASE("text-indent shifts first line text origin") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 20px; }
        p { margin: 0; font-size: 16px; line-height: 1.4; }
        .ind { text-indent: 48px; }
        </style>
        <p>plain</p>
        <p class="ind">indented</p>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    const auto* plain = find_text_draw(painter, "plain");
    const auto* ind = find_text_draw(painter, "indented");
    REQUIRE(plain != nullptr);
    REQUIRE(ind != nullptr);

    CHECK(ind->pos.x == plain->pos.x + 48);
}

TEST_CASE("floated form-check input sits in the gutter without squeezing label") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; font-size: 16px; line-height: 1.5; }
        .form-check { display: block; min-height: 24px; padding-left: 24px; }
        .form-check .form-check-input { float: left; margin-left: -24px; }
        .form-check-input {
            display: inline-block;
            width: 16px;
            height: 16px;
            margin-top: 4px;
            background: #0d6efd;
        }
        .form-check-label { display: inline-block; }
        </style>
        <div class="form-check">
          <input class="form-check-input" type="checkbox" checked>
          <label class="form-check-label">Checked checkbox</label>
        </div>
    )HTML");
    doc.layout(260, 0, &painter);
    doc.draw(painter);

    const auto* label = find_text_draw(painter, "Checked checkbox");
    REQUIRE(label != nullptr);

    const RecordingPainter::FillDraw* input_fill = nullptr;
    for (const auto& fill : painter.fill_draws) {
        if (same_color(fill.color, affineui::Color{0x0D, 0x6E, 0xFD, 0xFF})) {
            input_fill = &fill;
            break;
        }
    }
    REQUIRE(input_fill != nullptr);
    CHECK(input_fill->rect.x == 0);
    CHECK(input_fill->rect.y == 4);
    CHECK(input_fill->rect.w == 16);
    CHECK(input_fill->rect.h == 16);

    const int natural_label_w =
        static_cast<int>(std::string_view("Checked checkbox").size()) * 8;
    CHECK(label->pos.x >= 24);
    CHECK(label->max_width >= static_cast<float>(natural_label_w));
}

TEST_CASE("parent opacity applies to anonymous text descendants") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        label { opacity: .5; }
        </style>
        <label>Dimmed label</label>
    )HTML");
    doc.layout(200, 0, &painter);
    doc.draw(painter);

    const auto* label = find_text_draw(painter, "Dimmed label");
    REQUIRE(label != nullptr);
    CHECK(label->alpha == doctest::Approx(0.5f));
}

TEST_CASE("font-family resolves root custom property variables") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        :root {
            --ui-font: system-ui, serif;
            --body-font: var(--ui-font);
        }
        body { margin: 0; padding: 0; font-family: var(--body-font); }
        p { margin: 0; }
        </style>
        <p>Hello</p>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    bool requested_system_ui = false;
    for (const auto& family : painter.font_requests) {
        if (family.find("system-ui") != std::string::npos) {
            requested_system_ui = true;
        }
    }
    CHECK(requested_system_ui);
}

TEST_CASE("font-family preserves browser fallback stacks") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body {
            margin: 0;
            padding: 0;
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
        }
        p { margin: 0; }
        </style>
        <p>Hello</p>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    bool requested_segoe_stack = false;
    for (const auto& family : painter.font_requests) {
        if (family.find("-apple-system") != std::string::npos &&
            family.find("Segoe UI") != std::string::npos &&
            family.find("Roboto") != std::string::npos) {
            requested_segoe_stack = true;
        }
    }
    CHECK(requested_segoe_stack);
}

TEST_CASE("font-family preserves Bootstrap monospace fallback stacks") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body {
            margin: 0;
            padding: 0;
            font-family: sans-serif;
        }
        code {
            font-family: SFMono-Regular, Menlo, Monaco, Consolas,
                         "Liberation Mono", "Courier New", monospace;
        }
        </style>
        <code>$42,600</code>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    bool requested_mono_stack = false;
    for (const auto& family : painter.font_requests) {
        if (family.find("SFMono-Regular") != std::string::npos &&
            family.find("Consolas") != std::string::npos &&
            family.find("monospace") != std::string::npos) {
            requested_mono_stack = true;
        }
    }
    CHECK(requested_mono_stack);
}

TEST_CASE("first child top margin collapses through a plain block parent") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; font-size: 16px; line-height: 16px; }
        .parent { height: 80px; margin: 0; background: #010203; }
        .child { margin: 20px 0 0 0; }
        </style>
        <div class="parent"><div class="child">hit</div></div>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    const auto* parent_bg = find_fill_draw(
        painter, affineui::Color{1, 2, 3, 255});
    REQUIRE(parent_bg != nullptr);
    CHECK(parent_bg->rect.y == 20);

    const auto* draw = find_text_draw(painter, "hit");
    REQUIRE(draw != nullptr);
    CHECK(draw->pos.y == 20);
}

TEST_CASE("adjacent vertical block margins collapse to the larger margin") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; font-size: 16px; line-height: 16px; }
        .before { height: 10px; margin: 0 0 24px 0; }
        .parent { height: 80px; margin: 0; }
        .child { margin: 20px 0 0 0; }
        </style>
        <div class="before"></div>
        <div class="parent"><div class="child">hit</div></div>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    const auto* draw = find_text_draw(painter, "hit");
    REQUIRE(draw != nullptr);
    CHECK(draw->pos.y == 34);
}

TEST_CASE("last child bottom margin collapses through a plain block parent") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>body{margin:0}</style>
        <nav style="margin-bottom:24px">
            <div style="display:flex;width:20px;height:10px;margin-bottom:16px;background:#0d6efd"></div>
        </nav>
        <div style="width:20px;height:10px;background:#198754"></div>
    )HTML");
    doc.layout(320, 200, &painter);
    doc.draw(painter);

    const auto green = affineui::Color::rgb(0x19, 0x87, 0x54);
    bool saw_green_at_collapsed_y = false;
    for (const auto& fill : painter.fill_draws) {
        if (same_color(fill.color, green) && fill.rect.y == 34) {
            saw_green_at_collapsed_y = true;
        }
    }
    CHECK(saw_green_at_collapsed_y);
}

TEST_CASE("dispatch of a no-op event returns a quiescent result") {
    affineui::Document doc;
    affineui::Event ev{};
    auto r = doc.dispatch(ev);
    CHECK(r.redraw_requested == false);
    CHECK(r.invalidate_view == false);
}

TEST_CASE("hover restyle preserves inherited custom properties") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; --surface: #123456; }
        .wrap { display: block; }
        .btn {
            display: block;
            width: 40px;
            height: 20px;
            background-color: var(--surface);
            border: 1px solid transparent;
        }
        .btn:hover { border-color: #ffffff; }
        </style>
        <div class="wrap"><button class="btn"></button></div>
    )HTML");
    doc.layout(120, 80, &painter);

    const auto surface = affineui::Color::rgb(0x12, 0x34, 0x56);
    painter.fill_colors.clear();
    doc.draw(painter);
    REQUIRE(saw_fill(painter, surface));

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {4, 4};
    CHECK(doc.dispatch(move).redraw_requested);

    painter.fill_colors.clear();
    doc.draw(painter);
    CHECK(saw_fill(painter, surface));
}

TEST_CASE("ancestor hover restyles matching descendants") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .card {
            display: block;
            width: 80px;
            height: 50px;
            background-color: #101010;
        }
        .thumb {
            display: block;
            width: 16px;
            height: 16px;
            background-color: #202020;
        }
        .card:hover .thumb { background-color: #445566; }
        </style>
        <div class="card"><div class="thumb"></div></div>
    )HTML");
    doc.layout(120, 80, &painter);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {70, 40};
    CHECK(doc.dispatch(move).redraw_requested);

    painter.fill_colors.clear();
    doc.draw(painter);
    CHECK(saw_fill(painter, affineui::Color::rgb(0x44, 0x55, 0x66)));
}

TEST_CASE("hover overlay resolves var declarations against current scope") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; --hot: #445566; }
        button {
            display: block;
            width: 40px;
            height: 20px;
            background-color: #202020;
            border: 0;
        }
        button:hover { background-color: var(--hot); }
        </style>
        <button></button>
    )HTML");
    doc.layout(120, 80, &painter);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {4, 4};
    CHECK(doc.dispatch(move).redraw_requested);

    painter.fill_colors.clear();
    doc.draw(painter);
    CHECK(saw_fill(painter, affineui::Color::rgb(0x44, 0x55, 0x66)));
}

TEST_CASE("hover color inheritance reaches anonymous button text") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .btn {
            display: inline-block;
            padding: 4px 8px;
            color: #007bff;
            background-color: transparent;
            border: 1px solid #007bff;
        }
        .btn:hover {
            color: #ffffff;
            background-color: #007bff;
        }
        </style>
        <button class="btn">Off</button>
    )HTML");
    doc.layout(120, 80, &painter);

    doc.draw(painter);
    auto* off = find_text_draw(painter, "Off");
    REQUIRE(off != nullptr);
    CHECK(same_color(off->color, affineui::Color::rgb(0x00, 0x7b, 0xff)));

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {8, 8};
    CHECK(doc.dispatch(move).redraw_requested);

    painter.text_draws.clear();
    painter.fill_colors.clear();
    doc.draw(painter);
    off = find_text_draw(painter, "Off");
    REQUIRE(off != nullptr);
    CHECK(same_color(off->color, affineui::Color::rgb(0xff, 0xff, 0xff)));
    CHECK(saw_fill(painter, affineui::Color::rgb(0x00, 0x7b, 0xff)));
}

TEST_CASE("focused button keeps higher-specificity recovered border color") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .btn {
            padding: 8px 16px;
            border-radius: 6px;
            border: 1px solid transparent;
        }
        .btn:focus { border-color: #212529; }
        .btn-primary {
            background-color: #0d6efd;
            color: #ffffff;
            border-color: #0d6efd;
        }
        </style>
        <button class="btn btn-primary">Save</button>
    )HTML");
    doc.layout(320, 0, &painter);

    const auto button_pos = find_hovered_button(doc);
    REQUIRE(button_pos.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = button_pos;
    CHECK(doc.dispatch(down).redraw_requested);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = button_pos;
    doc.dispatch(up);

    painter.stroke_colors.clear();
    doc.draw(painter);
    CHECK(saw_stroke(painter, affineui::Color::rgb(0x21, 0x25, 0x29)));
    CHECK_FALSE(saw_stroke(painter, affineui::Color::rgb(0x0d, 0x6e, 0xfd)));

    affineui::Event esc{};
    esc.type = affineui::EventType::KeyDown;
    esc.key = affineui::Key::Escape;
    CHECK(doc.dispatch(esc).redraw_requested);

    painter.stroke_colors.clear();
    doc.draw(painter);
    CHECK(saw_stroke(painter, affineui::Color::rgb(0x0d, 0x6e, 0xfd)));
}

TEST_CASE("focused button paints parsed box-shadow") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        button {
            display: inline-block;
            padding: 8px 16px;
            border: 1px solid #0d6efd;
            border-radius: 4px;
        }
        button:focus {
            box-shadow: 0 0 0 .25rem rgba(13, 110, 253, .25);
        }
        </style>
        <button>Focus</button>
    )HTML");
    doc.layout(320, 0, &painter);

    const auto button_pos = find_hovered_button(doc);
    REQUIRE(button_pos.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = button_pos;
    CHECK(doc.dispatch(down).redraw_requested);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = button_pos;
    doc.dispatch(up);

    painter.fill_colors.clear();
    doc.draw(painter);
    CHECK(saw_fill(painter, affineui::Color::rgba(13, 110, 253, 64)));
}

TEST_CASE("multi-layer box-shadow paints outer and inset layers") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .panel {
            display: block;
            width: 40px;
            height: 24px;
            background: #222;
            border-radius: 4px;
            box-shadow:
                inset 0 1px 0 rgba(255,255,255,.25),
                0 2px 6px rgba(0,0,0,.4);
        }
        </style>
        <div class="panel"></div>
    )HTML");
    doc.layout(120, 0, &painter);

    painter.shadow_insets.clear();
    doc.draw(painter);

    REQUIRE(painter.shadow_insets.size() == 2);
    CHECK_FALSE(painter.shadow_insets[0]);
    CHECK(painter.shadow_insets[1]);
}

TEST_CASE("inset box-shadow is cast from the padding edge") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .panel {
            display: block;
            width: 40px;
            height: 24px;
            background: #222;
            border: 2px solid #111;
            border-radius: 6px;
            box-shadow:
                inset 0 1px 0 rgba(255,255,255,.25),
                inset 0 -1px 0 rgba(0,0,0,.25);
        }
        </style>
        <div class="panel"></div>
    )HTML");
    doc.layout(120, 0, &painter);

    painter.shadow_draws.clear();
    doc.draw(painter);

    REQUIRE(painter.shadow_draws.size() == 2);
    for (const auto& draw : painter.shadow_draws) {
        CHECK(draw.inset);
        CHECK(draw.rect.x == 2);
        CHECK(draw.rect.y == 2);
        CHECK(draw.rect.w == 40);
        CHECK(draw.rect.h == 24);
        CHECK(draw.radius == doctest::Approx(4.0f));
    }
}

TEST_CASE("focused input accepts text input and backspace") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        input {
            display: inline-block;
            width: 160px;
            padding: 4px 8px;
            border: 1px solid #123456;
        }
        input:focus { border-color: #abcdef; }
        </style>
        <input value="A" placeholder="Name">
    )HTML");
    doc.layout(320, 0, &painter);

    const auto input_pos = find_hovered_tag(doc, "input");
    REQUIRE(input_pos.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = input_pos;
    CHECK(doc.dispatch(down).redraw_requested);

    affineui::Event text{};
    text.type = affineui::EventType::TextInput;
    text.text = "B";
    CHECK(doc.dispatch(text).redraw_requested);

    painter.text_runs.clear();
    doc.draw(painter);
    REQUIRE(!painter.text_runs.empty());
    CHECK(painter.text_runs.back() == "AB");

    affineui::Event backspace{};
    backspace.type = affineui::EventType::KeyDown;
    backspace.key = affineui::Key::Backspace;
    CHECK(doc.dispatch(backspace).redraw_requested);

    painter.text_runs.clear();
    doc.draw(painter);
    REQUIRE(!painter.text_runs.empty());
    CHECK(painter.text_runs.back() == "A");
}

TEST_CASE("overflow clipping uses padding box so borders remain visible") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <div style="width:140px;height:90px;border:2px solid #607d8b;overflow:hidden">
            <div style="width:220px;height:160px;background:#ffe082"></div>
        </div>
    )HTML");
    doc.layout(640, 320, &painter);
    doc.draw(painter);

    REQUIRE(!painter.clip_rects.empty());
    CHECK(painter.clip_rects.front().x == 2);
    CHECK(painter.clip_rects.front().y == 2);
    CHECK(painter.clip_rects.front().w == 140);
    CHECK(painter.clip_rects.front().h == 90);
}

TEST_CASE("rounded overflow clips descendant background corners") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <div style="width:100px;height:20px;border-radius:6px;overflow:hidden;background:#eeeeee">
            <div style="width:40px;height:20px;background:#0d6efd"></div>
        </div>
    )HTML");
    doc.layout(640, 320, &painter);
    doc.draw(painter);

    bool saw_child_left_clip = false;
    for (const auto& draw : painter.rounded_fill_draws) {
        if (draw.rect.w == 40 &&
            same_color(draw.color, affineui::Color::rgb(0x0d, 0x6e, 0xfd))) {
            saw_child_left_clip =
                draw.tl == doctest::Approx(6.0f) &&
                draw.tr == doctest::Approx(0.0f) &&
                draw.br == doctest::Approx(0.0f) &&
                draw.bl == doctest::Approx(6.0f);
        }
    }
    CHECK(saw_child_left_clip);
}

// ── @media query tests ────────────────────────────────────────────────
//
// Verify that min-width / max-width media queries are evaluated against
// the actual layout viewport width and nested style rules are applied
// when the query matches (and ignored when it doesn't).

TEST_CASE("@media min-width matches and applies nested rules") {
    // A blue box becomes red inside @media (min-width: 600px).
    // At viewport 800px the query matches → fill should be red.
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            div { background: #0000ff; width: 100px; height: 100px; }
            @media (min-width: 600px) {
                div { background: #ff0000; }
            }
        </style>
        <div></div>
    )HTML");
    doc.layout(800, 600, &painter);  // 800 >= 600 → @media matches
    doc.draw(painter);

    // Red (#ff0000) should be present; blue (#0000ff) should NOT be
    // the final fill (media rule overrides the base rule).
    CHECK(saw_fill(painter, affineui::Color::rgb(0xff, 0x00, 0x00)));
    CHECK(!saw_fill(painter, affineui::Color::rgb(0x00, 0x00, 0xff)));
}

TEST_CASE("@media min-width does NOT match when viewport is smaller") {
    // Same setup as above, but viewport is 400px < 600px → query fails,
    // the nested rule is NOT applied, and the base blue remains.
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            div { background: #0000ff; width: 100px; height: 100px; }
            @media (min-width: 600px) {
                div { background: #ff0000; }
            }
        </style>
        <div></div>
    )HTML");
    doc.layout(400, 600, &painter);  // 400 < 600 → @media does NOT match
    doc.draw(painter);

    CHECK(saw_fill(painter, affineui::Color::rgb(0x00, 0x00, 0xff)));
    CHECK(!saw_fill(painter, affineui::Color::rgb(0xff, 0x00, 0x00)));
}

TEST_CASE("@media max-width matches when viewport is small enough") {
    // Green rule applies only up to 480px.
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            div { background: #ffffff; width: 100px; height: 100px; }
            @media (max-width: 480px) {
                div { background: #00ff00; }
            }
        </style>
        <div></div>
    )HTML");
    doc.layout(320, 480, &painter);  // 320 <= 480 → @media matches
    doc.draw(painter);

    CHECK(saw_fill(painter, affineui::Color::rgb(0x00, 0xff, 0x00)));
}

TEST_CASE("@media viewport re-evaluated on layout width change") {
    // At 800px viewport, red applies. After re-layout at 400px, blue applies.
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            div { background: #0000ff; width: 100px; height: 100px; }
            @media (min-width: 600px) {
                div { background: #ff0000; }
            }
        </style>
        <div></div>
    )HTML");

    // First layout at 800px → red
    doc.layout(800, 600, &painter);
    painter.fill_colors.clear();
    doc.draw(painter);
    CHECK(saw_fill(painter, affineui::Color::rgb(0xff, 0x00, 0x00)));

    // Second layout at 400px → media no longer matches → blue
    doc.layout(400, 600, &painter);
    painter.fill_colors.clear();
    doc.draw(painter);
    CHECK(saw_fill(painter, affineui::Color::rgb(0x00, 0x00, 0xff)));
    CHECK(!saw_fill(painter, affineui::Color::rgb(0xff, 0x00, 0x00)));
}

TEST_CASE("CSS keyframes make document report active animations") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            @keyframes spin {
                to { transform: rotate(360deg); }
            }
            .box {
                width: 20px;
                height: 20px;
                background: #ff0000;
                animation: 1s linear infinite spin;
            }
        </style>
        <div class="box"></div>
    )HTML");
    doc.layout(200, 200, &painter);

    CHECK(doc.has_active_animations());
}

TEST_CASE("animation none disables matching keyframes") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            @keyframes spin {
                to { transform: rotate(360deg); }
            }
            .box {
                width: 20px;
                height: 20px;
                background: #ff0000;
                animation: 1s linear infinite spin;
                animation: none !important;
            }
        </style>
        <div class="box"></div>
    )HTML");
    doc.layout(200, 200, &painter);

    CHECK(!doc.has_active_animations());
}

TEST_CASE("hover-created animation starts from the hover transition") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            html, body { margin: 0; }
            @keyframes fade {
                to { opacity: 0; }
            }
            .box {
                width: 20px;
                height: 20px;
                background: #ff0000;
            }
            .box:hover {
                animation: 1s linear fade;
            }
        </style>
        <div class="box"></div>
    )HTML");
    doc.layout(200, 200, &painter);
    doc.set_animation_time_for_testing(10.0);
    CHECK(!doc.has_active_animations());

    affineui::Event ev{};
    ev.type = affineui::EventType::MouseMove;
    ev.pos = {1, 1};
    doc.dispatch(ev);

    CHECK(doc.has_active_animations());
}

TEST_CASE("nested transforms compose child local transform before ancestors") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            html, body { margin: 0; }
            .parent {
                width: 100px;
                height: 100px;
                background: #0000ff;
                transform: translate(100px, 0) scale(2);
            }
            .child {
                width: 10px;
                height: 10px;
                background: #ff0000;
                transform: translate(10px, 0);
            }
        </style>
        <div class="parent"><div class="child"></div></div>
    )HTML");
    doc.layout(300, 200, &painter);

    painter.fill_draws.clear();
    doc.draw(painter);

    const RecordingPainter::FillDraw* child_fill = nullptr;
    for (const auto& draw : painter.fill_draws) {
        if (same_color(draw.color, affineui::Color::rgb(0xff, 0x00, 0x00))) {
            child_fill = &draw;
            break;
        }
    }

    REQUIRE(child_fill != nullptr);
    CHECK(child_fill->transform.a == doctest::Approx(2.0f));
    CHECK(child_fill->transform.d == doctest::Approx(2.0f));
    CHECK(child_fill->transform.tx == doctest::Approx(70.0f));
    CHECK(child_fill->transform.ty == doctest::Approx(-50.0f));
}

TEST_CASE("percentage translate resolves against the element box") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            html, body { margin: 0; }
            .thumb {
                width: 12px;
                height: 12px;
                background: #ff0000;
                transform: translate(-50%, -50%);
            }
        </style>
        <div class="thumb"></div>
    )HTML");
    doc.layout(100, 100, &painter);

    painter.fill_draws.clear();
    doc.draw(painter);

    const RecordingPainter::FillDraw* fill = nullptr;
    for (const auto& draw : painter.fill_draws) {
        if (same_color(draw.color, affineui::Color::rgb(0xff, 0x00, 0x00))) {
            fill = &draw;
            break;
        }
    }

    REQUIRE(fill != nullptr);
    CHECK(fill->transform.tx == doctest::Approx(-6.0f));
    CHECK(fill->transform.ty == doctest::Approx(-6.0f));
}

TEST_CASE("hit testing honors CSS transforms") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            html, body { margin: 0; }
            .stage {
                position: relative;
                width: 200px;
                height: 120px;
            }
            .clip {
                position: absolute;
                left: 0;
                top: 0;
                width: 100px;
                height: 30px;
                background: #2f86ee;
            }
            .lane {
                position: absolute;
                left: 0;
                top: 0;
                width: 100px;
                height: 30px;
                transform: translateY(50px);
            }
        </style>
        <div class="stage">
            <div id="top" class="clip"></div>
            <div class="lane"><div id="bottom" class="clip"></div></div>
        </div>
    )HTML");
    doc.layout(200, 120, &painter);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {10, 10};
    doc.dispatch(move);
    CHECK(doc.hovered_info().elem_id == "top");

    move.pos = {10, 60};
    doc.dispatch(move);
    CHECK(doc.hovered_info().elem_id == "bottom");
    CHECK(doc.hovered_info().bounds.y == 50);
}

TEST_CASE("transform-origin changes the transform pivot") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            html, body { margin: 0; }
            .needle {
                position: absolute;
                left: 50px;
                top: 50px;
                width: 2px;
                height: 20px;
                background: #ff0000;
                transform-origin: 50% 100%;
                transform: translate(-50%, -100%) rotate(90deg);
            }
        </style>
        <div class="needle"></div>
    )HTML");
    doc.layout(100, 100, &painter);

    painter.fill_draws.clear();
    doc.draw(painter);

    const RecordingPainter::FillDraw* fill = nullptr;
    for (const auto& draw : painter.fill_draws) {
        if (same_color(draw.color, affineui::Color::rgb(0xff, 0x00, 0x00))) {
            fill = &draw;
            break;
        }
    }

    REQUIRE(fill != nullptr);

    const auto pivot = fill->transform.apply({51.0f, 70.0f});
    CHECK(pivot.x == doctest::Approx(50.0f));
    CHECK(pivot.y == doctest::Approx(50.0f));
}

TEST_CASE("live style mutation restyles descendants and tracks dirty rects") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            html, body { margin: 0; padding: 0; }
            #box {
                position: absolute;
                left: var(--x);
                top: 10px;
                width: 10px;
                height: 10px;
                background: #112233;
            }
        </style>
        <div id="root" style="--x:20px"><div id="box"></div></div>
    )HTML");
    doc.layout(100, 100, &painter);

    painter.fill_draws.clear();
    doc.draw(painter);
    const auto* first = find_fill_draw(
        painter, affineui::Color::rgb(0x11, 0x22, 0x33));
    REQUIRE(first != nullptr);
    CHECK(first->rect.x == 20);
    (void)doc.take_dirty_rects();
    (void)doc.take_paint_dirty();

    REQUIRE(doc.set_attribute_by_id("root", "style", "--x:40px"));
    CHECK(doc.content_size().width == 100);
    CHECK_FALSE(doc.take_paint_dirty());
    doc.layout(100, 100, &painter);

    painter.fill_draws.clear();
    doc.draw(painter);
    const auto* second = find_fill_draw(
        painter, affineui::Color::rgb(0x11, 0x22, 0x33));
    REQUIRE(second != nullptr);
    CHECK(second->rect.x == 40);

    const auto dirty = doc.take_dirty_rects();
    bool saw_old = false;
    bool saw_new = false;
    for (const auto& r : dirty) {
        if (r.x <= 20 && r.x + r.w >= 30) saw_old = true;
        if (r.x <= 40 && r.x + r.w >= 50) saw_new = true;
    }
    CHECK(saw_old);
    CHECK(saw_new);
}

TEST_CASE("live text mutation updates a leaf without reparsing") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
            html, body { margin: 0; padding: 0; }
            #value { display: block; width: 120px; height: 24px; }
        </style>
        <div id="value">1.00</div>
    )HTML");
    doc.layout(160, 80, &painter);
    REQUIRE(doc.set_text_by_id("value", "0.42"));
    doc.layout(160, 80, &painter);

    painter.text_runs.clear();
    doc.draw(painter);
    CHECK(std::find(painter.text_runs.begin(), painter.text_runs.end(),
                    "0.42") != painter.text_runs.end());
    CHECK_FALSE(doc.take_dirty_rects().empty());
}
