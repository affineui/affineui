// Phase-2A cascade: lexbor-backed StyleResolver.
//
// "Start small, but very very very smart."
//
// What this file does:
//   - Implements StyleResolver from internal/style_resolver.h.
//   - Walks lexbor's matched, cascade-ordered declarations per element
//     via lxb_html_element_style_walk, then caches the resolved static
//     cascade result for stable dynamic restyles.
//   - Routes each declaration into either ComputedStyle (layout-
//     affecting) or AnimatedStyle (paint/composite-only), preserving
//     the split that docs/DESIGN.md § "Real-time render architecture"
//     depends on.
//
// What this file deliberately does NOT do (yet):
//   - Handle % lengths. Phase 3.
//   - Touch font_family or font_id. Font registry lands alongside.
//
// Adding a property = one switch arm and one small parser helper.

#include "internal/style_resolver.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if !defined(AFFINEUI_STUB_BUILD)
#    include <lexbor/css/property.h>
#    include <lexbor/css/value.h>
#    include <lexbor/css/declaration.h>
#    include <lexbor/css/parser.h>
#    include <lexbor/css/unit/const.h>
#    include <lexbor/html/html.h>
#endif

namespace affineui::detail {

#if defined(AFFINEUI_STUB_BUILD)

namespace {
class NullResolver final : public StyleResolver {
public:
    ResolvedStyle resolve(lxb_dom_element_t*, const ResolvedStyle& parent) override {
        return parent;  // pure inherit, no overrides — fine for stub mode
    }
    void apply_decl_list(const lxb_css_rule_declaration_list_t*,
                         ResolvedStyle&) override {}
    void invalidate(lxb_dom_element_t*) override {}
    void clear() override {}
};
}  // namespace

std::unique_ptr<StyleResolver> make_lexbor_resolver(lxb_html_document_t*,
                                                    int,
                                                    int) {
    return std::make_unique<NullResolver>();
}

#else  // !AFFINEUI_STUB_BUILD

namespace {

void mark_viewport_dependency(int unit, ViewportDependency* dependency) {
    if (dependency == nullptr) return;
    switch (unit) {
        case LXB_CSS_UNIT_VW:
        case LXB_CSS_UNIT_VI:
            dependency->width = true;
            break;
        case LXB_CSS_UNIT_VH:
        case LXB_CSS_UNIT_VB:
            dependency->height = true;
            break;
        case LXB_CSS_UNIT_VMIN:
        case LXB_CSS_UNIT_VMAX:
            dependency->width = true;
            dependency->height = true;
            break;
        default:
            break;
    }
}

bool ascii_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (static_cast<char>(std::tolower(
                static_cast<unsigned char>(a[i]))) != b[i]) {
            return false;
        }
    }
    return true;
}

void mark_viewport_dependency(std::string_view unit,
                              ViewportDependency* dependency) {
    if (dependency == nullptr || unit.empty()) return;
    if (ascii_iequals(unit, "vw") || ascii_iequals(unit, "vi")) {
        dependency->width = true;
    } else if (ascii_iequals(unit, "vh") || ascii_iequals(unit, "vb")) {
        dependency->height = true;
    } else if (ascii_iequals(unit, "vmin") || ascii_iequals(unit, "vmax")) {
        dependency->width = true;
        dependency->height = true;
    }
}

bool ascii_unit_at(std::string_view s, std::size_t i, std::string_view unit) {
    if (i + unit.size() > s.size()) return false;
    for (std::size_t k = 0; k < unit.size(); ++k) {
        if (static_cast<char>(std::tolower(
                static_cast<unsigned char>(s[i + k]))) != unit[k]) {
            return false;
        }
    }
    const bool before_ident =
        i > 0 &&
        (std::isalpha(static_cast<unsigned char>(s[i - 1])) ||
         s[i - 1] == '-' || s[i - 1] == '_');
    const std::size_t end = i + unit.size();
    const bool after_ident =
        end < s.size() &&
        (std::isalnum(static_cast<unsigned char>(s[end])) ||
         s[end] == '-' || s[end] == '_');
    return !before_ident && !after_ident;
}

void mark_viewport_dependencies_in_value(std::string_view value,
                                         ViewportDependency* dependency) {
    if (dependency == nullptr) return;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (ascii_unit_at(value, i, "vw") || ascii_unit_at(value, i, "vi")) {
            dependency->width = true;
        } else if (ascii_unit_at(value, i, "vh") ||
                   ascii_unit_at(value, i, "vb")) {
            dependency->height = true;
        } else if (ascii_unit_at(value, i, "vmin") ||
                   ascii_unit_at(value, i, "vmax")) {
            dependency->width = true;
            dependency->height = true;
        }
    }
}

// Pack an 8-bit RGBA quad in our canonical layout.
inline std::uint32_t make_rgba(std::uint8_t r, std::uint8_t g,
                               std::uint8_t b, std::uint8_t a) {
    return (std::uint32_t(r) << 24)
         | (std::uint32_t(g) << 16)
         | (std::uint32_t(b) <<  8)
         |  std::uint32_t(a);
}

std::uint8_t clamp_u8(double v) {
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<std::uint8_t>(std::lround(v));
}

// CSS hsl() → RGB conversion per the CSS Color Level 4 spec.
// h is in degrees [0, 360), s and l are fractions in [0, 1].
// Returns packed RGBA8 with full alpha (caller supplies alpha).
std::uint32_t fnv1a_32(std::string_view value) {
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : value) {
        hash ^= static_cast<std::uint32_t>(c);
        hash *= 16777619u;
    }
    return hash == 0 ? 1u : hash;
}

void hsl_to_rgb(double h, double s, double l,
                std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    // Normalise hue to [0, 360).
    h = std::fmod(h, 360.0);
    if (h < 0.0) h += 360.0;

    const double a_chroma = s * std::min(l, 1.0 - l);
    auto f = [&](double n) -> double {
        const double k = std::fmod(n + h / 30.0, 12.0);
        return l - a_chroma * std::max(-1.0, std::min({k - 3.0, 9.0 - k, 1.0}));
    };
    r = clamp_u8(f(0.0) * 255.0);
    g = clamp_u8(f(8.0) * 255.0);
    b = clamp_u8(f(4.0) * 255.0);
}

// Extract hue as degrees from lexbor's hue union.
double hue_to_degrees(const lxb_css_value_hue_t& hue) {
    constexpr double kPi = 3.14159265358979323846;
    if (hue.type == LXB_CSS_VALUE__NUMBER) {
        return hue.u.number.num;  // bare number = degrees in CSS Color 4
    }
    if (hue.type == LXB_CSS_VALUE__ANGLE) {
        const double num = hue.u.angle.num;
        switch (static_cast<int>(hue.u.angle.unit)) {
            case LXB_CSS_UNIT_DEG:  return num;
            case LXB_CSS_UNIT_GRAD: return num * 360.0 / 400.0;
            case LXB_CSS_UNIT_RAD:  return num * 180.0 / kPi;
            case LXB_CSS_UNIT_TURN: return num * 360.0;
            default:                return num;
        }
    }
    return 0.0;
}

float angle_to_radians(const lxb_css_value_angle_t& angle) {
    constexpr double kPi = 3.14159265358979323846;
    double degrees = angle.num;
    switch (static_cast<int>(angle.unit)) {
        case LXB_CSS_UNIT_DEG:  break;
        case LXB_CSS_UNIT_GRAD: degrees = angle.num * 360.0 / 400.0; break;
        case LXB_CSS_UNIT_RAD:  return static_cast<float>(angle.num);
        case LXB_CSS_UNIT_TURN: degrees = angle.num * 360.0; break;
        default:                break;
    }
    return static_cast<float>(degrees * kPi / 180.0);
}

std::uint8_t color_component(const lxb_css_value_number_percentage_t& c) {
    if (c.type == LXB_CSS_VALUE__PERCENTAGE)
        return clamp_u8(c.u.percentage.num * 255.0 / 100.0);
    if (c.type == LXB_CSS_VALUE__NUMBER)
        return clamp_u8(c.u.number.num);
    return 0;
}

std::uint8_t alpha_component(const lxb_css_value_number_percentage_t& a) {
    if (a.type == LXB_CSS_VALUE__PERCENTAGE)
        return clamp_u8(a.u.percentage.num * 255.0 / 100.0);
    if (a.type == LXB_CSS_VALUE__NUMBER)
        return clamp_u8(a.u.number.num * 255.0);
    return 0xFF;
}
// CSS named colors (aliceblue ... rebeccapurple): resolve a
// lxb_css_color_type_t keyword to packed RGBA (full alpha).
static bool parse_named_color(lxb_css_color_type_t type, std::uint32_t& out) {
    using T = lxb_css_color_type_t;
    struct Entry { T type; std::uint32_t rgb; };
    static constexpr Entry kTable[] = {
        {T(LXB_CSS_COLOR_ALICEBLUE),             0xF0F8FFUL},
        {T(LXB_CSS_COLOR_ANTIQUEWHITE),          0xFAEBD7UL},
        {T(LXB_CSS_COLOR_AQUA),                  0x00FFFFUL},
        {T(LXB_CSS_COLOR_AQUAMARINE),            0x7FFFD4UL},
        {T(LXB_CSS_COLOR_AZURE),                 0xF0FFFFUL},
        {T(LXB_CSS_COLOR_BEIGE),                 0xF5F5DCUL},
        {T(LXB_CSS_COLOR_BISQUE),                0xFFE4C4UL},
        {T(LXB_CSS_COLOR_BLACK),                 0x000000UL},
        {T(LXB_CSS_COLOR_BLANCHEDALMOND),        0xFFEBCDUL},
        {T(LXB_CSS_COLOR_BLUE),                  0x0000FFUL},
        {T(LXB_CSS_COLOR_BLUEVIOLET),            0x8A2BE2UL},
        {T(LXB_CSS_COLOR_BROWN),                 0xA52A2AUL},
        {T(LXB_CSS_COLOR_BURLYWOOD),             0xDEB887UL},
        {T(LXB_CSS_COLOR_CADETBLUE),             0x5F9EA0UL},
        {T(LXB_CSS_COLOR_CHARTREUSE),            0x7FFF00UL},
        {T(LXB_CSS_COLOR_CHOCOLATE),             0xD2691EUL},
        {T(LXB_CSS_COLOR_CORAL),                 0xFF7F50UL},
        {T(LXB_CSS_COLOR_CORNFLOWERBLUE),        0x6495EDUL},
        {T(LXB_CSS_COLOR_CORNSILK),              0xFFF8DCUL},
        {T(LXB_CSS_COLOR_CRIMSON),               0xDC143CUL},
        {T(LXB_CSS_COLOR_CYAN),                  0x00FFFFUL},
        {T(LXB_CSS_COLOR_DARKBLUE),              0x00008BUL},
        {T(LXB_CSS_COLOR_DARKCYAN),              0x008B8BUL},
        {T(LXB_CSS_COLOR_DARKGOLDENROD),         0xB8860BUL},
        {T(LXB_CSS_COLOR_DARKGRAY),              0xA9A9A9UL},
        {T(LXB_CSS_COLOR_DARKGREEN),             0x006400UL},
        {T(LXB_CSS_COLOR_DARKGREY),              0xA9A9A9UL},
        {T(LXB_CSS_COLOR_DARKKHAKI),             0xBDB76BUL},
        {T(LXB_CSS_COLOR_DARKMAGENTA),           0x8B008BUL},
        {T(LXB_CSS_COLOR_DARKOLIVEGREEN),        0x556B2FUL},
        {T(LXB_CSS_COLOR_DARKORANGE),            0xFF8C00UL},
        {T(LXB_CSS_COLOR_DARKORCHID),            0x9932CCUL},
        {T(LXB_CSS_COLOR_DARKRED),               0x8B0000UL},
        {T(LXB_CSS_COLOR_DARKSALMON),            0xE9967AUL},
        {T(LXB_CSS_COLOR_DARKSEAGREEN),          0x8FBC8FUL},
        {T(LXB_CSS_COLOR_DARKSLATEBLUE),         0x483D8BUL},
        {T(LXB_CSS_COLOR_DARKSLATEGRAY),         0x2F4F4FUL},
        {T(LXB_CSS_COLOR_DARKSLATEGREY),         0x2F4F4FUL},
        {T(LXB_CSS_COLOR_DARKTURQUOISE),         0x00CED1UL},
        {T(LXB_CSS_COLOR_DARKVIOLET),            0x9400D3UL},
        {T(LXB_CSS_COLOR_DEEPPINK),              0xFF1493UL},
        {T(LXB_CSS_COLOR_DEEPSKYBLUE),           0x00BFFFUL},
        {T(LXB_CSS_COLOR_DIMGRAY),               0x696969UL},
        {T(LXB_CSS_COLOR_DIMGREY),               0x696969UL},
        {T(LXB_CSS_COLOR_DODGERBLUE),            0x1E90FFUL},
        {T(LXB_CSS_COLOR_FIREBRICK),             0xB22222UL},
        {T(LXB_CSS_COLOR_FLORALWHITE),           0xFFFAF0UL},
        {T(LXB_CSS_COLOR_FORESTGREEN),           0x228B22UL},
        {T(LXB_CSS_COLOR_FUCHSIA),               0xFF00FFUL},
        {T(LXB_CSS_COLOR_GAINSBORO),             0xDCDCDCUL},
        {T(LXB_CSS_COLOR_GHOSTWHITE),            0xF8F8FFUL},
        {T(LXB_CSS_COLOR_GOLD),                  0xFFD700UL},
        {T(LXB_CSS_COLOR_GOLDENROD),             0xDAA520UL},
        {T(LXB_CSS_COLOR_GRAY),                  0x808080UL},
        {T(LXB_CSS_COLOR_GREEN),                 0x008000UL},
        {T(LXB_CSS_COLOR_GREENYELLOW),           0xADFF2FUL},
        {T(LXB_CSS_COLOR_GREY),                  0x808080UL},
        {T(LXB_CSS_COLOR_HONEYDEW),              0xF0FFF0UL},
        {T(LXB_CSS_COLOR_HOTPINK),               0xFF69B4UL},
        {T(LXB_CSS_COLOR_INDIANRED),             0xCD5C5CUL},
        {T(LXB_CSS_COLOR_INDIGO),                0x4B0082UL},
        {T(LXB_CSS_COLOR_IVORY),                 0xFFFFF0UL},
        {T(LXB_CSS_COLOR_KHAKI),                 0xF0E68CUL},
        {T(LXB_CSS_COLOR_LAVENDER),              0xE6E6FAUL},
        {T(LXB_CSS_COLOR_LAVENDERBLUSH),         0xFFF0F5UL},
        {T(LXB_CSS_COLOR_LAWNGREEN),             0x7CFC00UL},
        {T(LXB_CSS_COLOR_LEMONCHIFFON),          0xFFFACDUL},
        {T(LXB_CSS_COLOR_LIGHTBLUE),             0xADD8E6UL},
        {T(LXB_CSS_COLOR_LIGHTCORAL),            0xF08080UL},
        {T(LXB_CSS_COLOR_LIGHTCYAN),             0xE0FFFFUL},
        {T(LXB_CSS_COLOR_LIGHTGOLDENRODYELLOW),  0xFAFAD2UL},
        {T(LXB_CSS_COLOR_LIGHTGRAY),             0xD3D3D3UL},
        {T(LXB_CSS_COLOR_LIGHTGREEN),            0x90EE90UL},
        {T(LXB_CSS_COLOR_LIGHTGREY),             0xD3D3D3UL},
        {T(LXB_CSS_COLOR_LIGHTPINK),             0xFFB6C1UL},
        {T(LXB_CSS_COLOR_LIGHTSALMON),           0xFFA07AUL},
        {T(LXB_CSS_COLOR_LIGHTSEAGREEN),         0x20B2AAUL},
        {T(LXB_CSS_COLOR_LIGHTSKYBLUE),          0x87CEFAUL},
        {T(LXB_CSS_COLOR_LIGHTSLATEGRAY),        0x778899UL},
        {T(LXB_CSS_COLOR_LIGHTSLATEGREY),        0x778899UL},
        {T(LXB_CSS_COLOR_LIGHTSTEELBLUE),        0xB0C4DEUL},
        {T(LXB_CSS_COLOR_LIGHTYELLOW),           0xFFFFE0UL},
        {T(LXB_CSS_COLOR_LIME),                  0x00FF00UL},
        {T(LXB_CSS_COLOR_LIMEGREEN),             0x32CD32UL},
        {T(LXB_CSS_COLOR_LINEN),                 0xFAF0E6UL},
        {T(LXB_CSS_COLOR_MAGENTA),               0xFF00FFUL},
        {T(LXB_CSS_COLOR_MAROON),                0x800000UL},
        {T(LXB_CSS_COLOR_MEDIUMAQUAMARINE),      0x66CDAAUL},
        {T(LXB_CSS_COLOR_MEDIUMBLUE),            0x0000CDUL},
        {T(LXB_CSS_COLOR_MEDIUMORCHID),          0xBA55D3UL},
        {T(LXB_CSS_COLOR_MEDIUMPURPLE),          0x9370DBUL},
        {T(LXB_CSS_COLOR_MEDIUMSEAGREEN),        0x3CB371UL},
        {T(LXB_CSS_COLOR_MEDIUMSLATEBLUE),       0x7B68EEUL},
        {T(LXB_CSS_COLOR_MEDIUMSPRINGGREEN),     0x00FA9AUL},
        {T(LXB_CSS_COLOR_MEDIUMTURQUOISE),       0x48D1CCUL},
        {T(LXB_CSS_COLOR_MEDIUMVIOLETRED),       0xC71585UL},
        {T(LXB_CSS_COLOR_MIDNIGHTBLUE),          0x191970UL},
        {T(LXB_CSS_COLOR_MINTCREAM),             0xF5FFFAUL},
        {T(LXB_CSS_COLOR_MISTYROSE),             0xFFE4E1UL},
        {T(LXB_CSS_COLOR_MOCCASIN),              0xFFE4B5UL},
        {T(LXB_CSS_COLOR_NAVAJOWHITE),           0xFFDEADUL},
        {T(LXB_CSS_COLOR_NAVY),                  0x000080UL},
        {T(LXB_CSS_COLOR_OLDLACE),               0xFDF5E6UL},
        {T(LXB_CSS_COLOR_OLIVE),                 0x808000UL},
        {T(LXB_CSS_COLOR_OLIVEDRAB),             0x6B8E23UL},
        {T(LXB_CSS_COLOR_ORANGE),                0xFFA500UL},
        {T(LXB_CSS_COLOR_ORANGERED),             0xFF4500UL},
        {T(LXB_CSS_COLOR_ORCHID),                0xDA70D6UL},
        {T(LXB_CSS_COLOR_PALEGOLDENROD),         0xEEE8AAUL},
        {T(LXB_CSS_COLOR_PALEGREEN),             0x98FB98UL},
        {T(LXB_CSS_COLOR_PALETURQUOISE),         0xAFEEEEUL},
        {T(LXB_CSS_COLOR_PALEVIOLETRED),         0xDB7093UL},
        {T(LXB_CSS_COLOR_PAPAYAWHIP),            0xFFEFD5UL},
        {T(LXB_CSS_COLOR_PEACHPUFF),             0xFFDAB9UL},
        {T(LXB_CSS_COLOR_PERU),                  0xCD853FUL},
        {T(LXB_CSS_COLOR_PINK),                  0xFFC0CBUL},
        {T(LXB_CSS_COLOR_PLUM),                  0xDDA0DDUL},
        {T(LXB_CSS_COLOR_POWDERBLUE),            0xB0E0E6UL},
        {T(LXB_CSS_COLOR_PURPLE),                0x800080UL},
        {T(LXB_CSS_COLOR_REBECCAPURPLE),         0x663399UL},
        {T(LXB_CSS_COLOR_RED),                   0xFF0000UL},
        {T(LXB_CSS_COLOR_ROSYBROWN),             0xBC8F8FUL},
        {T(LXB_CSS_COLOR_ROYALBLUE),             0x4169E1UL},
        {T(LXB_CSS_COLOR_SADDLEBROWN),           0x8B4513UL},
        {T(LXB_CSS_COLOR_SALMON),                0xFA8072UL},
        {T(LXB_CSS_COLOR_SANDYBROWN),            0xF4A460UL},
        {T(LXB_CSS_COLOR_SEAGREEN),              0x2E8B57UL},
        {T(LXB_CSS_COLOR_SEASHELL),              0xFFF5EEUL},
        {T(LXB_CSS_COLOR_SIENNA),                0xA0522DUL},
        {T(LXB_CSS_COLOR_SILVER),                0xC0C0C0UL},
        {T(LXB_CSS_COLOR_SKYBLUE),               0x87CEEBUL},
        {T(LXB_CSS_COLOR_SLATEBLUE),             0x6A5ACDUL},
        {T(LXB_CSS_COLOR_SLATEGRAY),             0x708090UL},
        {T(LXB_CSS_COLOR_SLATEGREY),             0x708090UL},
        {T(LXB_CSS_COLOR_SNOW),                  0xFFFAFAUL},
        {T(LXB_CSS_COLOR_SPRINGGREEN),           0x00FF7FUL},
        {T(LXB_CSS_COLOR_STEELBLUE),             0x4682B4UL},
        {T(LXB_CSS_COLOR_TAN),                   0xD2B48CUL},
        {T(LXB_CSS_COLOR_TEAL),                  0x008080UL},
        {T(LXB_CSS_COLOR_THISTLE),               0xD8BFD8UL},
        {T(LXB_CSS_COLOR_TOMATO),                0xFF6347UL},
        {T(LXB_CSS_COLOR_TURQUOISE),             0x40E0D0UL},
        {T(LXB_CSS_COLOR_VIOLET),                0xEE82EEUL},
        {T(LXB_CSS_COLOR_WHEAT),                 0xF5DEB3UL},
        {T(LXB_CSS_COLOR_WHITE),                 0xFFFFFFUL},
        {T(LXB_CSS_COLOR_WHITESMOKE),            0xF5F5F5UL},
        {T(LXB_CSS_COLOR_YELLOW),                0xFFFF00UL},
        {T(LXB_CSS_COLOR_YELLOWGREEN),           0x9ACD32UL},
    };
    for (const auto& e : kTable) {
        if (static_cast<int>(e.type) == static_cast<int>(type)) {
            const std::uint32_t rgb = e.rgb;
            out = make_rgba(static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
                            static_cast<std::uint8_t>((rgb >>  8) & 0xFF),
                            static_cast<std::uint8_t>( rgb        & 0xFF),
                            0xFF);
            return true;
        }
    }
    return false;
}


