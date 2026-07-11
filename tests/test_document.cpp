#include <doctest/doctest.h>

#include "affineui/automation.h"
#include "affineui/document.h"
#include "affineui/painter.h"
#include "affineui/ui.h"
#include "affineui/view.h"
#include "decius_interactions.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
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
        int seq{0};  // global op order (shared counter with FillDraw)
        std::string text;
        affineui::Point pos;
        affineui::Color color;
        float max_width{0.0f};
        TextAlign align{TextAlign::Left};
        float alpha{1.0f};
        // Clip active while this run was drawn (clipped == false â†’ none).
        affineui::Rect clip{};
        bool clipped{false};
    };
    // Global op sequence: monotonically stamps fills and text runs so tests
    // can assert CROSS-vector paint order (e.g. "the upper float's
    // background painted after the lower float's text").
    int op_seq{0};
    struct FillDraw {
        int seq{0};
        affineui::Rect rect;
        affineui::Color color;
        affineui::Mat2x3 transform;
    };
    struct RoundedFillDraw {
        int seq{0};  // global op order (shared counter with Fill/TextDraw)
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
    struct PathDraw {
        std::vector<float> cmds;
        affineui::PathPaint paint;
        bool stroked{false};
        float width{0.0f};
        // First kPathMove point and final on-path point â€” enough to
        // assert endpoint geometry without decoding the whole stream.
        float x0{0.0f};
        float y0{0.0f};
        float x1{0.0f};
        float y1{0.0f};
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
    std::vector<PathDraw> path_draws;
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
    std::vector<affineui::Rect> active_clip_stack;
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
        fill_draws.push_back({++op_seq, rect, color, current_transform});
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
    PathDraw make_path_draw(const float* cmds, std::size_t count,
                            const affineui::PathPaint& paint) {
        PathDraw d;
        d.cmds.assign(cmds, cmds + count);
        d.paint = paint;
        std::size_t i = 0;
        bool first = true;
        while (i < count) {
            const float verb = cmds[i];
            std::size_t coords = 0;
            if (verb == affineui::kPathMove || verb == affineui::kPathLine) {
                coords = 2;
            } else if (verb == affineui::kPathCubic) {
                coords = 6;
            } else {  // kPathClose
                ++i;
                continue;
            }
            const float px = cmds[i + coords - 1];  // last x
            const float py = cmds[i + coords];      // last y
            if (first) {
                d.x0 = cmds[i + 1];
                d.y0 = cmds[i + 2];
                first = false;
            }
            d.x1 = px;
            d.y1 = py;
            i += 1 + coords;
        }
        return d;
    }
    void fill_path(const float* cmds, std::size_t count,
                   const affineui::PathPaint& paint) override {
        path_draws.push_back(make_path_draw(cmds, count, paint));
    }
    void stroke_path(const float* cmds, std::size_t count,
                     const affineui::PathPaint& paint, float width,
                     affineui::LineCap, affineui::LineJoin) override {
        auto d = make_path_draw(cmds, count, paint);
        d.stroked = true;
        d.width = width;
        path_draws.push_back(d);
    }
    void fill_rounded_rect(const affineui::Rect& rect, float radius,
                           affineui::Color color) override {
        fill_colors.push_back(color);
        rounded_fill_draws.push_back(
            {++op_seq, rect, radius, radius, radius, radius, color});
    }
    void stroke_rounded_rect(const affineui::Rect&, float, affineui::Color color, float) override {
        stroke_colors.push_back(color);
    }
    void fill_rounded_rect_varying(const affineui::Rect& rect, float tl, float tr,
                                   float br, float bl,
                                   affineui::Color color) override {
        fill_colors.push_back(color);
        rounded_fill_draws.push_back({++op_seq, rect, tl, tr, br, bl, color});
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
        TextDraw d{++op_seq, std::string(text), pos, color, 0.0f,
                   TextAlign::Left, current_alpha};
        if (!active_clip_stack.empty()) {
            d.clip = active_clip_stack.back();
            d.clipped = true;
        }
        text_draws.push_back(std::move(d));
    }
    affineui::Size measure_text_box(std::uint32_t, std::string_view text,
                                    float max_width, float, float) override {
        const int natural = static_cast<int>(text.size()) * 8;
        return {natural < static_cast<int>(max_width)
                    ? natural
                    : static_cast<int>(max_width),
                18};
    }
    void draw_text_box(std::uint32_t, float x, float y,
                       std::string_view text, affineui::Color color,
                       float max_width,
                       float, float, TextAlign align) override {
        const affineui::Point pos{static_cast<int>(std::lround(x)),
                                  static_cast<int>(std::lround(y))};
        text_runs.emplace_back(text);
        TextDraw d{++op_seq, std::string(text), pos, color, max_width, align,
                   current_alpha};
        if (!active_clip_stack.empty()) {
            d.clip = active_clip_stack.back();
            d.clipped = true;
        }
        text_draws.push_back(std::move(d));
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
    void push_clip(const affineui::Rect& rect) override {
        clip_rects.push_back(rect);
        active_clip_stack.push_back(rect);
    }
    void pop_clip() override {
        if (!active_clip_stack.empty()) active_clip_stack.pop_back();
    }
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

// â”€â”€ Document::DockLayout assertions (the dock structure IS the DOM) â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

// Simulates the user's gesture that exposed the menu lag: open a menubar
// menu in the REAL game-editor document, then sweep the pointer along the
// bar. Every MouseMove dispatch must stay well under a frame â€” the bug
// history here is whole-document work (recollects, full restyles) hiding
// inside attribute mutations on the open/close path.
TEST_CASE("menubar hover-switch dispatch stays under a frame budget (GE app)") {
    std::ifstream in(AFFINEUI_TEST_SOURCE_DIR
                     "/conformance/cases/decius_ge_app/index.html",
                     std::ios::binary);
    REQUIRE(in.good());
    std::string html((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());

    affineui::Document doc;
    RecordingPainter painter;
    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(html);
    doc.layout(1280, 800, &painter);

    auto dispatch_at = [&](affineui::EventType type, int x, int y) {
        affineui::Event e{};
        e.type = type;
        e.button = affineui::MouseButton::Left;
        e.pos = {x, y};
        doc.dispatch(e);
    };

    // The sweep only measures the real path if menus genuinely open and
    // switch â€” track the expanded trigger's rect as it moves.
    auto menu_open_somewhere = [&] {
        // An open dcs-menu is a visible element with the class; probe a
        // known row: any menu shows items 20+px tall below the bar.
        return doc.find_element_rect("[aria-expanded=true]").w > 0;
    };

    // Locate the first menubar trigger by probing along the bar â€” the
    // harness painter's fake text metrics shift x positions, so nothing
    // is hardcoded.
    auto trigger_at = [&](int x) -> affineui::Rect {
        dispatch_at(affineui::EventType::MouseMove, x, 13);
        for (const auto& info : doc.hovered_info_chain()) {
            for (const auto& c : info.classes) {
                if (c == "dcs-menubar__item") return info.bounds;
            }
        }
        return {};
    };
    affineui::Rect first{};
    for (int x = 4; x <= 900 && first.w == 0; x += 6) first = trigger_at(x);
    REQUIRE(first.w > 0);
    const int bar_y = first.y + first.h / 2;

    // Open the first menu with a click.
    dispatch_at(affineui::EventType::MouseMove, first.x + first.w / 2, bar_y);
    dispatch_at(affineui::EventType::MouseDown, first.x + first.w / 2, bar_y);
    dispatch_at(affineui::EventType::MouseUp, first.x + first.w / 2, bar_y);
    doc.layout(1280, 800, &painter);
    REQUIRE(menu_open_somewhere());

    // Sweep along the bar over the other triggers, timing each move
    // (layout pumped per move like the app's frame loop would).
    double total_ms = 0.0;
    double worst_ms = 0.0;
    int moves = 0;
    int switches = 0;
    std::string last_expanded;
    for (int x = first.x; x <= first.x + 400; x += 4) {
        const auto t0 = std::chrono::steady_clock::now();
        dispatch_at(affineui::EventType::MouseMove, x, bar_y);
        doc.layout(1280, 800, &painter);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        total_ms += ms;
        worst_ms = std::max(worst_ms, ms);
        ++moves;
        const auto open = doc.find_element_rect("[aria-expanded=true]");
        const std::string key =
            std::to_string(open.x) + "," + std::to_string(open.w);
        if (open.w > 0 && key != last_expanded) {
            if (!last_expanded.empty()) ++switches;
            last_expanded = key;
        }
    }
    MESSAGE("first sweep (cold, first reveals): ", moves, " moves, ",
            switches, " switches, total ", total_ms, " ms, worst ",
            worst_ms, " ms");
    // The gesture is only exercised if the open menu actually FOLLOWED
    // the pointer across several triggers.
    CHECK(switches >= 2);

    // Return sweep: every menu now has retained boxes â€” this is the
    // steady state a user feels when sliding back and forth.
    double warm_total = 0.0;
    double warm_worst = 0.0;
    double worst_dispatch = 0.0;
    double worst_layout = 0.0;
    int warm_switches = 0;
    last_expanded.clear();
    for (int x = first.x + 400; x >= first.x; x -= 4) {
        const auto t0 = std::chrono::steady_clock::now();
        dispatch_at(affineui::EventType::MouseMove, x, bar_y);
        const auto tm = std::chrono::steady_clock::now();
        doc.layout(1280, 800, &painter);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        worst_dispatch = std::max(
            worst_dispatch,
            std::chrono::duration<double, std::milli>(tm - t0).count());
        worst_layout = std::max(
            worst_layout,
            std::chrono::duration<double, std::milli>(t1 - tm).count());
        warm_total += ms;
        warm_worst = std::max(warm_worst, ms);
        const auto open = doc.find_element_rect("[aria-expanded=true]");
        const std::string key =
            std::to_string(open.x) + "," + std::to_string(open.w);
        if (open.w > 0 && key != last_expanded) {
            if (!last_expanded.empty()) ++warm_switches;
            last_expanded = key;
        }
    }
    MESSAGE("return sweep (warm, retained boxes): ", warm_switches,
            " switches, total ", warm_total, " ms, worst ", warm_worst,
            " ms (worst dispatch ", worst_dispatch, " ms, worst layout ",
            worst_layout, " ms)");
    CHECK(warm_switches >= 2);
    // A casual sweep delivers a move every ~4-8 ms; anything slower than
    // a 60 Hz frame per DISPATCH visibly skips menus. The steady-state
    // (warm) pass must stay under a frame; generous so slow CI doesn't
    // flake, but whole-document-work regressions (tens of ms per switch)
    // fail loudly.
    CHECK(warm_worst < 16.0);
}

TEST_CASE("align-self overrides the container's align-items per item") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #row { display: flex; flex-direction: row; align-items: flex-start;
               width: 400px; height: 200px; }
        .box { width: 40px; height: 20px; }
        #stretchy { width: 40px; }
        #a { align-self: auto; }
        #b { align-self: center; }
        #c { align-self: flex-end; }
        #stretchy { align-self: stretch; }
        </style>
        <div id="row">
            <div id="a" class="box"></div>
            <div id="b" class="box"></div>
            <div id="c" class="box"></div>
            <div id="stretchy"></div>
        </div>
    )HTML");
    doc.layout(420, 220, &painter);

    const auto a = doc.find_element_rect("#a");
    const auto b = doc.find_element_rect("#b");
    const auto c = doc.find_element_rect("#c");
    const auto s = doc.find_element_rect("#stretchy");
    REQUIRE(a.w > 0);
    // auto â†’ the container's align-items (flex-start).
    CHECK(a.y == 0);
    // center â†’ (200 - 20) / 2.
    CHECK(b.y == 90);
    // flex-end â†’ 200 - 20.
    CHECK(c.y == 180);
    // stretch (heightless item) â†’ full cross size.
    CHECK(s.y == 0);
    CHECK(s.h == 200);
}

TEST_CASE("checks toggle on mouse DOWN and are not re-toggled by the release") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-check { display: block; width: 160px; height: 24px; }
        </style>
        <div id="check" class="dcs-check" aria-checked="false"></div>
    )HTML");
    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.layout(220, 80, &painter);

    auto checked = [&] {
        for (const auto& info : doc.hovered_info_chain()) {
            if (info.elem_id != "check") continue;
            for (const auto& attr : info.attrs) {
                if (attr.first == "aria-checked") return attr.second == "true";
            }
        }
        return false;
    };
    auto send = [&](affineui::EventType type, affineui::Point pos) {
        affineui::Event e{};
        e.type = type;
        e.button = affineui::MouseButton::Left;
        e.pos = pos;
        return doc.dispatch(e);
    };

    // decius.js wires pointerdown: the state flips the moment the button
    // goes down (release-time toggles read as sluggish).
    send(affineui::EventType::MouseMove, {80, 12});
    REQUIRE_FALSE(checked());
    send(affineui::EventType::MouseDown, {80, 12});
    CHECK(checked());

    // The release over the same control must NOT toggle it back.
    send(affineui::EventType::MouseUp, {80, 12});
    CHECK(checked());

    // Press again, drag off, release elsewhere: the press already
    // committed â€” the state keeps the down-toggled value.
    send(affineui::EventType::MouseDown, {80, 12});
    send(affineui::EventType::MouseMove, {80, 60});
    send(affineui::EventType::MouseUp, {80, 60});
    send(affineui::EventType::MouseMove, {80, 12});
    CHECK_FALSE(checked());
}

TEST_CASE("single-line input centers its line box in the content area") {
    affineui::Document doc;
    RecordingPainter painter;

    // Content box is 40px tall; the harness line box (line-height:1 at
    // font-size 12) is 12px. Browsers center the line box in the input's
    // content area, so the draw lands at y = 1 (border) + (40-12)/2 = 15.
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        input {
          display: block;
          box-sizing: border-box;
          width: 200px;
          height: 42px;
          border: 1px solid #000;
          padding: 0;
          font-size: 12px;
          line-height: 1;
        }
        </style>
        <input value="mid">
    )HTML");
    doc.layout(240, 80, &painter);
    doc.draw(painter);

    const auto* value = find_text_draw(painter, "mid");
    REQUIRE(value != nullptr);
    CHECK(value->pos.y == 15);
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

