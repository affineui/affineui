// Paint driver. Now thinned down to a header — the replay() inline
// function in internal/display_list_painter.h owns the dispatch loop
// (and lives where the DisplayListBuilder Painter is defined, so the
// recording and playback halves can't drift out of sync).
//
// This TU is kept in the build so future per-subtree paint walkers
// (Phase 2D when we have multiple layers) have an obvious home.

namespace affineui::paint {

void paint_driver_reserved() noexcept {}

}  // namespace affineui::paint
