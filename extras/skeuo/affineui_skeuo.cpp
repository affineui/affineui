// affineui_skeuo.cpp — skeuomorphic hardware widget kit implementation.
//
// The jack/LCD/plug artwork and the patch-cable geometry + physics are
// ported from the decius.css site's skeuomorphic showpiece
// (sections-skeuomorphic.jsx) so the native kit renders the same gear.

#include "affineui_skeuo.h"

#include <affineui/painter.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace affineui::skeuo {

std::string_view stylesheet() {
    return R"CSS(
/* ── affineui_skeuo kit addendum ─────────────────────────────────────
   Rules for compositions the kit defines beyond the framework CSS. */

/* The kit puts 3-letter glyphs on 26px --sm lit buttons (the reference
   site only ever uses them blank). Rendered with the embedded Roboto
   they need tighter type than the framework's 10px/.08em to keep air
   inside the cap. */
.dcs-litbtn--sm { font-size: 8px; letter-spacing: .05em; }

/* Piano keyboard strip. White keys are flex columns per octave; black
   keys overlay on absolute percentage lefts (piano-true positions). */
.skeuo-kbd {
  display: flex;
  height: 88px;
  padding: 3px 3px 5px;
  background: linear-gradient(180deg, #05060a, #14161c 12%, #0b0d12);
  border-radius: 4px;
  box-shadow: inset 0 2px 6px rgba(0,0,0,.8), inset 0 -1px 0 rgba(255,255,255,.05);
}
.skeuo-kbd__oct { flex: 1 1 0; position: relative; display: flex; gap: 1px; padding: 0 1px; }
.skeuo-kbd__white {
  flex: 1 1 0;
  background: linear-gradient(180deg, #f6f4ee 0%, #e8e5da 70%, #c9c5b8 96%, #a8a498 100%);
  border: 1px solid #1a1c22;
  border-radius: 0 0 3px 3px;
  cursor: pointer;
  box-shadow: inset 0 -3px 4px rgba(0,0,0,.18), 0 1px 0 rgba(0,0,0,.5);
}
.skeuo-kbd__white:hover { background: linear-gradient(180deg, #fdfbf5, #efece1 70%, #d2cec1 96%, #b0aca0 100%); }
.skeuo-kbd__white:active {
  background: linear-gradient(180deg, #ddd9cd, #cbc7ba 80%, #b4b0a4);
  box-shadow: inset 0 2px 4px rgba(0,0,0,.35);
}
.skeuo-kbd__black {
  position: absolute;
  top: 0;
  width: 9.6%;
  height: 58%;
  background: linear-gradient(180deg, #4a4c52 0%, #26282e 18%, #0c0d10 90%);
  border: 1px solid #000;
  border-radius: 0 0 2px 2px;
  cursor: pointer;
  z-index: 2;
  box-shadow: inset 0 1px 0 rgba(255,255,255,.18), 0 3px 5px rgba(0,0,0,.6);
}
.skeuo-kbd__black:hover { background: linear-gradient(180deg, #55575d 0%, #303238 18%, #131418 90%); }
.skeuo-kbd__black:active {
  background: linear-gradient(180deg, #303238 0%, #1a1c20 30%, #0a0b0e 90%);
  box-shadow: inset 0 2px 3px rgba(0,0,0,.7);
}
)CSS";
}

namespace {

// Total "rope" length in px — a cable never stretches; sag is maximal
// when its jacks are close and fades out as the chord approaches it.
constexpr float kCableLength = 640.0f;

std::string num(double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    // Trim trailing zeros / dot for stable, compact attribute diffs.
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s.empty() || s == "-0") s = "0";
    return s;
}

std::string pct(double v01) { return num(v01 * 100.0) + "%"; }

std::string_view finish_class(HwFinish f) {
    switch (f) {
        case HwFinish::Brushed: return "dcs-hw dcs-hw--brushed";
        case HwFinish::Lacquer: return "dcs-hw dcs-hw--lacquer";
        case HwFinish::Cream:   return "dcs-hw dcs-hw--cream";
        case HwFinish::Red:     return "dcs-hw dcs-hw--red";
        case HwFinish::Steel:   break;
    }
    return "dcs-hw";
}

std::string_view lit_color_class(LitColor c) {
    switch (c) {
        case LitColor::Cyan:  return " dcs-litbtn--cyan";
        case LitColor::Amber: return " dcs-litbtn--amber";
        case LitColor::Green: return " dcs-litbtn--green";
        case LitColor::Red:   break;
    }
    return "";
}

std::string_view lcd_color_class(LcdColor c) {
    switch (c) {
        case LcdColor::Amber: return " dcs-lcd--amber";
        case LcdColor::Green: return " dcs-lcd--green";
        case LcdColor::Blue:  return " dcs-lcd--blue";
        case LcdColor::Red:   break;
    }
    return "";
}

std::string_view lcd_size_class(LcdSize s) {
    switch (s) {
        case LcdSize::Sm: return " dcs-lcd--sm";
        case LcdSize::Lg: return " dcs-lcd--lg";
        case LcdSize::Md: break;
    }
    return " dcs-lcd--md";
}

// ── Jack socket artwork (TS/TRS socket with chrome hex nut) ──────────
std::string jack_svg(bool patched) {
    std::string s;
    s.reserve(1600);
    s += "<svg viewBox=\"0 0 32 32\" aria-hidden=\"true\">"
         "<defs>"
         "<linearGradient id=\"jack-nut\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">"
         "<stop offset=\"0%\" stop-color=\"#e8eaee\"/>"
         "<stop offset=\"35%\" stop-color=\"#b8bcc4\"/>"
         "<stop offset=\"65%\" stop-color=\"#7a7e86\"/>"
         "<stop offset=\"100%\" stop-color=\"#3a3d44\"/>"
         "</linearGradient>"
         "<radialGradient id=\"jack-bevel\" cx=\"0.45\" cy=\"0.35\" r=\"0.7\">"
         "<stop offset=\"0%\" stop-color=\"#f0f2f5\"/>"
         "<stop offset=\"40%\" stop-color=\"#9a9ea6\"/>"
         "<stop offset=\"100%\" stop-color=\"#2a2d33\"/>"
         "</radialGradient>"
         "<radialGradient id=\"jack-inner\" cx=\"0.5\" cy=\"0.5\" r=\"0.5\">"
         "<stop offset=\"0%\" stop-color=\"#3a3d44\"/>"
         "<stop offset=\"100%\" stop-color=\"#181a20\"/>"
         "</radialGradient>"
         "<radialGradient id=\"jack-hole\" cx=\"0.5\" cy=\"0.4\" r=\"0.5\">"
         "<stop offset=\"0%\" stop-color=\"#0a0a0a\"/>"
         "<stop offset=\"85%\" stop-color=\"#000\"/>"
         "<stop offset=\"100%\" stop-color=\"#000\"/>"
         "</radialGradient>"
         "</defs>"
         // Hex nut with brushed metal shading (flat-top hexagon, r=15).
         "<polygon points=\"31,16 23.5,28.99 8.5,28.99 1,16 8.5,3.01 23.5,3.01\""
         " fill=\"url(#jack-nut)\" stroke=\"rgba(0,0,0,.55)\""
         " stroke-width=\"0.75\"/>"
         // Chrome beveled ring / recessed ring / centre hole.
         "<circle cx=\"16\" cy=\"16\" r=\"11\" fill=\"url(#jack-bevel)\"/>"
         "<circle cx=\"16\" cy=\"16\" r=\"7.5\" fill=\"url(#jack-inner)\"/>"
         "<circle cx=\"16\" cy=\"16\" r=\"4\" fill=\"url(#jack-hole)\"/>"
         "<circle cx=\"16\" cy=\"16\" r=\"4\" fill=\"none\""
         " stroke=\"rgba(0,0,0,.6)\" stroke-width=\"0.5\"/>"
         // Highlight glint on the top-left of the bevel.
         "<path d=\"M 9 12 a 7 7 0 0 1 5 -3\" fill=\"none\""
         " stroke=\"rgba(255,255,255,.45)\" stroke-width=\"1\""
         " stroke-linecap=\"round\"/>";
    if (patched) {
        // Patch indicator — orange plug visible in the socket.
        s += "<circle cx=\"16\" cy=\"16\" r=\"3\" fill=\"#f8a23a\"/>"
             "<circle cx=\"15\" cy=\"15\" r=\"1\""
             " fill=\"rgba(255,255,255,.5)\"/>";
    }
    s += "</svg>";
    return s;
}

// ── 7-segment glyphs ─────────────────────────────────────────────────
// segments: a=top, b=top-r, c=bot-r, d=bot, e=bot-l, f=top-l, g=mid
std::string_view lcd_segments_for(char ch) {
    switch (ch) {
        case '0': return "abcdef";
        case '1': return "bc";
        case '2': return "abged";
        case '3': return "abgcd";
        case '4': return "fbgc";
        case '5': return "afgcd";
        case '6': return "afgecd";
        case '7': return "abc";
        case '8': return "abcdefg";
        case '9': return "abcfgd";
        case 'A': return "abcefg";
        case 'B': return "cdefg";
        case 'C': return "adef";
        case 'D': return "bcdeg";
        case 'E': return "adefg";
        case 'F': return "aefg";
        case 'H': return "bcefg";
        case 'I': return "ef";
        case 'J': return "bcde";
        case 'K': return "efg";
        case 'L': return "def";
        case 'O': return "abcdef";
        case 'P': return "abefg";
        case 'R': return "efg";
        case 'S': return "afgcd";
        case 'U': return "bcdef";
        case '-': return "g";
        default:  return "";
    }
}

std::string_view lcd_segment_path(char seg) {
    switch (seg) {
        case 'a': return "M 5 2 L 19 2 L 17 5 L 7 5 Z";
        case 'b': return "M 19 3 L 21 5 L 21 18 L 19 19 L 17 17 L 17 6 Z";
        case 'c': return "M 19 21 L 21 22 L 21 35 L 19 37 L 17 34 L 17 23 Z";
        case 'd': return "M 5 38 L 19 38 L 17 35 L 7 35 Z";
        case 'e': return "M 5 21 L 7 23 L 7 34 L 5 37 L 3 35 L 3 22 Z";
        case 'f': return "M 5 3 L 7 6 L 7 17 L 5 19 L 3 18 L 3 5 Z";
        case 'g': return "M 5 20 L 7 18 L 17 18 L 19 20 L 17 22 L 7 22 Z";
    }
    return "";
}

// Uppercase + left-pad to the fixed display width (only non-dot
// characters count toward the width).
std::string lcd_padded(std::string_view value, int digits) {
    std::string upper(value);
    for (auto& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (digits > 0) {
        int glyphs = 0;
        for (char c : upper) {
            if (c != '.') ++glyphs;
        }
        if (glyphs < digits) {
            upper.insert(0, static_cast<std::size_t>(digits - glyphs), ' ');
        }
    }
    return upper;
}

// One 7-segment digit as RETAINED elements (div > svg > 7 paths). A
// value change then diffs to a handful of `fill` attribute writes on
// the flipped segments — never a raw-HTML swap, which would force a
// structural settle per LCD update (measured ~35 ms on big documents;
// it made every knob that refreshes an LCD feel like treacle).
void lcd_digit(View& v, char ch, std::string_view key) {
    auto digit = v.element("div", "dcs-lcd__digit", key);
    auto svg = v.element("svg", {}, "__svg");
    svg.attr("viewBox", "0 0 24 40");
    const auto on = lcd_segments_for(ch);
    for (char seg : {'a', 'b', 'c', 'd', 'e', 'f', 'g'}) {
        const bool lit = on.find(seg) != std::string_view::npos;
        auto p = v.element_ref("path", {}, std::string_view(&seg, 1));
        p.attr("d", lcd_segment_path(seg));
        if (lit) {
            // Lit segments get a faint same-colour stroke as the glow
            // (the CSS drop-shadow filter equivalent).
            p.attr("fill", "var(--lcd-on)");
            p.attr("stroke", "var(--lcd-on)");
            p.attr("stroke-opacity", "0.35");
            p.attr("stroke-width", "1.6");
        } else {
            p.attr("fill", "var(--lcd-off)");
            p.remove_attr("stroke");
            p.remove_attr("stroke-opacity");
            p.remove_attr("stroke-width");
        }
    }
}

// ── Cable geometry (constant-length catenary, ported verbatim) ───────
struct Pt {
    float x{0.0f};
    float y{0.0f};
};

float cable_sag(const Pt& a, const Pt& b) {
    const float chord = std::hypot(b.x - a.x, b.y - a.y);
    if (chord >= kCableLength) return 0.0f;
    return std::sqrt(std::max(0.0f, kCableLength * kCableLength -
                                        chord * chord)) *
           0.5f;
}

Pt cable_rest_bob(const Pt& a, const Pt& b) {
    return Pt{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f + cable_sag(a, b)};
}

// Smooth cubic through the (possibly swinging) midpoint: control points
// slightly past it so the curve passes through without a corner. Emits
// Painter path verbs straight into `out` — cable geometry is floats end
// to end (physics → path → rasterizer), never strings.
void cable_cmds(std::vector<float>& out, const Pt& a, const Pt& b,
                const Pt& m) {
    const float c1x = a.x + (m.x - a.x) * 0.5f;
    const float c1y = a.y + (m.y - a.y) * 1.10f;
    const float c2x = b.x + (m.x - b.x) * 0.5f;
    const float c2y = b.y + (m.y - b.y) * 1.10f;
    out.clear();
    out.insert(out.end(), {kPathMove, a.x, a.y,
                           kPathCubic, c1x, c1y, c2x, c2y, b.x, b.y});
}

// Axis-aligned ellipse as four cubics (kappa approximation).
void ellipse_cmds(std::vector<float>& out, float cx, float cy, float rx,
                  float ry) {
    constexpr float k = 0.5522847498f;
    const float kx = rx * k;
    const float ky = ry * k;
    out.clear();
    out.insert(out.end(), {
        kPathMove,  cx + rx, cy,
        kPathCubic, cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry,
        kPathCubic, cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy,
        kPathCubic, cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry,
        kPathCubic, cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy,
        kPathClose,
    });
}

Color hex_color(std::string_view hex) {
    if (hex.size() != 7 || hex[0] != '#') return Color{255, 0, 255, 255};
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    const auto channel = [&](std::size_t off) {
        return static_cast<std::uint8_t>(nibble(hex[off]) * 16 +
                                         nibble(hex[off + 1]));
    };
    return Color{channel(1), channel(3), channel(5), 255};
}

// Lighten / darken by amt (−1..1), same math as the site's shade().
Color shade_color(Color c, float amt) {
    const auto adj = [&](std::uint8_t v) {
        return static_cast<std::uint8_t>(std::clamp(
            static_cast<int>(std::lround(v + 255.0f * amt)), 0, 255));
    };
    return Color{adj(c.r), adj(c.g), adj(c.b), c.a};
}

// Point on the rendered cable cubic at parameter t (for hit-testing).
Pt cable_point(const Pt& a, const Pt& b, const Pt& m, float t) {
    const Pt c1{a.x + (m.x - a.x) * 0.5f, a.y + (m.y - a.y) * 1.10f};
    const Pt c2{b.x + (m.x - b.x) * 0.5f, b.y + (m.y - b.y) * 1.10f};
    const float u = 1.0f - t;
    const float w0 = u * u * u;
    const float w1 = 3.0f * u * u * t;
    const float w2 = 3.0f * u * t * t;
    const float w3 = t * t * t;
    return Pt{w0 * a.x + w1 * c1.x + w2 * c2.x + w3 * b.x,
              w0 * a.y + w1 * c1.y + w2 * c2.y + w3 * b.y};
}

// Chrome plug head — black outer rim, brushed-steel centre. Solid discs
// go through fill_circle; the brushed-steel bevel is a 3-stop radial
// PathPaint over a circle path (same stops as the old SVG defs).
void draw_plug(Painter& painter, std::vector<float>& scratch, const Pt& p,
               bool dragging) {
    const float r = dragging ? 10.0f : 9.0f;
    if (dragging) {
        ellipse_cmds(scratch, p.x, p.y + 2.0f, 10.0f, 3.5f);
        painter.fill_path(scratch.data(), scratch.size(),
                          PathPaint::solid(Color{0, 0, 0, 102}));
    }
    painter.fill_circle(p.x, p.y, r, Color{0x0a, 0x0a, 0x0a, 255});

    const float mr = dragging ? 8.5f : 7.5f;
    // SVG radialGradient cx=.4 cy=.35 r=.65 of the bounding box.
    auto metal = PathPaint::radial(p.x - 0.2f * mr, p.y - 0.3f * mr,
                                   0.0f, 1.3f * mr,
                                   Color{0xf0, 0xf2, 0xf5, 255},
                                   Color{0xf0, 0xf2, 0xf5, 255});
    metal.offsets[1] = 0.45f;
    metal.colors[1] = Color{0x9a, 0x9e, 0xa6, 255};
    metal.add_stop(1.0f, Color{0x3a, 0x3d, 0x44, 255});
    ellipse_cmds(scratch, p.x, p.y, mr, mr);
    painter.fill_path(scratch.data(), scratch.size(), metal);

    painter.fill_circle(p.x, p.y, dragging ? 3.5f : 3.0f,
                        Color{0x1a, 0x1a, 0x1a, 255});
    painter.fill_circle(p.x - 2.0f, p.y - 2.5f, 2.0f,
                        Color{255, 255, 255, 140});
}

// Cable body: cylinder-shaded gradient stroke (dark edge → light centre
// → dark edge along the vertical of its bounding box) + thin highlight.
void draw_cable_stroke(Painter& painter, std::vector<float>& scratch,
                       const Pt& a, const Pt& b, const Pt& m,
                       Color color) {
    cable_cmds(scratch, a, b, m);

    // Vertical extent of the curve (endpoints + bob approximation) for
    // the gradient axis, mirroring objectBoundingBox y1=0→y2=1.
    const float y0 = std::min({a.y, b.y, m.y});
    const float y1 = std::max({a.y, b.y, m.y});
    auto paint = PathPaint::linear(a.x, y0, a.x, std::max(y1, y0 + 1.0f),
                                   shade_color(color, -0.30f),
                                   shade_color(color, -0.30f));
    paint.offsets[1] = 0.5f;
    paint.colors[1] = shade_color(color, 0.10f);
    paint.add_stop(1.0f, shade_color(color, -0.30f));
    painter.stroke_path(scratch.data(), scratch.size(), paint, 6.0f,
                        LineCap::Round, LineJoin::Round);

    painter.stroke_path(scratch.data(), scratch.size(),
                        PathPaint::solid(Color{255, 255, 255, 51}), 1.2f,
                        LineCap::Round, LineJoin::Round);
}

void draw_cable_shadow(Painter& painter, std::vector<float>& scratch,
                       const Pt& a, const Pt& b, const Pt& m) {
    const Pt as{a.x, a.y + 5.0f};
    const Pt bs{b.x, b.y + 5.0f};
    const Pt ms{m.x, m.y + 5.0f};
    cable_cmds(scratch, as, bs, ms);
    painter.stroke_path(scratch.data(), scratch.size(),
                        PathPaint::solid(Color{0, 0, 0, 115}), 7.0f,
                        LineCap::Round, LineJoin::Round);
}

int bay_counter = 0;

}  // namespace

// ── Structure builders ───────────────────────────────────────────────

View::Scope hw_panel(View& v, HwFinish finish, std::string_view classes,
                     std::string_view key) {
    std::string cls(finish_class(finish));
    if (!classes.empty()) {
        cls.push_back(' ');
        cls.append(classes);
    }
    auto scope = v.element("div", cls, key);
    v.html("<span class=\"dcs-hw__screw dcs-hw__screw--tl\"></span>"
           "<span class=\"dcs-hw__screw dcs-hw__screw--tr\"></span>"
           "<span class=\"dcs-hw__screw dcs-hw__screw--bl\"></span>"
           "<span class=\"dcs-hw__screw dcs-hw__screw--br\"></span>",
           "__screws");
    return scope;
}

View::Scope silk(View& v, std::string_view title, std::string_view key) {
    auto scope = v.element("fieldset", "dcs-silk",
                           key.empty() ? title : key);
    v.element_ref("legend", "dcs-silk__title", "__legend").text(title);
    return scope;
}

WidgetRef hw_label(View& v, std::string_view text, std::string_view key) {
    auto ref = v.element_ref("div", "dcs-hw__label",
                             key.empty() ? text : key);
    ref.text(text);
    return ref;
}

WidgetRef control_label(View& v, std::string_view text,
                        std::string_view key) {
    auto ref = v.element_ref("div", "dcs-jack__label",
                             key.empty() ? text : key);
    ref.text(text);
    return ref;
}

// ── Controls ─────────────────────────────────────────────────────────

WidgetRef lit_button(View& v, std::string_view name,
                     std::string_view glyph, LitColor color, bool lit,
                     bool small, std::string_view key) {
    std::string cls = "dcs-litbtn";
    if (lit) cls += " dcs-litbtn--lit";
    cls += lit_color_class(color);
    if (small) cls += " dcs-litbtn--sm";

    auto ref = v.element_ref("button", cls, key.empty() ? name : key);
    ref.named(name);
    ref.attr("data-aui-name", name);
    ref.attr("type", "button");
    ref.text(glyph);
    return ref;
}

WidgetRef lcd(View& v, std::string_view value, LcdSize size,
              LcdColor color, int digits, std::string_view key) {
    std::string cls = "dcs-lcd";
    cls += lcd_size_class(size);
    cls += lcd_color_class(color);
    auto scope = v.element("div", cls, key);
    scope.attr("data-value", value);
    // Positional keys ("g0", "g1", ...) keep each glyph slot's element
    // subtree retained across value changes: a ticking display reduces
    // to attribute diffs on the segments that actually flipped.
    const std::string padded = lcd_padded(value, digits);
    int slot = 0;
    for (char ch : padded) {
        const std::string k = "g" + std::to_string(slot++);
        if (ch == '.') {
            v.element_ref("span", "dcs-lcd__dot dcs-lcd__dot--on", k);
        } else {
            lcd_digit(v, ch, k);
        }
    }
    return scope.ref();
}

WidgetRef led_meter(View& v, float value01, int segments, bool vertical,
                    std::string_view key) {
    segments = std::max(1, segments);
    auto scope = v.element(
        "div", vertical ? "dcs-meter dcs-meter--v" : "dcs-meter", key);
    scope.attr("style", vertical ? "width:14px;height:100px"
                                 : "height:14px");
    scope.attr("data-value", num(value01));
    const int lit = static_cast<int>(
        std::lround(std::clamp(value01, 0.0f, 1.0f) * segments));
    for (int i = 0; i < segments; ++i) {
        const float t = segments > 1
                            ? static_cast<float>(i) /
                                  static_cast<float>(segments - 1)
                            : 0.0f;
        const char* zone = t > 0.85f ? "peak" : (t > 0.65f ? "warn" : "ok");
        std::string cls = "dcs-meter__seg dcs-meter__seg--";
        if (i < lit) cls += "lit-";
        cls += zone;
        const std::string k = "s" + std::to_string(i);
        v.element_ref("div", {}, k).attr("class", cls);
    }
    return scope.ref();
}

WidgetRef fader(View& v, std::string_view name, double value, double min,
                double max, int height_px, std::string_view key) {
    if (max <= min) max = min + 1.0;
    const double clamped = std::clamp(value, min, max);
    const double p = 1.0 - (clamped - min) / (max - min);

    auto scope = v.element("div", "dcs-fader", key.empty() ? name : key);
    scope.named(name);
    scope.attr("data-aui-name", name);
    scope.attr("data-dcs-fader", "");
    scope.attr("role", "slider");
    scope.attr("aria-valuemin", num(min));
    scope.attr("aria-valuemax", num(max));
    scope.attr("aria-valuenow", num(clamped));
    scope.attr("data-min", num(min));
    scope.attr("data-max", num(max));
    scope.attr("data-value", num(clamped));
    std::string style = "--pos:" + pct(p);
    if (height_px > 0 && height_px != 140) {
        style += ";height:" + std::to_string(height_px) + "px";
    }
    scope.attr("style", style);

    v.element_ref("div", "dcs-fader__track", "__track");
    v.element_ref("div", "dcs-fader__tick", "__t25")
        .attr("style", "top:25%");
    v.element_ref("div", "dcs-fader__tick", "__t50")
        .attr("style", "top:50%");
    v.element_ref("div", "dcs-fader__tick", "__t75")
        .attr("style", "top:75%");
    v.element_ref("div", "dcs-fader__thumb", "__thumb");
    return scope.ref();
}

std::pair<WidgetRef, WidgetRef> step_pair(View& v,
                                          std::string_view up_name,
                                          std::string_view down_name,
                                          std::string_view key) {
    // Private key: a public key would auto-NAME the wrapper `up_name`,
    // shadowing the actual up button for name lookups (same collision
    // class as the jack socket/column split).
    const std::string wrapper_key =
        "__step:" + std::string(key.empty() ? up_name : key);
    auto scope = v.element("div", "dcs-step", wrapper_key);
    (void)scope;
    WidgetRef up;
    WidgetRef down;
    {
        auto b = v.element("button", "dcs-step__btn", "__up");
        b.attr("data-aui-name", up_name).attr("type", "button");
        b.attr("style", "width:22px;height:15px");
        b.named(up_name);
        v.html("<svg viewBox=\"0 0 16 16\" style=\"width:9px;height:9px\">"
               "<path d=\"M 3 10 L 8 5 L 13 10\""
               " fill=\"none\" stroke=\"#2a2d33\" stroke-width=\"2.5\""
               " stroke-linecap=\"round\"/></svg>",
               "__i");
        up = b.ref();
    }
    {
        auto b = v.element("button", "dcs-step__btn", "__down");
        b.attr("data-aui-name", down_name).attr("type", "button");
        b.attr("style", "width:22px;height:15px");
        b.named(down_name);
        v.html("<svg viewBox=\"0 0 16 16\" style=\"width:9px;height:9px\">"
               "<path d=\"M 3 6 L 8 11 L 13 6\""
               " fill=\"none\" stroke=\"#2a2d33\" stroke-width=\"2.5\""
               " stroke-linecap=\"round\"/></svg>",
               "__i");
        down = b.ref();
    }
    return {up, down};
}

WidgetRef keyboard(View& v, std::string_view name, int octaves,
                   std::function<void(int midi)> on_note,
                   int first_octave, std::string_view key) {
    octaves = std::max(1, octaves);
    // Semitone offset within the octave for each of the 7 white keys
    // and the 5 black keys, plus each black key's piano-true position
    // as a percentage of the octave width (centered on the white-key
    // boundaries 1,2,4,5,6 of 7, minus half the 9.6% key width).
    constexpr int white_semis[7] = {0, 2, 4, 5, 7, 9, 11};
    constexpr int black_semis[5] = {1, 3, 6, 8, 10};
    constexpr double black_left_pct[5] = {9.5, 23.8, 52.4, 66.7, 81.0};

    auto strip = v.element("div", "skeuo-kbd",
                           key.empty() ? std::string("__kbd:") +
                                             std::string(name)
                                       : std::string(key));
    strip.attr("data-aui-name", name);
    for (int oct = 0; oct < octaves; ++oct) {
        const int base = (first_octave + oct + 1) * 12;  // C of this octave
        auto oct_scope =
            v.element("div", "skeuo-kbd__oct", "o" + std::to_string(oct));
        for (int w = 0; w < 7; ++w) {
            const int midi = base + white_semis[w];
            auto ref = v.element_ref("button", "skeuo-kbd__white",
                                     "w" + std::to_string(w));
            ref.attr("type", "button");
            if (on_note) {
                ref.on_click([on_note, midi] { on_note(midi); });
            }
        }
        for (int b = 0; b < 5; ++b) {
            const int midi = base + black_semis[b];
            auto ref = v.element_ref("button", "skeuo-kbd__black",
                                     "b" + std::to_string(b));
            ref.attr("type", "button");
            char style[48];
            std::snprintf(style, sizeof(style), "left:%.1f%%",
                          black_left_pct[b]);
            ref.attr("style", style);
            if (on_note) {
                ref.on_click([on_note, midi] { on_note(midi); });
            }
        }
    }
    return strip.ref();
}

// ── Typed components ─────────────────────────────────────────────────

bool LitButton::matches(const WidgetNode& n) {
    return n.kind == WidgetKind::Container ||
           n.kind == WidgetKind::Button;
}

bool LitButton::lit() const {
    return valid() &&
           std::string(ref_.attr_value("class")).find("dcs-litbtn--lit") !=
               std::string::npos;
}

void LitButton::set_lit(bool lit) {
    if (!valid()) return;
    std::string cls(ref_.attr_value("class"));
    const auto pos = cls.find(" dcs-litbtn--lit");
    if (lit && pos == std::string::npos) {
        const auto base = cls.find("dcs-litbtn");
        if (base != std::string::npos) {
            cls.insert(base + std::strlen("dcs-litbtn"),
                       " dcs-litbtn--lit");
        } else {
            cls += " dcs-litbtn--lit";
        }
    } else if (!lit && pos != std::string::npos) {
        cls.erase(pos, std::strlen(" dcs-litbtn--lit"));
    }
    ref_.attr("class", cls);
}

bool Lcd::matches(const WidgetNode& n) {
    return n.kind == WidgetKind::Container;
}

std::string Lcd::value() const {
    return valid() ? std::string(ref_.attr_value("data-value"))
                   : std::string{};
}

bool LedMeter::matches(const WidgetNode& n) {
    return n.kind == WidgetKind::Container;
}

float LedMeter::value() const {
    if (!valid()) return 0.0f;
    return static_cast<float>(
        std::strtod(std::string(ref_.attr_value("data-value")).c_str(),
                    nullptr));
}

// ── PatchBay ─────────────────────────────────────────────────────────

PatchBay::PatchBay()
    : palette({"#6fb3ff", "#6fdfa8", "#f5c878", "#f29090", "#c8a8ff",
               "#80e0e0"}) {
    board_name_ = "skeuo-board-" + std::to_string(++bay_counter);
    canvas_name_ = board_name_ + "-cables";
}

View::Scope PatchBay::board(View& v, std::string_view classes,
                            std::string_view key) {
    jacks_.clear();
    auto scope = v.element("div", classes,
                           key.empty() ? "__skeuo-board" : key);
    scope.attr("data-aui-name", board_name_);
    scope.attr("style", "position:relative");
    return scope;
}

WidgetRef PatchBay::jack(View& v, std::string_view jack_id,
                         std::string_view label, std::string_view key) {
    jacks_.emplace_back(jack_id);
    const bool is_patched =
        patched(jack_id) || (drag_.active && drag_.anchor == jack_id);

    auto outer = v.element(
        "div", is_patched ? "dcs-jack dcs-jack--patched" : "dcs-jack",
        key.empty() ? jack_id : key);
    {
        auto socket = v.element("div", "dcs-jack__socket", "__socket");
        socket.attr("data-skeuo-jack", jack_id);
        socket.attr("style", "cursor:crosshair");
        v.html(jack_svg(is_patched), "__art");
    }
    if (!label.empty()) {
        control_label(v, label, "__label");
    }
    return outer.ref();
}

void PatchBay::cables_layer(View& v, std::string_view key) {
    // The DOM keeps only a positioned wrapper block for stacking order;
    // cable geometry is app-painted by the handler attach() registers
    // (custom paint / canvas). Per-frame drag repaints flow through
    // App::request_custom_repaint — no DOM mutations, no reconcile.
    v.canvas(canvas_name_, "skeuo-cables",
             key.empty() ? "__skeuo-cables" : key)
        .attr("style",
              "position:absolute;left:0;top:0;width:100%;height:100%;"
              "z-index:100;overflow:visible;pointer-events:none");
    // A rebuild may relayout anything — drop cached jack geometry.
    jack_pos_cache_.clear();
}

void PatchBay::paint_cables(Painter& painter, const Rect& bounds) {
    // The overlay fills the board; if its rect moved (resize, dock
    // shuffle) every cached jack position is stale.
    if (bounds.x != cached_board_rect_.x ||
        bounds.y != cached_board_rect_.y ||
        bounds.w != cached_board_rect_.w ||
        bounds.h != cached_board_rect_.h) {
        cached_board_rect_ = bounds;
        jack_pos_cache_.clear();
    }
    // Jack centers come from live block geometry, already in document
    // space — the painter draws in document space, so no origin math.
    struct Resolved {
        Pt a, b;
        Color color;
    };
    std::vector<Resolved> resolved;
    resolved.reserve(cables_.size());
    for (const auto& cable : cables_) {
        float ax = 0, ay = 0, bx = 0, by = 0;
        if (!jack_center(cable.from, ax, ay) ||
            !jack_center(cable.to, bx, by)) {
            continue;
        }
        const auto& hex =
            palette[static_cast<std::size_t>(cable.color) % palette.size()];
        resolved.push_back({Pt{ax, ay}, Pt{bx, by}, hex_color(hex)});
    }

    // Shadows first, then plug heads, then cable bodies on top (the
    // cable visually enters the plug), matching the reference stack.
    for (const auto& c : resolved) {
        draw_cable_shadow(painter, path_scratch_, c.a, c.b,
                          cable_rest_bob(c.a, c.b));
    }
    for (const auto& c : resolved) {
        draw_plug(painter, path_scratch_, c.a, false);
        draw_plug(painter, path_scratch_, c.b, false);
    }
    for (const auto& c : resolved) {
        draw_cable_stroke(painter, path_scratch_, c.a, c.b,
                          cable_rest_bob(c.a, c.b), c.color);
    }

    // Drag preview: anchored plug + free plug under the cursor, cable
    // swinging through the spring-damper bob.
    if (drag_.active) {
        float fx = 0, fy = 0;
        if (jack_center(drag_.anchor, fx, fy)) {
            const Pt a{fx, fy};
            const Pt t{drag_.target_x, drag_.target_y};
            const Pt m{drag_.bob_x, drag_.bob_y};
            const auto& hex =
                palette[static_cast<std::size_t>(drag_.color) %
                        palette.size()];
            draw_cable_shadow(painter, path_scratch_, a, t, m);
            draw_plug(painter, path_scratch_, a, false);
            draw_plug(painter, path_scratch_, t, true);
            draw_cable_stroke(painter, path_scratch_, a, t, m,
                              hex_color(hex));
        }
    }
}

void PatchBay::connect(std::string_view from, std::string_view to,
                       int color) {
    if (from == to) return;
    if (color < 0) color = next_color();
    cables_.erase(
        std::remove_if(cables_.begin(), cables_.end(),
                       [&](const Cable& c) {
                           return c.from == from || c.to == from ||
                                  c.from == to || c.to == to;
                       }),
        cables_.end());
    cables_.push_back(Cable{std::string(from), std::string(to), color});
}

void PatchBay::disconnect(std::string_view jack_id) {
    cables_.erase(
        std::remove_if(cables_.begin(), cables_.end(),
                       [&](const Cable& c) {
                           return c.from == jack_id || c.to == jack_id;
                       }),
        cables_.end());
}

void PatchBay::clear() {
    cables_.clear();
}

bool PatchBay::patched(std::string_view jack_id) const {
    for (const auto& c : cables_) {
        if (c.from == jack_id || c.to == jack_id) return true;
    }
    return false;
}

void PatchBay::attach(App& app) {
    app_ = &app;
    app.set_custom_paint(canvas_name_,
                         [this](Painter& painter, const Rect& bounds) {
                             paint_cables(painter, bounds);
                         });
    app.on_event([this, &app](const Event& ev,
                              const std::vector<Document::HoverInfo>&
                                  chain) {
        return handle_event(app, ev, chain);
    });
    app.on_frame([this](double dt) { tick(dt); });
}

int PatchBay::next_color() const {
    return palette.empty()
               ? 0
               : static_cast<int>(cables_.size() % palette.size());
}

bool PatchBay::jack_center(std::string_view jack_id, float& x,
                           float& y) const {
    if (!app_) return false;
    if (auto it = jack_pos_cache_.find(std::string(jack_id));
        it != jack_pos_cache_.end()) {
        x = it->second.x;
        y = it->second.y;
        return true;
    }
    // Anchor to the SOCKET artwork, not the jack's named outer column —
    // the outer box includes the label, which would hang every plug half
    // a label below its port. The socket carries data-skeuo-jack.
    const Rect r = app_->document().find_element_rect(
        "[data-skeuo-jack=" + std::string(jack_id) + "]");
    if (r.w <= 0 || r.h <= 0) return false;
    x = static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f;
    y = static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f;
    jack_pos_cache_.emplace(std::string(jack_id), JackPos{x, y});
    return true;
}

std::optional<std::string> PatchBay::jack_at(float doc_x,
                                             float doc_y) const {
    if (!app_) return std::nullopt;
    for (const auto& id : jacks_) {
        const Rect r = app_->document().find_element_rect(id);
        if (r.w <= 0 || r.h <= 0) continue;
        if (doc_x >= static_cast<float>(r.x) &&
            doc_x <= static_cast<float>(r.x + r.w) &&
            doc_y >= static_cast<float>(r.y) &&
            doc_y <= static_cast<float>(r.y + r.h)) {
            return id;
        }
    }
    return std::nullopt;
}

int PatchBay::cable_at(float doc_x, float doc_y) const {
    constexpr float kHitDist = 7.0f;
    for (std::size_t i = 0; i < cables_.size(); ++i) {
        float ax = 0, ay = 0, bx = 0, by = 0;
        if (!jack_center(cables_[i].from, ax, ay) ||
            !jack_center(cables_[i].to, bx, by)) {
            continue;
        }
        const Pt a{ax, ay};
        const Pt b{bx, by};
        const Pt m = cable_rest_bob(a, b);
        for (int s = 0; s <= 24; ++s) {
            const Pt p =
                cable_point(a, b, m, static_cast<float>(s) / 24.0f);
            if (std::hypot(p.x - doc_x, p.y - doc_y) <= kHitDist) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

bool PatchBay::handle_event(App& app, const Event& ev,
                            const std::vector<Document::HoverInfo>&
                                chain) {
    const float mx = static_cast<float>(ev.pos.x);
    const float my = static_cast<float>(ev.pos.y);

    if (ev.type == EventType::MouseMove && drag_.active) {
        drag_.target_x = mx;
        drag_.target_y = my;
        return true;
    }

    if (ev.type == EventType::MouseUp && drag_.active) {
        const auto hit = jack_at(mx, my);
        if (hit && *hit != drag_.anchor) {
            connect(drag_.anchor, *hit, drag_.color);
            if (on_change) on_change();
        }
        drag_.active = false;
        app.release_pointer();
        if (request_render) request_render();
        return true;
    }

    if (ev.type == EventType::MouseDown &&
        ev.button == MouseButton::Left && !drag_.active) {
        // Grab a jack — either starting a fresh cable or picking up the
        // plug of an existing one (its far end stays anchored).
        std::string jack_id;
        for (const auto& info : chain) {
            for (const auto& [name, value] : info.attrs) {
                if (name == "data-skeuo-jack") {
                    jack_id = value;
                    break;
                }
            }
            if (!jack_id.empty()) break;
        }
        if (!jack_id.empty()) {
            std::string anchor = jack_id;
            int color = next_color();
            const auto existing = std::find_if(
                cables_.begin(), cables_.end(), [&](const Cable& c) {
                    return c.from == jack_id || c.to == jack_id;
                });
            if (existing != cables_.end()) {
                anchor = existing->from == jack_id ? existing->to
                                                   : existing->from;
                color = existing->color;
                cables_.erase(existing);
                if (on_change) on_change();
            }
            float fx = 0, fy = 0;
            if (!jack_center(anchor, fx, fy)) return false;

            drag_.anchor = anchor;
            drag_.color = color;
            drag_.target_x = mx;
            drag_.target_y = my;
            // Initial bob = midpoint dropped by the current sag.
            const Pt rest = cable_rest_bob(Pt{fx, fy}, Pt{mx, my});
            drag_.bob_x = rest.x;
            drag_.bob_y = rest.y;
            drag_.vel_x = 0.0f;
            drag_.vel_y = 0.0f;
            drag_.active = true;
            app.capture_pointer();
            if (request_render) request_render();
            return true;
        }

        // Click on a cable removes it.
        const int ci = cable_at(mx, my);
        if (ci >= 0) {
            cables_.erase(cables_.begin() + ci);
            if (on_change) on_change();
            if (request_render) request_render();
            return true;
        }
    }

    return false;
}

void PatchBay::tick(double dt) {
    if (!drag_.active) return;

    // Spring-damper on the bob toward its rest position (underdamped
    // pendulum) — constants from the reference implementation. The
    // cable overlay is a custom-paint surface, so the per-frame update
    // is a repaint of that surface only: no DOM writes, no reconcile.
    const float step = static_cast<float>(std::min(dt, 0.05));
    if (step <= 0.0f) return;
    float fx = 0, fy = 0;
    if (!jack_center(drag_.anchor, fx, fy)) return;
    const Pt rest =
        cable_rest_bob(Pt{fx, fy}, Pt{drag_.target_x, drag_.target_y});
    constexpr float kSpring = 30.0f;
    constexpr float kDamp = 1.4f;
    const float ax = (rest.x - drag_.bob_x) * kSpring - drag_.vel_x * kDamp;
    const float ay = (rest.y - drag_.bob_y) * kSpring - drag_.vel_y * kDamp;
    drag_.vel_x += ax * step;
    drag_.vel_y += ay * step;
    drag_.bob_x += drag_.vel_x * step;
    drag_.bob_y += drag_.vel_y * step;

    if (app_) app_->request_custom_repaint(canvas_name_);
}

}  // namespace affineui::skeuo
