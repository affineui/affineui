// Layout corpus — drives the Yoga adapter directly and asserts on
// resolved bounds. The point is to pin down current behavior so the
// next feature doesn't silently regress one of:
//
//   • box-sizing: content-box semantics (the Yoga default-vs-CSS gap)
//   • padding/margin shorthand mirror expansion (lexbor 2.4 gap)
//   • flex distribution
//   • content-determined container sizing
//
// Tests target `affineui::detail::layout_blocks_with_yoga` — pure
// data in, pure data out. No GL, no NanoVG, no Painter. Fast.

#include <doctest/doctest.h>

#include "affineui/types.h"
#include "internal/computed_style.h"
#include "layout/yoga_adapter.h"

#include <array>
#include <vector>

using affineui::Rect;
using affineui::RectF;
using affineui::detail::BlockLayoutInput;
using affineui::detail::ComputedStyle;
using affineui::detail::layout_blocks_with_yoga;

namespace {

// Convenience: build an input from a ComputedStyle + optional explicit
// intrinsic height. `parent` defaults to -1 (top-level child of the
// synthetic root).
BlockLayoutInput make_input(const ComputedStyle& cs,
                            int intrinsic_h = 0,
                            int parent      = -1) {
    BlockLayoutInput in{};
    in.style          = &cs;
    in.intrinsic_h_px = intrinsic_h;
    in.parent_idx     = parent;
    return in;
}

// Run one layout pass; returns the resolved bounds vector.
std::vector<Rect> run(int viewport_w,
                      std::vector<BlockLayoutInput> inputs) {
    std::vector<Rect> out(inputs.size());
    layout_blocks_with_yoga(viewport_w, inputs, out, nullptr);
    return out;
}

}  // namespace

// ── Baseline: empty + single block ─────────────────────────────────

TEST_CASE("empty input list produces empty output without crashing") {
    auto out = run(800, {});
    CHECK(out.empty());
}

TEST_CASE("a single block with intrinsic height fills the viewport width") {
    ComputedStyle cs{};
    auto out = run(800, {make_input(cs, 50)});

    REQUIRE(out.size() == 1);
    CHECK(out[0].x == 0);
    CHECK(out[0].y == 0);
    CHECK(out[0].w == 800);     // stretches to viewport (default align-items: stretch)
    CHECK(out[0].h == 50);
}

// ── Box model: padding / border / margin ───────────────────────────

TEST_CASE("padding adds to the box size (content-box, the CSS default)") {
    // The original bug: Yoga's default box-sizing is border-box, so a
    // 50px content height with 16px vertical padding would have
    // collapsed to a 50px outer box. We override to content-box in
    // the adapter; this test pins that fix.
    ComputedStyle cs{};
    cs.padding_top    = 10;
    cs.padding_bottom = 10;

    auto out = run(800, {make_input(cs, 50)});

    REQUIRE(out.size() == 1);
    CHECK(out[0].h == 70);      // 50 content + 10 + 10 padding
}

TEST_CASE("border participates in the box size (same content-box principle)") {
    ComputedStyle cs{};
    cs.border_top    = 2;
    cs.border_bottom = 2;
    cs.border_style = ComputedStyle::BorderStyle::Solid;
    cs.border_style_sides = ComputedStyle::BorderTopSide |
                            ComputedStyle::BorderBottomSide;

    auto out = run(800, {make_input(cs, 50)});

    REQUIRE(out.size() == 1);
    CHECK(out[0].h == 54);      // 50 content + 2 + 2 border
}

TEST_CASE("margin spaces siblings apart vertically") {
    ComputedStyle a{};
    ComputedStyle b{};
    b.margin_top = 20;

    auto out = run(800, {make_input(a, 30), make_input(b, 30)});

    REQUIRE(out.size() == 2);
    CHECK(out[0].y == 0);
    CHECK(out[0].h == 30);
    // Yoga does NOT collapse vertical margins (CSS-block-flow does).
    // Documented divergence — pin it explicitly.
    CHECK(out[1].y == 30 + 20);  // = end of a + margin of b
    CHECK(out[1].h == 30);
}

