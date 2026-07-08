#include <doctest/doctest.h>

#include "renderer/engine/animation.h"

namespace {

affineui::KeyframeCurve linear_curve(float from, float to) {
    affineui::KeyframeCurve curve;
    curve.easing = affineui::EasingKind::Linear;
    curve.keyframes.push_back({0.0f, from});
    curve.keyframes.push_back({1.0f, to});
    return curve;
}

}  // namespace

TEST_CASE("animation runtime interpolates opacity onto the target layer") {
    affineui::LayerTree layers;
    affineui::AnimationRuntime runtime;
    const auto curve = runtime.add_curve(linear_curve(0.0f, 1.0f));

    affineui::ActiveAnimation anim;
    anim.layer = layers.root();
    anim.property = affineui::AnimatedProperty::Opacity;
    anim.curve = curve;
    anim.start_time_s = 10.0;
    anim.duration_s = 2.0f;
    runtime.play(anim);

    const auto sampled = runtime.sample(layers, 11.0);

    CHECK(sampled == 1);
    CHECK(layers.at(layers.root()).opacity == doctest::Approx(0.5f));
    CHECK(layers.at(layers.root()).dirty_composite());
}

TEST_CASE("animation runtime respects delay before sampling") {
    affineui::LayerTree layers;
    affineui::AnimationRuntime runtime;
    const auto curve = runtime.add_curve(linear_curve(0.0f, 1.0f));

    affineui::ActiveAnimation anim;
    anim.layer = layers.root();
    anim.property = affineui::AnimatedProperty::Opacity;
    anim.curve = curve;
    anim.start_time_s = 10.0;
    anim.delay_s = 1.0f;
    anim.duration_s = 2.0f;
    runtime.play(anim);

    CHECK(runtime.sample(layers, 10.5) == 0);
    CHECK(layers.at(layers.root()).opacity == doctest::Approx(1.0f));

    CHECK(runtime.sample(layers, 12.0) == 1);
    CHECK(layers.at(layers.root()).opacity == doctest::Approx(0.5f));
}

TEST_CASE("animation runtime writes transform channels without repainting") {
    affineui::LayerTree layers;
    affineui::AnimationRuntime runtime;
    const auto curve = runtime.add_curve(linear_curve(0.0f, 100.0f));
    layers.at(layers.root()).clear_raster_dirty();
    layers.at(layers.root()).clear_composite_dirty();

    affineui::ActiveAnimation anim;
    anim.layer = layers.root();
    anim.property = affineui::AnimatedProperty::TransformTranslateX;
    anim.curve = curve;
    anim.start_time_s = 0.0;
    anim.duration_s = 4.0f;
    runtime.play(anim);

    CHECK(runtime.sample(layers, 1.0) == 1);
    const auto& layer = layers.at(layers.root());
    CHECK(layer.transform.tx == doctest::Approx(25.0f));
    CHECK(layer.dirty_composite());
    CHECK_FALSE(layer.dirty_raster());
}

TEST_CASE("animation runtime snaps a finished animation to its final keyframe") {
    affineui::LayerTree layers;
    affineui::AnimationRuntime runtime;
    const auto curve = runtime.add_curve(linear_curve(0.25f, 0.75f));

    affineui::ActiveAnimation anim;
    anim.layer = layers.root();
    anim.property = affineui::AnimatedProperty::Opacity;
    anim.curve = curve;
    anim.start_time_s = 0.0;
    anim.duration_s = 1.0f;
    runtime.play(anim);

    CHECK(runtime.sample(layers, 2.0) == 1);
    CHECK(layers.at(layers.root()).opacity == doctest::Approx(0.75f));
    CHECK(runtime.sample(layers, 2.5) == 0);

    runtime.gc_finished();
    CHECK(runtime.active_count() == 0);
}

TEST_CASE("animation runtime garbage collection preserves paused animations") {
    affineui::LayerTree layers;
    affineui::AnimationRuntime runtime;
    const auto curve = runtime.add_curve(linear_curve(0.0f, 1.0f));

    affineui::ActiveAnimation anim;
    anim.layer = layers.root();
    anim.property = affineui::AnimatedProperty::Opacity;
    anim.curve = curve;
    const auto id = runtime.play(anim);

    runtime.pause(id);
    runtime.gc_finished();

    CHECK(runtime.active_count() == 1);
    CHECK(runtime.sample(layers, 0.5) == 0);
}

TEST_CASE("animation runtime alternate direction finishes on the last iteration end") {
    affineui::LayerTree layers;
    affineui::AnimationRuntime runtime;
    const auto curve = runtime.add_curve(linear_curve(0.0f, 1.0f));

    affineui::ActiveAnimation anim;
    anim.layer = layers.root();
    anim.property = affineui::AnimatedProperty::Opacity;
    anim.curve = curve;
    anim.duration_s = 1.0f;
    anim.iteration_count = 2;
    anim.flags = 1u << 2;  // alternate
    runtime.play(anim);

    CHECK(runtime.sample(layers, 2.0) == 1);
    CHECK(layers.at(layers.root()).opacity == doctest::Approx(0.0f));
}
