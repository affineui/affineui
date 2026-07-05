#pragma once

#include "affineui/painter.h"
#include "internal/display_list.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace affineui::detail {

inline void prepare_replay_metadata(DisplayList& list);

// ── Vector path blob ────────────────────────────────────────────────
// FillPath / StrokePath store their command stream + paint in the
// text_pool as a blob:
//   u32 paint kind, f32 x0 y0 x1 y1 r0 r1, u32 stop_count,
//   f32 offset × n, u32 rgba × n, then the raw float command stream.
// Blobs are 4-byte aligned in the pool so the float stream can be
// read in place.
inline bool path_blob_decode(std::string_view blob, PathPaint& paint,
                             const float*& cmds, std::size_t& count) {
    constexpr std::size_t kFixedWords = 8;  // kind + 6 geo floats + count
    if (blob.size() < kFixedWords * 4) return false;
    std::uint32_t fixed[kFixedWords];
    std::memcpy(fixed, blob.data(), sizeof(fixed));
    paint = PathPaint{};
    paint.kind = static_cast<PathPaint::Kind>(fixed[0]);
    std::memcpy(&paint.x0, &fixed[1], 6 * sizeof(float));
    const std::uint32_t n =
        std::min<std::uint32_t>(fixed[7], PathPaint::kMaxStops);
    paint.stop_count = static_cast<std::uint8_t>(n);
    const std::size_t header_bytes = (kFixedWords + 2u * n) * 4;
    if (blob.size() < header_bytes) return false;
    const auto unpack = [](std::uint32_t v) {
        return Color{
            static_cast<std::uint8_t>((v >> 24) & 0xFF),
            static_cast<std::uint8_t>((v >> 16) & 0xFF),
            static_cast<std::uint8_t>((v >>  8) & 0xFF),
            static_cast<std::uint8_t>( v        & 0xFF),
        };
    };
    std::memcpy(paint.offsets, blob.data() + kFixedWords * 4,
                n * sizeof(float));
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t rgba = 0;
        std::memcpy(&rgba, blob.data() + (kFixedWords + n + i) * 4, 4);
        paint.colors[i] = unpack(rgba);
    }
    cmds  = reinterpret_cast<const float*>(blob.data() + header_bytes);
    count = (blob.size() - header_bytes) / sizeof(float);
    return true;
}

/// A Painter that records all calls into a DisplayList instead of
/// emitting GL draw commands. The Document calls this just like any
/// other Painter — it has no idea it's being captured.
///
/// Frame lifecycle:
///   begin_frame() — clears the previous DL.
///   <paint calls>  — append PaintOps.
///   end_frame()    — finalizes the content hash.
///
/// After end_frame() the DL is read-only until the next begin_frame.
/// `take()` moves it out; usually we keep a stable instance and let
/// the next begin_frame() reset it in place (cheaper, reuses vector
/// capacity).
///
/// What this painter does NOT do:
///   - Anti-aliasing (text/path rendering still happens at replay
///     time through NanoVG).
///   - Font loading. `resolve_font` returns a stable opaque handle
///     that the replay-side painter resolves to a real face. We
///     forward to the wrapped font-resolver painter so resolve_font
///     / measure_text / image_size hit the actual NanoVG context.
class DisplayListBuilder final : public Painter {
public:
    /// `font_resolver` is a real Painter whose `resolve_font`,
    /// `measure_text`, `load_image`, and `image_size` are forwarded.
    /// Builder doesn't itself touch GL — but `Document::draw` legitimately
    /// asks for font handles and text widths, so we still need those
    /// to resolve through a live NanoVG context.
    explicit DisplayListBuilder(Painter* font_resolver)
        : font_resolver_(font_resolver) {}

    DisplayList& list()             { return list_; }
    const DisplayList& list() const { return list_; }

    // ── Frame lifecycle ────────────────────────────────────────────
    void begin_frame(int /*w*/, int /*h*/, float /*dpi*/) override {
        list_.clear();
    }
    void end_frame() override {
        list_.finalize_hash();
        prepare_replay_metadata(list_);
    }

    // ── Recording calls ────────────────────────────────────────────
    void fill_rect(const Rect& r, Color c) override {
        PaintOp op{};
        op.kind = PaintOpKind::FillRect;
        op.p.fill_rect.x = static_cast<std::int16_t>(r.x);
        op.p.fill_rect.y = static_cast<std::int16_t>(r.y);
        op.p.fill_rect.w = static_cast<std::int16_t>(r.w);
        op.p.fill_rect.h = static_cast<std::int16_t>(r.h);
        op.p.fill_rect.rgba = pack(c);
        list_.ops.push_back(op);
    }

    void stroke_rect(const Rect& r, Color c, float thickness) override {
        PaintOp op{};
        op.kind = PaintOpKind::StrokeRect;
        op.p.stroke_rect.x = static_cast<std::int16_t>(r.x);
        op.p.stroke_rect.y = static_cast<std::int16_t>(r.y);
        op.p.stroke_rect.w = static_cast<std::int16_t>(r.w);
        op.p.stroke_rect.h = static_cast<std::int16_t>(r.h);
        op.p.stroke_rect.rgba = pack(c);
        op.p.stroke_rect.thickness = thickness;
        list_.ops.push_back(op);
    }

    void stroke_line(float x0, float y0, float x1, float y1,
                     Color c, float w) override {
        PaintOp op{};
        op.kind = PaintOpKind::StrokeLine;
        op.p.stroke_line.x0 = x0;
        op.p.stroke_line.y0 = y0;
        op.p.stroke_line.x1 = x1;
        op.p.stroke_line.y1 = y1;
        op.p.stroke_line.rgba = pack(c);
        op.p.stroke_line.thickness = w;
        list_.ops.push_back(op);
    }

    void fill_circle(float cx, float cy, float radius, Color c) override {
        PaintOp op{};
        op.kind = PaintOpKind::FillCircle;
        op.p.fill_circle.cx     = cx;
        op.p.fill_circle.cy     = cy;
        op.p.fill_circle.radius = radius;
        op.p.fill_circle.rgba   = pack(c);
        op.p.fill_circle.pad0_  = 0;
        op.p.fill_circle.pad1_  = 0;
        list_.ops.push_back(op);
    }

    void stroke_arc(float cx, float cy, float radius,
                    float angle_start_deg, float angle_end_deg,
                    Color c, float w) override {
        PaintOp op{};
        op.kind = PaintOpKind::StrokeArc;
        op.p.stroke_arc.cx          = cx;
        op.p.stroke_arc.cy          = cy;
        op.p.stroke_arc.radius      = radius;
        op.p.stroke_arc.rgba        = pack(c);
        op.p.stroke_arc.angle_start = static_cast<std::int16_t>(angle_start_deg);
        op.p.stroke_arc.angle_end   = static_cast<std::int16_t>(angle_end_deg);
        op.p.stroke_arc.thickness   = w;
        list_.ops.push_back(op);
    }

    void fill_rounded_rect(const Rect& r, float radius, Color c) override {
        PaintOp op{};
        op.kind = PaintOpKind::FillRoundedRect;
        op.p.fill_rounded.x = static_cast<std::int16_t>(r.x);
        op.p.fill_rounded.y = static_cast<std::int16_t>(r.y);
        op.p.fill_rounded.w = static_cast<std::int16_t>(r.w);
        op.p.fill_rounded.h = static_cast<std::int16_t>(r.h);
        op.p.fill_rounded.rgba = pack(c);
        op.p.fill_rounded.radius = radius;
        list_.ops.push_back(op);
    }

    void stroke_rounded_rect(const Rect& r, float radius, Color c, float thickness) override {
        PaintOp op{};
        op.kind = PaintOpKind::StrokeRoundedRect;
        op.p.stroke_rounded.x = static_cast<std::int16_t>(r.x);
        op.p.stroke_rounded.y = static_cast<std::int16_t>(r.y);
        op.p.stroke_rounded.w = static_cast<std::int16_t>(r.w);
        op.p.stroke_rounded.h = static_cast<std::int16_t>(r.h);
        op.p.stroke_rounded.rgba = pack(c);
        op.p.stroke_rounded.radius = radius;
        op.p.stroke_rounded.thickness = thickness;
        list_.ops.push_back(op);
    }

