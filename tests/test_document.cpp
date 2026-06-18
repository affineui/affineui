#include <doctest/doctest.h>

#include "affineui/automation.h"
#include "affineui/document.h"
#include "affineui/painter.h"
#include "affineui/ui.h"
#include "affineui/view.h"
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

affineui::Rect hovered_bounds_for_id(affineui::Document& doc,
                                     std::string_view elem_id) {
    const auto chain = doc.hovered_info_chain();
    for (const auto& info : chain) {
        if (info.elem_id == elem_id) return info.bounds;
    }
    return {-1, -1, 0, 0};
}

std::string read_test_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ── Document::DockLayout assertions (the dock structure IS the DOM) ─────────
using DockLayoutNode = affineui::Document::DockLayout::Node;

bool dock_leaf_has_tab(const DockLayoutNode& n, std::string_view tab) {
    return !n.split &&
           std::find(n.tabs.begin(), n.tabs.end(), tab) != n.tabs.end();
}

// The (first) LEAF that carries `tab` in its tab row, or nullptr.
const DockLayoutNode* find_dock_leaf(const DockLayoutNode& n,
                                     std::string_view tab) {
    if (!n.split) return dock_leaf_has_tab(n, tab) ? &n : nullptr;
    for (const auto& c : n.children) {
        if (const auto* found = find_dock_leaf(c, tab)) return found;
    }
    return nullptr;
}

// The split node that directly holds `child`, or nullptr (child == root).
const DockLayoutNode* find_dock_parent(const DockLayoutNode& root,
                                       const DockLayoutNode* child) {
    if (!root.split) return nullptr;
    for (const auto& c : root.children) {
        if (&c == child) return &root;
        if (const auto* found = find_dock_parent(c, child)) return found;
    }
    return nullptr;
}

int dock_child_index(const DockLayoutNode& parent, const DockLayoutNode* child) {
    for (std::size_t i = 0; i < parent.children.size(); ++i) {
        if (&parent.children[i] == child) return static_cast<int>(i);
    }
    return -1;
}

std::vector<std::string> dock_tree_tabs(const DockLayoutNode& n) {
    std::vector<std::string> out;
    if (!n.split) return n.tabs;
    for (const auto& c : n.children) {
        auto sub = dock_tree_tabs(c);
        out.insert(out.end(), sub.begin(), sub.end());
    }
    return out;
}

bool dock_tree_has_tab(const DockLayoutNode& n, std::string_view tab) {
    return find_dock_leaf(n, tab) != nullptr;
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

    auto send = [&](affineui::EventType type, affineui::Point pos) {
        affineui::Event e{};
        e.type = type;
        e.button = affineui::MouseButton::Left;
        e.pos = pos;
        return doc.dispatch(e);
    };

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

TEST_CASE("UiControls script toggles Decius target menus") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-menubar__item { display: block; width: 64px; height: 28px; }
        .dcs-menu[hidden] { display: none; }
        .dcs-menu { display: flex; flex-direction: column; width: 132px; }
        .dcs-menu__item { display: block; width: 132px; height: 24px; }
        #cover { display: block; position: fixed; left: 0; top: 28px;
                 width: 132px; height: 24px; }
        </style>
        <button id="file" class="dcs-menubar__item"
                data-dcs-toggle="menu" data-dcs-target="#menu-file">File</button>
        <div id="menu-file" class="dcs-menu" data-aui-name="file-menu" hidden>
            <div id="open-item" class="dcs-menu__item"
                 data-dcs-value="open">Open</div>
            <div id="save-item" class="dcs-menu__item"
                 data-dcs-value="save">Save</div>
        </div>
        <button id="cover">Covered</button>
    )HTML");
    doc.layout(260, 120, &painter);

    auto click_at = [&](affineui::Point p) {
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

    auto file = find_hovered_id(doc, "file", 260, 120);
    REQUIRE(file.x >= 0);
    click_at(file);
    doc.layout(260, 120, &painter);

    file = find_hovered_id(doc, "file", 260, 120);
    REQUIRE(file.x >= 0);
    CHECK(hovered_attr_for_id(doc, "file", "aria-expanded") == "true");

    const auto open = find_hovered_chain_id(doc, "open-item", 260, 120);
    REQUIRE(open.x >= 0);
    click_at(open);
    doc.layout(260, 120, &painter);

    auto changes = doc.take_widget_changes();
    REQUIRE(changes.size() == 1);
    CHECK(changes.front().name == "file-menu");
    CHECK(changes.front().value == "open");

    file = find_hovered_id(doc, "file", 260, 120);
    REQUIRE(file.x >= 0);
    CHECK(hovered_attr_for_id(doc, "file", "aria-expanded") == "false");
    CHECK(find_hovered_chain_id(doc, "open-item", 260, 120).x < 0);
}

TEST_CASE("UiControls script selects Decius menus on down and activates on up") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-menubar__item { display: block; width: 64px; height: 28px; }
        .dcs-menu[hidden] { display: none; }
        .dcs-menu { display: flex; flex-direction: column; width: 132px; }
        .dcs-menu__item { display: block; width: 132px; height: 24px; }
        </style>
        <button id="file" class="dcs-menubar__item"
                data-dcs-toggle="menu" data-dcs-target="#menu-file">File</button>
        <div id="menu-file" class="dcs-menu" data-aui-name="file-menu" hidden>
            <div id="open-item" class="dcs-menu__item"
                 data-dcs-value="open">Open</div>
        </div>
    )HTML");
    doc.layout(260, 120, &painter);

    auto send = [&](affineui::EventType type, affineui::Point p) {
        affineui::Event e{};
        e.type = type;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        return doc.dispatch(e);
    };

    auto file = find_hovered_id(doc, "file", 260, 120);
    REQUIRE(file.x >= 0);
    CHECK(send(affineui::EventType::MouseDown, file).redraw_requested);
    CHECK(doc.take_widget_changes().empty());
    doc.layout(260, 120, &painter);

    file = find_hovered_id(doc, "file", 260, 120);
    REQUIRE(file.x >= 0);
    CHECK(hovered_attr_for_id(doc, "file", "aria-expanded") == "true");
    auto open = find_hovered_chain_id(doc, "open-item", 260, 120);
    REQUIRE(open.x >= 0);

    send(affineui::EventType::MouseUp, file);
    doc.layout(260, 120, &painter);
    file = find_hovered_id(doc, "file", 260, 120);
    REQUIRE(file.x >= 0);
    CHECK(hovered_attr_for_id(doc, "file", "aria-expanded") == "true");

    CHECK(send(affineui::EventType::MouseDown, open).redraw_requested);
    CHECK(doc.take_widget_changes().empty());
    doc.layout(260, 120, &painter);
    open = find_hovered_chain_id(doc, "open-item", 260, 120);
    REQUIRE(open.x >= 0);
    CHECK(hovered_attr_for_id(doc, "open-item", "class").find(
              "dcs-menu__item--active") != std::string::npos);

    CHECK(send(affineui::EventType::MouseUp, open).redraw_requested);
    auto changes = doc.take_widget_changes();
    REQUIRE(changes.size() == 1);
    CHECK(changes.front().name == "file-menu");
    CHECK(changes.front().value == "open");

    doc.layout(260, 120, &painter);
    file = find_hovered_id(doc, "file", 260, 120);
    REQUIRE(file.x >= 0);
    CHECK(hovered_attr_for_id(doc, "file", "aria-expanded") == "false");
    CHECK(find_hovered_chain_id(doc, "open-item", 260, 120).x < 0);
}

TEST_CASE("UiControls script keeps one Decius popup layer open and closes outside") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .bar { display: flex; }
        .dcs-menubar__item { display: block; width: 64px; height: 28px; }
        #outside { display: block; position: absolute; left: 0; top: 92px;
                   width: 180px; height: 28px; }
        .dcs-menu[hidden], .dcs-popover[hidden] { display: none; }
        .dcs-menu { display: flex; flex-direction: column; width: 132px; }
        .dcs-menu__item { display: block; width: 132px; height: 24px; }
        .dcs-popover { display: block; width: 140px; height: 48px; }
        #pop-body { display: block; width: 140px; height: 48px; }
        </style>
        <div class="bar">
            <button id="file" class="dcs-menubar__item"
                    data-dcs-toggle="menu" data-dcs-target="#menu-file">File</button>
            <button id="tweaks" class="dcs-menubar__item"
                    data-dcs-toggle="popover" data-dcs-target="#popover">Tweaks</button>
        </div>
        <div id="menu-file" class="dcs-menu" hidden>
            <div id="open-item" class="dcs-menu__item" data-dcs-value="open">Open</div>
        </div>
        <div id="popover" class="dcs-popover" hidden>
            <div id="pop-body">Theme tweaks</div>
        </div>
        <button id="outside">Outside</button>
    )HTML");
    doc.layout(260, 140, &painter);

    auto click_at = [&](affineui::Point p) {
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

    auto file = find_hovered_id(doc, "file", 260, 140);
    REQUIRE(file.x >= 0);
    click_at(file);
    doc.layout(260, 140, &painter);
    file = find_hovered_id(doc, "file", 260, 140);
    REQUIRE(file.x >= 0);
    CHECK(hovered_attr_for_id(doc, "file", "aria-expanded") == "true");
    CHECK(find_hovered_chain_id(doc, "open-item", 260, 140).x >= 0);

    auto tweaks = find_hovered_id(doc, "tweaks", 260, 140);
    REQUIRE(tweaks.x >= 0);
    click_at(tweaks);
    doc.layout(260, 140, &painter);
    file = find_hovered_id(doc, "file", 260, 140);
    REQUIRE(file.x >= 0);
    CHECK(hovered_attr_for_id(doc, "file", "aria-expanded") == "false");
    tweaks = find_hovered_id(doc, "tweaks", 260, 140);
    REQUIRE(tweaks.x >= 0);
    CHECK(hovered_attr_for_id(doc, "tweaks", "aria-expanded") == "true");
    CHECK(find_hovered_chain_id(doc, "open-item", 260, 140).x < 0);
    CHECK(find_hovered_chain_id(doc, "pop-body", 260, 140).x >= 0);

    const auto outside = find_hovered_id(doc, "outside", 260, 140);
    REQUIRE(outside.x >= 0);
    click_at(outside);
    doc.layout(260, 140, &painter);
    tweaks = find_hovered_id(doc, "tweaks", 260, 140);
    REQUIRE(tweaks.x >= 0);
    CHECK(hovered_attr_for_id(doc, "tweaks", "aria-expanded") == "false");
    CHECK(find_hovered_chain_id(doc, "pop-body", 260, 140).x < 0);
}

TEST_CASE("fixed Decius menus stay under scrolled triggers without changing scroll extents") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #pane { display: block; width: 180px; height: 80px; overflow-y: auto; }
        #spacer { display: block; height: 96px; }
        #file { display: block; width: 80px; height: 24px; }
        #tail { display: block; height: 180px; }
        .dcs-menu[hidden] { display: none; }
        .dcs-menu { display: flex; flex-direction: column; width: 132px; }
        .dcs-menu__item { display: block; width: 132px; height: 24px; }
        </style>
        <div id="pane">
            <div id="spacer"></div>
            <button id="file" class="dcs-menubar__item"
                    data-dcs-toggle="menu"
                    data-dcs-target="#menu-file">File</button>
            <div id="menu-file" class="dcs-menu"
                 data-aui-name="file-menu" hidden>
                <div id="open-item" class="dcs-menu__item"
                     data-dcs-value="open">Open</div>
                <div id="save-item" class="dcs-menu__item"
                     data-dcs-value="save">Save</div>
                <div class="dcs-menu__item">Export</div>
                <div class="dcs-menu__item">Import</div>
                <div class="dcs-menu__item">Recent</div>
                <div class="dcs-menu__item">Close</div>
            </div>
            <div id="tail"></div>
        </div>
    )HTML");
    doc.layout(240, 120, &painter);

    affineui::Event wheel{};
    wheel.type = affineui::EventType::MouseWheel;
    wheel.pos = {10, 10};
    wheel.wheel_dy = -3.0f;
    CHECK(doc.dispatch(wheel).redraw_requested);
    doc.layout(240, 120, &painter);

    auto file = find_hovered_id(doc, "file", 240, 120);
    REQUIRE(file.x >= 0);
    doc.layout(240, 120, &painter);
    file = find_hovered_id(doc, "file", 240, 120);
    REQUIRE(file.x >= 0);
    const auto trigger_bounds = hovered_bounds_for_id(doc, "file");
    REQUIRE(trigger_bounds.y >= 0);
    const auto before_size = doc.content_size();

    auto click_at = [&](affineui::Point p) {
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

    click_at(file);
    doc.layout(240, 120, &painter);
    CHECK(doc.content_size().height == before_size.height);

    const auto open = find_hovered_chain_id(doc, "open-item", 240, 120);
    REQUIRE(open.x >= 0);
    const auto open_bounds = hovered_bounds_for_id(doc, "open-item");
    REQUIRE(open_bounds.y >= 0);
    CHECK(open_bounds.y == trigger_bounds.y + trigger_bounds.h);
}

TEST_CASE("fixed Decius menus flip upward to stay inside the viewport") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #file { display: block; position: absolute; left: 16px; top: 96px;
                width: 80px; height: 24px; }
        .dcs-menu[hidden] { display: none; }
        .dcs-menu { display: flex; flex-direction: column; width: 132px; }
        .dcs-menu__item { display: block; width: 132px; height: 24px; }
        </style>
        <button id="file" class="dcs-menubar__item"
                data-dcs-toggle="menu" data-dcs-target="#menu-file">File</button>
        <div id="menu-file" class="dcs-menu" hidden>
            <div id="open-item" class="dcs-menu__item" data-dcs-value="open">Open</div>
            <div class="dcs-menu__item">Save</div>
            <div class="dcs-menu__item">Export</div>
            <div class="dcs-menu__item">Close</div>
        </div>
    )HTML");
    doc.layout(180, 140, &painter);

    auto file = find_hovered_id(doc, "file", 180, 140);
    REQUIRE(file.x >= 0);
    const auto trigger_bounds = hovered_bounds_for_id(doc, "file");

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = file;
    doc.dispatch(down);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = file;
    doc.dispatch(up);
    doc.layout(180, 140, &painter);

    const auto open = find_hovered_chain_id(doc, "open-item", 180, 140);
    REQUIRE(open.x >= 0);
    const auto item_bounds = hovered_bounds_for_id(doc, "open-item");
    const auto menu_bounds = hovered_bounds_for_id(doc, "menu-file");
    CHECK(item_bounds.y < trigger_bounds.y);
    CHECK(item_bounds.y >= 0);
    CHECK(menu_bounds.y + menu_bounds.h == trigger_bounds.y);
}

TEST_CASE("hidden auto-height Decius color menus flip upward flush to the trigger") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; font-size: 12px; }
        #tint { display: block; position: absolute; left: 40px; top: 160px;
                width: 180px; height: 20px; }
        .dcs-menu[hidden] { display: none; }
        .dcs-menu { display: flex; flex-direction: column; position: fixed;
                    width: 180px; padding: 2px; border: 1px solid #222; }
        .dcs-menu__item { display: flex; align-items: center; height: 20px;
                          width: 100%; box-sizing: border-box; }
        </style>
        <button id="tint" data-dcs-toggle="menu"
                data-dcs-target="#tint-menu">Tint</button>
        <div id="tint-menu" class="dcs-menu aui-color-menu" hidden>
            <button id="color-0" class="dcs-menu__item aui-color-option"
                    value="#33aaff">#33aaff</button>
            <button class="dcs-menu__item aui-color-option"
                    value="#3a3d45">#3a3d45</button>
            <button class="dcs-menu__item aui-color-option"
                    value="#3bb7ff">#3bb7ff</button>
            <button class="dcs-menu__item aui-color-option"
                    value="#3dd68a">#3dd68a</button>
            <button class="dcs-menu__item aui-color-option"
                    value="#8b6dff">#8b6dff</button>
            <button class="dcs-menu__item aui-color-option"
                    value="#cf6b3a">#cf6b3a</button>
            <button class="dcs-menu__item aui-color-option"
                    value="#ff8a3a">#ff8a3a</button>
        </div>
    )HTML");
    doc.layout(260, 190, &painter);

    auto tint = find_hovered_chain_id(doc, "tint", 260, 190);
    REQUIRE(tint.x >= 0);
    const auto trigger_bounds = hovered_bounds_for_id(doc, "tint");

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = tint;
    doc.dispatch(down);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = tint;
    doc.dispatch(up);
    doc.layout(260, 190, &painter);

    const auto first = find_hovered_chain_id(doc, "color-0", 260, 190);
    REQUIRE(first.x >= 0);
    const auto menu_bounds = hovered_bounds_for_id(doc, "tint-menu");
    REQUIRE(menu_bounds.y >= 0);
    CHECK(menu_bounds.y + menu_bounds.h == trigger_bounds.y);
}

TEST_CASE("top Decius popovers anchor to the trigger top edge") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #tweaks { display: block; position: absolute; left: 80px; top: 72px;
                  width: 72px; height: 28px; }
        .dcs-popover[hidden] { display: none; }
        .dcs-popover { display: block; width: 140px; height: 48px; }
        #pop-body { display: block; width: 140px; height: 48px; }
        </style>
        <button id="tweaks" data-dcs-toggle="popover"
                data-dcs-target="#popover" data-dcs-placement="top">
            Tweaks
        </button>
        <div id="popover" class="dcs-popover" hidden>
            <div id="pop-body">Theme tweaks</div>
        </div>
    )HTML");
    doc.layout(260, 140, &painter);

    auto trigger = find_hovered_id(doc, "tweaks", 260, 140);
    REQUIRE(trigger.x >= 0);
    const auto trigger_bounds = hovered_bounds_for_id(doc, "tweaks");

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = trigger;
    doc.dispatch(down);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = trigger;
    doc.dispatch(up);
    doc.layout(260, 140, &painter);

    const auto pop = find_hovered_chain_id(doc, "pop-body", 260, 140);
    REQUIRE(pop.x >= 0);
    const auto popover_bounds = hovered_bounds_for_id(doc, "popover");
    REQUIRE(popover_bounds.y >= 0);
    CHECK(popover_bounds.y + popover_bounds.h == trigger_bounds.y);
}

TEST_CASE("fixed inline geometry restyles against the viewport") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #host {
            display: block;
            position: relative;
            margin-left: 48px;
            margin-top: 36px;
            width: 160px;
            height: 120px;
            background: #222;
        }
        #overlay {
            display: block;
            position: fixed;
            left: 20px;
            top: 30px;
            width: 40px;
            height: 24px;
            background: #4d9fff;
        }
        </style>
        <div id="host">
            <div id="overlay"></div>
        </div>
    )HTML");
    doc.layout(260, 180, &painter);

    auto overlay = find_hovered_id(doc, "overlay", 260, 180);
    REQUIRE(overlay.x >= 0);
    CHECK(doc.hovered_info().bounds.x == 20);
    CHECK(doc.hovered_info().bounds.y == 30);

    REQUIRE(doc.set_attribute_by_id(
        "overlay", "style",
        "display:block;position:fixed;left:80px;top:50px;"
        "width:40px;height:24px;background:#4d9fff"));

    overlay = find_hovered_id(doc, "overlay", 260, 180);
    REQUIRE(overlay.x >= 0);
    CHECK(doc.hovered_info().bounds.x == 80);
    CHECK(doc.hovered_info().bounds.y == 50);
}

TEST_CASE("UiControls script toggles Decius popovers") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #tweaks { display: block; width: 72px; height: 28px; }
        .dcs-popover[hidden] { display: none; }
        .dcs-popover { display: block; width: 140px; height: 48px; }
        #pop-body { display: block; width: 140px; height: 48px; }
        </style>
        <button id="tweaks" data-dcs-toggle="popover"
                data-dcs-target="#popover" data-dcs-placement="bottom-end">
            Tweaks
        </button>
        <div id="popover" class="dcs-popover" hidden>
            <div id="pop-body">Theme tweaks</div>
        </div>
    )HTML");
    doc.layout(260, 140, &painter);

    auto click_at = [&](affineui::Point p) {
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

    auto trigger = find_hovered_id(doc, "tweaks", 260, 140);
    REQUIRE(trigger.x >= 0);
    click_at(trigger);
    doc.layout(260, 140, &painter);

    trigger = find_hovered_id(doc, "tweaks", 260, 140);
    REQUIRE(trigger.x >= 0);
    CHECK(hovered_attr_for_id(doc, "tweaks", "aria-expanded") == "true");
    CHECK(find_hovered_chain_id(doc, "pop-body", 260, 140).x >= 0);

    click_at(trigger);
    doc.layout(260, 140, &painter);
    trigger = find_hovered_id(doc, "tweaks", 260, 140);
    REQUIRE(trigger.x >= 0);
    CHECK(hovered_attr_for_id(doc, "tweaks", "aria-expanded") == "false");
    CHECK(find_hovered_chain_id(doc, "pop-body", 260, 140).x < 0);
}

TEST_CASE("UiControls script maps bounded Decius combo value to bar position") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-combo { display: block; position: relative; width: 100px;
                     height: 24px; margin: 8px; }
        .dcs-combo__fill { display: block; position: absolute; left: 0;
                           top: 0; width: var(--fill, 50%);
                           height: 24px; background: #3dd68a; }
        #combo-value { display: block; width: 50px; height: 24px;
                       margin-left: 50px; }
        </style>
        <div id="combo" class="dcs-combo" data-dcs-combo
             data-min="0" data-max="100" data-step="1"
             data-value="10" style="--fill:10%">
            <div class="dcs-combo__fill"></div>
            <input id="combo-value" class="dcs-combo__value" type="number"
                   value="10" data-aui-name="combo">
        </div>
    )HTML");
    doc.layout(180, 80, &painter);

    const auto input = find_hovered_id(doc, "combo-value", 180, 80);
    REQUIRE(input.x >= 0);
    const auto combo_hit = find_hovered_chain_id(doc, "combo", 180, 80);
    REQUIRE(combo_hit.x >= 0);
    const auto combo_bounds = hovered_bounds_for_id(doc, "combo");
    REQUIRE(combo_bounds.w == 100);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {combo_bounds.x + combo_bounds.w - 10, input.y};
    doc.dispatch(down);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {combo_bounds.x + combo_bounds.w / 2, input.y};
    doc.dispatch(move);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = move.pos;
    doc.dispatch(up);
    doc.layout(180, 80, &painter);

    const auto updated = find_hovered_id(doc, "combo-value", 180, 80);
    REQUIRE(updated.x >= 0);
    const double value =
        std::stod(hovered_attr_for_id(doc, "combo-value", "value"));
    CHECK(value >= 49.0);
    CHECK(value <= 51.0);
    CHECK(hovered_attr_for_id(doc, "combo", "style") == "--fill:50%");
}