// Common CSS color values used by framework stylesheets. Bootstrap
// relies heavily on hex + rgba; named-color coverage can grow as real
// inputs require it.
bool parse_color(const lxb_css_value_color_t* v, std::uint32_t& out) {
    if (!v) return false;
    if (v->type == LXB_CSS_COLOR_HEX) {
        const auto& h = v->u.hex.rgba;
        if (v->u.hex.type == LXB_CSS_PROPERTY_COLOR_HEX_TYPE_3 ||
            v->u.hex.type == LXB_CSS_PROPERTY_COLOR_HEX_TYPE_4) {
            const auto expand = [](std::uint8_t c) {
                return static_cast<std::uint8_t>((c << 4) | c);
            };
            out = make_rgba(expand(h.r), expand(h.g), expand(h.b),
                            v->u.hex.type == LXB_CSS_PROPERTY_COLOR_HEX_TYPE_4
                                ? expand(h.a)
                                : 0xFF);
            return true;
        }
        out = make_rgba(h.r, h.g, h.b,
                        v->u.hex.type == LXB_CSS_PROPERTY_COLOR_HEX_TYPE_8
                            ? h.a
                            : 0xFF);
        return true;
    }
    if (v->type == LXB_CSS_COLOR_RGB || v->type == LXB_CSS_COLOR_RGBA) {
        out = make_rgba(color_component(v->u.rgb.r),
                        color_component(v->u.rgb.g),
                        color_component(v->u.rgb.b),
                        alpha_component(v->u.rgb.a));
        return true;
    }
    if (v->type == LXB_CSS_COLOR_HSL || v->type == LXB_CSS_COLOR_HSLA) {
        const auto& hsl = v->u.hsl;
        const double h = hue_to_degrees(hsl.h);
        // s and l are lxb_css_value_percentage_type_t. In the legacy
        // comma-separated format (hsl.old == true), lexbor only fills
        // s.percentage / l.percentage and does NOT set the .type field.
        // In the modern format .type is LXB_CSS_VALUE__PERCENTAGE.
        // Read the raw percentage.num in both cases; % semantics apply
        // either way (old format only accepts percentages for s and l).
        const double s = hsl.s.percentage.num / 100.0;
        const double l = hsl.l.percentage.num / 100.0;
        std::uint8_t r, g, b;
        hsl_to_rgb(h, s, l, r, g, b);
        const std::uint8_t a = alpha_component(hsl.a);
        out = make_rgba(r, g, b, a);
        return true;
    }
    if (v->type == LXB_CSS_COLOR_TRANSPARENT) {
        out = 0x00000000u;  // special-cased: zero alpha
        return true;
    }
    // All other CSS named colors (black/white/aliceblue/…) resolve via
    // the named-color table.
    return parse_named_color(v->type, out);
}

// Variant that resolves `currentcolor` to `current_color_rgba`.
// CSS `currentcolor` is the element's own `color` property value.
// This overload is used at border-color call sites where the current
// foreground colour is already resolved into `s.animated.color_rgba`.
bool parse_color(const lxb_css_value_color_t* v, std::uint32_t& out,
                 std::uint32_t current_color_rgba) {
    if (v && v->type == LXB_CSS_COLOR_CURRENTCOLOR) {
        out = current_color_rgba;
        return true;
    }
    return parse_color(v, out);
}

// em_px: the font-size in CSS pixels used to resolve `em` lengths.
//   For most properties this is the element's own computed font-size.
//   For `font-size` itself it must be the PARENT's font-size (passed by
//   the caller), because `font-size: 0.75em` means "75% of the parent's
//   font-size", not "75% of myself".
// rem always resolves against the root font-size (16 px UA default).
bool resolve_length_px(double num, int unit, double& out_px,
                       double em_px = 16.0,
                       double viewport_w = 0.0,
                       double viewport_h = 0.0,
                       ViewportDependency* dependency = nullptr) {
    mark_viewport_dependency(unit, dependency);
    switch (unit) {
        case LXB_CSS_UNIT__UNDEF:
        case LXB_CSS_UNIT_PX:
            out_px = num;
            return true;
        case LXB_CSS_UNIT_REM:
            out_px = num * 16.0;
            return true;
        case LXB_CSS_UNIT_EM:
            out_px = num * em_px;
            return true;
        case LXB_CSS_UNIT_VW:
        case LXB_CSS_UNIT_VI:
            if (viewport_w <= 0.0) return false;
            out_px = num * viewport_w / 100.0;
            return true;
        case LXB_CSS_UNIT_VH:
        case LXB_CSS_UNIT_VB:
            if (viewport_h <= 0.0) return false;
            out_px = num * viewport_h / 100.0;
            return true;
        case LXB_CSS_UNIT_VMIN:
            if (viewport_w <= 0.0 || viewport_h <= 0.0) return false;
            out_px = num * std::min(viewport_w, viewport_h) / 100.0;
            return true;
        case LXB_CSS_UNIT_VMAX:
            if (viewport_w <= 0.0 || viewport_h <= 0.0) return false;
            out_px = num * std::max(viewport_w, viewport_h) / 100.0;
            return true;
        default:
            return false;
    }
}

bool parse_length_value(double num, int unit, int& out, double em_px = 16.0,
                        double viewport_w = 0.0,
                        double viewport_h = 0.0,
                        ViewportDependency* dependency = nullptr) {
    // std::lround rounds half away from zero, so negative lengths (e.g. a
    // calc()-derived negative margin) round symmetrically — plain
    // `int(num + 0.5)` would truncate -4.0 toward zero as -3.
    double px = 0.0;
    if (!resolve_length_px(num, unit, px, em_px, viewport_w, viewport_h,
                           dependency))
        return false;
    out = static_cast<int>(std::lround(px));
    return true;
}

bool parse_length_value_x100(double num, int unit, int& out,
                             double em_px = 16.0,
                             double viewport_w = 0.0,
                             double viewport_h = 0.0,
                             ViewportDependency* dependency = nullptr) {
    double px = 0.0;
    if (!resolve_length_px(num, unit, px, em_px, viewport_w, viewport_h,
                           dependency))
        return false;

    out = static_cast<int>(std::clamp(std::lround(px * 100.0),
                                      -32767L, 32767L));
    return true;
}

// Resolve a `<length>` value to integer CSS pixels. Phase 2A handles
// LENGTH-typed values in `px`, `rem`, and `em`; % follows when the
// percent-resolution context is wired in.
//
// CSS lets you write a unit-less `0` for any length value
// (`padding: 6px 0` is valid; the `0` parses as a NUMBER, not a
// LENGTH, but the spec treats it as zero pixels). Without this
// fallback, padding shorthands like `padding: 6px 0` silently lose
// the right/left sides and the mirror logic treats them as "unset"
// — copying the top value into all four sides. Visible bug: items
// got 6px of horizontal padding they didn't ask for.
//
// em_px: the font-size in px used for `em` unit resolution (see
// parse_length_value). Default 16 is correct for callers that have
// no font-size context (apply_decl_list, unit tests).
bool parse_length_px(const lxb_css_value_length_percentage_type_t* v, int& out,
                     double em_px = 16.0,
                     double viewport_w = 0.0,
                     double viewport_h = 0.0,
                     ViewportDependency* dependency = nullptr) {
    if (!v) return false;
    if (v->length.type == LXB_CSS_VALUE__LENGTH) {
        const auto& L = v->length.u.length;
        return parse_length_value(L.num, static_cast<int>(L.unit), out, em_px,
                                  viewport_w, viewport_h, dependency);
    }
    if (v->length.type == LXB_CSS_VALUE__PERCENTAGE &&
        v->length.u.percentage.num == 0.0) {
        out = 0;
        return true;
    }
    if (v->length.type == LXB_CSS_VALUE__NUMBER) {
        out = 0;  // unit-less zero is the only valid NUMBER here
        return true;
    }
    return false;
}

bool parse_length_px(const lxb_css_value_length_type_t* v, int& out,
                     double em_px = 16.0,
                     double viewport_w = 0.0,
                     double viewport_h = 0.0,
                     ViewportDependency* dependency = nullptr) {
    if (!v) return false;
    if (v->type == LXB_CSS_VALUE__LENGTH) {
        return parse_length_value(v->length.num,
                                  static_cast<int>(v->length.unit), out, em_px,
                                  viewport_w, viewport_h, dependency);
    }
    if (v->type == LXB_CSS_VALUE__NUMBER) {
        // The lxb_css_value_length_type_t union doesn't expose the
        // number field directly here — but a NUMBER-typed length is
        // always the unit-less zero shorthand per CSS spec. Treat
        // as 0.
        out = 0;
        return true;
    }
    return false;
}

void clear_box_shadow(AnimatedStyle& s) {
    s.shadow_rgba = 0x00000000u;
    s.shadow_offset_x = 0;
    s.shadow_offset_y = 0;
    s.shadow_blur = 0;
    s.shadow_spread = 0;
    s.shadow_inset = false;
}

bool resolve_box_shadow_layer(const lxb_css_property_box_shadow_layer_t& v,
                              const AnimatedStyle& current,
                              BoxShadowLayer& out,
                              double em_px = 16.0,
                              double viewport_w = 0.0,
                              double viewport_h = 0.0,
                              ViewportDependency* dependency = nullptr) {
    int ox = 0;
    int oy = 0;
    int blur = 0;
    int spread = 0;
    if (!parse_length_px(&v.offset_x, ox, em_px, viewport_w, viewport_h,
                         dependency) ||
        !parse_length_px(&v.offset_y, oy, em_px, viewport_w, viewport_h,
                         dependency)) {
        return false;
    }
    if (v.blur_radius.type != LXB_CSS_VALUE__UNDEF) {
        parse_length_px(&v.blur_radius, blur, em_px, viewport_w, viewport_h,
                        dependency);
    }
    if (v.spread_radius.type != LXB_CSS_VALUE__UNDEF) {
        parse_length_px(&v.spread_radius, spread, em_px, viewport_w,
                        viewport_h, dependency);
    }

    std::uint32_t rgba = current.color_rgba;
    parse_color(&v.color, rgba);
    out.rgba = rgba;
    out.offset_x = static_cast<std::int16_t>(ox);
    out.offset_y = static_cast<std::int16_t>(oy);
    out.blur = static_cast<std::int16_t>(blur);
    out.spread = static_cast<std::int16_t>(spread);
    out.inset = v.inset;
    return true;
}

void store_legacy_box_shadow(AnimatedStyle& s, const BoxShadowLayer& layer) {
    s.shadow_rgba = layer.rgba;
    s.shadow_offset_x = layer.offset_x;
    s.shadow_offset_y = layer.offset_y;
    s.shadow_blur = layer.blur;
    s.shadow_spread = layer.spread;
    s.shadow_inset = layer.inset;
}

void clear_border_side_colors(AnimatedStyle& s) {
    s.border_top_rgba = 0x00000000u;
    s.border_right_rgba = 0x00000000u;
    s.border_bottom_rgba = 0x00000000u;
    s.border_left_rgba = 0x00000000u;
    s.border_color_set = 0;
}

void set_border_side_color(AnimatedStyle& s, std::uint8_t side,
                           std::uint32_t rgba) {
    switch (side) {
        case AnimatedStyle::BorderTopColorSet:
            s.border_top_rgba = rgba;
            break;
        case AnimatedStyle::BorderRightColorSet:
            s.border_right_rgba = rgba;
            break;
        case AnimatedStyle::BorderBottomColorSet:
            s.border_bottom_rgba = rgba;
            break;
        case AnimatedStyle::BorderLeftColorSet:
            s.border_left_rgba = rgba;
            break;
        default:
            return;
    }
    s.border_color_set = static_cast<std::uint8_t>(s.border_color_set | side);
}

void apply_text_decoration_line(
    const lxb_css_property_text_decoration_line_t& line,
    ComputedStyle& computed) {
    using C = ComputedStyle;
    std::uint8_t bits = C::DecorationNone;
    if (line.type == LXB_CSS_TEXT_DECORATION_LINE_UNDERLINE ||
        line.underline == LXB_CSS_TEXT_DECORATION_LINE_UNDERLINE) {
        bits |= C::DecorationUnderline;
    }
    if (line.type == LXB_CSS_TEXT_DECORATION_LINE_OVERLINE ||
        line.overline == LXB_CSS_TEXT_DECORATION_LINE_OVERLINE) {
        bits |= C::DecorationOverline;
    }
    if (line.type == LXB_CSS_TEXT_DECORATION_LINE_LINE_THROUGH ||
        line.line_through == LXB_CSS_TEXT_DECORATION_LINE_LINE_THROUGH) {
        bits |= C::DecorationLineThrough;
    }

    if (bits != C::DecorationNone) {
        computed.text_decoration_line = bits;
    } else if (line.type == LXB_CSS_TEXT_DECORATION_LINE_NONE) {
        computed.text_decoration_line = C::DecorationNone;
    }
}

bool parse_length_px(const lxb_css_value_length_percentage_t* v, int& out,
                     double em_px = 16.0,
                     double viewport_w = 0.0,
                     double viewport_h = 0.0,
                     ViewportDependency* dependency = nullptr) {
    if (!v) return false;
    if (v->type == LXB_CSS_VALUE__LENGTH) {
        return parse_length_value(v->u.length.num,
                                  static_cast<int>(v->u.length.unit), out, em_px,
                                  viewport_w, viewport_h, dependency);
    }
    if (v->type == LXB_CSS_VALUE__PERCENTAGE &&
        v->u.percentage.num == 0.0) {
        out = 0;
        return true;
    }
    if (v->type == LXB_CSS_VALUE__NUMBER) {
        // Unit-less zero — lexbor reuses the length field's num for
        // the numeric value. CSS spec only allows unit-less zero for
        // length values; treat any value here as the zero-shorthand.
        out = 0;
        return true;
    }
    return false;
}

bool parse_length_float(const lxb_css_value_length_percentage_t& v,
                        float& out,
                        double em_px = 16.0,
                        double viewport_w = 0.0,
                        double viewport_h = 0.0,
                        ViewportDependency* dependency = nullptr) {
    if (v.type == LXB_CSS_VALUE__LENGTH) {
        double px = 0.0;
        if (!resolve_length_px(v.u.length.num,
                               static_cast<int>(v.u.length.unit), px,
                               em_px, viewport_w, viewport_h, dependency)) {
            return false;
        }
        out = static_cast<float>(px);
        return true;
    }
    if (v.type == LXB_CSS_VALUE__PERCENTAGE && v.u.percentage.num == 0.0) {
        out = 0.0f;
        return true;
    }
    if (v.type == LXB_CSS_VALUE__NUMBER) {
        out = 0.0f;
        return true;
    }
    return false;
}

bool parse_translate_float(const lxb_css_value_length_percentage_t& v,
                           float& px_out,
                           float& pct_out,
                           double em_px = 16.0,
                           double viewport_w = 0.0,
                           double viewport_h = 0.0,
                           ViewportDependency* dependency = nullptr) {
    if (v.type == LXB_CSS_VALUE__PERCENTAGE) {
        pct_out = static_cast<float>(v.u.percentage.num);
        return true;
    }
    return parse_length_float(v, px_out, em_px, viewport_w, viewport_h,
                              dependency);
}

bool parse_radius(const lxb_css_property_border_radius_corner_t& corner,
                  std::int16_t& out, double em_px = 16.0,
                  double viewport_w = 0.0,
                  double viewport_h = 0.0,
                  ViewportDependency* dependency = nullptr) {
    // AffineUI currently stores one scalar radius per corner. CSS
    // allows elliptical radii (`h / v`); use the horizontal radius
    // until the renderer grows paired radii.
    int px = 0;
    if (parse_length_px(&corner.h, px, em_px, viewport_w, viewport_h,
                        dependency)) {
        out = static_cast<std::int16_t>(px);
        return true;
    }

    if (corner.h.type == LXB_CSS_VALUE__PERCENTAGE) {
        const double pct = corner.h.u.percentage.num;
        const int pct_x100 = static_cast<int>(
            std::round(std::max(0.0, pct) * 100.0));
        out = encode_border_radius_pct_x100(pct_x100);
        return true;
    }

    return false;
}