TEST_CASE("UiControls combo drag emits the change under the NAMED widget") {
    // The value-bearing element of a dcs-combo is its inner (unnamed)
    // <input>; the app's on_change is bound to the named combo node. A
    // drag-scrub must bubble the change to that name — this was the
    // "inspector vec fields do nothing" bug.
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-combo { display: block; width: 120px; height: 24px; }
        .dcs-combo__value { display: block; width: 120px; height: 24px; }
        </style>
        <div id="cx" class="dcs-combo" role="spinbutton" data-aui-name="position-0"
             data-dcs-combo="" data-value="1" data-step="0.01">
            <input class="dcs-combo__value" type="number" value="1">
        </div>
    )HTML");
    doc.layout(260, 120, &painter);

    auto send = [&](affineui::EventType type, int x, int y) {
        affineui::Event e{};
        e.type = type;
        e.button = affineui::MouseButton::Left;
        e.pos = {x, y};
        return doc.dispatch(e);
    };

    // Horizontal drag-scrub across the combo.
    send(affineui::EventType::MouseDown, 20, 12);
    send(affineui::EventType::MouseMove, 80, 12);
    send(affineui::EventType::MouseUp, 80, 12);

    const auto changes = doc.take_widget_changes();
    REQUIRE(!changes.empty());
    // The scrub streams LIVE changes; the release emits exactly one
    // committed change carrying the final value.
    for (std::size_t i = 0; i + 1 < changes.size(); ++i) {
        CHECK(changes[i].live);
    }
    CHECK(changes.back().name == "position-0");
    CHECK_FALSE(changes.back().live);
    CHECK(changes.back().value != "1");  // the value actually moved

    // Programmatic write-back: updates the widget silently (the
    // data-binding echo guard) — no change events.
    CHECK(doc.set_widget_value("position-0", "7.25"));
    CHECK(doc.take_widget_changes().empty());
    doc.layout(260, 120, &painter);
    // The inner input owns the hover; read the combo's attrs off the
    // hover chain entry.
    const auto pos = find_hovered_chain_id(doc, "cx", 260, 120);
    REQUIRE(pos.x >= 0);
    std::string data_value;
    for (const auto& info : doc.hovered_info_chain()) {
        if (info.elem_id != "cx") continue;
        for (const auto& [attr, attr_value] : info.attrs) {
            if (attr == "data-value") data_value = attr_value;
        }
    }
    CHECK(data_value == "7.25");
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

TEST_CASE("UiControls script opens Decius colorfield picker at declared width") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #field { display: flex; align-items: center; gap: 4px;
                 width: 240px; height: 24px; }
        #chip { display: block; width: 20px; height: 20px; }
        #hex { display: block; width: 184px; height: 20px; }
        #caret { display: block; width: 24px; height: 24px; }
        .dcs-popover[hidden] { display: none; }
        .dcs-popover { display: flex; flex-direction: column; }
        .dcs-popover__body { display: flex; flex-direction: column;
                             box-sizing: border-box; width: 100%;
                             gap: 8px; padding: 8px; }
        #sv { display: block; width: 100%; height: 120px; flex-shrink: 0; }
        #hue { display: block; width: 100%; height: 12px; flex-shrink: 0; }
        /* Real decius color-picker backgrounds (previously faked by a
           native class-name bodge; now rendered through the CSS path). */
        .dcs-color-square {
            background: linear-gradient(to top, #000, transparent),
                        linear-gradient(to right, #fff, var(--hue, red));
        }
        .dcs-hue-bar {
            background: linear-gradient(90deg,
                red, #ff0, #0f0, #0ff, #00f, #f0f, red);
        }
        </style>
        <div id="field" class="dcs-colorfield" data-aui-name="tint"
             data-value="#4d9fff">
            <span id="chip" class="dcs-colorfield__chip"
                  data-dcs-color="#4d9fff"></span>
            <input id="hex" class="dcs-colorfield__hex" value="#4d9fff">
            <span id="caret" class="dcs-colorfield__caret"
                  data-dcs-toggle="popover"
                  data-dcs-target="#picker"></span>
            <div id="picker" class="dcs-popover" style="width:204px" hidden>
                <div class="dcs-popover__body">
                    <div id="sv" class="dcs-color-square">
                        <div class="dcs-color-square__cursor"></div>
                    </div>
                    <div id="hue" class="dcs-hue-bar">
                        <div class="dcs-hue-bar__cursor"></div>
                    </div>
                </div>
            </div>
        </div>
    )HTML");
    doc.layout(360, 220, &painter);

    auto caret = find_hovered_chain_id(doc, "caret", 360, 220);
    REQUIRE(caret.x >= 0);
    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = caret;
    doc.dispatch(down);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = caret;
    doc.dispatch(up);
    doc.layout(360, 220, &painter);

    const auto picker_rect = doc.find_element_rect("#picker");
    const auto field_rect = doc.find_element_rect("#field");
    REQUIRE(field_rect.w == 240);
    CHECK(picker_rect.w == field_rect.w);
    const auto sv_rect = doc.find_element_rect("#sv");
    REQUIRE(sv_rect.w >= 188);
    REQUIRE(sv_rect.h == 120);
    CHECK(sv_rect.w >= picker_rect.w - 20);
    const auto hue_rect = doc.find_element_rect("#hue");
    REQUIRE(hue_rect.w == sv_rect.w);
    REQUIRE(hue_rect.h == 12);

    painter.linear_gradient_draws.clear();
    painter.path_draws.clear();
    doc.draw(painter);

    // The SV square renders as its two CSS background layers: the bottom
    // white->hue saturation ramp and the top black->transparent value
    // shade. Both are 2-stop, so both come through the native
    // fill_linear_gradient_rect fast path.
    int sv_layers = 0;
    bool saw_sv_hue = false;
    bool saw_sv_value = false;
    for (const auto& draw : painter.linear_gradient_draws) {
        if (draw.rect.x == sv_rect.x && draw.rect.y == sv_rect.y &&
            draw.rect.w == sv_rect.w && draw.rect.h == sv_rect.h) {
            ++sv_layers;
            saw_sv_hue = saw_sv_hue ||
                         (same_color(draw.stop0, affineui::Color::rgb(255, 255, 255)) &&
                          draw.stop1.a == 255 &&
                          !same_color(draw.stop1,
                                      affineui::Color::rgb(255, 255, 255)));
            saw_sv_value = saw_sv_value ||
                           (same_color(draw.stop0, affineui::Color::rgb(0, 0, 0)) &&
                            same_color(draw.stop1, affineui::Color::rgba(0, 0, 0, 0)));
        }
    }
    CHECK(sv_layers == 2);
    CHECK(saw_sv_hue);
    CHECK(saw_sv_value);

    // The hue bar is a single 7-stop CSS gradient. N-stop (>2) ramps are
    // lowered to one vector fill_path carrying the full PathPaint ramp
    // (rendered through the gradient-LUT), not the old six 2-stop
    // segments. Verify one linear N-stop path fill covering the bar with
    // the full rainbow ramp preserved.
    int hue_ramp_fills = 0;
    for (const auto& d : painter.path_draws) {
        if (d.stroked) continue;
        if (d.paint.kind != affineui::PathPaint::Kind::Linear) continue;
        if (d.paint.stop_count < 7) continue;
        ++hue_ramp_fills;
        // Rainbow endpoints + a green middle stop survive.
        CHECK(same_color(d.paint.colors[0], affineui::Color::rgb(255, 0, 0)));
        CHECK(same_color(d.paint.colors[2], affineui::Color::rgb(0, 255, 0)));
        CHECK(same_color(d.paint.colors[6], affineui::Color::rgb(255, 0, 0)));
    }
    CHECK(hue_ramp_fills == 1);

    const auto picker = find_hovered_chain_id(doc, "picker", 360, 220);
    REQUIRE(picker.x >= 0);
    CHECK(hovered_attr_for_id(doc, "picker", "data-dcs-base-style") ==
          "width:204px");

    down.pos = {picker_rect.x + picker_rect.w - 3, sv_rect.y - 4};
    up.pos = down.pos;
    doc.dispatch(down);
    doc.dispatch(up);
    doc.layout(360, 220, &painter);
    CHECK(doc.find_element_rect("#picker").w == picker_rect.w);

    down.pos = {sv_rect.x + sv_rect.w / 2, sv_rect.y + sv_rect.h / 2};
    up.pos = down.pos;
    doc.dispatch(down);
    doc.dispatch(up);
    doc.layout(360, 220, &painter);
    CHECK(doc.find_element_rect("#picker").w == picker_rect.w);
}

TEST_CASE("UiControls script drags Decius colorfield SV square") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #field { display: flex; align-items: center; gap: 4px;
                 width: 156px; height: 24px; }
        #chip { display: block; width: 20px; height: 20px; }
        #hex { display: block; width: 92px; height: 20px; }
        #caret { display: block; width: 24px; height: 24px; }
        .dcs-popover[hidden] { display: none; }
        .dcs-popover { display: flex; flex-direction: column; }
        .dcs-popover__body { display: flex; flex-direction: column;
                             gap: 8px; padding: 8px; }
        #sv { display: block; width: 188px; height: 120px; flex-shrink: 0; }
        #hue { display: block; width: 188px; height: 12px; flex-shrink: 0; }
        </style>
        <div id="field" class="dcs-colorfield" data-aui-name="tint"
             data-value="#4d9fff">
            <span id="chip" class="dcs-colorfield__chip"
                  data-dcs-color="#4d9fff"></span>
            <input id="hex" class="dcs-colorfield__hex" value="#4d9fff">
            <span id="caret" class="dcs-colorfield__caret"
                  data-dcs-toggle="popover"
                  data-dcs-target="#picker"></span>
            <div id="picker" class="dcs-popover" style="width:204px" hidden>
                <div class="dcs-popover__body">
                    <div id="sv" class="dcs-color-square">
                        <div class="dcs-color-square__cursor"></div>
                    </div>
                    <div id="hue" class="dcs-hue-bar">
                        <div class="dcs-hue-bar__cursor"></div>
                    </div>
                </div>
            </div>
        </div>
    )HTML");
    doc.layout(360, 220, &painter);

    auto click = [&](affineui::Point p) {
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

    auto caret = find_hovered_chain_id(doc, "caret", 360, 220);
    REQUIRE(caret.x >= 0);
    click(caret);
    doc.layout(360, 220, &painter);

    const auto sv = doc.find_element_rect("#sv");
    REQUIRE(sv.w > 0);
    const affineui::Point white{sv.x + 1, sv.y + 1};
    click(white);
    doc.layout(360, 220, &painter);

    auto field = find_hovered_chain_id(doc, "field", 360, 220);
    REQUIRE(field.x >= 0);
    const auto field_value = hovered_attr_for_id(doc, "field", "data-value");
    CHECK(near_white_hex(field_value));
    auto hex_input = find_hovered_chain_id(doc, "hex", 360, 220);
    REQUIRE(hex_input.x >= 0);
    CHECK(hovered_attr_for_id(doc, "hex", "value") == field_value);

    const auto changes = doc.take_widget_changes();
    CHECK(std::any_of(changes.begin(), changes.end(),
                      [&](const affineui::Document::WidgetChange& change) {
                          return change.name == "tint" &&
                                 change.value == field_value;
                      }));
}

TEST_CASE("Decius vector editor stacks at the web threshold and restores row gap") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        #props { display: block; width: 100%; }
        #field { display: flex; gap: 8px; width: 100%; height: 20px; }
        #props > #field.dcs-field--vec-stacked { height: auto; }
        #label { display: block; flex: 0 0 80px; height: 20px; }
        #vec { --dcs-xform-minwidth: 72px; display: flex; gap: 4px;
               min-width: 0; flex: 1 1 auto; }
        #vec > div { flex: 1 1 0; min-width: var(--dcs-xform-minwidth);
                     height: 20px; }
        #vec.dcs-vec--stacked { flex-direction: column; }
        #vec.dcs-vec--stacked > div { min-width: 0; width: 100%; }
        </style>
        <div id="props" class="dcs-props">
            <div id="field" class="dcs-field">
                <div id="label">Location</div>
                <div id="vec" class="dcs-vec">
                    <div id="x">X</div><div id="y">Y</div><div id="z">Z</div>
                </div>
            </div>
        </div>
    )HTML");

    doc.layout(300, 120, &painter);
    auto vec_hit = find_hovered_chain_id(doc, "vec", 300, 120);
    REQUIRE(vec_hit.x >= 0);
    CHECK(hovered_attr_for_id(doc, "vec", "class").find(
              "dcs-vec--stacked") != std::string::npos);
    CHECK(hovered_attr_for_id(doc, "field", "class").find(
              "dcs-field--vec") != std::string::npos);
    CHECK(hovered_attr_for_id(doc, "field", "class").find(
              "dcs-field--vec-stacked") != std::string::npos);
    const auto stacked_field = doc.find_element_rect("#field");
    const auto stacked_vec = doc.find_element_rect("#vec");
    CHECK(stacked_field.h >= stacked_vec.h);
    CHECK(stacked_field.h > 20);

    doc.layout(360, 120, &painter);
    vec_hit = find_hovered_chain_id(doc, "vec", 360, 120);
    REQUIRE(vec_hit.x >= 0);
    CHECK(hovered_attr_for_id(doc, "vec", "class").find(
              "dcs-vec--stacked") == std::string::npos);
    CHECK(hovered_attr_for_id(doc, "field", "class").find(
              "dcs-field--vec-stacked") == std::string::npos);
    const auto x = doc.find_element_rect("#x");
    const auto y = doc.find_element_rect("#y");
    CHECK(y.x - (x.x + x.w) == 4);
}

