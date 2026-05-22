#pragma once

#include "affineui/types.h"

#include <cstdint>

namespace affineui::detail {

/// Properties that affect layout. Changing one of these triggers a
/// layout re-run. They change rarely (DOM mutation, stylesheet
/// attach) — never per-frame. See docs/DESIGN.md § "Real-time render
/// architecture" for why these are split from AnimatedStyle.
///
/// Target: <= 64 bytes (one cache line). All fields are fixed-size
/// integer types; strings (font-family) resolve to a `font_id` at
/// cascade time so we can keep the struct trivially copyable and
/// pointer-free.
struct ComputedStyle {
    // ── Box model (16 bytes) ──────────────────────────────────────
    std::int16_t margin_top    {0};
    std::int16_t margin_right  {0};
    std::int16_t margin_bottom {0};
    std::int16_t margin_left   {0};
    std::int16_t padding_top   {0};
    std::int16_t padding_right {0};
    std::int16_t padding_bottom{0};
    std::int16_t padding_left  {0};

    // ── Border widths (8 bytes) ───────────────────────────────────
    std::int16_t border_top    {0};
    std::int16_t border_right  {0};
    std::int16_t border_bottom {0};
    std::int16_t border_left   {0};

    // ── Border style + per-corner radius (10 bytes) ───────────────
    // Uniform border-style for Phase 2C — per-side styles land when
    // the cascade splits them out. The four radii correspond to the
    // CSS shorthand `border-radius: <tl> <tr> <br> <bl>`; CSS pairing
    // rules (1/2/3-value forms) are applied in the gap-fill scanner.
    // Setting `border_radius_*_px` to the same value across all four
    // corners recovers the previous uniform behavior; the painter
    // uses NanoVG's nvgRoundedRectVarying when corners differ.
    //
    // Radii strictly speaking don't affect layout — they only change
    // rasterization — so they could live in AnimatedStyle. Parking
    // them here to keep border data grouped; will migrate if
    // border-radius animations become a hot path.
    enum class BorderStyle : std::uint8_t {
        None = 0, Solid = 1, Dashed = 2, Dotted = 3, Double = 4,
    };
    // Uniform border style for all four sides. Per-side style variation
    // (e.g. `border-left: dashed; border-right: dotted`) is deferred —
    // the current design handles per-side widths and per-side colors (via
    // AnimatedStyle) but keeps one style that applies to all sides.
    // Mixed-style-per-side is rare in practice; this is a Phase 2C
    // limitation documented in the border drawing code in document.cpp.
    BorderStyle  border_style              {BorderStyle::None};
    // Percentage height: -1 = not a percentage, else 0..100 (integer %).
    // Repurposes the former pad_border_ byte; same alignment slot.
    std::int8_t  height_pct               {-1};
    std::int16_t border_radius_top_left_px {0};
    std::int16_t border_radius_top_right_px{0};
    std::int16_t border_radius_bot_right_px{0};
    std::int16_t border_radius_bot_left_px {0};

    // ── Layout sizing (10 bytes) ──────────────────────────────────
    std::int16_t width     {-1};  // -1 = auto
    std::int16_t height    {-1};
    std::int16_t min_width {0};
    std::int16_t max_width {-1};
    std::int16_t min_height{0};

    // ── Positioned-layout insets (8 bytes) ────────────────────────
    // CSS `top` / `right` / `bottom` / `left`. Only meaningful when
    // `position` is Relative / Absolute / Fixed. Each side is "auto"
    // by default — a bit in `inset_has` (declared above, packed into
    // the old cursor padding) records whether the author gave it an
    // explicit length, so the adapter only pushes the edges that were
    // actually specified. Yoga treats the rest as undefined / auto,
    // which is the correct CSS behaviour for absolute anchoring and a
    // no-op for relative.
    std::int16_t inset_top   {0};
    std::int16_t inset_right {0};
    std::int16_t inset_bottom{0};
    std::int16_t inset_left  {0};

    // ── Text layout (6 bytes) ─────────────────────────────────────
    std::uint16_t font_size_px{16};  // resolved px
    std::uint16_t font_weight {400};  // 100..900
    // Line-height as multiplier x 100. 0 = unset (paint uses NVG
    // default of 1.0 today; eventually we'll match browsers' 1.2
    // baseline). Length-typed `line-height: 24px` lands as a
    // negative value: -24 means "absolute 24 px," distinguished
    // from any positive multiplier.
    std::int16_t  line_height_x100{0};

    // ── Text features (8 bytes) ───────────────────────────────────
    // letter-spacing in 1/100 px (signed). 0 = normal. Max useful
    // value is a few hundred px; int16 range is ±327.67 px, plenty.
    std::int16_t letter_spacing_x100{0};

