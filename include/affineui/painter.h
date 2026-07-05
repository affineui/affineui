#pragma once

#include "affineui/geom.h"
#include "affineui/types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace affineui {

/// Verb tags for the flat vector-path command stream accepted by
/// Painter::fill_path / Painter::stroke_path. A path is an array of
/// floats where each segment is a verb tag followed by its coordinates:
///   kPathMove  x y                    — begin a new subpath
///   kPathLine  x y
///   kPathCubic c1x c1y c2x c2y x y
///   kPathClose                        — close the current subpath
/// Quadratics, elliptical arcs, and H/V shorthands are lowered to these
/// four verbs by the producer (see the inline-SVG path parser).
inline constexpr float kPathMove  = 0.0f;
inline constexpr float kPathLine  = 1.0f;
inline constexpr float kPathCubic = 2.0f;
inline constexpr float kPathClose = 3.0f;

/// Paint source for vector paths: solid colour or a multi-stop
/// gradient (up to kMaxStops). Two-stop gradients hit the backend's
/// native gradient paint; more stops are rendered through a cached
/// gradient-LUT texture, so arbitrary SVG-style ramps stay exact.
struct PathPaint {
    enum class Kind : std::uint8_t { Solid, Linear, Radial };
    static constexpr int kMaxStops = 8;

    Kind         kind{Kind::Solid};
    std::uint8_t stop_count{1};
    float        offsets[kMaxStops]{};  // ascending, 0–1
    Color        colors[kMaxStops]{};
    // Linear: gradient axis runs (x0,y0) → (x1,y1).
    // Radial: (x0,y0) is the centre; r0 = inner radius, r1 = outer.
    float x0{0.0f}, y0{0.0f};
    float x1{0.0f}, y1{0.0f};
    float r0{0.0f}, r1{0.0f};

    static PathPaint solid(Color c) {
        PathPaint p;
        p.colors[0] = c;
        return p;
    }
    static PathPaint linear(float x0, float y0, float x1, float y1,
                            Color c0, Color c1) {
        PathPaint p;
        p.kind = Kind::Linear;
        p.stop_count = 2;
        p.offsets[0] = 0.0f; p.offsets[1] = 1.0f;
        p.colors[0] = c0;    p.colors[1] = c1;
        p.x0 = x0; p.y0 = y0;
        p.x1 = x1; p.y1 = y1;
        return p;
    }
    static PathPaint radial(float cx, float cy, float inner_r, float outer_r,
                            Color c0, Color c1) {
        PathPaint p;
        p.kind = Kind::Radial;
        p.stop_count = 2;
        p.offsets[0] = 0.0f; p.offsets[1] = 1.0f;
        p.colors[0] = c0;    p.colors[1] = c1;
        p.x0 = cx; p.y0 = cy;
        p.r0 = inner_r; p.r1 = outer_r;
        return p;
    }
    /// Append a gradient stop (offset 0–1, ascending). The first call
    /// on a default-constructed paint replaces the implicit solid stop.
    bool add_stop(float offset, Color c) {
        if (kind == Kind::Solid) return false;
        if (stop_count >= kMaxStops) return false;
        offsets[stop_count] = offset;
        colors[stop_count]  = c;
        ++stop_count;
        return true;
    }
    /// Colour at a normalised position along the ramp.
    Color sample(float t) const {
        if (stop_count == 0) return Color{};
        if (stop_count == 1 || t <= offsets[0]) return colors[0];
        for (int i = 1; i < stop_count; ++i) {
            if (t <= offsets[i]) {
                const float span = offsets[i] - offsets[i - 1];
                const float f = span <= 0.0f
                                    ? 1.0f
                                    : (t - offsets[i - 1]) / span;
                const auto lerp8 = [f](std::uint8_t a, std::uint8_t b) {
                    return static_cast<std::uint8_t>(
                        static_cast<float>(a) +
                        (static_cast<float>(b) - static_cast<float>(a)) * f);
                };
                return Color{lerp8(colors[i - 1].r, colors[i].r),
                             lerp8(colors[i - 1].g, colors[i].g),
                             lerp8(colors[i - 1].b, colors[i].b),
                             lerp8(colors[i - 1].a, colors[i].a)};
            }
        }
        return colors[stop_count - 1];
    }
};

/// Stroke end-cap / join styles (SVG stroke-linecap / stroke-linejoin).
enum class LineCap  : std::uint8_t { Butt, Round, Square };
enum class LineJoin : std::uint8_t { Miter, Round, Bevel };

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

    // ── Vector paths ────────────────────────────────────────────────
    /// Fill / stroke an arbitrary path given as a flat float command
    /// stream (kPathMove / kPathLine / kPathCubic / kPathClose above).
    /// Fills use the nonzero winding rule; a subpath wound opposite to
    /// its enclosing subpath cuts a hole. Coordinates are in the
    /// painter's current (document) space. Backends without vector
    /// path support may no-op.
    virtual void fill_path(const float* cmds, std::size_t count,
                           const PathPaint& paint) {
        (void)cmds; (void)count; (void)paint;
    }
    virtual void stroke_path(const float* cmds, std::size_t count,
                             const PathPaint& paint, float width,
                             LineCap cap = LineCap::Butt,
                             LineJoin join = LineJoin::Miter) {
        (void)cmds; (void)count; (void)paint;
        (void)width; (void)cap; (void)join;
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
