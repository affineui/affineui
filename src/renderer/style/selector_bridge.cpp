// Adapter between affineui's cascade walker and lexbor's selector
// engine. lexbor exposes lxb_selectors_t which, given a stylesheet and
// a DOM node, fires a callback for each matching rule. We accumulate
// those into a per-element rule list and hand them to cascade.cpp.

namespace affineui::style {

void selector_bridge_stub() {
    // intentional no-op
}

}  // namespace affineui::style