TEST_CASE("UiControls script maps generated Decius combo fill range to mouse position") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-combo { display: block; position: relative; width: 100px;
                     height: 24px; margin: 8px; }
        .dcs-combo__fill { display: block; position: absolute; left: 0;
                           top: 0; width: var(--fill, 50%);
                           height: 24px; background: #3dd68a; }
        #combo-value { display: block; width: 50px; height: 24px;
                       margin-left: 50px; }
        </style>
        <div id="combo" class="dcs-combo" data-dcs-combo
             data-value="10" style="--fill:50%">
            <div class="dcs-combo__fill"></div>
            <input id="combo-value" class="dcs-combo__value" type="number"
                   value="10" data-fill-min="9" data-fill-max="11"
                   data-step="0.01" data-aui-name="combo">
        </div>
    )HTML");
    doc.layout(180, 80, &painter);

    const auto input = find_hovered_id(doc, "combo-value", 180, 80);
    REQUIRE(input.x >= 0);
    const auto combo_hit = find_hovered_chain_id(doc, "combo", 180, 80);
    REQUIRE(combo_hit.x >= 0);
    const auto combo_bounds = hovered_bounds_for_id(doc, "combo");
    REQUIRE(combo_bounds.w == 100);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {combo_bounds.x + combo_bounds.w - 10, input.y};
    doc.dispatch(down);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {combo_bounds.x + combo_bounds.w / 2, input.y};
    doc.dispatch(move);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = move.pos;
    doc.dispatch(up);
    doc.layout(180, 80, &painter);

    const auto updated = find_hovered_id(doc, "combo-value", 180, 80);
    REQUIRE(updated.x >= 0);
    const double value =
        std::stod(hovered_attr_for_id(doc, "combo-value", "value"));
    CHECK(value >= 9.99);
    CHECK(value <= 10.01);
    CHECK(hovered_attr_for_id(doc, "combo", "style") == "--fill:50%");
}

TEST_CASE("UiControls script maps raw Decius combo div value to bar position") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #combo { display: block; width: 100px; height: 24px; margin: 8px; }
        </style>
        <div id="combo" data-dcs-combo data-min="0" data-max="100"
             data-step="1" data-value="10" style="--fill:10%"></div>
    )HTML");
    doc.layout(180, 80, &painter);

    auto combo = find_hovered_id(doc, "combo", 180, 80);
    REQUIRE(combo.x >= 0);
    const auto combo_bounds = hovered_bounds_for_id(doc, "combo");
    REQUIRE(combo_bounds.w == 100);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {combo_bounds.x + combo_bounds.w - 10,
                combo_bounds.y + combo_bounds.h / 2};
    doc.dispatch(down);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {combo_bounds.x + combo_bounds.w / 2, down.pos.y};
    doc.dispatch(move);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = move.pos;
    doc.dispatch(up);
    doc.layout(180, 80, &painter);

    combo = find_hovered_id(doc, "combo", 180, 80);
    REQUIRE(combo.x >= 0);
    const double value =
        std::stod(hovered_attr_for_id(doc, "combo", "data-value"));
    CHECK(value >= 49.0);
    CHECK(value <= 51.0);
    CHECK(hovered_attr_for_id(doc, "combo", "style") == "--fill:50%");
}

TEST_CASE("UiControls script toggles raw Decius checks radios and button groups") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-check, .dcs-radio { display: flex; width: 180px; height: 24px; }
        .dcs-check__box { display: block; width: 14px; height: 14px; }
        .dcs-btn-group { display: flex; width: 180px; height: 28px; }
        .dcs-btn { display: block; width: 60px; height: 28px; }
        </style>
        <label id="cast" class="dcs-check" aria-checked="true">
            <span id="cast-box" class="dcs-check__box"></span>
            <span>Cast shadows</span>
        </label>
        <label id="solver-a" class="dcs-radio" data-dcs-name="solver"
               aria-checked="true">
            <span class="dcs-check__box"></span><span>BVH</span>
        </label>
        <label id="solver-b" class="dcs-radio" data-dcs-name="solver"
               aria-checked="false">
            <span class="dcs-check__box"></span><span>Embree</span>
        </label>
        <div id="blend" class="dcs-btn-group" data-aui-name="blend">
            <button id="blend-norm" class="dcs-btn dcs-btn--primary"
                    aria-pressed="true">Norm</button>
            <button id="blend-add" class="dcs-btn">Add</button>
            <button id="blend-mul" class="dcs-btn">Mul</button>
        </div>
    )HTML");
    doc.layout(240, 160, &painter);

    auto click_at = [&](affineui::Point p) {
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

    auto attr = [&](std::string_view id, std::string_view name) {
        const auto p = find_hovered_chain_id(doc, id, 240, 160);
        REQUIRE(p.x >= 0);
        return hovered_attr_for_id(doc, id, name);
    };

    auto cast = find_hovered_chain_id(doc, "cast", 240, 160);
    REQUIRE(cast.x >= 0);
    click_at(cast);
    CHECK(attr("cast", "aria-checked") == "false");

    auto solver_b = find_hovered_chain_id(doc, "solver-b", 240, 160);
    REQUIRE(solver_b.x >= 0);
    click_at(solver_b);
    CHECK(attr("solver-a", "aria-checked") == "false");
    CHECK(attr("solver-b", "aria-checked") == "true");

    auto add = find_hovered_chain_id(doc, "blend-add", 240, 160);
    REQUIRE(add.x >= 0);
    click_at(add);
    CHECK(attr("blend-norm", "aria-pressed") == "false");
    CHECK(attr("blend-add", "aria-pressed") == "true");
    CHECK(attr("blend-norm", "class").find("dcs-btn--primary") ==
          std::string::npos);
    CHECK(attr("blend-add", "class").find("dcs-btn--primary") !=
          std::string::npos);

    auto mul = find_hovered_chain_id(doc, "blend-mul", 240, 160);
    REQUIRE(mul.x >= 0);
    click_at(mul);
    CHECK(attr("blend-norm", "aria-pressed") == "false");
    CHECK(attr("blend-add", "aria-pressed") == "false");
    CHECK(attr("blend-mul", "aria-pressed") == "true");
    CHECK(attr("blend-add", "class").find("dcs-btn--primary") ==
          std::string::npos);
    CHECK(attr("blend-mul", "class").find("dcs-btn--primary") !=
          std::string::npos);

    auto norm = find_hovered_chain_id(doc, "blend-norm", 240, 160);
    REQUIRE(norm.x >= 0);
    click_at(norm);
    CHECK(attr("blend-norm", "aria-pressed") == "true");
    CHECK(attr("blend-add", "aria-pressed") == "false");
    CHECK(attr("blend-mul", "aria-pressed") == "false");
    CHECK(attr("blend-norm", "class").find("dcs-btn--primary") !=
          std::string::npos);
    CHECK(attr("blend-mul", "class").find("dcs-btn--primary") ==
          std::string::npos);

    const auto changes = doc.take_widget_changes();
    REQUIRE_FALSE(changes.empty());
    CHECK(changes.back().name == "blend");
    CHECK(changes.back().value == "Norm");
}

TEST_CASE("UiControls script updates Decius list selection") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-list { display: block; width: 140px; }
        .dcs-list__item { display: block; width: 140px; height: 24px; }
        </style>
        <div id="objects" class="dcs-list" data-dcs-select="multi"
             data-aui-name="objects">
            <div id="row-a" class="dcs-list__item" data-dcs-value="a">Alpha</div>
            <div id="row-b" class="dcs-list__item" data-dcs-value="b">Beta</div>
            <div id="row-c" class="dcs-list__item" data-dcs-value="c">Gamma</div>
        </div>
    )HTML");
    doc.layout(220, 110, &painter);

    auto mouse_down_at = [&](affineui::Point p,
                             bool ctrl = false,
                             bool shift = false) {
        affineui::Event down{};
        down.type = affineui::EventType::MouseDown;
        down.button = affineui::MouseButton::Left;
        down.pos = p;
        down.ctrl = ctrl;
        down.shift = shift;
        return doc.dispatch(down);
    };

    auto mouse_up_at = [&](affineui::Point p,
                           bool ctrl = false,
                           bool shift = false) {
        affineui::Event up{};
        up.type = affineui::EventType::MouseUp;
        up.button = affineui::MouseButton::Left;
        up.pos = p;
        up.ctrl = ctrl;
        up.shift = shift;
        return doc.dispatch(up);
    };

    auto selected = [&](std::string_view id) {
        const auto p = find_hovered_chain_id(doc, id, 220, 110);
        REQUIRE(p.x >= 0);
        return hovered_attr_for_id(doc, id, "aria-selected");
    };

    auto a = find_hovered_chain_id(doc, "row-a", 220, 110);
    REQUIRE(a.x >= 0);
    mouse_down_at(a);
    CHECK(selected("row-a") == "true");
    CHECK(selected("row-b") == "false");
    CHECK(selected("row-c") == "false");
    mouse_up_at(a);

    auto c = find_hovered_chain_id(doc, "row-c", 220, 110);
    REQUIRE(c.x >= 0);
    mouse_down_at(c, true);
    CHECK(selected("row-a") == "true");
    CHECK(selected("row-c") == "true");
    mouse_up_at(c, true);

    auto b = find_hovered_chain_id(doc, "row-b", 220, 110);
    REQUIRE(b.x >= 0);
    mouse_down_at(b, false, true);
    CHECK(selected("row-a") == "false");
    CHECK(selected("row-b") == "true");
    CHECK(selected("row-c") == "true");
    mouse_up_at(b, false, true);

    auto changes = doc.take_widget_changes();
    REQUIRE(changes.size() == 3);
    CHECK(changes.back().name == "objects");
    CHECK(changes.back().value == "b,c");
}

TEST_CASE("UiControls script toggles Decius tree chevrons without selecting rows") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-tree { display: block; width: 180px; }
        .dcs-tree__row { display: flex; align-items: center; height: 24px; }
        .dcs-tree__row[hidden] { display: none; }
        .dcs-tree__chevron { display: block; width: 20px; height: 24px; }
        .dcs-tree__label { flex: 1; }
        </style>
        <div id="scene" class="dcs-tree" data-dcs-select
             data-aui-name="scene">
            <div id="root" class="dcs-tree__row" style="--depth:0">
                <span id="root-chev"
                      class="dcs-tree__chevron dcs-tree__chevron--open"></span>
                <span class="dcs-tree__label">Scene</span>
            </div>
            <div id="camera" class="dcs-tree__row" style="--depth:1">
                <span class="dcs-tree__chevron"></span>
                <span class="dcs-tree__label">Camera</span>
            </div>
        </div>
    )HTML");
    doc.layout(240, 90, &painter);

    auto click_at = [&](affineui::Point p) {
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
        doc.layout(240, 90, &painter);
    };

    auto chev = find_hovered_chain_id(doc, "root-chev", 240, 90);
    REQUIRE(chev.x >= 0);
    click_at(chev);

    chev = find_hovered_chain_id(doc, "root-chev", 240, 90);
    REQUIRE(chev.x >= 0);
    CHECK(hovered_attr_for_id(doc, "root-chev", "class").find(
              "dcs-tree__chevron--open") == std::string::npos);
    CHECK(hovered_attr_for_id(doc, "root", "aria-selected") != "true");
    CHECK(find_hovered_chain_id(doc, "camera", 240, 90).x < 0);

    chev = find_hovered_chain_id(doc, "root-chev", 240, 90);
    REQUIRE(chev.x >= 0);
    click_at(chev);

    chev = find_hovered_chain_id(doc, "root-chev", 240, 90);
    REQUIRE(chev.x >= 0);
    CHECK(hovered_attr_for_id(doc, "root-chev", "class").find(
              "dcs-tree__chevron--open") != std::string::npos);
    CHECK(find_hovered_chain_id(doc, "camera", 240, 90).x >= 0);
}

TEST_CASE("Full decius bundle: horizontal dock splitter reports NS cursor") {
    std::ifstream in(AFFINEUI_TEST_SOURCE_DIR
        "/examples/frameworks/css/decius-css-0.6.2.bundle.min.css",
        std::ios::binary);
    REQUIRE(in.good());
    std::string bundle((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_user_stylesheet(bundle);
    doc.set_html(R"HTML(
      <div class="dcs" data-dcs-density="compact" data-dcs-accent="cyan">
        <div class="dcs-dock dcs-dock--v" style="width:200px;height:120px">
          <div style="flex:0 0 60px"></div>
          <div class="dcs-splitter dcs-splitter--h" data-dcs-splitter="h"></div>
          <div style="flex:1"></div>
        </div>
      </div>
    )HTML");
    doc.layout(220, 140, &painter);
    int got = -99;
    for (int y = 58; y <= 63; ++y) {
        affineui::Event mv{};
        mv.type = affineui::EventType::MouseMove;
        mv.pos = {100, y};
        doc.dispatch(mv);
        const int c = doc.hovered_cursor();
        if (c == 6 || c == 7) { got = c; break; }
    }
    CHECK(got == 7);   // 7 = NS up/down (not 6 = EW left/right)
}

TEST_CASE("Splitter cursor resolves row-resize from the decius rules ALONE "
          "(no inline cursor) — .dcs-splitter--h must beat base col-resize") {
    const char* rules =
        ".dcs-splitter{cursor:col-resize}"
        ".dcs-dock--v>.dcs-splitter,.dcs-splitter--h{cursor:row-resize}"
        ".dcs-dock--v{display:flex;flex-direction:column}"
        ".pane{height:40px}.dcs-splitter--h{height:6px;width:120px}";
    const char* htmlbody = R"HTML(
        <div class="dcs-dock dcs-dock--v">
          <div class="pane"></div>
          <div class="dcs-splitter dcs-splitter--h" data-dcs-splitter="h"></div>
          <div class="pane"></div>
        </div>)HTML";
    // (A) rules in an author <style> block.
    {
        affineui::Document doc;
        RecordingPainter painter;
        doc.set_html(std::string("<style>body{margin:0;padding:0}") + rules +
                     "</style>" + htmlbody);
        doc.layout(140, 100, &painter);
        affineui::Event mv{};
        mv.type = affineui::EventType::MouseMove;
        mv.pos = {60, 43};
        doc.dispatch(mv);
        CHECK(doc.hovered_cursor() == 7);  // author <style> path
    }
    // (B) rules in the USER stylesheet (how the game editor loads decius).
    {
        affineui::Document doc;
        RecordingPainter painter;
        doc.set_user_stylesheet(rules);
        doc.set_html(std::string("<style>body{margin:0;padding:0}</style>") +
                     htmlbody);
        doc.layout(140, 100, &painter);
        affineui::Event mv{};
        mv.type = affineui::EventType::MouseMove;
        mv.pos = {60, 43};
        doc.dispatch(mv);
        CHECK(doc.hovered_cursor() == 7);  // user-stylesheet path
    }
}

TEST_CASE("Horizontal splitter resolves row-resize even with a base "
          ".dcs-splitter{col-resize} rule present (direction, not thinness)") {
    // The decius bundle has `.dcs-splitter{cursor:col-resize}` (base) AND
    // `.dcs-dock--v>.dcs-splitter,.dcs-splitter--h{cursor:row-resize}`. The
    // bottom (horizontal) splitter must end up row-resize (NS, up/down) — the
    // base col-resize must NOT win. (User: "left-right cursor for an up-down
    // splitter".) Inline cursor:row-resize (set by View::splitter) must also win.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>body{margin:0;padding:0}
        .dcs-splitter{cursor:col-resize}
        .dcs-dock--v>.dcs-splitter,.dcs-splitter--h{cursor:row-resize}
        .dcs-dock--v{display:flex;flex-direction:column}
        .pane{height:40px}
        .dcs-splitter--h{height:6px;width:120px}
        </style>
        <div class="dcs-dock dcs-dock--v">
          <div class="pane"></div>
          <div class="dcs-splitter dcs-splitter--h" data-dcs-splitter="h"
               style="cursor:row-resize"></div>
          <div class="pane"></div>
        </div>
    )HTML");
    doc.layout(140, 100, &painter);
    affineui::Event mv{};
    mv.type = affineui::EventType::MouseMove;
    mv.pos = {60, 43};
    doc.dispatch(mv);
    CHECK(doc.hovered_cursor() == 7);  // 7 = NS (up/down), not 6 (EW left/right)
}

TEST_CASE("A row-resize (horizontal dock) splitter reports the NS up/down "
          "cursor on hover") {
    // The bottom-panel splitter is a horizontal bar dragged up/down. View emits
    // it with inline cursor:row-resize + dcs-splitter--h. Hovering it must yield
    // protocol code 7 (RESIZE_NS) so the OS shows the up/down cursor.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>body{margin:0;padding:0}
        .dcs-splitter--h{height:6px;width:120px;cursor:row-resize}</style>
        <div class="dcs-splitter dcs-splitter--h" data-dcs-splitter="h"
             style="cursor:row-resize"></div>
    )HTML");
    doc.layout(140, 40, &painter);
    affineui::Event mv{};
    mv.type = affineui::EventType::MouseMove;
    mv.pos = {60, 3};
    doc.dispatch(mv);
    CHECK(doc.hovered_cursor() == 7);   // 7 = RESIZE_NS (up/down)

    // And a vertical (column) splitter -> EW (left/right) = 6.
    affineui::Document doc2;
    RecordingPainter painter2;
    doc2.set_html(R"HTML(
        <style>body{margin:0;padding:0}
        .dcs-splitter{width:6px;height:120px;cursor:col-resize}</style>
        <div class="dcs-splitter" data-dcs-splitter="v" style="cursor:col-resize"></div>
    )HTML");
    doc2.layout(40, 140, &painter2);
    affineui::Event mv2{};
    mv2.type = affineui::EventType::MouseMove;
    mv2.pos = {3, 60};
    doc2.dispatch(mv2);
    CHECK(doc2.hovered_cursor() == 6);  // 6 = RESIZE_EW (left/right)
}

TEST_CASE("Class mutation re-cascades a child-combinator rule keyed on the "
          "ancestor's compound class") {
    // The exact shape decius uses to collapse a foldout: a rule keyed on a
    // *compound* class (.fold.fold--collapsed) with a child combinator at the
    // descendant body. Toggling the collapsed class on the parent must hide the
    // body purely through the cascade — no element ever sets `hidden`/`display`
    // directly. This isolates the renderer behaviour the foldout relies on.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .fold { display: flex; flex-direction: column; width: 120px; }
        .fold__body { height: 30px; }
        .fold--collapsed > .fold__body { display: none; }
        </style>
        <div id="a" class="fold"><div id="a-body" class="fold__body">A</div></div>
        <div id="b" class="fold fold--collapsed">
            <div id="b-body" class="fold__body">B</div>
        </div>
    )HTML");
    doc.layout(160, 120, &painter);

    // STATIC: the child-combinator rule keyed on the compound ancestor class
    // must already apply at load — 'a' open (body visible), 'b' collapsed
    // (body display:none, so not hit-testable).
    CHECK(find_hovered_chain_id(doc, "a-body", 160, 120).x >= 0);
    CHECK(find_hovered_chain_id(doc, "b-body", 160, 120).x < 0);

    // DYNAMIC (the common foldout round-trip — starts visible): adding the
    // collapsed class to 'a' hides a-body via re-cascade of the descendant
    // rule; removing it again must bring a-body back. a-body kept its box
    // through the collapse, so this exercises the restyle path.
    REQUIRE(doc.set_attribute_by_id("a", "class", "fold fold--collapsed"));
    doc.layout(160, 120, &painter);
    CHECK(find_hovered_chain_id(doc, "a-body", 160, 120).x < 0);
    REQUIRE(doc.set_attribute_by_id("a", "class", "fold"));
    doc.layout(160, 120, &painter);
    CHECK(find_hovered_chain_id(doc, "a-body", 160, 120).x >= 0);

    // DYNAMIC (the reveal case — hidden at load, so it never had a box):
    // removing the collapsed class from 'b' must create b-body's box and show
    // it. This is the path box-collection's display:none skip used to strand.
    REQUIRE(doc.set_attribute_by_id("b", "class", "fold"));
    doc.layout(160, 120, &painter);
    CHECK(find_hovered_chain_id(doc, "b-body", 160, 120).x >= 0);
}

