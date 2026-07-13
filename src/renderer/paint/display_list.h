#pragma once

#include "affineui/image.h"
#include "affineui/types.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace affineui::detail {

/// Paint primitive variants. Designed to be:
///   - POD (no destructors, no heap fields) so a DisplayList is a
///     plain block of memory that hashes byte-by-byte.
///   - Small (~32 bytes each) so a moderate UI's worth of ops fits
///     in one or two cache pages.
///   - Stable across frames — same content produces byte-identical
///     ops, which is what makes content-hashing a cheap idle skip.
///
/// Text payloads are stored separately in DisplayList::text_pool;
/// DrawText carries an offset + length into that pool.
enum class PaintOpKind : std::uint8_t {
    FillRect,
    StrokeRect,
    StrokeLine,
    FillCircle,
    StrokeArc,
    FillRoundedRect,
    StrokeRoundedRect,
    FillRoundedRectVarying,
    StrokeRoundedRectVarying,
    FillLinearGradientRect,
    FillRadialGradientRect,
    FillLinearStripesRect,
    FillGridRect,
    FillBoxShadow,
    DrawText,
    DrawTextBox,
    DrawImage,
    DrawNativeImage,
    PushClip,
    PopClip,
    PushAlpha,
    PopAlpha,
    PushTransform,
    PopTransform,
    FillPath,
    StrokePath,
};

struct PaintOp {
    PaintOpKind kind;
    std::uint8_t  pad0{0};
    std::uint16_t pad1{0};

    // 24 bytes of payload; each op uses the variant that matches its
    // kind. We never call placement new on these — they're all
    // trivially copyable.
    union Payload {
        struct {
            std::int16_t  x, y, w, h;
            std::uint32_t rgba;
            std::uint32_t reserved;
        } fill_rect;

        struct {
            std::int16_t  x, y, w, h;
            std::uint32_t rgba;
            float         thickness;
        } stroke_rect;

        // Line segment from (x0,y0) to (x1,y1) with given width.
        struct {
            float         x0, y0, x1, y1;   // 16
            std::uint32_t rgba;              // 4
            float         thickness;         // 4
        } stroke_line;

        // Filled circle centred at (cx,cy).
        struct {
            float         cx, cy, radius;   // 12
            std::uint32_t rgba;             // 4
            std::uint32_t pad0_;            // 4
            std::uint32_t pad1_;            // 4
        } fill_circle;

        // Circular arc centred at (cx,cy), CSS-clock angles in degrees.
        struct {
            float         cx, cy, radius;   // 12
            std::uint32_t rgba;             // 4
            std::int16_t  angle_start;      // 2  (degrees, CSS clock: 0=top CW)
            std::int16_t  angle_end;        // 2
            float         thickness;        // 4
        } stroke_arc;                       // = 24 bytes

        struct {
            std::int16_t  x, y, w, h;
            std::uint32_t rgba;
            float         radius;
        } fill_rounded;

        struct {
            std::int16_t  x, y, w, h;
            std::uint32_t rgba;
            float         radius;
            float         thickness;
        } stroke_rounded;

        // Per-corner radii. The radius fields are u16 so they cap
        // at 65535 px (way beyond any realistic UI). Stroke variant
        // also stows the line thickness in the trailing 4 bytes.
        struct {
            std::int16_t  x, y, w, h;          // 8
            std::uint32_t rgba;                 // 4
            std::uint16_t tl, tr, br, bl;       // 8
            std::uint32_t reserved;             // 4
        } fill_rounded_varying;
        struct {
            std::int16_t  x, y, w, h;          // 8
            std::uint32_t rgba;                 // 4
            std::uint16_t tl, tr, br, bl;       // 8
            float         thickness;            // 4
        } stroke_rounded_varying;

        struct {
            std::uint32_t font_handle;
            std::int16_t  x, y;
            std::uint32_t rgba;
            std::uint32_t text_offset;
            std::uint16_t text_len;
            std::uint16_t measured_w;
            std::uint16_t measured_h;
            std::uint16_t pad;
        } draw_text;

        // Same shape as draw_text plus a wrap-width. NanoVG's
        // nvgTextBox replays this at rasterize time.
        // line_height_x100 stores the line-height multiplier × 100
        // (so 1.5 lands as 150; 0 means "use 100"/1.0).
        // letter_spacing_x100 stores CSS letter-spacing × 100 in px
        // (so 3.0px lands as 300; 0 = normal/no extra spacing).
        // align: 0=Left, 1=Center, 2=Right, 3=Justify (matches
        // Painter::TextAlign ordinals).
        struct {
            std::uint32_t font_handle;
            std::int16_t  x, y;         // floor(position)
            std::uint32_t rgba;
            std::uint32_t text_offset;
            std::uint16_t text_len;
            std::uint16_t max_width;
            std::uint16_t line_height_x100;
            std::int16_t  letter_spacing_x100;
            std::uint8_t  align;
            // Sub-pixel position fractions, 4 bits each (1/16 px), packed
            // x in the high nibble / y in the low. Browsers place line
            // boxes fractionally (flex-centered text at y+3.5); replay
            // reconstructs x + frac/16 so glyphs land on Chrome's rows.
            std::uint8_t  subpx_frac;
            std::uint16_t measured_h;
        } draw_text_box;