TEST_CASE("negative margins are applied, not clamped to zero") {
    // Bootstrap's grid gutters (.row margin: calc(-.5*gutter)) and
    // .card-subtitle pull-up rely on negative margins. Clamping them to 0
    // shifted Small/Large and card content down.
    ComputedStyle a{};
    ComputedStyle b{};
    b.margin_top = -10;

    auto out = run(800, {make_input(a, 30), make_input(b, 30)});

    REQUIRE(out.size() == 2);
    CHECK(out[0].y == 0);
    CHECK(out[1].y == 30 - 10);   // pulled up by the negative margin
}

TEST_CASE("horizontal auto margins center a fixed-width block child") {
    ComputedStyle parent{};
    parent.width = 120;

    ComputedStyle child{};
    child.width = 80;
    child.height = 20;
    child.margin_auto.left = 1;
    child.margin_auto.right = 1;

    auto out = run(800, {
        make_input(parent),
        make_input(child, 0, /*parent=*/0),
    });

    REQUIRE(out.size() == 2);
    CHECK(out[1].x == 20);
    CHECK(out[1].w == 80);
}

TEST_CASE("percentage positioned insets resolve through Yoga") {
    ComputedStyle parent{};
    parent.position = ComputedStyle::Position::Relative;
    parent.width = 200;
    parent.height = 100;

    ComputedStyle child{};
    child.position = ComputedStyle::Position::Absolute;
    child.width = 20;
    child.height = 10;
    child.inset_top = 1800;   // 18%
    child.inset_left = 5000;  // 50%
    child.inset_has.top = 1;
    child.inset_has.left = 1;
    child.inset_has.top_pct = 1;
    child.inset_has.left_pct = 1;

    auto out = run(800, {
        make_input(parent),
        make_input(child, 0, /*parent=*/0),
    });

    REQUIRE(out.size() == 2);
    CHECK(out[1].x == 100);
    CHECK(out[1].y == 18);
    CHECK(out[1].w == 20);
    CHECK(out[1].h == 10);
}

TEST_CASE("fractional absolute insets are preserved for paint") {
    ComputedStyle parent{};
    parent.position = ComputedStyle::Position::Relative;
    parent.box_sizing = ComputedStyle::BoxSizing::BorderBox;
    parent.width = 28;
    parent.height = 140;
    parent.border_top = parent.border_right =
        parent.border_bottom = parent.border_left = 1;
    parent.border_style = ComputedStyle::BorderStyle::Solid;
    parent.border_style_sides = ComputedStyle::BorderAllSides;

    ComputedStyle child{};
    child.position = ComputedStyle::Position::Absolute;
    child.box_sizing = ComputedStyle::BoxSizing::BorderBox;
    child.width = 22;
    child.height = 12;
    child.inset_top = 2400;
    child.inset_has.top = 1;
    child.inset_has.top_pct = 1;

    std::vector<BlockLayoutInput> inputs{
        make_input(parent),
        make_input(child, 0, /*parent=*/0),
    };
    std::vector<Rect> out(inputs.size());
    std::vector<RectF> out_f(inputs.size());
    layout_blocks_with_yoga(200, inputs, out, nullptr, out_f);

    REQUIRE(out.size() == 2);
    CHECK(out[1].y == 34);
    CHECK(out_f[1].y == doctest::Approx(34.12f));
}

TEST_CASE("padding shifts content-box origin so child y starts after padding") {
    ComputedStyle parent{};
    parent.padding_top  = 16;
    parent.padding_left = 24;
    ComputedStyle child{};

    auto out = run(800, {
        make_input(parent),                     // container, auto height
        make_input(child, 30, /*parent=*/0),    // child of block 0
    });

    REQUIRE(out.size() == 2);
    CHECK(out[1].x == 24);       // shifted by parent's padding-left
    CHECK(out[1].y == 16);       // shifted by parent's padding-top
    CHECK(out[1].h == 30);
}

