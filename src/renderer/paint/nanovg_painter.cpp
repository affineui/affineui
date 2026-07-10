// Concrete Painter that draws via NanoVG.
//
// Lowers each affineui::Painter method onto nvg* calls. NanoVG handles
// the GL3 backend, the glyph atlas (via bundled fontstash), and
// stb_image-backed image loading.

#include "affineui/painter.h"
#include "renderer/paint/paint_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if !defined(AFFINEUI_STUB_BUILD)
#    include "nanovg.h"
#    include "stb_image.h"
#    if !defined(AFFINEUI_HOST_PROVIDES_NANOVG)
// adopt_native_image wraps an existing sg_image via the NanoVG-on-sokol
// backend (declarations only; the backend TU is nanovg_sokol.c).
#        include "sokol_gfx.h"
#        include "nanovg_sokol.h"
#    endif
#    if defined(AFFINEUI_HAVE_EMBEDDED_FONTS)
// Byte arrays generated at build time from assets/fonts/*.ttf by bin2c.
#        include "roboto_regular.h"
#        include "roboto_bold.h"
#    endif
#endif

namespace affineui {

#if defined(AFFINEUI_STUB_BUILD)

// Stub Painter that satisfies the interface so other code links when
// no deps have been fetched yet.
class NullPainter final : public Painter {
public:
    void begin_frame(int, int, float) override {}
    void end_frame() override {}
    void fill_rect(const Rect&, Color) override {}
    void stroke_rect(const Rect&, Color, float) override {}
    void stroke_line(float, float, float, float, Color, float) override {}
    void fill_circle(float, float, float, Color) override {}
    void stroke_arc(float, float, float, float, float, Color, float) override {}
    void fill_rounded_rect(const Rect&, float, Color) override {}
    void stroke_rounded_rect(const Rect&, float, Color, float) override {}
    void fill_rounded_rect_varying(const Rect&, float, float, float, float, Color) override {}
    void stroke_rounded_rect_varying(const Rect&, float, float, float, float, Color, float) override {}
    void fill_linear_gradient_rect(const Rect&, float, Color, Color,
                                   float, float, float, float) override {}
    void fill_radial_gradient_rect(const Rect&, Color, Color,
                                   float, float, float, float,
                                   float, float, float) override {}
    void fill_box_shadow(const Rect&, float, Color, float, float, float, float,
                         bool) override {}
    std::uint32_t resolve_font(std::string_view, int, int, bool) override { return 0; }
    int           measure_text(std::uint32_t, std::string_view) override { return 0; }
    TextMetrics   text_metrics(std::uint32_t) override { return {}; }
    void draw_text(std::uint32_t, const Point&, std::string_view, Color) override {}
    Size measure_text_box(std::uint32_t, std::string_view, float, float, float) override { return {}; }
    void draw_text_box(std::uint32_t, float, float, std::string_view, Color, float, float, float, TextAlign) override {}
    std::uint32_t load_image(std::string_view) override { return 0; }
    Size          image_size(std::uint32_t) override { return {}; }
    void draw_image(std::uint32_t, const Rect&, const Rect&) override {}
    void push_clip(const Rect&) override {}
    void pop_clip() override {}
    void push_alpha(float) override {}
    void pop_alpha() override {}
    void push_transform(const Mat2x3&) override {}
    void pop_transform() override {}
};

namespace detail {
std::unique_ptr<Painter> make_nanovg_painter(NVGcontext*) {
    return std::make_unique<NullPainter>();
}
std::uint32_t register_font_file(NVGcontext*, const char*, const char*) { return 0; }
std::string_view register_default_font(NVGcontext*) { return {}; }
}  // namespace detail

#else  // !AFFINEUI_STUB_BUILD

namespace {

inline NVGcolor to_nvg(Color c) {
    return nvgRGBA(c.r, c.g, c.b, c.a);
}

bool is_raw_sfnt_font(std::string_view bytes) {
    if (bytes.size() < 4) return false;
    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(bytes.data());
    const std::uint32_t tag =
        (static_cast<std::uint32_t>(p[0]) << 24) |
        (static_cast<std::uint32_t>(p[1]) << 16) |
        (static_cast<std::uint32_t>(p[2]) << 8)  |
        (static_cast<std::uint32_t>(p[3]));
    return tag == 0x00010000u || // TrueType
           tag == 0x4F54544Fu || // "OTTO" OpenType/CFF
           tag == 0x74746366u || // "ttcf" collection
           tag == 0x74727565u || // "true"
           tag == 0x74797031u;   // "typ1"
}

std::uint16_t read_be16(const unsigned char* p) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8) |
         static_cast<std::uint16_t>(p[1]));
}

std::uint32_t read_be32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}

void write_be16(unsigned char* p, std::uint16_t v) {
    p[0] = static_cast<unsigned char>((v >> 8) & 0xFFu);
    p[1] = static_cast<unsigned char>(v & 0xFFu);
}

void write_be32(unsigned char* p, std::uint32_t v) {
    p[0] = static_cast<unsigned char>((v >> 24) & 0xFFu);
    p[1] = static_cast<unsigned char>((v >> 16) & 0xFFu);
    p[2] = static_cast<unsigned char>((v >> 8) & 0xFFu);
    p[3] = static_cast<unsigned char>(v & 0xFFu);
}

std::uint32_t align4(std::uint32_t v) {
    return (v + 3u) & ~3u;
}

bool is_woff1_font(std::string_view bytes) {
    if (bytes.size() < 4) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    return read_be32(p) == 0x774F4646u; // "wOFF"
}

std::string decode_woff1_to_sfnt(std::string_view bytes) {
    if (bytes.size() < 44 || !is_woff1_font(bytes)) return {};
    const auto* src = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::uint32_t length = read_be32(src + 8);
    const std::uint16_t num_tables = read_be16(src + 12);
    const std::uint32_t total_sfnt_size = read_be32(src + 16);
    if (length > bytes.size() || num_tables == 0 ||
        44u + static_cast<std::uint32_t>(num_tables) * 20u > length ||
        total_sfnt_size < 12u + static_cast<std::uint32_t>(num_tables) * 16u) {
        return {};
    }

    struct Entry {
        std::uint32_t tag{0};
        std::uint32_t offset{0};
        std::uint32_t comp_len{0};
        std::uint32_t orig_len{0};
        std::uint32_t checksum{0};
        std::uint32_t out_offset{0};
    };

    std::vector<Entry> entries;
    entries.reserve(num_tables);
    std::uint32_t out_offset =
        12u + static_cast<std::uint32_t>(num_tables) * 16u;
    for (std::uint16_t i = 0; i < num_tables; ++i) {
        const unsigned char* rec = src + 44u + static_cast<std::uint32_t>(i) * 20u;
        Entry e;
        e.tag      = read_be32(rec + 0);
        e.offset   = read_be32(rec + 4);
        e.comp_len = read_be32(rec + 8);
        e.orig_len = read_be32(rec + 12);
        e.checksum = read_be32(rec + 16);
        if (e.offset > length || e.comp_len > length - e.offset ||
            e.orig_len > total_sfnt_size) {
            return {};
        }
        e.out_offset = out_offset;
        out_offset = align4(out_offset + e.orig_len);
        if (out_offset > total_sfnt_size + 3u) return {};
        entries.push_back(e);
    }

    std::string out(total_sfnt_size, '\0');
    auto* dst = reinterpret_cast<unsigned char*>(out.data());
    write_be32(dst + 0, read_be32(src + 4)); // sfntVersion/flavor
    write_be16(dst + 4, num_tables);

    std::uint16_t max_pow2 = 1;
    std::uint16_t entry_selector = 0;
    while (static_cast<std::uint16_t>(max_pow2 * 2u) <= num_tables) {
        max_pow2 = static_cast<std::uint16_t>(max_pow2 * 2u);
        ++entry_selector;
    }
    const std::uint16_t search_range =
        static_cast<std::uint16_t>(max_pow2 * 16u);
    write_be16(dst + 6, search_range);
    write_be16(dst + 8, entry_selector);
    write_be16(dst + 10,
               static_cast<std::uint16_t>(num_tables * 16u - search_range));

    for (std::uint16_t i = 0; i < num_tables; ++i) {
        const Entry& e = entries[i];
        unsigned char* rec = dst + 12u + static_cast<std::uint32_t>(i) * 16u;
        write_be32(rec + 0, e.tag);
        write_be32(rec + 4, e.checksum);
        write_be32(rec + 8, e.out_offset);
        write_be32(rec + 12, e.orig_len);

        auto* table_dst = dst + e.out_offset;
        const auto* table_src = reinterpret_cast<const char*>(src + e.offset);
        if (e.comp_len == e.orig_len) {
            std::memcpy(table_dst, table_src, e.orig_len);
        } else {
            const int decoded = stbi_zlib_decode_buffer(
                reinterpret_cast<char*>(table_dst),
                static_cast<int>(e.orig_len),
                table_src,
                static_cast<int>(e.comp_len));
            if (decoded != static_cast<int>(e.orig_len)) return {};
        }
    }
    return out;
}

std::string font_face_key(std::string_view family, int weight, bool italic) {
    std::string key;
    key.reserve(family.size() + 16);
    key.append(family);
    key.push_back('|');
    key.append(std::to_string(weight));
    key.push_back('|');
    key.push_back(italic ? 'i' : 'n');
    return key;
}

