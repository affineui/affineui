#include "engine/animation.h"

#include <algorithm>
#include <cmath>

namespace affineui {

namespace {

constexpr std::uint8_t kPausedFlag = 1u << 0;
constexpr std::uint8_t kReverseFlag = 1u << 1;
constexpr std::uint8_t kAlternateFlag = 1u << 2;
constexpr std::uint8_t kFinishedFlag = 1u << 3;

float clamp01(float v) noexcept {
    if (v <= 0.0f) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return v;
}

float solve_cubic_bezier(float x1, float y1, float x2, float y2, float x) {
    auto sample = [](float a1, float a2, float t) {
        const float inv = 1.0f - t;
        return 3.0f * inv * inv * t * a1 +
               3.0f * inv * t * t * a2 +
               t * t * t;
    };
    auto derivative = [](float a1, float a2, float t) {
        const float inv = 1.0f - t;
        return 3.0f * inv * inv * a1 +
               6.0f * inv * t * (a2 - a1) +
               3.0f * t * t * (1.0f - a2);
    };

    float t = x;
    for (int i = 0; i < 8; ++i) {
        const float dx = sample(x1, x2, t) - x;
        const float d = derivative(x1, x2, t);
        if (std::fabs(d) < 1e-5f) break;
        t = clamp01(t - dx / d);
    }
    return sample(y1, y2, t);
}

float ease(EasingKind kind, EasingParams params, float t) {
    t = clamp01(t);
    switch (kind) {
        case EasingKind::Linear:
            return t;
        case EasingKind::EaseIn:
            return t * t * t;
        case EasingKind::EaseOut: {
            const float inv = 1.0f - t;
            return 1.0f - inv * inv * inv;
        }
        case EasingKind::EaseInOut:
            return t < 0.5f
                ? 4.0f * t * t * t
                : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
        case EasingKind::CubicBezier:
            return solve_cubic_bezier(params.p0, params.p1,
                                      params.p2, params.p3, t);
        case EasingKind::Steps: {
            const float count = std::max(1.0f, params.p0);
            const bool step_start = params.p1 != 0.0f;
            const float pos = step_start ? std::ceil(t * count)
                                         : std::floor(t * count);
            return clamp01(pos / count);
        }
        case EasingKind::Spring: {
            const float stiffness = params.p0 > 0.0f ? params.p0 : 170.0f;
            const float damping = params.p1 > 0.0f ? params.p1 : 26.0f;
            const float mass = params.p2 > 0.0f ? params.p2 : 1.0f;
            const float omega = std::sqrt(stiffness / mass);
            const float decay = std::exp(-damping * t / (2.0f * mass));
            return clamp01(1.0f - decay * std::cos(omega * t));
        }
    }
    return t;
}

float sample_curve(const KeyframeCurve& curve, float t) {
    if (curve.keyframes.empty()) return t;
    if (curve.keyframes.size() == 1) return curve.keyframes.front().value;
    const auto& keyframes = curve.keyframes;

    if (t <= keyframes.front().time_norm) return keyframes.front().value;
    if (t >= keyframes.back().time_norm) return keyframes.back().value;

    for (std::size_t i = 1; i < keyframes.size(); ++i) {
        const auto& right = keyframes[i];
        if (t > right.time_norm) continue;
        const auto& left = keyframes[i - 1];
        const float span = std::max(1e-6f, right.time_norm - left.time_norm);
        const float local = ease(curve.easing, curve.easing_params,
                                 (t - left.time_norm) / span);
        return left.value + (right.value - left.value) * local;
    }
    return keyframes.back().value;
}

void write_layer_value(Layer& layer, AnimatedProperty property, float value) {
    switch (property) {
        case AnimatedProperty::TransformTranslateX:
            layer.transform.tx = value;
            layer.flags |= LF_HasTransform;
            break;
        case AnimatedProperty::TransformTranslateY:
            layer.transform.ty = value;
            layer.flags |= LF_HasTransform;
            break;
        case AnimatedProperty::TransformScaleX:
            layer.transform.a = value;
            layer.flags |= LF_HasTransform;
            break;
        case AnimatedProperty::TransformScaleY:
            layer.transform.d = value;
            layer.flags |= LF_HasTransform;
            break;
        case AnimatedProperty::TransformRotate: {
            const float tx = layer.transform.tx;
            const float ty = layer.transform.ty;
            layer.transform = Mat2x3::rotate(value);
            layer.transform.tx = tx;
            layer.transform.ty = ty;
            layer.flags |= LF_HasTransform;
            break;
        }
        case AnimatedProperty::TransformMatrix:
            break;
        case AnimatedProperty::Opacity:
            layer.opacity = clamp01(value);
            layer.flags |= LF_HasOpacity;
            break;
        case AnimatedProperty::FilterBlur:
        case AnimatedProperty::ScrollOffsetX:
        case AnimatedProperty::ScrollOffsetY:
            break;
    }
    layer.mark_composite_dirty();
}

}  // namespace

KeyframeCurveId AnimationRuntime::add_curve(KeyframeCurve curve) {
    const auto id = static_cast<KeyframeCurveId>(curves_.size());
    std::sort(curve.keyframes.begin(), curve.keyframes.end(),
              [](const Keyframe& a, const Keyframe& b) {
                  return a.time_norm < b.time_norm;
              });
    curves_.push_back(std::move(curve));
    return id;
}

std::uint32_t AnimationRuntime::play(ActiveAnimation anim) {
    const auto idx = static_cast<std::uint32_t>(animations_.size());
    animations_.push_back(anim);
    return idx;
}

void AnimationRuntime::stop(std::uint32_t index, bool /*snap_to_end*/) {
    if (index >= animations_.size()) return;
    animations_[index].flags |= kFinishedFlag;
}

void AnimationRuntime::pause(std::uint32_t index) {
    if (index < animations_.size()) animations_[index].flags |= kPausedFlag;
}

void AnimationRuntime::resume(std::uint32_t index, double resume_time_s) {
    if (index >= animations_.size()) return;
    auto& a = animations_[index];
    a.flags &= static_cast<std::uint8_t>(~kPausedFlag);
    a.start_time_s = resume_time_s;
}

std::uint32_t AnimationRuntime::sample(LayerTree& layers, double time_s) {
    std::uint32_t sampled = 0;
    for (auto& a : animations_) {
        if (a.flags & (kPausedFlag | kFinishedFlag)) continue;
        if (a.layer >= layers.size()) continue;
        if (a.curve >= curves_.size()) continue;

        const double local_time =
            time_s - a.start_time_s - static_cast<double>(a.delay_s);
        if (local_time < 0.0) continue;

        const float duration = std::max(a.duration_s, 1e-6f);
        const double raw_iteration = local_time / static_cast<double>(duration);
        const auto completed_iterations =
            static_cast<std::uint32_t>(std::floor(raw_iteration));
        const bool infinite = a.iteration_count == 0;
        const bool finished =
            !infinite && completed_iterations >= a.iteration_count;
        const std::uint32_t iteration_index = finished
            ? static_cast<std::uint32_t>(std::max<int>(0, a.iteration_count - 1))
            : completed_iterations;

        float t = finished
            ? 1.0f
            : static_cast<float>(raw_iteration -
                                 std::floor(raw_iteration));
        if (finished) {
            t = 1.0f;
        }

        const bool alternate =
            (a.flags & kAlternateFlag) != 0 &&
            (iteration_index % 2u) == 1u;
        if ((a.flags & kReverseFlag) != 0 || alternate) {
            t = 1.0f - t;
        }

        write_layer_value(layers.at(a.layer), a.property,
                          sample_curve(curves_[a.curve], t));
        if (finished) {
            a.flags |= kFinishedFlag;
        } else {
            ++sampled;
        }
        if (finished) ++sampled;
    }
    return sampled;
}

void AnimationRuntime::gc_finished() {
    animations_.erase(
        std::remove_if(animations_.begin(), animations_.end(),
            [](const ActiveAnimation& a) { return (a.flags & kFinishedFlag) != 0; }),
        animations_.end());
}

}  // namespace affineui