    void fill_rounded_rect_varying(const Rect& r,
                                   float tl, float tr, float br, float bl,
                                   Color c) override {
        PaintOp op{};
        op.kind = PaintOpKind::FillRoundedRectVarying;
        op.p.fill_rounded_varying.x = static_cast<std::int16_t>(r.x);
        op.p.fill_rounded_varying.y = static_cast<std::int16_t>(r.y);
        op.p.fill_rounded_varying.w = static_cast<std::int16_t>(r.w);
        op.p.fill_rounded_varying.h = static_cast<std::int16_t>(r.h);
        op.p.fill_rounded_varying.rgba = pack(c);
        op.p.fill_rounded_varying.tl = static_cast<std::uint16_t>(tl);
        op.p.fill_rounded_varying.tr = static_cast<std::uint16_t>(tr);
        op.p.fill_rounded_varying.br = static_cast<std::uint16_t>(br);
        op.p.fill_rounded_varying.bl = static_cast<std::uint16_t>(bl);
        op.p.fill_rounded_varying.reserved = 0;
        list_.ops.push_back(op);
    }

    void stroke_rounded_rect_varying(const Rect& r,
                                     float tl, float tr, float br, float bl,
                                     Color c, float thickness) override {
        PaintOp op{};
        op.kind = PaintOpKind::StrokeRoundedRectVarying;
        op.p.stroke_rounded_varying.x = static_cast<std::int16_t>(r.x);
        op.p.stroke_rounded_varying.y = static_cast<std::int16_t>(r.y);
        op.p.stroke_rounded_varying.w = static_cast<std::int16_t>(r.w);
        op.p.stroke_rounded_varying.h = static_cast<std::int16_t>(r.h);
        op.p.stroke_rounded_varying.rgba = pack(c);
        op.p.stroke_rounded_varying.tl = static_cast<std::uint16_t>(tl);
        op.p.stroke_rounded_varying.tr = static_cast<std::uint16_t>(tr);
        op.p.stroke_rounded_varying.br = static_cast<std::uint16_t>(br);
        op.p.stroke_rounded_varying.bl = static_cast<std::uint16_t>(bl);
        op.p.stroke_rounded_varying.thickness = thickness;
        list_.ops.push_back(op);
    }

    void fill_rounded_rect_ring(const Rect& r, float radius,
                                float thickness, Color c) override {
        fill_box_shadow(r, radius, c, 0.0f, 0.0f, 0.0f, thickness,
                        /*inset=*/true);
    }

    void fill_linear_gradient_rect(const Rect& r, float angle_deg,
                                   Color stop0, Color stop1,
                                   float tl = 0, float tr = 0,
                                   float br = 0, float bl = 0) override {
        PaintOp op{};
        op.kind = PaintOpKind::FillLinearGradientRect;
        auto& g = op.p.fill_linear_gradient;
        g.x          = static_cast<std::int16_t>(r.x);
        g.y          = static_cast<std::int16_t>(r.y);
        g.w          = static_cast<std::int16_t>(r.w);
        g.h          = static_cast<std::int16_t>(r.h);
        g.angle_deg  = static_cast<std::int16_t>(angle_deg);
        g.tl         = static_cast<std::uint8_t>(std::clamp(tl, 0.f, 255.f));
        g.tr         = static_cast<std::uint8_t>(std::clamp(tr, 0.f, 255.f));
        g.br         = static_cast<std::uint8_t>(std::clamp(br, 0.f, 255.f));
        g.bl         = static_cast<std::uint8_t>(std::clamp(bl, 0.f, 255.f));
        g.pad_       = 0;
        g.stop0_rgba = pack(stop0);
        g.stop1_rgba = pack(stop1);
        list_.ops.push_back(op);
    }

    void fill_radial_gradient_rect(const Rect& r,
                                   Color stop0, Color stop1,
                                   float tl = 0, float tr = 0,
                                   float br = 0, float bl = 0,
                                   float center_x_pct = 50,
                                   float center_y_pct = 50,
                                   float stop1_pos_pct = 100) override {
        PaintOp op{};
        op.kind = PaintOpKind::FillRadialGradientRect;
        auto& g = op.p.fill_radial_gradient;
        g.x          = static_cast<std::int16_t>(r.x);
        g.y          = static_cast<std::int16_t>(r.y);
        g.w          = static_cast<std::int16_t>(r.w);
        g.h          = static_cast<std::int16_t>(r.h);
        g.tl         = static_cast<std::uint8_t>(std::clamp(tl, 0.f, 255.f));
        g.tr         = static_cast<std::uint8_t>(std::clamp(tr, 0.f, 255.f));
        g.br         = static_cast<std::uint8_t>(std::clamp(br, 0.f, 255.f));
        g.bl         = static_cast<std::uint8_t>(std::clamp(bl, 0.f, 255.f));
        g.center_x_pct = static_cast<std::uint8_t>(
            std::clamp(std::round(center_x_pct), 0.f, 100.f));
        g.center_y_pct = static_cast<std::uint8_t>(
            std::clamp(std::round(center_y_pct), 0.f, 100.f));
        g.stop1_pos_pct = static_cast<std::uint8_t>(
            std::clamp(std::round(stop1_pos_pct), 1.f, 100.f));
        g.pad_       = 0;
        g.stop0_rgba = pack(stop0);
        g.stop1_rgba = pack(stop1);
        list_.ops.push_back(op);
    }

    void fill_linear_stripes_rect(const Rect& r, float angle_deg,
                                  Color stripe, float tile_size,
                                  float tl = 0, float tr = 0,
                                  float br = 0, float bl = 0) override {
        PaintOp op{};
        op.kind = PaintOpKind::FillLinearStripesRect;
        auto& g = op.p.fill_linear_stripes;
        g.x = static_cast<std::int16_t>(r.x);
        g.y = static_cast<std::int16_t>(r.y);
        g.w = static_cast<std::int16_t>(r.w);
        g.h = static_cast<std::int16_t>(r.h);
        g.angle_deg = static_cast<std::int16_t>(angle_deg);
        g.tile_size = static_cast<std::uint16_t>(
            std::clamp(tile_size, 1.0f, 65535.0f));
        g.tl = static_cast<std::uint8_t>(std::clamp(tl, 0.f, 255.f));
        g.tr = static_cast<std::uint8_t>(std::clamp(tr, 0.f, 255.f));
        g.br = static_cast<std::uint8_t>(std::clamp(br, 0.f, 255.f));
        g.bl = static_cast<std::uint8_t>(std::clamp(bl, 0.f, 255.f));
        g.stripe_rgba = pack(stripe);
        g.pad_ = 0;
        list_.ops.push_back(op);
    }

    void fill_grid_rect(const Rect& r, Color line, float tile_size,
                        float line_width, float tl = 0, float tr = 0,
                        float br = 0, float bl = 0) override {
        PaintOp op{};
        op.kind = PaintOpKind::FillGridRect;
        auto& g = op.p.fill_grid;
        g.x = static_cast<std::int16_t>(r.x);
        g.y = static_cast<std::int16_t>(r.y);
        g.w = static_cast<std::int16_t>(r.w);
        g.h = static_cast<std::int16_t>(r.h);
        g.tile_size = static_cast<std::uint16_t>(
            std::clamp(tile_size, 1.0f, 65535.0f));
        g.line_width = static_cast<std::uint16_t>(
            std::clamp(line_width, 1.0f, 65535.0f));
        g.tl = static_cast<std::uint8_t>(std::clamp(tl, 0.f, 255.f));
        g.tr = static_cast<std::uint8_t>(std::clamp(tr, 0.f, 255.f));
        g.br = static_cast<std::uint8_t>(std::clamp(br, 0.f, 255.f));
        g.bl = static_cast<std::uint8_t>(std::clamp(bl, 0.f, 255.f));
        g.line_rgba = pack(line);
        g.pad_ = 0;
        list_.ops.push_back(op);
    }