TEST_CASE("Tree icon glyph inherits the selected-row accent (descendant selector "
          "keyed on an ancestor attribute, color inherited to the inner glyph)") {
    // Decius colours tree icons muted normally and accent on the selected row,
    // via `.dcs-tree__row[aria-selected=true] .dcs-tree__icon{color:accent}` on
    // the *span*, with the glyph <i> inheriting that colour. This is the same
    // ancestor-keyed-selector shape the foldout relies on; verify the renderer
    // matches it AND inherits the colour down to the painted glyph.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-tree__row { display: flex; align-items: center; height: 20px; }
        .dcs-tree__icon { color: #808080; }
        .dcs-tree__row[aria-selected=true] .dcs-tree__icon { color: #13b6c8; }
        </style>
        <div class="dcs-tree">
          <div class="dcs-tree__row"><span class="dcs-tree__icon"><i>N</i></span></div>
          <div class="dcs-tree__row" aria-selected="true">
            <span class="dcs-tree__icon"><i>S</i></span>
          </div>
        </div>
    )HTML");
    doc.layout(120, 60, &painter);
    doc.draw(painter);
    // The glyph inherits its colour from the .dcs-tree__icon span: muted on the
    // normal row, accent on the selected row (the ancestor-keyed rule must win).
    const auto* normal = find_text_draw(painter, "N");
    const auto* selected = find_text_draw(painter, "S");
    REQUIRE(normal != nullptr);
    REQUIRE(selected != nullptr);
    CHECK(same_color(normal->color, affineui::Color::rgb(0x80, 0x80, 0x80)));
    CHECK(same_color(selected->color, affineui::Color::rgb(0x13, 0xb6, 0xc8)));
}

TEST_CASE("A custom-property override from an attribute selector resolves through "
          "var() (decius density spacing scale)") {
    // Decius sets its spacing scale on the root and overrides it per density:
    // `[data-dcs-density=compact]{--dcs-s-1:1px}` etc., then consumes it as
    // `gap:var(--dcs-s-1)`. If the attribute-conditional override doesn't win
    // (or var() doesn't pick it up), every compact gap/padding is wrong — which
    // would read as "the vec spacing looks off". Verify the override resolves.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .theme { --gap: 2px; }
        .theme[data-dcs-density=compact] { --gap: 8px; }
        .row { display: flex; gap: var(--gap); width: 100px; }
        .row > * { flex: 1 1 0; min-width: 0; height: 10px; }
        .a { background: #ff0000; } .b { background: #00ff00; }
        </style>
        <div class="theme" data-dcs-density="compact">
          <div class="row"><div class="a"></div><div class="b"></div></div>
        </div>
    )HTML");
    doc.layout(140, 40, &painter);
    doc.draw(painter);
    const auto* a = find_fill_draw(painter, affineui::Color::rgb(0xff, 0, 0));
    const auto* b = find_fill_draw(painter, affineui::Color::rgb(0, 0xff, 0));
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    // The compact override (--gap:8px) must win over the base (2px): 100px row,
    // one 8px gap, two items of (100-8)/2 = 46px each.
    CHECK(a->rect.w == 46);
    CHECK(b->rect.x - (a->rect.x + a->rect.w) == 8);
}

TEST_CASE("Vec channel combos share width evenly with only the declared gap "
          "between them (the inner number input must not bloat the combo)") {
    // The transform/vec widget is a flex row of .dcs-combo cells, each
    // flex:1 1 0 so they share the row equally, separated only by the small
    // `gap`. The cell holds a number <input> (flex:1; min-width:0) that must be
    // allowed to shrink — otherwise the cells bloat and the spacing looks wrong.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-vec { display: flex; gap: 6px; width: 156px; }
        .dcs-vec > .dcs-combo { flex: 1 1 0; min-width: 0; height: 20px;
                                display: inline-flex; align-items: stretch; }
        .dcs-combo__label { flex: 0 0 auto; padding: 0 4px; }
        .dcs-combo__value { flex: 1; min-width: 0; }
        .x { background: #ff0000; } .y { background: #00ff00; } .z { background: #0000ff; }
        </style>
        <div class="dcs-vec">
          <div class="dcs-combo x"><div class="dcs-combo__label">X</div>
            <input class="dcs-combo__value" type="number" value="12"></div>
          <div class="dcs-combo y"><div class="dcs-combo__label">Y</div>
            <input class="dcs-combo__value" type="number" value="4.2"></div>
          <div class="dcs-combo z"><div class="dcs-combo__label">Z</div>
            <input class="dcs-combo__value" type="number" value="-8.5"></div>
        </div>
    )HTML");
    doc.layout(220, 60, &painter);
    doc.draw(painter);
    const auto* x = find_fill_draw(painter, affineui::Color::rgb(0xff, 0, 0));
    const auto* y = find_fill_draw(painter, affineui::Color::rgb(0, 0xff, 0));
    const auto* z = find_fill_draw(painter, affineui::Color::rgb(0, 0, 0xff));
    REQUIRE(x != nullptr);
    REQUIRE(y != nullptr);
    REQUIRE(z != nullptr);
    // 156px row, two 6px gaps = 12px, leaving 144px split three ways = 48px each.
    CHECK(x->rect.w == 48);
    CHECK(y->rect.w == 48);
    CHECK(z->rect.w == 48);
    // The only space between cells is the 6px gap — not a larger spread.
    CHECK(y->rect.x - (x->rect.x + x->rect.w) == 6);
    CHECK(z->rect.x - (y->rect.x + y->rect.w) == 6);
}

TEST_CASE("Clicking a foldout's title TEXT toggles it, and the collapse rule "
          "living in the USER stylesheet re-cascades (the game-editor setup)") {
    // Two game-editor-faithful conditions in one: (1) the header has only a
    // title (no chevron), so the click lands on the title's TEXT block — the
    // matcher must walk up past it to the header (a prior early-return bailed on
    // text clicks, so clicking a foldout title never toggled it); (2) decius is
    // loaded as the USER stylesheet (App::set_stylesheet), so the collapse rule
    // is NOT in the document's <style> — the toggle must re-match against it.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_user_stylesheet(
        ".dcs-foldout{overflow:hidden}"
        ".dcs-foldout__header{display:flex;height:24px}"
        ".dcs-foldout__body{padding:4px}"
        ".dcs-foldout--collapsed>.dcs-foldout__body{display:none}");
    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <div class="dcs-foldout">
          <div id="hdr" class="dcs-foldout__header">
            <span class="dcs-foldout__title">Material</span>
          </div>
          <div class="dcs-foldout__body">
            <div id="field" style="height:22px">Roughness</div>
          </div>
        </div>
    )HTML");
    doc.layout(240, 160, &painter);
    REQUIRE(find_hovered_chain_id(doc, "field", 240, 160).x >= 0);

    auto h = find_hovered_chain_id(doc, "hdr", 240, 160);
    REQUIRE(h.x >= 0);
    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = h;
    doc.dispatch(down);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = h;
    doc.dispatch(up);
    doc.layout(240, 160, &painter);
    CHECK(find_hovered_chain_id(doc, "field", 240, 160).x < 0);
}

TEST_CASE("Foldout with nested body content collapses + expands on header click "
          "(the game-editor inspector shape)") {
    // The game-editor foldout body holds nested .dcs-props > fields (not simple
    // text). Reproduce that exact shape and drive it through real mouse-down
    // clicks (the toggle fires on press) to prove collapse hides the whole
    // nested subtree and expand brings it all back.
    affineui::Document doc;
    RecordingPainter painter;
    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-foldout { overflow: hidden; }
        .dcs-foldout__header { display: flex; align-items: center; height: 24px; }
        .dcs-foldout__chevron { width: 16px; height: 16px; display: flex; }
        .dcs-foldout__title { flex: 1; }
        .dcs-foldout__body { padding: 2px 8px 8px 8px; }
        .dcs-foldout--collapsed > .dcs-foldout__body { display: none; }
        .dcs-props { display: flex; flex-direction: column; gap: 4px; }
        .dcs-field { display: flex; height: 22px; }
        </style>
        <div class="dcs-foldouts">
          <div id="fold" class="dcs-foldout">
            <div id="fold-header" class="dcs-foldout__header">
              <span class="dcs-foldout__chevron dcs-foldout__chevron--open"><i>G</i></span>
              <span class="dcs-foldout__title">Material</span>
            </div>
            <div class="dcs-foldout__body">
              <div class="dcs-props"><div id="field1" class="dcs-field">Roughness</div></div>
            </div>
          </div>
        </div>
    )HTML");
    doc.layout(240, 160, &painter);

    auto press = [&](affineui::Point p) {
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
        doc.layout(240, 160, &painter);
    };

    REQUIRE(find_hovered_chain_id(doc, "field1", 240, 160).x >= 0);   // expanded
    press(find_hovered_chain_id(doc, "fold-header", 240, 160));
    CHECK(find_hovered_chain_id(doc, "field1", 240, 160).x < 0);      // collapsed
    press(find_hovered_chain_id(doc, "fold-header", 240, 160));
    CHECK(find_hovered_chain_id(doc, "field1", 240, 160).x >= 0);     // expanded
}

TEST_CASE("UiControls script toggles Decius foldouts and subpanels") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-subpanel, .dcs-foldout {
          display: flex; flex-direction: column; width: 180px;
        }
        .dcs-subpanel__header, .dcs-foldout__header {
          display: flex; align-items: center; height: 24px;
        }
        .dcs-subpanel__chevron, .dcs-foldout__chevron {
          display: block; width: 18px; height: 24px;
        }
        .dcs-subpanel__body, .dcs-foldout__body { height: 32px; }
        .dcs-subpanel--collapsed .dcs-subpanel__body,
        .dcs-foldout--collapsed > .dcs-foldout__body { display: none; }
        </style>
        <div id="props" class="dcs-subpanel" data-aui-name="props">
            <div id="props-header" class="dcs-subpanel__header">
                <span id="props-chev"
                      class="dcs-subpanel__chevron dcs-subpanel__chevron--open"></span>
                <span>Properties</span>
            </div>
            <div id="props-body" class="dcs-subpanel__body">X Y</div>
        </div>
        <div id="transform" class="dcs-foldout" data-aui-name="transform">
            <div id="transform-header" class="dcs-foldout__header">
                <span id="transform-chev"
                      class="dcs-foldout__chevron dcs-foldout__chevron--open"></span>
                <span>Transform</span>
            </div>
            <div id="transform-body" class="dcs-foldout__body">Scale</div>
        </div>
    )HTML");
    doc.layout(240, 140, &painter);

    auto click_at = [&](affineui::Point p) {
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
        doc.layout(240, 140, &painter);
    };

    // Collapsibles toggle on mouse-DOWN, so the click's own hover-chain
    // snapshot predates the layout that refreshes cached attrs; re-hover the
    // (always-present) header to read the class against the post-click layout.
    auto rehover = [&](const char* id) {
        find_hovered_chain_id(doc, id, 240, 140);
    };

    auto props_header = find_hovered_chain_id(doc, "props-header", 240, 140);
    REQUIRE(props_header.x >= 0);
    click_at(props_header);
    rehover("props-header");
    CHECK(hovered_attr_for_id(doc, "props", "class").find(
              "dcs-subpanel--collapsed") != std::string::npos);
    CHECK(find_hovered_chain_id(doc, "props-body", 240, 140).x < 0);

    props_header = find_hovered_chain_id(doc, "props-header", 240, 140);
    REQUIRE(props_header.x >= 0);
    click_at(props_header);
    rehover("props-header");
    CHECK(hovered_attr_for_id(doc, "props", "class").find(
              "dcs-subpanel--collapsed") == std::string::npos);
    CHECK(find_hovered_chain_id(doc, "props-body", 240, 140).x >= 0);

    auto foldout_header =
        find_hovered_chain_id(doc, "transform-header", 240, 140);
    REQUIRE(foldout_header.x >= 0);
    click_at(foldout_header);
    rehover("transform-header");
    CHECK(hovered_attr_for_id(doc, "transform", "class").find(
              "dcs-foldout--collapsed") != std::string::npos);
    CHECK(find_hovered_chain_id(doc, "transform-body", 240, 140).x < 0);
}

TEST_CASE("Decius radio uses selected accent text color") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-radio {
          display: inline-flex; align-items: center; gap: 6px;
          height: 24px; color: #dce6ff;
        }
        .dcs-check__box {
          display: flex; align-items: center; justify-content: center;
          width: 14px; height: 14px; border-radius: 50%;
          background: #20232b; color: transparent;
        }
        .dcs-radio[aria-checked=true] .dcs-check__box {
          background: #13b6c8; color: #0a1220;
        }
        </style>
        <label id="solver" class="dcs-radio" aria-checked="true">
            <span class="dcs-check__box"></span><span>Embree</span>
        </label>
    )HTML");
    doc.layout(160, 50, &painter);
    doc.draw(painter);

    CHECK(saw_fill(painter, affineui::Color::rgb(0x13, 0xb6, 0xc8)));
    CHECK(saw_fill(painter, affineui::Color::rgb(0x0a, 0x12, 0x20)));

    const auto* label = find_text_draw(painter, "Embree");
    REQUIRE(label != nullptr);
    CHECK(label->pos.y < 20);
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

    auto send = [&](affineui::EventType type, affineui::Point pos) {
        affineui::Event e{};
        e.type = type;
        e.button = affineui::MouseButton::Left;
        e.pos = pos;
        return doc.dispatch(e);
    };

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
    send(affineui::EventType::MouseDown, p);
    CHECK(doc.take_activated_widgets().empty());
    send(affineui::EventType::MouseUp, p);
    auto activations = doc.take_activated_widgets();
    REQUIRE(activations.size() == 1);
    CHECK(activations[0] == "run");

    send(affineui::EventType::MouseDown, p);
    send(affineui::EventType::MouseUp, {170, 70});
    CHECK(doc.take_activated_widgets().empty());

    send(affineui::EventType::MouseDown, {170, 70});
    send(affineui::EventType::MouseUp, p);
    CHECK(doc.take_activated_widgets().empty());
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
    CHECK(same_color(icon->color, affineui::Color::rgb(0x05, 0x07, 0x0d)));
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
    CHECK(same_color(icon->color, affineui::Color::rgb(0x05, 0x07, 0x0d)));
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
    CHECK(same_color(icon->color, affineui::Color::rgb(0x05, 0x07, 0x0d)));
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
    CHECK(same_color(icon->color, affineui::Color::rgb(0x05, 0x07, 0x0d)));
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
    CHECK(same_color(icon->color, affineui::Color::rgb(0x05, 0x07, 0x0d)));
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
    CHECK(same_color(icon->color, affineui::Color::rgb(0x05, 0x07, 0x0d)));
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
                  <div id="mix__fill" class="dcs-slider__fill"></div>
                </div>
              </div>
            </div>
          </div>
        </div>
    )HTML");
    doc.layout(360, 0, &painter);
    doc.draw(painter);

    affineui::Rect track_rect{};
    affineui::Rect fill_rect{};
    bool saw_gradient_at_track_height = false;
    bool saw_track_at_track_height = false;
    for (const auto& draw : painter.rounded_fill_draws) {
        if (same_color(draw.color, affineui::Color::rgb(0x20, 0x23, 0x2b))) {
            CAPTURE(draw.rect.x);
            CAPTURE(draw.rect.y);
            CAPTURE(draw.rect.w);
            CAPTURE(draw.rect.h);
            CHECK(draw.rect.h == 4);
            track_rect = draw.rect;
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
            fill_rect = draw.rect;
            saw_gradient_at_track_height = true;
        }
    }
    CHECK(saw_gradient_at_track_height);
    CHECK(fill_rect.y == track_rect.y + 1);

    REQUIRE(doc.set_attribute_by_id("mix__fill", "style", "width:82%"));
    doc.layout(360, 0, &painter);

    painter.rounded_fill_draws.clear();
    painter.linear_gradient_draws.clear();
    doc.draw(painter);

    bool saw_updated_gradient = false;
    for (const auto& draw : painter.linear_gradient_draws) {
        if (same_color(draw.stop0, affineui::Color::rgb(0x2f, 0x86, 0xee))) {
            CAPTURE(draw.rect.x);
            CAPTURE(draw.rect.y);
            CAPTURE(draw.rect.w);
            CAPTURE(draw.rect.h);
            CHECK(draw.rect.y == fill_rect.y);
            CHECK(draw.rect.h == fill_rect.h);
            saw_updated_gradient = true;
        }
    }
    CHECK(saw_updated_gradient);
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
        .field {
          display: flex;
          align-items: center;
          height: 100px;
        }
        textarea {
          display: inline-flex;
          align-items: center;
          box-sizing: border-box;
          width: 292px;
          height: 80px;
          border: 1px solid #000;
          padding: 6px;
          font-size: 12px;
          line-height: 1.45;
        }
        </style>
        <div class="field">
          <textarea>Dense native UI, browser semantics.</textarea>
        </div>
    )HTML");
    doc.layout(320, 0, &painter);
    doc.draw(painter);

    const auto textarea_pos = find_hovered_tag(doc, "textarea");
    REQUIRE(textarea_pos.x >= 0);
    const auto bounds = doc.hovered_info().bounds;
    const auto* value =
        find_text_draw(painter, "Dense native UI, browser semantics.");
    REQUIRE(value != nullptr);

    CHECK(value->pos.x == bounds.x + 7);
    CHECK(value->pos.y == bounds.y + 12);
}

TEST_CASE("textarea caret paints on the clicked visual line") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        textarea {
          display: block;
          box-sizing: border-box;
          width: 180px;
          height: 80px;
          border: 1px solid #000;
          padding: 6px;
          font-size: 12px;
          line-height: 18px;
          white-space: pre-wrap;
        }
        </style>
        <textarea>alpha
omega</textarea>
    )HTML");
    doc.layout(240, 0, &painter);
    doc.draw(painter);

    const auto textarea_pos = find_hovered_tag(doc, "textarea");
    REQUIRE(textarea_pos.x >= 0);
    const auto bounds = doc.hovered_info().bounds;

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {bounds.x + 8, bounds.y + 32};
    doc.dispatch(down);

    painter.stroke_line_draws.clear();
    doc.draw(painter);

    auto caret = std::find_if(
        painter.stroke_line_draws.begin(),
        painter.stroke_line_draws.end(),
        [&](const RecordingPainter::StrokeLineDraw& line) {
            return std::abs(line.x0 - line.x1) < 0.01f &&
                   line.y0 >= bounds.y + 24;
        });
    REQUIRE(caret != painter.stroke_line_draws.end());
    CHECK(caret->x0 <= bounds.x + 18);
}

TEST_CASE("textarea click focuses multiline editor and inserts at caret") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        textarea {
          display: block;
          box-sizing: border-box;
          width: 180px;
          height: 80px;
          border: 1px solid #000;
          padding: 6px;
          font-size: 12px;
          line-height: 18px;
          white-space: pre-wrap;
        }
        </style>
        <textarea>alpha
omega</textarea>
    )HTML");
    doc.layout(240, 0, &painter);
    doc.draw(painter);

    const auto textarea_pos = find_hovered_tag(doc, "textarea");
    REQUIRE(textarea_pos.x >= 0);
    const auto bounds = doc.hovered_info().bounds;

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {bounds.x + 52, bounds.y + 32};
    doc.dispatch(down);

    affineui::Event text{};
    text.type = affineui::EventType::TextInput;
    text.text = "!";
    CHECK(doc.dispatch(text).redraw_requested);

    painter.text_runs.clear();
    doc.draw(painter);
    CHECK(std::any_of(painter.text_runs.begin(), painter.text_runs.end(),
                      [](const std::string& run) {
                          return run.find('!') != std::string::npos;
                      }));
}

TEST_CASE("textarea caret hit testing follows soft wrapped text") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        textarea {
          display: block;
          box-sizing: border-box;
          width: 50px;
          height: 80px;
          border: 1px solid #000;
          padding: 4px;
          font-size: 12px;
          line-height: 18px;
          white-space: pre-wrap;
        }
        </style>
        <textarea>abcdefghi</textarea>
    )HTML");
    doc.layout(120, 0, &painter);
    doc.draw(painter);

    const auto textarea_pos = find_hovered_tag(doc, "textarea");
    REQUIRE(textarea_pos.x >= 0);
    const auto bounds = doc.hovered_info().bounds;

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {bounds.x + 6, bounds.y + 28};
    doc.dispatch(down);

    affineui::Event text{};
    text.type = affineui::EventType::TextInput;
    text.text = "!";
    CHECK(doc.dispatch(text).redraw_requested);

    painter.text_runs.clear();
    doc.draw(painter);
    CHECK(std::any_of(painter.text_runs.begin(), painter.text_runs.end(),
                      [](const std::string& run) {
                          return run.find("abcde!fghi") !=
                                 std::string::npos;
                      }));
}

TEST_CASE("textarea resize grip updates preferred size within css bounds") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #notes {
          display: block;
          box-sizing: border-box;
          width: 100px;
          height: 40px;
          min-width: 80px;
          max-width: 130px;
          min-height: 30px;
          max-height: 55px;
          resize: both;
          border: 1px solid #000;
          padding: 4px;
        }
        </style>
        <textarea id="notes">Resize me</textarea>
    )HTML");
    doc.layout(240, 0, &painter);

    const auto textarea_pos = find_hovered_id(doc, "notes", 240, 100);
    REQUIRE(textarea_pos.x >= 0);
    const auto before = doc.hovered_info().bounds;
    CHECK(before.w == 100);
    CHECK(before.h == 40);

    affineui::Event hover{};
    hover.type = affineui::EventType::MouseMove;
    hover.pos = {before.x + before.w - 2, before.y + before.h - 2};
    doc.dispatch(hover);
    CHECK(doc.hovered_cursor() == 4);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = hover.pos;
    doc.dispatch(down);

    affineui::Event drag{};
    drag.type = affineui::EventType::MouseMove;
    drag.pos = {hover.pos.x + 80, hover.pos.y + 80};
    CHECK(doc.dispatch(drag).redraw_requested);

    doc.layout(240, 0, &painter);
    const auto during_drag = doc.hovered_info().bounds;
    CHECK(during_drag.w == 130);
    CHECK(during_drag.h == 55);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = drag.pos;
    doc.dispatch(up);

    doc.layout(240, 0, &painter);
    const auto resized_pos = find_hovered_id(doc, "notes", 240, 100);
    REQUIRE(resized_pos.x >= 0);
    const auto after = doc.hovered_info().bounds;
    CHECK(after.w == 130);
    CHECK(after.h == 55);
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

    affineui::Event end{};
    end.type = affineui::EventType::KeyDown;
    end.key = affineui::Key::End;
    doc.dispatch(end);

    affineui::Event text{};
    text.type = affineui::EventType::TextInput;
    text.text = "B";
    CHECK(doc.dispatch(text).redraw_requested);
    doc.layout(320, 0, &painter);

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

TEST_CASE("focused text input paints a caret") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        input {
            display: block;
            width: 160px;
            padding: 4px 8px;
            border: 0;
        }
        </style>
        <input value="Ada">
    )HTML");
    doc.layout(320, 0, &painter);

    const auto input_pos = find_hovered_tag(doc, "input");
    REQUIRE(input_pos.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = input_pos;
    CHECK(doc.dispatch(down).redraw_requested);

    painter.stroke_line_draws.clear();
    doc.draw(painter);
    CHECK_FALSE(painter.stroke_line_draws.empty());
}