// ── Auto-sizing containers ─────────────────────────────────────────

TEST_CASE("container with no explicit height grows to fit its children") {
    ComputedStyle parent{};
    parent.padding_top    = 10;
    parent.padding_bottom = 10;
    ComputedStyle child{};

    auto out = run(800, {
        make_input(parent, /*intrinsic_h=*/0),  // auto height
        make_input(child,   /*intrinsic_h=*/40, /*parent=*/0),
    });

    REQUIRE(out.size() == 2);
    CHECK(out[0].h == 60);       // 40 child + 10 top pad + 10 bottom pad
}

TEST_CASE("fixed-height block child keeps its height and overflows block parent") {
    ComputedStyle parent{};
    parent.width  = 140;
    parent.height = 90;

    ComputedStyle child{};
    child.width  = 220;
    child.height = 160;

    auto out = run(640, {
        make_input(parent),
        make_input(child, 0, /*parent=*/0),
    });

    REQUIRE(out.size() == 2);
    CHECK(out[0].h == 90);
    CHECK(out[1].w == 220);
    CHECK(out[1].h == 160);
}

TEST_CASE("percentage height inside auto-height flex item behaves as auto") {
    // Bootstrap's .h-100 utility appears on cards inside .col flex items.
    // The .col has no definite height; CSS therefore treats the child's
    // height:100% as auto instead of stretching it to the viewport/root.
    ComputedStyle row{};
    row.display   = ComputedStyle::Display::Flex;
    row.flex_wrap = ComputedStyle::FlexWrap::Wrap;

    ComputedStyle col{};
    col.flex_grow = 1;

    ComputedStyle card{};
    card.height_pct = 100;

    auto out = run(800, {
        make_input(row, 0),
        make_input(col, 0, 0),
        make_input(card, 72, 1),
    });

    REQUIRE(out.size() == 3);
    CHECK(out[1].h == 72);
    CHECK(out[2].h == 72);
}

// ── Flex layout ────────────────────────────────────────────────────

TEST_CASE("flex row with flex-grow:1 children distributes width evenly") {
    ComputedStyle parent{};
    parent.display = ComputedStyle::Display::Flex;
    // flex_direction defaults to Row.

    ComputedStyle child{};
    child.flex_grow = 1;

    auto out = run(900, {
        make_input(parent, /*intrinsic_h=*/0),
        make_input(child,  /*intrinsic_h=*/40, /*parent=*/0),
        make_input(child,  /*intrinsic_h=*/40, /*parent=*/0),
        make_input(child,  /*intrinsic_h=*/40, /*parent=*/0),
    });

    REQUIRE(out.size() == 4);
    // Each child gets 1/3 of the 900px row.
    CHECK(out[1].w == 300);
    CHECK(out[2].w == 300);
    CHECK(out[3].w == 300);
    // Adjacent on the row — first at x=0, third ends at x=900.
    CHECK(out[1].x == 0);
    CHECK(out[2].x == 300);
    CHECK(out[3].x == 600);
}

TEST_CASE("border-box %-width flex items fit with padding (no overflow/wrap)") {
    // The Bootstrap grid case: two `width:50%` columns with horizontal
    // gutter padding inside a wrapping flex row. With box-sizing:border-box
    // the padding is inside the 50%, so both fit on one row. If border-box
    // is ignored for the width dimension, padding is added outside and the
    // second column wraps.
    ComputedStyle parent{};
    parent.display   = ComputedStyle::Display::Flex;
    parent.flex_wrap = ComputedStyle::FlexWrap::Wrap;

    ComputedStyle col{};
    col.box_sizing     = ComputedStyle::BoxSizing::BorderBox;
    col.width_pct_x100 = 5000;   // 50%
    col.flex_grow      = 0;
    col.flex_shrink    = 0;      // Bootstrap cols can't shrink → wrap on overflow
    col.padding_left   = 12;
    col.padding_right  = 12;

    auto out = run(800, {
        make_input(parent, 0),
        make_input(col, 40, 0),
        make_input(col, 40, 0),
    });

    REQUIRE(out.size() == 3);
    CHECK(out[1].w == 400);            // 50% border-box
    CHECK(out[2].w == 400);
    CHECK(out[1].y == out[2].y);       // same row — no wrap
    CHECK(out[2].x == 400);
}

