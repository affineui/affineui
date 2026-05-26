#include <doctest/doctest.h>

#include "internal/display_list_painter.h"

namespace {

class CountingPainter final : public affineui::Painter {
public:
    int fill_rects{0};
    int clips{0};
    int transforms{0};

    void begin_frame(int, int, float) override {}
    void end_frame() override {}

    void fill_rect(const affineui::Rect&, affineui::Color) override {
        ++fill_rects;
    }
    void stroke_rect(const affineui::Rect&, affineui::Color, float) override {}
    void stroke_line(float, float, float, float, affineui::Color, float) override {}
    void fill_circle(float, float, float, affineui::Color) override {}
    void stroke_arc(float, float, float, float, float, affineui::Color, float) override {}
    void fill_rounded_rect(const affineui::Rect&, float, affineui::Color) override {}
    void stroke_rounded_rect(const affineui::Rect&, float, affineui::Color, float) override {}
    void fill_rounded_rect_varying(const affineui::Rect&, float, float, float, float,
                                   affineui::Color) override {}
    void stroke_rounded_rect_varying(const affineui::Rect&, float, float, float, float,
                                     affineui::Color, float) override {}
    void fill_linear_gradient_rect(const affineui::Rect&, float, affineui::Color,
                                   affineui::Color, float, float, float, float) override {}
    void fill_radial_gradient_rect(const affineui::Rect&, affineui::Color,
                                   affineui::Color, float, float, float, float,
                                   float, float, float) override {}
    void fill_box_shadow(const affineui::Rect&, float, affineui::Color,
                         float, float, float, float, bool) override {}
    std::uint32_t resolve_font(std::string_view, int, int, bool) override { return 0; }
    int measure_text(std::uint32_t, std::string_view) override { return 0; }
    TextMetrics text_metrics(std::uint32_t) override { return {}; }
    void draw_text(std::uint32_t, const affineui::Point&, std::string_view,
                   affineui::Color) override {}
    affineui::Size measure_text_box(std::uint32_t, std::string_view, float,
                                    float, float) override { return {}; }
    void draw_text_box(std::uint32_t, const affineui::Point&, std::string_view,
                       affineui::Color, float, float, float, TextAlign) override {}
    std::uint32_t load_image(std::string_view) override { return 0; }
    affineui::Size image_size(std::uint32_t) override { return {}; }
    void draw_image(std::uint32_t, const affineui::Rect&,
                    const affineui::Rect&) override {}
    void push_clip(const affineui::Rect&) override { ++clips; }
    void pop_clip() override {}
    void push_alpha(float) override {}
    void pop_alpha() override {}
    void push_transform(const affineui::Mat2x3&) override { ++transforms; }
    void pop_transform() override {}
};

affineui::detail::PaintOp fill_rect(int x, int y, int w, int h) {
    affineui::detail::PaintOp op{};
    op.kind = affineui::detail::PaintOpKind::FillRect;
    op.p.fill_rect.x = static_cast<std::int16_t>(x);
    op.p.fill_rect.y = static_cast<std::int16_t>(y);
    op.p.fill_rect.w = static_cast<std::int16_t>(w);
    op.p.fill_rect.h = static_cast<std::int16_t>(h);
    return op;
}

affineui::detail::PaintOp push_clip(int x, int y, int w, int h) {
    affineui::detail::PaintOp op{};
    op.kind = affineui::detail::PaintOpKind::PushClip;
    op.p.clip.x = static_cast<std::int16_t>(x);
    op.p.clip.y = static_cast<std::int16_t>(y);
    op.p.clip.w = static_cast<std::int16_t>(w);
    op.p.clip.h = static_cast<std::int16_t>(h);
    return op;
}

affineui::detail::PaintOp pop_clip() {
    affineui::detail::PaintOp op{};
    op.kind = affineui::detail::PaintOpKind::PopClip;
    return op;
}

affineui::detail::PaintOp push_transform(const affineui::Mat2x3& m) {
    affineui::detail::PaintOp op{};
    op.kind = affineui::detail::PaintOpKind::PushTransform;
    op.p.push_transform.a = m.a;
    op.p.push_transform.b = m.b;
    op.p.push_transform.c = m.c;
    op.p.push_transform.d = m.d;
    op.p.push_transform.tx = m.tx;
    op.p.push_transform.ty = m.ty;
    return op;
}

affineui::detail::PaintOp pop_transform() {
    affineui::detail::PaintOp op{};
    op.kind = affineui::detail::PaintOpKind::PopTransform;
    return op;
}

}  // namespace

TEST_CASE("clipped replay culls paint inside translated transform") {
    affineui::detail::DisplayList list;
    list.ops.push_back(push_transform(affineui::Mat2x3::translate(1000.0f, 0.0f)));
    list.ops.push_back(fill_rect(0, 0, 10, 10));
    list.ops.push_back(pop_transform());

    CountingPainter painter;
    const auto stats = affineui::detail::replay_clipped(
        list, painter, affineui::Rect{0, 0, 100, 100});

    CHECK(painter.transforms == 0);
    CHECK(painter.fill_rects == 0);
    CHECK(stats.culled == 3);
}

TEST_CASE("clipped replay skips transformed clip subtree outside dirty rect") {
    affineui::detail::DisplayList list;
    list.ops.push_back(push_transform(affineui::Mat2x3::translate(1000.0f, 0.0f)));
    list.ops.push_back(push_clip(0, 0, 20, 20));
    list.ops.push_back(fill_rect(0, 0, 10, 10));
    list.ops.push_back(pop_clip());
    list.ops.push_back(pop_transform());

    CountingPainter painter;
    const auto stats = affineui::detail::replay_clipped(
        list, painter, affineui::Rect{0, 0, 100, 100});

    CHECK(painter.transforms == 0);
    CHECK(painter.clips == 0);
    CHECK(painter.fill_rects == 0);
    CHECK(stats.culled == 5);
}

