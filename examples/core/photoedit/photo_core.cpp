// Raster photo-editing core — implementation. See photo_core.h.
//
// Ported op-for-op from decius-css/samples/decius-photo:
//   engine.js  → document/layers/compositing/history/view/sample scene
//   paint.js   → brush engine, flood fill, wand, gradient, type, shape
//   ui.js      → adjustments & filters (exact pixel formulas)
//   dialogs.js → fill/stroke/place-embedded/resize ops
//
// Pixel format everywhere: RGBA8, non-premultiplied, stride = w * 4.
// Compositing follows the W3C compositing-and-blending spec (what canvas
// globalCompositeOperation implements).

#include "photo_core.h"

#include "affineui/app.h"
#include "affineui/painter.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace photo {

namespace {

// ── small helpers ────────────────────────────────────────────────────────

inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
inline std::uint8_t clamp8(double v) {
    return static_cast<std::uint8_t>(clampd(v, 0.0, 255.0) + 0.5);
}
inline std::uint8_t clamp8i(int v) {
    return static_cast<std::uint8_t>(clampi(v, 0, 255));
}

RectI intersect(const RectI& a, const RectI& b) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.w, b.x + b.w);
    const int y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

RectI union_rect(const RectI& a, const RectI& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w);
    const int y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

struct RGB {
    int r = 0, g = 0, b = 0;
};

// "#rgb" / "#rrggbb" → components; false on anything else.
bool parse_hex(const std::string& hex, RGB& out) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() == 4 && hex[0] == '#') {
        const int r = nib(hex[1]), g = nib(hex[2]), b = nib(hex[3]);
        if (r < 0 || g < 0 || b < 0) return false;
        out = {r * 17, g * 17, b * 17};
        return true;
    }
    if (hex.size() == 7 && hex[0] == '#') {
        int v[6];
        for (int i = 0; i < 6; ++i) {
            v[i] = nib(hex[1 + i]);
            if (v[i] < 0) return false;
        }
        out = {v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]};
        return true;
    }
    return false;
}

std::string to_hex(int r, int g, int b) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", clampi(r, 0, 255),
                  clampi(g, 0, 255), clampi(b, 0, 255));
    return buf;
}

// Deterministic LCG so the sample scene and noise are reproducible.
struct Rand {
    std::uint32_t s;
    explicit Rand(std::uint32_t seed) : s(seed) {}
    double next() {  // [0, 1)
        s = s * 1664525u + 1013904223u;
        return (s >> 8) / 16777216.0;
    }
};

// ── blend modes (canvas globalCompositeOperation subset the app uses) ───

enum class Blend : std::uint8_t {
    Normal, Multiply, Screen, Overlay, HardLight, Darken, Lighten,
    ColorDodge, ColorBurn, Difference, SoftLight,
    Hue, Saturation, ColorMode, Luminosity,
    DestinationOut,  // internal (eraser)
};

Blend parse_blend(const std::string& s) {
    if (s == "multiply") return Blend::Multiply;
    if (s == "screen") return Blend::Screen;
    if (s == "overlay") return Blend::Overlay;
    if (s == "hard-light") return Blend::HardLight;
    if (s == "darken") return Blend::Darken;
    if (s == "lighten") return Blend::Lighten;
    if (s == "color-dodge") return Blend::ColorDodge;
    if (s == "color-burn") return Blend::ColorBurn;
    if (s == "difference") return Blend::Difference;
    if (s == "soft-light") return Blend::SoftLight;
    if (s == "hue") return Blend::Hue;
    if (s == "saturation") return Blend::Saturation;
    if (s == "color") return Blend::ColorMode;
    if (s == "luminosity") return Blend::Luminosity;
    if (s == "destination-out") return Blend::DestinationOut;
    return Blend::Normal;  // "source-over" and anything unknown
}

// Separable blend B(Cb, Cs) on 0..255 ints (fast path).
inline int blend_channel_int(Blend mode, int cb, int cs) {
    switch (mode) {
        case Blend::Multiply: return cb * cs / 255;
        case Blend::Screen: return 255 - (255 - cb) * (255 - cs) / 255;
        case Blend::Overlay:
            return cb <= 127 ? 2 * cb * cs / 255
                             : 255 - 2 * (255 - cb) * (255 - cs) / 255;
        case Blend::HardLight:  // overlay with source/backdrop swapped
            return cs <= 127 ? 2 * cb * cs / 255
                             : 255 - 2 * (255 - cb) * (255 - cs) / 255;
        case Blend::Darken: return std::min(cb, cs);
        case Blend::Lighten: return std::max(cb, cs);
        case Blend::ColorDodge:
            if (cb == 0) return 0;
            if (cs == 255) return 255;
            return std::min(255, cb * 255 / (255 - cs));
        case Blend::ColorBurn:
            if (cb == 255) return 255;
            if (cs == 0) return 0;
            return 255 - std::min(255, (255 - cb) * 255 / cs);
        case Blend::Difference: return std::abs(cb - cs);
        default: return cs;  // Normal
    }
}

inline bool is_separable_int(Blend m) {
    switch (m) {
        case Blend::Normal:
        case Blend::Multiply:
        case Blend::Screen:
        case Blend::Overlay:
        case Blend::HardLight:
        case Blend::Darken:
        case Blend::Lighten:
        case Blend::ColorDodge:
        case Blend::ColorBurn:
        case Blend::Difference:
            return true;
        default:
            return false;
    }
}

// ── non-separable helpers (W3C, floats 0..1) ─────────────────────────────

struct F3 {
    double r, g, b;
};
inline double lum(const F3& c) {
    return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b;
}
F3 clip_color(F3 c) {
    const double l = lum(c);
    const double n = std::min({c.r, c.g, c.b});
    const double x = std::max({c.r, c.g, c.b});
    if (n < 0.0) {
        const double d = l - n;
        if (d > 1e-9) {
            c.r = l + (c.r - l) * l / d;
            c.g = l + (c.g - l) * l / d;
            c.b = l + (c.b - l) * l / d;
        } else {
            c = {l, l, l};
        }
    }
    if (x > 1.0) {
        const double d = x - l;
        if (d > 1e-9) {
            c.r = l + (c.r - l) * (1.0 - l) / d;
            c.g = l + (c.g - l) * (1.0 - l) / d;
            c.b = l + (c.b - l) * (1.0 - l) / d;
        } else {
            c = {l, l, l};
        }
    }
    return c;
}
F3 set_lum(F3 c, double l) {
    const double d = l - lum(c);
    return clip_color({c.r + d, c.g + d, c.b + d});
}
inline double sat(const F3& c) {
    return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b});
}
F3 set_sat(F3 c, double s) {
    double* ch[3] = {&c.r, &c.g, &c.b};
    // order min <= mid <= max
    if (*ch[0] > *ch[1]) std::swap(ch[0], ch[1]);
    if (*ch[1] > *ch[2]) std::swap(ch[1], ch[2]);
    if (*ch[0] > *ch[1]) std::swap(ch[0], ch[1]);
    if (*ch[2] > *ch[0]) {
        *ch[1] = (*ch[1] - *ch[0]) * s / (*ch[2] - *ch[0]);
        *ch[2] = s;
    } else {
        *ch[1] = *ch[2] = 0.0;
    }
    *ch[0] = 0.0;
    return c;
}

F3 blend_nonseparable(Blend mode, const F3& cb, const F3& cs) {
    switch (mode) {
        case Blend::Hue: return set_lum(set_sat(cs, sat(cb)), lum(cb));
        case Blend::Saturation:
            return set_lum(set_sat(cb, sat(cs)), lum(cb));
        case Blend::ColorMode: return set_lum(cs, lum(cb));
        case Blend::Luminosity: return set_lum(cb, lum(cs));
        case Blend::SoftLight: {
            auto one = [](double b, double s) {
                if (s <= 0.5) return b - (1 - 2 * s) * b * (1 - b);
                const double d =
                    b <= 0.25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
                return b + (2 * s - 1) * (d - b);
            };
            return {one(cb.r, cs.r), one(cb.g, cs.g), one(cb.b, cs.b)};
        }
        default: return cs;
    }
}

// Composite `src` over `dst` inside `rect` with a global alpha multiplier
// and blend mode. Both buffers are document-sized. This is one layer of
// engine.js PS.render (ctx.globalAlpha = ...; ctx.gco = blend; drawImage).
void compose_over(Buffer& dst, const Buffer& src, const RectI& rect,
                  double alpha_mult, Blend mode) {
    const RectI r = intersect(rect, {0, 0, dst.w, dst.h});
    if (r.empty() || alpha_mult <= 0.0) return;
    const int am = static_cast<int>(alpha_mult * 255.0 + 0.5);

    if (mode == Blend::DestinationOut) {
        for (int y = r.y; y < r.y + r.h; ++y) {
            const std::uint8_t* sp = &src.px[(static_cast<std::size_t>(y) * src.w + r.x) * 4];
            std::uint8_t* dp = &dst.px[(static_cast<std::size_t>(y) * dst.w + r.x) * 4];
            for (int x = 0; x < r.w; ++x, sp += 4, dp += 4) {
                const int as = sp[3] * am / 255;
                dp[3] = static_cast<std::uint8_t>(dp[3] * (255 - as) / 255);
            }
        }
        return;
    }

    if (is_separable_int(mode)) {
        for (int y = r.y; y < r.y + r.h; ++y) {
            const std::uint8_t* sp = &src.px[(static_cast<std::size_t>(y) * src.w + r.x) * 4];
            std::uint8_t* dp = &dst.px[(static_cast<std::size_t>(y) * dst.w + r.x) * 4];
            for (int x = 0; x < r.w; ++x, sp += 4, dp += 4) {
                const int as = sp[3] * am / 255;
                if (as == 0) continue;
                const int ab = dp[3];
                const int ao = as + ab * (255 - as) / 255;
                if (ao == 0) { dp[3] = 0; continue; }
                for (int c = 0; c < 3; ++c) {
                    const int cs = sp[c], cb = dp[c];
                    const int bl = blend_channel_int(mode, cb, cs);
                    // W3C: Co = (as(1-ab)Cs + as·ab·B + (1-as)ab·Cb) / ao
                    const long num =
                        static_cast<long>(as) * (255 - ab) * cs +
                        static_cast<long>(as) * ab * bl +
                        static_cast<long>(255 - as) * ab * cb;
                    dp[c] = static_cast<std::uint8_t>(
                        num / (static_cast<long>(ao) * 255));
                }
                dp[3] = static_cast<std::uint8_t>(ao);
            }
        }
        return;
    }

    // Float path (soft-light + non-separables).
    for (int y = r.y; y < r.y + r.h; ++y) {
        const std::uint8_t* sp = &src.px[(static_cast<std::size_t>(y) * src.w + r.x) * 4];
        std::uint8_t* dp = &dst.px[(static_cast<std::size_t>(y) * dst.w + r.x) * 4];
        for (int x = 0; x < r.w; ++x, sp += 4, dp += 4) {
            const double as = sp[3] / 255.0 * alpha_mult;
            if (as <= 0.0) continue;
            const double ab = dp[3] / 255.0;
            const double ao = as + ab * (1 - as);
            if (ao <= 0.0) { dp[3] = 0; continue; }
            const F3 cs{sp[0] / 255.0, sp[1] / 255.0, sp[2] / 255.0};
            const F3 cb{dp[0] / 255.0, dp[1] / 255.0, dp[2] / 255.0};
            const F3 bl = blend_nonseparable(mode, cb, cs);
            auto out = [&](double s, double b, double blv) {
                const double co =
                    (as * (1 - ab) * s + as * ab * blv + (1 - as) * ab * b) /
                    ao;
                return clamp8(co * 255.0);
            };
            dp[0] = out(cs.r, cb.r, bl.r);
            dp[1] = out(cs.g, cb.g, bl.g);
            dp[2] = out(cs.b, cb.b, bl.b);
            dp[3] = clamp8(ao * 255.0);
        }
    }
}

void clear_rect(Buffer& b, const RectI& rect) {
    const RectI r = intersect(rect, {0, 0, b.w, b.h});
    for (int y = r.y; y < r.y + r.h; ++y) {
        std::memset(&b.px[(static_cast<std::size_t>(y) * b.w + r.x) * 4], 0,
                    static_cast<std::size_t>(r.w) * 4);
    }
}

void copy_rect(Buffer& dst, const Buffer& src, const RectI& rect) {
    // dst and src are the same size; copies rect verbatim.
    const RectI r = intersect(rect, {0, 0, dst.w, dst.h});
    for (int y = r.y; y < r.y + r.h; ++y) {
        std::memcpy(&dst.px[(static_cast<std::size_t>(y) * dst.w + r.x) * 4],
                    &src.px[(static_cast<std::size_t>(y) * src.w + r.x) * 4],
                    static_cast<std::size_t>(r.w) * 4);
    }
}

// Source-over a solid color at per-pixel alpha (0..1) onto a buffer pixel.
inline void over_pixel(std::uint8_t* dp, int r, int g, int b, double a) {
    if (a <= 0.0) return;
    const double ab = dp[3] / 255.0;
    const double ao = a + ab * (1 - a);
    if (ao <= 0.0) { dp[3] = 0; return; }
    dp[0] = clamp8((a * r + (1 - a) * ab * dp[0]) / ao);
    dp[1] = clamp8((a * g + (1 - a) * ab * dp[1]) / ao);
    dp[2] = clamp8((a * b + (1 - a) * ab * dp[2]) / ao);
    dp[3] = clamp8(ao * 255.0);
}