TEST_CASE("saturated percentage flex row resolves main-axis auto margins to zero") {
    // CSS flexbox treats auto margins as zero while collecting items
    // into flex lines. Bootstrap dashboards rely on this shape:
    // a fixed-width sidebar column plus an ms-*-auto main column whose
    // percentage widths already consume the full row.
    ComputedStyle parent{};
    parent.display   = ComputedStyle::Display::Flex;
    parent.flex_wrap = ComputedStyle::FlexWrap::Wrap;

    ComputedStyle sidebar{};
    sidebar.box_sizing = ComputedStyle::BoxSizing::BorderBox;
    sidebar.width_pct_x100 = 2500;
    sidebar.flex_shrink = 0;

    ComputedStyle main{};
    main.box_sizing = ComputedStyle::BoxSizing::BorderBox;
    main.width_pct_x100 = 7500;
    main.flex_shrink = 0;
    main.margin_auto.left = 1;

    auto out = run(1000, {
        make_input(parent, 0),
        make_input(sidebar, 40, 0),
        make_input(main, 40, 0),
    });

    REQUIRE(out.size() == 3);
    CHECK(out[1].w == 250);
    CHECK(out[2].w == 750);
    CHECK(out[1].y == out[2].y);
    CHECK(out[2].x == 250);
}

TEST_CASE("percentage flex columns do not wrap because of oversized auto minimum width") {
    // A content-heavy Bootstrap dashboard column can have a large
    // min-content width, but the grid column itself still occupies its
    // declared percentage track. The content may overflow inside it; the
    // sibling columns should not be pushed onto separate flex lines.
    ComputedStyle parent{};
    parent.display   = ComputedStyle::Display::Flex;
    parent.flex_wrap = ComputedStyle::FlexWrap::Wrap;

    ComputedStyle sidebar{};
    sidebar.box_sizing = ComputedStyle::BoxSizing::BorderBox;
    sidebar.width_pct_x100 = 2500;
    sidebar.flex_shrink = 0;

    ComputedStyle main{};
    main.box_sizing = ComputedStyle::BoxSizing::BorderBox;
    main.width_pct_x100 = 7500;
    main.flex_shrink = 0;

    auto main_input = make_input(main, 40, 0);
    main_input.auto_min_w_px = 1600;

    auto out = run(1000, {
        make_input(parent, 0),
        make_input(sidebar, 40, 0),
        main_input,
    });

    REQUIRE(out.size() == 3);
    CHECK(out[1].w == 250);
    CHECK(out[2].w == 750);
    CHECK(out[1].y == out[2].y);
    CHECK(out[2].x == 250);
}

TEST_CASE("flex row gap separates children on the main axis") {
    ComputedStyle parent{};
    parent.display    = ComputedStyle::Display::Flex;
    parent.column_gap = 20;

    ComputedStyle child{};
    child.flex_grow = 1;

    auto out = run(840, {
        make_input(parent, 0),
        make_input(child, 40, 0),
        make_input(child, 40, 0),
        make_input(child, 40, 0),
    });

    REQUIRE(out.size() == 4);
    // Available width = 840 - 2 gaps × 20 = 800. Each child = 800/3 ≈ 267.
    // Allow ±1px for Yoga's pixel-grid rounding.
    CHECK(out[1].w >= 266);
    CHECK(out[1].w <= 268);
    // Second child starts after first + gap.
    CHECK(out[2].x >= out[1].x + out[1].w + 19);
    CHECK(out[2].x <= out[1].x + out[1].w + 21);
}

