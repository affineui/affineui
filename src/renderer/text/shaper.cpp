// Text shaping interface.
//
// Default impl: glyph-per-codepoint (works for Latin / Cyrillic / Greek).
// Opt-in HarfBuzz impl behind AFFINEUI_ENABLE_HARFBUZZ for Arabic /
// Devanagari / complex emoji / ligatures.
//
// Shapes a run into a vector of (glyph_id, advance_x, offset_y) tuples
// that the inline layout can position.

namespace affineui::text {

void shaper_stub() {
    // intentional no-op
}

}  // namespace affineui::text
