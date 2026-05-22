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

// ── Min/max sizing ─────────────────────────────────────────────────

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