// Bilinear resize (Image Size / export scale).
Buffer scaled_buffer(const Buffer& src, int nw, int nh) {
    Buffer out(std::max(1, nw), std::max(1, nh));
    if (src.w <= 0 || src.h <= 0) return out;
    for (int y = 0; y < out.h; ++y) {
        const double sy = (y + 0.5) * src.h / out.h - 0.5;
        const int y0 = clampi(static_cast<int>(std::floor(sy)), 0, src.h - 1);
        const int y1 = clampi(y0 + 1, 0, src.h - 1);
        const double fy = clampd(sy - y0, 0.0, 1.0);
        for (int x = 0; x < out.w; ++x) {
            const double sx = (x + 0.5) * src.w / out.w - 0.5;
            const int x0 =
                clampi(static_cast<int>(std::floor(sx)), 0, src.w - 1);
            const int x1 = clampi(x0 + 1, 0, src.w - 1);
            const double fx = clampd(sx - x0, 0.0, 1.0);
            const std::uint8_t* p00 =
                &src.px[(static_cast<std::size_t>(y0) * src.w + x0) * 4];
            const std::uint8_t* p10 =
                &src.px[(static_cast<std::size_t>(y0) * src.w + x1) * 4];
            const std::uint8_t* p01 =
                &src.px[(static_cast<std::size_t>(y1) * src.w + x0) * 4];
            const std::uint8_t* p11 =
                &src.px[(static_cast<std::size_t>(y1) * src.w + x1) * 4];
            std::uint8_t* op =
                &out.px[(static_cast<std::size_t>(y) * out.w + x) * 4];
            for (int c = 0; c < 4; ++c) {
                const double top = p00[c] + (p10[c] - p00[c]) * fx;
                const double bot = p01[c] + (p11[c] - p01[c]) * fx;
                op[c] = clamp8(top + (bot - top) * fy);
            }
        }
    }
    return out;
}

// ── gradient fills (sample scene / assets / gradient tool) ──────────────

struct Stop {
    double t;
    double r, g, b, a;  // 0..255 / 0..1
};

void eval_stops(const std::vector<Stop>& stops, double t, double& r,
                double& g, double& b, double& a) {
    if (stops.empty()) { r = g = b = a = 0; return; }
    if (t <= stops.front().t) {
        const Stop& s = stops.front();
        r = s.r; g = s.g; b = s.b; a = s.a;
        return;
    }
    if (t >= stops.back().t) {
        const Stop& s = stops.back();
        r = s.r; g = s.g; b = s.b; a = s.a;
        return;
    }
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].t) {
            const Stop& s0 = stops[i - 1];
            const Stop& s1 = stops[i];
            const double f =
                (s1.t - s0.t) > 1e-12 ? (t - s0.t) / (s1.t - s0.t) : 0.0;
            // Canvas interpolates premultiplied, which keeps color→
            // transparent ramps from darkening; do the same.
            const double a0 = s0.a, a1 = s1.a;
            a = a0 + (a1 - a0) * f;
            if (a > 1e-9) {
                r = (s0.r * a0 + (s1.r * a1 - s0.r * a0) * f) / a;
                g = (s0.g * a0 + (s1.g * a1 - s0.g * a0) * f) / a;
                b = (s0.b * a0 + (s1.b * a1 - s0.b * a0) * f) / a;
            } else {
                r = s1.r; g = s1.g; b = s1.b;
            }
            return;
        }
    }
    const Stop& s = stops.back();
    r = s.r; g = s.g; b = s.b; a = s.a;
}

void fill_linear_gradient(Buffer& buf, double x0, double y0, double x1,
                          double y1, const std::vector<Stop>& stops,
                          const RectI& rect, double alpha_mult = 1.0) {
    const RectI r = intersect(rect, {0, 0, buf.w, buf.h});
    const double dx = x1 - x0, dy = y1 - y0;
    const double len2 = dx * dx + dy * dy;
    for (int y = r.y; y < r.y + r.h; ++y) {
        std::uint8_t* dp =
            &buf.px[(static_cast<std::size_t>(y) * buf.w + r.x) * 4];
        for (int x = r.x; x < r.x + r.w; ++x, dp += 4) {
            const double t =
                len2 > 1e-12
                    ? clampd(((x + 0.5 - x0) * dx + (y + 0.5 - y0) * dy) /
                                 len2, 0.0, 1.0)
                    : 0.0;
            double cr, cg, cb, ca;
            eval_stops(stops, t, cr, cg, cb, ca);
            over_pixel(dp, static_cast<int>(cr), static_cast<int>(cg),
                       static_cast<int>(cb), ca * alpha_mult);
        }
    }
}

void fill_radial_gradient(Buffer& buf, double cx, double cy, double r0,
                          double r1, const std::vector<Stop>& stops,
                          const RectI& rect, double alpha_mult = 1.0) {
    const RectI r = intersect(rect, {0, 0, buf.w, buf.h});
    for (int y = r.y; y < r.y + r.h; ++y) {
        std::uint8_t* dp =
            &buf.px[(static_cast<std::size_t>(y) * buf.w + r.x) * 4];
        for (int x = r.x; x < r.x + r.w; ++x, dp += 4) {
            const double d =
                std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const double t =
                r1 - r0 > 1e-9 ? clampd((d - r0) / (r1 - r0), 0.0, 1.0)
                               : (d < r0 ? 0.0 : 1.0);
            double cr, cg, cb, ca;
            eval_stops(stops, t, cr, cg, cb, ca);
            over_pixel(dp, static_cast<int>(cr), static_cast<int>(cg),
                       static_cast<int>(cb), ca * alpha_mult);
        }
    }
}

// Anti-aliased filled circle (stars, sun disc).
void fill_circle(Buffer& buf, double cx, double cy, double radius, int r,
                 int g, int b, double alpha) {
    const RectI bbox = intersect(
        {static_cast<int>(cx - radius) - 1, static_cast<int>(cy - radius) - 1,
         static_cast<int>(radius * 2) + 3, static_cast<int>(radius * 2) + 3},
        {0, 0, buf.w, buf.h});
    for (int y = bbox.y; y < bbox.y + bbox.h; ++y) {
        std::uint8_t* dp =
            &buf.px[(static_cast<std::size_t>(y) * buf.w + bbox.x) * 4];
        for (int x = bbox.x; x < bbox.x + bbox.w; ++x, dp += 4) {
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const double cov = clampd(radius - d + 0.5, 0.0, 1.0);
            if (cov > 0) over_pixel(dp, r, g, b, alpha * cov);
        }
    }
}

// Anti-aliased rounded-rect fill (shape tool).
void fill_rounded_rect(Buffer& buf, double x, double y, double w, double h,
                       double radius, int r, int g, int b, double alpha,
                       const RectI& clip) {
    if (w <= 0 || h <= 0) return;
    radius = clampd(radius, 0.0, std::min(w, h) / 2.0);
    const RectI bbox = intersect(
        intersect({static_cast<int>(std::floor(x)) - 1,
                   static_cast<int>(std::floor(y)) - 1,
                   static_cast<int>(std::ceil(w)) + 3,
                   static_cast<int>(std::ceil(h)) + 3},
                  clip),
        {0, 0, buf.w, buf.h});
    const double cx0 = x + radius, cy0 = y + radius;
    const double cx1 = x + w - radius, cy1 = y + h - radius;
    for (int py = bbox.y; py < bbox.y + bbox.h; ++py) {
        std::uint8_t* dp =
            &buf.px[(static_cast<std::size_t>(py) * buf.w + bbox.x) * 4];
        for (int px = bbox.x; px < bbox.x + bbox.w; ++px, dp += 4) {
            const double sx = px + 0.5, sy = py + 0.5;
            // signed distance to the rounded rect
            const double qx = std::max({x - sx, 0.0, sx - (x + w)});
            const double qy = std::max({y - sy, 0.0, sy - (y + h)});
            double dist;
            if (radius > 0.0) {
                const double ix = clampd(sx, cx0, cx1);
                const double iy = clampd(sy, cy0, cy1);
                dist = std::hypot(sx - ix, sy - iy) - radius;
            } else {
                dist = std::max(qx, qy) > 0.0
                           ? std::hypot(qx, qy)
                           : std::max({x - sx, sx - (x + w), y - sy,
                                       sy - (y + h)});
            }
            const double cov = clampd(0.5 - dist, 0.0, 1.0);
            if (cov > 0) over_pixel(dp, r, g, b, alpha * cov);
        }
    }
}

// ── separable box blur ×3 ≈ gaussian (CSS blur(Npx)) ─────────────────────

void box_blur_pass(const Buffer& src, Buffer& dst, const RectI& r,
                   int radius, bool horizontal) {
    const int n = 2 * radius + 1;
    if (horizontal) {
        for (int y = r.y; y < r.y + r.h; ++y) {
            int acc[4] = {0, 0, 0, 0};
            auto at = [&](int x) {
                return &src.px[(static_cast<std::size_t>(y) * src.w +
                                clampi(x, r.x, r.x + r.w - 1)) * 4];
            };
            for (int i = -radius; i <= radius; ++i) {
                const std::uint8_t* p = at(r.x + i);
                for (int c = 0; c < 4; ++c) acc[c] += p[c];
            }
            for (int x = r.x; x < r.x + r.w; ++x) {
                std::uint8_t* dp =
                    &dst.px[(static_cast<std::size_t>(y) * dst.w + x) * 4];
                for (int c = 0; c < 4; ++c)
                    dp[c] = static_cast<std::uint8_t>(acc[c] / n);
                const std::uint8_t* add = at(x + radius + 1);
                const std::uint8_t* sub = at(x - radius);
                for (int c = 0; c < 4; ++c) acc[c] += add[c] - sub[c];
            }
        }
    } else {
        for (int x = r.x; x < r.x + r.w; ++x) {
            int acc[4] = {0, 0, 0, 0};
            auto at = [&](int y) {
                return &src.px[(static_cast<std::size_t>(
                                    clampi(y, r.y, r.y + r.h - 1)) * src.w +
                                x) * 4];
            };
            for (int i = -radius; i <= radius; ++i) {
                const std::uint8_t* p = at(r.y + i);
                for (int c = 0; c < 4; ++c) acc[c] += p[c];
            }
            for (int y = r.y; y < r.y + r.h; ++y) {
                std::uint8_t* dp =
                    &dst.px[(static_cast<std::size_t>(y) * dst.w + x) * 4];
                for (int c = 0; c < 4; ++c)
                    dp[c] = static_cast<std::uint8_t>(acc[c] / n);
                const std::uint8_t* add = at(y + radius + 1);
                const std::uint8_t* sub = at(y - radius);
                for (int c = 0; c < 4; ++c) acc[c] += add[c] - sub[c];
            }
        }
    }
}

// Blur `rect` of `buf` in place with ~gaussian sigma = radius_px.
void gaussian_blur(Buffer& buf, const RectI& rect, double radius_px) {
    const RectI r = intersect(rect, {0, 0, buf.w, buf.h});
    if (r.empty() || radius_px <= 0.05) return;
    // Three box passes of width w ≈ sigma * sqrt(12/3 + 1).
    const int box =
        std::max(1, static_cast<int>(radius_px * std::sqrt(3.0) / 2.0 + 0.5));
    Buffer tmp(buf.w, buf.h);
    copy_rect(tmp, buf, r);
    for (int pass = 0; pass < 3; ++pass) {
        box_blur_pass(buf, tmp, r, box, true);
        box_blur_pass(tmp, buf, r, box, false);
    }
}

// ── text rasterization (type tool + sample-scene title) ─────────────────

struct Font {
    stbtt_fontinfo info{};
    bool ok = false;
};

// The font bytes come from the host (they used to come from
// affineui::embedded_font_data, which is what forced this raster core to link
// the whole affineui runtime). Process-wide, like the parsed Font statics it
// feeds — PhotoDoc::attach installs it.
std::function<std::string_view(bool)>& font_provider() {
    static std::function<std::string_view(bool)> fn;
    return fn;
}

Font& get_font(bool bold) {
    static Font regular, boldf;
    Font& f = bold ? boldf : regular;
    static bool init[2] = {false, false};
    if (!init[bold ? 1 : 0]) {
        init[bold ? 1 : 0] = true;
        const auto& provider = font_provider();
        const std::string_view data =
            provider ? provider(bold) : std::string_view{};
        if (!data.empty()) {
            const auto* bytes =
                reinterpret_cast<const unsigned char*>(data.data());
            f.ok = stbtt_InitFont(&f.info, bytes,
                                  stbtt_GetFontOffsetForIndex(bytes, 0)) != 0;
        }
    }
    return f;
}

