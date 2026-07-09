// Compile-time bundled Decius CSS framework — the default look for the
// component API. `affineui_decius_apply()` loads the embedded stylesheet
// into an App so `View(ViewTheme::Decius)` gets styled widgets out of the
// box with zero on-disk asset copying.
//
// The bundle lives inside this translation unit as a zlib-compressed
// byte array (see tools/embed_decius.py); decompression uses
// `stbi_zlib_decode_buffer` which is already vendored via stb_image.h
// for the PNG loader. No new dependency, no runtime allocation of an
// mz_stream, ~250 KB of raw assets → ~120 KB embedded compressed.
//
// Ignore with `-DAFFINEUI_NO_BUNDLE_DECIUS` (cmake option
// `AFFINEUI_BUNDLE_DECIUS=OFF`) — used for the two-file drop-in when
// the host wants to ship no default assets.

#ifndef AFFINEUI_NO_BUNDLE_DECIUS

#include "affineui/decius_bundle.h"

#include "affineui/app.h"

#include <string>

// The generated header is written into the source tree at
// include/affineui/_decius_bundle.h (leading underscore = generated,
// gitignored). Living inside include/ lets the amalgamator inline it
// with everything else — no bespoke -I flag needed.
#include "affineui/_decius_bundle.h"

// stb_image.h provides `stbi_zlib_decode_buffer` — the zlib inflater we
// already use for PNG chunks. Forward-declaring it as `extern "C"` here
// avoids pulling all of stb_image.h into this small compilation unit.
extern "C" int stbi_zlib_decode_buffer(char* dst, int dst_len,
                                       const char* src, int src_len);

namespace affineui::decius {

namespace {

// Inflate one entry from the generated manifest. Returns an empty string
// on failure so the caller can pass that straight through as "no bundle".
std::string inflate(const BundledAsset& entry) {
    std::string out(entry.raw_len, '\0');
    const int decoded = stbi_zlib_decode_buffer(
        out.data(), static_cast<int>(entry.raw_len),
        reinterpret_cast<const char*>(entry.zlib_bytes),
        static_cast<int>(entry.zlib_len));
    if (decoded < 0 || static_cast<size_t>(decoded) != entry.raw_len) {
        return {};
    }
    return out;
}

// Find an asset in the manifest by its URL suffix.
const BundledAsset* find_asset(std::string_view url_suffix) {
    for (size_t i = 0; i < ASSET_COUNT; ++i) {
        if (url_suffix == ASSETS[i].url_suffix) {
            return &ASSETS[i];
        }
    }
    return nullptr;
}

}  // namespace

std::string css_bundle() {
    // The CSS is entry [0] in the manifest by construction (see
    // tools/embed_decius.py — ASSETS list order).
    return inflate(ASSETS[0]);
}

std::string load(std::string_view url_suffix) {
    const auto* entry = find_asset(url_suffix);
    return entry ? inflate(*entry) : std::string{};
}

bool available() { return ASSET_COUNT > 0; }

void apply(App& app) {
    std::string css = css_bundle();
    if (css.empty()) return;  // shouldn't happen — bundle is compiled in
    // Base URL "frameworks/css/" so relative url()s inside the sheet
    // (e.g. `url(../fonts/decius-icons.woff2)`) resolve to the
    // frameworks/fonts/* suffixes the resource-loader hook serves.
    // (Font URL hook is a follow-up — for now icons render as blank
    // glyphs and the rest of the widget styling comes through.)
    app.set_stylesheet(css, "frameworks/css/");
}

}  // namespace affineui::decius

#endif  // AFFINEUI_NO_BUNDLE_DECIUS