TEST_CASE("nowrap flex row shrinks default flex items to fit") {
    ComputedStyle parent{};
    parent.display        = ComputedStyle::Display::Flex;
    parent.flex_direction = ComputedStyle::FlexDirection::Row;
    parent.flex_wrap      = ComputedStyle::FlexWrap::NoWrap;
    parent.column_gap     = 8;
    parent.width          = 300;

    ComputedStyle child{};
    child.width  = 80;
    child.height = 40;

    auto out = run(640, {
        make_input(parent, 0),
        make_input(child, 0, 0),
        make_input(child, 0, 0),
        make_input(child, 0, 0),
        make_input(child, 0, 0),
    });

    REQUIRE(out.size() == 5);
    CHECK(out[1].w == 69);
    CHECK(out[2].x == 77);
    CHECK(out[4].x + out[4].w == 300);
}

TEST_CASE("percentage width in auto-sized nested flex item falls back to intrinsic width") {
    ComputedStyle outer{};
    outer.display = ComputedStyle::Display::Flex;
    outer.width   = 664;

    ComputedStyle form{};
    form.display = ComputedStyle::Display::Flex;

    ComputedStyle input{};
    input.width_pct_x100 = 10000;
    input.height = 38;
    input.margin_right = 8;

    ComputedStyle button{};
    button.width = 73;
    button.height = 38;

    auto out = run(800, {
        make_input(outer),
        make_input(form, 0, /*parent=*/0),
        make_input(input, 0, /*parent=*/1),
        make_input(button, 0, /*parent=*/1),
    });

    REQUIRE(out.size() == 4);
    CHECK(out[2].w == 0);
    CHECK(out[1].w == 81);

    auto sized_input = make_input(input, 0, /*parent=*/1);
    sized_input.intrinsic_w_px = 207;
    out = run(800, {
        make_input(outer),
        make_input(form, 0, /*parent=*/0),
        sized_input,
        make_input(button, 0, /*parent=*/1),
    });

    REQUIRE(out.size() == 4);
    CHECK(out[2].w == 207);
    CHECK(out[3].x == 215);
    CHECK(out[1].w == 288);
}

TEST_CASE("flex column with align-items: center centers children on cross axis") {
    ComputedStyle parent{};
    parent.display          = ComputedStyle::Display::Flex;
    parent.flex_direction   = ComputedStyle::FlexDirection::Column;
    parent.align_items      = ComputedStyle::AlignItems::Center;

    ComputedStyle child{};
    child.width = 100;        // explicit, so we know what to center

    auto out = run(800, {
        make_input(parent, 0),
        make_input(child, 50, 0),
    });

    REQUIRE(out.size() == 2);
    CHECK(out[1].w == 100);
    // Parent is 800 wide; child 100 wide. Centered → x = 350.
    CHECK(out[1].x == 350);
}

TEST_CASE("column flex-basis overrides explicit child height on main axis") {
    ComputedStyle parent{};
    parent.display        = ComputedStyle::Display::Flex;
    parent.flex_direction = ComputedStyle::FlexDirection::Column;
    parent.height         = 100;

    ComputedStyle header{};
    header.height = 20;
    header.flex_shrink = 0;

    ComputedStyle body{};
    body.height = 250;
    body.flex_grow = 1;
    body.flex_shrink = 1;
    body.flex_basis_pct = 0;

    auto out = run(800, {
        make_input(parent, 0),
        make_input(header, 0, 0),
        make_input(body,   0, 0),
    });

    REQUIRE(out.size() == 3);
    CHECK(out[0].h == 100);
    CHECK(out[1].h == 20);
    CHECK(out[2].y == 20);
    CHECK(out[2].h == 80);
}

