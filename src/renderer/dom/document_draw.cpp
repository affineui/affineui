// document_draw.cpp — part of the AffineUI HTML5 renderer's document implementation.
//
// Split out of the former monolithic document.cpp. Shared document-internal
// types (Block, detail::DocumentImpl, the CSS selector/rule/@media/keyframe
// side tables) live in internal/document_impl.h; cross-file helpers are
// declared there in namespace affineui::detail::doc.

#include "affineui/document.h"

#include "affineui/memory.h"
#include "affineui/painter.h"
#include "affineui/themes.h"
#include "affineui/view.h"
#include "framework/imm/imm_runtime.h"
#include "renderer/style/animated_style.h"
#include "renderer/style/computed_style.h"
#include "core/diag.h"
#include "renderer/dom/document_impl.h"
#include "renderer/style/element_id.h"
#include "core/embed_log.h"
#include "renderer/style/style_resolver.h"
#include "renderer/style/style_store.h"
#include "renderer/layout/yoga_adapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if !defined(AFFINEUI_STUB_BUILD)
#    include <lexbor/css/css.h>
#    include <lexbor/css/declaration.h>
#    include <lexbor/dom/dom.h>
#    include <lexbor/dom/interfaces/attr.h>
#    include <lexbor/html/html.h>
#endif

namespace affineui {

namespace {

#if !defined(AFFINEUI_STUB_BUILD)
// Append a circular arc as ≤90° cubic segments onto a kPath command
// stream (caller seeds the leading kPathMove). Y-down coordinates,
// angles in degrees, increasing = clockwise on screen — the same
// convention as decius_knob_ring_point. Negative sweeps work (bipolar
// knobs left of center).
void append_arc_cubics(std::vector<float>& cmds, float cx, float cy,
                       float r, double a0_deg, double a1_deg) {
    constexpr double pi = 3.14159265358979323846;
    const double total = (a1_deg - a0_deg) * pi / 180.0;
    if (total == 0.0) return;
    const int segs = std::max(
        1, static_cast<int>(std::ceil(std::abs(total) / (pi * 0.5))));
    double phi = a0_deg * pi / 180.0;
    const double step = total / segs;
    for (int s = 0; s < segs; ++s) {
        const double p0 = phi;
        const double p1 = phi + step;
        const double k = 4.0 / 3.0 * std::tan((p1 - p0) * 0.25) *
                         static_cast<double>(r);
        const float x0 = cx + r * static_cast<float>(std::cos(p0));
        const float y0 = cy + r * static_cast<float>(std::sin(p0));
        const float x3 = cx + r * static_cast<float>(std::cos(p1));
        const float y3 = cy + r * static_cast<float>(std::sin(p1));
        const float c1x = x0 - static_cast<float>(std::sin(p0) * k);
        const float c1y = y0 + static_cast<float>(std::cos(p0) * k);
        const float c2x = x3 + static_cast<float>(std::sin(p1) * k);
        const float c2y = y3 - static_cast<float>(std::cos(p1) * k);
        cmds.insert(cmds.end(),
                    {kPathCubic, c1x, c1y, c2x, c2y, x3, y3});
        phi = p1;
    }
}
#endif  // !AFFINEUI_STUB_BUILD

}  // namespace

void Document::draw(Painter& painter) {
    // Document::draw paints through *any* Painter â€” could be the real
    // NanoVGPainter, could be a DisplayListBuilder that records into
    // a DisplayList. The App layer decides which.
    //
    // This is the "paint" stage of the five-stage pipeline. It walks
    // the box tree, fetches per-element ResolvedStyle from the
    // StyleStore, and emits Painter calls. No GL calls happen here
    // directly â€” that's the rasterize stage's job.
    //
    // Scroll: per-block, sum ancestor scroll_y to get the effective
    // draw position. If any ancestor is a scrollable container, push
    // its bounds as the clip rect for the duration of this block's
    // draws so overflowing children stay inside the container.

    // Body background fills the page. <body> is the implicit root
    // and isn't in the block list (collect_blocks starts walking its
    // children), so its bg needs an explicit pre-pass. The clear
    // color is the window's, not the page's â€” without this, body's
// bg-color silently does nothing.
#if !defined(AFFINEUI_STUB_BUILD)
    detail::ensure_font_faces_registered(*impl_, painter);

    if (impl_->doc) {
        auto* body = lxb_html_document_body_element(impl_->doc);
        if (body && impl_->resolver) {
            const auto rs = impl_->resolver->resolve(
                lxb_dom_interface_element(lxb_dom_interface_node(body)),
                impl_->root_style);
            if ((rs.animated.background_rgba & 0xFFu) != 0) {
                painter.fill_rect(
                    Rect{0, 0, impl_->content_size.width,
                         impl_->content_size.height},
                    detail::unpack_rgba(rs.animated.background_rgba));
            }
        }
    }
#endif

    auto& child_counts = impl_->draw_child_counts;
    auto& first_child_indices = impl_->draw_first_child_indices;
    child_counts.assign(impl_->blocks.size(), 0);
    first_child_indices.assign(impl_->blocks.size(), -1);
    for (std::size_t child_idx = 0; child_idx < impl_->blocks.size(); ++child_idx) {
        const auto& b = impl_->blocks[child_idx];
        if (b.parent_idx >= 0 &&
            static_cast<std::size_t>(b.parent_idx) < child_counts.size()) {
            const auto parent_idx = static_cast<std::size_t>(b.parent_idx);
            if (first_child_indices[parent_idx] < 0) {
                first_child_indices[parent_idx] = static_cast<int>(child_idx);
            }
            ++child_counts[parent_idx];
        }
    }

    auto& list_ordinals = impl_->draw_list_ordinals;
    auto& list_counts_by_parent = impl_->draw_list_counts_by_parent;
    list_ordinals.assign(impl_->blocks.size(), 0);
    list_counts_by_parent.assign(impl_->blocks.size() + 1, 0);
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        const auto& b = impl_->blocks[i];
        const auto& cs = impl_->style_store.computed(b.id);
        if (cs.display == detail::ComputedStyle::Display::ListItem) {
            const auto parent_slot =
                static_cast<std::size_t>(std::max(-1, b.parent_idx) + 1);
            list_ordinals[i] = ++list_counts_by_parent[parent_slot];
        }
    }

    auto& paint_order = impl_->draw_paint_order;
    paint_order.resize(impl_->blocks.size());
    std::iota(paint_order.begin(), paint_order.end(), 0);
#if !defined(AFFINEUI_STUB_BUILD)
    std::stable_sort(paint_order.begin(), paint_order.end(),
        [&](int a, int b) {
            const int za = detail::effective_z_index(*impl_, a);
            const int zb = detail::effective_z_index(*impl_, b);
            if (za != zb) return za < zb;
            return a < b;
        });
#endif

    // Two paint passes per STACKING CONTEXT (CSS 2.1 Appendix E): every
    // block's background/border in a context paints before ANY of that
    // context's text. Glyph ink that overhangs its box (descenders in
    // line-height:1 menu rows) can then never be overpainted by the next
    // sibling's background — matching browser painting order.
    //
    // The grouping must be per stacking ROOT, not per z VALUE: two floating
    // panels both at z-index:60 are separate atomic units — the earlier
    // panel's TEXT must paint before the later panel's BACKGROUND, or a
    // covered palette's labels bleed through the panel above it. Blocks are
    // appended in DFS order, so a root's subtree is contiguous within its z
    // group and the group boundary is a simple root change.
    //
    // KNOWN LIMIT: effective_z_index is max-along-the-ancestor-chain, so a
    // HIGHER-z descendant (z:100 popover inside a z:60 float) sorts into its
    // own z band and escapes its parent's atomic group — it paints above a
    // LATER sibling float, where CSS keeps the whole subtree below it. Full
    // fidelity needs hierarchical (lexicographic z-path) paint ordering.
    // Today that divergence only shows for open popovers inside floats,
    // where painting above neighbouring panels is the desirable UX anyway.
    //
    // stacking_roots[i]: the NEAREST ancestor-or-self carrying a positive
    // z-index (the float section, a menu, a popover), or -1 for base flow.
    // NEAREST, not outermost: the View's float LAYER div also carries a
    // z-index, and an outermost rule made it the shared root of every
    // floating panel — collapsing them back into one paint group. CSS
    // semantics: each z-indexed positioned element is its own stacking
    // context, atomic WITHIN its parent context. parent_idx < i (DFS append
    // order), so one forward pass settles it.
    std::vector<int> stacking_roots(impl_->blocks.size(), -1);
#if !defined(AFFINEUI_STUB_BUILD)
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        const auto& blk = impl_->blocks[i];
        const int parent_root =
            blk.parent_idx >= 0
                ? stacking_roots[static_cast<std::size_t>(blk.parent_idx)]
                : -1;
        stacking_roots[i] =
            impl_->style_store.computed(blk.id).z_index_low > 0
                ? static_cast<int>(i)
                : parent_root;
    }
#endif
    // Overlay is a third per-context phase: a pane's scrollbar thumb paints
    // on top of its OWN context's content but underneath later/higher
    // contexts. (The old global draw-last scrollbar pass painted every
    // pane's thumb over overlapping floating panels — wrong z.) Only blocks
    // that actually have a scrollbar get an Overlay entry, so the extra
    // phase costs nothing for everything else.
    enum class BlockPaintPhase : std::uint8_t { Boxes, Text, Overlay };
    std::vector<std::pair<int, BlockPaintPhase>> phased_order;
    phased_order.reserve(paint_order.size() * 2);
    {
        std::size_t group_begin = 0;
        while (group_begin < paint_order.size()) {
            std::size_t group_end = group_begin;
#if !defined(AFFINEUI_STUB_BUILD)
            const int group_z =
                detail::effective_z_index(*impl_, paint_order[group_begin]);
            const int group_root = stacking_roots[static_cast<std::size_t>(
                paint_order[group_begin])];
            while (group_end < paint_order.size() &&
                   detail::effective_z_index(*impl_, paint_order[group_end]) ==
                       group_z &&
                   stacking_roots[static_cast<std::size_t>(
                       paint_order[group_end])] == group_root) {
                ++group_end;
            }
#else
            group_end = paint_order.size();
#endif
            for (std::size_t k = group_begin; k < group_end; ++k) {
                phased_order.emplace_back(paint_order[k],
                                          BlockPaintPhase::Boxes);
            }
            for (std::size_t k = group_begin; k < group_end; ++k) {
                phased_order.emplace_back(paint_order[k],
                                          BlockPaintPhase::Text);
            }
#if !defined(AFFINEUI_STUB_BUILD)
            for (std::size_t k = group_begin; k < group_end; ++k) {
                ScrollbarGeometry sb{};
                if (detail::vertical_scrollbar_geometry(*impl_,
                                                        paint_order[k], sb)) {
                    phased_order.emplace_back(paint_order[k],
                                              BlockPaintPhase::Overlay);
                }
            }
#endif
            group_begin = group_end;
        }
    }

    // display:none subtree suppression. Hidden subtrees keep their Blocks
    // (collection retains them so reveals are pure restyles), and unlike
    // `visibility`, display is NOT inherited — a hidden menu's rows still
    // compute display:flex. Propagate none-ness down the tree so no
    // descendant box or glyph of a hidden subtree ever paints (Yoga zeroes
    // their bounds, but text ink is drawn from the baseline and would
    // otherwise smear at the origin). Blocks are appended in DFS order, so
    // parent_idx < i and one forward pass settles the whole vector.
    std::vector<char> in_none_subtree(impl_->blocks.size(), 0);
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        const auto& blk = impl_->blocks[i];
        const bool parent_none =
            blk.parent_idx >= 0 &&
            in_none_subtree[static_cast<std::size_t>(blk.parent_idx)] != 0;
        in_none_subtree[i] =
            parent_none ||
            impl_->style_store.computed(blk.id).display ==
                detail::ComputedStyle::Display::None;
    }

    for (const auto& [paint_idx, phase] : phased_order) {
        const std::size_t i = static_cast<std::size_t>(paint_idx);
        const auto& b  = impl_->blocks[i];
        if (in_none_subtree[i]) continue;
        // Synthetic line-boxes are layout-only. They don't carry
        // any visual style â€” skip the whole draw stanza.
        if (b.synthetic) continue;
        const auto& cs = impl_->style_store.computed(b.id);
#if !defined(AFFINEUI_STUB_BUILD)
        auto current_an = b.base_animated;
        float anim_t = 0.0f;
        bool anim_applies = false;
        const double elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - b.animation_epoch).count();
        (void) animation_progress_at(b.animation, elapsed_s, anim_t,
                                     anim_applies);
        if (anim_applies) current_an = sample_keyframes(*impl_, b, anim_t);
        impl_->style_store.animated(b.id) = current_an;