    void fill_box_shadow(const Rect& r, float radius, Color color,
                         float offset_x, float offset_y,
                         float blur, float spread, bool inset) override {
        PaintOp op{};
        op.kind = PaintOpKind::FillBoxShadow;
        auto& s = op.p.fill_box_shadow;
        s.x        = static_cast<std::int16_t>(r.x);
        s.y        = static_cast<std::int16_t>(r.y);
        s.w        = static_cast<std::int16_t>(r.w);
        s.h        = static_cast<std::int16_t>(r.h);
        s.rgba     = pack(color);
        s.offset_x = static_cast<std::int16_t>(offset_x);
        s.offset_y = static_cast<std::int16_t>(offset_y);
        s.blur     = static_cast<std::int16_t>(blur);
        s.spread   = static_cast<std::int16_t>(spread);
        s.radius   = static_cast<std::uint16_t>(std::clamp(radius, 0.f, 65535.f));
        s.inset    = inset ? 1u : 0u;
        s.pad_     = 0;
        list_.ops.push_back(op);
    }

    void fill_path(const float* cmds, std::size_t count,
                   const PathPaint& paint) override {
        record_path(PaintOpKind::FillPath, cmds, count, paint, 0.0f,
                    LineCap::Butt, LineJoin::Miter);
    }

    void stroke_path(const float* cmds, std::size_t count,
                     const PathPaint& paint, float width,
                     LineCap cap, LineJoin join) override {
        if (width <= 0.0f) return;
        record_path(PaintOpKind::StrokePath, cmds, count, paint, width,
                    cap, join);
    }

    std::uint32_t resolve_font(std::string_view family, int size_px,
                               int weight, bool italic) override {
        return font_resolver_
                 ? font_resolver_->resolve_font(family, size_px, weight, italic)
                 : 0u;
    }

    bool register_font_face(std::string_view family, int weight, bool italic,
                            std::string_view bytes) override {
        return font_resolver_
                 ? font_resolver_->register_font_face(family, weight, italic,
                                                      bytes)
                 : false;
    }

    int measure_text(std::uint32_t font, std::string_view text) override {
        return font_resolver_ ? font_resolver_->measure_text(font, text) : 0;
    }

    TextMetrics text_metrics(std::uint32_t font) override {
        return font_resolver_ ? font_resolver_->text_metrics(font) : TextMetrics{};
    }

    void draw_text(std::uint32_t font, const Point& pos,
                   std::string_view text, Color c) override {
        const auto [off, len] = list_.intern_text(text);
        const int measured_w = measure_text(font, text);
        const TextMetrics metrics = text_metrics(font);
        const auto clamp_u16 = [](float v) {
            return static_cast<std::uint16_t>(
                std::clamp(std::ceil(v), 0.0f, 65535.0f));
        };
        PaintOp op{};
        op.kind = PaintOpKind::DrawText;
        op.p.draw_text.font_handle = font;
        op.p.draw_text.x = static_cast<std::int16_t>(pos.x);
        op.p.draw_text.y = static_cast<std::int16_t>(pos.y);
        op.p.draw_text.rgba = pack(c);
        op.p.draw_text.text_offset = off;
        op.p.draw_text.text_len    = len;
        op.p.draw_text.measured_w =
            static_cast<std::uint16_t>(std::clamp(measured_w, 0, 65535));
        op.p.draw_text.measured_h =
            clamp_u16(metrics.ascender + metrics.descender);
        list_.ops.push_back(op);
        list_.set_last_op_bounds_override(
            Rect{pos.x, pos.y, static_cast<int>(op.p.draw_text.measured_w),
                 static_cast<int>(op.p.draw_text.measured_h)});
    }

    Size measure_text_box(std::uint32_t font, std::string_view text, float max_w,
                          float line_height_mult = 1.0f,
                          float letter_spacing_px = 0.0f) override {
        return font_resolver_
                 ? font_resolver_->measure_text_box(font, text, max_w, line_height_mult,
                                                    letter_spacing_px)
                 : Size{};
    }

    void draw_text_box(std::uint32_t font, float pos_x, float pos_y,
                       std::string_view text, Color c, float max_w,
                       float line_height_mult = 1.0f,
                       float letter_spacing_px = 0.0f,
                       TextAlign align = TextAlign::Left) override {
        const auto [off, len] = list_.intern_text(text);
        const Size measured =
            measure_text_box(font, text, max_w, line_height_mult,
                             letter_spacing_px);
        const float fx = std::floor(pos_x);
        const float fy = std::floor(pos_y);
        PaintOp op{};
        op.kind = PaintOpKind::DrawTextBox;
        op.p.draw_text_box.font_handle = font;
        op.p.draw_text_box.x = static_cast<std::int16_t>(fx);
        op.p.draw_text_box.y = static_cast<std::int16_t>(fy);
        const auto frac4 = [](float v) {
            return static_cast<std::uint8_t>(
                std::clamp(v * 16.0f + 0.5f, 0.0f, 15.0f));
        };
        op.p.draw_text_box.subpx_frac = static_cast<std::uint8_t>(
            (frac4(pos_x - fx) << 4) | frac4(pos_y - fy));
        op.p.draw_text_box.rgba = pack(c);
        op.p.draw_text_box.text_offset = off;
        op.p.draw_text_box.text_len    = len;
        // Cap at u16; document content widths > 65535 don't happen.
        // For nowrap, max_w may be 1e6 — cap to u16 max (65535).
        op.p.draw_text_box.max_width =
            static_cast<std::uint16_t>(max_w > 65535.f ? 65535 : (max_w < 0.f ? 0 : max_w));
        op.p.draw_text_box.line_height_x100 =
            static_cast<std::uint16_t>(std::clamp(line_height_mult * 100.0f, 0.0f, 65535.0f));
        // letter_spacing × 100, clamped to int16 range.
        op.p.draw_text_box.letter_spacing_x100 =
            static_cast<std::int16_t>(std::clamp(letter_spacing_px * 100.0f,
                                                  -32768.0f, 32767.0f));
        op.p.draw_text_box.align = static_cast<std::uint8_t>(align);
        op.p.draw_text_box.measured_h =
            static_cast<std::uint16_t>(
                std::clamp(std::ceil(static_cast<double>(measured.height)),
                           0.0, 65535.0));
        list_.ops.push_back(op);
        int visual_x = static_cast<int>(fx);
        const int measured_w = std::max(0, measured.width);
        const int measured_h = std::max(0, measured.height);
        if (align == TextAlign::Center) {
            visual_x += static_cast<int>(
                std::floor((max_w - static_cast<float>(measured_w)) * 0.5f));
        } else if (align == TextAlign::Right) {
            visual_x += static_cast<int>(
                std::floor(max_w - static_cast<float>(measured_w)));
        }
        // +1 covers the sub-pixel fraction spilling into the next row/col.
        list_.set_last_op_bounds_override(
            Rect{visual_x, static_cast<int>(fy),
                 measured_w + 1, measured_h + 1});
    }

    std::uint32_t load_image(std::string_view url) override {
        return font_resolver_ ? font_resolver_->load_image(url) : 0u;
    }
    Size image_size(std::uint32_t image) override {
        return font_resolver_ ? font_resolver_->image_size(image) : Size{};
    }
    void draw_image(std::uint32_t image, const Rect& dst, const Rect& src) override {
        PaintOp op{};
        op.kind = PaintOpKind::DrawImage;
        op.p.draw_image.image_handle = image;
        op.p.draw_image.x  = static_cast<std::int16_t>(dst.x);
        op.p.draw_image.y  = static_cast<std::int16_t>(dst.y);
        op.p.draw_image.w  = static_cast<std::int16_t>(dst.w);
        op.p.draw_image.h  = static_cast<std::int16_t>(dst.h);
        op.p.draw_image.sx = static_cast<std::int16_t>(src.x);
        op.p.draw_image.sy = static_cast<std::int16_t>(src.y);
        op.p.draw_image.sw = static_cast<std::int16_t>(src.w);
        op.p.draw_image.sh = static_cast<std::int16_t>(src.h);
        list_.ops.push_back(op);
    }