TEST_CASE("focused input edits at caret and preserves caret across relayout") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        input {
            display: block;
            width: 160px;
            padding: 4px 8px;
            border: 0;
        }
        </style>
        <input value="abcd">
    )HTML");
    doc.layout(320, 0, &painter);

    const auto input_pos = find_hovered_tag(doc, "input");
    REQUIRE(input_pos.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = input_pos;
    CHECK(doc.dispatch(down).redraw_requested);

    affineui::Event home{};
    home.type = affineui::EventType::KeyDown;
    home.key = affineui::Key::Home;
    doc.dispatch(home);

    affineui::Event text{};
    text.type = affineui::EventType::TextInput;
    text.text = "X";
    CHECK(doc.dispatch(text).redraw_requested);

    affineui::Event right{};
    right.type = affineui::EventType::KeyDown;
    right.key = affineui::Key::ArrowRight;
    CHECK(doc.dispatch(right).redraw_requested);

    affineui::Event del{};
    del.type = affineui::EventType::KeyDown;
    del.key = affineui::Key::Delete;
    CHECK(doc.dispatch(del).redraw_requested);

    doc.layout(320, 0, &painter);

    affineui::Event bang{};
    bang.type = affineui::EventType::TextInput;
    bang.text = "!";
    CHECK(doc.dispatch(bang).redraw_requested);

    painter.text_runs.clear();
    doc.draw(painter);
    REQUIRE_FALSE(painter.text_runs.empty());
    CHECK(painter.text_runs.back() == "Xa!cd");
}

TEST_CASE("text input click placement uses measured glyph advances") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        input {
            display: block;
            width: 160px;
            padding: 0;
            border: 0;
        }
        </style>
        <input value="abcdefgh">
    )HTML");
    doc.layout(320, 0, &painter);

    const auto input_pos = find_hovered_tag(doc, "input");
    REQUIRE(input_pos.x >= 0);
    const auto input_bounds = doc.hovered_info().bounds;

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {input_bounds.x + 24, input_bounds.y + 8};
    CHECK(doc.dispatch(down).redraw_requested);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = down.pos;
    doc.dispatch(up);

    affineui::Event text{};
    text.type = affineui::EventType::TextInput;
    text.text = "X";
    CHECK(doc.dispatch(text).redraw_requested);

    painter.text_runs.clear();
    doc.draw(painter);
    REQUIRE_FALSE(painter.text_runs.empty());
    CHECK(painter.text_runs.back() == "abcXdefgh");
}

TEST_CASE("drag selection replacement edits selected text") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        input {
            display: block;
            width: 160px;
            padding: 0;
            border: 0;
        }
        </style>
        <input value="abcdefgh">
    )HTML");
    doc.layout(320, 0, &painter);

    const auto input_pos = find_hovered_tag(doc, "input");
    REQUIRE(input_pos.x >= 0);
    const auto input_bounds = doc.hovered_info().bounds;

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {input_bounds.x + 16, input_bounds.y + 8};
    doc.dispatch(down);

    affineui::Event drag{};
    drag.type = affineui::EventType::MouseMove;
    drag.pos = {input_bounds.x + 48, input_bounds.y + 8};
    CHECK(doc.dispatch(drag).redraw_requested);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = drag.pos;
    doc.dispatch(up);

    affineui::Event text{};
    text.type = affineui::EventType::TextInput;
    text.text = "X";
    CHECK(doc.dispatch(text).redraw_requested);

    painter.text_runs.clear();
    doc.draw(painter);
    REQUIRE_FALSE(painter.text_runs.empty());
    CHECK(painter.text_runs.back() == "abXgh");
}

TEST_CASE("double click selects a word for replacement") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        input {
            display: block;
            width: 200px;
            padding: 0;
            border: 0;
        }
        </style>
        <input value="red green blue">
    )HTML");
    doc.layout(320, 0, &painter);

    const auto input_pos = find_hovered_tag(doc, "input");
    REQUIRE(input_pos.x >= 0);
    const auto input_bounds = doc.hovered_info().bounds;
    const affineui::Point green_pos{input_bounds.x + 40, input_bounds.y + 8};

    for (int i = 0; i < 2; ++i) {
        affineui::Event down{};
        down.type = affineui::EventType::MouseDown;
        down.button = affineui::MouseButton::Left;
        down.pos = green_pos;
        doc.dispatch(down);

        affineui::Event up{};
        up.type = affineui::EventType::MouseUp;
        up.button = affineui::MouseButton::Left;
        up.pos = green_pos;
        doc.dispatch(up);
    }

    affineui::Event text{};
    text.type = affineui::EventType::TextInput;
    text.text = "cyan";
    CHECK(doc.dispatch(text).redraw_requested);

    painter.text_runs.clear();
    doc.draw(painter);
    REQUIRE_FALSE(painter.text_runs.empty());
    CHECK(painter.text_runs.back() == "red cyan blue");
}

TEST_CASE("focused input supports command selection and clipboard editing") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        input { display: block; width: 200px; padding: 0; border: 0; }
        </style>
        <input value="alpha beta">
    )HTML");
    doc.layout(320, 0, &painter);

    const auto input_pos = find_hovered_tag(doc, "input");
    REQUIRE(input_pos.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = input_pos;
    doc.dispatch(down);

    affineui::Event select_all{};
    select_all.type = affineui::EventType::KeyDown;
    select_all.key = affineui::Key::A;
    select_all.ctrl = true;
    CHECK(doc.dispatch(select_all).redraw_requested);

    affineui::Event copy{};
    copy.type = affineui::EventType::KeyDown;
    copy.key = affineui::Key::C;
    copy.ctrl = true;
    doc.dispatch(copy);

    affineui::Event cut{};
    cut.type = affineui::EventType::KeyDown;
    cut.key = affineui::Key::X;
    cut.ctrl = true;
    CHECK(doc.dispatch(cut).redraw_requested);

    affineui::Event paste{};
    paste.type = affineui::EventType::KeyDown;
    paste.key = affineui::Key::V;
    paste.ctrl = true;
    CHECK(doc.dispatch(paste).redraw_requested);

    painter.text_runs.clear();
    doc.draw(painter);
    REQUIRE_FALSE(painter.text_runs.empty());
    CHECK(painter.text_runs.back() == "alpha beta");
}

TEST_CASE("focused input supports shift selection and word deletion") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        input { display: block; width: 220px; padding: 0; border: 0; }
        </style>
        <input value="red green blue">
    )HTML");
    doc.layout(320, 0, &painter);

    const auto input_pos = find_hovered_tag(doc, "input");
    REQUIRE(input_pos.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = input_pos;
    doc.dispatch(down);

    affineui::Event end{};
    end.type = affineui::EventType::KeyDown;
    end.key = affineui::Key::End;
    doc.dispatch(end);

    affineui::Event word_backspace{};
    word_backspace.type = affineui::EventType::KeyDown;
    word_backspace.key = affineui::Key::Backspace;
    word_backspace.ctrl = true;
    CHECK(doc.dispatch(word_backspace).redraw_requested);

    affineui::Event left{};
    left.type = affineui::EventType::KeyDown;
    left.key = affineui::Key::ArrowLeft;
    left.shift = true;
    CHECK(doc.dispatch(left).redraw_requested);
    CHECK(doc.dispatch(left).redraw_requested);

    affineui::Event text{};
    text.type = affineui::EventType::TextInput;
    text.text = "!!";
    CHECK(doc.dispatch(text).redraw_requested);

    painter.text_runs.clear();
    doc.draw(painter);
    REQUIRE_FALSE(painter.text_runs.empty());
    CHECK(painter.text_runs.back() == "red gree!!");
}

TEST_CASE("scrollable block counts overflow through intermediate descendants") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .viewport {
            width: 160px;
            height: 60px;
            overflow: auto;
        }
        .stack {
            height: 20px;
        }
        .row {
            height: 140px;
        }
        </style>
        <div class="viewport">
            <div class="stack">
                <div class="row">Deep row</div>
            </div>
        </div>
    )HTML");
    doc.layout(200, 80, &painter);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {10, 10};
    doc.dispatch(move);

    affineui::Event wheel{};
    wheel.type = affineui::EventType::MouseWheel;
    wheel.pos = {10, 10};
    wheel.wheel_dy = -3.0f;
    const auto result = doc.dispatch(wheel);

    CHECK(result.redraw_requested);
}

TEST_CASE("vertical scrollbar thumb drag updates scroll position") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .viewport {
            width: 120px;
            height: 80px;
            overflow: auto;
        }
        #row {
            height: 240px;
        }
        </style>
        <div class="viewport">
            <div id="row">Deep row</div>
        </div>
    )HTML");
    doc.layout(160, 100, &painter);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {115, 6};
    (void)doc.dispatch(down);

    affineui::Event drag{};
    drag.type = affineui::EventType::MouseMove;
    drag.pos = {115, 56};
    CHECK(doc.dispatch(drag).redraw_requested);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = drag.pos;
    (void)doc.dispatch(up);

    affineui::Event probe{};
    probe.type = affineui::EventType::MouseMove;
    probe.pos = {10, 10};
    doc.dispatch(probe);

    const auto chain = doc.hovered_info_chain();
    const auto row = std::find_if(
        chain.begin(), chain.end(),
        [](const affineui::Document::HoverInfo& info) {
            return info.elem_id == "row";
        });
    REQUIRE(row != chain.end());
    CHECK(row->bounds.y < 0);
}

TEST_CASE("range and color inputs use native control paint instead of text") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        input { display: block; width: 160px; height: 24px; margin-bottom: 8px; }
        input[type=color] { width: 64px; height: 32px; padding: 4px; border: 0; }
        </style>
        <input type="range" value="0.5">
        <input type="color" value="#4da3ff">
    )HTML");
    doc.layout(320, 0, &painter);

    painter.text_runs.clear();
    painter.fill_draws.clear();
    doc.draw(painter);

    CHECK(std::find(painter.text_runs.begin(), painter.text_runs.end(),
                    "0.5") == painter.text_runs.end());
    const auto swatch = std::find_if(
        painter.fill_draws.begin(), painter.fill_draws.end(),
        [](const RecordingPainter::FillDraw& draw) {
            return draw.color.r == 0x4d && draw.color.g == 0xa3 &&
                   draw.color.b == 0xff && draw.color.a == 0xff;
        });
    CHECK(swatch != painter.fill_draws.end());
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

TEST_CASE("UiControls: dragging a dcs-splitter redistributes pane flex-basis") {
    affineui::Document doc;
    RecordingPainter painter;

    // A horizontal dock: two 200px panes separated by a draggable splitter.
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dock { display: flex; width: 440px; height: 200px; }
        .pane { min-width: 0; }
        .left  { flex: 0 0 200px; }
        .right { flex: 0 0 200px; }
        .split { flex: 0 0 8px; cursor: col-resize; background: #888; }
        </style>
        <div class="dock">
            <div id="left" class="pane left">L</div>
            <div id="split" class="split" data-dcs-splitter="v"></div>
            <div id="right" class="pane right">R</div>
        </div>
    )HTML");
    doc.layout(440, 200, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    // Sanity: initial pane widths.
    auto left_p = find_hovered_id(doc, "left", 440, 200);
    REQUIRE(left_p.x >= 0);
    const int left_w0 = doc.hovered_info().bounds.w;
    CHECK(left_w0 == 200);

    // Grab the splitter and drag right by +40px.
    auto split_p = find_hovered_id(doc, "split", 440, 200);
    REQUIRE(split_p.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = split_p;
    doc.dispatch(down);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {split_p.x + 40, split_p.y};
    doc.dispatch(move);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = {split_p.x + 40, split_p.y};
    const auto result = doc.dispatch(up);
    CHECK_FALSE(result.layout_changed);

    doc.layout(440, 200, &painter);

    // Left pane grew ~40px, right pane shrank ~40px; total budget preserved.
    auto left_after = find_hovered_id(doc, "left", 440, 200);
    REQUIRE(left_after.x >= 0);
    const int left_w1 = doc.hovered_info().bounds.w;
    auto right_after = find_hovered_id(doc, "right", 440, 200);
    REQUIRE(right_after.x >= 0);
    const int right_w1 = doc.hovered_info().bounds.w;

    CHECK(left_w1 == 240);
    CHECK(right_w1 == 160);
    CHECK(left_w1 + right_w1 == 400);
}

TEST_CASE("UiControls: a splitter next to a flex:1 grower never freezes it — "
          "the document keeps filling the window after a drag (regression)") {
    // Regression for the reported game-editor bug: the old splitter wrote
    // `flex:0 0 <px>` onto BOTH siblings, so a drag froze the flex:1
    // center/document at a fixed pixel size. The layout then detached from the
    // far window edge and window-resize stopped resizing the document (a dead
    // band appeared on the right/bottom). The splitter must pin only the FIXED
    // side and leave the grower as flex:1.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dock   { display: flex; width: 100%; height: 200px; }
        .left   { flex: 0 0 200px; min-width: 0; }
        .split  { flex: 0 0 8px; cursor: col-resize; }
        .center { flex: 1; min-width: 0; }
        </style>
        <div class="dock">
            <div id="left" class="left">L</div>
            <div id="split" class="split" data-dcs-splitter="v"></div>
            <div id="center" class="center">C</div>
        </div>
    )HTML");
    doc.layout(440, 200, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    // Initial: left 200, center fills the rest (440 - 200 - 8 = 232).
    find_hovered_id(doc, "center", 440, 200);
    CHECK(doc.hovered_info().bounds.w == 232);

    // Grab the splitter, drag right +40 -> left grows to 240.
    auto split_p = find_hovered_id(doc, "split", 440, 200);
    REQUIRE(split_p.x >= 0);
    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = split_p;
    doc.dispatch(down);
    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {split_p.x + 40, split_p.y};
    doc.dispatch(move);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = {split_p.x + 40, split_p.y};
    const auto result = doc.dispatch(up);
    CHECK_FALSE(result.layout_changed);

    // Same window size: left pinned to 240, center took the remainder.
    doc.layout(440, 200, &painter);
    find_hovered_id(doc, "left", 440, 200);
    CHECK(doc.hovered_info().bounds.w == 240);
    find_hovered_id(doc, "center", 440, 200);
    CHECK(doc.hovered_info().bounds.w == 192);  // 440 - 240 - 8

    // THE REGRESSION CHECK: grow the window by 100px. The center (still flex:1)
    // must absorb all of it; the pinned left stays put. (Old bug: center frozen
    // at 192 -> dead band on the right, document never grows.)
    doc.layout(540, 200, &painter);
    find_hovered_id(doc, "left", 540, 200);
    CHECK(doc.hovered_info().bounds.w == 240);   // pinned, unchanged
    find_hovered_id(doc, "center", 540, 200);
    CHECK(doc.hovered_info().bounds.w == 292);   // 540 - 240 - 8 -> GREW
}

TEST_CASE("UiControls: splitter with the grower on the prev side pins only the "
          "next (fixed) pane and the grower keeps filling the window") {
    // The Inspector case: [center(flex:1) | splitter | right(fixed)]. Dragging
    // must resize the fixed right pane and leave the center growing.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dock   { display: flex; width: 100%; height: 200px; }
        .center { flex: 1; min-width: 0; }
        .split  { flex: 0 0 8px; cursor: col-resize; }
        .right  { flex: 0 0 200px; min-width: 0; }
        </style>
        <div class="dock">
            <div id="center" class="center">C</div>
            <div id="split" class="split" data-dcs-splitter="v"></div>
            <div id="right" class="right">R</div>
        </div>
    )HTML");
    doc.layout(440, 200, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    // Grab the splitter, drag right +40 -> the right pane shrinks to 160.
    auto split_p = find_hovered_id(doc, "split", 440, 200);
    REQUIRE(split_p.x >= 0);
    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = split_p;
    doc.dispatch(down);
    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {split_p.x + 40, split_p.y};
    doc.dispatch(move);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = {split_p.x + 40, split_p.y};
    const auto result = doc.dispatch(up);
    CHECK_FALSE(result.layout_changed);

    doc.layout(440, 200, &painter);
    find_hovered_id(doc, "right", 440, 200);
    CHECK(doc.hovered_info().bounds.w == 160);  // 200 - 40, pinned
    find_hovered_id(doc, "center", 440, 200);
    CHECK(doc.hovered_info().bounds.w == 272);  // 440 - 8 - 160

    // Window grows by 100: the right pane stays pinned, the center absorbs it.
    doc.layout(540, 200, &painter);
    find_hovered_id(doc, "right", 540, 200);
    CHECK(doc.hovered_info().bounds.w == 160);
    find_hovered_id(doc, "center", 540, 200);
    CHECK(doc.hovered_info().bounds.w == 372);  // 540 - 8 - 160 -> GREW
}

TEST_CASE("UiControls: nested two-grow splitter resizes vertically without "
          "requesting dock-size persistence") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock { display: flex; width: 260px; height: 300px; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-height: 0; }
        .pane { flex: 1; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        </style>
        <div class="dcs-dock dcs-dock--v">
          <section id="top" class="dcs-dockpane pane" data-aui-name="pane-Hierarchy"></section>
          <div id="split" class="dcs-splitter dcs-splitter--h"
               data-dcs-splitter="h"></div>
          <section id="bottom" class="dcs-dockpane pane" data-aui-name="pane-Console"></section>
        </div>
    )HTML");
    doc.layout(260, 300, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    auto split_p = find_hovered_id(doc, "split", 260, 300);
    REQUIRE(split_p.x >= 0);
    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = split_p;
    doc.dispatch(down);
    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {split_p.x, split_p.y + 40};
    doc.dispatch(move);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = move.pos;
    const auto result = doc.dispatch(up);

    CHECK_FALSE(result.layout_changed);
    doc.layout(260, 300, &painter);
    find_hovered_id(doc, "top", 260, 300);
    CHECK(doc.hovered_info().bounds.h > 170);
    find_hovered_id(doc, "bottom", 260, 300);
    CHECK(doc.hovered_info().bounds.h < 120);
}

TEST_CASE("UiControls: clicking a dock-pane tab switches the active body") {
    // decius tab semantics: a .dcs-dockpane__tab[data-dcs-target="#body"] click
    // reveals its body and hides the pane's other bodies; aria-selected tracks
    // the active tab.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dockpane { display: flex; flex-direction: column; width: 300px; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 18px; }
        .dcs-dockpane__body { height: 120px; }
        [data-dcs-tabpanel] { height: 100px; }
        [hidden] { display: none; }
        </style>
        <section class="dcs-dockpane">
          <div class="dcs-dockpane__tabbar">
            <div class="dcs-dockpane__tabs">
              <button id="tabA" class="dcs-dockpane__tab" type="button"
                      aria-selected="true" data-dcs-target="#bodyA">A</button>
              <button id="tabB" class="dcs-dockpane__tab" type="button"
                      aria-selected="false" data-dcs-target="#bodyB">B</button>
            </div>
          </div>
          <div class="dcs-dockpane__body">
            <div id="bodyA" data-dcs-tabpanel>Content A</div>
            <div id="bodyB" data-dcs-tabpanel hidden>Content B</div>
          </div>
        </section>
    )HTML");
    doc.layout(300, 200, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    // Initially A is visible, B is hidden.
    CHECK(find_hovered_id(doc, "bodyA", 300, 200).x >= 0);
    CHECK(find_hovered_id(doc, "bodyB", 300, 200).x < 0);

    // Click tab B.
    auto tabB = find_hovered_id(doc, "tabB", 300, 200);
    REQUIRE(tabB.x >= 0);
    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = tabB;
    doc.dispatch(down);
    doc.layout(300, 200, &painter);

    // The switch happens on down, before release.
    CHECK(find_hovered_id(doc, "bodyB", 300, 200).x >= 0);
    CHECK(find_hovered_id(doc, "bodyA", 300, 200).x < 0);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = tabB;
    doc.dispatch(up);
    doc.layout(300, 200, &painter);

    // Now B is visible and A is hidden — the tab switched.
    CHECK(find_hovered_id(doc, "bodyB", 300, 200).x >= 0);
    CHECK(find_hovered_id(doc, "bodyA", 300, 200).x < 0);
}

TEST_CASE("UiControls: dragging a floating element by its handle repositions it "
          "within its drag-bounds") {
    // decius float drag: a [data-dcs-drag] container is moved by a
    // [data-dcs-drag-handle] inside it, writing inline left/top, clamped to the
    // [data-dcs-drag-bounds] container.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .host  { position: relative; width: 400px; height: 300px; }
        .float { position: absolute; left: 10px; top: 10px; width: 60px; height: 40px; }
        .grip  { display: block; width: 60px; height: 14px; }
        </style>
        <div class="host">
          <div id="float" class="float" data-dcs-drag data-dcs-drag-bounds=".host">
            <span id="grip" class="grip" data-dcs-drag-handle></span>
          </div>
        </div>
    )HTML");
    doc.layout(400, 300, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    // The grip sits at the float's top-left (10,10).
    auto grip = find_hovered_id(doc, "grip", 400, 300);
    REQUIRE(grip.x >= 0);
    CHECK(doc.hovered_info().bounds.x == 10);
    CHECK(doc.hovered_info().bounds.y == 10);

    auto press = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };
    // Grab the grip and drag by (+40,+50).
    press(affineui::EventType::MouseDown, grip);
    press(affineui::EventType::MouseMove, {grip.x + 40, grip.y + 50});
    press(affineui::EventType::MouseUp, {grip.x + 40, grip.y + 50});
    doc.layout(400, 300, &painter);

    // The float (hence its grip) moved to top-left (50,60).
    find_hovered_id(doc, "grip", 400, 300);
    CHECK(doc.hovered_info().bounds.x == 50);
    CHECK(doc.hovered_info().bounds.y == 60);

    // Drag far past the bounds — it clamps so the 60x40 float stays inside the
    // 400x300 host (max left 340, max top 260).
    auto grip2 = find_hovered_id(doc, "grip", 400, 300);
    press(affineui::EventType::MouseDown, grip2);
    press(affineui::EventType::MouseMove, {grip2.x + 10000, grip2.y + 10000});
    press(affineui::EventType::MouseUp, {grip2.x + 10000, grip2.y + 10000});
    doc.layout(400, 300, &painter);
    find_hovered_id(doc, "grip", 400, 300);
    CHECK(doc.hovered_info().bounds.x == 340);
    CHECK(doc.hovered_info().bounds.y == 260);
}

TEST_CASE("UiControls: dragging a dock tab OUT of its pane tears it off into a "
          "floating panel; a clean click does not") {
    affineui::Document doc;
    RecordingPainter painter;
    // Canonical workspace: a NON-dock float host wrapping ONE dock holding a
    // 200px panels pane and the documents center pane.
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 400px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        .dcs-panel--floating { position: absolute; }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock">
            <section class="dcs-dockpane" data-aui-name="pane-X" style="flex:0 0 200px">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabX" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#X-body">X</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="X-body" data-dcs-tabpanel>content</div></div>
            </section>
            <section class="dcs-dockpane dcs-dockpane--center" data-aui-name="pane-__document__" style="flex:1 1 0">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Doc-body">Doc</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="Doc-body" data-dcs-tabpanel>doc</div></div>
            </section>
          </div>
        </div>
    )HTML");
    doc.layout(600, 400, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    auto down = [&](affineui::Point p) {
        affineui::Event e{}; e.type = affineui::EventType::MouseDown;
        e.button = affineui::MouseButton::Left; e.pos = p; doc.dispatch(e);
    };
    auto move = [&](affineui::Point p) {
        affineui::Event e{}; e.type = affineui::EventType::MouseMove; e.pos = p;
        doc.dispatch(e);
    };
    auto up = [&](affineui::Point p) {
        affineui::Event e{}; e.type = affineui::EventType::MouseUp;
        e.button = affineui::MouseButton::Left; e.pos = p; doc.dispatch(e);
    };

    // A clean click (press + release in place) must NOT tear off.
    auto tab = find_hovered_id(doc, "tabX", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    up(tab);
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK(layout.floats.empty());
        CHECK(dock_tree_has_tab(layout.root, "X"));
    }

    // A drag dropped in free space (here: over the wrong-kind document pane)
    // tears the panel off into a floating panel; the emptied source pane is
    // removed from the split tree.
    find_hovered_id(doc, "tabX", 600, 400);  // re-hover the tab
    down(tab);
    move({400, 200});   // past threshold, over the documents-kind center pane
    up({400, 200});
    const auto layout = doc.dock_layout();
    REQUIRE(layout.present);
    REQUIRE(layout.floats.size() == 1);
    CHECK(layout.floats[0].pane.tabs == std::vector<std::string>{"X"});
    CHECK(layout.floats[0].title_only == true);
    CHECK(layout.floats[0].w > 0);
    CHECK(layout.floats[0].h > 0);
    CHECK_FALSE(dock_tree_has_tab(layout.root, "X"));
    CHECK(dock_tree_has_tab(layout.root, "Doc"));  // the document pane survives
}

TEST_CASE("UiControls: dropping a dragged tab on another pane's edge zone "
          "splits the target there (DOM surgery, not floating)") {
    affineui::Document doc;
    RecordingPainter painter;
    // Two side-by-side panes (A: x[0,300), B: x[300,600)) in a workspace dock.
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 400px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        .dcs-panel--floating { position: absolute; }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock">
            <section class="dcs-dockpane" data-aui-name="pane-A" style="flex:0 0 300px">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabA" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#A-body">A</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="A-body" data-dcs-tabpanel>A</div></div>
            </section>
            <section class="dcs-dockpane" data-aui-name="pane-B" style="flex:1 1 0">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabB" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#B-body">B</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="B-body" data-dcs-tabpanel>B</div></div>
            </section>
          </div>
        </div>
    )HTML");
    doc.layout(600, 400, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    auto down = [&](affineui::Point p) {
        affineui::Event e{}; e.type = affineui::EventType::MouseDown;
        e.button = affineui::MouseButton::Left; e.pos = p; doc.dispatch(e);
    };
    auto move = [&](affineui::Point p) {
        affineui::Event e{}; e.type = affineui::EventType::MouseMove; e.pos = p;
        doc.dispatch(e);
    };
    auto up = [&](affineui::Point p) {
        affineui::Event e{}; e.type = affineui::EventType::MouseUp;
        e.button = affineui::MouseButton::Left; e.pos = p; doc.dispatch(e);
    };

    // Drag B's tab onto pane A's LEFT edge zone (x≈40 < 22% of 300).
    auto tabB = find_hovered_id(doc, "tabB", 600, 400);
    REQUIRE(tabB.x >= 0);
    down(tabB);
    move({40, 200});
    up({40, 200});

    // A fresh single-tab pane holding B was inserted as A's LEFT sibling in
    // the same horizontal dock; B's emptied source pane is gone. No floats.
    const auto layout = doc.dock_layout();
    REQUIRE(layout.present);
    CHECK(layout.floats.empty());
    REQUIRE(layout.root.split);
    CHECK_FALSE(layout.root.vertical);
    REQUIRE(layout.root.children.size() == 2);
    CHECK_FALSE(layout.root.children[0].split);
    CHECK_FALSE(layout.root.children[1].split);
    CHECK(layout.root.children[0].tabs == std::vector<std::string>{"B"});
    CHECK(layout.root.children[1].tabs == std::vector<std::string>{"A"});
}

TEST_CASE("UiControls: viewport edge docking does not steal a pane edge") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 400px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        .dcs-panel--floating { position: absolute; }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock">
            <section class="dcs-dockpane" data-aui-name="pane-A" style="flex:0 0 300px">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabA" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#A-body">A</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="A-body" data-dcs-tabpanel>A</div></div>
            </section>
            <section class="dcs-dockpane" data-aui-name="pane-B" style="flex:1 1 0">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabB" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#B-body">B</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="B-body" data-dcs-tabpanel>B</div></div>
            </section>
          </div>
        </div>
    )HTML");
    doc.layout(600, 400, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);
    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{}; e.type = t; e.button = affineui::MouseButton::Left;
        e.pos = p; doc.dispatch(e);
    };
    // Drop at x=8: within the 32px window-edge band AND within pane A's left
    // edge zone. The pane edge under the cursor wins, so the fresh pane lands
    // as a direct sibling of A in the WORKSPACE dock (root.children order) —
    // a window-edge split would instead wrap the workspace dock in a new one
    // (root.children[1] would be a nested split, not the A leaf).
    auto tabB = find_hovered_id(doc, "tabB", 600, 400);
    REQUIRE(tabB.x >= 0);
    ev(affineui::EventType::MouseDown, tabB);
    ev(affineui::EventType::MouseMove, {8, 200});   // pane edge wins under cursor
    ev(affineui::EventType::MouseUp, {8, 200});
    const auto layout = doc.dock_layout();
    REQUIRE(layout.present);
    CHECK(layout.floats.empty());
    REQUIRE(layout.root.split);
    CHECK_FALSE(layout.root.vertical);
    REQUIRE(layout.root.children.size() == 2);
    CHECK_FALSE(layout.root.children[0].split);  // leaf, not a wrapped dock
    CHECK_FALSE(layout.root.children[1].split);
    CHECK(layout.root.children[0].tabs == std::vector<std::string>{"B"});
    CHECK(layout.root.children[1].tabs == std::vector<std::string>{"A"});
}

