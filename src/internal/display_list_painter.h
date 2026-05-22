#pragma once

#include "affineui/painter.h"
#include "internal/display_list.h"

#include <algorithm>
#include <cstdint>

namespace affineui::detail {

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
                                   float br = 0, float bl = 0) override {
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
        g.pad_       = 0;
        g.stop0_rgba = pack(stop0);
        g.stop1_rgba = pack(stop1);
        list_.ops.push_back(op);
    }

    std::uint32_t resolve_font(std::string_view family, int size_px,
                               int weight, bool italic) override {
        return font_resolver_
                 ? font_resolver_->resolve_font(family, size_px, weight, italic)
                 : 0u;
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
        PaintOp op{};
        op.kind = PaintOpKind::DrawText;
        op.p.draw_text.font_handle = font;
        op.p.draw_text.x = static_cast<std::int16_t>(pos.x);
        op.p.draw_text.y = static_cast<std::int16_t>(pos.y);
        op.p.draw_text.rgba = pack(c);
        op.p.draw_text.text_offset = off;
        op.p.draw_text.text_len    = len;
        list_.ops.push_back(op);
    }

    Size measure_text_box(std::uint32_t font, std::string_view text, float max_w,
                          float line_height_mult = 1.0f,
                          float letter_spacing_px = 0.0f) override {
        return font_resolver_
                 ? font_resolver_->measure_text_box(font, text, max_w, line_height_mult,
                                                    letter_spacing_px)
                 : Size{};
    }

    void draw_text_box(std::uint32_t font, const Point& pos,
                       std::string_view text, Color c, float max_w,
                       float line_height_mult = 1.0f,
                       float letter_spacing_px = 0.0f) override {
        const auto [off, len] = list_.intern_text(text);
        PaintOp op{};
        op.kind = PaintOpKind::DrawTextBox;
        op.p.draw_text_box.font_handle = font;
        op.p.draw_text_box.x = static_cast<std::int16_t>(pos.x);
        op.p.draw_text_box.y = static_cast<std::int16_t>(pos.y);
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
        list_.ops.push_back(op);
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

private:
    static std::uint32_t pack(Color c) {
        return (std::uint32_t(c.r) << 24)
             | (std::uint32_t(c.g) << 16)
             | (std::uint32_t(c.b) <<  8)
             |  std::uint32_t(c.a);
    }

    Painter*    font_resolver_;
    DisplayList list_;
};

/// Replay a recorded DisplayList through a Painter. Used at rasterize
/// time to actually emit nvg* calls. begin_frame / end_frame on the
/// target Painter are the caller's responsibility — replay() only
/// emits content ops.
inline void replay(const DisplayList& list, Painter& target) {
    static const auto unpack = [](std::uint32_t v) {
        return Color{
            static_cast<std::uint8_t>((v >> 24) & 0xFF),
            static_cast<std::uint8_t>((v >> 16) & 0xFF),
            static_cast<std::uint8_t>((v >>  8) & 0xFF),
            static_cast<std::uint8_t>( v        & 0xFF),
        };
    };
    for (const auto& op : list.ops) {
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
                    static_cast<float>(g.br), static_cast<float>(g.bl));
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
                target.draw_text_box(t.font_handle, Point{t.x, t.y},
                                     list.text_at(t.text_offset, t.text_len),
                                     unpack(t.rgba),
                                     static_cast<float>(t.max_width),
                                     static_cast<float>(t.line_height_x100) / 100.0f,
                                     static_cast<float>(t.letter_spacing_x100) / 100.0f);
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
        }
    }
}

}  // namespace affineui::detail