    void push_clip(const Rect& r) override {
        PaintOp op{};
        op.kind = PaintOpKind::PushClip;
        op.p.clip.x = static_cast<std::int16_t>(r.x);
        op.p.clip.y = static_cast<std::int16_t>(r.y);
        op.p.clip.w = static_cast<std::int16_t>(r.w);
        op.p.clip.h = static_cast<std::int16_t>(r.h);
        list_.ops.push_back(op);
    }
    void pop_clip() override {
        PaintOp op{};
        op.kind = PaintOpKind::PopClip;
        list_.ops.push_back(op);
    }

    void push_alpha(float alpha) override {
        PaintOp op{};
        op.kind = PaintOpKind::PushAlpha;
        op.p.push_alpha.alpha = alpha;
        list_.ops.push_back(op);
    }
    void pop_alpha() override {
        PaintOp op{};
        op.kind = PaintOpKind::PopAlpha;
        list_.ops.push_back(op);
    }

    void push_transform(const Mat2x3& m) override {
        PaintOp op{};
        op.kind = PaintOpKind::PushTransform;
        op.p.push_transform.a  = m.a;
        op.p.push_transform.b  = m.b;
        op.p.push_transform.c  = m.c;
        op.p.push_transform.d  = m.d;
        op.p.push_transform.tx = m.tx;
        op.p.push_transform.ty = m.ty;
        list_.ops.push_back(op);
    }
    void pop_transform() override {
        PaintOp op{};
        op.kind = PaintOpKind::PopTransform;
        list_.ops.push_back(op);
    }

private:
    static std::uint32_t pack(Color c) {
        return (std::uint32_t(c.r) << 24)
             | (std::uint32_t(c.g) << 16)
             | (std::uint32_t(c.b) <<  8)
             |  std::uint32_t(c.a);
    }

    void record_path(PaintOpKind kind, const float* cmds, std::size_t count,
                     const PathPaint& paint, float thickness,
                     LineCap cap, LineJoin join) {
        if (cmds == nullptr || count == 0) return;

        // Bounds over every coordinate in the stream (control points
        // included — a safe over-approximation), inflated by the stroke.
        float min_x = std::numeric_limits<float>::max();
        float min_y = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        float max_y = std::numeric_limits<float>::lowest();
        bool any = false;
        std::size_t i = 0;
        while (i < count) {
            const float verb = cmds[i];
            std::size_t coords = 0;
            if (verb == kPathMove || verb == kPathLine)  coords = 2;
            else if (verb == kPathCubic)                 coords = 6;
            else if (verb == kPathClose)                 coords = 0;
            else break;
            if (i + 1 + coords > count) break;
            for (std::size_t k = 0; k < coords; k += 2) {
                min_x = std::min(min_x, cmds[i + 1 + k]);
                max_x = std::max(max_x, cmds[i + 1 + k]);
                min_y = std::min(min_y, cmds[i + 2 + k]);
                max_y = std::max(max_y, cmds[i + 2 + k]);
                any = true;
            }
            i += 1 + coords;
        }
        if (!any) return;
        const float pad = thickness * 0.5f + 1.0f;
        const auto clamp_i16 = [](float v) {
            return static_cast<std::int16_t>(
                std::clamp(v, -32768.0f, 32767.0f));
        };
        const std::int16_t bx = clamp_i16(std::floor(min_x - pad));
        const std::int16_t by = clamp_i16(std::floor(min_y - pad));
        const std::int16_t bw = clamp_i16(std::ceil(max_x + pad) - bx);
        const std::int16_t bh = clamp_i16(std::ceil(max_y + pad) - by);

        // 4-align the pool so the float command stream is readable in
        // place at replay time. Padding is deterministic (zero bytes),
        // so content hashing stays stable.
        while (list_.text_pool.size() % 4 != 0) {
            list_.text_pool.push_back('\0');
        }
        const std::uint32_t n = std::min<std::uint32_t>(
            paint.stop_count, PathPaint::kMaxStops);
        std::uint32_t header[8 + 2 * PathPaint::kMaxStops] = {};
        header[0] = static_cast<std::uint32_t>(paint.kind);
        std::memcpy(&header[1], &paint.x0, 6 * sizeof(float));
        header[7] = n;
        std::memcpy(&header[8], paint.offsets, n * sizeof(float));
        for (std::uint32_t s = 0; s < n; ++s) {
            header[8 + n + s] = pack(paint.colors[s]);
        }
        const std::size_t header_bytes = (8 + 2u * n) * 4;
        const auto [off, hlen] = list_.intern_bytes(header, header_bytes);
        (void)hlen;
        list_.intern_bytes(cmds, count * sizeof(float));

        PaintOp op{};
        op.kind = kind;
        op.p.path.data_offset = off;
        op.p.path.data_len =
            static_cast<std::uint32_t>(header_bytes + count * sizeof(float));
        op.p.path.bx = bx;
        op.p.path.by = by;
        op.p.path.bw = bw;
        op.p.path.bh = bh;
        op.p.path.thickness = thickness;
        op.p.path.cap  = static_cast<std::uint8_t>(cap);
        op.p.path.join = static_cast<std::uint8_t>(join);
        op.p.path.pad_ = 0;
        list_.ops.push_back(op);
    }

    Painter*    font_resolver_;
    DisplayList list_;
};