TEST_CASE("UiControls: dock tab drag shows a transient title ghost") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 300px; display: flex; }
        .dcs-dockpane { width: 300px; height: 300px; display: flex; flex-direction: column; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; }
        .dcs-dockpane__tab-ghost {
            position: fixed;
            display: inline-flex;
            height: 24px;
            padding: 0 8px;
            align-items: center;
            color: rgb(255,255,255);
            background: rgb(16,24,32);
        }
        </style>
        <div class="dcs-dock dcs-dock--floathost">
          <section class="dcs-dockpane" data-aui-name="pane-A">
            <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
              <button id="tabA" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#A-body">A</button>
            </div></div><div class="dcs-dockpane__body" id="A-body">A</div>
          </section>
          <section class="dcs-dockpane" data-aui-name="pane-B">
            <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
              <button id="tabB" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#B-body">B</button>
            </div></div><div class="dcs-dockpane__body" id="B-body">B</div>
          </section>
        </div>
    )HTML");
    doc.layout(600, 300, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };

    const auto tab = find_hovered_id(doc, "tabB", 600, 300);
    REQUIRE(tab.x >= 0);
    ev(affineui::EventType::MouseDown, tab);
    ev(affineui::EventType::MouseMove, {90, 190});
    doc.layout(600, 300, &painter);
    painter.text_draws.clear();
    doc.draw(painter);
    const bool saw_ghost_text = std::any_of(
        painter.text_draws.begin(), painter.text_draws.end(),
        [](const RecordingPainter::TextDraw& draw) {
            return draw.text == "B" && draw.pos.x >= 90 && draw.pos.x < 140 &&
                   draw.pos.y >= 190 && draw.pos.y < 235;
        });
    CHECK(saw_ghost_text);

    ev(affineui::EventType::MouseUp, {90, 190});
    doc.layout(600, 300, &painter);
    painter.text_draws.clear();
    doc.draw(painter);
    const bool ghost_gone = std::none_of(
        painter.text_draws.begin(), painter.text_draws.end(),
        [](const RecordingPainter::TextDraw& draw) {
            return draw.text == "B" && draw.pos.x >= 90 && draw.pos.x < 140 &&
                   draw.pos.y >= 190 && draw.pos.y < 235;
        });
    CHECK(ghost_gone);
}

TEST_CASE("UiControls: dock preview uses the topmost element's dock ancestor") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 520px; height: 400px; }
        .dcs-dockpane { position: absolute; display: flex; flex-direction: column; }
        .behind { left: 0; top: 0; width: 300px; height: 360px; z-index: 1; }
        .source { left: 360px; top: 0; width: 130px; height: 140px; z-index: 1; }
        .console-log {
            position: absolute;
            left: 0;
            top: 220px;
            width: 300px;
            height: 160px;
            z-index: 10;
            background: rgb(20,24,32);
        }
        .dcs-dock { position: relative; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; }
        .dcs-panel--floating { position: absolute; }
        .dcs-drop { background: rgb(0,184,212); }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock">
            <section class="dcs-dockpane behind" data-aui-name="pane-Behind">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Behind-body">Behind</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="Behind-body" data-dcs-tabpanel>Behind</div></div>
            </section>
            <section class="dcs-dockpane source" data-aui-name="pane-Source">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabSource" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Source-body">Source</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="Source-body" data-dcs-tabpanel>Source</div></div>
            </section>
          </div>
          <div id="consoleText" class="console-log">Ready</div>
          <div id="__dropind" class="dcs-drop dcs-drop--valid" hidden
               style="position:absolute;pointer-events:none;z-index:200"></div>
        </div>
    )HTML");
    doc.layout(520, 400, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);
    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        return doc.dispatch(e);
    };

    const auto tab = find_hovered_id(doc, "tabSource", 520, 400);
    REQUIRE(tab.x >= 0);
    ev(affineui::EventType::MouseDown, tab);
    // (150,300) is geometrically inside pane Behind, but the TOPMOST element
    // there is the z-raised console overlay, which has no dockpane ancestor —
    // so there is no valid target and no preview.
    ev(affineui::EventType::MouseMove, {150, 300});
    doc.layout(520, 400, &painter);
    painter.fill_colors.clear();
    painter.fill_draws.clear();
    doc.draw(painter);
    CHECK_FALSE(saw_fill(painter, affineui::Color::rgba(0, 184, 212, 46)));

    // Dropping there is a free-space drop: the tab tears off into a float.
    ev(affineui::EventType::MouseUp, {150, 300});
    const auto layout = doc.dock_layout();
    REQUIRE(layout.present);
    REQUIRE(layout.floats.size() == 1);
    CHECK(layout.floats[0].pane.tabs == std::vector<std::string>{"Source"});
    CHECK(layout.floats[0].title_only == true);
    CHECK_FALSE(dock_tree_has_tab(layout.root, "Source"));
    CHECK(dock_tree_has_tab(layout.root, "Behind"));
}

TEST_CASE("UiControls: dock preview zones ignore active body offsets") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost {
            position: relative;
            width: 520px;
            height: 360px;
        }
        .dcs-dockpane {
            position: absolute;
            display: flex;
            flex-direction: column;
        }
        .target {
            left: 20px;
            top: 40px;
            width: 320px;
            height: 220px;
        }
        .source {
            left: 380px;
            top: 20px;
            width: 110px;
            height: 120px;
        }
        .dcs-dockpane__tabbar { flex: 0 0 24px; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; }
        .target > .dcs-dockpane__body {
            position: absolute;
            left: 0;
            right: 0;
            top: 130px;
            height: 80px;
        }
        .dcs-drop { background: rgb(0,184,212); }
        </style>
        <div class="dcs-dock dcs-dock--floathost">
          <section class="dcs-dockpane target" data-aui-name="pane-Target">
            <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
              <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Target-body">Target</button>
            </div></div><div class="dcs-dockpane__body" id="Target-body">Log text</div>
          </section>
          <section class="dcs-dockpane source" data-aui-name="pane-Source">
            <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
              <button id="tabSource" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Source-body">Source</button>
            </div></div><div class="dcs-dockpane__body" id="Source-body">Source</div>
          </section>
          <div id="__dropind" class="dcs-drop dcs-drop--valid" hidden
               style="position:absolute;pointer-events:none;z-index:200"></div>
        </div>
    )HTML");
    doc.layout(520, 360, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);
    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };

    const auto tab = find_hovered_id(doc, "tabSource", 520, 360);
    REQUIRE(tab.x >= 0);
    ev(affineui::EventType::MouseDown, tab);
    ev(affineui::EventType::MouseMove, {120, 190});
    doc.layout(520, 360, &painter);
    painter.fill_colors.clear();
    painter.fill_draws.clear();
    doc.draw(painter);

    const auto* preview =
        find_fill_draw(painter, affineui::Color::rgba(0, 184, 212, 46));
    REQUIRE(preview != nullptr);
    CHECK(preview->rect.x == 20);
    CHECK(preview->rect.y == 40);
    CHECK(preview->rect.w == 320);
    CHECK(preview->rect.h == 220);
}

TEST_CASE("UiControls: floating tearoff targets accept tabs but not edge splits") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 640px; height: 420px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-panel--floating {
            position: absolute;
            left: 300px;
            top: 60px;
            width: 220px;
            height: 180px;
            z-index: 60;
        }
        .dcs-panel--floating > .dcs-dockpane {
            width: 100%;
            height: 100%;
        }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock">
            <section class="dcs-dockpane" data-aui-name="pane-Source" style="flex:0 0 220px">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabSource" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Source-body">Source</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="Source-body" data-dcs-tabpanel>Source</div></div>
            </section>
            <section class="dcs-dockpane dcs-dockpane--center" data-aui-name="pane-__document__" style="flex:1 1 0">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Doc-body">Doc</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="Doc-body" data-dcs-tabpanel>doc</div></div>
            </section>
          </div>
          <section class="dcs-panel dcs-panel--floating" data-dcs-drag
                   data-dcs-drag-bounds=".dcs-dock--floathost" data-dcs-dock-id="Float">
            <section class="dcs-dockpane" data-aui-name="pane-Float">
              <div class="dcs-dockpane__tabbar" data-dcs-drag-handle>
                <div class="dcs-dockpane__tabs">
                  <button id="tabFloat" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Float-body">Float</button>
                </div>
                <div class="dcs-dockpane__toolbars"></div>
              </div>
              <div class="dcs-dockpane__body"><div id="Float-body" data-dcs-tabpanel>Float</div></div>
            </section>
          </section>
        </div>
    )HTML");
    doc.layout(640, 420, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);
    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };

    // Drop near the floating pane's BOTTOM edge: floating panes are always a
    // center/tab target (never an edge split), so the tab JOINS the float's
    // tab row instead of splitting it.
    const auto tab = find_hovered_id(doc, "tabSource", 640, 420);
    REQUIRE(tab.x >= 0);
    ev(affineui::EventType::MouseDown, tab);
    ev(affineui::EventType::MouseMove, {410, 230});
    ev(affineui::EventType::MouseUp, {410, 230});

    const auto layout = doc.dock_layout();
    REQUIRE(layout.present);
    REQUIRE(layout.floats.size() == 1);
    CHECK(layout.floats[0].pane.tabs ==
          std::vector<std::string>{"Float", "Source"});
    CHECK(layout.floats[0].pane.active == "Source");
    CHECK_FALSE(layout.floats[0].title_only);
    // The emptied source pane left the split tree; the document pane remains.
    CHECK_FALSE(dock_tree_has_tab(layout.root, "Source"));
    CHECK(dock_tree_has_tab(layout.root, "Doc"));
}

TEST_CASE("UiControls: co-tab dropped on a pane bottom preview splits out of "
          "its source pane") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 420px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        .dcs-panel--floating { position: absolute; }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock dcs-dock--v">
            <div class="dcs-dock" style="flex:0 0 320px">
              <section class="dcs-dockpane" data-aui-name="pane-Hierarchy" style="flex:0 0 240px">
                <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                  <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Hierarchy-body">Hierarchy</button>
                </div></div>
                <div class="dcs-dockpane__body"><div id="Hierarchy-body" data-dcs-tabpanel>Hierarchy</div></div>
              </section>
              <section class="dcs-dockpane dcs-dockpane--center" data-aui-name="pane-__document__" style="flex:1 1 0">
                <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                  <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Doc-body">Doc</button>
                </div></div>
                <div class="dcs-dockpane__body"><div id="Doc-body" data-dcs-tabpanel>Doc</div></div>
              </section>
            </div>
            <section class="dcs-dockpane" data-aui-name="pane-Assets" style="flex:1 1 0">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabAssets" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Assets-body">Assets</button>
                <button id="tabConsole" class="dcs-dockpane__tab" aria-selected="false" data-dcs-target="#Console-body">Console</button>
              </div></div>
              <div class="dcs-dockpane__body">
                <div id="Assets-body" data-dcs-tabpanel>Assets</div>
                <div id="Console-body" data-dcs-tabpanel hidden>Console</div>
              </div>
            </section>
          </div>
        </div>
    )HTML");
    doc.layout(600, 420, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };
    auto console_tab = find_hovered_id(doc, "tabConsole", 600, 420);
    REQUIRE(console_tab.x >= 0);
    auto hierarchy_p =
        find_hovered_attr(doc, "data-aui-name", "pane-Hierarchy", 600, 420);
    REQUIRE(hierarchy_p.x >= 0);
    affineui::Rect hierarchy_bounds{};
    for (const auto& info : doc.hovered_info_chain()) {
        for (const auto& attr : info.attrs) {
            if (attr.first == "data-aui-name" &&
                attr.second == "pane-Hierarchy") {
                hierarchy_bounds = info.bounds;
            }
        }
    }
    REQUIRE(hierarchy_bounds.h > 0);
    const affineui::Point hierarchy_bottom{
        hierarchy_bounds.x + hierarchy_bounds.w / 2,
        hierarchy_bounds.y + std::max(1, hierarchy_bounds.h - 4)};
    ev(affineui::EventType::MouseDown, console_tab);
    ev(affineui::EventType::MouseMove, hierarchy_bottom);
    ev(affineui::EventType::MouseUp, hierarchy_bottom);

    // Surgery: Console moved into a FRESH pane that splits Hierarchy on its
    // bottom edge (Hierarchy's parent dock is horizontal, so the target gets
    // wrapped in a new vertical dock). Assets keeps its own tab.
    const auto layout = doc.dock_layout();
    REQUIRE(layout.present);
    CHECK(layout.floats.empty());
    const auto* console_leaf = find_dock_leaf(layout.root, "Console");
    REQUIRE(console_leaf != nullptr);
    CHECK(console_leaf->tabs == std::vector<std::string>{"Console"});
    const auto* wrap = find_dock_parent(layout.root, console_leaf);
    REQUIRE(wrap != nullptr);
    CHECK(wrap->vertical);
    REQUIRE(wrap->children.size() == 2);
    CHECK(wrap->children[0].tabs == std::vector<std::string>{"Hierarchy"});
    CHECK(&wrap->children[1] == console_leaf);  // below Hierarchy
    const auto* assets_leaf = find_dock_leaf(layout.root, "Assets");
    REQUIRE(assets_leaf != nullptr);
    CHECK(assets_leaf->tabs == std::vector<std::string>{"Assets"});
    CHECK(assets_leaf->active == "Assets");
}

TEST_CASE("UiControls: dock preview target clears after leaving a valid pane") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 420px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        .dcs-panel--floating { position: absolute; }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock dcs-dock--v">
            <div class="dcs-dock" style="flex:0 0 320px">
              <section class="dcs-dockpane" data-aui-name="pane-Hierarchy" style="flex:0 0 240px">
                <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                  <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Hierarchy-body">Hierarchy</button>
                </div></div>
                <div class="dcs-dockpane__body"><div id="Hierarchy-body" data-dcs-tabpanel>Hierarchy</div></div>
              </section>
              <section class="dcs-dockpane dcs-dockpane--center" data-aui-name="pane-__document__" style="flex:1 1 0">
                <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                  <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Doc-body">Doc</button>
                </div></div>
                <div class="dcs-dockpane__body"><div id="Doc-body" data-dcs-tabpanel>Doc</div></div>
              </section>
            </div>
            <section class="dcs-dockpane" data-aui-name="pane-Assets" style="flex:1 1 0">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabAssets" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Assets-body">Assets</button>
                <button id="tabConsole" class="dcs-dockpane__tab" aria-selected="false" data-dcs-target="#Console-body">Console</button>
              </div></div>
              <div class="dcs-dockpane__body">
                <div id="Assets-body" data-dcs-tabpanel>Assets</div>
                <div id="Console-body" data-dcs-tabpanel hidden>Console</div>
              </div>
            </section>
          </div>
          <div id="__dropind" class="dcs-drop dcs-drop--valid" hidden
               style="position:absolute;pointer-events:none;z-index:200"></div>
        </div>
    )HTML");
    doc.layout(600, 420, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        return doc.dispatch(e);
    };
    auto console_tab = find_hovered_id(doc, "tabConsole", 600, 420);
    REQUIRE(console_tab.x >= 0);
    auto hierarchy_p =
        find_hovered_attr(doc, "data-aui-name", "pane-Hierarchy", 600, 420);
    REQUIRE(hierarchy_p.x >= 0);
    affineui::Rect hierarchy_bounds{};
    for (const auto& info : doc.hovered_info_chain()) {
        for (const auto& attr : info.attrs) {
            if (attr.first == "data-aui-name" &&
                attr.second == "pane-Hierarchy") {
                hierarchy_bounds = info.bounds;
            }
        }
    }
    REQUIRE(hierarchy_bounds.h > 0);
    const affineui::Point hierarchy_bottom{
        hierarchy_bounds.x + hierarchy_bounds.w / 2,
        hierarchy_bounds.y + std::max(1, hierarchy_bounds.h - 4)};
    const affineui::Point document_center{420, 180};
    const auto preview_fill = affineui::Color::rgba(0, 184, 212, 46);

    ev(affineui::EventType::MouseDown, console_tab);
    ev(affineui::EventType::MouseMove, hierarchy_bottom);
    doc.layout(600, 420, &painter);
    painter.fill_colors.clear();
    painter.fill_draws.clear();
    doc.draw(painter);
    CHECK(saw_fill(painter, preview_fill));

    const auto leave_result = ev(affineui::EventType::MouseMove, {-1, -1});
    CHECK(leave_result.redraw_requested);
    doc.layout(600, 420, &painter);
    painter.fill_colors.clear();
    painter.fill_draws.clear();
    doc.draw(painter);
    CHECK_FALSE(saw_fill(painter, preview_fill));

    ev(affineui::EventType::MouseMove, hierarchy_bottom);
    doc.layout(600, 420, &painter);
    painter.fill_colors.clear();
    painter.fill_draws.clear();
    doc.draw(painter);
    CHECK(saw_fill(painter, preview_fill));

    ev(affineui::EventType::MouseMove, document_center);
    doc.layout(600, 420, &painter);
    painter.fill_colors.clear();
    painter.fill_draws.clear();
    doc.draw(painter);
    CHECK_FALSE(saw_fill(painter, preview_fill));

    // Releasing over the wrong-kind document body is a free-space drop: the
    // co-tab tears off into a float; Assets keeps its own tab; nothing docked
    // into (or below) Hierarchy.
    ev(affineui::EventType::MouseUp, document_center);

    const auto layout = doc.dock_layout();
    REQUIRE(layout.present);
    REQUIRE(layout.floats.size() == 1);
    CHECK(layout.floats[0].pane.tabs == std::vector<std::string>{"Console"});
    CHECK(layout.floats[0].title_only == true);
    CHECK_FALSE(dock_tree_has_tab(layout.root, "Console"));
    const auto* hierarchy_leaf = find_dock_leaf(layout.root, "Hierarchy");
    REQUIRE(hierarchy_leaf != nullptr);
    CHECK(hierarchy_leaf->tabs == std::vector<std::string>{"Hierarchy"});
    const auto* assets_leaf = find_dock_leaf(layout.root, "Assets");
    REQUIRE(assets_leaf != nullptr);
    CHECK(assets_leaf->tabs == std::vector<std::string>{"Assets"});
}