// Rasterize `text` with its TOP-left at (x, y) (canvas textBaseline='top').
void raster_text(Buffer& buf, double x, double y, const std::string& text,
                 double size_px, int r, int g, int b, double alpha,
                 bool bold) {
    Font& font = get_font(bold);
    if (!font.ok || size_px <= 1.0) return;
    const float scale =
        stbtt_ScaleForPixelHeight(&font.info, static_cast<float>(size_px));
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &line_gap);
    double pen_x = x;
    const double baseline = y + ascent * scale;
    int prev = 0;
    for (unsigned char ch : text) {
        const int cp = ch;  // ASCII is all the sample needs
        if (prev) {
            pen_x +=
                scale * stbtt_GetCodepointKernAdvance(&font.info, prev, cp);
        }
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font.info, cp, &adv, &lsb);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(
            &font.info, cp, scale, scale,
            static_cast<float>(pen_x - std::floor(pen_x)), 0.0f, &x0, &y0,
            &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<unsigned char> bitmap(
                static_cast<std::size_t>(gw) * gh);
            stbtt_MakeCodepointBitmapSubpixel(
                &font.info, bitmap.data(), gw, gh, gw, scale, scale,
                static_cast<float>(pen_x - std::floor(pen_x)), 0.0f, cp);
            const int ox = static_cast<int>(std::floor(pen_x)) + x0;
            const int oy = static_cast<int>(std::floor(baseline + 0.5)) + y0;
            for (int gy = 0; gy < gh; ++gy) {
                const int py = oy + gy;
                if (py < 0 || py >= buf.h) continue;
                for (int gx = 0; gx < gw; ++gx) {
                    const int px = ox + gx;
                    if (px < 0 || px >= buf.w) continue;
                    const double cov =
                        bitmap[static_cast<std::size_t>(gy) * gw + gx] /
                        255.0;
                    if (cov <= 0.0) continue;
                    over_pixel(&buf.px[(static_cast<std::size_t>(py) * buf.w +
                                        px) * 4],
                               r, g, b, alpha * cov);
                }
            }
        }
        pen_x += adv * scale;
        prev = cp;
    }
}

double measure_text_width(const std::string& text, double size_px,
                          bool bold) {
    Font& font = get_font(bold);
    if (!font.ok) return 0.0;
    const float scale =
        stbtt_ScaleForPixelHeight(&font.info, static_cast<float>(size_px));
    double w = 0.0;
    int prev = 0;
    for (unsigned char ch : text) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font.info, ch, &adv, &lsb);
        if (prev)
            w += scale * stbtt_GetCodepointKernAdvance(&font.info, prev, ch);
        w += adv * scale;
        prev = ch;
    }
    return w;
}

// ── flood mask (paint.js floodMask) ──────────────────────────────────────

std::vector<std::uint8_t> flood_mask(const Buffer& src, int sx, int sy,
                                     double tol, bool contiguous) {
    const int W = src.w, H = src.h;
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(W) * H, 0);
    if (sx < 0 || sy < 0 || sx >= W || sy >= H) return mask;
    auto idx = [&](int x, int y) {
        return (static_cast<std::size_t>(y) * W + x) * 4;
    };
    const std::size_t i0 = idx(sx, sy);
    const int tr = src.px[i0], tg = src.px[i0 + 1], tb = src.px[i0 + 2],
              ta = src.px[i0 + 3];
    const double tol2 = tol * tol * 4.0;
    auto match = [&](std::size_t i) {
        const double dr = src.px[i] - tr, dg = src.px[i + 1] - tg,
                     db = src.px[i + 2] - tb, da = src.px[i + 3] - ta;
        return dr * dr + dg * dg + db * db + da * da <= tol2;
    };
    if (contiguous) {
        std::vector<std::pair<int, int>> stack{{sx, sy}};
        while (!stack.empty()) {
            const auto [x, y] = stack.back();
            stack.pop_back();
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            std::uint8_t& m = mask[static_cast<std::size_t>(y) * W + x];
            if (m) continue;
            if (!match(idx(x, y))) continue;
            m = 1;
            stack.emplace_back(x + 1, y);
            stack.emplace_back(x - 1, y);
            stack.emplace_back(x, y + 1);
            stack.emplace_back(x, y - 1);
        }
    } else {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (match(idx(x, y)))
                    mask[static_cast<std::size_t>(y) * W + x] = 1;
    }
    return mask;
}

RectI mask_bbox(const std::vector<std::uint8_t>& mask, int W, int H) {
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; ++y) {
        const std::uint8_t* row = &mask[static_cast<std::size_t>(y) * W];
        for (int x = 0; x < W; ++x) {
            if (!row[x]) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < 0) return {};
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// ── minimal PNG writer (stored-deflate; no external deps) ────────────────

std::uint32_t crc32_of(const std::uint8_t* data, std::size_t n,
                       std::uint32_t crc = 0xFFFFFFFFu) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        init = true;
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    for (std::size_t i = 0; i < n; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void put_be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24));
    v.push_back(static_cast<std::uint8_t>(x >> 16));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}

void png_chunk(std::vector<std::uint8_t>& out, const char* type,
               const std::vector<std::uint8_t>& payload) {
    put_be32(out, static_cast<std::uint32_t>(payload.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    const std::uint32_t crc =
        crc32_of(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu;
    put_be32(out, crc);
}

bool write_png(const std::string& path, const Buffer& img) {
    // Filtered scanlines (filter byte 0 per row).
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(img.h) * (img.w * 4 + 1));
    for (int y = 0; y < img.h; ++y) {
        raw.push_back(0);
        const std::uint8_t* row =
            &img.px[static_cast<std::size_t>(y) * img.w * 4];
        raw.insert(raw.end(), row, row + static_cast<std::size_t>(img.w) * 4);
    }
    // adler32
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    // zlib stream with stored blocks
    std::vector<std::uint8_t> idat;
    idat.push_back(0x78);
    idat.push_back(0x01);
    std::size_t off = 0;
    while (off < raw.size()) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        idat.push_back(last ? 1 : 0);
        idat.push_back(static_cast<std::uint8_t>(n & 0xFF));
        idat.push_back(static_cast<std::uint8_t>(n >> 8));
        idat.push_back(static_cast<std::uint8_t>(~n & 0xFF));
        idat.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
        idat.insert(idat.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    put_be32(idat, (b << 16) | a);

    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A,
                                  0x0A};
    std::vector<std::uint8_t> ihdr;
    put_be32(ihdr, static_cast<std::uint32_t>(img.w));
    put_be32(ihdr, static_cast<std::uint32_t>(img.h));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // color type RGBA
    ihdr.push_back(0);   // compression
    ihdr.push_back(0);   // filter
    ihdr.push_back(0);   // interlace
    png_chunk(out, "IHDR", ihdr);
    png_chunk(out, "IDAT", idat);
    png_chunk(out, "IEND", {});

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()),
            static_cast<std::streamsize>(out.size()));
    return f.good();
}

}  // namespace

// ── stroke / move / preview scratch state ────────────────────────────────

struct PhotoDoc::StrokeState {
    std::string tool;
    int layer_id = 0;
    double r = 12.0;      // radius, px
    double hard = 0.7;    // 0..1
    double op = 1.0;      // stroke ceiling opacity
    double flow = 1.0;    // per-stamp alpha
    RGB color{};
    Buffer base;          // layer pixels at stroke start
    Buffer buf;           // accumulated stamps
    double last_x = 0, last_y = 0;
    RectI bbox{};         // dirty region of buf so far
    // clone
    double clone_dx = 0, clone_dy = 0;
    Buffer clone_src;     // snapshot of the clone source layer
    bool has_clone = false;
    // history brush
    BufferPtr history_src;
};

struct PhotoDoc::MoveState {
    int layer_id = 0;
    Buffer base;
    int last_dx = 0, last_dy = 0;
};

struct PhotoDoc::PreviewState {
    int layer_id = 0;
    RectI region{};
    Buffer base;  // region-sized copy of the layer
};

// ── construction / documents ─────────────────────────────────────────────

PhotoDoc::PhotoDoc(int width, int height) {
    w_ = std::max(1, width);
    h_ = std::max(1, height);
    composite_ = Buffer(w_, h_);
    display_ = Buffer(w_, h_);
    Layer base;
    base.id = next_id_++;
    base.name = "Background";
    base.pixels = std::make_shared<Buffer>(w_, h_);
    layers_.push_back(std::move(base));
    active_ = 0;
    reset_history("New Document", "file");
    mark_all_dirty();
}

PhotoDoc::~PhotoDoc() { detach(); }

void PhotoDoc::new_document(int width, int height,
                            const std::string& background_hex) {
    cancel_stroke();
    cancel_preview();
    move_.reset();
    w_ = std::max(1, width);
    h_ = std::max(1, height);
    composite_ = Buffer(w_, h_);
    display_ = Buffer(w_, h_);
    layers_.clear();
    Layer base;
    base.id = next_id_++;
    base.name = "Background";
    base.locked = true;
    base.pixels = std::make_shared<Buffer>(w_, h_);
    RGB bg{};
    if (parse_hex(background_hex, bg)) {
        for (std::size_t i = 0; i < base.pixels->px.size(); i += 4) {
            base.pixels->px[i] = static_cast<std::uint8_t>(bg.r);
            base.pixels->px[i + 1] = static_cast<std::uint8_t>(bg.g);
            base.pixels->px[i + 2] = static_cast<std::uint8_t>(bg.b);
            base.pixels->px[i + 3] = 255;
        }
    }
    layers_.push_back(std::move(base));
    active_ = 0;
    sel_ = {};
    pen_pts_.clear();
    clone_source_set_ = false;
    reset_history("New Document", "file");
    mark_all_dirty();
    need_fit_ = true;
    sync_thumb_handlers();
    request_repaint();
}

// ── view transform ───────────────────────────────────────────────────────

void PhotoDoc::set_pan(double px, double py) {
    pan_x_ = px;
    pan_y_ = py;
    request_repaint();
}

void PhotoDoc::set_zoom(double z) {
    zoom_ = clampd(z, 0.05, 16.0);
    request_repaint();
}

void PhotoDoc::set_zoom_at(double z, double client_x, double client_y) {
    z = clampd(z, 0.05, 16.0);
    double bx = 0, by = 0;
    screen_to_doc(client_x, client_y, bx, by);
    zoom_ = z;
    double ax = 0, ay = 0;
    screen_to_doc(client_x, client_y, ax, ay);
    pan_x_ += (ax - bx) * z;
    pan_y_ += (ay - by) * z;
    request_repaint();
}

void PhotoDoc::fit_to_screen() {
    if (stage_rect_.w < 8 || stage_rect_.h < 8) {
        need_fit_ = true;
        return;
    }
    need_fit_ = false;
    const double z = std::min((stage_rect_.w - 48.0) / std::max(1, w_),
                              (stage_rect_.h - 48.0) / std::max(1, h_));
    zoom_ = clampd(z, 0.05, 16.0);
    pan_x_ = 0;
    pan_y_ = 0;
    request_repaint();
}

void PhotoDoc::doc_origin(double& x, double& y) const {
    const double cx = stage_rect_.x + stage_rect_.w / 2.0 + pan_x_;
    const double cy = stage_rect_.y + stage_rect_.h / 2.0 + pan_y_;
    x = cx - w_ * zoom_ / 2.0;
    y = cy - h_ * zoom_ / 2.0;
}

void PhotoDoc::screen_to_doc(double client_x, double client_y, double& doc_x,
                             double& doc_y) const {
    double ox = 0, oy = 0;
    doc_origin(ox, oy);
    doc_x = (client_x - ox) / zoom_;
    doc_y = (client_y - oy) / zoom_;
}

// ── layer bookkeeping ────────────────────────────────────────────────────

PhotoDoc::Layer* PhotoDoc::active_ptr() {
    if (active_ < 0 || active_ >= static_cast<int>(layers_.size()))
        return nullptr;
    return &layers_[active_];
}
const PhotoDoc::Layer* PhotoDoc::active_ptr() const {
    if (active_ < 0 || active_ >= static_cast<int>(layers_.size()))
        return nullptr;
    return &layers_[active_];
}
PhotoDoc::Layer* PhotoDoc::find_layer(int id) {
    for (auto& l : layers_)
        if (l.id == id) return &l;
    return nullptr;
}
PhotoDoc::Layer* PhotoDoc::unlocked_active() {
    Layer* l = active_ptr();
    return (l == nullptr || l->locked) ? nullptr : l;
}

Buffer& PhotoDoc::writable(Layer& layer) {
    if (!layer.pixels) layer.pixels = std::make_shared<Buffer>(w_, h_);
    if (layer.pixels.use_count() > 1) {
        layer.pixels = std::make_shared<Buffer>(*layer.pixels);
    }
    return *layer.pixels;
}

void PhotoDoc::mark_layer_dirty(Layer& layer, const RectI& rect) {
    layer.pixel_rev = ++revision_;
    pending_dirty_ = union_rect(pending_dirty_, intersect(rect, doc_rect()));
    request_repaint();
}

void PhotoDoc::mark_all_dirty() {
    ++revision_;
    full_dirty_ = true;
    for (auto& l : layers_) l.pixel_rev = revision_;
    request_repaint();
}

std::vector<LayerInfo> PhotoDoc::layers() const {
    std::vector<LayerInfo> out;
    out.reserve(layers_.size());
    for (const auto& l : layers_) {
        out.push_back({l.id, l.name, l.kind, l.blend, l.visible, l.locked,
                       l.opacity, l.fill});
    }
    return out;
}

LayerInfo PhotoDoc::active_layer() const {
    const Layer* l = active_ptr();
    if (l == nullptr) return {};
    return {l->id, l->name, l->kind, l->blend, l->visible, l->locked,
            l->opacity, l->fill};
}