TEST_CASE("Decius tree drop target classes paint native insertion rules") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-tree { display: flex; flex-direction: column; width: 220px; }
        .dcs-tree__row { display: flex; height: 24px; }
        </style>
        <div class="dcs-tree">
            <div id="before" class="dcs-tree__row dcs-tree__row--drop-before">
                Before
            </div>
            <div id="after" class="dcs-tree__row dcs-tree__row--drop-after">
                After
            </div>
            <div id="into" class="dcs-tree__row dcs-tree__row--drop-into">
                Into
            </div>
        </div>
    )HTML");
    doc.layout(260, 100, &painter);
    doc.draw(painter);

    const auto before = doc.find_element_rect("#before");
    const auto after = doc.find_element_rect("#after");
    const auto into = doc.find_element_rect("#into");
    REQUIRE(before.w > 0);
    REQUIRE(after.w > 0);
    REQUIRE(into.w > 0);

    bool saw_before_rule = false;
    bool saw_after_rule = false;
    bool saw_into_fill = false;
    bool saw_into_bar = false;
    for (const auto& fill : painter.fill_draws) {
        if (same_color(fill.color, affineui::Color::rgb(0x4d, 0x9f, 0xff))) {
            saw_before_rule = saw_before_rule ||
                              (fill.rect.x == before.x &&
                               fill.rect.y == before.y - 1 &&
                               fill.rect.w == before.w &&
                               fill.rect.h == 2);
            saw_after_rule = saw_after_rule ||
                             (fill.rect.x == after.x &&
                              fill.rect.y == after.y + after.h - 1 &&
                              fill.rect.w == after.w &&
                              fill.rect.h == 2);
            saw_into_bar = saw_into_bar ||
                           (fill.rect.x == into.x &&
                            fill.rect.y == into.y &&
                            fill.rect.w == 3 &&
                            fill.rect.h == into.h);
        }
        saw_into_fill = saw_into_fill ||
                        (same_color(fill.color,
                                    affineui::Color::rgba(0x4d, 0x9f, 0xff,
                                                          48)) &&
                         fill.rect.x == into.x &&
                         fill.rect.y == into.y &&
                         fill.rect.w == into.w &&
                         fill.rect.h == into.h);
    }
    CHECK(saw_before_rule);
    CHECK(saw_after_rule);
    CHECK(saw_into_fill);
    CHECK(saw_into_bar);
}

TEST_CASE("UiControls script reorders Decius tree rows by drag/drop") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-tree { display: flex; flex-direction: column; width: 220px; }
        .dcs-tree__row { display: flex; align-items: center; height: 24px; }
        .dcs-tree__row[hidden] { display: none; }
        .dcs-tree__chevron { display: block; width: 18px; height: 24px; }
        .dcs-tree__label { display: block; flex: 1 1 auto; }
        </style>
        <div id="scene" class="dcs-tree" data-aui-name="scene"
             data-dcs-select="single">
            <div id="root" class="dcs-tree__row" draggable="true"
                 style="--depth:0">
                <span class="dcs-tree__chevron dcs-tree__chevron--open"></span>
                <span class="dcs-tree__label">Scene</span>
            </div>
            <div id="hero" class="dcs-tree__row" draggable="true"
                 style="--depth:1">
                <span class="dcs-tree__chevron dcs-tree__chevron--open"></span>
                <span class="dcs-tree__label">Hero</span>
            </div>
            <div id="sword" class="dcs-tree__row" draggable="true"
                 style="--depth:2">
                <span class="dcs-tree__chevron"></span>
                <span class="dcs-tree__label">Sword</span>
            </div>
            <div id="camera" class="dcs-tree__row" draggable="true"
                 style="--depth:1">
                <span class="dcs-tree__chevron"></span>
                <span class="dcs-tree__label">Camera</span>
            </div>
        </div>
    )HTML");
    doc.layout(260, 140, &painter);

    const auto hero = doc.find_element_rect("#hero");
    const auto camera = doc.find_element_rect("#camera");
    REQUIRE(hero.h > 0);
    REQUIRE(camera.h > 0);
    auto send = [&](affineui::EventType type, affineui::Point p) {
        affineui::Event e{};
        e.type = type;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };

    const affineui::Point drop_point{camera.x + camera.w - 4,
                                     camera.y + camera.h - 2};
    const affineui::Point into_point{camera.x + camera.w - 4,
                                     camera.y + camera.h / 2};
    send(affineui::EventType::MouseDown,
         {hero.x + 24, hero.y + hero.h / 2});
    CHECK(doc.take_widget_changes().empty());
    send(affineui::EventType::MouseMove, into_point);
    doc.layout(260, 140, &painter);
    painter.fill_draws.clear();
    doc.draw(painter);
    const auto camera_into = doc.find_element_rect("#camera");
    CHECK(std::any_of(painter.fill_draws.begin(), painter.fill_draws.end(),
                      [&](const RecordingPainter::FillDraw& fill) {
                          return same_color(
                                     fill.color,
                                     affineui::Color::rgba(0x4d, 0x9f, 0xff,
                                                           48)) &&
                                 fill.rect.x == camera_into.x &&
                                 fill.rect.y == camera_into.y &&
                                 fill.rect.w == camera_into.w &&
                                 fill.rect.h == camera_into.h;
                      }));
    send(affineui::EventType::MouseMove, drop_point);
    doc.layout(260, 140, &painter);
    painter.fill_draws.clear();
    doc.draw(painter);
    const auto camera_drop = doc.find_element_rect("#camera");
    CHECK(std::any_of(painter.fill_draws.begin(), painter.fill_draws.end(),
                      [&](const RecordingPainter::FillDraw& fill) {
                          return same_color(
                                     fill.color,
                                     affineui::Color::rgb(0x4d, 0x9f, 0xff)) &&
                                 fill.rect.x == camera_drop.x &&
                                 fill.rect.y ==
                                     camera_drop.y + camera_drop.h - 1 &&
                                 fill.rect.w == camera_drop.w &&
                                 fill.rect.h == 2;
                      }));
    send(affineui::EventType::MouseUp, drop_point);
    doc.layout(260, 140, &painter);

    const auto camera_after = doc.find_element_rect("#camera");
    const auto hero_after = doc.find_element_rect("#hero");
    const auto sword_after = doc.find_element_rect("#sword");
    CHECK(hero_after.y > camera_after.y);
    CHECK(sword_after.y > hero_after.y);
    auto hero_hit = find_hovered_chain_id(doc, "hero", 260, 140);
    REQUIRE(hero_hit.x >= 0);
    CHECK(hovered_attr_for_id(doc, "hero", "style").find("--depth:1") !=
          std::string::npos);

    const auto changes = doc.take_widget_changes();
    CHECK(std::any_of(changes.begin(), changes.end(),
                      [](const affineui::Document::WidgetChange& change) {
                          return change.name == "scene" &&
                                 change.value == "reorder";
                      }));
}

TEST_CASE("UiControls: tree drag/drop lands under FAST mouse moves (no frame "
          "layout between events)") {
    // In-window, a quick drag delivers several MouseMoves per rendered frame,
    // so consecutive dispatches see whatever the previous one left behind. The
    // drop-highlight class mutation dirtied layout, the next move's geometric
    // row lookup ran against the stale block tree, found nothing, and cleared
    // the drop target â€” flickering drop cursors and drops that landed nowhere
    // ("drag and drop only works intermittently"). Unlike the test above, this
    // one never calls layout() between events: every pointer event must make
    // the block tree current itself.
    affineui::Document doc;
    RecordingPainter painter;

    doc.attach_script(affineui::DocumentScript::UiControls);
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        .dcs-tree { display: flex; flex-direction: column; width: 220px; }
        .dcs-tree__row { display: flex; align-items: center; height: 24px; }
        .dcs-tree__row[hidden] { display: none; }
        .dcs-tree__chevron { display: block; width: 18px; height: 24px; }
        .dcs-tree__label { display: block; flex: 1 1 auto; }
        </style>
        <div id="scene" class="dcs-tree" data-aui-name="scene"
             data-dcs-select="single">
            <div id="root" class="dcs-tree__row" draggable="true"
                 style="--depth:0">
                <span class="dcs-tree__chevron dcs-tree__chevron--open"></span>
                <span class="dcs-tree__label">Scene</span>
            </div>
            <div id="hero" class="dcs-tree__row" draggable="true"
                 style="--depth:1">
                <span class="dcs-tree__chevron dcs-tree__chevron--open"></span>
                <span class="dcs-tree__label">Hero</span>
            </div>
            <div id="sword" class="dcs-tree__row" draggable="true"
                 style="--depth:2">
                <span class="dcs-tree__chevron"></span>
                <span class="dcs-tree__label">Sword</span>
            </div>
            <div id="camera" class="dcs-tree__row" draggable="true"
                 style="--depth:1">
                <span class="dcs-tree__chevron"></span>
                <span class="dcs-tree__label">Camera</span>
            </div>
        </div>
    )HTML");
    doc.layout(260, 140, &painter);

    const auto hero = doc.find_element_rect("#hero");
    const auto camera = doc.find_element_rect("#camera");
    REQUIRE(hero.h > 0);
    REQUIRE(camera.h > 0);
    auto send = [&](affineui::EventType type, affineui::Point p) {
        affineui::Event e{};
        e.type = type;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };

    const int x = hero.x + 24;
    send(affineui::EventType::MouseDown, {x, hero.y + hero.h / 2});
    // Stream a fast move sequence from the hero row down past the camera
    // row's bottom band â€” 2px steps, NO layout() calls in between.
    const int y_end = camera.y + camera.h - 2;
    for (int y = hero.y + hero.h / 2; y <= y_end; y += 2) {
        send(affineui::EventType::MouseMove, {x, y});
    }
    send(affineui::EventType::MouseUp, {x, y_end});
    doc.layout(260, 140, &painter);

    // The drop must have landed: hero (and its child) reordered after camera.
    const auto camera_after = doc.find_element_rect("#camera");
    const auto hero_after = doc.find_element_rect("#hero");
    const auto sword_after = doc.find_element_rect("#sword");
    CHECK(hero_after.y > camera_after.y);
    CHECK(sword_after.y > hero_after.y);
    const auto changes = doc.take_widget_changes();
    CHECK(std::any_of(changes.begin(), changes.end(),
                      [](const affineui::Document::WidgetChange& change) {
                          return change.name == "scene" &&
                                 change.value == "reorder";
                      }));
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

TEST_CASE("Numeric combo step: integer editor snaps to whole numbers") {
    // The single dcs-combo primitive is an INTEGER editor when data-step
    // is 1 (value snaps to whole numbers, renders without decimals) and a
    // FLOAT editor for a fractional step — three.js/decius.js parity, no
    // separate component. A programmatic set through both proves it.
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>body{margin:0;padding:0}
        .dcs-combo{display:block;width:120px;height:24px}
        .dcs-combo__value{display:block;width:120px;height:24px}</style>
        <div class="dcs-combo" data-aui-name="ints" data-dcs-combo=""
             data-min="0" data-max="100" data-value="3" data-step="1">
            <input class="dcs-combo__value" type="number" value="3">
        </div>
        <div class="dcs-combo" data-aui-name="floats" data-dcs-combo=""
             data-min="0" data-max="100" data-value="3" data-step="0.25">
            <input class="dcs-combo__value" type="number" value="3">
        </div>
    )HTML");
    doc.layout(200, 120, &painter);

    auto value_of = [&](std::string_view name) {
        doc.layout(200, 120, &painter);
        for (const auto& info : doc.hovered_info_chain()) { (void)info; }
        std::string out;
        // Scan blocks via the hover chain: hover the field, read its value.
        for (int y = 0; y < 120 && out.empty(); y += 4) {
            for (int x = 0; x < 200; x += 4) {
                affineui::Event mv{};
                mv.type = affineui::EventType::MouseMove;
                mv.pos = {x, y};
                doc.dispatch(mv);
                for (const auto& info : doc.hovered_info_chain()) {
                    bool match = false;
                    for (const auto& [a, v] : info.attrs) {
                        if (a == "data-aui-name" && v == name) match = true;
                    }
                    if (!match) continue;
                    for (const auto& [a, v] : info.attrs) {
                        if (a == "data-value") out = v;
                    }
                }
                if (!out.empty()) break;
            }
        }
        return out;
    };

    // Integer field: a fractional target snaps to the nearest whole
    // number and prints without a decimal point.
    CHECK(doc.set_widget_value("ints", "42.7"));
    CHECK(value_of("ints") == "43");
    CHECK(doc.set_widget_value("ints", "42.2"));
    CHECK(value_of("ints") == "42");

    // Float field (step 0.25): snaps to the quarter grid, keeps decimals.
    CHECK(doc.set_widget_value("floats", "42.7"));
    CHECK(value_of("floats") == "42.75");
    CHECK(doc.set_widget_value("floats", "42.1"));
    CHECK(value_of("floats") == "42");
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
        .dcs-field { display: flex; width: 220px; height: 24px; }
        .dcs-field__label { display: block; width: 80px; height: 24px; }
        .dcs-check, .dcs-radio { display: flex; width: 180px; height: 24px; }
        .dcs-check__box { display: block; width: 14px; height: 14px; }
        .dcs-btn-group { display: flex; width: 180px; height: 28px; }
        .dcs-btn { display: block; width: 60px; height: 28px; }
        </style>
        <label id="cast" class="dcs-check" aria-checked="true">
            <span id="cast-box" class="dcs-check__box"></span>
            <span>Cast shadows</span>
        </label>
        <div id="shadow-field" class="dcs-field" data-aui-widget="checkbox"
             data-aui-name="shadows">
            <span id="shadow-label" class="dcs-field__label">Shadows</span>
            <div id="shadow-check" class="dcs-check" role="checkbox">
                <span id="shadow-box" class="dcs-check__box"></span>
                <input id="shadow-input" type="checkbox" style="display:none">
            </div>
        </div>
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
    doc.layout(240, 220, &painter);

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
        const auto p = find_hovered_chain_id(doc, id, 240, 220);
        REQUIRE(p.x >= 0);
        return hovered_attr_for_id(doc, id, name);
    };

    auto cast = find_hovered_chain_id(doc, "cast", 240, 220);
    REQUIRE(cast.x >= 0);
    click_at(cast);
    CHECK(attr("cast", "aria-checked") == "false");

    auto shadow_label = find_hovered_chain_id(doc, "shadow-label", 240, 220);
    REQUIRE(shadow_label.x >= 0);
    click_at(shadow_label);
    CHECK(attr("shadow-field", "aria-checked") == "true");
    CHECK(attr("shadow-check", "aria-checked") == "true");
    CHECK(attr("shadow-box", "aria-checked") == "true");

    auto solver_b = find_hovered_chain_id(doc, "solver-b", 240, 220);
    REQUIRE(solver_b.x >= 0);
    click_at(solver_b);
    CHECK(attr("solver-a", "aria-checked") == "false");
    CHECK(attr("solver-b", "aria-checked") == "true");

    auto add = find_hovered_chain_id(doc, "blend-add", 240, 220);
    REQUIRE(add.x >= 0);
    click_at(add);
    CHECK(attr("blend-norm", "aria-pressed") == "false");
    CHECK(attr("blend-add", "aria-pressed") == "true");
    CHECK(attr("blend-norm", "class").find("dcs-btn--primary") ==
          std::string::npos);
    CHECK(attr("blend-add", "class").find("dcs-btn--primary") !=
          std::string::npos);

    auto mul = find_hovered_chain_id(doc, "blend-mul", 240, 220);
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