TEST_CASE("flex row align-items center preserves explicit child height") {
    ComputedStyle parent{};
    parent.display     = ComputedStyle::Display::Flex;
    parent.align_items = ComputedStyle::AlignItems::Center;
    parent.width = 212;
    parent.height = 22;

    ComputedStyle child{};
    child.width_pct_x100 = 10000;
    child.height = 4;

    auto out = run(800, {
        make_input(parent),
        make_input(child, 0, 0),
    });

    REQUIRE(out.size() == 2);
    CHECK(out[1].w == 212);
    CHECK(out[1].h == 4);
    CHECK(out[1].y == 9);
}

TEST_CASE("absolute child with top and bottom anchors to positioned parent height") {
    ComputedStyle slider{};
    slider.display     = ComputedStyle::Display::Flex;
    slider.align_items = ComputedStyle::AlignItems::Center;
    slider.width = 212;
    slider.height = 22;

    ComputedStyle track{};
    track.position = ComputedStyle::Position::Relative;
    track.width_pct_x100 = 10000;
    track.height = 4;
    track.border_top = track.border_right =
        track.border_bottom = track.border_left = 1;
    track.border_style = ComputedStyle::BorderStyle::Solid;
    track.border_style_sides = ComputedStyle::BorderAllSides;
    track.box_sizing = ComputedStyle::BoxSizing::BorderBox;

    ComputedStyle fill{};
    fill.position = ComputedStyle::Position::Absolute;
    fill.inset_has.top = 1;
    fill.inset_has.bottom = 1;
    fill.inset_has.left = 1;
    fill.inset_top = 0;
    fill.inset_bottom = 0;
    fill.inset_left = 0;
    fill.width_pct_x100 = 6800;
    fill.box_sizing = ComputedStyle::BoxSizing::BorderBox;

    auto out = run(800, {
        make_input(slider),
        make_input(track, 0, 0),
        make_input(fill, 0, 1),
    });

    REQUIRE(out.size() == 3);
    CHECK(out[1].h == 4);
    CHECK(out[2].h == 2);
}

TEST_CASE("flex row baseline alignment uses supplied child baselines") {
    ComputedStyle parent{};
    parent.display     = ComputedStyle::Display::Flex;
    parent.align_items = ComputedStyle::AlignItems::Baseline;

    ComputedStyle child{};

    auto a = make_input(child, 20, 0);
    a.baseline_px = 15.0f;
    auto b = make_input(child, 12, 0);
    b.baseline_px = 8.0f;

    auto out = run(320, {
        make_input(parent, 0),
        a,
        b,
    });

    REQUIRE(out.size() == 3);
    CHECK(out[1].y == 0);
    CHECK(out[2].y == 7);
    CHECK(out[0].h == 20);
}

// ── Min/max sizing ─────────────────────────────────────────────────

TEST_CASE("vertical-align middle affects only synthetic inline children") {
    ComputedStyle parent{};
    parent.display     = ComputedStyle::Display::Flex;
    parent.align_items = ComputedStyle::AlignItems::Baseline;

    ComputedStyle baseline_child{};
    ComputedStyle middle_child{};
    middle_child.vertical_align = ComputedStyle::VerticalAlign::Middle;

    auto a = make_input(baseline_child, 20, 0);
    a.baseline_px = 15.0f;
    auto b = make_input(middle_child, 12, 0);
    b.baseline_px = 8.0f;
    b.inline_parent = true;

    auto out = run(320, {
        make_input(parent, 0),
        a,
        b,
    });

    REQUIRE(out.size() == 3);
    CHECK(out[1].y == 0);
    CHECK(out[2].y == 4);
    CHECK(out[0].h == 20);
}

