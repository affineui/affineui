// Reserved for Phase-2 lexbor adapter code (cascade + selector
// bridge against the real engine pipeline). Phase 1 parses inline in
// src/dom/document.cpp because the work is small enough not to need
// a dedicated module. Kept as an empty TU so the file path stays
// stable across phases.

namespace affineui::detail {

// Anchor symbol — guarantees this TU has something to link, which
// matters on some linkers that warn about pure-comment objects.
void lexbor_bridge_phase2_reserved() noexcept {}

}  // namespace affineui::detail