        struct {
            std::uint32_t image_handle;
            std::int16_t  x, y, w, h;
            std::int16_t  sx, sy, sw, sh;
        } draw_image;

        // A renderer-owned GPU texture. Unlike DrawImage this stores the
        // backend-native texture identity directly; replay creates a
        // frame-scoped rasterizer wrapper, so app code never retains a
        // Painter or painter-owned image handle.
        struct {
            std::uint32_t native_handle_lo;    // 4
            std::uint32_t native_handle_hi;    // 4
            std::int16_t  x, y, w, h;          // 8
            std::uint16_t native_w, native_h;  // 4
            std::uint8_t  flip_y;              // 1
            std::uint8_t  pad_[3];             // 3
        } draw_native_image;                   // = 24 bytes

        // Gradient fills: angle_deg is CSS-convention (0=up, 90=right).
        // Corner radii are capped to u8 (0–255 px), sufficient for CSS
        // border-radius values in real UIs.
        struct {
            std::int16_t  x, y, w, h;       // 8
            std::int16_t  angle_deg;         // 2  (CSS angle, i16 fits 0–359)
            std::uint8_t  tl, tr, br, bl;   // 4  (corner radii, u8 px)
            std::uint16_t pad_;             // 2
            std::uint32_t stop0_rgba;        // 4
            std::uint32_t stop1_rgba;        // 4
        } fill_linear_gradient;              // = 24 bytes

        struct {
            std::int16_t  x, y, w, h;       // 8
            std::uint8_t  tl, tr, br, bl;   // 4
            std::uint8_t  center_x_pct;      // 1
            std::uint8_t  center_y_pct;      // 1
            std::uint8_t  stop1_pos_pct;     // 1
            std::uint8_t  pad_;              // 1
            std::uint32_t stop0_rgba;        // 4
            std::uint32_t stop1_rgba;        // 4
        } fill_radial_gradient;              // = 24 bytes

        struct {
            std::int16_t  x, y, w, h;       // 8
            std::int16_t  angle_deg;         // 2
            std::uint16_t tile_size;         // 2
            std::uint8_t  tl, tr, br, bl;   // 4
            std::uint32_t stripe_rgba;       // 4
            std::uint32_t pad_;              // 4
        } fill_linear_stripes;               // = 24 bytes

        struct {
            std::int16_t  x, y, w, h;       // 8
            std::uint16_t tile_size;         // 2
            std::uint16_t line_width;        // 2
            std::uint8_t  tl, tr, br, bl;   // 4
            std::uint32_t line_rgba;         // 4
            std::uint32_t pad_;              // 4
        } fill_grid;                         // = 24 bytes

        // CSS box-shadow. Border box (x,y,w,h) + colour + offset + blur
        // + spread + a single corner radius; `inset` selects the inner
        // shadow paint. radius/blur/spread fit comfortably in u16/i16.
        struct {
            std::int16_t  x, y, w, h;       // 8
            std::uint32_t rgba;             // 4
            std::int16_t  offset_x, offset_y;  // 4
            std::int16_t  blur, spread;     // 4
            std::uint16_t radius;           // 2
            std::uint8_t  inset;            // 1
            std::uint8_t  pad_;             // 1
        } fill_box_shadow;                   // = 24 bytes

        struct {
            std::int16_t x, y, w, h;
            std::uint32_t pad0_;
            std::uint32_t pad1_;
        } clip;

        // PushAlpha: `alpha` is the value passed by the caller (0–1).
        // PopAlpha carries no payload — the painter restores its stack.
        struct {
            float        alpha;
            std::uint32_t pad0_;
            std::uint32_t pad1_;
            std::uint32_t pad2_;
            std::uint32_t pad3_;
            std::uint32_t pad4_;
        } push_alpha;

        struct {
            float a, b, c, d, tx, ty;
        } push_transform;

        // Vector path (FillPath / StrokePath). The command stream and
        // paint parameters live in text_pool as a blob (see
        // path_blob_* helpers in display_list_painter.h); the op
        // carries the pool ref, precomputed stroke-inflated visual
        // bounds, and the stroke parameters.
        struct {
            std::uint32_t data_offset;    // 4 — blob offset in text_pool
            std::uint32_t data_len;       // 4 — blob length in bytes
            std::int16_t  bx, by, bw, bh; // 8 — visual bounds
            float         thickness;      // 4 — stroke width (0 for fill)
            std::uint8_t  cap;            // 1 — LineCap ordinal
            std::uint8_t  join;           // 1 — LineJoin ordinal
            std::uint16_t pad_;           // 2
        } path;                           // = 24 bytes

