// Typed C++ views over lexbor DOM nodes.
//
// We do not own the DOM — lexbor does. This file provides RAII-friendly
// wrappers (`Element`, `TextNode`) that hold `lxb_dom_*` pointers and
// expose ergonomic accessors. Style/Box attachments live in lexbor's
// custom-data slot on each node, so this header is the natural place
// to keep that attachment logic.

namespace affineui::dom {

void dom_view_stub() {
    // intentional no-op; fully fleshed once lexbor is wired
}

}  // namespace affineui::dom