int PhotoDoc::active_id() const {
    const Layer* l = active_ptr();
    return l != nullptr ? l->id : 0;
}

bool PhotoDoc::set_active_index(int index) {
    if (index < 0 || index >= static_cast<int>(layers_.size())) return false;
    active_ = index;
    return true;
}

bool PhotoDoc::set_active_id(int id) {
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        if (layers_[i].id == id) {
            active_ = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

int PhotoDoc::add_layer(const std::string& name, const std::string& blend,
                        double opacity01, const std::string& label,
                        const std::string& icon) {
    Layer l;
    l.id = next_id_++;
    l.name = name.empty()
                 ? "Layer " + std::to_string(layers_.size())
                 : name;
    l.blend = blend.empty() ? "source-over" : blend;
    l.opacity = clampd(opacity01, 0.0, 1.0);
    l.pixels = std::make_shared<Buffer>(w_, h_);
    layers_.push_back(std::move(l));
    active_ = static_cast<int>(layers_.size()) - 1;
    // Transparent new layer: composite unchanged, but panels/thumbs move.
    ++revision_;
    snapshot(label.empty() ? "New Layer" : label,
             icon.empty() ? "plus" : icon);
    sync_thumb_handlers();
    request_repaint();
    return layers_.back().id;
}

bool PhotoDoc::duplicate_active() {
    const Layer* src = active_ptr();
    if (src == nullptr) return false;
    Layer copy;
    copy.id = next_id_++;
    copy.name = src->name + " copy";
    copy.kind = src->kind;
    copy.blend = src->blend;
    copy.visible = src->visible;
    copy.opacity = src->opacity;
    copy.fill = src->fill;
    copy.pixels = src->pixels;  // COW: shares until either side draws
    copy.pixel_rev = ++revision_;
    layers_.insert(layers_.begin() + active_ + 1, std::move(copy));
    ++active_;
    mark_all_dirty();
    snapshot("Duplicate Layer", "duplicate");
    sync_thumb_handlers();
    return true;
}

bool PhotoDoc::delete_active() {
    if (layers_.size() <= 1) return false;
    Layer* l = active_ptr();
    if (l == nullptr) return false;
    layers_.erase(layers_.begin() + active_);
    active_ = std::max(0, active_ - 1);
    mark_all_dirty();
    snapshot("Delete Layer", "trash");
    sync_thumb_handlers();
    return true;
}

bool PhotoDoc::move_active(int dir) {
    const int i = active_;
    const int j = i + (dir > 0 ? 1 : -1);
    if (dir == 0 || i < 0 || j < 0 ||
        j >= static_cast<int>(layers_.size()))
        return false;
    std::swap(layers_[i], layers_[j]);
    active_ = j;
    mark_all_dirty();
    snapshot("Reorder Layer", "layers");
    return true;
}

bool PhotoDoc::reorder_layer(int id, int new_index) {
    int from = -1;
    for (std::size_t i = 0; i < layers_.size(); ++i)
        if (layers_[i].id == id) from = static_cast<int>(i);
    if (from < 0) return false;
    new_index = clampi(new_index, 0, static_cast<int>(layers_.size()) - 1);
    if (new_index == from) return false;
    Layer moved = std::move(layers_[from]);
    layers_.erase(layers_.begin() + from);
    layers_.insert(layers_.begin() + new_index, std::move(moved));
    active_ = new_index;
    mark_all_dirty();
    snapshot("Reorder Layers", "layers");
    return true;
}

bool PhotoDoc::rename_layer(int id, const std::string& name) {
    Layer* l = find_layer(id);
    if (l == nullptr || name.empty()) return false;
    if (l->name == name) return true;
    l->name = name;
    ++revision_;
    snapshot("Rename Layer", "edit");
    return true;
}

bool PhotoDoc::toggle_visible(int id) {
    Layer* l = find_layer(id);
    if (l == nullptr) return false;
    l->visible = !l->visible;
    mark_all_dirty();
    snapshot(l->visible ? "Show Layer" : "Hide Layer", "eye");
    return true;
}

bool PhotoDoc::set_active_opacity(double v01) {
    Layer* l = active_ptr();
    if (l == nullptr) return false;
    l->opacity = clampd(v01, 0.0, 1.0);
    mark_all_dirty();
    return true;
}

bool PhotoDoc::set_active_fill(double v01) {
    Layer* l = active_ptr();
    if (l == nullptr) return false;
    l->fill = clampd(v01, 0.0, 1.0);
    mark_all_dirty();
    return true;
}

bool PhotoDoc::set_active_blend(const std::string& blend) {
    Layer* l = active_ptr();
    if (l == nullptr || blend.empty()) return false;
    l->blend = blend;
    mark_all_dirty();
    snapshot("Blend: " + blend, "layers");
    return true;
}

bool PhotoDoc::set_active_locked(bool locked) {
    Layer* l = active_ptr();
    if (l == nullptr || l->locked == locked) return false;
    l->locked = locked;
    ++revision_;
    snapshot(locked ? "Lock Layer" : "Unlock Layer", "lock");
    return true;
}

bool PhotoDoc::merge_down() {
    if (active_ <= 0) return false;
    Layer& top = layers_[active_];
    Layer& bottom = layers_[active_ - 1];
    Buffer& dst = writable(bottom);
    compose_over(dst, *top.pixels, doc_rect(), top.opacity * top.fill,
                 parse_blend(top.blend));
    layers_.erase(layers_.begin() + active_);
    --active_;
    mark_all_dirty();
    snapshot("Merge Down", "compress");
    sync_thumb_handlers();
    return true;
}

bool PhotoDoc::flatten() {
    Layer flat;
    flat.id = next_id_++;
    flat.name = "Background";
    flat.pixels = std::make_shared<Buffer>(w_, h_);
    // Web parity: flatten starts from a white base, then draws the render.
    for (std::size_t i = 0; i < flat.pixels->px.size(); ++i)
        flat.pixels->px[i] = 255;
    flush_composite();
    compose_over(*flat.pixels, composite_, doc_rect(), 1.0, Blend::Normal);
    layers_.clear();
    layers_.push_back(std::move(flat));
    active_ = 0;
    mark_all_dirty();
    snapshot("Flatten Image", "layers");
    sync_thumb_handlers();
    return true;
}

// ── selection ────────────────────────────────────────────────────────────

void PhotoDoc::set_selection(int x, int y, int w, int h) {
    if (w > 0 && h > 0) {
        sel_ = intersect({x, y, w, h}, doc_rect());
    } else {
        sel_ = {};
    }
}

void PhotoDoc::clear_selection() { sel_ = {}; }

void PhotoDoc::select_all() { sel_ = doc_rect(); }

bool PhotoDoc::delete_selection_pixels() {
    Layer* l = unlocked_active();
    if (l == nullptr || sel_.empty()) return false;
    clear_rect(writable(*l), sel_);
    mark_layer_dirty(*l, sel_);
    snapshot("Clear", "delete");
    return true;
}

// ── brush engine (paint.js) ──────────────────────────────────────────────

namespace {

// Soft round dab (paint.js stamp): solid color, radial alpha ramp.
// hardness >= 0.985 → hard circle w/ AA edge; else linear ramp from
// r*hardness to r (canvas radial-gradient look).
void stamp_dab(Buffer& buf, double cx, double cy, double r, RGB color,
               double hardness, double alpha, RectI& bbox) {
    const RectI rect = intersect(
        {static_cast<int>(cx - r) - 1, static_cast<int>(cy - r) - 1,
         static_cast<int>(r * 2) + 3, static_cast<int>(r * 2) + 3},
        {0, 0, buf.w, buf.h});
    if (rect.empty()) return;
    const double inner = r * clampd(hardness, 0.0, 1.0);
    for (int y = rect.y; y < rect.y + rect.h; ++y) {
        std::uint8_t* dp =
            &buf.px[(static_cast<std::size_t>(y) * buf.w + rect.x) * 4];
        for (int x = rect.x; x < rect.x + rect.w; ++x, dp += 4) {
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            double cov;
            if (hardness >= 0.985) {
                cov = clampd(r - d + 0.5, 0.0, 1.0);
            } else if (d <= inner) {
                cov = 1.0;
            } else if (d >= r) {
                cov = 0.0;
            } else {
                cov = 1.0 - (d - inner) / (r - inner);
            }
            if (cov > 0.0)
                over_pixel(dp, color.r, color.g, color.b, alpha * cov);
        }
    }
    bbox = union_rect(bbox, rect);
}

// Clip-to-circle image stamp (clone / history brushes): copies src pixels
// (with an offset for clone) into buf inside the dab circle at `alpha`.
void stamp_image(Buffer& buf, const Buffer& src, double off_x, double off_y,
                 double cx, double cy, double r, double alpha, RectI& bbox) {
    const RectI rect = intersect(
        {static_cast<int>(cx - r) - 1, static_cast<int>(cy - r) - 1,
         static_cast<int>(r * 2) + 3, static_cast<int>(r * 2) + 3},
        {0, 0, buf.w, buf.h});
    if (rect.empty()) return;
    for (int y = rect.y; y < rect.y + rect.h; ++y) {
        std::uint8_t* dp =
            &buf.px[(static_cast<std::size_t>(y) * buf.w + rect.x) * 4];
        for (int x = rect.x; x < rect.x + rect.w; ++x, dp += 4) {
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const double cov = clampd(r - d + 0.5, 0.0, 1.0);
            if (cov <= 0.0) continue;
            const int sxp = x + static_cast<int>(std::lround(off_x));
            const int syp = y + static_cast<int>(std::lround(off_y));
            if (sxp < 0 || syp < 0 || sxp >= src.w || syp >= src.h) continue;
            const std::uint8_t* sp =
                &src.px[(static_cast<std::size_t>(syp) * src.w + sxp) * 4];
            const double sa = sp[3] / 255.0 * alpha * cov;
            over_pixel(dp, sp[0], sp[1], sp[2], sa);
        }
    }
    bbox = union_rect(bbox, rect);
}

}  // namespace

bool PhotoDoc::begin_stroke(const std::string& tool, double x, double y,
                            double size_px, double hardness01,
                            double opacity01, double flow01,
                            const std::string& color_hex) {
    cancel_stroke();
    Layer* layer = unlocked_active();
    if (layer == nullptr) return false;

    auto st = std::make_unique<StrokeState>();
    st->tool = tool;
    st->layer_id = layer->id;
    st->r = std::max(0.5, size_px / 2.0);
    st->hard = tool == "pencil" ? 1.0 : clampd(hardness01, 0.0, 1.0);
    st->op = clampd(opacity01, 0.0, 1.0);
    st->flow = clampd(flow01, 0.0, 1.0);
    parse_hex(color_hex, st->color);
    st->base = *layer->pixels;  // stroke replays over this baseline
    st->buf = Buffer(w_, h_);
    st->last_x = x;
    st->last_y = y;

    if (tool == "clone") {
        if (!clone_source_set_) return false;
        st->has_clone = true;
        st->clone_dx = clone_x_ - x;
        st->clone_dy = clone_y_ - y;
        // Snapshot the clone source layer so mid-stroke commits don't
        // feed back into the source (the web reads the live canvas; the
        // visible difference within one stroke is negligible).
        Layer* src = find_layer(clone_layer_id_);
        st->clone_src = src != nullptr ? *src->pixels : st->base;
    }
    if (tool == "history") {
        if (history_source_ >= 0 &&
            history_source_ < static_cast<int>(history_.size())) {
            for (const auto& lr : history_[history_source_].layers) {
                if (lr.id == layer->id) {
                    st->history_src = lr.pixels;
                    break;
                }
            }
        }
        if (!st->history_src) return false;  // web paints nothing
    }

    stroke_ = std::move(st);
    stroke_to(x, y);
    return true;
}

void PhotoDoc::stroke_to(double x, double y) {
    if (!stroke_) return;
    StrokeState& s = *stroke_;
    Layer* layer = find_layer(s.layer_id);
    if (layer == nullptr) { cancel_stroke(); return; }

    const double fx = s.last_x, fy = s.last_y;
    const double dx = x - fx, dy = y - fy;
    const double dist = std::hypot(dx, dy);
    const double sp =
        std::max(0.6, s.r * (s.tool == "pencil" ? 0.5 : 0.16));
    const int n = std::max(1, static_cast<int>(dist / sp));

    if (s.tool == "smudge" || s.tool == "blur") {
        // directSmear: mutate the layer in place per dab.
        Buffer& dst = writable(*layer);
        RectI dirty{};
        for (int i = 1; i <= n; ++i) {
            const double px = fx + dx * i / n, py = fy + dy * i / n;
            const RectI rect = intersect(
                {static_cast<int>(px - s.r) - 1,
                 static_cast<int>(py - s.r) - 1,
                 static_cast<int>(s.r * 2) + 3,
                 static_cast<int>(s.r * 2) + 3},
                doc_rect());
            if (rect.empty()) continue;
            if (s.tool == "smudge") {
                // draw the layer itself offset by -(delta)*0.6 inside the
                // dab (drawImage snapshots, so copy first)
                Buffer snap(rect.w, rect.h);
                const int ox = static_cast<int>(std::lround(-dx * 0.6));
                const int oy = static_cast<int>(std::lround(-dy * 0.6));
                for (int yy = 0; yy < rect.h; ++yy) {
                    for (int xx = 0; xx < rect.w; ++xx) {
                        const int sxp = clampi(rect.x + xx - ox, 0, w_ - 1);
                        const int syp = clampi(rect.y + yy - oy, 0, h_ - 1);
                        std::memcpy(
                            &snap.px[(static_cast<std::size_t>(yy) * rect.w +
                                      xx) * 4],
                            &dst.px[(static_cast<std::size_t>(syp) * w_ +
                                     sxp) * 4],
                            4);
                    }
                }
                const double a = s.op * 0.5;
                for (int yy = 0; yy < rect.h; ++yy) {
                    std::uint8_t* dp =
                        &dst.px[(static_cast<std::size_t>(rect.y + yy) * w_ +
                                 rect.x) * 4];
                    for (int xx = 0; xx < rect.w; ++xx, dp += 4) {
                        const double d = std::hypot(rect.x + xx + 0.5 - px,
                                                    rect.y + yy + 0.5 - py);
                        const double cov = clampd(s.r - d + 0.5, 0.0, 1.0);
                        if (cov <= 0) continue;
                        const std::uint8_t* spx =
                            &snap.px[(static_cast<std::size_t>(yy) * rect.w +
                                      xx) * 4];
                        const double sa = spx[3] / 255.0 * a * cov;
                        over_pixel(dp, spx[0], spx[1], spx[2], sa);
                    }
                }
            } else {  // blur: draw the blurred stroke base into the dab
                Buffer blurred(rect.w, rect.h);
                for (int yy = 0; yy < rect.h; ++yy) {
                    std::memcpy(
                        &blurred.px[static_cast<std::size_t>(yy) * rect.w *
                                    4],
                        &s.base.px[(static_cast<std::size_t>(rect.y + yy) *
                                        w_ + rect.x) * 4],
                        static_cast<std::size_t>(rect.w) * 4);
                }
                gaussian_blur(blurred, {0, 0, rect.w, rect.h},
                              std::max(1.0, s.r / 6.0));
                const double a = s.op * 0.6;
                for (int yy = 0; yy < rect.h; ++yy) {
                    std::uint8_t* dp =
                        &dst.px[(static_cast<std::size_t>(rect.y + yy) * w_ +
                                 rect.x) * 4];
                    for (int xx = 0; xx < rect.w; ++xx, dp += 4) {
                        const double d = std::hypot(rect.x + xx + 0.5 - px,
                                                    rect.y + yy + 0.5 - py);
                        const double cov = clampd(s.r - d + 0.5, 0.0, 1.0);
                        if (cov <= 0) continue;
                        const std::uint8_t* spx =
                            &blurred.px[(static_cast<std::size_t>(yy) *
                                             rect.w + xx) * 4];
                        const double sa = spx[3] / 255.0 * a * cov;
                        over_pixel(dp, spx[0], spx[1], spx[2], sa);
                    }
                }
            }
            dirty = union_rect(dirty, rect);
        }
        s.last_x = x;
        s.last_y = y;
        if (!dirty.empty()) mark_layer_dirty(*layer, dirty);
        return;
    }

    // Stamp family: accumulate dabs into the stroke buffer, then replay
    // base + buffer (commitLive).
    for (int i = 1; i <= n; ++i) {
        const double px = fx + dx * i / n, py = fy + dy * i / n;
        if (s.tool == "clone") {
            stamp_image(s.buf, s.clone_src, s.clone_dx, s.clone_dy, px, py,
                        s.r, s.flow, s.bbox);
        } else if (s.tool == "history") {
            stamp_image(s.buf, *s.history_src, 0, 0, px, py, s.r, s.flow,
                        s.bbox);
        } else if (s.tool == "eraser") {
            stamp_dab(s.buf, px, py, s.r, {0, 0, 0}, s.hard, s.flow, s.bbox);
        } else if (s.tool == "dodge") {
            stamp_dab(s.buf, px, py, s.r, {255, 255, 255}, s.hard, s.flow,
                      s.bbox);
        } else if (s.tool == "burn") {
            stamp_dab(s.buf, px, py, s.r, {0, 0, 0}, s.hard, s.flow,
                      s.bbox);
        } else {  // brush / pencil
            stamp_dab(s.buf, px, py, s.r, s.color, s.hard, s.flow, s.bbox);
        }
    }
    s.last_x = x;
    s.last_y = y;

    // commitLive: layer = base, then buf over it with the tool's mode,
    // clipped to the selection.
    RectI region = intersect(s.bbox, sel_or_doc());
    if (region.empty()) return;
    Buffer& dst = writable(*layer);
    copy_rect(dst, s.base, s.bbox);
    Blend mode = Blend::Normal;
    if (s.tool == "eraser") mode = Blend::DestinationOut;
    else if (s.tool == "dodge") mode = Blend::Screen;
    else if (s.tool == "burn") mode = Blend::Multiply;
    compose_over(dst, s.buf, region, s.op, mode);
    mark_layer_dirty(*layer, s.bbox);
}

void PhotoDoc::end_stroke() {
    if (!stroke_) return;
    static const std::unordered_map<std::string, std::pair<const char*,
                                                           const char*>>
        kNames = {
            {"brush", {"Brush", "brush"}},
            {"pencil", {"Pencil", "pencil"}},
            {"eraser", {"Eraser", "eraser"}},
            {"clone", {"Clone Stamp", "stamp"}},
            {"history", {"History Brush", "history-brush"}},
            {"dodge", {"Dodge", "dodge"}},
            {"burn", {"Burn", "burn"}},
            {"smudge", {"Smudge", "smudge"}},
            {"blur", {"Blur", "blur"}},
        };
    const auto it = kNames.find(stroke_->tool);
    stroke_.reset();
    if (it != kNames.end())
        snapshot(it->second.first, it->second.second);
    else
        snapshot("Paint", "brush");
}

void PhotoDoc::cancel_stroke() { stroke_.reset(); }

void PhotoDoc::set_clone_source(double x, double y) {
    clone_source_set_ = true;
    clone_x_ = x;
    clone_y_ = y;
    clone_layer_id_ = active_id();
}

// ── move tool ────────────────────────────────────────────────────────────

bool PhotoDoc::begin_move() {
    Layer* l = unlocked_active();
    if (l == nullptr) return false;
    auto mv = std::make_unique<MoveState>();
    mv->layer_id = l->id;
    mv->base = *l->pixels;
    move_ = std::move(mv);
    return true;
}

void PhotoDoc::move_to(double dxf, double dyf) {
    if (!move_) return;
    Layer* l = find_layer(move_->layer_id);
    if (l == nullptr) { move_.reset(); return; }
    const int dx = static_cast<int>(std::lround(dxf));
    const int dy = static_cast<int>(std::lround(dyf));
    if (dx == move_->last_dx && dy == move_->last_dy) return;
    move_->last_dx = dx;
    move_->last_dy = dy;
    Buffer& dst = writable(*l);
    clear_rect(dst, doc_rect());
    const Buffer& src = move_->base;
    const RectI dr = intersect({dx, dy, w_, h_}, doc_rect());
    for (int y = dr.y; y < dr.y + dr.h; ++y) {
        std::memcpy(&dst.px[(static_cast<std::size_t>(y) * w_ + dr.x) * 4],
                    &src.px[(static_cast<std::size_t>(y - dy) * w_ +
                             (dr.x - dx)) * 4],
                    static_cast<std::size_t>(dr.w) * 4);
    }
    mark_layer_dirty(*l, doc_rect());
}

void PhotoDoc::end_move() {
    if (!move_) return;
    move_.reset();
    snapshot("Move Layer", "move");
}

// ── click tools ──────────────────────────────────────────────────────────

bool PhotoDoc::fill_at(double xd, double yd, const std::string& hex,
                       double tolerance, bool contiguous, double opacity01) {
    Layer* l = unlocked_active();
    if (l == nullptr) return false;
    const int sx = static_cast<int>(std::floor(xd));
    const int sy = static_cast<int>(std::floor(yd));
    if (sx < 0 || sy < 0 || sx >= w_ || sy >= h_) return false;
    RGB color{};
    if (!parse_hex(hex, color)) return false;
    const auto mask = flood_mask(*l->pixels, sx, sy, tolerance, contiguous);
    const RectI bbox = intersect(mask_bbox(mask, w_, h_), sel_or_doc());
    if (bbox.empty()) return false;
    Buffer& dst = writable(*l);
    const double a = clampd(opacity01, 0.0, 1.0);
    for (int y = bbox.y; y < bbox.y + bbox.h; ++y) {
        std::uint8_t* dp =
            &dst.px[(static_cast<std::size_t>(y) * w_ + bbox.x) * 4];
        const std::uint8_t* mp = &mask[static_cast<std::size_t>(y) * w_ +
                                       bbox.x];
        for (int x = 0; x < bbox.w; ++x, dp += 4, ++mp) {
            if (*mp) over_pixel(dp, color.r, color.g, color.b, a);
        }
    }
    mark_layer_dirty(*l, bbox);
    snapshot("Paint Bucket", "fill");
    return true;
}

RectI PhotoDoc::wand_select(double xd, double yd, double tolerance,
                            bool contiguous) {
    const int sx = static_cast<int>(std::floor(xd));
    const int sy = static_cast<int>(std::floor(yd));
    if (sx < 0 || sy < 0 || sx >= w_ || sy >= h_) return {};
    flush_composite();
    const auto mask = flood_mask(composite_, sx, sy, tolerance, contiguous);
    const RectI bbox = mask_bbox(mask, w_, h_);
    if (!bbox.empty()) sel_ = bbox;
    return bbox;
}

std::string PhotoDoc::pick_color(double xd, double yd) const {
    const int x = clampi(static_cast<int>(std::floor(xd)), 0, w_ - 1);
    const int y = clampi(static_cast<int>(std::floor(yd)), 0, h_ - 1);
    // const-cast: flushing the pending composite is logically const.
    const_cast<PhotoDoc*>(this)->flush_composite();
    const std::uint8_t* p =
        &composite_.px[(static_cast<std::size_t>(y) * w_ + x) * 4];
    return to_hex(p[0], p[1], p[2]);
}

bool PhotoDoc::apply_gradient(double x0, double y0, double x1, double y1,
                              const std::string& hex, double opacity01) {
    Layer* l = unlocked_active();
    if (l == nullptr) return false;
    RGB c{};
    if (!parse_hex(hex, c)) return false;
    const std::vector<Stop> stops = {
        {0.0, static_cast<double>(c.r), static_cast<double>(c.g),
         static_cast<double>(c.b), 1.0},
        {1.0, static_cast<double>(c.r), static_cast<double>(c.g),
         static_cast<double>(c.b), 0.0},
    };
    fill_linear_gradient(writable(*l), x0, y0, x1, y1, stops, sel_or_doc(),
                         clampd(opacity01, 0.0, 1.0));
    mark_layer_dirty(*l, sel_or_doc());
    snapshot("Gradient", "fill");
    return true;
}

int PhotoDoc::place_type(double x, double y, const std::string& text,
                         double size_px, const std::string& hex, bool bold) {
    RGB c{};
    if (!parse_hex(hex, c)) c = {0, 0, 0};
    const std::string content = text.empty() ? "Text" : text;
    Layer l;
    l.id = next_id_++;
    l.name = "T " + content;
    l.kind = "text";
    l.pixels = std::make_shared<Buffer>(w_, h_);
    raster_text(*l.pixels, x, y, content, std::max(2.0, size_px), c.r, c.g,
                c.b, 1.0, bold);
    layers_.push_back(std::move(l));
    active_ = static_cast<int>(layers_.size()) - 1;
    mark_all_dirty();
    snapshot("Type: " + content, "edit");
    sync_thumb_handlers();
    return layers_.back().id;
}

bool PhotoDoc::draw_shape(double x0, double y0, double x1, double y1,
                          const std::string& hex, double corner_radius) {
    Layer* l = unlocked_active();
    if (l == nullptr) return false;
    RGB c{};
    if (!parse_hex(hex, c)) return false;
    const double x = std::min(x0, x1), y = std::min(y0, y1);
    const double w = std::abs(x1 - x0), h = std::abs(y1 - y0);
    if (w < 1 || h < 1) return false;
    fill_rounded_rect(writable(*l), x, y, w, h,
                      std::max(0.0, corner_radius), c.r, c.g, c.b, 1.0,
                      doc_rect());
    const RectI dirty{static_cast<int>(x) - 1, static_cast<int>(y) - 1,
                      static_cast<int>(w) + 3, static_cast<int>(h) + 3};
    mark_layer_dirty(*l, dirty);
    snapshot("Rectangle", "shape");
    return true;
}

void PhotoDoc::pen_add_point(double x, double y) {
    pen_pts_.push_back({x, y});
    request_repaint();
}

void PhotoDoc::pen_clear() {
    if (pen_pts_.empty()) return;
    pen_pts_.clear();
    request_repaint();
}

// ── edit-menu ops ────────────────────────────────────────────────────────

bool PhotoDoc::fill_layer(const std::string& hex, double opacity01,
                          const std::string& blend,
                          const std::string& label) {
    Layer* l = unlocked_active();
    if (l == nullptr) return false;
    RGB c{};
    if (!parse_hex(hex, c)) return false;
    const RectI region = sel_or_doc();
    // A solid-color source buffer for the blend machinery.
    Buffer src(w_, h_);
    for (int y = region.y; y < region.y + region.h; ++y) {
        std::uint8_t* dp =
            &src.px[(static_cast<std::size_t>(y) * w_ + region.x) * 4];
        for (int x = 0; x < region.w; ++x, dp += 4) {
            dp[0] = static_cast<std::uint8_t>(c.r);
            dp[1] = static_cast<std::uint8_t>(c.g);
            dp[2] = static_cast<std::uint8_t>(c.b);
            dp[3] = 255;
        }
    }
    compose_over(writable(*l), src, region, clampd(opacity01, 0.0, 1.0),
                 parse_blend(blend));
    mark_layer_dirty(*l, region);
    snapshot(label.empty() ? "Fill" : label, "fill");
    return true;
}

bool PhotoDoc::stroke_selection(const std::string& hex, double width,
                                const std::string& location,
                                double opacity01) {
    Layer* l = unlocked_active();
    if (l == nullptr || width <= 0) return false;
    RGB c{};
    if (!parse_hex(hex, c)) return false;
    RectI s = sel_.empty() ? RectI{1, 1, w_ - 2, h_ - 2} : sel_;
    double off = 0.0;  // center
    if (location == "inside") off = width / 2.0;
    else if (location == "outside") off = -width / 2.0;
    const double x = s.x + off, y = s.y + off;
    const double w = s.w - off * 2, h = s.h - off * 2;
    if (w <= 0 || h <= 0) return false;
    const double a = clampd(opacity01, 0.0, 1.0);
    Buffer& dst = writable(*l);
    // strokeRect: the stroked band spans ±width/2 around the rect path.
    const double half = width / 2.0;
    const RectI outer = intersect(
        {static_cast<int>(x - half) - 1, static_cast<int>(y - half) - 1,
         static_cast<int>(w + width) + 3, static_cast<int>(h + width) + 3},
        doc_rect());
    for (int py = outer.y; py < outer.y + outer.h; ++py) {
        std::uint8_t* dp =
            &dst.px[(static_cast<std::size_t>(py) * w_ + outer.x) * 4];
        for (int px = outer.x; px < outer.x + outer.w; ++px, dp += 4) {
            const double sx = px + 0.5, sy = py + 0.5;
            // distance to the rect path (its outline)
            const double ix = clampd(sx, x, x + w);
            const double iy = clampd(sy, y, y + h);
            double dist;
            if (sx >= x && sx <= x + w && sy >= y && sy <= y + h) {
                dist = std::min({sx - x, x + w - sx, sy - y, y + h - sy});
            } else {
                dist = std::hypot(sx - ix, sy - iy);
            }
            const double cov = clampd(half - dist + 0.5, 0.0, 1.0);
            if (cov > 0) over_pixel(dp, c.r, c.g, c.b, a * cov);
        }
    }
    mark_layer_dirty(*l, outer);
    snapshot("Stroke", "edit");
    return true;
}

// ── document geometry ────────────────────────────────────────────────────

bool PhotoDoc::apply_crop() {
    if (sel_.empty()) return false;
    return crop_to(sel_.x, sel_.y, sel_.w, sel_.h);
}

bool PhotoDoc::crop_to(int x, int y, int w, int h) {
    const RectI r = intersect({x, y, w, h}, doc_rect());
    if (r.w < 4 || r.h < 4) return false;
    for (auto& l : layers_) {
        auto next = std::make_shared<Buffer>(r.w, r.h);
        for (int yy = 0; yy < r.h; ++yy) {
            std::memcpy(
                &next->px[static_cast<std::size_t>(yy) * r.w * 4],
                &l.pixels->px[(static_cast<std::size_t>(r.y + yy) * w_ +
                               r.x) * 4],
                static_cast<std::size_t>(r.w) * 4);
        }
        l.pixels = std::move(next);
    }
    w_ = r.w;
    h_ = r.h;
    composite_ = Buffer(w_, h_);
    display_ = Buffer(w_, h_);
    sel_ = {};
    mark_all_dirty();
    fit_to_screen();
    snapshot("Crop", "clip");
    return true;
}

bool PhotoDoc::resize_image(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == w_ && h == h_) return false;
    for (auto& l : layers_) {
        l.pixels = std::make_shared<Buffer>(scaled_buffer(*l.pixels, w, h));
    }
    w_ = w;
    h_ = h;
    composite_ = Buffer(w_, h_);
    display_ = Buffer(w_, h_);
    sel_ = {};
    mark_all_dirty();
    fit_to_screen();
    snapshot("Image Size", "aspect");
    return true;
}

bool PhotoDoc::resize_canvas(int w, int h, const std::string& anchor) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == w_ && h == h_) return false;
    double ax = 0.5, ay = 0.5;  // mc default
    if (!anchor.empty()) {
        if (anchor[0] == 't') ay = 0.0;
        else if (anchor[0] == 'b') ay = 1.0;
        if (anchor.size() > 1) {
            if (anchor[1] == 'l') ax = 0.0;
            else if (anchor[1] == 'r') ax = 1.0;
        }
    }
    const int dx = static_cast<int>(std::lround((w - w_) * ax));
    const int dy = static_cast<int>(std::lround((h - h_) * ay));
    for (auto& l : layers_) {
        auto next = std::make_shared<Buffer>(w, h);
        const RectI dr = intersect({dx, dy, w_, h_}, {0, 0, w, h});
        for (int yy = dr.y; yy < dr.y + dr.h; ++yy) {
            std::memcpy(
                &next->px[(static_cast<std::size_t>(yy) * w + dr.x) * 4],
                &l.pixels->px[(static_cast<std::size_t>(yy - dy) * w_ +
                               (dr.x - dx)) * 4],
                static_cast<std::size_t>(dr.w) * 4);
        }
        l.pixels = std::move(next);
    }
    w_ = w;
    h_ = h;
    composite_ = Buffer(w_, h_);
    display_ = Buffer(w_, h_);
    sel_ = {};
    mark_all_dirty();
    fit_to_screen();
    snapshot("Canvas Size", "fit");
    return true;
}