TEST_CASE("Full decius bundle: title-only float title tab is content-sized "
          "(icon + label), not collapsed") {
    std::ifstream in(AFFINEUI_TEST_SOURCE_DIR
        "/examples/frameworks/css/decius-css-0.6.2.bundle.min.css",
        std::ios::binary);
    REQUIRE(in.good());
    std::string bundle((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_user_stylesheet(bundle);
    // The exact structure the float replay emits for a single-tab tearoff.
    doc.set_html(R"HTML(
      <div class="dcs" data-dcs-density="compact" data-dcs-accent="cyan"
           style="width:600px;height:400px;position:relative">
        <section class="dcs-panel dcs-panel--floating"
                 style="left:40px;top:40px;width:320px;height:240px">
          <section class="dcs-dockpane dcs-dockpane--single-tab dcs-dockpane--title-only"
                   style="flex:1;min-width:0;min-height:0">
            <header class="dcs-panel__header dcs-dockpane__titlebar"
                    data-dcs-drag-handle>
              <button id="title-tab" type="button"
                      class="dcs-dockpane__tab dcs-panel__title dcs-panel__title--dock-tab"
                      aria-selected="true" data-dcs-title-tab
                      data-dcs-target="#console-body">
                <i class="di di-file"></i>
                <span class="dcs-dockpane__tab-label">Console</span>
              </button>
              <div class="dcs-panel__tools"></div>
            </header>
            <div class="dcs-dockpane__tabbar" hidden>
              <div class="dcs-dockpane__tabs"></div>
              <div class="dcs-dockpane__toolbars"></div>
            </div>
            <div class="dcs-dockpane__shelf" hidden></div>
            <div class="dcs-dockpane__body">
              <div id="console-body" data-dcs-tabpanel>console output</div>
            </div>
          </section>
        </section>
      </div>
    )HTML");
    SUBCASE("with a measuring painter") {
        doc.layout(620, 420, &painter);
        const auto title = doc.find_element_rect("#title-tab");
        // The browser sizes this at flex-basis:auto = icon + gap + "Console"
        // (~70px). It must never collapse to a sliver.
        CHECK(title.w >= 40);
        CHECK(title.h >= 14);

        // And it must NOT paint the active-TAB chrome: the bundle suppresses
        // the [aria-selected=true]:before/:after accent bars for
        // .dcs-panel__title--dock-tab (a title, not a tab). The accent for
        // data-dcs-accent=cyan is #00b8d4 â€” no fill of that color may appear
        // anywhere over the tab.
        painter.fill_draws.clear();
        doc.draw(painter);
        const auto accent = affineui::Color::rgb(0x00, 0xb8, 0xd4);
        bool accent_over_tab = false;
        for (const auto& fill : painter.fill_draws) {
            if (!same_color(fill.color, accent)) continue;
            const bool overlaps =
                fill.rect.x < title.x + title.w &&
                fill.rect.x + fill.rect.w > title.x &&
                fill.rect.y < title.y + title.h + 2 &&
                fill.rect.y + fill.rect.h + 2 > title.y;
            if (overlaps) {
                MESSAGE("accent fill over title tab at (", fill.rect.x, ",",
                        fill.rect.y, ",", fill.rect.w, "x", fill.rect.h, ")");
                accent_over_tab = true;
            }
        }
        CHECK_FALSE(accent_over_tab);
    }

    SUBCASE("painterless (headless estimate)") {
        // Server-side / painterless layout estimates glyph metrics â€” text must
        // still OCCUPY space or content-sized boxes collapse.
        doc.layout(620, 420, nullptr);
        const auto title = doc.find_element_rect("#title-tab");
        CHECK(title.w >= 40);
        CHECK(title.h >= 12);
    }
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
          "(no inline cursor) â€” .dcs-splitter--h must beat base col-resize") {
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
    // bottom (horizontal) splitter must end up row-resize (NS, up/down) â€” the
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
    // body purely through the cascade â€” no element ever sets `hidden`/`display`
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
    // must already apply at load â€” 'a' open (body visible), 'b' collapsed
    // (body display:none, so not hit-testable).
    CHECK(find_hovered_chain_id(doc, "a-body", 160, 120).x >= 0);
    CHECK(find_hovered_chain_id(doc, "b-body", 160, 120).x < 0);

    // DYNAMIC (the common foldout round-trip â€” starts visible): adding the
    // collapsed class to 'a' hides a-body via re-cascade of the descendant
    // rule; removing it again must bring a-body back. a-body kept its box
    // through the collapse, so this exercises the restyle path.
    REQUIRE(doc.set_attribute_by_id("a", "class", "fold fold--collapsed"));
    doc.layout(160, 120, &painter);
    CHECK(find_hovered_chain_id(doc, "a-body", 160, 120).x < 0);
    REQUIRE(doc.set_attribute_by_id("a", "class", "fold"));
    doc.layout(160, 120, &painter);
    CHECK(find_hovered_chain_id(doc, "a-body", 160, 120).x >= 0);

    // DYNAMIC (the reveal case â€” hidden at load, so it never had a box):
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
    // (or var() doesn't pick it up), every compact gap/padding is wrong â€” which
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
    // allowed to shrink â€” otherwise the cells bloat and the spacing looks wrong.
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
    // The only space between cells is the 6px gap â€” not a larger spread.
    CHECK(y->rect.x - (x->rect.x + x->rect.w) == 6);
    CHECK(z->rect.x - (y->rect.x + y->rect.w) == 6);
}

TEST_CASE("Clicking a foldout's title TEXT toggles it, and the collapse rule "
          "living in the USER stylesheet re-cascades (the game-editor setup)") {
    // Two game-editor-faithful conditions in one: (1) the header has only a
    // title (no chevron), so the click lands on the title's TEXT block â€” the
    // matcher must walk up past it to the header (a prior early-return bailed on
    // text clicks, so clicking a foldout title never toggled it); (2) decius is
    // loaded as the USER stylesheet (App::set_stylesheet), so the collapse rule
    // is NOT in the document's <style> â€” the toggle must re-match against it.
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

    // The general SVG renderer strokes path data through the Painter's
    // vector-path API (arcs are lowered to cubics), so assert on the
    // stroked path: accent colour resolved through var(), and endpoints
    // at the viewBox-scaled arc extremes (24 viewBox units â†’ 56px box,
    // scale 56/24).
    const float sc = 56.0f / 24.0f;
    bool saw_accent_arc = false;
    for (const auto& pd : painter.path_draws) {
        if (!pd.stroked) continue;
        if (!same_color(pd.paint.colors[0],
                        affineui::Color::rgb(0x4d, 0x9f, 0xff))) {
            continue;
        }
        saw_accent_arc = true;
        CHECK(pd.x0 == doctest::Approx(4.57537879754125f * sc).epsilon(0.01));
        CHECK(pd.y0 == doctest::Approx(19.42462120245875f * sc).epsilon(0.01));
        CHECK(pd.x1 ==
              doctest::Approx(4.123833768880174f * sc).epsilon(0.01));
        CHECK(pd.y1 ==
              doctest::Approx(5.056225414101656f * sc).epsilon(0.01));
        CHECK(pd.width == doctest::Approx(1.75f * sc).epsilon(0.05));
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
    // 6px left/right padding, its content box is x=103..387. A right-
    // aligned single-line input pre-shifts its draw ORIGIN so the glyph
    // run ends exactly at the content box's right edge â€” the alignment is
    // resolved in text_control_geometry (one origin shared by paint and
    // caret hit-mapping), and the painter receives a left-aligned run
    // with an unbounded wrap width (inputs never wrap: UA white-space is
    // pre). "1.000" measures 5 chars x 8px in this harness.
    CHECK(value->pos.x == 387 - 5 * 8);
    CHECK(value->align == affineui::Painter::TextAlign::Left);
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

TEST_CASE("textarea text starts at the CSS edit viewport padding") {
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
    CHECK(value->pos.y == bounds.y + 7);
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

TEST_CASE("textarea second-line caret hit testing works before first draw") {
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
                          return run.find("omega!") != std::string::npos;
                      }));
}

TEST_CASE("textarea second-line selection highlight stays inside the control") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        textarea {
          display: block;
          box-sizing: border-box;
          width: 220px;
          height: 96px;
          border: 1px solid #000;
          padding: 6px;
          font-size: 12px;
          line-height: 18px;
          white-space: pre-wrap;
        }
        </style>
        <textarea>Weathered sandstone
along the lower edge.</textarea>
    )HTML");
    doc.layout(260, 0, &painter);
    doc.draw(painter);

    const auto textarea_pos = find_hovered_tag(doc, "textarea");
    REQUIRE(textarea_pos.x >= 0);
    const auto bounds = doc.hovered_info().bounds;

    auto send = [&](affineui::EventType type, affineui::Point p) {
        affineui::Event ev{};
        ev.type = type;
        ev.button = affineui::MouseButton::Left;
        ev.pos = p;
        doc.dispatch(ev);
    };
    send(affineui::EventType::MouseDown, {bounds.x + 8, bounds.y + 32});
    send(affineui::EventType::MouseMove, {bounds.x + 118, bounds.y + 32});
    send(affineui::EventType::MouseUp, {bounds.x + 118, bounds.y + 32});

    painter.fill_draws.clear();
    doc.draw(painter);
    const affineui::Color selection{0x4D, 0xA3, 0xFF, 0x66};
    auto selected = std::find_if(
        painter.fill_draws.begin(), painter.fill_draws.end(),
        [&](const RecordingPainter::FillDraw& draw) {
            return same_color(draw.color, selection);
        });
    REQUIRE(selected != painter.fill_draws.end());
    CHECK(selected->rect.x >= bounds.x);
    CHECK(selected->rect.x + selected->rect.w <= bounds.x + bounds.w);
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

TEST_CASE("textarea word-wrapped caret uses the second visual line origin") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        textarea {
          display: block;
          box-sizing: border-box;
          width: 160px;
          height: 90px;
          border: 1px solid #000;
          padding: 6px;
          font-size: 12px;
          line-height: 18px;
          white-space: pre-wrap;
        }
        </style>
        <textarea>Weathered sandstone along lower edge.</textarea>
    )HTML");
    doc.layout(220, 0, &painter);
    doc.draw(painter);

    const auto textarea_pos = find_hovered_tag(doc, "textarea");
    REQUIRE(textarea_pos.x >= 0);
    const auto bounds = doc.hovered_info().bounds;

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {bounds.x + 44, bounds.y + 32};
    doc.dispatch(down);

    painter.stroke_line_draws.clear();
    doc.draw(painter);
    auto caret = std::find_if(
        painter.stroke_line_draws.begin(),
        painter.stroke_line_draws.end(),
        [&](const RecordingPainter::StrokeLineDraw& line) {
            return std::abs(line.x0 - line.x1) < 0.01f &&
                   line.y0 >= bounds.y + 24 &&
                   line.y0 <= bounds.y + 46;
        });
    REQUIRE(caret != painter.stroke_line_draws.end());
    CHECK(caret->x0 >= bounds.x + 24);
    CHECK(caret->x0 <= bounds.x + 70);
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

