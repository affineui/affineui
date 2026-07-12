#pragma once

#include "affineui/geom.h"
#include "affineui/types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

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
    /// Radial only: the outer radius along Y, when the ending shape is an
    /// ELLIPSE rather than a circle (CSS `radial-gradient(ellipse 110% 90%
    /// …)`). 0 means "circular" — use r1 on both axes, which is what every
    /// pre-existing caller wants and gets by default.
    ///
    /// Only the N-stop (>2) route honours this. That route already renders
    /// through a normalised 2D LUT stretched over a pattern rect, so a
    /// non-square rect gives a true ellipse for free. The 2-stop route maps
    /// to the backend's analytic circular radial paint and stays circular —
    /// no CSS in play needs a 2-stop ellipse, and faking one there would
    /// mean giving up the analytic fast path.
    float r1y{0.0f};

    /// The outer radius along Y, falling back to the circular r1.
    float outer_ry() const { return r1y > 0.0f ? r1y : r1; }

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

// The display-list path blob serializes PathPaint's geometry as one
// contiguous run of floats starting at x0 (see kPathBlobGeoFloats in
// display_list_painter.h). Lock that in: reordering or padding these
// fields would silently corrupt every recorded gradient.
static_assert(offsetof(PathPaint, y0)  == offsetof(PathPaint, x0) + 4);
static_assert(offsetof(PathPaint, x1)  == offsetof(PathPaint, x0) + 8);
static_assert(offsetof(PathPaint, y1)  == offsetof(PathPaint, x0) + 12);
static_assert(offsetof(PathPaint, r0)  == offsetof(PathPaint, x0) + 16);
static_assert(offsetof(PathPaint, r1)  == offsetof(PathPaint, x0) + 20);
static_assert(offsetof(PathPaint, r1y) == offsetof(PathPaint, x0) + 24);

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

    /// One color stop of an N-stop gradient rect fill: `offset` is the
    /// normalised ramp position (0–1, ascending), `color` the stop color.
    struct GradientStop {
        float offset{0.0f};
        Color color{};
    };

    /// N-stop (>2) linear/radial gradient rect fills. These are the CSS
    /// multi-stop path (e.g. a rainbow hue bar). `stops`/`stop_count`
    /// carry the full ordered ramp — up to PathPaint::kMaxStops; extra
    /// stops are dropped. The default implementations lower the fill to a
    /// rounded-rect vector `fill_path` carrying a PathPaint, so every
    /// painter (device + display-list recorder) reuses the existing
    /// N-stop PathPaint machinery — the cached gradient-LUT texture in
    /// the device painter and the path-blob serialization in the
    /// recorder — without any new primitive or texture-lifetime surface.
    /// A backend can override for a more direct route, but is not
    /// required to.
    virtual void fill_linear_gradient_rect_n(const Rect& r,
                                             float angle_deg,
                                             const GradientStop* stops,
                                             std::size_t stop_count,
                                             float tl = 0, float tr = 0,
                                             float br = 0, float bl = 0);
    /// `center_*_pct` may be negative or exceed 100 — CSS routinely puts a
    /// specular highlight's centre outside the box (`at 50% -10%`).
    /// `radius_*_pct` size the ending shape as a percentage of the box's
    /// full width / height (CSS `ellipse 110% 90%`); 0 selects the
    /// CSS default of farthest-corner, scaled by `stop1_pos_pct`.
    virtual void fill_radial_gradient_rect_n(const Rect& r,
                                             const GradientStop* stops,
                                             std::size_t stop_count,
                                             float tl = 0, float tr = 0,
                                             float br = 0, float bl = 0,
                                             float center_x_pct = 50,
                                             float center_y_pct = 50,
                                             float stop1_pos_pct = 100,
                                             float radius_x_pct = 0,
                                             float radius_y_pct = 0);
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

    // ── Native GPU images (renderer-owned textures) ─────────────────
    /// Adopt a GPU texture owned by other rendering code (a 3D engine's
    /// offscreen render target, a video decoder surface, ...) as a
    /// drawable image. `native_handle` is rasterizer-specific — for the
    /// sokol/NanoVG rasterizer it is the sg_image id. The painter does
    /// NOT take ownership of the texture: release_native_image() frees
    /// only the wrapper, and the texture must outlive it. `flip_y`
    /// marks textures whose row 0 is the bottom scanline (GL render
    /// targets). Resource ops, not draw ops: safe to call from a
    /// custom-paint handler. Returns 0 when this painter cannot draw
    /// native textures (headless/stub rasterizers).
    virtual std::uint32_t adopt_native_image(std::uint64_t native_handle,
                                             int w, int h, bool flip_y) {
        (void)native_handle; (void)w; (void)h; (void)flip_y;
        return 0;
    }
    /// Release a wrapper created by adopt_native_image. Handles from
    /// load_image are cache-owned and must NOT be passed here.
    virtual void release_native_image(std::uint32_t image) { (void)image; }

    // ── Dynamic images (app-owned pixel surfaces) ───────────────────
    /// Create an image from raw RGBA8 pixels (tightly packed, stride =
    /// w * 4, non-premultiplied). Returns a handle usable with
    /// draw_image / update_image_rgba / delete_image; zero on failure.
    /// Unlike load_image the pixels come from the app (a raster
    /// document, a video frame, a CPU-composited canvas), so the handle
    /// is NOT cached/deduplicated — the caller owns its lifetime and
    /// must delete_image() it. Resource ops, not draw ops: safe to call
    /// from a custom-paint handler; the display-list recorder forwards
    /// them straight to the device painter.
    virtual std::uint32_t create_image_rgba(int w, int h,
                                            const std::uint8_t* pixels) {
        (void)w; (void)h; (void)pixels;
        return 0;
    }
    /// Replace the full pixel contents of an image created by
    /// create_image_rgba. Dimensions must match the creation size.
    virtual void update_image_rgba(std::uint32_t image,
                                   const std::uint8_t* pixels) {
        (void)image; (void)pixels;
    }
    /// Release an image created by create_image_rgba. Handles from
    /// load_image are cache-owned and must NOT be passed here.
    virtual void delete_image(std::uint32_t image) { (void)image; }

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