TEST_CASE("UiControls: a co-tab dropped on its own source pane tears off "
          "(the source pane is never a target)") {
    // Strict decius.js semantics: releasing a drag back inside the source pane
    // is the Photoshop tearoff gesture. The old "cancel inside the source
    // bounds" and "edge-dock against your own pane" behaviors are gone.
    affineui::Document doc;
    RecordingPainter painter;
    const char* html = R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 260px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        .dcs-panel--floating { position: absolute; }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock">
            <section class="dcs-dockpane" data-aui-name="pane-Assets" style="flex:0 0 300px">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabAssets" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Assets-body">Assets</button>
                <button id="tabConsole" class="dcs-dockpane__tab" aria-selected="false" data-dcs-target="#Console-body">Console</button>
              </div></div>
              <div class="dcs-dockpane__body">
                <div id="Assets-body" data-dcs-tabpanel>Assets</div>
                <div id="Console-body" data-dcs-tabpanel hidden>Console</div>
              </div>
            </section>
            <section class="dcs-dockpane" data-aui-name="pane-Other" style="flex:1 1 0">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Other-body">Other</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="Other-body" data-dcs-tabpanel>Other</div></div>
            </section>
          </div>
          <div id="__dropind" class="dcs-drop dcs-drop--valid" hidden
               style="position:absolute;pointer-events:none;z-index:200"></div>
        </div>
    )HTML";
    doc.set_html(html);
    doc.layout(600, 260, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        return doc.dispatch(e);
    };
    auto check_tearoff = [&]() {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        REQUIRE(layout.floats.size() == 1);
        CHECK(layout.floats[0].pane.tabs ==
              std::vector<std::string>{"Console"});
        CHECK(layout.floats[0].title_only == true);
        CHECK_FALSE(dock_tree_has_tab(layout.root, "Console"));
        // The sibling tab stays docked in the source pane.
        const auto* assets_leaf = find_dock_leaf(layout.root, "Assets");
        REQUIRE(assets_leaf != nullptr);
        CHECK(assets_leaf->tabs == std::vector<std::string>{"Assets"});
        CHECK(dock_tree_has_tab(layout.root, "Other"));
    };

    auto console_tab = find_hovered_id(doc, "tabConsole", 600, 260);
    REQUIRE(console_tab.x >= 0);
    auto assets_p = find_hovered_attr(doc, "data-aui-name", "pane-Assets", 600, 260);
    REQUIRE(assets_p.x >= 0);
    affineui::Rect pane_bounds{};
    for (const auto& info : doc.hovered_info_chain()) {
        for (const auto& attr : info.attrs) {
            if (attr.first == "data-aui-name" && attr.second == "pane-Assets") {
                pane_bounds = info.bounds;
            }
        }
    }
    REQUIRE(pane_bounds.w > 0);

    const affineui::Point source_center{
        pane_bounds.x + pane_bounds.w / 2,
        pane_bounds.y + pane_bounds.h / 2};

    // Drop on the source pane's CENTER: no preview (the source pane is not a
    // target), and the release tears Console off into a float.
    ev(affineui::EventType::MouseDown, console_tab);
    const auto center_move_result =
        ev(affineui::EventType::MouseMove, source_center);
    CHECK(center_move_result.redraw_requested);
    doc.layout(600, 260, &painter);
    painter.fill_colors.clear();
    painter.fill_draws.clear();
    doc.draw(painter);
    CHECK_FALSE(saw_fill(painter, affineui::Color::rgba(0, 184, 212, 46)));
    const auto center_up_result =
        ev(affineui::EventType::MouseUp, source_center);
    CHECK(center_up_result.layout_changed);
    check_tearoff();

    // The source pane's own RIGHT edge band: a MULTI-tab pane's edge zones ARE
    // valid for its own tab (the "split Console out of Assets" gesture —
    // upstreamed to decius.js dockDropDecision as well). The drop splits the
    // pane: Console gets a fresh pane to the right of Assets.
    doc.set_html(html);
    doc.layout(600, 260, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);
    console_tab = find_hovered_id(doc, "tabConsole", 600, 260);
    REQUIRE(console_tab.x >= 0);

    const affineui::Point source_right_zone{
        pane_bounds.x + pane_bounds.w - std::max(2, pane_bounds.w / 20),
        pane_bounds.y + pane_bounds.h / 2};

    ev(affineui::EventType::MouseDown, console_tab);
    const auto move_result =
        ev(affineui::EventType::MouseMove, source_right_zone);
    CHECK(move_result.redraw_requested);
    ev(affineui::EventType::MouseUp, source_right_zone);
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK(layout.floats.empty());
        const auto* assets_leaf = find_dock_leaf(layout.root, "Assets");
        REQUIRE(assets_leaf != nullptr);
        CHECK(assets_leaf->tabs == std::vector<std::string>{"Assets"});
        const auto* console_leaf = find_dock_leaf(layout.root, "Console");
        REQUIRE(console_leaf != nullptr);
        CHECK(console_leaf->tabs == std::vector<std::string>{"Console"});
        // Console sits in the same split as Assets, AFTER it (right edge).
        const auto* parent = find_dock_parent(layout.root, console_leaf);
        REQUIRE(parent != nullptr);
        CHECK(dock_child_index(*parent, assets_leaf) >= 0);
        CHECK(dock_child_index(*parent, console_leaf) >
              dock_child_index(*parent, assets_leaf));
        CHECK(dock_tree_has_tab(layout.root, "Other"));
    }
}