// ── adjustments & filters (ui.js pixel formulas) ─────────────────────────

namespace {

void rgb_to_hsv(double r, double g, double b, double& h, double& s,
                double& v) {
    r /= 255; g /= 255; b /= 255;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const double d = mx - mn;
    h = 0;
    if (d > 1e-12) {
        if (mx == r) h = std::fmod((g - b) / d, 6.0);
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h *= 60;
        if (h < 0) h += 360;
    }
    s = mx > 1e-12 ? d / mx : 0;
    v = mx;
}

void hsv_to_rgb(double h, double s, double v, double& r, double& g,
                double& b) {
    h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0) / 360.0;
    const int i = static_cast<int>(std::floor(h * 6));
    const double f = h * 6 - i;
    const double p = v * (1 - s), q = v * (1 - f * s),
                 t = v * (1 - (1 - f) * s);
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    r *= 255; g *= 255; b *= 255;
}

// Applies `kind` in place over `rect` of `buf`, base-independent per pixel.
// Matches ui.js PS.adjust / PS.filter formulas exactly.
void apply_pixels(Buffer& buf, const RectI& rect, const std::string& kind,
                  double a, double b, double c, Rand* rng) {
    const RectI r = intersect(rect, {0, 0, buf.w, buf.h});
    auto for_each = [&](auto&& fn) {
        for (int y = r.y; y < r.y + r.h; ++y) {
            std::uint8_t* dp =
                &buf.px[(static_cast<std::size_t>(y) * buf.w + r.x) * 4];
            for (int x = 0; x < r.w; ++x, dp += 4) fn(dp);
        }
    };

    if (kind == "bc") {  // a = brightness, b = contrast (ui.js formulas)
        const double cc = (b / 100.0) + 1.0;
        const double bb = a * 1.2;
        for_each([&](std::uint8_t* dp) {
            for (int j = 0; j < 3; ++j)
                dp[j] = clamp8((dp[j] - 128.0) * cc + 128.0 + bb);
        });
    } else if (kind == "hsl" || kind == "vibrance") {
        const double dh = kind == "hsl" ? a : 0.0;
        const double ds = (kind == "hsl" ? b : a) / 100.0;
        const double dl = (kind == "hsl" ? c : 0.0) / 100.0;
        for_each([&](std::uint8_t* dp) {
            double h, s, v;
            rgb_to_hsv(dp[0], dp[1], dp[2], h, s, v);
            h = std::fmod(h + dh + 360.0, 360.0);
            s = clampd(s * (1 + ds), 0.0, 1.0);
            v = clampd(v * (1 + dl), 0.0, 1.0);
            double rr, gg, bb2;
            hsv_to_rgb(h, s, v, rr, gg, bb2);
            dp[0] = clamp8(rr);
            dp[1] = clamp8(gg);
            dp[2] = clamp8(bb2);
        });
    } else if (kind == "levels") {
        const double lo = a;
        const double hi = std::max(a + 1.0, b);
        const double g = 100.0 / std::max(10.0, c);
        for_each([&](std::uint8_t* dp) {
            for (int j = 0; j < 3; ++j) {
                double t = (dp[j] - lo) / (hi - lo);
                t = std::pow(clampd(t, 0.0, 1.0), g);
                dp[j] = clamp8(t * 255.0);
            }
        });
    } else if (kind == "generic") {
        for_each([&](std::uint8_t* dp) {
            for (int j = 0; j < 3; ++j) dp[j] = clamp8(dp[j] + a);
        });
    } else if (kind == "invert") {
        for_each([&](std::uint8_t* dp) {
            dp[0] = static_cast<std::uint8_t>(255 - dp[0]);
            dp[1] = static_cast<std::uint8_t>(255 - dp[1]);
            dp[2] = static_cast<std::uint8_t>(255 - dp[2]);
        });
    } else if (kind == "desat") {
        for_each([&](std::uint8_t* dp) {
            const double y2 =
                dp[0] * .299 + dp[1] * .587 + dp[2] * .114;
            dp[0] = dp[1] = dp[2] = clamp8(y2);
        });
    } else if (kind == "threshold") {
        for_each([&](std::uint8_t* dp) {
            const double y2 =
                dp[0] * .299 + dp[1] * .587 + dp[2] * .114;
            const std::uint8_t t = y2 > 128 ? 255 : 0;
            dp[0] = dp[1] = dp[2] = t;
        });
    } else if (kind == "noise") {
        const double amt = a > 0 ? a : 90.0;
        Rand fallback(0x9E3779B9u);
        Rand& rr = rng != nullptr ? *rng : fallback;
        for_each([&](std::uint8_t* dp) {
            const double n = (rr.next() - 0.5) * amt;
            for (int j = 0; j < 3; ++j) dp[j] = clamp8(dp[j] + n);
        });
    } else if (kind == "contrast") {  // CSS contrast(a)
        for_each([&](std::uint8_t* dp) {
            for (int j = 0; j < 3; ++j)
                dp[j] = clamp8((dp[j] - 127.5) * a + 127.5);
        });
    } else if (kind == "saturate") {  // CSS saturate(a) matrix
        const double s = a;
        for_each([&](std::uint8_t* dp) {
            const double rr2 = dp[0], gg = dp[1], bb2 = dp[2];
            const double nr = (0.213 + 0.787 * s) * rr2 +
                              0.715 * (1 - s) * gg + 0.072 * (1 - s) * bb2;
            const double ng = 0.213 * (1 - s) * rr2 +
                              (0.715 + 0.285 * s) * gg +
                              0.072 * (1 - s) * bb2;
            const double nb = 0.213 * (1 - s) * rr2 +
                              0.715 * (1 - s) * gg +
                              (0.072 + 0.928 * s) * bb2;
            dp[0] = clamp8(nr);
            dp[1] = clamp8(ng);
            dp[2] = clamp8(nb);
        });
    } else if (kind == "pixelate") {
        const int block = a >= 2 ? static_cast<int>(a) : 10;
        for (int by = r.y; by < r.y + r.h; by += block) {
            for (int bx = r.x; bx < r.x + r.w; bx += block) {
                const int cxp = clampi(bx + block / 2, r.x, r.x + r.w - 1);
                const int cyp = clampi(by + block / 2, r.y, r.y + r.h - 1);
                const std::uint8_t* sp =
                    &buf.px[(static_cast<std::size_t>(cyp) * buf.w + cxp) *
                            4];
                std::uint8_t px4[4];
                std::memcpy(px4, sp, 4);
                const int ymax = std::min(by + block, r.y + r.h);
                const int xmax = std::min(bx + block, r.x + r.w);
                for (int yy = by; yy < ymax; ++yy) {
                    std::uint8_t* dp =
                        &buf.px[(static_cast<std::size_t>(yy) * buf.w + bx) *
                                4];
                    for (int xx = bx; xx < xmax; ++xx, dp += 4)
                        std::memcpy(dp, px4, 4);
                }
            }
        }
    } else if (kind == "blur") {
        gaussian_blur(buf, r, a);
    } else if (kind == "sharpen") {
        apply_pixels(buf, r, "contrast", 1.4, 0, 0, rng);
        apply_pixels(buf, r, "saturate", 1.1, 0, 0, rng);
    } else if (kind == "emboss") {
        apply_pixels(buf, r, "desat", 0, 0, 0, rng);
        apply_pixels(buf, r, "contrast", 2.0, 0, 0, rng);
    } else if (kind == "findedges") {
        apply_pixels(buf, r, "desat", 0, 0, 0, rng);
        apply_pixels(buf, r, "invert", 0, 0, 0, rng);
        apply_pixels(buf, r, "contrast", 2.5, 0, 0, rng);
    }
}

}  // namespace