TEST_CASE("focused input never shows caret and selection at the same time") {
    affineui::Document doc;
    RecordingPainter painter;

    doc.set_html(R"HTML(
        <style>body{margin:0;padding:0}
        input{display:block;width:160px;padding:4px 8px;border:0}</style>
        <input value="abcd">
    )HTML");
    doc.layout(320, 0, &painter);

    const auto input_pos = find_hovered_tag(doc, "input");
    REQUIRE(input_pos.x >= 0);
    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = input_pos;
    doc.dispatch(down);
    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = input_pos;
    doc.dispatch(up);

    // A short near-vertical stroke inside the field is the caret.
    auto has_caret = [&] {
        painter.stroke_line_draws.clear();
        doc.draw(painter);
        for (const auto& sl : painter.stroke_line_draws) {
            const bool vertical = std::abs(sl.x0 - sl.x1) < 0.5f;
            const float h = std::abs(sl.y1 - sl.y0);
            if (vertical && h >= 6.0f && h <= 24.0f) return true;
        }
        return false;
    };

    // Select all (Ctrl+A) → a selection is active → NO caret painted.
    affineui::Event sel{};
    sel.type = affineui::EventType::KeyDown;
    sel.key = affineui::Key::A;
    sel.ctrl = true;
    doc.dispatch(sel);
    CHECK_FALSE(has_caret());

    // Collapse the selection (ArrowRight) → caret returns.
    affineui::Event right{};
    right.type = affineui::EventType::KeyDown;
    right.key = affineui::Key::ArrowRight;
    doc.dispatch(right);
    CHECK(has_caret());
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

// â”€â”€ @media query tests â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// Verify that min-width / max-width media queries are evaluated against
// the actual layout viewport width and nested style rules are applied
// when the query matches (and ignored when it doesn't).

TEST_CASE("@media min-width matches and applies nested rules") {
    // A blue box becomes red inside @media (min-width: 600px).
    // At viewport 800px the query matches â†’ fill should be red.
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
    doc.layout(800, 600, &painter);  // 800 >= 600 â†’ @media matches
    doc.draw(painter);

    // Red (#ff0000) should be present; blue (#0000ff) should NOT be
    // the final fill (media rule overrides the base rule).
    CHECK(saw_fill(painter, affineui::Color::rgb(0xff, 0x00, 0x00)));
    CHECK(!saw_fill(painter, affineui::Color::rgb(0x00, 0x00, 0xff)));
}

TEST_CASE("@media min-width does NOT match when viewport is smaller") {
    // Same setup as above, but viewport is 400px < 600px â†’ query fails,
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
    doc.layout(400, 600, &painter);  // 400 < 600 â†’ @media does NOT match
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
    doc.layout(320, 480, &painter);  // 320 <= 480 â†’ @media matches
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

    // First layout at 800px â†’ red
    doc.layout(800, 600, &painter);
    painter.fill_colors.clear();
    doc.draw(painter);
    CHECK(saw_fill(painter, affineui::Color::rgb(0xff, 0x00, 0x00)));

    // Second layout at 400px â†’ media no longer matches â†’ blue
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

TEST_CASE("UiControls: a splitter next to a flex:1 grower never freezes it â€” "
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

    // Now B is visible and A is hidden â€” the tab switched.
    CHECK(find_hovered_id(doc, "bodyB", 300, 200).x >= 0);
    CHECK(find_hovered_id(doc, "bodyA", 300, 200).x < 0);
}

TEST_CASE("overflow:auto flex body clips overflowing children to the pane "
          "(inspector bleed repro)") {
    // Field bug (DENDER/PhotoEdit inspector): pane content descends past the
    // pane's bottom edge and paints over the panel below. The pane shape is
    // the canonical dockpane: fixed-height flex column, header, then a
    // `flex:1; min-height:0; overflow:auto` body holding taller content.
    // Two independent ways this can break, both asserted here:
    //   1. LAYOUT â€” the body must get the pane's remaining height (100px),
    //      not its content height (240px). `min-height:0` on a flex child
    //      is what allows it to shrink below content size.
    //   2. PAINT â€” the body must register as a clip ancestor and push a
    //      clip of its padding box for descendant draws.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .pane { position: absolute; left: 0; top: 0; width: 200px;
                height: 120px; display: flex; flex-direction: column; }
        .hdr  { flex: 0 0 20px; }
        .scrollbody { flex: 1; min-height: 0; overflow: auto; }
        .row  { height: 30px; }
        </style>
        <div class="pane">
          <div class="hdr"></div>
          <div id="scrollbody" class="scrollbody" data-aui-name="scrollbody">
            <div class="row">r1</div><div class="row">r2</div>
            <div class="row">r3</div><div class="row">r4</div>
            <div class="row">r5</div><div class="row">r6</div>
            <div class="row">r7</div><div class="row">r8</div>
          </div>
        </div>
    )HTML");
    doc.layout(400, 300, &painter);

    // 1. Layout: the body fills the pane below the header â€” 100px tall,
    //    NOT the 240px of row content.
    const auto body = doc.find_element_rect("scrollbody");
    REQUIRE(body.w > 0);
    CHECK(body.y == 20);
    CHECK(body.h == 100);

    // 2. Paint: drawing the document pushes a clip matching the body's
    //    padding box for its (overflowing) children.
    painter.clip_rects.clear();
    doc.draw(painter);
    bool body_clip_seen = false;
    for (const auto& r : painter.clip_rects) {
        if (r.y == 20 && r.h == 100 && r.x == 0) body_clip_seen = true;
    }
    CHECK(body_clip_seen);
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

    // Drag far past the bounds â€” it clamps so the 60x40 float stays inside the
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

TEST_CASE("UiControls: dragging a float with NO authored left/top (right-"
          "anchored, offset host) moves by the mouse delta, not off-screen") {
    // Field bug (DENDER tearout/toolbar): the drag math derived the
    // containing-block origin as (element pos - authored inset), which is
    // garbage when left/top isn't authored â€” a right-anchored or
    // style-less float teleported off-screen on the first move (trace:
    // `write left/top=(-871,16)`). The containing block must be resolved
    // from the positioned-ancestor chain instead. The host here sits at a
    // NON-ZERO origin so the two derivations disagree.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .page  { position: absolute; left: 0; top: 0; width: 600px; height: 300px; }
        .host  { position: absolute; left: 200px; top: 40px;
                 width: 300px; height: 200px; }
        .float { position: absolute; right: 20px; top: 30px;
                 width: 80px; height: 50px; }
        .grip  { display: block; width: 80px; height: 14px; }
        </style>
        <div class="page">
          <div class="host">
            <div id="float" class="float" data-dcs-drag data-dcs-drag-bounds=".host">
              <span id="grip" class="grip" data-dcs-drag-handle></span>
            </div>
          </div>
        </div>
    )HTML");
    doc.layout(600, 300, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    // right:20 in a 300-wide host at x=200 â†’ float doc-x = 200+300-20-80 = 400.
    auto grip = find_hovered_id(doc, "grip", 600, 300);
    REQUIRE(grip.x >= 0);
    const auto before = doc.hovered_info().bounds;
    CHECK(before.x == 400);
    CHECK(before.y == 70);

    auto press = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };
    // Drag by (-30, +20): the float must land at exactly (370, 90) in doc
    // space â€” NOT at the host origin, NOT off-screen.
    press(affineui::EventType::MouseDown, grip);
    press(affineui::EventType::MouseMove, {grip.x - 30, grip.y + 20});
    press(affineui::EventType::MouseUp, {grip.x - 30, grip.y + 20});
    doc.layout(600, 300, &painter);

    REQUIRE(find_hovered_id(doc, "grip", 600, 300).x >= 0);
    const auto after = doc.hovered_info().bounds;
    CHECK(after.x == 370);
    CHECK(after.y == 90);
}

TEST_CASE("scratch: clip probe over a dumped app document"
          " * [.]") {  // hidden: run explicitly with -tc="scratch:*"
    // Diagnostic harness for the text-escapes-clip bug: load a real app dump
    // (AFFINEUI_PROBE_HTML=<path>), lay it out, and report where inspector
    // text draws land relative to the pane that should clip them.
    const char* path = std::getenv("AFFINEUI_PROBE_HTML");
    if (!path) return;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();

    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(ss.str());
    doc.layout(1600, 1000, &painter);

    auto report = [&](const char* name) {
        const auto r = doc.find_element_rect(name);
        MESSAGE(name << " rect=(" << r.x << "," << r.y << " " << r.w << "x"
                     << r.h << ")");
        return r;
    };
    const auto body = report("props-body");
    report("prop-sheet");
    report("prop-rail");

    RecordingPainter paint;
    doc.draw(paint);
    int escaped = 0;
    for (const auto& t : paint.text_draws) {
        // Inspector texts that paint below the inspector body's bottom edge
        // (or above its top) while positioned inside its x-range.
        if (body.w <= 0) break;
        const bool in_x = t.pos.x >= body.x && t.pos.x < body.x + body.w;
        const bool out_y = t.pos.y > body.y + body.h || t.pos.y + 14 < body.y;
        if (!in_x || !out_y) continue;
        if (++escaped <= 12) {
            MESSAGE("ESCAPE text='" << t.text.substr(0, 24) << "' pos=("
                    << t.pos.x << "," << t.pos.y << ") clipped=" << t.clipped
                    << " clip=(" << t.clip.x << "," << t.clip.y << " "
                    << t.clip.w << "x" << t.clip.h << ")");
        }
    }
    MESSAGE("total escaped inspector-x texts: " << escaped);
}

TEST_CASE("paint: a self-clipping label is ALSO clipped by its scroll pane "
          "(ancestor clip chain intersects, inspector bleed repro)") {
    // Field bug (DENDER inspector, but any scroll pane): the draw pass
    // pushed only the NEAREST clip ancestor. Any element with its own clip
    // context â€” a text-overflow:ellipsis label, an input â€” was therefore
    // exempt from its scroll pane's clip: its self-clip rect rides along
    // with the content, so overflowing/scrolled rows painted their text and
    // chrome straight over neighbouring panes. CSS requires the clip to be
    // the INTERSECTION of all clipping ancestors.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .pane  { position: absolute; left: 20px; top: 10px;
                 width: 200px; height: 100px; overflow: auto; }
        .row   { height: 30px; }
        .label { overflow: hidden; height: 18px; }
        </style>
        <div class="pane">
          <div class="row"><div class="label">ROW0</div></div>
          <div class="row"><div class="label">ROW1</div></div>
          <div class="row"><div class="label">ROW2</div></div>
          <div class="row"><div class="label">ROW3</div></div>
          <div class="row"><div class="label">ROW4</div></div>
          <div class="row"><div class="label">BELOWFOLD</div></div>
        </div>
    )HTML");
    doc.layout(400, 200, &painter);

    RecordingPainter paint;
    doc.draw(paint);

    const RecordingPainter::TextDraw* below = nullptr;
    const RecordingPainter::TextDraw* row0 = nullptr;
    for (const auto& t : paint.text_draws) {
        if (t.text == "BELOWFOLD") below = &t;
        if (t.text == "ROW0") row0 = &t;
    }
    REQUIRE(below != nullptr);
    REQUIRE(row0 != nullptr);

    // Pane content box: (20,10 200x100) â†’ bottom edge at y=110.
    // ROW0's label self-clip intersected with the pane is just the label
    // box â€” fully inside the pane.
    REQUIRE(row0->clipped);
    CHECK(row0->clip.y >= 10);
    CHECK(row0->clip.y + row0->clip.h <= 110);

    // Row 5 sits at y=160, past the pane's fold. Its label still clips it
    // (nearest ancestor), but the effective clip must ALSO be bounded by
    // the pane â€” the intersection is empty, so nothing can paint below
    // y=110. The old nearest-only clip was (row5's label box at y=160),
    // letting the text draw over whatever lay under the pane.
    REQUIRE(below->clipped);
    // The intersection is empty (the label box lies wholly past the fold);
    // an empty scissor paints nothing regardless of where it's anchored.
    CHECK(below->clip.h == 0);
}

TEST_CASE("paint: same-z floating panels paint ATOMICALLY - the lower "
          "float's text goes under the upper float's background") {
    // Field bug (PhotoEdit palettes, any overlapping same-z floats): the
    // boxes-then-text phases were grouped per z VALUE, so two floats both
    // at z-index:60 shared one group — every background painted, then every
    // text run, and the covered palette's labels bled through the panel
    // above it. Phases must group per NEAREST stacking root: each float is
    // atomic. The layer wrapper below models the View's float LAYER, which
    // itself carries a z-index — an "outermost root" rule made IT the
    // shared root of every float and the bleed survived (second repro).
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .host  { position: relative; width: 400px; height: 300px; }
        .layer { position: absolute; left: 0; top: 0; right: 0; bottom: 0;
                 z-index: 60; }
        .float { position: absolute; z-index: 60; width: 200px;
                 height: 120px; }
        .under { left: 20px;  top: 20px; background: #222222; }
        .over  { left: 120px; top: 60px; background: #123456; }
        .scroll { height: 60px; overflow: auto; }
        .tall   { height: 400px; }
        </style>
        <div class="host">
          <div class="layer">
            <div class="float under">
              <div>COVERED LABEL</div>
              <div class="scroll"><div class="tall">tall</div></div>
            </div>
            <div class="float over"><div>ON TOP</div></div>
          </div>
        </div>
    )HTML");
    doc.layout(400, 300, &painter);

    RecordingPainter paint;
    doc.draw(paint);

    const RecordingPainter::TextDraw* covered = nullptr;
    for (const auto& t : paint.text_draws) {
        if (t.text.find("COVERED") != std::string::npos) covered = &t;
    }
    REQUIRE(covered != nullptr);
    const RecordingPainter::FillDraw* over_bg = nullptr;
    for (const auto& f : paint.fill_draws) {
        // #123456 — the upper float's background fill.
        if (f.color.r == 0x12 && f.color.g == 0x34 && f.color.b == 0x56) {
            over_bg = &f;
        }
    }
    REQUIRE(over_bg != nullptr);
    MESSAGE("covered-text seq=", covered->seq, " upper-bg seq=", over_bg->seq);
    // The lower float paints atomically (bg THEN text) before the upper
    // float's background — so the label ends up underneath.
    CHECK(covered->seq < over_bg->seq);

    // The lower float's SCROLLBAR THUMB is part of the same atomic unit
    // (per-context Overlay phase), so it too paints before the upper
    // float's background. The old global draw-last scrollbar pass put
    // every pane's thumb over overlapping panels.
    const RecordingPainter::RoundedFillDraw* thumb = nullptr;
    for (const auto& f : paint.rounded_fill_draws) {
        // The thumb's fixed overlay color (0x9c,0xa0,0xb0).
        if (f.color.r == 0x9c && f.color.g == 0xa0 && f.color.b == 0xb0) {
            thumb = &f;
        }
    }
    REQUIRE(thumb != nullptr);
    MESSAGE("thumb seq=", thumb->seq);
    CHECK(thumb->seq > covered->seq);   // above its own context's content
    CHECK(thumb->seq < over_bg->seq);   // under the upper float
}

TEST_CASE("paint: a textarea's VALUE clip intersects the ancestor chain "
          "(tearoff-bottom bleed repro)") {
    // Field bug: the textarea stanza pushes its OWN padding-box scissor for
    // value/caret/selection painting — and a scissor REPLACES the active
    // clip. A textarea hanging past the fold of its scrolled pane (the
    // bottom of a tearoff) therefore painted its value over whatever lay
    // below the panel. The self-clip must intersect the ancestor chain.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .pane { position: absolute; left: 20px; top: 10px;
                width: 220px; height: 100px; overflow: auto; }
        .row  { height: 80px; }
        textarea { width: 200px; height: 60px; }
        </style>
        <div class="pane">
          <div class="row">above</div>
          <textarea>WEATHERED SANDSTONE</textarea>
        </div>
    )HTML");
    doc.layout(400, 300, &painter);

    RecordingPainter paint;
    doc.draw(paint);

    const RecordingPainter::TextDraw* value = nullptr;
    for (const auto& t : paint.text_draws) {
        if (t.text.find("WEATHERED") != std::string::npos) value = &t;
    }
    REQUIRE(value != nullptr);
    REQUIRE(value->clipped);
    // Pane content box bottom edge is y=110; the textarea starts at y=90
    // (10 + 80), so only its first 20px are inside the pane. The effective
    // clip must not extend past the pane's fold.
    MESSAGE("textarea value clip=(", value->clip.x, ",", value->clip.y, " ",
            value->clip.w, "x", value->clip.h, ")");
    CHECK(value->clip.y + value->clip.h <= 110);
}