TEST_CASE("UiControls: View dock tab switches from Assets to Console") {
    constexpr int W = 520;
    constexpr int H = 260;
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_user_stylesheet(R"CSS(
        html, body { width: 520px; height: 260px; margin: 0; padding: 0; }
        #aui-root { width: 520px; height: 260px; min-height: 0; padding: 0; }
        .shell { width: 520px; height: 260px; display: flex; flex-direction: column; }
        .dcs-dock { display: flex; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dock--floathost { position: relative; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane--center { flex: 1; }
        .dcs-dockpane__tabbar { flex: 0 0 24px; display: flex; min-width: 0; }
        .dcs-dockpane__tabs { display: flex; min-width: 0; }
        .dcs-dockpane__tab { display: inline-flex; padding: 0 12px; white-space: nowrap; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        [hidden] { display: none; }
    )CSS");

    auto html = [&]() {
        affineui::View v{affineui::ViewTheme::Decius};
        v.set_dock_active_tab_provider([&](std::string_view id) {
            return doc.dock_active_tab(id);
        });
        v.begin();
        {
            auto shell = v.container("shell", "shell");
            (void) shell;
            v.document_view("workarea", [&](affineui::View& dv) {
                dv.document([](affineui::View& p) { p.text("Viewport", "viewport-text"); },
                            "View", "cube");
                auto assets = dv.dockpanel(
                    "Assets",
                    affineui::DockLocation::docked(affineui::Dock::Bottom, 90),
                    [](affineui::View& p) { p.text("Asset body", "assets-text"); },
                    "image", "Assets");
                dv.dockpanel(
                    "Console", affineui::DockLocation::tab().in(assets),
                    [](affineui::View& p) { p.text("Console body", "console-text"); },
                    "file", "Console");
            });
        }
        v.end();
        return v.to_html_document();
    };
    auto rebuild = [&]() {
        doc.set_html(html());
        doc.attach_script(affineui::DocumentScript::UiControls);
        doc.layout(W, H, &painter);
    };

    const auto initial_html = html();
    CHECK(initial_html.find("Asset body") != std::string::npos);
    CHECK(initial_html.find("Console body") == std::string::npos);
    doc.set_html(initial_html);
    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.layout(W, H, &painter);

    CHECK(find_hovered_attr(doc, "data-aui-name", "Assets-body", W, H).x >= 0);
    CHECK(find_hovered_attr(doc, "data-aui-name", "Console-body", W, H).x < 0);
    const auto tab = find_hovered_attr(doc, "data-dcs-target",
                                       "#Console-body", W, H);
    REQUIRE(tab.x >= 0);

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = tab;
    const auto down_result = doc.dispatch(down);
    CHECK(down_result.layout_changed);
    CHECK(down_result.invalidate_view);
    doc.layout(W, H, &painter);
    CHECK(doc.dock_active_tab("Assets") == "Console");

    // The app rebuilds immediately from the down event, so the newly selected
    // panel creates its DOM before the mouse button is released.
    const auto active_html = html();
    CHECK(active_html.find("Console body") != std::string::npos);
    CHECK(active_html.find("Asset body") == std::string::npos);
    rebuild();
    painter.text_draws.clear();
    doc.draw(painter);
    CHECK(find_text_draw(painter, "Console body") != nullptr);
    CHECK(find_text_draw(painter, "Asset body") == nullptr);

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = tab;
    const auto up_result = doc.dispatch(up);
    CHECK_FALSE(up_result.layout_changed);
}

TEST_CASE("UiScript: bottom pane docks to the bottom of Hierarchy and back "
          "again, repeatedly (in-window crash sequence)") {
    // The exact gesture sequence reported crashing in the windowed editor:
    // drag the bottom pane's tab to the BOTTOM of Hierarchy (vertical split
    // under it), then back to the bottom area, several times — with the
    // editor's reload-after-every-layout-change loop in play.
    constexpr int W = 640;
    constexpr int H = 360;
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_user_stylesheet(R"CSS(
        html, body { width: 640px; height: 360px; margin: 0; padding: 0; }
        #aui-root { width: 640px; height: 360px; min-height: 0; padding: 0; }
        .shell { width: 640px; height: 360px; display: flex; flex-direction: column; }
        .dcs-dock { display: flex; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dock--floathost { position: relative; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane--center { flex: 1; }
        .dcs-dockpane__tabbar { flex: 0 0 24px; display: flex; min-width: 0; }
        .dcs-dockpane__tabs { display: flex; min-width: 0; }
        .dcs-dockpane__tab { display: inline-flex; padding: 0 12px; white-space: nowrap; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        [hidden] { display: none; }
    )CSS");

    auto rebuild = [&]() {
        affineui::View v{affineui::ViewTheme::Decius};
        v.set_dock_layout_provider([&] { return doc.dock_layout(); });
        v.begin();
        {
            auto shell = v.container("shell", "shell");
            (void) shell;
            v.document_view("workarea", [&](affineui::View& dv) {
                dv.document(
                    [](affineui::View& p) { p.text("Viewport", "vp-text"); },
                    "View", "cube");
                dv.dockpanel(
                    "Hierarchy",
                    affineui::DockLocation::docked(affineui::Dock::Left, 180),
                    [](affineui::View& p) { p.text("H body", "h-text"); },
                    "layers", "Hierarchy");
                dv.dockpanel(
                    "Assets",
                    affineui::DockLocation::docked(affineui::Dock::Bottom, 90),
                    [](affineui::View& p) { p.text("A body", "a-text"); },
                    "image", "Assets");
            });
        }
        v.end();
        doc.set_html(v.to_html_document());
        doc.attach_script(affineui::DocumentScript::UiControls);
        doc.layout(W, H, &painter);
    };
    rebuild();

    affineui::UiScript ui(doc, W, H, &painter);
    ui.set_step_hook([&](const affineui::DispatchResult& r) {
        if (r.layout_changed) rebuild();
    });
    const std::vector<std::string> all = {"__document__", "Hierarchy",
                                          "Assets"};
    auto expect_valid = [&](const char* when) {
        const auto issues =
            affineui::UiScript::validate_dock_layout(doc.dock_layout(), all);
        for (const auto& i : issues) {
            FAIL_CHECK(when << ": " << i);
        }
        REQUIRE(issues.empty());
    };

    for (int round = 0; round < 3; ++round) {
        // Bottom pane tab → bottom band of Hierarchy: splits under it.
        REQUIRE(ui.drag("[data-dcs-target=#Assets-body]", "pane-Hierarchy",
                        affineui::UiScript::Anchor::Bottom));
        expect_valid("after dock under Hierarchy");
        // ... and back again: to the bottom band of the workspace (over the
        // document pane's lower edge — window-edge band docks it back at the
        // workspace level, the original arrangement).
        REQUIRE(ui.drag("[data-dcs-target=#Assets-body]", "pane-__document__",
                        affineui::UiScript::Anchor::Bottom));
        expect_valid("after dock back to the bottom");
    }
}

TEST_CASE("UiControls: game-editor-shaped tearoff — press reload, doc toolbar, "
          "panel toolbar rides along, second gesture survives") {
    // Mirrors the REAL game editor sequence that crashed in-window: the press
    // selects the tab which triggers an app reload MID-GESTURE (selection
    // changed), the release tears the panel off over the document body, the
    // layout_changed reload replays the surgery, and a SECOND gesture follows.
    // The document pane has a tab toolbar (viewport toolbar) and the dragged
    // panel has its own toolbar that must ride along into the float.
    constexpr int W = 640;
    constexpr int H = 360;
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_user_stylesheet(R"CSS(
        html, body { width: 640px; height: 360px; margin: 0; padding: 0; }
        #aui-root { width: 640px; height: 360px; min-height: 0; padding: 0; }
        .shell { width: 640px; height: 360px; display: flex; flex-direction: column; }
        .dcs-dock { display: flex; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dock--floathost { position: relative; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane--center { flex: 1; }
        .dcs-dockpane__tabbar { flex: 0 0 24px; display: flex; min-width: 0; }
        .dcs-dockpane__tabs { display: flex; min-width: 0; }
        .dcs-dockpane__tab { display: inline-flex; padding: 0 12px; white-space: nowrap; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        [hidden] { display: none; }
    )CSS");

    auto rebuild = [&]() {
        affineui::View v{affineui::ViewTheme::Decius};
        v.set_dock_layout_provider([&] { return doc.dock_layout(); });
        v.begin();
        {
            auto shell = v.container("shell", "shell");
            (void) shell;
            v.document_view("workarea", [&](affineui::View& dv) {
                dv.document(
                     [](affineui::View& p) { p.text("Viewport", "vp-text"); },
                     "View", "cube")
                    .toolbar([](affineui::View& tb) {
                        tb.button("Mode", false, "vp-mode-btn");
                    });
                dv.dockpanel(
                      "Hierarchy",
                      affineui::DockLocation::docked(affineui::Dock::Left, 180),
                      [](affineui::View& p) { p.text("H body", "h-text"); },
                      "layers", "Hierarchy")
                    .toolbar([](affineui::View& tb) {
                        tb.button("Filter", false, "h-filter-btn");
                    });
                dv.dockpanel(
                    "Inspector",
                    affineui::DockLocation::docked(affineui::Dock::Right, 160),
                    [](affineui::View& p) { p.text("I body", "i-text"); },
                    "cog", "Inspector");
            });
        }
        v.end();
        doc.set_html(v.to_html_document());
        doc.attach_script(affineui::DocumentScript::UiControls);
        doc.layout(W, H, &painter);
    };
    auto ev = [&](affineui::EventType t, affineui::Point p, bool reload_after) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        const auto result = doc.dispatch(e);
        if (reload_after || result.layout_changed) rebuild();
        return result;
    };

    rebuild();
    // Gesture 1: press the Hierarchy tab (app reloads on the press, like the
    // editor's selection reload), drag to the document body, release → tearoff.
    auto tab = find_hovered_attr(doc, "data-dcs-target", "#Hierarchy-body", W, H);
    REQUIRE(tab.x >= 0);
    ev(affineui::EventType::MouseDown, tab, /*reload_after=*/true);
    REQUIRE(find_hovered_id(doc, "__document__-host", W, H).x >= 0);
    const auto host = hovered_bounds_for_id(doc, "__document__-host");
    const affineui::Point drop{host.x + host.w / 2, host.y + host.h / 2};
    ev(affineui::EventType::MouseMove, drop, false);
    ev(affineui::EventType::MouseUp, drop, false);

    auto layout = doc.dock_layout();
    REQUIRE(layout.floats.size() == 1);
    CHECK(layout.floats[0].pane.tabs == std::vector<std::string>{"Hierarchy"});
    // The panel toolbar rode along into the float (replay keeps it bound).
    CHECK(find_hovered_attr(doc, "data-dcs-tabtoolbar", "#Hierarchy-body", W, H)
              .x >= 0);

    // Gesture 2 (the in-window crash): drag the float's title tab back onto
    // the Inspector pane to re-dock — after the reload replay. Bounds are
    // probed BEFORE the press: once a drag is armed, the drop indicator
    // overlay follows the cursor and steals hover probes.
    REQUIRE(find_hovered_attr(doc, "data-aui-name", "pane-Inspector", W, H).x >=
            0);
    affineui::Rect insp{-1, -1, 0, 0};
    for (const auto& info : doc.hovered_info_chain()) {
        for (const auto& attr : info.attrs) {
            if (attr.first == "data-aui-name" &&
                attr.second == "pane-Inspector") {
                insp = info.bounds;
            }
        }
    }
    REQUIRE(insp.w > 0);
    auto title = find_hovered_attr(doc, "data-dcs-target", "#Hierarchy-body",
                                   W, H);
    REQUIRE(title.x >= 0);
    ev(affineui::EventType::MouseDown, title, /*reload_after=*/true);
    // Aim at the middle of the Inspector pane (center → join as tab).
    const affineui::Point redock{insp.x + insp.w / 2, insp.y + insp.h / 2};
    ev(affineui::EventType::MouseMove, redock, false);
    ev(affineui::EventType::MouseUp, redock, false);

    layout = doc.dock_layout();
    CHECK(layout.floats.empty());
    bool joined = false;
    std::function<void(const affineui::Document::DockLayout::Node&)> find_join =
        [&](const affineui::Document::DockLayout::Node& n) {
            if (!n.split &&
                std::find(n.tabs.begin(), n.tabs.end(), "Hierarchy") !=
                    n.tabs.end() &&
                std::find(n.tabs.begin(), n.tabs.end(), "Inspector") !=
                    n.tabs.end())
                joined = true;
            for (const auto& c : n.children) find_join(c);
        };
    find_join(layout.root);
    CHECK(joined);
}

TEST_CASE("UiControls: dragging the primary dock tab leaves sibling tabs behind") {
    constexpr int W = 640;
    constexpr int H = 360;
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_user_stylesheet(R"CSS(
        html, body { width: 640px; height: 360px; margin: 0; padding: 0; }
        #aui-root { width: 640px; height: 360px; min-height: 0; padding: 0; }
        .shell { width: 640px; height: 360px; display: flex; flex-direction: column; }
        .dcs-dock { display: flex; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dock--floathost { position: relative; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane--center { flex: 1; }
        .dcs-dockpane__tabbar { flex: 0 0 24px; display: flex; min-width: 0; }
        .dcs-dockpane__tabs { display: flex; min-width: 0; }
        .dcs-dockpane__tab { display: inline-flex; padding: 0 12px; white-space: nowrap; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        [hidden] { display: none; }
    )CSS");

    auto rebuild = [&]() {
        affineui::View v{affineui::ViewTheme::Decius};
        // Replay the live dock arrangement (the DOM surgery result) on every
        // re-emit; without this the rebuild would re-seed the declared layout
        // and wipe the drag's effect.
        v.set_dock_layout_provider([&] { return doc.dock_layout(); });
        v.set_dock_active_tab_provider([&](std::string_view id) {
            return doc.dock_active_tab(id);
        });
        v.begin();
        {
            auto shell = v.container("shell", "shell");
            (void) shell;
            v.document_view("workarea", [&](affineui::View& dv) {
                dv.document(
                    [](affineui::View& p) { p.text("Viewport", "viewport-text"); },
                    "View", "cube");
                dv.dockpanel(
                    "Hierarchy",
                    affineui::DockLocation::docked(affineui::Dock::Left, 180),
                    [](affineui::View& p) { p.text("Hierarchy body", "hierarchy-text"); },
                    "layers", "Hierarchy");
                auto assets = dv.dockpanel(
                    "Assets",
                    affineui::DockLocation::docked(affineui::Dock::Bottom, 90),
                    [](affineui::View& p) { p.text("Assets body", "assets-text"); },
                    "image", "Assets");
                dv.dockpanel(
                    "Console", affineui::DockLocation::tab().in(assets),
                    [](affineui::View& p) { p.text("Console body", "console-text"); },
                    "file", "Console");
                dv.dockpanel(
                    "Log", affineui::DockLocation::tab().in(assets),
                    [](affineui::View& p) { p.text("Log body", "log-text"); },
                    "terminal", "Log");
            });
        }
        v.end();
        doc.set_html(v.to_html_document());
        doc.attach_script(affineui::DocumentScript::UiControls);
        doc.layout(W, H, &painter);
    };
    auto bounds_for_attr = [&](std::string_view name, std::string_view value) {
        const auto p = find_hovered_attr(doc, name, value, W, H);
        REQUIRE(p.x >= 0);
        for (const auto& info : doc.hovered_info_chain()) {
            for (const auto& attr : info.attrs) {
                if (attr.first == name && attr.second == value) {
                    return info.bounds;
                }
            }
        }
        FAIL("missing bounds for expected element");
        return affineui::Rect{};
    };
    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        const auto result = doc.dispatch(e);
        if (result.layout_changed) rebuild();
        return result;
    };

    SUBCASE("dragged to another pane") {
        rebuild();
        const auto tab = find_hovered_attr(doc, "data-dcs-target",
                                           "#Assets-body", W, H);
        REQUIRE(tab.x >= 0);
        const auto hierarchy = bounds_for_attr("data-aui-name", "pane-Hierarchy");
        const auto source = bounds_for_attr("data-aui-name", "pane-Assets");
        const affineui::Point hierarchy_bottom{
            hierarchy.x + hierarchy.w / 2,
            hierarchy.y + std::max(1, hierarchy.h - 4)};

        ev(affineui::EventType::MouseDown, tab);
        ev(affineui::EventType::MouseMove, hierarchy_bottom);
        ev(affineui::EventType::MouseUp, hierarchy_bottom);

        // Assets moved into a fresh pane below Hierarchy; the sibling tabs
        // (Console, Log) stay behind in the source pane — they are real DOM
        // children, nothing re-anchors.
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK(layout.floats.empty());
        const auto* assets_leaf = find_dock_leaf(layout.root, "Assets");
        REQUIRE(assets_leaf != nullptr);
        CHECK(assets_leaf->tabs == std::vector<std::string>{"Assets"});
        const auto* wrap = find_dock_parent(layout.root, assets_leaf);
        REQUIRE(wrap != nullptr);
        CHECK(wrap->vertical);
        REQUIRE(wrap->children.size() == 2);
        CHECK(wrap->children[0].tabs == std::vector<std::string>{"Hierarchy"});
        CHECK(&wrap->children[1] == assets_leaf);  // below Hierarchy
        const auto* source_leaf = find_dock_leaf(layout.root, "Console");
        REQUIRE(source_leaf != nullptr);
        CHECK(source_leaf->tabs ==
              std::vector<std::string>{"Console", "Log"});
        CHECK(source_leaf->active == "Console");  // first survivor activated

        // The surviving pane keeps the source slot's geometry (the replay
        // names it after its new primary tab, Console).
        CHECK(find_hovered_attr(doc, "data-aui-name", "pane-Console", W, H).x >= 0);
        CHECK(find_hovered_attr(doc, "data-dcs-target", "#Log-body", W, H).x >= 0);
        CHECK(find_hovered_attr(doc, "data-aui-name", "pane-Assets", W, H).x >= 0);
        const auto console_bounds =
            bounds_for_attr("data-aui-name", "pane-Console");
        CHECK(std::abs(console_bounds.x - source.x) <= 1);
        CHECK(std::abs(console_bounds.y - source.y) <= 1);
        CHECK(std::abs(console_bounds.w - source.w) <= 1);
        CHECK(std::abs(console_bounds.h - source.h) <= 1);
    }

    SUBCASE("dropped back on its own source pane: tears off, siblings stay") {
        rebuild();
        const auto tab = find_hovered_attr(doc, "data-dcs-target",
                                           "#Assets-body", W, H);
        REQUIRE(tab.x >= 0);
        const auto source = bounds_for_attr("data-aui-name", "pane-Assets");
        const affineui::Point source_center{
            source.x + source.w / 2, source.y + source.h / 2};

        ev(affineui::EventType::MouseDown, tab);
        ev(affineui::EventType::MouseMove, source_center);
        ev(affineui::EventType::MouseUp, source_center);

        // Strict decius.js: the source pane is never a target — releasing the
        // primary tab over its own pane is a tearoff. The co-tabs stay docked.
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        REQUIRE(layout.floats.size() == 1);
        CHECK(layout.floats[0].pane.tabs == std::vector<std::string>{"Assets"});
        CHECK(layout.floats[0].title_only == true);
        CHECK_FALSE(dock_tree_has_tab(layout.root, "Assets"));
        const auto* source_leaf = find_dock_leaf(layout.root, "Console");
        REQUIRE(source_leaf != nullptr);
        CHECK(source_leaf->tabs ==
              std::vector<std::string>{"Console", "Log"});
        CHECK(dock_tree_has_tab(layout.root, "Hierarchy"));

        CHECK(find_hovered_attr(doc, "data-aui-name", "pane-Console", W, H).x >= 0);
        CHECK(find_hovered_attr(doc, "data-dcs-target", "#Log-body", W, H).x >= 0);
        CHECK(find_hovered_attr(doc, "data-aui-name", "float-Assets", W, H).x >= 0);
    }
}

TEST_CASE("UiControls: dragging a dock parent onto its child preserves both panes") {
    constexpr int W = 720;
    constexpr int H = 420;
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_user_stylesheet(R"CSS(
        html, body { width: 720px; height: 420px; margin: 0; padding: 0; }
        #aui-root { width: 720px; height: 420px; min-height: 0; padding: 0; }
        .shell { width: 720px; height: 420px; display: flex; flex-direction: column; }
        .dcs-dock { display: flex; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dock--floathost { position: relative; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane--center { flex: 1; }
        .dcs-dockpane__tabbar { flex: 0 0 24px; display: flex; min-width: 0; }
        .dcs-dockpane__tabs { display: flex; min-width: 0; }
        .dcs-dockpane__tab { display: inline-flex; padding: 0 12px; white-space: nowrap; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        [hidden] { display: none; }
    )CSS");

    auto rebuild = [&]() {
        affineui::View v{affineui::ViewTheme::Decius};
        // Replay the live dock arrangement so each rebuild round-trips the
        // surgery instead of re-seeding the declared layout.
        v.set_dock_layout_provider([&] { return doc.dock_layout(); });
        v.set_dock_active_tab_provider([&](std::string_view id) {
            return doc.dock_active_tab(id);
        });
        v.begin();
        {
            auto shell = v.container("shell", "shell");
            (void) shell;
            v.document_view("workarea", [&](affineui::View& dv) {
                dv.document(
                    [](affineui::View& p) { p.text("Viewport", "viewport-text"); },
                    "View", "cube");
                auto assets = dv.dockpanel(
                    "Assets",
                    affineui::DockLocation::docked(affineui::Dock::Bottom, 110),
                    [](affineui::View& p) { p.text("Assets body", "assets-text"); },
                    "image", "Assets");
                dv.dockpanel(
                    "Console", affineui::DockLocation::tab().in(assets),
                    [](affineui::View& p) { p.text("Console body", "console-text"); },
                    "file", "Console");
            });
        }
        v.end();
        doc.set_html(v.to_html_document());
        doc.attach_script(affineui::DocumentScript::UiControls);
        doc.layout(W, H, &painter);
    };
    auto bounds_for_attr = [&](std::string_view name, std::string_view value) {
        const auto p = find_hovered_attr(doc, name, value, W, H);
        REQUIRE(p.x >= 0);
        for (const auto& info : doc.hovered_info_chain()) {
            for (const auto& attr : info.attrs) {
                if (attr.first == name && attr.second == value) {
                    return info.bounds;
                }
            }
        }
        FAIL("missing bounds for expected element");
        return affineui::Rect{};
    };
    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        const auto result = doc.dispatch(e);
        if (result.layout_changed) rebuild();
    };
    auto drag_tab_to = [&](std::string_view body_id, affineui::Point target) {
        const auto tab = find_hovered_attr(doc, "data-dcs-target", body_id, W, H);
        REQUIRE(tab.x >= 0);
        ev(affineui::EventType::MouseDown, tab);
        ev(affineui::EventType::MouseMove, target);
        ev(affineui::EventType::MouseUp, target);
    };

    // "Dock cycles" are impossible by construction now: a tab drop is plain
    // DOM surgery (tabs move between real panes), so dragging panes onto each
    // other in any order must keep both alive. Round-trip Console out of and
    // Assets into the same panes and check every step's tree.
    rebuild();

    // 1. Drag Console out to the workspace LEFT window edge: it becomes its
    //    own pane at the workspace level.
    const auto assets = bounds_for_attr("data-aui-name", "pane-Assets");
    drag_tab_to("#Console-body", {10, assets.y - 40});
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK(layout.floats.empty());
        const auto* console_leaf = find_dock_leaf(layout.root, "Console");
        REQUIRE(console_leaf != nullptr);
        CHECK(console_leaf->tabs == std::vector<std::string>{"Console"});
        const auto* assets_leaf = find_dock_leaf(layout.root, "Assets");
        REQUIRE(assets_leaf != nullptr);
        CHECK(assets_leaf->tabs == std::vector<std::string>{"Assets"});
        CHECK(dock_tree_has_tab(layout.root, "__document__"));
    }
    CHECK(find_hovered_attr(doc, "data-aui-name", "pane-Console", W, H).x >= 0);
    CHECK(find_hovered_attr(doc, "data-aui-name", "pane-Assets", W, H).x >= 0);

    // 2. Drag the Assets tab onto the Console pane's CENTER: Assets joins
    //    Console's tab row (just a tab move — no cycle, no lost pane).
    const auto console = bounds_for_attr("data-aui-name", "pane-Console");
    drag_tab_to("#Assets-body",
                {console.x + console.w / 2, console.y + console.h / 2});
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK(layout.floats.empty());
        const auto* leaf = find_dock_leaf(layout.root, "Console");
        REQUIRE(leaf != nullptr);
        CHECK(leaf->tabs == std::vector<std::string>{"Console", "Assets"});
        CHECK(leaf->active == "Assets");
        CHECK(dock_tree_has_tab(layout.root, "__document__"));
    }
    CHECK(find_hovered_attr(doc, "data-dcs-target", "#Assets-body", W, H).x >= 0);
    CHECK(find_hovered_attr(doc, "data-dcs-target", "#Console-body", W, H).x >= 0);

    // 3. Round-trip back: drag Assets to the BOTTOM window edge — it splits
    //    back out into its own pane; Console keeps its pane. Both survive.
    const auto merged = bounds_for_attr("data-aui-name", "pane-Console");
    drag_tab_to("#Assets-body",
                {merged.x + merged.w + 80, H - 10});
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK(layout.floats.empty());
        const auto* console_leaf = find_dock_leaf(layout.root, "Console");
        REQUIRE(console_leaf != nullptr);
        CHECK(console_leaf->tabs == std::vector<std::string>{"Console"});
        const auto* assets_leaf = find_dock_leaf(layout.root, "Assets");
        REQUIRE(assets_leaf != nullptr);
        CHECK(assets_leaf->tabs == std::vector<std::string>{"Assets"});
        CHECK(dock_tree_has_tab(layout.root, "__document__"));
    }
    CHECK(find_hovered_attr(doc, "data-aui-name", "pane-Console", W, H).x >= 0);
    CHECK(find_hovered_attr(doc, "data-aui-name", "pane-Assets", W, H).x >= 0);
}

TEST_CASE("UiControls: console can redock between Assets and Hierarchy "
          "repeatedly without shell text corruption") {
    constexpr int W = 720;
    constexpr int H = 520;
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_user_stylesheet(R"CSS(
        html, body { width: 720px; height: 520px; margin: 0; padding: 0; }
        #aui-root {
            width: 720px;
            height: 520px;
            min-height: 0;
            padding: 0;
            box-sizing: border-box;
        }
        .ge-app { width: 720px; height: 520px; display: flex; flex-direction: column; }
        .dcs-menubar { flex: 0 0 24px; display: flex; align-items: center; }
        .dcs-statusbar { flex: 0 0 20px; display: flex; align-items: center; }
        .dcs-dock { display: flex; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dock--floathost { position: relative; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane--center { flex: 1; }
        .dcs-dockpane__tabbar { flex: 0 0 24px; display: flex; min-width: 0; }
        .dcs-dockpane__tabs { display: flex; min-width: 0; }
        .dcs-dockpane__tab { display: inline-flex; padding: 0 12px; white-space: nowrap; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        .dcs-panel--floating { position: absolute; }
        [hidden] { display: none; }
    )CSS");

    auto rebuild = [&]() {
        affineui::View v{affineui::ViewTheme::Decius};
        // Replay the live dock arrangement (the DOM surgery result) on every
        // rebuild — this is the same wiring as a real app.
        v.set_dock_layout_provider([&] { return doc.dock_layout(); });
        v.set_dock_active_tab_provider([&](std::string_view id) {
            return doc.dock_active_tab(id);
        });
        v.begin();
        {
            auto app = v.container("ge-app", "app");
            (void) app;
            {
                auto menu = v.container("dcs-menubar", "menubar");
                (void) menu;
                v.text("File Edit Build", "menu-text");
            }
            v.document_view("workarea", [&](affineui::View& dv) {
                dv.document(
                    [](affineui::View& p) { p.text("Viewport", "viewport-text"); },
                    "View", "cube");
                dv.dockpanel(
                    "Hierarchy",
                    affineui::DockLocation::docked(affineui::Dock::Left, 240),
                    [](affineui::View& p) { p.text("WorldRoot Hero mesh", "hierarchy-text"); },
                    "layers", "Hierarchy");
                auto assets = dv.dockpanel(
                    "Assets",
                    affineui::DockLocation::docked(affineui::Dock::Bottom, 110),
                    [](affineui::View& p) { p.text("Hero Albedo Rig Shot", "assets-text"); },
                    "image", "Assets");
                dv.dockpanel(
                    "Console", affineui::DockLocation::tab().in(assets),
                    [](affineui::View& p) { p.text("READY Scene loaded", "console-text"); },
                    "file", "Console");
            });
            {
                auto status = v.container("dcs-statusbar", "statusbar");
                (void) status;
                v.text("READY 60 FPS", "status-text");
            }
        }
        v.end();
        doc.set_html(v.to_html_document());
        doc.attach_script(affineui::DocumentScript::UiControls);
        doc.layout(W, H, &painter);
    };

    auto bounds_for_attr = [&](std::string_view name, std::string_view value) {
        const auto p = find_hovered_attr(doc, name, value, W, H);
        REQUIRE(p.x >= 0);
        for (const auto& info : doc.hovered_info_chain()) {
            for (const auto& attr : info.attrs) {
                if (attr.first == name && attr.second == value) {
                    return info.bounds;
                }
            }
        }
        FAIL("missing bounds for expected element");
        return affineui::Rect{};
    };
    auto assert_shell_text = [&]() {
        const auto menu = bounds_for_attr("data-aui-name", "menubar");
        const auto status = bounds_for_attr("data-aui-name", "statusbar");
        CHECK(menu.w > 600);
        CHECK(status.w > 600);
        CHECK(status.y > menu.y);
        painter.text_draws.clear();
        doc.draw(painter);
        bool saw_status = false;
        bool saw_menu = false;
        for (const auto& draw : painter.text_draws) {
            if (draw.text == "READY 60 FPS") {
                saw_status = true;
                CHECK(draw.max_width > 80.0f);
            }
            if (draw.text == "File Edit Build") {
                saw_menu = true;
                CHECK(draw.max_width > 100.0f);
            }
        }
        CHECK(saw_status);
        CHECK(saw_menu);
    };

    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        const auto result = doc.dispatch(e);
        if (result.layout_changed) rebuild();
    };

    auto drag_console_to = [&](affineui::Point target) {
        const auto tab = find_hovered_attr(doc, "data-dcs-target",
                                           "#Console-body", W, H);
        REQUIRE(tab.x >= 0);
        ev(affineui::EventType::MouseDown, tab);
        ev(affineui::EventType::MouseMove, target);
        ev(affineui::EventType::MouseUp, target);
    };

    // Structural probes against the live dock tree (the override store is
    // dead — the dock structure IS the DOM).
    auto console_split_from = [&](std::string_view neighbor_tab,
                                  bool console_after) {
        // Console is a single-tab leaf sharing a split with `neighbor_tab`'s
        // leaf; `console_after` gives the expected order in that split.
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK(layout.floats.empty());
        const auto* console_leaf = find_dock_leaf(layout.root, "Console");
        REQUIRE(console_leaf != nullptr);
        CHECK(console_leaf->tabs == std::vector<std::string>{"Console"});
        const auto* parent = find_dock_parent(layout.root, console_leaf);
        REQUIRE(parent != nullptr);
        CHECK(parent->vertical);  // every probe here splits top/bottom
        const auto* neighbor_leaf = find_dock_leaf(*parent, neighbor_tab);
        REQUIRE(neighbor_leaf != nullptr);
        const int ci = dock_child_index(*parent, console_leaf);
        REQUIRE(ci >= 0);
        bool neighbor_before = false, neighbor_after = false;
        for (std::size_t i = 0; i < parent->children.size(); ++i) {
            if (find_dock_leaf(parent->children[i], neighbor_tab)) {
                if (static_cast<int>(i) < ci) neighbor_before = true;
                if (static_cast<int>(i) > ci) neighbor_after = true;
            }
        }
        CHECK((console_after ? neighbor_before : neighbor_after));
    };
    auto console_tabbed_into = [&](std::string_view host_tab) {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK(layout.floats.empty());
        const auto* leaf = find_dock_leaf(layout.root, "Console");
        REQUIRE(leaf != nullptr);
        CHECK(dock_leaf_has_tab(*leaf, host_tab));  // joined that pane's row
        CHECK(leaf->active == "Console");
    };

    rebuild();
    const int initial_hierarchy_w =
        bounds_for_attr("data-aui-name", "pane-Hierarchy").w;
    auto assert_hierarchy_width_stable = [&]() {
        const auto hierarchy =
            bounds_for_attr("data-aui-name", "pane-Hierarchy");
        CHECK(std::abs(hierarchy.w - initial_hierarchy_w) <= 1);
    };
    auto assert_hierarchy_body_visible = [&]() {
        painter.text_draws.clear();
        doc.draw(painter);
        CHECK(find_text_draw(painter, "WorldRoot Hero mesh") != nullptr);
    };
    for (int i = 0; i < 3; ++i) {
        // Console (sourced from the Assets pane) onto Hierarchy's bottom edge
        // band: a fresh Console pane splits in below Hierarchy.
        const auto hierarchy = bounds_for_attr("data-aui-name", "pane-Hierarchy");
        drag_console_to({hierarchy.x + hierarchy.w / 2,
                         hierarchy.y + std::max(1, hierarchy.h - 4)});
        console_split_from("Hierarchy", /*console_after=*/true);
        assert_hierarchy_width_stable();
        assert_shell_text();

        // And back onto the Assets pane's center: it re-joins the tab row.
        const auto assets = bounds_for_attr("data-aui-name", "pane-Assets");
        drag_console_to({assets.x + assets.w / 2,
                         assets.y + assets.h / 2});
        console_tabbed_into("Assets");
        assert_hierarchy_width_stable();
        assert_shell_text();
    }

    // Center drop on Hierarchy: Console joins Hierarchy's tab row.
    auto hierarchy = bounds_for_attr("data-aui-name", "pane-Hierarchy");
    drag_console_to({hierarchy.x + hierarchy.w / 2,
                     hierarchy.y + hierarchy.h / 2});
    console_tabbed_into("Hierarchy");
    assert_shell_text();

    // From Hierarchy back into Assets' center.
    auto assets = bounds_for_attr("data-aui-name", "pane-Assets");
    drag_console_to({assets.x + assets.w / 2,
                     assets.y + assets.h / 2});
    console_tabbed_into("Assets");
    assert_hierarchy_width_stable();
    assert_shell_text();
    assert_hierarchy_body_visible();

    hierarchy = bounds_for_attr("data-aui-name", "pane-Hierarchy");
    drag_console_to({hierarchy.x + hierarchy.w / 2,
                     hierarchy.y + hierarchy.h / 2});
    console_tabbed_into("Hierarchy");

    // Probe the Assets PANE's bottom edge band (the tabpanel is content-sized
    // under the canonical shape, so zone math must aim at the pane rect);
    // Console (sourced from Hierarchy) splits in BELOW Assets.
    auto assets_pane = bounds_for_attr("data-aui-name", "pane-Assets");
    drag_console_to({assets_pane.x + assets_pane.w / 2,
                     assets_pane.y + std::max(1, assets_pane.h * 9 / 10)});
    console_split_from("Assets", /*console_after=*/true);

    hierarchy = bounds_for_attr("data-aui-name", "pane-Hierarchy");
    drag_console_to({hierarchy.x + hierarchy.w / 2,
                     hierarchy.y + hierarchy.h / 2});
    console_tabbed_into("Hierarchy");

    // Top edge band of the Assets pane: zone bands start BELOW the tabbar
    // chrome (dockpane_zone_bounds), so probe just under the Assets tab row;
    // Console splits in ABOVE Assets.
    assets_pane = bounds_for_attr("data-aui-name", "pane-Assets");
    const auto assets_tab_rect =
        bounds_for_attr("data-dcs-target", "#Assets-body");
    drag_console_to({assets_pane.x + assets_pane.w / 2,
                     assets_tab_rect.y + assets_tab_rect.h + 4});
    console_split_from("Assets", /*console_after=*/false);
    assert_shell_text();
}

TEST_CASE("UiControls: View panel tearoff uses a default size inside the "
          "document body") {
    constexpr int W = 720;
    constexpr int H = 520;
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_user_stylesheet(R"CSS(
        html, body { width: 720px; height: 520px; margin: 0; padding: 0; }
        #aui-root {
            width: 720px;
            height: 520px;
            min-height: 0;
            padding: 0;
            box-sizing: border-box;
        }
        .ge-app { width: 720px; height: 520px; display: flex; flex-direction: column; }
        .dcs-menubar { flex: 0 0 24px; display: flex; align-items: center; }
        .dcs-statusbar { flex: 0 0 20px; display: flex; align-items: center; }
        .dcs-dock { display: flex; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dock--floathost { position: relative; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane--center { flex: 1; }
        .dcs-dockpane__tabbar { flex: 0 0 24px; display: flex; min-width: 0; }
        .dcs-dockpane__tabs { display: flex; min-width: 0; }
        .dcs-dockpane__tab { display: inline-flex; padding: 0 12px; white-space: nowrap; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        .dcs-panel--floating {
            position: absolute;
            display: flex;
            flex-direction: column;
            overflow: hidden;
        }
        .dcs-panel--floating > .dcs-dockpane {
            flex: 1 1 auto;
            min-width: 0;
            min-height: 0;
            display: flex;
            flex-direction: column;
        }
        .dcs-panel__header {
            flex: 0 0 24px;
            display: flex;
            align-items: center;
            min-width: 0;
        }
        .dcs-panel__title { flex: 1 1 auto; }
        .dcs-dockpane__titlebar > .dcs-panel__title.dcs-panel__title--dock-tab {
            flex: 0 1 auto;
        }
        .dcs-panel__tools { flex: 1 1 auto; align-self: stretch; }
        .dcs-panel__resize-zones {
            position: absolute;
            inset: 0;
            z-index: 4;
            pointer-events: none;
        }
        .dcs-panel__resize-zone {
            position: absolute;
            pointer-events: auto;
            background: transparent;
        }
        .dcs-panel__resize-zone--n { top: 0; left: 0; right: 0; height: 5px; }
        .dcs-panel__resize-zone--s { bottom: 0; left: 0; right: 0; height: 5px; }
        .dcs-panel__resize-zone--w { top: 0; left: 0; bottom: 0; width: 5px; }
        .dcs-panel__resize-zone--e { top: 0; right: 0; bottom: 0; width: 5px; }
        .dcs-panel__resize-zone--nw { top: 0; left: 0; width: 12px; height: 12px; }
        .dcs-panel__resize-zone--ne { top: 0; right: 0; width: 12px; height: 12px; }
        .dcs-panel__resize-zone--sw { bottom: 0; left: 0; width: 12px; height: 12px; }
        .dcs-panel__resize-zone--se { bottom: 0; right: 0; width: 12px; height: 12px; }
        .dcs-panel__resize {
            position: absolute;
            right: 0;
            bottom: 0;
            width: 14px;
            height: 14px;
            pointer-events: none;
        }
        [hidden] { display: none; }
    )CSS");

    std::string last_html;
    auto rebuild = [&]() {
        affineui::View v{affineui::ViewTheme::Decius};
        // Replay the live dock arrangement (the tearoff/redock surgery) on
        // every rebuild — this also exercises the replay round-trip.
        v.set_dock_layout_provider([&] { return doc.dock_layout(); });
        v.set_dock_active_tab_provider([&](std::string_view id) {
            return doc.dock_active_tab(id);
        });
        v.begin();
        {
            auto app = v.container("ge-app", "app");
            (void) app;
            {
                auto menu = v.container("dcs-menubar", "menubar");
                (void) menu;
                v.text("File Edit Build", "menu-text");
            }
            v.document_view("workarea", [&](affineui::View& dv) {
                dv.document(
                    [](affineui::View& p) { p.text("Viewport", "viewport-text"); },
                    "View", "cube");
                dv.dockpanel(
                    "Hierarchy",
                    affineui::DockLocation::docked(affineui::Dock::Left, 240),
                    [](affineui::View& p) { p.text("WorldRoot Hero mesh", "hierarchy-text"); },
                    "layers", "Hierarchy");
                auto assets = dv.dockpanel(
                    "Assets",
                    affineui::DockLocation::docked(affineui::Dock::Bottom, 110),
                    [](affineui::View& p) { p.text("Hero Albedo Rig Shot", "assets-text"); },
                    "image", "Assets");
                dv.dockpanel(
                    "Console", affineui::DockLocation::tab().in(assets),
                    [](affineui::View& p) { p.text("READY Scene loaded", "console-text"); },
                    "file", "Console");
            });
            {
                auto status = v.container("dcs-statusbar", "statusbar");
                (void) status;
                v.text("READY 60 FPS", "status-text");
            }
        }
        v.end();
        last_html = v.to_html_document();
        doc.set_html(last_html);
        doc.attach_script(affineui::DocumentScript::UiControls);
        doc.layout(W, H, &painter);
    };

    auto bounds_for_attr = [&](std::string_view name, std::string_view value) {
        const auto p = find_hovered_attr(doc, name, value, W, H);
        REQUIRE(p.x >= 0);
        for (const auto& info : doc.hovered_info_chain()) {
            for (const auto& attr : info.attrs) {
                if (attr.first == name && attr.second == value) {
                    return info.bounds;
                }
            }
        }
        FAIL("missing bounds for expected element");
        return affineui::Rect{};
    };
    auto bounds_for_id = [&](std::string_view id) {
        const auto p = find_hovered_chain_id(doc, id, W, H);
        REQUIRE(p.x >= 0);
        return hovered_bounds_for_id(doc, id);
    };
    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        const auto result = doc.dispatch(e);
        if (result.layout_changed) rebuild();
        return result;
    };

    rebuild();
    const auto doc_body = bounds_for_id("__document__-host");
    const auto source_pane = bounds_for_attr("data-aui-name", "pane-Assets");
    const affineui::Point drop{
        doc_body.x + doc_body.w / 2,
        doc_body.y + doc_body.h / 2};
    const auto tab = find_hovered_attr(doc, "data-dcs-target",
                                       "#Console-body", W, H);
    REQUIRE(tab.x >= 0);

    ev(affineui::EventType::MouseDown, tab);
    ev(affineui::EventType::MouseMove, drop);
    ev(affineui::EventType::MouseUp, drop);

    // Tearoff default size: min(420, source pane width) x min(360, source
    // pane height); the float spawns clamped INSIDE the document-body float
    // host with an 8px margin.
    const int expected_w = std::min(420, source_pane.w);
    const int expected_h = std::min(360, source_pane.h);
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        REQUIRE(layout.floats.size() == 1);
        CHECK(layout.floats[0].pane.tabs ==
              std::vector<std::string>{"Console"});
        CHECK(layout.floats[0].title_only == true);
        CHECK(layout.floats[0].w == expected_w);
        CHECK(layout.floats[0].h == expected_h);
        CHECK_FALSE(dock_tree_has_tab(layout.root, "Console"));
    }
    CHECK(last_html.find("dcs-dockpane--title-only") != std::string::npos);

    const auto doc_body_after = bounds_for_id("__document__-host");
    const auto workarea = bounds_for_attr("data-aui-name", "workarea");
    const auto floating = bounds_for_attr("data-aui-name", "float-Console");
    CHECK(floating.w == expected_w);
    CHECK(floating.h == expected_h);
    CHECK(floating.x >= doc_body_after.x + 8);
    CHECK(floating.y >= doc_body_after.y + 8);
    CHECK(floating.x + floating.w <= doc_body_after.x + doc_body_after.w - 8);
    CHECK(floating.y + floating.h <= doc_body_after.y + doc_body_after.h - 8);

    // Corner-resize the float: width/height grow, the position stays, and
    // the float never escapes the workspace float host.
    const affineui::Point resize_start{floating.x + floating.w - 2,
                                       floating.y + floating.h - 2};
    ev(affineui::EventType::MouseMove, resize_start);
    CHECK(doc.hovered_cursor() == 8);
    ev(affineui::EventType::MouseDown, resize_start);
    ev(affineui::EventType::MouseMove,
       {resize_start.x + 70, resize_start.y + 45});
    ev(affineui::EventType::MouseUp,
       {resize_start.x + 70, resize_start.y + 45});

    const auto resized_floating =
        bounds_for_attr("data-aui-name", "float-Console");
    CHECK(resized_floating.x == floating.x);
    CHECK(resized_floating.y == floating.y);
    CHECK(resized_floating.w > floating.w);
    CHECK(resized_floating.h > floating.h);
    CHECK(resized_floating.x + resized_floating.w <= workarea.x + workarea.w);
    CHECK(resized_floating.y + resized_floating.h <= workarea.y + workarea.h);
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        REQUIRE(layout.floats.size() == 1);
        CHECK(layout.floats[0].w == resized_floating.w);
        CHECK(layout.floats[0].h == resized_floating.h);
    }

    // Dragging the title bar CHROME (not the title tab) moves the panel only —
    // it never re-docks (decius float-drag).
    const affineui::Point title_empty{
        resized_floating.x + resized_floating.w -
            std::min(24, std::max(8, resized_floating.w / 8)),
        resized_floating.y + 12};
    const affineui::Point title_move{
        title_empty.x - 36,
        title_empty.y - 28};
    ev(affineui::EventType::MouseDown, title_empty);
    ev(affineui::EventType::MouseMove, title_move);
    ev(affineui::EventType::MouseUp, title_move);

    const auto moved_floating =
        bounds_for_attr("data-aui-name", "float-Console");
    CHECK(moved_floating.x < resized_floating.x);
    CHECK(moved_floating.y < resized_floating.y);
    CHECK(moved_floating.w == resized_floating.w);
    CHECK(moved_floating.h == resized_floating.h);
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        REQUIRE(layout.floats.size() == 1);  // still floating, not re-docked
        CHECK(layout.floats[0].pane.tabs ==
              std::vector<std::string>{"Console"});
    }

    const auto assets = bounds_for_attr("data-aui-name", "pane-Assets");
    const auto status = bounds_for_attr("data-aui-name", "statusbar");
    CHECK(status.y + status.h <= H);
    CHECK(assets.y + assets.h <= status.y);
    CHECK(std::abs((assets.y + assets.h) - status.y) <= 1);

    // Dragging the floating panel's TITLE TAB onto a docked pane re-docks it.
    const auto title_tab = find_hovered_attr(doc, "data-dcs-target",
                                             "#Console-body", W, H);
    REQUIRE(title_tab.x >= 0);
    const auto hierarchy =
        bounds_for_attr("data-aui-name", "pane-Hierarchy");
    const affineui::Point hierarchy_center{
        hierarchy.x + hierarchy.w / 2,
        hierarchy.y + hierarchy.h / 2};
    ev(affineui::EventType::MouseDown, title_tab);
    ev(affineui::EventType::MouseMove, hierarchy_center);
    ev(affineui::EventType::MouseUp, hierarchy_center);

    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK(layout.floats.empty());  // the floating wrapper was removed
        const auto* leaf = find_dock_leaf(layout.root, "Console");
        REQUIRE(leaf != nullptr);
        CHECK(leaf->tabs == std::vector<std::string>{"Hierarchy", "Console"});
        CHECK(leaf->active == "Console");
    }

    // Tear Console off a second time, then drop the Assets tab onto the
    // float's center: the float accepts it as a co-tab. This leg drives the
    // raw surgery (no rebuild between gestures) and replays once at the end.
    // NOTE: joining a float that has been round-tripped through the View
    // replay does not work yet — emit_one_floating_panel's title-only branch
    // emits no (hidden) tabbar, so ensure_tabbed_dock cannot re-home the
    // title tab (src bug, reported separately).
    auto ev_raw = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        return doc.dispatch(e);
    };
    const auto doc_body_redock = bounds_for_id("__document__-host");
    const affineui::Point second_tearoff_drop{
        doc_body_redock.x + doc_body_redock.w / 2,
        doc_body_redock.y + doc_body_redock.h / 2};
    const auto redocked_title_tab =
        find_hovered_attr(doc, "data-dcs-target", "#Console-body", W, H);
    REQUIRE(redocked_title_tab.x >= 0);
    ev_raw(affineui::EventType::MouseDown, redocked_title_tab);
    ev_raw(affineui::EventType::MouseMove, second_tearoff_drop);
    ev_raw(affineui::EventType::MouseUp, second_tearoff_drop);
    doc.layout(W, H, &painter);

    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        REQUIRE(layout.floats.size() == 1);
        CHECK(layout.floats[0].pane.tabs ==
              std::vector<std::string>{"Console"});
        CHECK(layout.floats[0].title_only == true);
        CHECK(dock_tree_has_tab(layout.root, "Hierarchy"));
        CHECK_FALSE(dock_tree_has_tab(layout.root, "Console"));
    }
    const auto refloated_panel =
        bounds_for_attr("data-aui-name", "float-Console");
    const affineui::Point float_center{
        refloated_panel.x + refloated_panel.w / 2,
        refloated_panel.y + refloated_panel.h / 2};
    const auto assets_tab = find_hovered_attr(doc, "data-dcs-target",
                                              "#Assets-body", W, H);
    REQUIRE(assets_tab.x >= 0);
    ev_raw(affineui::EventType::MouseDown, assets_tab);
    ev_raw(affineui::EventType::MouseMove, float_center);
    ev_raw(affineui::EventType::MouseUp, float_center);
    doc.layout(W, H, &painter);

    auto check_multi_tab_float = [&]() {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        REQUIRE(layout.floats.size() == 1);
        CHECK(layout.floats[0].pane.tabs ==
              std::vector<std::string>{"Console", "Assets"});
        CHECK(layout.floats[0].pane.active == "Assets");
        CHECK_FALSE(layout.floats[0].title_only);  // grew a real tab row
        CHECK_FALSE(dock_tree_has_tab(layout.root, "Assets"));
        CHECK(dock_tree_has_tab(layout.root, "__document__"));
    };
    check_multi_tab_float();

    // The multi-tab float survives the View replay round-trip.
    rebuild();
    check_multi_tab_float();
    CHECK(last_html.find("dcs-dockpane--multi-tab") != std::string::npos);
    CHECK(find_hovered_attr(doc, "data-dcs-target", "#Assets-body", W, H).x >= 0);
    CHECK(find_hovered_attr(doc, "data-dcs-target", "#Console-body", W, H).x >= 0);
}

TEST_CASE("UiControls: a 'panels' tab dropped on the document body does NOT dock "
          "(dock-kind mismatch) — it tears off") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 400px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        .dcs-panel--floating { position: absolute; }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock">
            <section class="dcs-dockpane dcs-dockpane--center" data-aui-name="pane-__document__" style="flex:1 1 0">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#doc-body">Doc</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="doc-body" data-dcs-tabpanel>doc</div></div>
            </section>
            <section class="dcs-dockpane" data-aui-name="pane-P" style="flex:0 0 200px">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabP" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#P-body">P</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="P-body" data-dcs-tabpanel>p</div></div>
            </section>
          </div>
        </div>
    )HTML");
    doc.layout(600, 400, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);
    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{}; e.type = t; e.button = affineui::MouseButton::Left;
        e.pos = p; doc.dispatch(e);
    };
    auto tabP = find_hovered_id(doc, "tabP", 600, 400);
    REQUIRE(tabP.x >= 0);
    ev(affineui::EventType::MouseDown, tabP);
    ev(affineui::EventType::MouseMove, {220, 220});  // over the document BODY
    ev(affineui::EventType::MouseUp, {220, 220});
    // No dock into the document (kind mismatch) -> it tore off into a float;
    // the document pane is untouched.
    const auto layout = doc.dock_layout();
    REQUIRE(layout.present);
    REQUIRE(layout.floats.size() == 1);
    CHECK(layout.floats[0].pane.tabs == std::vector<std::string>{"P"});
    CHECK(layout.floats[0].title_only == true);
    CHECK_FALSE(dock_tree_has_tab(layout.root, "P"));
    CHECK(dock_tree_has_tab(layout.root, "doc"));
}

TEST_CASE("UiControls: moving floating tearoff chrome does not re-dock the "
          "panel") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 400px; display: flex; }
        .target { flex: 0 0 280px; height: 400px; display: flex; flex-direction: column; }
        .doc-host { position: absolute; left: 300px; top: 30px; width: 260px; height: 320px; }
        .dcs-panel--floating { position: absolute; left: 330px; top: 40px; width: 180px; height: 150px; }
        .dcs-panel--floating > .dcs-dockpane { width: 100%; height: 100%; display: flex; flex-direction: column; }
        .dcs-panel__header { height: 30px; display: flex; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .move-zone { flex: 1; display: block; }
        .dcs-dockpane__body { flex: 1; }
        </style>
        <div class="dcs-dock dcs-dock--floathost">
          <section class="dcs-dockpane target" data-aui-name="pane-A">
            <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
              <button id="tabA" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#A-body">A</button>
            </div></div><div class="dcs-dockpane__body" id="A-body">A</div>
          </section>
          <div class="doc-host" data-dcs-float-host></div>
          <section class="dcs-panel dcs-panel--floating" data-dcs-drag
                   data-dcs-drag-bounds=".dcs-dock--floathost" data-dcs-dock-id="P">
            <section class="dcs-dockpane dcs-dockpane--title-only" data-aui-name="pane-P">
              <header class="dcs-panel__header dcs-dockpane__titlebar" data-dcs-drag-handle>
                <button id="tabP" class="dcs-dockpane__tab dcs-panel__title dcs-panel__title--dock-tab"
                        aria-selected="true" data-dcs-title-tab data-dcs-target="#P-body">P</button>
                <span id="moveHandle" class="move-zone" data-dcs-drag-handle></span>
              </header>
              <div class="dcs-dockpane__body" id="P-body">P</div>
            </section>
          </section>
        </div>
    )HTML");
    doc.layout(600, 400, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };
    auto grip = find_hovered_id(doc, "moveHandle", 600, 400);
    REQUIRE(grip.x >= 0);
    ev(affineui::EventType::MouseDown, grip);
    ev(affineui::EventType::MouseMove, {140, 160});
    ev(affineui::EventType::MouseUp, {140, 160});

    const auto ov = doc.dock_override("P");
    CHECK(ov.present == true);
    CHECK(ov.floating == true);
    CHECK(ov.x >= 300);
    CHECK(ov.y >= 30);
}