bool PhotoDoc::begin_preview() {
    cancel_preview();
    Layer* l = unlocked_active();
    if (l == nullptr) return false;
    auto pv = std::make_unique<PreviewState>();
    pv->layer_id = l->id;
    pv->region = sel_or_doc();
    pv->base = Buffer(pv->region.w, pv->region.h);
    for (int y = 0; y < pv->region.h; ++y) {
        std::memcpy(
            &pv->base.px[static_cast<std::size_t>(y) * pv->region.w * 4],
            &l->pixels->px[(static_cast<std::size_t>(pv->region.y + y) * w_ +
                            pv->region.x) * 4],
            static_cast<std::size_t>(pv->region.w) * 4);
    }
    preview_ = std::move(pv);
    return true;
}

void PhotoDoc::preview_adjust(const std::string& kind, double a, double b,
                              double c) {
    if (!preview_) return;
    Layer* l = find_layer(preview_->layer_id);
    if (l == nullptr) { cancel_preview(); return; }
    // work = base copy, adjusted, written back into the layer region
    Buffer work = preview_->base;
    Rand rng(0xC0FFEEu);
    apply_pixels(work, {0, 0, work.w, work.h}, kind, a, b, c, &rng);
    Buffer& dst = writable(*l);
    for (int y = 0; y < preview_->region.h; ++y) {
        std::memcpy(
            &dst.px[(static_cast<std::size_t>(preview_->region.y + y) * w_ +
                     preview_->region.x) * 4],
            &work.px[static_cast<std::size_t>(y) * work.w * 4],
            static_cast<std::size_t>(work.w) * 4);
    }
    mark_layer_dirty(*l, preview_->region);
}