/// Replay a recorded DisplayList through a Painter. Used at rasterize
/// time to actually emit nvg* calls. begin_frame / end_frame on the
/// target Painter are the caller's responsibility — replay() only
/// emits content ops.
inline void replay_op(const DisplayList& list, const PaintOp& op,
                      Painter& target) {
    static const auto unpack = [](std::uint32_t v) {
        return Color{
            static_cast<std::uint8_t>((v >> 24) & 0xFF),
            static_cast<std::uint8_t>((v >> 16) & 0xFF),
            static_cast<std::uint8_t>((v >>  8) & 0xFF),
            static_cast<std::uint8_t>( v        & 0xFF),
        };
    };
    switch (op.kind) {
            case PaintOpKind::FillRect: {
                const auto& r = op.p.fill_rect;
                target.fill_rect(Rect{r.x, r.y, r.w, r.h}, unpack(r.rgba));
                break;
            }
            case PaintOpKind::StrokeRect: {
                const auto& r = op.p.stroke_rect;
                target.stroke_rect(Rect{r.x, r.y, r.w, r.h},
                                   unpack(r.rgba), r.thickness);
                break;
            }
            case PaintOpKind::StrokeLine: {
                const auto& ln = op.p.stroke_line;
                target.stroke_line(ln.x0, ln.y0, ln.x1, ln.y1,
                                   unpack(ln.rgba), ln.thickness);
                break;
            }
            case PaintOpKind::FillCircle: {
                const auto& fc = op.p.fill_circle;
                target.fill_circle(fc.cx, fc.cy, fc.radius, unpack(fc.rgba));
                break;
            }
            case PaintOpKind::StrokeArc: {
                const auto& sa = op.p.stroke_arc;
                target.stroke_arc(sa.cx, sa.cy, sa.radius,
                                  static_cast<float>(sa.angle_start),
                                  static_cast<float>(sa.angle_end),
                                  unpack(sa.rgba), sa.thickness);
                break;
            }
            case PaintOpKind::FillRoundedRect: {
                const auto& r = op.p.fill_rounded;
                target.fill_rounded_rect(Rect{r.x, r.y, r.w, r.h},
                                         r.radius, unpack(r.rgba));
                break;
            }
            case PaintOpKind::StrokeRoundedRect: {
                const auto& r = op.p.stroke_rounded;
                target.stroke_rounded_rect(Rect{r.x, r.y, r.w, r.h},
                                           r.radius, unpack(r.rgba), r.thickness);
                break;
            }
            case PaintOpKind::FillRoundedRectVarying: {
                const auto& r = op.p.fill_rounded_varying;
                target.fill_rounded_rect_varying(
                    Rect{r.x, r.y, r.w, r.h},
                    static_cast<float>(r.tl), static_cast<float>(r.tr),
                    static_cast<float>(r.br), static_cast<float>(r.bl),
                    unpack(r.rgba));
                break;
            }
            case PaintOpKind::StrokeRoundedRectVarying: {
                const auto& r = op.p.stroke_rounded_varying;
                target.stroke_rounded_rect_varying(
                    Rect{r.x, r.y, r.w, r.h},
                    static_cast<float>(r.tl), static_cast<float>(r.tr),
                    static_cast<float>(r.br), static_cast<float>(r.bl),
                    unpack(r.rgba), r.thickness);
                break;
            }
            case PaintOpKind::FillLinearGradientRect: {
                const auto& g = op.p.fill_linear_gradient;
                target.fill_linear_gradient_rect(
                    Rect{g.x, g.y, g.w, g.h},
                    static_cast<float>(g.angle_deg),
                    unpack(g.stop0_rgba), unpack(g.stop1_rgba),
                    static_cast<float>(g.tl), static_cast<float>(g.tr),
                    static_cast<float>(g.br), static_cast<float>(g.bl));
                break;
            }
            case PaintOpKind::FillRadialGradientRect: {
                const auto& g = op.p.fill_radial_gradient;
                target.fill_radial_gradient_rect(
                    Rect{g.x, g.y, g.w, g.h},
                    unpack(g.stop0_rgba), unpack(g.stop1_rgba),
                    static_cast<float>(g.tl), static_cast<float>(g.tr),
                    static_cast<float>(g.br), static_cast<float>(g.bl),
                    static_cast<float>(g.center_x_pct),
                    static_cast<float>(g.center_y_pct),
                    static_cast<float>(g.stop1_pos_pct));
                break;
            }
            case PaintOpKind::FillLinearStripesRect: {
                const auto& g = op.p.fill_linear_stripes;
                target.fill_linear_stripes_rect(
                    Rect{g.x, g.y, g.w, g.h},
                    static_cast<float>(g.angle_deg),
                    unpack(g.stripe_rgba),
                    static_cast<float>(g.tile_size),
                    static_cast<float>(g.tl), static_cast<float>(g.tr),
                    static_cast<float>(g.br), static_cast<float>(g.bl));
                break;
            }
            case PaintOpKind::FillGridRect: {
                const auto& g = op.p.fill_grid;
                target.fill_grid_rect(
                    Rect{g.x, g.y, g.w, g.h},
                    unpack(g.line_rgba),
                    static_cast<float>(g.tile_size),
                    static_cast<float>(g.line_width),
                    static_cast<float>(g.tl), static_cast<float>(g.tr),
                    static_cast<float>(g.br), static_cast<float>(g.bl));
                break;
            }
            case PaintOpKind::FillBoxShadow: {
                const auto& s = op.p.fill_box_shadow;
                target.fill_box_shadow(
                    Rect{s.x, s.y, s.w, s.h},
                    static_cast<float>(s.radius), unpack(s.rgba),
                    static_cast<float>(s.offset_x), static_cast<float>(s.offset_y),
                    static_cast<float>(s.blur), static_cast<float>(s.spread),
                    s.inset != 0);
                break;
            }
            case PaintOpKind::DrawText: {
                const auto& t = op.p.draw_text;
                target.draw_text(t.font_handle, Point{t.x, t.y},
                                 list.text_at(t.text_offset, t.text_len),
                                 unpack(t.rgba));
                break;
            }
            case PaintOpKind::DrawTextBox: {
                const auto& t = op.p.draw_text_box;
                target.draw_text_box(t.font_handle,
                                     static_cast<float>(t.x) +
                                         static_cast<float>(t.subpx_frac >> 4) /
                                             16.0f,
                                     static_cast<float>(t.y) +
                                         static_cast<float>(t.subpx_frac & 0xF) /
                                             16.0f,
                                     list.text_at(t.text_offset, t.text_len),
                                     unpack(t.rgba),
                                     static_cast<float>(t.max_width),
                                     static_cast<float>(t.line_height_x100) / 100.0f,
                                     static_cast<float>(t.letter_spacing_x100) / 100.0f,
                                     static_cast<Painter::TextAlign>(t.align));
                break;
            }
            case PaintOpKind::DrawImage: {
                const auto& i = op.p.draw_image;
                target.draw_image(i.image_handle,
                                  Rect{i.x, i.y, i.w, i.h},
                                  Rect{i.sx, i.sy, i.sw, i.sh});
                break;
            }
            case PaintOpKind::PushClip: {
                const auto& c = op.p.clip;
                target.push_clip(Rect{c.x, c.y, c.w, c.h});
                break;
            }
            case PaintOpKind::PopClip:
                target.pop_clip();
                break;
            case PaintOpKind::PushAlpha:
                target.push_alpha(op.p.push_alpha.alpha);
                break;
            case PaintOpKind::PopAlpha:
                target.pop_alpha();
                break;
            case PaintOpKind::PushTransform: {
                const auto& m = op.p.push_transform;
                target.push_transform(Mat2x3{m.a, m.b, m.c, m.d, m.tx, m.ty});
                break;
            }
            case PaintOpKind::PopTransform:
                target.pop_transform();
                break;
            case PaintOpKind::FillPath:
            case PaintOpKind::StrokePath: {
                const auto& pp = op.p.path;
                PathPaint paint{};
                const float* cmds = nullptr;
                std::size_t count = 0;
                if (!path_blob_decode(
                        list.bytes_at(pp.data_offset, pp.data_len),
                        paint, cmds, count)) {
                    break;
                }
                if (op.kind == PaintOpKind::FillPath) {
                    target.fill_path(cmds, count, paint);
                } else {
                    target.stroke_path(cmds, count, paint, pp.thickness,
                                       static_cast<LineCap>(pp.cap),
                                       static_cast<LineJoin>(pp.join));
                }
                break;
            }
        }
}

struct ReplayClipStats {
    std::uint32_t emitted{0};
    std::uint32_t culled{0};
};

struct DisplayListDiffBounds {
    bool known{true};
    Rect bounds{};
    std::uint32_t changed_ops{0};
    std::uint8_t first_changed_kind{0xFF};
    Rect first_old_bounds{};
    Rect first_new_bounds{};
    std::uint8_t largest_changed_kind{0xFF};
    Rect largest_changed_bounds{};
};

inline bool replay_rect_valid(const Rect& r) {
    return r.w > 0 && r.h > 0;
}

