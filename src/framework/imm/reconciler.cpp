// Virtual DOM reconciler (React-style).
//
// Given prev_tree and new_tree (both built by the imm calls), produce
// the minimal patch sequence to transform the retained DOM into the
// shape of new_tree:
//
//   Patch ops:
//     - INSERT(node, parent, index)
//     - REMOVE(node)
//     - MOVE(node, new_index)
//     - SET_ATTR(node, name, value)
//     - REMOVE_ATTR(node, name)
//     - SET_TEXT(node, text)
//     - REPLACE_HANDLER(node, event, fn_id)
//
// Patches are then applied to lexbor's DOM via its mutation API.
//
// Keying: by default the call-site path; explicit .key(...) overrides
// for keyed-children semantics matching React.

namespace affineui::imm {

void reconciler_stub() {
    // intentional no-op
}

}  // namespace affineui::imm