// ── N-stop gradient rect defaults ───────────────────────────────────
// These lower an N-stop rect fill to a rounded-rect vector path plus a
// PathPaint carrying the full stop ramp, then dispatch through
// fill_path(). Doing it here means the whole N-stop pipeline —
// the device painter's cached gradient-LUT texture and the display-list
// recorder's path-blob serialization — is reused unchanged, with no new
// paint primitive and no additional texture-lifetime surface.
namespace detail {

// Append a rounded-rect outline (TL→TR→BR→BL, clockwise) as a flat
// kPath* command stream. Corners use the 4-cubic circle approximation.
inline void append_rounded_rect_path(std::vector<float>& cmds,
                                     float x, float y, float w, float h,
                                     float tl, float tr, float br, float bl) {
    const float max_r = std::min(w, h) * 0.5f;
    tl = std::clamp(tl, 0.0f, max_r);
    tr = std::clamp(tr, 0.0f, max_r);
    br = std::clamp(br, 0.0f, max_r);
    bl = std::clamp(bl, 0.0f, max_r);
    constexpr float k = 0.5522847498307936f;  // 4/3*(sqrt(2)-1)
    const float x1 = x + w, y1 = y + h;
    // Start after the top-left corner arc.
    cmds.push_back(kPathMove); cmds.push_back(x + tl); cmds.push_back(y);
    // Top edge → top-right corner.
    cmds.push_back(kPathLine); cmds.push_back(x1 - tr); cmds.push_back(y);
    if (tr > 0.0f) {
        cmds.push_back(kPathCubic);
        cmds.push_back(x1 - tr + tr * k); cmds.push_back(y);
        cmds.push_back(x1);               cmds.push_back(y + tr - tr * k);
        cmds.push_back(x1);               cmds.push_back(y + tr);
    }
    // Right edge → bottom-right corner.
    cmds.push_back(kPathLine); cmds.push_back(x1); cmds.push_back(y1 - br);
    if (br > 0.0f) {
        cmds.push_back(kPathCubic);
        cmds.push_back(x1);               cmds.push_back(y1 - br + br * k);
        cmds.push_back(x1 - br + br * k);  cmds.push_back(y1);
        cmds.push_back(x1 - br);          cmds.push_back(y1);
    }
    // Bottom edge → bottom-left corner.
    cmds.push_back(kPathLine); cmds.push_back(x + bl); cmds.push_back(y1);
    if (bl > 0.0f) {
        cmds.push_back(kPathCubic);
        cmds.push_back(x + bl - bl * k);  cmds.push_back(y1);
        cmds.push_back(x);                cmds.push_back(y1 - bl + bl * k);
        cmds.push_back(x);                cmds.push_back(y1 - bl);
    }
    // Left edge → top-left corner.
    cmds.push_back(kPathLine); cmds.push_back(x); cmds.push_back(y + tl);
    if (tl > 0.0f) {
        cmds.push_back(kPathCubic);
        cmds.push_back(x);                cmds.push_back(y + tl - tl * k);
        cmds.push_back(x + tl - tl * k);  cmds.push_back(y);
        cmds.push_back(x + tl);           cmds.push_back(y);
    }
    cmds.push_back(kPathClose);
}

// Copy up to kMaxStops stops into a PathPaint's ramp arrays.
inline void fill_pathpaint_stops(PathPaint& p,
                                 const Painter::GradientStop* stops,
                                 std::size_t stop_count) {
    const std::size_t n = std::min<std::size_t>(stop_count,
                                                PathPaint::kMaxStops);
    p.stop_count = static_cast<std::uint8_t>(n);
    for (std::size_t i = 0; i < n; ++i) {
        p.offsets[i] = std::clamp(stops[i].offset, 0.0f, 1.0f);
        p.colors[i]  = stops[i].color;
    }
}

}  // namespace detail

