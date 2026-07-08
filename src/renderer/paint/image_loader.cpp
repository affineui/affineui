// Image loader: process-wide LRU cache keyed by URL.
//
// Decoding via stb_image (PNG / JPG / GIF / BMP / TGA). Decoded pixels
// uploaded to NanoVG as RGBA textures. Cache size capped — entries
// evicted when total uploaded bytes exceeds the limit.

namespace affineui::paint {

void image_loader_stub() {
    // intentional no-op
}

}  // namespace affineui::paint
