// Flexbox layout via Yoga adapter.
//
// We do not implement flexbox math ourselves — Yoga is the reference
// impl and battle-tested in React Native. This file is the translation:
//
//   ComputedStyle  ──translate──▶  YGNodeStyleSetFlex* calls
//   YGNodeCalculateLayout()
//   YGNodeLayoutGet*           ──translate──▶  Box rectangles
//
// Yoga is unaware of the rest of CSS — `width: 50%` on a flex item
// must be resolved against its flex container before we hand it over.

namespace affineui::layout {

void flex_stub() {
    // intentional no-op
}

}  // namespace affineui::layout