inline void Painter::fill_linear_gradient_rect_n(const Rect& r,
                                                 float angle_deg,
                                                 const GradientStop* stops,
                                                 std::size_t stop_count,
                                                 float tl, float tr,
                                                 float br, float bl) {
    if (r.w <= 0 || r.h <= 0 || stops == nullptr || stop_count == 0) return;
    if (stop_count == 1) {
        fill_linear_gradient_rect(r, angle_deg, stops[0].color, stops[0].color,
                                  tl, tr, br, bl);
        return;
    }
    if (stop_count == 2) {
        // Native 2-stop fast path (no LUT).
        fill_linear_gradient_rect(r, angle_deg, stops[0].color, stops[1].color,
                                  tl, tr, br, bl);
        return;
    }

    const float fx = static_cast<float>(r.x);
    const float fy = static_cast<float>(r.y);
    const float fw = static_cast<float>(r.w);
    const float fh = static_cast<float>(r.h);
    const float cx = fx + fw * 0.5f;
    const float cy = fy + fh * 0.5f;
    // CSS angle → gradient axis (matches fill_linear_gradient_rect).
    const float rad = angle_deg * (3.14159265358979323846f / 180.0f);
    const float dx = std::sin(rad);
    const float dy = -std::cos(rad);
    const float ext = std::abs(dx) * (fw * 0.5f) + std::abs(dy) * (fh * 0.5f);

    PathPaint p;
    p.kind = PathPaint::Kind::Linear;
    p.x0 = cx - dx * ext; p.y0 = cy - dy * ext;
    p.x1 = cx + dx * ext; p.y1 = cy + dy * ext;
    detail::fill_pathpaint_stops(p, stops, stop_count);

    std::vector<float> cmds;
    detail::append_rounded_rect_path(cmds, fx, fy, fw, fh, tl, tr, br, bl);
    fill_path(cmds.data(), cmds.size(), p);
}

