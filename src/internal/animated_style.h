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
    // ── Colors (12 bytes) ─────────────────────────────────────────
    // Packed as RGBA8 (one u32 each) for compact storage and trivial
    // GPU-uniform handoff. Color (the type) unpacks at use-time.
    std::uint32_t color_rgba           {0xFFFFFFFFu};  // foreground
    std::uint32_t background_rgba      {0x00000000u};
    std::uint32_t border_rgba          {0x00000000u};  // uniform border color
    std::uint32_t shadow_rgba          {0x00000000u};

    // ── Shadow geometry (8 bytes) ─────────────────────────────────
    std::int16_t shadow_offset_x{0};
    std::int16_t shadow_offset_y{0};
    std::int16_t shadow_blur    {0};
    std::int16_t shadow_spread  {0};

    // ── Transform (20 bytes) ──────────────────────────────────────
    // 2D affine: translate + scale + rotation. The full 3x2 matrix
    // can be reconstructed for composite-shader handoff.
    float tx       {0.0f};
    float ty       {0.0f};
    float scale_x  {1.0f};
    float scale_y  {1.0f};
    float rotation {0.0f};  // radians

    // ── Compositor (4 bytes) ──────────────────────────────────────
    float opacity{1.0f};

    // ── Background gradient (12 bytes) ────────────────────────────
    // 2-stop gradient descriptor.  NanoVG natively supports 2-stop
    // gradients (nvgLinearGradient / nvgRadialGradient), so this is
    // the correct primitive for our paint layer.  N-stop (>2) is a
    // future extension — add an external stop table at that time.
    //
    // kind: 0 = none (use background_rgba), 1 = linear, 2 = radial.
    // angle_deg: CSS angle in degrees (0 = upward, clockwise).
    //   For `to right` that is 90 deg, for `45deg` that is 45 deg.
    //   For radial gradients this field is unused (set to 0).
    enum class GradientKind : std::uint8_t { None = 0, Linear = 1, Radial = 2 };
    GradientKind  gradient_kind   {GradientKind::None};
    std::uint8_t  gradient_pad_   {0};
    std::int16_t  gradient_angle_deg{0};  // CSS angle, 0–359
    std::uint32_t gradient_stop0_rgba{0};
    std::uint32_t gradient_stop1_rgba{0};

    // Total: 16 + 8 + 20 + 4 + 12 = 60 bytes.
};

static_assert(sizeof(AnimatedStyle) <= 64,
              "AnimatedStyle should stay inside two cache lines");
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
