#pragma once

#include "affineui/types.h"

#include <cstdint>

namespace affineui::detail {

/// Properties that only affect paint (not layout). Changing one of
/// these never triggers layout. They change frequently — animations,
/// hover transitions, per-frame state updates.
///
/// Target: <= 32 bytes (half a cache line). One AnimatedStyle per
/// element lives in a tight array; the animation engine writes here
/// directly and the cascade is not invoked.
///
/// See docs/DESIGN.md § "Real-time render architecture" for the
/// rationale.
struct AnimatedStyle {
    // ── Colors (28 bytes) ────────────────────────────────────────
    // Packed as RGBA8 (one u32 each) for compact storage and trivial
    // GPU-uniform handoff. Color (the type) unpacks at use-time.
    std::uint32_t color_rgba           {0xFFFFFFFFu};  // foreground
    std::uint32_t background_rgba      {0x00000000u};
    std::uint32_t border_rgba          {0x00000000u};  // uniform border color
    std::uint32_t shadow_rgba          {0x00000000u};
    // Per-side border colors. Explicit transparent is a valid CSS value, so
    // the colors cannot use 0x00000000 as an "unset" sentinel. The bitmask
    // records which side colors were explicitly supplied; unset sides fall
    // back to border_rgba (the common `border: N style color` shorthand).
    enum BorderColorSide : std::uint8_t {
        BorderTopColorSet    = 1u << 0,
        BorderRightColorSet  = 1u << 1,
        BorderBottomColorSet = 1u << 2,
        BorderLeftColorSet   = 1u << 3,
        BorderAllColorsSet   = BorderTopColorSet | BorderRightColorSet |
                               BorderBottomColorSet | BorderLeftColorSet,
    };
    std::uint32_t border_top_rgba      {0x00000000u};
    std::uint32_t border_right_rgba    {0x00000000u};
    std::uint32_t border_bottom_rgba   {0x00000000u};
    std::uint32_t border_left_rgba     {0x00000000u};
    std::uint8_t  border_color_set     {0};
    // 0 = currentColor; otherwise an explicit text-decoration-color.
    std::uint32_t text_decoration_rgba {0x00000000u};

    // ── Shadow geometry (8 bytes) ─────────────────────────────────
    std::int16_t shadow_offset_x{0};
    std::int16_t shadow_offset_y{0};
    std::int16_t shadow_blur    {0};
    std::int16_t shadow_spread  {0};
    // CSS `inset` keyword: paint the shadow inside the box instead of
    // behind it. Stored separately so the paint pass can order it after
    // the background (CSS painting order) rather than before.
    bool         shadow_inset   {false};

    // ── Transform (20 bytes) ──────────────────────────────────────
    // 2D affine: translate + scale + rotation. The full 3x2 matrix
    // can be reconstructed for composite-shader handoff.
    float tx       {0.0f};
    float ty       {0.0f};
    float tx_pct   {0.0f};
    float ty_pct   {0.0f};
    float scale_x  {1.0f};
    float scale_y  {1.0f};
    float rotation {0.0f};  // radians
    float origin_x {0.0f};
    float origin_y {0.0f};
    float origin_x_pct {50.0f};
    float origin_y_pct {50.0f};

    // ── Compositor (4 bytes) ──────────────────────────────────────
    float opacity{1.0f};

    // ── Background gradient (12 bytes) ────────────────────────────
    // 2-stop gradient descriptor.  NanoVG natively supports 2-stop
    // gradients (nvgLinearGradient / nvgRadialGradient), so this is
    // the correct primitive for our paint layer.  N-stop (>2) is a
    // future extension — add an external stop table at that time.
    //
    // kind: 0 = none, 1 = linear, 2 = radial, 3 = repeating stripe tile.
    // angle_deg: CSS angle in degrees (0 = upward, clockwise).
    //   For `to right` that is 90 deg, for `45deg` that is 45 deg.
    // center_*_pct: radial-gradient center position. Linear gradients
    //   leave these at the CSS radial default (`center`, 50/50).
    enum class GradientKind : std::uint8_t {
        None = 0,
        Linear = 1,
        Radial = 2,
        LinearStripes = 3,
    };
    GradientKind  gradient_kind   {GradientKind::None};
    std::uint8_t  gradient_center_x_pct{50};
    std::uint8_t  gradient_center_y_pct{50};
    std::uint8_t  gradient_stop1_pos_pct{100};
    std::uint8_t  gradient_pad{0};
    std::int16_t  gradient_angle_deg{0};  // CSS angle, 0–359
    std::uint32_t gradient_stop0_rgba{0};
    std::uint32_t gradient_stop1_rgba{0};
    std::uint32_t background_grid_rgba{0};
    std::uint8_t  background_grid_size_px{0};
    std::uint8_t  background_grid_line_px{1};
    std::uint16_t background_grid_pad{0};

    // ~92 bytes after per-side border colors (16 B), text decoration colour,
    // and the 2-stop gradient descriptor with radial center were added.
};

// The original 48-byte target, then 64, were soft "fits in a cache line"
// goals. Per-side border colors and the gradient descriptor pushed this to
// ~76 bytes — still well within one 128-byte Apple Silicon line, and the
// paint hot loop touches only a few fields per node. The assert is a budget
// that flags when the struct has grown problematically large.
static_assert(sizeof(AnimatedStyle) <= 128,
              "AnimatedStyle exceeded its size budget — re-pack before growing further");
static_assert(std::is_trivially_copyable_v<AnimatedStyle>,
              "AnimatedStyle must be trivially copyable");

inline std::uint32_t pack_rgba(Color c) noexcept {
    return (std::uint32_t(c.r) << 24)
         | (std::uint32_t(c.g) << 16)
         | (std::uint32_t(c.b) <<  8)
         |  std::uint32_t(c.a);
}

inline Color unpack_rgba(std::uint32_t v) noexcept {
    return Color{
        static_cast<std::uint8_t>((v >> 24) & 0xFF),
        static_cast<std::uint8_t>((v >> 16) & 0xFF),
        static_cast<std::uint8_t>((v >>  8) & 0xFF),
        static_cast<std::uint8_t>( v        & 0xFF),
    };
}

}  // namespace affineui::detail