std::string trim_ascii_ws(std::string_view value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string nvg_strip_css_quotes(std::string value) {
    if (value.size() >= 2) {
        const char first = value.front();
        if ((first == '"' || first == '\'') && value.back() == first) {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

std::vector<std::string> split_font_family_list(std::string_view family) {
    std::vector<std::string> out;
    char quote = '\0';
    std::size_t start = 0;
    for (std::size_t i = 0; i <= family.size(); ++i) {
        const char c = i < family.size() ? family[i] : ',';
        if (quote != '\0') {
            if (c == quote) quote = '\0';
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c != ',') continue;
        auto item = nvg_strip_css_quotes(trim_ascii_ws(family.substr(start, i - start)));
        if (!item.empty()) out.push_back(std::move(item));
        start = i + 1;
    }
    return out;
}

bool ascii_equal_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool is_generic_sans_family(std::string_view family) {
    return ascii_equal_ci(family, "sans") ||
           ascii_equal_ci(family, "sans-serif") ||
           ascii_equal_ci(family, "ui-sans-serif") ||
           ascii_equal_ci(family, "system-ui");
}

bool is_generic_mono_family(std::string_view family) {
    return ascii_equal_ci(family, "monospace") ||
           ascii_equal_ci(family, "ui-monospace");
}

bool is_unavailable_platform_alias(std::string_view family) {
#if defined(_WIN32)
    return ascii_equal_ci(family, "-apple-system") ||
           ascii_equal_ci(family, "BlinkMacSystemFont");
#elif defined(__APPLE__)
    (void)family;
    return false;
#else
    return ascii_equal_ci(family, "-apple-system") ||
           ascii_equal_ci(family, "BlinkMacSystemFont");
#endif
}

int create_font_if_absent(NVGcontext* vg, const char* name, const char* path) {
    if (!name || !path || nvgFindFont(vg, name) >= 0) return nvgFindFont(vg, name);
    return nvgCreateFont(vg, name, path);
}

void register_font_alias_family(NVGcontext* vg,
                                const char* family,
                                const char* regular,
                                const char* bold,
                                const char* italic,
                                const char* bold_italic) {
    if (!family || !regular) return;
    create_font_if_absent(vg, family, regular);
    std::string suffix;
    if (bold) {
        suffix = std::string(family) + "-bold";
        create_font_if_absent(vg, suffix.c_str(), bold);
    }
    if (italic) {
        suffix = std::string(family) + "-italic";
        create_font_if_absent(vg, suffix.c_str(), italic);
    }
    if (bold_italic) {
        suffix = std::string(family) + "-bold-italic";
        create_font_if_absent(vg, suffix.c_str(), bold_italic);
    }
}

void register_platform_font_aliases(NVGcontext* vg) {
#if defined(_WIN32)
    constexpr const char* kSegoe = "C:/Windows/Fonts/segoeui.ttf";
    constexpr const char* kSegoeBold = "C:/Windows/Fonts/segoeuib.ttf";
    constexpr const char* kSegoeItalic = "C:/Windows/Fonts/segoeuii.ttf";
    constexpr const char* kSegoeBoldItalic = "C:/Windows/Fonts/segoeuiz.ttf";
    register_font_alias_family(vg, "Segoe UI", kSegoe, kSegoeBold,
                               kSegoeItalic, kSegoeBoldItalic);
    register_font_alias_family(vg, "system-ui", kSegoe, kSegoeBold,
                               kSegoeItalic, kSegoeBoldItalic);
    register_font_alias_family(vg, "ui-sans-serif", kSegoe, kSegoeBold,
                               kSegoeItalic, kSegoeBoldItalic);
    register_font_alias_family(vg, "Arial", "C:/Windows/Fonts/arial.ttf",
                               "C:/Windows/Fonts/arialbd.ttf",
                               "C:/Windows/Fonts/ariali.ttf",
                               "C:/Windows/Fonts/arialbi.ttf");
    constexpr const char* kConsolas = "C:/Windows/Fonts/consola.ttf";
    constexpr const char* kConsolasBold = "C:/Windows/Fonts/consolab.ttf";
    constexpr const char* kConsolasItalic = "C:/Windows/Fonts/consolai.ttf";
    constexpr const char* kConsolasBoldItalic = "C:/Windows/Fonts/consolaz.ttf";
    register_font_alias_family(vg, "Consolas", kConsolas, kConsolasBold,
                               kConsolasItalic, kConsolasBoldItalic);
    register_font_alias_family(vg, "monospace", kConsolas, kConsolasBold,
                               kConsolasItalic, kConsolasBoldItalic);
    register_font_alias_family(vg, "ui-monospace", kConsolas, kConsolasBold,
                               kConsolasItalic, kConsolasBoldItalic);
    register_font_alias_family(vg, "Courier New", "C:/Windows/Fonts/cour.ttf",
                               "C:/Windows/Fonts/courbd.ttf",
                               "C:/Windows/Fonts/couri.ttf",
                               "C:/Windows/Fonts/courbi.ttf");
#elif defined(__APPLE__)
    constexpr const char* kHelvetica = "/System/Library/Fonts/Helvetica.ttc";
    register_font_alias_family(vg, "-apple-system", kHelvetica, nullptr,
                               nullptr, nullptr);
    register_font_alias_family(vg, "BlinkMacSystemFont", kHelvetica, nullptr,
                               nullptr, nullptr);
    register_font_alias_family(vg, "system-ui", kHelvetica, nullptr,
                               nullptr, nullptr);
    register_font_alias_family(vg, "ui-sans-serif", kHelvetica, nullptr,
                               nullptr, nullptr);
    constexpr const char* kMenlo = "/System/Library/Fonts/Menlo.ttc";
    register_font_alias_family(vg, "Menlo", kMenlo, nullptr, nullptr,
                               nullptr);
    register_font_alias_family(vg, "monospace", kMenlo, nullptr, nullptr,
                               nullptr);
    register_font_alias_family(vg, "ui-monospace", kMenlo, nullptr, nullptr,
                               nullptr);
#else
    register_font_alias_family(
        vg, "system-ui", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf");
    register_font_alias_family(
        vg, "ui-sans-serif", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf");
    register_font_alias_family(
        vg, "monospace",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Oblique.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-BoldOblique.ttf");
    register_font_alias_family(
        vg, "ui-monospace",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Oblique.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-BoldOblique.ttf");
#endif
}

class NanoVGPainter final : public Painter {
public:
    explicit NanoVGPainter(NVGcontext* vg) : vg_(vg) {}

    void begin_frame(int width, int height, float dpi_scale) override {
        width_  = width;
        height_ = height;
        nvgBeginFrame(vg_, static_cast<float>(width), static_cast<float>(height), dpi_scale);
    }

    void end_frame() override { nvgEndFrame(vg_); }

    void fill_rect(const Rect& r, Color c) override {
        nvgBeginPath(vg_);
        nvgRect(vg_, static_cast<float>(r.x), static_cast<float>(r.y),
                static_cast<float>(r.w), static_cast<float>(r.h));
        nvgFillColor(vg_, to_nvg(c));
        nvgFill(vg_);
    }

    void stroke_rect(const Rect& r, Color c, float thickness) override {
        nvgBeginPath(vg_);
        nvgRect(vg_, static_cast<float>(r.x), static_cast<float>(r.y),
                static_cast<float>(r.w), static_cast<float>(r.h));
        nvgStrokeColor(vg_, to_nvg(c));
        nvgStrokeWidth(vg_, thickness);
        nvgStroke(vg_);
    }

    void stroke_line(float x0, float y0, float x1, float y1,
                     Color c, float w) override {
        nvgBeginPath(vg_);
        nvgMoveTo(vg_, x0, y0);
        nvgLineTo(vg_, x1, y1);
        nvgStrokeColor(vg_, to_nvg(c));
        nvgStrokeWidth(vg_, w);
        // Square caps place the stroke end exactly at the endpoint, which
        // gives clean corners when adjacent border sides are drawn separately.
        nvgLineCap(vg_, NVG_SQUARE);
        nvgStroke(vg_);
    }

    void fill_circle(float cx, float cy, float radius, Color c) override {
        nvgBeginPath(vg_);
        nvgCircle(vg_, cx, cy, radius);
        nvgFillColor(vg_, to_nvg(c));
        nvgFill(vg_);
    }

    // CSS-clock-convention: 0 = top (12 o'clock), increasing clockwise.
    // NanoVG uses standard math angles (0 = right, CCW positive), and its
    // Y axis points down so CCW in screen space is actually CW visually.
    // Mapping: css_deg → nvg_rad = (css_deg - 90) * pi/180 with CW winding.
    void stroke_arc(float cx, float cy, float radius,
                    float angle_start_deg, float angle_end_deg,
                    Color c, float w) override {
        constexpr float kPi = 3.14159265358979323846f;
        const float a0 = (angle_start_deg - 90.0f) * kPi / 180.0f;
        const float a1 = (angle_end_deg   - 90.0f) * kPi / 180.0f;
        nvgBeginPath(vg_);
        // NVG_CW produces the clockwise arc in screen-space (Y-down coords).
        nvgArc(vg_, cx, cy, radius, a0, a1, NVG_CW);
        nvgStrokeColor(vg_, to_nvg(c));
        nvgStrokeWidth(vg_, w);
        nvgLineCap(vg_, NVG_ROUND);
        nvgStroke(vg_);
    }

    // Walk a flat path command stream (kPathMove/kPathLine/kPathCubic/
    // kPathClose) into the current NanoVG path. Malformed streams stop
    // at the first short segment rather than reading out of bounds.
    void build_path(const float* c, std::size_t n) {
        nvgBeginPath(vg_);
        std::size_t i = 0;
        while (i < n) {
            const float verb = c[i];
            if (verb == kPathMove) {
                if (i + 3 > n) break;
                nvgMoveTo(vg_, c[i + 1], c[i + 2]);
                i += 3;
            } else if (verb == kPathLine) {
                if (i + 3 > n) break;
                nvgLineTo(vg_, c[i + 1], c[i + 2]);
                i += 3;
            } else if (verb == kPathCubic) {
                if (i + 7 > n) break;
                nvgBezierTo(vg_, c[i + 1], c[i + 2], c[i + 3], c[i + 4],
                            c[i + 5], c[i + 6]);
                i += 7;
            } else if (verb == kPathClose) {
                nvgClosePath(vg_);
                i += 1;
            } else {
                break;
            }
        }
    }

    // Multi-stop gradients render through a cached LUT texture wrapped
    // in an image pattern (the default sampler is clamp-to-edge, which
    // gives the CSS/SVG "pad" spread for free). Two-stop gradients use
    // NanoVG's native analytic paints.
    std::uint64_t gradient_lut_key(const PathPaint& p) const {
        std::uint64_t h = 0xcbf29ce484222325ull;
        const auto mix = [&h](const void* data, std::size_t n) {
            const auto* b = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < n; ++i) {
                h ^= b[i];
                h *= 0x100000001b3ull;
            }
        };
        const std::uint8_t kind = static_cast<std::uint8_t>(p.kind);
        mix(&kind, 1);
        mix(&p.stop_count, 1);
        mix(p.offsets, sizeof(float) * p.stop_count);
        mix(p.colors, sizeof(Color) * p.stop_count);
        if (p.kind == PathPaint::Kind::Radial) {
            // The 2D LUT bakes the inner/outer radius ratio.
            const float ratio = p.r1 > 0.0f ? p.r0 / p.r1 : 0.0f;
            mix(&ratio, sizeof(ratio));
        }
        return h;
    }

    int gradient_lut_image(const PathPaint& p) {
        const std::uint64_t key = gradient_lut_key(p);
        if (const auto it = gradient_luts_.find(key);
            it != gradient_luts_.end()) {
            return it->second;
        }
        int image = 0;
        if (p.kind == PathPaint::Kind::Linear) {
            constexpr int kW = 256;
            std::vector<std::uint8_t> px(kW * 4);
            for (int x = 0; x < kW; ++x) {
                const Color c =
                    p.sample(static_cast<float>(x) /
                             static_cast<float>(kW - 1));
                px[x * 4 + 0] = c.r;
                px[x * 4 + 1] = c.g;
                px[x * 4 + 2] = c.b;
                px[x * 4 + 3] = c.a;
            }
            image = nvgCreateImageRGBA(vg_, kW, 1, 0, px.data());
        } else {
            // Radial: bake a 2D disc. Texture spans the outer circle's
            // bounding square; t maps texel distance through [r0, r1].
            constexpr int kW = 256;
            const float ratio = p.r1 > 0.0f
                                    ? std::clamp(p.r0 / p.r1, 0.0f, 1.0f)
                                    : 0.0f;
            std::vector<std::uint8_t> px(kW * kW * 4);
            const float half = static_cast<float>(kW - 1) * 0.5f;
            for (int y = 0; y < kW; ++y) {
                for (int x = 0; x < kW; ++x) {
                    const float dx = (static_cast<float>(x) - half) / half;
                    const float dy = (static_cast<float>(y) - half) / half;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    const float span = 1.0f - ratio;
                    const float t = span <= 0.0f
                                        ? 1.0f
                                        : (d - ratio) / span;
                    const Color c = p.sample(std::clamp(t, 0.0f, 1.0f));
                    const std::size_t o =
                        (static_cast<std::size_t>(y) * kW + x) * 4;
                    px[o + 0] = c.r;
                    px[o + 1] = c.g;
                    px[o + 2] = c.b;
                    px[o + 3] = c.a;
                }
            }
            image = nvgCreateImageRGBA(vg_, kW, kW, 0, px.data());
        }
        gradient_luts_.emplace(key, image);
        return image;
    }

    NVGpaint gradient_paint(const PathPaint& p) {
        const Color c0 = p.colors[0];
        const Color c1 = p.colors[p.stop_count > 0 ? p.stop_count - 1 : 0];
        if (p.stop_count > 2) {
            const int image = gradient_lut_image(p);
            if (image != 0) {
                if (p.kind == PathPaint::Kind::Linear) {
                    const float dx = p.x1 - p.x0;
                    const float dy = p.y1 - p.y0;
                    const float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 1e-6f) {
                        const float angle = std::atan2(dy, dx);
                        // Pattern rect: gradient axis along local +x,
                        // one texel row stretched across a large
                        // perpendicular extent (clamped anyway).
                        return nvgImagePattern(vg_, p.x0, p.y0 - 5000.0f,
                                               len, 10000.0f, angle, image,
                                               1.0f);
                    }
                } else if (p.r1 > 0.0f) {
                    return nvgImagePattern(vg_, p.x0 - p.r1, p.y0 - p.r1,
                                           p.r1 * 2.0f, p.r1 * 2.0f, 0.0f,
                                           image, 1.0f);
                }
            }
        }
        if (p.kind == PathPaint::Kind::Radial) {
            return nvgRadialGradient(vg_, p.x0, p.y0, p.r0, p.r1,
                                     to_nvg(c0), to_nvg(c1));
        }
        return nvgLinearGradient(vg_, p.x0, p.y0, p.x1, p.y1,
                                 to_nvg(c0), to_nvg(c1));
    }

    void fill_path(const float* cmds, std::size_t count,
                   const PathPaint& paint) override {
        if (cmds == nullptr || count == 0) return;
        build_path(cmds, count);
        if (paint.kind == PathPaint::Kind::Solid) {
            nvgFillColor(vg_, to_nvg(paint.colors[0]));
        } else {
            nvgFillPaint(vg_, gradient_paint(paint));
        }
        nvgFill(vg_);
    }

    void stroke_path(const float* cmds, std::size_t count,
                     const PathPaint& paint, float width,
                     LineCap cap, LineJoin join) override {
        if (cmds == nullptr || count == 0 || width <= 0.0f) return;
        build_path(cmds, count);
        if (paint.kind == PathPaint::Kind::Solid) {
            nvgStrokeColor(vg_, to_nvg(paint.colors[0]));
        } else {
            nvgStrokePaint(vg_, gradient_paint(paint));
        }
        nvgStrokeWidth(vg_, width);
        nvgLineCap(vg_, cap == LineCap::Round    ? NVG_ROUND
                        : cap == LineCap::Square ? NVG_SQUARE
                                                 : NVG_BUTT);
        nvgLineJoin(vg_, join == LineJoin::Round   ? NVG_ROUND
                         : join == LineJoin::Bevel ? NVG_BEVEL
                                                   : NVG_MITER);
        nvgStroke(vg_);
    }

    // CSS clamps border-radius UNIFORMLY to min(w,h)/2, keeping corners
    // circular — a huge radius (e.g. `border-radius:50rem` for a pill)
    // becomes a pill (semicircular ends), not an ellipse. NanoVG instead
    // clamps per-axis (rx→w/2, ry→h/2 independently), which turns a large
    // radius into a full ellipse. So pre-clamp here.
    float clamp_radius(float radius, int w, int h) const {
        const float maxr = 0.5f * static_cast<float>(std::min(w, h));
        return radius < maxr ? radius : maxr;
    }

    void clamp_radii(const Rect& r, float& tl, float& tr,
                     float& br, float& bl) const {
        const float maxr = 0.5f * static_cast<float>(std::min(r.w, r.h));
        tl = std::min(tl, maxr);
        tr = std::min(tr, maxr);
        br = std::min(br, maxr);
        bl = std::min(bl, maxr);
    }

    void path_rounded_rect(float x, float y, float w, float h,
                           float radius) {
        if (radius > 0.0f) nvgRoundedRect(vg_, x, y, w, h, radius);
        else               nvgRect(vg_, x, y, w, h);
    }

    bool snap_rect_to_pixel_grid(float& x, float& y, float& w, float& h) {
        float xform[6]{};
        nvgCurrentTransform(vg_, xform);
        constexpr float eps = 1e-4f;
        if (std::abs(xform[0] - 1.0f) > eps ||
            std::abs(xform[1]) > eps ||
            std::abs(xform[2]) > eps ||
            std::abs(xform[3] - 1.0f) > eps) {
            return false;
        }

        const float tx = xform[4];
        const float ty = xform[5];
        const float x0 = std::round(x + tx) - tx;
        const float y0 = std::round(y + ty) - ty;
        const float x1 = std::round(x + w + tx) - tx;
        const float y1 = std::round(y + h + ty) - ty;
        x = x0;
        y = y0;
        w = std::max(0.0f, x1 - x0);
        h = std::max(0.0f, y1 - y0);
        return true;
    }

    void fill_hard_inset_shadow(const Rect& r, float radius, Color color,
                                float offset_x, float offset_y,
                                float spread) {
        const float bx = static_cast<float>(r.x);
        const float by = static_cast<float>(r.y);
        const float bw = static_cast<float>(r.w);
        const float bh = static_cast<float>(r.h);
        const float ix = bx + offset_x + spread;
        const float iy = by + offset_y + spread;
        const float iw = bw - spread * 2.0f;
        const float ih = bh - spread * 2.0f;
        const float ir = std::max(0.0f, radius - spread);

        nvgBeginPath(vg_);
        path_rounded_rect(bx, by, bw, bh, radius);
        if (iw > 0.0f && ih > 0.0f) {
            path_rounded_rect(ix, iy, iw, ih, ir);
            nvgPathWinding(vg_, NVG_HOLE);
        }
        nvgFillColor(vg_, to_nvg(color));
        nvgFill(vg_);
    }

    bool fill_axis_aligned_hard_inset_shadow(const Rect& r, float radius,
                                             Color color, float offset_x,
                                             float offset_y, float spread) {
        if (spread != 0.0f) return false;
        if ((offset_x == 0.0f) == (offset_y == 0.0f)) return false;

        const float bx = static_cast<float>(r.x);
        const float by = static_cast<float>(r.y);
        const float bw = static_cast<float>(r.w);
        const float bh = static_cast<float>(r.h);
        if (bw <= 0.0f || bh <= 0.0f) return true;

        float x = bx;
        float y = by;
        float w = bw;
        float h = bh;
        float tl = 0.0f;
        float tr = 0.0f;
        float br = 0.0f;
        float bl = 0.0f;

        if (offset_y > 0.0f) {
            h = std::min(offset_y, bh);
            tl = radius;
            tr = radius;
        } else if (offset_y < 0.0f) {
            h = std::min(-offset_y, bh);
            y = by + bh - h;
            br = radius;
            bl = radius;
        } else if (offset_x > 0.0f) {
            w = std::min(offset_x, bw);
            tl = radius;
            bl = radius;
        } else {
            w = std::min(-offset_x, bw);
            x = bx + bw - w;
            tr = radius;
            br = radius;
        }

        if (w <= 0.0f || h <= 0.0f) return true;
        snap_rect_to_pixel_grid(x, y, w, h);
        const float maxr = 0.5f * std::min(w, h);
        tl = std::min(tl, maxr);
        tr = std::min(tr, maxr);
        br = std::min(br, maxr);
        bl = std::min(bl, maxr);

        nvgBeginPath(vg_);
        if (tl == 0.0f && tr == 0.0f && br == 0.0f && bl == 0.0f) {
            nvgRect(vg_, x, y, w, h);
        } else {
            nvgRoundedRectVarying(vg_, x, y, w, h, tl, tr, br, bl);
        }
        nvgFillColor(vg_, to_nvg(color));
        nvgFill(vg_);
        return true;
    }

    void fill_hard_outset_shadow(const Rect& r, float radius, Color color,
                                 float offset_x, float offset_y,
                                 float spread) {
        const float bx = static_cast<float>(r.x);
        const float by = static_cast<float>(r.y);
        const float bw = static_cast<float>(r.w);
        const float bh = static_cast<float>(r.h);
        const float sx = bx + offset_x - spread;
        const float sy = by + offset_y - spread;
        const float sw = bw + spread * 2.0f;
        const float sh = bh + spread * 2.0f;
        const float sr = std::max(0.0f, radius + spread);
        if (sw <= 0.0f || sh <= 0.0f) return;

        nvgBeginPath(vg_);
        path_rounded_rect(sx, sy, sw, sh, sr);
        path_rounded_rect(bx, by, bw, bh, radius);
        nvgPathWinding(vg_, NVG_HOLE);
        nvgFillColor(vg_, to_nvg(color));
        nvgFill(vg_);
    }

    static bool point_in_rounded_rect(float px, float py,
                                      float x, float y, float w, float h,
                                      float radius) {
        if (w <= 0.0f || h <= 0.0f) return false;
        if (px < x || py < y || px >= x + w || py >= y + h) return false;
        radius = std::max(0.0f, std::min(radius, 0.5f * std::min(w, h)));
        if (radius <= 0.0f) return true;
        const float cx = std::clamp(px, x + radius, x + w - radius);
        const float cy = std::clamp(py, y + radius, y + h - radius);
        const float dx = px - cx;
        const float dy = py - cy;
        return dx * dx + dy * dy <= radius * radius;
    }

    static void blur_mask(std::vector<float>& mask, int w, int h, float sigma) {
        if (w <= 0 || h <= 0 || sigma <= 0.0f) return;
        const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
        std::vector<float> kernel(static_cast<std::size_t>(radius * 2 + 1));
        float sum = 0.0f;
        for (int i = -radius; i <= radius; ++i) {
            const float v = std::exp(-(static_cast<float>(i * i)) /
                                     (2.0f * sigma * sigma));
            kernel[static_cast<std::size_t>(i + radius)] = v;
            sum += v;
        }
        if (sum <= 0.0f) return;
        for (float& v : kernel) v /= sum;

        std::vector<float> tmp(mask.size(), 0.0f);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float acc = 0.0f;
                for (int k = -radius; k <= radius; ++k) {
                    const int sx = std::clamp(x + k, 0, w - 1);
                    acc += mask[static_cast<std::size_t>(y * w + sx)] *
                           kernel[static_cast<std::size_t>(k + radius)];
                }
                tmp[static_cast<std::size_t>(y * w + x)] = acc;
            }
        }

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float acc = 0.0f;
                for (int k = -radius; k <= radius; ++k) {
                    const int sy = std::clamp(y + k, 0, h - 1);
                    acc += tmp[static_cast<std::size_t>(sy * w + x)] *
                           kernel[static_cast<std::size_t>(k + radius)];
                }
                mask[static_cast<std::size_t>(y * w + x)] = acc;
            }
        }
    }

    static void hash_mix(std::uint64_t& h, std::uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    }

    static int quantize64(float v) {
        return static_cast<int>(std::lround(v * 64.0f));
    }

    bool fill_raster_box_shadow(const Rect& r, float radius, Color color,
                                float offset_x, float offset_y,
                                float blur, float spread, bool inset) {
        if (blur <= 0.0f || color.a == 0) return false;

        const float bx = static_cast<float>(r.x);
        const float by = static_cast<float>(r.y);
        const float bw = static_cast<float>(r.w);
        const float bh = static_cast<float>(r.h);
        const float sigma = blur * 0.5f;
        const float padf = std::ceil(blur * 2.0f);
        const int pad = static_cast<int>(std::max(1.0f, padf));

        float origin_x = bx;
        float origin_y = by;
        int img_w = std::max(1, r.w);
        int img_h = std::max(1, r.h);

        float shadow_x = 0.0f;
        float shadow_y = 0.0f;
        float shadow_w = bw;
        float shadow_h = bh;
        float shadow_radius = radius;
        float clip_x = 0.0f;
        float clip_y = 0.0f;
        float clip_w = bw;
        float clip_h = bh;
        float clip_radius = radius;

        if (!inset) {
            shadow_x = offset_x - spread;
            shadow_y = offset_y - spread;
            shadow_w = bw + spread * 2.0f;
            shadow_h = bh + spread * 2.0f;
            shadow_radius = std::max(0.0f, radius + spread);
            if (shadow_w <= 0.0f || shadow_h <= 0.0f) return false;

            origin_x = std::floor(bx + shadow_x - static_cast<float>(pad));
            origin_y = std::floor(by + shadow_y - static_cast<float>(pad));
            img_w = std::max(1, static_cast<int>(
                std::ceil(shadow_w + static_cast<float>(pad * 2))));
            img_h = std::max(1, static_cast<int>(
                std::ceil(shadow_h + static_cast<float>(pad * 2))));

            shadow_x = bx + shadow_x - origin_x;
            shadow_y = by + shadow_y - origin_y;
            clip_x = bx - origin_x;
            clip_y = by - origin_y;
        } else {
            shadow_x = offset_x + spread;
            shadow_y = offset_y + spread;
            shadow_w = bw - spread * 2.0f;
            shadow_h = bh - spread * 2.0f;
            shadow_radius = std::max(0.0f, radius - spread);
        }

        // Raster shadows are excellent for small 3D controls because their
        // exact pixel mask is cached and reused. They are the wrong tool for
        // large responsive panels: every 1px resize creates a new large mask,
        // allocates two temporary blur buffers, and spends CPU on pixels that
        // browsers normally handle with a cached/procedural shadow path. Keep
        // the raster path for control-sized shadows and let NanoVG's analytic
        // box gradient handle large/card-like shadows.
        constexpr int kMaxRasterShadowDim = 256;
        constexpr int kMaxRasterShadowArea = 96 * 1024;
        if (img_w > kMaxRasterShadowDim || img_h > kMaxRasterShadowDim ||
            img_w * img_h > kMaxRasterShadowArea) {
            return false;
        }

        std::uint64_t key = inset ? 0x9f46e997b4a7c15ull
                                  : 0x6a09e667f3bcc909ull;
        hash_mix(key, static_cast<std::uint32_t>(img_w));
        hash_mix(key, static_cast<std::uint32_t>(img_h));
        hash_mix(key, static_cast<std::uint32_t>(color.r) << 24 |
                      static_cast<std::uint32_t>(color.g) << 16 |
                      static_cast<std::uint32_t>(color.b) << 8 |
                      static_cast<std::uint32_t>(color.a));
        hash_mix(key, static_cast<std::uint32_t>(quantize64(radius)));
        hash_mix(key, static_cast<std::uint32_t>(quantize64(offset_x)));
        hash_mix(key, static_cast<std::uint32_t>(quantize64(offset_y)));
        hash_mix(key, static_cast<std::uint32_t>(quantize64(blur)));
        hash_mix(key, static_cast<std::uint32_t>(quantize64(spread)));
        hash_mix(key, static_cast<std::uint32_t>(quantize64(shadow_x)));
        hash_mix(key, static_cast<std::uint32_t>(quantize64(shadow_y)));
        hash_mix(key, static_cast<std::uint32_t>(quantize64(shadow_w)));
        hash_mix(key, static_cast<std::uint32_t>(quantize64(shadow_h)));

        int image = 0;
        if (auto it = shadow_cache_.find(key); it != shadow_cache_.end()) {
            image = it->second;
        } else {
            std::vector<float> mask(
                static_cast<std::size_t>(img_w * img_h), 0.0f);
            for (int y = 0; y < img_h; ++y) {
                for (int x = 0; x < img_w; ++x) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float py = static_cast<float>(y) + 0.5f;
                    const bool in_shadow = point_in_rounded_rect(
                        px, py, shadow_x, shadow_y, shadow_w, shadow_h,
                        shadow_radius);
                    if (!inset) {
                        mask[static_cast<std::size_t>(y * img_w + x)] =
                            in_shadow ? 1.0f : 0.0f;
                    } else {
                        const bool in_clip = point_in_rounded_rect(
                            px, py, clip_x, clip_y, clip_w, clip_h,
                            clip_radius);
                        mask[static_cast<std::size_t>(y * img_w + x)] =
                            (in_clip && !in_shadow) ? 1.0f : 0.0f;
                    }
                }
            }

            blur_mask(mask, img_w, img_h, sigma);

            std::vector<unsigned char> pixels(
                static_cast<std::size_t>(img_w * img_h * 4), 0);
            for (int y = 0; y < img_h; ++y) {
                for (int x = 0; x < img_w; ++x) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float py = static_cast<float>(y) + 0.5f;
                    if (!inset && point_in_rounded_rect(
                            px, py, clip_x, clip_y, clip_w, clip_h,
                            clip_radius)) {
                        continue;
                    }
                    if (inset && !point_in_rounded_rect(
                            px, py, clip_x, clip_y, clip_w, clip_h,
                            clip_radius)) {
                        continue;
                    }
                    const std::size_t mi =
                        static_cast<std::size_t>(y * img_w + x);
                    const std::uint8_t a = static_cast<std::uint8_t>(
                        std::clamp(std::lround(mask[mi] *
                                               static_cast<float>(color.a)),
                                   0l, 255l));
                    const std::size_t off = mi * 4;
                    pixels[off + 0] = color.r;
                    pixels[off + 1] = color.g;
                    pixels[off + 2] = color.b;
                    pixels[off + 3] = a;
                }
            }

            image = nvgCreateImageRGBA(vg_, img_w, img_h, 0, pixels.data());
            if (image <= 0) return false;
            shadow_cache_[key] = image;
        }

        NVGpaint paint = nvgImagePattern(
            vg_, origin_x, origin_y, static_cast<float>(img_w),
            static_cast<float>(img_h), 0.0f, image, 1.0f);
        nvgBeginPath(vg_);
        if (inset) {
            path_rounded_rect(bx, by, bw, bh, radius);
        } else {
            nvgRect(vg_, origin_x, origin_y, static_cast<float>(img_w),
                    static_cast<float>(img_h));
        }
        nvgFillPaint(vg_, paint);
        nvgFill(vg_);
        return true;
    }

    void fill_rounded_rect(const Rect& r, float radius, Color c) override {
        radius = clamp_radius(radius, r.w, r.h);
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, static_cast<float>(r.x), static_cast<float>(r.y),
                       static_cast<float>(r.w), static_cast<float>(r.h), radius);
        nvgFillColor(vg_, to_nvg(c));
        nvgFill(vg_);
    }

    void stroke_rounded_rect(const Rect& r, float radius, Color c, float w) override {
        radius = clamp_radius(radius, r.w, r.h);
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, static_cast<float>(r.x), static_cast<float>(r.y),
                       static_cast<float>(r.w), static_cast<float>(r.h), radius);
        nvgStrokeColor(vg_, to_nvg(c));
        nvgStrokeWidth(vg_, w);
        nvgStroke(vg_);
    }

    void fill_rounded_rect_varying(const Rect& r,
                                   float tl, float tr, float br, float bl,
                                   Color c) override {
        clamp_radii(r, tl, tr, br, bl);
        nvgBeginPath(vg_);
        nvgRoundedRectVarying(vg_,
            static_cast<float>(r.x), static_cast<float>(r.y),
            static_cast<float>(r.w), static_cast<float>(r.h),
            tl, tr, br, bl);
        nvgFillColor(vg_, to_nvg(c));
        nvgFill(vg_);
    }

    void stroke_rounded_rect_varying(const Rect& r,
                                     float tl, float tr, float br, float bl,
                                     Color c, float w) override {
        clamp_radii(r, tl, tr, br, bl);
        nvgBeginPath(vg_);
        nvgRoundedRectVarying(vg_,
            static_cast<float>(r.x), static_cast<float>(r.y),
            static_cast<float>(r.w), static_cast<float>(r.h),
            tl, tr, br, bl);
        nvgStrokeColor(vg_, to_nvg(c));
        nvgStrokeWidth(vg_, w);
        nvgStroke(vg_);
    }

    void fill_rounded_rect_ring(const Rect& r, float radius,
                                float thickness, Color color) override {
        if (thickness <= 0.0f) return;
        radius = clamp_radius(radius, r.w, r.h);
        fill_hard_inset_shadow(r, radius, color, 0.0f, 0.0f, thickness);
    }

    void fill_linear_gradient_rect(const Rect& r, float angle_deg,
                                   Color stop0, Color stop1,
                                   float tl, float tr, float br, float bl) override {
        clamp_radii(r, tl, tr, br, bl);

        // Convert CSS angle to gradient start/end points.
        // CSS: 0deg = upward, 90deg = rightward, 180deg = downward.
        // We derive a start point and end point spanning the bounding box.
        const float fx = static_cast<float>(r.x);
        const float fy = static_cast<float>(r.y);
        const float fw = static_cast<float>(r.w);
        const float fh = static_cast<float>(r.h);
        const float cx = fx + fw * 0.5f;
        const float cy = fy + fh * 0.5f;

        // angle_deg is CSS convention: 0=upward, clockwise.
        // Convert to standard math angle (anticlockwise from positive X):
        //   CSS 0  → point upward   → math_angle = 90  → sin=1, cos=0 → dx=0,dy=-1
        //   CSS 90 → point rightward → math_angle = 0  → dx=1, dy=0
        //   CSS 180→ point downward  → math_angle = -90 → dx=0, dy=1
        const float rad = angle_deg * (3.14159265358979323846f / 180.0f);
        // In CSS: the line is drawn in direction (sin(angle), -cos(angle))
        // from the start to the end of the gradient.
        const float dx = std::sin(rad);
        const float dy = -std::cos(rad);

        // Scale to cover the full bounding box (use half-diagonal projection).
        // The gradient runs from center-minus-half-extent to center+half-extent.
        // Extent: max of |dx*w/2 + dy*h/2| makes it cover the box.
        const float ext = std::abs(dx) * (fw * 0.5f) + std::abs(dy) * (fh * 0.5f);

        const float sx = cx - dx * ext;
        const float sy = cy - dy * ext;
        const float ex = cx + dx * ext;
        const float ey = cy + dy * ext;

        NVGpaint paint = nvgLinearGradient(vg_, sx, sy, ex, ey,
                                           to_nvg(stop0), to_nvg(stop1));
        nvgBeginPath(vg_);
        const float all_same = (tl == tr && tr == br && br == bl);
        if (all_same && tl == 0.0f) {
            nvgRect(vg_, fx, fy, fw, fh);
        } else if (all_same) {
            nvgRoundedRect(vg_, fx, fy, fw, fh, tl);
        } else {
            nvgRoundedRectVarying(vg_, fx, fy, fw, fh, tl, tr, br, bl);
        }
        nvgFillPaint(vg_, paint);
        nvgFill(vg_);
    }

    void fill_radial_gradient_rect(const Rect& r,
                                   Color stop0, Color stop1,
                                   float tl, float tr, float br, float bl,
                                   float center_x_pct, float center_y_pct,
                                   float stop1_pos_pct) override {
        clamp_radii(r, tl, tr, br, bl);

        const float fx = static_cast<float>(r.x);
        const float fy = static_cast<float>(r.y);
        const float fw = static_cast<float>(r.w);
        const float fh = static_cast<float>(r.h);
        const float cx = fx + fw * (std::clamp(center_x_pct, 0.0f, 100.0f) / 100.0f);
        const float cy = fy + fh * (std::clamp(center_y_pct, 0.0f, 100.0f) / 100.0f);
        // CSS `farthest-corner`: radius reaches the farthest corner from
        // the requested center point.
        const float dx = std::max(cx - fx, fx + fw - cx);
        const float dy = std::max(cy - fy, fy + fh - cy);
        const float farthest_r = std::sqrt(dx * dx + dy * dy);
        const float outer_r = farthest_r *
            (std::clamp(stop1_pos_pct, 1.0f, 100.0f) / 100.0f);

        NVGpaint paint = nvgRadialGradient(vg_, cx, cy, 0.0f, outer_r,
                                           to_nvg(stop0), to_nvg(stop1));
        nvgBeginPath(vg_);
        const bool all_same = (tl == tr && tr == br && br == bl);
        if (all_same && tl == 0.0f) {
            nvgRect(vg_, fx, fy, fw, fh);
        } else if (all_same) {
            nvgRoundedRect(vg_, fx, fy, fw, fh, tl);
        } else {
            nvgRoundedRectVarying(vg_, fx, fy, fw, fh, tl, tr, br, bl);
        }
        nvgFillPaint(vg_, paint);
        nvgFill(vg_);
    }

    void fill_linear_stripes_rect(const Rect& r, float angle_deg,
                                  Color stripe, float tile_size,
                                  float tl, float tr, float br, float bl) override {
        clamp_radii(r, tl, tr, br, bl);

        const int tile = std::clamp(
            static_cast<int>(std::round(tile_size)), 1, 128);
        const int angle_bucket =
            ((static_cast<int>(std::round(angle_deg)) % 360) + 360) % 360;
        const std::uint32_t rgba =
            (std::uint32_t(stripe.r) << 24) |
            (std::uint32_t(stripe.g) << 16) |
            (std::uint32_t(stripe.b) << 8) |
            std::uint32_t(stripe.a);
        const std::uint64_t key =
            (std::uint64_t(tile) << 48) |
            (std::uint64_t(angle_bucket) << 32) |
            std::uint64_t(rgba);

        int image = 0;
        if (auto it = stripe_cache_.find(key); it != stripe_cache_.end()) {
            image = it->second;
        } else {
            std::vector<unsigned char> pixels(
                static_cast<std::size_t>(tile * tile * 4), 0);
            const float rad = static_cast<float>(angle_bucket) *
                (3.14159265358979323846f / 180.0f);
            const float dx = std::sin(rad);
            const float dy = -std::cos(rad);
            for (int y = 0; y < tile; ++y) {
                for (int x = 0; x < tile; ++x) {
                    float phase = (static_cast<float>(x) * dx +
                                   static_cast<float>(y) * dy) /
                                  static_cast<float>(tile);
                    phase -= std::floor(phase);
                    const bool on =
                        phase < 0.25f || (phase >= 0.5f && phase < 0.75f);
                    if (!on) continue;
                    const std::size_t off =
                        static_cast<std::size_t>((y * tile + x) * 4);
                    pixels[off + 0] = stripe.r;
                    pixels[off + 1] = stripe.g;
                    pixels[off + 2] = stripe.b;
                    pixels[off + 3] = stripe.a;
                }
            }
            image = nvgCreateImageRGBA(vg_, tile, tile,
                                       NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY,
                                       pixels.data());
            if (image <= 0) return;
            stripe_cache_[key] = image;
        }

        const float fx = static_cast<float>(r.x);
        const float fy = static_cast<float>(r.y);
        const float fw = static_cast<float>(r.w);
        const float fh = static_cast<float>(r.h);
        NVGpaint paint = nvgImagePattern(vg_, fx, fy,
                                         static_cast<float>(tile),
                                         static_cast<float>(tile),
                                         0.0f, image, 1.0f);
        nvgBeginPath(vg_);
        const bool all_same = (tl == tr && tr == br && br == bl);
        if (all_same && tl == 0.0f) {
            nvgRect(vg_, fx, fy, fw, fh);
        } else if (all_same) {
            nvgRoundedRect(vg_, fx, fy, fw, fh, tl);
        } else {
            nvgRoundedRectVarying(vg_, fx, fy, fw, fh, tl, tr, br, bl);
        }
        nvgFillPaint(vg_, paint);
        nvgFill(vg_);
    }

    void fill_grid_rect(const Rect& r, Color line, float tile_size,
                        float line_width, float tl, float tr,
                        float br, float bl) override {
        clamp_radii(r, tl, tr, br, bl);

        const int tile = std::clamp(
            static_cast<int>(std::round(tile_size)), 1, 256);
        const int lw = std::clamp(
            static_cast<int>(std::round(line_width)), 1, tile);
        const std::uint32_t rgba =
            (std::uint32_t(line.r) << 24) |
            (std::uint32_t(line.g) << 16) |
            (std::uint32_t(line.b) << 8) |
            std::uint32_t(line.a);
        const std::uint64_t key =
            (std::uint64_t(tile) << 48) |
            (std::uint64_t(lw) << 32) |
            std::uint64_t(rgba);

        int image = 0;
        if (auto it = grid_cache_.find(key); it != grid_cache_.end()) {
            image = it->second;
        } else {
            std::vector<unsigned char> pixels(
                static_cast<std::size_t>(tile * tile * 4), 0);
            for (int y = 0; y < tile; ++y) {
                for (int x = 0; x < tile; ++x) {
                    if (x >= lw && y >= lw) continue;
                    const std::size_t off =
                        static_cast<std::size_t>((y * tile + x) * 4);
                    pixels[off + 0] = line.r;
                    pixels[off + 1] = line.g;
                    pixels[off + 2] = line.b;
                    pixels[off + 3] = line.a;
                }
            }
            image = nvgCreateImageRGBA(vg_, tile, tile,
                                       NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY,
                                       pixels.data());
            if (image <= 0) return;
            grid_cache_[key] = image;
        }

        const float fx = static_cast<float>(r.x);
        const float fy = static_cast<float>(r.y);
        const float fw = static_cast<float>(r.w);
        const float fh = static_cast<float>(r.h);
        NVGpaint paint = nvgImagePattern(vg_, fx, fy,
                                         static_cast<float>(tile),
                                         static_cast<float>(tile),
                                         0.0f, image, 1.0f);
        nvgBeginPath(vg_);
        const bool all_same = (tl == tr && tr == br && br == bl);
        if (all_same && tl == 0.0f) {
            nvgRect(vg_, fx, fy, fw, fh);
        } else if (all_same) {
            nvgRoundedRect(vg_, fx, fy, fw, fh, tl);
        } else {
            nvgRoundedRectVarying(vg_, fx, fy, fw, fh, tl, tr, br, bl);
        }
        nvgFillPaint(vg_, paint);
        nvgFill(vg_);
    }

    void fill_box_shadow(const Rect& r, float radius, Color color,
                         float offset_x, float offset_y,
                         float blur, float spread, bool inset) override {
        const float bx = static_cast<float>(r.x);
        const float by = static_cast<float>(r.y);
        const float bw = static_cast<float>(r.w);
        const float bh = static_cast<float>(r.h);
        const NVGcolor solid = to_nvg(color);
        const NVGcolor clear = nvgRGBA(color.r, color.g, color.b, 0);
        radius = clamp_radius(radius, r.w, r.h);

        if (blur <= 0.0f) {
            if (inset) {
                if (!fill_axis_aligned_hard_inset_shadow(
                        r, radius, color, offset_x, offset_y, spread)) {
                    fill_hard_inset_shadow(r, radius, color, offset_x,
                                           offset_y, spread);
                }
            } else {
                fill_hard_outset_shadow(r, radius, color, offset_x,
                                        offset_y, spread);
            }
            return;
        }
        if (fill_raster_box_shadow(r, radius, color, offset_x, offset_y,
                                   blur, spread, inset)) {
            return;
        }
        // CSS blur:0 is a hard-edged shadow. Blurred shadows still use
        // NanoVG's box gradient; widen its linear falloff to approximate
        // the softer browser Gaussian tail.
        const float feather = blur * 1.5f;

        if (!inset) {
            // Outset drop shadow: the shadow box is the border box shifted
            // by the offset and grown by `spread`. Painted before the box
            // background, so the solid interior is covered by the box.
            const float sx = bx + offset_x - spread;
            const float sy = by + offset_y - spread;
            const float sw = bw + spread * 2.0f;
            const float sh = bh + spread * 2.0f;
            const float srad = std::max(0.0f, radius + spread);
            NVGpaint paint = nvgBoxGradient(vg_, sx, sy, sw, sh, srad,
                                            feather, solid, clear);
            nvgBeginPath(vg_);
            // Cover the shadow box plus the feathered falloff on every side.
            nvgRect(vg_, sx - feather, sy - feather,
                    sw + feather * 2.0f, sh + feather * 2.0f);
            nvgFillPaint(vg_, paint);
            nvgFill(vg_);
            return;
        }

        // Inset shadow: shadow colour creeps inward from the edges. The
        // clear inner region is the border box shifted by the offset and
        // shrunk by `spread`; the gradient is transparent inside it and
        // fades to the shadow colour at its boundary. Clipped to the box.
        const float ix = bx + offset_x + spread;
        const float iy = by + offset_y + spread;
        const float iw = std::max(1.0f, bw - spread * 2.0f);
        const float ih = std::max(1.0f, bh - spread * 2.0f);
        const float irad = std::max(0.0f, radius - spread);
        NVGpaint paint = nvgBoxGradient(vg_, ix, iy, iw, ih, irad,
                                        feather, clear, solid);
        nvgBeginPath(vg_);
        if (radius > 0.0f) nvgRoundedRect(vg_, bx, by, bw, bh, radius);
        else               nvgRect(vg_, bx, by, bw, bh);
        nvgFillPaint(vg_, paint);
        nvgFill(vg_);
    }

    std::uint32_t resolve_font(std::string_view family,
                               int               size_px,
                               int               weight,
                               bool              italic) override {
        // NanoVG addresses fonts by face id + a per-frame size. We pack
        // (face_id, size) into a handle so draw_text/measure_text can
        // reconstitute it without another lookup.
        //
        // With only regular(400) and bold(700) faces available, browsers
        // resolve CSS weight 500 to the regular face. Synthetic emboldening
        // here made Bootstrap's 500-weight headings visibly heavier than
        // the browser reference, so only 600+ selects the bold face.
        //
        // Italic falls back through (italic + bold) → italic → bold →
        // regular. Each combo is its own face — if a system lacks an
        // italic variant the cascade silently uses the upright form
        // (NanoVG has no synthetic-oblique path either).
        const bool prefer_bold = weight >= 600;

        auto resolve_one = [&](const std::string& family_str) -> int {
            if (family_str.empty() ||
                is_unavailable_platform_alias(family_str)) {
                return -1;
            }

            int face = find_registered_font_face(family_str, weight, italic);
            if (face < 0 && italic && prefer_bold) {
                face = nvgFindFont(vg_, (family_str + "-bold-italic").c_str());
                if (face < 0 && is_generic_sans_family(family_str) &&
                    bold_italic_face_ >= 0) {
                    face = bold_italic_face_;
                }
            }
            if (face < 0 && italic) {
                face = nvgFindFont(vg_, (family_str + "-italic").c_str());
                if (face < 0 && is_generic_sans_family(family_str) &&
                    italic_face_ >= 0) {
                    face = italic_face_;
                }
            }
            if (face < 0 && prefer_bold) {
                face = nvgFindFont(vg_, (family_str + "-bold").c_str());
                if (face < 0 && is_generic_sans_family(family_str) &&
                    bold_face_ >= 0) {
                    face = bold_face_;
                }
            }
            if (face < 0) face = nvgFindFont(vg_, family_str.c_str());
            if (face < 0 && is_generic_mono_family(family_str)) {
                face = nvgFindFont(vg_, "monospace");
            }
            if (face < 0 && is_generic_sans_family(family_str)) {
                face = default_face_;
            }
            return face;
        };

        int face = -1;
        for (const auto& candidate : split_font_family_list(family)) {
            face = resolve_one(candidate);
            if (face >= 0) break;
        }
        if (face < 0) face = default_face_;
        if (face < 0) return 0;
        return pack_handle(static_cast<std::uint16_t>(face),
                           static_cast<std::uint16_t>(size_px));
    }

    bool register_font_face(std::string_view family,
                            int weight,
                            bool italic,
                            std::string_view bytes) override {
        if (family.empty() || bytes.empty()) return false;
        std::string decoded;
        std::string_view font_bytes = bytes;
        if (!is_raw_sfnt_font(font_bytes)) {
            decoded = decode_woff1_to_sfnt(bytes);
            if (decoded.empty()) return false;
            font_bytes = decoded;
        }

        weight = std::clamp(weight, 1, 1000);
        const auto key = font_face_key(family, weight, italic);
        if (registered_face_ids_.find(key) != registered_face_ids_.end())
            return true;

        auto copy = std::make_unique<unsigned char[]>(font_bytes.size());
        std::memcpy(copy.get(), font_bytes.data(), font_bytes.size());

        std::string nvg_name{"__aui_css_font_"};
        nvg_name.append(key);
        const int id = nvgCreateFontMem(
            vg_, nvg_name.c_str(), copy.get(),
            static_cast<int>(font_bytes.size()), /*freeData=*/0);
        if (id < 0) return false;

        registered_face_buffers_.push_back(std::move(copy));
        registered_face_ids_[key] = id;
        registered_faces_.push_back(
            RegisteredFontFace{std::string(family), weight, italic, id});
        return true;
    }

    int measure_text(std::uint32_t handle, std::string_view text) override {
        if (handle == 0) return 0;
        apply_handle(handle);
        float bounds[4] = {0, 0, 0, 0};
        const float advance = nvgTextBounds(vg_, 0.0f, 0.0f,
                                            text.data(), text.data() + text.size(),
                                            bounds);
        return static_cast<int>(std::ceil(advance));
    }

    TextMetrics text_metrics(std::uint32_t handle) override {
        TextMetrics m{};
        if (handle == 0) return m;
        // Browser-model (Skia-rounded) metrics — the same numbers layout,
        // measurement, and draw all use, so a CSS `normal` line box computed
        // here matches the box the glyphs are drawn into.
        apply_handle(handle);
        const FontVMetrics vm = font_vmetrics(handle);
        m.ascender    = vm.ascent;
        m.descender   = vm.descent;
        m.line_height = vm.line_height;
        return m;
    }

    void draw_text(std::uint32_t   handle,
                   const Point&    pos,
                   std::string_view text,
                   Color           color) override {
        if (handle == 0) return;
        apply_handle(handle);
        nvgFillColor(vg_, to_nvg(color));
        const float fx = static_cast<float>(pos.x);
        const float fy = static_cast<float>(pos.y);
        nvgText(vg_, fx, fy, text.data(), text.data() + text.size());
    }

    Size measure_text_box(std::uint32_t handle,
                          std::string_view text,
                          float max_width,
                          float line_height_mult,
                          float letter_spacing_px) override {
        if (handle == 0 || text.empty()) return {};
        apply_handle(handle);
        nvgTextLetterSpacing(vg_, letter_spacing_px);
        const float natural_line_h = natural_line_height_px(handle);
        const float css_line_h =
            css_line_height_px(handle, line_height_mult, natural_line_h);

        const bool has_newline = text.find('\n') != std::string_view::npos;

        // Natural single-line sizing is based on text advances, not ink
        // bounds. Browsers lay out inline content from advances; using
        // nvgTextBoxBounds here includes glyph overhangs and makes compact
        // inline-blocks such as Bootstrap badges several pixels too wide.
        if (!has_newline && max_width >= 60000.0f) {
            float bounds[4] = {0, 0, 0, 0};
            const float advance = nvgTextBounds(
                vg_, 0.0f, 0.0f,
                text.data(), text.data() + text.size(),
                bounds);
            nvgTextLetterSpacing(vg_, 0.0f);
            return Size{
                static_cast<int>(std::ceil(advance)),
                static_cast<int>(std::ceil(css_line_h)),
            };
        }

        // Pre-formatted text (max_width >= 60000 + has '\n'): measure each
        // line individually so that leading spaces in indented lines are
        // counted rather than skipped by nvgTextBoxBounds's word-wrap logic.
        // NOTE: 60000 is chosen so that the sentinel 65535 stored in the
        // display-list's uint16 max_width field also triggers this path.
        if (has_newline && max_width >= 60000.0f) {
            float max_line_w = 0.0f;
            int   line_count = 0;
            const char* p = text.data();
            const char* end = text.data() + text.size();
            while (p <= end) {
                const char* nl = static_cast<const char*>(
                    std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
                const char* line_end = nl ? nl : end;
                float bounds[4] = {0, 0, 0, 0};
                nvgTextBounds(vg_, 0.0f, 0.0f, p, line_end, bounds);
                const float lw = bounds[2] - bounds[0];
                if (lw > max_line_w) max_line_w = lw;
                ++line_count;
                if (!nl) break;
                p = nl + 1;
            }
            if (line_count == 0) line_count = 1;
            const float total_h = css_line_h * static_cast<float>(line_count);
            nvgTextLetterSpacing(vg_, 0.0f);
            return Size{
                static_cast<int>(std::ceil(max_line_w)),
                static_cast<int>(std::ceil(total_h)),
            };
        }

        nvgTextLineHeight(vg_, nvg_line_height_mult(css_line_h, natural_line_h));
        // by epsilon" and break the word onto a new line — invisible
        // Layout consumes CSS line boxes, not ink bounds. Count the rows that
        // NanoVG will paint at this width, then use row_count * css_line_h for
        // height so a single constrained line does not leave empty block space
        // below it.
        float max_row_w = 0.0f;
        int line_count = 0;
        const char* cursor = text.data();
        const char* const text_end = text.data() + text.size();
        NVGtextRow rows[8];
        while (cursor < text_end) {
            const int n = nvgTextBreakLines(vg_, cursor, text_end,
                                            max_width + kWrapEpsilonPx,
                                            rows, 8);
            if (n == 0) break;
            for (int i = 0; i < n; ++i) {
                if (rows[i].width > max_row_w) max_row_w = rows[i].width;
                ++line_count;
            }
            cursor = rows[n - 1].next;
        }
        const bool whitespace_only = text_is_whitespace_only(text);
        int width = static_cast<int>(std::ceil(max_row_w));
        if (width == 0 && whitespace_only) {
            float line_bounds[4] = {0, 0, 0, 0};
            const float advance = nvgTextBounds(
                vg_, 0.0f, 0.0f, text.data(), text.data() + text.size(),
                line_bounds);
            width = static_cast<int>(std::ceil(advance));
        }
        if (line_count == 0 && whitespace_only) line_count = 1;
        int height = static_cast<int>(
            std::ceil(css_line_h * static_cast<float>(std::max(line_count, 1))));
        // Reset letter spacing to default so subsequent draws are unaffected.
        nvgTextLetterSpacing(vg_, 0.0f);
        return Size{
            width,
            height,
        };
    }

    void draw_text_box(std::uint32_t handle,
                       float         pos_x,
                       float         pos_y,
                       std::string_view text,
                       Color         color,
                       float         max_width,
                       float         line_height_mult,
                       float         letter_spacing_px,
                       TextAlign     align) override {
        if (handle == 0 || text.empty()) return;
        apply_handle(handle);
        nvgTextLetterSpacing(vg_, letter_spacing_px);
        const float natural_line_h = natural_line_height_px(handle);
        const float css_line_h =
            css_line_height_px(handle, line_height_mult, natural_line_h);
        nvgFillColor(vg_, to_nvg(color));

        // nvgTextBox reads `state->textAlign` for halign before entering
        // its inner loop. For each row it computes:
        //   LEFT:   nvgText(x, ...)
        //   CENTER: nvgText(x + breakRowWidth/2 - row->width/2, ...)
        //   RIGHT:  nvgText(x + breakRowWidth - row->width, ...)
        // In all three cases x is the LEFT edge of the content column —
        // i.e. we pass the unchanged content origin. NanoVG does the
        // per-row offset arithmetic itself. Justify falls back to left
        // (NanoVG has no full justify mode).
        const float fx = pos_x;
        int nvg_halign = NVG_ALIGN_LEFT;
        switch (align) {
            case TextAlign::Center:  nvg_halign = NVG_ALIGN_CENTER; break;
            case TextAlign::Right:   nvg_halign = NVG_ALIGN_RIGHT;  break;
            case TextAlign::Left:
            case TextAlign::Justify:
            default:                 nvg_halign = NVG_ALIGN_LEFT;   break;
        }
        // Set align BEFORE nvgTextBox — it reads state->textAlign at
        // entry (line 2548 in nanovg.c). BASELINE, placed explicitly with
        // the browser formula: the first baseline sits at
        //   box_top + (line_box − (ascent + descent)) / 2 + ascent
        // i.e. half-leading centers the font's ink box inside the CSS line
        // box, then the baseline is one ascent below the ink top. Going
        // through NanoVG's TOP alignment instead double-interprets the
        // ascent and drifts by a pixel against browsers.
        nvgTextAlign(vg_, nvg_halign | NVG_ALIGN_BASELINE);

        const FontVMetrics vm = font_vmetrics(handle);
        // Browser formula: half-leading centers the (nearest-rounded) ink
        // box in the CSS line box; the first baseline sits one rounded
        // ascent below the ink top. The baseline stays FRACTIONAL — Chrome
        // rasterizes from the fractional baseline with AA, and snapping it
        // here landed our glyphs one row below Chrome's whenever the
        // fraction crossed .5.
        const float ink_h = vm.ascent + vm.descent;
        const float fy = pos_y
                       + (css_line_h - ink_h) * 0.5f + vm.ascent;
        static const bool text_trace =
            std::getenv("AFFINEUI_TEXT_TRACE") != nullptr;
        if (text_trace) {
            std::fprintf(stderr,
                         "[text] fs=%d asc=%.1f desc=%.1f gap=%.1f "
                         "css_lh=%.2f pos.y=%d baseline=%.1f '%.*s'\n",
                         handle_size_px(handle), vm.ascent, vm.descent,
                         vm.line_gap, css_line_h, pos_y, fy,
                         static_cast<int>(std::min<std::size_t>(12,
                                                                text.size())),
                         text.data());
        }

        // CSS white-space: pre text arrives with a huge max_width AND may
        // contain explicit '\n' newlines. NanoVG's nvgTextBox intentionally
        // skips leading whitespace after each newline (word-wrap behaviour),
        // which breaks pre-formatted indentation. When the text has explicit
        // newlines AND the caller signalled no-wrap (max_width >= 60000), we
        // split and draw each line individually with nvgText so that leading
        // spaces on indented lines are preserved.
        // NOTE: 60000 is chosen so that the sentinel 65535 (uint16 max, stored
        // by DisplayListBuilder) also triggers this path.
        const bool has_newline = text.find('\n') != std::string_view::npos;
        if (has_newline && max_width >= 60000.0f) {
            // Draw pre-formatted text line by line at the exact x origin.
            // Each line inherits the same Y-advance as css_line_h.
            const float lh = css_line_h;
            float cy = fy;
            const char* p = text.data();
            const char* end = text.data() + text.size();
            while (p <= end) {
                const char* nl = static_cast<const char*>(
                    std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
                const char* line_end = nl ? nl : end;
                nvgText(vg_, fx, cy, p, line_end);
                cy += lh;
                if (!nl) break;
                p = nl + 1;
            }
        } else if (align == TextAlign::Justify) {
            // NanoVG has no justify mode. Break the text into rows and, for
            // every row except the last (and rows that end at a hard
            // break), distribute the leftover width evenly across the
            // inter-word gaps so the line is flush on both edges. The last
            // / paragraph-final line stays left-aligned (CSS semantics).
            const auto draw_run = [&](float x, float y, const char* s, const char* e) {
                nvgText(vg_, x, y, s, e);
            };
            struct Row { const char* start; const char* end; const char* next; };
            std::vector<Row> rows;
            const char* cursor = text.data();
            const char* const text_end = text.data() + text.size();
            NVGtextRow buf[8];
            while (cursor < text_end) {
                int n = nvgTextBreakLines(vg_, cursor, text_end,
                                           max_width + kWrapEpsilonPx, buf, 8);
                if (n == 0) break;
                for (int i = 0; i < n; ++i)
                    rows.push_back({buf[i].start, buf[i].end, buf[i].next});
                cursor = buf[n - 1].next;
            }
            float cy = fy;
            for (std::size_t ri = 0; ri < rows.size(); ++ri) {
                const Row& row = rows[ri];
                bool hard_break = false;
                for (const char* p = row.end; p < row.next; ++p)
                    if (*p == '\n' || *p == '\r') { hard_break = true; break; }
                const bool last = (ri + 1 == rows.size());
                if (last || hard_break) {
                    draw_run(fx, cy, row.start, row.end);
                } else {
                    struct W { const char* s; const char* e; float adv; };
                    std::vector<W> words;
                    const char* p = row.start;
                    while (p < row.end) {
                        while (p < row.end && (*p == ' ' || *p == '\t')) ++p;
                        if (p >= row.end) break;
                        const char* ws = p;
                        while (p < row.end && *p != ' ' && *p != '\t') ++p;
                        words.push_back({ws, p,
                            nvgTextBounds(vg_, 0.0f, 0.0f, ws, p, nullptr)});
                    }
                    if (words.size() <= 1) {
                        if (!words.empty())
                            draw_run(fx, cy, words[0].s, words[0].e);
                    } else {
                        float sum = 0.0f;
                        for (const auto& w : words) sum += w.adv;
                        const float gap = (max_width - sum) /
                                          static_cast<float>(words.size() - 1);
                        float wx = fx;
                        for (const auto& w : words) {
                            draw_run(wx, cy, w.s, w.e);
                            wx += w.adv + gap;
                        }
                    }
                }
                cy += css_line_h;
            }
        } else {
            nvgTextLineHeight(vg_, nvg_line_height_mult(css_line_h, natural_line_h));
            nvgTextBox(vg_, fx, fy, max_width + kWrapEpsilonPx,
                       text.data(), text.data() + text.size());
        }
        // Reset letter spacing to default so subsequent draws are unaffected.
        nvgTextLetterSpacing(vg_, 0.0f);
    }

    std::uint32_t load_image(std::string_view url) override {
        const std::string key(url);
        if (auto it = image_cache_.find(key); it != image_cache_.end())
            return static_cast<std::uint32_t>(it->second);
        const int img = nvgCreateImage(vg_, key.c_str(), 0);
        if (img <= 0) return 0;
        image_cache_[key] = img;
        return static_cast<std::uint32_t>(img);
    }

    Size image_size(std::uint32_t image) override {
        if (image == 0) return {};
        int w = 0, h = 0;
        nvgImageSize(vg_, static_cast<int>(image), &w, &h);
        return {w, h};
    }

    void draw_image(std::uint32_t image, const Rect& dst, const Rect& src) override {
        if (image == 0 || dst.w <= 0 || dst.h <= 0) return;
        // Pattern that maps the WHOLE image onto dst by default; when a
        // sub-rect `src` is given, scale/offset the pattern so `src`
        // lands exactly on `dst` and scissor away the rest.
        float px = static_cast<float>(dst.x);
        float py = static_cast<float>(dst.y);
        float pw = static_cast<float>(dst.w);
        float ph = static_cast<float>(dst.h);
        bool  sub = false;
        if (src.w > 0 && src.h > 0) {
            int iw = 0, ih = 0;
            nvgImageSize(vg_, static_cast<int>(image), &iw, &ih);
            if (iw > 0 && ih > 0 && (src.x != 0 || src.y != 0 ||
                                     src.w != iw || src.h != ih)) {
                const float sx = static_cast<float>(dst.w) /
                                 static_cast<float>(src.w);
                const float sy = static_cast<float>(dst.h) /
                                 static_cast<float>(src.h);
                px = static_cast<float>(dst.x) - static_cast<float>(src.x) * sx;
                py = static_cast<float>(dst.y) - static_cast<float>(src.y) * sy;
                pw = static_cast<float>(iw) * sx;
                ph = static_cast<float>(ih) * sy;
                sub = true;
            }
        }
        if (sub) {
            nvgSave(vg_);
            nvgIntersectScissor(vg_, static_cast<float>(dst.x),
                                static_cast<float>(dst.y),
                                static_cast<float>(dst.w),
                                static_cast<float>(dst.h));
        }
        NVGpaint p = nvgImagePattern(vg_, px, py, pw, ph,
                                     0.0f, static_cast<int>(image), 1.0f);
        nvgBeginPath(vg_);
        nvgRect(vg_, static_cast<float>(dst.x), static_cast<float>(dst.y),
                static_cast<float>(dst.w), static_cast<float>(dst.h));
        nvgFillPaint(vg_, p);
        nvgFill(vg_);
        if (sub) nvgRestore(vg_);
    }

    std::uint32_t create_image_rgba(int w, int h,
                                    const std::uint8_t* pixels) override {
        if (w <= 0 || h <= 0 || pixels == nullptr) return 0;
        const int img = nvgCreateImageRGBA(vg_, w, h, 0, pixels);
        return img > 0 ? static_cast<std::uint32_t>(img) : 0u;
    }

    void update_image_rgba(std::uint32_t image,
                           const std::uint8_t* pixels) override {
        if (image == 0 || pixels == nullptr) return;
        nvgUpdateImage(vg_, static_cast<int>(image), pixels);
    }

    void delete_image(std::uint32_t image) override {
        if (image == 0) return;
        nvgDeleteImage(vg_, static_cast<int>(image));
    }

    std::uint32_t adopt_native_image(std::uint64_t native_handle, int w,
                                     int h, bool flip_y) override {
#if !defined(AFFINEUI_HOST_PROVIDES_NANOVG)
        // The texture stays owned by the caller (NVG_IMAGE_NODELETE);
        // the {0} sampler falls back to the backend's default.
        sg_image img;
        img.id = static_cast<std::uint32_t>(native_handle);
        sg_sampler smp;
        smp.id = 0;
        const int flags =
            NVG_IMAGE_NODELETE | (flip_y ? NVG_IMAGE_FLIPY : 0);
        const int id =
            nvsgCreateImageFromHandle(vg_, img, smp, w, h, flags);
        return id > 0 ? static_cast<std::uint32_t>(id) : 0u;
#else
        // Host-provided NanoVG backend: no sokol texture injection.
        (void)native_handle; (void)w; (void)h; (void)flip_y;
        return 0u;
#endif
    }

    void release_native_image(std::uint32_t image) override {
        if (image != 0) nvgDeleteImage(vg_, static_cast<int>(image));
    }

    void push_clip(const Rect& r) override {
        nvgSave(vg_);
        nvgScissor(vg_, static_cast<float>(r.x), static_cast<float>(r.y),
                   static_cast<float>(r.w), static_cast<float>(r.h));
    }

    void pop_clip() override { nvgRestore(vg_); }

    // CSS `opacity` group alpha: save NanoVG state and apply a global
    // alpha multiplier. All subsequent NanoVG draws are composited at
    // `current_alpha * alpha` until pop_alpha() restores the old state.
    void push_alpha(float alpha) override {
        nvgSave(vg_);
        nvgGlobalAlpha(vg_, alpha);
    }

    void pop_alpha() override { nvgRestore(vg_); }

    void push_transform(const Mat2x3& m) override {
        nvgSave(vg_);
        nvgTransform(vg_, m.a, m.b, m.c, m.d, m.tx, m.ty);
    }

    void pop_transform() override { nvgRestore(vg_); }

    void set_default_face    (int face) { default_face_     = face; }
    void set_bold_face       (int face) { bold_face_        = face; }
    void set_italic_face     (int face) { italic_face_      = face; }
    void set_bold_italic_face(int face) { bold_italic_face_ = face; }

private:
    // Handle bit layout (32 bits):
    //   [31..16]  face id (16 bits)
    //   [15..0]   size px (16 bits)
    static constexpr std::uint32_t pack_handle(std::uint16_t face,
                                               std::uint16_t size) {
        return (static_cast<std::uint32_t>(face) << 16)
             | static_cast<std::uint32_t>(size);
    }
    static constexpr int handle_size_px(std::uint32_t handle) {
        return static_cast<int>(handle & 0xFFFFu);
    }
    struct RegisteredFontFace {
        std::string family;
        int         weight{400};
        bool        italic{false};
        int         face{-1};
    };
    int find_registered_font_face(const std::string& family,
                                  int weight,
                                  bool italic) const {
        const auto exact = registered_face_ids_.find(
            font_face_key(family, weight, italic));
        if (exact != registered_face_ids_.end()) return exact->second;

        int best_face = -1;
        int best_score = std::numeric_limits<int>::max();
        for (const auto& face : registered_faces_) {
            if (face.family != family) continue;
            const int italic_penalty = face.italic == italic ? 0 : 2000;
            const int score = italic_penalty + std::abs(face.weight - weight);
            if (score < best_score) {
                best_score = score;
                best_face = face.face;
            }
        }
        return best_face;
    }
    static bool text_is_whitespace_only(std::string_view text) {
        for (char c : text) {
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r' &&
                c != '\f' && c != '\v') {
                return false;
            }
        }
        return true;
    }
    // Font vertical metrics in the BROWSER model: Skia rounds ascent and
    // descent OUTWARD to whole pixels before struts/line boxes are built, so
    // every browser line box and baseline lands on integer-friendly spots.
    // Matching that rounding is what makes our text land on the same pixels
    // as Chrome for the same font file.
    struct FontVMetrics {
        float ascent{0.0f};    // rounded up, px above baseline
        float descent{0.0f};   // rounded up, px below baseline (positive)
        float line_gap{0.0f};
        float line_height{0.0f};  // ascent + descent + line_gap
    };
    // PRECONDITION: apply_handle(handle) already ran (this reads the current
    // NanoVG font state and must not disturb alignment set by the caller).
    FontVMetrics font_vmetrics(std::uint32_t handle) {
        float asc = 0.0f;
        float desc = 0.0f;  // NanoVG reports ≤ 0
        float lineh = 0.0f;
        nvgTextMetrics(vg_, &asc, &desc, &lineh);
        FontVMetrics m;
        // Chrome/Skia rounds ascent and descent to the NEAREST pixel
        // (canvas fontBoundingBox for Plex@11 reports 11/3 from raw
        // 11.275/3.025 — not ceil). The ink box built from these places
        // the baseline; the LINE height stays the raw font value (Chrome
        // computes `line-height: normal` as the unrounded metric, e.g.
        // 15.95px for Plex@11).
        m.ascent = std::floor(asc + 0.5f);
        m.descent = std::floor(-desc + 0.5f);
        const float gap = lineh - (asc - desc);
        m.line_gap = gap > 0.0f ? gap : 0.0f;
        m.line_height = lineh > 0.0f
            ? lineh
            : m.ascent + m.descent + m.line_gap;
        if (m.line_height <= 0.0f) {
            m.line_height = static_cast<float>(
                std::max(handle_size_px(handle), 1));
        }
        return m;
    }
    float natural_line_height_px(std::uint32_t handle) {
        return font_vmetrics(handle).line_height;
    }
    static float css_line_height_px(std::uint32_t handle,
                                    float line_height_mult,
                                    float natural_line_h) {
        // mult 0 = CSS `normal`: the font's own line height, exactly what
        // browsers use. A positive mult is relative to FONT SIZE (CSS
        // unitless line-height semantics).
        if (line_height_mult <= 0.0f) return std::max(natural_line_h, 1.0f);
        const float requested =
            static_cast<float>(handle_size_px(handle)) * line_height_mult;
        return std::max(requested, 1.0f);
    }
    static float nvg_line_height_mult(float css_line_h, float natural_line_h) {
        return natural_line_h > 0.0f ? css_line_h / natural_line_h : 1.0f;
    }
    static float line_box_leading(float css_line_h, float natural_line_h) {
        return css_line_h - natural_line_h;
    }
    void apply_handle(std::uint32_t handle) {
        const int face = static_cast<int>((handle >> 16) & 0xFFFF);
        const int size = handle_size_px(handle);
        nvgFontFaceId(vg_, face);
        nvgFontSize(vg_, static_cast<float>(size));
        // Layout treats Y as the top edge of the text box. NanoVG's
        // default is BASELINE — flip so callers get the intuitive
        // meaning.
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    }

    // A content box sized to EXACTLY its text advance must not wrap its
    // final glyph because NanoVG's float advance sums land an epsilon over
    // the integer box width. Layout sizes boxes to the exact advance
    // (browser behavior — no slack), so every break decision gets this
    // shared epsilon; measure and draw use it identically and stay in
    // agreement about row counts.
    static constexpr float kWrapEpsilonPx = 0.75f;

    NVGcontext*                                  vg_{nullptr};
    int                                          width_{0};
    int                                          height_{0};
    int                                          default_face_{-1};
    int                                          bold_face_{-1};
    int                                          italic_face_{-1};
    int                                          bold_italic_face_{-1};
    std::unordered_map<std::string, int>         image_cache_;
    std::unordered_map<std::uint64_t, int>       stripe_cache_;
    std::unordered_map<std::uint64_t, int>       gradient_luts_;
    std::unordered_map<std::uint64_t, int>       grid_cache_;
    std::unordered_map<std::uint64_t, int>       shadow_cache_;
    std::vector<RegisteredFontFace>              registered_faces_;
    std::unordered_map<std::string, int>         registered_face_ids_;
    std::vector<std::unique_ptr<unsigned char[]>> registered_face_buffers_;
};

}  // namespace