void PhotoDoc::commit_preview(const std::string& label,
                              const std::string& icon) {
    if (!preview_) return;
    preview_.reset();
    snapshot(label.empty() ? "Adjustment" : label,
             icon.empty() ? "color-grade" : icon);
}

void PhotoDoc::cancel_preview() {
    if (!preview_) return;
    Layer* l = find_layer(preview_->layer_id);
    if (l != nullptr) {
        Buffer& dst = writable(*l);
        for (int y = 0; y < preview_->region.h; ++y) {
            std::memcpy(
                &dst.px[(static_cast<std::size_t>(preview_->region.y + y) *
                             w_ + preview_->region.x) * 4],
                &preview_->base.px[static_cast<std::size_t>(y) *
                                   preview_->region.w * 4],
                static_cast<std::size_t>(preview_->region.w) * 4);
        }
        mark_layer_dirty(*l, preview_->region);
    }
    preview_.reset();
}

bool PhotoDoc::apply_adjust(const std::string& kind, double a,
                            const std::string& label,
                            const std::string& icon) {
    Layer* l = unlocked_active();
    if (l == nullptr) return false;
    const RectI region = sel_or_doc();
    Rand rng(static_cast<std::uint32_t>(revision_ * 2654435761u + 1));
    apply_pixels(writable(*l), region, kind, a, 0, 0, &rng);
    mark_layer_dirty(*l, region);
    snapshot(label.empty() ? kind : label,
             icon.empty() ? "color-grade" : icon);
    return true;
}

// ── place embedded assets (dialogs.js dlgPlace) ─────────────────────────

int PhotoDoc::place_asset(const std::string& asset) {
    Layer l;
    l.id = next_id_++;
    l.name = asset;
    l.pixels = std::make_shared<Buffer>(w_, h_);
    Buffer& px = *l.pixels;
    const double W = w_, H = h_;
    if (asset == "Vignette") {
        fill_radial_gradient(px, W / 2, H / 2, H * 0.3, H * 0.75,
                             {{0.0, 0, 0, 0, 0.0}, {1.0, 0, 0, 0, 0.6}},
                             doc_rect());
        l.blend = "multiply";
    } else if (asset == "Noise texture") {
        Rand rng(0xA11CE5u);
        for (std::size_t i = 0; i < px.px.size(); i += 4) {
            const auto n = static_cast<std::uint8_t>(rng.next() * 255.0);
            px.px[i] = px.px[i + 1] = px.px[i + 2] = n;
            px.px[i + 3] = 40;
        }
        l.blend = "overlay";
    } else if (asset == "Gradient map") {
        fill_linear_gradient(px, 0, 0, W, 0,
                             {{0.0, 0x1f, 0x6f, 0xeb, 1.0},
                              {1.0, 0xff, 0x7a, 0xb8, 1.0}},
                             doc_rect());
        l.blend = "soft-light";
        l.opacity = 0.6;
    } else {  // "Sun flare" (default)
        fill_radial_gradient(px, W * 0.7, H * 0.3, 8, 360,
                             {{0.0, 255, 240, 200, 0.9},
                              {1.0, 255, 200, 120, 0.0}},
                             doc_rect());
        l.blend = "screen";
    }
    layers_.push_back(std::move(l));
    active_ = static_cast<int>(layers_.size()) - 1;
    mark_all_dirty();
    snapshot("Place " + asset, "import");
    sync_thumb_handlers();
    return layers_.back().id;
}

// ── history ──────────────────────────────────────────────────────────────

void PhotoDoc::reset_history(const std::string& label,
                             const std::string& icon) {
    history_.clear();
    history_index_ = -1;
    history_source_ = 0;
    snapshot(label, icon);
}

void PhotoDoc::snapshot(const std::string& name, const std::string& icon) {
    if (name.empty()) return;
    if (history_index_ >= 0 &&
        history_index_ < static_cast<int>(history_.size()) - 1) {
        history_.erase(history_.begin() + history_index_ + 1,
                       history_.end());
    }
    HistoryRec rec;
    rec.name = name;
    rec.icon = icon.empty() ? "edit" : icon;
    rec.active = active_;
    rec.w = w_;
    rec.h = h_;
    rec.layers.reserve(layers_.size());
    for (const auto& l : layers_) {
        rec.layers.push_back({l.id, l.name, l.kind, l.blend, l.visible,
                              l.locked, l.opacity, l.fill, l.pixels});
    }
    history_.push_back(std::move(rec));
    while (history_.size() > kHistoryMax) {
        history_.erase(history_.begin());
        if (history_source_ > 0) --history_source_;
    }
    history_index_ = static_cast<int>(history_.size()) - 1;
    ++revision_;
}

std::vector<HistoryEntry> PhotoDoc::history_entries() const {
    std::vector<HistoryEntry> out;
    out.reserve(history_.size());
    for (const auto& rec : history_) out.push_back({rec.name, rec.icon});
    return out;
}

void PhotoDoc::set_history_source(int index) {
    history_source_ =
        clampi(index, 0, std::max(0, static_cast<int>(history_.size()) - 1));
}

void PhotoDoc::restore(const HistoryRec& rec) {
    cancel_stroke();
    cancel_preview();
    move_.reset();
    if (rec.w != w_ || rec.h != h_) {
        w_ = rec.w;
        h_ = rec.h;
        composite_ = Buffer(w_, h_);
        display_ = Buffer(w_, h_);
        sel_ = {};
    }
    layers_.clear();
    layers_.reserve(rec.layers.size());
    for (const auto& lr : rec.layers) {
        Layer l;
        l.id = lr.id;
        l.name = lr.name;
        l.kind = lr.kind;
        l.blend = lr.blend;
        l.visible = lr.visible;
        l.locked = lr.locked;
        l.opacity = lr.opacity;
        l.fill = lr.fill;
        l.pixels = lr.pixels;  // shared; next edit clones (COW)
        layers_.push_back(std::move(l));
    }
    active_ = clampi(rec.active, 0, static_cast<int>(layers_.size()) - 1);
    sel_ = intersect(sel_, doc_rect());
    mark_all_dirty();
    sync_thumb_handlers();
}

std::string PhotoDoc::undo() {
    if (history_index_ > 0) {
        --history_index_;
        restore(history_[history_index_]);
    }
    return history_.empty() ? std::string{}
                            : history_[history_index_].name;
}

std::string PhotoDoc::redo() {
    if (history_index_ < static_cast<int>(history_.size()) - 1) {
        ++history_index_;
        restore(history_[history_index_]);
    }
    return history_.empty() ? std::string{}
                            : history_[history_index_].name;
}

bool PhotoDoc::jump_to(int index) {
    if (history_.empty()) return false;
    index = clampi(index, 0, static_cast<int>(history_.size()) - 1);
    if (index == history_index_) return false;
    history_index_ = index;
    restore(history_[history_index_]);
    return true;
}

// ── sample scene (engine.js generateSampleScene) ─────────────────────────

void PhotoDoc::load_sample_scene() {
    cancel_stroke();
    cancel_preview();
    move_.reset();
    const double W = w_, H = h_;
    layers_.clear();
    Rand rng(0xDEC1A5u);

    auto make_layer = [&](const char* name, const char* kind,
                          const char* blend, double opacity, bool locked) {
        Layer l;
        l.id = next_id_++;
        l.name = name;
        l.kind = kind;
        l.blend = blend;
        l.opacity = opacity;
        l.locked = locked;
        l.pixels = std::make_shared<Buffer>(w_, h_);
        layers_.push_back(std::move(l));
        return &layers_.back();
    };

    // 1. Background: sunset sky + stars.
    {
        Layer* bg = make_layer("Background", "pixel", "source-over", 1.0,
                               true);
        Buffer& px = *bg->pixels;
        fill_linear_gradient(px, 0, 0, 0, H,
                             {{0.00, 0x0b, 0x14, 0x37, 1.0},
                              {0.45, 0x1d, 0x2b, 0x66, 1.0},
                              {0.72, 0x6a, 0x4a, 0x86, 1.0},
                              {0.88, 0xd9, 0x8a, 0x5a, 1.0},
                              {1.00, 0xf2, 0xc2, 0x77, 1.0}},
                             doc_rect());
        for (int i = 0; i < 90; ++i) {
            const double x = rng.next() * W;
            const double y = rng.next() * H * 0.5;
            const double r = rng.next() * 1.2;
            const double a = (rng.next() * 0.8 + 0.2) * 0.9;
            fill_circle(px, x, y, std::max(0.4, r), 255, 255, 255, a);
        }
    }
    // 2. Sun: radial glow + disc, screen blend.
    {
        Layer* sun = make_layer("Sun", "pixel", "screen", 1.0, false);
        Buffer& px = *sun->pixels;
        const double cx = W * 0.5, cy = H * 0.62;
        fill_radial_gradient(px, cx, cy, 8, 260,
                             {{0.00, 255, 240, 200, 1.00},
                              {0.25, 255, 190, 120, 0.85},
                              {0.60, 255, 140, 90, 0.35},
                              {1.00, 255, 120, 80, 0.00}},
                             doc_rect());
        fill_circle(px, cx, cy, 70, 255, 250, 235, 1.0);
    }
    // 3-4. Mountain ridges (jagged silhouettes, like the web's ridge()).
    auto ridge = [&](Buffer& px, double base_y, double jag, int count,
                     RGB color) {
        std::vector<double> ys(static_cast<std::size_t>(count) + 1);
        for (int i = 0; i <= count; ++i) {
            ys[i] = base_y -
                    std::abs(std::sin(i * 0.7 + 1)) * (40 + (i % 3) * 26) -
                    rng.next() * jag * 4;
        }
        const double seg = W / count;
        for (int x = 0; x < w_; ++x) {
            const double fi = x / seg;
            const int i = clampi(static_cast<int>(fi), 0, count - 1);
            const double f = fi - i;
            const double top = ys[i] + (ys[i + 1] - ys[i]) * f;
            for (int y = std::max(0, static_cast<int>(top)); y < h_; ++y) {
                const double cov =
                    clampd(y + 0.5 - top + 0.5, 0.0, 1.0);
                over_pixel(&px.px[(static_cast<std::size_t>(y) * w_ + x) * 4],
                           color.r, color.g, color.b, cov);
            }
        }
    };
    {
        Layer* far = make_layer("Mountains · Far", "pixel", "source-over",
                                0.85, false);
        ridge(*far->pixels, H * 0.66, 7, 90, {0x34, 0x30, 0x5a});
    }
    {
        Layer* near = make_layer("Mountains · Near", "pixel", "source-over",
                                 1.0, false);
        ridge(*near->pixels, H * 0.76, 5, 150, {0x1a, 0x17, 0x30});
    }
    // 5. Water reflection band, overlay blend.
    {
        Layer* water = make_layer("Water", "pixel", "overlay", 0.55, false);
        fill_linear_gradient(*water->pixels, 0, H * 0.82, 0, H,
                             {{0.0, 255, 200, 150, 0.55},
                              {1.0, 60, 40, 90, 0.20}},
                             {0, static_cast<int>(H * 0.82), w_,
                              h_ - static_cast<int>(H * 0.82)});
    }
    // 6. Title text layer (Roboto stands in for IBM Plex).
    {
        Layer* title = make_layer("“DECIUS”", "text", "source-over", 1.0,
                                  false);
        Buffer& px = *title->pixels;
        // soft drop shadow: blurred black copy at +6y
        Buffer shadow(w_, h_);
        raster_text(shadow, 64, 70, "DECIUS", 120, 0, 0, 0, 0.35, true);
        gaussian_blur(shadow, {0, 40, std::min(w_, 700), 220}, 6.0);
        compose_over(px, shadow, doc_rect(), 1.0, Blend::Normal);
        raster_text(px, 64, 70, "DECIUS", 120, 255, 255, 255, 0.96, true);
        raster_text(px, 70, 210, "a decius.css showcase", 30, 255, 255, 255,
                    0.7, false);
    }
    // 7. Empty paint layer on top — active at boot.
    make_layer("Paint", "pixel", "source-over", 1.0, false);

    active_ = static_cast<int>(layers_.size()) - 1;
    sel_ = {};
    pen_pts_.clear();
    clone_source_set_ = false;
    mark_all_dirty();
    reset_history("Open", "folder-open");
    sync_thumb_handlers();
    request_repaint();
}

