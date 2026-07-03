#pragma once

#include "affineui/geom.h"
#include "affineui/types.h"

#include <string_view>

namespace affineui {

/// Abstract painter interface. The default implementation wraps
/// NanoVG-on-sokol_gfx, but the embedder can supply their own (useful
/// for headless image rendering or piping into another graphics stack).
///
/// Painters are stateful — clip stacks and current transforms persist
/// across calls within a single `Document::draw()` invocation. The
/// frame-level begin/end is owned by the App; the painter sees only
/// the inside of a frame.
class Painter {
public:
    virtual ~Painter() = default;

    // ── Frame lifecycle (called by Document::draw) ──────────────────
    virtual void begin_frame(int width, int height, float dpi_scale) = 0;
    virtual void end_frame()                                          = 0;

    // ── Fills & strokes ─────────────────────────────────────────────
    virtual void fill_rect(const Rect& r, Color color)                = 0;
    virtual void stroke_rect(const Rect& r, Color color, float w)     = 0;
    /// Draw a line segment from (x0, y0) to (x1, y1) with the given
    /// stroke width. Used for per-side border drawing.
    virtual void stroke_line(float x0, float y0, float x1, float y1,
                             Color color, float w)                     = 0;
    /// Draw a filled circle (used for dotted border dots).
    virtual void fill_circle(float cx, float cy, float radius,
                             Color color)                              = 0;
    /// Draw a circular arc centered at (cx, cy) with the given radius.
    /// `angle_start` and `angle_end` are in degrees measured clockwise
    /// from the 12-o'clock (top) position (CSS/SVG convention).
    /// Used for spinner-border UA representation.
    virtual void stroke_arc(float cx, float cy, float radius,
                            float angle_start_deg, float angle_end_deg,
                            Color color, float w)                      = 0;
    virtual void fill_rounded_rect(const Rect& r, float radius, Color color) = 0;
    virtual void stroke_rounded_rect(const Rect& r, float radius, Color color, float w) = 0;
    /// Per-corner radii, top-left / top-right / bottom-right / bottom-left.
    /// Falls back to the uniform variant when all four are equal.
    virtual void fill_rounded_rect_varying(const Rect& r,
                                           float tl, float tr, float br, float bl,
                                           Color color) = 0;
    virtual void stroke_rounded_rect_varying(const Rect& r,
                                             float tl, float tr, float br, float bl,
                                             Color color, float w) = 0;
    /// Fill the CSS border area for a uniform solid border. Unlike a normal
    /// vector stroke, the painted ring lies inside the border box `r`.
    virtual void fill_rounded_rect_ring(const Rect& r,
                                        float radius,
                                        float thickness,
                                        Color color) {
        if (thickness <= 0.0f) return;
        stroke_rounded_rect(r, radius, color, thickness);
    }

    // ── Gradient fills ──────────────────────────────────────────────
    /// CSS-convention angle: 0=upward, 90=right, 180=downward, 270=left.
    /// Supports optional corner radii (pass 0 for sharp corners).
    virtual void fill_linear_gradient_rect(const Rect& r,
                                           float angle_deg,
                                           Color stop0, Color stop1,
                                           float tl = 0, float tr = 0,
                                           float br = 0, float bl = 0) = 0;
    /// Radial gradient centered at a CSS `at <position>` percentage.
    /// Inner radius = 0; outer radius = farthest-corner distance
    /// (Chrome default for `circle`).
    virtual void fill_radial_gradient_rect(const Rect& r,
                                           Color stop0, Color stop1,
                                           float tl = 0, float tr = 0,
                                           float br = 0, float bl = 0,
                                           float center_x_pct = 50,
                                           float center_y_pct = 50,
                                           float stop1_pos_pct = 100) = 0;
    virtual void fill_linear_stripes_rect(const Rect& r,
                                          float angle_deg,
                                          Color stripe,
                                          float tile_size,
                                          float tl = 0, float tr = 0,
                                          float br = 0, float bl = 0) {
        (void)r; (void)angle_deg; (void)stripe; (void)tile_size;
        (void)tl; (void)tr; (void)br; (void)bl;
    }

    // ── Box shadow ──────────────────────────────────────────────────
    /// Draw a CSS box-shadow for the border box `r` (corner radius
    /// `radius`, 0 = sharp). `offset_x/offset_y` shift the shadow,
    /// `blur` is the CSS blur radius (feathered falloff), `spread`
    /// grows (outset) or shrinks (inset) the shadow box. When `inset`
    /// is true the shadow is painted inside the box; otherwise it is an
    /// outset drop shadow behind the box. Backends without a native
    /// feather (e.g. NanoVG's box gradient) approximate the Gaussian.
    virtual void fill_grid_rect(const Rect& r,
                                Color line,
                                float tile_size,
                                float line_width,
                                float tl = 0, float tr = 0,
                                float br = 0, float bl = 0) {
        (void)r; (void)line; (void)tile_size; (void)line_width;
        (void)tl; (void)tr; (void)br; (void)bl;
    }

    virtual void fill_box_shadow(const Rect& r, float radius, Color color,
                                 float offset_x, float offset_y,
                                 float blur, float spread, bool inset) = 0;