TEST_CASE("vertical-align is ignored for ordinary flex items") {
    ComputedStyle parent{};
    parent.display     = ComputedStyle::Display::Flex;
    parent.align_items = ComputedStyle::AlignItems::Baseline;

    ComputedStyle baseline_child{};
    ComputedStyle middle_child{};
    middle_child.vertical_align = ComputedStyle::VerticalAlign::Middle;

    auto a = make_input(baseline_child, 20, 0);
    a.baseline_px = 15.0f;
    auto b = make_input(middle_child, 12, 0);
    b.baseline_px = 8.0f;

    auto out = run(320, {
        make_input(parent, 0),
        a,
        b,
    });

    REQUIRE(out.size() == 3);
    CHECK(out[1].y == 0);
    CHECK(out[2].y == 7);
    CHECK(out[0].h == 20);
}

TEST_CASE("min-width clamps a flex-grow child up from its content size") {
    ComputedStyle parent{};
    parent.display = ComputedStyle::Display::Flex;

    ComputedStyle small{};
    small.flex_grow = 1;

    ComputedStyle big{};
    big.flex_grow = 1;
    big.min_width = 400;

    auto out = run(600, {
        make_input(parent, 0),
        make_input(small, 30, 0),
        make_input(big,   30, 0),
    });

    REQUIRE(out.size() == 3);
    // big claims at least 400; small gets the remainder.
    CHECK(out[2].w >= 400);
    CHECK(out[1].w + out[2].w == 600);
}

// ── Shorthand mirror (CSS specs `padding: A B` → top=bottom=A,
//    right=left=B). The cascade has the mirror logic; this test
//    confirms the *layout* honors whatever the cascade produces. ──

TEST_CASE("auto min-width protects text flex items unless explicitly zeroed") {
    ComputedStyle parent{};
    parent.display = ComputedStyle::Display::Flex;
    parent.width = 120;

    ComputedStyle input{};
    input.flex_grow = 1;

    ComputedStyle button{};
    button.flex_shrink = 1;

    auto btn = make_input(button, 24, 0);
    btn.auto_min_w_px = 64;

    auto out = run(800, {
        make_input(parent),
        make_input(input, 24, 0),
        btn,
    });

    REQUIRE(out.size() == 3);
    CHECK(out[2].w == 64);

    button.min_width = 0;
    btn = make_input(button, 24, 0);
    btn.auto_min_w_px = 64;

    out = run(800, {
        make_input(parent),
        make_input(input, 24, 0),
        btn,
    });

    REQUIRE(out.size() == 3);
    CHECK(out[2].w < 64);
}

TEST_CASE("explicit per-side padding values produce the expected box dimensions") {
    ComputedStyle cs{};
    cs.padding_top    = 16;
    cs.padding_right  = 24;
    cs.padding_bottom = 16;
    cs.padding_left   = 24;

    auto out = run(800, {make_input(cs, 50)});

    REQUIRE(out.size() == 1);
    CHECK(out[0].w == 800);                  // stretches to viewport (right/left padding inside)
    CHECK(out[0].h == 50 + 16 + 16);         // content + top + bottom
}

// ── Nested layout: child positions are doc-relative after adapter
//    accumulates parent offsets ──────────────────────────────────────

TEST_CASE("nested children get document-relative bounds, not parent-relative") {
    ComputedStyle outer{};
    outer.padding_top  = 20;
    outer.padding_left = 30;
    ComputedStyle middle{};
    middle.padding_top  = 10;
    middle.padding_left = 15;
    ComputedStyle leaf{};

    auto out = run(800, {
        make_input(outer,  0),                  // 0
        make_input(middle, 0, /*parent=*/0),    // 1
        make_input(leaf,  40, /*parent=*/1),    // 2
    });

    REQUIRE(out.size() == 3);
    // Leaf's x = outer-pad-left + middle-pad-left = 30 + 15 = 45.
    // Leaf's y = outer-pad-top  + middle-pad-top  = 20 + 10 = 30.
    CHECK(out[2].x == 45);
    CHECK(out[2].y == 30);
    CHECK(out[2].h == 40);
}
