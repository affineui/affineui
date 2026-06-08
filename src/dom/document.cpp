// affineui::Document â€” Phase 2A.
//
// Parses HTML with lexbor, attaches stylesheets (user-agent + user +
// any embedded `<style>` blocks) through lexbor's cascade, then for
// each block-level element collects a `ResolvedStyle` (ComputedStyle
// + AnimatedStyle) via the StyleResolver. The Phase 1 `style_for(tag)`
// fallback is gone â€” real CSS now drives every visible attribute we
// expose this phase.
//
// Lifetime: the lxb_html_document_t and the resolver live for the
// lifetime of DocumentImpl. Element pointers in our Block list remain
// valid until the next set_html() (which replaces the document).
//
// What's intentionally still simple (Phase 2B-2E plan):
//   - One flat list of block-level elements; no nested boxes.
//   - Painter is invoked directly each frame (no DisplayList yet).
//   - Resolver is uncached. We pay the walk on every set_html, never
//     per frame.

#include "affineui/document.h"

#include "affineui/painter.h"
#include "affineui/themes.h"
#include "imm/imm_runtime.h"
#include "internal/animated_style.h"
#include "internal/computed_style.h"
#include "internal/element_id.h"
#include "internal/style_resolver.h"
#include "internal/style_store.h"
#include "layout/yoga_adapter.h"

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
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
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

// A laid-out, paintable block. Style data lives in the Document's
// StyleStore (SoA); Block just carries the handle + the cheap, block-
// specific stuff (text content, computed bounds, tag for debugging).
//
// `parent_idx` is the index into Document::blocks of this block's
// containing block (or -1 for top-level). Blocks are appended in DFS
// order during collect_blocks(), so a parent always has a lower index
// than its children â€” that lets paint walk the vector linearly and
// hit parents before children (correct z-order for the box-bg-then-
// text emit pattern).
struct Block {
    detail::ElementId id{};        // StyleStore handle
    std::string       tag;
    std::string       elem_id;     // value of the `id` attribute (for "#x" selectors)
    std::vector<std::string> classes;  // tokenized `class` attribute
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string       text;
    std::string       image_src;
    std::string       placeholder;
    int               parent_idx{-1};
    Rect              bounds{};
    RectF             bounds_f{};
    // Scroll state. Only meaningful when ComputedStyle.overflow_y is
    // Scroll or Auto. content_h is the total height of descendant
    // content measured from this block's bounds.y â€” i.e. how far the
    // user can scroll before the bottom of the deepest descendant
    // clears the visible window.
    int               scroll_y{0};
    int               content_h{0};
    // Synthetic line-box: a Yoga flex-row wrapper inserted by
    // collect_blocks around runs of inline / inline-block siblings.
    // No DOM element backs it; paint skips bg/border/text (it's
    // transparent above its children). Click routing + hit-test
    // treat it normally â€” children sit on top anyway.
    bool              synthetic{false};
    bool              text_control{false};
    bool              placeholder_visible{false};
    std::string       text_value;       // unmasked live value for editable controls
    std::size_t       caret_offset{0};  // byte offset into text_value
    std::size_t       selection_anchor{0};
    std::size_t       selection_focus{0};
    // Input-control UA paint hints. Populated for <input> elements at
    // collect time; empty / false for all other elements.
    std::string       input_type;       // "checkbox" / "radio" / ""
    std::string       role_attr;        // "switch" etc.
    bool              is_checked{false};
    bool              is_disabled{false};
    std::shared_ptr<const detail::CustomPropMap> custom_props;
    std::shared_ptr<const detail::BoxShadowList> box_shadows;
    std::array<detail::GridTrackHint, detail::kMaxGridTrackHints> grid_columns{};
    std::uint8_t grid_column_count{0};
    detail::AnimatedStyle base_animated{};
    detail::ResolvedStyle::CssAnimation animation{};
    std::chrono::steady_clock::time_point animation_epoch{
        std::chrono::steady_clock::now()};
};

struct TextLayoutEntry {
    std::uint64_t signature{0};
    std::vector<std::size_t> caret_offsets;
    std::vector<float> caret_x;
    std::vector<std::uint16_t> caret_lines;
    std::vector<float> line_widths;
    float css_line_height{1.0f};
    float natural_line_height{1.0f};
    int text_x{0};
    int text_y{0};
    float content_w{1.0f};
    Painter::TextAlign align{Painter::TextAlign::Left};
    bool nowrap{false};
};

struct TextControlGeometry {
    std::uint32_t font{0};
    int text_x{0};
    int text_y{0};
    float content_w{1.0f};
    float letter_spacing_px{0.0f};
    float line_height_mult{1.0f};
    Painter::TextAlign align{Painter::TextAlign::Left};
    bool nowrap{false};
};

std::uint64_t fnv1a_64_bytes(std::uint64_t h,
                             const void* data,
                             std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= static_cast<std::uint64_t>(bytes[i]);
        h *= 1099511628211ull;
    }
    return h;
}

template <class T>
void hash_mix(std::uint64_t& h, const T& value) {
    h = fnv1a_64_bytes(h, &value, sizeof(value));
}

void hash_mix_string(std::uint64_t& h, std::string_view value) {
    h = fnv1a_64_bytes(h, value.data(), value.size());
    const std::uint64_t size = static_cast<std::uint64_t>(value.size());
    hash_mix(h, size);
}

#if !defined(AFFINEUI_STUB_BUILD)
// CSS pseudo-class side-table entry, parsed out of attached
// stylesheets by scan_pseudo_rules(). Each compound is the AND of
// one-or-more simple identifiers (tag/class/id/attribute). The chain is
// `target` (the element receiving declarations) plus zero-or-more
// `ancestors` walked deepest â†’ root with descendant-combinator gaps.
//
// Today's grammar: simple selectors + descendant combinator only.
// `>`, `+`, `~`, and functional pseudos are silently skipped at scan time.
struct SimpleSelector {
    enum class Kind : std::uint8_t { Tag, Class, Id, Attr };
    Kind        kind;
    std::string name;
    std::string value;
    bool        attr_value_set{false};
};

struct CompoundSelector {
    std::vector<SimpleSelector> simples;  // AND'd together
};

struct PseudoRule {
    enum class Pseudo : std::uint8_t { Hover, Active, Focus };
    Pseudo                                        pseudo;
    CompoundSelector                              target;
    std::vector<CompoundSelector>                 ancestors;  // nearest â†’ root
    CompoundSelector                              state_target;
    bool                                          state_on_target{true};
    const lxb_css_rule_declaration_list_t*        decls;
};

// FontFill carries font-family values we recover via a raw-text
// pre-scan of each attached stylesheet, keyed by the same
// CompoundSelector chain we use for pseudo overlays. The resolver
// still maps font-family through AffineUI's font registry here.
struct RuleFill {
    CompoundSelector              target;
    std::vector<CompoundSelector> ancestors;
    lxb_css_selector_specificity_t specificity{0};
    std::uint32_t                 source_order{0};
    std::string                   font_family;            // empty = unset
    std::array<detail::GridTrackHint, detail::kMaxGridTrackHints> grid_columns{};
    std::uint8_t                  grid_column_count{0};
    detail::ComputedStyle::Resize resize{
        detail::ComputedStyle::Resize::None};
    bool                          has_resize{false};
    // `cursor` recovered from a rule (the cascade only resolves cursor from
    // inline styles otherwise, so rule-based cursors — splitter col-resize,
    // combo/chip ew-resize, … — would never show).
    detail::ComputedStyle::Cursor cursor{detail::ComputedStyle::Cursor::Default};
    bool                          has_cursor{false};
    // When non-zero, this fill is gated on the named pseudo-class
    // state bit (kHoverStateBit / kActiveStateBit / kFocusStateBit)
    // being set on the matched element.
    std::uint8_t                  state_bit{0};
};

detail::ComputedStyle::Cursor parse_cursor_keyword(std::string_view kw);

// Generated-content rules for `::before` / `::after`. Lexbor owns the
// declaration parsing; this side table records pseudo-element selectors so
// collect_blocks can materialize generated inline boxes into normal layout.
struct GeneratedContentRule {
    enum class Position : std::uint8_t { Before, After };

    Position                       position{Position::Before};
    CompoundSelector               target;
    std::vector<CompoundSelector>  ancestors;
    bool                           has_previous_adjacent{false};
    CompoundSelector               previous_adjacent;
    lxb_css_selector_specificity_t specificity{0};
    std::uint32_t                  source_order{0};
    std::string                    content_value;
    std::string                    color_value;
    std::string                    background_value;
    std::string                    background_color_value;
    std::string                    padding_left_value;
    std::string                    padding_right_value;
    const lxb_css_rule_declaration_list_t* decls{nullptr};
};

struct FontFaceSource {
    std::string url;
    std::string format;
};

struct FontFaceRule {
    std::string                 family;
    int                         weight{400};
    bool                        italic{false};
    std::vector<FontFaceSource> sources;
    bool                        loaded{false};
    bool                        attempted{false};
};

// Per-element state bits in StyleStore::state_bits().
constexpr std::uint8_t kHoverStateBit  = 1u << 0;
constexpr std::uint8_t kActiveStateBit = 1u << 1;
constexpr std::uint8_t kFocusStateBit  = 1u << 2;

// A CSS @media block whose nested rules apply only when the viewport
// width satisfies the (min-width)/(max-width) constraints. Both
// min_width_px and max_width_px use -1 to mean "unset / unconstrained".
// block_css is the raw text of the rules inside the braces (not including
// the braces themselves) â€” ready to pass to lxb_css_stylesheet_parse as
// a standalone stylesheet.
struct MediaBlock {
    int         min_width_px{-1};
    int         max_width_px{-1};
    std::string block_css;

    bool matches(int viewport_px) const {
        if (min_width_px >= 0 && viewport_px < min_width_px) return false;
        if (max_width_px >= 0 && viewport_px > max_width_px) return false;
        return true;
    }
};

struct KeyframeStep {
    float offset{0.0f};
    const lxb_css_rule_declaration_list_t* decls{nullptr};
};

struct KeyframeBlock {
    std::uint32_t name_hash{0};
    std::vector<KeyframeStep> steps;
};

enum class LiveControlKind : std::uint8_t {
    None,
    RangeInput,
    NumericInput,
    TextAreaResize,
    AuiKnob,
    DeciusSlider,
    DeciusFader,
    DeciusKnob,
};

void scan_font_face_rules(std::string_view css,
                          std::string_view stylesheet_base_url,
                          std::vector<FontFaceRule>& out);
#endif

}  // namespace

namespace detail {

#if !defined(AFFINEUI_STUB_BUILD)
struct DomWeakSlot {
    lxb_dom_node_t* node{nullptr};
    std::uint32_t generation{1};
};
#else
struct DomWeakSlot {
    std::uint32_t generation{1};
};
#endif

std::uint32_t next_document_id() {
    static std::atomic<std::uint32_t> next{1};
    std::uint32_t id = next.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) id = next.fetch_add(1, std::memory_order_relaxed);
    return id == 0 ? 1 : id;
}

struct DocumentImpl {
    DocumentImpl() : document_id(next_document_id()) {}

    std::string               html;
    std::string               user_stylesheet;
    std::string               user_stylesheet_base_url;  // resolves its url()s
    ResourceLoader            resource_loader;
    Document::ClipboardGet    clipboard_get;
    Document::ClipboardSet    clipboard_set;
    std::string               fallback_clipboard;
    Size                      content_size{0, 0};
    std::vector<Block>        blocks;
    bool                      paint_dirty{true};  // Phase 2C flips this
    std::vector<Rect>         dirty_rects;
    std::vector<int>          pending_dirty_roots;

    // Interaction state. -1 = no block (off-window or pointer not down).
    // Updated by Document::dispatch; read by App to drive cursor +
    // :hover / :active and click routing. The *_chain vectors hold the
    // deepest â†’ root block indices for the currently-hovered (resp.
    // -pressed) element. Recomputed on every relevant event; diffed
    // against the previous chain to toggle the pseudo state bit per
    // affected element.
    int                       hovered_idx{-1};
    int                       active_idx{-1};
    int                       focused_idx{-1};
    std::vector<int>          hovered_chain;
    std::vector<int>          active_chain;
    Point                     last_mouse_pos{};
    bool                      ui_control_script_attached{false};
    std::vector<std::string>  activated_widgets;
    std::vector<Document::WidgetChange> changed_widgets;
    bool                      mouse_down_consumed_release{false};
    // Runtime dock-placement overrides (panel id -> placement) produced by
    // drag-to-dock / tearoff interactions. Survives view reloads (the app reads
    // them back to re-seed resolve_dock). See Document::dock_override.
    std::unordered_map<std::string, Document::DockPlacement> dock_overrides;
    std::unordered_map<std::string, std::string> dock_active_tabs;

    struct ScrollbarDrag {
        int block_idx{-1};
        int start_y{0};
        int start_scroll_y{0};
        int thumb_offset_y{0};
    } scrollbar_drag;

#if !defined(AFFINEUI_STUB_BUILD)
    // A dock splitter being dragged. The splitter sits between two flex
    // siblings (prev/next pane); dragging shifts the shared size budget
    // between them by setting their inline flex-basis. Mirrors decius.js
    // initSplitter (data-dcs-splitter): axis from --h, minPx=24, budget =
    // prev_size + next_size held constant.
    struct SplitterDrag {
        int                block_idx{-1};   // the splitter block
        lxb_dom_element_t* prev{nullptr};
        lxb_dom_element_t* next{nullptr};
        bool               horizontal{false};  // row-resize (vertical dock)
        int                start_pos{0};        // pointer coord on the axis
        int                prev_size{0};         // prev pane size at grab
        int                next_size{0};         // next pane size at grab
        int                budget{0};            // prev_size + next_size
        int                min_px{24};
        // Which side is the flex-grow pane (the center/document or a nested
        // dock). A splitter must only PIN the fixed side and leave the grower
        // as flex:1, or dragging freezes the grower at a fixed pixel size and
        // the layout stops filling the window (dead band on the far edge).
        bool               prev_grows{false};
        bool               next_grows{false};
        bool               persist_layout{false};
    } splitter_drag;

    // A floating element (toolbar / torn-off panel) being dragged by its handle.
    // Moving a [data-dcs-drag-handle] repositions the whole [data-dcs-drag]
    // container via inline left/top, clamped to its [data-dcs-drag-bounds]
    // container. Mirrors decius drag; the position persists on release via
    // result.layout_changed, like the splitter. cb_* is the containing-block
    // origin in document space, derived at grab from (doc pos - inline left/top)
    // so the math works regardless of which ancestor is the offset parent.
    struct FloatDrag {
        lxb_dom_element_t* elem{nullptr};   // the moved [data-dcs-drag] container
        int                start_x{0};      // pointer at grab
        int                start_y{0};
        int                elem_doc_x{0};   // element border-box doc pos at grab
        int                elem_doc_y{0};
        int                cb_x{0};         // containing-block origin (doc space)
        int                cb_y{0};
        int                elem_w{0};
        int                elem_h{0};
        int                bounds_x{0};     // clamp rect (doc space);
        int                bounds_y{0};     //   w/h == 0 -> move unconstrained
        int                bounds_w{0};
        int                bounds_h{0};
        std::string        panel_id;        // data-dcs-dock-id (empty = not a
                                            //   dockable, e.g. a float toolbar)
    } float_drag;

    // A dock-pane tab pressed and possibly being dragged. The tab switches on
    // mouse-down; if the press turns into a drag, the release docks or tears
    // off the already-selected panel.
    struct TabDrag {
        lxb_dom_element_t* tab{nullptr};    // the grabbed tab button
        lxb_dom_element_t* pane{nullptr};   // its enclosing .dcs-dockpane
        std::string        panel_id;        // dockpanel id (target body minus -body)
        int                start_x{0};
        int                start_y{0};
        bool               dragging{false}; // moved past the threshold
        bool               drop_valid{false};
        std::string        drop_parent;
        int                drop_zone{0};
        int                drop_x{0};
        int                drop_y{0};
        int                drop_w{0};
        int                drop_h{0};
        bool               drop_indicator_visible{false};
        bool               switched_on_down{false};
    } tab_drag;

    struct PendingTabPress {
        std::string panel_id;
        int         start_x{0};
        int         start_y{0};
        bool        switched_on_down{false};
    } pending_tab_press;

    lxb_dom_element_t* pressed_dcs_menu_item{nullptr};
    bool               pressed_dcs_menu_item_was_active{false};
    Rect               pressed_dcs_menu_item_bounds{};
    lxb_dom_element_t* pressed_button{nullptr};

    // A colorfield chip being drag-scrubbed. Snapshot HSV at grab; horizontal
    // drag = hue (1°/px), Ctrl+horizontal = saturation, vertical = value. The
    // canonical Decius color-chip contract (decius regression — app-supplied in
    // the web world, first-class here).
    struct ColorfieldDrag {
        lxb_dom_element_t* field{nullptr};  // the dcs-colorfield
        lxb_dom_element_t* chip{nullptr};
        int                start_x{0};
        int                start_y{0};
        double             h{0.0};  // snapshot HSV
        double             s{0.0};
        double             v{0.0};
    } colorfield_drag;

    struct LiveControlDrag {
        LiveControlKind  kind{LiveControlKind::None};
        lxb_dom_element_t* elem{nullptr};
        int              block_idx{-1};
        Rect             bounds{};
        double           min{0.0};
        double           max{1.0};
        double           start_value{0.0};
        double           step{0.01};
        int              start_x{0};
        int              start_y{0};
        int              start_w{0};
        int              start_h{0};
        int              last_x{0};
        bool             bipolar{false};
        bool             bounded{false};
        bool             moved{false};
        bool             resize_x{false};
        bool             resize_y{false};
        bool             defer_text_focus{false};
        int              focus_idx{-1};
        Point            focus_point{};
    } live_drag;
#endif

    // Immediate-mode runtime â€” lazily created on the first
    // set_imm_view() call. Holds state slots, click handlers, and the
    // view function across re-renders.
    std::unique_ptr<ImmRuntime> imm;

    // Per-element style + dirty bookkeeping. Lives across set_html()
    // calls; reset() inside set_html() recycles capacity.
    StyleStore                style_store;

    // Viewport dimensions used for @media query evaluation and viewport
    // units (`vw`, `vh`, `vmin`, `vmax`). 0 = unknown. Updated by
    // layout() before calling set_html() when the viewport changes, so
    // attach_stylesheet() and the resolver see the current CSS viewport
    // during the re-parse triggered by layout().
    int                       media_viewport_width_px{0};
    int                       media_viewport_height_px{0};

#if !defined(AFFINEUI_STUB_BUILD)
    lxb_html_document_t*               doc{nullptr};
    lxb_dom_event_remove_f             lexbor_ev_remove{nullptr};
    lxb_dom_event_destroy_f            lexbor_ev_destroy{nullptr};
    std::vector<lxb_css_stylesheet_t*> sheets;
    // :hover / :active overlay rules â€” populated by scan_pseudo_rules()
    // during attach. Pointers in `decls` reference rule data owned by
    // the document's CSS memory pool; valid for the document's lifetime.
    std::vector<PseudoRule>            pseudo_rules;
    // Font-family fill rules populated by scan_rule_fills() from the raw
    // CSS source at attach time.
    std::vector<RuleFill>              rule_fills;
    // Generated ::before / ::after content rules populated from the raw
    // CSS source and matched during DOM block collection.
    std::vector<GeneratedContentRule>   generated_content_rules;
    std::vector<FontFaceRule>           font_faces;
    std::unordered_map<lxb_dom_node_t*, std::string> live_text_values;
    std::unordered_map<lxb_dom_node_t*, std::size_t> live_text_carets;
    std::unordered_map<lxb_dom_node_t*,
                       std::pair<std::size_t, std::size_t>>
        live_text_selections;
    std::unordered_map<std::uint64_t, TextLayoutEntry> text_layout_cache;
    std::unordered_map<lxb_dom_node_t*, std::uint64_t> text_layout_signatures;
    Painter* last_measurer{nullptr};
    struct UserTextAreaSize {
        int width{-1};
        int height{-1};
    };
    std::unordered_map<lxb_dom_node_t*, UserTextAreaSize> user_textarea_sizes;
    std::unordered_map<lxb_dom_node_t*, lxb_dom_node_t*> dcs_select_anchors;
    int                                       text_selection_drag_idx{-1};
    std::size_t                               text_selection_drag_anchor{0};
    bool                                      last_text_click_valid{false};
    int                                       last_text_click_idx{-1};
    Point                                     last_text_click_pos{};
    std::chrono::steady_clock::time_point     last_text_click_time{};
    // @media blocks collected during attach_stylesheet. Evaluated against
    // media_viewport_width_px; matching blocks are re-attached as extra
    // stylesheets so their rules participate in the normal cascade.
    std::vector<MediaBlock>            media_blocks;
    std::uint64_t                      media_match_signature{0};
    std::vector<KeyframeBlock>         keyframes;
    std::unique_ptr<StyleResolver>     resolver;
    ResolvedStyle                      root_style{};  // inheritance root
#endif
    std::chrono::steady_clock::time_point animation_epoch{
        std::chrono::steady_clock::now()};
    std::uint32_t              animation_candidate_count{0};

    // Paint-pass scratch. Document::draw is on the hot path for live
    // control interaction and CSS animation sampling, so keep these
    // buffers retained instead of allocating them every frame.
    std::vector<int>           draw_child_counts;
    std::vector<int>           draw_first_child_indices;
    std::vector<int>           draw_list_ordinals;
    std::vector<int>           draw_list_counts_by_parent;
    std::vector<int>           draw_paint_order;

    std::uint32_t              document_id{1};
    std::vector<DomWeakSlot>   dom_weak_slots;

    ~DocumentImpl() {
#if !defined(AFFINEUI_STUB_BUILD)
        resolver.reset();
        // Stylesheets are owned by the document's CSS memory pool once
        // attached â€” destroying the document tears them down. Just drop
        // our tracking refs.
        sheets.clear();
        if (doc) lxb_html_document_destroy(doc);
#endif
    }
};

#if !defined(AFFINEUI_STUB_BUILD)

void advance_generation(DomWeakSlot& slot) {
    ++slot.generation;
    if (slot.generation == 0) slot.generation = 1;
}

void invalidate_dom_weak_slot(DocumentImpl& impl, lxb_dom_node_t* node) {
    if (node == nullptr) return;
    impl.live_text_values.erase(node);
    impl.live_text_carets.erase(node);
    impl.live_text_selections.erase(node);
    impl.user_textarea_sizes.erase(node);
    impl.dcs_select_anchors.erase(node);
    for (auto it = impl.dcs_select_anchors.begin();
         it != impl.dcs_select_anchors.end(); ) {
        if (it->second == node) {
            it = impl.dcs_select_anchors.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& slot : impl.dom_weak_slots) {
        if (slot.node == node) {
            slot.node = nullptr;
            advance_generation(slot);
        }
    }
}

DocumentImpl* document_impl_from_node(lxb_dom_node_t* node) {
    if (node == nullptr || node->owner_document == nullptr) return nullptr;
    return static_cast<DocumentImpl*>(node->owner_document->user);
}

lxb_status_t affineui_dom_event_remove(lxb_dom_node_t* node) {
    auto* impl = document_impl_from_node(node);
    if (impl != nullptr) {
        invalidate_dom_weak_slot(*impl, node);
        if (impl->lexbor_ev_remove != nullptr) {
            return impl->lexbor_ev_remove(node);
        }
    }
    return LXB_STATUS_OK;
}

lxb_status_t affineui_dom_event_destroy(lxb_dom_node_t* node) {
    auto* impl = document_impl_from_node(node);
    if (impl != nullptr) {
        invalidate_dom_weak_slot(*impl, node);
        if (impl->lexbor_ev_destroy != nullptr) {
            return impl->lexbor_ev_destroy(node);
        }
    }
    return LXB_STATUS_OK;
}

void install_dom_event_hooks(DocumentImpl& impl) {
    if (impl.doc == nullptr) return;
    auto& dom_doc = impl.doc->dom_document;
    impl.lexbor_ev_remove = dom_doc.ev_remove;
    impl.lexbor_ev_destroy = dom_doc.ev_destroy;
    dom_doc.user = &impl;
    dom_doc.ev_remove = affineui_dom_event_remove;
    dom_doc.ev_destroy = affineui_dom_event_destroy;
}

#endif

}  // namespace detail

namespace {

#if !defined(AFFINEUI_STUB_BUILD)

// â”€â”€ DOM utilities â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

std::uint64_t media_match_signature(const detail::DocumentImpl& impl,
                                    int viewport_width) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    mix(static_cast<std::uint64_t>(impl.media_blocks.size()));
    for (const auto& block : impl.media_blocks) {
        mix(block.matches(viewport_width) ? 1u : 0u);
    }
    return h;
}

bool is_html_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\r' || c == '\f' || c == '\v';
}

// Extract text content from a DOM node. Behaviour depends on the
// CSS white-space mode inherited by the element:
//
//   Normal (default): collapse all whitespace runs to a single space,
//     trim leading/trailing. Wrap is determined by container width.
//
//   Pre / PreLine / PreWrap: preserve spaces and newlines literally.
//     Tabs are converted to a single space (we don't compute tab stops).
//     NanoVG's nvgTextBox honours '\n' as a hard line break, so the
//     painter naturally renders multi-line pre content correctly.
//
//   Nowrap: same collapsing as Normal; wrap is suppressed at paint/
//     measure time by passing a very large max-width.
std::string node_text(lxb_dom_node_t* node,
                      detail::ComputedStyle::WhiteSpace ws
                          = detail::ComputedStyle::WhiteSpace::Normal) {
    size_t len = 0;
    lxb_char_t* raw = lxb_dom_node_text_content(node, &len);
    if (!raw || len == 0) return {};

    using WS = detail::ComputedStyle::WhiteSpace;
    const bool preserve = (ws == WS::Pre || ws == WS::PreWrap ||
                           ws == WS::PreLine);

    if (preserve) {
        // Keep newlines; convert tabs to space; keep other chars as-is.
        std::string out;
        out.reserve(len);
        for (std::size_t i = 0; i < len; ++i) {
            const auto c = static_cast<unsigned char>(raw[i]);
            if (c == '\r') {
                // CR or CRLF â†’ LF (normalize line endings).
                out.push_back('\n');
                if (i + 1 < len && raw[i + 1] == '\n') ++i;
            } else if (c == '\t') {
                out.push_back(' ');
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    // Normal / Nowrap: collapse whitespace.
    std::string out;
    out.reserve(len);
    bool prev_was_ws = true;  // leading whitespace is dropped
    for (std::size_t i = 0; i < len; ++i) {
        const auto c = static_cast<unsigned char>(raw[i]);
        if (is_html_ws(c)) {
            if (!prev_was_ws) out.push_back(' ');
            prev_was_ws = true;
        } else {
            out.push_back(static_cast<char>(c));
            prev_was_ws = false;
        }
    }
    // Trim a trailing space we may have appended just before EOF.
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Apply text-transform to a string. All transforms operate on
// ASCII letters only â€” full Unicode case-mapping is out of scope.
// Capitalize uppercases the first character of each whitespace-
// delimited word (matching CSS spec word boundary definition for
// ASCII content).
std::string apply_text_transform(std::string s,
                                 detail::ComputedStyle::TextTransform tt) {
    using TT = detail::ComputedStyle::TextTransform;
    switch (tt) {
        case TT::Uppercase:
            for (char& c : s) c = static_cast<char>(std::toupper(
                static_cast<unsigned char>(c)));
            break;
        case TT::Lowercase:
            for (char& c : s) c = static_cast<char>(std::tolower(
                static_cast<unsigned char>(c)));
            break;
        case TT::Capitalize: {
            bool at_word_start = true;
            for (char& c : s) {
                if (is_html_ws(static_cast<unsigned char>(c))) {
                    at_word_start = true;
                } else {
                    if (at_word_start)
                        c = static_cast<char>(std::toupper(
                            static_cast<unsigned char>(c)));
                    at_word_start = false;
                }
            }
            break;
        }
        case TT::None:
        default:
            break;
    }
    return s;
}

bool node_is_collapsible_whitespace(lxb_dom_node_t* node) {
    if (!node || node->type != LXB_DOM_NODE_TYPE_TEXT) return false;

    size_t len = 0;
    lxb_char_t* raw = lxb_dom_node_text_content(node, &len);
    if (!raw || len == 0) return false;

    for (std::size_t i = 0; i < len; ++i) {
        if (!is_html_ws(static_cast<unsigned char>(raw[i]))) {
            return false;
        }
    }
    return true;
}

detail::ResolvedStyle anonymous_text_style(const detail::ResolvedStyle& parent) {
    detail::ResolvedStyle rs{};
    rs.animated.color_rgba           = parent.animated.color_rgba;
    rs.animated.text_decoration_rgba = parent.animated.text_decoration_rgba;
    rs.computed.font_size_px         = parent.computed.font_size_px;
    rs.computed.font_weight          = parent.computed.font_weight;
    rs.computed.font_style           = parent.computed.font_style;
    rs.computed.line_height_x100     = parent.computed.line_height_x100;
    rs.computed.font_id              = parent.computed.font_id;
    rs.computed.cursor               = parent.computed.cursor;
    // text-align is inherited; anonymous text runs must honour the
    // containing block's alignment so center/right/justify render
    // correctly on inline content.
    rs.computed.text_align           = parent.computed.text_align;
    rs.computed.display              = detail::ComputedStyle::Display::Inline;
    // Inherited text features â€” propagate from parent.
    rs.computed.letter_spacing_x100  = parent.computed.letter_spacing_x100;
    rs.computed.text_indent_value    = parent.computed.text_indent_value;
    rs.computed.text_indent_is_pct   = parent.computed.text_indent_is_pct;
    rs.computed.white_space          = parent.computed.white_space;
    rs.computed.text_transform       = parent.computed.text_transform;
    rs.computed.text_decoration_line = parent.computed.text_decoration_line;
    rs.custom_props                  = parent.custom_props;
    return rs;
}

void append_anonymous_inline_text(detail::DocumentImpl& impl,
                                  const detail::ResolvedStyle& parent_style,
                                  int parent_idx,
                                  std::string text) {
    if (text.empty()) return;

    const auto id = impl.style_store.acquire_synthetic();
    const auto rs = anonymous_text_style(parent_style);
    impl.style_store.computed(id) = rs.computed;
    impl.style_store.animated(id) = rs.animated;
    impl.style_store.dirty(id) &=
        static_cast<std::uint8_t>(~detail::StyleStore::DirtyStyle);

    Block b;
    b.id         = id;
    b.tag        = "#text";
    b.text       = std::move(text);
    b.parent_idx = parent_idx;
    b.box_shadows = rs.box_shadows;
    b.base_animated = rs.animated;
    b.animation = rs.animation;
    b.animation_epoch = impl.animation_epoch;
    impl.blocks.push_back(std::move(b));
}

int ensure_inline_run(detail::DocumentImpl& impl,
                      const detail::ResolvedStyle& parent_style,
                      int parent_idx,
                      int& open_synth_idx) {
    if (open_synth_idx >= 0) return open_synth_idx;

    using Display = detail::ComputedStyle::Display;

    const auto sid = impl.style_store.acquire_synthetic();
    auto& synth_cs = impl.style_store.computed(sid);
    synth_cs.display        = Display::Flex;
    synth_cs.flex_direction = detail::ComputedStyle::FlexDirection::Row;
    synth_cs.flex_wrap      = detail::ComputedStyle::FlexWrap::Wrap;
    switch (parent_style.computed.text_align) {
        case detail::ComputedStyle::TextAlign::Center:
            synth_cs.justify_content =
                detail::ComputedStyle::JustifyContent::Center;
            break;
        case detail::ComputedStyle::TextAlign::Right:
            synth_cs.justify_content =
                detail::ComputedStyle::JustifyContent::End;
            break;
        case detail::ComputedStyle::TextAlign::Left:
        case detail::ComputedStyle::TextAlign::Justify:
        default:
            synth_cs.justify_content =
                detail::ComputedStyle::JustifyContent::Start;
            break;
    }
    // Synthetic inline runs approximate a CSS line box. Inline-level boxes
    // align on their baselines by default; Yoga gets text baselines from the
    // adapter's metric callback, while inline-block containers synthesize
    // theirs from their first line.
    synth_cs.align_items    = detail::ComputedStyle::AlignItems::Baseline;

    Block synth;
    synth.id         = sid;
    synth.parent_idx = parent_idx;
    synth.synthetic  = true;
    impl.blocks.push_back(std::move(synth));
    open_synth_idx = static_cast<int>(impl.blocks.size()) - 1;
    return open_synth_idx;
}

std::string tag_name(lxb_dom_element_t* elem) {
    size_t len = 0;
    const lxb_char_t* name = lxb_dom_element_qualified_name(elem, &len);
    if (!name || len == 0) return {};
    return std::string(reinterpret_cast<const char*>(name), len);
}

// Pull the value of an attribute as a std::string, empty if absent.
std::string attr_string(lxb_dom_element_t* elem, std::string_view name) {
    size_t len = 0;
    const lxb_char_t* v = lxb_dom_element_get_attribute(
        elem,
        reinterpret_cast<const lxb_char_t*>(name.data()), name.size(),
        &len);
    if (!v || len == 0) return {};
    return std::string(reinterpret_cast<const char*>(v), len);
}

const lxb_char_t* as_lxb(std::string_view s) {
    return reinterpret_cast<const lxb_char_t*>(s.data());
}

std::vector<std::pair<std::string, std::string>>
element_attrs(lxb_dom_element_t* elem) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!elem) return out;
    for (auto* attr = lxb_dom_element_first_attribute(elem);
         attr != nullptr; attr = lxb_dom_element_next_attribute(attr)) {
        size_t name_len = 0;
        const lxb_char_t* name = lxb_dom_attr_local_name(attr, &name_len);
        if (!name || name_len == 0) continue;

        size_t value_len = 0;
        const lxb_char_t* value = lxb_dom_attr_value(attr, &value_len);
        out.emplace_back(
            std::string(reinterpret_cast<const char*>(name), name_len),
            value && value_len
                ? std::string(reinterpret_cast<const char*>(value), value_len)
                : std::string{});
    }
    return out;
}

// True if a (possibly value-less, boolean) attribute is present.
bool has_attr(lxb_dom_element_t* elem, std::string_view name) {
    return lxb_dom_element_has_attribute(
        elem, reinterpret_cast<const lxb_char_t*>(name.data()), name.size());
}

// The text a closed <select> shows: the `selected` <option>'s text, or
// the first option if none is marked. We render no popup/list â€” only the
// chosen option â€” matching a closed native control.
std::string select_display_text(lxb_dom_element_t* select) {
    lxb_dom_node_t* first = nullptr;
    for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(select));
         c != nullptr; c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* el = lxb_dom_interface_element(c);
        if (tag_name(el) != "option") continue;
        if (first == nullptr) first = c;
        if (has_attr(el, "selected")) return node_text(c);
    }
    return first ? node_text(first) : std::string{};
}

// Mask each visible character of a password value with a bullet (U+2022).
// Skips UTF-8 continuation bytes so multibyte characters mask one-for-one.
std::string mask_password(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if ((c & 0xC0) == 0x80) continue;
        out += "\xE2\x80\xA2";
    }
    return out;
}

bool input_type_accepts_text(std::string_view type) {
    return type.empty() ||
           type == "text" ||
           type == "password" ||
           type == "search" ||
           type == "email" ||
           type == "url" ||
           type == "tel" ||
           type == "number";
}

std::string text_control_display_value(const Block& block,
                                       std::string_view value) {
    if (block.tag == "input" && block.input_type == "password" &&
        !value.empty()) {
        return mask_password(value);
    }
    return std::string(value);
}

// Tokenize a class attribute on whitespace runs.
std::vector<std::string> split_classes(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
        if (i >= s.size()) break;
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\n') ++j;
        out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool block_has_class(const Block& block, std::string_view cls) {
    return std::find(block.classes.begin(), block.classes.end(), cls) !=
           block.classes.end();
}

const std::string* block_attr_value(const Block& block, std::string_view name) {
    for (const auto& [attr_name, attr_value] : block.attrs) {
        if (attr_name == name) return &attr_value;
    }
    return nullptr;
}

bool block_has_attr(const Block& block, std::string_view name) {
    return block_attr_value(block, name) != nullptr;
}

int block_attr_int(const Block& block,
                   std::string_view name,
                   int fallback,
                   int min_value,
                   int max_value) {
    const auto* value = block_attr_value(block, name);
    if (!value || value->empty()) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value->c_str(), &end, 10);
    if (end == value->c_str()) return fallback;
    return std::clamp(static_cast<int>(parsed), min_value, max_value);
}

struct ScrollStateEntry {
#if !defined(AFFINEUI_STUB_BUILD)
    lxb_dom_element_t* element{nullptr};
#endif
    std::string elem_id;
    std::string aui_name;
    std::string tag;
    int scroll_y{0};
};

std::vector<ScrollStateEntry> snapshot_scroll_state(
        const detail::DocumentImpl& impl,
        bool include_elements) {
    std::vector<ScrollStateEntry> out;
    for (const auto& block : impl.blocks) {
        if (block.scroll_y <= 0) continue;
        ScrollStateEntry entry{};
#if !defined(AFFINEUI_STUB_BUILD)
        if (include_elements) {
            entry.element = impl.style_store.element_of(block.id);
        }
#else
        (void)include_elements;
#endif
        entry.elem_id = block.elem_id;
        if (const auto* name = block_attr_value(block, "data-aui-name")) {
            entry.aui_name = *name;
        }
        entry.tag = block.tag;
        entry.scroll_y = block.scroll_y;
        out.push_back(std::move(entry));
    }
    return out;
}

void restore_scroll_state(detail::DocumentImpl& impl,
                          const std::vector<ScrollStateEntry>& state) {
    if (state.empty()) return;

    std::vector<bool> used(state.size(), false);
    for (auto& block : impl.blocks) {
        const auto* name = block_attr_value(block, "data-aui-name");
        std::size_t best = state.size();
        int best_score = 0;
#if !defined(AFFINEUI_STUB_BUILD)
        auto* element = impl.style_store.element_of(block.id);
#endif
        for (std::size_t i = 0; i < state.size(); ++i) {
            if (used[i]) continue;
            const auto& entry = state[i];
            int score = 0;
#if !defined(AFFINEUI_STUB_BUILD)
            if (entry.element != nullptr && entry.element == element) {
                score = 100;
            } else
#endif
            if (!entry.aui_name.empty() && name &&
                entry.aui_name == *name) {
                score = 80;
            } else if (!entry.elem_id.empty() &&
                       entry.elem_id == block.elem_id) {
                score = 70;
            }
            if (score > 0 && !entry.tag.empty() && entry.tag == block.tag) {
                score += 5;
            }
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        if (best == state.size()) continue;
        block.scroll_y = state[best].scroll_y;
        used[best] = true;
    }
}

double block_attr_double(const Block& block,
                         std::string_view name,
                         double fallback) {
    const auto* value = block_attr_value(block, name);
    if (!value || value->empty()) return fallback;

    char* end = nullptr;
    const double parsed = std::strtod(value->c_str(), &end);
    if (end == value->c_str()) return fallback;
    return parsed;
}

std::string compact_number(double value, int places = 2) {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%.*f", places, value);
    std::string out{buf};
    while (out.size() > 1 && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

std::string percent_string(double fraction) {
    return compact_number(std::clamp(fraction, 0.0, 1.0) * 100.0) + "%";
}

double normalized_control_value(double value, double min, double max) {
    if (max <= min) return 0.0;
    return std::clamp((value - min) / (max - min), 0.0, 1.0);
}

int nearest_block_with_tag(const std::vector<Block>& blocks,
                           int idx,
                           std::string_view tag) {
    while (idx >= 0 && static_cast<std::size_t>(idx) < blocks.size()) {
        const auto& block = blocks[static_cast<std::size_t>(idx)];
        if (block.tag == tag) return idx;
        idx = block.parent_idx;
    }
    return -1;
}

bool is_block_tag(const std::string& tag) {
    return tag == "h1" || tag == "h2" || tag == "h3" ||
           tag == "h4" || tag == "h5" || tag == "h6" ||
           tag == "p"  || tag == "div" ||
           tag == "section" || tag == "article" || tag == "header" ||
           tag == "footer"  || tag == "main"    || tag == "nav" ||
           tag == "aside"   || tag == "figure"  || tag == "figcaption" ||
           // Form-ish + a few common containers. We treat them as
           // block-level for layout â€” Phase 3 inline layout splits
           // these into their proper inline / inline-block flow.
           tag == "button" || tag == "input"  || tag == "textarea" ||
           tag == "select" || tag == "label"  || tag == "form" ||
           tag == "fieldset" || tag == "legend" ||
           tag == "ul"     || tag == "ol" ||
           tag == "li"     || tag == "a"      || tag == "span" ||
           tag == "img"    ||
           // CSS table model â€” each produces a box (display is set by the
           // UA stylesheet); the layout engine gives them table semantics.
           tag == "table"  || tag == "thead"  || tag == "tbody" ||
           tag == "tfoot"  || tag == "tr"     || tag == "td"    ||
           tag == "th"     || tag == "caption";
}

// â”€â”€ Stylesheet extraction â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// Walk the parsed DOM looking for author stylesheets and collect them
// in document order. Inline `<style>` blocks append their text. Linked
// stylesheets go through the embedder resource loader.

bool rel_includes_stylesheet(std::string_view rel) {
    std::string token;
    for (char rel_ch : rel) {
        const auto ch = static_cast<unsigned char>(rel_ch);
        if (std::isspace(ch)) {
            if (token == "stylesheet") return true;
            token.clear();
            continue;
        }
        token.push_back(static_cast<char>(std::tolower(ch)));
    }
    return token == "stylesheet";
}

std::string stylesheet_base_url(std::string_view href) {
    const auto slash = href.find_last_of("/\\");
    if (slash == std::string_view::npos) return {};
    return std::string(href.substr(0, slash + 1));
}

void collect_author_stylesheets(lxb_dom_node_t* node,
                                detail::DocumentImpl& impl,
                                std::string& out) {
    for (auto* c = lxb_dom_node_first_child(node); c;
         c = lxb_dom_node_next(c)) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            auto* el = lxb_dom_interface_element(c);
            const auto tag = tag_name(el);
            if (tag == "style") {
                size_t len = 0;
                if (auto* t = lxb_dom_node_text_content(c, &len); t && len) {
                    std::string_view css{
                        reinterpret_cast<const char*>(t), len};
                    scan_font_face_rules(css, {}, impl.font_faces);
                    out.append(css);
                    out.push_back('\n');
                }
                continue;  // skip <style>'s descendants
            }
            if (tag == "link" && impl.resource_loader) {
                const auto rel = attr_string(el, "rel");
                const auto href = attr_string(el, "href");
                if (!href.empty() && rel_includes_stylesheet(rel)) {
                    if (auto css = impl.resource_loader(href); !css.empty()) {
                        scan_font_face_rules(
                            css, stylesheet_base_url(href), impl.font_faces);
                        out.append(css);
                        out.push_back('\n');
                    }
                }
            }
        }
        collect_author_stylesheets(c, impl, out);
    }
}

// Parse + attach a single stylesheet string. Quietly tolerates parse
// failures so a malformed user stylesheet doesn't take the whole
// pipeline down.
// Walk one stylesheet's parsed rules and pull out :hover / :active
// rules we can apply via the overlay path. Today's grammar:
//   - one or more compounds joined by descendant combinator
//   - each compound is one or more simple selectors (tag/class/id) AND'd
//   - exactly one :hover or :active pseudo in the LAST compound (the
//     "target" â€” the element whose state flips the rule on)
// Anything else (`>`, `+`, `~`, attribute selectors, functional
// pseudos, the pseudo on a non-target compound) is silently skipped.
// One compound matches when every one of its simples matches.
bool compound_matches(const CompoundSelector& compound,
                      std::string_view tag,
                      std::string_view elem_id,
                      const std::vector<std::string>& classes,
                      const std::vector<std::pair<std::string, std::string>>*
                          attrs = nullptr) {
    for (const auto& s : compound.simples) {
        switch (s.kind) {
            case SimpleSelector::Kind::Tag:
                if (s.name != tag) return false; break;
            case SimpleSelector::Kind::Id:
                if (s.name != elem_id) return false; break;
            case SimpleSelector::Kind::Class:
                if (std::find(classes.begin(), classes.end(), s.name)
                    == classes.end()) return false;
                break;
            case SimpleSelector::Kind::Attr: {
                if (!attrs) return false;
                const auto it = std::find_if(
                    attrs->begin(), attrs->end(),
                    [&](const auto& a) { return a.first == s.name; });
                if (it == attrs->end()) return false;
                if (s.attr_value_set && it->second != s.value) return false;
                break;
            }
        }
    }
    return true;
}

int collapse_vertical_margins(int a, int b) {
    if (a >= 0 && b >= 0) return std::max(a, b);
    if (a <= 0 && b <= 0) return std::min(a, b);
    return a + b;
}

std::int16_t clamp_css_px(int value) {
    return static_cast<std::int16_t>(std::clamp(
        value,
        static_cast<int>(std::numeric_limits<std::int16_t>::min()),
        static_cast<int>(std::numeric_limits<std::int16_t>::max())));
}

bool is_block_flow_box(const Block& block,
                       const detail::ComputedStyle& style) {
    return !block.synthetic &&
           (style.display == detail::ComputedStyle::Display::Block ||
            style.display == detail::ComputedStyle::Display::ListItem);
}

bool is_flex_container_display(detail::ComputedStyle::Display display) {
    return display == detail::ComputedStyle::Display::Flex ||
           display == detail::ComputedStyle::Display::InlineFlex;
}

bool is_block_level_margin_box(const Block& block,
                               const detail::ComputedStyle& style) {
    return !block.synthetic &&
           (style.display == detail::ComputedStyle::Display::Block ||
            style.display == detail::ComputedStyle::Display::ListItem ||
            style.display == detail::ComputedStyle::Display::Flex ||
            style.display == detail::ComputedStyle::Display::Grid);
}

bool can_collapse_first_child_top_margin(
    const Block& parent_block,
    const detail::ComputedStyle& parent_style,
    const Block& child_block,
    const detail::ComputedStyle& child_style) {
    return is_block_flow_box(parent_block, parent_style) &&
           is_block_level_margin_box(child_block, child_style) &&
           parent_style.used_border_top() == 0 &&
           parent_style.padding_top == 0;
}

bool can_collapse_last_child_bottom_margin(
    const Block& parent_block,
    const detail::ComputedStyle& parent_style,
    const Block& child_block,
    const detail::ComputedStyle& child_style) {
    return is_block_flow_box(parent_block, parent_style) &&
           is_block_level_margin_box(child_block, child_style) &&
           parent_style.used_border_bottom() == 0 &&
           parent_style.padding_bottom == 0 &&
           parent_style.height < 0 &&
           parent_style.min_height == 0;
}

void collapse_block_flow_vertical_margins(
    const std::vector<std::vector<int>>& child_indices,
    const std::vector<Block>& blocks,
    std::vector<detail::ComputedStyle>& styles) {
    for (int pi = static_cast<int>(blocks.size()) - 1; pi >= 0; --pi) {
        const auto& kids = child_indices[static_cast<std::size_t>(pi)];
        if (kids.empty()) continue;

        const int first = kids.front();
        auto& parent_style = styles[static_cast<std::size_t>(pi)];
        auto& child_style = styles[static_cast<std::size_t>(first)];
        if (can_collapse_first_child_top_margin(
                blocks[static_cast<std::size_t>(pi)], parent_style,
                blocks[static_cast<std::size_t>(first)], child_style)) {
            parent_style.margin_top = clamp_css_px(collapse_vertical_margins(
                parent_style.margin_top, child_style.margin_top));
            child_style.margin_top = 0;
        }

        const int last = kids.back();
        auto& last_child_style = styles[static_cast<std::size_t>(last)];
        if (can_collapse_last_child_bottom_margin(
                blocks[static_cast<std::size_t>(pi)], parent_style,
                blocks[static_cast<std::size_t>(last)], last_child_style)) {
            parent_style.margin_bottom = clamp_css_px(collapse_vertical_margins(
                parent_style.margin_bottom, last_child_style.margin_bottom));
            last_child_style.margin_bottom = 0;
        }
    }

    auto collapse_sibling_run = [&](const std::vector<int>& kids) {
        int previous = -1;
        for (const int child : kids) {
            const auto child_idx = static_cast<std::size_t>(child);
            if (!is_block_level_margin_box(blocks[child_idx], styles[child_idx])) {
                previous = -1;
                continue;
            }

            if (previous >= 0) {
                auto& current_style = styles[child_idx];
                const auto& previous_style =
                    styles[static_cast<std::size_t>(previous)];
                const int collapsed = collapse_vertical_margins(
                    previous_style.margin_bottom, current_style.margin_top);
                current_style.margin_top = clamp_css_px(
                    collapsed - previous_style.margin_bottom);
            }
            previous = child;
        }
    };

    std::vector<int> root_children;
    root_children.reserve(blocks.size());
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].parent_idx < 0) root_children.push_back(static_cast<int>(i));
    }
    collapse_sibling_run(root_children);

    for (std::size_t pi = 0; pi < blocks.size(); ++pi) {
        const auto& parent_style = styles[pi];
        if (!is_block_flow_box(blocks[pi], parent_style)) continue;
        collapse_sibling_run(child_indices[pi]);
    }
}

// Walk up `parent_idx` through `blocks`, greedy-matching each ancestor
// compound in order. Returns true when all ancestors have been
// satisfied (gaps are allowed â€” descendant combinator semantics).
bool ancestor_chain_matches(const std::vector<CompoundSelector>& ancestors,
                            int parent_idx,
                            const std::vector<Block>& blocks) {
    std::size_t i = 0;
    int idx = parent_idx;
    while (i < ancestors.size() && idx >= 0) {
        const auto& a = blocks[static_cast<std::size_t>(idx)];
        if (compound_matches(ancestors[i], a.tag, a.elem_id, a.classes,
                             &a.attrs)) {
            ++i;
        }
        idx = a.parent_idx;
    }
    return i == ancestors.size();
}

std::uint8_t pseudo_state_bit(PseudoRule::Pseudo pseudo) {
    switch (pseudo) {
        case PseudoRule::Pseudo::Hover:  return kHoverStateBit;
        case PseudoRule::Pseudo::Active: return kActiveStateBit;
        case PseudoRule::Pseudo::Focus:  return kFocusStateBit;
        default:                         return 0;
    }
}

bool block_has_state(const detail::DocumentImpl& impl, const Block& block,
                     const CompoundSelector& state_target, std::uint8_t bit) {
    if (bit == 0) return false;
    if (!compound_matches(state_target, block.tag, block.elem_id,
                          block.classes, &block.attrs)) {
        return false;
    }
    return (impl.style_store.state_bits(block.id) & bit) != 0;
}

bool ancestor_has_state(const detail::DocumentImpl& impl, int parent_idx,
                        const CompoundSelector& state_target,
                        std::uint8_t bit) {
    if (bit == 0) return false;
    for (int idx = parent_idx; idx >= 0; ) {
        const auto& a = impl.blocks[static_cast<std::size_t>(idx)];
        if (compound_matches(state_target, a.tag, a.elem_id, a.classes,
                             &a.attrs) &&
            (impl.style_store.state_bits(a.id) & bit) != 0) {
            return true;
        }
        idx = a.parent_idx;
    }
    return false;
}

void apply_font_family_fills(detail::DocumentImpl& impl,
                             std::string_view tag,
                             std::string_view elem_id,
                             const std::vector<std::string>& classes,
                             int parent_idx,
                             std::uint8_t state_bits,
                             detail::ResolvedStyle& rs,
                             std::array<detail::GridTrackHint,
                                        detail::kMaxGridTrackHints>* grid_columns = nullptr,
                             std::uint8_t* grid_column_count = nullptr) {
    for (const auto& rf : impl.rule_fills) {
        // Pseudo-scoped fills only apply when the state bit is set.
        // Unscoped fills (state_bit == 0) always apply.
        if (rf.state_bit && !(state_bits & rf.state_bit)) continue;
        if (!compound_matches(rf.target, tag, elem_id, classes)) continue;
        if (!ancestor_chain_matches(rf.ancestors, parent_idx, impl.blocks))
            continue;
        if (!rf.font_family.empty()) {
            rs.computed.font_id = impl.style_store.intern_font_family(
                rf.font_family);
        }
        if (rf.has_resize) {
            rs.computed.resize = rf.resize;
        }
        if (rf.has_cursor) {
            rs.computed.cursor = rf.cursor;
        }
        if (rf.grid_column_count > 0 && grid_columns && grid_column_count) {
            *grid_column_count = rf.grid_column_count;
            *grid_columns = rf.grid_columns;
        }
    }
}

void apply_user_textarea_size(detail::DocumentImpl& impl,
                              lxb_dom_element_t* elem,
                              detail::ResolvedStyle& rs) {
    if (!elem) return;
    auto* elem_node = lxb_dom_interface_node(elem);
    const auto it = impl.user_textarea_sizes.find(elem_node);
    if (it == impl.user_textarea_sizes.end()) return;
    if (it->second.width > 0) {
        rs.computed.width = static_cast<std::int16_t>(
            std::min(it->second.width, 32767));
        rs.computed.width_pct_x100 = -1;
    }
    if (it->second.height > 0) {
        rs.computed.height = static_cast<std::int16_t>(
            std::min(it->second.height, 32767));
        rs.computed.height_pct = -1;
    }
}

void scan_pseudo_rules(lxb_css_stylesheet_t* sst,
                       std::vector<PseudoRule>& out) {
    if (!sst || !sst->root) return;
    auto* rule_list = lxb_css_rule_list(sst->root);
    for (auto* r = rule_list->first; r != nullptr; r = r->next) {
        if (r->type != LXB_CSS_RULE_STYLE) continue;
        auto* style = lxb_css_rule_style(r);

        // `selector` is a comma-separated chain of compound chains;
        // walk each group independently â€” each becomes its own rule.
        for (auto* sl = style->selector; sl != nullptr; sl = sl->next) {
            // Build the chain of compounds for this group.
            std::vector<CompoundSelector> compounds;
            CompoundSelector              current;
            PseudoRule::Pseudo            pseudo{};
            bool                          has_pseudo  = false;
            std::size_t                   pseudo_compound_index = 0;
            bool                          ok          = true;

            for (auto* sel = sl->first; sel != nullptr; sel = sel->next) {
                const bool starts_new_compound =
                    (sel != sl->first) &&
                    (sel->combinator != LXB_CSS_SELECTOR_COMBINATOR_CLOSE);
                if (starts_new_compound) {
                    if (sel->combinator != LXB_CSS_SELECTOR_COMBINATOR_DESCENDANT) {
                        // `>`, `+`, `~` â€” not in MVP grammar.
                        ok = false; break;
                    }
                    compounds.push_back(std::move(current));
                    current = {};
                }

                if (sel->type == LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS) {
                    if (has_pseudo) { ok = false; break; }
                    switch (sel->u.pseudo.type) {
                        case LXB_CSS_SELECTOR_PSEUDO_CLASS_HOVER:
                            pseudo = PseudoRule::Pseudo::Hover;
                            has_pseudo = true;
                            pseudo_compound_index = compounds.size();
                            break;
                        case LXB_CSS_SELECTOR_PSEUDO_CLASS_ACTIVE:
                            pseudo = PseudoRule::Pseudo::Active;
                            has_pseudo = true;
                            pseudo_compound_index = compounds.size();
                            break;
                        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FOCUS:
                            pseudo = PseudoRule::Pseudo::Focus;
                            has_pseudo = true;
                            pseudo_compound_index = compounds.size();
                            break;
                        default:
                            ok = false; break;
                    }
                    if (!ok) break;
                } else if (sel->type == LXB_CSS_SELECTOR_TYPE_ELEMENT ||
                           sel->type == LXB_CSS_SELECTOR_TYPE_CLASS   ||
                           sel->type == LXB_CSS_SELECTOR_TYPE_ID      ||
                           sel->type == LXB_CSS_SELECTOR_TYPE_ATTRIBUTE) {
                    SimpleSelector s;
                    switch (sel->type) {
                        case LXB_CSS_SELECTOR_TYPE_ELEMENT: s.kind = SimpleSelector::Kind::Tag;   break;
                        case LXB_CSS_SELECTOR_TYPE_CLASS:   s.kind = SimpleSelector::Kind::Class; break;
                        case LXB_CSS_SELECTOR_TYPE_ID:      s.kind = SimpleSelector::Kind::Id;    break;
                        case LXB_CSS_SELECTOR_TYPE_ATTRIBUTE:
                            s.kind = SimpleSelector::Kind::Attr;
                            if (sel->u.attribute.match !=
                                LXB_CSS_SELECTOR_MATCH_EQUAL) {
                                ok = false;
                            }
                            break;
                        default: ok = false; break;
                    }
                    if (!ok) break;
                    s.name.assign(
                        reinterpret_cast<const char*>(sel->name.data),
                        sel->name.length);
                    if (sel->type == LXB_CSS_SELECTOR_TYPE_ATTRIBUTE &&
                        sel->u.attribute.value.data != nullptr) {
                        s.value.assign(
                            reinterpret_cast<const char*>(
                                sel->u.attribute.value.data),
                            sel->u.attribute.value.length);
                        s.attr_value_set = true;
                    }
                    current.simples.push_back(std::move(s));
                } else {
                    ok = false; break;
                }
            }

            if (!ok || !has_pseudo) continue;
            // The current compound is the target. It must have at
            // least one identifier â€” `:hover { ... }` (universal)
            // alone is not supported in MVP.
            if (current.simples.empty()) continue;
            compounds.push_back(std::move(current));
            if (pseudo_compound_index >= compounds.size() ||
                compounds[pseudo_compound_index].simples.empty()) {
                continue;
            }

            PseudoRule pr;
            pr.pseudo = pseudo;
            pr.state_target = compounds[pseudo_compound_index];
            pr.state_on_target =
                (pseudo_compound_index == compounds.size() - 1);
            pr.target = std::move(compounds.back());
            compounds.pop_back();
            // compounds left over are the ancestor constraints, with
            // the OUTERMOST first in CSS source order. We want them
            // nearest â†’ root (reverse).
            pr.ancestors.reserve(compounds.size());
            for (auto it = compounds.rbegin(); it != compounds.rend(); ++it) {
                pr.ancestors.push_back(std::move(*it));
            }
            pr.decls = style->declarations;
            out.push_back(std::move(pr));
        }
    }
}

// â”€â”€ Font-family fill scanner â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// AffineUI keeps font-family names in a local registry. Until the main
// resolver maps lexbor's font-family values into that registry, this
// scanner builds a side-table of selector + first family name from the
// original CSS source. Selector grammar matches scan_pseudo_rules:
// simple selectors (tag / class / id) AND'd in compounds, compounds
// joined by descendant combinator.

// ASCII-only whitespace test we use throughout (CSS is ASCII for
// our purposes). Avoids std::isspace pulling in locale.
bool is_css_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
std::string_view rtrim_ws(std::string_view s) {
    while (!s.empty() && is_css_ws(s.back())) s.remove_suffix(1);
    return s;
}
std::string_view ltrim_ws(std::string_view s) {
    while (!s.empty() && is_css_ws(s.front())) s.remove_prefix(1);
    return s;
}
std::string_view trim_css_ws(std::string_view s) { return ltrim_ws(rtrim_ws(s)); }

// Chunk raw CSS into (selector, decls) rules. Handles `/* ... */`
// comments and skips `@`-prefixed at-rules. Doesn't try to validate
// the body â€” it's just collecting the source range so other helpers
// can scan inside it for the properties we care about.
struct RawRule { std::string_view selector; std::string_view decls; };

std::vector<RawRule> split_css_rules(std::string_view src) {
    std::vector<RawRule> out;
    std::size_t i = 0;
    auto skip_ws_and_comments = [&] {
        for (;;) {
            while (i < src.size() && is_css_ws(src[i])) ++i;
            if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
                i += 2;
                while (i + 1 < src.size() &&
                       !(src[i] == '*' && src[i + 1] == '/')) ++i;
                if (i + 1 < src.size()) i += 2;
            } else break;
        }
    };
    while (i < src.size()) {
        skip_ws_and_comments();
        if (i >= src.size()) break;
        if (src[i] == '@') {
            // Skip the at-rule. Either ends at ';' (descriptor form)
            // or wraps a `{ ... }` block we step over balanced.
            while (i < src.size() && src[i] != ';' && src[i] != '{') ++i;
            if (i < src.size() && src[i] == '{') {
                int depth = 1; ++i;
                while (i < src.size() && depth > 0) {
                    if (src[i] == '{') ++depth;
                    else if (src[i] == '}') --depth;
                    ++i;
                }
            } else if (i < src.size()) {
                ++i;
            }
            continue;
        }
        const std::size_t sel_start = i;
        while (i < src.size() && src[i] != '{') ++i;
        if (i >= src.size()) break;
        const auto sel = src.substr(sel_start, i - sel_start);
        ++i;  // skip '{'
        const std::size_t decl_start = i;
        int depth = 1;
        while (i < src.size() && depth > 0) {
            if (src[i] == '{') ++depth;
            else if (src[i] == '}' && --depth == 0) break;
            ++i;
        }
        if (i >= src.size()) break;
        const auto decls = src.substr(decl_start, i - decl_start);
        ++i;  // skip '}'
        out.push_back({sel, decls});
    }
    return out;
}

// Parse a static (no `>` / `+` / `~`) selector text into a target compound
// + ancestor chain matching the shape
// scan_pseudo_rules builds. A single trailing pseudo-class
// (:hover / :active / :focus) on the last compound is allowed and
// returned via `out_state_bit`; any other pseudo (including anywhere
// but the last compound) causes a parse failure. Returns false on
// anything we don't support (the rule's other properties still apply
// through lexbor; we just won't fill the missing ones).
bool parse_attribute_simple(std::string_view sel,
                            std::size_t& i,
                            CompoundSelector& compound) {
    if (i >= sel.size() || sel[i] != '[') return false;
    const std::size_t close = sel.find(']', i + 1);
    if (close == std::string_view::npos) return false;

    auto body = trim_css_ws(sel.substr(i + 1, close - i - 1));
    if (body.empty()) return false;

    SimpleSelector s;
    s.kind = SimpleSelector::Kind::Attr;

    const auto eq = body.find('=');
    if (eq == std::string_view::npos) {
        s.name = std::string(trim_css_ws(body));
    } else {
        const auto name = trim_css_ws(body.substr(0, eq));
        auto value = trim_css_ws(body.substr(eq + 1));
        if (name.empty() || value.empty()) return false;
        s.name = std::string(name);
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        s.value = std::string(value);
        s.attr_value_set = true;
    }
    if (s.name.empty() ||
        s.name.find_first_of("~|^$* \t\r\n\f") != std::string::npos) {
        return false;
    }

    compound.simples.push_back(std::move(s));
    i = close + 1;
    return true;
}

bool parse_static_selector(std::string_view sel,
                           CompoundSelector& target,
                           std::vector<CompoundSelector>& ancestors,
                           std::uint8_t& out_state_bit) {
    target = {};
    ancestors.clear();
    out_state_bit = 0;

    std::vector<CompoundSelector> compounds;
    bool pseudo_seen = false;
    std::size_t i = 0;
    while (i < sel.size()) {
        while (i < sel.size() && is_css_ws(sel[i])) ++i;
        if (i >= sel.size()) break;
        if (pseudo_seen) return false;  // pseudo must be on last compound
        CompoundSelector compound;
        // Optional leading tag (anything not starting with . # or :).
        if (sel[i] != '.' && sel[i] != '#' && sel[i] != ':' &&
            sel[i] != '[') {
            const std::size_t s = i;
            while (i < sel.size() && sel[i] != '.' && sel[i] != '#' &&
                   sel[i] != ':' && sel[i] != '[' &&
                   !is_css_ws(sel[i])) ++i;
            if (s == i) return false;
            compound.simples.push_back(
                {SimpleSelector::Kind::Tag, std::string(sel.substr(s, i - s))});
        }
        // Then any number of `.name` / `#name` segments, optionally
        // followed by a single `:pseudo` recognized below.
        while (i < sel.size() && !is_css_ws(sel[i])) {
            if (sel[i] == '[') {
                if (!parse_attribute_simple(sel, i, compound)) return false;
                continue;
            }
            if (sel[i] == ':') {
                if (pseudo_seen) return false;
                ++i;
                const std::size_t s = i;
                while (i < sel.size() && sel[i] != '.' && sel[i] != '#' &&
                       sel[i] != ':' && sel[i] != '[' &&
                       !is_css_ws(sel[i])) ++i;
                const auto name = sel.substr(s, i - s);
                if      (name == "hover")  out_state_bit = kHoverStateBit;
                else if (name == "active") out_state_bit = kActiveStateBit;
                else if (name == "focus")  out_state_bit = kFocusStateBit;
                else return false;  // unsupported pseudo
                pseudo_seen = true;
                continue;
            }
            if (sel[i] != '.' && sel[i] != '#') return false;
            const auto kind = (sel[i] == '.')
                ? SimpleSelector::Kind::Class
                : SimpleSelector::Kind::Id;
            ++i;
            const std::size_t s = i;
            while (i < sel.size() && sel[i] != '.' && sel[i] != '#' &&
                   sel[i] != ':' && sel[i] != '[' &&
                   !is_css_ws(sel[i])) ++i;
            if (s == i) return false;
            compound.simples.push_back(
                {kind, std::string(sel.substr(s, i - s))});
        }
        if (compound.simples.empty()) return false;
        compounds.push_back(std::move(compound));
    }
    if (compounds.empty()) return false;
    target = std::move(compounds.back());
    compounds.pop_back();
    ancestors.reserve(compounds.size());
    for (auto it = compounds.rbegin(); it != compounds.rend(); ++it) {
        ancestors.push_back(std::move(*it));
    }
    return true;
}

// Find `key: <value>` in a declaration list and return the full value
// up to the declaration semicolon. Parentheses and strings are honored
// so values like `var(--font, system-ui, sans-serif)` stay intact.
std::string find_decl_value(std::string_view decls,
                            std::string_view key) {
    std::size_t pos = 0;
    while (pos < decls.size()) {
        const auto kp = decls.find(key, pos);
        if (kp == std::string_view::npos) return {};
        const bool at_boundary =
            (kp == 0) || decls[kp - 1] == ';' || is_css_ws(decls[kp - 1]);
        if (!at_boundary) { pos = kp + 1; continue; }
        auto rest = decls.substr(kp + key.size());
        rest = ltrim_ws(rest);
        if (rest.empty() || rest.front() != ':') { pos = kp + 1; continue; }
        rest = ltrim_ws(rest.substr(1));

        std::size_t end = 0;
        int depth = 0;
        char quote = '\0';
        while (end < rest.size()) {
            const char c = rest[end];
            if (quote != '\0') {
                if (c == quote) quote = '\0';
                ++end;
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')' && depth > 0) {
                --depth;
            } else if (c == ';' && depth == 0) {
                break;
            }
            ++end;
        }
        return std::string(trim_css_ws(rest.substr(0, end)));
    }
    return {};
}

std::string strip_css_quotes(std::string tok) {
    if (tok.size() >= 2 &&
        ((tok.front() == '"'  && tok.back() == '"') ||
         (tok.front() == '\'' && tok.back() == '\''))) {
        tok = tok.substr(1, tok.size() - 2);
    }
    return tok;
}

int css_string_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

void append_utf8_codepoint(std::string& out, std::uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFFu ||
        (cp >= 0xD800u && cp <= 0xDFFFu)) {
        cp = 0xFFFDu;
    }
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string decode_css_string_escapes(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i >= s.size()) break;

        char escaped = s[i];
        if (escaped == '\r' || escaped == '\n' || escaped == '\f') {
            if (escaped == '\r' && i + 1 < s.size() && s[i + 1] == '\n')
                ++i;
            continue;
        }

        int digit = css_string_hex_digit(escaped);
        if (digit < 0) {
            out.push_back(escaped);
            continue;
        }

        std::uint32_t cp = static_cast<std::uint32_t>(digit);
        int count = 1;
        while (count < 6 && i + 1 < s.size()) {
            digit = css_string_hex_digit(s[i + 1]);
            if (digit < 0) break;
            cp = (cp << 4) | static_cast<std::uint32_t>(digit);
            ++i;
            ++count;
        }

        if (i + 1 < s.size() && is_css_ws(s[i + 1])) {
            ++i;
            if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n')
                ++i;
        }
        append_utf8_codepoint(out, cp);
    }
    return out;
}

std::size_t find_top_level_comma(std::string_view s) {
    int depth = 0;
    char quote = '\0';
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote != '\0') {
            if (c == quote) quote = '\0';
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(') ++depth;
        else if (c == ')' && depth > 0) --depth;
        else if (c == ',' && depth == 0) return i;
    }
    return std::string_view::npos;
}

std::string first_font_family(std::string_view value) {
    value = trim_css_ws(value);
    const auto comma = find_top_level_comma(value);
    if (comma != std::string_view::npos) value = value.substr(0, comma);
    const auto important = value.find('!');
    if (important != std::string_view::npos) {
        value = trim_css_ws(value.substr(0, important));
    }
    return strip_css_quotes(std::string(trim_css_ws(value)));
}

std::string css_keyword_value(std::string value) {
    value = std::string(trim_css_ws(value));
    const auto important = value.find('!');
    if (important != std::string::npos) {
        value = std::string(trim_css_ws(
            std::string_view(value).substr(0, important)));
    }
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::pair<detail::ComputedStyle::Resize, bool>
parse_resize_keyword(std::string value) {
    using R = detail::ComputedStyle::Resize;
    value = css_keyword_value(std::move(value));
    if (value == "none" || value == "initial") {
        return {R::None, true};
    }
    if (value == "both") {
        return {R::Both, true};
    }
    if (value == "horizontal" || value == "inline") {
        return {R::Horizontal, true};
    }
    if (value == "vertical" || value == "block") {
        return {R::Vertical, true};
    }
    return {R::None, false};
}

bool starts_with_ascii_ci(std::string_view s, std::string_view prefix);
std::size_t find_ascii_ci(std::string_view s, std::string_view needle,
                          std::size_t pos);

bool ends_with_ascii_ci(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    return starts_with_ascii_ci(s.substr(s.size() - suffix.size()), suffix);
}

std::string strip_css_important(std::string value) {
    const auto important = find_ascii_ci(value, "!", 0);
    if (important != std::string::npos) {
        value = std::string(trim_css_ws(
            std::string_view(value).substr(0, important)));
    }
    return value;
}

std::vector<std::string_view> split_css_top_level_ws(std::string_view value) {
    std::vector<std::string_view> out;
    std::size_t start = std::string_view::npos;
    int depth = 0;
    char quote = '\0';
    for (std::size_t i = 0; i <= value.size(); ++i) {
        const char c = i < value.size() ? value[i] : ' ';
        if (i < value.size()) {
            if (quote != '\0') {
                if (c == quote) quote = '\0';
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')' && depth > 0) {
                --depth;
            }
        }
        const bool sep = i == value.size() ||
            (depth == 0 && quote == '\0' &&
             is_css_ws(static_cast<unsigned char>(c)));
        if (!sep) {
            if (start == std::string_view::npos) start = i;
            continue;
        }
        if (start != std::string_view::npos) {
            auto tok = trim_css_ws(value.substr(start, i - start));
            if (!tok.empty()) out.push_back(tok);
            start = std::string_view::npos;
        }
    }
    return out;
}

bool parse_css_number(std::string_view s, float& out) {
    s = trim_css_ws(s);
    if (s.empty()) return false;
    std::string tmp(s);
    char* end = nullptr;
    out = std::strtof(tmp.c_str(), &end);
    return end != tmp.c_str();
}

std::string_view css_function_inner(std::string_view value,
                                    std::string_view name) {
    value = trim_css_ws(value);
    if (!starts_with_ascii_ci(value, name)) return {};
    auto rest = trim_css_ws(value.substr(name.size()));
    if (rest.size() < 2 || rest.front() != '(' || rest.back() != ')')
        return {};
    return rest.substr(1, rest.size() - 2);
}

bool parse_grid_track_token(std::string_view tok,
                            detail::GridTrackHint& out);

bool append_grid_template_tracks(
        std::string_view value,
        std::array<detail::GridTrackHint,
                   detail::kMaxGridTrackHints>& out,
        std::uint8_t& count) {
    bool appended = false;
    for (auto tok : split_css_top_level_ws(value)) {
        tok = trim_css_ws(tok);
        if (tok.empty()) continue;
        if (auto inner = css_function_inner(tok, "repeat"); !inner.empty()) {
            const auto comma = find_top_level_comma(inner);
            if (comma == std::string_view::npos) continue;
            float repeats_f = 0.0f;
            if (!parse_css_number(inner.substr(0, comma), repeats_f))
                continue;
            const int repeats = std::clamp(
                static_cast<int>(std::round(repeats_f)), 0, 32);
            auto pattern = inner.substr(comma + 1);
            std::array<detail::GridTrackHint,
                       detail::kMaxGridTrackHints> pattern_tracks{};
            std::uint8_t pattern_count = 0;
            if (!append_grid_template_tracks(pattern, pattern_tracks,
                                             pattern_count) ||
                pattern_count == 0) {
                continue;
            }
            for (int r = 0; r < repeats; ++r) {
                for (std::uint8_t i = 0; i < pattern_count; ++i) {
                    if (count >= detail::kMaxGridTrackHints) return true;
                    out[count++] = pattern_tracks[i];
                    appended = true;
                }
            }
            continue;
        }
        detail::GridTrackHint track{};
        if (!parse_grid_track_token(tok, track)) continue;
        if (count >= detail::kMaxGridTrackHints) return true;
        out[count++] = track;
        appended = true;
    }
    return appended;
}

bool parse_grid_track_token(std::string_view tok,
                            detail::GridTrackHint& out) {
    tok = trim_css_ws(tok);
    if (tok.empty()) return false;
    if (auto inner = css_function_inner(tok, "minmax"); !inner.empty()) {
        const auto comma = find_top_level_comma(inner);
        if (comma == std::string_view::npos) return false;
        return parse_grid_track_token(inner.substr(comma + 1), out);
    }
    if (starts_with_ascii_ci(tok, "auto")) {
        out.fr_x100 = 100;
        return true;
    }
    if (ends_with_ascii_ci(tok, "px")) {
        float px = 0.0f;
        if (!parse_css_number(tok.substr(0, tok.size() - 2), px)) return false;
        out.px = static_cast<std::int16_t>(
            std::clamp(static_cast<int>(std::round(px)), 0, 32767));
        return out.px > 0;
    }
    if (ends_with_ascii_ci(tok, "fr")) {
        float fr = 0.0f;
        if (!parse_css_number(tok.substr(0, tok.size() - 2), fr)) return false;
        out.fr_x100 = static_cast<std::int16_t>(
            std::clamp(static_cast<int>(std::round(fr * 100.0f)), 1, 32767));
        return true;
    }
    return false;
}

std::uint8_t parse_grid_template_columns(
        std::string value,
        std::array<detail::GridTrackHint,
                   detail::kMaxGridTrackHints>& out) {
    value = strip_css_important(std::string(trim_css_ws(value)));
    if (value.empty()) return 0;
    std::uint8_t count = 0;
    append_grid_template_tracks(value, out, count);
    return count;
}

bool same_grid_track_hints(
        const std::array<detail::GridTrackHint,
                         detail::kMaxGridTrackHints>& a,
        std::uint8_t a_count,
        const std::array<detail::GridTrackHint,
                         detail::kMaxGridTrackHints>& b,
        std::uint8_t b_count) {
    if (a_count != b_count) return false;
    for (std::uint8_t i = 0; i < a_count; ++i) {
        if (a[i].px != b[i].px || a[i].fr_x100 != b[i].fr_x100) {
            return false;
        }
    }
    return true;
}

bool starts_with_ascii_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const auto a = static_cast<unsigned char>(s[i]);
        const auto b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

std::size_t find_ascii_ci(std::string_view s, std::string_view needle,
                          std::size_t pos = 0) {
    if (needle.empty()) return pos <= s.size() ? pos : std::string_view::npos;
    for (; pos + needle.size() <= s.size(); ++pos) {
        if (starts_with_ascii_ci(s.substr(pos), needle)) return pos;
    }
    return std::string_view::npos;
}

bool is_absolute_resource_url(std::string_view url) {
    return url.find("://") != std::string_view::npos ||
           starts_with_ascii_ci(url, "data:") ||
           starts_with_ascii_ci(url, "file:") ||
           (!url.empty() && (url.front() == '/' || url.front() == '\\')) ||
           (url.size() >= 2 && std::isalpha(static_cast<unsigned char>(url[0]))
                            && url[1] == ':');
}

std::string resolve_css_url(std::string_view stylesheet_base_url,
                            std::string_view url) {
    url = trim_css_ws(url);
    if (url.empty() || stylesheet_base_url.empty() ||
        is_absolute_resource_url(url)) {
        return std::string(url);
    }
    std::string out{stylesheet_base_url};
    out.append(url);
    return out;
}

std::size_t find_matching_paren(std::string_view s, std::size_t open);

std::string parse_css_function_arg(std::string_view s,
                                   std::string_view fn_name) {
    const auto fn = find_ascii_ci(s, fn_name);
    if (fn == std::string_view::npos) return {};
    std::size_t open = fn + fn_name.size();
    while (open < s.size() && is_css_ws(s[open])) ++open;
    if (open >= s.size() || s[open] != '(') return {};
    const auto close = find_matching_paren(s, open);
    if (close == std::string_view::npos || close <= open) return {};
    auto arg = trim_css_ws(s.substr(open + 1, close - open - 1));
    return strip_css_quotes(std::string(arg));
}

int parse_font_face_weight(std::string_view value) {
    value = trim_css_ws(value);
    if (value.empty() || value == "normal") return 400;
    if (value == "bold") return 700;
    int weight = 0;
    for (char c : value) {
        if (c < '0' || c > '9') break;
        weight = weight * 10 + (c - '0');
    }
    return weight > 0 ? std::clamp(weight, 1, 1000) : 400;
}

bool parse_font_face_style(std::string_view value) {
    return find_ascii_ci(value, "italic") != std::string_view::npos ||
           find_ascii_ci(value, "oblique") != std::string_view::npos;
}

std::vector<FontFaceSource> parse_font_face_sources(
    std::string_view src,
    std::string_view stylesheet_base_url) {
    std::vector<FontFaceSource> out;
    while (!src.empty()) {
        const auto comma = find_top_level_comma(src);
        const auto item = trim_css_ws(src.substr(
            0, comma == std::string_view::npos ? src.size() : comma));
        if (!item.empty()) {
            auto url = parse_css_function_arg(item, "url");
            if (!url.empty()) {
                FontFaceSource source;
                source.url = resolve_css_url(stylesheet_base_url, url);
                source.format = parse_css_function_arg(item, "format");
                out.push_back(std::move(source));
            }
        }
        if (comma == std::string_view::npos) break;
        src = src.substr(comma + 1);
    }
    return out;
}

std::size_t find_matching_brace(std::string_view s, std::size_t open) {
    int depth = 0;
    char quote = '\0';
    for (std::size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (quote != '\0') {
            if (c == quote) quote = '\0';
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return i;
    }
    return std::string_view::npos;
}

void scan_font_face_rules(std::string_view css,
                          std::string_view stylesheet_base_url,
                          std::vector<FontFaceRule>& out) {
    std::size_t pos = 0;
    while ((pos = find_ascii_ci(css, "@font-face", pos))
           != std::string_view::npos) {
        const auto open = css.find('{', pos);
        if (open == std::string_view::npos) break;
        const auto close = find_matching_brace(css, open);
        if (close == std::string_view::npos) break;

        const auto decls = css.substr(open + 1, close - open - 1);
        FontFaceRule face;
        face.family = first_font_family(find_decl_value(decls, "font-family"));
        face.weight = parse_font_face_weight(find_decl_value(decls, "font-weight"));
        face.italic = parse_font_face_style(find_decl_value(decls, "font-style"));
        face.sources = parse_font_face_sources(
            find_decl_value(decls, "src"), stylesheet_base_url);
        if (!face.family.empty() && !face.sources.empty()) {
            out.push_back(std::move(face));
        }
        pos = close + 1;
    }
}

std::string lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool font_source_is_backend_supported(const FontFaceSource& source) {
    const auto format = lower_ascii(source.format);
    if (format == "truetype" || format == "opentype" ||
        format == "ttf" || format == "otf" || format == "collection" ||
        format == "woff") {
        return true;
    }

    const auto query = source.url.find_first_of("?#");
    const auto path = lower_ascii(source.url.substr(
        0, query == std::string_view::npos ? source.url.size() : query));
    return (path.size() >= 4 &&
            (path.rfind(".ttf") == path.size() - 4 ||
             path.rfind(".otf") == path.size() - 4 ||
             path.rfind(".ttc") == path.size() - 4)) ||
           (path.size() >= 5 &&
            path.rfind(".woff") == path.size() - 5);
}

std::string ttf_companion_url(std::string_view url) {
    if (url.empty() || starts_with_ascii_ci(url, "data:")) return {};
    const auto query = url.find_first_of("?#");
    const auto path = url.substr(0, query);
    const auto dot = path.find_last_of('.');
    const auto slash = path.find_last_of("/\\");
    if (dot == std::string_view::npos ||
        (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    auto ext = lower_ascii(path.substr(dot));
    if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") return {};

    std::string out;
    out.reserve(url.size());
    out.append(url.substr(0, dot));
    out.append(".ttf");
    if (query != std::string_view::npos) out.append(url.substr(query));
    return out;
}

void ensure_font_faces_registered(detail::DocumentImpl& impl,
                                  Painter& painter) {
    if (!impl.resource_loader) return;

    for (auto& face : impl.font_faces) {
        if (face.loaded || face.attempted) continue;

        std::vector<FontFaceSource> sources;
        sources.reserve(face.sources.size() * 2);
        for (const auto& source : face.sources) {
            if (font_source_is_backend_supported(source)) sources.push_back(source);
        }

        // NanoVG/fontstash consumes raw SFNT data; the painter converts WOFF1
        // before registration. Some first-party icon bundles also ship the
        // same generated face as a sibling TTF for native renderer paths, so
        // keep that as a final fallback after declared backend-supported URLs.
        for (const auto& source : face.sources) {
            auto companion = ttf_companion_url(source.url);
            if (!companion.empty()) {
                FontFaceSource fallback;
                fallback.url = std::move(companion);
                fallback.format = "truetype";
                sources.push_back(std::move(fallback));
            }
        }

        for (const auto& source : sources) {
            if (source.url.empty()) continue;
            auto bytes = impl.resource_loader(source.url);
            if (bytes.empty()) continue;
            if (painter.register_font_face(face.family, face.weight,
                                           face.italic, bytes)) {
                face.loaded = true;
                break;
            }
        }
        face.attempted = !face.loaded;
    }
}

bool selector_list_contains_root(std::string_view selector) {
    while (!selector.empty()) {
        const auto comma = find_top_level_comma(selector);
        const auto group = trim_css_ws(selector.substr(
            0,
            comma == std::string_view::npos ? selector.size() : comma));
        if (group == ":root") return true;
        if (comma == std::string_view::npos) break;
        selector = selector.substr(comma + 1);
    }
    return false;
}

std::string read_declaration_value(std::string_view decls,
                                   std::size_t& pos) {
    pos = decls.find(':', pos);
    if (pos == std::string_view::npos) return {};
    ++pos;
    while (pos < decls.size() && is_css_ws(decls[pos])) ++pos;

    const std::size_t start = pos;
    int depth = 0;
    char quote = '\0';
    while (pos < decls.size()) {
        const char c = decls[pos];
        if (quote != '\0') {
            if (c == quote) quote = '\0';
            ++pos;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(') ++depth;
        else if (c == ')' && depth > 0) --depth;
        else if (c == ';' && depth == 0) break;
        ++pos;
    }
    return std::string(trim_css_ws(decls.substr(start, pos - start)));
}

std::unordered_map<std::string, std::string>
collect_root_custom_properties(const std::vector<RawRule>& rules) {
    std::unordered_map<std::string, std::string> out;
    for (const auto& raw : rules) {
        if (!selector_list_contains_root(raw.selector)) continue;

        std::size_t pos = 0;
        while (pos < raw.decls.size()) {
            while (pos < raw.decls.size() &&
                   (is_css_ws(raw.decls[pos]) || raw.decls[pos] == ';')) {
                ++pos;
            }
            if (pos >= raw.decls.size()) break;
            if (pos + 1 >= raw.decls.size() ||
                raw.decls[pos] != '-' || raw.decls[pos + 1] != '-') {
                pos = raw.decls.find(';', pos);
                if (pos == std::string_view::npos) break;
                ++pos;
                continue;
            }

            const std::size_t name_start = pos;
            while (pos < raw.decls.size() &&
                   raw.decls[pos] != ':' &&
                   !is_css_ws(raw.decls[pos])) {
                ++pos;
            }
            const auto name = std::string(
                raw.decls.substr(name_start, pos - name_start));
            while (pos < raw.decls.size() && is_css_ws(raw.decls[pos])) ++pos;
            if (pos >= raw.decls.size() || raw.decls[pos] != ':') {
                pos = raw.decls.find(';', pos);
                if (pos == std::string_view::npos) break;
                ++pos;
                continue;
            }

            auto value = read_declaration_value(raw.decls, pos);
            if (!name.empty() && !value.empty()) out[name] = std::move(value);
            if (pos < raw.decls.size() && raw.decls[pos] == ';') ++pos;
        }
    }
    return out;
}

std::size_t find_matching_paren(std::string_view s, std::size_t open) {
    int depth = 0;
    char quote = '\0';
    for (std::size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (quote != '\0') {
            if (c == quote) quote = '\0';
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(') ++depth;
        else if (c == ')' && --depth == 0) return i;
    }
    return std::string_view::npos;
}

std::string substitute_root_vars(
    std::string_view value,
    const std::unordered_map<std::string, std::string>& vars,
    int depth = 0) {
    if (depth > 12 || value.find("var(") == std::string_view::npos)
        return std::string(value);

    std::string out;
    std::size_t pos = 0;
    while (pos < value.size()) {
        const auto var_pos = value.find("var(", pos);
        if (var_pos == std::string_view::npos) {
            out.append(value.substr(pos));
            break;
        }

        out.append(value.substr(pos, var_pos - pos));
        const auto close = find_matching_paren(value, var_pos + 3);
        if (close == std::string_view::npos) {
            out.append(value.substr(var_pos));
            break;
        }

        const auto args = trim_css_ws(
            value.substr(var_pos + 4, close - (var_pos + 4)));
        const auto comma = find_top_level_comma(args);
        const auto name = std::string(trim_css_ws(args.substr(
            0,
            comma == std::string_view::npos ? args.size() : comma)));

        const auto found = vars.find(name);
        if (found != vars.end()) {
            out.append(substitute_root_vars(found->second, vars, depth + 1));
        } else if (comma != std::string_view::npos) {
            out.append(substitute_root_vars(
                trim_css_ws(args.substr(comma + 1)), vars, depth + 1));
        } else {
            out.append(value.substr(var_pos, close - var_pos + 1));
        }
        pos = close + 1;
    }
    return out;
}

// Walk the raw CSS source for each rule's font-family declaration.
// For rules whose selector(s) parse as static (no pseudo / no advanced
// combinators), append a RuleFill entry per comma-separated group.
// Specificity (id, class/attr, tag) packed like lexbor's, computed from OUR
// parsed selector chain. Computing it here decouples it from aligning
// split_css_rules() with lexbor's parsed rule list — that alignment desyncs on
// any stylesheet containing @media/@keyframes/@font-face (e.g. the decius
// bundle), which previously handed rules a wrong specificity and broke the
// cascade (a base `.dcs-splitter{col-resize}` wrongly outranking the later,
// equal-specificity `.dcs-splitter--h{row-resize}`).
lxb_css_selector_specificity_t compound_chain_specificity(
    const CompoundSelector& target,
    const std::vector<CompoundSelector>& ancestors) {
    unsigned ids = 0;
    unsigned classes_attrs = 0;
    unsigned tags = 0;
    auto add = [&](const CompoundSelector& compound) {
        for (const auto& simple : compound.simples) {
            switch (simple.kind) {
                case SimpleSelector::Kind::Id:    ++ids; break;
                case SimpleSelector::Kind::Class:
                case SimpleSelector::Kind::Attr:  ++classes_attrs; break;
                case SimpleSelector::Kind::Tag:
                    if (simple.name != "*") ++tags;
                    break;
            }
        }
    };
    add(target);
    for (const auto& a : ancestors) add(a);
    ids = std::min(ids, 511u);
    classes_attrs = std::min(classes_attrs, 511u);
    tags = std::min(tags, 511u);
    return static_cast<lxb_css_selector_specificity_t>(
        (ids << 18) | (classes_attrs << 9) | tags);
}

void scan_rule_fills(lxb_css_stylesheet_t* sst,
                     std::string_view css,
                     std::vector<RuleFill>& out) {
    (void) sst;
    const auto raw_rules = split_css_rules(css);
    const auto root_vars = collect_root_custom_properties(raw_rules);

    for (const auto& raw : raw_rules) {
        // font-family fallback follows browser-style first-installed wins.
        // Preserve the full fallback list; the painter picks the first
        // installed face, matching browser font fallback semantics.
        const auto ff_value = find_decl_value(raw.decls, "font-family");
        auto ff = std::string(
            trim_css_ws(substitute_root_vars(ff_value, root_vars)));
        const auto important = ff.find('!');
        if (important != std::string::npos) {
            ff = std::string(trim_css_ws(
                std::string_view(ff).substr(0, important)));
        }

        const auto resize_value = find_decl_value(raw.decls, "resize");
        const auto [resize, has_resize] = parse_resize_keyword(
            substitute_root_vars(resize_value, root_vars));
        const auto cursor_value = std::string(trim_css_ws(
            substitute_root_vars(find_decl_value(raw.decls, "cursor"),
                                 root_vars)));
        const bool has_cursor = !cursor_value.empty();
        const auto cursor = has_cursor ? parse_cursor_keyword(cursor_value)
                                       : detail::ComputedStyle::Cursor::Default;
        std::array<detail::GridTrackHint,
                   detail::kMaxGridTrackHints> grid_columns{};
        const auto grid_columns_value =
            find_decl_value(raw.decls, "grid-template-columns");
        const std::uint8_t grid_column_count =
            parse_grid_template_columns(
                substitute_root_vars(grid_columns_value, root_vars),
                grid_columns);
        if (ff.empty() && !has_resize && !has_cursor && grid_column_count == 0)
            continue;

        // Each comma-separated group becomes its own RuleFill.
        std::string_view sel_text = trim_css_ws(raw.selector);
        std::size_t s = 0;
        while (s <= sel_text.size()) {
            const auto comma = sel_text.find(',', s);
            const auto group = trim_css_ws(sel_text.substr(s,
                (comma == std::string_view::npos
                     ? sel_text.size() : comma) - s));
            if (!group.empty()) {
                CompoundSelector target;
                std::vector<CompoundSelector> ancestors;
                std::uint8_t state_bit = 0;
                if (parse_static_selector(group, target, ancestors, state_bit)) {
                    RuleFill rf;
                    rf.target               = std::move(target);
                    rf.ancestors            = std::move(ancestors);
                    rf.specificity          =
                        compound_chain_specificity(rf.target, rf.ancestors);
                    rf.source_order         =
                        static_cast<std::uint32_t>(out.size());
                    rf.font_family          = ff;
                    rf.grid_columns         = grid_columns;
                    rf.grid_column_count    = grid_column_count;
                    rf.resize               = resize;
                    rf.has_resize           = has_resize;
                    rf.cursor               = cursor;
                    rf.has_cursor           = has_cursor;
                    rf.state_bit            = state_bit;
                    out.push_back(std::move(rf));
                }
            }
            if (comma == std::string_view::npos) break;
            s = comma + 1;
        }
    }

    std::stable_sort(out.begin(), out.end(),
        [](const RuleFill& a, const RuleFill& b) {
            if (a.specificity != b.specificity)
                return a.specificity < b.specificity;
            return a.source_order < b.source_order;
        });
}

// â”€â”€ @media query support â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// lexbor recognises the `@media` at-keyword but its state handler returns
// `failed`, so every media rule ends up stored as LXB_CSS_AT_RULE__UNDEF
// with the original type recorded in lxb_css_at_rule__undef_t::type.
// The `prelude` lexbor_str_t holds the raw query text (everything between
// `@media` and the opening `{`), and `block` holds the nested CSS text
// (without the surrounding braces).
//
// We walk the rule list here to collect those blocks and parse the
// min-width / max-width constraints from the prelude so layout() can
// later evaluate them against the known viewport width.

std::size_t find_top_level_char(std::string_view s, char needle) {
    int depth = 0;
    char quote = '\0';
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote != '\0') {
            if (c == quote) quote = '\0';
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(') ++depth;
        else if (c == ')' && depth > 0) --depth;
        else if (c == needle && depth == 0) return i;
    }
    return std::string_view::npos;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

bool parse_single_compound_selector(std::string_view sel,
                                    CompoundSelector& out) {
    CompoundSelector target;
    std::vector<CompoundSelector> ancestors;
    std::uint8_t state_bit = 0;
    if (!parse_static_selector(sel, target, ancestors, state_bit)) return false;
    if (!ancestors.empty() || state_bit != 0) return false;
    out = std::move(target);
    return true;
}

bool strip_generated_pseudo(std::string_view& selector,
                            GeneratedContentRule::Position& position) {
    selector = trim_css_ws(selector);
    if (ends_with(selector, "::before")) {
        selector.remove_suffix(8);
        position = GeneratedContentRule::Position::Before;
    } else if (ends_with(selector, ":before")) {
        selector.remove_suffix(7);
        position = GeneratedContentRule::Position::Before;
    } else if (ends_with(selector, "::after")) {
        selector.remove_suffix(7);
        position = GeneratedContentRule::Position::After;
    } else if (ends_with(selector, ":after")) {
        selector.remove_suffix(6);
        position = GeneratedContentRule::Position::After;
    } else {
        return false;
    }
    selector = trim_css_ws(selector);
    return !selector.empty();
}

bool parse_generated_selector(std::string_view selector,
                              GeneratedContentRule& rule) {
    selector = trim_css_ws(selector);
    if (!strip_generated_pseudo(selector, rule.position)) return false;

    const auto plus = find_top_level_char(selector, '+');
    if (plus != std::string_view::npos) {
        const auto previous = trim_css_ws(selector.substr(0, plus));
        const auto target = trim_css_ws(selector.substr(plus + 1));
        if (previous.empty() || target.empty()) return false;
        if (!parse_single_compound_selector(previous,
                                            rule.previous_adjacent)) {
            return false;
        }
        rule.has_previous_adjacent = true;
        std::uint8_t state_bit = 0;
        if (!parse_static_selector(target, rule.target, rule.ancestors,
                                   state_bit)) {
            return false;
        }
        return state_bit == 0;
    }

    std::uint8_t state_bit = 0;
    if (!parse_static_selector(selector, rule.target, rule.ancestors,
                               state_bit)) {
        return false;
    }
    return state_bit == 0;
}

lxb_css_selector_specificity_t generated_selector_specificity(
    const GeneratedContentRule& rule) {
    unsigned ids = 0;
    unsigned classes_attrs = 0;
    unsigned tags = 1;  // ::before / ::after count like a type selector.

    auto add_compound = [&](const CompoundSelector& compound) {
        for (const auto& simple : compound.simples) {
            switch (simple.kind) {
                case SimpleSelector::Kind::Id:
                    ++ids;
                    break;
                case SimpleSelector::Kind::Class:
                case SimpleSelector::Kind::Attr:
                    ++classes_attrs;
                    break;
                case SimpleSelector::Kind::Tag:
                    if (simple.name != "*") ++tags;
                    break;
            }
        }
    };

    add_compound(rule.target);
    for (const auto& ancestor : rule.ancestors) add_compound(ancestor);
    if (rule.has_previous_adjacent) add_compound(rule.previous_adjacent);

    ids = std::min(ids, 511u);
    classes_attrs = std::min(classes_attrs, 511u);
    tags = std::min(tags, 511u);
    return static_cast<lxb_css_selector_specificity_t>(
        (ids << 18) | (classes_attrs << 9) | tags);
}

std::string substitute_style_vars(
    std::string_view value,
    const detail::ResolvedStyle& style) {
    static const detail::CustomPropMap kEmpty;
    return substitute_root_vars(
        value,
        style.custom_props ? *style.custom_props : kEmpty);
}

std::string generated_content_text(std::string value,
                                   const detail::ResolvedStyle& style) {
    value = std::string(trim_css_ws(substitute_style_vars(value, style)));
    if (value.empty() || value == "normal" || value == "none") return {};
    return decode_css_string_escapes(strip_css_quotes(std::move(value)));
}

bool generated_content_enabled(std::string value,
                               const detail::ResolvedStyle& style) {
    value = std::string(trim_css_ws(substitute_style_vars(value, style)));
    return !value.empty() && value != "normal" && value != "none";
}

int parse_generated_length_px(std::string value,
                              const detail::ResolvedStyle& style) {
    value = std::string(trim_css_ws(substitute_style_vars(value, style)));
    if (value.empty()) return 0;

    char* end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    if (end == value.c_str()) return 0;
    while (*end != '\0' && is_css_ws(*end)) ++end;

    std::string_view unit(end);
    if (unit.empty() || unit == "px") {
        return static_cast<int>(std::lround(number));
    }
    if (unit == "em") {
        return static_cast<int>(std::lround(
            number * static_cast<double>(style.computed.font_size_px)));
    }
    if (unit == "rem") {
        return static_cast<int>(std::lround(number * 16.0));
    }
    return 0;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool parse_hex_color(std::string_view value, std::uint32_t& out) {
    if (value.empty() || value.front() != '#') return false;
    value.remove_prefix(1);
    auto byte_pair = [](int hi, int lo) {
        return static_cast<std::uint8_t>((hi << 4) | lo);
    };
    if (value.size() == 3 || value.size() == 4) {
        int r = hex_digit(value[0]);
        int g = hex_digit(value[1]);
        int b = hex_digit(value[2]);
        int a = value.size() == 4 ? hex_digit(value[3]) : 0xF;
        if (r < 0 || g < 0 || b < 0 || a < 0) return false;
        out = detail::pack_rgba(Color{
            byte_pair(r, r), byte_pair(g, g), byte_pair(b, b),
            byte_pair(a, a)});
        return true;
    }
    if (value.size() == 6 || value.size() == 8) {
        int digits[8] = {};
        for (std::size_t i = 0; i < value.size(); ++i) {
            digits[i] = hex_digit(value[i]);
            if (digits[i] < 0) return false;
        }
        out = detail::pack_rgba(Color{
            byte_pair(digits[0], digits[1]),
            byte_pair(digits[2], digits[3]),
            byte_pair(digits[4], digits[5]),
            value.size() == 8 ? byte_pair(digits[6], digits[7])
                              : static_cast<std::uint8_t>(0xFF)});
        return true;
    }
    return false;
}

std::string ascii_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool parse_number_list(std::string_view s, std::vector<double>& out) {
    std::size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() &&
               (is_css_ws(s[pos]) || s[pos] == ',' || s[pos] == '/')) {
            ++pos;
        }
        if (pos >= s.size()) break;
        std::string tail(s.substr(pos));
        char* end = nullptr;
        const double number = std::strtod(tail.c_str(), &end);
        if (end == tail.c_str()) return false;
        out.push_back(number);
        pos += static_cast<std::size_t>(end - tail.c_str());
        while (pos < s.size() && is_css_ws(s[pos])) ++pos;
        if (pos < s.size() && s[pos] == '%') ++pos;
    }
    return !out.empty();
}

std::uint8_t alpha_to_u8(double alpha) {
    if (alpha <= 0.0) return 0;
    if (alpha <= 1.0) {
        return static_cast<std::uint8_t>(std::lround(alpha * 255.0));
    }
    if (alpha >= 255.0) return 255;
    return static_cast<std::uint8_t>(std::lround(alpha));
}

std::uint8_t channel_to_u8(double channel) {
    if (channel <= 0.0) return 0;
    if (channel >= 255.0) return 255;
    return static_cast<std::uint8_t>(std::lround(channel));
}

bool parse_function_color(std::string_view value, std::uint32_t& out) {
    const auto open = value.find('(');
    if (open == std::string_view::npos || value.back() != ')') return false;
    const auto name = ascii_lower(trim_css_ws(value.substr(0, open)));
    if (name != "rgb" && name != "rgba") return false;

    std::vector<double> components;
    if (!parse_number_list(value.substr(open + 1,
                                        value.size() - open - 2),
                           components) ||
        components.size() < 3) {
        return false;
    }

    out = detail::pack_rgba(Color{
        channel_to_u8(components[0]),
        channel_to_u8(components[1]),
        channel_to_u8(components[2]),
        components.size() >= 4 ? alpha_to_u8(components[3])
                               : static_cast<std::uint8_t>(0xFF)});
    return true;
}

bool parse_generated_color(std::string value,
                           const detail::ResolvedStyle& style,
                           std::uint32_t& out) {
    value = std::string(trim_css_ws(substitute_style_vars(value, style)));
    const auto lower = ascii_lower(value);
    if (lower.empty()) return false;
    if (lower == "currentcolor") {
        out = style.animated.color_rgba;
        return true;
    }
    if (lower == "transparent") {
        out = 0x00000000u;
        return true;
    }
    if (lower == "black") {
        out = detail::pack_rgba(Color{0, 0, 0, 255});
        return true;
    }
    if (lower == "white") {
        out = detail::pack_rgba(Color{255, 255, 255, 255});
        return true;
    }
    if (lower == "gray" || lower == "grey") {
        out = detail::pack_rgba(Color{128, 128, 128, 255});
        return true;
    }
    return parse_hex_color(value, out) || parse_function_color(value, out);
}

struct SvgArcPath {
    double x1{0.0};
    double y1{0.0};
    double rx{0.0};
    double ry{0.0};
    double x_axis_rotation{0.0};
    bool   large_arc{false};
    bool   sweep{false};
    double x2{0.0};
    double y2{0.0};
};

std::string svg_path_number_stream(std::string_view d) {
    std::string out;
    out.reserve(d.size());
    for (char ch : d) {
        switch (ch) {
            case 'M': case 'm': case 'A': case 'a': case 'L': case 'l':
            case 'H': case 'h': case 'V': case 'v': case 'C': case 'c':
            case 'S': case 's': case 'Q': case 'q': case 'T': case 't':
            case 'Z': case 'z':
                out.push_back(' ');
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

bool parse_svg_arc_path(std::string_view d, SvgArcPath& out) {
    if (d.empty()) return false;
    std::vector<double> n;
    if (!parse_number_list(svg_path_number_stream(d), n) || n.size() < 9) {
        return false;
    }
    out.x1 = n[0];
    out.y1 = n[1];
    out.rx = std::abs(n[2]);
    out.ry = std::abs(n[3]);
    out.x_axis_rotation = n[4];
    out.large_arc = std::abs(n[5]) > 0.5;
    out.sweep = std::abs(n[6]) > 0.5;
    out.x2 = n[7];
    out.y2 = n[8];
    return out.rx > 0.0 && out.ry > 0.0;
}

bool parse_svg_viewbox(std::string_view view_box,
                       double& x, double& y, double& w, double& h) {
    std::vector<double> n;
    if (!parse_number_list(view_box, n) || n.size() < 4) return false;
    x = n[0];
    y = n[1];
    w = n[2];
    h = n[3];
    return w > 0.0 && h > 0.0;
}

bool parse_svg_stroke_width(std::string_view value, float& out) {
    if (value.empty()) {
        out = 1.0f;
        return true;
    }
    std::string s(trim_css_ws(value));
    char* end = nullptr;
    const double n = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || n <= 0.0) return false;
    out = static_cast<float>(n);
    return true;
}

double angle_from_top_clockwise(double x, double y, double cx, double cy) {
    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    return std::atan2(x - cx, cy - y) * kRadToDeg;
}

void normalize_clockwise_arc(double& start_deg, double& end_deg, bool large_arc) {
    while (end_deg <= start_deg) end_deg += 360.0;
    const double delta = end_deg - start_deg;
    if (large_arc && delta < 180.0) {
        end_deg += 360.0;
    } else if (!large_arc && delta > 180.0) {
        end_deg -= 360.0;
        if (end_deg <= start_deg) end_deg += 360.0;
    }
}

bool svg_arc_center(const SvgArcPath& arc, double& cx, double& cy, double& r) {
    // First slice: circular, unrotated arcs. This covers Decius knob rings
    // and maps directly onto the Painter stroke_arc primitive.
    if (std::abs(arc.rx - arc.ry) > 0.01 ||
        std::abs(arc.x_axis_rotation) > 0.01) {
        return false;
    }
    r = arc.rx;
    const double dx = (arc.x1 - arc.x2) * 0.5;
    const double dy = (arc.y1 - arc.y2) * 0.5;
    const double d2 = dx * dx + dy * dy;
    if (d2 <= 1e-9) return false;
    const double chord_half = std::sqrt(d2);
    if (r < chord_half) r = chord_half;
    double factor = std::sqrt(std::max(0.0, (r * r) / d2 - 1.0));
    if (arc.large_arc == arc.sweep) factor = -factor;
    cx = (arc.x1 + arc.x2) * 0.5 + factor * dy;
    cy = (arc.y1 + arc.y2) * 0.5 - factor * dx;
    return true;
}

#if !defined(AFFINEUI_STUB_BUILD)
void paint_inline_svg(const Block& b,
                      const Rect& eff,
                      const detail::ComputedStyle& cs,
                      const detail::AnimatedStyle& an,
                      Painter& painter,
                      lxb_dom_element_t* svg_elem) {
    double vb_x = 0.0;
    double vb_y = 0.0;
    double vb_w = static_cast<double>(std::max(1, eff.w));
    double vb_h = static_cast<double>(std::max(1, eff.h));
    (void) parse_svg_viewbox(attr_string(svg_elem, "viewBox"),
                             vb_x, vb_y, vb_w, vb_h);
    const double sx = static_cast<double>(eff.w) / vb_w;
    const double sy = static_cast<double>(eff.h) / vb_h;
    const double sr = (sx + sy) * 0.5;

    detail::ResolvedStyle svg_style{};
    svg_style.computed = cs;
    svg_style.animated = an;
    svg_style.custom_props = b.custom_props;

    for (auto* node = lxb_dom_node_first_child(lxb_dom_interface_node(svg_elem));
         node != nullptr; node = lxb_dom_node_next(node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* path_elem = lxb_dom_interface_element(node);
        if (tag_name(path_elem) != "path") continue;

        SvgArcPath arc;
        if (!parse_svg_arc_path(attr_string(path_elem, "d"), arc)) continue;

        std::string stroke_value =
            std::string(trim_css_ws(attr_string(path_elem, "stroke")));
        if (stroke_value.empty() || ascii_lower(stroke_value) == "none") {
            continue;
        }

        std::uint32_t stroke_rgba = 0;
        if (!parse_generated_color(stroke_value, svg_style, stroke_rgba) ||
            (stroke_rgba & 0xFFu) == 0) {
            continue;
        }

        float stroke_width = 1.0f;
        if (!parse_svg_stroke_width(attr_string(path_elem, "stroke-width"),
                                    stroke_width)) {
            continue;
        }

        double cx = 0.0;
        double cy = 0.0;
        double r = 0.0;
        if (!svg_arc_center(arc, cx, cy, r)) continue;

        double start = angle_from_top_clockwise(arc.x1, arc.y1, cx, cy);
        double end = angle_from_top_clockwise(arc.x2, arc.y2, cx, cy);
        if (!arc.sweep) std::swap(start, end);
        normalize_clockwise_arc(start, end, arc.large_arc);

        const float px_cx = static_cast<float>(
            static_cast<double>(eff.x) + (cx - vb_x) * sx);
        const float px_cy = static_cast<float>(
            static_cast<double>(eff.y) + (cy - vb_y) * sy);
        const float px_r = static_cast<float>(r * sr);
        const float px_w = std::max(0.5f, stroke_width * static_cast<float>(sr));

        painter.stroke_arc(px_cx, px_cy, px_r,
                           static_cast<float>(start),
                           static_cast<float>(end),
                           detail::unpack_rgba(stroke_rgba), px_w);
    }
}

void paint_direct_child_svgs(const Block& b,
                             const Rect& eff,
                             const detail::ComputedStyle& cs,
                             const detail::AnimatedStyle& an,
                             Painter& painter,
                             lxb_dom_element_t* parent_elem) {
    for (auto* node = lxb_dom_node_first_child(lxb_dom_interface_node(parent_elem));
         node != nullptr; node = lxb_dom_node_next(node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* elem = lxb_dom_interface_element(node);
        if (tag_name(elem) == "svg") {
            paint_inline_svg(b, eff, cs, an, painter, elem);
        }
    }
}
#endif

void scan_generated_content_rules(lxb_css_parser_t* parser,
                                  lxb_css_memory_t* memory,
                                  std::string_view css,
                                  std::vector<GeneratedContentRule>& out) {
    const auto raw_rules = split_css_rules(css);
    for (const auto& raw : raw_rules) {
        const auto content_value = find_decl_value(raw.decls, "content");
        const auto color_value = find_decl_value(raw.decls, "color");
        const auto background_value =
            find_decl_value(raw.decls, "background");
        const auto background_color_value =
            find_decl_value(raw.decls, "background-color");
        const auto padding_left_value =
            find_decl_value(raw.decls, "padding-left");
        const auto padding_right_value =
            find_decl_value(raw.decls, "padding-right");

        std::string_view sel_text = trim_css_ws(raw.selector);
        const lxb_css_rule_declaration_list_t* parsed_decls = nullptr;
        bool parsed_decls_attempted = false;
        auto decls_for_raw_rule = [&]() -> const lxb_css_rule_declaration_list_t* {
            if (!parsed_decls_attempted) {
                parsed_decls_attempted = true;
                if (parser && memory) {
                    parsed_decls = lxb_css_declaration_list_parse(
                        parser, memory,
                        reinterpret_cast<const lxb_char_t*>(raw.decls.data()),
                        raw.decls.size());
                }
            }
            return parsed_decls;
        };

        while (!sel_text.empty()) {
            const auto comma = find_top_level_comma(sel_text);
            const auto group = trim_css_ws(sel_text.substr(
                0,
                comma == std::string_view::npos ? sel_text.size() : comma));
            if (!group.empty()) {
                GeneratedContentRule rule;
                if (parse_generated_selector(group, rule)) {
                    rule.specificity = generated_selector_specificity(rule);
                    rule.source_order =
                        static_cast<std::uint32_t>(out.size());
                    rule.content_value = content_value;
                    rule.color_value = color_value;
                    rule.background_value = background_value;
                    rule.background_color_value = background_color_value;
                    rule.padding_left_value = padding_left_value;
                    rule.padding_right_value = padding_right_value;
                    rule.decls = decls_for_raw_rule();
                    out.push_back(std::move(rule));
                }
            }
            if (comma == std::string_view::npos) break;
            sel_text = trim_css_ws(sel_text.substr(comma + 1));
        }
    }

    std::stable_sort(out.begin(), out.end(),
        [](const GeneratedContentRule& a, const GeneratedContentRule& b) {
            if (a.specificity != b.specificity)
                return a.specificity < b.specificity;
            return a.source_order < b.source_order;
        });
}

// Parse a dimension value `NNNpx` (only `px` units are relevant for
// min/max-width media features) from a string_view. Returns -1 if the
// value is absent or not in pixels.
static int parse_media_px(std::string_view s, std::string_view key) {
    // key is e.g. "min-width" or "max-width"
    auto pos = s.find(key);
    while (pos != std::string_view::npos) {
        // Walk past the keyword
        std::size_t i = pos + key.size();
        while (i < s.size() && is_css_ws(s[i])) ++i;
        if (i >= s.size() || s[i] != ':') {
            pos = s.find(key, pos + 1); continue;
        }
        ++i;  // skip ':'
        while (i < s.size() && is_css_ws(s[i])) ++i;
        // Parse optional decimal number
        if (i >= s.size() || (!std::isdigit(static_cast<unsigned char>(s[i]))
                              && s[i] != '.')) {
            pos = s.find(key, pos + 1); continue;
        }
        std::size_t num_start = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i]))
                                 || s[i] == '.')) ++i;
        // Expect "px" (case-insensitive)
        if (i + 1 < s.size() && (s[i] == 'p' || s[i] == 'P') &&
                                 (s[i+1] == 'x' || s[i+1] == 'X')) {
            double val = 0.0;
            for (std::size_t k = num_start; k < i; ++k) {
                if (s[k] == '.') { /* skip â€“ no sub-px needed */ }
                else val = val * 10.0 + (s[k] - '0');
            }
            return static_cast<int>(val);
        }
        pos = s.find(key, pos + 1);
    }
    return -1;
}

// Walk one parsed stylesheet and collect @media at-rules that lexbor
// stored as _undef. For each, parse min-width / max-width from the
// prelude and record a MediaBlock entry. This runs at attach time so
// that layout() can evaluate the collected blocks against the then-known
// viewport width.
void scan_media_blocks(lxb_css_stylesheet_t* sst,
                       std::vector<MediaBlock>& out) {
    if (!sst || !sst->root) return;
    auto* rule_list = lxb_css_rule_list(sst->root);
    for (auto* r = rule_list->first; r != nullptr; r = r->next) {
        if (r->type != LXB_CSS_RULE_AT_RULE) continue;
        auto* at = lxb_css_rule_at(r);
        // The at-rule must be UNDEF (media state handler failed) with the
        // original type recorded as LXB_CSS_AT_RULE_MEDIA.
        if (at->type != LXB_CSS_AT_RULE__UNDEF) continue;
        const auto* undef = at->u.undef;
        if (!undef || undef->type != LXB_CSS_AT_RULE_MEDIA) continue;

        // block must be present; a media rule without a block is useless.
        if (!undef->block.data || undef->block.length == 0) continue;

        std::string_view prelude(
            reinterpret_cast<const char*>(undef->prelude.data),
            undef->prelude.length);
        std::string_view block(
            reinterpret_cast<const char*>(undef->block.data),
            undef->block.length);

        MediaBlock mb;
        mb.min_width_px = parse_media_px(prelude, "min-width");
        mb.max_width_px = parse_media_px(prelude, "max-width");
        mb.block_css.assign(block.data(), block.size());
        out.push_back(std::move(mb));
    }
}

std::uint32_t fnv1a_32(std::string_view s) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

bool parse_keyframe_offset(std::string_view token, float& out) {
    token = trim_css_ws(token);
    if (token == "from") {
        out = 0.0f;
        return true;
    }
    if (token == "to") {
        out = 1.0f;
        return true;
    }
    if (token.empty() || token.back() != '%') return false;
    token.remove_suffix(1);
    token = trim_css_ws(token);
    if (token.empty()) return false;

    std::string tmp(token);
    char* end = nullptr;
    const double pct = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str()) return false;
    while (*end != '\0' && is_css_ws(*end)) ++end;
    if (*end != '\0') return false;
    out = static_cast<float>(std::clamp(pct / 100.0, 0.0, 1.0));
    return true;
}

std::vector<float> parse_keyframe_selector_offsets(std::string_view selector) {
    std::vector<float> offsets;
    selector = trim_css_ws(selector);
    while (!selector.empty()) {
        const auto comma = find_top_level_comma(selector);
        const auto piece = trim_css_ws(selector.substr(
            0, comma == std::string_view::npos ? selector.size() : comma));
        float offset = 0.0f;
        if (parse_keyframe_offset(piece, offset)) offsets.push_back(offset);
        if (comma == std::string_view::npos) break;
        selector = trim_css_ws(selector.substr(comma + 1));
    }
    return offsets;
}

void scan_keyframe_blocks(lxb_css_stylesheet_t* sst,
                          lxb_css_parser_t* parser,
                          lxb_css_memory_t* memory,
                          std::vector<KeyframeBlock>& out) {
    if (!sst || !sst->root || !parser || !memory) return;
    auto* rule_list = lxb_css_rule_list(sst->root);
    for (auto* r = rule_list->first; r != nullptr; r = r->next) {
        if (r->type != LXB_CSS_RULE_AT_RULE) continue;
        auto* at = lxb_css_rule_at(r);
        if (at->type != LXB_CSS_AT_RULE__CUSTOM || !at->u.custom) continue;

        const auto* custom = at->u.custom;
        const std::string at_name = ascii_lower(std::string_view(
            reinterpret_cast<const char*>(custom->name.data),
            custom->name.length));
        if (at_name != "keyframes" && at_name != "-webkit-keyframes") continue;
        if (!custom->block.data || custom->block.length == 0) continue;

        auto name = strip_css_quotes(std::string(trim_css_ws(std::string_view(
            reinterpret_cast<const char*>(custom->prelude.data),
            custom->prelude.length))));
        if (name.empty() || name == "none") continue;

        std::string_view body(
            reinterpret_cast<const char*>(custom->block.data),
            custom->block.length);
        KeyframeBlock block;
        block.name_hash = fnv1a_32(name);

        std::size_t i = 0;
        auto skip_ws_and_comments = [&] {
            for (;;) {
                while (i < body.size() && is_css_ws(body[i])) ++i;
                if (i + 1 < body.size() && body[i] == '/' && body[i + 1] == '*') {
                    i += 2;
                    while (i + 1 < body.size() &&
                           !(body[i] == '*' && body[i + 1] == '/')) ++i;
                    if (i + 1 < body.size()) i += 2;
                } else {
                    break;
                }
            }
        };

        while (i < body.size()) {
            skip_ws_and_comments();
            if (i >= body.size()) break;
            const std::size_t selector_start = i;
            while (i < body.size() && body[i] != '{') ++i;
            if (i >= body.size()) break;
            const auto selector = body.substr(selector_start, i - selector_start);
            ++i;
            const std::size_t decl_start = i;
            int depth = 1;
            char quote = '\0';
            while (i < body.size() && depth > 0) {
                const char c = body[i];
                if (quote != '\0') {
                    if (c == quote) quote = '\0';
                    ++i;
                    continue;
                }
                if (c == '"' || c == '\'') quote = c;
                else if (c == '{') ++depth;
                else if (c == '}' && --depth == 0) break;
                ++i;
            }
            if (i > body.size()) break;
            const auto decls = body.substr(decl_start, i - decl_start);
            if (i < body.size() && body[i] == '}') ++i;

            const auto offsets = parse_keyframe_selector_offsets(selector);
            if (offsets.empty()) continue;
            auto* list = lxb_css_declaration_list_parse(
                parser, memory,
                reinterpret_cast<const lxb_char_t*>(decls.data()),
                decls.size());
            if (!list) continue;
            for (float offset : offsets) {
                block.steps.push_back({offset, list});
            }
        }

        if (block.steps.empty()) continue;
        std::stable_sort(block.steps.begin(), block.steps.end(),
            [](const KeyframeStep& a, const KeyframeStep& b) {
                return a.offset < b.offset;
            });

        auto existing = std::find_if(out.begin(), out.end(),
            [&](const KeyframeBlock& kf) {
                return kf.name_hash == block.name_hash;
            });
        if (existing != out.end()) {
            *existing = std::move(block);
        } else {
            out.push_back(std::move(block));
        }
    }
}

void attach_media_block(detail::DocumentImpl& impl, const MediaBlock& mb) {
    auto* sst_media = lxb_css_stylesheet_parse(
        impl.doc->css.parser,
        reinterpret_cast<const lxb_char_t*>(mb.block_css.data()),
        mb.block_css.size());
    if (!sst_media) return;

    if (lxb_html_document_stylesheet_attach(impl.doc, sst_media)
            == LXB_STATUS_OK) {
        impl.sheets.push_back(sst_media);
        scan_pseudo_rules(sst_media, impl.pseudo_rules);
        scan_rule_fills(sst_media, mb.block_css, impl.rule_fills);
        scan_font_face_rules(mb.block_css, {}, impl.font_faces);
        scan_generated_content_rules(
            impl.doc->css.parser,
            impl.doc->css.memory, mb.block_css,
            impl.generated_content_rules);
        scan_keyframe_blocks(
            sst_media, impl.doc->css.parser,
            impl.doc->css.memory, impl.keyframes);
        // Nested @media blocks are not in scope for the current media
        // implementation; do not rescan this generated stylesheet.
    } else {
        lxb_css_stylesheet_destroy(sst_media, true);
    }
}

void attach_stylesheet(detail::DocumentImpl& impl, std::string_view css,
                       std::string_view base_url = {}) {
    if (css.empty()) return;
    // Parse via the document's own CSS parser (pre-wired with the
    // document's memory pool + selectors engine). Parsing through a
    // standalone parser allocates rules in a foreign pool that the
    // document's ev_destroy hook can't safely tear down.
    const auto media_start = impl.media_blocks.size();
    auto* sst = lxb_css_stylesheet_parse(
        impl.doc->css.parser,
        reinterpret_cast<const lxb_char_t*>(css.data()),
        css.size());
    if (!sst) return;
    if (lxb_html_document_stylesheet_attach(impl.doc, sst) == LXB_STATUS_OK) {
        impl.sheets.push_back(sst);
        scan_pseudo_rules(sst, impl.pseudo_rules);
        // Recover font-family names into AffineUI's font registry side
        // table. attach_stylesheet's `css` argument outlives this call
        // (UA stylesheet is static, user_stylesheet is owned by impl_,
        // author CSS is a local that's destroyed when set_html returns,
        // but RuleFill values copy what they need). `base_url` resolves any
        // url()s inside the sheet (e.g. @font-face src) just like a <link>ed
        // sheet — empty for the UA/author sheets, set for an App-supplied one.
        scan_rule_fills(sst, css, impl.rule_fills);
        scan_font_face_rules(css, base_url, impl.font_faces);
        scan_generated_content_rules(impl.doc->css.parser,
                                     impl.doc->css.memory, css,
                                     impl.generated_content_rules);
        scan_keyframe_blocks(sst, impl.doc->css.parser, impl.doc->css.memory,
                             impl.keyframes);
        // Collect @media blocks for later evaluation in layout().
        scan_media_blocks(sst, impl.media_blocks);
        if (impl.media_viewport_width_px > 0) {
            for (std::size_t i = media_start; i < impl.media_blocks.size(); ++i) {
                if (impl.media_blocks[i].matches(impl.media_viewport_width_px)) {
                    attach_media_block(impl, impl.media_blocks[i]);
                }
            }
        }
    } else {
        lxb_css_stylesheet_destroy(sst, true);
    }
}

// Keyword-value variant of the inline-style scanner. Returns the
// identifier text after `<key>:` (e.g. "pointer" for `cursor:
// pointer`). Stops at the first ';' or end of attribute. Used for
// properties whose values are CSS keywords rather than lengths.
std::string scan_inline_keyword(lxb_dom_element_t* elem, std::string_view key) {
    size_t len = 0;
    const lxb_char_t* attr = lxb_dom_element_get_attribute(
        elem,
        reinterpret_cast<const lxb_char_t*>("style"), 5,
        &len);
    if (!attr || len == 0) return {};
    std::string_view s(reinterpret_cast<const char*>(attr), len);
    const auto pos = s.find(key);
    if (pos == std::string_view::npos) return {};
    auto rest = s.substr(pos + key.size());
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
    if (rest.empty() || rest.front() != ':') return {};
    rest.remove_prefix(1);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
    std::size_t end = 0;
    while (end < rest.size()
           && rest[end] != ';' && rest[end] != ' '
           && rest[end] != '\t' && rest[end] != '\n') {
        ++end;
    }
    return std::string(rest.substr(0, end));
}

// Map a `cursor` keyword onto our enum. Unknown values â†’ Default.
std::string scan_inline_decl_value(lxb_dom_element_t* elem,
                                   std::string_view key) {
    size_t len = 0;
    const lxb_char_t* attr = lxb_dom_element_get_attribute(
        elem,
        reinterpret_cast<const lxb_char_t*>("style"), 5,
        &len);
    if (!attr || len == 0) return {};
    std::string_view s(reinterpret_cast<const char*>(attr), len);
    return find_decl_value(s, key);
}

detail::ComputedStyle::Cursor parse_cursor_keyword(std::string_view kw) {
    using C = detail::ComputedStyle::Cursor;
    if (kw == "pointer")     return C::Pointer;
    if (kw == "text")        return C::Text;
    if (kw == "crosshair")   return C::Crosshair;
    if (kw == "move")        return C::Move;
    if (kw == "not-allowed") return C::NotAllowed;
    if (kw == "ew-resize" || kw == "col-resize") return C::ResizeEW;
    if (kw == "ns-resize" || kw == "row-resize") return C::ResizeNS;
    return C::Default;
}

lxb_dom_element_t* previous_element_sibling(lxb_dom_node_t* node) {
    for (auto* prev = lxb_dom_node_prev(node); prev;
         prev = lxb_dom_node_prev(prev)) {
        if (prev->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            return lxb_dom_interface_element(prev);
        }
    }
    return nullptr;
}

lxb_dom_element_t* next_element_sibling(lxb_dom_node_t* node) {
    for (auto* next = lxb_dom_node_next(node); next;
         next = lxb_dom_node_next(next)) {
        if (next->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            return lxb_dom_interface_element(next);
        }
    }
    return nullptr;
}

lxb_dom_element_t* parent_element(lxb_dom_element_t* elem) {
    if (!elem) return nullptr;
    for (auto* parent = lxb_dom_node_parent(lxb_dom_interface_node(elem));
         parent; parent = lxb_dom_node_parent(parent)) {
        if (parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            return lxb_dom_interface_element(parent);
        }
    }
    return nullptr;
}

bool element_matches_compound(lxb_dom_element_t* elem,
                              const CompoundSelector& compound) {
    if (!elem) return false;
    const auto tag = tag_name(elem);
    const auto id = attr_string(elem, "id");
    const auto classes = split_classes(attr_string(elem, "class"));
    const auto attrs = element_attrs(elem);
    return compound_matches(compound, tag, id, classes, &attrs);
}

bool dom_ancestor_chain_matches(const std::vector<CompoundSelector>& ancestors,
                                lxb_dom_element_t* elem) {
    std::size_t i = 0;
    auto* ancestor = parent_element(elem);
    while (i < ancestors.size() && ancestor) {
        if (element_matches_compound(ancestor, ancestors[i])) {
            ++i;
        }
        ancestor = parent_element(ancestor);
    }
    return i == ancestors.size();
}

bool generated_rule_matches(const detail::DocumentImpl& impl,
                            const GeneratedContentRule& rule,
                            lxb_dom_element_t* elem,
                            int selector_parent_idx) {
    (void) impl;
    (void) selector_parent_idx;
    if (!element_matches_compound(elem, rule.target)) return false;
    if (!dom_ancestor_chain_matches(rule.ancestors, elem)) return false;
    if (rule.has_previous_adjacent) {
        auto* prev = previous_element_sibling(lxb_dom_interface_node(elem));
        if (!element_matches_compound(prev, rule.previous_adjacent)) {
            return false;
        }
    }
    return true;
}

void append_generated_inline_text(detail::DocumentImpl& impl,
                                  const detail::ResolvedStyle& parent_style,
                                  int parent_idx,
                                  std::string text,
                                  int padding_left,
                                  int padding_right,
                                  bool has_color,
                                  std::uint32_t color_rgba,
                                  GeneratedContentRule::Position position) {
    text = apply_text_transform(std::move(text),
                                parent_style.computed.text_transform);
    if (text.empty()) return;

    const auto id = impl.style_store.acquire_synthetic();
    auto rs = anonymous_text_style(parent_style);
    rs.computed.padding_left = clamp_css_px(padding_left);
    rs.computed.padding_right = clamp_css_px(padding_right);
    if (has_color) rs.animated.color_rgba = color_rgba;

    impl.style_store.computed(id) = rs.computed;
    impl.style_store.animated(id) = rs.animated;
    impl.style_store.dirty(id) &=
        static_cast<std::uint8_t>(~detail::StyleStore::DirtyStyle);

    Block b;
    b.id         = id;
    b.tag        = position == GeneratedContentRule::Position::Before
                     ? "#before"
                     : "#after";
    b.text       = std::move(text);
    b.parent_idx = parent_idx;
    b.box_shadows = rs.box_shadows;
    b.base_animated = rs.animated;
    b.animation = rs.animation;
    b.animation_epoch = impl.animation_epoch;
    impl.blocks.push_back(std::move(b));
}

void append_generated_box(detail::DocumentImpl& impl,
                          detail::ResolvedStyle rs,
                          int parent_idx,
                          GeneratedContentRule::Position position) {
    const auto id = impl.style_store.acquire_synthetic();
    impl.style_store.computed(id) = rs.computed;
    impl.style_store.animated(id) = rs.animated;
    impl.style_store.dirty(id) &=
        static_cast<std::uint8_t>(~detail::StyleStore::DirtyStyle);

    Block b;
    b.id         = id;
    b.tag        = position == GeneratedContentRule::Position::Before
                     ? "#before"
                     : "#after";
    b.parent_idx = parent_idx;
    b.box_shadows = rs.box_shadows;
    b.base_animated = rs.animated;
    b.animation = rs.animation;
    b.animation_epoch = impl.animation_epoch;
    impl.blocks.push_back(std::move(b));
}

void append_generated_content_for_element(
    detail::DocumentImpl& impl,
    lxb_dom_element_t* elem,
    const detail::ResolvedStyle& elem_style,
    int generated_parent_idx,
    int selector_parent_idx,
    GeneratedContentRule::Position position,
    int& open_synth_idx,
    bool& pending_inline_space) {
    if (!elem || impl.generated_content_rules.empty()) return;

    bool has_content = false;
    bool has_color = false;
    bool has_padding_left = false;
    bool has_padding_right = false;
    std::string content_value;
    std::uint32_t color_rgba = elem_style.animated.color_rgba;
    int padding_left = 0;
    int padding_right = 0;
    auto pseudo_style = impl.resolver
        ? impl.resolver->resolve(nullptr, elem_style)
        : elem_style;

    for (const auto& rule : impl.generated_content_rules) {
        if (rule.position != position) continue;
        if (!generated_rule_matches(impl, rule, elem, selector_parent_idx))
            continue;
        if (rule.decls && impl.resolver) {
            impl.resolver->apply_decl_list(rule.decls, pseudo_style);
        }
        if (!rule.content_value.empty()) {
            has_content = true;
            content_value = rule.content_value;
        }
        if (!rule.color_value.empty()) {
            std::uint32_t parsed = color_rgba;
            if (parse_generated_color(rule.color_value, elem_style, parsed)) {
                has_color = true;
                color_rgba = parsed;
                pseudo_style.animated.color_rgba = parsed;
            }
        }
        if (!rule.background_value.empty()) {
            std::uint32_t parsed = pseudo_style.animated.background_rgba;
            if (parse_generated_color(rule.background_value, elem_style,
                                      parsed)) {
                pseudo_style.animated.background_rgba = parsed;
            }
        }
        if (!rule.background_color_value.empty()) {
            std::uint32_t parsed = pseudo_style.animated.background_rgba;
            if (parse_generated_color(rule.background_color_value, elem_style,
                                      parsed)) {
                pseudo_style.animated.background_rgba = parsed;
            }
        }
        if (!rule.padding_left_value.empty()) {
            has_padding_left = true;
            padding_left =
                parse_generated_length_px(rule.padding_left_value, elem_style);
        }
        if (!rule.padding_right_value.empty()) {
            has_padding_right = true;
            padding_right =
                parse_generated_length_px(rule.padding_right_value, elem_style);
        }
    }

    if (!has_content) return;
    if (!generated_content_enabled(content_value, elem_style)) return;
    auto text = generated_content_text(std::move(content_value), elem_style);
    if (text.empty()) {
        append_generated_box(impl, std::move(pseudo_style),
                             generated_parent_idx, position);
        return;
    }

    const int inline_parent_idx =
        ensure_inline_run(impl, elem_style, generated_parent_idx,
                          open_synth_idx);
    if (pending_inline_space) {
        append_anonymous_inline_text(impl, elem_style, inline_parent_idx, " ");
        pending_inline_space = false;
    }
    append_generated_inline_text(
        impl, elem_style, inline_parent_idx, std::move(text),
        has_padding_left ? padding_left : 0,
        has_padding_right ? padding_right : 0,
        has_color, color_rgba, position);
}

void apply_pseudo_overlay(detail::DocumentImpl& impl, const Block& block,
                          detail::ResolvedStyle& rs);

// Recursive DFS collector. Walks the DOM, creates one Block per
// block-level element, links it to its parent, and wraps inline text
// runs in synthetic line boxes. Leaf blocks can still own direct text,
// but mixed content such as "text <strong>inline</strong> <button>"
// is represented as anonymous inline text boxes instead of being
// dropped when a block-level child is present.
void collect_blocks(detail::DocumentImpl& impl,
                    lxb_dom_node_t* node,
                    const detail::ResolvedStyle& parent_style,
                    int parent_idx) {
    // Per-recursion-level state: the index of an open synthetic
    // line-box block wrapping a run of inline / inline-block
    // siblings. -1 means we're not currently in such a run; the
    // next inline child opens a fresh line-box, and the next
    // non-inline child resets back to the actual parent.
    int  open_synth_idx = -1;
    bool pending_inline_space = false;
    auto* current_elem = node && node->type == LXB_DOM_NODE_TYPE_ELEMENT
        ? lxb_dom_interface_element(node)
        : nullptr;
    const int current_selector_parent_idx =
        current_elem && parent_idx >= 0
            ? impl.blocks[static_cast<std::size_t>(parent_idx)].parent_idx
            : -1;
    if (current_elem) {
        append_generated_content_for_element(
            impl, current_elem, parent_style, parent_idx,
            current_selector_parent_idx,
            GeneratedContentRule::Position::Before, open_synth_idx,
            pending_inline_space);
    }

    for (auto* child = lxb_dom_node_first_child(node); child;
         child = lxb_dom_node_next(child)) {
        if (node_is_collapsible_whitespace(child)) {
            if (open_synth_idx >= 0) pending_inline_space = true;
            continue;
        }

        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            // Capture source leading/trailing whitespace BEFORE node_text
            // collapse-trims it. Between inline-level boxes CSS keeps one
            // collapsed space â€” so a text node's boundary whitespace is a
            // real inter-inline gap (e.g. "Heading <span class=badge>New"),
            // not something to drop. Only the line-box *edges* trim, which
            // falls out naturally: leading ws at run start (open_synth_idx
            // < 0) is ignored, and a dangling trailing space with no inline
            // sibling after it is never emitted.
            std::size_t rlen = 0;
            lxb_char_t* rraw = lxb_dom_node_text_content(child, &rlen);
            const bool lead_ws  = rraw && rlen && is_html_ws(rraw[0]);
            const bool trail_ws = rraw && rlen && is_html_ws(rraw[rlen - 1]);
            auto text = apply_text_transform(
                node_text(child, parent_style.computed.white_space),
                parent_style.computed.text_transform);
            if (!text.empty()) {
                if (lead_ws && open_synth_idx >= 0) pending_inline_space = true;
                const int inline_parent_idx =
                    ensure_inline_run(impl, parent_style, parent_idx,
                                      open_synth_idx);
                if (pending_inline_space) {
                    append_anonymous_inline_text(impl, parent_style,
                                                 inline_parent_idx, " ");
                    pending_inline_space = false;
                }
                append_anonymous_inline_text(impl, parent_style,
                                             inline_parent_idx,
                                             std::move(text));
                if (trail_ws) pending_inline_space = true;
            }
            continue;
        }

        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* elem = lxb_dom_interface_element(child);
        std::string tag = tag_name(elem);

        if (tag == "head" || tag == "script" || tag == "style" ||
            tag == "meta" || tag == "link"   || tag == "title")
            continue;

        // <option>/<optgroup> are not flow content â€” a <select> renders
        // only its chosen option's text (handled as the select's leaf
        // text below), so don't flatten every option into the box.
        if (tag == "svg")
            continue;

        if (tag == "option" || tag == "optgroup")
            continue;

        if (!is_block_tag(tag)) {
            auto rs_inline = impl.resolver->resolve(elem, parent_style);
            const auto elem_id_attr = attr_string(elem, "id");
            const auto cls_attr = split_classes(attr_string(elem, "class"));
            apply_font_family_fills(impl, tag, elem_id_attr, cls_attr,
                                    parent_idx,
                                    /*state_bits=*/0,
                                    rs_inline);

            using Display = detail::ComputedStyle::Display;
            using CssFloat = detail::ComputedStyle::Float;
            if (rs_inline.computed.display == Display::None) {
                open_synth_idx = -1;
                pending_inline_space = false;
                continue;
            }
            const bool parent_blockifies_inline_children =
                is_flex_container_display(parent_style.computed.display) ||
                parent_style.computed.display == Display::Grid ||
                parent_style.computed.display == Display::InlineGrid;
            const bool flatten_as_inline_text =
                !parent_blockifies_inline_children &&
                rs_inline.computed.css_float == CssFloat::None &&
                rs_inline.computed.display == Display::Inline;

            if (flatten_as_inline_text) {
                append_generated_content_for_element(
                    impl, elem, rs_inline, parent_idx, parent_idx,
                    GeneratedContentRule::Position::Before, open_synth_idx,
                    pending_inline_space);
                auto text = apply_text_transform(
                    node_text(child, rs_inline.computed.white_space),
                    rs_inline.computed.text_transform);
                if (!text.empty()) {
                    const int inline_parent_idx =
                        ensure_inline_run(impl, parent_style, parent_idx,
                                          open_synth_idx);
                    if (pending_inline_space) {
                        append_anonymous_inline_text(impl, parent_style,
                                                     inline_parent_idx, " ");
                        pending_inline_space = false;
                    }
                    append_anonymous_inline_text(impl, rs_inline,
                                                 inline_parent_idx,
                                                 std::move(text));
                }
                append_generated_content_for_element(
                    impl, elem, rs_inline, parent_idx, parent_idx,
                    GeneratedContentRule::Position::After, open_synth_idx,
                    pending_inline_space);
                continue;
            }
        }

        // Resolve this element's style under the parent's resolved
        // style (so inheritance flows correctly down the tree).
        const auto id = impl.style_store.acquire(elem);
        auto rs = impl.resolver->resolve(elem, parent_style);

        // Pseudo-class overlay (:hover, :active) â€” at collect time the
        // bits are preserved from any prior interaction state (they
        // survive reset/acquire). dispatch() re-resolves affected
        // blocks when chains change, so collect-time work is the
        // steady-state path. The block's parent_idx is `parent_idx`
        // (function arg), and impl.blocks already contains everything
        // up to but not including this element â€” exactly what
        // ancestor_chain_matches needs to walk.
        const auto sb_at_collect = impl.style_store.state_bits(id);
        const auto elem_id_attr  = attr_string(elem, "id");
        const auto cls_attr      = split_classes(attr_string(elem, "class"));
        const auto elem_attrs    = element_attrs(elem);

        if (!impl.pseudo_rules.empty()) {
            Block pseudo_block;
            pseudo_block.id = id;
            pseudo_block.tag = tag;
            pseudo_block.elem_id = elem_id_attr;
            pseudo_block.classes = cls_attr;
            pseudo_block.attrs = elem_attrs;
            pseudo_block.parent_idx = parent_idx;
            apply_pseudo_overlay(impl, pseudo_block, rs);
        }

        // Font-family fill overlay. Same selector grammar as pseudo
        // overlay; later rules win (the scan is in attach order, which
        // matches CSS source order).
        std::array<detail::GridTrackHint,
                   detail::kMaxGridTrackHints> grid_columns{};
        std::uint8_t grid_column_count = 0;
        apply_font_family_fills(impl, tag, elem_id_attr, cls_attr,
                                parent_idx, sb_at_collect, rs,
                                &grid_columns, &grid_column_count);
        if (auto value = scan_inline_decl_value(elem, "grid-template-columns");
            !value.empty()) {
            grid_column_count = parse_grid_template_columns(
                std::move(value), grid_columns);
        }
        if (tag == "textarea") {
            apply_user_textarea_size(impl, elem, rs);
        }

        // CSS display:none removes the element and its entire subtree from
        // layout/paint. Do this before appending a Block so descendants cannot
        // leak out at (0,0) when a framework hides a parent such as Bootstrap's
        // `.collapse:not(.show)`.
        if (rs.computed.display == detail::ComputedStyle::Display::None) {
            open_synth_idx = -1;
            pending_inline_space = false;
            continue;
        }

        impl.style_store.computed(id) = rs.computed;
        impl.style_store.animated(id) = rs.animated;

        if (auto kw = scan_inline_keyword(elem, "cursor"); !kw.empty()) {
            impl.style_store.computed(id).cursor = parse_cursor_keyword(kw);
        }
        if (auto kw = scan_inline_keyword(elem, "resize"); !kw.empty()) {
            const auto [resize, has_resize] = parse_resize_keyword(kw);
            if (has_resize) {
                impl.style_store.computed(id).resize = resize;
            }
        }
        impl.style_store.dirty(id) &=
            static_cast<std::uint8_t>(~detail::StyleStore::DirtyStyle);

        // Inline-run wrapping. Consecutive inline / inline-block
        // siblings get a synthetic flex-row-wrap line-box around
        // them so they flow horizontally instead of stacking. A
        // block-level sibling breaks the run; the next inline
        // sibling opens a fresh line-box.
        //
        // Flex-item blockification (CSS Flexbox Â§4): the children of a
        // flex container are flex items, and an inline-level flex item is
        // blockified. So when the parent establishes a flex formatting
        // context we DON'T group inline children into a line-box â€” each
        // becomes a direct block-level flex item. This is what makes an
        // `<a class="nav-link">` inside a `display:flex` navbar lay out as
        // a flex item rather than collapsing into an inline run.
        using Display = detail::ComputedStyle::Display;
        using CssFloat = detail::ComputedStyle::Float;
        const bool parent_is_flex =
            is_flex_container_display(parent_style.computed.display);
        const bool parent_is_grid =
            parent_style.computed.display == Display::Grid ||
            parent_style.computed.display == Display::InlineGrid;
        const bool parent_blockifies_inline_children =
            parent_is_flex || parent_is_grid;
        const bool child_is_float =
            !parent_blockifies_inline_children &&
            rs.computed.css_float != CssFloat::None;
        const bool child_is_inline =
            !parent_blockifies_inline_children &&
            !child_is_float &&
            (rs.computed.display == Display::Inline ||
             rs.computed.display == Display::InlineBlock ||
             rs.computed.display == Display::InlineFlex ||
             rs.computed.display == Display::InlineGrid);
        int effective_parent_idx;
        if (child_is_inline) {
            ensure_inline_run(impl, parent_style, parent_idx, open_synth_idx);
            if (pending_inline_space) {
                append_anonymous_inline_text(impl, parent_style,
                                             open_synth_idx, " ");
                pending_inline_space = false;
            }
            effective_parent_idx = open_synth_idx;
        } else {
            open_synth_idx       = -1;
            pending_inline_space = false;
            effective_parent_idx = parent_idx;
        }

        const int my_idx = static_cast<int>(impl.blocks.size());
        Block b;
        b.id         = id;
        b.tag        = std::move(tag);
        b.elem_id    = elem_id_attr;
        b.classes    = cls_attr;
        b.attrs      = elem_attrs;
        b.custom_props = rs.custom_props;
        b.box_shadows = rs.box_shadows;
        if (b.tag == "img") {
            b.image_src = attr_string(elem, "src");
        }
        if (b.tag == "input") {
            b.input_type  = attr_string(elem, "type");
            b.role_attr   = attr_string(elem, "role");
            b.is_checked  = has_attr(elem, "checked");
            b.is_disabled = has_attr(elem, "disabled");
        }
        if (b.tag == "textarea") {
            b.text_control = true;
            b.is_disabled = has_attr(elem, "disabled");
            b.placeholder = attr_string(elem, "placeholder");
        } else if (b.tag == "input") {
            b.text_control = input_type_accepts_text(b.input_type);
            b.placeholder = attr_string(elem, "placeholder");
        }
        b.parent_idx = effective_parent_idx;
        if ((rs.computed.display == detail::ComputedStyle::Display::Grid ||
             rs.computed.display == detail::ComputedStyle::Display::InlineGrid) &&
            grid_column_count > 0) {
            b.grid_columns = grid_columns;
            b.grid_column_count = grid_column_count;
        }
        b.base_animated = rs.animated;
        b.animation = rs.animation;
        b.animation_epoch = impl.animation_epoch;
        impl.blocks.push_back(std::move(b));

        // SVG child elements live in SVG's own presentation tree, not the
        // HTML box tree. Keep them out of the HTML style/layout resolver and
        // let the SVG paint helper walk the foreign-content subtree directly.
        if (impl.blocks[static_cast<std::size_t>(my_idx)].tag == "svg") {
            continue;
        }

        // Recurse â€” children get my_idx as their parent. Track whether
        // any blocks were appended; if not, this block is a leaf and
        // gets the concatenated descendant text.
        // Textarea child text is the control value, not child layout content.
        const std::size_t before = impl.blocks.size();
        if (impl.blocks[static_cast<std::size_t>(my_idx)].tag != "textarea") {
            collect_blocks(impl, child, rs, my_idx);
        }
        const bool is_leaf =
            impl.blocks[static_cast<std::size_t>(my_idx)].tag == "textarea" ||
            (impl.blocks.size() == before);
        if (is_leaf) {
            auto& leaf = impl.blocks[static_cast<std::size_t>(my_idx)];
            if (leaf.tag == "input") {
                if (leaf.text_control) {
                    auto* elem_node = lxb_dom_interface_node(elem);
                    auto live = impl.live_text_values.find(
                        elem_node);
                    leaf.text_value = live != impl.live_text_values.end()
                        ? live->second
                        : attr_string(elem, "value");
                    auto caret = impl.live_text_carets.find(elem_node);
                    leaf.caret_offset = caret != impl.live_text_carets.end()
                        ? std::min(caret->second, leaf.text_value.size())
                        : leaf.text_value.size();
                    auto selection =
                        impl.live_text_selections.find(elem_node);
                    if (selection != impl.live_text_selections.end()) {
                        leaf.selection_anchor =
                            std::min(selection->second.first,
                                     leaf.text_value.size());
                        leaf.selection_focus =
                            std::min(selection->second.second,
                                     leaf.text_value.size());
                    } else {
                        leaf.selection_anchor = leaf.caret_offset;
                        leaf.selection_focus = leaf.caret_offset;
                    }
                    leaf.text = text_control_display_value(leaf, leaf.text_value);
                    if (leaf.text.empty() && !leaf.placeholder.empty()) {
                        leaf.text = leaf.placeholder;
                        leaf.placeholder_visible = true;
                    }
                }
            } else if (leaf.tag == "select") {
                leaf.text = select_display_text(elem);
            } else if (leaf.tag == "textarea") {
                auto* elem_node = lxb_dom_interface_node(elem);
                auto live = impl.live_text_values.find(
                    elem_node);
                leaf.text_value = live != impl.live_text_values.end()
                    ? live->second
                    : node_text(child, rs.computed.white_space);
                auto caret = impl.live_text_carets.find(elem_node);
                leaf.caret_offset = caret != impl.live_text_carets.end()
                    ? std::min(caret->second, leaf.text_value.size())
                    : leaf.text_value.size();
                auto selection = impl.live_text_selections.find(elem_node);
                if (selection != impl.live_text_selections.end()) {
                    leaf.selection_anchor =
                        std::min(selection->second.first,
                                 leaf.text_value.size());
                    leaf.selection_focus =
                        std::min(selection->second.second,
                                 leaf.text_value.size());
                } else {
                    leaf.selection_anchor = leaf.caret_offset;
                    leaf.selection_focus = leaf.caret_offset;
                }
                leaf.text = leaf.text_value;
                if (leaf.text.empty() && !leaf.placeholder.empty()) {
                    leaf.text = leaf.placeholder;
                    leaf.placeholder_visible = true;
                }
            } else if (leaf.tag != "img") {
                leaf.text = apply_text_transform(
                    node_text(child, rs.computed.white_space),
                    rs.computed.text_transform);
            }
        }
    }

    if (current_elem) {
        append_generated_content_for_element(
            impl, current_elem, parent_style, parent_idx,
            current_selector_parent_idx,
            GeneratedContentRule::Position::After, open_synth_idx,
            pending_inline_space);
    }
}

// Resolve table column widths so cells in the same column line up across
// rows, then pin each cell's content width via intrinsic_w_px. Runs on the
// *natural* (first-pass) layout: `natural[i].w` is the width Yoga gave
// block i with cells free to size to content. Returns true if any table
// was found, so the caller re-runs layout with the pinned widths.
//
// Scope: no rowspan/colspan (column index = a cell's position in its row);
// border-collapse is approximated (each cell keeps its own borders). Both
// match what the current corpus needs; widen later if a test requires it.
bool assign_table_column_widths(std::vector<detail::BlockLayoutInput>& inputs,
                                const std::vector<Rect>& natural) {
    using D = detail::ComputedStyle::Display;
    const int n = static_cast<int>(inputs.size());
    bool found = false;

    for (int t = 0; t < n; ++t) {
        if (!inputs[t].style || inputs[t].style->display != D::Table) continue;
        found = true;

        // Rows = TableRow children of the table, plus TableRow children of
        // any TableRowGroup (thead/tbody/tfoot) child of the table.
        std::vector<int> rows;
        for (int c = t + 1; c < n; ++c) {
            const auto* cs = inputs[c].style;
            if (!cs || inputs[c].parent_idx != t) continue;
            if (cs->display == D::TableRow) {
                rows.push_back(c);
            } else if (cs->display == D::TableRowGroup) {
                for (int r = c + 1; r < n; ++r) {
                    if (inputs[r].parent_idx == c && inputs[r].style &&
                        inputs[r].style->display == D::TableRow) {
                        rows.push_back(r);
                    }
                }
            }
        }
        if (rows.empty()) continue;

        // Per-column width = widest natural cell in that column. Collect the
        // cells per row so we can pin them after.
        std::vector<int> colw;
        std::vector<std::vector<int>> row_cells;
        row_cells.reserve(rows.size());
        for (int r : rows) {
            std::vector<int> cells;
            for (int c = r + 1; c < n; ++c) {
                if (inputs[c].parent_idx == r && inputs[c].style &&
                    inputs[c].style->display == D::TableCell) {
                    cells.push_back(c);
                }
            }
            for (std::size_t j = 0; j < cells.size(); ++j) {
                const int w = natural[static_cast<std::size_t>(cells[j])].w;
                if (j >= colw.size()) colw.resize(j + 1, 0);
                if (w > colw[j]) colw[j] = w;
            }
            row_cells.push_back(std::move(cells));
        }
        const int ncols = static_cast<int>(colw.size());
        if (ncols == 0) continue;

        // Scale columns to fill the table's resolved content width
        // (proportional to their natural widths). Using the first-pass
        // laid-out table width (natural[t].w) handles every case
        // uniformly: explicit px, percentage (e.g. Bootstrap's
        // width:100%), and auto (where it equals the natural content sum,
        // so the scale is a no-op).
        const auto& tcs = *inputs[t].style;
        const int table_w = natural[static_cast<std::size_t>(t)].w
                          - tcs.padding_left - tcs.padding_right
                          - tcs.used_border_left() - tcs.used_border_right();
        long long sum = 0;
        for (int w : colw) sum += w;
        if (table_w > 0 && sum > 0) {
            int assigned = 0;
            for (int j = 0; j < ncols; ++j) {
                colw[j] = static_cast<int>(
                    static_cast<long long>(table_w) * colw[j] / sum);
                assigned += colw[j];
            }
            colw[static_cast<std::size_t>(ncols - 1)] += table_w - assigned;
        }

        // Pin each cell so its border box equals the column width and the
        // columns line up. intrinsic_w_px feeds YGNodeStyleSetWidth, whose
        // meaning depends on the cell's box-sizing:
        //   border-box (Bootstrap's `*{box-sizing:border-box}`): the width
        //     IS the border box â€” pin colw[j] directly. (Subtracting
        //     padding+border here is the bug that left every cell ~18px too
        //     narrow, summing to a phantom empty column on the table's right.)
        //   content-box: Yoga adds padding+border back â€” pin the content box.
        using BoxSizing = detail::ComputedStyle::BoxSizing;
        for (const auto& cells : row_cells) {
            for (std::size_t j = 0; j < cells.size(); ++j) {
                const auto& ccs = *inputs[static_cast<std::size_t>(cells[j])].style;
                int w = (ccs.box_sizing == BoxSizing::BorderBox)
                    ? colw[j]
                    : colw[j] - ccs.padding_left - ccs.padding_right
                              - ccs.used_border_left() - ccs.used_border_right();
                if (w < 0) w = 0;
                inputs[static_cast<std::size_t>(cells[j])].intrinsic_w_px = w;
            }
        }
    }
    return found;
}

#endif  // !AFFINEUI_STUB_BUILD

}  // namespace

Document::Document() : impl_{std::make_unique<detail::DocumentImpl>()} {}
Document::~Document() = default;

Document::Document(Document&&) noexcept            = default;
Document& Document::operator=(Document&&) noexcept = default;

void Document::set_html(std::string_view html) {
    const auto previous_scroll =
        snapshot_scroll_state(*impl_, /*include_elements=*/false);
    impl_->html.assign(html);
    impl_->blocks.clear();
    impl_->style_store.reset();
    impl_->paint_dirty = true;
    impl_->content_size = Size{0, 0};
    impl_->dirty_rects.clear();
    impl_->pending_dirty_roots.clear();
    impl_->activated_widgets.clear();
    impl_->changed_widgets.clear();
    impl_->animation_candidate_count = 0;
#if !defined(AFFINEUI_STUB_BUILD)
    impl_->scrollbar_drag = {};
    impl_->splitter_drag = {};
    impl_->float_drag = {};
    impl_->tab_drag = {};
    impl_->live_drag = {};
#else
    impl_->scrollbar_drag = {};
#endif
    for (auto& slot : impl_->dom_weak_slots) {
#if !defined(AFFINEUI_STUB_BUILD)
        slot.node = nullptr;
#endif
        ++slot.generation;
        if (slot.generation == 0) slot.generation = 1;
    }

#if !defined(AFFINEUI_STUB_BUILD)
    // Tear down the previous document; its CSS pool owns the
    // attached stylesheets, so destroying doc tears them down too.
    impl_->resolver.reset();
    impl_->sheets.clear();
    impl_->pseudo_rules.clear();
    impl_->rule_fills.clear();
    impl_->generated_content_rules.clear();
    impl_->font_faces.clear();
    impl_->media_blocks.clear();
    impl_->keyframes.clear();
    impl_->live_text_values.clear();
    impl_->live_text_carets.clear();
    impl_->live_text_selections.clear();
    impl_->dcs_select_anchors.clear();
    impl_->text_layout_cache.clear();
    impl_->text_layout_signatures.clear();
    impl_->text_selection_drag_idx = -1;
    impl_->last_text_click_valid = false;
    impl_->hovered_chain.clear();
    impl_->active_chain.clear();
    impl_->hovered_idx = -1;
    impl_->active_idx = -1;
    impl_->focused_idx = -1;
    impl_->animation_epoch = std::chrono::steady_clock::now();
    if (impl_->doc) {
        lxb_html_document_destroy(impl_->doc);
        impl_->doc = nullptr;
    }

    impl_->doc = lxb_html_document_create();
    if (!impl_->doc) return;

    // Initialise CSS subsystems on the document BEFORE parsing HTML so
    // any <style>/style="..." inline declarations are kept.
    if (lxb_html_document_css_init(impl_->doc) != LXB_STATUS_OK) {
        return;
    }
    detail::install_dom_event_hooks(*impl_);

    if (lxb_html_document_parse(
            impl_->doc,
            reinterpret_cast<const lxb_char_t*>(impl_->html.data()),
            impl_->html.size()) != LXB_STATUS_OK) {
        return;
    }

    // Cascade order (lower â†’ higher specificity, ties to last):
    //   1. User-agent baseline
    //   2. Author <style> blocks from the page
    //   3. User stylesheet (App-supplied, often a framework/theme)
    // Matching @media blocks are attached by attach_stylesheet() after the
    // stylesheet that owns them once a viewport is known.
    attach_stylesheet(*impl_, theme::ua_default());

    std::string author_css;
    collect_author_stylesheets(lxb_dom_interface_node(impl_->doc), *impl_, author_css);
    attach_stylesheet(*impl_, author_css);

    attach_stylesheet(*impl_, impl_->user_stylesheet,
                      impl_->user_stylesheet_base_url);

    // Resolver runs against the now fully-cascade-attached document.
    impl_->resolver = detail::make_lexbor_resolver(
        impl_->doc, impl_->media_viewport_width_px,
        impl_->media_viewport_height_px);
    impl_->media_match_signature =
        media_match_signature(*impl_, impl_->media_viewport_width_px);

    // Establish a root inheritance baseline. Reasonable initial values
    // for the implicit document root â€” anything not overridden by CSS
    // gets these. AnimatedStyle's foreground defaults to near-white
    // (#dcdce6) so unstyled docs are readable on the dark clear color.
    impl_->root_style                       = detail::ResolvedStyle{};
    impl_->root_style.animated.color_rgba   = 0xDCDCE6FFu;
    impl_->root_style.computed.font_size_px = 16;
    impl_->root_style.computed.font_weight  = 400;

    // Use <body>'s resolved style as the parent for all blocks so
    // body-level CSS (e.g. `body { color: ... }`) inherits down.
    // Store the resolved body style BACK into root_style so the
    // dispatch-time restyle path (parent_resolved for parent_idx=-1)
    // sees the same parent the initial collect used. Without this,
    // hover-triggered restyles silently reset top-level blocks to
    // the placeholder root color, leaking grey into things like h1
    // that should be inheriting body's color.
    auto* body = lxb_html_document_body_element(impl_->doc);
    if (body) {
        // Resolve <html> first so `:root { --custom: ... }` properties
        // (e.g. Bootstrap's whole --bs-* palette) are collected and
        // inherited down through <body> into every block. <html> is
        // <body>'s parent node.
        detail::ResolvedStyle html_style = impl_->root_style;
        auto* body_node = lxb_dom_interface_node(body);
        if (body_node->parent != nullptr &&
            body_node->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            html_style = impl_->resolver->resolve(
                lxb_dom_interface_element(body_node->parent), impl_->root_style);
        }
        auto* body_elem = lxb_dom_interface_element(body_node);
        impl_->root_style = impl_->resolver->resolve(body_elem, html_style);
        apply_font_family_fills(*impl_, "body", attr_string(body_elem, "id"),
                                split_classes(attr_string(body_elem, "class")),
                                /*parent_idx=*/-1,
                                /*state_bits=*/0,
                                impl_->root_style);
    }
    collect_blocks(*impl_,
                   body ? lxb_dom_interface_node(body)
                        : lxb_dom_interface_node(impl_->doc),
                   impl_->root_style,
                   /*parent_idx=*/-1);
    restore_scroll_state(*impl_, previous_scroll);
    for (const auto& b : impl_->blocks) {
        if (b.animation.active && b.animation.name_hash != 0) {
            ++impl_->animation_candidate_count;
        }
    }
#endif
}

void Document::attach_script(DocumentScript script) {
    switch (script) {
        case DocumentScript::UiControls:
            impl_->ui_control_script_attached = true;
            break;
    }
}

void Document::detach_script(DocumentScript script) {
    switch (script) {
        case DocumentScript::UiControls:
            impl_->ui_control_script_attached = false;
#if !defined(AFFINEUI_STUB_BUILD)
            impl_->live_drag = {};
#endif
            break;
    }
}

void Document::clear_scripts() {
    impl_->ui_control_script_attached = false;
#if !defined(AFFINEUI_STUB_BUILD)
    impl_->live_drag = {};
#endif
}

std::vector<std::string> Document::take_activated_widgets() {
    auto out = std::move(impl_->activated_widgets);
    impl_->activated_widgets.clear();
    return out;
}

std::vector<Document::WidgetChange> Document::take_widget_changes() {
    auto out = std::move(impl_->changed_widgets);
    impl_->changed_widgets.clear();
    return out;
}

std::vector<std::pair<std::string, int>> Document::dock_pane_sizes() const {
    std::vector<std::pair<std::string, int>> out;
    for (const auto& b : impl_->blocks) {
        if (std::find(b.classes.begin(), b.classes.end(), "dcs-dockpane") ==
            b.classes.end())
            continue;
        std::string_view name;
        std::string_view style;
        for (const auto& a : b.attrs) {
            if (a.first == "data-aui-name") name = a.second;
            else if (a.first == "style") style = a.second;
        }
        if (name.rfind("pane-", 0) != 0) continue;  // engine names panes pane-<id>
        // Parse the fixed flex-basis: "flex:0 0 <N>px".
        const auto pos = style.find("flex:0 0 ");
        if (pos == std::string_view::npos) continue;  // flexible center pane
        const char* first = style.data() + pos + 9;
        const char* last = style.data() + style.size();
        int px = 0;
        const auto [ptr, ec] = std::from_chars(first, last, px);
        if (ec != std::errc{} || px <= 0) continue;
        (void) ptr;
        out.emplace_back(std::string(name.substr(5)), px);
    }
    return out;
}

Document::DockPlacement Document::dock_override(std::string_view panel_id) const {
    const auto it = impl_->dock_overrides.find(std::string(panel_id));
    return it == impl_->dock_overrides.end() ? DockPlacement{} : it->second;
}

std::vector<std::pair<std::string, Document::DockPlacement>>
Document::dock_overrides() const {
    std::vector<std::pair<std::string, DockPlacement>> out;
    out.reserve(impl_->dock_overrides.size());
    for (const auto& [id, p] : impl_->dock_overrides) out.emplace_back(id, p);
    return out;
}

std::string Document::dock_active_tab(std::string_view pane_id) const {
    const auto it = impl_->dock_active_tabs.find(std::string(pane_id));
    return it == impl_->dock_active_tabs.end() ? std::string{} : it->second;
}

void Document::set_user_stylesheet(std::string_view css) {
    set_user_stylesheet(css, {});
}

void Document::set_user_stylesheet(std::string_view css,
                                   std::string_view base_url) {
    impl_->user_stylesheet.assign(css);
    impl_->user_stylesheet_base_url.assign(base_url);
    if (!impl_->html.empty()) {
        set_html(impl_->html);
    } else {
        impl_->paint_dirty = true;
    }
}

void Document::reload_stylesheets() {
    if (!impl_->html.empty()) set_html(impl_->html);
}

namespace {
#if !defined(AFFINEUI_STUB_BUILD)
void add_dirty_rect(detail::DocumentImpl& impl, const Rect& r);
Rect subtree_visual_rect(const detail::DocumentImpl& impl, int root_idx);
std::uint64_t media_match_signature(const detail::DocumentImpl& impl,
                                    int viewport_width);
void recollect_blocks_from_current_dom(detail::DocumentImpl& impl);
void attach_matching_media_blocks_for_viewport(detail::DocumentImpl& impl) {
    if (!impl.doc || impl.media_viewport_width_px <= 0) return;
    const std::size_t media_count = impl.media_blocks.size();
    for (std::size_t i = 0; i < media_count; ++i) {
        if (impl.media_blocks[i].matches(impl.media_viewport_width_px)) {
            attach_media_block(impl, impl.media_blocks[i]);
        }
    }
    impl.resolver = detail::make_lexbor_resolver(
        impl.doc, impl.media_viewport_width_px,
        impl.media_viewport_height_px);
    impl.media_match_signature =
        media_match_signature(impl, impl.media_viewport_width_px);
    recollect_blocks_from_current_dom(impl);
}
#endif
}  // namespace

namespace {
#if !defined(AFFINEUI_STUB_BUILD)
TextControlGeometry text_control_geometry(const detail::DocumentImpl& impl,
                                          int idx,
                                          Painter& painter);
TextLayoutEntry& ensure_text_layout_entry(detail::DocumentImpl& impl,
                                          int idx,
                                          const TextControlGeometry& g,
                                          const Block& block,
                                          Painter& painter);
#endif
}  // namespace

void Document::layout(int viewport_width, int viewport_height,
                      Painter* measurer) {
    // Layout delegates to Yoga via src/layout/yoga_adapter. Text
    // leaves get a Yoga measure callback that calls nvgTextBoxBounds
    // â€” Yoga asks "given width W, what height?" and we return the
    // *actually rendered* wrapped bbox. No metric heuristics; the
    // top/bottom padding ends up symmetric for free because the
    // content area matches what the painter will draw into.
    //
    // Page gutter is driven by body's CSS padding. The UA stylesheet
    // keeps body padding at the browser-compatible zero default;
    // demos or applications that want a gutter author it explicitly.

#if !defined(AFFINEUI_STUB_BUILD)
    // Viewport-dependent cascade: this is the one call site that knows
    // the real CSS viewport. Rebuild the parsed HTML/CSS attachment graph
    // only when the active @media set changes. Ordinary resize ticks still
    // need fresh computed styles for vw/vh/calc(), but they can be collected
    // from the current live DOM; reparsing from impl_->html would lose live
    // attribute/text mutations and makes interactive resizing much heavier.
    if (!impl_->html.empty() &&
        (viewport_width != impl_->media_viewport_width_px ||
         viewport_height != impl_->media_viewport_height_px)) {
        const bool width_changed =
            viewport_width != impl_->media_viewport_width_px;
        const bool height_changed =
            viewport_height != impl_->media_viewport_height_px;
        const bool first_viewport =
            impl_->media_viewport_width_px <= 0 ||
            impl_->media_viewport_height_px <= 0;
        const auto next_media_sig =
            media_match_signature(*impl_, viewport_width);
        const bool media_set_changed =
            first_viewport ||
            next_media_sig != impl_->media_match_signature;
        const auto viewport_dependency =
            impl_->resolver ? impl_->resolver->viewport_dependency()
                            : detail::ViewportDependency{true, true};
        const bool computed_style_viewport_changed =
            first_viewport ||
            (width_changed && viewport_dependency.width) ||
            (height_changed && viewport_dependency.height);
        impl_->media_viewport_width_px = viewport_width;
        impl_->media_viewport_height_px = viewport_height;
        if (media_set_changed) {
            if (first_viewport) {
                attach_matching_media_blocks_for_viewport(*impl_);
            } else {
                // Changing an already-attached media set requires detaching old
                // stylesheet matches. Keep this conservative until the Lexbor
                // stylesheet replacement path is hardened.
                const std::string html = impl_->html;
                set_html(html);
            }
        } else if (computed_style_viewport_changed) {
            impl_->resolver = detail::make_lexbor_resolver(
                impl_->doc, impl_->media_viewport_width_px,
                impl_->media_viewport_height_px);
            recollect_blocks_from_current_dom(*impl_);
            impl_->media_match_signature = next_media_sig;
        } else {
            // A resize can change available layout width without changing any
            // computed CSS values. Keep the existing block/style tree and only
            // rerun Yoga below. This is the hot path for Bootstrap dashboards:
            // media queries stay in the same bucket and their active rules use
            // vh but not vw, so horizontal resize does not need a full cascade.
            if (impl_->resolver) {
                impl_->resolver->set_viewport(impl_->media_viewport_width_px,
                                              impl_->media_viewport_height_px);
            }
            impl_->media_match_signature = next_media_sig;
        }
    }
#endif

#if !defined(AFFINEUI_STUB_BUILD)
    if (measurer != nullptr) {
        impl_->last_measurer = measurer;
        ensure_font_faces_registered(*impl_, *measurer);
    }
#endif

    int pad_l = 0, pad_t = 0, pad_r = 0, pad_b = 0;
#if !defined(AFFINEUI_STUB_BUILD)
    if (impl_->doc && impl_->resolver) {
        if (auto* body = lxb_html_document_body_element(impl_->doc); body) {
            const auto rs = impl_->resolver->resolve(
                lxb_dom_interface_element(lxb_dom_interface_node(body)),
                impl_->root_style);
            pad_l = rs.computed.padding_left;
            pad_t = rs.computed.padding_top;
            pad_r = rs.computed.padding_right;
            pad_b = rs.computed.padding_bottom;
        }
    }
#endif

    std::vector<std::vector<int>> child_indices(impl_->blocks.size());
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        const int parent = impl_->blocks[i].parent_idx;
        if (parent >= 0) child_indices[static_cast<std::size_t>(parent)]
            .push_back(static_cast<int>(i));
    }

    std::vector<float> block_baselines(impl_->blocks.size(), 0.0f);
    if (measurer != nullptr) {
        for (int i = static_cast<int>(impl_->blocks.size()) - 1; i >= 0; --i) {
            const auto& b = impl_->blocks[static_cast<std::size_t>(i)];
            const auto& cs = impl_->style_store.computed(b.id);

            if (!b.text.empty()) {
                const auto font = measurer->resolve_font(
                    impl_->style_store.font_family_of(cs.font_id),
                    cs.font_size_px, cs.font_weight, cs.font_style != 0);
                const auto tm = measurer->text_metrics(font);
                const float css_line_h = std::max(
                    static_cast<float>(cs.font_size_px) *
                        detail::effective_line_height_mult(cs),
                    1.0f);
                if (tm.ascender > 0.0f && tm.line_height > 0.0f) {
                    const float leading = css_line_h - tm.line_height;
                    block_baselines[static_cast<std::size_t>(i)] =
                        static_cast<float>(cs.used_border_top() + cs.padding_top) +
                        leading * 0.5f + tm.ascender;
                } else {
                    block_baselines[static_cast<std::size_t>(i)] =
                        static_cast<float>(cs.used_border_top() + cs.padding_top) +
                        css_line_h;
                }
                continue;
            }

            const auto& kids = child_indices[static_cast<std::size_t>(i)];
            if (b.synthetic) {
                float baseline = 0.0f;
                for (const int child : kids) {
                    baseline = std::max(
                        baseline,
                        block_baselines[static_cast<std::size_t>(child)]);
                }
                block_baselines[static_cast<std::size_t>(i)] = baseline;
                continue;
            }

            using D = detail::ComputedStyle::Display;
            if (cs.display == D::InlineBlock || cs.display == D::Inline ||
                cs.display == D::TableCell) {
                for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                    const float child_baseline =
                        block_baselines[static_cast<std::size_t>(*it)];
                    if (child_baseline > 0.0f) {
                        block_baselines[static_cast<std::size_t>(i)] =
                            static_cast<float>(cs.used_border_top() + cs.padding_top) +
                            child_baseline;
                        break;
                    }
                }
            }
        }
    }

    std::vector<detail::ComputedStyle> layout_styles;
    layout_styles.reserve(impl_->blocks.size());
    for (const auto& b : impl_->blocks) {
        layout_styles.push_back(impl_->style_store.computed(b.id));
    }

    // NanoVG's bounds measurement reports ink extents, while its text-box
    // wrapping decisions use glyph advances. Paint gets the same slack before
    // draw_text_box; min-content sizing needs it too or tight controls can
    // wrap their final glyph even though measurement said the label fit.
    //
    // Generated pseudo-content is commonly used for icons. Its inline-block
    // box should size to the glyph advance itself; adding label slack there
    // makes centered icon controls look left-biased.
    constexpr int kTextAdvanceSlackPx = 4;
    auto text_advance_slack = [](const Block& b) {
        return (b.tag == "#before" || b.tag == "#after")
            ? 0
            : kTextAdvanceSlackPx;
    };

    collapse_block_flow_vertical_margins(child_indices, impl_->blocks,
                                         layout_styles);

    std::vector<detail::BlockLayoutInput> inputs;
    inputs.reserve(impl_->blocks.size());
    for (std::size_t bi = 0; bi < impl_->blocks.size(); ++bi) {
        auto& b = impl_->blocks[bi];
        const auto& cs = layout_styles[bi];
        detail::BlockLayoutInput in{};
        in.style          = &cs;
        in.parent_idx     = b.parent_idx;
        in.intrinsic_w_px = 0;  // let parent stretch on cross axis
        in.inline_parent  =
            b.parent_idx >= 0 &&
            impl_->blocks[static_cast<std::size_t>(b.parent_idx)].synthetic;
        in.baseline_px    = block_baselines[bi];
        if ((cs.display == detail::ComputedStyle::Display::Grid ||
             cs.display == detail::ComputedStyle::Display::InlineGrid) &&
            b.grid_column_count > 0) {
            in.grid_columns = b.grid_columns;
            in.grid_column_count = b.grid_column_count;
        }

        if (!b.image_src.empty()) {
            if (measurer != nullptr) {
                const auto image = measurer->load_image(b.image_src);
                const auto sz = measurer->image_size(image);
                if (sz.width > 0 && sz.height > 0) {
                    if (cs.width > 0 && cs.height <= 0) {
                        in.intrinsic_h_px =
                            std::max(1, (sz.height * cs.width) / sz.width);
                    } else if (cs.height > 0 && cs.width <= 0) {
                        in.intrinsic_w_px =
                            std::max(1, (sz.width * cs.height) / sz.height);
                    } else if (cs.width <= 0 && cs.height <= 0) {
                        in.intrinsic_w_px = sz.width;
                        in.intrinsic_h_px = sz.height;
                    }
                }
            }
            inputs.push_back(in);
            continue;
        }

        // Container blocks (no direct text â€” wrap child blocks) leave
        // intrinsic_h at 0 so Yoga sizes them from their children's
        // resolved heights + their own padding/border.
        if (b.tag == "textarea" && b.text_control) {
            if (measurer != nullptr) {
                in.font = measurer->resolve_font(
                    impl_->style_store.font_family_of(cs.font_id),
                    cs.font_size_px, cs.font_weight, cs.font_style != 0);
                const int ch =
                    std::max(1, measurer->measure_text(in.font, "0"));
                const int rows = block_attr_int(b, "rows", 2, 1, 1000);
                const int cols = block_attr_int(b, "cols", 20, 1, 1000);
                const int control_w =
                    ch * cols + cs.padding_left + cs.padding_right +
                    cs.used_border_left() + cs.used_border_right();
                const int line_h = std::max(
                    1, static_cast<int>(std::ceil(
                           static_cast<float>(cs.font_size_px) *
                           detail::effective_line_height_mult(cs))));
                const int control_h =
                    line_h * rows + cs.padding_top + cs.padding_bottom +
                    cs.used_border_top() + cs.used_border_bottom();
                if (cs.width < 0 && cs.width_pct_x100 < 0) {
                    in.intrinsic_w_px = control_w;
                }
                if (cs.height < 0 && cs.height_pct < 0) {
                    in.intrinsic_h_px = control_h;
                }
            } else {
                const int ch = std::max(1, static_cast<int>(cs.font_size_px) / 2);
                const int cols = block_attr_int(b, "cols", 20, 1, 1000);
                if (cs.width < 0 && cs.width_pct_x100 < 0) {
                    in.intrinsic_w_px =
                        ch * cols + cs.padding_left + cs.padding_right +
                        cs.used_border_left() + cs.used_border_right();
                }
                in.intrinsic_h_px =
                    cs.font_size_px * block_attr_int(b, "rows", 2, 1, 1000) +
                    cs.padding_top + cs.padding_bottom +
                    cs.used_border_top() + cs.used_border_bottom();
            }
            inputs.push_back(in);
            continue;
        }

        if (b.text.empty()) {
            in.intrinsic_h_px = 0;
            inputs.push_back(in);
            continue;
        }

        // Text-bearing leaf. Hand it the text + a font handle; the
        // adapter wires a Yoga measure callback that runs the live
        // painter's nvgTextBoxBounds per constraint width.
        if (measurer != nullptr) {
            in.font = measurer->resolve_font(
                impl_->style_store.font_family_of(cs.font_id), cs.font_size_px, cs.font_weight, cs.font_style != 0);
            in.text = b.text;
            in.letter_spacing_px = static_cast<float>(cs.letter_spacing_x100) / 100.0f;
            if (cs.min_width < 0 &&
                b.parent_idx >= 0 &&
                static_cast<std::size_t>(b.parent_idx) < impl_->blocks.size()) {
                const auto& parent_style =
                    layout_styles[static_cast<std::size_t>(b.parent_idx)];
                if (is_flex_container_display(parent_style.display) &&
                (parent_style.flex_direction ==
                         detail::ComputedStyle::FlexDirection::Row ||
                     parent_style.flex_direction ==
                         detail::ComputedStyle::FlexDirection::RowReverse)) {
                    in.auto_min_w_px =
                        std::max(1, measurer->measure_text(in.font, b.text))
                        + text_advance_slack(b)
                        + cs.padding_left + cs.padding_right
                        + cs.used_border_left() + cs.used_border_right();
                }
            }
            const bool in_synthetic_inline_parent =
                b.parent_idx >= 0 &&
                static_cast<std::size_t>(b.parent_idx) < impl_->blocks.size() &&
                impl_->blocks[static_cast<std::size_t>(b.parent_idx)].synthetic;
            const bool first_in_synthetic_inline_parent =
                in_synthetic_inline_parent &&
                !child_indices[static_cast<std::size_t>(b.parent_idx)].empty() &&
                child_indices[static_cast<std::size_t>(b.parent_idx)].front() ==
                    static_cast<int>(bi);
            if ((!in_synthetic_inline_parent || first_in_synthetic_inline_parent) &&
                !cs.text_indent_is_pct) {
                in.text_indent_px = static_cast<float>(cs.text_indent_value);
            }
            if (b.text_control &&
                b.input_type != "checkbox" && b.input_type != "radio") {
                const int ch = std::max(1, measurer->measure_text(in.font, "0"));
                in.intrinsic_w_px = ch * 20
                    + cs.padding_left + cs.padding_right
                    + cs.used_border_left() + cs.used_border_right();
            }
            using WS = detail::ComputedStyle::WhiteSpace;
            in.nowrap = (cs.white_space == WS::Nowrap ||
                         cs.white_space == WS::Pre);
            // Leave intrinsic_h_px = 0 â€” the measure callback supplies
            // the height instead.
        } else {
            in.intrinsic_h_px = cs.font_size_px;
        }
        inputs.push_back(in);
    }

    if (measurer != nullptr) {
        std::vector<int> min_content_w(impl_->blocks.size(), 0);
        for (std::size_t ri = impl_->blocks.size(); ri-- > 0; ) {
            const auto& b = impl_->blocks[ri];
            const auto& cs = layout_styles[ri];
            int content_w = 0;

            if (!b.text.empty() && inputs[ri].font != 0) {
                content_w = std::max(
                    content_w,
                    std::max(1, measurer->measure_text(inputs[ri].font, b.text))
                    + text_advance_slack(b));
            }

            const auto& kids = child_indices[ri];
            if (!kids.empty()) {
                int children_w = 0;
                const bool row_flex =
                    is_flex_container_display(cs.display) &&
                    (cs.flex_direction ==
                         detail::ComputedStyle::FlexDirection::Row ||
                     cs.flex_direction ==
                         detail::ComputedStyle::FlexDirection::RowReverse);
                if (row_flex && cs.flex_wrap == detail::ComputedStyle::FlexWrap::NoWrap) {
                    for (const int child : kids) {
                        children_w += min_content_w[static_cast<std::size_t>(child)];
                    }
                    if (kids.size() > 1 && cs.column_gap > 0) {
                        children_w += static_cast<int>(kids.size() - 1) * cs.column_gap;
                    }
                } else {
                    for (const int child : kids) {
                        children_w = std::max(
                            children_w,
                            min_content_w[static_cast<std::size_t>(child)]);
                    }
                }
                content_w = std::max(content_w, children_w);
            }

            if (content_w > 0) {
                min_content_w[ri] =
                    content_w + cs.padding_left + cs.padding_right +
                    cs.used_border_left() + cs.used_border_right();
            }
        }

        for (std::size_t bi = 0; bi < impl_->blocks.size(); ++bi) {
            const auto& cs = layout_styles[bi];
            if (cs.min_width >= 0 || min_content_w[bi] <= 0 ||
                impl_->blocks[bi].parent_idx < 0) {
                continue;
            }
            const auto parent_idx =
                static_cast<std::size_t>(impl_->blocks[bi].parent_idx);
            const auto& parent_style = layout_styles[parent_idx];
            const bool row_flex_parent =
                is_flex_container_display(parent_style.display) &&
                (parent_style.flex_direction ==
                     detail::ComputedStyle::FlexDirection::Row ||
                 parent_style.flex_direction ==
                     detail::ComputedStyle::FlexDirection::RowReverse);
            if (row_flex_parent) {
                inputs[bi].auto_min_w_px =
                    std::max(inputs[bi].auto_min_w_px, min_content_w[bi]);
            }
        }
    }

    std::vector<Rect> out(impl_->blocks.size());
    std::vector<RectF> out_f(impl_->blocks.size());
    // Yoga's root has no per-block padding of its own. We bake body's
    // padding in by shrinking the viewport handed to Yoga and
    // shifting frames back out below. Cleaner future: a real Box
    // tree where body is its own Yoga node.
    const int inner_w = viewport_width - pad_l - pad_r;
    detail::layout_blocks_with_yoga(
        inner_w, viewport_height, inputs, out, measurer, out_f);

    // Table column alignment. Yoga lays each row out independently, so
    // cells in column N of different rows wouldn't line up. Using the
    // natural cell widths from the pass above, resolve one width per
    // column (the widest cell, scaled to any explicit table width), pin
    // every cell to it via intrinsic_w_px, and re-run layout so columns
    // align. No-op (returns false) when the document has no tables.
    if (assign_table_column_widths(inputs, out)) {
        detail::layout_blocks_with_yoga(
            inner_w, viewport_height, inputs, out, measurer, out_f);
    }

    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        out[i].x += pad_l;
        out[i].y += pad_t;
        out_f[i].x += static_cast<float>(pad_l);
        out_f[i].y += static_cast<float>(pad_t);
        impl_->blocks[i].bounds = out[i];
        impl_->blocks[i].bounds_f = out_f[i];
    }
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        const auto& cs = layout_styles[i];
        if (cs.css_float == detail::ComputedStyle::Float::None ||
            impl_->blocks[i].parent_idx < 0) {
            continue;
        }

        const auto& parent =
            impl_->blocks[static_cast<std::size_t>(impl_->blocks[i].parent_idx)];
        const auto& parent_style =
            layout_styles[static_cast<std::size_t>(impl_->blocks[i].parent_idx)];
        auto& r = out[i];
        if (cs.width > 0) {
            r.w = cs.box_sizing == detail::ComputedStyle::BoxSizing::BorderBox
                ? cs.width
                : cs.width + cs.padding_left + cs.padding_right +
                    cs.used_border_left() + cs.used_border_right();
        }
        if (cs.height > 0) {
            r.h = cs.box_sizing == detail::ComputedStyle::BoxSizing::BorderBox
                ? cs.height
                : cs.height + cs.padding_top + cs.padding_bottom +
                    cs.used_border_top() + cs.used_border_bottom();
        }
        const int parent_left = parent.bounds.x + parent_style.used_border_left() +
                                parent_style.padding_left;
        const int parent_right = parent.bounds.x + parent.bounds.w -
                                 parent_style.used_border_right() -
                                 parent_style.padding_right;
        if (cs.css_float == detail::ComputedStyle::Float::Right) {
            r.x = parent_right - r.w - cs.margin_right;
        } else {
            r.x = parent_left + cs.margin_left;
        }
        r.y = parent.bounds.y + parent_style.used_border_top() +
              parent_style.padding_top + cs.margin_top;
        impl_->blocks[i].bounds = r;
        impl_->blocks[i].bounds_f = to_float(r);
    }

    auto block_is_fixed_position = [&](std::size_t i) {
        return layout_styles[i].position ==
               detail::ComputedStyle::Position::Fixed;
    };
    auto block_is_in_fixed_subtree = [&](std::size_t i) {
        int idx = static_cast<int>(i);
        while (idx >= 0) {
            if (block_is_fixed_position(static_cast<std::size_t>(idx))) {
                return true;
            }
            idx = impl_->blocks[static_cast<std::size_t>(idx)].parent_idx;
        }
        return false;
    };

    int max_bottom = 0;
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        if (block_is_in_fixed_subtree(i)) continue;
        const auto& b = impl_->blocks[i];
        const int bottom = b.bounds.y + b.bounds.h;
        if (bottom > max_bottom) max_bottom = bottom;
    }
    // content_size = max(natural body height, viewport floor). The
    // floor ensures body's background fills the visible window even
    // when natural content is shorter.
    const int natural_h = max_bottom + pad_b;
    impl_->content_size = Size{viewport_width,
                               std::max(natural_h, viewport_height)};

    // Compute per-block content_h = max(descendant bottom edge) - own top.
    // Used by the scroll path: how far the user can scroll before the
    // last descendant clears the visible region. Iterate children in
    // doc order; each parent gets the max bottom of all its
    // descendants (transitive: child's content already reflects its
    // own descendants).
    for (auto& b : impl_->blocks) b.content_h = b.bounds.h;
    for (std::size_t i = impl_->blocks.size(); i-- > 0; ) {
        if (block_is_fixed_position(i)) continue;
        const auto& child = impl_->blocks[i];
        if (child.parent_idx < 0) continue;
        auto& parent = impl_->blocks[static_cast<std::size_t>(child.parent_idx)];
        const int child_bottom_in_parent =
            (child.bounds.y - parent.bounds.y) + child.content_h;
        if (child_bottom_in_parent > parent.content_h)
            parent.content_h = child_bottom_in_parent;
    }
    using Overflow = detail::ComputedStyle::Overflow;
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        auto& block = impl_->blocks[i];
        const auto ov = layout_styles[i].overflow_y;
        if (ov == Overflow::Scroll || ov == Overflow::Auto) {
            const int max_scroll = std::max(0, block.content_h - block.bounds.h);
            block.scroll_y = std::clamp(block.scroll_y, 0, max_scroll);
        } else {
            block.scroll_y = 0;
        }
    }

#if !defined(AFFINEUI_STUB_BUILD)
    if (measurer != nullptr) {
        for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
            auto& b = impl_->blocks[i];
            if (!b.text_control || b.placeholder_visible) continue;
            const auto g = text_control_geometry(
                *impl_, static_cast<int>(i), *measurer);
            ensure_text_layout_entry(
                *impl_, static_cast<int>(i), g, b, *measurer);
        }
    }
#endif

#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->pending_dirty_roots.empty()) {
        for (const int root_idx : impl_->pending_dirty_roots) {
            if (root_idx >= 0 &&
                root_idx < static_cast<int>(impl_->blocks.size())) {
                add_dirty_rect(*impl_, subtree_visual_rect(*impl_, root_idx));
            }
        }
        impl_->pending_dirty_roots.clear();
    }
#endif
}

// Forward decls â€” definitions live in the anonymous namespace below,
// alongside the dispatch helpers. Used by Document::draw to compute
// scroll offsets + clip rects for the paint walk.
namespace {
#if !defined(AFFINEUI_STUB_BUILD)
bool block_is_scrollable_y(const detail::DocumentImpl& impl, int idx);
bool block_clips_overflow(const detail::DocumentImpl& impl, int idx);
int  nearest_clip_ancestor_for_block(const detail::DocumentImpl& impl,
                                     int idx);
int  scroll_offset_y_for(const std::vector<Block>& blocks,
                         const detail::StyleStore& styles, int idx);
int  effective_z_index(const detail::DocumentImpl& impl, int idx);
struct ScrollbarGeometry {
    Rect track{};
    Rect thumb{};
    int scroll_range{0};
    int thumb_travel{0};
};
bool vertical_scrollbar_geometry(const detail::DocumentImpl& impl,
                                 int idx,
                                 ScrollbarGeometry& out);
const KeyframeBlock* find_keyframes(const detail::DocumentImpl& impl,
                                    std::uint32_t name_hash);
bool has_text_selection(const Block& block);
std::pair<std::size_t, std::size_t> normalized_selection(const Block& block);
TextLayoutEntry& ensure_text_layout_entry(detail::DocumentImpl& impl,
                                          int idx,
                                          const TextControlGeometry& g,
                                          const Block& block,
                                          Painter& painter);
float aligned_line_origin_x(const TextControlGeometry& g,
                            const TextLayoutEntry& entry,
                            std::uint16_t line);
float aligned_line_origin_x(const TextLayoutEntry& entry,
                            std::uint16_t line);
#endif
}  // namespace

#if !defined(AFFINEUI_STUB_BUILD)
float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

float solve_cubic_bezier(float x1, float y1, float x2, float y2, float x) {
    auto sample = [](float a1, float a2, float t) {
        const float inv = 1.0f - t;
        return 3.0f * inv * inv * t * a1 +
               3.0f * inv * t * t * a2 +
               t * t * t;
    };
    auto derivative = [](float a1, float a2, float t) {
        const float inv = 1.0f - t;
        return 3.0f * inv * inv * a1 +
               6.0f * inv * t * (a2 - a1) +
               3.0f * t * t * (1.0f - a2);
    };

    float t = clamp01(x);
    for (int i = 0; i < 8; ++i) {
        const float dx = sample(x1, x2, t) - x;
        const float d = derivative(x1, x2, t);
        if (std::fabs(d) < 1e-5f) break;
        t = clamp01(t - dx / d);
    }
    return sample(y1, y2, t);
}

float ease_animation_progress(detail::ResolvedStyle::CssAnimation::Timing timing,
                              float t) {
    t = clamp01(t);
    switch (timing) {
        case detail::ResolvedStyle::CssAnimation::Timing::Linear:
            return t;
        case detail::ResolvedStyle::CssAnimation::Timing::EaseIn:
            return solve_cubic_bezier(0.42f, 0.0f, 1.0f, 1.0f, t);
        case detail::ResolvedStyle::CssAnimation::Timing::EaseOut:
            return solve_cubic_bezier(0.0f, 0.0f, 0.58f, 1.0f, t);
        case detail::ResolvedStyle::CssAnimation::Timing::EaseInOut:
            return solve_cubic_bezier(0.42f, 0.0f, 0.58f, 1.0f, t);
        case detail::ResolvedStyle::CssAnimation::Timing::StepStart:
            return t > 0.0f ? 1.0f : 0.0f;
        case detail::ResolvedStyle::CssAnimation::Timing::StepEnd:
            return t >= 1.0f ? 1.0f : 0.0f;
        case detail::ResolvedStyle::CssAnimation::Timing::Ease:
        default:
            return solve_cubic_bezier(0.25f, 0.1f, 0.25f, 1.0f, t);
    }
}

bool animation_progress_at(const detail::ResolvedStyle::CssAnimation& anim,
                           double elapsed_s,
                           float& out_t,
                           bool& out_applies) {
    out_t = 0.0f;
    out_applies = false;
    if (!anim.active || anim.duration_s <= 0.0f || anim.name_hash == 0) {
        return false;
    }

    const bool paused =
        anim.play_state == detail::ResolvedStyle::CssAnimation::PlayState::Paused;
    const double timeline_s = paused ? 0.0 : elapsed_s;
    const double active_s = timeline_s - static_cast<double>(anim.delay_s);
    const bool fills_back =
        anim.fill_mode == detail::ResolvedStyle::CssAnimation::FillMode::Backwards ||
        anim.fill_mode == detail::ResolvedStyle::CssAnimation::FillMode::Both;
    const bool fills_forward =
        anim.fill_mode == detail::ResolvedStyle::CssAnimation::FillMode::Forwards ||
        anim.fill_mode == detail::ResolvedStyle::CssAnimation::FillMode::Both;
    auto directed = [&](float local, long long iteration) {
        using Direction = detail::ResolvedStyle::CssAnimation::Direction;
        const bool odd = (iteration & 1ll) != 0;
        switch (anim.direction) {
            case Direction::Reverse:
                return 1.0f - local;
            case Direction::Alternate:
                return odd ? 1.0f - local : local;
            case Direction::AlternateReverse:
                return odd ? local : 1.0f - local;
            case Direction::Normal:
            default:
                return local;
        }
    };
    if (active_s < 0.0) {
        if (fills_back) {
            out_applies = true;
            out_t = ease_animation_progress(anim.timing, directed(0.0f, 0));
        }
        return !paused;
    }

    const bool infinite = anim.iteration_count == 0.0f;
    const double total_s = static_cast<double>(anim.duration_s) *
                           static_cast<double>(anim.iteration_count);
    if (!infinite && active_s >= total_s) {
        if (fills_forward) {
            const double count = std::max(0.0, static_cast<double>(anim.iteration_count));
            const double whole = std::floor(count);
            const double frac = count - whole;
            const bool fractional = frac > 1e-6;
            const long long final_iteration = fractional
                ? static_cast<long long>(whole)
                : std::max<long long>(0, static_cast<long long>(whole) - 1);
            const float local = fractional ? static_cast<float>(frac) : 1.0f;
            out_applies = true;
            out_t = ease_animation_progress(anim.timing,
                                            directed(local, final_iteration));
        }
        return false;
    }

    const double duration = static_cast<double>(anim.duration_s);
    double iteration = std::floor(active_s / duration);
    float local = static_cast<float>((active_s - iteration * duration) / duration);
    out_applies = true;
    out_t = ease_animation_progress(
        anim.timing, directed(local, static_cast<long long>(iteration)));
    return !paused;
}

detail::AnimatedStyle animated_at_keyframe(
    detail::DocumentImpl& impl,
    const Block& block,
    const KeyframeStep* step) {
    if (!step || !step->decls || !impl.resolver) return block.base_animated;
    detail::ResolvedStyle rs;
    rs.computed = impl.style_store.computed(block.id);
    rs.animated = block.base_animated;
    impl.resolver->apply_decl_list(step->decls, rs);
    return rs.animated;
}

float lerp_float(float a, float b, float t) {
    return a + (b - a) * t;
}

detail::AnimatedStyle interpolate_animated(detail::AnimatedStyle a,
                                           const detail::AnimatedStyle& b,
                                           float t) {
    a.opacity  = lerp_float(a.opacity,  b.opacity,  t);
    a.tx       = lerp_float(a.tx,       b.tx,       t);
    a.ty       = lerp_float(a.ty,       b.ty,       t);
    a.tx_pct   = lerp_float(a.tx_pct,   b.tx_pct,   t);
    a.ty_pct   = lerp_float(a.ty_pct,   b.ty_pct,   t);
    a.scale_x  = lerp_float(a.scale_x,  b.scale_x,  t);
    a.scale_y  = lerp_float(a.scale_y,  b.scale_y,  t);
    a.rotation = lerp_float(a.rotation, b.rotation, t);
    return a;
}

detail::AnimatedStyle sample_keyframes(detail::DocumentImpl& impl,
                                       const Block& block,
                                       float t) {
    const auto* kf = find_keyframes(impl, block.animation.name_hash);
    if (!kf || kf->steps.empty()) return block.base_animated;

    const KeyframeStep* left = nullptr;
    const KeyframeStep* right = nullptr;
    for (const auto& step : kf->steps) {
        if (step.offset <= t + 1e-6f) left = &step;
        if (right == nullptr && step.offset >= t - 1e-6f) right = &step;
    }

    const float left_offset = left ? left->offset : 0.0f;
    const float right_offset = right ? right->offset : 1.0f;
    const auto left_style = animated_at_keyframe(impl, block, left);
    if (!right || std::abs(right_offset - left_offset) < 1e-6f) {
        return left_style;
    }
    const auto right_style = animated_at_keyframe(impl, block, right);
    const float local = clamp01((t - left_offset) / (right_offset - left_offset));
    return interpolate_animated(left_style, right_style, local);
}

Mat2x3 local_transform_for(const detail::AnimatedStyle& an, const RectF& r) {
    if (an.tx == 0.0f && an.ty == 0.0f &&
        an.tx_pct == 0.0f && an.ty_pct == 0.0f &&
        an.scale_x == 1.0f && an.scale_y == 1.0f &&
        an.rotation == 0.0f) {
        return Mat2x3::identity();
    }
    const float ox = r.x + an.origin_x + r.w * an.origin_x_pct * 0.01f;
    const float oy = r.y + an.origin_y + r.h * an.origin_y_pct * 0.01f;
    const float tx = an.tx + r.w * an.tx_pct * 0.01f;
    const float ty = an.ty + r.h * an.ty_pct * 0.01f;
    return Mat2x3::translate(-ox, -oy)
        .then(Mat2x3::scale(an.scale_x, an.scale_y))
        .then(Mat2x3::rotate(an.rotation))
        .then(Mat2x3::translate(tx, ty))
        .then(Mat2x3::translate(ox, oy));
}

Mat2x3 effective_transform_for(const detail::DocumentImpl& impl, int idx) {
    std::vector<int> chain;
    for (int cur = idx; cur >= 0; ) {
        chain.push_back(cur);
        cur = impl.blocks[static_cast<std::size_t>(cur)].parent_idx;
    }
    Mat2x3 combined = Mat2x3::identity();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto& b = impl.blocks[static_cast<std::size_t>(*it)];
        const int dy = scroll_offset_y_for(impl.blocks, impl.style_store, *it);
        const RectF eff{
            b.bounds_f.x,
            b.bounds_f.y - static_cast<float>(dy),
            b.bounds_f.w,
            b.bounds_f.h,
        };
        combined = local_transform_for(
            impl.style_store.animated(b.id), eff).then(combined);
    }
    return combined;
}
#endif

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
    ensure_font_faces_registered(*impl_, painter);

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
            const int za = effective_z_index(*impl_, a);
            const int zb = effective_z_index(*impl_, b);
            if (za != zb) return za < zb;
            return a < b;
        });
#endif

    for (int paint_idx : paint_order) {
        const std::size_t i = static_cast<std::size_t>(paint_idx);
        const auto& b  = impl_->blocks[i];
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

        const int dy = scroll_offset_y_for(impl_->blocks, impl_->style_store,
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

        // Find the nearest ancestor whose overflow clips children
        // (overflow: hidden | clip | scroll | auto). CSS clips descendant
        // paint to the padding box, not the border box, so the ancestor's
        // own border remains visible above clipped children.
        const int clip_idx =
            nearest_clip_ancestor_for_block(*impl_, static_cast<int>(i));
        const bool clipped = (clip_idx >= 0);
        Rect active_clip_rect{};
        if (clipped) {
            const auto& cb = impl_->blocks[static_cast<std::size_t>(clip_idx)];
            const auto& ccs = impl_->style_store.computed(cb.id);
            const int clip_dy = scroll_offset_y_for(
                impl_->blocks, impl_->style_store, clip_idx);
            active_clip_rect = Rect{
                cb.bounds.x + ccs.used_border_left(),
                cb.bounds.y - clip_dy + ccs.used_border_top(),
                std::max(0, cb.bounds.w - ccs.used_border_left() - ccs.used_border_right()),
                std::max(0, cb.bounds.h - ccs.used_border_top() - ccs.used_border_bottom()),
            };
            painter.push_clip(active_clip_rect);
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
        if (clipped) {
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
        const bool bg_any_radius =
            (bg_r_tl > 0 || bg_r_tr > 0 || bg_r_br > 0 || bg_r_bl > 0);
        const bool bg_uniform_r =
            (bg_r_tl == bg_r_tr && bg_r_tr == bg_r_br && bg_r_br == bg_r_bl);
        // Background color paints first; background images/gradients layer over it.
        const bool has_gradient =
            (an.gradient_kind != detail::AnimatedStyle::GradientKind::None);
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
        const int used_border_top    = cs.used_border_top();
        const int used_border_right  = cs.used_border_right();
        const int used_border_bottom = cs.used_border_bottom();
        const int used_border_left   = cs.used_border_left();

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

        if (has_bg) {
            const Color bg = detail::unpack_rgba(an.background_rgba);
            if      (!bg_any_radius)          painter.fill_rect(eff, bg);
            else if (bg_uniform_r)            painter.fill_rounded_rect(eff, bg_r_tl, bg);
            else                              painter.fill_rounded_rect_varying(
                                                  eff, bg_r_tl, bg_r_tr,
                                                  bg_r_br, bg_r_bl, bg);
        }

        if (has_gradient) {
            const Color s0 = detail::unpack_rgba(an.gradient_stop0_rgba);
            const Color s1 = detail::unpack_rgba(an.gradient_stop1_rgba);
            if (an.gradient_kind == detail::AnimatedStyle::GradientKind::Linear) {
                painter.fill_linear_gradient_rect(
                    eff, static_cast<float>(an.gradient_angle_deg),
                    s0, s1, bg_r_tl, bg_r_tr, bg_r_br, bg_r_bl);
            } else if (an.gradient_kind == detail::AnimatedStyle::GradientKind::Radial) {
                painter.fill_radial_gradient_rect(
                    eff, s0, s1, bg_r_tl, bg_r_tr, bg_r_br, bg_r_bl,
                    static_cast<float>(an.gradient_center_x_pct),
                    static_cast<float>(an.gradient_center_y_pct),
                    static_cast<float>(an.gradient_stop1_pos_pct));
            } else if (an.gradient_kind == detail::AnimatedStyle::GradientKind::LinearStripes) {
                painter.fill_linear_stripes_rect(
                    eff, static_cast<float>(an.gradient_angle_deg),
                    s0, static_cast<float>(std::max(1, eff.h)),
                    bg_r_tl, bg_r_tr, bg_r_br, bg_r_bl);
            }
        }

        if (has_grid) {
            painter.fill_grid_rect(
                eff, detail::unpack_rgba(an.background_grid_rgba),
                static_cast<float>(an.background_grid_size_px),
                static_cast<float>(std::max<std::uint8_t>(
                    1, an.background_grid_line_px)),
                bg_r_tl, bg_r_tr, bg_r_br, bg_r_bl);
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
            paint_direct_child_svgs(b, eff, cs, an, painter, elem);
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
                static_cast<float>(cs.font_size_px) *
                detail::effective_line_height_mult(cs);
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

        if (!b.text.empty()) {
            const auto font = painter.resolve_font(
                impl_->style_store.font_family_of(cs.font_id), cs.font_size_px, cs.font_weight, cs.font_style != 0);
            const int textarea_idx_for_text = nearest_block_with_tag(
                impl_->blocks, static_cast<int>(i), "textarea");
            int text_y = eff.y + used_border_top  + cs.padding_top;
            const bool single_line_text =
                b.text.find('\n') == std::string::npos &&
                b.text.find('\r') == std::string::npos;
            if (single_line_text && textarea_idx_for_text < 0 &&
                (cs.display == detail::ComputedStyle::Display::Flex ||
                 cs.display == detail::ComputedStyle::Display::InlineFlex)) {
                const float content_h = static_cast<float>(
                    eff.h - used_border_top - used_border_bottom
                          - cs.padding_top - cs.padding_bottom);
                const float css_line_h =
                    static_cast<float>(cs.font_size_px) *
                    detail::effective_line_height_mult(cs);
                const float free_h = content_h - css_line_h;
                if (free_h > 0.0f) {
                    using AI = detail::ComputedStyle::AlignItems;
                    if (cs.align_items == AI::Center) {
                        text_y += static_cast<int>(free_h * 0.5f);
                    } else if (cs.align_items == AI::End) {
                        text_y += static_cast<int>(std::lround(free_h));
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
                b.tag == "select" && !block_has_class(b, "form-select");
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
                        const int anc_dy = scroll_offset_y_for(
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

            if (b.text_control) {
                const auto g = text_control_geometry(
                    *impl_, static_cast<int>(i), painter);
                text_x = g.text_x;
                text_y = g.text_y;
                content_w = g.content_w;
                paint_align = g.align;
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
                cached_text_layout = &ensure_text_layout_entry(
                    *impl_, static_cast<int>(i), g, b, painter);
            }
            if (cached_text_layout != nullptr && has_text_selection(b)) {
                const auto& text_layout = *cached_text_layout;
                const auto [sel_begin, sel_end] = normalized_selection(b);
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
                        aligned_line_origin_x(
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
            painter.draw_text_box(font, Point{text_x, text_y}, b.text,
                                  detail::unpack_rgba(an.color_rgba),
                                  draw_max_w,
                                  line_height_mult,
                                  letter_spacing_px,
                                  paint_align);
            if (b.text_control && static_cast<int>(i) == impl_->focused_idx) {
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
                    caret_layout = &ensure_text_layout_entry(
                        *impl_, static_cast<int>(i), g, b, painter);
                }

                const auto caret_offset =
                    std::min(b.caret_offset, b.text_value.size());
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
                    aligned_line_origin_x(*caret_layout, line) +
                    caret_layout->caret_x[caret_index];
                const float natural_line_h =
                    std::max(1.0f, caret_layout->natural_line_height);
                const float css_line_h =
                    std::max(1.0f, caret_layout->css_line_height);
                const float line_top =
                    static_cast<float>(text_y) +
                    static_cast<float>(line) * css_line_h +
                    (css_line_h - natural_line_h) * 0.5f;
                const float y0 = std::floor(line_top + 2.0f) + 0.5f;
                const float y1 =
                    std::ceil(line_top + natural_line_h - 2.0f) + 0.5f;
                painter.stroke_line(caret_x, y0, caret_x, y1,
                                    detail::unpack_rgba(an.color_rgba),
                                    1.0f);
            }
            if (cs.text_decoration_line != detail::ComputedStyle::DecorationNone) {
                const auto metrics = painter.text_metrics(font);
                const float natural_line_h =
                    metrics.line_height > 0.0f
                        ? metrics.line_height
                        : static_cast<float>(cs.font_size_px);
                const float css_line_h =
                    static_cast<float>(cs.font_size_px) *
                    detail::effective_line_height_mult(cs);
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
        }

        // Closed single-row <select> controls expose an indicator supplied by
        // either the native widget or a CSS background. We don't rasterize
        // Bootstrap's data-URI SVG yet, so draw the same chevron geometry here.
        if (b.tag == "select") {
            const auto* size_attr = block_attr_value(b, "size");
            const bool listbox =
                block_attr_value(b, "multiple") != nullptr ||
                (size_attr != nullptr && !size_attr->empty() && *size_attr != "1");
            if (!listbox) {
                const bool bootstrap_form_select =
                    block_has_class(b, "form-select");
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

        if (block_has_class(b, "dcs-check__box") && b.parent_idx >= 0 &&
            static_cast<std::size_t>(b.parent_idx) < impl_->blocks.size()) {
            const auto& parent =
                impl_->blocks[static_cast<std::size_t>(b.parent_idx)];
            const bool decius_check = block_has_class(parent, "dcs-check");
            const bool decius_radio = block_has_class(parent, "dcs-radio");
            const auto* checked_attr = block_attr_value(parent, "aria-checked");
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
            const auto* value = block_attr_value(b, "value");
            if (value && parse_hex_color(*value, rgba)) {
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
            const double min_attr = block_attr_double(b, "min", 0.0);
            double max_attr = block_attr_double(b, "max", 100.0);
            if (max_attr <= min_attr) max_attr = min_attr + 1.0;
            const double value_attr =
                std::clamp(block_attr_double(b, "value", min_attr),
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
            const float track_h = block_has_class(b, "form-range") ? 8.0f : 6.0f;
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
                block_has_class(b, "form-range")
                    ? Color{0xDE, 0xE2, 0xE6, 0xFF}
                    : Color{0xB8, 0xC0, 0xCC, 0xFF};
            const Color fill_color =
                block_has_class(b, "form-range")
                    ? Color{0x0D, 0x6E, 0xFD, 0xFF}
                    : detail::unpack_rgba(an.color_rgba);
            const Color thumb_color =
                block_has_class(b, "form-range")
                    ? Color{0x0D, 0x6E, 0xFD, 0xFF}
                    : detail::unpack_rgba(an.color_rgba);

            painter.fill_rounded_rect(track, track_h * 0.5f, track_color);
            painter.fill_rounded_rect(fill, track_h * 0.5f, fill_color);
            painter.fill_circle(thumb_x, cy, thumb_r, thumb_color);
        }

        if (has_opacity) painter.pop_alpha();
        if (clipped) painter.pop_clip();
        if (has_transform) painter.pop_transform();
    }

    // Scrollbar overlay â€” drawn last so it sits on top of any
    // clipped content. A simple right-side thumb showing how far
    // we've scrolled; track is transparent.
    for (const auto& b : impl_->blocks) {
        ScrollbarGeometry scrollbar{};
        if (!vertical_scrollbar_geometry(
                *impl_, static_cast<int>(&b - impl_->blocks.data()),
                scrollbar)) {
            continue;
        }
        // Catppuccin overlay0-ish, semi-transparent.
        painter.fill_rounded_rect(
            scrollbar.thumb, 3.0f, Color{0x9c, 0xa0, 0xb0, 0xC0});
    }
}

namespace {

// True iff (x, y) is inside `r` (half-open: right/bottom edges are
// excluded so adjacent rects don't both match).
inline bool rect_contains(const Rect& r, int x, int y) noexcept {
    return x >= r.x && x < r.x + r.w
        && y >= r.y && y < r.y + r.h;
}

// Sum of scroll offsets contributed by scrollable ancestors of
// `idx`. Used by hit-test and paint to convert document-space block
// bounds into the effective on-screen position.
int scroll_offset_y_for(const std::vector<Block>& blocks,
#if !defined(AFFINEUI_STUB_BUILD)
                        const detail::StyleStore& styles,
#endif
                        int idx) {
    int sum = 0;
#if !defined(AFFINEUI_STUB_BUILD)
    using O = detail::ComputedStyle::Overflow;
    using P = detail::ComputedStyle::Position;
    if (idx >= 0 && static_cast<std::size_t>(idx) < blocks.size() &&
        styles.computed(blocks[static_cast<std::size_t>(idx)].id).position ==
            P::Fixed) {
        return 0;
    }
    int p = (idx >= 0) ? blocks[static_cast<std::size_t>(idx)].parent_idx : -1;
    while (p >= 0) {
        const auto& pb = blocks[static_cast<std::size_t>(p)];
        const auto& pcs = styles.computed(pb.id);
        const auto ov = pcs.overflow_y;
        if ((ov == O::Scroll || ov == O::Auto) && pb.scroll_y != 0) {
            sum += pb.scroll_y;
        }
        if (pcs.position == P::Fixed) break;
        p = pb.parent_idx;
    }
#else
    (void)blocks; (void)idx;
#endif
    return sum;
}

#if !defined(AFFINEUI_STUB_BUILD)
int effective_z_index(const detail::DocumentImpl& impl, int idx) {
    int z = 0;
    for (int cur = idx;
         cur >= 0 && cur < static_cast<int>(impl.blocks.size());
         cur = impl.blocks[static_cast<std::size_t>(cur)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(cur)];
        z = std::max<int>(z, impl.style_store.computed(block.id).z_index_low);
    }
    return z;
}

const KeyframeBlock* find_keyframes(const detail::DocumentImpl& impl,
                                    std::uint32_t name_hash) {
    if (name_hash == 0) return nullptr;
    auto it = std::find_if(impl.keyframes.begin(), impl.keyframes.end(),
        [name_hash](const KeyframeBlock& kf) {
            return kf.name_hash == name_hash;
        });
    return it == impl.keyframes.end() ? nullptr : &*it;
}
#endif

#if !defined(AFFINEUI_STUB_BUILD)
bool invert_transform(const Mat2x3& m, Mat2x3& out) {
    const float det = m.a * m.d - m.b * m.c;
    if (std::abs(det) < 1e-6f) return false;
    const float inv_det = 1.0f / det;
    out.a =  m.d * inv_det;
    out.b = -m.b * inv_det;
    out.c = -m.c * inv_det;
    out.d =  m.a * inv_det;
    out.tx = -(out.a * m.tx + out.c * m.ty);
    out.ty = -(out.b * m.tx + out.d * m.ty);
    return true;
}

bool rect_contains_float(const Rect& r, float x, float y) noexcept {
    return x >= static_cast<float>(r.x) &&
           x <  static_cast<float>(r.x + r.w) &&
           y >= static_cast<float>(r.y) &&
           y <  static_cast<float>(r.y + r.h);
}
#endif

// Deepest block whose effective border-box (after applying ancestor scroll
// offsets and CSS transforms) contains (x, y), or -1 if none. z-index buckets
// win first, then normal document order breaks ties.
int hit_test_blocks(const detail::DocumentImpl& impl, int x, int y) {
    const auto& blocks = impl.blocks;
    int hit = -1;
    int hit_z = std::numeric_limits<int>::min();
    for (std::size_t i = 0; i < blocks.size(); ++i) {
#if !defined(AFFINEUI_STUB_BUILD)
        const int dy = scroll_offset_y_for(
            blocks, impl.style_store, static_cast<int>(i));
#else
        const int dy = 0;
#endif
        Rect eff = blocks[i].bounds;
        eff.y -= dy;
#if !defined(AFFINEUI_STUB_BUILD)
        const Mat2x3 transform =
            effective_transform_for(impl, static_cast<int>(i));
        bool contains = false;
        if (!transform.is_identity()) {
            Mat2x3 inverse{};
            if (!invert_transform(transform, inverse)) continue;
            const Vec2 local = inverse.apply(
                Vec2{static_cast<float>(x), static_cast<float>(y)});
            contains = rect_contains_float(eff, local.x, local.y);
        } else {
            contains = rect_contains(eff, x, y);
        }
        if (contains) {
            const int z = effective_z_index(impl, static_cast<int>(i));
            if (z > hit_z || (z == hit_z && static_cast<int>(i) > hit)) {
                hit = static_cast<int>(i);
                hit_z = z;
            }
        }
#else
        if (rect_contains(eff, x, y)) {
            hit = static_cast<int>(i);
        }
#endif
    }
    return hit;
}

}  // namespace

namespace {
#if !defined(AFFINEUI_STUB_BUILD)
// Build the ancestor chain (deepest â†’ root) for the block at `idx`.
// Walks parent_idx, which collect_blocks set up. Empty when idx == -1.
std::vector<int> build_hover_chain(const std::vector<Block>& blocks, int idx) {
    std::vector<int> chain;
    while (idx >= 0) {
        chain.push_back(idx);
        idx = blocks[static_cast<std::size_t>(idx)].parent_idx;
    }
    return chain;
}

// Look up the resolved style of `block_idx`'s parent. Returns the
// document's root style when there is no parent (top-level body).
detail::ResolvedStyle parent_resolved(const detail::DocumentImpl& impl,
                                      int block_idx) {
    const int p = impl.blocks[static_cast<std::size_t>(block_idx)].parent_idx;
    if (p < 0) return impl.root_style;
    const auto pid = impl.blocks[static_cast<std::size_t>(p)].id;
    detail::ResolvedStyle rs;
    rs.computed = impl.style_store.computed(pid);
    rs.animated = impl.style_store.animated(pid);
    rs.custom_props = impl.blocks[static_cast<std::size_t>(p)].custom_props;
    return rs;
}

// Apply currently-active pseudo-class overlays (per the block's
// state_bits) on top of `rs`. Shared by restyle_block (dispatch
// path) and the equivalent collect-time path inline above.
// True if `block` is in an explicit selected/checked state (aria-selected /
// aria-checked = "true"). Such a state is styled by an attribute rule that, in
// decius, has specificity equal to (and source order after) the matching
// `:hover` rule — so it wins the cascade. The pseudo overlay (which can't see
// per-property cascade provenance) approximates that by not letting :hover
// repaint a selected/checked element. (A full specificity-aware overlay is a
// deeper renderer change; this fixes the common selection-vs-hover case.)
bool block_is_selected_state(const Block& block) {
    for (const auto& a : block.attrs) {
        if ((a.first == "aria-selected" || a.first == "aria-checked") &&
            a.second == "true")
            return true;
    }
    return false;
}

void apply_pseudo_overlay(detail::DocumentImpl& impl, const Block& block,
                          detail::ResolvedStyle& rs) {
    const bool selected = block_is_selected_state(block);
    for (const auto& pr : impl.pseudo_rules) {
        const std::uint8_t bit = pseudo_state_bit(pr.pseudo);
        if (bit == 0) continue;
        // Don't let :hover repaint an explicitly selected/checked element.
        if (pr.pseudo == PseudoRule::Pseudo::Hover && selected) continue;
        if (!compound_matches(pr.target, block.tag, block.elem_id,
                              block.classes, &block.attrs)) continue;
        if (!ancestor_chain_matches(pr.ancestors, block.parent_idx,
                                    impl.blocks)) continue;
        const bool state_matches = pr.state_on_target
            ? block_has_state(impl, block, pr.state_target, bit)
            : ancestor_has_state(impl, block.parent_idx, pr.state_target, bit);
        if (!state_matches) continue;
        impl.resolver->apply_decl_list(pr.decls, rs);
    }
}

bool same_animation(const detail::ResolvedStyle::CssAnimation& a,
                    const detail::ResolvedStyle::CssAnimation& b) {
    return a.name_hash == b.name_hash
        && a.duration_s == b.duration_s
        && a.delay_s == b.delay_s
        && a.iteration_count == b.iteration_count
        && a.timing == b.timing
        && a.direction == b.direction
        && a.fill_mode == b.fill_mode
        && a.play_state == b.play_state
        && a.active == b.active;
}

bool animation_candidate(const detail::ResolvedStyle::CssAnimation& a) {
    return a.active && a.name_hash != 0;
}

bool computed_change_needs_layout(const detail::ComputedStyle& a,
                                  const detail::ComputedStyle& b) {
    return a.margin_top != b.margin_top ||
        a.margin_right != b.margin_right ||
        a.margin_bottom != b.margin_bottom ||
        a.margin_left != b.margin_left ||
        a.padding_top != b.padding_top ||
        a.padding_right != b.padding_right ||
        a.padding_bottom != b.padding_bottom ||
        a.padding_left != b.padding_left ||
        a.border_top != b.border_top ||
        a.border_right != b.border_right ||
        a.border_bottom != b.border_bottom ||
        a.border_left != b.border_left ||
        a.border_style != b.border_style ||
        a.border_style_sides != b.border_style_sides ||
        a.border_collapse != b.border_collapse ||
        a.height_pct != b.height_pct ||
        a.width != b.width ||
        a.height != b.height ||
        a.min_width != b.min_width ||
        a.max_width != b.max_width ||
        a.min_height != b.min_height ||
        a.max_height != b.max_height ||
        a.inset_top != b.inset_top ||
        a.inset_right != b.inset_right ||
        a.inset_bottom != b.inset_bottom ||
        a.inset_left != b.inset_left ||
        a.font_size_px != b.font_size_px ||
        a.font_weight != b.font_weight ||
        a.line_height_x100 != b.line_height_x100 ||
        a.letter_spacing_x100 != b.letter_spacing_x100 ||
        a.text_indent_value != b.text_indent_value ||
        a.white_space != b.white_space ||
        a.text_transform != b.text_transform ||
        a.text_align != b.text_align ||
        a.list_style_type != b.list_style_type ||
        a.display != b.display ||
        a.position != b.position ||
        a.flex_direction != b.flex_direction ||
        a.font_style != b.font_style ||
        a.vertical_align != b.vertical_align ||
        a.box_sizing != b.box_sizing ||
        a.css_float != b.css_float ||
        a.text_indent_is_pct != b.text_indent_is_pct ||
        a.inset_has.top != b.inset_has.top ||
        a.inset_has.right != b.inset_has.right ||
        a.inset_has.bottom != b.inset_has.bottom ||
        a.inset_has.left != b.inset_has.left ||
        a.inset_has.top_pct != b.inset_has.top_pct ||
        a.inset_has.right_pct != b.inset_has.right_pct ||
        a.inset_has.bottom_pct != b.inset_has.bottom_pct ||
        a.inset_has.left_pct != b.inset_has.left_pct ||
        a.margin_auto.left != b.margin_auto.left ||
        a.margin_auto.right != b.margin_auto.right ||
        a.justify_content != b.justify_content ||
        a.align_items != b.align_items ||
        a.flex_wrap != b.flex_wrap ||
        a.flex_basis_pct != b.flex_basis_pct ||
        a.row_gap != b.row_gap ||
        a.column_gap != b.column_gap ||
        a.flex_grow != b.flex_grow ||
        a.flex_shrink != b.flex_shrink ||
        a.flex_basis != b.flex_basis ||
        a.font_id != b.font_id ||
        a.width_pct_x100 != b.width_pct_x100;
}

detail::ComputedStyle::JustifyContent
inline_justify_for_text_align(detail::ComputedStyle::TextAlign align) {
    using JC = detail::ComputedStyle::JustifyContent;
    switch (align) {
        case detail::ComputedStyle::TextAlign::Center: return JC::Center;
        case detail::ComputedStyle::TextAlign::Right:  return JC::End;
        case detail::ComputedStyle::TextAlign::Left:
        case detail::ComputedStyle::TextAlign::Justify:
        default:                                      return JC::Start;
    }
}

bool restyle_synthetic_block(detail::DocumentImpl& impl, int idx) {
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    auto parent = parent_resolved(impl, idx);
    const auto old_computed = impl.style_store.computed(block.id);

    if (block.synthetic) {
        auto next_computed = old_computed;
        next_computed.font_size_px         = parent.computed.font_size_px;
        next_computed.font_weight          = parent.computed.font_weight;
        next_computed.font_style           = parent.computed.font_style;
        next_computed.line_height_x100     = parent.computed.line_height_x100;
        next_computed.font_id              = parent.computed.font_id;
        next_computed.cursor               = parent.computed.cursor;
        next_computed.text_align           = parent.computed.text_align;
        next_computed.letter_spacing_x100  = parent.computed.letter_spacing_x100;
        next_computed.text_indent_value    = parent.computed.text_indent_value;
        next_computed.text_indent_is_pct   = parent.computed.text_indent_is_pct;
        next_computed.white_space          = parent.computed.white_space;
        next_computed.text_transform       = parent.computed.text_transform;
        next_computed.text_decoration_line =
            parent.computed.text_decoration_line;
        next_computed.justify_content =
            inline_justify_for_text_align(parent.computed.text_align);

        auto next_animated = impl.style_store.animated(block.id);
        next_animated.color_rgba = parent.animated.color_rgba;
        next_animated.text_decoration_rgba =
            parent.animated.text_decoration_rgba;

        impl.style_store.computed(block.id) = next_computed;
        impl.style_store.animated(block.id) = next_animated;
        block.custom_props = parent.custom_props;
        block.base_animated = next_animated;
        return computed_change_needs_layout(old_computed, next_computed);
    }

    if (block.tag == "#text") {
        auto rs = anonymous_text_style(parent);
        impl.style_store.computed(block.id) = rs.computed;
        impl.style_store.animated(block.id) = rs.animated;
        block.custom_props = rs.custom_props;
        block.box_shadows = rs.box_shadows;
        block.base_animated = rs.animated;
        block.animation = rs.animation;
        return computed_change_needs_layout(old_computed, rs.computed);
    }

    return false;
}

bool absolute_geometry_change_can_stay_local(detail::ComputedStyle a,
                                             detail::ComputedStyle b) {
    using Position = detail::ComputedStyle::Position;
    if (a.position != b.position ||
        (b.position != Position::Absolute && b.position != Position::Fixed)) {
        return false;
    }

    a.width = b.width = 0;
    a.height = b.height = 0;
    a.height_pct = b.height_pct = -1;
    a.width_pct_x100 = b.width_pct_x100 = -1;
    a.inset_top = b.inset_top = 0;
    a.inset_right = b.inset_right = 0;
    a.inset_bottom = b.inset_bottom = 0;
    a.inset_left = b.inset_left = 0;
    a.inset_has.top = b.inset_has.top = 0;
    a.inset_has.right = b.inset_has.right = 0;
    a.inset_has.bottom = b.inset_has.bottom = 0;
    a.inset_has.left = b.inset_has.left = 0;
    a.inset_has.top_pct = b.inset_has.top_pct = 0;
    a.inset_has.right_pct = b.inset_has.right_pct = 0;
    a.inset_has.bottom_pct = b.inset_has.bottom_pct = 0;
    a.inset_has.left_pct = b.inset_has.left_pct = 0;

    return !computed_change_needs_layout(a, b);
}

int resolve_position_edge(std::int16_t value, bool is_pct, int basis) {
    if (!is_pct) return static_cast<int>(value);
    return static_cast<int>(
        std::lround(static_cast<double>(basis) *
                    (static_cast<double>(value) / 10000.0)));
}

int resolved_outer_length(std::int16_t px,
                          std::int16_t pct_x100,
                          int basis,
                          int fallback,
                          const detail::ComputedStyle& cs,
                          bool horizontal) {
    int length = fallback;
    if (pct_x100 >= 0) {
        length = resolve_position_edge(pct_x100, true, basis);
    } else if (px >= 0) {
        length = static_cast<int>(px);
    }
    if (cs.box_sizing == detail::ComputedStyle::BoxSizing::ContentBox &&
        (pct_x100 >= 0 || px >= 0)) {
        if (horizontal) {
            length += cs.padding_left + cs.padding_right +
                      cs.used_border_left() + cs.used_border_right();
        } else {
            length += cs.padding_top + cs.padding_bottom +
                      cs.used_border_top() + cs.used_border_bottom();
        }
    }
    return std::max(0, length);
}

void translate_subtree_bounds(std::vector<Block>& blocks,
                              int root_idx,
                              int dx,
                              int dy) {
    if (dx == 0 && dy == 0) return;
    for (int idx = root_idx; idx < static_cast<int>(blocks.size()); ++idx) {
        bool in_subtree = false;
        for (int cur = idx; cur >= 0; ) {
            if (cur == root_idx) {
                in_subtree = true;
                break;
            }
            cur = blocks[static_cast<std::size_t>(cur)].parent_idx;
        }
        if (!in_subtree) continue;
        auto& block = blocks[static_cast<std::size_t>(idx)];
        block.bounds.x += dx;
        block.bounds.y += dy;
        block.bounds_f.x += static_cast<float>(dx);
        block.bounds_f.y += static_cast<float>(dy);
    }
}

bool update_absolute_geometry(detail::DocumentImpl& impl,
                              int idx,
                              const detail::ComputedStyle& cs) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return false;
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (block.parent_idx < 0 ||
        block.parent_idx >= static_cast<int>(impl.blocks.size())) {
        return false;
    }

    const auto& parent = impl.blocks[static_cast<std::size_t>(block.parent_idx)];
    const auto& parent_cs = impl.style_store.computed(parent.id);
    Rect containing{
        parent.bounds.x + parent_cs.used_border_left(),
        parent.bounds.y + parent_cs.used_border_top(),
        std::max(0, parent.bounds.w - parent_cs.used_border_left() -
                         parent_cs.used_border_right()),
        std::max(0, parent.bounds.h - parent_cs.used_border_top() -
                         parent_cs.used_border_bottom())
    };
    if (cs.position == detail::ComputedStyle::Position::Fixed &&
        impl.media_viewport_width_px > 0) {
        const int viewport_h =
            impl.media_viewport_height_px > 0
                ? impl.media_viewport_height_px
                : std::max(impl.content_size.height, parent.bounds.h);
        containing = Rect{0, 0, impl.media_viewport_width_px,
                          std::max(0, viewport_h)};
    }

    const auto resolve_horizontal_inset = [&](std::int16_t value,
                                              bool is_pct) {
        return resolve_position_edge(value, is_pct, containing.w);
    };
    const auto resolve_vertical_inset = [&](std::int16_t value,
                                            bool is_pct) {
        return resolve_position_edge(value, is_pct, containing.h);
    };

    Rect next = block.bounds;
    next.w = resolved_outer_length(cs.width, cs.width_pct_x100, containing.w,
                                   next.w, cs, true);
    next.h = resolved_outer_length(
        cs.height,
        cs.height_pct >= 0 ? static_cast<std::int16_t>(cs.height_pct * 100)
                           : static_cast<std::int16_t>(-1),
        containing.h, next.h, cs, false);

    const bool explicit_width = cs.width >= 0 || cs.width_pct_x100 >= 0;
    if (!explicit_width && cs.inset_has.left && cs.inset_has.right) {
        const int left = resolve_horizontal_inset(
            cs.inset_left, cs.inset_has.left_pct);
        const int right = resolve_horizontal_inset(
            cs.inset_right, cs.inset_has.right_pct);
        next.w = std::max(0, containing.w - left - right);
    }
    const bool explicit_height = cs.height >= 0 || cs.height_pct >= 0;
    if (!explicit_height && cs.inset_has.top && cs.inset_has.bottom) {
        const int top = resolve_vertical_inset(
            cs.inset_top, cs.inset_has.top_pct);
        const int bottom = resolve_vertical_inset(
            cs.inset_bottom, cs.inset_has.bottom_pct);
        next.h = std::max(0, containing.h - top - bottom);
    }

    if (cs.inset_has.left) {
        next.x = containing.x +
                 resolve_horizontal_inset(cs.inset_left,
                                          cs.inset_has.left_pct);
    } else if (cs.inset_has.right) {
        next.x = containing.x + containing.w -
                 resolve_horizontal_inset(cs.inset_right,
                                          cs.inset_has.right_pct) -
                 next.w;
    }

    if (cs.inset_has.top) {
        next.y = containing.y +
                 resolve_vertical_inset(cs.inset_top,
                                        cs.inset_has.top_pct);
    } else if (cs.inset_has.bottom) {
        next.y = containing.y + containing.h -
                 resolve_vertical_inset(cs.inset_bottom,
                                        cs.inset_has.bottom_pct) -
                 next.h;
    }

    const int dx = next.x - block.bounds.x;
    const int dy = next.y - block.bounds.y;
    translate_subtree_bounds(impl.blocks, idx, dx, dy);
    block.bounds.w = next.w;
    block.bounds.h = next.h;
    block.bounds_f.w = static_cast<float>(next.w);
    block.bounds_f.h = static_cast<float>(next.h);
    return true;
}

// Re-resolve one block's style, applying any active pseudo overlays.
// Returns true if the computed layout fields changed and the caller
// must schedule layout. Paint-only changes can stay inside the block's
// visual rect and avoid a layout walk.
bool restyle_block(detail::DocumentImpl& impl, int idx) {
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    auto* elem  = impl.style_store.element_of(block.id);
    if (!elem) return restyle_synthetic_block(impl, idx);
    auto parent = parent_resolved(impl, idx);
    auto rs     = impl.resolver->resolve(elem, parent);
    apply_pseudo_overlay(impl, block, rs);
    // Re-apply font-family fills too: restyle_block runs on the dispatch
    // path (hover/active/focus toggles), and pseudo-scoped fills gate on
    // the matching state bit being set.
    const auto sb_rs = impl.style_store.state_bits(block.id);
    std::array<detail::GridTrackHint,
               detail::kMaxGridTrackHints> grid_columns{};
    std::uint8_t grid_column_count = 0;
    apply_font_family_fills(impl, block.tag, block.elem_id, block.classes,
                            block.parent_idx, sb_rs, rs,
                            &grid_columns, &grid_column_count);
    if (auto value = scan_inline_decl_value(elem, "grid-template-columns");
        !value.empty()) {
        grid_column_count = parse_grid_template_columns(
            std::move(value), grid_columns);
    }
    if (rs.computed.display != detail::ComputedStyle::Display::Grid &&
        rs.computed.display != detail::ComputedStyle::Display::InlineGrid) {
        grid_column_count = 0;
    }
    if (block.tag == "textarea") {
        apply_user_textarea_size(impl, elem, rs);
    }
    if (auto kw = scan_inline_keyword(elem, "cursor"); !kw.empty()) {
        rs.computed.cursor = parse_cursor_keyword(kw);
    }
    if (auto kw = scan_inline_keyword(elem, "resize"); !kw.empty()) {
        const auto [resize, has_resize] = parse_resize_keyword(kw);
        if (has_resize) {
            rs.computed.resize = resize;
        }
    }
    const auto old_animation = block.animation;
    const auto old_computed = impl.style_store.computed(block.id);
    const bool grid_template_changed =
        !same_grid_track_hints(block.grid_columns,
                               block.grid_column_count,
                               grid_columns,
                               grid_column_count);
    const bool needs_layout =
        computed_change_needs_layout(old_computed, rs.computed) ||
        grid_template_changed;
    impl.style_store.computed(block.id) = rs.computed;
    impl.style_store.animated(block.id) = rs.animated;
    block.custom_props = rs.custom_props;
    block.box_shadows = rs.box_shadows;
    block.grid_columns = grid_columns;
    block.grid_column_count = grid_column_count;
    block.base_animated = rs.animated;
    block.animation = rs.animation;
    bool local_absolute_geometry_update = false;
    if (needs_layout &&
        absolute_geometry_change_can_stay_local(old_computed, rs.computed)) {
        local_absolute_geometry_update =
            update_absolute_geometry(impl, idx, rs.computed);
    }
    if (!same_animation(old_animation, block.animation)) {
        const bool old_candidate = animation_candidate(old_animation);
        const bool new_candidate = animation_candidate(block.animation);
        if (old_candidate && !new_candidate && impl.animation_candidate_count > 0) {
            --impl.animation_candidate_count;
        } else if (!old_candidate && new_candidate) {
            ++impl.animation_candidate_count;
        }
        block.animation_epoch = std::chrono::steady_clock::now();
    }
    return needs_layout && !local_absolute_geometry_update;
}

bool is_descendant_of_or_self(const std::vector<Block>& blocks,
                              int idx,
                              int root_idx) {
    for (int cur = idx; cur >= 0; ) {
        if (cur == root_idx) return true;
        cur = blocks[static_cast<std::size_t>(cur)].parent_idx;
    }
    return false;
}

bool restyle_subtree(detail::DocumentImpl& impl, int root_idx) {
    if (root_idx < 0 || root_idx >= static_cast<int>(impl.blocks.size()))
        return false;
    bool needs_layout = false;
    for (int idx = root_idx; idx < static_cast<int>(impl.blocks.size()); ++idx) {
        if (is_descendant_of_or_self(impl.blocks, idx, root_idx)) {
            needs_layout = restyle_block(impl, idx) || needs_layout;
        }
    }
    return needs_layout;
}

bool restyle_all_blocks(detail::DocumentImpl& impl) {
    bool needs_layout = false;
    for (int idx = 0; idx < static_cast<int>(impl.blocks.size()); ++idx) {
        needs_layout = restyle_block(impl, idx) || needs_layout;
    }
    return needs_layout;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

bool attribute_can_affect_selector_matching(std::string_view name) {
    // Inline style changes are parsed as declarations on the element and
    // SVG geometry attrs (e.g. path "d") should stay on the cheap paint path.
    // These are the live attributes our framework/CSS selectors commonly
    // depend on. The long-term version is a selector attribute-dependency
    // index, but this keeps hot control drags from forcing a full cascade.
    return name == "class" || name == "id" || name == "type" ||
           name == "hidden" ||
           name == "role" || name == "checked" || name == "disabled" ||
           starts_with(name, "aria-") || starts_with(name, "data-");
}

#if !defined(AFFINEUI_STUB_BUILD)
bool selector_simple_depends_on_attribute(const lxb_css_selector_t* sel,
                                          std::string_view name) {
    if (!sel) return false;
    switch (sel->type) {
        case LXB_CSS_SELECTOR_TYPE_CLASS:
            return name == "class";
        case LXB_CSS_SELECTOR_TYPE_ID:
            return name == "id";
        case LXB_CSS_SELECTOR_TYPE_ATTRIBUTE:
            return std::string_view(
                       reinterpret_cast<const char*>(sel->name.data),
                       sel->name.length) == name;
        default:
            return false;
    }
}

bool stylesheet_dependencies_stay_in_mutated_subtree(
    const detail::DocumentImpl& impl,
    std::string_view name) {
    for (auto* sst : impl.sheets) {
        if (!sst || !sst->root) continue;
        auto* rule_list = lxb_css_rule_list(sst->root);
        if (!rule_list) continue;
        for (auto* r = rule_list->first; r != nullptr; r = r->next) {
            if (r->type != LXB_CSS_RULE_STYLE) continue;
            auto* style = lxb_css_rule_style(r);
            if (!style) continue;
            for (auto* sl = style->selector; sl != nullptr; sl = sl->next) {
                bool selector_mentions_attr = false;
                bool selector_is_subtree_local = true;
                for (auto* sel = sl->first; sel != nullptr; sel = sel->next) {
                    if (sel->combinator ==
                            LXB_CSS_SELECTOR_COMBINATOR_SIBLING ||
                        sel->combinator ==
                            LXB_CSS_SELECTOR_COMBINATOR_FOLLOWING ||
                        sel->combinator ==
                            LXB_CSS_SELECTOR_COMBINATOR_CELL) {
                        selector_is_subtree_local = false;
                    }
                    selector_mentions_attr =
                        selector_mentions_attr ||
                        selector_simple_depends_on_attribute(sel, name);
                }
                if (selector_mentions_attr && !selector_is_subtree_local) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool simple_selector_depends_on_attribute(const SimpleSelector& simple,
                                          std::string_view name) {
    switch (simple.kind) {
        case SimpleSelector::Kind::Class: return name == "class";
        case SimpleSelector::Kind::Id: return name == "id";
        case SimpleSelector::Kind::Attr: return simple.name == name;
        case SimpleSelector::Kind::Tag: return false;
    }
    return false;
}

bool compound_depends_on_attribute(const CompoundSelector& compound,
                                   std::string_view name) {
    return std::any_of(compound.simples.begin(), compound.simples.end(),
                       [name](const SimpleSelector& simple) {
                           return simple_selector_depends_on_attribute(simple,
                                                                       name);
                       });
}

bool generated_content_depends_on_attribute(
    const detail::DocumentImpl& impl,
    std::string_view name) {
    for (const auto& rule : impl.generated_content_rules) {
        if (compound_depends_on_attribute(rule.target, name)) return true;
        if (compound_depends_on_attribute(rule.previous_adjacent, name))
            return true;
        for (const auto& ancestor : rule.ancestors) {
            if (compound_depends_on_attribute(ancestor, name)) return true;
        }
    }
    return false;
}

#endif

void reset_dynamic_block_state(detail::DocumentImpl& impl) {
    impl.hovered_idx = -1;
    impl.active_idx = -1;
    impl.focused_idx = -1;
    impl.hovered_chain.clear();
    impl.active_chain.clear();
    impl.pending_dirty_roots.clear();
    impl.content_size = Size{0, 0};
}

void recollect_blocks_from_current_dom(detail::DocumentImpl& impl) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl.doc) return;

    const auto previous_scroll =
        snapshot_scroll_state(impl, /*include_elements=*/true);
    impl.blocks.clear();
    impl.style_store.reset();
    impl.animation_candidate_count = 0;
    if (impl.resolver) impl.resolver->clear();

    impl.root_style                       = detail::ResolvedStyle{};
    impl.root_style.animated.color_rgba   = 0xDCDCE6FFu;
    impl.root_style.computed.font_size_px = 16;
    impl.root_style.computed.font_weight  = 400;

    auto* body = lxb_html_document_body_element(impl.doc);
    if (body && impl.resolver) {
        detail::ResolvedStyle html_style = impl.root_style;
        auto* body_node = lxb_dom_interface_node(body);
        if (body_node->parent != nullptr &&
            body_node->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            html_style = impl.resolver->resolve(
                lxb_dom_interface_element(body_node->parent), impl.root_style);
        }
        auto* body_elem = lxb_dom_interface_element(body_node);
        impl.root_style = impl.resolver->resolve(body_elem, html_style);
        apply_font_family_fills(impl, "body", attr_string(body_elem, "id"),
                                split_classes(attr_string(body_elem, "class")),
                                /*parent_idx=*/-1,
                                /*state_bits=*/0,
                                impl.root_style);
    }

    collect_blocks(impl,
                   body ? lxb_dom_interface_node(body)
                        : lxb_dom_interface_node(impl.doc),
                   impl.root_style,
                   /*parent_idx=*/-1);
    restore_scroll_state(impl, previous_scroll);
    for (const auto& block : impl.blocks) {
        if (block.animation.active && block.animation.name_hash != 0) {
            ++impl.animation_candidate_count;
        }
    }

    reset_dynamic_block_state(impl);
    impl.paint_dirty = true;
#else
    (void)impl;
#endif
}

lxb_status_t rematch_stylesheet_matches(lxb_dom_node_t* node) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!node) return LXB_STATUS_OK;

    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && node->ns == LXB_NS_HTML) {
        const auto status = lxb_html_document_element_styles_rematch(
            lxb_html_interface_element(node));
        if (status != LXB_STATUS_OK) return status;
    }

    for (auto* child = lxb_dom_node_first_child(node);
         child != nullptr; child = lxb_dom_node_next(child)) {
        const auto status = rematch_stylesheet_matches(child);
        if (status != LXB_STATUS_OK) return status;
    }
#else
    (void)node;
#endif
    return LXB_STATUS_OK;
}

bool rematch_stylesheet_matches_for_subtree(detail::DocumentImpl& impl,
                                            int root_idx) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl.doc) return false;

    lxb_dom_node_t* root_node = nullptr;
    if (root_idx >= 0 && root_idx < static_cast<int>(impl.blocks.size())) {
        auto* elem = impl.style_store.element_of(
            impl.blocks[static_cast<std::size_t>(root_idx)].id);
        if (elem) root_node = lxb_dom_interface_node(elem);
    }

    if (!root_node) {
        auto* body = lxb_html_document_body_element(impl.doc);
        root_node = body ? lxb_dom_interface_node(body)
                         : lxb_dom_interface_node(impl.doc);
    }

    return rematch_stylesheet_matches(root_node) == LXB_STATUS_OK;
#else
    (void)impl;
    (void)root_idx;
    return false;
#endif
}

bool rect_valid(const Rect& r) {
    return r.w > 0 && r.h > 0;
}

Rect union_rect(const Rect& a, const Rect& b) {
    if (!rect_valid(a)) return b;
    if (!rect_valid(b)) return a;
    const int x0 = std::min(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w);
    const int y1 = std::max(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

void add_dirty_rect(detail::DocumentImpl& impl, const Rect& r) {
    if (!rect_valid(r)) return;
    impl.dirty_rects.push_back(r);
}

Rect shadow_extent(const Rect& base,
                   const detail::BoxShadowLayer& layer) {
    if (layer.inset || (layer.rgba & 0xFFu) == 0) return base;
    if (layer.blur == 0 && layer.spread == 0 &&
        layer.offset_x == 0 && layer.offset_y == 0) {
        return base;
    }
    const int out = std::max(0, static_cast<int>(layer.blur)) +
                    std::max(0, static_cast<int>(layer.spread));
    return Rect{
        base.x + static_cast<int>(layer.offset_x) - out,
        base.y + static_cast<int>(layer.offset_y) - out,
        base.w + out * 2,
        base.h + out * 2,
    };
}

Rect transform_visual_rect(const Rect& r, const Mat2x3& m) {
    if (!rect_valid(r) || m.is_identity()) return r;
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

Rect transform_border_rect(const Rect& r, const Mat2x3& m) {
    if (!rect_valid(r) || m.is_identity()) return r;
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
    const int x0 = static_cast<int>(std::floor(min_x));
    const int y0 = static_cast<int>(std::floor(min_y));
    const int x1 = static_cast<int>(std::ceil(max_x));
    const int y1 = static_cast<int>(std::ceil(max_y));
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

Rect block_border_visual_rect(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return {};
    const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
    const int dy = scroll_offset_y_for(impl.blocks, impl.style_store, idx);
    const Rect base{b.bounds.x, b.bounds.y - dy, b.bounds.w, b.bounds.h};
    return transform_border_rect(base, effective_transform_for(impl, idx));
}

Rect block_visual_rect(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return {};
    const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
    const int dy = scroll_offset_y_for(impl.blocks, impl.style_store, idx);
    const Rect base{b.bounds.x, b.bounds.y - dy, b.bounds.w, b.bounds.h};
    Rect out = base;
    if (b.box_shadows) {
        for (const auto& layer : *b.box_shadows) {
            out = union_rect(out, shadow_extent(base, layer));
        }
    } else {
        const auto& an = impl.style_store.animated(b.id);
        detail::BoxShadowLayer layer{};
        layer.rgba = an.shadow_rgba;
        layer.offset_x = an.shadow_offset_x;
        layer.offset_y = an.shadow_offset_y;
        layer.blur = an.shadow_blur;
        layer.spread = an.shadow_spread;
        layer.inset = an.shadow_inset;
        out = union_rect(out, shadow_extent(base, layer));
    }
#if !defined(AFFINEUI_STUB_BUILD)
    out = transform_visual_rect(out, effective_transform_for(impl, idx));
#endif
    return out;
}

bool vertical_scrollbar_geometry(const detail::DocumentImpl& impl,
                                 int idx,
                                 ScrollbarGeometry& out) {
    if (!block_is_scrollable_y(impl, idx)) return false;
    const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
    const Rect box = block_border_visual_rect(impl, idx);
    constexpr int kTrackWidth = 6;
    constexpr int kTrackPad = 2;
    if (box.w <= kTrackWidth + kTrackPad * 2 ||
        box.h <= kTrackPad * 2 + 1) {
        return false;
    }

    const int track_h = box.h - 2 * kTrackPad;
    const float ratio =
        static_cast<float>(b.bounds.h) / static_cast<float>(b.content_h);
    const int natural_thumb_h = static_cast<int>(
        std::round(static_cast<float>(track_h) * ratio));
    const int thumb_h = std::clamp(
        std::max(24, natural_thumb_h), 1, std::max(1, track_h));
    const int scroll_range = std::max(1, b.content_h - b.bounds.h);
    const int thumb_travel = std::max(0, track_h - thumb_h);
    const int thumb_y_off = thumb_travel == 0
        ? 0
        : static_cast<int>(
            std::round(static_cast<float>(thumb_travel) *
                       static_cast<float>(b.scroll_y) /
                       static_cast<float>(scroll_range)));

    out.track = Rect{
        box.x + box.w - kTrackWidth - kTrackPad,
        box.y + kTrackPad,
        kTrackWidth,
        track_h,
    };
    out.thumb = Rect{
        out.track.x,
        out.track.y + thumb_y_off,
        out.track.w,
        thumb_h,
    };
    out.scroll_range = scroll_range;
    out.thumb_travel = thumb_travel;
    return true;
}

bool set_block_scroll_y(detail::DocumentImpl& impl, int idx, int scroll_y) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return false;
    if (!block_is_scrollable_y(impl, idx)) return false;
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    const int max_scroll = std::max(0, block.content_h - block.bounds.h);
    const int next = std::clamp(scroll_y, 0, max_scroll);
    if (next == block.scroll_y) return false;

    add_dirty_rect(impl, block_visual_rect(impl, idx));
    block.scroll_y = next;
    add_dirty_rect(impl, block_visual_rect(impl, idx));
    return true;
}

bool scrollbar_scroll_from_thumb_y(detail::DocumentImpl& impl,
                                   int idx,
                                   int thumb_y) {
    ScrollbarGeometry geometry{};
    if (!vertical_scrollbar_geometry(impl, idx, geometry)) return false;
    if (geometry.thumb_travel <= 0) return false;
    const int track_relative = std::clamp(
        thumb_y - geometry.track.y, 0, geometry.thumb_travel);
    const int next = static_cast<int>(
        std::round(static_cast<double>(track_relative) *
                   static_cast<double>(geometry.scroll_range) /
                   static_cast<double>(geometry.thumb_travel)));
    return set_block_scroll_y(impl, idx, next);
}

bool find_vertical_scrollbar_at(const detail::DocumentImpl& impl,
                                Point point,
                                int& out_idx,
                                ScrollbarGeometry& out) {
    for (std::size_t i = impl.blocks.size(); i-- > 0; ) {
        ScrollbarGeometry geometry{};
        if (!vertical_scrollbar_geometry(
                impl, static_cast<int>(i), geometry)) {
            continue;
        }
        if (rect_contains(geometry.track, point.x, point.y)) {
            out_idx = static_cast<int>(i);
            out = geometry;
            return true;
        }
    }
    return false;
}

Rect subtree_visual_rect(const detail::DocumentImpl& impl, int root_idx) {
    Rect out{};
    if (root_idx < 0 || root_idx >= static_cast<int>(impl.blocks.size()))
        return out;
    for (int idx = root_idx; idx < static_cast<int>(impl.blocks.size()); ++idx) {
        if (!is_descendant_of_or_self(impl.blocks, idx, root_idx)) continue;
        out = union_rect(out, block_visual_rect(impl, idx));
    }
    return out;
}

Rect document_visual_rect(const detail::DocumentImpl& impl) {
    Rect out{};
    for (int idx = 0; idx < static_cast<int>(impl.blocks.size()); ++idx) {
        out = union_rect(out, block_visual_rect(impl, idx));
    }
    return out;
}

int find_block_by_elem_id(const detail::DocumentImpl& impl,
                          std::string_view elem_id) {
    if (elem_id.empty()) return -1;
    for (std::size_t i = 0; i < impl.blocks.size(); ++i) {
        if (impl.blocks[i].elem_id == elem_id) return static_cast<int>(i);
    }
    return -1;
}

int block_index_for_exact_element(const detail::DocumentImpl& impl,
                                  lxb_dom_element_t* elem) {
    if (!elem) return -1;
    for (std::size_t i = 0; i < impl.blocks.size(); ++i) {
        if (impl.style_store.element_of(impl.blocks[i].id) == elem) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

lxb_dom_element_t* element_for_block(detail::DocumentImpl& impl, int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return nullptr;
    return impl.style_store.element_of(
        impl.blocks[static_cast<std::size_t>(idx)].id);
}

const lxb_dom_element_t* element_for_block(const detail::DocumentImpl& impl,
                                           int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return nullptr;
    return impl.style_store.element_of(
        impl.blocks[static_cast<std::size_t>(idx)].id);
}

lxb_dom_element_t* find_dom_element_by_id(lxb_dom_node_t* root,
                                          std::string_view elem_id) {
    if (!root || elem_id.empty()) return nullptr;
    if (root->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        auto* elem = lxb_dom_interface_element(root);
        if (has_attr(elem, "id") && attr_string(elem, "id") == elem_id) {
            return elem;
        }
    }
    for (auto* child = lxb_dom_node_first_child(root);
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (auto* found = find_dom_element_by_id(child, elem_id)) {
            return found;
        }
    }
    return nullptr;
}

lxb_dom_element_t* find_dom_element_by_id(detail::DocumentImpl& impl,
                                          std::string_view elem_id) {
    const int block_idx = find_block_by_elem_id(impl, elem_id);
    if (block_idx >= 0) {
        return impl.style_store.element_of(
            impl.blocks[static_cast<std::size_t>(block_idx)].id);
    }
    if (!impl.doc) return nullptr;
    auto* body = lxb_html_document_body_element(impl.doc);
    auto* root = body ? lxb_dom_interface_node(body)
                      : lxb_dom_interface_node(impl.doc);
    return find_dom_element_by_id(root, elem_id);
}

int block_index_for_element_or_ancestor(const detail::DocumentImpl& impl,
                                        lxb_dom_element_t* elem) {
    if (!elem) return -1;
    for (auto* node = lxb_dom_interface_node(elem);
         node != nullptr; node = node->parent) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* cur_elem = lxb_dom_interface_element(node);
        for (std::size_t i = 0; i < impl.blocks.size(); ++i) {
            if (impl.style_store.element_of(impl.blocks[i].id) == cur_elem) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

void refresh_block_metadata_from_element(Block& block,
                                         lxb_dom_element_t* elem) {
    if (!elem) return;
    block.elem_id = attr_string(elem, "id");
    block.classes = split_classes(attr_string(elem, "class"));
    block.attrs = element_attrs(elem);
    if (block.tag == "img") {
        block.image_src = attr_string(elem, "src");
    }
    if (block.tag == "input" || block.tag == "textarea") {
        block.placeholder = attr_string(elem, "placeholder");
    }
    if (block.tag == "input") {
        block.input_type  = attr_string(elem, "type");
        block.role_attr   = attr_string(elem, "role");
        block.is_checked  = has_attr(elem, "checked");
        block.is_disabled = has_attr(elem, "disabled");
        block.text_control = input_type_accepts_text(block.input_type);
    } else if (block.tag == "textarea") {
        block.is_disabled = has_attr(elem, "disabled");
        block.text_control = true;
    }
}

bool element_has_element_child(lxb_dom_element_t* elem) {
    if (!elem) return false;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) return true;
    }
    return false;
}

void mark_live_mutation_dirty(detail::DocumentImpl& impl,
                              int dirty_root_idx,
                              const Rect& old_rect,
                              bool needs_layout) {
    const auto dirty_count_before = impl.dirty_rects.size();
    add_dirty_rect(impl, old_rect);
    if (needs_layout) {
        if (dirty_root_idx >= 0) {
            impl.pending_dirty_roots.push_back(dirty_root_idx);
        }
        impl.content_size = Size{0, 0};
    } else if (dirty_root_idx >= 0) {
        add_dirty_rect(impl, subtree_visual_rect(impl, dirty_root_idx));
    }
    if (impl.dirty_rects.size() == dirty_count_before && !needs_layout) {
        impl.paint_dirty = true;
    }
}

lxb_dom_element_t* first_descendant_with_class(lxb_dom_element_t* elem,
                                               std::string_view cls) {
    if (!elem) return nullptr;
    const auto classes = split_classes(attr_string(elem, "class"));
    if (std::find(classes.begin(), classes.end(), cls) != classes.end()) {
        return elem;
    }
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (auto* found = first_descendant_with_class(
                lxb_dom_interface_element(child), cls)) {
            return found;
        }
    }
    return nullptr;
}

lxb_dom_element_t* nearest_ancestor_with_class(lxb_dom_element_t* elem,
                                               std::string_view cls) {
    for (auto* current = elem; current != nullptr;
         current = parent_element(current)) {
        const auto classes = split_classes(attr_string(current, "class"));
        if (std::find(classes.begin(), classes.end(), cls) != classes.end()) {
            return current;
        }
    }
    return nullptr;
}

lxb_dom_element_t* first_descendant_input(lxb_dom_element_t* elem) {
    if (!elem) return nullptr;
    if (tag_name(elem) == "input") return elem;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (auto* found = first_descendant_input(
                lxb_dom_interface_element(child))) {
            return found;
        }
    }
    return nullptr;
}

lxb_dom_element_t* first_descendant_tag(lxb_dom_element_t* elem,
                                        std::string_view tag) {
    if (!elem) return nullptr;
    if (tag_name(elem) == tag) return elem;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (auto* found = first_descendant_tag(
                lxb_dom_interface_element(child), tag)) {
            return found;
        }
    }
    return nullptr;
}

lxb_dom_element_t* nearest_checkbox_wrapper(lxb_dom_element_t* elem) {
    lxb_dom_element_t* decius_candidate = nullptr;
    for (auto* node = elem ? lxb_dom_interface_node(elem) : nullptr;
         node != nullptr; node = node->parent) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* candidate = lxb_dom_interface_element(node);
        const auto classes = split_classes(attr_string(candidate, "class"));
        const auto widget = attr_string(candidate, "data-aui-widget");
        const bool checkbox_widget = widget == "checkbox";
        const bool decius_widget =
            std::find(classes.begin(), classes.end(), "dcs-check") !=
                classes.end() ||
            std::find(classes.begin(), classes.end(), "dcs-radio") !=
                classes.end() ||
            std::find(classes.begin(), classes.end(), "dcs-switch") !=
                classes.end();
        if (checkbox_widget) return candidate;
        if (decius_widget && decius_candidate == nullptr) {
            decius_candidate = candidate;
        }
    }
    return decius_candidate != nullptr ? decius_candidate : elem;
}

// Box collection omits `display:none` subtrees entirely (no Block — see the
// Display::None `continue` in the collection pass), so a subtree that was
// hidden has no boxes at all. A selector-affecting mutation (e.g. removing a
// `.collapsed` class keyed by `.collapsed > .body{display:none}`) can flip
// such a subtree back to visible — but restyling the *existing* blocks can
// never recreate the missing boxes; only a fresh box collection can. Detect
// exactly that case so we recollect when (and only when) a hidden subtree
// became visible, rather than on every class/aria/data tick (which would wreck
// slider-drag and toggle perf). Run AFTER restyle so parent computed styles
// (which the child inherits from) are already up to date.
//
// Boundary insight: a hidden subtree's root is a DOM child of a still-visible
// element (which keeps its Block). So we only resolve DOM children that have no
// Block of their own — in the common no-display-change case there are none.
bool selector_mutation_reveals_hidden_subtree(detail::DocumentImpl& impl,
                                              int root_idx) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (root_idx < 0 || root_idx >= static_cast<int>(impl.blocks.size())) {
        return false;
    }
    using Display = detail::ComputedStyle::Display;
    for (int idx = root_idx; idx < static_cast<int>(impl.blocks.size()); ++idx) {
        if (!is_descendant_of_or_self(impl.blocks, idx, root_idx)) continue;
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        auto* elem = impl.style_store.element_of(block.id);
        if (!elem) continue;
        detail::ResolvedStyle parent_rs;
        parent_rs.computed = impl.style_store.computed(block.id);
        parent_rs.animated = impl.style_store.animated(block.id);
        parent_rs.custom_props = block.custom_props;
        for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
             c != nullptr; c = lxb_dom_node_next(c)) {
            if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            // Only HTML elements go through the HTML style resolver (foreign
            // SVG/MathML elements would crash lxb_html_element_style_walk) — and
            // their box participation is handled by their own subtree pass.
            if (c->ns != LXB_NS_HTML) continue;
            auto* child = lxb_dom_interface_element(c);
            if (block_index_for_exact_element(impl, child) >= 0) continue;
            // No Block for this child — it resolved to display:none when boxes
            // were last collected. If it now resolves visible, a hidden subtree
            // needs its boxes (re)created.
            if (impl.resolver &&
                impl.resolver->resolve(child, parent_rs).computed.display !=
                    Display::None) {
                return true;
            }
        }
    }
#else
    (void) impl;
    (void) root_idx;
#endif
    return false;
}

bool set_attribute_on_element(detail::DocumentImpl& impl,
                              lxb_dom_element_t* elem,
                              std::string_view name,
                              std::string_view value) {
    if (!elem || name.empty()) return false;
    const bool already_present = has_attr(elem, name);
    if (already_present && attr_string(elem, name) == value) return false;

    const int target_idx = block_index_for_exact_element(impl, elem);
    const int dirty_root_idx =
        target_idx >= 0 ? target_idx
                        : block_index_for_element_or_ancestor(impl, elem);
    const bool selector_affecting =
        attribute_can_affect_selector_matching(name);
    const bool subtree_local_selectors =
        !selector_affecting ||
        stylesheet_dependencies_stay_in_mutated_subtree(impl, name);
    const int mutation_dirty_root_idx =
        selector_affecting && !subtree_local_selectors && target_idx >= 0 &&
                impl.blocks[static_cast<std::size_t>(target_idx)].parent_idx >= 0
            ? impl.blocks[static_cast<std::size_t>(target_idx)].parent_idx
            : dirty_root_idx;
    const Rect old_rect = mutation_dirty_root_idx >= 0
                              ? subtree_visual_rect(impl, mutation_dirty_root_idx)
                              : document_visual_rect(impl);
    const bool recollect_generated_subtree =
        selector_affecting &&
        generated_content_depends_on_attribute(impl, name);
    const bool recollect_box_tree =
        name == "hidden" || name == "style";

    if (!lxb_dom_element_set_attribute(elem, as_lxb(name), name.size(),
                                       as_lxb(value), value.size())) {
        return false;
    }

    bool needs_layout = false;
    if (selector_affecting) {
        if (!rematch_stylesheet_matches_for_subtree(
                impl, mutation_dirty_root_idx)) {
            return false;
        }
        if (impl.resolver) impl.resolver->clear();

        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            refresh_block_metadata_from_element(block, elem);
        }

        if (recollect_generated_subtree || recollect_box_tree) {
            recollect_blocks_from_current_dom(impl);
            mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                     /*needs_layout=*/true);
            return true;
        }

        needs_layout = mutation_dirty_root_idx >= 0
                           ? restyle_subtree(impl, mutation_dirty_root_idx)
                           : restyle_all_blocks(impl);
        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            if (block.tag == "img" && name == "src") needs_layout = true;
        }
        // If this mutation revealed a previously display:none subtree, the box
        // tree is missing those boxes (collection skips hidden subtrees) and
        // restyle alone can't recreate them — recollect so they reappear.
        if (mutation_dirty_root_idx >= 0 &&
            selector_mutation_reveals_hidden_subtree(impl,
                                                     mutation_dirty_root_idx)) {
            recollect_blocks_from_current_dom(impl);
            needs_layout = true;
        }
        mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                 needs_layout);
        return true;
    }

    if (target_idx >= 0) {
        if (impl.resolver) impl.resolver->invalidate(elem);
        auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
        refresh_block_metadata_from_element(block, elem);
        needs_layout = restyle_subtree(impl, target_idx);
        if (block.tag == "img" && name == "src") needs_layout = true;
    }
    mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                             needs_layout);
    return true;
}

bool remove_attribute_on_element(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* elem,
                                 std::string_view name) {
    if (!elem || name.empty() || !has_attr(elem, name)) return false;

    const int target_idx = block_index_for_exact_element(impl, elem);
    const int dirty_root_idx =
        target_idx >= 0 ? target_idx
                        : block_index_for_element_or_ancestor(impl, elem);
    const bool selector_affecting =
        attribute_can_affect_selector_matching(name);
    const bool subtree_local_selectors =
        !selector_affecting ||
        stylesheet_dependencies_stay_in_mutated_subtree(impl, name);
    const int mutation_dirty_root_idx =
        selector_affecting && !subtree_local_selectors && target_idx >= 0 &&
                impl.blocks[static_cast<std::size_t>(target_idx)].parent_idx >= 0
            ? impl.blocks[static_cast<std::size_t>(target_idx)].parent_idx
            : dirty_root_idx;
    const Rect old_rect = mutation_dirty_root_idx >= 0
                              ? subtree_visual_rect(impl, mutation_dirty_root_idx)
                              : document_visual_rect(impl);
    const bool recollect_generated_subtree =
        selector_affecting &&
        generated_content_depends_on_attribute(impl, name);
    const bool recollect_box_tree =
        name == "hidden" || name == "style";

    if (lxb_dom_element_remove_attribute(elem, as_lxb(name), name.size())
            != LXB_STATUS_OK) {
        return false;
    }

    bool needs_layout = false;
    if (selector_affecting) {
        if (!rematch_stylesheet_matches_for_subtree(
                impl, mutation_dirty_root_idx)) {
            return false;
        }
        if (impl.resolver) impl.resolver->clear();

        if (target_idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
            refresh_block_metadata_from_element(block, elem);
        }

        if (recollect_generated_subtree || recollect_box_tree) {
            recollect_blocks_from_current_dom(impl);
            mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                     /*needs_layout=*/true);
            return true;
        }

        needs_layout = mutation_dirty_root_idx >= 0
                           ? restyle_subtree(impl, mutation_dirty_root_idx)
                           : restyle_all_blocks(impl);
        mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                                 needs_layout);
        return true;
    }

    if (target_idx >= 0) {
        if (impl.resolver) impl.resolver->invalidate(elem);
        auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
        refresh_block_metadata_from_element(block, elem);
        needs_layout = restyle_subtree(impl, target_idx);
    }
    mark_live_mutation_dirty(impl, mutation_dirty_root_idx, old_rect,
                             needs_layout);
    return true;
}

bool set_text_on_element(detail::DocumentImpl& impl,
                         lxb_dom_element_t* elem,
                         std::string_view text) {
    if (!elem) return false;
    auto* node = lxb_dom_interface_node(elem);
    if (node_text(node) == text) return false;

    const int target_idx = block_index_for_exact_element(impl, elem);
    if (target_idx < 0) return false;
    const Rect old_rect = subtree_visual_rect(impl, target_idx);
    if (lxb_dom_node_text_content_set(node, as_lxb(text), text.size())
            != LXB_STATUS_OK) {
        return false;
    }
    auto& block = impl.blocks[static_cast<std::size_t>(target_idx)];
    block.text = std::string(text);
    mark_live_mutation_dirty(impl, target_idx, old_rect,
                             /*needs_layout=*/true);
    return true;
}

double element_attr_double(lxb_dom_element_t* elem,
                           std::string_view name,
                           double fallback) {
    if (!elem || !has_attr(elem, name)) return fallback;
    const auto value = attr_string(elem, name);
    if (value.empty()) return fallback;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    return end == value.c_str() ? fallback : parsed;
}

bool element_attr_true(lxb_dom_element_t* elem, std::string_view name) {
    if (!elem || !has_attr(elem, name)) return false;
    const auto value = attr_string(elem, name);
    return value.empty() || value == "true" || value == "checked" ||
           value == "1";
}

std::string widget_event_name(lxb_dom_element_t* elem) {
    if (!elem) return {};
    if (auto name = attr_string(elem, "data-aui-name"); !name.empty()) {
        return name;
    }
    return attr_string(elem, "id");
}

void emit_widget_change(detail::DocumentImpl& impl,
                        lxb_dom_element_t* elem,
                        std::string_view value) {
    auto name = widget_event_name(elem);
    if (name.empty()) return;
    impl.changed_widgets.push_back({std::move(name), std::string(value)});
}

void set_live_text_value(detail::DocumentImpl& impl,
                         int idx,
                         Block& block,
                         std::string value);
void set_live_text_state(detail::DocumentImpl& impl,
                         int idx,
                         Block& block,
                         std::string value,
                         std::size_t caret);
std::string emitted_text_control_value(const Block& block);

std::string decius_slider_fill_style(double min, double max, double value,
                                     bool bipolar) {
    const double p = normalized_control_value(value, min, max);
    if (!bipolar) return "width:" + percent_string(p);
    const double start = std::min(0.5, p);
    const double width = std::abs(p - 0.5);
    return "left:" + percent_string(start) + ";right:auto;width:" +
           percent_string(width);
}

std::string decius_slider_thumb_style(double min, double max, double value) {
    return "left:" + percent_string(normalized_control_value(value, min, max));
}

std::string decius_fader_style(double min, double max, double value) {
    const double p = 1.0 - normalized_control_value(value, min, max);
    return "--pos:" + percent_string(p);
}

double decius_knob_angle(double min, double max, double value) {
    return -135.0 + normalized_control_value(value, min, max) * 270.0;
}

std::pair<double, double> decius_knob_ring_point(double deg) {
    constexpr double r = 10.5;
    constexpr double pi = 3.14159265358979323846;
    const double rad = deg * pi / 180.0;
    return {12.0 + r * std::cos(rad), 12.0 + r * std::sin(rad)};
}

std::string decius_knob_arc_path(double min, double max, double value,
                                 bool bipolar) {
    const double p = normalized_control_value(value, min, max);
    const double sweep_degrees = bipolar ? (p - 0.5) * 270.0 : p * 270.0;
    if (std::abs(sweep_degrees) <= 0.5) return {};

    const double start_degrees = bipolar ? -90.0 : -225.0;
    const double end_degrees = start_degrees + sweep_degrees;
    const auto [x0, y0] = decius_knob_ring_point(start_degrees);
    const auto [x1, y1] = decius_knob_ring_point(end_degrees);
    const int large = std::abs(sweep_degrees) > 180.0 ? 1 : 0;
    const int sweep = end_degrees >= start_degrees ? 1 : 0;

    return "M " + compact_number(x0) + " " + compact_number(y0) +
           " A 10.5 10.5 0 " + std::to_string(large) + " " +
           std::to_string(sweep) + " " + compact_number(x1) + " " +
           compact_number(y1);
}

double value_from_x(const Rect& bounds, int x, double min, double max) {
    const double t = bounds.w > 0
        ? std::clamp((static_cast<double>(x) - bounds.x) / bounds.w,
                     0.0, 1.0)
        : 0.0;
    return min + t * (max - min);
}

double value_from_y(const Rect& bounds, int y, double min, double max) {
    const double t = bounds.h > 0
        ? std::clamp((static_cast<double>(y) - bounds.y) / bounds.h,
                     0.0, 1.0)
        : 0.0;
    return min + (1.0 - t) * (max - min);
}

constexpr int kLiveDragThresholdPx = 3;
constexpr int kTextareaResizeGripPx = 16;

bool pointer_moved_past_threshold(const Event& ev,
                                  const detail::DocumentImpl::LiveControlDrag& drag) {
    return std::abs(ev.pos.x - drag.start_x) >= kLiveDragThresholdPx ||
           std::abs(ev.pos.y - drag.start_y) >= kLiveDragThresholdPx;
}

bool textarea_resize_axes(detail::ComputedStyle::Resize resize,
                          bool& resize_x,
                          bool& resize_y) {
    using R = detail::ComputedStyle::Resize;
    resize_x = resize == R::Both || resize == R::Horizontal;
    resize_y = resize == R::Both || resize == R::Vertical;
    return resize_x || resize_y;
}

Rect textarea_resize_grip_rect(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return {};
    const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (block.tag != "textarea" || block.is_disabled) return {};
    bool resize_x = false;
    bool resize_y = false;
    const auto& cs = impl.style_store.computed(block.id);
    if (!textarea_resize_axes(cs.resize, resize_x, resize_y)) return {};
    const Rect bounds = block_border_visual_rect(impl, idx);
    const int grip = std::min(kTextareaResizeGripPx,
                              std::max(1, std::min(bounds.w, bounds.h)));
    return Rect{bounds.x + bounds.w - grip, bounds.y + bounds.h - grip,
                grip, grip};
}

bool point_in_textarea_resize_grip(const detail::DocumentImpl& impl,
                                   int idx,
                                   Point point,
                                   bool& resize_x,
                                   bool& resize_y) {
    resize_x = false;
    resize_y = false;
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return false;
    const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (block.tag != "textarea" || block.is_disabled) return false;
    const auto& cs = impl.style_store.computed(block.id);
    if (!textarea_resize_axes(cs.resize, resize_x, resize_y)) return false;
    return rect_contains(textarea_resize_grip_rect(impl, idx),
                         point.x, point.y);
}

bool update_live_control_value(detail::DocumentImpl& impl,
                               lxb_dom_element_t* elem,
                               LiveControlKind kind,
                               double min,
                               double max,
                               double value,
                               bool bipolar) {
    if (!elem) return false;
    if (max <= min) max = min + 1.0;
    const double clamped = std::clamp(value, min, max);
    const std::string value_text = compact_number(clamped);

    bool changed = false;
    const bool value_changed =
        set_attribute_on_element(impl, elem, "value", value_text);
    changed = value_changed || changed;
    if (value_changed && kind == LiveControlKind::NumericInput) {
        const int idx = block_index_for_exact_element(impl, elem);
        if (idx >= 0) {
            auto& block = impl.blocks[static_cast<std::size_t>(idx)];
            if (block.text_control) {
                set_live_text_value(impl, idx, block, value_text);
            }
        }
        auto* combo = nearest_ancestor_with_class(elem, "dcs-combo");
        if (!combo && has_attr(elem, "data-dcs-combo")) combo = elem;
        if (combo) {
            changed =
                set_attribute_on_element(impl, combo, "data-value", value_text) ||
                changed;
            changed =
                set_attribute_on_element(impl, combo, "aria-valuenow", value_text) ||
                changed;
            const double fill_min =
                element_attr_double(elem, "data-fill-min", min);
            const double fill_max =
                element_attr_double(elem, "data-fill-max", max);
            changed = set_attribute_on_element(
                impl, combo, "style",
                "--fill:" + percent_string(
                    normalized_control_value(clamped, fill_min, fill_max))) ||
                changed;
        }
    }
    if (kind == LiveControlKind::AuiKnob ||
        kind == LiveControlKind::DeciusSlider ||
        kind == LiveControlKind::DeciusFader ||
        kind == LiveControlKind::DeciusKnob) {
        changed =
            set_attribute_on_element(impl, elem, "data-value", value_text) ||
            changed;
    }
    if (value_changed) emit_widget_change(impl, elem, value_text);

    if (kind == LiveControlKind::DeciusSlider) {
        if (auto* fill = first_descendant_with_class(elem, "dcs-slider__fill")) {
            changed = set_attribute_on_element(
                impl, fill, "style",
                decius_slider_fill_style(min, max, clamped, bipolar)) || changed;
        }
        if (auto* thumb = first_descendant_with_class(elem, "dcs-slider__thumb")) {
            changed = set_attribute_on_element(
                impl, thumb, "style",
                decius_slider_thumb_style(min, max, clamped)) || changed;
        }
    } else if (kind == LiveControlKind::DeciusFader) {
        changed = set_attribute_on_element(
            impl, elem, "style",
            decius_fader_style(min, max, clamped)) || changed;
    } else if (kind == LiveControlKind::AuiKnob ||
               kind == LiveControlKind::DeciusKnob) {
        const char* indicator_class = kind == LiveControlKind::AuiKnob
            ? "aui-knob__indicator"
            : "dcs-knob__indicator";
        const char* arc_class = kind == LiveControlKind::AuiKnob
            ? "aui-knob__arc"
            : "dcs-knob__arc";
        const char* value_class = kind == LiveControlKind::AuiKnob
            ? "aui-knob__value"
            : "dcs-knob__value";
        if (auto* indicator = first_descendant_with_class(elem, indicator_class)) {
            changed = set_attribute_on_element(
                impl, indicator, "style",
                "--angle:" + compact_number(
                    decius_knob_angle(min, max, clamped)) + "deg") || changed;
        }
        if (auto* arc = first_descendant_with_class(elem, arc_class)) {
            const auto path = decius_knob_arc_path(min, max, clamped, bipolar);
            changed = path.empty()
                ? (remove_attribute_on_element(impl, arc, "d") || changed)
                : (set_attribute_on_element(impl, arc, "d", path) || changed);
        }
        if (auto* label = first_descendant_with_class(elem, value_class)) {
            changed = set_text_on_element(impl, label, value_text) || changed;
        }
    }

    return changed;
}

LiveControlKind live_control_kind_for_block(const Block& block) {
    if (block.tag == "input" && block.input_type == "range") {
        return LiveControlKind::RangeInput;
    }
    if (block.tag == "input" && block.input_type == "number") {
        return LiveControlKind::NumericInput;
    }
    if (block_has_attr(block, "data-dcs-combo")) {
        return LiveControlKind::NumericInput;
    }
    if (block_has_attr(block, "data-aui-knob")) {
        return LiveControlKind::AuiKnob;
    }
    if (block_has_attr(block, "data-dcs-slider")) {
        return LiveControlKind::DeciusSlider;
    }
    if (block_has_attr(block, "data-dcs-fader")) {
        return LiveControlKind::DeciusFader;
    }
    if (block_has_attr(block, "data-dcs-knob")) {
        return LiveControlKind::DeciusKnob;
    }
    return LiveControlKind::None;
}

// ── Dock splitter drag (data-dcs-splitter) ──────────────────────────────────
// A `.dcs-splitter` between two flex panes. Grabbing it captures the pair's
// sizes; dragging redistributes their shared budget via inline flex-basis.
// Canonical math from decius.js initSplitter: axis = clientX (vertical split,
// the default) or clientY (data-dcs-splitter="h" / .dcs-splitter--h, a
// horizontal divider in a column dock); minPx = 24; budget = prev+next held
// constant so the rest of the dock never reflows.
bool find_splitter_at(detail::DocumentImpl& impl,
                      int from_idx,
                      Point /*point*/,
                      detail::DocumentImpl::SplitterDrag& out) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = element_for_block(impl, idx);
        if (!elem || !has_attr(elem, "data-dcs-splitter")) continue;

        auto* node = lxb_dom_interface_node(elem);
        auto* prev = previous_element_sibling(node);
        auto* next = next_element_sibling(node);
        if (!prev || !next) return false;
        const int prev_idx = block_index_for_exact_element(impl, prev);
        const int next_idx = block_index_for_exact_element(impl, next);
        if (prev_idx < 0 || next_idx < 0) return false;

        const auto& blk = impl.blocks[static_cast<std::size_t>(idx)];
        const bool horizontal =
            block_has_class(blk, "dcs-splitter--h") ||
            attr_string(elem, "data-dcs-splitter") == "h";
        const auto& pb = impl.blocks[static_cast<std::size_t>(prev_idx)];
        const auto& nb = impl.blocks[static_cast<std::size_t>(next_idx)];
        out.block_idx = idx;
        out.prev = prev;
        out.next = next;
        out.horizontal = horizontal;
        out.prev_size = horizontal ? pb.bounds.h : pb.bounds.w;
        out.next_size = horizontal ? nb.bounds.h : nb.bounds.w;
        out.budget = out.prev_size + out.next_size;
        out.min_px = 24;
        // Note which adjacent pane grows (flex:1). The center/document and
        // nested dock containers are emitted flex:1; side leaves are flex:0 0.
        out.prev_grows = impl.style_store.computed(pb.id).flex_grow > 0;
        out.next_grows = impl.style_store.computed(nb.id).flex_grow > 0;
        // Splitter drags are local DOM resizes, like upstream Decius. Asking
        // the app to rebuild the declarative dock tree here re-resolves
        // sibling slots and can move unrelated borders; structural dock and
        // tearoff operations are the ones that request an app layout rebuild.
        out.persist_layout = false;
        return true;
    }
    return false;
}

// Apply the splitter drag at the current pointer position. Sets inline
// flex-basis on prev/next; the style mutation triggers relayout.
bool update_splitter_drag(detail::DocumentImpl& impl, const Event& ev) {
    auto& d = impl.splitter_drag;
    if (d.block_idx < 0 || !d.prev || !d.next) return false;
    const int pos = d.horizontal ? ev.pos.y : ev.pos.x;
    const int delta = pos - d.start_pos;
    const int lo = d.min_px;
    const int hi = std::max(d.min_px, d.budget - d.min_px);
    auto pin = [&](lxb_dom_element_t* el, int basis) {
        return set_attribute_on_element(
            impl, el, "style",
            "flex:0 0 " + std::to_string(basis) + "px;min-width:0;min-height:0");
    };
    // Pin only the FIXED side; leave the flex:1 grower untouched so it keeps
    // absorbing window space. Freezing the grower (the old both-sides write)
    // detached the layout from the far window edge and broke window-resize of
    // the document. Mirrors decius.js, where the splitter resizes the sized
    // pane and the flexible pane reflows.
    if (d.next_grows && !d.prev_grows) {
        // prev is the sized pane; growing/shrinking it moves the boundary and
        // the grower (next) reflows into the remainder.
        return pin(d.prev, std::clamp(d.prev_size + delta, lo, hi));
    }
    if (d.prev_grows && !d.next_grows) {
        // next is the sized pane; dragging right (delta>0) shrinks it.
        return pin(d.next, std::clamp(d.next_size - delta, lo, hi));
    }
    // Two fixed siblings under a growing parent (or, rarely, two growers):
    // split the shared budget between them — the parent still fills the window.
    const int prev_basis = std::clamp(d.prev_size + delta, lo, hi);
    bool changed = pin(d.prev, prev_basis);
    changed = pin(d.next, d.budget - prev_basis) || changed;
    return changed;
}

bool find_live_control_at(detail::DocumentImpl& impl,
                          int from_idx,
                          Point point,
                          detail::DocumentImpl::LiveControlDrag& out) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        bool resize_x = false;
        bool resize_y = false;
        if (point_in_textarea_resize_grip(impl, idx, point,
                                          resize_x, resize_y)) {
            auto* elem = element_for_block(impl, idx);
            if (!elem) continue;
            out.kind = LiveControlKind::TextAreaResize;
            out.elem = elem;
            out.block_idx = idx;
            out.bounds = block_border_visual_rect(impl, idx);
            out.start_w = out.bounds.w;
            out.start_h = out.bounds.h;
            out.resize_x = resize_x;
            out.resize_y = resize_y;
            return true;
        }

        const auto kind = live_control_kind_for_block(block);
        if (kind == LiveControlKind::None || block.is_disabled) continue;

        auto* elem = element_for_block(impl, idx);
        if (!elem) continue;
        out.kind = kind;
        out.elem = elem;
        out.block_idx = idx;
        out.bounds = block_border_visual_rect(impl, idx);
        auto* combo = kind == LiveControlKind::NumericInput
            ? nearest_ancestor_with_class(elem, "dcs-combo")
            : nullptr;
        if (kind == LiveControlKind::NumericInput && !combo &&
            has_attr(elem, "data-dcs-combo")) {
            combo = elem;
        }
        const bool has_min_attr =
            has_attr(elem, "min") || has_attr(elem, "data-min") ||
            (combo && has_attr(combo, "data-min"));
        const bool has_max_attr =
            has_attr(elem, "max") || has_attr(elem, "data-max") ||
            (combo && has_attr(combo, "data-max"));
        const bool has_fill_min_attr =
            has_attr(elem, "data-fill-min") ||
            (combo && has_attr(combo, "data-fill-min"));
        const bool has_fill_max_attr =
            has_attr(elem, "data-fill-max") ||
            (combo && has_attr(combo, "data-fill-max"));
        out.min = element_attr_double(
            combo, "data-min",
            element_attr_double(elem, "min",
                element_attr_double(elem, "data-min", 0.0)));
        out.max = element_attr_double(
            combo, "data-max",
            element_attr_double(elem, "max",
                element_attr_double(elem, "data-max", 1.0)));
        if (has_fill_min_attr) {
            out.min = element_attr_double(
                combo, "data-fill-min",
                element_attr_double(elem, "data-fill-min", out.min));
        }
        if (has_fill_max_attr) {
            out.max = element_attr_double(
                combo, "data-fill-max",
                element_attr_double(elem, "data-fill-max", out.max));
        }
        out.start_value = element_attr_double(
            elem, "value", element_attr_double(elem, "data-value", out.min));
        if (kind == LiveControlKind::NumericInput) {
            out.bounded = combo != nullptr &&
                (has_min_attr || has_max_attr ||
                 has_fill_min_attr || has_fill_max_attr);
            if (combo != nullptr) {
                const int combo_idx = block_index_for_exact_element(impl, combo);
                if (combo_idx >= 0) {
                    out.bounds = block_border_visual_rect(impl, combo_idx);
                }
            }
            if (!has_min_attr && !has_fill_min_attr) {
                out.min = out.start_value - 100000.0;
            }
            if (!has_max_attr && !has_fill_max_attr) {
                out.max = out.start_value + 100000.0;
            }
            out.step = element_attr_double(
                elem, "step",
                element_attr_double(
                    elem, "data-step",
                    element_attr_double(combo, "data-step", 0.01)));
            if (out.step <= 0.0) out.step = 0.01;
            out.last_x = point.x;
        }
        if (out.max <= out.min) out.max = out.min + 1.0;
        out.bipolar = has_attr(elem, "data-bipolar");
        return true;
    }
    return false;
}

bool update_textarea_resize(detail::DocumentImpl& impl,
                            detail::DocumentImpl::LiveControlDrag& drag,
                            const Event& ev) {
    if (!drag.elem || drag.block_idx < 0 ||
        drag.block_idx >= static_cast<int>(impl.blocks.size())) {
        return false;
    }
    auto* node = lxb_dom_interface_node(drag.elem);
    auto& size = impl.user_textarea_sizes[node];
    const int next_w = drag.resize_x
        ? std::max(1, drag.start_w + (ev.pos.x - drag.start_x))
        : (size.width > 0 ? size.width : drag.start_w);
    const int next_h = drag.resize_y
        ? std::max(1, drag.start_h + (ev.pos.y - drag.start_y))
        : (size.height > 0 ? size.height : drag.start_h);
    if (next_w == size.width && next_h == size.height) return false;

    const Rect old_rect = subtree_visual_rect(impl, drag.block_idx);
    size.width = next_w;
    size.height = next_h;
    auto& block = impl.blocks[static_cast<std::size_t>(drag.block_idx)];
    auto& cs = impl.style_store.computed(block.id);
    if (drag.resize_x) {
        cs.width = static_cast<std::int16_t>(std::min(next_w, 32767));
        cs.width_pct_x100 = -1;
    }
    if (drag.resize_y) {
        cs.height = static_cast<std::int16_t>(std::min(next_h, 32767));
        cs.height_pct = -1;
    }
    mark_live_mutation_dirty(impl, drag.block_idx, old_rect,
                             /*needs_layout=*/true);
    return true;
}

bool update_active_live_control(detail::DocumentImpl& impl, const Event& ev) {
    auto& drag = impl.live_drag;
    if (drag.kind == LiveControlKind::None || !drag.elem) return false;

    if (!drag.moved && pointer_moved_past_threshold(ev, drag)) {
        drag.moved = true;
    }
    if (drag.kind == LiveControlKind::NumericInput && !drag.moved) {
        return false;
    }
    if (drag.kind == LiveControlKind::TextAreaResize) {
        if (!drag.moved) return false;
        return update_textarea_resize(impl, drag, ev);
    }

    double value = drag.start_value;
    if (drag.kind == LiveControlKind::RangeInput ||
        drag.kind == LiveControlKind::DeciusSlider) {
        value = value_from_x(drag.bounds, ev.pos.x, drag.min, drag.max);
    } else if (drag.kind == LiveControlKind::NumericInput) {
        if (drag.bounded) {
            value = value_from_x(drag.bounds, ev.pos.x, drag.min, drag.max);
        } else {
            const double mult = ev.shift ? 4.0 : 1.0;
            const double current = element_attr_double(
                drag.elem, "value",
                element_attr_double(drag.elem, "data-value", drag.start_value));
            const double scaled_step =
                std::max(drag.step, std::abs(current) / 100.0);
            value = current +
                    static_cast<double>(ev.pos.x - drag.last_x) *
                        scaled_step * mult;
            drag.last_x = ev.pos.x;
        }
    } else if (drag.kind == LiveControlKind::DeciusFader) {
        value = value_from_y(drag.bounds, ev.pos.y, drag.min, drag.max);
    } else if (drag.kind == LiveControlKind::AuiKnob ||
               drag.kind == LiveControlKind::DeciusKnob) {
        value = drag.start_value +
                (static_cast<double>(drag.start_y - ev.pos.y) / 150.0) *
                    (drag.max - drag.min);
    }
    return update_live_control_value(impl, drag.elem, drag.kind, drag.min,
                                     drag.max, value, drag.bipolar);
}

bool is_checkbox_like_block(const Block& block) {
    if (block.tag == "input" &&
        (block.input_type == "checkbox" || block.input_type == "radio")) {
        return true;
    }
    if (block_attr_value(block, "data-aui-widget") &&
        *block_attr_value(block, "data-aui-widget") == "checkbox") {
        return true;
    }
    return block_has_class(block, "dcs-check") ||
           block_has_class(block, "dcs-radio") ||
           block_has_class(block, "dcs-switch");
}

bool find_checkbox_control_at(detail::DocumentImpl& impl,
                              int from_idx,
                              int& out_idx,
                              lxb_dom_element_t*& out_elem) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        if (!is_checkbox_like_block(block) || block.is_disabled) continue;
        auto* elem = element_for_block(impl, idx);
        if (!elem) continue;
        out_idx = idx;
        out_elem = elem;
        return true;
    }
    return false;
}

bool class_list_contains(lxb_dom_element_t* elem, std::string_view cls);

std::string radio_group_name(lxb_dom_element_t* elem,
                             lxb_dom_element_t* input) {
    if (input) {
        auto name = attr_string(input, "name");
        if (!name.empty()) return name;
    }
    auto name = attr_string(elem, "data-dcs-name");
    if (!name.empty()) return name;
    return attr_string(elem, "name");
}

bool radio_peer_matches(lxb_dom_element_t* peer,
                        lxb_dom_element_t* current,
                        std::string_view group_name) {
    if (!peer || peer == current) return false;
    const bool peer_radio = class_list_contains(peer, "dcs-radio") ||
                            (tag_name(peer) == "input" &&
                             attr_string(peer, "type") == "radio");
    if (!peer_radio) return false;
    if (!group_name.empty()) {
        return radio_group_name(peer,
                                tag_name(peer) == "input" ? peer : nullptr) ==
               group_name;
    }
    return parent_element(peer) == parent_element(current);
}

bool uncheck_radio_peers(detail::DocumentImpl& impl,
                         lxb_dom_element_t* current,
                         std::string_view group_name) {
    auto* root = parent_element(current);
    if (!root) return false;
    bool changed = false;
    auto walk = [&](auto& self, lxb_dom_element_t* elem) -> void {
        if (!elem) return;
        if (radio_peer_matches(elem, current, group_name)) {
            if (tag_name(elem) == "input") {
                changed = remove_attribute_on_element(
                    impl, elem, "checked") || changed;
            } else {
                changed = set_attribute_on_element(
                    impl, elem, "aria-checked", "false") || changed;
            }
        }
        for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
             child != nullptr; child = lxb_dom_node_next(child)) {
            if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            self(self, lxb_dom_interface_element(child));
        }
    };
    walk(walk, root);
    return changed;
}

bool toggle_checkbox_control(detail::DocumentImpl& impl, int idx,
                             lxb_dom_element_t* elem) {
    if (!elem) return false;
    const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    lxb_dom_element_t* input =
        block.tag == "input" ? elem : first_descendant_input(elem);
    lxb_dom_element_t* visual_check =
        first_descendant_with_class(elem, "dcs-check__box");
    if (!visual_check) {
        visual_check = block_has_class(block, "dcs-check") ||
                       block_has_class(block, "dcs-radio")
            ? elem
            : first_descendant_with_class(elem, "dcs-check");
    }
    const bool radio = (input && attr_string(input, "type") == "radio") ||
                       block_has_class(block, "dcs-radio") ||
                       class_list_contains(elem, "dcs-radio");
    const bool old_checked = input
        ? has_attr(input, "checked")
        : element_attr_true(elem, "aria-checked");
    const bool checked = radio ? true : !old_checked;

    bool changed = false;
    if (radio && checked) {
        changed = uncheck_radio_peers(
            impl, elem, radio_group_name(elem, input)) || changed;
    }
    if (input) {
        changed = checked
            ? (set_attribute_on_element(impl, input, "checked", "checked") || changed)
            : (remove_attribute_on_element(impl, input, "checked") || changed);
    }
    if (input != elem || has_attr(elem, "aria-checked") ||
        block_has_class(block, "dcs-check") ||
        block_has_class(block, "dcs-radio") ||
        block_has_class(block, "dcs-switch") ||
        block_attr_value(block, "data-aui-widget")) {
        changed = set_attribute_on_element(
            impl, elem, "aria-checked", checked ? "true" : "false") ||
            changed;
    }
    if (visual_check != nullptr && visual_check != elem) {
        changed = set_attribute_on_element(
            impl, visual_check, "aria-checked",
            checked ? "true" : "false") || changed;
    }
    auto* wrapper = nearest_checkbox_wrapper(elem);
    if (wrapper != nullptr && wrapper != elem &&
        attr_string(wrapper, "data-aui-widget") == "checkbox") {
        changed = checked
            ? (set_attribute_on_element(impl, wrapper, "aria-checked", "true") || changed)
            : (remove_attribute_on_element(impl, wrapper, "aria-checked") || changed);
    }
    if (changed) {
        emit_widget_change(impl, wrapper, checked ? "true" : "false");
    }
    return changed;
}

bool is_button_like_block(const Block& block) {
    if (block.tag == "button") return true;
    if (const auto* role = block_attr_value(block, "role");
        role && *role == "button") {
        return true;
    }
    if (const auto* widget = block_attr_value(block, "data-aui-widget");
        widget && *widget == "button") {
        return true;
    }
    return false;
}

bool find_button_control_at(detail::DocumentImpl& impl,
                            int from_idx,
                            lxb_dom_element_t*& out_elem) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        if (!is_button_like_block(block) || block.is_disabled) continue;
        auto* elem = element_for_block(impl, idx);
        if (!elem) continue;
        out_elem = elem;
        return true;
    }
    return false;
}

std::string activation_name(lxb_dom_element_t* elem) {
    if (!elem) return {};
    if (auto name = attr_string(elem, "data-aui-name"); !name.empty()) {
        return name;
    }
    return attr_string(elem, "id");
}

bool activate_button_control(detail::DocumentImpl& impl,
                             lxb_dom_element_t* elem) {
    auto name = activation_name(elem);
    if (name.empty()) return false;
    impl.activated_widgets.push_back(std::move(name));
    return true;
}

bool find_button_group_option_at(detail::DocumentImpl& impl,
                                 int from_idx,
                                 lxb_dom_element_t*& out_group,
                                 lxb_dom_element_t*& out_option) {
    out_group = nullptr;
    out_option = nullptr;
    lxb_dom_element_t* decius_group = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        auto* elem = element_for_block(impl, idx);
        if (!elem) continue;
        if (!out_option && block.tag == "button" && !block.is_disabled &&
            (block_has_attr(block, "value") ||
             block_has_attr(block, "data-dcs-value") ||
             block_has_class(block, "dcs-btn"))) {
            out_option = elem;
        }
        const auto* widget = block_attr_value(block, "data-aui-widget");
        if (widget && *widget == "button-group") {
            out_group = elem;
            return out_option != nullptr;
        }
        if (!decius_group && block_has_class(block, "dcs-btn-group")) {
            decius_group = elem;
        }
    }
    if (decius_group && out_option) {
        // A bare .dcs-btn-group can be either a segmented selector or just
        // a visual grouping for independently-bound buttons. Generated
        // button_group widgets mark the outer field with data-aui-widget;
        // otherwise a named child button should keep its own on_click path.
        if (has_attr(out_option, "data-aui-name") &&
            attr_string(decius_group, "data-aui-widget") != "button-group") {
            out_group = nullptr;
            out_option = nullptr;
            return false;
        }
        out_group = decius_group;
        return true;
    }
    out_group = nullptr;
    out_option = nullptr;
    return false;
}

bool class_list_contains(lxb_dom_element_t* elem, std::string_view cls) {
    const auto classes = split_classes(attr_string(elem, "class"));
    return std::find(classes.begin(), classes.end(), cls) != classes.end();
}

std::string class_list_set(lxb_dom_element_t* elem,
                           std::string_view cls,
                           bool present) {
    auto classes = split_classes(attr_string(elem, "class"));
    auto it = std::find(classes.begin(), classes.end(), cls);
    if (present && it == classes.end()) {
        classes.emplace_back(cls);
    } else if (!present && it != classes.end()) {
        classes.erase(it);
    }
    std::string out;
    for (const auto& c : classes) {
        if (!out.empty()) out.push_back(' ');
        out += c;
    }
    return out;
}

// Add/remove a class on a live element (re-matches selectors, restyles).
bool set_element_class(detail::DocumentImpl& impl,
                       lxb_dom_element_t* elem,
                       std::string_view cls,
                       bool present) {
    if (!elem) return false;
    if (class_list_contains(elem, cls) == present) return false;
    return set_attribute_on_element(impl, elem, "class",
                                    class_list_set(elem, cls, present));
}

std::string button_group_option_value(lxb_dom_element_t* elem) {
    if (!elem) return {};
    auto value = attr_string(elem, "value");
    if (!value.empty()) return value;
    value = attr_string(elem, "data-dcs-value");
    if (!value.empty()) return value;
    value = node_text(lxb_dom_interface_node(elem));
    return std::string(trim_css_ws(value));
}

bool update_button_group_option_states(detail::DocumentImpl& impl,
                                       lxb_dom_element_t* elem,
                                       std::string_view selected) {
    if (!elem) return false;
    bool changed = false;
    if (tag_name(elem) == "button" &&
        (has_attr(elem, "value") ||
         has_attr(elem, "data-dcs-value") ||
         class_list_contains(elem, "dcs-btn"))) {
        const bool active = button_group_option_value(elem) == selected;
        changed = set_attribute_on_element(
            impl, elem, "aria-pressed", active ? "true" : "false") ||
            changed;
        if (class_list_contains(elem, "btn")) {
            changed = set_attribute_on_element(
                impl, elem, "class",
                active ? "btn btn-primary" : "btn btn-outline-primary") || changed;
        }
        if (class_list_contains(elem, "dcs-btn")) {
            changed = set_attribute_on_element(
                impl, elem, "class",
                class_list_set(elem, "dcs-btn--primary", active)) || changed;
        }
    }
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        changed = update_button_group_option_states(
            impl, lxb_dom_interface_element(child), selected) || changed;
    }
    return changed;
}

bool update_button_group_control(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* group,
                                 lxb_dom_element_t* option) {
    if (!group || !option) return false;
    const auto selected = button_group_option_value(option);
    if (selected.empty()) return false;
    bool changed =
        set_attribute_on_element(impl, group, "data-value", selected);
    changed = update_button_group_option_states(impl, group, selected) || changed;
    if (changed) emit_widget_change(impl, group, selected);
    return changed;
}

bool find_dropdown_control_at(detail::DocumentImpl& impl,
                              int from_idx,
                              lxb_dom_element_t*& out_group,
                              lxb_dom_element_t*& out_select,
                              lxb_dom_element_t*& out_option) {
    out_group = nullptr;
    out_select = nullptr;
    out_option = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
        auto* elem = element_for_block(impl, idx);
        if (!elem) continue;
        if (!out_option && block.tag == "button" &&
            block_has_attr(block, "value") &&
            block_attr_value(block, "role") &&
            *block_attr_value(block, "role") == "option" &&
            !block.is_disabled) {
            out_option = elem;
        }
        if (!out_select && block.tag == "select" && !block.is_disabled) {
            out_select = elem;
        }
        const auto* widget = block_attr_value(block, "data-aui-widget");
        if (widget && *widget == "dropdown") {
            out_group = elem;
            return out_select != nullptr || out_option != nullptr;
        }
    }
    out_group = nullptr;
    out_select = nullptr;
    out_option = nullptr;
    return false;
}

bool update_dropdown_selection_states(detail::DocumentImpl& impl,
                                      lxb_dom_element_t* elem,
                                      std::string_view selected) {
    if (!elem) return false;
    bool changed = false;
    const auto tag = tag_name(elem);
    if (tag == "select") {
        changed =
            set_attribute_on_element(impl, elem, "value", selected) || changed;
    } else if (tag == "option" && has_attr(elem, "value")) {
        const bool active = attr_string(elem, "value") == selected;
        changed = active
            ? (set_attribute_on_element(impl, elem, "selected", "selected") || changed)
            : (remove_attribute_on_element(impl, elem, "selected") || changed);
    } else if (tag == "button" && has_attr(elem, "value") &&
               attr_string(elem, "role") == "option") {
        const bool active = attr_string(elem, "value") == selected;
        changed = active
            ? (set_attribute_on_element(impl, elem, "aria-selected", "true") || changed)
            : (remove_attribute_on_element(impl, elem, "aria-selected") || changed);
        if (class_list_contains(elem, "dropdown-item")) {
            changed = set_attribute_on_element(
                impl, elem, "class", class_list_set(elem, "active", active)) || changed;
        }
        if (class_list_contains(elem, "dcs-menu__item")) {
            changed = set_attribute_on_element(
                impl, elem, "class",
                class_list_set(elem, "dcs-menu__item--active", active)) || changed;
        }
    }
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        changed = update_dropdown_selection_states(
            impl, lxb_dom_interface_element(child), selected) || changed;
    }
    return changed;
}

bool close_transient_layers(detail::DocumentImpl& impl,
                            lxb_dom_element_t* except = nullptr);

int viewport_width_for_overlay(const detail::DocumentImpl& impl) {
    if (impl.media_viewport_width_px > 0) return impl.media_viewport_width_px;
    return std::max(1, impl.content_size.width);
}

int viewport_height_for_overlay(const detail::DocumentImpl& impl) {
    if (impl.media_viewport_height_px > 0) return impl.media_viewport_height_px;
    return std::max(1, impl.content_size.height);
}

int overlay_item_count(lxb_dom_element_t* elem) {
    if (!elem) return 0;
    int count = 0;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* child_elem = lxb_dom_interface_element(child);
        if (class_list_contains(child_elem, "dcs-menu__item") ||
            class_list_contains(child_elem, "dropdown-item") ||
            attr_string(child_elem, "role") == "option" ||
            attr_string(child_elem, "role") == "menuitem") {
            ++count;
        }
        count += overlay_item_count(child_elem);
    }
    return count;
}

bool element_has_direct_text(lxb_dom_element_t* elem) {
    if (!elem) return false;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_TEXT) continue;
        if (!node_text(child).empty()) return true;
    }
    return false;
}

int computed_border_padding_height(const detail::ComputedStyle& cs) {
    return cs.padding_top + cs.padding_bottom +
           cs.used_border_top() + cs.used_border_bottom();
}

int computed_outer_declared_height(const detail::ComputedStyle& cs) {
    if (cs.height <= 0) return 0;
    int h = cs.height;
    if (cs.box_sizing == detail::ComputedStyle::BoxSizing::ContentBox) {
        h += computed_border_padding_height(cs);
    }
    return std::max(1, h);
}

int estimate_hidden_overlay_height_from_css(const detail::DocumentImpl& impl,
                                            lxb_dom_element_t* elem,
                                            int depth = 0) {
    if (!elem || !impl.resolver || depth > 8) return 0;

    auto rs = impl.resolver->resolve(elem, impl.root_style);
    const auto& cs = rs.computed;
    if (depth > 0 &&
        cs.display == detail::ComputedStyle::Display::None) {
        return 0;
    }

    if (const int declared = computed_outer_declared_height(cs); declared > 0) {
        return declared;
    }

    int children_h = 0;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* child_elem = lxb_dom_interface_element(child);
        auto child_rs = impl.resolver->resolve(child_elem, impl.root_style);
        const auto& child_cs = child_rs.computed;
        if (child_cs.position == detail::ComputedStyle::Position::Absolute ||
            child_cs.position == detail::ComputedStyle::Position::Fixed) {
            continue;
        }
        children_h += estimate_hidden_overlay_height_from_css(
            impl, child_elem, depth + 1);
    }

    int content_h = children_h;
    if (content_h <= 0 && element_has_direct_text(elem)) {
        content_h = std::max(
            1,
            static_cast<int>(std::ceil(
                static_cast<float>(cs.font_size_px) *
                detail::effective_line_height_mult(cs))));
    }

    if (content_h <= 0) return 0;
    return std::max(1, content_h + computed_border_padding_height(cs));
}

int overlay_estimated_height(const detail::DocumentImpl& impl,
                             lxb_dom_element_t* elem,
                             int fallback) {
    const int idx = block_index_for_exact_element(impl, elem);
    if (idx >= 0) {
        const Rect rect = block_border_visual_rect(impl, idx);
        if (rect.h > 0) return rect.h;
    }
    if (const int css_estimate =
            estimate_hidden_overlay_height_from_css(impl, elem);
        css_estimate > 0) {
        return std::clamp(css_estimate, 1, 240);
    }
    const int count = overlay_item_count(elem);
    if (count > 0) return std::clamp(count * 24 + 8, 24, 240);
    return std::max(1, fallback);
}

int overlay_declared_outer_height(const detail::DocumentImpl& impl,
                                  lxb_dom_element_t* elem,
                                  int fallback) {
    if (!elem || !impl.resolver) return overlay_estimated_height(impl, elem, fallback);

    auto rs = impl.resolver->resolve(elem, impl.root_style);
    const auto& cs = rs.computed;
    if (const int declared = computed_outer_declared_height(cs);
        declared > 0) {
        return declared;
    }
    return overlay_estimated_height(impl, elem, fallback);
}

struct OverlayPlacement {
    int left{8};
    int top{8};
    int max_height{1};
    std::string side{"bottom"};
};

OverlayPlacement place_anchored_overlay(const detail::DocumentImpl& impl,
                                        Rect anchor,
                                        int overlay_width,
                                        int overlay_height,
                                        std::string_view placement,
                                        int gap) {
    constexpr int edge = 8;
    const int viewport_w = viewport_width_for_overlay(impl);
    const int viewport_h = viewport_height_for_overlay(impl);
    overlay_width = std::clamp(std::max(1, overlay_width), 1,
                               std::max(1, viewport_w - edge * 2));
    const int desired_height =
        std::clamp(std::max(1, overlay_height), 1,
                   std::max(1, viewport_h - edge * 2));

    std::string side{"bottom"};
    if (placement.rfind("top", 0) == 0) side = "top";
    else if (placement.rfind("left", 0) == 0) side = "left";
    else if (placement.rfind("right", 0) == 0) side = "right";
    const bool end_aligned = placement.find("end") != std::string_view::npos;

    const int above = anchor.y - edge;
    const int below = viewport_h - (anchor.y + anchor.h) - edge;
    const int left_space = anchor.x - edge;
    const int right_space = viewport_w - (anchor.x + anchor.w) - edge;
    if (side == "bottom" && below < desired_height + gap && above > below) {
        side = "top";
    } else if (side == "top" && above < desired_height + gap && below > above) {
        side = "bottom";
    } else if (side == "right" &&
               right_space < overlay_width + gap && left_space > right_space) {
        side = "left";
    } else if (side == "left" &&
               left_space < overlay_width + gap && right_space > left_space) {
        side = "right";
    }

    int available_height = std::max(1, viewport_h - edge * 2);
    if (side == "bottom") {
        available_height = std::max(1, below);
    } else if (side == "top") {
        available_height = std::max(1, above);
    }
    overlay_height = std::min(desired_height, available_height);

    int left = anchor.x;
    int top = anchor.y + anchor.h + gap;
    if (side == "top") {
        top = anchor.y - overlay_height;
        left = end_aligned ? anchor.x + anchor.w - overlay_width : anchor.x;
    } else if (side == "bottom") {
        top = anchor.y + anchor.h + gap;
        left = end_aligned ? anchor.x + anchor.w - overlay_width : anchor.x;
    } else if (side == "left") {
        left = anchor.x - overlay_width - gap;
        top = anchor.y;
    } else {
        left = anchor.x + anchor.w + gap;
        top = anchor.y;
    }

    const int max_left =
        std::max(edge, viewport_w - overlay_width - edge);
    const int max_top = std::max(edge, viewport_h - overlay_height - edge);
    OverlayPlacement out;
    out.left = std::clamp(left, edge, max_left);
    out.top = std::clamp(top, edge, max_top);
    out.max_height = overlay_height;
    out.side = std::move(side);
    return out;
}

bool hide_dropdown_menu(detail::DocumentImpl& impl, lxb_dom_element_t* group) {
    auto* menu = first_descendant_with_class(group, "aui-select__menu");
    if (!menu) return false;
    bool changed = set_attribute_on_element(impl, menu, "hidden", "");
    changed = remove_attribute_on_element(impl, menu, "style") || changed;
    return changed;
}

std::string dropdown_menu_open_style(const detail::DocumentImpl& impl,
                                     lxb_dom_element_t* group,
                                     lxb_dom_element_t* menu) {
    int width = 160;
    Rect anchor_rect{0, 0, width, 1};
    auto* anchor = first_descendant_tag(group, "select");
    if (!anchor) anchor = group;
    const int anchor_idx = block_index_for_element_or_ancestor(impl, anchor);
    if (anchor_idx >= 0) {
        anchor_rect = block_border_visual_rect(impl, anchor_idx);
        width = std::max(1, anchor_rect.w);
    }
    const auto placed = place_anchored_overlay(
        impl, anchor_rect, width,
        overlay_declared_outer_height(impl, menu, 160),
        "bottom", 0);
    return "display:flex;position:fixed;left:" + std::to_string(placed.left) +
           "px;top:" + std::to_string(placed.top) +
           "px;width:" + std::to_string(width) +
           "px;min-width:" + std::to_string(width) +
           "px;max-width:" + std::to_string(width) +
           "px;max-height:" + std::to_string(placed.max_height) +
           "px;overflow:auto;flex-direction:column;align-items:stretch;z-index:400";
}

bool toggle_dropdown_menu(detail::DocumentImpl& impl, lxb_dom_element_t* group) {
    auto* menu = first_descendant_with_class(group, "aui-select__menu");
    if (!menu) return false;
    if (!has_attr(menu, "hidden")) {
        return hide_dropdown_menu(impl, group);
    }
    const std::string open_style = dropdown_menu_open_style(impl, group, menu);
    bool changed = close_transient_layers(impl, menu);
    changed = remove_attribute_on_element(impl, menu, "hidden") || changed;
    changed = set_attribute_on_element(
        impl, menu, "style", open_style) ||
        changed;
    return changed;
}

bool update_dropdown_control(detail::DocumentImpl& impl,
                             lxb_dom_element_t* group,
                             lxb_dom_element_t* option) {
    if (!group || !option) return false;
    const auto selected = attr_string(option, "value");
    if (selected.empty()) return false;
    bool changed =
        set_attribute_on_element(impl, group, "data-value", selected);
    changed = update_dropdown_selection_states(impl, group, selected) || changed;
    changed = hide_dropdown_menu(impl, group) || changed;
    if (changed) emit_widget_change(impl, group, selected);
    return changed;
}

template <class Fn>
void walk_dom_elements(lxb_dom_node_t* node, Fn& fn) {
    if (!node) return;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        fn(lxb_dom_interface_element(node));
    }
    for (auto* child = lxb_dom_node_first_child(node);
         child != nullptr; child = lxb_dom_node_next(child)) {
        walk_dom_elements(child, fn);
    }
}

lxb_dom_node_t* document_dom_root(detail::DocumentImpl& impl) {
    if (!impl.doc) return nullptr;
    auto* body = lxb_html_document_body_element(impl.doc);
    return body ? lxb_dom_interface_node(body) : lxb_dom_interface_node(impl.doc);
}

bool close_dropdown_menu_element(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* menu) {
    if (!menu) return false;
    bool changed = set_attribute_on_element(impl, menu, "hidden", "");
    changed = remove_attribute_on_element(impl, menu, "style") || changed;
    return changed;
}

bool close_all_dropdown_menus(detail::DocumentImpl& impl,
                              lxb_dom_element_t* except = nullptr) {
    std::vector<lxb_dom_element_t*> menus;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (elem != except && class_list_contains(elem, "aui-select__menu") &&
            !has_attr(elem, "hidden")) {
            menus.push_back(elem);
        }
    };
    walk_dom_elements(document_dom_root(impl), collect);

    bool changed = false;
    for (auto* menu : menus) {
        changed = close_dropdown_menu_element(impl, menu) || changed;
    }
    return changed;
}

std::string target_id_from_selector(std::string_view selector) {
    selector = trim_css_ws(selector);
    if (selector.empty() || selector.front() != '#') return {};
    selector.remove_prefix(1);
    selector = trim_css_ws(selector);
    std::size_t end = 0;
    while (end < selector.size() &&
           !std::isspace(static_cast<unsigned char>(selector[end]))) {
        ++end;
    }
    return std::string(selector.substr(0, end));
}

lxb_dom_element_t* dcs_target_for_trigger(detail::DocumentImpl& impl,
                                          lxb_dom_element_t* trigger) {
    if (!trigger) return nullptr;
    auto selector = attr_string(trigger, "data-dcs-target");
    if (selector.empty()) selector = attr_string(trigger, "href");
    const auto target_id = target_id_from_selector(selector);
    return target_id.empty() ? nullptr : find_dom_element_by_id(impl, target_id);
}

// ── Floating element drag (data-dcs-drag + data-dcs-drag-handle) ─────────────
// A [data-dcs-drag] container (a floating toolbar or torn-off panel) is moved
// by dragging a [data-dcs-drag-handle] inside it. Movement writes inline
// left/top and is clamped to the [data-dcs-drag-bounds] container. Mirrors
// decius drag; position persists via result.layout_changed like the splitter.
// Rebuild an inline style with left/top set and right/bottom dropped (so the
// written left/top win), preserving every other declaration.
std::string with_float_position(std::string_view style, int left, int top) {
    std::string out;
    std::size_t i = 0;
    while (i < style.size()) {
        const std::size_t semi = style.find(';', i);
        const std::string_view decl = style.substr(
            i, semi == std::string_view::npos ? semi : semi - i);
        i = (semi == std::string_view::npos) ? style.size() : semi + 1;
        const std::size_t colon = decl.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view prop = trim_css_ws(decl.substr(0, colon));
            if (prop == "left" || prop == "top" || prop == "right" ||
                prop == "bottom")
                continue;
        }
        const std::string_view t = trim_css_ws(decl);
        if (!t.empty()) {
            if (!out.empty()) out += ';';
            out += t;
        }
    }
    if (!out.empty()) out += ';';
    out += "left:" + std::to_string(left) + "px;top:" + std::to_string(top) +
           "px";
    return out;
}

// The [data-dcs-drag-bounds] container for `dragged`: the selector (.class or
// #id) is matched against the float's ancestors (the bounds box always encloses
// the float). Returns null if there is no selector / no match.
lxb_dom_element_t* resolve_drag_bounds_elem(detail::DocumentImpl& impl,
                                            lxb_dom_element_t* dragged,
                                            std::string_view selector) {
    (void) impl;
    if (!dragged || selector.empty()) return nullptr;
    const bool by_id = selector.front() == '#';
    const std::string name(
        (by_id || selector.front() == '.') ? selector.substr(1) : selector);
    for (auto* n = lxb_dom_node_parent(lxb_dom_interface_node(dragged)); n;
         n = lxb_dom_node_parent(n)) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(n);
        const bool m = by_id ? (attr_string(e, "id") == name)
                             : class_list_contains(e, name);
        if (m) return e;
    }
    return nullptr;
}

Rect root_float_host_bounds(detail::DocumentImpl& impl) {
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* e = element_for_block(impl, i);
        if (e && class_list_contains(e, "dcs-dock--floathost")) {
            return impl.blocks[static_cast<std::size_t>(i)].bounds;
        }
    }
    return {};
}

Rect document_float_host_bounds(detail::DocumentImpl& impl) {
    Rect fallback = root_float_host_bounds(impl);
    Rect best{};
    long long best_area = 0;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* e = element_for_block(impl, i);
        if (!e || !has_attr(e, "data-dcs-float-host")) continue;
        if (class_list_contains(e, "dcs-dock--floathost")) continue;
        const auto& b = impl.blocks[static_cast<std::size_t>(i)].bounds;
        if (b.w <= 0 || b.h <= 0) continue;
        const long long area = static_cast<long long>(b.w) * b.h;
        if (best_area == 0 || area < best_area) {
            best = b;
            best_area = area;
        }
    }
    return best_area > 0 ? best : fallback;
}

bool find_float_drag_at(detail::DocumentImpl& impl, int from_idx, Point point,
                        detail::DocumentImpl::FloatDrag& out) {
    bool have_handle = false;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = element_for_block(impl, idx);
        if (!elem) continue;
        // Match decius.js' gesture split: dragging a dock tab/title is a dock
        // operation, while dragging empty floating chrome moves the panel.
        if (class_list_contains(elem, "dcs-dockpane__tab") ||
            class_list_contains(elem, "dcs-dockpane__tab-close")) {
            return false;
        }
        const std::string tag = tag_name(elem);
        if (tag == "button" || tag == "a" || tag == "input" ||
            tag == "select" || tag == "textarea" || tag == "label" ||
            class_list_contains(elem, "dcs-btn") ||
            class_list_contains(elem, "dcs-select") ||
            class_list_contains(elem, "dcs-slider") ||
            class_list_contains(elem, "dcs-fader") ||
            class_list_contains(elem, "dcs-knob") ||
            class_list_contains(elem, "dcs-combo")) {
            return false;
        }
        if (has_attr(elem, "data-dcs-drag-handle")) have_handle = true;
        if (!has_attr(elem, "data-dcs-drag")) continue;
        // The draggable container. Require the press to have started on a handle
        // inside it, so the toolbar's own buttons still click rather than drag.
        if (!have_handle) return false;
        const int bidx = block_index_for_exact_element(impl, elem);
        if (bidx < 0) return false;
        const auto& blk = impl.blocks[static_cast<std::size_t>(bidx)];
        // The element's current left/top may come from the cascade (a class
        // rule) or a prior inline write, so read them from the COMPUTED style.
        // Deriving the containing-block origin as (doc pos - computed inset)
        // makes the drag math independent of where left/top was authored.
        const auto cs = impl.style_store.computed(blk.id);
        const int cur_left =
            (cs.inset_has.left && !cs.inset_has.left_pct) ? cs.inset_left : 0;
        const int cur_top =
            (cs.inset_has.top && !cs.inset_has.top_pct) ? cs.inset_top : 0;
        out = {};
        out.elem = elem;
        out.start_x = point.x;
        out.start_y = point.y;
        out.elem_doc_x = blk.bounds.x;
        out.elem_doc_y = blk.bounds.y;
        out.cb_x = blk.bounds.x - cur_left;
        out.cb_y = blk.bounds.y - cur_top;
        out.elem_w = blk.bounds.w;
        out.elem_h = blk.bounds.h;
        out.panel_id = attr_string(elem, "data-dcs-dock-id");
        if (auto* be = resolve_drag_bounds_elem(
                impl, elem, attr_string(elem, "data-dcs-drag-bounds"))) {
            const int beidx = block_index_for_exact_element(impl, be);
            if (beidx >= 0) {
                const auto& bb = impl.blocks[static_cast<std::size_t>(beidx)];
                out.bounds_x = bb.bounds.x;
                out.bounds_y = bb.bounds.y;
                out.bounds_w = bb.bounds.w;
                out.bounds_h = bb.bounds.h;
            }
        }
        if (!out.panel_id.empty()) {
            const Rect hb = document_float_host_bounds(impl);
            if (hb.w > 0 && hb.h > 0) {
                out.bounds_x = hb.x;
                out.bounds_y = hb.y;
                out.bounds_w = hb.w;
                out.bounds_h = hb.h;
            }
        }
        return true;
    }
    return false;
}

bool update_float_drag(detail::DocumentImpl& impl, const Event& ev) {
    auto& d = impl.float_drag;
    if (!d.elem) return false;
    int x = d.elem_doc_x + (ev.pos.x - d.start_x);
    int y = d.elem_doc_y + (ev.pos.y - d.start_y);
    if (d.bounds_w > 0 && d.bounds_h > 0) {
        x = std::clamp(x, d.bounds_x,
                       std::max(d.bounds_x, d.bounds_x + d.bounds_w - d.elem_w));
        y = std::clamp(y, d.bounds_y,
                       std::max(d.bounds_y, d.bounds_y + d.bounds_h - d.elem_h));
    }
    return set_attribute_on_element(
        impl, d.elem, "style",
        with_float_position(attr_string(d.elem, "style"), x - d.cb_x,
                            y - d.cb_y));
}

bool is_dcs_menu_trigger(lxb_dom_element_t* elem) {
    return elem && attr_string(elem, "data-dcs-toggle") == "menu" &&
           !has_attr(elem, "disabled");
}

bool set_all_dcs_menu_triggers_expanded(detail::DocumentImpl& impl,
                                        std::string_view value) {
    std::vector<lxb_dom_element_t*> triggers;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (is_dcs_menu_trigger(elem)) triggers.push_back(elem);
    };
    walk_dom_elements(document_dom_root(impl), collect);

    bool changed = false;
    for (auto* trigger : triggers) {
        changed =
            set_attribute_on_element(impl, trigger, "aria-expanded", value) ||
            changed;
    }
    return changed;
}

bool close_dcs_menu(detail::DocumentImpl& impl, lxb_dom_element_t* menu) {
    if (!menu) return false;
    bool changed = set_attribute_on_element(impl, menu, "hidden", "");
    changed = remove_attribute_on_element(impl, menu, "style") || changed;
    return changed;
}

bool close_all_dcs_menus(detail::DocumentImpl& impl,
                         lxb_dom_element_t* except = nullptr) {
    std::vector<lxb_dom_element_t*> menus;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (elem != except && class_list_contains(elem, "dcs-menu") &&
            !has_attr(elem, "hidden")) {
            menus.push_back(elem);
        }
    };
    walk_dom_elements(document_dom_root(impl), collect);

    bool changed = false;
    for (auto* menu : menus) {
        changed = close_dcs_menu(impl, menu) || changed;
    }
    changed = set_all_dcs_menu_triggers_expanded(impl, "false") || changed;
    return changed;
}

std::string dcs_menu_open_style(const detail::DocumentImpl& impl,
                                lxb_dom_element_t* trigger,
                                lxb_dom_element_t* menu) {
    Rect anchor_rect{0, 0, 80, 1};
    int overlay_width = class_list_contains(menu, "aui-color-menu") ? 160 : 180;
    const bool stretch_to_anchor = class_list_contains(menu, "aui-color-menu");
    const int trigger_idx =
        block_index_for_element_or_ancestor(impl, trigger);
    if (trigger_idx >= 0) {
        anchor_rect = block_border_visual_rect(impl, trigger_idx);
        if (stretch_to_anchor) {
            overlay_width = std::max(1, anchor_rect.w);
        }
    }
    const auto placed = place_anchored_overlay(
        impl, anchor_rect, overlay_width,
        overlay_declared_outer_height(impl, menu, 160), "bottom", 0);
    std::string style = "display:flex;position:fixed;left:" +
        std::to_string(placed.left) + "px;top:" +
        std::to_string(placed.top) +
        "px;max-height:" + std::to_string(placed.max_height) +
        "px;overflow:auto;flex-direction:column;align-items:stretch;z-index:400";
    if (stretch_to_anchor) {
        style += ";width:" + std::to_string(overlay_width) +
                 "px;min-width:" + std::to_string(overlay_width) +
                 "px;max-width:" + std::to_string(overlay_width) + "px";
    }
    return style;
}

bool toggle_dcs_menu(detail::DocumentImpl& impl,
                     lxb_dom_element_t* trigger,
                     lxb_dom_element_t* menu) {
    if (!trigger || !menu) return false;
    if (!has_attr(menu, "hidden")) {
        bool changed = close_dcs_menu(impl, menu);
        changed =
            set_attribute_on_element(impl, trigger, "aria-expanded", "false") ||
            changed;
        return changed;
    }

    const std::string open_style = dcs_menu_open_style(impl, trigger, menu);
    bool changed = close_transient_layers(impl, menu);
    changed = remove_attribute_on_element(impl, menu, "hidden") || changed;
    changed =
        set_attribute_on_element(impl, menu, "style", open_style) ||
        changed;
    changed =
        set_attribute_on_element(impl, trigger, "aria-expanded", "true") ||
        changed;
    return changed;
}

bool is_dcs_popover_trigger(lxb_dom_element_t* elem) {
    return elem && attr_string(elem, "data-dcs-toggle") == "popover" &&
           !has_attr(elem, "disabled");
}

bool set_all_dcs_popover_triggers_expanded(detail::DocumentImpl& impl,
                                           std::string_view value) {
    std::vector<lxb_dom_element_t*> triggers;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (is_dcs_popover_trigger(elem)) triggers.push_back(elem);
    };
    walk_dom_elements(document_dom_root(impl), collect);

    bool changed = false;
    for (auto* trigger : triggers) {
        changed =
            set_attribute_on_element(impl, trigger, "aria-expanded", value) ||
            changed;
    }
    return changed;
}

bool close_dcs_popover(detail::DocumentImpl& impl, lxb_dom_element_t* popover) {
    if (!popover) return false;
    bool changed = set_attribute_on_element(impl, popover, "hidden", "");
    changed = remove_attribute_on_element(impl, popover, "style") || changed;
    return changed;
}

bool close_all_dcs_popovers(detail::DocumentImpl& impl,
                            lxb_dom_element_t* except = nullptr) {
    std::vector<lxb_dom_element_t*> popovers;
    auto collect = [&](lxb_dom_element_t* elem) {
        if (elem != except && class_list_contains(elem, "dcs-popover") &&
            !has_attr(elem, "hidden")) {
            popovers.push_back(elem);
        }
    };
    walk_dom_elements(document_dom_root(impl), collect);

    bool changed = false;
    for (auto* popover : popovers) {
        changed = close_dcs_popover(impl, popover) || changed;
    }
    changed = set_all_dcs_popover_triggers_expanded(impl, "false") || changed;
    return changed;
}

bool close_transient_layers(detail::DocumentImpl& impl,
                            lxb_dom_element_t* except) {
    bool changed = false;
    changed = close_all_dropdown_menus(impl, except) || changed;
    changed = close_all_dcs_menus(impl, except) || changed;
    changed = close_all_dcs_popovers(impl, except) || changed;
    return changed;
}

std::string dcs_popover_open_style(const detail::DocumentImpl& impl,
                                   lxb_dom_element_t* trigger,
                                   lxb_dom_element_t* popover) {
    int pop_w = 220;
    int pop_h = 64;
    const int popover_idx =
        block_index_for_element_or_ancestor(impl, popover);
    if (popover_idx >= 0) {
        const Rect pop_rect = block_border_visual_rect(impl, popover_idx);
        if (pop_rect.w > 0) pop_w = pop_rect.w;
        if (pop_rect.h > 0) pop_h = pop_rect.h;
    }

    Rect anchor_rect{0, 0, 1, 1};
    const int trigger_idx =
        block_index_for_element_or_ancestor(impl, trigger);
    if (trigger_idx >= 0) {
        anchor_rect = block_border_visual_rect(impl, trigger_idx);
    }

    std::string placement = attr_string(trigger, "data-dcs-placement");
    if (placement.empty()) placement = "bottom";
    const auto placed = place_anchored_overlay(
        impl, anchor_rect, pop_w,
        overlay_declared_outer_height(impl, popover, pop_h), placement, 6);
    return "display:flex;position:fixed;left:" +
           std::to_string(placed.left) +
           "px;top:" + std::to_string(placed.top) +
           "px;max-height:" + std::to_string(placed.max_height) +
           "px;overflow:auto;z-index:400";
}

bool toggle_dcs_popover(detail::DocumentImpl& impl,
                        lxb_dom_element_t* trigger,
                        lxb_dom_element_t* popover) {
    if (!trigger || !popover) return false;
    if (!has_attr(popover, "hidden")) {
        bool changed = close_dcs_popover(impl, popover);
        changed =
            set_attribute_on_element(impl, trigger, "aria-expanded", "false") ||
            changed;
        return changed;
    }

    const std::string open_style =
        dcs_popover_open_style(impl, trigger, popover);
    bool changed = close_transient_layers(impl, popover);
    changed = remove_attribute_on_element(impl, popover, "hidden") || changed;
    changed =
        set_attribute_on_element(impl, popover, "style", open_style) ||
        changed;
    changed =
        set_attribute_on_element(impl, trigger, "aria-expanded", "true") ||
        changed;
    return changed;
}

bool find_dcs_menu_trigger_at(detail::DocumentImpl& impl,
                              int from_idx,
                              lxb_dom_element_t*& out_trigger,
                              lxb_dom_element_t*& out_menu) {
    out_trigger = nullptr;
    out_menu = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = element_for_block(impl, idx);
        if (!is_dcs_menu_trigger(elem)) continue;
        auto* menu = dcs_target_for_trigger(impl, elem);
        if (!menu || !class_list_contains(menu, "dcs-menu")) continue;
        out_trigger = elem;
        out_menu = menu;
        return true;
    }
    return false;
}

bool find_dcs_popover_trigger_at(detail::DocumentImpl& impl,
                                 int from_idx,
                                 lxb_dom_element_t*& out_trigger,
                                 lxb_dom_element_t*& out_popover) {
    out_trigger = nullptr;
    out_popover = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = element_for_block(impl, idx);
        if (!is_dcs_popover_trigger(elem)) continue;
        auto* popover = dcs_target_for_trigger(impl, elem);
        if (!popover || !class_list_contains(popover, "dcs-popover")) continue;
        out_trigger = elem;
        out_popover = popover;
        return true;
    }
    return false;
}

bool is_disabled_dcs_menu_item(lxb_dom_element_t* elem) {
    return has_attr(elem, "disabled") ||
           attr_string(elem, "aria-disabled") == "true" ||
           class_list_contains(elem, "dcs-menu__item--disabled");
}

bool find_dcs_menu_item_at(detail::DocumentImpl& impl,
                           int from_idx,
                           lxb_dom_element_t*& out_menu,
                           lxb_dom_element_t*& out_item) {
    out_menu = nullptr;
    out_item = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = element_for_block(impl, idx);
        if (!elem) continue;
        if (!out_item && class_list_contains(elem, "dcs-menu__item")) {
            if (is_disabled_dcs_menu_item(elem)) return false;
            out_item = elem;
        }
        if (out_item && class_list_contains(elem, "dcs-menu")) {
            out_menu = elem;
            return true;
        }
    }
    out_item = nullptr;
    return false;
}

bool clear_pressed_dcs_menu_item(detail::DocumentImpl& impl) {
    auto* item = impl.pressed_dcs_menu_item;
    const bool was_active = impl.pressed_dcs_menu_item_was_active;
    impl.pressed_dcs_menu_item = nullptr;
    impl.pressed_dcs_menu_item_was_active = false;
    impl.pressed_dcs_menu_item_bounds = {};
    if (!item || was_active) return false;
    return set_element_class(impl, item, "dcs-menu__item--active", false);
}

bool press_dcs_menu_item(detail::DocumentImpl& impl,
                         lxb_dom_element_t* item) {
    bool changed = clear_pressed_dcs_menu_item(impl);
    if (!item) return changed;
    impl.pressed_dcs_menu_item = item;
    impl.pressed_dcs_menu_item_was_active =
        class_list_contains(item, "dcs-menu__item--active");
    if (const int idx = block_index_for_exact_element(impl, item); idx >= 0) {
        impl.pressed_dcs_menu_item_bounds =
            impl.blocks[static_cast<std::size_t>(idx)].bounds;
    } else {
        impl.pressed_dcs_menu_item_bounds = {};
    }
    changed = set_element_class(impl, item, "dcs-menu__item--active", true) ||
              changed;
    return changed;
}

bool activate_dcs_menu_item(detail::DocumentImpl& impl,
                            lxb_dom_element_t* menu,
                            lxb_dom_element_t* item) {
    if (!menu || !item) return false;
    if (has_attr(menu, "data-aui-colorfield") &&
        has_attr(item, "data-dcs-value")) {
        const auto value = attr_string(item, "data-dcs-value");
        auto* colorfield =
            find_dom_element_by_id(impl, attr_string(menu, "data-aui-colorfield"));
        bool changed = false;
        if (colorfield) {
            changed =
                set_attribute_on_element(impl, colorfield, "data-value", value) ||
                changed;
            if (auto* chip =
                    first_descendant_with_class(colorfield,
                                                "dcs-colorfield__chip")) {
                changed = set_attribute_on_element(
                    impl, chip, "style",
                    "--c:" + value + ";background:" + value) || changed;
            }
            if (auto* hex =
                    first_descendant_with_class(colorfield,
                                                "dcs-colorfield__hex")) {
                changed = set_text_on_element(impl, hex, value) || changed;
            }
            emit_widget_change(impl, colorfield, value);
        }
        changed = close_dcs_menu(impl, menu) || changed;
        changed = set_all_dcs_menu_triggers_expanded(impl, "false") || changed;
        return changed;
    }
    if (has_attr(item, "data-dcs-value")) {
        emit_widget_change(impl, menu, attr_string(item, "data-dcs-value"));
    }
    if (class_list_contains(item, "dcs-menu__item--has-sub")) return false;

    // A leaf item: fire the app's on_click (its activation name) before the
    // menu closes, so menu actions route like button activations.
    if (auto name = activation_name(item); !name.empty()) {
        impl.activated_widgets.push_back(std::move(name));
    }

    bool changed = close_dcs_menu(impl, menu);
    changed = set_all_dcs_menu_triggers_expanded(impl, "false") || changed;
    return changed;
}

bool is_dcs_select_row(lxb_dom_element_t* elem) {
    return elem && (class_list_contains(elem, "dcs-list__item") ||
                    class_list_contains(elem, "dcs-tree__row"));
}

bool is_disabled_dcs_select_row(lxb_dom_element_t* elem) {
    return has_attr(elem, "disabled") ||
           attr_string(elem, "aria-disabled") == "true" ||
           class_list_contains(elem, "dcs-list__item--disabled") ||
           class_list_contains(elem, "dcs-tree__row--disabled");
}

bool find_dcs_select_row_at(detail::DocumentImpl& impl,
                            int from_idx,
                            lxb_dom_element_t*& out_box,
                            lxb_dom_element_t*& out_row) {
    out_box = nullptr;
    out_row = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = element_for_block(impl, idx);
        if (!elem) continue;
        if (!out_row && is_dcs_select_row(elem)) {
            if (is_disabled_dcs_select_row(elem)) return false;
            out_row = elem;
        }
        if (out_row && has_attr(elem, "data-dcs-select")) {
            out_box = elem;
            return true;
        }
    }
    out_row = nullptr;
    return false;
}

void collect_dcs_select_rows(lxb_dom_element_t* elem,
                             std::vector<lxb_dom_element_t*>& rows) {
    if (!elem) return;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* child_elem = lxb_dom_interface_element(child);
        if (is_dcs_select_row(child_elem) &&
            !is_disabled_dcs_select_row(child_elem)) {
            rows.push_back(child_elem);
        }
        collect_dcs_select_rows(child_elem, rows);
    }
}

bool set_dcs_row_selected(detail::DocumentImpl& impl,
                          lxb_dom_element_t* row,
                          bool selected) {
    return set_attribute_on_element(
        impl, row, "aria-selected", selected ? "true" : "false");
}

std::string dcs_selected_rows_value(
    const std::vector<lxb_dom_element_t*>& rows) {
    std::string out;
    for (auto* row : rows) {
        if (attr_string(row, "aria-selected") != "true") continue;
        std::string value = attr_string(row, "data-dcs-value");
        if (value.empty()) value = attr_string(row, "id");
        if (value.empty()) continue;
        if (!out.empty()) out.push_back(',');
        out += value;
    }
    return out;
}

bool update_dcs_select_control(detail::DocumentImpl& impl,
                               lxb_dom_element_t* box,
                               lxb_dom_element_t* row,
                               const Event& ev) {
    if (!box || !row) return false;
    std::vector<lxb_dom_element_t*> rows;
    collect_dcs_select_rows(box, rows);
    if (rows.empty()) return false;

    const auto row_it = std::find(rows.begin(), rows.end(), row);
    if (row_it == rows.end()) return false;

    const bool multi = attr_string(box, "data-dcs-select") == "multi";
    bool changed = false;
    if (multi && (ev.ctrl || ev.super)) {
        const bool selected = attr_string(row, "aria-selected") == "true";
        changed = set_dcs_row_selected(impl, row, !selected) || changed;
        impl.dcs_select_anchors[lxb_dom_interface_node(box)] =
            lxb_dom_interface_node(row);
    } else if (multi && ev.shift) {
        auto* anchor_node = [&]() -> lxb_dom_node_t* {
            const auto it =
                impl.dcs_select_anchors.find(lxb_dom_interface_node(box));
            return it == impl.dcs_select_anchors.end() ? nullptr : it->second;
        }();
        auto* anchor = anchor_node && anchor_node->type == LXB_DOM_NODE_TYPE_ELEMENT
            ? lxb_dom_interface_element(anchor_node)
            : nullptr;
        const auto anchor_it = anchor
            ? std::find(rows.begin(), rows.end(), anchor)
            : rows.end();
        if (anchor_it != rows.end()) {
            const auto a = static_cast<std::size_t>(anchor_it - rows.begin());
            const auto b = static_cast<std::size_t>(row_it - rows.begin());
            const auto lo = std::min(a, b);
            const auto hi = std::max(a, b);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                changed =
                    set_dcs_row_selected(impl, rows[i], i >= lo && i <= hi) ||
                    changed;
            }
        } else {
            for (auto* r : rows) {
                changed = set_dcs_row_selected(impl, r, r == row) || changed;
            }
            impl.dcs_select_anchors[lxb_dom_interface_node(box)] =
                lxb_dom_interface_node(row);
        }
    } else {
        for (auto* r : rows) {
            changed = set_dcs_row_selected(impl, r, r == row) || changed;
        }
        impl.dcs_select_anchors[lxb_dom_interface_node(box)] =
            lxb_dom_interface_node(row);
    }

    emit_widget_change(impl, box, dcs_selected_rows_value(rows));
    return changed;
}

bool find_decius_collapse_at(detail::DocumentImpl& impl,
                             int from_idx,
                             lxb_dom_element_t*& out_block,
                             lxb_dom_element_t*& out_chevron,
                             std::string_view& out_collapsed_class,
                             std::string_view& out_chevron_open_class) {
    out_block = nullptr;
    out_chevron = nullptr;
    out_collapsed_class = {};
    out_chevron_open_class = {};
    // Walk up from the hit block. A click commonly lands on a text/inline block
    // (e.g. the foldout title's own text run) whose element_for_block is null —
    // the loop skips those via `if (!elem) continue` and keeps climbing, exactly
    // like the tree-chevron matcher. (A previous early `if (!hit) return false`
    // here bailed on such clicks, so clicking a foldout's title text never
    // toggled it — only clicking the chevron/padding did.)
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = element_for_block(impl, idx);
        if (!elem) continue;
        if (class_list_contains(elem, "dcs-subpanel__close") ||
            class_list_contains(elem, "dcs-foldout__tools")) {
            return false;
        }
        if (class_list_contains(elem, "dcs-subpanel__header")) {
            auto* block = nearest_ancestor_with_class(elem, "dcs-subpanel");
            if (!block) return false;
            out_block = block;
            out_chevron =
                first_descendant_with_class(block, "dcs-subpanel__chevron");
            out_collapsed_class = "dcs-subpanel--collapsed";
            out_chevron_open_class = "dcs-subpanel__chevron--open";
            return true;
        }
        if (class_list_contains(elem, "dcs-foldout__header")) {
            auto* block = nearest_ancestor_with_class(elem, "dcs-foldout");
            if (!block) return false;
            out_block = block;
            out_chevron =
                first_descendant_with_class(block, "dcs-foldout__chevron");
            out_collapsed_class = "dcs-foldout--collapsed";
            out_chevron_open_class = "dcs-foldout__chevron--open";
            return true;
        }
    }
    return false;
}

bool toggle_decius_collapse_control(detail::DocumentImpl& impl, int from_idx) {
    lxb_dom_element_t* block = nullptr;
    lxb_dom_element_t* chevron = nullptr;
    std::string_view collapsed_class;
    std::string_view chevron_open_class;
    if (!find_decius_collapse_at(impl, from_idx, block, chevron,
                                 collapsed_class, chevron_open_class)) {
        return false;
    }

    const bool collapsed = class_list_contains(block, collapsed_class);
    const bool next_collapsed = !collapsed;
    bool changed = set_attribute_on_element(
        impl, block, "class",
        class_list_set(block, collapsed_class, next_collapsed));
    if (chevron) {
        changed = set_attribute_on_element(
            impl, chevron, "class",
            class_list_set(chevron, chevron_open_class, !next_collapsed)) ||
                  changed;
    }
    // Flipping the collapsed class on the block is all decius does — the rule
    // `.dcs-foldout--collapsed > .dcs-foldout__body{display:none}` hides the
    // body via the cascade. set_attribute_on_element() restyles the subtree, so
    // that descendant rule must re-match the body. (If it doesn't, that is a
    // renderer cascade bug to fix in the renderer, not to paper over here.)
    emit_widget_change(impl, block, next_collapsed ? "closed" : "open");
    return changed;
}

// ── Dock-pane tab switch (.dcs-dockpane__tab[data-dcs-target]) ───────────────
// Clicking a pane tab activates its target body and deactivates the pane's
// other tabs/bodies — decius semantics: aria-selected on the tab, `hidden` on
// the inactive bodies. Toggling `hidden` recollects the box tree (see
// set_attribute_on_element), so a revealed body's content reappears.
bool find_dockpane_tab_at(detail::DocumentImpl& impl, int from_idx,
                          lxb_dom_element_t*& out_tab) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = element_for_block(impl, idx);
        if (elem && class_list_contains(elem, "dcs-dockpane__tab")) {
            out_tab = elem;
            return true;
        }
    }
    return false;
}

std::string pane_panel_id(lxb_dom_element_t* pane);
std::string dockpane_tab_panel_id(lxb_dom_element_t* tab);

bool switch_dockpane_tab(detail::DocumentImpl& impl, lxb_dom_element_t* tab) {
    if (!tab) return false;
    auto* target = dcs_target_for_trigger(impl, tab);  // the body to reveal
    if (!target) return false;
    // No-op if this tab is already the selected one (avoids churn when a future
    // drag begins on the active tab).
    if (attr_string(tab, "aria-selected") == "true" && !has_attr(target, "hidden"))
        return false;

    // The tab's parent is the .dcs-dockpane__tabs container; the pane is the
    // nearest .dcs-dockpane ancestor.
    auto* tabs_node = lxb_dom_node_parent(lxb_dom_interface_node(tab));
    if (!tabs_node) return false;
    lxb_dom_element_t* pane = nullptr;
    for (auto* n = tabs_node; n; n = lxb_dom_node_parent(n)) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(n);
        if (class_list_contains(e, "dcs-dockpane")) {
            pane = e;
            break;
        }
    }
    if (!pane) return false;

    const std::string pane_id = pane_panel_id(pane);
    const std::string active_id = dockpane_tab_panel_id(tab);

    bool changed = false;
    // Activate the clicked tab; deactivate its siblings (the tabs container's
    // direct-child tabs).
    for (auto* c = lxb_dom_node_first_child(tabs_node); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (!class_list_contains(e, "dcs-dockpane__tab")) continue;
        changed = set_attribute_on_element(impl, e, "aria-selected",
                                           e == tab ? "true" : "false") ||
                  changed;
    }
    // Reveal the target body; hide the pane's other bodies (direct children).
    for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(pane)); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (!class_list_contains(e, "dcs-dockpane__body")) continue;
        changed = (e == target ? remove_attribute_on_element(impl, e, "hidden")
                               : set_attribute_on_element(impl, e, "hidden", "")) ||
                  changed;
    }
    if (!pane_id.empty() && !active_id.empty()) {
        if (active_id == pane_id) impl.dock_active_tabs.erase(pane_id);
        else impl.dock_active_tabs[pane_id] = active_id;
    }
    emit_widget_change(impl, tab, "tab");
    return changed;
}

// Nearest ancestor (inclusive) carrying a class.
lxb_dom_element_t* ancestor_with_class(lxb_dom_element_t* e,
                                       std::string_view cls) {
    for (auto* n = e ? lxb_dom_interface_node(e) : nullptr; n;
         n = lxb_dom_node_parent(n)) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* el = lxb_dom_interface_element(n);
        if (class_list_contains(el, cls)) return el;
    }
    return nullptr;
}

// The dockpanel id behind a tab (its data-dcs-target is "#<id>-body").
std::string dockpane_tab_panel_id(lxb_dom_element_t* tab) {
    std::string sel = attr_string(tab, "data-dcs-target");
    if (!sel.empty() && sel.front() == '#') sel.erase(0, 1);
    static constexpr std::string_view kSuffix = "-body";
    if (sel.size() > kSuffix.size() &&
        sel.compare(sel.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0)
        sel.erase(sel.size() - kSuffix.size());
    return sel;
}

int positive_int_attr(lxb_dom_element_t* elem, std::string_view name,
                      int fallback) {
    if (!elem) return fallback;
    const std::string value = attr_string(elem, name);
    if (value.empty()) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || parsed <= 0) return fallback;
    return static_cast<int>(std::min<long>(parsed, 10000));
}

lxb_dom_element_t* find_dockpane_tab_for_panel_id(detail::DocumentImpl& impl,
                                                  std::string_view panel_id) {
    if (panel_id.empty()) return nullptr;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* elem = element_for_block(impl, i);
        if (!elem || !class_list_contains(elem, "dcs-dockpane__tab")) {
            continue;
        }
        const std::string tab_panel_id = dockpane_tab_panel_id(elem);
        if (std::string_view(tab_panel_id) == panel_id) return elem;
    }
    return nullptr;
}

// Tear a docked panel off into a floating panel at the drop point: records a
// floating placement override so the next resolve_dock emits it as a
// .dcs-panel--floating overlay (then movable via float-drag). The app reloads on
// result.layout_changed, which re-seeds the layout from these overrides.
bool tear_off_panel(detail::DocumentImpl& impl, std::string_view panel_id,
                    lxb_dom_element_t* pane, Point drop) {
    if (panel_id.empty()) return false;
    (void) pane;
    constexpr int kDefaultW = 320;
    constexpr int kDefaultH = 240;
    constexpr int kMargin = 8;
    const Rect host = document_float_host_bounds(impl);
    const Rect root = root_float_host_bounds(impl);
    auto* tab = find_dockpane_tab_for_panel_id(impl, panel_id);
    int w = positive_int_attr(tab, "data-dcs-tearout-width", kDefaultW);
    int h = positive_int_attr(tab, "data-dcs-tearout-height", kDefaultH);
    // Place so the title lands near the cursor, clamped into the document
    // content float host. Runtime floating overrides are root-relative because
    // View emits tearoffs as root overlays, while the allowed rectangle remains
    // the document body.
    const int hx = root.w > 0 ? root.x : host.x;
    const int hy = root.h > 0 ? root.y : host.y;
    const int hw = host.w;
    const int hh = host.h;
    if (hw > kMargin * 2) {
        w = std::max(1, std::min(w, hw - kMargin * 2));
    }
    if (hh > kMargin * 2) {
        h = std::max(1, std::min(h, hh - kMargin * 2));
    }
    const int host_x = host.x - hx;
    const int host_y = host.y - hy;
    int x = drop.x - hx - 60;
    int y = drop.y - hy - 12;
    if (hw > 0) {
        const int lo = host_x + kMargin;
        const int hi = host_x + hw - w - kMargin;
        x = std::clamp(x, lo, std::max(lo, hi));
    }
    if (hh > 0) {
        const int lo = host_y + kMargin;
        const int hi = host_y + hh - h - kMargin;
        y = std::clamp(y, lo, std::max(lo, hi));
    }
    Document::DockPlacement p;
    p.present = true;
    p.floating = true;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    impl.dock_overrides[std::string(panel_id)] = p;
    return true;
}

// ── Drop zones (drag-to-dock / re-dock) ─────────────────────────────────────
// Faithful port of decius.js Je/je/Ke (see [[decius-js-docking-algorithm]]):
//  • Ke: within 32px of the viewport outer edge -> dock to the WHOLE-AREA edge
//        (parent = the document/root), unless a pane edge under the cursor is a
//        better local target.
//  • else the .dcs-dockpane under the cursor (dock-kind must match the dragged
//    tab's kind): over its tabbar -> center (co-tab); else je() picks the
//    nearest edge if within the outer 22%, else center. The document
//    (--center, kind "documents") is only a target for
//    documents-kind tabs and only via window-edge / its tabbar — a body drop
//    there yields no target, so the caller tears off.
enum class DropZone { None, Left, Right, Top, Bottom, Tab };

struct DropTarget {
    std::string parent;   // "__document__" (window-edge dock) or target pane id
    DropZone    zone{DropZone::None};
    int x{0}, y{0}, w{0}, h{0};   // highlight rect in float-host coords
    bool        valid{false};
};

// A pane's dock-kind: data-dcs-dock-kind, else "documents" for the center, else
// "panels". A drag only docks into a pane of the same kind (decius J()).
std::string dock_kind_of(lxb_dom_element_t* pane) {
    if (!pane) return "panels";
    const std::string k = attr_string(pane, "data-dcs-dock-kind");
    if (!k.empty()) return k;
    return class_list_contains(pane, "dcs-dockpane--center") ? "documents"
                                                             : "panels";
}

std::string pane_panel_id(lxb_dom_element_t* pane) {
    const std::string n = attr_string(pane, "data-aui-name");  // pane-<id>
    return n.rfind("pane-", 0) == 0 ? n.substr(5) : std::string();
}

bool arm_tab_drag_from_pending_press(
    detail::DocumentImpl& impl,
    const detail::DocumentImpl::PendingTabPress& press) {
    auto* tab = find_dockpane_tab_for_panel_id(impl, press.panel_id);
    if (!tab) return false;
    auto* pane = ancestor_with_class(tab, "dcs-dockpane");
    if (!pane || dock_kind_of(pane) == "documents") return false;
    impl.tab_drag = {};
    impl.tab_drag.tab = tab;
    impl.tab_drag.pane = pane;
    impl.tab_drag.panel_id = press.panel_id;
    impl.tab_drag.start_x = press.start_x;
    impl.tab_drag.start_y = press.start_y;
    impl.tab_drag.switched_on_down = press.switched_on_down;
    return true;
}

// Is the point over the pane's own tabbar (its direct-child .dcs-dockpane__tabbar)?
bool point_over_pane_tabbar(detail::DocumentImpl& impl, lxb_dom_element_t* pane,
                            Point pt) {
    for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(pane)); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (!class_list_contains(e, "dcs-dockpane__tabbar")) continue;
        const int bi = block_index_for_exact_element(impl, e);
        if (bi < 0) return false;
        const auto& b = impl.blocks[static_cast<std::size_t>(bi)].bounds;
        return pt.x >= b.x && pt.x < b.x + b.w && pt.y >= b.y && pt.y < b.y + b.h;
    }
    return false;
}

bool selected_pane_body_bounds(detail::DocumentImpl& impl,
                               lxb_dom_element_t* pane,
                               Rect& out) {
    if (!pane) return false;
    lxb_dom_element_t* fallback = nullptr;
    for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(pane)); c;
         c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* e = lxb_dom_interface_element(c);
        if (!class_list_contains(e, "dcs-dockpane__body")) continue;
        if (!fallback) fallback = e;
        if (has_attr(e, "hidden")) continue;
        const int bi = block_index_for_exact_element(impl, e);
        if (bi < 0) return false;
        out = impl.blocks[static_cast<std::size_t>(bi)].bounds;
        return out.w > 0 && out.h > 0;
    }
    if (!fallback) return false;
    const int bi = block_index_for_exact_element(impl, fallback);
    if (bi < 0) return false;
    out = impl.blocks[static_cast<std::size_t>(bi)].bounds;
    return out.w > 0 && out.h > 0;
}

DropTarget compute_drop_target(detail::DocumentImpl& impl, Point pt,
                               std::string_view drag_kind) {
    DropTarget out;
    int hx = 0, hy = 0, hw = 0, hh = 0;
    for (int i = 0; i < static_cast<int>(impl.blocks.size()); ++i) {
        auto* e = element_for_block(impl, i);
        if (e && class_list_contains(e, "dcs-dock--floathost")) {
            const auto& hb = impl.blocks[static_cast<std::size_t>(i)].bounds;
            hx = hb.x; hy = hb.y; hw = hb.w; hh = hb.h;
            break;
        }
    }
    if (hw <= 0) return out;
    if (!(pt.x >= hx && pt.x < hx + hw && pt.y >= hy && pt.y < hy + hh))
        return out;  // outside the dock area -> no target (caller tears off)

    // (Ke) window-edge: JS checks the browser viewport, not the dock workarea.
    // Keep the preview rect root-relative, but do the gesture band against the
    // viewport so chrome below/above the dock does not make panel edges vanish.
    constexpr int kWin = 32;
    const int vw = viewport_width_for_overlay(impl);
    const int vh = viewport_height_for_overlay(impl);
    DropTarget window_edge;
    if (pt.x < kWin) window_edge.zone = DropZone::Left;
    else if (pt.x > vw - kWin) window_edge.zone = DropZone::Right;
    else if (pt.y < kWin) window_edge.zone = DropZone::Top;
    else if (pt.y > vh - kWin) window_edge.zone = DropZone::Bottom;
    if (window_edge.zone != DropZone::None) {
        window_edge.parent = "__document__";
        window_edge.valid = true;
        const int sw = hw * 30 / 100, sh = hh * 30 / 100;
        switch (window_edge.zone) {
            case DropZone::Left:   window_edge.x = 0;       window_edge.y = 0;       window_edge.w = sw; window_edge.h = hh; break;
            case DropZone::Right:  window_edge.x = hw - sw; window_edge.y = 0;       window_edge.w = sw; window_edge.h = hh; break;
            case DropZone::Top:    window_edge.x = 0;       window_edge.y = 0;       window_edge.w = hw; window_edge.h = sh; break;
            case DropZone::Bottom: window_edge.x = 0;       window_edge.y = hh - sh; window_edge.w = hw; window_edge.h = sh; break;
            default: break;
        }
    }

    // (Ne) the dock pane under the cursor (same dock-kind). Source panes are
    // still valid targets: dropping back onto the source center is a no-op
    // cancel, while source edge zones can split a tab beside its former stack.
    // Pane edge drops win over the viewport band. This keeps shallow bottom
    // panes (Assets/Console shelves) dockable on their own bottom edge even
    // when they sit near the app's bottom chrome.
    // Walk later blocks first so floating overlays and nested hit targets win
    // over older docked panes that happen to sit underneath them.
    for (int i = static_cast<int>(impl.blocks.size()) - 1; i >= 0; --i) {
        auto* e = element_for_block(impl, i);
        if (!e) continue;
        if (!class_list_contains(e, "dcs-dockpane")) continue;
        const auto& b = impl.blocks[static_cast<std::size_t>(i)].bounds;
        if (!(pt.x >= b.x && pt.x < b.x + b.w && pt.y >= b.y && pt.y < b.y + b.h))
            continue;
        if (dock_kind_of(e) != drag_kind) return out;  // kind mismatch -> tearoff
        const std::string id = pane_panel_id(e);
        if (id.empty()) return out;
        out.parent = id;
        const int lx = b.x - hx, ly = b.y - hy;
        if (ancestor_with_class(e, "dcs-panel--floating") ||
            point_over_pane_tabbar(impl, e, pt)) {
            out.zone = DropZone::Tab;
        } else {
            // Edge intent is percentage-based: choose the closest normalized
            // edge within the cutoff, otherwise use the center/tab zone. The
            // corners become triangular ownership regions and narrow panes
            // keep usable side targets.
            constexpr double kT = 0.22;
            Rect zone_bounds = b;
            Rect body_bounds{};
            if (selected_pane_body_bounds(impl, e, body_bounds) &&
                pt.x >= body_bounds.x &&
                pt.x < body_bounds.x + body_bounds.w &&
                pt.y >= body_bounds.y &&
                pt.y < body_bounds.y + body_bounds.h) {
                zone_bounds = body_bounds;
            }
            const double x = zone_bounds.w > 0
                                 ? (pt.x - zone_bounds.x) /
                                       static_cast<double>(zone_bounds.w)
                                 : 0.5;
            const double y = zone_bounds.h > 0
                                 ? (pt.y - zone_bounds.y) /
                                       static_cast<double>(zone_bounds.h)
                                 : 0.5;
            const double dists[] = {x, 1.0 - x, y, 1.0 - y};
            const DropZone zones[] = {DropZone::Left, DropZone::Right,
                                      DropZone::Top, DropZone::Bottom};
            double best = kT;
            out.zone = DropZone::Tab;
            for (int zi = 0; zi < 4; ++zi) {
                if (dists[zi] < best) {
                    best = dists[zi];
                    out.zone = zones[zi];
                }
            }
        }
        out.valid = true;
        if (out.zone == DropZone::Tab && window_edge.valid) return window_edge;
        switch (out.zone) {
            case DropZone::Left:   out.x = lx;             out.y = ly;             out.w = b.w / 2; out.h = b.h;     break;
            case DropZone::Right:  out.x = lx + b.w - b.w / 2; out.y = ly;         out.w = b.w / 2; out.h = b.h;     break;
            case DropZone::Top:    out.x = lx;             out.y = ly;             out.w = b.w;     out.h = b.h / 2; break;
            case DropZone::Bottom: out.x = lx;             out.y = ly + b.h - b.h / 2; out.w = b.w; out.h = b.h / 2; break;
            default:               out.x = lx;             out.y = ly;             out.w = b.w;     out.h = b.h;     break;
        }
        return out;
    }
    if (window_edge.valid) return window_edge;
    return out;
}

// Position (and show) or hide the drop indicator. Reuses decius .dcs-drop--valid
// (accent inset outline + dim fill); the rect is float-host-relative.
bool set_drop_indicator(detail::DocumentImpl& impl, const DropTarget* t) {
    auto* ind = find_dom_element_by_id(impl, "__dropind");
    if (!ind) return false;
    if (!t || !t->valid) {
        bool changed = set_attribute_on_element(impl, ind, "hidden", "");
        changed = set_attribute_on_element(
                      impl, ind, "style",
                      "position:absolute;pointer-events:none;z-index:200;"
                      "display:none;left:0px;top:0px;width:0px;height:0px") ||
                  changed;
        return changed;
    }
    bool changed = set_attribute_on_element(
        impl, ind, "style",
        "position:absolute;pointer-events:none;z-index:200;border:2px solid "
        "rgb(0,184,212);background:rgba(0,184,212,0.18);"
        "box-sizing:border-box;left:" +
            std::to_string(t->x) + "px;top:" + std::to_string(t->y) +
            "px;width:" + std::to_string(t->w) + "px;height:" +
            std::to_string(t->h) + "px");
    changed = remove_attribute_on_element(impl, ind, "hidden") || changed;
    return changed;
}

// Record a docked placement override: dock `panel_id` to a side of (or as a tab
// of) the target pane. The app re-resolves the layout on rebuild.
bool apply_dock(detail::DocumentImpl& impl, std::string_view panel_id,
                const DropTarget& t) {
    if (panel_id.empty() || !t.valid || t.zone == DropZone::None) return false;
    if (t.parent.empty() || t.parent == panel_id) return false;
    Document::DockPlacement p;
    p.present = true;
    p.floating = false;
    p.parent = t.parent;
    switch (t.zone) {
        case DropZone::Left:   p.side = 0; break;
        case DropZone::Right:  p.side = 1; break;
        case DropZone::Top:    p.side = 2; break;
        case DropZone::Bottom: p.side = 3; break;
        case DropZone::Tab:    p.side = 4; break;
        default: return false;
    }
    const std::string key(panel_id);
    if (const auto it = impl.dock_overrides.find(key);
        it != impl.dock_overrides.end()) {
        const auto& old = it->second;
        if (old.present && !old.floating && old.parent == p.parent &&
            old.side == p.side && old.size == p.size) {
            return false;
        }
    }
    impl.dock_overrides[key] = p;
    return true;
}

int dcs_tree_row_depth(lxb_dom_element_t* row) {
    const std::string depth_value =
        find_decl_value(attr_string(row, "style"), "--depth");
    if (depth_value.empty()) return 0;
    char* end = nullptr;
    const long parsed = std::strtol(depth_value.c_str(), &end, 10);
    if (end == depth_value.c_str()) return 0;
    return static_cast<int>(std::clamp<long>(parsed, 0, 128));
}

void collect_dcs_tree_rows(lxb_dom_element_t* elem,
                           std::vector<lxb_dom_element_t*>& rows) {
    if (!elem) return;
    for (auto* child = lxb_dom_node_first_child(lxb_dom_interface_node(elem));
         child != nullptr; child = lxb_dom_node_next(child)) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* child_elem = lxb_dom_interface_element(child);
        if (class_list_contains(child_elem, "dcs-tree__row")) {
            rows.push_back(child_elem);
        }
        collect_dcs_tree_rows(child_elem, rows);
    }
}

bool dcs_tree_row_has_direct_child(
    const std::vector<lxb_dom_element_t*>& rows,
    std::size_t row_index,
    int depth) {
    for (std::size_t i = row_index + 1; i < rows.size(); ++i) {
        const int child_depth = dcs_tree_row_depth(rows[i]);
        if (child_depth <= depth) return false;
        if (child_depth == depth + 1) return true;
    }
    return false;
}

bool refresh_dcs_tree_visibility(detail::DocumentImpl& impl,
                                 lxb_dom_element_t* tree) {
    std::vector<lxb_dom_element_t*> rows;
    collect_dcs_tree_rows(tree, rows);
    if (rows.empty()) return false;

    std::vector<bool> open_by_depth(129, true);
    bool changed = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto* row = rows[i];
        const int depth = dcs_tree_row_depth(row);
        bool visible = true;
        for (int d = 0; d < depth && d < static_cast<int>(open_by_depth.size());
             ++d) {
            if (!open_by_depth[static_cast<std::size_t>(d)]) {
                visible = false;
                break;
            }
        }

        changed = visible ? (remove_attribute_on_element(impl, row, "hidden") ||
                             changed)
                          : (set_attribute_on_element(impl, row, "hidden", "") ||
                             changed);

        if (auto* chevron =
                first_descendant_with_class(row, "dcs-tree__chevron")) {
            const bool has_child =
                dcs_tree_row_has_direct_child(rows, i, depth);
            changed = set_attribute_on_element(
                          impl, chevron, "class",
                          class_list_set(chevron, "dcs-tree__chevron--leaf",
                                         !has_child)) ||
                      changed;
            open_by_depth[static_cast<std::size_t>(depth)] =
                has_child && class_list_contains(
                                 chevron, "dcs-tree__chevron--open");
        } else {
            open_by_depth[static_cast<std::size_t>(depth)] = true;
        }
        for (std::size_t d = static_cast<std::size_t>(depth + 1);
             d < open_by_depth.size(); ++d) {
            open_by_depth[d] = true;
        }
    }
    return changed;
}

bool find_dcs_tree_chevron_at(detail::DocumentImpl& impl,
                              int from_idx,
                              lxb_dom_element_t*& out_tree,
                              lxb_dom_element_t*& out_row,
                              lxb_dom_element_t*& out_chevron) {
    out_tree = nullptr;
    out_row = nullptr;
    out_chevron = nullptr;
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = element_for_block(impl, idx);
        if (!elem) continue;
        if (class_list_contains(elem, "dcs-tree__chevron")) {
            if (class_list_contains(elem, "dcs-tree__chevron--leaf")) {
                return false;
            }
            out_chevron = elem;
        }
        if (out_chevron && !out_row &&
            class_list_contains(elem, "dcs-tree__row")) {
            out_row = elem;
        }
        if (out_chevron && out_row &&
            class_list_contains(elem, "dcs-tree")) {
            out_tree = elem;
            return true;
        }
    }
    return false;
}

bool toggle_dcs_tree_chevron_control(detail::DocumentImpl& impl, int from_idx) {
    lxb_dom_element_t* tree = nullptr;
    lxb_dom_element_t* row = nullptr;
    lxb_dom_element_t* chevron = nullptr;
    if (!find_dcs_tree_chevron_at(impl, from_idx, tree, row, chevron)) {
        return false;
    }

    std::vector<lxb_dom_element_t*> rows;
    collect_dcs_tree_rows(tree, rows);
    const auto row_it = std::find(rows.begin(), rows.end(), row);
    if (row_it == rows.end()) return false;
    if (!dcs_tree_row_has_direct_child(
            rows, static_cast<std::size_t>(row_it - rows.begin()),
            dcs_tree_row_depth(row))) {
        return false;
    }

    const bool open =
        class_list_contains(chevron, "dcs-tree__chevron--open");
    bool changed = set_attribute_on_element(
        impl, chevron, "class",
        class_list_set(chevron, "dcs-tree__chevron--open", !open));
    changed = refresh_dcs_tree_visibility(impl, tree) || changed;
    emit_widget_change(impl, tree, !open ? "open" : "closed");
    return changed;
}

bool is_open_transient_layer(lxb_dom_element_t* elem) {
    return elem && !has_attr(elem, "hidden") &&
           (class_list_contains(elem, "aui-select__menu") ||
            class_list_contains(elem, "dcs-menu") ||
            class_list_contains(elem, "dcs-popover"));
}

bool click_preserves_transient_layers(detail::DocumentImpl& impl,
                                      int from_idx) {
    for (int idx = from_idx;
         idx >= 0 && idx < static_cast<int>(impl.blocks.size());
         idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx) {
        auto* elem = element_for_block(impl, idx);
        if (!elem) continue;
        if (is_open_transient_layer(elem)) return true;
        if (is_dcs_menu_trigger(elem) || is_dcs_popover_trigger(elem)) {
            return true;
        }
        if (attr_string(elem, "data-aui-widget") == "dropdown" ||
            tag_name(elem) == "select") {
            return true;
        }
    }
    return false;
}

// Generic chain-refresh helper used by both :hover (chain follows the
// pointer) and :active (chain follows the pressed element). `bit`
// selects which state bit to toggle; `current_chain` is the previous
// chain that we'll diff against and overwrite. Returns true on change.
bool refresh_pseudo_chain(detail::DocumentImpl& impl,
                          std::vector<int>& current_chain,
                          int target_idx,
                          std::uint8_t bit) {
    auto new_chain = build_hover_chain(impl.blocks, target_idx);
    if (new_chain == current_chain) return false;

    const auto in = [](int x, const std::vector<int>& v) {
        return std::find(v.begin(), v.end(), x) != v.end();
    };
    std::vector<int> changed_roots;
    // Leaving blocks: clear bit + restyle.
    for (int old_idx : current_chain) {
        if (in(old_idx, new_chain)) continue;
        const auto id = impl.blocks[static_cast<std::size_t>(old_idx)].id;
        impl.style_store.state_bits(id) &= static_cast<std::uint8_t>(~bit);
        changed_roots.push_back(old_idx);
    }
    // Entering blocks: set bit + restyle.
    for (int new_idx : new_chain) {
        if (in(new_idx, current_chain)) continue;
        const auto id = impl.blocks[static_cast<std::size_t>(new_idx)].id;
        impl.style_store.state_bits(id) |= bit;
        changed_roots.push_back(new_idx);
    }
    for (int root_idx : changed_roots) {
        bool covered_by_ancestor = false;
        for (int other_idx : changed_roots) {
            if (other_idx == root_idx) continue;
            if (is_descendant_of_or_self(impl.blocks, root_idx, other_idx)) {
                covered_by_ancestor = true;
                break;
            }
        }
        if (covered_by_ancestor) continue;
        const Rect old_rect = subtree_visual_rect(impl, root_idx);
        const bool needs_layout = restyle_subtree(impl, root_idx);
        mark_live_mutation_dirty(impl, root_idx, old_rect, needs_layout);
    }
    current_chain = std::move(new_chain);
    return true;
}

bool refresh_hover_chain(detail::DocumentImpl& impl) {
    return refresh_pseudo_chain(impl, impl.hovered_chain,
                                impl.hovered_idx, kHoverStateBit);
}

bool refresh_active_chain(detail::DocumentImpl& impl) {
    return refresh_pseudo_chain(impl, impl.active_chain,
                                impl.active_idx, kActiveStateBit);
}

// Move :focus to `target_idx` (use -1 to clear). Toggles the focus
// state bit on the leaving and entering elements, restyles each so
// the :focus overlay takes effect on the next paint, and records the
// new focused element. Unlike :hover / :active, focus is a single
// element rather than a chain â€” there is no inheritance up the
// ancestor list.
bool set_focus(detail::DocumentImpl& impl, int target_idx) {
    if (target_idx == impl.focused_idx) return false;
    const int old_idx = impl.focused_idx;
    impl.focused_idx  = target_idx;
    if (old_idx >= 0 && old_idx < static_cast<int>(impl.blocks.size())) {
        const Rect old_rect = subtree_visual_rect(impl, old_idx);
        const auto id = impl.blocks[static_cast<std::size_t>(old_idx)].id;
        impl.style_store.state_bits(id) &= static_cast<std::uint8_t>(~kFocusStateBit);
        const bool needs_layout = restyle_block(impl, old_idx);
        mark_live_mutation_dirty(impl, old_idx, old_rect, needs_layout);
    }
    if (target_idx >= 0 && target_idx < static_cast<int>(impl.blocks.size())) {
        const Rect old_rect = subtree_visual_rect(impl, target_idx);
        const auto id = impl.blocks[static_cast<std::size_t>(target_idx)].id;
        impl.style_store.state_bits(id) |= kFocusStateBit;
        const bool needs_layout = restyle_block(impl, target_idx);
        mark_live_mutation_dirty(impl, target_idx, old_rect, needs_layout);
    }
    return true;
}

// Walk up from `idx` looking for the nearest focusable element. A tag
// is focusable if it natively accepts keyboard input today â€” buttons,
// inputs, textareas, selects, and <a href>. Returns -1 when no such
// ancestor exists; callers should treat that as "click outside any
// focusable element â†’ clear focus".
int focusable_ancestor(const detail::DocumentImpl& impl, int idx) {
    while (idx >= 0) {
        const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
        const auto& t = b.tag;
        if (t == "button" || t == "input" || t == "textarea" ||
            t == "select") {
            return idx;
        }
        if (t == "a") {
            auto* elem = impl.style_store.element_of(b.id);
            if (elem && !attr_string(elem, "href").empty()) return idx;
        }
        idx = b.parent_idx;
    }
    return -1;
}

// True iff this block clips its children (overflow is non-visible).
// CSS overflow: hidden | clip | scroll | auto all clip descendant paint.
bool block_clips_overflow(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0) return false;
    const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
    const auto ov = impl.style_store.computed(b.id).overflow_y;
    using O = detail::ComputedStyle::Overflow;
    return ov == O::Hidden || ov == O::Clip
        || ov == O::Scroll || ov == O::Auto;
}

int nearest_fixed_ancestor_or_self(const detail::DocumentImpl& impl, int idx) {
    using P = detail::ComputedStyle::Position;
    while (idx >= 0 && idx < static_cast<int>(impl.blocks.size())) {
        const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
        if (impl.style_store.computed(b.id).position == P::Fixed) {
            return idx;
        }
        idx = b.parent_idx;
    }
    return -1;
}

int nearest_clip_ancestor_for_block(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return -1;
    const int fixed_idx = nearest_fixed_ancestor_or_self(impl, idx);
    const int fixed_parent =
        fixed_idx >= 0
            ? impl.blocks[static_cast<std::size_t>(fixed_idx)].parent_idx
            : -2;
    int clip_idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx;
    while (clip_idx >= 0) {
        if (fixed_idx >= 0 && clip_idx == fixed_parent) break;
        if (block_clips_overflow(impl, clip_idx)) return clip_idx;
        clip_idx = impl.blocks[static_cast<std::size_t>(clip_idx)].parent_idx;
    }
    return -1;
}

// True iff this block accepts scroll input on its Y axis.
bool block_is_scrollable_y(const detail::DocumentImpl& impl, int idx) {
    if (idx < 0) return false;
    const auto& b = impl.blocks[static_cast<std::size_t>(idx)];
    const auto ov = impl.style_store.computed(b.id).overflow_y;
    using O = detail::ComputedStyle::Overflow;
    if (ov != O::Scroll && ov != O::Auto) return false;
    return b.content_h > b.bounds.h;
}

// Find the nearest scrollable-Y ancestor (or self) of `idx`. Returns
// -1 when none exists. Used by wheel routing.
int find_scrollable_y_ancestor(const detail::DocumentImpl& impl, int idx) {
    while (idx >= 0) {
        if (block_is_scrollable_y(impl, idx)) return idx;
        idx = impl.blocks[static_cast<std::size_t>(idx)].parent_idx;
    }
    return -1;
}

bool focused_text_control(detail::DocumentImpl& impl, Block*& out) {
    out = nullptr;
    const int idx = impl.focused_idx;
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return false;
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (!block.text_control) return false;
    out = &block;
    return true;
}

void remove_last_utf8_codepoint(std::string& text) {
    if (text.empty()) return;
    std::size_t pos = text.size() - 1;
    while (pos > 0 &&
           (static_cast<unsigned char>(text[pos]) & 0xC0u) == 0x80u) {
        --pos;
    }
    text.erase(pos);
}

std::size_t previous_utf8_boundary(std::string_view text, std::size_t pos) {
    pos = std::min(pos, text.size());
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 &&
           (static_cast<unsigned char>(text[pos]) & 0xC0u) == 0x80u) {
        --pos;
    }
    return pos;
}

std::size_t next_utf8_boundary(std::string_view text, std::size_t pos) {
    pos = std::min(pos, text.size());
    if (pos >= text.size()) return text.size();
    ++pos;
    while (pos < text.size() &&
           (static_cast<unsigned char>(text[pos]) & 0xC0u) == 0x80u) {
        ++pos;
    }
    return pos;
}

Painter::TextAlign painter_text_align(const detail::ComputedStyle& cs) {
    switch (cs.text_align) {
        case detail::ComputedStyle::TextAlign::Center:
            return Painter::TextAlign::Center;
        case detail::ComputedStyle::TextAlign::Right:
            return Painter::TextAlign::Right;
        case detail::ComputedStyle::TextAlign::Justify:
            return Painter::TextAlign::Justify;
        case detail::ComputedStyle::TextAlign::Left:
        default:
            return Painter::TextAlign::Left;
    }
}

TextControlGeometry text_control_geometry(const detail::DocumentImpl& impl,
                                          int idx,
                                          Painter& painter) {
    const auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    const auto& cs = impl.style_store.computed(block.id);
    const int dy = scroll_offset_y_for(impl.blocks, impl.style_store, idx);
    const Rect eff{block.bounds.x, block.bounds.y - dy,
                   block.bounds.w, block.bounds.h};
    const int used_border_top = cs.used_border_top();
    const int used_border_left = cs.used_border_left();
    const int used_border_right = cs.used_border_right();

    TextControlGeometry g{};
    g.font = painter.resolve_font(impl.style_store.font_family_of(cs.font_id),
                                  cs.font_size_px,
                                  cs.font_weight,
                                  cs.font_style != 0);
    g.text_x = eff.x + used_border_left + cs.padding_left;
    g.text_y = eff.y + used_border_top + cs.padding_top;
    g.content_w = static_cast<float>(
        eff.w - used_border_left - used_border_right -
        cs.padding_left - cs.padding_right);
    g.content_w = std::max(1.0f, g.content_w);
    g.letter_spacing_px = static_cast<float>(cs.letter_spacing_x100) / 100.0f;
    g.line_height_mult = detail::effective_line_height_mult(cs);
    g.align = painter_text_align(cs);
    using WS = detail::ComputedStyle::WhiteSpace;
    g.nowrap = cs.white_space == WS::Nowrap || cs.white_space == WS::Pre;

    const int textarea_idx =
        nearest_block_with_tag(impl.blocks, idx, "textarea");
    if (textarea_idx >= 0) {
        const auto& textarea_cs = impl.style_store.computed(
            impl.blocks[static_cast<std::size_t>(textarea_idx)].id);
        g.text_y += std::max(
            0, textarea_cs.padding_top - textarea_cs.used_border_top());
    } else if (block.tag == "input" &&
               block.input_type != "checkbox" &&
               block.input_type != "radio") {
        g.text_y += std::min<int>(1, used_border_top);
    }

    int indent_px = cs.text_indent_value;
    if (cs.text_indent_is_pct) {
        indent_px = static_cast<int>(std::lround(
            g.content_w * static_cast<float>(cs.text_indent_value) /
            10000.0f));
    }
    if (indent_px != 0) {
        g.text_x += indent_px;
        g.content_w = std::max(
            1.0f, g.content_w - static_cast<float>(indent_px));
    }

    if (g.nowrap && (g.align == Painter::TextAlign::Center ||
                     g.align == Painter::TextAlign::Right)) {
        const std::string display =
            block.placeholder_visible
                ? block.text
                : text_control_display_value(block, block.text_value);
        const Size measured = g.letter_spacing_px == 0.0f
            ? Size{painter.measure_text(g.font, display), 0}
            : painter.measure_text_box(g.font, display, 1e6f,
                                       g.line_height_mult,
                                       g.letter_spacing_px);
        const float slack = g.content_w - static_cast<float>(measured.width);
        if (g.align == Painter::TextAlign::Center) {
            g.text_x += static_cast<int>(std::lround(slack * 0.5f));
        } else {
            g.text_x += static_cast<int>(std::lround(slack));
        }
        g.align = Painter::TextAlign::Left;
    }
    return g;
}

std::uint64_t text_layout_signature(const detail::DocumentImpl& impl,
                                    int idx,
                                    const TextControlGeometry& g,
                                    const Block& block) {
    std::uint64_t h = 1469598103934665603ull;
    if (const auto* elem = element_for_block(impl, idx)) {
        const auto* node = lxb_dom_interface_node(
            const_cast<lxb_dom_element_t*>(elem));
        hash_mix(h, node);
    } else {
        hash_mix(h, idx);
    }
    hash_mix_string(h, block.tag);
    hash_mix_string(h, block.input_type);
    hash_mix_string(h, block.text_value);
    const auto& cs = impl.style_store.computed(block.id);
    hash_mix(h, g.font);
    hash_mix(h, g.content_w);
    hash_mix(h, g.letter_spacing_px);
    hash_mix(h, g.line_height_mult);
    hash_mix(h, g.align);
    hash_mix(h, g.nowrap);
    hash_mix(h, cs.white_space);
    return h;
}

float measure_text_advance(Painter& painter,
                           std::uint32_t font,
                           std::string_view text,
                           float line_height_mult,
                           float letter_spacing_px) {
    if (text.empty()) return 0.0f;
    if (letter_spacing_px == 0.0f) {
        return static_cast<float>(painter.measure_text(font, text));
    }
    return static_cast<float>(
        painter.measure_text_box(font, text, 1e6f, line_height_mult,
                                 letter_spacing_px).width);
}

TextLayoutEntry& ensure_text_layout_entry(detail::DocumentImpl& impl,
                                          int idx,
                                          const TextControlGeometry& g,
                                          const Block& block,
                                          Painter& painter) {
    const std::uint64_t signature =
        text_layout_signature(impl, idx, g, block);
    lxb_dom_node_t* node = nullptr;
    if (auto* elem = element_for_block(impl, idx)) {
        node = lxb_dom_interface_node(elem);
    }
    if (auto found = impl.text_layout_cache.find(signature);
        found != impl.text_layout_cache.end() &&
        found->second.signature == signature &&
        !found->second.caret_offsets.empty()) {
        if (node) impl.text_layout_signatures[node] = signature;
        return found->second;
    }
    if (impl.text_layout_cache.size() > 256) {
        impl.text_layout_cache.clear();
        impl.text_layout_signatures.clear();
    }
    auto& entry = impl.text_layout_cache[signature];

    entry = TextLayoutEntry{};
    entry.signature = signature;
    entry.text_x = g.text_x;
    entry.text_y = g.text_y;
    entry.content_w = g.content_w;
    entry.align = g.align;
    entry.nowrap = g.nowrap;
    if (node) impl.text_layout_signatures[node] = signature;
    const auto metrics = painter.text_metrics(g.font);
    entry.natural_line_height =
        metrics.line_height > 0.0f
            ? metrics.line_height
            : static_cast<float>(
                  impl.style_store.computed(block.id).font_size_px);
    entry.css_line_height = std::max(
        1.0f,
        static_cast<float>(impl.style_store.computed(block.id).font_size_px) *
            g.line_height_mult);

    const auto display_segment = [&](std::size_t begin, std::size_t end) {
        begin = std::min(begin, block.text_value.size());
        end = std::min(end, block.text_value.size());
        if (begin > end) std::swap(begin, end);
        return text_control_display_value(
            block,
            std::string_view(block.text_value).substr(begin, end - begin));
    };
    const auto push_caret = [&](std::size_t offset,
                                float x,
                                std::uint16_t caret_line) {
        if (!entry.caret_offsets.empty() &&
            entry.caret_offsets.back() == offset) {
            entry.caret_x.back() = x;
            entry.caret_lines.back() = caret_line;
            return;
        }
        entry.caret_offsets.push_back(offset);
        entry.caret_x.push_back(x);
        entry.caret_lines.push_back(caret_line);
    };

    push_caret(0, 0.0f, 0);
    entry.line_widths.push_back(0.0f);

    std::size_t line_start = 0;
    std::uint16_t line = 0;
    for (std::size_t pos = 0; pos < block.text_value.size();) {
        const std::size_t next = next_utf8_boundary(block.text_value, pos);
        if (block.text_value[pos] == '\n') {
            entry.line_widths[static_cast<std::size_t>(line)] =
                measure_text_advance(
                    painter, g.font,
                    display_segment(line_start, pos),
                    g.line_height_mult, g.letter_spacing_px);
            ++line;
            line_start = next;
            entry.line_widths.push_back(0.0f);
            push_caret(next, 0.0f, line);
            pos = next;
            continue;
        }

        if (!g.nowrap && line_start < pos) {
            const float next_width =
                measure_text_advance(
                    painter, g.font, display_segment(line_start, next),
                    g.line_height_mult, g.letter_spacing_px);
            if (next_width > g.content_w) {
                entry.line_widths[static_cast<std::size_t>(line)] =
                    measure_text_advance(
                        painter, g.font, display_segment(line_start, pos),
                        g.line_height_mult, g.letter_spacing_px);
                if (line < std::numeric_limits<std::uint16_t>::max()) {
                    ++line;
                }
                line_start = pos;
                entry.line_widths.push_back(0.0f);
                push_caret(pos, 0.0f, line);
            }
        }

        push_caret(
            next,
            measure_text_advance(
                painter, g.font,
                display_segment(line_start, next),
                g.line_height_mult, g.letter_spacing_px),
            line);
        pos = next;
    }
    if (line_start <= block.text_value.size()) {
        entry.line_widths[static_cast<std::size_t>(line)] =
            measure_text_advance(
                painter, g.font,
                display_segment(line_start, block.text_value.size()),
                g.line_height_mult, g.letter_spacing_px);
    }
    return entry;
}

const TextLayoutEntry* cached_text_layout_entry(const detail::DocumentImpl& impl,
                                                int idx) {
    if (auto* elem = element_for_block(impl, idx)) {
        auto* node = lxb_dom_interface_node(elem);
        if (auto sig = impl.text_layout_signatures.find(node);
            sig != impl.text_layout_signatures.end()) {
            if (auto found = impl.text_layout_cache.find(sig->second);
                found != impl.text_layout_cache.end() &&
                found->second.signature == sig->second &&
                !found->second.caret_offsets.empty()) {
                return &found->second;
            }
        }
    }
    return nullptr;
}

float aligned_line_origin_x(const TextControlGeometry& g,
                            const TextLayoutEntry& entry,
                            std::uint16_t line) {
    float x = static_cast<float>(g.text_x);
    const float line_w = line < entry.line_widths.size()
        ? entry.line_widths[static_cast<std::size_t>(line)]
        : 0.0f;
    if (g.align == Painter::TextAlign::Right) {
        x += g.content_w - line_w;
    } else if (g.align == Painter::TextAlign::Center) {
        x += (g.content_w - line_w) * 0.5f;
    }
    return x;
}

float aligned_line_origin_x(const TextLayoutEntry& entry,
                            std::uint16_t line) {
    float x = static_cast<float>(entry.text_x);
    const float line_w = line < entry.line_widths.size()
        ? entry.line_widths[static_cast<std::size_t>(line)]
        : 0.0f;
    if (entry.align == Painter::TextAlign::Right) {
        x += entry.content_w - line_w;
    } else if (entry.align == Painter::TextAlign::Center) {
        x += (entry.content_w - line_w) * 0.5f;
    }
    return x;
}

std::size_t text_caret_offset_from_point(detail::DocumentImpl& impl,
                                         int idx,
                                         Point p) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return 0;
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (!block.text_control || block.text_value.empty()) return 0;
    const auto* entry = cached_text_layout_entry(impl, idx);
    if (entry == nullptr) {
        const auto& cs = impl.style_store.computed(block.id);
        const int content_x = block.bounds.x + cs.used_border_left() +
                              cs.padding_left;
        const int content_w = std::max(
            1,
            block.bounds.w - cs.used_border_left() - cs.used_border_right() -
                cs.padding_left - cs.padding_right);
        const double ratio = std::clamp(
            static_cast<double>(p.x - content_x) /
                static_cast<double>(content_w),
            0.0, 1.0);
        std::size_t pos = static_cast<std::size_t>(std::llround(
            ratio * static_cast<double>(block.text_value.size())));
        while (pos > 0 && pos < block.text_value.size() &&
               (static_cast<unsigned char>(block.text_value[pos]) & 0xC0u) ==
                   0x80u) {
            ++pos;
        }
        return std::min(pos, block.text_value.size());
    }

    const int line_count = std::max<int>(
        1, static_cast<int>(entry->line_widths.size()));
    int target_line = static_cast<int>(
        std::floor((static_cast<float>(p.y - entry->text_y)) /
                   entry->css_line_height));
    target_line = std::clamp(target_line, 0, line_count - 1);
    const std::uint16_t line = static_cast<std::uint16_t>(target_line);
    const float origin_x = aligned_line_origin_x(*entry, line);
    const float local_x = static_cast<float>(p.x) - origin_x;

    std::size_t best = 0;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < entry->caret_offsets.size(); ++i) {
        if (entry->caret_lines[i] != target_line) continue;
        const float distance = std::abs(entry->caret_x[i] - local_x);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return entry->caret_offsets[best];
}

std::pair<std::size_t, std::size_t>
normalized_selection(const Block& block) {
    const auto a = std::min(block.selection_anchor, block.text_value.size());
    const auto b = std::min(block.selection_focus, block.text_value.size());
    return {std::min(a, b), std::max(a, b)};
}

bool has_text_selection(const Block& block) {
    const auto [begin, end] = normalized_selection(block);
    return begin != end;
}

void set_text_selection(detail::DocumentImpl& impl,
                        int idx,
                        Block& block,
                        std::size_t anchor,
                        std::size_t focus) {
    anchor = std::min(anchor, block.text_value.size());
    focus = std::min(focus, block.text_value.size());
    block.selection_anchor = anchor;
    block.selection_focus = focus;
    block.caret_offset = focus;
    if (auto* elem = element_for_block(impl, idx)) {
        auto* node = lxb_dom_interface_node(elem);
        impl.live_text_carets[node] = focus;
        impl.live_text_selections[node] = {anchor, focus};
    }
}

bool set_text_caret_from_point(detail::DocumentImpl& impl,
                               int idx,
                               Point p,
                               bool extend_selection = false,
                               std::size_t anchor = 0) {
    if (idx < 0 || idx >= static_cast<int>(impl.blocks.size())) return false;
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    if (!block.text_control) return false;
    const std::size_t next = text_caret_offset_from_point(impl, idx, p);
    const std::size_t next_anchor =
        extend_selection ? std::min(anchor, block.text_value.size()) : next;
    if (next == block.caret_offset &&
        next_anchor == block.selection_anchor &&
        next == block.selection_focus) {
        return false;
    }
    set_text_selection(impl, idx, block, next_anchor, next);
    add_dirty_rect(impl, block_visual_rect(impl, idx));
    return true;
}

std::pair<std::size_t, std::size_t>
word_bounds_at(std::string_view text, std::size_t caret) {
    caret = std::min(caret, text.size());
    const auto is_word = [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    };
    std::size_t begin = caret;
    if (begin == text.size() && begin > 0) begin = previous_utf8_boundary(text, begin);
    while (begin > 0) {
        const std::size_t prev = previous_utf8_boundary(text, begin);
        if (prev >= text.size() || !is_word(static_cast<unsigned char>(text[prev]))) {
            break;
        }
        begin = prev;
    }
    std::size_t end = caret;
    while (end < text.size()) {
        if (!is_word(static_cast<unsigned char>(text[end]))) break;
        end = next_utf8_boundary(text, end);
    }
    if (begin == end && caret < text.size()) {
        begin = caret;
        end = next_utf8_boundary(text, caret);
    }
    return {begin, end};
}

std::string erase_selected_text(std::string text,
                                std::size_t begin,
                                std::size_t end,
                                std::size_t& caret) {
    begin = std::min(begin, text.size());
    end = std::min(end, text.size());
    if (begin > end) std::swap(begin, end);
    text.erase(begin, end - begin);
    caret = begin;
    return text;
}

std::string replace_selected_text(std::string text,
                                  std::size_t begin,
                                  std::size_t end,
                                  std::string_view insert,
                                  std::size_t& caret) {
    begin = std::min(begin, text.size());
    end = std::min(end, text.size());
    if (begin > end) std::swap(begin, end);
    text.replace(begin, end - begin, insert);
    caret = begin + insert.size();
    return text;
}

std::string erase_previous_codepoint(std::string text, std::size_t& caret) {
    caret = std::min(caret, text.size());
    if (caret == 0) return text;
    const std::size_t start = previous_utf8_boundary(text, caret);
    text.erase(start, caret - start);
    caret = start;
    return text;
}

std::string erase_next_codepoint(std::string text, std::size_t& caret) {
    caret = std::min(caret, text.size());
    if (caret >= text.size()) return text;
    const std::size_t end = next_utf8_boundary(text, caret);
    text.erase(caret, end - caret);
    return text;
}

std::string insert_text_at_caret(std::string text,
                                 std::size_t& caret,
                                 std::string_view insert) {
    caret = std::min(caret, text.size());
    text.insert(caret, insert);
    caret += insert.size();
    return text;
}

void set_live_text_value(detail::DocumentImpl& impl,
                         int idx,
                         Block& block,
                         std::string value) {
    const std::size_t caret = value.size();
    set_live_text_state(impl, idx, block, std::move(value), caret);
}

void set_live_text_state(detail::DocumentImpl& impl,
                         int idx,
                         Block& block,
                         std::string value,
                         std::size_t caret) {
    block.text_value = std::move(value);
    block.caret_offset = std::min(caret, block.text_value.size());
    block.selection_anchor = block.caret_offset;
    block.selection_focus = block.caret_offset;
    block.placeholder_visible = false;
    block.text = text_control_display_value(block, block.text_value);
    if (block.text.empty() && !block.placeholder.empty()) {
        block.text = block.placeholder;
        block.placeholder_visible = true;
    }

    if (auto* elem = element_for_block(impl, idx)) {
        auto* node = lxb_dom_interface_node(elem);
        impl.text_layout_signatures.erase(node);
        impl.live_text_values[node] = block.text_value;
        impl.live_text_carets[node] = block.caret_offset;
        impl.live_text_selections[node] = {
            block.selection_anchor, block.selection_focus};
    }
}

std::string emitted_text_control_value(const Block& block) {
    return block.placeholder_visible ? std::string{} : block.text_value;
}

bool command_modifier(const Event& ev) {
    return ev.ctrl || ev.super;
}

bool text_word_byte(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) return false;
    const unsigned char c = static_cast<unsigned char>(text[pos]);
    return c >= 0x80u || std::isalnum(c) || c == '_';
}

std::size_t previous_word_boundary(std::string_view text, std::size_t pos) {
    pos = std::min(pos, text.size());
    while (pos > 0) {
        const std::size_t prev = previous_utf8_boundary(text, pos);
        if (text_word_byte(text, prev)) break;
        pos = prev;
    }
    while (pos > 0) {
        const std::size_t prev = previous_utf8_boundary(text, pos);
        if (!text_word_byte(text, prev)) break;
        pos = prev;
    }
    return pos;
}

std::size_t next_word_boundary(std::string_view text, std::size_t pos) {
    pos = std::min(pos, text.size());
    while (pos < text.size() && text_word_byte(text, pos)) {
        pos = next_utf8_boundary(text, pos);
    }
    while (pos < text.size() && !text_word_byte(text, pos)) {
        pos = next_utf8_boundary(text, pos);
    }
    return pos;
}

std::string selected_text(const Block& block) {
    const auto [begin, end] = normalized_selection(block);
    if (begin == end) return {};
    return emitted_text_control_value(block).substr(begin, end - begin);
}

std::string clipboard_get_text(detail::DocumentImpl& impl) {
    if (impl.clipboard_get) {
        try {
            return impl.clipboard_get();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "AffineUI clipboard get failed: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "AffineUI clipboard get failed\n");
        }
    }
    return impl.fallback_clipboard;
}

void clipboard_set_text(detail::DocumentImpl& impl, std::string_view text) {
    impl.fallback_clipboard.assign(text);
    if (impl.clipboard_set) {
        try {
            impl.clipboard_set(text);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "AffineUI clipboard set failed: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "AffineUI clipboard set failed\n");
        }
    }
}

void emit_text_control_change(detail::DocumentImpl& impl, int idx, Block& block) {
    if (auto* elem = element_for_block(impl, idx)) {
        emit_widget_change(impl, elem, emitted_text_control_value(block));
    }
}

bool delete_text_range(detail::DocumentImpl& impl,
                       int idx,
                       Block& block,
                       std::size_t begin,
                       std::size_t end) {
    std::string next = emitted_text_control_value(block);
    std::size_t caret = block.caret_offset;
    const std::string old = next;
    next = erase_selected_text(std::move(next), begin, end, caret);
    if (next == old && caret == block.caret_offset) return false;
    const Rect old_rect = subtree_visual_rect(impl, idx);
    set_live_text_state(impl, idx, block, std::move(next), caret);
    mark_live_mutation_dirty(impl, idx, old_rect, /*needs_layout=*/true);
    emit_text_control_change(impl, idx, block);
    return true;
}

bool replace_text_selection_or_insert(detail::DocumentImpl& impl,
                                      int idx,
                                      Block& block,
                                      std::string_view text) {
    if (text.empty()) return false;
    const Rect old_rect = subtree_visual_rect(impl, idx);
    std::string next = emitted_text_control_value(block);
    std::size_t caret = block.caret_offset;
    if (has_text_selection(block)) {
        const auto [begin, end] = normalized_selection(block);
        next = replace_selected_text(std::move(next), begin, end, text, caret);
    } else {
        next = insert_text_at_caret(std::move(next), caret, text);
    }
    set_live_text_state(impl, idx, block, std::move(next), caret);
    mark_live_mutation_dirty(impl, idx, old_rect, /*needs_layout=*/true);
    emit_text_control_change(impl, idx, block);
    return true;
}

bool move_text_caret(detail::DocumentImpl& impl,
                     int idx,
                     Block& block,
                     std::size_t caret,
                     bool extend_selection) {
    caret = std::min(caret, block.text_value.size());
    const std::size_t anchor = extend_selection
        ? (has_text_selection(block) ? block.selection_anchor
                                     : block.caret_offset)
        : caret;
    if (caret == block.caret_offset &&
        anchor == block.selection_anchor &&
        caret == block.selection_focus) {
        return false;
    }
    set_text_selection(impl, idx, block, anchor, caret);
    add_dirty_rect(impl, block_visual_rect(impl, idx));
    return true;
}

bool apply_deferred_text_focus(detail::DocumentImpl& impl,
                               const detail::DocumentImpl::LiveControlDrag& drag,
                               Point point) {
    if (!drag.defer_text_focus || drag.focus_idx < 0 ||
        drag.focus_idx >= static_cast<int>(impl.blocks.size())) {
        return false;
    }
    bool changed = set_focus(impl, drag.focus_idx);
    changed = set_text_caret_from_point(impl, drag.focus_idx, point) || changed;
    return changed;
}
#else  // stub build â€” no DOM, no pseudo / scroll bookkeeping
bool refresh_hover_chain(detail::DocumentImpl&)  { return false; }
bool refresh_active_chain(detail::DocumentImpl&) { return false; }
bool set_focus(detail::DocumentImpl&, int)       { return false; }
int  focusable_ancestor(const detail::DocumentImpl&, int) { return -1; }
int  find_scrollable_y_ancestor(const detail::DocumentImpl&, int) { return -1; }
bool focused_text_control(detail::DocumentImpl&, Block*&) { return false; }
bool set_text_caret_from_point(detail::DocumentImpl&, int, Point, bool = false,
                               std::size_t = 0) { return false; }
void remove_last_utf8_codepoint(std::string&) {}
void set_live_text_value(detail::DocumentImpl&, int, Block&, std::string) {}
void set_live_text_state(detail::DocumentImpl&, int, Block&, std::string,
                         std::size_t) {}
std::string emitted_text_control_value(const Block&) { return {}; }
#endif
}  // namespace

DispatchResult Document::dispatch(const Event& ev) {
    DispatchResult result{};
    auto ensure_interaction_layout = [&]() {
#if !defined(AFFINEUI_STUB_BUILD)
        if (impl_->content_size.width == 0 &&
            impl_->media_viewport_width_px > 0 &&
            impl_->last_measurer != nullptr) {
            layout(impl_->media_viewport_width_px,
                   impl_->media_viewport_height_px, impl_->last_measurer);
        }
#endif
    };
    switch (ev.type) {
        case EventType::MouseMove: {
            impl_->last_mouse_pos = ev.pos;
            if (impl_->scrollbar_drag.block_idx >= 0) {
                if (scrollbar_scroll_from_thumb_y(
                        *impl_,
                        impl_->scrollbar_drag.block_idx,
                        ev.pos.y - impl_->scrollbar_drag.thumb_offset_y)) {
                    result.redraw_requested = true;
                }
                break;
            }
#if !defined(AFFINEUI_STUB_BUILD)
            if (impl_->splitter_drag.block_idx >= 0) {
                if (update_splitter_drag(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                break;
            }
            if (impl_->float_drag.elem) {
                if (update_float_drag(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                break;
            }
            // A pressed tab becomes a drag once it moves past a small threshold;
            // while dragging, show the drop indicator for the hovered zone.
            if (!impl_->tab_drag.tab &&
                !impl_->pending_tab_press.panel_id.empty()) {
                const int dx = ev.pos.x - impl_->pending_tab_press.start_x;
                const int dy = ev.pos.y - impl_->pending_tab_press.start_y;
                if (dx * dx + dy * dy > 36) {
                    ensure_interaction_layout();
                    if (arm_tab_drag_from_pending_press(
                            *impl_, impl_->pending_tab_press)) {
                        impl_->tab_drag.dragging = true;
                    }
                    impl_->pending_tab_press = {};
                }
            }
            if (impl_->tab_drag.tab) {
                if (!impl_->tab_drag.dragging) {
                    const int dx = ev.pos.x - impl_->tab_drag.start_x;
                    const int dy = ev.pos.y - impl_->tab_drag.start_y;
                    if (dx * dx + dy * dy > 36) impl_->tab_drag.dragging = true;
                }
                if (impl_->tab_drag.dragging) {
                    ensure_interaction_layout();
                    const auto t = compute_drop_target(
                        *impl_, ev.pos, dock_kind_of(impl_->tab_drag.pane));
                    const bool indicator_was_visible =
                        impl_->tab_drag.drop_indicator_visible;
                    impl_->tab_drag.drop_valid = t.valid;
                    impl_->tab_drag.drop_parent = t.valid ? t.parent : std::string();
                    impl_->tab_drag.drop_zone =
                        t.valid ? static_cast<int>(t.zone)
                                : static_cast<int>(DropZone::None);
                    impl_->tab_drag.drop_x = t.x;
                    impl_->tab_drag.drop_y = t.y;
                    impl_->tab_drag.drop_w = t.w;
                    impl_->tab_drag.drop_h = t.h;
                    impl_->tab_drag.drop_indicator_visible = t.valid;
                    if (set_drop_indicator(*impl_, t.valid ? &t : nullptr) ||
                        indicator_was_visible != t.valid) {
                        result.redraw_requested = true;
                    }
                }
            }
            if (impl_->ui_control_script_attached &&
                impl_->live_drag.kind != LiveControlKind::None) {
                if (update_active_live_control(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                break;
            }
#endif
            if (impl_->text_selection_drag_idx >= 0) {
                if (set_text_caret_from_point(
                        *impl_, impl_->text_selection_drag_idx, ev.pos,
                        true, impl_->text_selection_drag_anchor)) {
                    result.redraw_requested = true;
                }
                break;
            }
            const int new_hover = hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
            if (new_hover != impl_->hovered_idx) {
                impl_->hovered_idx      = new_hover;
                result.redraw_requested = true;
            }
            // Refresh :hover chain even when hovered_idx didn't change â€”
            // mouse may have moved within the same leaf block (no-op
            // here) or the tree may have churned underneath us (rare,
            // but cheap to verify).
            if (refresh_hover_chain(*impl_)) {
                result.redraw_requested = true;
            }
            break;
        }
        case EventType::MouseDown: {
            impl_->last_mouse_pos = ev.pos;
            impl_->hovered_idx    = hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
            impl_->mouse_down_consumed_release = false;
            impl_->text_selection_drag_idx = -1;
            impl_->scrollbar_drag = {};
#if !defined(AFFINEUI_STUB_BUILD)
            impl_->splitter_drag = {};
            impl_->pending_tab_press = {};
            impl_->pressed_button = nullptr;
#endif
            if (ev.button == MouseButton::Left) {
                int scrollbar_idx = -1;
                ScrollbarGeometry scrollbar{};
                if (find_vertical_scrollbar_at(
                        *impl_, ev.pos, scrollbar_idx, scrollbar)) {
                    impl_->scrollbar_drag.block_idx = scrollbar_idx;
                    impl_->scrollbar_drag.start_y = ev.pos.y;
                    impl_->scrollbar_drag.start_scroll_y =
                        impl_->blocks[static_cast<std::size_t>(
                            scrollbar_idx)].scroll_y;
                    impl_->scrollbar_drag.thumb_offset_y =
                        rect_contains(scrollbar.thumb, ev.pos.x, ev.pos.y)
                            ? ev.pos.y - scrollbar.thumb.y
                            : scrollbar.thumb.h / 2;
                    if (!rect_contains(scrollbar.thumb, ev.pos.x, ev.pos.y) &&
                        scrollbar_scroll_from_thumb_y(
                            *impl_, scrollbar_idx,
                            ev.pos.y -
                                impl_->scrollbar_drag.thumb_offset_y)) {
                        result.redraw_requested = true;
                    }
                    break;
                }
            }
#if !defined(AFFINEUI_STUB_BUILD)
            // Dock splitter grab takes priority over anything inside the panes
            // (the splitter is a thin element the pointer lands on directly).
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left) {
                detail::DocumentImpl::SplitterDrag sd{};
                if (find_splitter_at(*impl_, impl_->hovered_idx, ev.pos, sd)) {
                    impl_->splitter_drag = sd;
                    impl_->splitter_drag.start_pos =
                        sd.horizontal ? ev.pos.y : ev.pos.x;
                    if (auto* selem =
                            element_for_block(*impl_, sd.block_idx)) {
                        set_element_class(*impl_, selem,
                                          "dcs-splitter--active", true);
                    }
                    break;
                }
            }
            // Floating toolbar / panel grab: a [data-dcs-drag] container dragged
            // by a [data-dcs-drag-handle] inside it. Falls through if the press
            // wasn't on a handle, so the toolbar's own buttons still click.
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left) {
                detail::DocumentImpl::FloatDrag fd{};
                if (find_float_drag_at(*impl_, impl_->hovered_idx, ev.pos, fd)) {
                    impl_->float_drag = fd;
                    result.redraw_requested = true;
                    break;
                }
            }
#endif
            // :active follows the press: set to whatever's under the
            // pointer right now, refresh the active chain so the bit
            // toggles on and an immediate restyle visualizes the press.
            impl_->active_idx     = impl_->hovered_idx;
            const bool h = refresh_hover_chain(*impl_);
            const bool a = refresh_active_chain(*impl_);
#if !defined(AFFINEUI_STUB_BUILD)
            detail::DocumentImpl::LiveControlDrag pending_live_drag{};
            const bool has_pending_live_drag =
                impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left &&
                find_live_control_at(*impl_, impl_->hovered_idx, ev.pos,
                                     pending_live_drag);
            const bool defer_text_focus =
                has_pending_live_drag &&
                pending_live_drag.kind == LiveControlKind::NumericInput;
            const bool resize_textarea =
                has_pending_live_drag &&
                pending_live_drag.kind == LiveControlKind::TextAreaResize;
#else
            const bool defer_text_focus = false;
            const bool resize_textarea = false;
#endif
            // Focus moves to the nearest focusable ancestor of whatever
            // the press landed on. Clicking blank space (no focusable
            // ancestor) clears focus, matching browser behavior for
            // mousedown outside any form control.
            const int target = focusable_ancestor(*impl_, impl_->hovered_idx);
            bool f = false;
            bool caret = false;
            if (!defer_text_focus && !resize_textarea) {
                f = set_focus(*impl_, target);
                caret = set_text_caret_from_point(*impl_, target, ev.pos);
            }
            if (!defer_text_focus && !resize_textarea && target >= 0 &&
                target < static_cast<int>(impl_->blocks.size()) &&
                impl_->blocks[static_cast<std::size_t>(target)].text_control) {
                auto& block = impl_->blocks[static_cast<std::size_t>(target)];
                const auto now = std::chrono::steady_clock::now();
                const bool double_click =
                    impl_->last_text_click_valid &&
                    impl_->last_text_click_idx == target &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - impl_->last_text_click_time).count() < 500 &&
                    std::abs(ev.pos.x - impl_->last_text_click_pos.x) <= 4 &&
                    std::abs(ev.pos.y - impl_->last_text_click_pos.y) <= 4;
                if (double_click && !block.text_value.empty()) {
                    const auto [begin, end] =
                        word_bounds_at(block.text_value, block.caret_offset);
                    set_text_selection(*impl_, target, block, begin, end);
                    add_dirty_rect(*impl_, block_visual_rect(*impl_, target));
                    caret = true;
                    impl_->last_text_click_valid = false;
                } else {
                    impl_->last_text_click_valid = true;
                    impl_->last_text_click_idx = target;
                    impl_->last_text_click_pos = ev.pos;
                    impl_->last_text_click_time = now;
                    if (ev.button == MouseButton::Left) {
                        impl_->text_selection_drag_idx = target;
                        impl_->text_selection_drag_anchor =
                            block.caret_offset;
                    }
                }
            } else {
                impl_->text_selection_drag_idx = -1;
            }
            if (h || a || f || caret) result.redraw_requested = true;
#if !defined(AFFINEUI_STUB_BUILD)
            // Collapsibles (foldout/subpanel headers), tree chevrons, and
            // selectable rows resolve on PRESS for the same immediate feel as
            // tabs. A chevron press consumes row selection so expand/collapse
            // does not also select the tree row underneath it.
            bool press_consumed_by_collapse = false;
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left && !has_pending_live_drag) {
                if (toggle_decius_collapse_control(*impl_, impl_->hovered_idx) ||
                    toggle_dcs_tree_chevron_control(*impl_, impl_->hovered_idx)) {
                    result.redraw_requested = true;
                    press_consumed_by_collapse = true;
                }
                if (!press_consumed_by_collapse) {
                    lxb_dom_element_t* select_box = nullptr;
                    lxb_dom_element_t* select_row = nullptr;
                    if (find_dcs_select_row_at(*impl_, impl_->hovered_idx,
                                               select_box, select_row) &&
                        update_dcs_select_control(*impl_, select_box,
                                                  select_row, ev)) {
                        result.redraw_requested = true;
                    }
                }
            }
            // Decius menu triggers/items SELECT on press. Opening a menubar
            // menu is selection; leaf item activation still happens on release.
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left && !has_pending_live_drag) {
                lxb_dom_element_t* menu_elem = nullptr;
                lxb_dom_element_t* menu_item = nullptr;
                lxb_dom_element_t* trigger_elem = nullptr;
                lxb_dom_element_t* dropdown_group = nullptr;
                lxb_dom_element_t* dropdown_select = nullptr;
                lxb_dom_element_t* dropdown_option = nullptr;
                const bool over_dropdown =
                    find_dropdown_control_at(*impl_, impl_->hovered_idx,
                                             dropdown_group, dropdown_select,
                                             dropdown_option);
                bool consume_release = false;
                if (!over_dropdown &&
                    find_dcs_menu_item_at(*impl_, impl_->hovered_idx,
                                          menu_elem, menu_item)) {
                    if (press_dcs_menu_item(*impl_, menu_item)) {
                        result.redraw_requested = true;
                    }
                } else if (!over_dropdown &&
                           find_dcs_menu_trigger_at(*impl_, impl_->hovered_idx,
                                                    trigger_elem, menu_elem)) {
                    if (toggle_dcs_menu(*impl_, trigger_elem, menu_elem)) {
                        result.redraw_requested = true;
                    }
                    consume_release = true;
                }
                if (consume_release) {
                    impl_->mouse_down_consumed_release = true;
                }
            }
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left && !has_pending_live_drag) {
                lxb_dom_element_t* button_elem = nullptr;
                if (find_button_control_at(*impl_, impl_->hovered_idx,
                                           button_elem)) {
                    impl_->pressed_button = button_elem;
                }
            }
            // Dock-pane tab: SELECT IMMEDIATELY on press so it feels responsive
            // (first click always selects — the standard drag/drop rule), then
            // arm a potential drag. If the pointer then moves out of the pane it
            // tears off; if not, the press already did its job (selection).
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left) {
                lxb_dom_element_t* tab_elem = nullptr;
                if (find_dockpane_tab_at(*impl_, impl_->hovered_idx, tab_elem)) {
                    const bool switched = switch_dockpane_tab(*impl_, tab_elem);
                    if (switched) {
                        result.invalidate_view = true;
                        result.layout_changed = true;
                        result.redraw_requested = true;
                    }
                    auto* pane = ancestor_with_class(tab_elem, "dcs-dockpane");
                    const std::string panel_id = dockpane_tab_panel_id(tab_elem);
                    impl_->pending_tab_press.panel_id = panel_id;
                    impl_->pending_tab_press.start_x = ev.pos.x;
                    impl_->pending_tab_press.start_y = ev.pos.y;
                    impl_->pending_tab_press.switched_on_down = switched;
                    if (dock_kind_of(pane) == "documents") {
                        break;
                    }
                    impl_->tab_drag = {};
                    impl_->tab_drag.tab = tab_elem;
                    impl_->tab_drag.pane = pane;
                    impl_->tab_drag.panel_id = panel_id;
                    impl_->tab_drag.start_x = ev.pos.x;
                    impl_->tab_drag.start_y = ev.pos.y;
                    impl_->tab_drag.switched_on_down = switched;
                }
            }
            if (has_pending_live_drag) {
                impl_->live_drag = pending_live_drag;
                impl_->live_drag.start_x = ev.pos.x;
                impl_->live_drag.start_y = ev.pos.y;
                impl_->live_drag.last_x = ev.pos.x;
                if (impl_->live_drag.kind == LiveControlKind::NumericInput) {
                    impl_->live_drag.defer_text_focus = true;
                    impl_->live_drag.focus_idx = target;
                    impl_->live_drag.focus_point = ev.pos;
                }
                if (impl_->live_drag.kind != LiveControlKind::AuiKnob &&
                    impl_->live_drag.kind != LiveControlKind::DeciusKnob &&
                    impl_->live_drag.kind != LiveControlKind::NumericInput &&
                    impl_->live_drag.kind != LiveControlKind::TextAreaResize &&
                    update_active_live_control(*impl_, ev)) {
                    result.redraw_requested = true;
                }
            }
#endif
            break;
        }
        case EventType::MouseUp: {
            impl_->last_mouse_pos = ev.pos;
            if (impl_->scrollbar_drag.block_idx >= 0) {
                if (scrollbar_scroll_from_thumb_y(
                        *impl_,
                        impl_->scrollbar_drag.block_idx,
                        ev.pos.y - impl_->scrollbar_drag.thumb_offset_y)) {
                    result.redraw_requested = true;
                }
                impl_->scrollbar_drag = {};
                break;
            }
#if !defined(AFFINEUI_STUB_BUILD)
            if (impl_->splitter_drag.block_idx >= 0) {
                const bool persist_layout = impl_->splitter_drag.persist_layout;
                if (update_splitter_drag(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                if (auto* selem =
                        element_for_block(*impl_, impl_->splitter_drag.block_idx)) {
                    if (set_element_class(*impl_, selem, "dcs-splitter--active",
                                          false)) {
                        result.redraw_requested = true;
                    }
                }
                impl_->splitter_drag = {};
                // A pane was resized — let the app persist the new dock layout.
                result.layout_changed = persist_layout;
                break;
            }
            if (impl_->float_drag.elem) {
                if (update_float_drag(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                const auto fd = impl_->float_drag;
                impl_->float_drag = {};
                if (set_drop_indicator(*impl_, nullptr)) {  // hide
                    result.redraw_requested = true;
                }
                if (!fd.panel_id.empty()) {
                    // Moving floating chrome only moves the panel. Re-docking is
                    // handled by dragging the dock tab/title, mirroring
                    // decius.js and preventing accidental reparents while a
                    // user is simply repositioning a tearoff.
                    int nx = fd.elem_doc_x + (ev.pos.x - fd.start_x);
                    int ny = fd.elem_doc_y + (ev.pos.y - fd.start_y);
                    if (fd.bounds_w > 0)
                        nx = std::clamp(nx, fd.bounds_x,
                                        std::max(fd.bounds_x, fd.bounds_x +
                                                                  fd.bounds_w -
                                                                  fd.elem_w));
                    if (fd.bounds_h > 0)
                        ny = std::clamp(ny, fd.bounds_y,
                                        std::max(fd.bounds_y, fd.bounds_y +
                                                                  fd.bounds_h -
                                                                  fd.elem_h));
                    Document::DockPlacement p;
                    p.present = true;
                    p.floating = true;
                    p.x = nx - fd.cb_x;
                    p.y = ny - fd.cb_y;
                    p.w = fd.elem_w;
                    p.h = fd.elem_h;
                    impl_->dock_overrides[fd.panel_id] = p;
                    result.layout_changed = true;  // re-seed + rebuild
                    result.redraw_requested = true;
                } else {
                    // A non-dockable float (e.g. a toolbar): in-session move only
                    // (no override store yet — see the float-position follow-up).
                    result.redraw_requested = true;
                }
                break;
            }
            // Dock-pane tab release: the tab was already selected on press; here
            // we only complete a DRAG — a drag that ends outside the pane tears
            // the panel off into a floating panel.
            if (impl_->tab_drag.tab) {
                const auto td = impl_->tab_drag;
                impl_->tab_drag = {};
                impl_->pending_tab_press = {};
                if (td.dragging) {
                    if (set_drop_indicator(*impl_, nullptr)) {  // hide
                        result.redraw_requested = true;
                    }
                    bool changed_dock = false;
                    // Dropped on a dock zone of another pane -> dock there.
                    DropTarget t;
                    if (td.drop_valid) {
                        t.valid = true;
                        t.parent = td.drop_parent;
                        t.zone = static_cast<DropZone>(td.drop_zone);
                        t.x = td.drop_x;
                        t.y = td.drop_y;
                        t.w = td.drop_w;
                        t.h = td.drop_h;
                    } else {
                        ensure_interaction_layout();
                        t = compute_drop_target(
                            *impl_, ev.pos, dock_kind_of(td.pane));
                    }
                    if (t.valid) {
                        bool source_center_noop = false;
                        if (t.zone == DropZone::Tab && td.pane) {
                            const std::string source_id = pane_panel_id(td.pane);
                            source_center_noop =
                                !source_id.empty() && source_id == t.parent;
                        }
                        changed_dock = source_center_noop
                                           ? false
                                           : apply_dock(*impl_, td.panel_id, t);
                    } else {
                        // Not over a zone: tear off if released outside the pane.
                        bool outside = true;
                        if (td.pane) {
                            const int pi =
                                block_index_for_exact_element(*impl_, td.pane);
                            if (pi >= 0) {
                                const auto& pb =
                                    impl_->blocks[static_cast<std::size_t>(pi)]
                                        .bounds;
                                outside =
                                    !(ev.pos.x >= pb.x && ev.pos.x < pb.x + pb.w &&
                                      ev.pos.y >= pb.y && ev.pos.y < pb.y + pb.h);
                            }
                        }
                        if (outside)
                            changed_dock = tear_off_panel(*impl_, td.panel_id,
                                                          td.pane, ev.pos);
                    }
                    if (changed_dock) {
                        result.layout_changed = true;  // app re-seeds + rebuilds
                        result.redraw_requested = true;
                    }
                }
                break;  // tab interaction consumed this release
            }
            if (!impl_->pending_tab_press.panel_id.empty()) {
                impl_->pending_tab_press = {};
                break;  // tab press survived a rebuild; release is consumed
            }
#endif
            bool released_live_control = false;
#if !defined(AFFINEUI_STUB_BUILD)
            if (impl_->ui_control_script_attached &&
                impl_->live_drag.kind != LiveControlKind::None) {
                const auto released_drag = impl_->live_drag;
                if (update_active_live_control(*impl_, ev)) {
                    result.redraw_requested = true;
                }
                if (released_drag.kind == LiveControlKind::NumericInput &&
                    !impl_->live_drag.moved &&
                    apply_deferred_text_focus(*impl_, released_drag, ev.pos)) {
                    result.redraw_requested = true;
                }
                released_live_control = true;
                impl_->live_drag = {};
            }
#endif
            impl_->hovered_idx    = hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
            impl_->text_selection_drag_idx = -1;
            // Clear :active on every MouseUp â€” the press is over. We
            // don't try to be clever about "release outside the
            // pressed element" today; that nuance is part of the
            // click-state machinery to layer in later.
            impl_->active_idx     = -1;
            const bool h = refresh_hover_chain(*impl_);
            const bool a = refresh_active_chain(*impl_);
            if (h || a) result.redraw_requested = true;
            auto* pressed_menu_item = impl_->pressed_dcs_menu_item;
            const auto pressed_menu_item_bounds =
                impl_->pressed_dcs_menu_item_bounds;
            const bool pressed_menu_item_in_bounds =
                pressed_menu_item &&
                rect_contains(pressed_menu_item_bounds, ev.pos.x, ev.pos.y);
            if (clear_pressed_dcs_menu_item(*impl_)) {
                result.redraw_requested = true;
            }
            auto* pressed_button = impl_->pressed_button;
            impl_->pressed_button = nullptr;
            bool activated_pressed_menu_item = false;
            if (pressed_menu_item_in_bounds) {
                if (auto* menu_elem =
                        ancestor_with_class(pressed_menu_item, "dcs-menu")) {
                    if (activate_dcs_menu_item(*impl_, menu_elem,
                                               pressed_menu_item)) {
                        result.redraw_requested = true;
                    }
                    activated_pressed_menu_item = true;
                }
            }
            const bool release_consumed_on_down =
                impl_->mouse_down_consumed_release;
            impl_->mouse_down_consumed_release = false;
#if !defined(AFFINEUI_STUB_BUILD)
            if (release_consumed_on_down || activated_pressed_menu_item) {
                break;
            }
            if (impl_->ui_control_script_attached &&
                ev.button == MouseButton::Left && !released_live_control) {
                if (!click_preserves_transient_layers(*impl_, impl_->hovered_idx) &&
                    close_transient_layers(*impl_)) {
                    result.redraw_requested = true;
                    impl_->hovered_idx =
                        hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
                    if (refresh_hover_chain(*impl_)) {
                        result.redraw_requested = true;
                    }
                }
                bool toggled_checkbox = false;
                int check_idx = -1;
                lxb_dom_element_t* check_elem = nullptr;
                if (find_checkbox_control_at(*impl_, impl_->hovered_idx,
                                             check_idx, check_elem) &&
                    toggle_checkbox_control(*impl_, check_idx, check_elem)) {
                    result.redraw_requested = true;
                    toggled_checkbox = true;
                }
                bool changed_button_group = false;
                bool changed_dropdown = false;
                if (!toggled_checkbox) {
                    lxb_dom_element_t* dropdown_group = nullptr;
                    lxb_dom_element_t* dropdown_select = nullptr;
                    lxb_dom_element_t* dropdown_option = nullptr;
                    if (find_dropdown_control_at(*impl_, impl_->hovered_idx,
                                                 dropdown_group,
                                                 dropdown_select,
                                                 dropdown_option)) {
                        if (dropdown_option &&
                            update_dropdown_control(*impl_, dropdown_group,
                                                    dropdown_option)) {
                            result.redraw_requested = true;
                            changed_dropdown = true;
                        } else if (dropdown_select &&
                                   toggle_dropdown_menu(*impl_,
                                                        dropdown_group)) {
                            result.redraw_requested = true;
                            changed_dropdown = true;
                        }
                    }
                }
                if (!toggled_checkbox && !changed_dropdown) {
                    lxb_dom_element_t* group_elem = nullptr;
                    lxb_dom_element_t* option_elem = nullptr;
                    if (find_button_group_option_at(*impl_, impl_->hovered_idx,
                                                    group_elem, option_elem) &&
                        update_button_group_control(*impl_, group_elem,
                                                    option_elem)) {
                        result.redraw_requested = true;
                        changed_button_group = true;
                    }
                }
                bool changed_menu = false;
                if (!toggled_checkbox && !changed_dropdown &&
                    !changed_button_group) {
                    lxb_dom_element_t* menu_elem = nullptr;
                    lxb_dom_element_t* menu_item = nullptr;
                    lxb_dom_element_t* trigger_elem = nullptr;
                    if (find_dcs_menu_item_at(*impl_, impl_->hovered_idx,
                                              menu_elem, menu_item)) {
                        if (menu_item == pressed_menu_item &&
                            activate_dcs_menu_item(*impl_, menu_elem,
                                                   menu_item)) {
                            result.redraw_requested = true;
                        }
                        changed_menu = true;
                    } else if (find_dcs_menu_trigger_at(*impl_,
                                                       impl_->hovered_idx,
                                                       trigger_elem,
                                                       menu_elem)) {
                        if (toggle_dcs_menu(*impl_, trigger_elem,
                                            menu_elem)) {
                            result.redraw_requested = true;
                        }
                        changed_menu = true;
                    }
                }
                bool changed_popover = false;
                if (!toggled_checkbox && !changed_dropdown &&
                    !changed_button_group && !changed_menu) {
                    lxb_dom_element_t* trigger_elem = nullptr;
                    lxb_dom_element_t* popover_elem = nullptr;
                    if (find_dcs_popover_trigger_at(*impl_,
                                                    impl_->hovered_idx,
                                                    trigger_elem,
                                                    popover_elem)) {
                        if (toggle_dcs_popover(*impl_, trigger_elem,
                                               popover_elem)) {
                            result.redraw_requested = true;
                        }
                        changed_popover = true;
                    }
                }
                // NB: foldout/subpanel collapse and tree-chevron expand are
                // handled on MouseDown (press) — see the MouseDown case — so
                // they are intentionally absent here.
                bool changed_selection = false;
                if (!toggled_checkbox && !changed_dropdown &&
                    !changed_button_group && !changed_menu &&
                    !changed_popover) {
                    lxb_dom_element_t* select_box = nullptr;
                    lxb_dom_element_t* select_row = nullptr;
                    if (find_dcs_select_row_at(*impl_, impl_->hovered_idx,
                                               select_box, select_row)) {
                        changed_selection = true;
                    }
                }
                // NB: dock-pane tab selection is handled by the tab-drag release
                // path above (a clean click selects; a drag tears off), so it is
                // intentionally absent from this click chain.
                if (!toggled_checkbox && !changed_dropdown &&
                    !changed_button_group && !changed_menu &&
                    !changed_popover && !changed_selection) {
                    lxb_dom_element_t* button_elem = nullptr;
                    if (find_button_control_at(*impl_, impl_->hovered_idx,
                                               button_elem) &&
                        button_elem == pressed_button &&
                        activate_button_control(*impl_, button_elem)) {
                        result.redraw_requested = true;
                    }
                }
            }
#endif
            break;
        }
#if !defined(AFFINEUI_STUB_BUILD)
        case EventType::KeyDown: {
            // ESC clears focus, matching the convention browsers use for
            // dismissing a focused control.
            if (ev.key == Key::Escape) {
#if !defined(AFFINEUI_STUB_BUILD)
                if (impl_->ui_control_script_attached &&
                    close_transient_layers(*impl_)) {
                    result.redraw_requested = true;
                }
#endif
                if (set_focus(*impl_, -1)) result.redraw_requested = true;
                break;
            }

            Block* control = nullptr;
            if (!focused_text_control(*impl_, control)) break;
            const int idx = impl_->focused_idx;
            const auto text = emitted_text_control_value(*control);
            const bool command = command_modifier(ev);

            if (command && ev.key == Key::A) {
                if (move_text_caret(*impl_, idx, *control, text.size(), true)) {
                    set_text_selection(*impl_, idx, *control, 0, text.size());
                    add_dirty_rect(*impl_, block_visual_rect(*impl_, idx));
                    result.redraw_requested = true;
                } else if (!has_text_selection(*control) && !text.empty()) {
                    set_text_selection(*impl_, idx, *control, 0, text.size());
                    add_dirty_rect(*impl_, block_visual_rect(*impl_, idx));
                    result.redraw_requested = true;
                }
            } else if (command && ev.key == Key::C) {
                if (has_text_selection(*control)) {
                    clipboard_set_text(*impl_, selected_text(*control));
                }
            } else if (command && ev.key == Key::X) {
                if (has_text_selection(*control)) {
                    clipboard_set_text(*impl_, selected_text(*control));
                    const auto [begin, end] = normalized_selection(*control);
                    result.redraw_requested =
                        delete_text_range(*impl_, idx, *control, begin, end);
                }
            } else if (command && ev.key == Key::V) {
                const std::string paste = clipboard_get_text(*impl_);
                result.redraw_requested =
                    replace_text_selection_or_insert(*impl_, idx, *control, paste);
            } else if (ev.key == Key::Backspace) {
                if (has_text_selection(*control)) {
                    const auto [begin, end] = normalized_selection(*control);
                    result.redraw_requested =
                        delete_text_range(*impl_, idx, *control, begin, end);
                } else if (command) {
                    const std::size_t begin =
                        previous_word_boundary(text, control->caret_offset);
                    result.redraw_requested =
                        delete_text_range(*impl_, idx, *control, begin,
                                          control->caret_offset);
                } else {
                    std::size_t begin =
                        previous_utf8_boundary(text, control->caret_offset);
                    result.redraw_requested =
                        delete_text_range(*impl_, idx, *control, begin,
                                          control->caret_offset);
                }
            } else if (ev.key == Key::Delete) {
                if (has_text_selection(*control)) {
                    const auto [begin, end] = normalized_selection(*control);
                    result.redraw_requested =
                        delete_text_range(*impl_, idx, *control, begin, end);
                } else if (command) {
                    const std::size_t end =
                        next_word_boundary(text, control->caret_offset);
                    result.redraw_requested =
                        delete_text_range(*impl_, idx, *control,
                                          control->caret_offset, end);
                } else {
                    const std::size_t end =
                        next_utf8_boundary(text, control->caret_offset);
                    result.redraw_requested =
                        delete_text_range(*impl_, idx, *control,
                                          control->caret_offset, end);
                }
            } else if (ev.key == Key::ArrowLeft ||
                       ev.key == Key::ArrowRight ||
                       ev.key == Key::Home ||
                       ev.key == Key::End) {
                std::size_t caret = control->caret_offset;
                if (!ev.shift && has_text_selection(*control) &&
                    ev.key == Key::ArrowLeft) {
                    caret = normalized_selection(*control).first;
                } else if (!ev.shift && has_text_selection(*control) &&
                           ev.key == Key::ArrowRight) {
                    caret = normalized_selection(*control).second;
                } else if (ev.key == Key::ArrowLeft) {
                    caret = command
                        ? previous_word_boundary(text, caret)
                        : previous_utf8_boundary(text, caret);
                } else if (ev.key == Key::ArrowRight) {
                    caret = command
                        ? next_word_boundary(text, caret)
                        : next_utf8_boundary(text, caret);
                } else if (ev.key == Key::Home) {
                    caret = 0;
                } else {
                    caret = text.size();
                }
                if (move_text_caret(*impl_, idx, *control, caret, ev.shift)) {
                    result.redraw_requested = true;
                }
            }
            break;
        }
        case EventType::TextInput: {
            Block* control = nullptr;
            if (focused_text_control(*impl_, control) && !ev.text.empty()) {
                result.redraw_requested =
                    replace_text_selection_or_insert(
                        *impl_, impl_->focused_idx, *control, ev.text);
            }
            break;
        }
        case EventType::MouseWheel: {
            // Route to the nearest scrollable-Y ancestor of whatever
            // the pointer is over. Convention: positive wheel_dy
            // scrolls content up (i.e. scroll position increases).
            // The platform adapter is responsible for normalizing
            // direction + step size before we get here.
            const int wheel_hover =
                hit_test_blocks(*impl_, ev.pos.x, ev.pos.y);
            const int target = find_scrollable_y_ancestor(
                *impl_, wheel_hover >= 0 ? wheel_hover : impl_->hovered_idx);
            if (target < 0) break;
            auto& sb = impl_->blocks[static_cast<std::size_t>(target)];
            constexpr int kPxPerWheelStep = 24;
            const int delta = static_cast<int>(
                -ev.wheel_dy * kPxPerWheelStep);
            const int max_scroll = std::max(0, sb.content_h - sb.bounds.h);
            const int next       = std::clamp(sb.scroll_y + delta,
                                              0, max_scroll);
            if (set_block_scroll_y(*impl_, target, next)) {
                result.redraw_requested = true;
            }
            break;
        }
#endif
        default:
            break;
    }
    return result;
}

namespace {
// Walk from the hovered block up the parent chain, returning the
// nearest non-default cursor. CSS-correct: a child without its own
// cursor inherits from its parent. The cascade already does this for
// ComputedStyle::cursor, but the *root* element with no inline
// cursor returns Default â€” so this walk is mostly belt-and-braces.
detail::ComputedStyle::Cursor effective_cursor(
        const std::vector<Block>& blocks,
        const detail::StyleStore& styles,
        int idx) {
    using C = detail::ComputedStyle::Cursor;
    while (idx >= 0) {
        const auto c = styles.computed(blocks[static_cast<std::size_t>(idx)].id).cursor;
        if (c != C::Default) return c;
        idx = blocks[static_cast<std::size_t>(idx)].parent_idx;
    }
    return C::Default;
}

// Translate the internal Cursor enum to the stable integer protocol that
// App's map_cursor() consumes: 0 default, 1 pointer, 2 text, 3 crosshair,
// 4 move/all, 5 not-allowed, 6 ew-resize, 7 ns-resize. Explicit on purpose —
// do NOT lean on enum ordinals lining up with the protocol codes (they don't,
// and that mismatch silently showed a diagonal cursor for ew-resize and a
// plain arrow for ns-resize).
int cursor_protocol_code(detail::ComputedStyle::Cursor c) {
    using C = detail::ComputedStyle::Cursor;
    switch (c) {
        case C::Pointer:    return 1;
        case C::Text:       return 2;
        case C::Crosshair:  return 3;
        case C::Move:       return 4;
        case C::NotAllowed: return 5;
        case C::ResizeEW:   return 6;
        case C::ResizeNS:   return 7;
        default:            return 0;  // Default
    }
}
}  // namespace

/// Cursor the OS should display right now (under the last mouse pos).
/// Lives on the public Document surface so App can poll it once per
/// frame without taking a Painter-style dependency.
int Document::hovered_cursor() const {
#if !defined(AFFINEUI_STUB_BUILD)
    for (int idx = impl_->hovered_idx;
         idx >= 0 && idx < static_cast<int>(impl_->blocks.size());
         idx = impl_->blocks[static_cast<std::size_t>(idx)].parent_idx) {
        const auto& block = impl_->blocks[static_cast<std::size_t>(idx)];
        if (live_control_kind_for_block(block) == LiveControlKind::NumericInput) {
            return 6;
        }
        bool resize_x = false;
        bool resize_y = false;
        if (point_in_textarea_resize_grip(*impl_, idx, impl_->last_mouse_pos,
                                          resize_x, resize_y)) {
            if (resize_x && !resize_y) return 6;
            if (!resize_x && resize_y) return 7;
            return 4;
        }
    }
#endif
    return cursor_protocol_code(
        effective_cursor(impl_->blocks, impl_->style_store, impl_->hovered_idx));
}

Document::HoverInfo Document::hovered_info() const {
    HoverInfo info{};
    const int idx = impl_->hovered_idx;
    if (idx < 0 || idx >= static_cast<int>(impl_->blocks.size())) return info;
    const auto& b = impl_->blocks[static_cast<std::size_t>(idx)];
    info.valid   = true;
    info.tag     = b.tag;
    info.elem_id = b.elem_id;
    info.classes = b.classes;
    info.attrs   = b.attrs;
#if !defined(AFFINEUI_STUB_BUILD)
    info.bounds  = block_border_visual_rect(*impl_, idx);
#else
    info.bounds  = b.bounds;
#endif
    return info;
}

std::vector<Document::HoverInfo> Document::hovered_info_chain() const {
    std::vector<HoverInfo> chain;
    hovered_info_chain(chain);
    return chain;
}

void Document::hovered_info_chain(std::vector<HoverInfo>& chain) const {
    chain.clear();
    chain.reserve(impl_->hovered_chain.size());
    int idx = impl_->hovered_idx;
    while (idx >= 0 && idx < static_cast<int>(impl_->blocks.size())) {
        const auto& b = impl_->blocks[static_cast<std::size_t>(idx)];
        HoverInfo info{};
        info.valid   = true;
        info.tag     = b.tag;
        info.elem_id = b.elem_id;
        info.classes = b.classes;
        info.attrs   = b.attrs;
#if !defined(AFFINEUI_STUB_BUILD)
        info.bounds  = block_border_visual_rect(*impl_, idx);
#else
        info.bounds  = b.bounds;
#endif
        chain.push_back(std::move(info));
        idx = b.parent_idx;
    }
}

void Document::set_resource_loader(ResourceLoader loader) {
    impl_->resource_loader = std::move(loader);
}

void Document::set_clipboard(ClipboardGet get, ClipboardSet set) {
    impl_->clipboard_get = std::move(get);
    impl_->clipboard_set = std::move(set);
}

Size Document::content_size() const { return impl_->content_size; }

DomHandle Document::weak_handle_for_id(std::string_view elem_id) {
    DomHandle out{};
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc || elem_id.empty()) return out;
    auto* elem = find_dom_element_by_id(*impl_, elem_id);
    if (!elem) return out;
    auto* node = lxb_dom_interface_node(elem);
    if (!node) return out;

    out.document_id = impl_->document_id;
    for (std::size_t i = 0; i < impl_->dom_weak_slots.size(); ++i) {
        auto& slot = impl_->dom_weak_slots[i];
        if (slot.node == node) {
            out.node_slot = static_cast<std::uint32_t>(i + 1);
            out.generation = slot.generation;
            return out;
        }
    }

    std::size_t slot_index = impl_->dom_weak_slots.size();
    for (std::size_t i = 0; i < impl_->dom_weak_slots.size(); ++i) {
        if (impl_->dom_weak_slots[i].node == nullptr) {
            slot_index = i;
            break;
        }
    }
    if (slot_index == impl_->dom_weak_slots.size()) {
        impl_->dom_weak_slots.push_back({});
    }
    auto& slot = impl_->dom_weak_slots[slot_index];
    slot.node = node;
    if (slot.generation == 0) slot.generation = 1;
    out.node_slot = static_cast<std::uint32_t>(slot_index + 1);
    out.generation = slot.generation;
#else
    (void)elem_id;
#endif
    return out;
}

bool Document::weak_handle_valid(DomHandle handle) const {
    if (handle.document_id != impl_->document_id || handle.node_slot == 0) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(handle.node_slot - 1);
    if (index >= impl_->dom_weak_slots.size()) return false;
    const auto& slot = impl_->dom_weak_slots[index];
    if (slot.generation != handle.generation) return false;
#if !defined(AFFINEUI_STUB_BUILD)
    return slot.node != nullptr;
#else
    return false;
#endif
}

bool Document::set_attribute_by_id(std::string_view elem_id,
                                   std::string_view name,
                                   std::string_view value) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc || name.empty()) return false;
    auto* elem = find_dom_element_by_id(*impl_, elem_id);
    if (!elem) return false;
    return set_attribute_on_element(*impl_, elem, name, value);
#else
    (void)elem_id; (void)name; (void)value;
    return false;
#endif
}

bool Document::remove_attribute_by_id(std::string_view elem_id,
                                      std::string_view name) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc || name.empty()) return false;
    auto* elem = find_dom_element_by_id(*impl_, elem_id);
    if (!elem) return false;
    return remove_attribute_on_element(*impl_, elem, name);
#else
    (void)elem_id; (void)name;
    return false;
#endif
}

bool Document::set_text_by_id(std::string_view elem_id,
                              std::string_view text) {
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc) return false;
    auto* elem = find_dom_element_by_id(*impl_, elem_id);
    if (!elem || element_has_element_child(elem)) return false;

    int target_idx = -1;
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        if (impl_->style_store.element_of(impl_->blocks[i].id) == elem) {
            target_idx = static_cast<int>(i);
            break;
        }
    }
    if (target_idx < 0) return false;

    auto& block = impl_->blocks[static_cast<std::size_t>(target_idx)];
    int text_idx = -1;
    bool has_unsupported_descendant = false;
    for (int idx = target_idx + 1;
         idx < static_cast<int>(impl_->blocks.size()); ++idx) {
        if (!is_descendant_of_or_self(impl_->blocks, idx, target_idx)) continue;
        const auto& child = impl_->blocks[static_cast<std::size_t>(idx)];
        if (child.synthetic) continue;
        if (child.tag == "#text") {
            if (text_idx < 0) text_idx = idx;
            continue;
        }
        has_unsupported_descendant = true;
        break;
    }
    if (has_unsupported_descendant) return false;

    if (text_idx < 0 && block.text == text) return true;
    if (text_idx >= 0 &&
        impl_->blocks[static_cast<std::size_t>(text_idx)].text == text) {
        return true;
    }

    const Rect old_rect = subtree_visual_rect(*impl_, target_idx);
    auto* node = lxb_dom_interface_node(elem);
    if (lxb_dom_node_text_content_set(node, as_lxb(text), text.size())
            != LXB_STATUS_OK) {
        return false;
    }

    if (text_idx >= 0) {
        auto& text_block = impl_->blocks[static_cast<std::size_t>(text_idx)];
        const auto& cs = impl_->style_store.computed(text_block.id);
        text_block.text = apply_text_transform(
            node_text(node, cs.white_space), cs.text_transform);
        for (int idx = text_idx + 1;
             idx < static_cast<int>(impl_->blocks.size()); ++idx) {
            if (!is_descendant_of_or_self(impl_->blocks, idx, target_idx))
                continue;
            auto& child = impl_->blocks[static_cast<std::size_t>(idx)];
            if (child.tag == "#text") child.text.clear();
        }
    } else {
        const auto& cs = impl_->style_store.computed(block.id);
        block.text = apply_text_transform(
            node_text(node, cs.white_space), cs.text_transform);
        block.placeholder_visible = false;
    }
    mark_live_mutation_dirty(*impl_, target_idx, old_rect,
                             /*needs_layout=*/true);
    return true;
#else
    (void)elem_id; (void)text;
    return false;
#endif
}

std::vector<Rect> Document::take_dirty_rects() {
    std::vector<Rect> out;
    out.swap(impl_->dirty_rects);
    return out;
}

bool Document::take_paint_dirty() {
    const bool dirty = impl_->paint_dirty;
    impl_->paint_dirty = false;
    return dirty;
}

bool Document::has_active_animations() const {
#if !defined(AFFINEUI_STUB_BUILD)
    if (impl_->animation_candidate_count == 0) return false;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& b : impl_->blocks) {
        if (!b.animation.active || !find_keyframes(*impl_, b.animation.name_hash))
            continue;
        const double elapsed_s = std::chrono::duration<double>(
            now - b.animation_epoch).count();
        float t = 0.0f;
        bool applies = false;
        if (animation_progress_at(b.animation, elapsed_s, t, applies)) {
            return true;
        }
    }
#endif
    return false;
}

void Document::set_animation_time_for_testing(double seconds) {
#if !defined(AFFINEUI_STUB_BUILD)
    const double clamped = std::max(0.0, seconds);
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(clamped));
    impl_->animation_epoch = std::chrono::steady_clock::now() - elapsed;
    for (auto& b : impl_->blocks) {
        b.animation_epoch = impl_->animation_epoch;
    }
#else
    (void)seconds;
#endif
}

// â”€â”€ Immediate mode â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void Document::set_imm_view(std::function<void()> view_fn) {
    if (!impl_->imm) impl_->imm = std::make_unique<detail::ImmRuntime>();
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc) {
        // No DOM yet â€” establish a minimal empty document so the
        // runtime has a body to mutate. set_html("") goes through the
        // normal parse path and ends with an empty <body>.
        set_html("");
    }
    impl_->imm->bind(this, impl_->doc);
#endif
    impl_->imm->set_view_fn(std::move(view_fn));
}

bool Document::imm_dirty() const {
    return impl_->imm && impl_->imm->dirty() && impl_->imm->has_view_fn();
}

void Document::invalidate_imm() {
    if (impl_->imm) impl_->imm->mark_dirty();
}

void Document::tick_imm() {
    if (!impl_->imm || !impl_->imm->has_view_fn()) return;
    if (!impl_->imm->dirty()) return;

#if !defined(AFFINEUI_STUB_BUILD)
    // 1. Run the view fn â€” it mutates lexbor's DOM directly via the
    //    runtime, replacing the body's children.
    impl_->imm->run_view_fn();

    // 2. Re-cascade + re-collect. This is the same tail as set_html
    //    after parsing â€” minus the stylesheet re-attach (those are
    //    still bound to impl_->doc from the original set_html).
    recollect_blocks_from_current_dom(*impl_);
#endif
}

bool Document::invoke_imm_click(std::string_view elem_id) {
    return impl_->imm && impl_->imm->invoke_click(elem_id);
}

}  // namespace affineui