namespace detail {

std::unique_ptr<Painter> make_nanovg_painter(NVGcontext* vg) {
    auto painter = std::make_unique<NanoVGPainter>(vg);
    // Pick up whichever faces register_default_font installed.
    auto find = [&](const char* a, const char* b) {
        int f = nvgFindFont(vg, a);
        if (f < 0 && b) f = nvgFindFont(vg, b);
        return f;
    };
    if (int f = find("sans", "sans-serif");              f >= 0) painter->set_default_face(f);
    if (int f = find("sans-bold", "sans-serif-bold");    f >= 0) painter->set_bold_face(f);
    if (int f = find("sans-italic", "sans-serif-italic"); f >= 0) painter->set_italic_face(f);
    if (int f = find("sans-bold-italic", "sans-serif-bold-italic"); f >= 0) painter->set_bold_italic_face(f);
    return painter;
}

std::uint32_t register_font_file(NVGcontext* vg, const char* family, const char* ttf_path) {
    const int id = nvgCreateFont(vg, family, ttf_path);
    return id >= 0 ? static_cast<std::uint32_t>(id) : 0u;
}

std::string_view register_default_font(NVGcontext* vg) {
#if defined(AFFINEUI_HAVE_EMBEDDED_FONTS)
    // Vendored default: Roboto, embedded into the library. This makes text
    // metrics — and therefore layout — identical on Windows/macOS/Linux,
    // instead of depending on whatever font the host OS happens to ship.
    // Apps may register additional fonts afterwards.
    if (nvgCreateFontMem(vg, "sans", affineui_font_roboto_regular,
                         static_cast<int>(affineui_font_roboto_regular_len), 0) >= 0) {
        nvgCreateFontMem(vg, "sans-serif", affineui_font_roboto_regular,
                         static_cast<int>(affineui_font_roboto_regular_len), 0);
        nvgCreateFontMem(vg, "Roboto", affineui_font_roboto_regular,
                         static_cast<int>(affineui_font_roboto_regular_len), 0);
        if (nvgCreateFontMem(vg, "sans-bold", affineui_font_roboto_bold,
                             static_cast<int>(affineui_font_roboto_bold_len), 0) >= 0) {
            nvgCreateFontMem(vg, "sans-serif-bold", affineui_font_roboto_bold,
                             static_cast<int>(affineui_font_roboto_bold_len), 0);
            nvgCreateFontMem(vg, "Roboto-bold", affineui_font_roboto_bold,
                             static_cast<int>(affineui_font_roboto_bold_len), 0);
        }
        register_platform_font_aliases(vg);
        // NOTE: italic / bold-italic faces aren't vendored yet, so emphasized
        // text falls back to the upright form (see assets/fonts/NOTICE.md).
        return "sans";
    }
    // If embedding somehow failed, fall through to host/system fonts.
#endif
    // A font-file candidate. `index` > 0 selects a specific face from
    // a .ttc/.otc font collection (macOS Helvetica.ttc bundles
    // Regular + Bold + Light + obliques as faces 0..5; HelveticaNeue
    // is the same shape). Index 0 == default load via nvgCreateFont.
    struct FontCandidate { const char* path; int index; };

    static constexpr FontCandidate kCandidates[] = {
#if defined(__APPLE__)
        // NanoVG/fontstash does not drive Apple's variable system-font
        // weight axes, so pairing SFNS Regular with Helvetica bold
        // produces mismatched metrics. Prefer complete TTC families
        // whose regular/bold/italic faces all come from the same file.
        {"/System/Library/Fonts/HelveticaNeue.ttc",              0},
        {"/System/Library/Fonts/Helvetica.ttc",                  0},
        {"/System/Library/Fonts/SFNS.ttf",                       0},
        {"/System/Library/Fonts/SFCompact.ttf",                  0},
        {"/System/Library/Fonts/Supplemental/Arial.ttf",         0},
        {"/Library/Fonts/Arial.ttf",                             0},
#elif defined(__linux__)
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",       0},
        {"/usr/share/fonts/TTF/DejaVuSans.ttf",                   0},
        {"/usr/share/fonts/liberation/LiberationSans-Regular.ttf", 0},
        {"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", 0},
        {"/usr/share/fonts/noto/NotoSans-Regular.ttf",            0},
#elif defined(_WIN32)
        {"C:/Windows/Fonts/segoeui.ttf",                          0},
        {"C:/Windows/Fonts/arial.ttf",                            0},
        {"C:/Windows/Fonts/calibri.ttf",                          0},
#endif
    };
    static constexpr FontCandidate kBoldCandidates[] = {
#if defined(__APPLE__)
        // HelveticaNeue.ttc face index 1 is the Bold face on macOS;
        // same for Helvetica.ttc. Try them before the standalone TTFs
        // so the bold matches the regular family.
        {"/System/Library/Fonts/HelveticaNeue.ttc",              1},
        {"/System/Library/Fonts/Helvetica.ttc",                  1},
        {"/System/Library/Fonts/Supplemental/Arial Bold.ttf",    0},
        {"/Library/Fonts/Arial Bold.ttf",                        0},
#elif defined(__linux__)
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",  0},
        {"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",              0},
        {"/usr/share/fonts/liberation/LiberationSans-Bold.ttf",   0},
        {"/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", 0},
        {"/usr/share/fonts/noto/NotoSans-Bold.ttf",               0},
#elif defined(_WIN32)
        {"C:/Windows/Fonts/segoeuib.ttf",                         0},
        {"C:/Windows/Fonts/arialbd.ttf",                          0},
        {"C:/Windows/Fonts/calibrib.ttf",                         0},
#endif
    };
    // Italic + bold-italic. macOS face indices for Helvetica.ttc /
    // HelveticaNeue.ttc are well known: 4 = oblique, 5 = bold oblique.
    static constexpr FontCandidate kItalicCandidates[] = {
#if defined(__APPLE__)
        {"/System/Library/Fonts/HelveticaNeue.ttc",              4},
        {"/System/Library/Fonts/Helvetica.ttc",                  4},
        {"/System/Library/Fonts/Supplemental/Arial Italic.ttf",  0},
        {"/Library/Fonts/Arial Italic.ttf",                      0},
#elif defined(__linux__)
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf", 0},
        {"/usr/share/fonts/liberation/LiberationSans-Italic.ttf",   0},
        {"/usr/share/fonts/truetype/liberation/LiberationSans-Italic.ttf", 0},
        {"/usr/share/fonts/noto/NotoSans-Italic.ttf",               0},
#elif defined(_WIN32)
        {"C:/Windows/Fonts/segoeuii.ttf",                         0},
        {"C:/Windows/Fonts/ariali.ttf",                           0},
        {"C:/Windows/Fonts/calibrii.ttf",                         0},
#endif
    };
    static constexpr FontCandidate kBoldItalicCandidates[] = {
#if defined(__APPLE__)
        {"/System/Library/Fonts/HelveticaNeue.ttc",              5},
        {"/System/Library/Fonts/Helvetica.ttc",                  5},
        {"/System/Library/Fonts/Supplemental/Arial Bold Italic.ttf", 0},
        {"/Library/Fonts/Arial Bold Italic.ttf",                 0},
#elif defined(__linux__)
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf", 0},
        {"/usr/share/fonts/liberation/LiberationSans-BoldItalic.ttf",   0},
        {"/usr/share/fonts/truetype/liberation/LiberationSans-BoldItalic.ttf", 0},
        {"/usr/share/fonts/noto/NotoSans-BoldItalic.ttf",               0},
#elif defined(_WIN32)
        {"C:/Windows/Fonts/segoeuiz.ttf",                         0},
        {"C:/Windows/Fonts/arialbi.ttf",                          0},
        {"C:/Windows/Fonts/calibriz.ttf",                         0},
#endif
    };
    auto load_one = [&](const char* name, FontCandidate c) {
        return (c.index == 0)
            ? nvgCreateFont       (vg, name, c.path)
            : nvgCreateFontAtIndex(vg, name, c.path, c.index);
    };
    std::string_view registered_regular;
    for (FontCandidate c : kCandidates) {
        if (load_one("sans", c) >= 0) {
            load_one("sans-serif", c);
            registered_regular = "sans";
            break;
        }
    }
    // Bold is best-effort. If no bold face is on the system, the
    // weight cascade falls back to the regular face for >= 500.
    for (FontCandidate c : kBoldCandidates) {
        if (load_one("sans-bold", c) >= 0) {
            load_one("sans-serif-bold", c);
            break;
        }
    }
    // Italic + bold-italic are also best-effort. If absent, the
    // resolve_font cascade silently substitutes the upright form.
    for (FontCandidate c : kItalicCandidates) {
        if (load_one("sans-italic", c) >= 0) {
            load_one("sans-serif-italic", c);
            break;
        }
    }
    for (FontCandidate c : kBoldItalicCandidates) {
        if (load_one("sans-bold-italic", c) >= 0) {
            load_one("sans-serif-bold-italic", c);
            break;
        }
    }
    register_platform_font_aliases(vg);
    return registered_regular;
}

}  // namespace detail

#endif  // AFFINEUI_STUB_BUILD

std::string_view embedded_font_data(bool bold) {
#if !defined(AFFINEUI_STUB_BUILD) && defined(AFFINEUI_HAVE_EMBEDDED_FONTS)
    if (bold) {
        return {reinterpret_cast<const char*>(affineui_font_roboto_bold),
                static_cast<std::size_t>(affineui_font_roboto_bold_len)};
    }
    return {reinterpret_cast<const char*>(affineui_font_roboto_regular),
            static_cast<std::size_t>(affineui_font_roboto_regular_len)};
#else
    (void)bold;
    return {};
#endif
}

}  // namespace affineui