#endif
        const auto& an = impl_->style_store.animated(b.id);

        // CSS `display:none` removes the element from layout AND from
        // paint â€” nothing is drawn, no space is reserved.
        if (cs.display == detail::ComputedStyle::Display::None) {
            continue;
        }

        // CSS `visibility:hidden` (or collapse): the box keeps its
        // layout space but paints nothing â€” neither this element nor
        // its descendants (unless a descendant re-asserts
        // visibility:visible, in which case that block's own
        // cs.visibility will be Visible and it will paint normally).
        using V = detail::ComputedStyle::Visibility;
        if (cs.visibility == V::Hidden || cs.visibility == V::Collapse) {
            continue;
        }

        const int dy = detail::scroll_offset_y_for(impl_->blocks, impl_->style_store,
                                           static_cast<int>(i));
        const Rect eff{b.bounds.x, b.bounds.y - dy, b.bounds.w, b.bounds.h};

#if !defined(AFFINEUI_STUB_BUILD)
        const Mat2x3 paint_transform =
            effective_transform_for(*impl_, static_cast<int>(i));
        const bool has_transform = !paint_transform.is_identity();
        if (has_transform) painter.push_transform(paint_transform);
#else
        const bool has_transform = false;
#endif

        // Clip to the INTERSECTION of every ancestor whose overflow clips
        // children (overflow: hidden | clip | scroll | auto) — an ellipsis
        // label's own clip must not exempt it from its scroll pane's. CSS
        // clips descendant paint to the padding box, not the border box, so
        // each ancestor's own border remains visible above clipped children.
        Rect active_clip_rect{};
        const bool clipped =
            detail::clip_rect_for_block(*impl_, static_cast<int>(i),
                                        active_clip_rect);
        if (clipped) {
            painter.push_clip(active_clip_rect);
        }

        static const bool paint_trace =
            std::getenv("AFFINEUI_PAINT_TRACE") != nullptr;
        if (paint_trace && detail::block_has_class(b, "dcs-menu__sub")) {
            std::fprintf(stderr,
                         "[paint %zu] sub eff=%d,%d %dx%d clipped=%d "
                         "clip=%d,%d %dx%d bg=%08x border=%08x opacity=%.2f "
                         "anim=%d\n",
                         i, eff.x, eff.y, eff.w, eff.h, clipped ? 1 : 0,
                         active_clip_rect.x, active_clip_rect.y,
                         active_clip_rect.w, active_clip_rect.h,
                         an.background_rgba, an.border_rgba, an.opacity,
                         b.animation.active ? 1 : 0);
        }

        // CSS `opacity` â€” composite this element's entire subtree at a
        // group alpha. NanoVG's nvgGlobalAlpha multiplies onto whatever
        // alpha is currently set, so save/restore gives clean isolation.
        float effective_opacity = an.opacity;
        for (int ai = b.parent_idx; ai >= 0; ) {
            const auto& ab = impl_->blocks[static_cast<std::size_t>(ai)];
            effective_opacity *= impl_->style_store.animated(ab.id).opacity;
            ai = ab.parent_idx;
        }
        const bool has_opacity = (effective_opacity < 1.0f - 1e-5f);
        if (has_opacity) painter.push_alpha(effective_opacity);

        const float r_tl = detail::resolve_border_radius_px(
            cs.border_radius_top_left, eff.w, eff.h);
        const float r_tr = detail::resolve_border_radius_px(
            cs.border_radius_top_right, eff.w, eff.h);
        const float r_br = detail::resolve_border_radius_px(
            cs.border_radius_bot_right, eff.w, eff.h);
        const float r_bl = detail::resolve_border_radius_px(
            cs.border_radius_bot_left, eff.w, eff.h);
        const bool any_radius  = (r_tl > 0 || r_tr > 0 || r_br > 0 || r_bl > 0);
        const bool uniform_r   = (r_tl == r_tr && r_tr == r_br && r_br == r_bl);
        float bg_r_tl = r_tl;
        float bg_r_tr = r_tr;
        float bg_r_br = r_br;
        float bg_r_bl = r_bl;
        // Rounded-clip emulation: scissors are rectangular, so a child whose
        // box touches a rounded clip ancestor's edge inherits that corner's
        // radius on its background. The radius donor is the NEAREST clipping
        // ancestor (the intersection rect above has no radii of its own).
        const int clip_idx =
            clipped ? detail::nearest_clip_ancestor_for_block(
                          *impl_, static_cast<int>(i))
                    : -1;
        if (clip_idx >= 0) {
            const auto& cb = impl_->blocks[static_cast<std::size_t>(clip_idx)];
            const auto& ccs = impl_->style_store.computed(cb.id);
            const float clip_tl = std::max(0.0f, detail::resolve_border_radius_px(
                ccs.border_radius_top_left, cb.bounds.w, cb.bounds.h)
                - static_cast<float>(std::max(ccs.used_border_left(), ccs.used_border_top())));
            const float clip_tr = std::max(0.0f, detail::resolve_border_radius_px(
                ccs.border_radius_top_right, cb.bounds.w, cb.bounds.h)
                - static_cast<float>(std::max(ccs.used_border_right(), ccs.used_border_top())));
            const float clip_br = std::max(0.0f, detail::resolve_border_radius_px(
                ccs.border_radius_bot_right, cb.bounds.w, cb.bounds.h)
                - static_cast<float>(std::max(ccs.used_border_right(), ccs.used_border_bottom())));
            const float clip_bl = std::max(0.0f, detail::resolve_border_radius_px(
                ccs.border_radius_bot_left, cb.bounds.w, cb.bounds.h)
                - static_cast<float>(std::max(ccs.used_border_left(), ccs.used_border_bottom())));
            constexpr float eps = 0.5f;
            const float eff_r = static_cast<float>(eff.x + eff.w);
            const float eff_b = static_cast<float>(eff.y + eff.h);
            const float clip_r = static_cast<float>(active_clip_rect.x + active_clip_rect.w);
            const float clip_b = static_cast<float>(active_clip_rect.y + active_clip_rect.h);
            const bool touches_left = eff.x <= active_clip_rect.x + eps;
            const bool touches_top = eff.y <= active_clip_rect.y + eps;
            const bool touches_right = eff_r >= clip_r - eps;
            const bool touches_bottom = eff_b >= clip_b - eps;
            if (touches_left && touches_top) bg_r_tl = std::max(bg_r_tl, clip_tl);
            if (touches_right && touches_top) bg_r_tr = std::max(bg_r_tr, clip_tr);
            if (touches_right && touches_bottom) bg_r_br = std::max(bg_r_br, clip_br);
            if (touches_left && touches_bottom) bg_r_bl = std::max(bg_r_bl, clip_bl);
        }
        // Shared by both phases: text positioning needs the border insets.
        const int used_border_top    = cs.used_border_top();
        const int used_border_right  = cs.used_border_right();
        const int used_border_bottom = cs.used_border_bottom();
        const int used_border_left   = cs.used_border_left();
        // ── PHASE: boxes ─────────────────────────────────────────────
        // CSS painting order (2.1 Appendix E): within one stacking level,
        // ALL block backgrounds/borders paint before ANY inline content.
        // Interleaving them per block lets a sibling's background overpaint
        // a neighbour's glyph overhang — e.g. menu rows at line-height:1,
        // where the next row's hover highlight beheads descenders. The
        // guarded region below (backgrounds, shadows, gradients, images,
        // borders, focus rings) runs in the Boxes pass; text and widget
        // chrome follow in the Text pass. (Intentionally not re-indented —
        // the guard is the change, the stanzas are not.)
        if (phase == BlockPaintPhase::Boxes) {
        // Background color paints first; background images/gradients layer over it.
        const bool has_gradient =
            an.gradient_kind != detail::AnimatedStyle::GradientKind::None;
        const bool has_grid =
            ((an.background_grid_rgba & 0xFFu) != 0 &&
             an.background_grid_size_px != 0);
        const bool has_bg = (an.background_rgba & 0xFFu) != 0;

        // Resolve effective per-side border color. Explicit transparent is a
        // valid override, so use AnimatedStyle's side bitmask rather than the
        // color value itself as the "is set" marker.
        using BS = detail::ComputedStyle::BorderStyle;
        auto side_rgba = [&](std::uint32_t per_side_rgba,
                             std::uint8_t side_bit,
                             std::uint32_t fallback_rgba) -> std::uint32_t {
            return (an.border_color_set & side_bit) ? per_side_rgba
                                                    : fallback_rgba;
        };

        const std::uint32_t c_top    = side_rgba(an.border_top_rgba,
            detail::AnimatedStyle::BorderTopColorSet, an.border_rgba);
        const std::uint32_t c_right  = side_rgba(an.border_right_rgba,
            detail::AnimatedStyle::BorderRightColorSet, an.border_rgba);
        const std::uint32_t c_bottom = side_rgba(an.border_bottom_rgba,
            detail::AnimatedStyle::BorderBottomColorSet, an.border_rgba);
        const std::uint32_t c_left   = side_rgba(an.border_left_rgba,
            detail::AnimatedStyle::BorderLeftColorSet, an.border_rgba);

        // A side is visible if that side's border-style is non-None, the
        // side has a non-zero used width, and its effective color is opaque.
        auto side_visible = [&](int used_w, std::uint32_t rgba) -> bool {
            return used_w > 0 && (rgba & 0xFFu) != 0;
        };

        const bool vis_top    = side_visible(used_border_top,    c_top);
        const bool vis_right  = side_visible(used_border_right,  c_right);
        const bool vis_bottom = side_visible(used_border_bottom, c_bottom);
        const bool vis_left   = side_visible(used_border_left,   c_left);

        const bool has_border = vis_top || vis_right || vis_bottom || vis_left;

        // True when all visible sides share identical color and width, enabling
        // the fast-path stroke_rect / stroke_rounded_rect for the Solid style.
        const bool uniform_border =
            has_border
            && cs.border_style == BS::Solid
            && c_top == c_right && c_right == c_bottom && c_bottom == c_left
            && used_border_top == used_border_right &&
               used_border_right == used_border_bottom &&
               used_border_bottom == used_border_left;
        const bool has_shadow = (an.shadow_rgba & 0xFFu) != 0
            && (an.shadow_blur != 0 || an.shadow_spread != 0 ||
                an.shadow_offset_x != 0 || an.shadow_offset_y != 0);

        // Single radius for the shadow primitive (CSS box-shadow follows
        // the largest corner; per-corner shadow radii are not a thing).
        const float shadow_radius = any_radius
            ? std::max({r_tl, r_tr, r_br, r_bl}) : 0.0f;
        auto shadow_visible = [](const detail::BoxShadowLayer& layer) {
            return (layer.rgba & 0xFFu) != 0 &&
                (layer.blur != 0 || layer.spread != 0 ||
                 layer.offset_x != 0 || layer.offset_y != 0);
        };
        auto adjust_inset_shadow_geometry = [&](Rect& shadow_rect,
                                                float& layer_radius,
                                                bool inset) {
            if (inset) {
                const int border_l = std::max(0, used_border_left);
                const int border_t = std::max(0, used_border_top);
                const int border_r = std::max(0, used_border_right);
                const int border_b = std::max(0, used_border_bottom);
                shadow_rect = Rect{
                    eff.x + border_l,
                    eff.y + border_t,
                    std::max(0, eff.w - border_l - border_r),
                    std::max(0, eff.h - border_t - border_b),
                };
                if (shadow_rect.w <= 0 || shadow_rect.h <= 0) return;
                const int border_for_radius =
                    std::max({border_l, border_t, border_r, border_b});
                layer_radius = std::max(
                    0.0f, shadow_radius - static_cast<float>(border_for_radius));
            }
        };
        auto paint_shadow_layer = [&](const detail::BoxShadowLayer& layer,
                                      bool inset) {
            Rect shadow_rect = eff;
            float layer_radius = shadow_radius;
            adjust_inset_shadow_geometry(shadow_rect, layer_radius, inset);
            if (shadow_rect.w <= 0 || shadow_rect.h <= 0) return;
            painter.fill_box_shadow(
                shadow_rect, layer_radius, detail::unpack_rgba(layer.rgba),
                static_cast<float>(layer.offset_x),
                static_cast<float>(layer.offset_y),
                static_cast<float>(layer.blur),
                static_cast<float>(layer.spread),
                inset);
        };
        // Outset shadow paints BEHIND the background (CSS painting order).
        if (b.box_shadows) {
            for (auto it = b.box_shadows->rbegin();
                 it != b.box_shadows->rend(); ++it) {
                if (!it->inset && shadow_visible(*it)) {
                    paint_shadow_layer(*it, false);
                }
            }
        } else if (has_shadow && !an.shadow_inset) {
            painter.fill_box_shadow(
                eff, shadow_radius, detail::unpack_rgba(an.shadow_rgba),
                static_cast<float>(an.shadow_offset_x),
                static_cast<float>(an.shadow_offset_y),
                static_cast<float>(an.shadow_blur),
                static_cast<float>(an.shadow_spread),
                /*inset=*/false);
        }

        // CSS `background-clip`: all background layers (color, gradients,
        // grid) paint into the border box by default, or inset to the
        // padding/content box. The transparent-border + padding-box pattern
        // draws an inset highlight inside a full-bleed hit box (menu rows).
        Rect bg_rect = eff;
        float clip_r_tl = bg_r_tl;
        float clip_r_tr = bg_r_tr;
        float clip_r_br = bg_r_br;
        float clip_r_bl = bg_r_bl;
        using BgClip = detail::ComputedStyle::BackgroundClip;
        if (cs.background_clip() != BgClip::BorderBox) {
            int inset_l = used_border_left;
            int inset_t = used_border_top;
            int inset_r = used_border_right;
            int inset_b = used_border_bottom;
            if (cs.background_clip() == BgClip::ContentBox) {
                inset_l += cs.padding_left;
                inset_t += cs.padding_top;
                inset_r += cs.padding_right;
                inset_b += cs.padding_bottom;
            }
            bg_rect = Rect{eff.x + inset_l, eff.y + inset_t,
                           std::max(0, eff.w - inset_l - inset_r),
                           std::max(0, eff.h - inset_t - inset_b)};
            clip_r_tl = std::max(0.0f, clip_r_tl - std::max(inset_l, inset_t));
            clip_r_tr = std::max(0.0f, clip_r_tr - std::max(inset_r, inset_t));
            clip_r_br = std::max(0.0f, clip_r_br - std::max(inset_r, inset_b));
            clip_r_bl = std::max(0.0f, clip_r_bl - std::max(inset_l, inset_b));
        }
        const bool clip_any_radius =
            (clip_r_tl > 0 || clip_r_tr > 0 || clip_r_br > 0 || clip_r_bl > 0);
        const bool clip_uniform_r =
            (clip_r_tl == clip_r_tr && clip_r_tr == clip_r_br &&
             clip_r_br == clip_r_bl);

        if (has_bg && bg_rect.w > 0 && bg_rect.h > 0) {
            const Color bg = detail::unpack_rgba(an.background_rgba);
            if      (!clip_any_radius)        painter.fill_rect(bg_rect, bg);
            else if (clip_uniform_r)          painter.fill_rounded_rect(bg_rect, clip_r_tl, bg);
            else                              painter.fill_rounded_rect_varying(
                                                  bg_rect, clip_r_tl, clip_r_tr,
                                                  clip_r_br, clip_r_bl, bg);
        }

        if (has_gradient && bg_rect.w > 0 && bg_rect.h > 0) {
            const Color s0 = detail::unpack_rgba(an.gradient_stop0_rgba);
            const Color s1 = detail::unpack_rgba(an.gradient_stop1_rgba);
            // N-stop (>2) ramps carry a full out-of-line stop list; 2-stop
            // and stripe fills use the compact inline stop0/stop1 fast path.
            const bool multi_stop =
                b.gradient_stops && b.gradient_stops->size() > 2 &&
                an.gradient_kind != detail::AnimatedStyle::GradientKind::LinearStripes;
            if (an.gradient_kind == detail::AnimatedStyle::GradientKind::Linear) {
                if (multi_stop) {
                    std::array<Painter::GradientStop, PathPaint::kMaxStops> gs{};
                    const std::size_t n = std::min<std::size_t>(
                        b.gradient_stops->size(), PathPaint::kMaxStops);
                    for (std::size_t i = 0; i < n; ++i) {
                        gs[i].offset = (*b.gradient_stops)[i].offset;
                        gs[i].color  = detail::unpack_rgba((*b.gradient_stops)[i].rgba);
                    }
                    painter.fill_linear_gradient_rect_n(
                        bg_rect, static_cast<float>(an.gradient_angle_deg),
                        gs.data(), n,
                        clip_r_tl, clip_r_tr, clip_r_br, clip_r_bl);
                } else {
                    painter.fill_linear_gradient_rect(
                        bg_rect, static_cast<float>(an.gradient_angle_deg),
                        s0, s1, clip_r_tl, clip_r_tr, clip_r_br, clip_r_bl);
                }
            } else if (an.gradient_kind == detail::AnimatedStyle::GradientKind::Radial) {
                if (multi_stop) {
                    std::array<Painter::GradientStop, PathPaint::kMaxStops> gs{};
                    const std::size_t n = std::min<std::size_t>(
                        b.gradient_stops->size(), PathPaint::kMaxStops);
                    for (std::size_t i = 0; i < n; ++i) {
                        gs[i].offset = (*b.gradient_stops)[i].offset;
                        gs[i].color  = detail::unpack_rgba((*b.gradient_stops)[i].rgba);
                    }
                    painter.fill_radial_gradient_rect_n(
                        bg_rect, gs.data(), n,
                        clip_r_tl, clip_r_tr, clip_r_br, clip_r_bl,
                        static_cast<float>(an.gradient_center_x_pct),
                        static_cast<float>(an.gradient_center_y_pct),
                        // 100, NOT gradient_stop1_pos_pct. That field means
                        // "where the ramp ends" and only makes sense for a
                        // TWO-stop gradient, where it scales the outer radius.
                        // In an N-stop ramp every stop carries its own offset
                        // (they are in `gs`), and stop *1* is merely the second
                        // one — 4% in `.dcs-hw--lacquer`. Feeding that in scaled
                        // the radius to 4% of its proper size and collapsed the
                        // gradient into an invisible dot. The stacked-layer call
                        // below has always passed 100 for the same reason.
                        /*stop1_pos_pct=*/100.0f);
                } else {
                    painter.fill_radial_gradient_rect(
                        bg_rect, s0, s1, clip_r_tl, clip_r_tr, clip_r_br, clip_r_bl,
                        static_cast<float>(an.gradient_center_x_pct),
                        static_cast<float>(an.gradient_center_y_pct),
                        static_cast<float>(an.gradient_stop1_pos_pct));
                }
            } else if (an.gradient_kind == detail::AnimatedStyle::GradientKind::LinearStripes) {
                painter.fill_linear_stripes_rect(
                    bg_rect, static_cast<float>(an.gradient_angle_deg),
                    s0, static_cast<float>(std::max(1, bg_rect.h)),
                    clip_r_tl, clip_r_tr, clip_r_br, clip_r_bl);
            }
        }

        if (has_grid && bg_rect.w > 0 && bg_rect.h > 0) {
            painter.fill_grid_rect(
                bg_rect, detail::unpack_rgba(an.background_grid_rgba),
                static_cast<float>(an.background_grid_size_px),
                static_cast<float>(std::max<std::uint8_t>(
                    1, an.background_grid_line_px)),
                clip_r_tl, clip_r_tr, clip_r_br, clip_r_bl);
        }

        // The rest of the CSS `background` stack — every layer ABOVE the
        // bottom one, painted back-to-front. `background_layers` is stored
        // in CSS source order (topmost first), so walk it in REVERSE: the
        // last entry is the layer immediately above the bottom gradient and
        // must go down first.
        //
        // Each layer carries its own full N-stop ramp, so a 6-stop specular
        // highlight over a 3-stop base (the Decius skeuo panels) renders as
        // authored. The predecessor of this loop collapsed the top layer to
        // its first and last stop, which turned that highlight's steep
        // falloff into a flat white wash across the whole panel.
        if (b.background_layers && bg_rect.w > 0 && bg_rect.h > 0) {
            using LK = detail::BackgroundLayer::Kind;
            std::array<Painter::GradientStop, PathPaint::kMaxStops> gs{};
            for (auto it = b.background_layers->rbegin();
                 it != b.background_layers->rend(); ++it) {
                const auto& layer = *it;
                if (layer.kind == LK::None || layer.stops.empty()) continue;

                const std::size_t n = std::min<std::size_t>(
                    layer.stops.size(), PathPaint::kMaxStops);
                for (std::size_t i = 0; i < n; ++i) {
                    gs[i].offset = layer.stops[i].offset;
                    gs[i].color  = detail::unpack_rgba(layer.stops[i].rgba);
                }

                switch (layer.kind) {
                    case LK::Linear:
                        painter.fill_linear_gradient_rect_n(
                            bg_rect, static_cast<float>(layer.angle_deg),
                            gs.data(), n,
                            clip_r_tl, clip_r_tr, clip_r_br, clip_r_bl);
                        break;
                    case LK::Radial:
                        painter.fill_radial_gradient_rect_n(
                            bg_rect, gs.data(), n,
                            clip_r_tl, clip_r_tr, clip_r_br, clip_r_bl,
                            static_cast<float>(layer.center_x_pct),
                            static_cast<float>(layer.center_y_pct),
                            /*stop1_pos_pct=*/100.0f,
                            static_cast<float>(layer.radius_x_pct),
                            static_cast<float>(layer.radius_y_pct));
                        break;
                    case LK::LinearStripes:
                        // repeating-linear-gradient: NOT painted, which is the
                        // behavior these panels had before stacked layers
                        // existed (a `layer_count == 2` guard used to drop it),
                        // and it is the look the samples are designed around —
                        // the synth modules are smooth gradients, not striped.
                        //
                        // We also cannot render it faithfully: the period lives
                        // in the stops' PIXEL offsets (`0 1px, 1px 2px` — a 2px
                        // repeat) and lexbor's descriptor stores stop positions
                        // as percentages, so it never survives the parse; and the
                        // stripe primitive paints one flat colour, not a
                        // two-colour ramp. Approximating it with the box height
                        // as a tile size clamps to 128 and bands the panel in
                        // 128px light/dark stripes over the specular highlight.
                        //
                        // The one repeating-linear-gradient in the Decius bundle
                        // is .dcs-hw--brushed's metal texture at 2% alpha —
                        // imperceptible when absent. Rendering it properly needs
                        // pixel stop offsets carried through the lexbor fork and
                        // a real repeating-ramp primitive.
                        break;
                    case LK::None:
                        break;
                }
            }
        }

        // Inset shadow paints ON TOP of the background/gradient but under
        // the border and content (CSS painting order).
        if (b.box_shadows) {
            for (auto it = b.box_shadows->rbegin();
                 it != b.box_shadows->rend(); ++it) {
                if (it->inset && shadow_visible(*it)) {
                    paint_shadow_layer(*it, true);
                }
            }
        } else if (has_shadow && an.shadow_inset) {
            Rect shadow_rect = eff;
            float layer_radius = shadow_radius;
            adjust_inset_shadow_geometry(
                shadow_rect, layer_radius, /*inset=*/true);
            painter.fill_box_shadow(
                shadow_rect, layer_radius, detail::unpack_rgba(an.shadow_rgba),
                static_cast<float>(an.shadow_offset_x),
                static_cast<float>(an.shadow_offset_y),
                static_cast<float>(an.shadow_blur),
                static_cast<float>(an.shadow_spread),
                /*inset=*/true);
        }

        if (!b.image_src.empty()) {
            const auto image = painter.load_image(b.image_src);
            const auto sz = painter.image_size(image);
            if (image != 0 && sz.width > 0 && sz.height > 0) {
                const Rect content_r{
                    eff.x + used_border_left + cs.padding_left,
                    eff.y + used_border_top + cs.padding_top,
                    eff.w - used_border_left - used_border_right
                          - cs.padding_left - cs.padding_right,
                    eff.h - used_border_top - used_border_bottom
                          - cs.padding_top - cs.padding_bottom,
                };
                if (content_r.w > 0 && content_r.h > 0) {
                    painter.draw_image(image, content_r,
                                       Rect{0, 0, sz.width, sz.height});
                }
            }
        }

#if !defined(AFFINEUI_STUB_BUILD)
        if (auto* elem = impl_->style_store.element_of(b.id)) {
            detail::paint_direct_child_svgs(*impl_, b, eff, cs, an, painter, elem);
            // Custom paint (canvas): delegate the element's border box to
            // the app handler named by data-aui-paint. Runs after any
            // inline-svg children so a handler can overlay static art.
            if (!impl_->paint_handlers.empty()) {
                if (const auto handler_name =
                        detail::attr_string(elem, "data-aui-paint");
                    !handler_name.empty()) {
                    if (auto it = impl_->paint_handlers.find(handler_name);
                        it != impl_->paint_handlers.end() && it->second) {
                        it->second(painter, eff);
                    }
                }
            }
        }
#endif

        if (has_border) {
            const bool equal_border_widths =
                used_border_top == used_border_right &&
                used_border_right == used_border_bottom &&
                used_border_bottom == used_border_left;
            const float short_side =
                static_cast<float>(std::min(eff.w, eff.h));
            const bool circular_equal_border =
                cs.border_style == BS::Solid && any_radius && uniform_r &&
                equal_border_widths && short_side > 0.0f &&
                std::abs(static_cast<float>(eff.w - eff.h)) <= 1.0f &&
                r_tl >= short_side * 0.5f - 0.5f;

            if (!uniform_border && circular_equal_border) {
                // Equal-width circular borders with per-side colors are common
                // in CSS spinners: each side owns one quadrant, with joins at
                // 45-degree diagonals. This preserves transparent sides without
                // a class-specific paint hook.
                const float thickness = static_cast<float>(used_border_top);
                const float radius = short_side * 0.5f - thickness * 0.5f;
                if (radius > 0.0f) {
                    const float cx = static_cast<float>(eff.x) +
                                     static_cast<float>(eff.w) * 0.5f;
                    const float cy = static_cast<float>(eff.y) +
                                     static_cast<float>(eff.h) * 0.5f;
                    if (vis_top) {
                        painter.stroke_arc(cx, cy, radius, -45.0f, 45.0f,
                                           detail::unpack_rgba(c_top),
                                           thickness);
                    }
                    if (vis_right) {
                        painter.stroke_arc(cx, cy, radius, 45.0f, 135.0f,
                                           detail::unpack_rgba(c_right),
                                           thickness);
                    }
                    if (vis_bottom) {
                        painter.stroke_arc(cx, cy, radius, 135.0f, 225.0f,
                                           detail::unpack_rgba(c_bottom),
                                           thickness);
                    }
                    if (vis_left) {
                        painter.stroke_arc(cx, cy, radius, 225.0f, 315.0f,
                                           detail::unpack_rgba(c_left),
                                           thickness);
                    }
                }
            } else if (uniform_border && !any_radius) {
                // CSS uniform solid border: fill the border area inside the
                // border box. A centered vector stroke puts half of a 1px
                // border outside the element and makes tiny controls mushy.
                const float thickness = static_cast<float>(used_border_top);
                painter.fill_rounded_rect_ring(
                    eff, 0.0f, thickness, detail::unpack_rgba(c_top));
            } else if (uniform_border && any_radius && uniform_r) {
                // CSS uniform solid rounded border. The filled ring matches
                // the border box / padding box geometry instead of stroking
                // the centreline of the outer rounded rect.
                const float thickness = static_cast<float>(used_border_top);
                const Color bc = detail::unpack_rgba(c_top);
                painter.fill_rounded_rect_ring(eff, r_tl, thickness, bc);
            } else if (uniform_border && any_radius) {
                const float thickness = static_cast<float>(used_border_top);
                const int   inset     = used_border_top / 2;
                const Rect  stroke_r{
                    eff.x + inset, eff.y + inset,
                    eff.w - 2 * inset, eff.h - 2 * inset,
                };
                const Color bc = detail::unpack_rgba(c_top);
                painter.stroke_rounded_rect_varying(
                    stroke_r, r_tl, r_tr, r_br, r_bl, bc, thickness);
            } else {
                // General path: draw each visible side independently.
                //
                // Geometry for per-side edge drawing (CSS box model):
                //   The border sits inside the element's border-box.
                //   Each edge is centred on the midpoint of its own border
                //   half-width from the outer border-box edge.
                //
                // Corner convention: horizontal edges own the full width
                // including their corner squares; vertical edges span only
                // between the outer horizontal edge endpoints â€” matching
                // the T-intersect rendering browsers produce for different
                // side widths/colors.
                //
                // For non-solid styles (dashed/dotted/double), border-radius
                // is ignored in this phase (same as the previous implementation
                // which always used a single stroke_rect call regardless of
                // style). Radius + non-solid border is rare in practice.

                const float ex = static_cast<float>(eff.x);
                const float ey = static_cast<float>(eff.y);
                const float ew = static_cast<float>(eff.w);
                const float eh = static_cast<float>(eff.h);

                auto fill_border_rect = [&](float x, float y, float w, float h,
                                            Color color) {
                    const int ix = static_cast<int>(std::round(x));
                    const int iy = static_cast<int>(std::round(y));
                    const int iw = static_cast<int>(std::round(w));
                    const int ih = static_cast<int>(std::round(h));
                    if (iw > 0 && ih > 0) {
                        painter.fill_rect(Rect{ix, iy, iw, ih}, color);
                    }
                };

                // Helper: draw one edge segment with a given border style.
                // (ax,ay)â†’(bx,by) are the outer-edge start/end points.
                // `w` is the border width in px; style and color are side-specific.
                auto draw_edge = [&](float ax, float ay, float bx, float by,
                                     float w, BS style, Color color) {
                    if (w <= 0.0f) return;
                    const float dx = bx - ax;
                    const float dy = by - ay;
                    const float len = std::sqrt(dx * dx + dy * dy);
                    if (len <= 0.0f) return;
                    const bool horiz = (std::abs(dy) < 0.5f);
                    const float ux = dx / len;
                    const float uy = dy / len;
                    // Centre of the border stripe, perpendicular to the edge.
                    // For horizontal edges: shift down by w/2. For vertical:
                    // shift right by w/2. Both are embedded in ax/ay already
                    // for our usage â€” the caller passes midpoint coordinates.
                    switch (style) {
                        case BS::Solid: {
                            painter.stroke_line(ax, ay, bx, by, color, w);
                            break;
                        }
                        case BS::Dashed: {
                            // CSS dashed: dash ~= 3x border width, gap ~= border width.
                            // Fill border-area rectangles so stroke caps do not erase the gap.
                            const float dash = std::max(w * 3.0f, 1.0f);
                            const float gap = std::max(w, 1.0f);
                            const float period = dash + gap;
                            float t = 0.0f;
                            while (t < len) {
                                const float dash_end = std::min(t + dash, len);
                                const float seg_len = dash_end - t;
                                if (horiz) {
                                    fill_border_rect(ax + ux * t,
                                                     ay - w * 0.5f,
                                                     seg_len,
                                                     w,
                                                     color);
                                } else {
                                    fill_border_rect(ax - w * 0.5f,
                                                     ay + uy * t,
                                                     w,
                                                     seg_len,
                                                     color);
                                }
                                t += period;
                            }
                            break;
                        }
                        case BS::Dotted: {
                            // CSS dotted: circular dots, diameter = border width,
                            // spaced at ~2x diameter (dot + equal gap).
                            const float radius = w * 0.5f;
                            const float period = w * 2.0f;
                            float t = radius;  // first dot centred at w/2 from edge
                            while (t <= len) {
                                painter.fill_circle(ax + ux * t, ay + uy * t,
                                                    radius, color);
                                t += period;
                            }
                            break;
                        }
                        case BS::Double: {
                            // CSS double: outer stripe + gap + inner stripe.
                            const float sub_w = std::max(1.0f, std::floor(w / 3.0f));
                            if (horiz) {
                                const float y = ay - w * 0.5f;
                                fill_border_rect(ax, y, len, sub_w, color);
                                fill_border_rect(ax, y + w - sub_w, len, sub_w, color);
                            } else {
                                const float x = ax - w * 0.5f;
                                fill_border_rect(x, ay, sub_w, len, color);
                                fill_border_rect(x + w - sub_w, ay, sub_w, len, color);
                            }
                            break;
                        }
                        default: break;
                    }
                };

                // All sides share the same style (per-side style variation
                // is Phase 2C+ â€” see computed_style.h comment).
                const BS bstyle = cs.border_style;

                // border-collapse: collapse â€” adjacent cells share one border.
                // Normally each edge sits half its width INSIDE the border box;
                // two adjacent cells then draw the shared edge one stripe apart,
                // doubling the interior grid line. In collapse mode we snap each
                // edge to (boundary - 0.5): a 1px stroke there covers exactly one
                // pixel column/row just inside the boundary, and BOTH neighbours
                // (cell right == next cell left; row bottom == next row top, which
                // also coincides with the cells' base border-bottom) snap to that
                // same pixel â€” a single crisp grid line instead of a double.
                const bool collapse = cs.border_collapse;
                const float cwt = collapse ? -0.5f : 0.0f;  // half-pixel snap

                // Top edge: runs full width from left edge to right edge.
                if (vis_top) {
                    const float wt = static_cast<float>(used_border_top);
                    const float my = collapse ? ey + cwt : ey + wt * 0.5f;
                    draw_edge(ex, my, ex + ew, my, wt, bstyle,
                              detail::unpack_rgba(c_top));
                }
                // Bottom edge: full width.
                if (vis_bottom) {
                    const float wb = static_cast<float>(used_border_bottom);
                    const float my = collapse ? ey + eh + cwt : ey + eh - wb * 0.5f;
                    draw_edge(ex, my, ex + ew, my, wb, bstyle,
                              detail::unpack_rgba(c_bottom));
                }
                // Left edge: between the top and bottom edges' outer boundaries.
                if (vis_left) {
                    const float wl  = static_cast<float>(used_border_left);
                    const float mx  = collapse ? ex + cwt : ex + wl * 0.5f;
                    const float y0  = collapse ? ey + cwt
                                      : ey + static_cast<float>(used_border_top);
                    const float y1  = collapse ? ey + eh + cwt
                                      : ey + eh - static_cast<float>(used_border_bottom);
                    draw_edge(mx, y0, mx, y1, wl, bstyle,
                              detail::unpack_rgba(c_left));
                }
                // Right edge: between top and bottom edges' outer boundaries.
                if (vis_right) {
                    const float wr  = static_cast<float>(used_border_right);
                    const float mx  = collapse ? ex + ew + cwt : ex + ew - wr * 0.5f;
                    const float y0  = collapse ? ey + cwt
                                      : ey + static_cast<float>(used_border_top);
                    const float y1  = collapse ? ey + eh + cwt
                                      : ey + eh - static_cast<float>(used_border_bottom);
                    draw_edge(mx, y0, mx, y1, wr, bstyle,
                              detail::unpack_rgba(c_right));
                }
            }
        }

        const bool dcs_drop_before =
            detail::block_has_class(b, "dcs-tree__row--drop-before") ||
            detail::block_has_class(b, "dcs-list__item--drop-before");
        const bool dcs_drop_after =
            detail::block_has_class(b, "dcs-tree__row--drop-after") ||
            detail::block_has_class(b, "dcs-list__item--drop-after");
        const bool dcs_drop_into =
            detail::block_has_class(b, "dcs-tree__row--drop-into") ||
            detail::block_has_class(b, "dcs-list__item--drop-into");
        if ((dcs_drop_before || dcs_drop_after || dcs_drop_into) &&
            eff.w > 0) {
            Color accent = Color::rgb(77, 159, 255);
            if (b.custom_props) {
                const auto it = b.custom_props->find("--dcs-accent");
                if (it != b.custom_props->end()) {
                    std::uint32_t rgba = 0;
                    if (detail::parse_hex_color(std::string(detail::trim_css_ws(it->second)),
                                        rgba)) {
                        accent = detail::unpack_rgba(rgba);
                    }
                }
            }
            if (dcs_drop_into) {
                painter.fill_rect(Rect{eff.x, eff.y, eff.w, eff.h},
                                  Color::rgba(accent.r, accent.g, accent.b,
                                              48));
                painter.fill_rect(Rect{eff.x, eff.y, 3, eff.h}, accent);
            } else {
                const int y = dcs_drop_before ? eff.y - 1 : eff.y + eff.h - 1;
                painter.fill_rect(Rect{eff.x, y, eff.w, 2}, accent);
            }
        }

        if (cs.display == detail::ComputedStyle::Display::ListItem &&
            cs.list_style_type != detail::ComputedStyle::ListStyleType::None) {
            const auto font = painter.resolve_font(
                impl_->style_store.font_family_of(cs.font_id), cs.font_size_px,
                cs.font_weight, cs.font_style != 0);
            const Color marker_color = detail::unpack_rgba(an.color_rgba);
            const int text_y = eff.y + used_border_top + cs.padding_top;
            const auto metrics = painter.text_metrics(font);
            const float natural_line_h = metrics.line_height > 0.0f
                ? metrics.line_height
                : static_cast<float>(cs.font_size_px);
            const float css_line_h =
                detail::resolved_line_height_px(cs, natural_line_h);
            const float line_top =
                static_cast<float>(text_y) + (css_line_h - natural_line_h) * 0.5f;
            const float marker_cy = line_top + natural_line_h * 0.5f;
            const float marker_cx = static_cast<float>(eff.x - 16);

            using LT = detail::ComputedStyle::ListStyleType;
            switch (cs.list_style_type) {
                case LT::Disc:
                case LT::Circle:
                    painter.fill_circle(marker_cx, marker_cy, 3.0f,
                                        marker_color);
                    break;
                case LT::Square:
                    painter.fill_rect(
                        Rect{static_cast<int>(std::round(marker_cx - 2.5f)),
                             static_cast<int>(std::round(marker_cy - 2.5f)),
                             5, 5},
                        marker_color);
                    break;
                case LT::Decimal: {
                    const auto marker =
                        std::to_string(std::max(1, list_ordinals[i])) + ".";
                    const float letter_spacing_px =
                        static_cast<float>(cs.letter_spacing_x100) / 100.0f;
                    painter.draw_text_box(
                        font, Point{eff.x - 26, text_y}, marker, marker_color,
                        20.0f, detail::effective_line_height_mult(cs),
                        letter_spacing_px, Painter::TextAlign::Right);
                    break;
                }
                case LT::None:
                default:
                    break;
            }
        }
        }  // phase == BlockPaintPhase::Boxes

        // ── PHASE: text + widget chrome ──────────────────────────────
        if (phase == BlockPaintPhase::Text) {
        // The dcs-grip drag handle is a pixel-art texture — widget chrome the
        // painter draws directly (a peer of the checkbox tick / switch knob
        // below), not a CSS background. Drawn as 1px rects, so there is no
        // image resource to own or free. The unit is a 5x4 "raised nub" tile
        // (2px nub body with a top-left highlight and bottom-right shadow over
        // a dark cell) repeated to fill the grip rect — the classic embossed
        // grip texture.
        if (detail::block_has_class(b, "dcs-grip") &&
            eff.w > 0 && eff.h > 0) {
            // Exact 5x4 raised-nub tile from the design. Palette (index →
            // colour): 0 highlight (lightest, top-left), 1 base, 2 shadow,
            // 3 darkest accent.
            const Color pal[4] = {
                {0x96, 0x9b, 0xa6, 0xff},  // 0 highlight
                {0x6d, 0x74, 0x84, 0xff},  // 1 base
                {0x4c, 0x52, 0x62, 0xff},  // 2 shadow
                {0x2d, 0x31, 0x3d, 0xff},  // 3 darkest
            };
            static const std::uint8_t kNub[4][5] = {
                {3, 1, 1, 2, 3},
                {1, 0, 0, 1, 2},
                {1, 1, 1, 1, 2},
                {2, 2, 2, 2, 2},
            };
            const int tw = 5, th = 4;
            // CEIL the tile counts and clip each pixel to `eff`: flooring left
            // a remainder strip unpainted on sizes that aren't a whole number
            // of tiles (16px wide → 3 tiles → a dead 1px column), while the
            // max(1,…) floor painted a whole tile OUTSIDE a grip smaller than
            // one tile. Ceil covers the box; the clip keeps the overhang in.
            const int cols = std::max(1, (eff.w + tw - 1) / tw);
            const int rows = std::max(1, (eff.h + th - 1) / th);
            const int x0 = eff.x + (eff.w - cols * tw) / 2;
            const int y0 = eff.y + (eff.h - rows * th) / 2;
            const int x1 = eff.x + eff.w;
            const int y1 = eff.y + eff.h;
            for (int ry = 0; ry < rows; ++ry) {
                for (int cx = 0; cx < cols; ++cx) {
                    for (int py = 0; py < th; ++py) {
                        const int y = y0 + ry * th + py;
                        if (y < eff.y || y >= y1) continue;
                        for (int px = 0; px < tw; ++px) {
                            const int x = x0 + cx * tw + px;
                            if (x < eff.x || x >= x1) continue;
                            painter.fill_rect(Rect{x, y, 1, 1},
                                              pal[kNub[py][px]]);
                        }
                    }
                }
            }
        }
        // A focused text control still has a line box and caret when its value
        // is empty. Keep controls on this path even without a text run; the
        // empty draw is harmless and the shared layout table supplies offset
        // zero for caret painting and IME anchoring.
        if (!b.text.empty() || b.text_control) {
            const auto font = painter.resolve_font(
                impl_->style_store.font_family_of(cs.font_id), cs.font_size_px, cs.font_weight, cs.font_style != 0);
            const int textarea_idx_for_text = detail::nearest_block_with_tag(
                impl_->blocks, static_cast<int>(i), "textarea");
            int text_y = eff.y + used_border_top  + cs.padding_top;
            // Sub-pixel remainders the int math drops. Browsers keep text
            // positions fractional (yoga's float layout, half of an odd
            // free space) and rasterize from the fractional baseline; the
            // painter consumes the exact value, so the fractions ride along
            // and only the rasterizer rounds.
            float text_y_frac = b.bounds_f.y - static_cast<float>(b.bounds.y);
            float text_x_frac = b.bounds_f.x - static_cast<float>(b.bounds.x);
            const bool single_line_text =
                b.text.find('\n') == std::string::npos &&
                b.text.find('\r') == std::string::npos;
            if (single_line_text && textarea_idx_for_text < 0 &&
                (cs.display == detail::ComputedStyle::Display::Flex ||
                 cs.display == detail::ComputedStyle::Display::InlineFlex)) {
                const float content_h = static_cast<float>(
                    eff.h - used_border_top - used_border_bottom
                          - cs.padding_top - cs.padding_bottom);
                const float css_line_h = detail::resolved_line_height_px(
                    cs, painter.text_metrics(font).line_height);
                const float free_h = content_h - css_line_h;
                if (free_h > 0.0f) {
                    using AI = detail::ComputedStyle::AlignItems;
                    if (cs.align_items == AI::Center) {
                        const float add = free_h * 0.5f;
                        text_y += static_cast<int>(add);
                        text_y_frac += add - std::floor(add);
                    } else if (cs.align_items == AI::End) {
                        text_y += static_cast<int>(free_h);
                        text_y_frac += free_h - std::floor(free_h);
                    }
                }
            }

            // Map ComputedStyle::TextAlign â†’ Painter::TextAlign.
            if (textarea_idx_for_text < 0 && b.tag == "input" &&
                       b.input_type != "checkbox" &&
                       b.input_type != "radio") {
                // Single-line native text inputs also paint through an edit
                // viewport inset from the painted border. CSS padding still
                // controls the content origin; this accounts for the native
                // control text viewport itself.
                text_y += std::min<int>(1, used_border_top);
            }

            Painter::TextAlign paint_align = Painter::TextAlign::Left;
            switch (cs.text_align) {
                case detail::ComputedStyle::TextAlign::Left:
                    paint_align = Painter::TextAlign::Left;    break;
                case detail::ComputedStyle::TextAlign::Center:
                    paint_align = Painter::TextAlign::Center;  break;
                case detail::ComputedStyle::TextAlign::Right:
                    paint_align = Painter::TextAlign::Right;   break;
                case detail::ComputedStyle::TextAlign::Justify:
                    paint_align = Painter::TextAlign::Justify; break;
            }

            // For text alignment (center/right), nvgTextBox needs:
            //   x = left edge of the line box
            //   breakRowWidth = width of the line box
            // For block-level leaves (spans, divs) this is the block's
            // own content area. For anonymous #text leaves that live
            // inside a synthetic flex-row, the block's own width is the
            // natural text width, so centering/right-aligning within it
            // is a no-op. Instead, walk up to the nearest non-synthetic
            // ancestor block and use its content geometry as the line box.
            int text_x    = eff.x + used_border_left + cs.padding_left;
            float content_w = static_cast<float>(
                eff.w - used_border_left - used_border_right
                      - cs.padding_left - cs.padding_right);
            const bool native_select_text =
                b.tag == "select" && !detail::block_has_class(b, "form-select");
            if (native_select_text) {
                // Chrome's native closed select paints its value inside an
                // internal edit field inset in addition to CSS padding. This
                // inset is part of the platform control, not reflected in
                // getComputedStyle(). Bootstrap's .form-select opts out with
                // appearance:none and supplies its own SVG background.
                constexpr int kNativeSelectTextInsetPx = 5;
                text_x += kNativeSelectTextInsetPx;
                content_w = std::max(
                    1.0f,
                    content_w - static_cast<float>(kNativeSelectTextInsetPx));
            }

            const bool is_justify = (paint_align == Painter::TextAlign::Justify);
            const bool in_mixed_inline_run =
                b.parent_idx >= 0 &&
                static_cast<std::size_t>(b.parent_idx) < impl_->blocks.size() &&
                impl_->blocks[static_cast<std::size_t>(b.parent_idx)].synthetic &&
                static_cast<std::size_t>(b.parent_idx) < child_counts.size() &&
                child_counts[static_cast<std::size_t>(b.parent_idx)] > 1;
            const bool in_synthetic_inline_parent =
                b.parent_idx >= 0 &&
                static_cast<std::size_t>(b.parent_idx) < impl_->blocks.size() &&
                impl_->blocks[static_cast<std::size_t>(b.parent_idx)].synthetic;
            const bool first_in_synthetic_inline_parent =
                in_synthetic_inline_parent &&
                static_cast<std::size_t>(b.parent_idx) < first_child_indices.size() &&
                first_child_indices[static_cast<std::size_t>(b.parent_idx)] ==
                    static_cast<int>(i);
            if (!in_synthetic_inline_parent || first_in_synthetic_inline_parent) {
                int indent_px = cs.text_indent_value;
                if (cs.text_indent_is_pct) {
                    indent_px = static_cast<int>(std::lround(
                        content_w * static_cast<float>(cs.text_indent_value) /
                        10000.0f));
                }
                if (indent_px != 0) {
                    text_x += indent_px;
                    content_w = std::max(1.0f,
                                         content_w - static_cast<float>(indent_px));
                }
            }
            if (paint_align != Painter::TextAlign::Left &&
                in_synthetic_inline_parent && b.synthetic == false &&
                !in_mixed_inline_run) {
                // Walk parent chain to find the first non-synthetic block.
                int anc = b.parent_idx;
                while (anc >= 0) {
                    const auto& ab = impl_->blocks[static_cast<std::size_t>(anc)];
                    if (!ab.synthetic) {
                        const auto& acs =
                            impl_->style_store.computed(ab.id);
                        const int anc_dy = detail::scroll_offset_y_for(
                            impl_->blocks, impl_->style_store, anc);
                        const Rect ae{
                            ab.bounds.x, ab.bounds.y - anc_dy,
                            ab.bounds.w, ab.bounds.h,
                        };
                        const int al = ae.x + acs.used_border_left() + acs.padding_left;
                        const float aw = static_cast<float>(
                            ae.w - acs.used_border_left() - acs.used_border_right()
                                 - acs.padding_left - acs.padding_right);
                        // Use the ancestor geometry when it's meaningfully
                        // wider (the current block is narrower than the
                        // container) â€” for center/right that's the line box.
                        // For JUSTIFY we must also clamp DOWN to the
                        // container when the leaf's natural (unwrapped) width
                        // overflows it, so the text wraps to the line box and
                        // fills it edge-to-edge instead of overflowing on one
                        // line.
                        if (aw > content_w + 1.0f || is_justify) {
                            text_x    = al;
                            content_w = aw;
                        }
                        break;
                    }
                    anc = ab.parent_idx;
                }
            }

            bool pushed_text_control_clip = false;
            if (b.text_control) {
                const auto g = detail::text_control_geometry(
                    *impl_, static_cast<int>(i), painter);
                text_x = g.text_x;
                text_y = g.text_y;
                content_w = g.content_w;
                paint_align = g.align;
                if (b.tag == "textarea") {
                    // A textarea is a scroll container for its VALUE (UA
                    // overflow:auto). text_control_geometry already shifted
                    // the origin by the element's own scroll offset; clip
                    // everything (selection, text, caret, decorations) to the
                    // padding box so overflowing lines never paint over
                    // content below. The scissor REPLACES the active clip,
                    // so intersect with the ancestor clip chain too — a
                    // textarea hanging past its scrolled pane must not paint
                    // its value outside the pane (tearoff bottom edge).
                    Rect text_clip{
                        eff.x + used_border_left,
                        eff.y + used_border_top,
                        std::max(0, eff.w - used_border_left -
                                        used_border_right),
                        std::max(0, eff.h - used_border_top -
                                        used_border_bottom)};
                    Rect anc_clip;
                    if (detail::clip_rect_for_block(*impl_,
                                                    static_cast<int>(i),
                                                    anc_clip)) {
                        const auto x0 = std::max(text_clip.x, anc_clip.x);
                        const auto y0 = std::max(text_clip.y, anc_clip.y);
                        const auto x1 = std::min(text_clip.x + text_clip.w,
                                                 anc_clip.x + anc_clip.w);
                        const auto y1 = std::min(text_clip.y + text_clip.h,
                                                 anc_clip.y + anc_clip.h);
                        text_clip.x = x0;
                        text_clip.y = y0;
                        text_clip.w = std::max(x1 - x0, decltype(x1){0});
                        text_clip.h = std::max(y1 - y0, decltype(y1){0});
                    }
                    painter.push_clip(text_clip);
                    pushed_text_control_clip = true;
                }
            }

            // Add 1px slack to wrap width: measure rounds + draw word-
            // break can disagree at the edge (text whose natural width
            // exactly equals content_w sometimes wraps the last word
            // onto a new line). The block was already sized to match
            // the measure, so giving paint a single-pixel tolerance
            // matches the design intent without overflowing.
            // Slack: nvgTextBoxBounds returns RENDERED bounds (ink
            // extent) but nvgTextBox's word-wrap uses glyph ADVANCE
            // widths. Advance > rendered for fonts with sub-pixel
            // overhang, so passing exactly content_w would wrap text
            // that measure said fits. A few pixels of slack covers
            // the gap for typical UI fonts at typical sizes.
            //
            // white-space: nowrap / pre â€” suppress line-wrapping by
            // passing a very large max-width to both measure and draw.
            using WS = detail::ComputedStyle::WhiteSpace;
            const bool is_nowrap = (cs.white_space == WS::Nowrap ||
                                    cs.white_space == WS::Pre);
            const float letter_spacing_px =
                static_cast<float>(cs.letter_spacing_x100) / 100.0f;
            const float line_height_mult =
                detail::effective_line_height_mult(cs);
            const auto measure_text_width = [&](std::string_view text) {
                if (cs.letter_spacing_x100 == 0) {
                    return static_cast<float>(painter.measure_text(font, text));
                }
                return static_cast<float>(
                    painter
                        .measure_text_box(font, text, 1e6f,
                                          line_height_mult,
                                          letter_spacing_px)
                        .width);
            };
            const auto natural_text_width = [&] {
                return measure_text_width(b.text);
            };

            // white-space:nowrap forces a single line, which we signal to
            // the painter with a huge wrap width. But nvgTextBox aligns
            // center/right *within* that wrap width â€” at 1e6 it would fling
            // the glyphs ~500k px off-screen (the symptom: nowrap centered
            // text like progress-bar "%" labels and badges renders blank).
            // So resolve the alignment offset here against the real line box
            // and hand the painter a pre-positioned LEFT single-line draw.
            if (is_nowrap && (paint_align == Painter::TextAlign::Center ||
                              paint_align == Painter::TextAlign::Right)) {
                const float tw = natural_text_width();
                const float slack = content_w - tw;
                if (paint_align == Painter::TextAlign::Center)
                    text_x += static_cast<int>(std::lround(slack * 0.5f));
                else  // Right
                    text_x += static_cast<int>(std::lround(slack));
                paint_align = Painter::TextAlign::Left;
            }

            bool force_single_line = is_nowrap;
            if (!is_nowrap &&
                !b.text_control &&
                b.tag != "select" &&
                !in_mixed_inline_run &&
                b.text.find('\n') == std::string::npos &&
                (paint_align == Painter::TextAlign::Center ||
                 paint_align == Painter::TextAlign::Right)) {
                constexpr float kAdvanceTolerancePx = 4.0f;
                const float tw = natural_text_width();
                if (tw <= content_w + kAdvanceTolerancePx) {
                    const float slack = content_w - tw;
                    if (paint_align == Painter::TextAlign::Center)
                        text_x += static_cast<int>(std::lround(slack * 0.5f));
                    else
                        text_x += static_cast<int>(std::lround(slack));
                    paint_align = Painter::TextAlign::Left;
                    force_single_line = true;
                }
            }

            // Justify fills exactly to the content edge â€” no wrap slack
            // (the +4 below would let justified lines spill 4px past it).
            const bool aligned_text =
                paint_align == Painter::TextAlign::Center ||
                paint_align == Painter::TextAlign::Right;
            const bool suppress_wrap_slack = b.text_control ||
                b.tag == "select" || aligned_text;
            const float wrap_slack = suppress_wrap_slack ? 0.0f : 4.0f;
            const float draw_max_w = force_single_line ? 1e6f
                                   : (is_justify ? content_w
                                                 : content_w + wrap_slack);
            const TextLayoutEntry* cached_text_layout = nullptr;
            if (b.text_control) {
                TextControlGeometry g{};
                g.font = font;
                g.text_x = text_x;
                g.text_y = text_y;
                g.content_w = content_w;
                g.letter_spacing_px = letter_spacing_px;
                g.line_height_mult = line_height_mult;
                g.align = paint_align;
                g.nowrap = force_single_line;
                cached_text_layout = &detail::ensure_text_layout_entry(
                    *impl_, static_cast<int>(i), g, b, painter);
            }
            if (cached_text_layout != nullptr && detail::has_text_selection(b)) {
                const auto& text_layout = *cached_text_layout;
                const auto [sel_begin, sel_end] = detail::normalized_selection(b);
                const auto caret_index_for =
                    [&](std::size_t offset) -> std::size_t {
                    auto it = std::lower_bound(
                        text_layout.caret_offsets.begin(),
                        text_layout.caret_offsets.end(), offset);
                    if (it == text_layout.caret_offsets.end()) {
                        return text_layout.caret_offsets.empty()
                            ? 0
                            : text_layout.caret_offsets.size() - 1;
                    }
                    return static_cast<std::size_t>(
                        std::distance(text_layout.caret_offsets.begin(), it));
                };
                const std::size_t begin_idx = caret_index_for(sel_begin);
                const std::size_t end_idx = caret_index_for(sel_end);
                const int first_line = text_layout.caret_lines[begin_idx];
                const int last_line = text_layout.caret_lines[end_idx];
                const float line_h = std::max(1.0f,
                                              text_layout.css_line_height);
                const float ink_h = std::max(1.0f,
                                             text_layout.natural_line_height);
                const Color selection_color{0x4D, 0xA3, 0xFF, 0x66};
                for (int line = first_line; line <= last_line; ++line) {
                    float x0 = 0.0f;
                    float x1 = line < static_cast<int>(
                                      text_layout.line_widths.size())
                        ? text_layout.line_widths[static_cast<std::size_t>(line)]
                        : 0.0f;
                    if (line == first_line) {
                        x0 = text_layout.caret_x[begin_idx];
                    }
                    if (line == last_line) {
                        x1 = text_layout.caret_x[end_idx];
                    }
                    if (x1 < x0) std::swap(x0, x1);
                    if (x1 <= x0) continue;
                    const float line_origin =
                        detail::aligned_line_origin_x(
                            text_layout,
                            static_cast<std::uint16_t>(line));
                    const float y = static_cast<float>(text_y) +
                                    static_cast<float>(line) * line_h +
                                    (line_h - ink_h) * 0.5f;
                    painter.fill_rect(
                        Rect{
                            static_cast<int>(std::floor(line_origin + x0)),
                            static_cast<int>(std::floor(y)),
                            std::max(1, static_cast<int>(std::ceil(x1 - x0))),
                            std::max(1, static_cast<int>(std::ceil(ink_h))),
                        },
                        selection_color);
                }
            }
            painter.draw_text_box(font,
                                  static_cast<float>(text_x) + text_x_frac,
                                  static_cast<float>(text_y) + text_y_frac,
                                  b.text,
                                  detail::unpack_rgba(an.color_rgba),
                                  draw_max_w,
                                  line_height_mult,
                                  letter_spacing_px,
                                  paint_align);
            // A text field shows EITHER a selection highlight OR the
            // caret, never both: suppress the caret while a non-empty
            // selection is active (the select-all a numeric field does
            // on first focus, or any range drag).
            if (b.text_control && static_cast<int>(i) == impl_->focused_idx &&
                !detail::has_text_selection(b) &&
                impl_->caret_blink_visible) {
                const TextLayoutEntry* caret_layout = cached_text_layout;
                if (caret_layout == nullptr) {
                    TextControlGeometry g{};
                    g.font = font;
                    g.text_x = text_x;
                    g.text_y = text_y;
                    g.content_w = content_w;
                    g.letter_spacing_px = letter_spacing_px;
                    g.line_height_mult = line_height_mult;
                    g.align = paint_align;
                    g.nowrap = force_single_line;
                    caret_layout = &detail::ensure_text_layout_entry(
                        *impl_, static_cast<int>(i), g, b, painter);
                }

                // Composed space: with an active IME preedit the layout's
                // caret table indexes text_value + spliced preedit, and the
                // visible caret sits at the IME's cursor inside it.
                const auto caret_offset = std::min(
                    detail::composed_caret_offset(
                        *impl_, static_cast<int>(i), b),
                    detail::composed_text_value(
                        *impl_, static_cast<int>(i), b).size());
                auto it = std::lower_bound(
                    caret_layout->caret_offsets.begin(),
                    caret_layout->caret_offsets.end(), caret_offset);
                std::size_t caret_index = it == caret_layout->caret_offsets.end()
                    ? caret_layout->caret_offsets.size() - 1
                    : static_cast<std::size_t>(
                          std::distance(caret_layout->caret_offsets.begin(),
                                        it));
                if (caret_layout->caret_offsets[caret_index] != caret_offset) {
                    caret_index = caret_index == 0 ? 0 : caret_index - 1;
                }
                const auto line = caret_layout->caret_lines[caret_index];
                const float caret_x =
                    detail::aligned_line_origin_x(*caret_layout, line) +
                    caret_layout->caret_x[caret_index];
                const float natural_line_h =
                    std::max(1.0f, caret_layout->natural_line_height);
                const float css_line_h =
                    std::max(1.0f, caret_layout->css_line_height);
                const float line_top =
                    static_cast<float>(text_y) +
                    static_cast<float>(line) * css_line_h +
                    (css_line_h - natural_line_h) * 0.5f;
                // Match the font's complete natural line box. The previous
                // two-pixel inset at each end made an 18px UI font produce a
                // visibly undersized 14px caret.
                const float y0 = std::floor(line_top) + 0.5f;
                const float y1 =
                    std::ceil(line_top + natural_line_h) + 0.5f;
                painter.stroke_line(caret_x, y0, caret_x, y1,
                                    detail::unpack_rgba(an.color_rgba),
                                    1.0f);

                // IME preedit decoration: a thin underline across the whole
                // preedit and a thick one under the IME's active clause —
                // the conventional composition rendering on every platform.
                const auto [pre_begin, pre_end] =
                    detail::composition_display_range(
                        *impl_, static_cast<int>(i), b);
                if (pre_end > pre_begin &&
                    !caret_layout->caret_offsets.empty()) {
                    const auto index_of = [&](std::size_t offset) {
                        auto iter = std::lower_bound(
                            caret_layout->caret_offsets.begin(),
                            caret_layout->caret_offsets.end(), offset);
                        std::size_t k =
                            iter == caret_layout->caret_offsets.end()
                                ? caret_layout->caret_offsets.size() - 1
                                : static_cast<std::size_t>(std::distance(
                                      caret_layout->caret_offsets.begin(),
                                      iter));
                        if (caret_layout->caret_offsets[k] != offset && k > 0) {
                            --k;
                        }
                        return k;
                    };
                    const Color underline = detail::unpack_rgba(an.color_rgba);
                    const auto draw_span = [&](std::size_t span_begin,
                                               std::size_t span_end,
                                               float thickness) {
                        if (span_end <= span_begin) return;
                        const std::size_t bi = index_of(span_begin);
                        const std::size_t ei = index_of(span_end);
                        const int first_line = caret_layout->caret_lines[bi];
                        const int last_line = caret_layout->caret_lines[ei];
                        for (int ln = first_line; ln <= last_line; ++ln) {
                            const float sx = ln == first_line
                                ? caret_layout->caret_x[bi] : 0.0f;
                            const float ex = ln == last_line
                                ? caret_layout->caret_x[ei]
                                : (static_cast<std::size_t>(ln) <
                                           caret_layout->line_widths.size()
                                       ? caret_layout->line_widths
                                             [static_cast<std::size_t>(ln)]
                                       : 0.0f);
                            if (ex <= sx) continue;
                            const float origin = detail::aligned_line_origin_x(
                                *caret_layout, static_cast<std::uint16_t>(ln));
                            const float ln_top =
                                static_cast<float>(text_y) +
                                static_cast<float>(ln) * css_line_h +
                                (css_line_h - natural_line_h) * 0.5f;
                            const float uy =
                                std::ceil(ln_top + natural_line_h - 1.5f) +
                                0.5f;
                            painter.stroke_line(origin + sx, uy,
                                                origin + ex, uy,
                                                underline, thickness);
                        }
                    };
                    draw_span(pre_begin, pre_end, 1.0f);
                    draw_span(pre_begin + impl_->composition_clause_begin,
                              pre_begin + impl_->composition_clause_end,
                              2.0f);
                }
            }
            if (cs.text_decoration_line != detail::ComputedStyle::DecorationNone) {
                const auto metrics = painter.text_metrics(font);
                const float natural_line_h =
                    metrics.line_height > 0.0f
                        ? metrics.line_height
                        : static_cast<float>(cs.font_size_px);
                const float css_line_h =
                    detail::resolved_line_height_px(cs, natural_line_h);
                const float line_top =
                    static_cast<float>(text_y) +
                    (css_line_h - natural_line_h) * 0.5f;
                const float baseline = line_top + metrics.ascender;
                const float tw = static_cast<float>(
                    std::max(1, painter.measure_text(font, b.text)));
                const float x0 = static_cast<float>(text_x);
                const float x1 = x0 + tw;
                const Color deco = an.text_decoration_rgba != 0
                    ? detail::unpack_rgba(an.text_decoration_rgba)
                    : detail::unpack_rgba(an.color_rgba);
                const float thickness =
                    std::max(1.0f, static_cast<float>(cs.font_size_px) / 16.0f);
                if (cs.text_decoration_line &
                    detail::ComputedStyle::DecorationUnderline) {
                    painter.stroke_line(x0, baseline + thickness * 1.5f,
                                        x1, baseline + thickness * 1.5f,
                                        deco, thickness);
                }
                if (cs.text_decoration_line &
                    detail::ComputedStyle::DecorationOverline) {
                    painter.stroke_line(x0, line_top + thickness * 0.5f,
                                        x1, line_top + thickness * 0.5f,
                                        deco, thickness);
                }
                if (cs.text_decoration_line &
                    detail::ComputedStyle::DecorationLineThrough) {
                    const float y = line_top + metrics.ascender * 0.55f;
                    painter.stroke_line(x0, y, x1, y, deco, thickness);
                }
            }
            if (pushed_text_control_clip) painter.pop_clip();
        }

        // Closed single-row <select> controls expose an indicator supplied by
        // either the native widget or a CSS background. We don't rasterize
        // Bootstrap's data-URI SVG yet, so draw the same chevron geometry here.
        if (b.tag == "select") {
            const auto* size_attr = detail::block_attr_value(b, "size");
            const bool listbox =
                detail::block_attr_value(b, "multiple") != nullptr ||
                (size_attr != nullptr && !size_attr->empty() && *size_attr != "1");
            if (!listbox) {
                const bool bootstrap_form_select =
                    detail::block_has_class(b, "form-select");
                const float right = static_cast<float>(eff.x + eff.w);
                const float cy = static_cast<float>(eff.y) +
                                 static_cast<float>(eff.h) * 0.5f;
                const float cx = bootstrap_form_select
                    ? right - 20.5f
                    : right - 9.75f;
                const float half_w = bootstrap_form_select ? 4.0f : 3.75f;
                const float half_h = 2.5f;
                const float thickness = bootstrap_form_select ? 1.25f : 1.35f;
                const Color chev = bootstrap_form_select
                    ? Color{0x34, 0x3a, 0x40, 0xFF}
                    : detail::unpack_rgba(an.color_rgba);
                painter.stroke_line(cx - half_w, cy - half_h,
                                    cx, cy + half_h,
                                    chev, thickness);
                painter.stroke_line(cx, cy + half_h,
                                    cx + half_w, cy - half_h,
                                    chev, thickness);
            }
        }

        // â”€â”€ UA form-control drawing â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // Bootstrap's form-check-input uses SVG data: URIs for its
        // checkbox checkmark, radio dot, and switch knob â€” none of which
        // we rasterize. Draw UA approximations for :checked / unchecked
        // states that match Chrome's static appearance. The box itself
        // (border + background) is already painted by the normal path above.
        if (b.tag == "input" &&
            (b.input_type == "checkbox" || b.input_type == "radio")) {
            const float bx = static_cast<float>(eff.x);
            const float by = static_cast<float>(eff.y);
            const float bw = static_cast<float>(eff.w);
            const float bh = static_cast<float>(eff.h);
            const float cx = bx + bw * 0.5f;
            const float cy = by + bh * 0.5f;

            const bool is_switch = (b.role_attr == "switch");

            if (b.input_type == "checkbox" && !is_switch) {
                if (b.is_checked) {
                    // Bootstrap's checkbox icon is a 20x20 SVG path:
                    // m6 10 3 3 6-6, stroke-width 3, round caps/joins.
                    const float m = std::min(bw, bh);
                    const float s = m / 20.0f;
                    const float x0 = bx + 6.0f  * s, y0 = by + 10.0f * s;
                    const float x1 = bx + 9.0f  * s, y1 = by + 13.0f * s;
                    const float x2 = bx + 15.0f * s, y2 = by + 7.0f  * s;
                    const float sw = std::max(1.0f, 3.0f * s);
                    const Color white{0xFF, 0xFF, 0xFF, 0xFF};
                    painter.stroke_line(x0, y0, x1, y1, white, sw);
                    painter.stroke_line(x1, y1, x2, y2, white, sw);
                    const float cap_r = sw * 0.5f;
                    painter.fill_circle(x0, y0, cap_r, white);
                    painter.fill_circle(x1, y1, cap_r, white);
                    painter.fill_circle(x2, y2, cap_r, white);
                }
            } else if (b.input_type == "radio") {
                if (b.is_checked) {
                    // Bootstrap's radio icon is a circle r=2 in an 8x8
                    // viewBox, scaled to contain in the control box.
                    const float dot_r = std::min(bw, bh) * 0.25f;
                    painter.fill_circle(cx, cy, dot_r, Color{0xFF, 0xFF, 0xFF, 0xFF});
                }
            }

            // Switch knob: pill-shaped background is drawn by CSS (the
            // form-switch input has a wide border-radius). We draw the
            // round white knob positioned at left (unchecked) or right
            // (checked).
            if (b.input_type == "checkbox" && is_switch) {
                const float knob_r   = bh * 0.5f - 2.0f;
                const float knob_cx  = b.is_checked
                    ? (bx + bw - knob_r - 2.0f)
                    : (bx      + knob_r + 2.0f);
                const Color knob = b.is_checked
                    ? Color{0xFF, 0xFF, 0xFF, 0xFF}
                    : Color{0x00, 0x00, 0x00, 0x40};
                painter.fill_circle(knob_cx, cy, knob_r,
                                    knob);
            }
        }

        // ── Knob chrome (UA-drawn, same tier as checkbox/radio/switch) ──
        // Ring + value arc paint here, UNDER the cap child block; the
        // indicator paints in the CAP's own chrome below so it lands on
        // top — matching the framework DOM's stacking (ring < cap <
        // indicator). All state reads from data-* attrs at paint time:
        // a knob move is one attribute write — no SVG path strings, no
        // per-paint reparse ("SVG → static, paint → dynamic").
        constexpr double kDegRad = 3.14159265358979323846 / 180.0;
        if (detail::block_has_attr(b, "data-dcs-knob") ||
            detail::block_has_attr(b, "data-aui-knob")) {
            auto* kelem = impl_->style_store.element_of(b.id);
            const float side = std::min(static_cast<float>(eff.w),
                                        static_cast<float>(eff.h));
            if (kelem != nullptr && side > 4.0f) {
                const float scale = side / 24.0f;
                const float kcx = static_cast<float>(eff.x) +
                                  static_cast<float>(eff.w) * 0.5f;
                const float kcy = static_cast<float>(eff.y) +
                                  static_cast<float>(eff.h) * 0.5f;
                const float radius = 10.5f * scale;
                std::vector<float> cmds;
                cmds.reserve(46);
                const auto seed = [&](double deg) {
                    cmds.clear();
                    cmds.push_back(kPathMove);
                    cmds.push_back(kcx + radius * static_cast<float>(
                                             std::cos(deg * kDegRad)));
                    cmds.push_back(kcy + radius * static_cast<float>(
                                             std::sin(deg * kDegRad)));
                };
                // Ring background: -225° → 45°. Decius rings are white @
                // 8%; the aui/Bootstrap variant used rgba(108,117,125,.35).
                const bool aui_variant =
                    !detail::block_has_attr(b, "data-dcs-knob");
                seed(-225.0);
                append_arc_cubics(cmds, kcx, kcy, radius, -225.0, 45.0);
                painter.stroke_path(cmds.data(), cmds.size(),
                                    PathPaint::solid(
                                        aui_variant
                                            ? Color{108, 117, 125, 89}
                                            : Color{255, 255, 255, 20}),
                                    1.5f * scale, LineCap::Round,
                                    LineJoin::Round);
                // Value arc in the accent color.
                const double vmin = detail::elem_attr_num(kelem, "data-min", 0.0);
                const double vmax = detail::elem_attr_num(kelem, "data-max", 1.0);
                const double val = detail::elem_attr_num(kelem, "data-value", vmin);
                const bool bipolar = detail::has_attr(kelem, "data-bipolar");
                const double p = detail::normalized_control_value(val, vmin, vmax);
                const double sweep = (bipolar ? p - 0.5 : p) * 270.0;
                if (std::abs(sweep) > 0.5) {
                    const double start = bipolar ? -90.0 : -225.0;
                    detail::ResolvedStyle vrs;
                    vrs.computed = cs;
                    vrs.animated = an;
                    vrs.custom_props = b.custom_props;
                    // Bootstrap-blue fallback for the aui variant (its
                    // old arc was a literal #0d6efd); Decius resolves
                    // its accent custom property.
                    std::uint32_t accent =
                        aui_variant ? 0x0D6EFDFFu : 0x4D9FFFFFu;
                    (void) detail::parse_generated_color("var(--dcs-accent)", vrs,
                                                 accent);
                    seed(start);
                    append_arc_cubics(cmds, kcx, kcy, radius, start,
                                      start + sweep);
                    painter.stroke_path(
                        cmds.data(), cmds.size(),
                        PathPaint::solid(detail::unpack_rgba(accent)),
                        1.75f * scale, LineCap::Round, LineJoin::Round);
                }
            }
        }
        if ((detail::block_has_class(b, "dcs-knob__cap") ||
             detail::block_has_class(b, "aui-knob__cap")) &&
            b.parent_idx >= 0 &&
            static_cast<std::size_t>(b.parent_idx) < impl_->blocks.size()) {
            const auto& kb =
                impl_->blocks[static_cast<std::size_t>(b.parent_idx)];
            auto* kelem = impl_->style_store.element_of(kb.id);
            if (kelem != nullptr && (detail::has_attr(kelem, "data-dcs-knob") ||
                                     detail::has_attr(kelem, "data-aui-knob"))) {
                // The cap is inset 18% per side, so the knob box is
                // cap/0.64; indicator length is 38% of the knob box
                // (.dcs-knob__indicator: height 38%, pivot at center,
                // angle 0 = straight up, clockwise positive).
                const float cap_side = std::min(static_cast<float>(eff.w),
                                                static_cast<float>(eff.h));
                const float kcx = static_cast<float>(eff.x) +
                                  static_cast<float>(eff.w) * 0.5f;
                const float kcy = static_cast<float>(eff.y) +
                                  static_cast<float>(eff.h) * 0.5f;
                const float len = cap_side * (0.38f / 0.64f);
                const double vmin = detail::elem_attr_num(kelem, "data-min", 0.0);
                const double vmax = detail::elem_attr_num(kelem, "data-max", 1.0);
                const double val = detail::elem_attr_num(kelem, "data-value", vmin);
                const double angle =
                    detail::decius_knob_angle(vmin, vmax, val) * kDegRad;
                const float tx =
                    kcx + len * static_cast<float>(std::sin(angle));
                const float ty =
                    kcy - len * static_cast<float>(std::cos(angle));
                // Resolve --dcs-accent against the KNOB block's style
                // (where the custom property cascades to), not the cap's.
                detail::ResolvedStyle vrs;
                vrs.computed = impl_->style_store.computed(kb.id);
                vrs.animated = impl_->style_store.animated(kb.id);
                vrs.custom_props = kb.custom_props;
                std::uint32_t accent =
                    detail::has_attr(kelem, "data-dcs-knob") ? 0x4D9FFFFFu
                                                     : 0x0D6EFDFFu;
                (void) detail::parse_generated_color("var(--dcs-accent)", vrs,
                                             accent);
                const float icmds[] = {kPathMove, kcx, kcy,
                                       kPathLine, tx, ty};
                painter.stroke_path(icmds, 6,
                                    PathPaint::solid(
                                        detail::unpack_rgba(accent)),
                                    2.0f, LineCap::Round, LineJoin::Round);
            }
        }

        if (detail::block_has_class(b, "dcs-check__box") && b.parent_idx >= 0 &&
            static_cast<std::size_t>(b.parent_idx) < impl_->blocks.size()) {
            const auto& parent =
                impl_->blocks[static_cast<std::size_t>(b.parent_idx)];
            const bool decius_check = detail::block_has_class(parent, "dcs-check");
            const bool decius_radio = detail::block_has_class(parent, "dcs-radio");
            const auto* checked_attr = detail::block_attr_value(parent, "aria-checked");
            const bool checked = checked_attr && *checked_attr == "true";
            if (checked && (decius_check || decius_radio)) {
                const float bx = static_cast<float>(eff.x);
                const float by = static_cast<float>(eff.y);
                const float bw = static_cast<float>(eff.w);
                const float bh = static_cast<float>(eff.h);
                Color mark_color =
                    detail::unpack_rgba(an.color_rgba);
                if (mark_color.a == 0) {
                    mark_color = Color{0xFF, 0xFF, 0xFF, 0xFF};
                }
                if (decius_radio) {
                    painter.fill_circle(bx + bw * 0.5f, by + bh * 0.5f,
                                        std::max(2.0f, std::min(bw, bh) * 0.25f),
                                        mark_color);
                } else {
                    const float m = std::min(bw, bh);
                    const float s = m / 20.0f;
                    const float x0 = bx + 6.0f  * s;
                    const float y0 = by + 10.0f * s;
                    const float x1 = bx + 9.0f  * s;
                    const float y1 = by + 13.0f * s;
                    const float x2 = bx + 15.0f * s;
                    const float y2 = by + 7.0f  * s;
                    const float sw = std::max(1.0f, 3.0f * s);
                    painter.stroke_line(x0, y0, x1, y1, mark_color, sw);
                    painter.stroke_line(x1, y1, x2, y2, mark_color, sw);
                    const float cap_r = sw * 0.5f;
                    painter.fill_circle(x0, y0, cap_r, mark_color);
                    painter.fill_circle(x1, y1, cap_r, mark_color);
                    painter.fill_circle(x2, y2, cap_r, mark_color);
                }
            }
        }

        if (b.tag == "input" && b.input_type == "color") {
            std::uint32_t rgba = 0;
            const auto* value = detail::block_attr_value(b, "value");
            if (value && detail::parse_hex_color(*value, rgba)) {
                const int inset_x = std::max(3, cs.padding_left / 2);
                const int inset_y = std::max(3, cs.padding_top / 2);
                Rect swatch{
                    eff.x + cs.used_border_left() + inset_x,
                    eff.y + cs.used_border_top() + inset_y,
                    std::max(1, eff.w - cs.used_border_left()
                                      - cs.used_border_right()
                                      - inset_x * 2),
                    std::max(1, eff.h - cs.used_border_top()
                                      - cs.used_border_bottom()
                                      - inset_y * 2),
                };
                painter.fill_rect(swatch, detail::unpack_rgba(rgba));
            }
        }

        if (b.tag == "textarea" &&
            cs.resize != detail::ComputedStyle::Resize::None) {
            const float x1 = static_cast<float>(
                eff.x + eff.w - cs.used_border_right() - 4);
            const float y1 = static_cast<float>(
                eff.y + eff.h - cs.used_border_bottom() - 4);
            const Color grip = detail::unpack_rgba(an.color_rgba);
            painter.stroke_line(x1 - 10.0f, y1, x1, y1 - 10.0f, grip, 1.0f);
            painter.stroke_line(x1 - 5.0f, y1, x1, y1 - 5.0f, grip, 1.0f);
        }

        if (b.tag == "input" && b.input_type == "range") {
            const double min_attr = detail::block_attr_double(b, "min", 0.0);
            double max_attr = detail::block_attr_double(b, "max", 100.0);
            if (max_attr <= min_attr) max_attr = min_attr + 1.0;
            const double value_attr =
                std::clamp(detail::block_attr_double(b, "value", min_attr),
                           min_attr, max_attr);
            const float t = static_cast<float>(
                (value_attr - min_attr) / (max_attr - min_attr));

            const float bx = static_cast<float>(eff.x);
            const float by = static_cast<float>(eff.y);
            const float bw = static_cast<float>(eff.w);
            const float bh = static_cast<float>(eff.h);
            const float cy = by + bh * 0.5f;
            const float thumb_r = std::clamp(bh * 0.34f, 5.0f, 10.0f);
            const float x0 = bx + thumb_r;
            const float x1 = bx + std::max(thumb_r, bw - thumb_r);
            const float thumb_x = x0 + (x1 - x0) * t;
            const float track_h = detail::block_has_class(b, "form-range") ? 8.0f : 6.0f;
            const Rect track{
                static_cast<int>(std::round(x0)),
                static_cast<int>(std::round(cy - track_h * 0.5f)),
                std::max(1, static_cast<int>(std::round(x1 - x0))),
                std::max(1, static_cast<int>(std::round(track_h))),
            };
            const Rect fill{
                track.x,
                track.y,
                std::max(1, static_cast<int>(std::round(thumb_x - x0))),
                track.h,
            };

            const Color track_color =
                detail::block_has_class(b, "form-range")
                    ? Color{0xDE, 0xE2, 0xE6, 0xFF}
                    : Color{0xB8, 0xC0, 0xCC, 0xFF};
            const Color fill_color =
                detail::block_has_class(b, "form-range")
                    ? Color{0x0D, 0x6E, 0xFD, 0xFF}
                    : detail::unpack_rgba(an.color_rgba);
            const Color thumb_color =
                detail::block_has_class(b, "form-range")
                    ? Color{0x0D, 0x6E, 0xFD, 0xFF}
                    : detail::unpack_rgba(an.color_rgba);

            painter.fill_rounded_rect(track, track_h * 0.5f, track_color);
            painter.fill_rounded_rect(fill, track_h * 0.5f, fill_color);
            painter.fill_circle(thumb_x, cy, thumb_r, thumb_color);
        }
        }  // phase == BlockPaintPhase::Text

        // ── PHASE: overlay ───────────────────────────────────────────
        // Scrollbar thumb, on top of this stacking context's own content
        // (boxes + text painted above) but under later/higher contexts —
        // the old global draw-last pass put every pane's thumb over
        // overlapping floating panels. The geometry is already in VISUAL
        // space (block_border_visual_rect applies effective_transform_for),
        // so it must draw with the block's transform popped — drawing it
        // transformed applied the drag translation twice and the thumb
        // diverged from its pane as a float moved.
#if !defined(AFFINEUI_STUB_BUILD)
        if (phase == BlockPaintPhase::Overlay) {
            ScrollbarGeometry scrollbar{};
            if (detail::vertical_scrollbar_geometry(
                    *impl_, static_cast<int>(i), scrollbar)) {
                if (has_transform) painter.pop_transform();
                // Catppuccin overlay0-ish, semi-transparent.
                painter.fill_rounded_rect(
                    scrollbar.thumb, 3.0f, Color{0x9c, 0xa0, 0xb0, 0xC0});
                if (has_transform) painter.push_transform(paint_transform);
            }
        }
#endif

        if (has_opacity) painter.pop_alpha();
        if (clipped) painter.pop_clip();
        if (has_transform) painter.pop_transform();
    }
}
}  // namespace affineui
