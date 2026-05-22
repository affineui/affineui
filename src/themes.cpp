#include "affineui/themes.h"

// Bundled theme stylesheets. Stored as raw string literals so they
// travel with the library and need no external file at runtime. The
// content is intentionally small in this scaffolding pass — final
// stylesheets land alongside the corresponding example demos in
// examples/01_bootstrap / examples/02_material.

namespace affineui::theme {

std::string_view ua_default() {
    static constexpr std::string_view kCss =
        // Minimal user-agent baseline. Mirrors the parts of the HTML5
        // default stylesheet we actually depend on.
        //
        // Browser-compatible defaults. Page gutters are author CSS,
        // not user-agent CSS; frameworks such as Bootstrap assume
        // body padding starts at zero.
        "html,body{margin:0;padding:0}"
        // No `line-height` here: the CSS initial value is `normal`, which
        // is what browsers use on <body>. Setting a fixed multiplier (we
        // used to set 1.5) inherits into every element and inflates every
        // line box ~50% vs a real browser. Authors set line-height when
        // they want it; the UA default stays `normal`.
        "body{font-family:sans-serif;font-size:16px;"
        "color:#1f2328;background:#ffffff}"
        "h1{font-size:2em;margin:.67em 0}"
        "h2{font-size:1.5em;margin:.75em 0}"
        "h3{font-size:1.17em;margin:.83em 0}"
        "h4{margin:1.12em 0}"
        "p{margin:1em 0}"
        "a{color:#0366d6;text-decoration:underline}"
        "strong,b{font-weight:bold}"
        "em,i{font-style:italic}"
        "ul,ol{padding-left:40px;margin:1em 0}"
        "img{display:inline-block}"
        // Form controls default to inline-block (HTML5 UA stylesheet).
        "button,input,select,textarea{font:inherit;display:inline-block}"
        // Phrasing elements default to inline so they flow with other
        // inline siblings via the synthetic line-box wrapper. `a` is
        // intentionally left out — it's commonly a flex item in
        // navbars where an explicit inline override clashes.
        "span,strong,b,em,i,code,kbd,samp{display:inline}"
        "code,pre,kbd,samp{font-family:monospace}";
    return kCss;
}

std::string_view material_dark() {
    static constexpr std::string_view kCss = "";  // filled in alongside examples/02_material
    return kCss;
}

std::string_view material_light() {
    static constexpr std::string_view kCss = "";
    return kCss;
}

std::string_view bootstrap_dark() {
    static constexpr std::string_view kCss = "";
    return kCss;
}

std::string_view bootstrap_light() {
    static constexpr std::string_view kCss = "";
    return kCss;
}

}  // namespace affineui::theme