void apply_flex_basis_value(const lxb_css_property_flex_basis_t& basis,
                            ResolvedStyle& s, double em_px = 16.0,
                            double viewport_w = 0.0,
                            double viewport_h = 0.0,
                            ViewportDependency* dependency = nullptr) {
    int px = 0;
    if (basis.type == LXB_CSS_VALUE_AUTO ||
        basis.type == LXB_CSS_VALUE_MIN_CONTENT ||
        basis.type == LXB_CSS_VALUE_MAX_CONTENT ||
        basis.type == LXB_CSS_FLEX_BASIS_CONTENT) {
        s.computed.flex_basis = -1;
        s.computed.flex_basis_pct = -1;
        return;
    }
    // Handle percentage flex-basis (e.g. flex: 0 0 0%, flex: 0 0 50%).
    if (basis.type == LXB_CSS_VALUE__PERCENTAGE) {
        const double pct = basis.u.percentage.num;
        const double clamped = pct < 0.0 ? 0.0 : (pct > 100.0 ? 100.0 : pct);
        // int8_t range: store integer percent (0..100). -1 = unset.
        s.computed.flex_basis_pct =
            static_cast<std::int8_t>(clamped + 0.5);
        s.computed.flex_basis = -1;  // px field → auto when pct governs
        return;
    }
    s.computed.flex_basis_pct = -1;
    if (parse_length_px(&basis, px, em_px, viewport_w, viewport_h,
                        dependency) && px >= 0) {
        s.computed.flex_basis = static_cast<std::int16_t>(px);
    }
}

// Apply a CSS width/height value to the px and (optionally) pct fields.
// `pct_out` receives percentage × 100 rounded to int16 (0..10000),
// or -1 if the value is not a percentage. `out` (the px field) is set
// to `auto_value` (-1) when a percentage is applied so yoga_adapter
// can tell at a glance which field governs.
void apply_width_value(const lxb_css_property_width_t& width,
                       std::int16_t& out,
                       std::int16_t auto_value,
                       std::int16_t* pct_out = nullptr,
                       double em_px = 16.0,
                       double viewport_w = 0.0,
                       double viewport_h = 0.0,
                       ViewportDependency* dependency = nullptr) {
    if (pct_out) *pct_out = -1;  // default: not a percentage
    int px = 0;
    if (width.type == LXB_CSS_VALUE_AUTO ||
        width.type == LXB_CSS_VALUE_NONE ||
        width.type == LXB_CSS_VALUE_MIN_CONTENT ||
        width.type == LXB_CSS_VALUE_MAX_CONTENT) {
        out = auto_value;
        return;
    }
    // Handle percentage values (e.g. width: 33.33333333%).
    if (width.type == LXB_CSS_VALUE__PERCENTAGE && pct_out) {
        const double pct = width.u.percentage.num;
        // Clamp to 0..100, store as pct × 100 (0..10000).
        // Use floor (truncation) so sibling percentages that sum to
        // 100% (e.g. col-2+col-8+col-2 = 16.67+66.67+16.67%) do not
        // round up and cause Yoga to wrap the flex row.
        const double clamped = pct < 0.0 ? 0.0 : (pct > 100.0 ? 100.0 : pct);
        *pct_out = static_cast<std::int16_t>(clamped * 100.0);
        out = auto_value;  // px field → auto when pct governs
        return;
    }
    if (parse_length_px(&width, px, em_px, viewport_w, viewport_h,
                        dependency) && px >= 0) {
        out = static_cast<std::int16_t>(px);
    }
}

enum class InsetEdge : std::uint8_t { Top, Right, Bottom, Left };

void store_inset_edge(ComputedStyle& cs, InsetEdge edge, int value,
                      bool is_pct, bool is_set) {
    const auto clamped = static_cast<std::int16_t>(
        std::clamp(value, -32768, 32767));
    switch (edge) {
        case InsetEdge::Top:
            cs.inset_top = is_set ? clamped : 0;
            cs.inset_has.top = is_set ? 1 : 0;
            cs.inset_has.top_pct = is_pct ? 1 : 0;
            break;
        case InsetEdge::Right:
            cs.inset_right = is_set ? clamped : 0;
            cs.inset_has.right = is_set ? 1 : 0;
            cs.inset_has.right_pct = is_pct ? 1 : 0;
            break;
        case InsetEdge::Bottom:
            cs.inset_bottom = is_set ? clamped : 0;
            cs.inset_has.bottom = is_set ? 1 : 0;
            cs.inset_has.bottom_pct = is_pct ? 1 : 0;
            break;
        case InsetEdge::Left:
            cs.inset_left = is_set ? clamped : 0;
            cs.inset_has.left = is_set ? 1 : 0;
            cs.inset_has.left_pct = is_pct ? 1 : 0;
            break;
    }
}

bool apply_inset_value(const lxb_css_value_length_percentage_t& value,
                       ComputedStyle& cs, InsetEdge edge,
                       double em_px = 16.0,
                       double viewport_w = 0.0,
                       double viewport_h = 0.0) {
    if (value.type == LXB_CSS_VALUE_AUTO ||
        value.type == LXB_CSS_VALUE_INITIAL ||
        value.type == LXB_CSS_VALUE_UNSET ||
        value.type == LXB_CSS_VALUE_REVERT)
    {
        store_inset_edge(cs, edge, 0, false, false);
        return true;
    }
    if (value.type == LXB_CSS_VALUE__PERCENTAGE) {
        const int pct_x100 = static_cast<int>(
            std::lround(value.u.percentage.num * 100.0));
        store_inset_edge(cs, edge, pct_x100, true, true);
        return true;
    }

    int px = 0;
    if (parse_length_px(&value, px, em_px, viewport_w, viewport_h)) {
        store_inset_edge(cs, edge, px, false, true);
        return true;
    }
    return false;
}

// ── CSS custom properties + var() substitution ──────────────────────
//
// lexbor parses declarations into typed structs at stylesheet-parse
// time, but `var()` can only be resolved per element at cascade time
// (custom properties inherit and may differ per element). lexbor
// already preserves what we need: a `--name: value` declaration lands
// as LXB_CSS_PROPERTY__CUSTOM (name + raw value string); a normal
// property whose value failed to type-parse (because it contains
// var()) lands as LXB_CSS_PROPERTY__UNDEF (the intended property id +
// raw value string). So the whole feature lives here: build the
// element's inherited custom-property map, substitute var() in the raw
// string, then re-parse the resolved declaration back through lexbor.

void apply_transform_value(const lxb_css_property_transform_t& transform,
                           AnimatedStyle& s,
                           double em_px = 16.0,
                           double viewport_w = 0.0,
                           double viewport_h = 0.0) {
    s.tx = 0.0f;
    s.ty = 0.0f;
    s.tx_pct = 0.0f;
    s.ty_pct = 0.0f;
    s.scale_x = 1.0f;
    s.scale_y = 1.0f;
    s.rotation = 0.0f;

    if (transform.type != LXB_CSS_TRANSFORM_VALUE_LIST) return;

    for (std::uint8_t i = 0; i < transform.count; ++i) {
        const auto& fn = transform.functions[i];
        float x = 0.0f;
        float y = 0.0f;
        float xp = 0.0f;
        float yp = 0.0f;
        switch (fn.type) {
            case LXB_CSS_TRANSFORM_FUNCTION_TRANSLATE:
                if (parse_translate_float(fn.x, x, xp, em_px, viewport_w, viewport_h)) {
                    s.tx += x;
                    s.tx_pct += xp;
                }
                if (parse_translate_float(fn.y, y, yp, em_px, viewport_w, viewport_h)) {
                    s.ty += y;
                    s.ty_pct += yp;
                }
                break;
            case LXB_CSS_TRANSFORM_FUNCTION_TRANSLATE_X:
                if (parse_translate_float(fn.x, x, xp, em_px, viewport_w, viewport_h)) {
                    s.tx += x;
                    s.tx_pct += xp;
                }
                break;
            case LXB_CSS_TRANSFORM_FUNCTION_TRANSLATE_Y:
                if (parse_translate_float(fn.y, y, yp, em_px, viewport_w, viewport_h)) {
                    s.ty += y;
                    s.ty_pct += yp;
                }
                break;
            case LXB_CSS_TRANSFORM_FUNCTION_SCALE:
                s.scale_x *= static_cast<float>(fn.numbers[0].num);
                s.scale_y *= static_cast<float>(fn.numbers[1].num);
                break;
            case LXB_CSS_TRANSFORM_FUNCTION_SCALE_X:
                s.scale_x *= static_cast<float>(fn.numbers[0].num);
                break;
            case LXB_CSS_TRANSFORM_FUNCTION_SCALE_Y:
                s.scale_y *= static_cast<float>(fn.numbers[1].num);
                break;
            case LXB_CSS_TRANSFORM_FUNCTION_ROTATE:
                s.rotation += angle_to_radians(fn.angle);
                break;
            case LXB_CSS_TRANSFORM_FUNCTION_MATRIX:
                s.scale_x *= static_cast<float>(fn.numbers[0].num);
                s.scale_y *= static_cast<float>(fn.numbers[3].num);
                s.tx += static_cast<float>(fn.numbers[4].num);
                s.ty += static_cast<float>(fn.numbers[5].num);
                break;
        }
    }
}

void apply_transform_origin_value(
    const lxb_css_property_transform_origin_t& origin,
    AnimatedStyle& s,
    double em_px = 16.0,
    double viewport_w = 0.0,
    double viewport_h = 0.0) {
    float px = 0.0f;
    float pct = 0.0f;

    if (parse_translate_float(origin.x, px, pct, em_px, viewport_w, viewport_h)) {
        s.origin_x = px;
        s.origin_x_pct = pct;
    }

    px = 0.0f;
    pct = 0.0f;
    if (parse_translate_float(origin.y, px, pct, em_px, viewport_w, viewport_h)) {
        s.origin_y = px;
        s.origin_y_pct = pct;
    }
}

void apply_animation_value(const lxb_css_property_animation_t& in,
                           ResolvedStyle::CssAnimation& out) {
    if (in.has_name && in.name.data != nullptr && in.name.length > 0) {
        out.name_hash = fnv1a_32(std::string_view(
            reinterpret_cast<const char*>(in.name.data), in.name.length));
        out.active = true;
    }

    out.duration_s = static_cast<float>(std::max(0.0, in.duration_s));
    out.delay_s = static_cast<float>(in.delay_s);
    out.iteration_count = static_cast<float>(
        std::max(0.0, in.iteration_count));

    using A = ResolvedStyle::CssAnimation;
    switch (in.timing) {
        case LXB_CSS_ANIMATION_TIMING_LINEAR:      out.timing = A::Timing::Linear; break;
        case LXB_CSS_ANIMATION_TIMING_EASE_IN:     out.timing = A::Timing::EaseIn; break;
        case LXB_CSS_ANIMATION_TIMING_EASE_OUT:    out.timing = A::Timing::EaseOut; break;
        case LXB_CSS_ANIMATION_TIMING_EASE_IN_OUT: out.timing = A::Timing::EaseInOut; break;
        case LXB_CSS_ANIMATION_TIMING_STEP_START:  out.timing = A::Timing::StepStart; break;
        case LXB_CSS_ANIMATION_TIMING_STEP_END:    out.timing = A::Timing::StepEnd; break;
        case LXB_CSS_ANIMATION_TIMING_EASE:
        default:                                   out.timing = A::Timing::Ease; break;
    }
    switch (in.direction) {
        case LXB_CSS_ANIMATION_DIRECTION_REVERSE:
            out.direction = A::Direction::Reverse; break;
        case LXB_CSS_ANIMATION_DIRECTION_ALTERNATE:
            out.direction = A::Direction::Alternate; break;
        case LXB_CSS_ANIMATION_DIRECTION_ALTERNATE_REVERSE:
            out.direction = A::Direction::AlternateReverse; break;
        case LXB_CSS_ANIMATION_DIRECTION_NORMAL:
        default:
            out.direction = A::Direction::Normal; break;
    }
    switch (in.fill_mode) {
        case LXB_CSS_ANIMATION_FILL_MODE_FORWARDS:
            out.fill_mode = A::FillMode::Forwards; break;
        case LXB_CSS_ANIMATION_FILL_MODE_BACKWARDS:
            out.fill_mode = A::FillMode::Backwards; break;
        case LXB_CSS_ANIMATION_FILL_MODE_BOTH:
            out.fill_mode = A::FillMode::Both; break;
        case LXB_CSS_ANIMATION_FILL_MODE_NONE:
        default:
            out.fill_mode = A::FillMode::None; break;
    }
    out.play_state = in.play_state == LXB_CSS_ANIMATION_PLAY_STATE_PAUSED
        ? A::PlayState::Paused
        : A::PlayState::Running;
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

std::string_view trim_ws(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.remove_suffix(1);
    return s;
}

// Index of the matching ')' for the '(' at `open`, honoring nesting.
// Returns npos if unbalanced.
std::size_t match_paren(std::string_view s, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') { if (--depth == 0) return i; }
    }
    return std::string_view::npos;
}

// Find the next `var(` function token at/after `from` (must be at a
// token boundary, not the tail of a longer ident). Returns the index
// of the 'v', or npos.
std::size_t find_var(std::string_view s, std::size_t from) {
    for (std::size_t i = from; i + 4 <= s.size(); ++i) {
        if ((s[i] == 'v' || s[i] == 'V') &&
            (s[i + 1] == 'a' || s[i + 1] == 'A') &&
            (s[i + 2] == 'r' || s[i + 2] == 'R') &&
            s[i + 3] == '(') {
            if (i == 0 || !is_ident_char(s[i - 1])) return i;
        }
    }
    return std::string_view::npos;
}

// Replace every var(--name[, fallback]) in `input` with the custom
// property's value (or the fallback), recursively — a custom property
// value may itself contain var(). `depth` guards against cycles.
std::string substitute_vars(std::string_view input, const CustomPropMap& map,
                            int depth = 0) {
    if (depth > 32) return std::string(input);  // cycle / runaway guard
    std::string out;
    std::size_t i = 0;
    while (i < input.size()) {
        const std::size_t v = find_var(input, i);
        if (v == std::string_view::npos) { out.append(input.substr(i)); break; }
        out.append(input.substr(i, v - i));
        const std::size_t open = v + 3;             // the '(' after "var"
        const std::size_t close = match_paren(input, open);
        if (close == std::string_view::npos) {       // unbalanced — bail
            out.append(input.substr(v));
            break;
        }
        std::string_view args = input.substr(open + 1, close - open - 1);
        // Split off the custom-property name at the first top-level comma.
        std::string_view name = args, fallback;
        bool has_fb = false;
        int pd = 0;
        for (std::size_t k = 0; k < args.size(); ++k) {
            if (args[k] == '(') ++pd;
            else if (args[k] == ')') --pd;
            else if (args[k] == ',' && pd == 0) {
                name = args.substr(0, k);
                fallback = args.substr(k + 1);
                has_fb = true;
                break;
            }
        }
        const std::string key(trim_ws(name));
        std::string replacement;
        if (auto it = map.find(key); it != map.end()) {
            replacement = substitute_vars(trim_ws(it->second), map, depth + 1);
        } else if (has_fb) {
            replacement = substitute_vars(trim_ws(fallback), map, depth + 1);
        }  // else: invalid var with no fallback → empties out (CSS: IACVT)
        out.append(replacement);
        i = close + 1;
    }
    return out;
}

// ── calc() evaluation ──────────────────────────────────────────────
// lexbor does not parse calc(). var() resolution already does string-
// level substitution + re-parse, so calc() is evaluated in that same
// pipeline (run AFTER substitute_vars, so only numbers/units/operators
// remain). We resolve calc()s that reduce to one computed scalar:
// a pure number, a px length, or a pure percentage. Mixed length +
// percentage expressions still need layout context, so those are left
// verbatim and the declaration is then dropped on re-parse (CSS
// "invalid at computed-value time" in our current subset).

bool calc_kw_at(std::string_view s, std::size_t i) {  // "calc(" at boundary
    static constexpr std::string_view kw = "calc(";
    if (i + kw.size() > s.size()) return false;
    for (std::size_t k = 0; k < kw.size(); ++k)
        if (static_cast<char>(std::tolower(
                static_cast<unsigned char>(s[i + k]))) != kw[k]) return false;
    return i == 0 || !is_ident_char(s[i - 1]);
}

std::size_t find_calc(std::string_view s, std::size_t from) {
    for (std::size_t i = from; i < s.size(); ++i)
        if ((s[i] == 'c' || s[i] == 'C') && calc_kw_at(s, i)) return i;
    return std::string_view::npos;
}

struct CalcVal {
    double v{0.0};
    int    dims{0};      // 0 = pure number, 1 = length (px), 2 = percentage
    bool   ok{false};
};

void calc_ws(std::string_view s, std::size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}

CalcVal calc_expr(std::string_view s, std::size_t& i,
                  double rem, double em,
                  double viewport_w, double viewport_h,
                  ViewportDependency* dependency);

CalcVal calc_factor(std::string_view s, std::size_t& i,
                    double rem, double em,
                    double viewport_w, double viewport_h,
                    ViewportDependency* dependency) {
    calc_ws(s, i);
    if (i >= s.size()) return {};
    // Unary sign that applies to a parenthesised group / nested calc.
    double sign = 1.0;
    if (s[i] == '+' || s[i] == '-') {
        std::size_t k = i + 1;
        calc_ws(s, k);
        if (k < s.size() && (s[k] == '(' || calc_kw_at(s, k))) {
            sign = (s[i] == '-') ? -1.0 : 1.0;
            i = k;
        }
    }
    if (i < s.size() && (s[i] == '(' || calc_kw_at(s, i))) {
        i += (s[i] == '(') ? 1 : 5;
        CalcVal inner =
            calc_expr(s, i, rem, em, viewport_w, viewport_h, dependency);
        if (!inner.ok) return {};
        calc_ws(s, i);
        if (i >= s.size() || s[i] != ')') return {};
        ++i;
        inner.v *= sign;
        return inner;
    }
    // Number (from_chars handles a leading sign and a bare ".5").
    double num = 0.0;
    auto fc = std::from_chars(s.data() + i, s.data() + s.size(), num);
    if (fc.ec != std::errc()) return {};
    i = static_cast<std::size_t>(fc.ptr - s.data());
    const std::size_t us = i;
    while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
    std::string_view unit = s.substr(us, i - us);
    mark_viewport_dependency(unit, dependency);
    if (i < s.size() && s[i] == '%') {
        if (!unit.empty()) return {};
        ++i;
        return {num, 2, true};
    }
    auto ieq = [](std::string_view a, const char* b) {
        std::size_t n = 0; for (; b[n]; ++n) {}
        if (a.size() != n) return false;
        for (std::size_t k = 0; k < n; ++k)
            if (static_cast<char>(std::tolower(
                    static_cast<unsigned char>(a[k]))) != b[k]) return false;
        return true;
    };
    if (unit.empty())     return {num,        0, true};
    if (ieq(unit, "px"))  return {num,        1, true};
    if (ieq(unit, "rem")) return {num * rem,  1, true};
    if (ieq(unit, "em"))  return {num * em,   1, true};
    if ((ieq(unit, "vw") || ieq(unit, "vi")) && viewport_w > 0.0)
        return {num * viewport_w / 100.0, 1, true};
    if ((ieq(unit, "vh") || ieq(unit, "vb")) && viewport_h > 0.0)
        return {num * viewport_h / 100.0, 1, true};
    if (ieq(unit, "vmin") && viewport_w > 0.0 && viewport_h > 0.0)
        return {num * std::min(viewport_w, viewport_h) / 100.0, 1, true};
    if (ieq(unit, "vmax") && viewport_w > 0.0 && viewport_h > 0.0)
        return {num * std::max(viewport_w, viewport_h) / 100.0, 1, true};
    return {};   // unknown unit
}