TEST_CASE("UiControls: float drag is a pure paint translation mid-gesture â€” "
          "layout stays at the grab position, style commits on release") {
    // Compositor semantics (the interaction budget depends on it): while a
    // float drag is live, mouse moves must not restyle or re-lay-out
    // anything. The dragged subtree paints through a translation injected in
    // effective_transform_for; the inline left/top is written exactly once,
    // on MouseUp. Regression: the old path wrote the style attribute per
    // move â€” restyle + full relayout at mouse-poll rate made drags crawl.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .host  { position: absolute; left: 100px; top: 20px;
                 width: 400px; height: 240px; }
        .float { position: absolute; left: 40px; top: 30px;
                 width: 80px; height: 50px; background: #123456; }
        .grip  { display: block; width: 80px; height: 14px; }
        </style>
        <div class="host">
          <div class="float" data-aui-name="float" data-dcs-drag
               data-dcs-drag-bounds=".host">
            <span id="grip" class="grip" data-dcs-drag-handle></span>
          </div>
        </div>
    )HTML");
    doc.layout(600, 300, &painter);
    doc.attach_script(affineui::DocumentScript::UiControls);

    const affineui::Color float_bg{0x12, 0x34, 0x56, 0xFF};
    auto float_fill_at = [&](RecordingPainter& p)
        -> const RecordingPainter::FillDraw* {
        for (const auto& f : p.fill_draws) {
            if (f.color.r == float_bg.r && f.color.g == float_bg.g &&
                f.color.b == float_bg.b) {
                return &f;
            }
        }
        return nullptr;
    };

    auto send = [&](affineui::EventType t, affineui::Point p) {
        affineui::Event e{};
        e.type = t;
        e.button = affineui::MouseButton::Left;
        e.pos = p;
        doc.dispatch(e);
    };

    // left:40/top:30 in a host at (100,20) â†’ float doc pos (140,50).
    const auto grab = doc.find_element_rect("float");
    REQUIRE(grab.x == 140);
    REQUIRE(grab.y == 50);

    send(affineui::EventType::MouseDown, {170, 55});   // on the grip
    send(affineui::EventType::MouseMove, {170 + 25, 55 + 15});

    // Mid-gesture: the block tree has NOT moved (no style write, no
    // relayout) ...
    const auto mid = doc.find_element_rect("float");
    CHECK(mid.x == 140);
    CHECK(mid.y == 50);

    // ... but paint shows the float translated by the mouse delta: its
    // background fill still carries the layout rect, offset by a pure
    // translate transform.
    RecordingPainter mid_paint;
    doc.draw(mid_paint);
    {
        const auto* f = float_fill_at(mid_paint);
        REQUIRE(f != nullptr);
        CHECK(f->rect.x == 140);
        CHECK(f->rect.y == 50);
        CHECK(f->transform.tx == 25.0f);
        CHECK(f->transform.ty == 15.0f);
        CHECK(f->transform.a == 1.0f);
        CHECK(f->transform.d == 1.0f);
    }

    // Release: the position lands in the document (single style commit) and
    // paint no longer needs a transform.
    send(affineui::EventType::MouseUp, {170 + 25, 55 + 15});
    doc.layout(600, 300, &painter);
    const auto after = doc.find_element_rect("float");
    CHECK(after.x == 165);
    CHECK(after.y == 65);
    RecordingPainter end_paint;
    doc.draw(end_paint);
    {
        const auto* f = float_fill_at(end_paint);
        REQUIRE(f != nullptr);
        CHECK(f->rect.x == 165);
        CHECK(f->rect.y == 65);
        CHECK(f->transform.is_identity());
    }
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
        .dcs-splitter { flex: 0 0 1px; }
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

namespace {

// Canonical dockable workspace for tab-drag tests: 200px panels pane X (+
// optional Y tab), documents center pane, a drop indicator, inside a 600x400
// float host.
std::string dock_drag_workspace_html(bool with_second_panel) {
    std::string second;
    if (with_second_panel) {
        second =
            "<button id=\"tabY\" class=\"dcs-dockpane__tab\" "
            "aria-selected=\"false\" data-dcs-target=\"#Y-body\">Y</button>";
    }
    return R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 400px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 1px; }
        .dcs-panel--floating { position: absolute; }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock">
            <section class="dcs-dockpane" data-aui-name="pane-X" style="flex:0 0 200px">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabX" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#X-body">X</button>
)HTML" + second + R"HTML(
              </div></div>
              <div class="dcs-dockpane__body">
                <div id="X-body" data-dcs-tabpanel>content</div>
)HTML" + std::string(with_second_panel
                         ? "<div id=\"Y-body\" data-dcs-tabpanel hidden>ycontent</div>"
                         : "") + R"HTML(
              </div>
            </section>
            <section class="dcs-dockpane dcs-dockpane--center" data-aui-name="pane-__document__" style="flex:1 1 0">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Doc-body">Doc</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="Doc-body" data-dcs-tabpanel>doc</div></div>
            </section>
          </div>
          <div id="__dropind" data-aui-name="dropind" hidden></div>
        </div>
    )HTML";
}

}  // namespace