TEST_CASE("UiControls: dragging a floating tearoff title tab re-docks it") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 400px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .doc-host { position: absolute; left: 300px; top: 30px; width: 260px; height: 320px; }
        .dcs-panel--floating { position: absolute; left: 330px; top: 40px; width: 180px; height: 150px; }
        .dcs-panel--floating > .dcs-dockpane { width: 100%; height: 100%; }
        .dcs-panel__header { height: 30px; display: flex; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .move-zone { flex: 1; display: block; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 6px; }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock">
            <section class="dcs-dockpane" data-aui-name="pane-A" style="flex:0 0 280px">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabA" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#A-body">A</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="A-body" data-dcs-tabpanel>A</div></div>
            </section>
          </div>
          <div class="doc-host" data-dcs-float-host></div>
          <section class="dcs-panel dcs-panel--floating" data-dcs-drag
                   data-dcs-drag-bounds=".dcs-dock--floathost" data-dcs-dock-id="P">
            <section class="dcs-dockpane dcs-dockpane--title-only" data-aui-name="pane-P">
              <header class="dcs-panel__header dcs-dockpane__titlebar" data-dcs-drag-handle>
                <button id="tabP" class="dcs-dockpane__tab dcs-panel__title dcs-panel__title--dock-tab"
                        aria-selected="true" data-dcs-title-tab data-dcs-target="#P-body">P</button>
                <span class="move-zone" data-dcs-drag-handle></span>
              </header>
              <div class="dcs-dockpane__body"><div id="P-body" data-dcs-tabpanel>P</div></div>
            </section>
          </section>
        </div>
    )HTML");
    doc.layout(600, 400, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };
    auto tab = find_hovered_id(doc, "tabP", 600, 400);
    REQUIRE(tab.x >= 0);
    ev(affineui::EventType::MouseDown, tab);
    ev(affineui::EventType::MouseMove, {140, 200});
    ev(affineui::EventType::MouseUp, {140, 200});  // pane A's center: tab join

    // The title tab moved into pane A's tab row (and became active); the
    // emptied floating wrapper was removed.
    const auto layout = doc.dock_layout();
    REQUIRE(layout.present);
    CHECK(layout.floats.empty());
    const auto* leaf = find_dock_leaf(layout.root, "P");
    REQUIRE(leaf != nullptr);
    CHECK(leaf->tabs == std::vector<std::string>{"A", "P"});
    CHECK(leaf->active == "P");
}

TEST_CASE("UiControls: document-kind tabs do not enter the panel tearoff path") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 400px; display: flex; }
        .dcs-dockpane { flex: 1; height: 400px; display: flex; flex-direction: column; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; }
        </style>
        <div class="dcs-dock dcs-dock--floathost">
          <section class="dcs-dockpane dcs-dockpane--center" data-aui-name="pane-__document__">
            <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
              <button id="docTab" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#doc-body">Doc</button>
            </div></div><div class="dcs-dockpane__body" id="doc-body">doc</div>
          </section>
        </div>
    )HTML");
    doc.layout(600, 400, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    auto ev = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };
    auto tab = find_hovered_id(doc, "docTab", 600, 400);
    REQUIRE(tab.x >= 0);
    ev(affineui::EventType::MouseDown, tab);
    ev(affineui::EventType::MouseMove, {500, 220});
    ev(affineui::EventType::MouseUp, {500, 220});

    CHECK(doc.dock_override("__document__").present == false);
}

TEST_CASE("UiControls: splitter is inert without the script attached") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dock { display: flex; width: 440px; height: 200px; }
        .left  { flex: 0 0 200px; min-width: 0; }
        .right { flex: 0 0 200px; min-width: 0; }
        .split { flex: 0 0 8px; background: #888; }
        </style>
        <div class="dock">
            <div id="left" class="left">L</div>
            <div id="split" class="split" data-dcs-splitter="v"></div>
            <div id="right" class="right">R</div>
        </div>
    )HTML");
    doc.layout(440, 200, &painter);
    // No attach_script -> dragging the splitter must not resize anything.
    auto split_p = find_hovered_id(doc, "split", 440, 200);
    REQUIRE(split_p.x >= 0);
    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = split_p;
    doc.dispatch(down);
    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {split_p.x + 40, split_p.y};
    doc.dispatch(move);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = {split_p.x + 40, split_p.y};
    doc.dispatch(up);
    doc.layout(440, 200, &painter);
    auto left_after = find_hovered_id(doc, "left", 440, 200);
    REQUIRE(left_after.x >= 0);
    CHECK(doc.hovered_info().bounds.w == 200);
}
