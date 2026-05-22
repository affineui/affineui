#include <doctest/doctest.h>

#include "affineui/document.h"
#include "affineui/painter.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

class RecordingPainter final : public affineui::Painter {
public:
    std::vector<affineui::Color> fill_colors;
    std::vector<affineui::Color> stroke_colors;
    std::vector<std::string> text_runs;
    std::vector<std::string> image_urls;
    std::vector<affineui::Rect> image_draws;

    void begin_frame(int, int, float) override {}
    void end_frame() override {}
    void fill_rect(const affineui::Rect&, affineui::Color color) override {
        fill_colors.push_back(color);
    }
    void stroke_rect(const affineui::Rect&, affineui::Color color, float) override {
        stroke_colors.push_back(color);
    }
    void stroke_line(float, float, float, float, affineui::Color color, float) override {
        stroke_colors.push_back(color);
    }
    void fill_circle(float, float, float, affineui::Color color) override {
        fill_colors.push_back(color);
    }
    void fill_rounded_rect(const affineui::Rect&, float,
                           affineui::Color color) override {
        fill_colors.push_back(color);
    }
    void stroke_rounded_rect(const affineui::Rect&, float, affineui::Color color, float) override {
        stroke_colors.push_back(color);
    }
    void fill_rounded_rect_varying(const affineui::Rect&, float, float,
                                   float, float,
                                   affineui::Color color) override {
        fill_colors.push_back(color);
    }
    void stroke_rounded_rect_varying(const affineui::Rect&, float, float, float, float,
                                     affineui::Color color, float) override {
        stroke_colors.push_back(color);
    }
    void fill_linear_gradient_rect(const affineui::Rect&, float,
                                   affineui::Color c0, affineui::Color,
                                   float, float, float, float) override {
        fill_colors.push_back(c0);
    }
    void fill_radial_gradient_rect(const affineui::Rect&,
                                   affineui::Color c0, affineui::Color,
                                   float, float, float, float) override {
        fill_colors.push_back(c0);
    }
    std::uint32_t resolve_font(std::string_view, int, int, bool) override { return 1; }
    int measure_text(std::uint32_t, std::string_view text) override {
        return static_cast<int>(text.size()) * 8;
    }
    TextMetrics text_metrics(std::uint32_t) override { return {12.0f, 4.0f, 18.0f}; }
    void draw_text(std::uint32_t, const affineui::Point&, std::string_view,
                   affineui::Color) override {}
    affineui::Size measure_text_box(std::uint32_t, std::string_view text,
                                    float max_width, float) override {
        const int natural = static_cast<int>(text.size()) * 8;
        return {natural < static_cast<int>(max_width)
                    ? natural
                    : static_cast<int>(max_width),
                18};
    }
    void draw_text_box(std::uint32_t, const affineui::Point&, std::string_view text,
                       affineui::Color, float, float) override {
        text_runs.emplace_back(text);
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
    void push_clip(const affineui::Rect&) override {}
    void pop_clip() override {}
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

TEST_CASE("dispatch of a no-op event returns a quiescent result") {
    affineui::Document doc;
    affineui::Event ev{};
    auto r = doc.dispatch(ev);
    CHECK(r.redraw_requested == false);
    CHECK(r.invalidate_view == false);
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
