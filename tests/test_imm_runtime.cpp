#include <doctest/doctest.h>

#include "affineui/document.h"
#include "affineui/imm.h"
#include "affineui/painter.h"
#include "affineui/ui.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace ui = affineui::imm;

namespace {

class TestPainter final : public affineui::Painter {
public:
    void begin_frame(int, int, float) override {}
    void end_frame() override {}
    void fill_rect(const affineui::Rect&, affineui::Color) override {}
    void stroke_rect(const affineui::Rect&, affineui::Color, float) override {}
    void fill_rounded_rect(const affineui::Rect&, float, affineui::Color) override {}
    void stroke_rounded_rect(const affineui::Rect&, float, affineui::Color, float) override {}
    void fill_rounded_rect_varying(const affineui::Rect&, float, float, float, float, affineui::Color) override {}
    void stroke_rounded_rect_varying(const affineui::Rect&, float, float, float, float, affineui::Color, float) override {}
    void fill_linear_gradient_rect(const affineui::Rect&, float, affineui::Color, affineui::Color,
                                   float, float, float, float) override {}
    void fill_radial_gradient_rect(const affineui::Rect&, affineui::Color, affineui::Color,
                                   float, float, float, float) override {}
    std::uint32_t resolve_font(std::string_view, int, int, bool) override { return 1; }
    int measure_text(std::uint32_t, std::string_view text) override {
        return static_cast<int>(text.size()) * 8;
    }
    TextMetrics text_metrics(std::uint32_t) override { return {12.0f, 4.0f, 18.0f}; }
    void draw_text(std::uint32_t, const affineui::Point&, std::string_view, affineui::Color) override {}
    affineui::Size measure_text_box(std::uint32_t, std::string_view text, float max_width, float) override {
        const int natural = static_cast<int>(text.size()) * 8;
        return {natural < static_cast<int>(max_width) ? natural : static_cast<int>(max_width), 18};
    }
    void draw_text_box(std::uint32_t, const affineui::Point&, std::string_view, affineui::Color, float, float) override {}
    std::uint32_t load_image(std::string_view) override { return 0; }
    affineui::Size image_size(std::uint32_t) override { return {}; }
    void draw_image(std::uint32_t, const affineui::Rect&, const affineui::Rect&) override {}
    void push_clip(const affineui::Rect&) override {}
    void pop_clip() override {}
};

std::string imm_id(ui::CallSite here) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "aui-imm-%016llx",
                  static_cast<unsigned long long>(here.hash()));
    return std::string(buf);
}

}  // namespace

TEST_CASE("imm clear-and-rebuild survives repeated css-backed dom replacement") {
    affineui::Document doc;
    doc.set_user_stylesheet(R"(
        body { color: #cdd6f4; }
        .card { color: #f38ba8; padding: 4px; }
        button { color: #11111b; background-color: #f38ba8; }
    )");

    constexpr ui::CallSite state_site{"imm-runtime-test", 7, 2};
    constexpr ui::CallSite button_site{"imm-runtime-test", 10, 2};
    int rendered_count = -1;

    doc.set_imm_view([&] {
        if (auto card = ui::div("card", ui::CallSite{"imm-runtime-test", 8, 2})) {
            auto count = ui::use_state(0, state_site);
            rendered_count = count.get();
            ui::p("", ui::CallSite{"imm-runtime-test", 9, 2}).text(std::to_string(count.get()));
            ui::button("Increment", "", button_site).on_click([count]() mutable {
                count = count.get() + 1;
            });
        }
    });

    CHECK(doc.imm_dirty());
    doc.tick_imm();
    CHECK_FALSE(doc.imm_dirty());
    CHECK(rendered_count == 0);

    for (int i = 0; i < 8; ++i) {
        CHECK(doc.invoke_imm_click(imm_id(button_site)));
        CHECK(doc.imm_dirty());
        doc.tick_imm();
        CHECK(rendered_count == i + 1);
    }

    CHECK_FALSE(doc.imm_dirty());
    CHECK(rendered_count == 8);
}

TEST_CASE("imm counter works through layout hit testing and Ui dispatch") {
    affineui::Ui app;
    TestPainter painter;
    int rendered_count = -1;

    app.css(R"(
        body { color: #cdd6f4; }
        .card { color: #f38ba8; padding: 24px; margin: 24px; }
        button { color: #11111b; background-color: #f38ba8; padding: 12px 24px; }
    )");

    app.mount([&] {
        if (auto card = ui::div("card", ui::CallSite{"imm-ui-test", 1, 1})) {
            auto count = ui::use_state(0, ui::CallSite{"imm-ui-test", 2, 1});
            rendered_count = count.get();
            ui::h1("", ui::CallSite{"imm-ui-test", 3, 1}).text("Counter");
            ui::p("", ui::CallSite{"imm-ui-test", 4, 1}).text(std::to_string(count.get()));
            ui::button("Increment", "", ui::CallSite{"imm-ui-test", 5, 1}).on_click([count]() mutable {
                count = count.get() + 1;
            });
        }
    });

    auto render_headless = [&] {
        app.document().tick_imm();
        app.document().layout(520, /*viewport_height=*/0, &painter);
    };
    auto find_button_point = [&]() {
        for (int y = 0; y < 360; y += 4) {
            for (int x = 0; x < 520; x += 4) {
                affineui::Event move{};
                move.type = affineui::EventType::MouseMove;
                move.pos = {x, y};
                app.dispatch(move);
                if (app.document().hovered_info().tag == "button") {
                    return affineui::Point{x, y};
                }
            }
        }
        return affineui::Point{-1, -1};
    };

    render_headless();
    CHECK(rendered_count == 0);

    for (int i = 0; i < 8; ++i) {
        const auto button_pos = find_button_point();
        REQUIRE(button_pos.x >= 0);

        affineui::Event up{};
        up.type = affineui::EventType::MouseUp;
        up.button = affineui::MouseButton::Left;
        up.pos = button_pos;
        CHECK(app.dispatch(up));

        render_headless();
        CHECK(rendered_count == i + 1);
    }
}