CalcVal calc_term(std::string_view s, std::size_t& i,
                  double rem, double em,
                  double viewport_w, double viewport_h,
                  ViewportDependency* dependency) {
    CalcVal a =
        calc_factor(s, i, rem, em, viewport_w, viewport_h, dependency);
    if (!a.ok) return {};
    for (;;) {
        calc_ws(s, i);
        if (i >= s.size() || (s[i] != '*' && s[i] != '/')) break;
        const char op = s[i++];
        CalcVal b =
            calc_factor(s, i, rem, em, viewport_w, viewport_h, dependency);
        if (!b.ok) return {};
        if (op == '*') {
            if (a.dims != 0 && b.dims != 0) return {};
            a.v *= b.v;
            a.dims = a.dims != 0 ? a.dims : b.dims;
        } else {
            if (b.v == 0.0) return {};
            a.v /= b.v;
            if (b.dims != 0) {
                if (a.dims != b.dims) return {};
                a.dims = 0;
            }
        }
    }
    return a;
}

CalcVal calc_expr(std::string_view s, std::size_t& i,
                  double rem, double em,
                  double viewport_w, double viewport_h,
                  ViewportDependency* dependency) {
    CalcVal a =
        calc_term(s, i, rem, em, viewport_w, viewport_h, dependency);
    if (!a.ok) return {};
    for (;;) {
        calc_ws(s, i);
        if (i >= s.size() || (s[i] != '+' && s[i] != '-')) break;
        const char op = s[i++];
        CalcVal b =
            calc_term(s, i, rem, em, viewport_w, viewport_h, dependency);
        if (!b.ok || a.dims != b.dims) return {};   // mismatched unit/number
        a.v += (op == '+') ? b.v : -b.v;
    }
    return a;
}

// Replace every resolvable top-level calc(...) in `input` with its
// evaluated literal (e.g. "calc(-.5 * 0.5rem)" -> "-4px"). Unresolvable
// calc()s are left untouched. `rem` is the root font size (px), `em` the
// element's font size (px), and viewport_w/viewport_h the CSS viewport
// dimensions for viewport units.
std::string evaluate_calc(std::string_view input, double rem, double em,
                          double viewport_w = 0.0,
                          double viewport_h = 0.0,
                          ViewportDependency* dependency = nullptr) {
    mark_viewport_dependencies_in_value(input, dependency);
    if (find_calc(input, 0) == std::string_view::npos)
        return std::string(input);
    std::string out;
    std::size_t i = 0;
    while (i < input.size()) {
        const std::size_t c = find_calc(input, i);
        if (c == std::string_view::npos) { out.append(input.substr(i)); break; }
        out.append(input.substr(i, c - i));
        std::size_t j = c + 5;   // past "calc("
        CalcVal r =
            calc_expr(input, j, rem, em, viewport_w, viewport_h, dependency);
        calc_ws(input, j);
        if (r.ok && (r.dims == 0 || r.dims == 1 || r.dims == 2) &&
            j < input.size() && input[j] == ')') {
            char buf[40];
            auto tc = std::to_chars(buf, buf + sizeof(buf), r.v);
            out.append(buf, tc.ptr);
            if (r.dims == 1) out += "px";
            else if (r.dims == 2) out += "%";
            i = j + 1;
        } else {
            // Unresolvable: keep the literal "calc(" and continue; the
            // remaining text (incl. its ')') copies through verbatim, so
            // the declaration fails to re-parse and is dropped.
            out.append(input.substr(c, 5));
            i = c + 5;
        }
    }
    return out;
}

