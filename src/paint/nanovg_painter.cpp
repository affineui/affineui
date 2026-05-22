// Concrete Painter that draws via NanoVG.
//
// Lowers each affineui::Painter method onto nvg* calls. NanoVG handles
// the GL3 backend, the glyph atlas (via bundled fontstash), and
// stb_image-backed image loading.

#include "affineui/painter.h"
#include "internal/paint_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if !defined(AFFINEUI_STUB_BUILD)
#    include "nanovg.h"
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
    void fill_rounded_rect(const Rect&, float, Color) override {}
    void stroke_rounded_rect(const Rect&, float, Color, float) override {}
    void fill_rounded_rect_varying(const Rect&, float, float, float, float, Color) override {}
    void stroke_rounded_rect_varying(const Rect&, float, float, float, float, Color, float) override {}
    void fill_linear_gradient_rect(const Rect&, float, Color, Color,
                                   float, float, float, float) override {}
    void fill_radial_gradient_rect(const Rect&, Color, Color,
                                   float, float, float, float) override {}
    std::uint32_t resolve_font(std::string_view, int, int, bool) override { return 0; }
    int           measure_text(std::uint32_t, std::string_view) override { return 0; }
    TextMetrics   text_metrics(std::uint32_t) override { return {}; }
    void draw_text(std::uint32_t, const Point&, std::string_view, Color) override {}
    Size measure_text_box(std::uint32_t, std::string_view, float, float, float) override { return {}; }
    void draw_text_box(std::uint32_t, const Point&, std::string_view, Color, float, float, float) override {}
    std::uint32_t load_image(std::string_view) override { return 0; }
    Size          image_size(std::uint32_t) override { return {}; }
    void draw_image(std::uint32_t, const Rect&, const Rect&) override {}
    void push_clip(const Rect&) override {}
    void pop_clip() override {}
    void push_alpha(float) override {}
    void pop_alpha() override {}
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

    void fill_rounded_rect(const Rect& r, float radius, Color c) override {
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, static_cast<float>(r.x), static_cast<float>(r.y),
                       static_cast<float>(r.w), static_cast<float>(r.h), radius);
        nvgFillColor(vg_, to_nvg(c));
        nvgFill(vg_);
    }

    void stroke_rounded_rect(const Rect& r, float radius, Color c, float w) override {
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
        nvgBeginPath(vg_);
        nvgRoundedRectVarying(vg_,
            static_cast<float>(r.x), static_cast<float>(r.y),
            static_cast<float>(r.w), static_cast<float>(r.h),
            tl, tr, br, bl);
        nvgStrokeColor(vg_, to_nvg(c));
        nvgStrokeWidth(vg_, w);
        nvgStroke(vg_);
    }

    void fill_linear_gradient_rect(const Rect& r, float angle_deg,
                                   Color stop0, Color stop1,
                                   float tl, float tr, float br, float bl) override {
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
                                   float tl, float tr, float br, float bl) override {
        const float fx = static_cast<float>(r.x);
        const float fy = static_cast<float>(r.y);
        const float fw = static_cast<float>(r.w);
        const float fh = static_cast<float>(r.h);
        // Center the gradient on the box center.
        const float cx = fx + fw * 0.5f;
        const float cy = fy + fh * 0.5f;
        // Outer radius: CSS `farthest-corner` default for `circle`.
        // Half-diagonal of the box is the radius to the farthest corner.
        const float outer_r = std::sqrt(fw * fw + fh * fh) * 0.5f;

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

    std::uint32_t resolve_font(std::string_view family,
                               int               size_px,
                               int               weight,
                               bool              italic) override {
        // NanoVG addresses fonts by face id + a per-frame size. We pack
        // (face_id, size) into a handle so draw_text/measure_text can
        // reconstitute it without another lookup. Top bit of `size`
        // = the synthetic-bold flag (see below).
        //
        // Weight selection three-tier:
        //   < 500   →  regular face, no synth
        //   500-599 →  regular face + synthetic bold (double-draw at
        //              +0.5px to thicken strokes — matches what
        //              browsers do when no Medium variant is available)
        //   >= 600  →  the bold face if loaded, regular otherwise
        //
        // Italic falls back through (italic + bold) → italic → bold →
        // regular. Each combo is its own face — if a system lacks an
        // italic variant the cascade silently uses the upright form
        // (NanoVG has no synthetic-oblique path either).
        const std::string family_str(family);
        const bool        prefer_bold = weight >= 600;
        const bool        synth_bold  = (weight >= 500 && weight < 600);

        int face = -1;
        if (italic && prefer_bold) {
            face = nvgFindFont(vg_, (family_str + "-bold-italic").c_str());
            if (face < 0 && bold_italic_face_ >= 0) face = bold_italic_face_;
        }
        if (italic && face < 0) {
            face = nvgFindFont(vg_, (family_str + "-italic").c_str());
            if (face < 0 && italic_face_ >= 0) face = italic_face_;
        }
        if (face < 0 && prefer_bold) {
            face = nvgFindFont(vg_, (family_str + "-bold").c_str());
            if (face < 0 && bold_face_ >= 0) face = bold_face_;
        }
        if (face < 0) face = nvgFindFont(vg_, family_str.c_str());
        if (face < 0) face = default_face_;
        if (face < 0) return 0;
        return pack_handle(static_cast<std::uint16_t>(face),
                           static_cast<std::uint16_t>(size_px),
                           synth_bold);
    }

    int measure_text(std::uint32_t handle, std::string_view text) override {
        if (handle == 0) return 0;
        apply_handle(handle);
        float bounds[4] = {0, 0, 0, 0};
        const float advance = nvgTextBounds(vg_, 0.0f, 0.0f,
                                            text.data(), text.data() + text.size(),
                                            bounds);
        return static_cast<int>(advance + 0.5f);
    }

    TextMetrics text_metrics(std::uint32_t handle) override {
        TextMetrics m{};
        if (handle == 0) return m;
        apply_handle(handle);
        // NanoVG returns descender as a *negative* value (pixels below
        // baseline). We expose it as positive — the spec contract for
        // TextMetrics is "magnitude in pixels."
        float asc = 0, desc = 0, lh = 0;
        nvgTextMetrics(vg_, &asc, &desc, &lh);
        m.ascender    = asc;
        m.descender   = desc < 0 ? -desc : desc;
        m.line_height = lh;
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
        if (handle_is_synth_bold(handle)) {
            // Synthetic medium: a second pass offset by half a pixel
            // thickens strokes without doubling them. Cheap and gives
            // a visible "between regular and bold" weight.
            nvgText(vg_, fx + 0.5f, fy, text.data(), text.data() + text.size());
        }
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

        // Pre-formatted text (max_width >= 60000 + has '\n'): measure each
        // line individually so that leading spaces in indented lines are
        // counted rather than skipped by nvgTextBoxBounds's word-wrap logic.
        // NOTE: 60000 is chosen so that the sentinel 65535 stored in the
        // display-list's uint16 max_width field also triggers this path.
        const bool has_newline = text.find('\n') != std::string_view::npos;
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
            const float leading = line_box_leading(css_line_h, natural_line_h);
            const float total_h = css_line_h * static_cast<float>(line_count) + leading;
            nvgTextLetterSpacing(vg_, 0.0f);
            return Size{
                static_cast<int>(std::ceil(max_line_w)),
                static_cast<int>(std::ceil(total_h)),
            };
        }

        nvgTextLineHeight(vg_, nvg_line_height_mult(css_line_h, natural_line_h));
        // nvgTextBoxBounds returns [xmin, ymin, xmax, ymax] for the
        // rendered text wrapped at `breakRowWidth`. The bounds are in
        // the same local coords as the (x,y) we passed (we pass 0,0).
        //
        // CEIL (not round-to-nearest) on the way out. Yoga sizes the
        // box to whatever we report. At paint time we pass that same
        // width as `breakRowWidth` to nvgTextBox; if measure rounded
        // DOWN by .5, the paint-side wrap would see "actual > width
        // by epsilon" and break the word onto a new line — invisible
        // wrap on what was supposed to be a single-line box.
        float bounds[4] = {0, 0, 0, 0};
        nvgTextBoxBounds(vg_, 0.0f, 0.0f, max_width,
                         text.data(), text.data() + text.size(), bounds);
        const bool whitespace_only = text_is_whitespace_only(text);
        int width = static_cast<int>(std::ceil(bounds[2] - bounds[0]));
        if (width == 0 && whitespace_only) {
            float line_bounds[4] = {0, 0, 0, 0};
            const float advance = nvgTextBounds(
                vg_, 0.0f, 0.0f, text.data(), text.data() + text.size(),
                line_bounds);
            width = static_cast<int>(std::ceil(advance));
        }
        float measured_h = bounds[3] - bounds[1];
        if (measured_h <= 0.0f && whitespace_only) measured_h = natural_line_h;
        const float leading = line_box_leading(css_line_h, natural_line_h);
        int height = static_cast<int>(std::ceil(measured_h + leading));
        if (height < static_cast<int>(std::ceil(css_line_h))) {
            height = static_cast<int>(std::ceil(css_line_h));
        }
        // Reset letter spacing to default so subsequent draws are unaffected.
        nvgTextLetterSpacing(vg_, 0.0f);
        return Size{
            width,
            height,
        };
    }

    void draw_text_box(std::uint32_t handle,
                       const Point&  pos,
                       std::string_view text,
                       Color         color,
                       float         max_width,
                       float         line_height_mult,
                       float         letter_spacing_px) override {
        if (handle == 0 || text.empty()) return;
        apply_handle(handle);
        nvgTextLetterSpacing(vg_, letter_spacing_px);
        const float natural_line_h = natural_line_height_px(handle);
        const float css_line_h =
            css_line_height_px(handle, line_height_mult, natural_line_h);
        nvgFillColor(vg_, to_nvg(color));
        const float fx = static_cast<float>(pos.x);
        const float fy = static_cast<float>(pos.y)
                       + line_box_leading(css_line_h, natural_line_h) * 0.5f;

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
                if (handle_is_synth_bold(handle))
                    nvgText(vg_, fx + 0.5f, cy, p, line_end);
                cy += lh;
                if (!nl) break;
                p = nl + 1;
            }
        } else {
            nvgTextLineHeight(vg_, nvg_line_height_mult(css_line_h, natural_line_h));
            nvgTextBox(vg_, fx, fy, max_width,
                       text.data(), text.data() + text.size());
            if (handle_is_synth_bold(handle)) {
                nvgTextBox(vg_, fx + 0.5f, fy, max_width,
                           text.data(), text.data() + text.size());
            }
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

    void draw_image(std::uint32_t image, const Rect& dst, const Rect& /*src*/) override {
        if (image == 0) return;
        NVGpaint p = nvgImagePattern(vg_,
                                     static_cast<float>(dst.x), static_cast<float>(dst.y),
                                     static_cast<float>(dst.w), static_cast<float>(dst.h),
                                     0.0f, static_cast<int>(image), 1.0f);
        nvgBeginPath(vg_);
        nvgRect(vg_, static_cast<float>(dst.x), static_cast<float>(dst.y),
                static_cast<float>(dst.w), static_cast<float>(dst.h));
        nvgFillPaint(vg_, p);
        nvgFill(vg_);
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

    void set_default_face    (int face) { default_face_     = face; }
    void set_bold_face       (int face) { bold_face_        = face; }
    void set_italic_face     (int face) { italic_face_      = face; }
    void set_bold_italic_face(int face) { bold_italic_face_ = face; }

private:
    // Handle bit layout (32 bits):
    //   [31..16]  face id (16 bits)
    //   [15]      synth-bold flag (1 bit)
    //   [14..0]   size px (15 bits — max 32767, plenty for any UI)
    static constexpr std::uint32_t pack_handle(std::uint16_t face,
                                               std::uint16_t size,
                                               bool synth_bold = false) {
        const std::uint32_t s = (static_cast<std::uint32_t>(size) & 0x7FFFu)
                              | (synth_bold ? 0x8000u : 0u);
        return (static_cast<std::uint32_t>(face) << 16) | s;
    }
    static constexpr bool handle_is_synth_bold(std::uint32_t handle) {
        return (handle & 0x8000u) != 0u;
    }
    static constexpr int handle_size_px(std::uint32_t handle) {
        return static_cast<int>(handle & 0x7FFFu);
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
    float natural_line_height_px(std::uint32_t handle) {
        float line_h = 0.0f;
        nvgTextMetrics(vg_, nullptr, nullptr, &line_h);
        if (line_h > 0.0f) return line_h;
        return static_cast<float>(std::max(handle_size_px(handle), 1));
    }
    static float css_line_height_px(std::uint32_t handle,
                                    float line_height_mult,
                                    float natural_line_h) {
        const float requested =
            static_cast<float>(handle_size_px(handle)) * line_height_mult;
        return std::max({requested, natural_line_h, 1.0f});
    }
    static float nvg_line_height_mult(float css_line_h, float natural_line_h) {
        return natural_line_h > 0.0f ? css_line_h / natural_line_h : 1.0f;
    }
    static float line_box_leading(float css_line_h, float natural_line_h) {
        return std::max(0.0f, css_line_h - natural_line_h);
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

    NVGcontext*                                  vg_{nullptr};
    int                                          width_{0};
    int                                          height_{0};
    int                                          default_face_{-1};
    int                                          bold_face_{-1};
    int                                          italic_face_{-1};
    int                                          bold_italic_face_{-1};
    std::unordered_map<std::string, int>         image_cache_;
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
        if (nvgCreateFontMem(vg, "sans-bold", affineui_font_roboto_bold,
                             static_cast<int>(affineui_font_roboto_bold_len), 0) >= 0) {
            nvgCreateFontMem(vg, "sans-serif-bold", affineui_font_roboto_bold,
                             static_cast<int>(affineui_font_roboto_bold_len), 0);
        }
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
    return registered_regular;
}

}  // namespace detail

#endif  // AFFINEUI_STUB_BUILD

}  // namespace affineui
