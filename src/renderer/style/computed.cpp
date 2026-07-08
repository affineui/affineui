// ComputedStyle: the resolved set of CSS properties for one element.
//
// Designed to be hot, ~256 bytes, trivially copyable, layout-cache
// friendly. Inherited properties propagate from parent during cascade;
// percentage and `auto` values resolve to absolute pixels during layout.

namespace affineui::style {

void computed_stub() {
    // intentional no-op
}

}  // namespace affineui::style