    // white-space: controls whitespace collapsing and wrapping.
    enum class WhiteSpace : std::uint8_t {
        Normal = 0, // collapse + wrap (CSS default)
        Pre,        // preserve spaces+newlines, no wrap
        Nowrap,     // collapse + no wrap
        PreWrap,    // preserve spaces+newlines + wrap
        PreLine,    // preserve newlines, collapse spaces + wrap
    };
    WhiteSpace white_space{WhiteSpace::Normal};

    // text-transform: none / uppercase / lowercase / capitalize.
    enum class TextTransform : std::uint8_t {
        None = 0, Uppercase, Lowercase, Capitalize,
    };
    TextTransform text_transform{TextTransform::None};

    // 4-byte pad to keep struct on an even boundary (letter_spacing
    // is int16 + 2 enums = 4 bytes, so no explicit pad needed here).
    // Struct size stays at ~74 bytes after this addition.

    // ── Display + flex + position (4 bytes) ───────────────────────
    enum class Display : std::uint8_t {
        Block, Inline, InlineBlock, Flex, None,
        // CSS table model. Table stacks its row-groups/rows vertically;
        // TableRowGroup (thead/tbody/tfoot) is a transparent vertical
        // grouping; TableRow lays its cells horizontally; TableCell is a
        // sized item. Column widths are aligned across rows by a pre-pass
        // in the layout engine (see Document::layout).
        Table, TableRowGroup, TableRow, TableCell,
    };
    enum class Position : std::uint8_t {
        Static, Relative, Absolute, Fixed,
    };
    enum class FlexDirection : std::uint8_t {
        Row, Column, RowReverse, ColumnReverse,
    };

    Display       display       {Display::Block};
    Position      position      {Position::Static};
    FlexDirection flex_direction{FlexDirection::Row};
    std::uint8_t  font_style    {0};  // 0=normal, 1=italic, 2=oblique

    // Cursor enum — drives sapp_set_mouse_cursor when this element is
    // under the pointer. Default = the OS default arrow. Pointer = the
    // hand cursor used for clickable things.
    enum class Cursor : std::uint8_t {
        Default = 0, Pointer, Text, Crosshair, Move,
        NotAllowed, ResizeEW, ResizeNS,
    };
    Cursor cursor{Cursor::Default};

    // CSS `overflow-y`. Determines whether children of this block can
    // be scrolled when content exceeds the box. Visible is the CSS
    // default (children paint outside the parent's bounds, no scroll).
    // Auto + Scroll both make the block scrollable; we don't yet
    // distinguish them visually (no scrollbar when not needed).
    enum class Overflow : std::uint8_t {
        Visible = 0, Hidden, Clip, Scroll, Auto,
    };
    Overflow      overflow_y{Overflow::Visible};

    // Presence bits for the positioned-layout insets below. An inset
    // that the author left at its `auto` initial value keeps its bit
    // clear so the Yoga adapter skips that edge (Yoga then treats it
    // as undefined — the correct behaviour for absolute anchoring).
    // Packed into the byte that used to be cursor padding so adding
    // positioning costs no extra struct size.
    struct InsetHas {
        std::uint8_t top    : 1 {};
        std::uint8_t right  : 1 {};
        std::uint8_t bottom : 1 {};
        std::uint8_t left   : 1 {};
    } inset_has{};

    // CSS `box-sizing`. ContentBox (the CSS default) means width/height
    // size the content box and padding+border add on top; BorderBox
    // means width/height size the border box (padding+border eat into
    // it). The Yoga adapter maps this onto YGBoxSizing — Yoga's own
    // default is border-box, so the adapter explicitly pushes whichever
    // value the cascade resolved. Lives in what used to be cursor
    // padding, so it costs no struct size.
    enum class BoxSizing : std::uint8_t {
        ContentBox = 0, BorderBox,
    };
    BoxSizing     box_sizing{BoxSizing::ContentBox};

    // ── Flex container + item properties (12 bytes) ───────────────
    // CSS flex enums collapsed to our minimum-needed sets. Each maps
    // directly onto a Yoga YGAlign/YGJustify/YGWrap enum in the
    // adapter — keep the values in sync if you reorder.
    enum class JustifyContent : std::uint8_t {
        Start, End, Center, SpaceBetween, SpaceAround, SpaceEvenly,
    };
    enum class AlignItems : std::uint8_t {
        Stretch, Start, End, Center, Baseline,
    };
    enum class FlexWrap : std::uint8_t {
        NoWrap, Wrap, WrapReverse,
    };

