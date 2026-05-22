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
        // Headings are bold in the HTML5 UA stylesheet (browsers render
        // h1–h6 at font-weight:bold). Without this they came out regular.
        "h1{font-size:2em;font-weight:bold;margin:.67em 0}"
        "h2{font-size:1.5em;font-weight:bold;margin:.75em 0}"
        "h3{font-size:1.17em;font-weight:bold;margin:.83em 0}"
        "h4{font-weight:bold;margin:1.12em 0}"
        "h5{font-size:.83em;font-weight:bold;margin:1.5em 0}"
        "h6{font-size:.67em;font-weight:bold;margin:1.67em 0}"
        "p{margin:1em 0}"
        "a{color:#0366d6;text-decoration:underline}"
        "strong,b{font-weight:bold}"
        "em,i{font-style:italic}"
        "ul,ol{padding-left:40px;margin:1em 0}"
        "img{display:inline-block}"
        // Form controls default to inline-block (HTML5 UA stylesheet).
        "button,input,select,textarea{font:inherit;display:inline-block}"
        // Phrasing elements (including `a`) default to inline so they flow
        // with their inline siblings via the synthetic line-box wrapper.
        // When such an element is the child of a flex container it is
        // blockified into a flex item (CSS flex item rules) — see the
        // flex-parent check in document.cpp's child collection — so the
        // navbar/nav-link case (an `a` flex item) behaves correctly
        // without leaving `a` out of this UA default.
        "a,span,strong,b,em,i,code,kbd,samp{display:inline}"
        "code,pre,kbd,samp{font-family:monospace}"
        // CSS table model defaults (HTML5 UA stylesheet).
        "table{display:table}"
        "thead{display:table-row-group}"
        "tbody{display:table-row-group}"
        "tfoot{display:table-row-group}"
        "tr{display:table-row}"
        "td{display:table-cell}"
        "th{display:table-cell;font-weight:bold;text-align:center}";
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
