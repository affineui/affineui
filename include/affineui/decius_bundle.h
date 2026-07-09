// Bundled Decius CSS framework — public API for the language bindings and
// for direct C++ consumers that want the default look.
//
// When compiled with `-DAFFINEUI_NO_BUNDLE_DECIUS`, `available()` returns
// false and the other functions return empty strings / no-op. Consumers
// can check `available()` at runtime and gracefully fall back to their
// own stylesheet-loading path.

#pragma once

#include <string>
#include <string_view>

namespace affineui {

class App;

namespace decius {

// True when the CSS bundle was compiled in (i.e., -DAFFINEUI_NO_BUNDLE_DECIUS
// was NOT set at build time).
bool available();

// Return the Decius CSS as a fresh std::string, decompressed on demand.
// Empty when the bundle isn't compiled in.
std::string css_bundle();

// Return the bytes for a bundled asset by URL suffix (e.g.
// "frameworks/fonts/decius-icons.woff2"). Empty when the URL isn't in
// the bundle. Intended for use by App's resource-loader hook so CSS
// url()s resolve against the embedded fonts.
std::string load(std::string_view url_suffix);

// Wire the bundled CSS into an App: sets the app's stylesheet to the
// Decius bundle. No-op when the bundle isn't compiled in.
void apply(App& app);

}  // namespace decius
}  // namespace affineui