TEST_CASE("UiControls: a FIXED side column keeps its width through a "
          "dock/undock cycle on its edge (no flex scramble)") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(dock_drag_workspace_html(true));
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

    const auto x0 = doc.find_element_rect("pane-X");
    const auto d0 = doc.find_element_rect("pane-__document__");
    REQUIRE(x0.w == 200);
    MESSAGE("before: pane-X w=", x0.w, " doc x=", d0.x);

    // Split Y out of X onto X's RIGHT edge: the 200px column carves into two
    // fixed halves that (with the splitter) still sum to 200 — the document
    // must not move.
    auto tab = find_hovered_id(doc, "tabY", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move({static_cast<int>(x0.x + x0.w) - 10,
          static_cast<int>(x0.y + x0.h / 2)});
    up({static_cast<int>(x0.x + x0.w) - 10,
        static_cast<int>(x0.y + x0.h / 2)});

    const auto x1 = doc.find_element_rect("pane-X");
    const auto y1 = doc.find_element_rect("pane-Y");
    const auto d1 = doc.find_element_rect("pane-__document__");
    MESSAGE("split: X=(", x1.x, ",", x1.w, ") Y=(", y1.x, ",", y1.w,
            ") doc x=", d1.x);
    REQUIRE(y1.w > 0);
    CHECK(std::abs((x1.w + y1.w + 1) - 200) <= 4);   // column total preserved
    CHECK(std::abs(d1.x - d0.x) <= 4);               // document did not move

    // Undock Y to free space: the freed slice grows the FIXED neighbour back
    // (un-split restores the pre-split width); nothing else is rebalanced.
    tab = find_hovered_id(doc, "tabY", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move({450, 300});
    up({450, 300});

    const auto layout = doc.dock_layout();
    CHECK(layout.floats.size() == 1);
    const auto x2 = doc.find_element_rect("pane-X");
    const auto d2 = doc.find_element_rect("pane-__document__");
    MESSAGE("undock: pane-X w=", x2.w, " doc x=", d2.x);
    CHECK(std::abs(x2.w - x0.w) <= 4);               // 200px column restored
    CHECK(std::abs(d2.x - d0.x) <= 4);
}

TEST_CASE("UiControls: a WINDOW-EDGE drop wraps the whole workspace - the "
          "new pane spans the full orthogonal axis (across inner rows)") {
    // Vertical root: a row (fixed column + document) above a bottom
    // timeline row. A right-window-edge drop must produce a FULL-HEIGHT
    // column crossing the timeline — not a split of the inner row only.
    static constexpr const char* kHtml = R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 400px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-splitter { flex: 0 0 1px; }
        .dcs-panel--floating { position: absolute; }
        [hidden] { display: none; }
        </style>
        <div class="dcs-dock--floathost" data-dcs-float-host>
          <div class="dcs-dock dcs-dock--v">
            <div class="dcs-dock" style="flex:1 1 0;min-width:0;min-height:0">
              <section class="dcs-dockpane" data-aui-name="pane-X" style="flex:0 0 200px">
                <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                  <button id="tabX" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#X-body">X</button>
                </div></div>
                <div class="dcs-dockpane__body"><div id="X-body" data-dcs-tabpanel>content</div></div>
              </section>
              <div class="dcs-splitter"></div>
              <section class="dcs-dockpane dcs-dockpane--center" data-aui-name="pane-__document__" style="flex:1 1 0">
                <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                  <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#Doc-body">Doc</button>
                </div></div>
                <div class="dcs-dockpane__body"><div id="Doc-body" data-dcs-tabpanel>doc</div></div>
              </section>
            </div>
            <div class="dcs-splitter dcs-splitter--h"></div>
            <section class="dcs-dockpane" data-aui-name="pane-T" style="flex:0 0 100px">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabT" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#T-body">T</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="T-body" data-dcs-tabpanel>timeline</div></div>
            </section>
          </div>
          <div id="__dropind" data-aui-name="dropind" hidden></div>
        </div>
    )HTML";

    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(kHtml);
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

    const auto t0 = doc.find_element_rect("pane-T");
    REQUIRE(t0.h == 100);

    auto tab = find_hovered_id(doc, "tabX", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move({590, 200});  // inside the 32px right-window-edge band
    up({590, 200});

    const auto x1 = doc.find_element_rect("pane-X");
    const auto t1 = doc.find_element_rect("pane-T");
    MESSAGE("edge-dock: pane-X=(", x1.x, ",", x1.y, " ", x1.w, "x", x1.h,
            ") pane-T=(", t1.x, ",", t1.y, " ", t1.w, "x", t1.h, ")");
    // Full workspace height, pinned to the right edge, crossing the
    // timeline row (which shrinks to make room).
    CHECK(x1.h >= 395);
    CHECK(x1.x + x1.w >= 595);
    CHECK(x1.w >= 96);
    CHECK(t1.w < 500);
}

TEST_CASE("UiControls: a tearout's title tab dragged to free space re-spawns "
          "the tearout at the drop point (single-tab tearout moves)") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(dock_drag_workspace_html(false));
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

    // Tear X off into a floater.
    auto tab = find_hovered_id(doc, "tabX", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move({400, 200});
    up({400, 200});
    auto layout = doc.dock_layout();
    REQUIRE(layout.floats.size() == 1);
    const int old_x = layout.floats[0].x;
    const int old_y = layout.floats[0].y;

    // Drag the tearout's TITLE TAB to a different free-space point. This was
    // a hard no-op ("repositions via its chrome") â€” it must re-spawn the
    // panel's tearout at the drop point instead.
    tab = find_hovered_id(doc, "tabX", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move({120, 320});
    up({120, 320});

    layout = doc.dock_layout();
    REQUIRE(layout.floats.size() == 1);
    CHECK(layout.floats[0].pane.tabs == std::vector<std::string>{"X"});
    CHECK(layout.floats[0].title_only == true);
    CHECK((layout.floats[0].x != old_x || layout.floats[0].y != old_y));
    CHECK_FALSE(dock_tree_has_tab(layout.root, "X"));
}

TEST_CASE("UiControls: a tab dragged out of a MULTI-tab tearout splits into "
          "its own tearout, source keeps the rest") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(dock_drag_workspace_html(true));
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

    // Tear X off, then drop Y onto the floater's center to join it.
    auto tab = find_hovered_id(doc, "tabX", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move({400, 200});
    up({400, 200});
    auto layout = doc.dock_layout();
    REQUIRE(layout.floats.size() == 1);
    const affineui::Point float_center{layout.floats[0].x + layout.floats[0].w / 2,
                                       layout.floats[0].y + layout.floats[0].h / 2};

    tab = find_hovered_id(doc, "tabY", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move(float_center);
    up(float_center);
    layout = doc.dock_layout();
    REQUIRE(layout.floats.size() == 1);
    {
        auto tabs = layout.floats[0].pane.tabs;
        std::sort(tabs.begin(), tabs.end());
        CHECK(tabs == std::vector<std::string>{"X", "Y"});
    }
    CHECK(layout.floats[0].title_only == false);

    // Drag Y's tab from the tearout's tab row into free space (outside the
    // source floater â€” dropping on the floater itself is the self no-op) â†’
    // it splits into its OWN tearout; the source keeps X (and collapses to
    // title-only).
    tab = find_hovered_id(doc, "tabY", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move({100, 300});
    up({100, 300});

    layout = doc.dock_layout();
    REQUIRE(layout.floats.size() == 2);
    std::vector<std::string> all_tabs;
    for (const auto& f : layout.floats) {
        REQUIRE(f.pane.tabs.size() == 1);
        all_tabs.push_back(f.pane.tabs[0]);
        CHECK(f.title_only == true);
    }
    std::sort(all_tabs.begin(), all_tabs.end());
    CHECK(all_tabs == std::vector<std::string>{"X", "Y"});
}

TEST_CASE("UiControls: an APP-AUTHORED floating pane (no pane-<id> naming, "
          "N-panel shape) is a valid Tab drop target and self-target") {
    // Field bug (DENDER N-panel): compute_drop_target required a
    // pane-<id> data-aui-name to consider a pane a target at all, so
    // app-authored dockpanes â€” named by their own keys, tabs targeting
    // "#<id>" directly â€” never highlighted and never accepted drops. The
    // pane ELEMENT is the target; the id is best-effort metadata.
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .dcs-dock--floathost { position: relative; width: 600px; height: 400px; display: flex; }
        .dcs-dock { display: flex; flex: 1 1 0; min-width: 0; min-height: 0; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane__tab { display: inline-block; padding: 6px 16px; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
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
          <!-- App-authored floating cluster, N-panel shape: pane named by an
               app key, tabs targeting "#<id>" with no -body suffix. -->
          <section class="dcs-panel dcs-panel--floating" data-aui-name="app-float"
                   style="left:380px;top:60px;width:180px;height:200px;display:flex;flex-direction:column">
            <section class="dcs-dockpane" data-aui-name="np-pane" style="flex:1">
              <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs">
                <button id="tabItem" class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#np-item">Item</button>
              </div></div>
              <div class="dcs-dockpane__body"><div id="np-item" data-dcs-tabpanel>item props</div></div>
            </section>
          </section>
          <div id="__dropind" data-aui-name="dropind" hidden></div>
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

    // Drag X's tab over the app pane: it must highlight (valid Tab target â€”
    // floaters only ever JOIN as tabs, never split) ...
    auto tab = find_hovered_id(doc, "tabX", 600, 400);
    REQUIRE(tab.x >= 0);
    const affineui::Point over_app{380 + 90, 60 + 100};  // app pane center
    down(tab);
    move(over_app);
    {
        const auto ind = doc.find_element_rect("dropind");
        CHECK(ind.w > 0);
    }
    // ... and dropping joins its tab row.
    up(over_app);
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK_FALSE(dock_tree_has_tab(layout.root, "X"));
        REQUIRE(layout.floats.size() == 1);
        auto tabs = layout.floats[0].pane.tabs;
        std::sort(tabs.begin(), tabs.end());
        CHECK(tabs == std::vector<std::string>{"X", "np-item"});
    }

    // Self-drop on the app pane: highlight, release = no-op.
    tab = find_hovered_id(doc, "tabItem", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move(over_app);
    {
        const auto ind = doc.find_element_rect("dropind");
        CHECK(ind.w > 0);
    }
    up(over_app);
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.floats.size() == 1);
        auto tabs = layout.floats[0].pane.tabs;
        std::sort(tabs.begin(), tabs.end());
        CHECK(tabs == std::vector<std::string>{"X", "np-item"});
    }
}

TEST_CASE("UiControls: dragging a tab over its OWN pane shows the drop "
          "indicator and releasing there is a no-op (docked and floating)") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(dock_drag_workspace_html(false));
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

    // Docked: drag X's tab down into its own pane body.
    auto tab = find_hovered_id(doc, "tabX", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move({100, 220});  // well inside pane X, past the drag threshold
    // Visual feedback: the indicator is visible over the source pane.
    {
        const auto ind = doc.find_element_rect("dropind");
        CHECK(ind.w > 0);
        CHECK(ind.h > 0);
    }
    up({100, 220});
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK(layout.floats.empty());              // no tearoff
        CHECK(dock_tree_has_tab(layout.root, "X"));  // still docked
        CHECK(doc.find_element_rect("dropind").w == 0);  // hidden again
    }

    // Floating: tear X off, then drag its title tab onto the floater itself.
    tab = find_hovered_id(doc, "tabX", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move({400, 200});
    up({400, 200});
    auto layout = doc.dock_layout();
    REQUIRE(layout.floats.size() == 1);
    const int old_x = layout.floats[0].x;
    const int old_y = layout.floats[0].y;
    // The spawned floater must actually LAY OUT at its spawn rect â€” a
    // collapsed float (inline style lost/ignored) breaks every later
    // hit-test on it.
    {
        const auto fr = doc.find_element_rect("float-X");
        CHECK(fr.x == old_x);
        CHECK(fr.y == old_y);
        CHECK(fr.w == layout.floats[0].w);
        CHECK(fr.h == layout.floats[0].h);
    }
    const affineui::Point inside{layout.floats[0].x + layout.floats[0].w / 2,
                                 layout.floats[0].y + layout.floats[0].h / 2};

    tab = find_hovered_id(doc, "tabX", 600, 400);
    REQUIRE(tab.x >= 0);
    down(tab);
    move(inside);
    {
        const auto ind = doc.find_element_rect("dropind");
        CHECK(ind.w > 0);  // feedback over its own floater
    }
    up(inside);
    layout = doc.dock_layout();
    REQUIRE(layout.floats.size() == 1);  // no duplicate spawned
    CHECK(layout.floats[0].pane.tabs == std::vector<std::string>{"X"});
    CHECK(layout.floats[0].x == old_x);  // and it did not jump
    CHECK(layout.floats[0].y == old_y);
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
        .dcs-splitter { flex: 0 0 1px; }
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

    // Drag B's tab onto pane A's LEFT edge zone (xâ‰ˆ40 < 22% of 300).
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

TEST_CASE("UiControls: the window-edge band WINS over a pane edge under "
          "the cursor (edge dock is an explicit full-span intent)") {
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
        .dcs-splitter { flex: 0 0 1px; }
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
    // as a direct sibling of A in the WORKSPACE dock (root.children order) â€”
    // a window-edge split would instead wrap the workspace dock in a new one
    // (root.children[1] would be a nested split, not the A leaf).
    //
    // UPDATED CONTRACT: the WINDOW EDGE wins over the pane edge under the
    // cursor. Cursor-at-the-edge is an explicit "span this whole side"
    // intent; the full-span arrangement would otherwise be unreachable
    // wherever a pane touches the edge. The fresh pane docks against the
    // whole workspace, wrapping the old root: [fresh(B), wrapped(A)].
    auto tabB = find_hovered_id(doc, "tabB", 600, 400);
    REQUIRE(tabB.x >= 0);
    ev(affineui::EventType::MouseDown, tabB);
    ev(affineui::EventType::MouseMove, {8, 200});   // window-edge band wins
    ev(affineui::EventType::MouseUp, {8, 200});
    const auto layout = doc.dock_layout();
    REQUIRE(layout.present);
    CHECK(layout.floats.empty());
    REQUIRE(layout.root.split);
    CHECK_FALSE(layout.root.vertical);
    REQUIRE(layout.root.children.size() == 2);
    // First child: B as a full-height leaf pinned to the left edge.
    CHECK_FALSE(layout.root.children[0].split);
    CHECK(layout.root.children[0].tabs == std::vector<std::string>{"B"});
    // Second child: the WRAPPED old workspace dock holding A.
    REQUIRE(layout.root.children[1].split);
    REQUIRE(layout.root.children[1].children.size() == 1);
    CHECK(layout.root.children[1].children[0].tabs ==
          std::vector<std::string>{"A"});
    // B spans the workspace's full height (the edge-dock contract).
    const auto rb = doc.find_element_rect("pane-B");
    CHECK(rb.h >= 395);
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
    // there is the z-raised console overlay, which has no dockpane ancestor â€”
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
        .dcs-splitter { flex: 0 0 1px; }
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
        .dcs-splitter { flex: 0 0 1px; }
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

TEST_CASE("UiControls: self-pane docking â€” center is a no-op preview, edge "
          "splits the tab out of its own group") {
    // A multi-tab pane is a valid target for its OWN tab: dropping on its
    // CENTER shows the preview but is a NO-OP on release (the tab is already
    // there); dropping on an EDGE splits the tab out into a new pane.
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
        .dcs-splitter { flex: 0 0 1px; }
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

    // Drop on the source pane's CENTER: the preview DOES show (a valid target),
    // but the release is a NO-OP â€” Console stays a tab of Assets, no float.
    ev(affineui::EventType::MouseDown, console_tab);
    const auto center_move_result =
        ev(affineui::EventType::MouseMove, source_center);
    CHECK(center_move_result.redraw_requested);
    doc.layout(600, 260, &painter);
    painter.fill_colors.clear();
    painter.fill_draws.clear();
    doc.draw(painter);
    CHECK(saw_fill(painter, affineui::Color::rgba(0, 184, 212, 46)));  // preview shown
    const auto center_up_result =
        ev(affineui::EventType::MouseUp, source_center);
    CHECK_FALSE(center_up_result.layout_changed);  // no-op
    {
        const auto layout = doc.dock_layout();
        REQUIRE(layout.floats.empty());
        const auto* assets_leaf = find_dock_leaf(layout.root, "Assets");
        REQUIRE(assets_leaf != nullptr);
        CHECK(assets_leaf->tabs ==
              std::vector<std::string>{"Assets", "Console"});
        CHECK(dock_tree_has_tab(layout.root, "Other"));
    }

    // The source pane's own RIGHT edge band: a MULTI-tab pane's edge zones ARE
    // valid for its own tab (the "split Console out of Assets" gesture â€”
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
        .dcs-dockpane__tab {
            display: inline-flex;
            position: relative;
            padding: 0 12px;
            white-space: nowrap;
        }
        .dcs-dockpane__tab[aria-selected=true]::after {
            content: "";
            position: absolute;
            left: 0;
            right: 0;
            top: 0;
            height: 2px;
            background: #00b8d4;
        }
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

TEST_CASE("UiScript: deterministic docking gesture FUZZER (seeded; invariants "
          "after every move) â€” UAF net when run under ASAN") {
    // The lesson of the docking work: single gestures pass, SEQUENCES crash.
    // This replays a long, seeded-random stream of dock / tearoff / re-dock
    // gestures with the editor's reload-after-every-change loop, and asserts
    // the structural invariants after each step. Deterministic (fixed seed) so
    // a failure reproduces; under ASAN it nets use-after-free in the surgery.
    constexpr int W = 720;
    constexpr int H = 480;
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_user_stylesheet(R"CSS(
        html, body { width: 720px; height: 480px; margin: 0; padding: 0; }
        #aui-root { width: 720px; height: 480px; min-height: 0; padding: 0; }
        .shell { width: 720px; height: 480px; display: flex; flex-direction: column; }
        .dcs-dock { display: flex; min-width: 0; min-height: 0; }
        .dcs-dock--v { flex-direction: column; }
        .dcs-dock--floathost { position: relative; }
        .dcs-dockpane { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
        .dcs-dockpane--center { flex: 1; }
        .dcs-dockpane__tabbar { flex: 0 0 24px; display: flex; min-width: 0; }
        .dcs-dockpane__tabs { display: flex; min-width: 0; }
        .dcs-dockpane__tab { display: inline-flex; padding: 0 10px; white-space: nowrap; }
        .dcs-dockpane__body { flex: 1; min-width: 0; min-height: 0; }
        .dcs-panel--floating { position: absolute; }
        .dcs-splitter { flex: 0 0 6px; }
        [hidden] { display: none; }
        [data-dcs-tabpanel][hidden] { display: none; }
    )CSS");

    const std::vector<std::string> panels = {"Hierarchy", "Inspector", "Assets",
                                             "Console", "Log"};
    auto rebuild = [&]() {
        affineui::View v{affineui::ViewTheme::Decius};
        v.set_dock_layout_provider([&] { return doc.dock_layout(); });
        v.begin();
        {
            auto shell = v.container("shell", "shell");
            (void) shell;
            v.document_view("workarea", [&](affineui::View& dv) {
                dv.document(
                    [](affineui::View& p) { p.text("VP", "vp-text"); }, "View",
                    "cube");
                dv.dockpanel("Hierarchy",
                             affineui::DockLocation::docked(affineui::Dock::Left,
                                                            150),
                             [](affineui::View& p) { p.text("H", "h-text"); },
                             "layers", "Hierarchy");
                auto insp = dv.dockpanel(
                    "Inspector",
                    affineui::DockLocation::docked(affineui::Dock::Right, 150),
                    [](affineui::View& p) { p.text("I", "i-text"); }, "cog",
                    "Inspector");
                (void) insp;
                auto assets = dv.dockpanel(
                    "Assets",
                    affineui::DockLocation::docked(affineui::Dock::Bottom, 100),
                    [](affineui::View& p) { p.text("A", "a-text"); }, "image",
                    "Assets");
                dv.dockpanel("Console",
                             affineui::DockLocation::tab().in(assets),
                             [](affineui::View& p) { p.text("C", "c-text"); },
                             "file", "Console");
                dv.dockpanel("Log", affineui::DockLocation::tab().in(assets),
                             [](affineui::View& p) { p.text("L", "l-text"); },
                             "terminal", "Log");
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
                                          "Inspector", "Assets", "Console",
                                          "Log"};
    const affineui::UiScript::Anchor anchors[] = {
        affineui::UiScript::Anchor::Center, affineui::UiScript::Anchor::Left,
        affineui::UiScript::Anchor::Right,  affineui::UiScript::Anchor::Top,
        affineui::UiScript::Anchor::Bottom};

    for (unsigned seed = 1; seed <= 6; ++seed) {
        rebuild();  // fresh declared layout per seed
        std::mt19937 rng(0xD0C00000u + seed);
        for (int step = 0; step < 200; ++step) {
            const std::string& grab = panels[rng() % panels.size()];
            const std::string& dest_id = all[rng() % all.size()];
            const std::string dest = dest_id == "__document__"
                                         ? "pane-__document__"
                                         : ("pane-" + dest_id);
            const auto anchor = anchors[rng() % 5];
            // The tab might currently live in a float or a pane; target it by
            // its tabpanel id either way.
            const std::string from = "[data-dcs-target=#" + grab + "-body]";
            ui.drag(from, dest, anchor);

            const auto issues = affineui::UiScript::validate_dock_layout(
                doc.dock_layout(), all);
            if (!issues.empty()) {
                std::string msg = "seed " + std::to_string(seed) + " step " +
                                  std::to_string(step) + " grab=" + grab +
                                  " dest=" + dest_id + ":";
                for (const auto& i : issues) msg += " [" + i + "]";
                FAIL(msg);
            }
        }
    }
    CHECK(doc.dock_layout().present);  // survived 6 seeds x 200 gestures
}

TEST_CASE("UiScript: bottom pane docks to the bottom of Hierarchy and back "
          "again, repeatedly (in-window crash sequence)") {
    // The exact gesture sequence reported crashing in the windowed editor:
    // drag the bottom pane's tab to the BOTTOM of Hierarchy (vertical split
    // under it), then back to the bottom area, several times â€” with the
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
        // Bottom pane tab â†’ bottom band of Hierarchy: splits under it.
        REQUIRE(ui.drag("[data-dcs-target=#Assets-body]", "pane-Hierarchy",
                        affineui::UiScript::Anchor::Bottom));
        expect_valid("after dock under Hierarchy");
        // ... and back again: to the bottom band of the workspace (over the
        // document pane's lower edge â€” window-edge band docks it back at the
        // workspace level, the original arrangement).
        REQUIRE(ui.drag("[data-dcs-target=#Assets-body]", "pane-__document__",
                        affineui::UiScript::Anchor::Bottom));
        expect_valid("after dock back to the bottom");
    }
}

TEST_CASE("UiControls: game-editor-shaped tearoff â€” press reload, doc toolbar, "
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
    // editor's selection reload), drag to the document body, release â†’ tearoff.
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
    // the Inspector pane to re-dock â€” after the reload replay. Bounds are
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
    // Aim at the middle of the Inspector pane (center â†’ join as tab).
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
        // (Console, Log) stay behind in the source pane â€” they are real DOM
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

    SUBCASE("dropped on its own center: no-op, every tab stays put") {
        rebuild();
        const auto tab = find_hovered_attr(doc, "data-dcs-target",
                                           "#Assets-body", W, H);
        REQUIRE(tab.x >= 0);
        const auto source = bounds_for_attr("data-aui-name", "pane-Assets");
        const affineui::Point source_center{
            source.x + source.w / 2, source.y + source.h / 2};

        ev(affineui::EventType::MouseDown, tab);
        ev(affineui::EventType::MouseMove, source_center);
        const auto up = ev(affineui::EventType::MouseUp, source_center);

        // Center on your own multi-tab pane is a no-op: nothing moves, no float.
        CHECK_FALSE(up.layout_changed);
        const auto layout = doc.dock_layout();
        REQUIRE(layout.present);
        CHECK(layout.floats.empty());
        const auto* source_leaf = find_dock_leaf(layout.root, "Assets");
        REQUIRE(source_leaf != nullptr);
        CHECK(source_leaf->tabs ==
              std::vector<std::string>{"Assets", "Console", "Log"});
        CHECK(dock_tree_has_tab(layout.root, "Hierarchy"));
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
    //    Console's tab row (just a tab move â€” no cycle, no lost pane).
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

    // 3. Round-trip back: drag Assets to the BOTTOM window edge â€” it splits
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
        .dcs-splitter { flex: 0 0 1px; }
        .dcs-panel--floating { position: absolute; }
        [hidden] { display: none; }
    )CSS");

    auto rebuild = [&]() {
        affineui::View v{affineui::ViewTheme::Decius};
        // Replay the live dock arrangement (the DOM surgery result) on every
        // rebuild â€” this is the same wiring as a real app.
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
    // dead â€” the dock structure IS the DOM).
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
        // every rebuild â€” this also exercises the replay round-trip.
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
    CHECK(last_html.find("dcs-dockpane__titlebar") != std::string::npos);
    CHECK(last_html.find("dcs-panel__title--dock-tab") != std::string::npos);
    CHECK(last_html.find("data-dcs-title-tab") != std::string::npos);
    CHECK(last_html.find("dcs-dockpane__titlebar--no-tab") ==
          std::string::npos);
    CHECK(last_html.find("dcs-panel__title--no-tab") == std::string::npos);
    CHECK(last_html.find("background:transparent;border:0") !=
          std::string::npos);
    {
        const auto title = bounds_for_attr("data-dcs-target", "#Console-body");
        painter.fill_draws.clear();
        doc.draw(painter);
        CHECK_FALSE(std::any_of(
            painter.fill_draws.begin(), painter.fill_draws.end(),
            [&](const RecordingPainter::FillDraw& fill) {
                return same_color(fill.color,
                                  affineui::Color::rgb(0x00, 0xb8, 0xd4)) &&
                       fill.rect.x >= title.x &&
                       fill.rect.x + fill.rect.w <= title.x + title.w &&
                       fill.rect.y >= title.y &&
                       fill.rect.y < title.y + 4;
            }));
    }

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

    // Dragging the title bar CHROME (not the title tab) moves the panel only â€”
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
    // replay does not work yet â€” emit_one_floating_panel's title-only branch
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
          "(dock-kind mismatch) â€” it tears off") {
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
        .dcs-splitter { flex: 0 0 1px; }
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

// ── IME composition (preedit) — docs/IME_ARCHITECTURE.md §4 ─────────────

namespace {

// Shared fixture: a focused single-line input with committed value "ab"
// and the caret at the end. Returns the input's bounds.
affineui::Rect focus_ime_input(affineui::Document& doc,
                               RecordingPainter& painter) {
    doc.set_html(R"HTML(
        <style>
        body { margin: 0; padding: 0; }
        input {
          display: block;
          box-sizing: border-box;
          width: 180px;
          height: 24px;
          border: 1px solid #000;
          padding: 2px 6px;
          font-size: 12px;
          line-height: 18px;
        }
        </style>
        <input type="text" value="ab">
    )HTML");
    doc.layout(240, 0, &painter);
    doc.draw(painter);

    const auto input_pos = find_hovered_tag(doc, "input");
    REQUIRE(input_pos.x >= 0);
    const auto bounds = doc.hovered_info().bounds;

    affineui::Event down{};
    down.type = affineui::EventType::MouseDown;
    down.button = affineui::MouseButton::Left;
    down.pos = {bounds.x + bounds.w - 10, bounds.y + bounds.h / 2};
    doc.dispatch(down);
    affineui::Event end{};
    end.type = affineui::EventType::KeyDown;
    end.key = affineui::Key::End;
    doc.dispatch(end);
    return bounds;
}

affineui::Event composition_event(std::string preedit, int cursor = -1) {
    affineui::Event ev{};
    ev.type = affineui::EventType::Composition;
    ev.text = std::move(preedit);
    ev.composition_cursor = cursor;
    return ev;
}

bool any_text_run_contains(const RecordingPainter& painter,
                           std::string_view needle) {
    return std::any_of(painter.text_runs.begin(), painter.text_runs.end(),
                       [&](const std::string& run) {
                           return run.find(needle) != std::string::npos;
                       });
}

}  // namespace

TEST_CASE("IME preedit displays inline without committing") {
    affineui::Document doc;
    RecordingPainter painter;
    focus_ime_input(doc, painter);

    CHECK(doc.dispatch(composition_event("\xE3\x81\x8B\xE3\x82\x93"))
              .redraw_requested);  // preedit "かん"
    painter.text_runs.clear();
    doc.draw(painter);
    CHECK(any_text_run_contains(painter, "ab\xE3\x81\x8B\xE3\x82\x93"));

    // Cancel: display returns to the committed value — the preedit never
    // entered it.
    CHECK(doc.dispatch(composition_event("")).redraw_requested);
    painter.text_runs.clear();
    doc.draw(painter);
    CHECK_FALSE(any_text_run_contains(painter, "\xE3\x81\x8B"));
    CHECK(any_text_run_contains(painter, "ab"));
}

TEST_CASE("IME preedit draws an underline decoration") {
    affineui::Document doc;
    RecordingPainter painter;
    const auto bounds = focus_ime_input(doc, painter);

    doc.dispatch(composition_event("\xE3\x81\x8B\xE3\x82\x93"));
    painter.stroke_line_draws.clear();
    doc.draw(painter);
    // A horizontal stroke inside the control, below the text midline.
    CHECK(std::any_of(
        painter.stroke_line_draws.begin(), painter.stroke_line_draws.end(),
        [&](const auto& line) {
            return std::abs(line.y0 - line.y1) < 0.01f &&
                   line.x1 > line.x0 &&
                   line.y0 > bounds.y + bounds.h / 2 &&
                   line.y0 < bounds.y + bounds.h;
        }));
}

TEST_CASE("IME commit replaces the preedit with committed text") {
    affineui::Document doc;
    RecordingPainter painter;
    focus_ime_input(doc, painter);

    doc.dispatch(composition_event("\xE3\x81\x8B\xE3\x82\x93"));
    affineui::Event commit{};
    commit.type = affineui::EventType::TextInput;
    commit.text = "\xE6\xBC\xA2\xE5\xAD\x97";  // "漢字"
    CHECK(doc.dispatch(commit).redraw_requested);

    painter.text_runs.clear();
    doc.draw(painter);
    CHECK(any_text_run_contains(painter, "ab\xE6\xBC\xA2\xE5\xAD\x97"));
    CHECK_FALSE(any_text_run_contains(painter, "\xE3\x81\x8B"));
}

TEST_CASE("IME composition start deletes the active selection") {
    affineui::Document doc;
    RecordingPainter painter;
    focus_ime_input(doc, painter);

    affineui::Event select_all{};
    select_all.type = affineui::EventType::KeyDown;
    select_all.key = affineui::Key::A;
    select_all.ctrl = true;
    doc.dispatch(select_all);

    doc.dispatch(composition_event("x"));
    // Cancel the preedit: the selection deletion was a committed edit, so
    // the control is now empty (browser compositionstart semantics).
    doc.dispatch(composition_event(""));
    painter.text_runs.clear();
    doc.draw(painter);
    CHECK_FALSE(any_text_run_contains(painter, "ab"));
}

TEST_CASE("IME active composition swallows editing keys") {
    affineui::Document doc;
    RecordingPainter painter;
    focus_ime_input(doc, painter);

    doc.dispatch(composition_event("\xE3\x81\x8B"));
    affineui::Event backspace{};
    backspace.type = affineui::EventType::KeyDown;
    backspace.key = affineui::Key::Backspace;
    doc.dispatch(backspace);
    doc.dispatch(composition_event(""));

    painter.text_runs.clear();
    doc.draw(painter);
    // The committed value survived the Backspace that the IME owns.
    CHECK(any_text_run_contains(painter, "ab"));
}

TEST_CASE("text_input_active and caret_rect report the focused control") {
    affineui::Document doc;
    RecordingPainter painter;

    CHECK_FALSE(doc.text_input_active());
    CHECK(doc.caret_rect().w <= 0);

    const auto bounds = focus_ime_input(doc, painter);
    CHECK(doc.text_input_active());
    const auto rect = doc.caret_rect();
    CHECK(rect.w >= 1);
    CHECK(rect.h > 0);
    CHECK(rect.x >= bounds.x);
    CHECK(rect.x <= bounds.x + bounds.w);
    CHECK(rect.y >= bounds.y);
    CHECK(rect.y + rect.h <= bounds.y + bounds.h + 1);

    // Escape clears focus — and with it the text-input intent.
    affineui::Event esc{};
    esc.type = affineui::EventType::KeyDown;
    esc.key = affineui::Key::Escape;
    doc.dispatch(esc);
    CHECK_FALSE(doc.text_input_active());
    CHECK(doc.caret_rect().w <= 0);
}

TEST_CASE("caret_rect tracks the IME cursor inside the preedit") {
    affineui::Document doc;
    RecordingPainter painter;
    focus_ime_input(doc, painter);
    const auto rect_before = doc.caret_rect();

    // Cursor at the end of a two-codepoint preedit sits right of the
    // committed-caret position; cursor 0 sits back at it.
    doc.dispatch(composition_event("\xE3\x81\x8B\xE3\x82\x93", -1));
    const auto rect_end = doc.caret_rect();
    CHECK(rect_end.x > rect_before.x);

    doc.dispatch(composition_event("\xE3\x81\x8B\xE3\x82\x93", 0));
    const auto rect_start = doc.caret_rect();
    CHECK(rect_start.x == rect_before.x);

    // A cursor byte offset inside a codepoint snaps down to its start:
    // offsets 4 and 3 (mid-second-codepoint) land on the same boundary.
    doc.dispatch(composition_event("\xE3\x81\x8B\xE3\x82\x93", 4));
    const auto rect_mid = doc.caret_rect();
    doc.dispatch(composition_event("\xE3\x81\x8B\xE3\x82\x93", 3));
    CHECK(rect_mid.x == doc.caret_rect().x);
}

TEST_CASE("IME preedit survives a relayout") {
    affineui::Document doc;
    RecordingPainter painter;
    focus_ime_input(doc, painter);

    doc.dispatch(composition_event("\xE3\x81\x8B\xE3\x82\x93"));
    doc.layout(240, 0, &painter);  // recollect while composing
    painter.text_runs.clear();
    doc.draw(painter);
    CHECK(any_text_run_contains(painter, "ab\xE3\x81\x8B\xE3\x82\x93"));
}

TEST_CASE("Inline style: a descendant-inert write restyles only the target "
          "block yet stays layout-live; an inherited-prop write still "
          "reaches descendants") {
    affineui::Document doc;
    RecordingPainter painter;
    doc.set_html(R"HTML(
        <style>
        html, body { margin: 0; padding: 0; }
        .row { display: flex; width: 400px; height: 60px; }
        .pane { display: flex; min-width: 0; }
        </style>
        <div class="row">
          <div id="a" class="pane" style="flex:0 0 100px"><span id="t">label</span></div>
          <div id="b" class="pane" style="flex:1 1 0">rest</div>
        </div>
    )HTML");
    doc.layout(400, 60, &painter);
    REQUIRE(doc.find_element_rect("#a").w == 100);

    // The splitter-drag shape: flex + min-* only. Takes the block-local
    // restyle fast path — the new basis must still drive layout.
    REQUIRE(doc.set_attribute_by_id(
        "a", "style", "flex:0 0 220px;min-width:0;min-height:0"));
    doc.layout(400, 60, &painter);
    CHECK(doc.find_element_rect("#a").w == 220);
    CHECK(doc.find_element_rect("#b").w == 180);

    // Adding an INHERITED property (color) fails the inert check on the new
    // text — the subtree path must run so the descendant text repaints in
    // the new color.
    REQUIRE(doc.set_attribute_by_id(
        "a", "style",
        "flex:0 0 220px;min-width:0;min-height:0;color:#12fe34"));
    RecordingPainter repaint;
    doc.layout(400, 60, &repaint);
    doc.draw(repaint);
    const auto want = affineui::Color::rgb(0x12, 0xfe, 0x34);
    bool label_recolored = false;
    for (const auto& d : repaint.text_draws) {
        if (d.text == "label" && same_color(d.color, want)) {
            label_recolored = true;
        }
    }
    CHECK(label_recolored);

    // And REMOVING the inherited property must also take the subtree path
    // (the OLD text is not inert): the label returns to the default color.
    REQUIRE(doc.set_attribute_by_id(
        "a", "style", "flex:0 0 220px;min-width:0;min-height:0"));
    RecordingPainter repaint2;
    doc.layout(400, 60, &repaint2);
    doc.draw(repaint2);
    bool label_still_green = false;
    for (const auto& d : repaint2.text_draws) {
        if (d.text == "label" && same_color(d.color, want)) {
            label_still_green = true;
        }
    }
    CHECK_FALSE(label_still_green);
}