inline void Painter::fill_radial_gradient_rect_n(const Rect& r,
                                                 const GradientStop* stops,
                                                 std::size_t stop_count,
                                                 float tl, float tr,
                                                 float br, float bl,
                                                 float center_x_pct,
                                                 float center_y_pct,
                                                 float stop1_pos_pct,
                                                 float radius_x_pct,
                                                 float radius_y_pct) {
    if (r.w <= 0 || r.h <= 0 || stops == nullptr || stop_count == 0) return;
    if (stop_count <= 2) {
        // 2 stops map to the backend's analytic circular radial paint, which
        // has no ellipse and clamps the centre into the box. That is fine for
        // every 2-stop radial we render; a sized/offset ellipse only ever
        // shows up with a multi-stop falloff.
        const Color c0 = stops[0].color;
        const Color c1 = stops[stop_count - 1].color;
        fill_radial_gradient_rect(r, c0, c1, tl, tr, br, bl,
                                  center_x_pct, center_y_pct, stop1_pos_pct);
        return;
    }

    const float fx = static_cast<float>(r.x);
    const float fy = static_cast<float>(r.y);
    const float fw = static_cast<float>(r.w);
    const float fh = static_cast<float>(r.h);
    // NOT clamped to the box: `at 50% -10%` centres the ramp above the box
    // so only the tail of the falloff lands on it. Clamping to 0% would drag
    // the hot centre onto the top edge and wash the box out.
    const float cx = fx + fw * (center_x_pct / 100.0f);
    const float cy = fy + fh * (center_y_pct / 100.0f);

    float outer_rx, outer_ry;
    if (radius_x_pct > 0.0f || radius_y_pct > 0.0f) {
        // Explicit ending shape. CSS Images defines each percentage radius
        // against the corresponding FULL dimension of the gradient box.
        // Therefore `ellipse 100% 80%` in a W x H box has radii W and .8H,
        // not W/2 and .4H. Halving these values compressed the 2600 panel's
        // specular falloff into a narrow band around its socket row.
        const float rx = radius_x_pct > 0.0f ? radius_x_pct : radius_y_pct;
        const float ry = radius_y_pct > 0.0f ? radius_y_pct : radius_x_pct;
        outer_rx = fw * (rx / 100.0f);
        outer_ry = fh * (ry / 100.0f);
    } else {
        // CSS default: farthest-corner, a circle through the corner most
        // distant from the centre.
        const float ddx = std::max(cx - fx, fx + fw - cx);
        const float ddy = std::max(cy - fy, fy + fh - cy);
        outer_rx = outer_ry = std::sqrt(ddx * ddx + ddy * ddy);
    }
    const float scale = std::clamp(stop1_pos_pct, 1.0f, 100.0f) / 100.0f;
    outer_rx *= scale;
    outer_ry *= scale;

    PathPaint p;
    p.kind = PathPaint::Kind::Radial;
    p.x0 = cx; p.y0 = cy;
    p.r0 = 0.0f; p.r1 = outer_rx;
    p.r1y = (outer_ry != outer_rx) ? outer_ry : 0.0f;
    detail::fill_pathpaint_stops(p, stops, stop_count);

    std::vector<float> cmds;
    detail::append_rounded_rect_path(cmds, fx, fy, fw, fh, tl, tr, br, bl);
    fill_path(cmds.data(), cmds.size(), p);
}

/// Raw bytes of the library's embedded default UI font (Roboto Regular
/// or Bold), for apps that rasterize text themselves (e.g. a raster
/// canvas document). Empty when the library was built without embedded
/// fonts. The storage is static — the view never dangles.
std::string_view embedded_font_data(bool bold = false);

}  // namespace affineui
