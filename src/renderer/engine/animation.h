#pragma once

// Composite-only animation runtime.
//
// Two animation classes exist in the engine:
//
//   1. **Composite-only** (this file). Animates `transform`, `opacity`,
//      `filter: blur`, `scroll-offset`. Runs every frame inside the
//      compositor pass. Does NOT re-enter Style, Layout, or Paint.
//      Reads one struct per active animation, writes one uniform.
//
//   2. **Style-driven** (handled in StyleEngine). Animates anything
//      that requires re-cascade (e.g. `color`, `background-color`,
//      `width`). Style ticks the animation, marks the property
//      style-dirty for that node, restyle pass runs. Slower path.
//      Used only when composite-only isn't an option.
//
// The split is the architectural commitment that makes 120 fps idle
// animations achievable. See docs/OPTIMIZATION.md § 1.

#include "affineui/geom.h"
#include "affineui/style.h"
#include "renderer/engine/layer.h"

#include <cstdint>
#include <vector>

namespace affineui {

enum class EasingKind : std::uint8_t {
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
    CubicBezier,    // params in EasingParams
    Spring,         // damped harmonic; params in EasingParams
    Steps,          // step-end / step-start
};

struct EasingParams {
    // Bezier: control point coords (x1, y1, x2, y2)
    // Spring: (stiffness, damping, mass, _)
    // Steps:  (count, step_position, _, _)
    float p0{0.0f};
    float p1{0.0f};
    float p2{0.0f};
    float p3{0.0f};
};

/// Animated property family. Determines which uniform the sampler
/// writes to on the target layer.
enum class AnimatedProperty : std::uint8_t {
    TransformTranslateX,
    TransformTranslateY,
    TransformScaleX,
    TransformScaleY,
    TransformRotate,
    TransformMatrix,       // full 6-float interpolation
    Opacity,
    FilterBlur,
    ScrollOffsetX,
    ScrollOffsetY,
};

/// One scalar keyframe.
struct Keyframe {
    float time_norm;   // 0.0 ... 1.0
    float value;
};

/// A reusable keyframe curve. Shared across animations that use the
/// same animation-name or transition-timing-function combination.
struct KeyframeCurve {
    std::vector<Keyframe> keyframes;
    EasingKind            easing{EasingKind::Linear};
    EasingParams          easing_params{};
};

using KeyframeCurveId = std::uint16_t;
constexpr KeyframeCurveId kInvalidCurve = 0xFFFF;

/// A single playing animation. ~32 bytes; the active-animations vector
/// is the hot data structure for the compositor's animation pass.
struct ActiveAnimation {
    LayerId          layer{kInvalidLayer};
    AnimatedProperty property{AnimatedProperty::Opacity};
    EasingKind       easing{EasingKind::Linear};
    std::uint8_t     iteration_count{1};  // 0 = infinite
    std::uint8_t     flags{0};            // bit 0: paused; bit 1: reverse; bit 2: alternate; bit 3: finished

    KeyframeCurveId  curve{kInvalidCurve};

    double           start_time_s{0.0};
    float            duration_s{0.3f};
    float            delay_s{0.0f};
};

static_assert(sizeof(ActiveAnimation) <= 32,
              "ActiveAnimation must stay compact; the hot loop iterates this vector");

/// AnimationRuntime owns the keyframe table and the list of currently
/// playing animations. The compositor calls `sample()` once per frame
/// (before composite proper) to advance every animation and write its
/// interpolated value onto the target layer.
class AnimationRuntime {
public:
    AnimationRuntime() = default;

    /// Register a reusable curve. Returns its id for use by
    /// ActiveAnimation::curve.
    KeyframeCurveId add_curve(KeyframeCurve curve);

    /// Start an animation. Returns its index in the active vector
    /// (stable for the animation's lifetime).
    std::uint32_t play(ActiveAnimation anim);

    /// Stop an animation. Optional: also writes the end-state value
    /// onto the layer so the post-anim look is the final keyframe.
    void stop(std::uint32_t index, bool snap_to_end = true);

    /// Pause / resume.
    void pause(std::uint32_t index);
    void resume(std::uint32_t index, double resume_time_s);

    /// Sample every active animation against the wall clock. For each
    /// animation: compute progress, ease it, interpolate keyframes,
    /// write to the target layer's transform/opacity. Marks the layer
    /// composite-dirty (NOT raster-dirty).
    ///
    /// Returns the number of animations sampled (i.e. not paused / not
    /// finished). When this returns zero the next frame can skip the
    /// animation sample step entirely.
    std::uint32_t sample(LayerTree& layers, double time_s);

    /// Drop finished, non-looping animations. Called periodically.
    void gc_finished();

    std::size_t active_count() const noexcept { return animations_.size(); }

private:
    std::vector<KeyframeCurve>   curves_;
    std::vector<ActiveAnimation> animations_;
};

}  // namespace affineui