    // ── Text ────────────────────────────────────────────────────────
    /// Returns an opaque font handle. Implementation-defined; zero means
    /// "use default fallback face."
    virtual std::uint32_t resolve_font(std::string_view family,
                                       int size_px,
                                       int weight,
                                       bool italic) = 0;
    /// Register an author-provided font face discovered from CSS
    /// `@font-face`. The byte buffer contains either a raw SFNT face
    /// (.ttf/.otf/.ttc) or a backend-supported webfont container. Backends
    /// that cannot consume the payload should return false. Implementations
    /// are expected to make this idempotent for the same family/weight/style
    /// tuple.
    virtual bool register_font_face(std::string_view family,
                                    int weight,
                                    bool italic,
                                    std::string_view bytes) {
        (void)family; (void)weight; (void)italic; (void)bytes;
        return false;
    }
    virtual int  measure_text(std::uint32_t font, std::string_view text) = 0;

    /// Per-font-at-size vertical metrics. Used by layout to size text
    /// runs without guessing — the embedder's actual glyph bbox drives
    /// the content area, which keeps top/bottom padding symmetric.
    ///
    /// `ascender`    : pixels above the baseline (font's tallest glyph extent)
    /// `descender`   : pixels below the baseline, **positive** value
    /// `line_height` : recommended line-to-line distance (CSS "normal")
    ///
    /// `text_height = ascender + descender` is the tight rendered
    /// bounding box for a single line of text in this font.
    struct TextMetrics {
        float ascender{0.0f};
        float descender{0.0f};
        float line_height{0.0f};
    };
    virtual TextMetrics text_metrics(std::uint32_t font) = 0;

    virtual void draw_text(std::uint32_t font,
                           const Point& pos,
                           std::string_view text,
                           Color color) = 0;

    /// Measure the rendered bounding box of `text` when wrapped to a
    /// row width of `max_width` px. Returns the actual rendered width
    /// (≤ max_width) and the total height (potentially many lines).
    /// `line_height_mult` is the inter-line spacing multiplier (1.0 =
    /// font's natural spacing, 1.5 = "line-height: 1.5"). Used by the
    /// layout pass to size text leaves before paint runs.
    /// `letter_spacing_px` adds extra advance between glyphs (CSS
    /// letter-spacing). Default 0.0 = no extra spacing.
    virtual Size measure_text_box(std::uint32_t   font,
                                  std::string_view text,
                                  float           max_width,
                                  float           line_height_mult = 1.0f,
                                  float           letter_spacing_px = 0.0f) = 0;

    /// Horizontal alignment for draw_text_box. Matches CSS text-align
    /// semantics. Justify falls back to Left (NanoVG has no justify
    /// mode; justified rendering requires a custom line-breaker).
    enum class TextAlign : std::uint8_t {
        Left = 0, Center, Right, Justify,
    };

    /// Render `text` wrapped to `max_width`. (x, y) is the top-left of the
    /// wrapped text block in FRACTIONAL pixels — browsers position line
    /// boxes sub-pixel (a flex-centered 16px line in a 23px box sits at
    /// y+3.5) and rasterize from the fractional baseline; rounding the
    /// position first lands glyphs a row off Chrome whenever the fraction
    /// crosses .5. `line_height_mult` must match the value passed to
    /// measure_text_box for the layout's height to agree with what's
    /// actually drawn. `letter_spacing_px` adds extra advance between
    /// glyphs (CSS letter-spacing); must match measure_text_box. `align`
    /// controls horizontal placement of each line within the `max_width`
    /// column (CSS text-align).
    virtual void draw_text_box(std::uint32_t   font,
                               float           x,
                               float           y,
                               std::string_view text,
                               Color           color,
                               float           max_width,
                               float           line_height_mult = 1.0f,
                               float           letter_spacing_px = 0.0f,
                               TextAlign       align = TextAlign::Left) = 0;

    /// Integer-position convenience; forwards to the fractional overload.
    void draw_text_box(std::uint32_t font,
                       const Point&  pos,
                       std::string_view text,
                       Color         color,
                       float         max_width,
                       float         line_height_mult = 1.0f,
                       float         letter_spacing_px = 0.0f,
                       TextAlign     align = TextAlign::Left) {
        draw_text_box(font, static_cast<float>(pos.x),
                      static_cast<float>(pos.y), text, color, max_width,
                      line_height_mult, letter_spacing_px, align);
    }

    // ── Images ──────────────────────────────────────────────────────
    /// Returns an opaque image handle. Zero on miss.
    virtual std::uint32_t load_image(std::string_view url) = 0;
    virtual Size          image_size(std::uint32_t image)  = 0;
    virtual void          draw_image(std::uint32_t image,
                                     const Rect&   dst,
                                     const Rect&   src) = 0;

    // ── Clipping ────────────────────────────────────────────────────
    virtual void push_clip(const Rect& r) = 0;
    virtual void pop_clip()               = 0;

    // ── Opacity group ───────────────────────────────────────────────
    /// Save the current global alpha and multiply it by `alpha` (0–1).
    /// All subsequent drawing calls are composited at the resulting
    /// combined alpha. Must be balanced by pop_alpha().
    virtual void push_alpha(float alpha) = 0;
    virtual void pop_alpha()             = 0;

    // ── Transform stack ─────────────────────────────────────────────
    /// Save the current transform and concatenate `m` for subsequent
    /// drawing. Must be balanced by pop_transform().
    virtual void push_transform(const Mat2x3& m) { (void)m; }
    virtual void pop_transform() {}
};

}  // namespace affineui