    JustifyContent justify_content{JustifyContent::Start};
    AlignItems     align_items    {AlignItems::Stretch};
    FlexWrap       flex_wrap      {FlexWrap::NoWrap};
    // Percentage flex-basis: -1 = not a pct, else 0..100 (integer %).
    // flex: 1 0 0% → flex_basis_pct=0. Repurposes the former pad_flex_ byte.
    std::int8_t    flex_basis_pct {-1};

    // Gaps. CSS allows length or %; we currently support px integers.
    std::int16_t row_gap   {0};
    std::int16_t column_gap{0};

    // Flex item properties. flex_grow/shrink are byte-sized integers —
    // typical CSS values are 0 or 1, occasionally 2-3. Sub-integer
    // factors round to nearest int. Phase 2D could widen to a real
    // float when fractional factors land in a demo.
    std::uint8_t  flex_grow  {0};
    std::uint8_t  flex_shrink{1};
    std::int16_t  flex_basis {-1};  // -1 = auto, else px

    // ── Font face id (1 byte) ─────────────────────────────────────
    // Resolved at cascade time from font-family. 0 = default (sans).
    // 256 distinct faces per document is plenty.
    std::uint8_t  font_id      {0};
    std::uint8_t  z_index_high {0};  // top 8 bits of i16 z-index
    std::int16_t  z_index_low  {0};  // bottom 16 bits (i16 for sort)

    // ── Percentage width (2 bytes) ────────────────────────────────
    // Stores width as percentage × 100 when a % value is present.
    // -1 = not a percentage; otherwise 0..10000 (e.g. 33.33% → 3333).
    // High precision required so that column fractions like 33.33%
    // can be floored without accumulating error. Stored as int16_t
    // after the other fields to avoid disturbing existing alignment.
    std::int16_t  width_pct_x100{-1};

    // ── Inheritance presence bitset (4 bytes, plenty of room) ─────
    // Only inherited properties care about presence; non-inherited
    // properties default to their CSS initial value and never need
    // a "use parent's" path.
    struct InheritedHas {
        std::uint32_t color       : 1 {};
        std::uint32_t font_family : 1 {};
        std::uint32_t font_size   : 1 {};
        std::uint32_t font_weight : 1 {};
        std::uint32_t font_style  : 1 {};
    } has{};

    // Roughly ~92 bytes after flex, positioned-layout insets, and
    // percentage-sizing fields (width_pct_x100, height_pct, flex_basis_pct,
    // for Bootstrap column widths) were added. The original 64-byte "fits
    // in one x86_64 cache line" target survives only as a soft goal —
    // modern Apple Silicon has 128-byte lines anyway, and the hot loops
    // that read this struct read just a few fields per node, not the whole
    // thing. The assert is a budget that tells us when we've grown large.
};

static_assert(sizeof(ComputedStyle) <= 96,
              "ComputedStyle exceeded its size budget — re-pack before bumping further");
static_assert(std::is_trivially_copyable_v<ComputedStyle>,
              "ComputedStyle must be trivially copyable");

/// CSS `line-height: normal`. The spec leaves the value impl-defined
/// ("a reasonable value based on the font"); browsers use the font's
/// own line metrics, (ascender − descender + lineGap) / unitsPerEm.
/// For our embedded default font (Roboto) that is (2146 + 555 + 0) /
/// 2048 ≈ 1.32 — and that is exactly what Chrome renders for it, so
/// matching it is what makes `normal`-spaced text line up in the
/// conformance A/B. fontstash normalizes its reported line height to
/// 1.0em (it folds away the real ratio), so we can't read this back
/// from nvgTextMetrics; we encode the UA default here. TODO: when the
/// font registry exposes real per-font v-metrics, derive this per font
/// instead of assuming the default face.
inline constexpr float kNormalLineHeight = 1.32f;

/// Translate `ComputedStyle.line_height_x100` into a multiplier
/// suitable for `Painter::draw_text_box` / `measure_text_box`. The
/// stored value is signed: positive = multiplier × 100, negative =
/// absolute pixels (negated). Layout + paint both call this so the
/// wrap pass and the actual draw agree on inter-line spacing.
inline float effective_line_height_mult(const ComputedStyle& cs) {
    if (cs.line_height_x100 > 0)
        return static_cast<float>(cs.line_height_x100) / 100.0f;
    if (cs.line_height_x100 < 0 && cs.font_size_px > 0)
        return static_cast<float>(-cs.line_height_x100)
             / static_cast<float>(cs.font_size_px);
    return kNormalLineHeight;  // unset → CSS `normal` (font-derived)
}

}  // namespace affineui::detail