inline bool replay_rect_intersects(const Rect& a, const Rect& b) {
    return replay_rect_valid(a) && replay_rect_valid(b) &&
           a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

inline Rect replay_inflate_rect(Rect r, int pad) {
    r.x -= pad;
    r.y -= pad;
    r.w += pad * 2;
    r.h += pad * 2;
    return r;
}

inline bool replay_op_bounds(const PaintOp& op, Rect& out) {
    switch (op.kind) {
        case PaintOpKind::FillRect: {
            const auto& r = op.p.fill_rect;
            out = Rect{r.x, r.y, r.w, r.h};
            return true;
        }
        case PaintOpKind::StrokeRect: {
            const auto& r = op.p.stroke_rect;
            out = replay_inflate_rect(Rect{r.x, r.y, r.w, r.h},
                                      static_cast<int>(std::ceil(r.thickness)));
            return true;
        }
        case PaintOpKind::StrokeLine: {
            const auto& ln = op.p.stroke_line;
            const int pad = static_cast<int>(std::ceil(ln.thickness)) + 1;
            const int x0 = static_cast<int>(std::floor(std::min(ln.x0, ln.x1))) - pad;
            const int y0 = static_cast<int>(std::floor(std::min(ln.y0, ln.y1))) - pad;
            const int x1 = static_cast<int>(std::ceil(std::max(ln.x0, ln.x1))) + pad;
            const int y1 = static_cast<int>(std::ceil(std::max(ln.y0, ln.y1))) + pad;
            out = Rect{x0, y0, x1 - x0, y1 - y0};
            return true;
        }
        case PaintOpKind::FillCircle: {
            const auto& c = op.p.fill_circle;
            const int x0 = static_cast<int>(std::floor(c.cx - c.radius)) - 1;
            const int y0 = static_cast<int>(std::floor(c.cy - c.radius)) - 1;
            const int x1 = static_cast<int>(std::ceil(c.cx + c.radius)) + 1;
            const int y1 = static_cast<int>(std::ceil(c.cy + c.radius)) + 1;
            out = Rect{x0, y0, x1 - x0, y1 - y0};
            return true;
        }
        case PaintOpKind::StrokeArc: {
            const auto& a = op.p.stroke_arc;
            const float rr = a.radius + a.thickness + 1.0f;
            const int x0 = static_cast<int>(std::floor(a.cx - rr));
            const int y0 = static_cast<int>(std::floor(a.cy - rr));
            const int x1 = static_cast<int>(std::ceil(a.cx + rr));
            const int y1 = static_cast<int>(std::ceil(a.cy + rr));
            out = Rect{x0, y0, x1 - x0, y1 - y0};
            return true;
        }
        case PaintOpKind::FillRoundedRect: {
            const auto& r = op.p.fill_rounded;
            out = Rect{r.x, r.y, r.w, r.h};
            return true;
        }
        case PaintOpKind::StrokeRoundedRect: {
            const auto& r = op.p.stroke_rounded;
            out = replay_inflate_rect(Rect{r.x, r.y, r.w, r.h},
                                      static_cast<int>(std::ceil(r.thickness)));
            return true;
        }
        case PaintOpKind::FillRoundedRectVarying: {
            const auto& r = op.p.fill_rounded_varying;
            out = Rect{r.x, r.y, r.w, r.h};
            return true;
        }
        case PaintOpKind::StrokeRoundedRectVarying: {
            const auto& r = op.p.stroke_rounded_varying;
            out = replay_inflate_rect(Rect{r.x, r.y, r.w, r.h},
                                      static_cast<int>(std::ceil(r.thickness)));
            return true;
        }
        case PaintOpKind::FillLinearGradientRect: {
            const auto& r = op.p.fill_linear_gradient;
            out = Rect{r.x, r.y, r.w, r.h};
            return true;
        }
        case PaintOpKind::FillRadialGradientRect: {
            const auto& r = op.p.fill_radial_gradient;
            out = Rect{r.x, r.y, r.w, r.h};
            return true;
        }
        case PaintOpKind::FillLinearStripesRect: {
            const auto& r = op.p.fill_linear_stripes;
            out = Rect{r.x, r.y, r.w, r.h};
            return true;
        }
        case PaintOpKind::FillGridRect: {
            const auto& r = op.p.fill_grid;
            out = Rect{r.x, r.y, r.w, r.h};
            return true;
        }
        case PaintOpKind::FillBoxShadow: {
            const auto& s = op.p.fill_box_shadow;
            const int spread = std::max(0, static_cast<int>(s.spread));
            const int blur = std::max(0, static_cast<int>(s.blur));
            const int pad = spread + blur + 2;
            out = Rect{s.x + static_cast<int>(s.offset_x) - pad,
                       s.y + static_cast<int>(s.offset_y) - pad,
                       s.w + pad * 2,
                       s.h + pad * 2};
            return true;
        }
        case PaintOpKind::DrawImage: {
            const auto& r = op.p.draw_image;
            out = Rect{r.x, r.y, r.w, r.h};
            return true;
        }
        case PaintOpKind::DrawText: {
            const auto& t = op.p.draw_text;
            out = Rect{t.x, t.y, static_cast<int>(t.measured_w),
                       static_cast<int>(t.measured_h)};
            return replay_rect_valid(out);
        }
        case PaintOpKind::DrawTextBox: {
            const auto& t = op.p.draw_text_box;
            out = Rect{t.x, t.y, static_cast<int>(t.max_width),
                       static_cast<int>(t.measured_h)};
            return replay_rect_valid(out);
        }
        case PaintOpKind::PushClip: {
            const auto& c = op.p.clip;
            out = Rect{c.x, c.y, c.w, c.h};
            return true;
        }
        case PaintOpKind::FillPath:
        case PaintOpKind::StrokePath: {
            const auto& pp = op.p.path;
            out = Rect{pp.bx, pp.by, pp.bw, pp.bh};
            return true;
        }
        case PaintOpKind::PopClip:
        case PaintOpKind::PushAlpha:
        case PaintOpKind::PopAlpha:
        case PaintOpKind::PushTransform:
        case PaintOpKind::PopTransform:
            return false;
    }
    return false;
}

inline bool replay_op_bounds(const DisplayList& list,
                             std::size_t index,
                             Rect& out) {
    if (index < list.op_bounds_override.size() &&
        replay_rect_valid(list.op_bounds_override[index])) {
        out = list.op_bounds_override[index];
        return true;
    }
    if (index >= list.ops.size()) return false;
    return replay_op_bounds(list.ops[index], out);
}

inline Rect replay_transform_bounds(const Rect& r, const Mat2x3& m) {
    if (m.is_identity()) return r;
    const Vec2 p0 = m.apply(Vec2{static_cast<float>(r.x),
                                 static_cast<float>(r.y)});
    const Vec2 p1 = m.apply(Vec2{static_cast<float>(r.x + r.w),
                                 static_cast<float>(r.y)});
    const Vec2 p2 = m.apply(Vec2{static_cast<float>(r.x),
                                 static_cast<float>(r.y + r.h)});
    const Vec2 p3 = m.apply(Vec2{static_cast<float>(r.x + r.w),
                                 static_cast<float>(r.y + r.h)});
    const float min_x = std::min({p0.x, p1.x, p2.x, p3.x});
    const float min_y = std::min({p0.y, p1.y, p2.y, p3.y});
    const float max_x = std::max({p0.x, p1.x, p2.x, p3.x});
    const float max_y = std::max({p0.y, p1.y, p2.y, p3.y});
    const int x0 = static_cast<int>(std::floor(min_x)) - 1;
    const int y0 = static_cast<int>(std::floor(min_y)) - 1;
    const int x1 = static_cast<int>(std::ceil(max_x)) + 1;
    const int y1 = static_cast<int>(std::ceil(max_y)) + 1;
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

inline Mat2x3 replay_transform_from_op(const PaintOp& op) {
    const auto& m = op.p.push_transform;
    return Mat2x3{m.a, m.b, m.c, m.d, m.tx, m.ty};
}

inline Rect replay_union_rect(const Rect& a, const Rect& b) {
    if (!replay_rect_valid(a)) return b;
    if (!replay_rect_valid(b)) return a;
    const int x0 = std::min(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w);
    const int y1 = std::max(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

inline bool replay_paint_ops_equal(const DisplayList& a,
                                   const PaintOp& a_op,
                                   const DisplayList& b,
                                   const PaintOp& b_op) {
    if (a_op.kind != b_op.kind) return false;
    if (std::memcmp(&a_op, &b_op, sizeof(PaintOp)) != 0) return false;
    if (a_op.kind == PaintOpKind::DrawText) {
        return a.text_at(a_op.p.draw_text.text_offset,
                         a_op.p.draw_text.text_len)
            == b.text_at(b_op.p.draw_text.text_offset,
                         b_op.p.draw_text.text_len);
    }
    if (a_op.kind == PaintOpKind::DrawTextBox) {
        return a.text_at(a_op.p.draw_text_box.text_offset,
                         a_op.p.draw_text_box.text_len)
            == b.text_at(b_op.p.draw_text_box.text_offset,
                         b_op.p.draw_text_box.text_len);
    }
    if (a_op.kind == PaintOpKind::FillPath ||
        a_op.kind == PaintOpKind::StrokePath) {
        return a.bytes_at(a_op.p.path.data_offset, a_op.p.path.data_len)
            == b.bytes_at(b_op.p.path.data_offset, b_op.p.path.data_len);
    }
    return true;
}

inline bool replay_resized_solid_fill_diff_bounds(const PaintOp& old_op,
                                                  const PaintOp& new_op,
                                                  const Rect& old_bounds,
                                                  const Rect& new_bounds,
                                                  Rect& out) {
    if (old_op.kind != PaintOpKind::FillRect ||
        new_op.kind != PaintOpKind::FillRect) {
        return false;
    }
    if (old_op.p.fill_rect.rgba != new_op.p.fill_rect.rgba) {
        return false;
    }
    if (old_bounds.x == new_bounds.x &&
        old_bounds.y == new_bounds.y &&
        old_bounds.h == new_bounds.h) {
        const int old_x1 = old_bounds.x + old_bounds.w;
        const int new_x1 = new_bounds.x + new_bounds.w;
        const int x0 = std::min(old_x1, new_x1);
        const int x1 = std::max(old_x1, new_x1);
        out = Rect{x0, old_bounds.y, x1 - x0, old_bounds.h};
        return replay_rect_valid(out);
    }
    if (old_bounds.x == new_bounds.x &&
        old_bounds.y == new_bounds.y &&
        old_bounds.w == new_bounds.w) {
        const int old_y1 = old_bounds.y + old_bounds.h;
        const int new_y1 = new_bounds.y + new_bounds.h;
        const int y0 = std::min(old_y1, new_y1);
        const int y1 = std::max(old_y1, new_y1);
        out = Rect{old_bounds.x, y0, old_bounds.w, y1 - y0};
        return replay_rect_valid(out);
    }
    return false;
}

inline std::size_t replay_matching_transform_pop(const DisplayList& list,
                                                 std::size_t push_index) {
    int depth = 1;
    for (std::size_t i = push_index + 1; i < list.ops.size(); ++i) {
        if (list.ops[i].kind == PaintOpKind::PushTransform) ++depth;
        if (list.ops[i].kind == PaintOpKind::PopTransform) {
            --depth;
            if (depth == 0) return i;
        }
    }
    return list.ops.size();
}

inline bool replay_range_bounds(const DisplayList& list,
                                std::size_t begin,
                                std::size_t end,
                                Mat2x3 current_transform,
                                Rect& out) {
    std::array<Mat2x3, 64> transform_stack{};
    int transform_depth = 0;
    int transform_overflow_depth = 0;
    out = {};

    for (std::size_t i = begin; i < end; ++i) {
        const PaintOp& op = list.ops[i];
        if (op.kind == PaintOpKind::PushTransform) {
            if (transform_overflow_depth > 0 ||
                transform_depth >= static_cast<int>(transform_stack.size())) {
                ++transform_overflow_depth;
            } else {
                transform_stack[static_cast<std::size_t>(transform_depth)] =
                    current_transform;
                ++transform_depth;
                current_transform =
                    current_transform.then(replay_transform_from_op(op));
            }
            continue;
        }
        if (op.kind == PaintOpKind::PopTransform) {
            if (transform_overflow_depth > 0) {
                --transform_overflow_depth;
            } else if (transform_depth > 0) {
                --transform_depth;
                current_transform =
                    transform_stack[static_cast<std::size_t>(transform_depth)];
            }
            continue;
        }
        if (transform_overflow_depth > 0) {
            return false;
        }

        Rect bounds{};
        if (!replay_op_bounds(list, i, bounds)) continue;
        if (op.kind == PaintOpKind::PushClip) continue;
        out = replay_union_rect(out, replay_transform_bounds(bounds,
                                                             current_transform));
    }
    return true;
}

inline void prepare_replay_metadata(DisplayList& list) {
    list.transform_ranges.clear();
    list.clip_ranges.clear();
    list.transform_ranges.resize(list.ops.size());
    list.clip_ranges.resize(list.ops.size());

    std::array<std::size_t, 64> clip_stack{};
    int clip_depth = 0;
    int clip_overflow_depth = 0;

    for (std::size_t i = 0; i < list.ops.size(); ++i) {
        if (list.ops[i].kind == PaintOpKind::PushClip) {
            if (clip_overflow_depth > 0 ||
                clip_depth >= static_cast<int>(clip_stack.size())) {
                ++clip_overflow_depth;
            } else {
                clip_stack[static_cast<std::size_t>(clip_depth)] = i;
                ++clip_depth;
            }
            continue;
        }
        if (list.ops[i].kind == PaintOpKind::PopClip) {
            if (clip_overflow_depth > 0) {
                --clip_overflow_depth;
            } else if (clip_depth > 0) {
                --clip_depth;
                const std::size_t push_index =
                    clip_stack[static_cast<std::size_t>(clip_depth)];
                if (i <= std::numeric_limits<std::uint32_t>::max()) {
                    list.clip_ranges[push_index].pop_index =
                        static_cast<std::uint32_t>(i);
                }
            }
            continue;
        }

        if (list.ops[i].kind == PaintOpKind::PushTransform) {
            const std::size_t matching_pop = replay_matching_transform_pop(list, i);
            if (matching_pop >= list.ops.size() ||
                matching_pop > std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }

            auto& range = list.transform_ranges[i];
            range.pop_index = static_cast<std::uint32_t>(matching_pop);

            Rect bounds{};
            if (replay_range_bounds(list, i + 1, matching_pop,
                                    replay_transform_from_op(list.ops[i]),
                                    bounds)) {
                range.bounds = bounds;
                range.bounds_known = 1;
            }
        }
    }
}

inline const DisplayListTransformRange* replay_transform_range_at(
    const DisplayList& list,
    std::size_t index) {
    if (index >= list.transform_ranges.size()) return nullptr;
    const auto& range = list.transform_ranges[index];
    if (range.pop_index == std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    return &range;
}

inline const DisplayListClipRange* replay_clip_range_at(
    const DisplayList& list,
    std::size_t index) {
    if (index >= list.clip_ranges.size()) return nullptr;
    const auto& range = list.clip_ranges[index];
    if (range.pop_index == std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    return &range;
}

inline bool replay_visual_bounds_for_op(const DisplayList& list,
                                        std::size_t index,
                                        const Mat2x3& current_transform,
                                        Rect& out) {
    if (index >= list.ops.size()) return false;
    const auto& op = list.ops[index];
    if (op.kind == PaintOpKind::PushTransform) {
        if (const auto* range = replay_transform_range_at(list, index)) {
            if (range->bounds_known) {
                out = replay_transform_bounds(range->bounds,
                                              current_transform);
                return replay_rect_valid(out);
            }
        }
        return false;
    }
    if (op.kind == PaintOpKind::PushAlpha) {
        return false;
    }

    Rect local{};
    if (!replay_op_bounds(list, index, local)) return false;
    out = replay_transform_bounds(local, current_transform);
    return replay_rect_valid(out);
}

inline bool replay_compute_visual_bounds(const DisplayList& list,
                                         std::vector<Rect>& bounds,
                                         std::vector<std::uint8_t>& known) {
    bounds.clear();
    known.clear();
    bounds.resize(list.ops.size());
    known.resize(list.ops.size(), 0);

    std::array<Mat2x3, 64> transform_stack{};
    int transform_depth = 0;
    int transform_overflow_depth = 0;
    Mat2x3 current_transform = Mat2x3::identity();

    bool all_known = true;
    for (std::size_t i = 0; i < list.ops.size(); ++i) {
        const auto& op = list.ops[i];
        Rect op_bounds{};
        if (transform_overflow_depth == 0 &&
            replay_visual_bounds_for_op(list, i, current_transform,
                                        op_bounds)) {
            bounds[i] = op_bounds;
            known[i] = 1;
        } else if (op.kind != PaintOpKind::PopTransform &&
                   op.kind != PaintOpKind::PopClip &&
                   op.kind != PaintOpKind::PushClip &&
                   op.kind != PaintOpKind::PopAlpha) {
            if (op.kind == PaintOpKind::DrawText ||
                op.kind == PaintOpKind::DrawTextBox ||
                op.kind == PaintOpKind::PushAlpha ||
                transform_overflow_depth > 0) {
                all_known = false;
            }
        }

        if (op.kind == PaintOpKind::PushTransform) {
            const auto next_transform =
                current_transform.then(replay_transform_from_op(op));
            if (transform_overflow_depth > 0 ||
                transform_depth >= static_cast<int>(transform_stack.size())) {
                ++transform_overflow_depth;
            } else {
                transform_stack[static_cast<std::size_t>(transform_depth)] =
                    current_transform;
                ++transform_depth;
                current_transform = next_transform;
            }
        } else if (op.kind == PaintOpKind::PopTransform) {
            if (transform_overflow_depth > 0) {
                --transform_overflow_depth;
            } else if (transform_depth > 0) {
                --transform_depth;
                current_transform =
                    transform_stack[static_cast<std::size_t>(transform_depth)];
            }
        }
    }
    return all_known;
}

inline DisplayListDiffBounds display_list_diff_bounds(const DisplayList& old_list,
                                                      const DisplayList& new_list) {
    DisplayListDiffBounds result{};
    std::vector<Rect> old_bounds;
    std::vector<Rect> new_bounds;
    std::vector<std::uint8_t> old_known;
    std::vector<std::uint8_t> new_known;
    replay_compute_visual_bounds(old_list, old_bounds, old_known);
    replay_compute_visual_bounds(new_list, new_bounds, new_known);

    const std::size_t shared =
        std::min(old_list.ops.size(), new_list.ops.size());
    for (std::size_t i = 0; i < shared; ++i) {
        if (replay_paint_ops_equal(old_list, old_list.ops[i],
                                   new_list, new_list.ops[i])) {
            continue;
        }
        ++result.changed_ops;
        if (!old_known[i] || !new_known[i]) {
            if (result.first_changed_kind == 0xFF) {
                result.first_changed_kind =
                    static_cast<std::uint8_t>(old_list.ops[i].kind);
            }
            result.known = false;
            return result;
        }
        if (result.first_changed_kind == 0xFF) {
            result.first_changed_kind =
                static_cast<std::uint8_t>(old_list.ops[i].kind);
            result.first_old_bounds = old_bounds[i];
            result.first_new_bounds = new_bounds[i];
        }
        Rect resized_solid_fill_bounds{};
        if (replay_resized_solid_fill_diff_bounds(
                old_list.ops[i], new_list.ops[i],
                old_bounds[i], new_bounds[i],
                resized_solid_fill_bounds)) {
            if (static_cast<std::uint64_t>(resized_solid_fill_bounds.w) *
                    static_cast<std::uint64_t>(resized_solid_fill_bounds.h) >
                static_cast<std::uint64_t>(result.largest_changed_bounds.w) *
                    static_cast<std::uint64_t>(result.largest_changed_bounds.h)) {
                result.largest_changed_kind =
                    static_cast<std::uint8_t>(old_list.ops[i].kind);
                result.largest_changed_bounds = resized_solid_fill_bounds;
            }
            result.bounds =
                replay_union_rect(result.bounds, resized_solid_fill_bounds);
        } else {
            const Rect changed_bounds =
                replay_union_rect(old_bounds[i], new_bounds[i]);
            if (static_cast<std::uint64_t>(changed_bounds.w) *
                    static_cast<std::uint64_t>(changed_bounds.h) >
                static_cast<std::uint64_t>(result.largest_changed_bounds.w) *
                    static_cast<std::uint64_t>(result.largest_changed_bounds.h)) {
                result.largest_changed_kind =
                    static_cast<std::uint8_t>(old_list.ops[i].kind);
                result.largest_changed_bounds = changed_bounds;
            }
            result.bounds = replay_union_rect(result.bounds, old_bounds[i]);
            result.bounds = replay_union_rect(result.bounds, new_bounds[i]);
        }
    }

    for (std::size_t i = shared; i < old_list.ops.size(); ++i) {
        if (!old_known[i]) {
            result.known = false;
            return result;
        }
        result.bounds = replay_union_rect(result.bounds, old_bounds[i]);
    }
    for (std::size_t i = shared; i < new_list.ops.size(); ++i) {
        if (!new_known[i]) {
            result.known = false;
            return result;
        }
        result.bounds = replay_union_rect(result.bounds, new_bounds[i]);
    }

    return result;
}

inline ReplayClipStats replay_clipped(const DisplayList& list,
                                      Painter& target,
                                      const Rect& clip) {
    ReplayClipStats stats{};
    std::array<Mat2x3, 64> transform_stack{};
    int transform_depth = 0;
    int transform_overflow_depth = 0;
    Mat2x3 current_transform = Mat2x3::identity();
    int skip_clip_depth = 0;
    for (std::size_t i = 0; i < list.ops.size(); ) {
        const auto& op = list.ops[i];
        if (skip_clip_depth > 0) {
            if (op.kind == PaintOpKind::PushClip) ++skip_clip_depth;
            if (op.kind == PaintOpKind::PopClip) --skip_clip_depth;
            ++stats.culled;
            ++i;
            continue;
        }

        if (op.kind == PaintOpKind::PushTransform) {
            const auto next_transform =
                current_transform.then(replay_transform_from_op(op));
            std::size_t matching_pop = list.ops.size();
            bool used_prepared_bounds = false;
            if (transform_overflow_depth == 0) {
                if (const auto* range = replay_transform_range_at(list, i)) {
                    matching_pop = range->pop_index;
                    if (range->bounds_known) {
                        used_prepared_bounds = true;
                        const Rect subtree_bounds =
                            replay_transform_bounds(range->bounds,
                                                    current_transform);
                        if (!replay_rect_intersects(subtree_bounds, clip)) {
                            stats.culled += static_cast<std::uint32_t>(
                                matching_pop - i + 1);
                            i = matching_pop + 1;
                            continue;
                        }
                    }
                } else {
                    matching_pop = replay_matching_transform_pop(list, i);
                }
                if (!used_prepared_bounds && matching_pop < list.ops.size()) {
                    Rect subtree_bounds{};
                    if (replay_range_bounds(list, i + 1, matching_pop,
                                            next_transform, subtree_bounds) &&
                        !replay_rect_intersects(subtree_bounds, clip)) {
                        stats.culled += static_cast<std::uint32_t>(
                            matching_pop - i + 1);
                        i = matching_pop + 1;
                        continue;
                    }
                }
            }

            if (transform_overflow_depth > 0 ||
                transform_depth >= static_cast<int>(transform_stack.size())) {
                ++transform_overflow_depth;
            } else {
                transform_stack[static_cast<std::size_t>(transform_depth)] =
                    current_transform;
                ++transform_depth;
                current_transform = next_transform;
            }
            replay_op(list, op, target);
            ++stats.emitted;
            ++i;
            continue;
        }
        if (op.kind == PaintOpKind::PopTransform) {
            if (transform_overflow_depth > 0) {
                --transform_overflow_depth;
            } else if (transform_depth > 0) {
                --transform_depth;
                current_transform =
                    transform_stack[static_cast<std::size_t>(transform_depth)];
            }
            replay_op(list, op, target);
            ++stats.emitted;
            ++i;
            continue;
        }

        Rect bounds{};
        if (transform_overflow_depth == 0 &&
            replay_op_bounds(list, i, bounds)) {
            bounds = replay_transform_bounds(bounds, current_transform);
            if (!replay_rect_intersects(bounds, clip)) {
                if (op.kind == PaintOpKind::PushClip) {
                    if (const auto* range = replay_clip_range_at(list, i)) {
                        const std::size_t matching_pop = range->pop_index;
                        stats.culled += static_cast<std::uint32_t>(
                            matching_pop - i + 1);
                        i = matching_pop + 1;
                        continue;
                    }
                    skip_clip_depth = 1;
                }
                ++stats.culled;
                ++i;
                continue;
            }
        }

        replay_op(list, op, target);
        ++stats.emitted;
        ++i;
    }
    return stats;
}

/// Replay a recorded DisplayList through a Painter. Used at rasterize
/// time to actually emit nvg* calls. begin_frame / end_frame on the
/// target Painter are the caller's responsibility.
inline void replay(const DisplayList& list, Painter& target) {
    for (const auto& op : list.ops) {
        replay_op(list, op, target);
    }
}

}  // namespace affineui::detail