// Route one declaration into the right struct.
// em_px: element's own computed font-size in px, used for `em` length
// resolution in all properties EXCEPT font-size itself (the caller
// must pass the PARENT's font-size when applying FONT_SIZE / FONT).
void apply_declaration(const lxb_css_rule_declaration_t* d, ResolvedStyle& s,
                       double em_px = 16.0,
                       const ResolvedStyle* parent = nullptr,
                       bool apply_font_size = true,
                       double viewport_w = 0.0,
                       double viewport_h = 0.0,
                       ViewportDependency* dependency = nullptr) {
    // Local helpers that forward em_px to the free-function overloads so
    // every length in this declaration resolves against the correct em.
    // These shadow the free functions within apply_declaration's scope.
    auto plx_lpt = [em_px, viewport_w, viewport_h, dependency](
                       const lxb_css_value_length_percentage_type_t* v, int& o) {
        return parse_length_px(v, o, em_px, viewport_w, viewport_h,
                               dependency);
    };
    auto plx_lt = [em_px, viewport_w, viewport_h, dependency](
                      const lxb_css_value_length_type_t* v, int& o) {
        return parse_length_px(v, o, em_px, viewport_w, viewport_h,
                               dependency);
    };
    auto plx_lp = [em_px, viewport_w, viewport_h, dependency](
                      const lxb_css_value_length_percentage_t* v, int& o) {
        return parse_length_px(v, o, em_px, viewport_w, viewport_h,
                               dependency);
    };
    auto plv = [em_px, viewport_w, viewport_h, dependency](
                   double num, int unit, int& o) {
        return parse_length_value(num, unit, o, em_px, viewport_w,
                                  viewport_h, dependency);
    };
    auto plv_x100 = [em_px, viewport_w, viewport_h, dependency](
                        double num, int unit, int& o) {
        return parse_length_value_x100(num, unit, o, em_px, viewport_w,
                                       viewport_h, dependency);
    };
    auto radius = [em_px, viewport_w, viewport_h, dependency](
                      const lxb_css_property_border_radius_corner_t& c,
                      std::int16_t& o) {
        return parse_radius(c, o, em_px, viewport_w, viewport_h, dependency);
    };
    auto awv = [em_px, viewport_w, viewport_h, dependency](
                   const lxb_css_property_width_t& w, std::int16_t& o,
                   std::int16_t av, std::int16_t* po = nullptr) {
        return apply_width_value(w, o, av, po, em_px, viewport_w,
                                 viewport_h, dependency);
    };
    auto afb = [em_px, viewport_w, viewport_h, dependency](
                   const lxb_css_property_flex_basis_t& b, ResolvedStyle& rs) {
        return apply_flex_basis_value(b, rs, em_px, viewport_w, viewport_h,
                                      dependency);
    };
    (void)plx_lpt; (void)plx_lt; (void)plx_lp; (void)plv; (void)plv_x100;
    (void)radius; (void)awv; (void)afb;
    switch (d->type) {
        // ── Paint-only ─────────────────────────────────────────────
        case LXB_CSS_PROPERTY_COLOR: {
            const auto* v = static_cast<const lxb_css_property_color_t*>(d->u.user);
            std::uint32_t rgba;
            if (parse_color(v, rgba)) s.animated.color_rgba = rgba;
            break;
        }
        case LXB_CSS_PROPERTY_BACKGROUND_COLOR: {
            const auto* v =
                static_cast<const lxb_css_property_background_color_t*>(d->u.user);
            std::uint32_t rgba;
            if (parse_color(v, rgba, s.animated.color_rgba))
                s.animated.background_rgba = rgba;
            break;
        }
        case LXB_CSS_PROPERTY_BACKGROUND:
        case LXB_CSS_PROPERTY_BACKGROUND_IMAGE: {
            const auto* v =
                static_cast<const lxb_css_property_background_t*>(d->u.user);
            std::uint32_t rgba;
            if (d->type == LXB_CSS_PROPERTY_BACKGROUND &&
                parse_color(&v->color, rgba, s.animated.color_rgba)) {
                s.animated.background_rgba = rgba;
            }

            const auto apply_gradient =
                [&](const lxb_css_property_gradient_t& gradient) {
                using GK = AnimatedStyle::GradientKind;
                switch (gradient.kind) {
                    case LXB_CSS_GRADIENT_LINEAR:
                        s.animated.gradient_kind = GK::Linear;
                        break;
                    case LXB_CSS_GRADIENT_RADIAL:
                        s.animated.gradient_kind = GK::Radial;
                        break;
                    case LXB_CSS_GRADIENT_LINEAR_STRIPES:
                        s.animated.gradient_kind = GK::LinearStripes;
                        break;
                    default:
                        s.animated.gradient_kind = GK::None;
                        break;
                }
                // Clamp angle to [0, 360)
                double ang = gradient.angle_deg;
                ang = ang - 360.0 * std::floor(ang / 360.0);
                s.animated.gradient_angle_deg = static_cast<std::int16_t>(ang);
                s.animated.gradient_center_x_pct =
                    static_cast<std::uint8_t>(std::clamp(
                        std::lround(gradient.center_x_pct), 0L, 100L));
                s.animated.gradient_center_y_pct =
                    static_cast<std::uint8_t>(std::clamp(
                        std::lround(gradient.center_y_pct), 0L, 100L));
                s.animated.gradient_stop1_pos_pct =
                    static_cast<std::uint8_t>(std::clamp(
                        std::lround(gradient.has_stop1_pos_pct
                            ? gradient.stop1_pos_pct : 100.0), 1L, 100L));

                std::uint32_t stop0 = 0, stop1 = 0;
                parse_color(&gradient.stop0, stop0);
                parse_color(&gradient.stop1, stop1);
                s.animated.gradient_stop0_rgba = stop0;
                s.animated.gradient_stop1_rgba = stop1;
            };

            // CSS background layers are painted back-to-front: the last
            // parsed layer is the bottom image. Keep our existing single
            // gradient descriptor for that bottom layer, then record the
            // common tiled two-linear-gradient grid overlay separately.
            if (v->layer_count != 0) {
                apply_gradient(v->layers[v->layer_count - 1]);

                if (v->layer_count >= 2 &&
                    v->layers[0].kind == LXB_CSS_GRADIENT_LINEAR &&
                    v->layers[1].kind == LXB_CSS_GRADIENT_LINEAR) {
                    std::uint32_t grid0 = 0, grid1 = 0, clear0 = 0, clear1 = 0;
                    parse_color(&v->layers[0].stop0, grid0);
                    parse_color(&v->layers[1].stop0, grid1);
                    parse_color(&v->layers[0].stop1, clear0);
                    parse_color(&v->layers[1].stop1, clear1);
                    if ((grid0 & 0xFFu) != 0 &&
                        (grid1 & 0xFFu) != 0 &&
                        (clear0 & 0xFFu) == 0 &&
                        (clear1 & 0xFFu) == 0) {
                        s.animated.background_grid_rgba = grid0;
                    }
                }
            } else if (v->gradient.kind != LXB_CSS_GRADIENT_NONE) {
                apply_gradient(v->gradient);
            } else {
                s.animated.gradient_kind = AnimatedStyle::GradientKind::None;
            }
            break;
        }
        case LXB_CSS_PROPERTY_BACKGROUND_SIZE: {
            const auto* v =
                static_cast<const lxb_css_property_background_size_t*>(d->u.user);
            if (v->layer_count >= 2) {
                int w0 = 0, h0 = 0, w1 = 0, h1 = 0;
                if (plx_lp(&v->layers[0].width, w0) &&
                    plx_lp(&v->layers[0].height, h0) &&
                    plx_lp(&v->layers[1].width, w1) &&
                    plx_lp(&v->layers[1].height, h1) &&
                    w0 > 0 && h0 > 0 && w0 == h0 &&
                    w1 == w0 && h1 == h0) {
                    s.animated.background_grid_size_px =
                        static_cast<std::uint8_t>(std::clamp(w0, 1, 255));
                }
            }
            break;
        }
        case LXB_CSS_PROPERTY_BOX_SHADOW: {
            const auto* v =
                static_cast<const lxb_css_property_box_shadow_t*>(d->u.user);
            s.box_shadows.reset();
            switch (v->type) {
                case LXB_CSS_VALUE_INITIAL:
                case LXB_CSS_VALUE_UNSET:
                case LXB_CSS_VALUE_REVERT:
                case LXB_CSS_BOX_SHADOW_NONE:
                    clear_box_shadow(s.animated);
                    break;
                case LXB_CSS_BOX_SHADOW__LENGTH: {
                    if (v->layer_count == 0) {
                        clear_box_shadow(s.animated);
                        break;
                    }

                    BoxShadowList layers;
                    layers.reserve(v->layer_count);
                    for (std::uint8_t i = 0; i < v->layer_count; ++i) {
                        BoxShadowLayer layer;
                        if (resolve_box_shadow_layer(
                                v->layers[i], s.animated, layer, em_px,
                                viewport_w, viewport_h)) {
                            layers.push_back(layer);
                        }
                    }
                    if (layers.empty()) {
                        clear_box_shadow(s.animated);
                        break;
                    }

                    store_legacy_box_shadow(s.animated, layers.front());
                    if (layers.size() > 1) {
                        s.box_shadows =
                            std::make_shared<BoxShadowList>(std::move(layers));
                    }
                    break;
                }
                default:
                    break;
            }
            break;
        }
        // ── Borders ────────────────────────────────────────────────
        // The four `border-<side>` shorthands and the all-sides
        // `border` shorthand share `lxb_css_property_border_t`
        // (width + style + color). We treat all five as uniform for
        // Phase 2C — per-side variation lands when we split
        // AnimatedStyle / ComputedStyle to hold per-side colors and
        // styles.
        case LXB_CSS_PROPERTY_BORDER:
        case LXB_CSS_PROPERTY_BORDER_TOP:
        case LXB_CSS_PROPERTY_BORDER_RIGHT:
        case LXB_CSS_PROPERTY_BORDER_BOTTOM:
        case LXB_CSS_PROPERTY_BORDER_LEFT: {
            const auto* v =
                static_cast<const lxb_css_property_border_t*>(d->u.user);
            // Width — lexbor's `width` is a length value, same type as
            // padding/margin lengths, so parse_length_px applies.
            int px = 0;
            if (plx_lt(&v->width, px) && px >= 0) {
                const auto w16 = static_cast<std::int16_t>(px);
                if (d->type == LXB_CSS_PROPERTY_BORDER_TOP)    s.computed.border_top    = w16;
                else if (d->type == LXB_CSS_PROPERTY_BORDER_RIGHT)  s.computed.border_right  = w16;
                else if (d->type == LXB_CSS_PROPERTY_BORDER_BOTTOM) s.computed.border_bottom = w16;
                else if (d->type == LXB_CSS_PROPERTY_BORDER_LEFT)   s.computed.border_left   = w16;
                else {
                    s.computed.border_top = s.computed.border_right =
                        s.computed.border_bottom = s.computed.border_left = w16;
                }
            }
            // Style — `border_t::style` is the value-type enum directly
            // (not a nested struct), so compare against the
            // LXB_CSS_BORDER_* constants which alias to LXB_CSS_VALUE_*.
            using BS = ComputedStyle::BorderStyle;
            BS style = BS::None;
            switch (v->style) {
                case LXB_CSS_BORDER_SOLID:  style = BS::Solid;  break;
                case LXB_CSS_BORDER_DASHED: style = BS::Dashed; break;
                case LXB_CSS_BORDER_DOTTED: style = BS::Dotted; break;
                case LXB_CSS_BORDER_DOUBLE: style = BS::Double; break;
                default:                    style = BS::None;   break;
            }
            // Store style uniformly — per-side style variation is a
            // Phase 2C+ feature (see computed_style.h comment).
            // For side-shorthands, we set border_style only if ALL four
            // sides are being set (the `border:` shorthand). Per-side
            // shorthands (border-top:, border-left: etc.) update border_style
            // only if the style value is non-None, so that the last of the
            // two forms wins. This matches CSS cascade: `border: 4px solid`
            // followed by `border-top: 6px dashed` should produce dashed on
            // top and solid on the other three sides — currently we can only
            // approximate by using the last-written style for all sides.
            // Acceptable for Phase 2C; the test only uses mixed colors,
            // not mixed styles.
            if (d->type == LXB_CSS_PROPERTY_BORDER) {
                s.computed.border_style = style;
                s.computed.border_style_sides =
                    (style == BS::None)
                        ? 0
                        : ComputedStyle::BorderAllSides;
            } else if (style != BS::None) {
                // Per-side shorthand: update uniform style if it was None
                // or if the new style overrides it (last shorthand wins).
                s.computed.border_style = style;
            }
            if (d->type != LXB_CSS_PROPERTY_BORDER) {
                std::uint8_t side = 0;
                if (d->type == LXB_CSS_PROPERTY_BORDER_TOP) {
                    side = ComputedStyle::BorderTopSide;
                } else if (d->type == LXB_CSS_PROPERTY_BORDER_RIGHT) {
                    side = ComputedStyle::BorderRightSide;
                } else if (d->type == LXB_CSS_PROPERTY_BORDER_BOTTOM) {
                    side = ComputedStyle::BorderBottomSide;
                } else if (d->type == LXB_CSS_PROPERTY_BORDER_LEFT) {
                    side = ComputedStyle::BorderLeftSide;
                }
                if (side != 0) {
                    if (style == BS::None) {
                        s.computed.border_style_sides &= ~side;
                    } else {
                        s.computed.border_style_sides |= side;
                    }
                }
            }
            // Color — Bootstrap relies on rgba() here for card borders.
            // Use the currentcolor-aware overload so that
            // `border-bottom-color: currentcolor` resolves to the element's
            // foreground colour (CSS `color` property, already resolved above).
            std::uint32_t rgba = 0;
            if (parse_color(&v->color, rgba, s.animated.color_rgba)) {
                if (d->type == LXB_CSS_PROPERTY_BORDER_TOP) {
                    set_border_side_color(s.animated,
                                          AnimatedStyle::BorderTopColorSet,
                                          rgba);
                } else if (d->type == LXB_CSS_PROPERTY_BORDER_RIGHT) {
                    set_border_side_color(s.animated,
                                          AnimatedStyle::BorderRightColorSet,
                                          rgba);
                } else if (d->type == LXB_CSS_PROPERTY_BORDER_BOTTOM) {
                    set_border_side_color(s.animated,
                                          AnimatedStyle::BorderBottomColorSet,
                                          rgba);
                } else if (d->type == LXB_CSS_PROPERTY_BORDER_LEFT) {
                    set_border_side_color(s.animated,
                                          AnimatedStyle::BorderLeftColorSet,
                                          rgba);
                } else {
                    // `border` shorthand: sets uniform color and clears
                    // per-side overrides.
                    s.animated.border_rgba = rgba;
                    clear_border_side_colors(s.animated);
                }
            }
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_COLOR: {
            const auto* v =
                static_cast<const lxb_css_property_border_color_t*>(d->u.user);
            // `border-color: inherit` (Bootstrap's reboot uses this on table
            // cells so they pick up the table's --bs-table-border-color).
            // border-color is normally NON-inherited, so `s = parent` was
            // reset; copy the parent's resolved border colour back here.
            if (v->top.type == LXB_CSS_VALUE_INHERIT && parent) {
                s.animated.border_rgba        = parent->animated.border_rgba;
                s.animated.border_top_rgba    = parent->animated.border_top_rgba;
                s.animated.border_right_rgba  = parent->animated.border_right_rgba;
                s.animated.border_bottom_rgba = parent->animated.border_bottom_rgba;
                s.animated.border_left_rgba   = parent->animated.border_left_rgba;
                s.animated.border_color_set   = parent->animated.border_color_set;
                break;
            }
            std::uint32_t top_rgba = 0;
            std::uint32_t right_rgba = 0;
            std::uint32_t bottom_rgba = 0;
            std::uint32_t left_rgba = 0;
            bool any = false;
            if (parse_color(&v->top, top_rgba, s.animated.color_rgba)) {
                set_border_side_color(s.animated,
                                      AnimatedStyle::BorderTopColorSet,
                                      top_rgba);
                any = true;
            }
            if (parse_color(&v->right, right_rgba, s.animated.color_rgba)) {
                set_border_side_color(s.animated,
                                      AnimatedStyle::BorderRightColorSet,
                                      right_rgba);
                any = true;
            }
            if (parse_color(&v->bottom, bottom_rgba, s.animated.color_rgba)) {
                set_border_side_color(s.animated,
                                      AnimatedStyle::BorderBottomColorSet,
                                      bottom_rgba);
                any = true;
            }
            if (parse_color(&v->left, left_rgba, s.animated.color_rgba)) {
                set_border_side_color(s.animated,
                                      AnimatedStyle::BorderLeftColorSet,
                                      left_rgba);
                any = true;
            }
            if (any && top_rgba == right_rgba && right_rgba == bottom_rgba &&
                bottom_rgba == left_rgba &&
                s.animated.border_color_set == AnimatedStyle::BorderAllColorsSet) {
                s.animated.border_rgba = top_rgba;
                clear_border_side_colors(s.animated);
            }
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_RADIUS: {
            const auto* v =
                static_cast<const lxb_css_property_border_radius_t*>(d->u.user);
            std::int16_t tl = 0;
            std::int16_t tr = 0;
            std::int16_t br = 0;
            std::int16_t bl = 0;
            if (radius(v->top_left, tl)) {
                s.computed.border_radius_top_left = tl;
            }
            if (radius(v->top_right, tr)) {
                s.computed.border_radius_top_right = tr;
            }
            if (radius(v->bottom_right, br)) {
                s.computed.border_radius_bot_right = br;
            }
            if (radius(v->bottom_left, bl)) {
                s.computed.border_radius_bot_left = bl;
            }
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_TOP_LEFT_RADIUS: {
            const auto* v = static_cast<
                const lxb_css_property_border_top_left_radius_t*>(d->u.user);
            std::int16_t r = 0;
            if (radius(*v, r)) {
                s.computed.border_radius_top_left = r;
            }
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_TOP_RIGHT_RADIUS: {
            const auto* v = static_cast<
                const lxb_css_property_border_top_right_radius_t*>(d->u.user);
            std::int16_t r = 0;
            if (radius(*v, r)) {
                s.computed.border_radius_top_right = r;
            }
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_BOTTOM_RIGHT_RADIUS: {
            const auto* v = static_cast<
                const lxb_css_property_border_bottom_right_radius_t*>(
                    d->u.user);
            std::int16_t r = 0;
            if (radius(*v, r)) {
                s.computed.border_radius_bot_right = r;
            }
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_BOTTOM_LEFT_RADIUS: {
            const auto* v = static_cast<
                const lxb_css_property_border_bottom_left_radius_t*>(
                    d->u.user);
            std::int16_t r = 0;
            if (radius(*v, r)) {
                s.computed.border_radius_bot_left = r;
            }
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_TOP_COLOR: {
            const auto* v =
                static_cast<const lxb_css_value_color_t*>(d->u.user);
            std::uint32_t rgba;
            if (parse_color(v, rgba, s.animated.color_rgba))
                set_border_side_color(s.animated,
                                      AnimatedStyle::BorderTopColorSet,
                                      rgba);
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_RIGHT_COLOR: {
            const auto* v =
                static_cast<const lxb_css_value_color_t*>(d->u.user);
            std::uint32_t rgba;
            if (parse_color(v, rgba, s.animated.color_rgba))
                set_border_side_color(s.animated,
                                      AnimatedStyle::BorderRightColorSet,
                                      rgba);
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_BOTTOM_COLOR: {
            const auto* v =
                static_cast<const lxb_css_value_color_t*>(d->u.user);
            std::uint32_t rgba;
            if (parse_color(v, rgba, s.animated.color_rgba))
                set_border_side_color(s.animated,
                                      AnimatedStyle::BorderBottomColorSet,
                                      rgba);
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_LEFT_COLOR: {
            const auto* v =
                static_cast<const lxb_css_value_color_t*>(d->u.user);
            std::uint32_t rgba;
            if (parse_color(v, rgba, s.animated.color_rgba))
                set_border_side_color(s.animated,
                                      AnimatedStyle::BorderLeftColorSet,
                                      rgba);
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_STYLE: {
            // border-style shorthand: sets uniform border_style for all four
            // sides (per-side style variation is a Phase 2C+ feature).
            // The shorthand has top/right/bottom/left, we use `top` to
            // represent the overall intent (solid is the important case).
            const auto* v =
                static_cast<const lxb_css_property_border_style_t*>(d->u.user);
            using BS = ComputedStyle::BorderStyle;
            // Aggregate: if any side is non-None, use that style for all.
            // Most Bootstrap usage sets all sides to the same value.
            auto map_style = [](lxb_css_value_type_t t) -> BS {
                switch (t) {
                    case LXB_CSS_VALUE_SOLID:  return BS::Solid;
                    case LXB_CSS_VALUE_DASHED: return BS::Dashed;
                    case LXB_CSS_VALUE_DOTTED: return BS::Dotted;
                    case LXB_CSS_VALUE_DOUBLE: return BS::Double;
                    default:                   return BS::None;
                }
            };
            BS top    = map_style(v->top);
            BS right  = map_style(v->right);
            BS bottom = map_style(v->bottom);
            BS left   = map_style(v->left);
            // Pick the "strongest" style: prefer Solid > Dashed > Dotted >
            // Double > None. This matches the Bootstrap case where all four
            // sides are set to solid.
            BS best = BS::None;
            std::uint8_t mask = 0;
            if (top    != BS::None) mask |= ComputedStyle::BorderTopSide;
            if (right  != BS::None) mask |= ComputedStyle::BorderRightSide;
            if (bottom != BS::None) mask |= ComputedStyle::BorderBottomSide;
            if (left   != BS::None) mask |= ComputedStyle::BorderLeftSide;
            for (BS b : {top, right, bottom, left}) {
                if (b == BS::Solid)  { best = b; break; }
                if (b != BS::None)     best = b;
            }
            s.computed.border_style = best;
            s.computed.border_style_sides = mask;
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_WIDTH: {
            // border-width shorthand: sets per-side widths.
            const auto* v =
                static_cast<const lxb_css_property_border_width_t*>(d->u.user);
            auto resolve_width = [&](const lxb_css_value_length_type_t& lt) -> int {
                int px = 0;
                if (plx_lt(&lt, px)) return px >= 0 ? px : 0;
                // thin/medium/thick keywords
                switch (lt.type) {
                    case LXB_CSS_VALUE_THIN:   return 1;
                    case LXB_CSS_VALUE_MEDIUM: return 3;
                    case LXB_CSS_VALUE_THICK:  return 5;
                    default:                   return 0;
                }
            };
            s.computed.border_top    = static_cast<std::int16_t>(resolve_width(v->top));
            s.computed.border_right  = static_cast<std::int16_t>(resolve_width(v->right));
            s.computed.border_bottom = static_cast<std::int16_t>(resolve_width(v->bottom));
            s.computed.border_left   = static_cast<std::int16_t>(resolve_width(v->left));
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_TOP_WIDTH: {
            const auto* v =
                static_cast<const lxb_css_property_border_top_width_t*>(d->u.user);
            int px = 0;
            if (plx_lt(v, px) && px >= 0)
                s.computed.border_top = static_cast<std::int16_t>(px);
            else switch (v->type) {
                case LXB_CSS_VALUE_THIN:   s.computed.border_top = 1; break;
                case LXB_CSS_VALUE_MEDIUM: s.computed.border_top = 3; break;
                case LXB_CSS_VALUE_THICK:  s.computed.border_top = 5; break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_RIGHT_WIDTH: {
            const auto* v =
                static_cast<const lxb_css_property_border_right_width_t*>(d->u.user);
            int px = 0;
            if (plx_lt(v, px) && px >= 0)
                s.computed.border_right = static_cast<std::int16_t>(px);
            else switch (v->type) {
                case LXB_CSS_VALUE_THIN:   s.computed.border_right = 1; break;
                case LXB_CSS_VALUE_MEDIUM: s.computed.border_right = 3; break;
                case LXB_CSS_VALUE_THICK:  s.computed.border_right = 5; break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_BOTTOM_WIDTH: {
            const auto* v =
                static_cast<const lxb_css_property_border_bottom_width_t*>(d->u.user);
            int px = 0;
            if (plx_lt(v, px) && px >= 0)
                s.computed.border_bottom = static_cast<std::int16_t>(px);
            else switch (v->type) {
                case LXB_CSS_VALUE_THIN:   s.computed.border_bottom = 1; break;
                case LXB_CSS_VALUE_MEDIUM: s.computed.border_bottom = 3; break;
                case LXB_CSS_VALUE_THICK:  s.computed.border_bottom = 5; break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_LEFT_WIDTH: {
            const auto* v =
                static_cast<const lxb_css_property_border_left_width_t*>(d->u.user);
            int px = 0;
            if (plx_lt(v, px) && px >= 0)
                s.computed.border_left = static_cast<std::int16_t>(px);
            else switch (v->type) {
                case LXB_CSS_VALUE_THIN:   s.computed.border_left = 1; break;
                case LXB_CSS_VALUE_MEDIUM: s.computed.border_left = 3; break;
                case LXB_CSS_VALUE_THICK:  s.computed.border_left = 5; break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_BORDER_COLLAPSE: {
            // border-collapse is an INHERITED property. Setting it on a table
            // element causes all descendant tr/td/th to inherit `true`, which
            // signals the layout engine to treat cells as having no border
            // contribution to box size (approximating the collapsed model).
            const auto type = *static_cast<const lxb_css_property_border_collapse_t*>(d->u.user);
            s.computed.border_collapse = (type == LXB_CSS_VALUE_COLLAPSE);
            break;
        }
        case LXB_CSS_PROPERTY_GAP: {
            const auto* v =
                static_cast<const lxb_css_property_gap_t*>(d->u.user);
            int row = 0;
            int column = 0;
            if (plx_lp(&v->row, row)) {
                s.computed.row_gap = static_cast<std::int16_t>(row);
            }
            if (plx_lp(&v->column, column)) {
                s.computed.column_gap = static_cast<std::int16_t>(column);
            }
            break;
        }
        case LXB_CSS_PROPERTY_ROW_GAP: {
            const auto* v =
                static_cast<const lxb_css_property_row_gap_t*>(d->u.user);
            int px = 0;
            if (plx_lp(v, px)) {
                s.computed.row_gap = static_cast<std::int16_t>(px);
            }
            break;
        }
        case LXB_CSS_PROPERTY_COLUMN_GAP: {
            const auto* v =
                static_cast<const lxb_css_property_column_gap_t*>(d->u.user);
            int px = 0;
            if (plx_lp(v, px)) {
                s.computed.column_gap = static_cast<std::int16_t>(px);
            }
            break;
        }
        // ── Box model: padding / margin ────────────────────────────
        // The `padding` and `margin` shorthands deliver a struct with
        // four sides; longhands deliver just the side's length-
        // percentage value. Two cascade arms each. All four parse
        // through the inner-direct overload of parse_length_px.
        case LXB_CSS_PROPERTY_PADDING: {
            // CSS shorthand expansion rules (which lexbor 2.4 does NOT
            // apply for us — it just sets the sides the author wrote
            // and leaves the rest typed as "unset"):
            //   1 value  →  T=R=B=L
            //   2 values →  T=B=v1, R=L=v2
            //   3 values →  T=v1, R=L=v2, B=v3
            //   4 values →  T=v1, R=v2, B=v3, L=v4
            const auto* v =
                static_cast<const lxb_css_property_padding_t*>(d->u.user);
            int t = 0, r = 0, bp = 0, l = 0;
            const bool ok_t = plx_lp(&v->top,    t);
            const bool ok_r = plx_lp(&v->right,  r);
            const bool ok_b = plx_lp(&v->bottom, bp);
            const bool ok_l = plx_lp(&v->left,   l);

            // Mirror per the shorthand rules: a side that lexbor left
            // unset inherits from its CSS-shorthand peer.
            const int T = ok_t ? t : 0;
            const int R = ok_r ? r : T;
            const int B = ok_b ? bp : T;
            const int L = ok_l ? l : R;
            if (T >= 0) s.computed.padding_top    = static_cast<std::int16_t>(T);
            if (R >= 0) s.computed.padding_right  = static_cast<std::int16_t>(R);
            if (B >= 0) s.computed.padding_bottom = static_cast<std::int16_t>(B);
            if (L >= 0) s.computed.padding_left   = static_cast<std::int16_t>(L);
            break;
        }
        case LXB_CSS_PROPERTY_PADDING_TOP: {
            const auto* v = static_cast<const lxb_css_property_padding_top_t*>(d->u.user);
            int px = 0;
            if (plx_lp(v, px) && px >= 0)
                s.computed.padding_top = static_cast<std::int16_t>(px);
            break;
        }
        case LXB_CSS_PROPERTY_PADDING_RIGHT: {
            const auto* v = static_cast<const lxb_css_property_padding_right_t*>(d->u.user);
            int px = 0;
            if (plx_lp(v, px) && px >= 0)
                s.computed.padding_right = static_cast<std::int16_t>(px);
            break;
        }
        case LXB_CSS_PROPERTY_PADDING_BOTTOM: {
            const auto* v = static_cast<const lxb_css_property_padding_bottom_t*>(d->u.user);
            int px = 0;
            if (plx_lp(v, px) && px >= 0)
                s.computed.padding_bottom = static_cast<std::int16_t>(px);
            break;
        }
        case LXB_CSS_PROPERTY_PADDING_LEFT: {
            const auto* v = static_cast<const lxb_css_property_padding_left_t*>(d->u.user);
            int px = 0;
            if (plx_lp(v, px) && px >= 0)
                s.computed.padding_left = static_cast<std::int16_t>(px);
            break;
        }
        case LXB_CSS_PROPERTY_MARGIN: {
            // Same shorthand-mirror dance as padding above. Lexbor 2.4
            // sets only the sides the author wrote; we expand to 4.
            const auto* v =
                static_cast<const lxb_css_property_margin_t*>(d->u.user);
            int t = 0, r = 0, bp = 0, l = 0;
            const bool ok_t = plx_lp(&v->top,    t);
            const bool ok_r = plx_lp(&v->right,  r);
            const bool ok_b = plx_lp(&v->bottom, bp);
            const bool ok_l = plx_lp(&v->left,   l);
            const int T = ok_t ? t : 0;
            const int R = ok_r ? r : T;
            const int B = ok_b ? bp : T;
            const int L = ok_l ? l : R;
            s.computed.margin_top    = static_cast<std::int16_t>(T);
            s.computed.margin_right  = static_cast<std::int16_t>(R);
            s.computed.margin_bottom = static_cast<std::int16_t>(B);
            s.computed.margin_left   = static_cast<std::int16_t>(L);
            const auto top_type = v->top.type;
            const auto right_type =
                v->right.type != LXB_CSS_VALUE__UNDEF ? v->right.type : top_type;
            const auto left_type =
                v->left.type != LXB_CSS_VALUE__UNDEF ? v->left.type : right_type;
            // Detect horizontal `auto` after CSS shorthand mirroring. For
            // `margin: 0 auto`, lexbor only records the second token on the
            // right side; CSS also applies it to the left side.
            s.computed.margin_auto.right =
                right_type == LXB_CSS_VALUE_AUTO ? 1 : 0;
            s.computed.margin_auto.left =
                left_type == LXB_CSS_VALUE_AUTO ? 1 : 0;
            break;
        }
        case LXB_CSS_PROPERTY_MARGIN_TOP: {
            const auto* v = static_cast<const lxb_css_property_margin_top_t*>(d->u.user);
            int px = 0;
            if (plx_lp(v, px)) s.computed.margin_top = static_cast<std::int16_t>(px);
            break;
        }
        case LXB_CSS_PROPERTY_MARGIN_RIGHT: {
            const auto* v = static_cast<const lxb_css_property_margin_right_t*>(d->u.user);
            int px = 0;
            if (plx_lp(v, px)) {
                s.computed.margin_right = static_cast<std::int16_t>(px);
                s.computed.margin_auto.right = 0;
            } else if (v->type == LXB_CSS_VALUE_AUTO) {
                s.computed.margin_auto.right = 1;
            }
            break;
        }
        case LXB_CSS_PROPERTY_MARGIN_BOTTOM: {
            const auto* v = static_cast<const lxb_css_property_margin_bottom_t*>(d->u.user);
            int px = 0;
            if (plx_lp(v, px)) s.computed.margin_bottom = static_cast<std::int16_t>(px);
            break;
        }
        case LXB_CSS_PROPERTY_MARGIN_LEFT: {
            const auto* v = static_cast<const lxb_css_property_margin_left_t*>(d->u.user);
            int px = 0;
            if (plx_lp(v, px)) {
                s.computed.margin_left = static_cast<std::int16_t>(px);
                s.computed.margin_auto.left = 0;
            } else if (v->type == LXB_CSS_VALUE_AUTO) {
                s.computed.margin_auto.left = 1;
            }
            break;
        }
        // ── Positioned layout ──────────────────────────────────────
        // `position` selects the layout scheme; `top/right/bottom/left`
        // give the insets the scheme applies. lexbor parses all five;
        // we map them onto ComputedStyle and the Yoga adapter turns
        // them into YGPositionType + per-edge positions. Insets are
        // length-percentage-auto — parse_length_px returns false for
        // `auto`, so an unspecified/auto side leaves its presence bit
        // clear and the adapter skips it (Yoga then treats it as
        // undefined, the correct CSS behaviour).
        case LXB_CSS_PROPERTY_POSITION: {
            const auto* v =
                static_cast<const lxb_css_property_position_t*>(d->u.user);
            using P = ComputedStyle::Position;
            switch (v->type) {
                case LXB_CSS_POSITION_STATIC:   s.computed.position = P::Static;   break;
                case LXB_CSS_POSITION_RELATIVE: s.computed.position = P::Relative; break;
                case LXB_CSS_POSITION_ABSOLUTE: s.computed.position = P::Absolute; break;
                case LXB_CSS_POSITION_FIXED:    s.computed.position = P::Fixed;    break;
                // `sticky` has no Yoga equivalent — fall back to the
                // closest in-flow scheme (relative) rather than break
                // layout. Documented gap; revisit if a test needs it.
                case LXB_CSS_POSITION_STICKY:   s.computed.position = P::Relative; break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_Z_INDEX: {
            const auto* v =
                static_cast<const lxb_css_property_z_index_t*>(d->u.user);
            if (v->type == LXB_CSS_Z_INDEX__INTEGER) {
                const long raw = std::clamp(
                    v->integer.num,
                    static_cast<long>(std::numeric_limits<std::int16_t>::min()),
                    static_cast<long>(std::numeric_limits<std::int16_t>::max()));
                s.computed.z_index_low =
                    static_cast<std::int16_t>(raw);
            } else {
                s.computed.z_index_low = 0;
            }
            s.computed.z_index_high = 0;
            break;
        }
        case LXB_CSS_PROPERTY_FLOAT: {
            const auto* v =
                static_cast<const lxb_css_property_float_t*>(d->u.user);
            using F = ComputedStyle::Float;
            switch (v->type) {
                case LXB_CSS_FLOAT_LEFT:
                case LXB_CSS_FLOAT_INLINE_START:
                case LXB_CSS_FLOAT_START:
                    s.computed.css_float = F::Left;
                    break;
                case LXB_CSS_FLOAT_RIGHT:
                case LXB_CSS_FLOAT_INLINE_END:
                case LXB_CSS_FLOAT_END:
                    s.computed.css_float = F::Right;
                    break;
                case LXB_CSS_FLOAT_NONE:
                default:
                    s.computed.css_float = F::None;
                    break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_INSET: {
            const auto* v = static_cast<const lxb_css_property_inset_t*>(d->u.user);
            const auto* top = &v->top;
            const auto* right =
                v->right.type != LXB_CSS_VALUE__UNDEF ? &v->right : top;
            const auto* bottom =
                v->bottom.type != LXB_CSS_VALUE__UNDEF ? &v->bottom : top;
            const auto* left =
                v->left.type != LXB_CSS_VALUE__UNDEF ? &v->left : right;
            apply_inset_value(*top,    s.computed, InsetEdge::Top,    em_px,
                              viewport_w, viewport_h);
            apply_inset_value(*right,  s.computed, InsetEdge::Right,  em_px,
                              viewport_w, viewport_h);
            apply_inset_value(*bottom, s.computed, InsetEdge::Bottom, em_px,
                              viewport_w, viewport_h);
            apply_inset_value(*left,   s.computed, InsetEdge::Left,   em_px,
                              viewport_w, viewport_h);
            break;
        }
        case LXB_CSS_PROPERTY_TOP: {
            const auto* v = static_cast<const lxb_css_property_top_t*>(d->u.user);
            apply_inset_value(*v, s.computed, InsetEdge::Top, em_px,
                              viewport_w, viewport_h);
            break;
        }
        case LXB_CSS_PROPERTY_RIGHT: {
            const auto* v = static_cast<const lxb_css_property_right_t*>(d->u.user);
            apply_inset_value(*v, s.computed, InsetEdge::Right, em_px,
                              viewport_w, viewport_h);
            break;
        }
        case LXB_CSS_PROPERTY_BOTTOM: {
            const auto* v = static_cast<const lxb_css_property_bottom_t*>(d->u.user);
            apply_inset_value(*v, s.computed, InsetEdge::Bottom, em_px,
                              viewport_w, viewport_h);
            break;
        }
        case LXB_CSS_PROPERTY_LEFT: {
            const auto* v = static_cast<const lxb_css_property_left_t*>(d->u.user);
            apply_inset_value(*v, s.computed, InsetEdge::Left, em_px,
                              viewport_w, viewport_h);
            break;
        }

        // ── Box sizing ──────────────────────────────────────────────
        // Selects whether width/height measure the content box (CSS
        // default) or the border box. lexbor parses it; we map onto
        // ComputedStyle and the Yoga adapter forwards it to YGBoxSizing.
        case LXB_CSS_PROPERTY_BOX_SIZING: {
            const auto* v =
                static_cast<const lxb_css_property_box_sizing_t*>(d->u.user);
            using BX = ComputedStyle::BoxSizing;
            switch (v->type) {
                case LXB_CSS_BOX_SIZING_BORDER_BOX:
                    s.computed.box_sizing = BX::BorderBox;  break;
                case LXB_CSS_BOX_SIZING_CONTENT_BOX:
                    s.computed.box_sizing = BX::ContentBox; break;
                default: break;
            }
            break;
        }

        // ── Flex container properties ──────────────────────────────
        case LXB_CSS_PROPERTY_VERTICAL_ALIGN: {
            const auto* v =
                static_cast<const lxb_css_property_vertical_align_t*>(d->u.user);
            using VA = ComputedStyle::VerticalAlign;

            auto map_alignment = [&](unsigned int type) {
                switch (type) {
                    case LXB_CSS_ALIGNMENT_BASELINE_BASELINE:
                    case LXB_CSS_VALUE_INITIAL:
                    case LXB_CSS_VALUE_UNSET:
                        s.computed.vertical_align = VA::Baseline;   break;
                    case LXB_CSS_ALIGNMENT_BASELINE_MIDDLE:
                    case LXB_CSS_ALIGNMENT_BASELINE_CENTRAL:
                        s.computed.vertical_align = VA::Middle;     break;
                    case LXB_CSS_ALIGNMENT_BASELINE_TEXT_TOP:
                        s.computed.vertical_align = VA::TextTop;    break;
                    case LXB_CSS_ALIGNMENT_BASELINE_TEXT_BOTTOM:
                        s.computed.vertical_align = VA::TextBottom; break;
                    default:
                        return false;
                }
                return true;
            };

            map_alignment(v->type);
            map_alignment(v->alignment.type);
            switch (v->shift.type) {
                case LXB_CSS_BASELINE_SHIFT_TOP:
                    s.computed.vertical_align = VA::Top;    break;
                case LXB_CSS_BASELINE_SHIFT_CENTER:
                    s.computed.vertical_align = VA::Middle; break;
                case LXB_CSS_BASELINE_SHIFT_BOTTOM:
                    s.computed.vertical_align = VA::Bottom; break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_DISPLAY: {
            const auto* v =
                static_cast<const lxb_css_property_display_t*>(d->u.user);
            // Phase 2D: only the values our engine understands.
            // Unknown values fall through to "block" (the default).
            switch (v->a) {
                case LXB_CSS_DISPLAY_FLEX:
                    s.computed.display = ComputedStyle::Display::Flex; break;
                case LXB_CSS_DISPLAY_INLINE_FLEX:
                    s.computed.display = ComputedStyle::Display::InlineFlex; break;
                case LXB_CSS_DISPLAY_GRID:
                    s.computed.display = ComputedStyle::Display::Grid; break;
                case LXB_CSS_DISPLAY_INLINE_GRID:
                    s.computed.display = ComputedStyle::Display::InlineGrid; break;
                case LXB_CSS_DISPLAY_BLOCK:
                    s.computed.display = ComputedStyle::Display::Block; break;
                case LXB_CSS_DISPLAY_INLINE:
                    s.computed.display = ComputedStyle::Display::Inline; break;
                case LXB_CSS_DISPLAY_INLINE_BLOCK:
                    s.computed.display = ComputedStyle::Display::InlineBlock; break;
                case LXB_CSS_DISPLAY_NONE:
                    s.computed.display = ComputedStyle::Display::None; break;
                case LXB_CSS_DISPLAY_LIST_ITEM:
                    s.computed.display = ComputedStyle::Display::ListItem; break;
                // CSS table model.
                case LXB_CSS_DISPLAY_TABLE:
                    s.computed.display = ComputedStyle::Display::Table; break;
                case LXB_CSS_DISPLAY_TABLE_ROW:
                    s.computed.display = ComputedStyle::Display::TableRow; break;
                case LXB_CSS_DISPLAY_TABLE_CELL:
                    s.computed.display = ComputedStyle::Display::TableCell; break;
                case LXB_CSS_DISPLAY_TABLE_ROW_GROUP:
                case LXB_CSS_DISPLAY_TABLE_HEADER_GROUP:
                case LXB_CSS_DISPLAY_TABLE_FOOTER_GROUP:
                    s.computed.display = ComputedStyle::Display::TableRowGroup; break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_LIST_STYLE_TYPE:
        case LXB_CSS_PROPERTY_LIST_STYLE: {
            const auto* v =
                static_cast<const lxb_css_property_list_style_type_t*>(d->u.user);
            using LT = ComputedStyle::ListStyleType;
            switch (v->type) {
                case LXB_CSS_LIST_STYLE_TYPE_DISC:
                    s.computed.list_style_type = LT::Disc; break;
                case LXB_CSS_LIST_STYLE_TYPE_CIRCLE:
                    s.computed.list_style_type = LT::Circle; break;
                case LXB_CSS_LIST_STYLE_TYPE_SQUARE:
                    s.computed.list_style_type = LT::Square; break;
                case LXB_CSS_LIST_STYLE_TYPE_DECIMAL:
                    s.computed.list_style_type = LT::Decimal; break;
                case LXB_CSS_LIST_STYLE_TYPE_NONE:
                    s.computed.list_style_type = LT::None; break;
                case LXB_CSS_VALUE_INITIAL:
                case LXB_CSS_VALUE_UNSET:
                case LXB_CSS_VALUE_REVERT:
                    s.computed.list_style_type = LT::Disc; break;
                case LXB_CSS_VALUE_INHERIT:
                default:
                    break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_FLEX_DIRECTION: {
            const auto* v =
                static_cast<const lxb_css_property_flex_direction_t*>(d->u.user);
            using FD = ComputedStyle::FlexDirection;
            switch (v->type) {
                case LXB_CSS_FLEX_DIRECTION_ROW:            s.computed.flex_direction = FD::Row;           break;
                case LXB_CSS_FLEX_DIRECTION_ROW_REVERSE:    s.computed.flex_direction = FD::RowReverse;    break;
                case LXB_CSS_FLEX_DIRECTION_COLUMN:         s.computed.flex_direction = FD::Column;        break;
                case LXB_CSS_FLEX_DIRECTION_COLUMN_REVERSE: s.computed.flex_direction = FD::ColumnReverse; break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_FLEX_WRAP: {
            const auto* v =
                static_cast<const lxb_css_property_flex_wrap_t*>(d->u.user);
            using FW = ComputedStyle::FlexWrap;
            switch (v->type) {
                case LXB_CSS_FLEX_WRAP_NOWRAP:       s.computed.flex_wrap = FW::NoWrap;      break;
                case LXB_CSS_FLEX_WRAP_WRAP:         s.computed.flex_wrap = FW::Wrap;        break;
                case LXB_CSS_FLEX_WRAP_WRAP_REVERSE: s.computed.flex_wrap = FW::WrapReverse; break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_JUSTIFY_CONTENT: {
            const auto* v =
                static_cast<const lxb_css_property_justify_content_t*>(d->u.user);
            using JC = ComputedStyle::JustifyContent;
            switch (v->type) {
                case LXB_CSS_VALUE_START:
                case LXB_CSS_JUSTIFY_CONTENT_FLEX_START:    s.computed.justify_content = JC::Start;        break;
                case LXB_CSS_VALUE_END:
                case LXB_CSS_JUSTIFY_CONTENT_FLEX_END:      s.computed.justify_content = JC::End;          break;
                case LXB_CSS_JUSTIFY_CONTENT_CENTER:        s.computed.justify_content = JC::Center;       break;
                case LXB_CSS_JUSTIFY_CONTENT_SPACE_BETWEEN: s.computed.justify_content = JC::SpaceBetween; break;
                case LXB_CSS_JUSTIFY_CONTENT_SPACE_AROUND:  s.computed.justify_content = JC::SpaceAround;  break;
                case LXB_CSS_JUSTIFY_CONTENT_SPACE_EVENLY:  s.computed.justify_content = JC::SpaceEvenly;  break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_ALIGN_ITEMS: {
            const auto* v =
                static_cast<const lxb_css_property_align_items_t*>(d->u.user);
            using AI = ComputedStyle::AlignItems;
            switch (v->type) {
                case LXB_CSS_ALIGN_ITEMS_STRETCH:    s.computed.align_items = AI::Stretch;  break;
                case LXB_CSS_VALUE_START:
                case LXB_CSS_ALIGN_ITEMS_FLEX_START: s.computed.align_items = AI::Start;    break;
                case LXB_CSS_VALUE_END:
                case LXB_CSS_ALIGN_ITEMS_FLEX_END:   s.computed.align_items = AI::End;      break;
                case LXB_CSS_ALIGN_ITEMS_CENTER:     s.computed.align_items = AI::Center;   break;
                case LXB_CSS_ALIGN_ITEMS_BASELINE:   s.computed.align_items = AI::Baseline; break;
                default: break;
            }
            break;
        }

        // ── Flex item properties ───────────────────────────────────
        case LXB_CSS_PROPERTY_FLEX_GROW: {
            const auto* v =
                static_cast<const lxb_css_value_number_type_t*>(d->u.user);
            const auto n = static_cast<int>(v->number.num + 0.5);
            s.computed.flex_grow = static_cast<std::uint8_t>(std::clamp(n, 0, 255));
            break;
        }
        case LXB_CSS_PROPERTY_FLEX_SHRINK: {
            const auto* v =
                static_cast<const lxb_css_value_number_type_t*>(d->u.user);
            const auto n = static_cast<int>(v->number.num + 0.5);
            s.computed.flex_shrink = static_cast<std::uint8_t>(std::clamp(n, 0, 255));
            break;
        }
        case LXB_CSS_PROPERTY_FLEX_BASIS: {
            const auto* v =
                static_cast<const lxb_css_property_flex_basis_t*>(d->u.user);
            afb(*v, s);
            break;
        }
        case LXB_CSS_PROPERTY_FLEX: {
            const auto* v =
                static_cast<const lxb_css_property_flex_t*>(d->u.user);
            if (v->type == LXB_CSS_FLEX_NONE) {
                s.computed.flex_grow = 0;
                s.computed.flex_shrink = 0;
                s.computed.flex_basis = -1;
                break;
            }
            if (v->grow.type == LXB_CSS_FLEX_GROW__NUMBER) {
                const auto n = static_cast<int>(v->grow.number.num + 0.5);
                s.computed.flex_grow =
                    static_cast<std::uint8_t>(std::clamp(n, 0, 255));
            }
            if (v->shrink.type == LXB_CSS_FLEX_SHRINK__NUMBER) {
                const auto n = static_cast<int>(v->shrink.number.num + 0.5);
                s.computed.flex_shrink =
                    static_cast<std::uint8_t>(std::clamp(n, 0, 255));
            }
            if (v->basis.type != LXB_CSS_VALUE__UNDEF) {
                afb(v->basis, s);
            }
            break;
        }

        // ── Layout-affecting ───────────────────────────────────────

        // font shorthand: sets font-size, line-height, and optionally
        // font-weight/font-style from a single declaration like
        // `font: bold 32px/1.5 sans-serif`.
        case LXB_CSS_PROPERTY_FONT: {
            const auto* v =
                static_cast<const lxb_css_property_font_t*>(d->u.user);
            if (!v) break;

            if (v->type == LXB_CSS_VALUE_INHERIT ||
                v->type == LXB_CSS_VALUE_UNSET) {
                if (parent) {
                    if (apply_font_size) {
                        s.computed.font_size_px =
                            parent->computed.font_size_px;
                    }
                    s.computed.line_height_x100 =
                        parent->computed.line_height_x100;
                    s.computed.font_weight = parent->computed.font_weight;
                    s.computed.font_style  = parent->computed.font_style;
                    s.computed.font_id     = parent->computed.font_id;
                }
                break;
            }

            if (v->type == LXB_CSS_VALUE_INITIAL) {
                if (apply_font_size) s.computed.font_size_px = 16;
                s.computed.line_height_x100 = 0;
                s.computed.font_weight = 400;
                s.computed.font_style  = 0;
                s.computed.font_id     = 0;
                break;
            }

            if (v->type != LXB_CSS_FONT__DETAIL) break;

            // font-size: em_px here is the PARENT's font-size (caller
            // invokes apply_declaration with parent_em for FONT/FONT_SIZE).
            if (apply_font_size) {
                int px = 0;
                if (plx_lpt(&v->size, px) && px > 0)
                    s.computed.font_size_px = static_cast<std::uint16_t>(px);
            }

            // line-height (same logic as LXB_CSS_PROPERTY_LINE_HEIGHT)
            {
                const auto* lh = &v->line_height;
                switch (lh->type) {
                    case LXB_CSS_LINE_HEIGHT_NORMAL:
                        s.computed.line_height_x100 = 0;
                        break;
                    case LXB_CSS_LINE_HEIGHT__NUMBER: {
                        const auto m = lh->u.number.num * 100.0;
                        s.computed.line_height_x100 =
                            static_cast<std::int16_t>(std::clamp(m, 0.0, 32760.0));
                        break;
                    }
                    case LXB_CSS_LINE_HEIGHT__PERCENTAGE: {
                        const auto m = lh->u.percentage.num;
                        s.computed.line_height_x100 =
                            static_cast<std::int16_t>(std::clamp(m, 0.0, 32760.0));
                        break;
                    }
                    case LXB_CSS_LINE_HEIGHT__LENGTH: {
                        int px = 0;
                        plv(lh->u.length.num,
                            static_cast<int>(lh->u.length.unit),
                            px);
                        if (px > 0)
                            s.computed.line_height_x100 =
                                static_cast<std::int16_t>(-px);
                        break;
                    }
                    default: break;
                }
            }

            // font-weight
            {
                const auto* fw = &v->weight;
                int w = 0;
                switch (fw->type) {
                    case LXB_CSS_FONT_WEIGHT__NUMBER:
                        w = static_cast<int>(fw->number.num + 0.5);
                        break;
                    case LXB_CSS_FONT_WEIGHT_NORMAL:  w = 400; break;
                    case LXB_CSS_FONT_WEIGHT_BOLD:    w = 700; break;
                    case LXB_CSS_FONT_WEIGHT_BOLDER:  w = 700; break;
                    case LXB_CSS_FONT_WEIGHT_LIGHTER: w = 300; break;
                    default: break;
                }
                if (w > 0)
                    s.computed.font_weight =
                        static_cast<std::uint16_t>(std::clamp(w, 1, 999));
            }

            // font-style
            {
                const auto* fs = &v->style;
                switch (fs->type) {
                    case LXB_CSS_FONT_STYLE_NORMAL:  s.computed.font_style = 0; break;
                    case LXB_CSS_FONT_STYLE_ITALIC:  s.computed.font_style = 1; break;
                    case LXB_CSS_FONT_STYLE_OBLIQUE: s.computed.font_style = 2; break;
                    default: break;
                }
            }

            break;
        }

        case LXB_CSS_PROPERTY_FONT_SIZE: {
            // em_px here is the PARENT's font-size (caller invokes
            // apply_declaration with parent_em for FONT_SIZE).
            const auto* v =
                static_cast<const lxb_css_property_font_size_t*>(d->u.user);
            int px = 0;
            if (plx_lpt(v, px) && px > 0)
                s.computed.font_size_px = static_cast<std::uint16_t>(px);
            break;
        }
        case LXB_CSS_PROPERTY_LINE_HEIGHT: {
            const auto* v =
                static_cast<const lxb_css_property_line_height_t*>(d->u.user);
            switch (v->type) {
                case LXB_CSS_LINE_HEIGHT_NORMAL:
                    // CSS "normal" is font-dependent (~1.2 typical).
                    // Leave at 0 = unset; paint applies the default.
                    break;
                case LXB_CSS_LINE_HEIGHT__NUMBER: {
                    const auto m = v->u.number.num * 100.0;
                    s.computed.line_height_x100 =
                        static_cast<std::int16_t>(std::clamp(m, 0.0, 32760.0));
                    break;
                }
                case LXB_CSS_LINE_HEIGHT__PERCENTAGE: {
                    // 150% → multiplier 1.5 → x100 = 150.
                    const auto m = v->u.percentage.num;
                    s.computed.line_height_x100 =
                        static_cast<std::int16_t>(std::clamp(m, 0.0, 32760.0));
                    break;
                }
                case LXB_CSS_LINE_HEIGHT__LENGTH: {
                    // line-height: <px>. Stored as a negative
                    // sentinel so the paint side can tell "absolute"
                    // apart from "multiplier."
                    int px = 0;
                    // For length type, the value lives in v->u.length.
                    plv(v->u.length.num,
                        static_cast<int>(v->u.length.unit),
                        px);
                    if (px > 0)
                        s.computed.line_height_x100 =
                            static_cast<std::int16_t>(-px);
                    break;
                }
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_FONT_STYLE: {
            const auto* v =
                static_cast<const lxb_css_property_font_style_t*>(d->u.user);
            switch (v->type) {
                case LXB_CSS_FONT_STYLE_NORMAL:  s.computed.font_style = 0; break;
                case LXB_CSS_FONT_STYLE_ITALIC:  s.computed.font_style = 1; break;
                case LXB_CSS_FONT_STYLE_OBLIQUE: s.computed.font_style = 2; break;
                default: break;
            }
            s.computed.has.font_style = 1;
            break;
        }
        case LXB_CSS_PROPERTY_FONT_WEIGHT: {
            const auto* v =
                static_cast<const lxb_css_property_font_weight_t*>(d->u.user);
            int w = 0;
            switch (v->type) {
                case LXB_CSS_FONT_WEIGHT__NUMBER:
                    w = static_cast<int>(v->number.num + 0.5);
                    break;
                case LXB_CSS_FONT_WEIGHT_NORMAL:  w = 400; break;
                case LXB_CSS_FONT_WEIGHT_BOLD:    w = 700; break;
                case LXB_CSS_FONT_WEIGHT_BOLDER:  w = 700; break;  // approx
                case LXB_CSS_FONT_WEIGHT_LIGHTER: w = 300; break;  // approx
                default: break;
            }
            if (w > 0)
                s.computed.font_weight =
                    static_cast<std::uint16_t>(std::clamp(w, 1, 999));
            s.computed.has.font_weight = 1;
            break;
        }
        // ── Text features ──────────────────────────────────────────
        case LXB_CSS_PROPERTY_LETTER_SPACING: {
            const auto* v =
                static_cast<const lxb_css_property_letter_spacing_t*>(d->u.user);
            if (v->type == LXB_CSS_VALUE_NORMAL) {
                s.computed.letter_spacing_x100 = 0;
            } else if (v->type == LXB_CSS_VALUE__LENGTH) {
                int px_x100 = 0;
                if (plv_x100(v->length.num,
                             static_cast<int>(v->length.unit), px_x100)) {
                    s.computed.letter_spacing_x100 =
                        static_cast<std::int16_t>(px_x100);
                }
            }
            break;
        }
        case LXB_CSS_PROPERTY_TEXT_INDENT: {
            const auto* v =
                static_cast<const lxb_css_property_text_indent_t*>(d->u.user);
            switch (v->type) {
                case LXB_CSS_VALUE_INITIAL:
                case LXB_CSS_VALUE_UNSET:
                case LXB_CSS_VALUE_REVERT:
                    s.computed.text_indent_value = 0;
                    s.computed.text_indent_is_pct = 0;
                    break;
                case LXB_CSS_TEXT_INDENT__LENGTH: {
                    int px = 0;
                    if (plx_lp(&v->length, px)) {
                        s.computed.text_indent_value =
                            static_cast<std::int16_t>(
                                std::clamp(px, -32768, 32767));
                        s.computed.text_indent_is_pct = 0;
                    }
                    break;
                }
                case LXB_CSS_TEXT_INDENT__PERCENTAGE: {
                    const int pct_x100 = static_cast<int>(
                        std::lround(v->length.u.percentage.num * 100.0));
                    s.computed.text_indent_value =
                        static_cast<std::int16_t>(
                            std::clamp(pct_x100, -32768, 32767));
                    s.computed.text_indent_is_pct = 1;
                    break;
                }
                case LXB_CSS_VALUE_INHERIT:
                default:
                    break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_WHITE_SPACE: {
            const auto* v =
                static_cast<const lxb_css_property_white_space_t*>(d->u.user);
            using WS = ComputedStyle::WhiteSpace;
            switch (v->type) {
                case LXB_CSS_WHITE_SPACE_NORMAL:
                    s.computed.white_space = WS::Normal;   break;
                case LXB_CSS_WHITE_SPACE_PRE:
                    s.computed.white_space = WS::Pre;      break;
                case LXB_CSS_WHITE_SPACE_NOWRAP:
                    s.computed.white_space = WS::Nowrap;   break;
                case LXB_CSS_WHITE_SPACE_PRE_WRAP:
                    s.computed.white_space = WS::PreWrap;  break;
                case LXB_CSS_WHITE_SPACE_PRE_LINE:
                    s.computed.white_space = WS::PreLine;  break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_TEXT_TRANSFORM: {
            const auto* v =
                static_cast<const lxb_css_property_text_transform_t*>(d->u.user);
            using TT = ComputedStyle::TextTransform;
            switch (v->type_case) {
                case LXB_CSS_TEXT_TRANSFORM_NONE:
                    s.computed.text_transform = TT::None;       break;
                case LXB_CSS_TEXT_TRANSFORM_UPPERCASE:
                    s.computed.text_transform = TT::Uppercase;  break;
                case LXB_CSS_TEXT_TRANSFORM_LOWERCASE:
                    s.computed.text_transform = TT::Lowercase;  break;
                case LXB_CSS_TEXT_TRANSFORM_CAPITALIZE:
                    s.computed.text_transform = TT::Capitalize; break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_TEXT_DECORATION_LINE: {
            const auto* v = static_cast<
                const lxb_css_property_text_decoration_line_t*>(d->u.user);
            apply_text_decoration_line(*v, s.computed);
            break;
        }
        case LXB_CSS_PROPERTY_TEXT_DECORATION_COLOR: {
            const auto* v =
                static_cast<const lxb_css_property_text_decoration_color_t*>(
                    d->u.user);
            std::uint32_t rgba;
            if (parse_color(v, rgba, s.animated.color_rgba))
                s.animated.text_decoration_rgba = rgba;
            break;
        }
        case LXB_CSS_PROPERTY_TEXT_DECORATION: {
            const auto* v =
                static_cast<const lxb_css_property_text_decoration_t*>(
                    d->u.user);
            apply_text_decoration_line(v->line, s.computed);
            std::uint32_t rgba;
            if (parse_color(&v->color, rgba, s.animated.color_rgba))
                s.animated.text_decoration_rgba = rgba;
            break;
        }
        case LXB_CSS_PROPERTY_WIDTH: {
            const auto* v =
                static_cast<const lxb_css_property_width_t*>(d->u.user);
            awv(*v, s.computed.width, -1, &s.computed.width_pct_x100);
            break;
        }
        case LXB_CSS_PROPERTY_HEIGHT: {
            const auto* v =
                static_cast<const lxb_css_property_height_t*>(d->u.user);
            // height_pct is int8_t (0..100); use a temporary int16_t
            // for apply_width_value and truncate to integer percent.
            std::int16_t pct_x100 = -1;
            awv(*v, s.computed.height, -1, &pct_x100);
            s.computed.height_pct = (pct_x100 >= 0)
                ? static_cast<std::int8_t>(pct_x100 / 100)
                : static_cast<std::int8_t>(-1);
            break;
        }
        case LXB_CSS_PROPERTY_MIN_WIDTH: {
            const auto* v =
                static_cast<const lxb_css_property_min_width_t*>(d->u.user);
            awv(*v, s.computed.min_width, -1);
            break;
        }
        case LXB_CSS_PROPERTY_MAX_WIDTH: {
            const auto* v =
                static_cast<const lxb_css_property_max_width_t*>(d->u.user);
            awv(*v, s.computed.max_width, -1);
            break;
        }
        case LXB_CSS_PROPERTY_MIN_HEIGHT: {
            const auto* v =
                static_cast<const lxb_css_property_min_height_t*>(d->u.user);
            awv(*v, s.computed.min_height, 0);
            break;
        }
        case LXB_CSS_PROPERTY_MAX_HEIGHT: {
            const auto* v =
                static_cast<const lxb_css_property_max_height_t*>(d->u.user);
            awv(*v, s.computed.max_height, -1);
            break;
        }
        case LXB_CSS_PROPERTY_OVERFLOW: {
            // CSS `overflow` shorthand applies the single value to both
            // overflow-x and overflow-y. AffineUI currently tracks only
            // the Y axis for clipping; map it there.
            const auto* v =
                static_cast<const lxb_css_property_overflow_t*>(d->u.user);
            using O = ComputedStyle::Overflow;
            switch (v->type) {
                case LXB_CSS_OVERFLOW_VISIBLE: s.computed.overflow_y = O::Visible; break;
                case LXB_CSS_OVERFLOW_HIDDEN:  s.computed.overflow_y = O::Hidden;  break;
                case LXB_CSS_OVERFLOW_CLIP:    s.computed.overflow_y = O::Clip;    break;
                case LXB_CSS_OVERFLOW_SCROLL:  s.computed.overflow_y = O::Scroll;  break;
                case LXB_CSS_OVERFLOW_AUTO:    s.computed.overflow_y = O::Auto;    break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_OVERFLOW_Y: {
            const auto* v =
                static_cast<const lxb_css_property_overflow_y_t*>(d->u.user);
            using O = ComputedStyle::Overflow;
            switch (v->type) {
                case LXB_CSS_OVERFLOW_Y_VISIBLE: s.computed.overflow_y = O::Visible; break;
                case LXB_CSS_OVERFLOW_Y_HIDDEN:  s.computed.overflow_y = O::Hidden;  break;
                case LXB_CSS_OVERFLOW_Y_CLIP:    s.computed.overflow_y = O::Clip;    break;
                case LXB_CSS_OVERFLOW_Y_SCROLL:  s.computed.overflow_y = O::Scroll;  break;
                case LXB_CSS_OVERFLOW_Y_AUTO:    s.computed.overflow_y = O::Auto;    break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_OPACITY: {
            const auto* v =
                static_cast<const lxb_css_property_opacity_t*>(d->u.user);
            // opacity is lxb_css_value_number_percentage_t.
            // CSS: number 0–1 or percentage 0%–100%.
            float op = 1.0f;
            if (v->type == LXB_CSS_VALUE__NUMBER) {
                op = static_cast<float>(
                    std::clamp(v->u.number.num, 0.0, 1.0));
            } else if (v->type == LXB_CSS_VALUE__PERCENTAGE) {
                op = static_cast<float>(
                    std::clamp(v->u.percentage.num / 100.0, 0.0, 1.0));
            }
            s.animated.opacity = op;
            break;
        }
        // ── Visibility ─────────────────────────────────────────────
        // CSS `visibility` is an inherited property (initial = visible).
        // Visible    → painted normally.
        // Hidden     → box kept in layout, nothing painted (self +
        //              descendants unless a child re-asserts `visible`).
        // Collapse   → same as hidden for non-table elements (CSS spec).
        case LXB_CSS_PROPERTY_TRANSFORM: {
            const auto* v =
                static_cast<const lxb_css_property_transform_t*>(d->u.user);
            apply_transform_value(*v, s.animated, em_px,
                                  viewport_w, viewport_h);
            break;
        }
        case LXB_CSS_PROPERTY_TRANSFORM_ORIGIN: {
            const auto* v =
                static_cast<const lxb_css_property_transform_origin_t*>(d->u.user);
            apply_transform_origin_value(*v, s.animated, em_px,
                                         viewport_w, viewport_h);
            break;
        }
        case LXB_CSS_PROPERTY_ANIMATION: {
            const auto* v =
                static_cast<const lxb_css_property_animation_t*>(d->u.user);
            apply_animation_value(*v, s.animation);
            break;
        }
        case LXB_CSS_PROPERTY_ANIMATION_NAME: {
            const auto* v =
                static_cast<const lxb_css_property_animation_name_t*>(d->u.user);
            if (v->has_name && v->name.data && v->name.length > 0) {
                s.animation.name_hash = fnv1a_32(std::string_view(
                    reinterpret_cast<const char*>(v->name.data),
                    v->name.length));
                s.animation.active = true;
            } else {
                s.animation.name_hash = 0;
                s.animation.active = false;
            }
            break;
        }
        case LXB_CSS_PROPERTY_ANIMATION_DURATION: {
            const auto* v = static_cast<
                const lxb_css_property_animation_duration_t*>(d->u.user);
            s.animation.duration_s = static_cast<float>(
                std::max(0.0, v->duration_s));
            break;
        }
        case LXB_CSS_PROPERTY_ANIMATION_DELAY: {
            const auto* v = static_cast<
                const lxb_css_property_animation_delay_t*>(d->u.user);
            s.animation.delay_s = static_cast<float>(v->delay_s);
            break;
        }
        case LXB_CSS_PROPERTY_ANIMATION_ITERATION_COUNT: {
            const auto* v = static_cast<
                const lxb_css_property_animation_iteration_count_t*>(d->u.user);
            s.animation.iteration_count = static_cast<float>(
                std::max(0.0, v->iteration_count));
            break;
        }
        case LXB_CSS_PROPERTY_ANIMATION_TIMING_FUNCTION: {
            const auto* v = static_cast<
                const lxb_css_property_animation_timing_function_t*>(d->u.user);
            ResolvedStyle::CssAnimation mapped{};
            apply_animation_value(*v, mapped);
            s.animation.timing = mapped.timing;
            break;
        }
        case LXB_CSS_PROPERTY_ANIMATION_DIRECTION: {
            const auto* v = static_cast<
                const lxb_css_property_animation_direction_t*>(d->u.user);
            ResolvedStyle::CssAnimation mapped{};
            apply_animation_value(*v, mapped);
            s.animation.direction = mapped.direction;
            break;
        }
        case LXB_CSS_PROPERTY_ANIMATION_FILL_MODE: {
            const auto* v = static_cast<
                const lxb_css_property_animation_fill_mode_t*>(d->u.user);
            ResolvedStyle::CssAnimation mapped{};
            apply_animation_value(*v, mapped);
            s.animation.fill_mode = mapped.fill_mode;
            break;
        }
        case LXB_CSS_PROPERTY_ANIMATION_PLAY_STATE: {
            const auto* v = static_cast<
                const lxb_css_property_animation_play_state_t*>(d->u.user);
            ResolvedStyle::CssAnimation mapped{};
            apply_animation_value(*v, mapped);
            s.animation.play_state = mapped.play_state;
            break;
        }
        case LXB_CSS_PROPERTY_VISIBILITY: {
            const auto* v =
                static_cast<const lxb_css_property_visibility_t*>(d->u.user);
            using V = ComputedStyle::Visibility;
            switch (v->type) {
                case LXB_CSS_VISIBILITY_VISIBLE:  s.computed.visibility = V::Visible;  break;
                case LXB_CSS_VISIBILITY_HIDDEN:   s.computed.visibility = V::Hidden;   break;
                case LXB_CSS_VISIBILITY_COLLAPSE: s.computed.visibility = V::Collapse; break;
                default: break;
            }
            break;
        }
        case LXB_CSS_PROPERTY_TEXT_ALIGN: {
            // text-align is an inherited property. The cascade resolver
            // pre-seeds `s` from the parent, so inherited values arrive
            // automatically. An explicit declaration overrides.
            const auto* v =
                static_cast<const lxb_css_property_text_align_t*>(d->u.user);
            using TA = ComputedStyle::TextAlign;
            switch (v->type) {
                case LXB_CSS_TEXT_ALIGN_LEFT:
                case LXB_CSS_TEXT_ALIGN_START: s.computed.text_align = TA::Left;    break;
                case LXB_CSS_TEXT_ALIGN_RIGHT:
                case LXB_CSS_TEXT_ALIGN_END:   s.computed.text_align = TA::Right;   break;
                case LXB_CSS_TEXT_ALIGN_CENTER: s.computed.text_align = TA::Center; break;
                case LXB_CSS_TEXT_ALIGN_JUSTIFY:
                case LXB_CSS_TEXT_ALIGN_JUSTIFY_ALL:
                                               s.computed.text_align = TA::Justify; break;
                default: break;
            }
            s.computed.has.text_align = 1;
            break;
        }
        // Everything else lands when we have a test for it.
        default:
            break;
    }
}

// lxb_html_element_style_walk callback. Lexbor's pre-matched store
// keeps the highest-specificity rule per property as the AVL node's
// "primary" value, with lower-specificity matches chained behind as
// "weak" entries. The walk visits PRIMARY first then WEAK, so if we
// applied every declaration with last-write-wins we'd let the lower-
// specificity rule overwrite the higher one — exactly backwards from
// CSS cascade semantics. Solution: ignore the weak chain entirely.
// The primary entry already encodes the cascade winner for this
// property on this element.
// A declaration whose typed parse failed because it contains var():
// the intended property id plus the raw, unresolved value string. We
// can't apply it until the element's full custom-property scope is
// known, so we collect these during the walk and resolve them after.
struct DeferredVar {
    std::uintptr_t                 property_id;
    std::string                    raw_value;
    lxb_css_selector_specificity_t spec{0};
};

// One per-property cascade winner collected during the walk, paired with
// its specificity so we can apply the winners in CSS cascade order rather
// than in lexbor's AVL-tree (pre-order) traversal order.
struct PendingDecl {
    lxb_css_selector_specificity_t    spec;
    const lxb_css_rule_declaration_t* declr;
};

// CSS shorthands (margin, padding, border, …) expand to several
// longhands. lexbor's pre-matched store keeps each *declared* property —
// shorthand or longhand — as its own cascade winner under a separate id,
// so an overlapping shorthand and longhand (e.g. a UA `margin` shorthand
// vs an author `margin-top:0` reset) never compete; both survive as
// primaries. We must therefore order their application ourselves: a
// shorthand has to be written BEFORE the longhands it can set, otherwise
// a same-specificity longhand reset — the universal pattern in
// resets/normalizers such as Bootstrap Reboot — would be silently
// clobbered. Broader shorthands rank lower (applied earlier) than the
// per-side shorthands they contain, which in turn precede longhands.
inline int shorthand_rank(std::uintptr_t id) {
    switch (id) {
        case LXB_CSS_PROPERTY_MARGIN:
        case LXB_CSS_PROPERTY_PADDING:
        case LXB_CSS_PROPERTY_BORDER:
        case LXB_CSS_PROPERTY_BORDER_COLOR:
        case LXB_CSS_PROPERTY_BORDER_STYLE:
        case LXB_CSS_PROPERTY_BORDER_WIDTH:
        case LXB_CSS_PROPERTY_BORDER_RADIUS:
        case LXB_CSS_PROPERTY_BACKGROUND:
        case LXB_CSS_PROPERTY_GAP:
        case LXB_CSS_PROPERTY_FLEX:
        case LXB_CSS_PROPERTY_OVERFLOW:
        case LXB_CSS_PROPERTY_INSET:
        case LXB_CSS_PROPERTY_FONT:
            return 0;  // broad shorthands
        case LXB_CSS_PROPERTY_BORDER_TOP:
        case LXB_CSS_PROPERTY_BORDER_RIGHT:
        case LXB_CSS_PROPERTY_BORDER_BOTTOM:
        case LXB_CSS_PROPERTY_BORDER_LEFT:
            return 1;  // per-side border shorthands
        default:
            return 2;  // longhands
    }
}

struct WalkCtx {
    ResolvedStyle*            out;
    CustomPropMap*            own_customs;  // `--x` declared on this element
    std::vector<DeferredVar>* deferred;     // var()-bearing declarations
    std::vector<PendingDecl>* pending;      // typed cascade winners, unsorted
};

lxb_status_t walk_callback(lxb_html_element_t* /*element*/,
                           const lxb_css_rule_declaration_t* declr,
                           void* ctx,
                           lxb_css_selector_specificity_t spec,
                           bool is_weak) {
    if (is_weak) return LXB_STATUS_OK;
    auto* w = static_cast<WalkCtx*>(ctx);

    // `--name: value` → record the cascade-winning custom property.
    if (declr->type == LXB_CSS_PROPERTY__CUSTOM) {
        const auto* c = declr->u.custom;
        if (c && c->name.data && c->value.data) {
            (*w->own_customs)[std::string(
                reinterpret_cast<const char*>(c->name.data), c->name.length)] =
                std::string(reinterpret_cast<const char*>(c->value.data),
                            c->value.length);
        }
        return LXB_STATUS_OK;
    }

    // A declaration lexbor couldn't type-parse. If it's a known
    // property carrying var(), defer it for substitution + re-parse;
    // anything else here is genuinely invalid CSS we correctly drop.
    if (declr->type == LXB_CSS_PROPERTY__UNDEF) {
        const auto* u = declr->u.undef;
        if (u && u->value.data && u->type != LXB_CSS_PROPERTY__UNDEF) {
            std::string_view val(reinterpret_cast<const char*>(u->value.data),
                                 u->value.length);
            if (find_var(val, 0) != std::string_view::npos ||
                find_calc(val, 0) != std::string_view::npos) {
                w->deferred->push_back({u->type, std::string(val), spec});
            }
        }
        return LXB_STATUS_OK;
    }

    // Defer application: collect the winner with its specificity so the
    // resolver can apply all winners in cascade order (see shorthand_rank).
    w->pending->push_back({spec, declr});
    return LXB_STATUS_OK;
}

class LexborResolver final : public StyleResolver {
public:
    // doc_ is kept (but unused for now) so Phase 2E can attach
    // per-document caching/invalidation hooks without changing the
    // construction shape. parser_/reparse_mem_ re-parse var()-resolved
    // declaration strings back into typed lexbor declarations.
    explicit LexborResolver(lxb_html_document_t* doc,
                            int viewport_width_px,
                            int viewport_height_px)
        : doc_(doc),
          viewport_width_px_(viewport_width_px),
          viewport_height_px_(viewport_height_px) {
        (void)doc_;
        parser_ = lxb_css_parser_create();
        if (parser_ && lxb_css_parser_init(parser_, nullptr) != LXB_STATUS_OK) {
            lxb_css_parser_destroy(parser_, true);
            parser_ = nullptr;
        }
        reparse_mem_ = lxb_css_memory_create();
        if (reparse_mem_ &&
            lxb_css_memory_init(reparse_mem_, 128) != LXB_STATUS_OK) {
            lxb_css_memory_destroy(reparse_mem_, true);
            reparse_mem_ = nullptr;
        }
    }

    ~LexborResolver() override {
        if (reparse_mem_) lxb_css_memory_destroy(reparse_mem_, true);
        if (parser_)      lxb_css_parser_destroy(parser_, true);
    }

    ResolvedStyle resolve(lxb_dom_element_t* element,
                          const ResolvedStyle& parent) override {
        if (element) {
            if (auto it = cache_.find(element); it != cache_.end()) {
                return it->second;
            }
        }

        // Start from the parent's resolved style so inherited
        // properties pre-arrive. The cascade then overrides whatever
        // the matching rules specified for this element.
        //
        // Non-inherited properties (background, margins, paddings)
        // are reset to CSS initial values before the walk — we mask
        // them out of the parent here.
        ResolvedStyle s = parent;
        // Reset non-inherited fields to their initial values.
        s.animated.background_rgba     = 0x00000000u;  // transparent
        s.animated.gradient_kind =
            AnimatedStyle::GradientKind::None;
        s.animated.gradient_center_x_pct = 50;
        s.animated.gradient_center_y_pct = 50;
        s.animated.gradient_stop1_pos_pct = 100;
        s.animated.gradient_stop0_rgba = 0x00000000u;
        s.animated.gradient_stop1_rgba = 0x00000000u;
        s.animated.background_grid_rgba = 0x00000000u;
        s.animated.background_grid_size_px = 0;
        s.animated.background_grid_line_px = 1;
        s.animated.border_rgba         = 0x00000000u;  // transparent
        s.animated.border_top_rgba     = 0x00000000u;
        s.animated.border_right_rgba   = 0x00000000u;
        s.animated.border_bottom_rgba  = 0x00000000u;
        s.animated.border_left_rgba    = 0x00000000u;
        s.animated.border_color_set    = 0;
        s.animated.text_decoration_rgba = 0x00000000u;
        clear_box_shadow(s.animated);
        s.box_shadows.reset();
        s.animated.tx = s.animated.ty = 0.0f;
        s.animated.tx_pct = s.animated.ty_pct = 0.0f;
        s.animated.scale_x = s.animated.scale_y = 1.0f;
        s.animated.rotation = 0.0f;
        s.animated.origin_x = s.animated.origin_y = 0.0f;
        s.animated.origin_x_pct = s.animated.origin_y_pct = 50.0f;
        s.animated.opacity  = 1.0f;
        s.animation = ResolvedStyle::CssAnimation{};
        // ComputedStyle: margins, padding, borders, width/height,
        // display reset to defaults (struct's brace-init values).
        s.computed.margin_top = s.computed.margin_right =
            s.computed.margin_bottom = s.computed.margin_left = 0;
        s.computed.margin_auto = ComputedStyle::MarginAuto{};
        s.computed.padding_top = s.computed.padding_right =
            s.computed.padding_bottom = s.computed.padding_left = 0;
        s.computed.border_top = s.computed.border_right =
            s.computed.border_bottom = s.computed.border_left = 0;
        s.computed.border_style              = ComputedStyle::BorderStyle::None;
        s.computed.border_radius_top_left  = 0;
        s.computed.border_radius_top_right = 0;
        s.computed.border_radius_bot_right = 0;
        s.computed.border_radius_bot_left  = 0;
        s.computed.border_style_sides      = 0;
        // Flex (non-inherited; reset to CSS initial values).
        s.computed.flex_direction   = ComputedStyle::FlexDirection::Row;
        s.computed.flex_wrap        = ComputedStyle::FlexWrap::NoWrap;
        s.computed.justify_content  = ComputedStyle::JustifyContent::Start;
        s.computed.align_items      = ComputedStyle::AlignItems::Stretch;
        s.computed.row_gap          = 0;
        s.computed.column_gap       = 0;
        s.computed.flex_grow        = 0;
        s.computed.flex_shrink      = 1;
        s.computed.flex_basis       = -1;
        // Cursor is technically *inherited* in CSS (an element with
        // no cursor inherits its parent's). But cascade's `s = parent`
        // already does that for us — explicit reset would break the
        // inheritance. Note for the next person looking at this list.
        // (No reset.)
        s.computed.width = s.computed.height = -1;
        s.computed.min_width = -1;
        s.computed.max_width = -1;
        s.computed.min_height = 0;
        s.computed.max_height = -1;
        s.computed.resize = ComputedStyle::Resize::None;
        // Percentage sizing (non-inherited; -1 = not a percentage).
        s.computed.width_pct_x100 = -1;
        s.computed.height_pct = -1;
        s.computed.flex_basis_pct = -1;
        s.computed.display = ComputedStyle::Display::Block;
        s.computed.position = ComputedStyle::Position::Static;
        s.computed.z_index_low = 0;
        s.computed.z_index_high = 0;
        s.computed.css_float = ComputedStyle::Float::None;
        s.computed.overflow_y = ComputedStyle::Overflow::Visible;
        // Positioned insets (non-inherited; CSS initial = auto).
        s.computed.inset_top = s.computed.inset_right =
            s.computed.inset_bottom = s.computed.inset_left = 0;
        s.computed.inset_has = ComputedStyle::InsetHas{};
        // Box-sizing (non-inherited; CSS initial = content-box).
        s.computed.box_sizing = ComputedStyle::BoxSizing::ContentBox;
        // vertical-align is non-inherited; CSS initial = baseline.
        s.computed.vertical_align = ComputedStyle::VerticalAlign::Baseline;
        s.computed.text_decoration_line = ComputedStyle::DecorationNone;
        // white-space, letter-spacing, text-transform, and text-indent are
        // inherited in CSS. cascade's `s = parent` at the top of resolve()
        // already propagated their values down, so no reset is needed here —
        // the cascade overrides only when the element explicitly sets them.

        // Custom properties inherit: start the element's scope as its
        // parent's (cheap shared_ptr copy via `s = parent`). The walk
        // below layers on any `--x` this element declares.
        if (!element) return s;

        CustomPropMap own_customs;
        std::vector<DeferredVar> deferred;
        std::vector<PendingDecl> pending;
        WalkCtx ctx{&s, &own_customs, &deferred, &pending};
        auto* html_el =
            lxb_html_interface_element(lxb_dom_interface_node(element));
        // with_weak=false: only walk the cascade winner per property.
        // The walk_callback also guards against weak entries for
        // belt-and-braces in case lexbor's contract ever shifts.
        lxb_html_element_style_walk(html_el, walk_callback, &ctx,
                                    /*with_weak=*/false);

        // Finalize this element's custom-property scope FIRST (copy-on-write:
        // only elements that declare `--x` clone the inherited map) so the
        // deferred var() declarations below can resolve against it.
        if (!own_customs.empty()) {
            auto merged = std::make_shared<CustomPropMap>(
                parent.custom_props ? *parent.custom_props : CustomPropMap{});
            for (auto& [k, v] : own_customs) (*merged)[k] = std::move(v);
            s.custom_props = std::move(merged);
        }

        // Apply the cascade winners. Two requirements drive the ordering:
        //   1. `em` resolution (CSS §): font-size resolves `em` against the
        //      PARENT's font-size; every other property resolves `em` against
        //      the element's OWN computed font-size — so font-size goes first.
        //   2. Specificity across var(): a higher-specificity declaration must
        //      win whether or not it uses var(), so typed (pending) and
        //      deferred var() declarations are merged in specificity order
        //      (e.g. `.nav-link.active{border-bottom-color:currentcolor}` must
        //      beat `.nav-link{border-bottom:var(--w) solid transparent}`).
        // lexbor visits its style AVL in pre-order (not cascade order) and
        // keeps overlapping shorthands/longhands as separate winners, so we
        // sort by (specificity asc, shorthand-before-longhand) + last-write-wins.
        std::stable_sort(pending.begin(), pending.end(),
            [](const PendingDecl& a, const PendingDecl& b) {
                if (a.spec != b.spec) return a.spec < b.spec;
                return shorthand_rank(a.declr->type) <
                       shorthand_rank(b.declr->type);
            });
        std::stable_sort(deferred.begin(), deferred.end(),
            [](const DeferredVar& a, const DeferredVar& b) {
                if (a.spec != b.spec) return a.spec < b.spec;
                return shorthand_rank(a.property_id) <
                       shorthand_rank(b.property_id);
            });

        static const CustomPropMap kEmpty;
        const CustomPropMap& scope = s.custom_props ? *s.custom_props : kEmpty;
        constexpr double rem = 16.0;
        auto is_font_size = [](std::uintptr_t t) {
            return t == LXB_CSS_PROPERTY_FONT_SIZE || t == LXB_CSS_PROPERTY_FONT;
        };
        auto is_only_font_size = [](std::uintptr_t t) {
            return t == LXB_CSS_PROPERTY_FONT_SIZE;
        };
        auto is_color = [](std::uintptr_t t) {
            return t == LXB_CSS_PROPERTY_COLOR;
        };

        // ── Pass 1: font-size only (em resolves against the PARENT's size).
        auto deferred_before = [](const DeferredVar& dv,
                                  const PendingDecl& pd) {
            if (dv.spec != pd.spec) return dv.spec < pd.spec;
            return shorthand_rank(dv.property_id) <
                   shorthand_rank(pd.declr->type);
        };

        const double parent_em = (s.computed.font_size_px > 0)
            ? static_cast<double>(s.computed.font_size_px) : 16.0;
        std::size_t font_di = 0;
        auto apply_deferred_font = [&](const DeferredVar& dv) {
            if (!is_font_size(dv.property_id)) return;
            const std::string resolved = evaluate_calc(
                substitute_vars(dv.raw_value, scope), rem, parent_em,
                static_cast<double>(viewport_width_px_),
                static_cast<double>(viewport_height_px_),
                &viewport_dependency_);
            apply_resolved_decl(dv.property_id, resolved, s, parent_em,
                                &parent, /*apply_font_size=*/true);
        };
        for (const PendingDecl& pd : pending) {
            if (!is_font_size(pd.declr->type)) continue;
            while (font_di < deferred.size() &&
                   deferred_before(deferred[font_di], pd))
                apply_deferred_font(deferred[font_di++]);
            apply_declaration(pd.declr, s, parent_em, &parent,
                              /*apply_font_size=*/true,
                              static_cast<double>(viewport_width_px_),
                              static_cast<double>(viewport_height_px_),
                              &viewport_dependency_);
        }
        while (font_di < deferred.size())
            apply_deferred_font(deferred[font_di++]);

        // Pass 1b: resolve `color` before other properties. CSS `currentColor`
        // references the element's computed color regardless of declaration
        // source order, so border/background/text-decoration colors must see
        // this final value when they resolve.
        for (const PendingDecl& pd : pending)
            if (is_color(pd.declr->type))
                apply_declaration(pd.declr, s, parent_em, &parent,
                                  /*apply_font_size=*/true,
                                  static_cast<double>(viewport_width_px_),
                                  static_cast<double>(viewport_height_px_),
                                  &viewport_dependency_);
        for (const DeferredVar& dv : deferred)
            if (is_color(dv.property_id)) {
                const std::string resolved = evaluate_calc(
                    substitute_vars(dv.raw_value, scope), rem, parent_em,
                    static_cast<double>(viewport_width_px_),
                    static_cast<double>(viewport_height_px_),
                    &viewport_dependency_);
                apply_resolved_decl(dv.property_id, resolved, s, parent_em,
                                    &parent);
            }

        // Pass 2: everything else, interleaving typed + deferred var() by
        // ascending specificity; em now resolves against the element's own
        // computed font-size. Font-size and color entries are skipped.
        const double own_em = (s.computed.font_size_px > 0)
            ? static_cast<double>(s.computed.font_size_px) : 16.0;
        std::size_t di = 0;
        auto apply_deferred = [&](const DeferredVar& dv) {
            if (is_only_font_size(dv.property_id)) return;  // applied in pass 1
            if (is_color(dv.property_id)) return;
            const std::string resolved = evaluate_calc(
                substitute_vars(dv.raw_value, scope), rem, own_em,
                static_cast<double>(viewport_width_px_),
                static_cast<double>(viewport_height_px_),
                &viewport_dependency_);
            apply_resolved_decl(dv.property_id, resolved, s, own_em, &parent,
                                dv.property_id != LXB_CSS_PROPERTY_FONT);
        };
        for (const PendingDecl& pd : pending) {
            if (is_only_font_size(pd.declr->type)) continue;  // applied in pass 1
            if (is_color(pd.declr->type)) continue;
            while (di < deferred.size() &&
                   deferred_before(deferred[di], pd))
                apply_deferred(deferred[di++]);
            apply_declaration(pd.declr, s, own_em, &parent,
                              pd.declr->type != LXB_CSS_PROPERTY_FONT,
                              static_cast<double>(viewport_width_px_),
                              static_cast<double>(viewport_height_px_),
                              &viewport_dependency_);
        }
        while (di < deferred.size()) apply_deferred(deferred[di++]);

        cache_[element] = s;
        return s;
    }

    void apply_decl_list(const lxb_css_rule_declaration_list_t* list,
                         ResolvedStyle& out) override {
        if (!list) return;
        const double em = out.computed.font_size_px > 0
            ? static_cast<double>(out.computed.font_size_px)
            : 16.0;
        auto ensure_custom_scope = [&]() -> CustomPropMap& {
            auto mutable_scope = std::make_shared<CustomPropMap>(
                out.custom_props ? *out.custom_props : CustomPropMap{});
            out.custom_props = mutable_scope;
            return *mutable_scope;
        };

        // Each declaration in the list is an lxb_css_rule_t-derived
        // node; iterate via the base ->next pointer (the list's storage
        // is the same intrusive chain lexbor uses for any rule list).
        for (auto* node = list->first; node != nullptr; node = node->next) {
            auto* decl = reinterpret_cast<const lxb_css_rule_declaration_t*>(node);
            if (decl->type == LXB_CSS_PROPERTY__CUSTOM) {
                const auto* c = decl->u.custom;
                if (c && c->name.data && c->value.data) {
                    ensure_custom_scope()[std::string(
                        reinterpret_cast<const char*>(c->name.data),
                        c->name.length)] =
                        std::string(reinterpret_cast<const char*>(c->value.data),
                                    c->value.length);
                }
                continue;
            }
            if (decl->type == LXB_CSS_PROPERTY__UNDEF) {
                const auto* u = decl->u.undef;
                if (u && u->value.data && u->type != LXB_CSS_PROPERTY__UNDEF) {
                    std::string_view val(
                        reinterpret_cast<const char*>(u->value.data),
                        u->value.length);
                    if (find_var(val, 0) != std::string_view::npos ||
                        find_calc(val, 0) != std::string_view::npos) {
                        static const CustomPropMap kEmpty;
                        const CustomPropMap& scope =
                            out.custom_props ? *out.custom_props : kEmpty;
                        const std::string resolved = evaluate_calc(
                            substitute_vars(std::string(val), scope),
                            16.0, em,
                            static_cast<double>(viewport_width_px_),
                            static_cast<double>(viewport_height_px_),
                            &viewport_dependency_);
                        apply_resolved_decl(u->type, resolved, out, em,
                                            nullptr, /*apply_font_size=*/true);
                    }
                }
                continue;
            }
            apply_declaration(decl, out, em, nullptr, /*apply_font_size=*/true,
                              static_cast<double>(viewport_width_px_),
                              static_cast<double>(viewport_height_px_),
                              &viewport_dependency_);
        }
    }

    ViewportDependency viewport_dependency() const override {
        return viewport_dependency_;
    }

    void set_viewport(int width_px, int height_px) override {
        viewport_width_px_ = width_px;
        viewport_height_px_ = height_px;
    }

    void invalidate(lxb_dom_element_t*) override {
        // Attribute/class/style mutations can affect descendant selectors and
        // inherited custom-property scopes, so use a conservative document-wide
        // cache clear until the style invalidation graph is more selective.
        cache_.clear();
    }
    void clear() override { cache_.clear(); }

private:
    // Re-parse a var()-resolved value back into a typed declaration and
    // apply it. `property_id` is the intended property (lexbor kept it
    // on the failed declaration); `value` is the substituted value with
    // no var() left. We build "<property-name>:<value>", run it through
    // lexbor's declaration parser, and route the resulting typed
    // declaration(s) through the normal apply path.
    // em_px: element's own font-size used for `em` length resolution.
    void apply_resolved_decl(std::uintptr_t property_id,
                             const std::string& value, ResolvedStyle& s,
                             double em_px = 16.0,
                             const ResolvedStyle* parent = nullptr,
                             bool apply_font_size = true) {
        if (value.empty() || !parser_ || !reparse_mem_) return;
        const lxb_css_entry_data_t* pd = lxb_css_property_by_id(property_id);
        if (!pd || pd->name == nullptr) return;

        std::string decl(reinterpret_cast<const char*>(pd->name), pd->length);
        decl += ':';
        decl += value;

        lxb_css_rule_declaration_list_t* list = lxb_css_declaration_list_parse(
            parser_, reparse_mem_,
            reinterpret_cast<const lxb_char_t*>(decl.data()), decl.size());
        if (list != nullptr) {
            for (auto* node = list->first; node != nullptr; node = node->next) {
                apply_declaration(
                    reinterpret_cast<const lxb_css_rule_declaration_t*>(node), s,
                    em_px, parent, apply_font_size,
                    static_cast<double>(viewport_width_px_),
                    static_cast<double>(viewport_height_px_),
                    &viewport_dependency_);
            }
        }
        // Reset the arena for the next re-parse (the typed values we
        // care about were already copied into `s` by apply_declaration).
        lxb_css_memory_clean(reparse_mem_);
    }

    lxb_html_document_t* doc_;
    int                  viewport_width_px_{0};
    int                  viewport_height_px_{0};
    lxb_css_parser_t*    parser_{nullptr};
    lxb_css_memory_t*    reparse_mem_{nullptr};
    ViewportDependency   viewport_dependency_{};
    std::unordered_map<lxb_dom_element_t*, ResolvedStyle> cache_;
};

}  // namespace

std::unique_ptr<StyleResolver> make_lexbor_resolver(lxb_html_document_t* doc,
                                                    int viewport_width_px,
                                                    int viewport_height_px) {
    return std::make_unique<LexborResolver>(
        doc, viewport_width_px, viewport_height_px);
}

#endif  // AFFINEUI_STUB_BUILD

}  // namespace affineui::detail