// ── compositing / display ────────────────────────────────────────────────

void PhotoDoc::composite_rect(const RectI& rect) {
    const RectI r = intersect(rect, doc_rect());
    if (r.empty()) return;
    clear_rect(composite_, r);
    for (const auto& l : layers_) {
        if (!l.visible || l.opacity <= 0.0) continue;
        compose_over(composite_, *l.pixels, r, l.opacity * l.fill,
                     parse_blend(l.blend));
    }
    // display = checkerboard underlay + composite (the classic
    // transparency checker; 8px squares in document space)
    for (int y = r.y; y < r.y + r.h; ++y) {
        const std::uint8_t* sp =
            &composite_.px[(static_cast<std::size_t>(y) * w_ + r.x) * 4];
        std::uint8_t* dp =
            &display_.px[(static_cast<std::size_t>(y) * w_ + r.x) * 4];
        for (int x = r.x; x < r.x + r.w; ++x, sp += 4, dp += 4) {
            const int check = (((x >> 3) ^ (y >> 3)) & 1) ? 204 : 255;
            const int a = sp[3];
            dp[0] = static_cast<std::uint8_t>(
                (sp[0] * a + check * (255 - a)) / 255);
            dp[1] = static_cast<std::uint8_t>(
                (sp[1] * a + check * (255 - a)) / 255);
            dp[2] = static_cast<std::uint8_t>(
                (sp[2] * a + check * (255 - a)) / 255);
            dp[3] = 255;
        }
    }
}

void PhotoDoc::flush_composite() {
    if (full_dirty_) {
        composite_rect(doc_rect());
        full_dirty_ = false;
        pending_dirty_ = {};
    } else if (!pending_dirty_.empty()) {
        composite_rect(pending_dirty_);
        pending_dirty_ = {};
    }
}

// ── export ───────────────────────────────────────────────────────────────

bool PhotoDoc::export_png(const std::string& path, double scale,
                          bool opaque_white) const {
    auto* self = const_cast<PhotoDoc*>(this);
    self->flush_composite();
    Buffer out = composite_;
    if (opaque_white) {
        for (std::size_t i = 0; i < out.px.size(); i += 4) {
            const int a = out.px[i + 3];
            for (int c = 0; c < 3; ++c) {
                out.px[i + c] = static_cast<std::uint8_t>(
                    (out.px[i + c] * a + 255 * (255 - a)) / 255);
            }
            out.px[i + 3] = 255;
        }
    }
    if (scale > 0 && std::abs(scale - 1.0) > 1e-6) {
        out = scaled_buffer(out,
                            std::max(1, static_cast<int>(w_ * scale + 0.5)),
                            std::max(1, static_cast<int>(h_ * scale + 0.5)));
    }
    return write_png(path, out);
}

// ── UI integration (custom paint) ────────────────────────────────────────

std::string PhotoDoc::thumb_paint_name(int layer_id) const {
    return "ps-thumb-" + std::to_string(layer_id);
}

void PhotoDoc::attach(Host host) {
    host_ = std::move(host);
    if (!host_) return;
    if (host_.font_data) font_provider() = host_.font_data;
    const auto weak = affineui::to_weak_ref(this);
    host_.set_custom_paint(
        "ps-stage", [weak](affineui::Painter& p, const affineui::Rect& r) {
            if (auto* self = weak.get()) self->paint_stage(p, r);
        });
    host_.set_custom_paint(
        "ps-nav", [weak](affineui::Painter& p, const affineui::Rect& r) {
            if (auto* self = weak.get()) self->paint_nav(p, r);
        });
    sync_thumb_handlers();
}

void PhotoDoc::detach() {
    if (host_) {
        host_.set_custom_paint("ps-stage", nullptr);
        host_.set_custom_paint("ps-nav", nullptr);
        for (int id : thumb_names_)
            host_.set_custom_paint(thumb_paint_name(id), nullptr);
    }
    thumb_names_.clear();
    destroy_image(stage_img_);
    for (auto& entry : thumbs_) destroy_image(entry.second.img);
    thumbs_.clear();
    host_ = {};
}

// Hand an image back to the host and forget it. Safe on 0 / detached.
void PhotoDoc::destroy_image(std::uint32_t& id) {
    if (id != 0 && host_ && host_.destroy_image) host_.destroy_image(id);
    id = 0;
}

void PhotoDoc::sync_thumb_handlers() {
    if (!host_) return;
    std::unordered_set<int> want;
    for (const auto& l : layers_) want.insert(l.id);
    for (auto it = thumb_names_.begin(); it != thumb_names_.end();) {
        if (want.count(*it) == 0) {
            host_.set_custom_paint(thumb_paint_name(*it), nullptr);
            auto tex = thumbs_.find(*it);
            if (tex != thumbs_.end()) {
                destroy_image(tex->second.img);
                thumbs_.erase(tex);
            }
            it = thumb_names_.erase(it);
        } else {
            ++it;
        }
    }
    for (int id : want) {
        if (thumb_names_.count(id)) continue;
        thumb_names_.insert(id);
        const auto weak = affineui::to_weak_ref(this);
        host_.set_custom_paint(
            thumb_paint_name(id),
            [weak, id](affineui::Painter& p, const affineui::Rect& r) {
                if (auto* self = weak.get()) self->paint_thumb(id, p, r);
            });
    }
}

void PhotoDoc::request_repaint() {
    if (!host_) return;
    host_.request_custom_repaint("ps-stage");
    host_.request_custom_repaint("ps-nav");
    for (const auto& l : layers_) {
        const auto it = thumbs_.find(l.id);
        if (it == thumbs_.end() || it->second.rev != l.pixel_rev)
            host_.request_custom_repaint(thumb_paint_name(l.id));
    }
}

void PhotoDoc::paint_stage(affineui::Painter& p, const affineui::Rect& r) {
    stage_rect_ = {r.x, r.y, r.w, r.h};
    if (need_fit_) fit_to_screen();

    flush_composite();
    if (stage_img_ == 0 || stage_img_w_ != w_ || stage_img_h_ != h_) {
        destroy_image(stage_img_);
        if (host_ && host_.create_image_rgba) {
            stage_img_ = host_.create_image_rgba(w_, h_, display_.px);
        }
        stage_img_w_ = w_;
        stage_img_h_ = h_;
        uploaded_rev_ = revision_;
    } else if (uploaded_rev_ != revision_) {
        if (host_.update_image) (void) host_.update_image(stage_img_, display_.px);
        uploaded_rev_ = revision_;
    }
    if (stage_img_ == 0) return;

    double ox = 0, oy = 0;
    doc_origin(ox, oy);
    const double dw = w_ * zoom_, dh = h_ * zoom_;

    // visible = doc rect ∩ stage element; draw only the covered sub-rect
    const double vx0 = std::max(ox, static_cast<double>(r.x));
    const double vy0 = std::max(oy, static_cast<double>(r.y));
    const double vx1 = std::min(ox + dw, static_cast<double>(r.x + r.w));
    const double vy1 = std::min(oy + dh, static_cast<double>(r.y + r.h));
    if (vx1 <= vx0 || vy1 <= vy0) return;

    const affineui::Rect dst{
        static_cast<int>(std::floor(vx0)), static_cast<int>(std::floor(vy0)),
        static_cast<int>(std::ceil(vx1) - std::floor(vx0)),
        static_cast<int>(std::ceil(vy1) - std::floor(vy0))};
    const affineui::Rect src{
        static_cast<int>((dst.x - ox) / zoom_),
        static_cast<int>((dst.y - oy) / zoom_),
        std::max(1, static_cast<int>(std::ceil(dst.w / zoom_))),
        std::max(1, static_cast<int>(std::ceil(dst.h / zoom_)))};

    // subtle document drop shadow / border
    p.fill_rect(affineui::Rect{static_cast<int>(ox) + 4,
                               static_cast<int>(oy) + 4,
                               static_cast<int>(dw), static_cast<int>(dh)},
                affineui::Color{0, 0, 0, 70});
    p.draw_image(stage_img_, dst, src);

    // pen-tool path preview (paint.js drawPen)
    if (!pen_pts_.empty()) {
        auto to_screen = [&](const PenPt& pt) {
            return std::pair<float, float>(
                static_cast<float>(ox + pt.x * zoom_),
                static_cast<float>(oy + pt.y * zoom_));
        };
        for (std::size_t i = 1; i < pen_pts_.size(); ++i) {
            const auto [x0f, y0f] = to_screen(pen_pts_[i - 1]);
            const auto [x1f, y1f] = to_screen(pen_pts_[i]);
            p.stroke_line(x0f, y0f, x1f, y1f,
                          affineui::Color{0x4d, 0x9f, 0xff, 255}, 1.5f);
        }
        for (const auto& pt : pen_pts_) {
            const auto [xf, yf] = to_screen(pt);
            const affineui::Rect anchor{static_cast<int>(xf) - 3,
                                        static_cast<int>(yf) - 3, 6, 6};
            p.fill_rect(anchor, affineui::Color{255, 255, 255, 255});
            p.stroke_rect(anchor, affineui::Color{0x1f, 0x6f, 0xeb, 255},
                          1.0f);
        }
    }
}

void PhotoDoc::paint_nav(affineui::Painter& p, const affineui::Rect& r) {
    if (stage_img_ == 0 || r.w <= 2 || r.h <= 2) return;
    const double s = std::min(r.w / static_cast<double>(w_),
                              r.h / static_cast<double>(h_));
    const int tw = std::max(1, static_cast<int>(w_ * s));
    const int th = std::max(1, static_cast<int>(h_ * s));
    const affineui::Rect dst{r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, tw,
                             th};
    p.draw_image(stage_img_, dst, affineui::Rect{0, 0, w_, h_});
    // viewport rectangle = doc region visible in the stage
    if (stage_rect_.w > 0) {
        double tlx, tly, brx, bry;
        screen_to_doc(stage_rect_.x, stage_rect_.y, tlx, tly);
        screen_to_doc(stage_rect_.x + stage_rect_.w,
                      stage_rect_.y + stage_rect_.h, brx, bry);
        const double x0 = clampd(tlx, 0, w_), y0 = clampd(tly, 0, h_);
        const double x1 = clampd(brx, 0, w_), y1 = clampd(bry, 0, h_);
        const affineui::Rect view{
            dst.x + static_cast<int>(x0 * s),
            dst.y + static_cast<int>(y0 * s),
            std::max(2, static_cast<int>((x1 - x0) * s)),
            std::max(2, static_cast<int>((y1 - y0) * s))};
        p.stroke_rect(view, affineui::Color{255, 255, 255, 230}, 1.0f);
    }
}

void PhotoDoc::paint_thumb(int layer_id, affineui::Painter& p,
                           const affineui::Rect& r) {
    Layer* l = find_layer(layer_id);
    if (l == nullptr) return;
    ThumbTex& tex = thumbs_[layer_id];
    constexpr int kThumb = 34;
    if (tex.img == 0 || tex.rev != l->pixel_rev) {
        // box-downsample the layer into a 34×34 tile, centered like the web
        Buffer tile(kThumb, kThumb);
        const double s =
            std::min(kThumb / static_cast<double>(w_),
                     kThumb / static_cast<double>(h_));
        const int tw = std::max(1, static_cast<int>(w_ * s));
        const int th = std::max(1, static_cast<int>(h_ * s));
        Buffer small = scaled_buffer(*l->pixels, tw, th);
        const int offx = (kThumb - tw) / 2, offy = (kThumb - th) / 2;
        for (int y = 0; y < th; ++y) {
            std::memcpy(
                &tile.px[(static_cast<std::size_t>(y + offy) * kThumb +
                          offx) * 4],
                &small.px[static_cast<std::size_t>(y) * tw * 4],
                static_cast<std::size_t>(tw) * 4);
        }
        if (tex.img == 0) {
            if (host_ && host_.create_image_rgba)
                tex.img = host_.create_image_rgba(kThumb, kThumb, tile.px);
        } else if (host_.update_image) {
            (void) host_.update_image(tex.img, tile.px);
        }
        tex.rev = l->pixel_rev;
    }
    if (tex.img != 0) {
        p.draw_image(tex.img, r, affineui::Rect{0, 0, kThumb, kThumb});
    }
}

}  // namespace photo