        std::uint8_t raw[28];
    } p{};
};

// draw_text_box is the largest member (28 B): it carries font + position +
// color + text ref + wrap width + line-height + letter-spacing + align.
static_assert(sizeof(PaintOp) == 32, "PaintOp must stay compact");
static_assert(std::is_trivially_copyable_v<PaintOp>,
              "PaintOp must be trivially copyable for byte-hashing");

struct DisplayListTransformRange {
    std::uint32_t pop_index{std::numeric_limits<std::uint32_t>::max()};
    Rect          bounds{};
    std::uint8_t  bounds_known{0};
    std::uint8_t  pad0{0};
    std::uint16_t pad1{0};
};

struct DisplayListClipRange {
    std::uint32_t pop_index{std::numeric_limits<std::uint32_t>::max()};
};

/// A frame's worth of paint commands.
///
/// Two consumers:
///   1. **Hash** the buffer to detect "nothing changed" — that drives
///      the idle-frame skip in the rasterize stage.
///   2. **Replay** through a real Painter to render into an FBO when
///      the hash *has* changed.
///
/// Both consumers are cheap. The expensive work (cascade, layout,
/// glyph rasterization) lives upstream of the DisplayList and is
/// gated by the dirty bits documented in DESIGN.md.
struct DisplayList {
    std::vector<PaintOp>   ops;
    std::vector<char>      text_pool;  // contiguous text storage
    // Dynamic-image draw ops store compact backend ids in PaintOp, while this
    // side table retains the corresponding shared leases through replay.
    // Without it a temporary ImageHandle could release its GPU image after
    // recording and leave a stale/recycled backend id in the cached list.
    std::vector<ImageHandle> managed_images;
    // Optional measured visual bounds for ops whose compact payload cannot
    // carry enough geometry for precise retained-surface invalidation
    // (notably wrapped/aligned text boxes).
    std::vector<Rect>      op_bounds_override;
    std::vector<DisplayListTransformRange> transform_ranges;
    std::vector<DisplayListClipRange>      clip_ranges;
    std::uint64_t          content_hash{0};

    void clear() {
        ops.clear();
        text_pool.clear();
        managed_images.clear();
        op_bounds_override.clear();
        transform_ranges.clear();
        clip_ranges.clear();
        content_hash = 0;
    }

    // Push a text payload into the pool; returns (offset, length).
    std::pair<std::uint32_t, std::uint16_t> intern_text(std::string_view s) {
        const auto off = static_cast<std::uint32_t>(text_pool.size());
        text_pool.insert(text_pool.end(), s.begin(), s.end());
        return {off, static_cast<std::uint16_t>(s.size())};
    }

    std::string_view text_at(std::uint32_t offset, std::uint16_t len) const {
        if (offset + len > text_pool.size()) return {};
        return std::string_view(text_pool.data() + offset, len);
    }

    // Push an arbitrary byte blob into the pool (vector path data);
    // returns (offset, length). Same storage as text so the content
    // hash covers it for free.
    std::pair<std::uint32_t, std::uint32_t> intern_bytes(const void* data,
                                                         std::size_t n) {
        const auto off = static_cast<std::uint32_t>(text_pool.size());
        const auto* p = static_cast<const char*>(data);
        text_pool.insert(text_pool.end(), p, p + n);
        return {off, static_cast<std::uint32_t>(n)};
    }

    std::string_view bytes_at(std::uint32_t offset, std::uint32_t len) const {
        if (offset + len > text_pool.size()) return {};
        return std::string_view(text_pool.data() + offset, len);
    }

    void set_last_op_bounds_override(const Rect& bounds) {
        op_bounds_override.resize(ops.size());
        op_bounds_override.back() = bounds;
    }

    // FNV-1a over the byte representation of ops + text_pool. Same
    // content → same hash; different content → astronomically unlikely
    // collision for our scale. Run once after the builder finalizes,
    // stored in `content_hash`.
    void finalize_hash() {
        constexpr std::uint64_t kOffset = 0xcbf29ce484222325ull;
        constexpr std::uint64_t kPrime  = 0x100000001b3ull;
        std::uint64_t h = kOffset;
        const auto mix = [&](const void* data, std::size_t n) {
            const auto* p = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < n; ++i) {
                h ^= p[i];
                h *= kPrime;
            }
        };
        if (!ops.empty())       mix(ops.data(),       ops.size() * sizeof(PaintOp));
        if (!text_pool.empty()) mix(text_pool.data(), text_pool.size());
        content_hash = h;
    }
};

}  // namespace affineui::detail