TEST_CASE("clipped replay composes nested transform bounds like the painter") {
    affineui::detail::DisplayList list;
    list.ops.push_back(push_transform(affineui::Mat2x3::translate(10.0f, 0.0f)));
    list.ops.push_back(push_transform(affineui::Mat2x3::scale(2.0f, 2.0f)));
    list.ops.push_back(fill_rect(0, 0, 5, 5));
    list.ops.push_back(pop_transform());
    list.ops.push_back(pop_transform());

    CountingPainter painter;
    const auto stats = affineui::detail::replay_clipped(
        list, painter, affineui::Rect{21, 0, 8, 16});

    CHECK(painter.transforms == 2);
    CHECK(painter.fill_rects == 1);
    CHECK(stats.culled == 0);
}

TEST_CASE("display list prepares transform range bounds for replay") {
    affineui::detail::DisplayList list;
    list.ops.push_back(push_transform(affineui::Mat2x3::translate(1000.0f, 0.0f)));
    list.ops.push_back(fill_rect(0, 0, 10, 10));
    list.ops.push_back(pop_transform());

    affineui::detail::prepare_replay_metadata(list);

    REQUIRE(list.transform_ranges.size() == list.ops.size());
    CHECK(list.transform_ranges[0].pop_index == 2);
    CHECK(list.transform_ranges[0].bounds_known == 1);

    CountingPainter painter;
    const auto stats = affineui::detail::replay_clipped(
        list, painter, affineui::Rect{0, 0, 100, 100});

    CHECK(painter.transforms == 0);
    CHECK(painter.fill_rects == 0);
    CHECK(stats.culled == 3);
}

TEST_CASE("display list prepares clip range jumps for replay") {
    affineui::detail::DisplayList list;
    list.ops.push_back(push_clip(1000, 0, 20, 20));
    list.ops.push_back(fill_rect(1000, 0, 10, 10));
    list.ops.push_back(pop_clip());

    affineui::detail::prepare_replay_metadata(list);

    REQUIRE(list.clip_ranges.size() == list.ops.size());
    CHECK(list.clip_ranges[0].pop_index == 2);

    CountingPainter painter;
    const auto stats = affineui::detail::replay_clipped(
        list, painter, affineui::Rect{0, 0, 100, 100});

    CHECK(painter.clips == 0);
    CHECK(painter.fill_rects == 0);
    CHECK(stats.culled == 3);
}

TEST_CASE("display list prepares nested clip range jumps") {
    affineui::detail::DisplayList list;
    list.ops.push_back(push_clip(1000, 0, 40, 40));
    list.ops.push_back(push_clip(1000, 0, 20, 20));
    list.ops.push_back(fill_rect(1000, 0, 10, 10));
    list.ops.push_back(pop_clip());
    list.ops.push_back(pop_clip());

    affineui::detail::prepare_replay_metadata(list);

    CHECK(list.clip_ranges[0].pop_index == 4);
    CHECK(list.clip_ranges[1].pop_index == 3);

    CountingPainter painter;
    const auto stats = affineui::detail::replay_clipped(
        list, painter, affineui::Rect{0, 0, 100, 100});

    CHECK(painter.clips == 0);
    CHECK(painter.fill_rects == 0);
    CHECK(stats.culled == 5);
}

TEST_CASE("clipped replay still handles unprepared clip ranges") {
    affineui::detail::DisplayList list;
    list.ops.push_back(push_clip(1000, 0, 20, 20));
    list.ops.push_back(fill_rect(1000, 0, 10, 10));
    list.ops.push_back(pop_clip());

    CountingPainter painter;
    const auto stats = affineui::detail::replay_clipped(
        list, painter, affineui::Rect{0, 0, 100, 100});

    CHECK(painter.clips == 0);
    CHECK(painter.fill_rects == 0);
    CHECK(stats.culled == 3);
}

TEST_CASE("clipped replay advances after culling a single paint op") {
    affineui::detail::DisplayList list;
    list.ops.push_back(fill_rect(1000, 0, 10, 10));
    list.ops.push_back(fill_rect(0, 0, 10, 10));

    CountingPainter painter;
    const auto stats = affineui::detail::replay_clipped(
        list, painter, affineui::Rect{0, 0, 100, 100});

    CHECK(painter.fill_rects == 1);
    CHECK(stats.culled == 1);
    CHECK(stats.emitted == 1);
}

TEST_CASE("clipped replay disables group culling inside transform overflow") {
    affineui::detail::DisplayList list;
    for (int i = 0; i < 65; ++i) {
        list.ops.push_back(push_transform(affineui::Mat2x3::translate(20.0f, 0.0f)));
    }
    list.ops.push_back(push_transform(affineui::Mat2x3::translate(-1300.0f, 0.0f)));
    list.ops.push_back(fill_rect(0, 0, 10, 10));
    list.ops.push_back(pop_transform());
    for (int i = 0; i < 65; ++i) {
        list.ops.push_back(pop_transform());
    }

    CountingPainter painter;
    const auto stats = affineui::detail::replay_clipped(
        list, painter, affineui::Rect{0, 0, 20, 20});

    CHECK(painter.fill_rects == 1);
    CHECK(stats.culled == 0);
}
