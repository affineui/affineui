// affineui::detail::DocumentImpl + the shared block/style side-table types.
//
// This internal header carries the TU-local types that the document_*.cpp
// implementation files share: the paintable `Block`, the CSS selector /
// rule / @media / keyframe side tables, the text-layout caches, and the
// central `detail::DocumentImpl` state object. It is included only by the
// renderer's own document_*.cpp files, never by public consumers.
//
// The amalgamator inlines this once (INTERNAL_HEADERS) ahead of the
// document_*.cpp bodies, so the two-file SDK sees the same definitions.
#pragma once

#include "affineui/document.h"        // Document + its nested types, DomHandle
#include "affineui/painter.h"         // Painter::TextAlign, Painter*
#include "affineui/view.h"            // ViewSink (unique_ptr member)
#include "imm/imm_runtime.h"          // ImmRuntime (unique_ptr member)
#include "internal/animated_style.h"  // AnimatedStyle
#include "internal/computed_style.h"  // ComputedStyle, CustomPropMap, BoxShadowList
#include "internal/element_id.h"      // ElementId
#include "internal/style_resolver.h"  // StyleResolver, ResolvedStyle
#include "internal/style_store.h"     // StyleStore
#include "layout/yoga_adapter.h"      // detail::GridTrackHint, kMaxGridTrackHints

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(AFFINEUI_STUB_BUILD)
#    include <lexbor/css/css.h>
#    include <lexbor/dom/dom.h>
#    include <lexbor/html/html.h>
#endif

namespace affineui {

// ── Shared paintable/style side-table types (were TU-local in document.cpp) ──

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
    // Generated ::before/::after text whose color came from the generated
    // rule itself (not inheritance). Restyles must keep the rule color;
    // without the flag they can't tell it apart from an inherited one.
    bool              generated_color_locked{false};
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

struct TextVisualLine {
    std::size_t begin{0};
    std::size_t end{0};
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

inline std::uint64_t fnv1a_64_bytes(std::uint64_t h,
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

inline void hash_mix_string(std::uint64_t& h, std::string_view value) {
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
    /// Combinator BETWEEN this compound and the one it qualifies (the next
    /// compound toward the target). true = child (`A > B`), false = descendant
    /// (`A B`). Only meaningful on ancestor entries.
    bool direct_parent{false};
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
    // `background-clip` (border-box | padding-box | content-box). Lexbor has
    // no typed property for it, so it rides this side table like cursor.
    std::uint8_t                  background_clip{0};
    bool                          has_background_clip{false};
    // When non-zero, this fill is gated on the named pseudo-class
    // state bit (kHoverStateBit / kActiveStateBit / kFocusStateBit)
    // being set on the matched element.
    std::uint8_t                  state_bit{0};
};

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
    std::string                    display_value;
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

// Geometry of a block's painted vertical scrollbar (track/thumb in doc
// space); filled by detail::vertical_scrollbar_geometry.
struct ScrollbarGeometry {
    Rect track{};
    Rect thumb{};
    int scroll_range{0};
    int thumb_travel{0};
};
#endif  // !AFFINEUI_STUB_BUILD

namespace detail {

// Defined in document_core.cpp; DocumentImpl's ctor calls it.
std::uint32_t next_document_id();

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
    std::string last_dock_trace_signature;

    // View-reconcile sink (App fast path; see begin_view_mutations).
    // The sink owns the WidgetNode remote_id → DOM node mapping; the
    // reset hook clears it when the document is replaced wholesale.
    std::unique_ptr<ViewSink>  view_sink;
    std::function<void()>      view_sink_reset;
    bool                       view_batch_active{false};
    bool                       view_structure_dirty{false};
    // §8.2 batched attr contract (WIDGET_RECONCILIATION.md): selector-
    // affecting attr ops inside a view batch record their dirty root here;
    // end_view_mutations settles all of them with ONE rematch/resolver-
    // clear/restyle/reveal pass instead of per-op full machinery.
    struct ViewBatchAttrRoot {
        int  root_idx;
        Rect old_rect;
        bool needs_subtree_rematch;
        bool force_layout;
    };
    std::vector<ViewBatchAttrRoot> view_batch_attr_roots;

    // Structural changes recorded per-op by the view sink: the nearest
    // block ancestor of each insert/remove/text/raw-html splice. The
    // settle scopes its selector rematch to these subtrees and keeps the
    // resolver cache warm ("most changes don't require style
    // recomputation"); view_structure_dirty remains the global-settle
    // fallback for changes with no block scope (bootstrap, empty shell).
    std::vector<int> view_batch_structure_roots;

    // Custom paint (canvas) handlers, keyed by the data-aui-paint
    // attribute value. Handlers draw immediate-mode into the active
    // Painter during the block paint pass; they survive document
    // replacement (registration is app-side state, elements re-bind by
    // attribute on the next paint).
    std::unordered_map<std::string, Document::CustomPaintFn> paint_handlers;

    struct ScrollbarDrag {
        int block_idx{-1};
        int start_y{0};
        int start_scroll_y{0};
        int thumb_offset_y{0};
    } scrollbar_drag;

#if !defined(AFFINEUI_STUB_BUILD)
    // Saved lexbor eager-style-attach hook while a view-mutation batch
    // is active (see begin_view_mutations).
    lxb_dom_event_insert_f view_saved_ev_insert{nullptr};

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

    // A floating tearoff panel being resized from one of the JS-compatible
    // synthetic edge/corner zones.
    struct FloatResize {
        lxb_dom_element_t* elem{nullptr};
        int                dir{0};          // bitmask: n=1,s=2,w=4,e=8
        int                start_x{0};
        int                start_y{0};
        int                elem_doc_x{0};
        int                elem_doc_y{0};
        int                elem_w{0};
        int                elem_h{0};
        int                cb_x{0};
        int                cb_y{0};
        int                bounds_x{0};
        int                bounds_y{0};
        int                bounds_w{0};
        int                bounds_h{0};
        std::string        panel_id;
    } float_resize;

    // A dock-pane tab pressed and possibly being dragged. The tab switches on
    // mouse-down; if the press turns into a drag, the release docks or tears
    // off the already-selected panel.
    struct TabDrag {
        lxb_dom_element_t* tab{nullptr};    // the grabbed tab button
        lxb_dom_element_t* pane{nullptr};   // its enclosing .dcs-dockpane
        std::string        panel_id;        // dockpanel id (target body minus -body)
        std::string        source_pane_id;
        std::string        drag_kind{"panels"};
        std::string        label;
        std::vector<std::string> source_tab_ids;
        Document::DockPlacement source_placement;
        Rect               source_pane_bounds{};
        bool               source_pane_bounds_valid{false};
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
    lxb_dom_element_t* tab_drag_ghost{nullptr};

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
        enum class Kind { None, Chip, Square, Hue };
        Kind               kind{Kind::None};
        lxb_dom_element_t* field{nullptr};  // the dcs-colorfield
        lxb_dom_element_t* part{nullptr};
        Rect               bounds{};
        int                start_x{0};
        int                start_y{0};
        double             h{0.0};  // snapshot HSV
        double             s{0.0};
        double             v{0.0};
    } colorfield_drag;

    struct TreeDrag {
        enum class Zone { None, Before, After, Into };
        lxb_dom_element_t* tree{nullptr};
        lxb_dom_element_t* row{nullptr};
        lxb_dom_element_t* target{nullptr};
        lxb_dom_element_t* select_box{nullptr};
        lxb_dom_element_t* select_row{nullptr};
        Zone               zone{Zone::None};
        int                start_x{0};
        int                start_y{0};
        bool               press_ctrl{false};
        bool               press_shift{false};
        bool               press_super{false};
        bool               dragging{false};
    } tree_drag;

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
    // Per-attribute-name result cache for
    // stylesheet_dependencies_stay_in_mutated_subtree() — the scan walks
    // every rule of every sheet, which is far too slow to repeat on each
    // Parsed SVG geometry cache: element → local-space path commands +
    // a hash of the geometry attributes it was parsed from. Static art
    // (jack sockets, LCD digits, chevrons) parses its `d`/shape strings
    // ONCE; subsequent paints reuse the local commands and only re-apply
    // the view transform (transform_local_cmds). A geometry-attr write
    // busts the entry via the hash mismatch, so it stays correct through
    // mutation without an explicit invalidation hook. mutable: filled
    // during const draw(). Keyed on the raw element pointer — entries for
    // destroyed elements are harmless (never looked up again) and the map
    // is cleared on document teardown / full recollect.
    struct SvgPathCacheEntry {
        std::size_t        attr_hash{0};
        std::vector<float> local_cmds;
        double             min_x{0}, min_y{0}, max_x{0}, max_y{0};
        bool               has_bbox{false};
    };
    mutable std::unordered_map<const lxb_dom_element_t*, SvgPathCacheEntry>
        svg_path_cache;

    // live attribute mutation. Invalidated whenever `sheets` changes.
    // (mutable: the scan runs behind const DocumentImpl&.)
    mutable std::unordered_map<std::string, bool> attr_subtree_local_cache;
    // Same shape for attribute_matches_confined_to_subject(): whether every
    // stylesheet mention of the attribute sits in a subject compound, so a
    // write can only re-match the mutated element itself.
    mutable std::unordered_map<std::string, bool> attr_subject_confined_cache;
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

// ── Cross-file document helpers ─────────────────────────────────────────────
// Shared by the document_*.cpp implementation files. Each is defined exactly
// once, in the file that owns its domain; call sites qualify `detail::`.
// (Helpers used by only one file stay as anonymous-namespace locals there.)

int scroll_offset_y_for(const std::vector<Block>& blocks,
#if !defined(AFFINEUI_STUB_BUILD)
                        const detail::StyleStore& styles,
#endif
                        int idx);

#if !defined(AFFINEUI_STUB_BUILD)
// element / block attribute primitives
std::string attr_string(lxb_dom_element_t* elem, std::string_view name);
bool has_attr(lxb_dom_element_t* elem, std::string_view name);
double elem_attr_num(lxb_dom_element_t* elem, std::string_view name,
                     double fallback);
const std::string* block_attr_value(const Block& block, std::string_view name);
bool block_has_attr(const Block& block, std::string_view name);
bool block_has_class(const Block& block, std::string_view cls);
double block_attr_double(const Block& block,
                         std::string_view name,
                         double fallback);
int nearest_block_with_tag(const std::vector<Block>& blocks,
                           int idx,
                           std::string_view tag);

// CSS text / color parsing
std::string_view trim_css_ws(std::string_view s);
bool parse_hex_color(std::string_view value, std::uint32_t& out);
bool parse_generated_color(std::string value,
                           const detail::ResolvedStyle& style,
                           std::uint32_t& out);

// geometry / z-order / scrollbars
int effective_z_index(const detail::DocumentImpl& impl, int idx);
int nearest_clip_ancestor_for_block(const detail::DocumentImpl& impl, int idx);
bool vertical_scrollbar_geometry(const detail::DocumentImpl& impl,
                                 int idx,
                                 ScrollbarGeometry& out);

// live-control value mapping
double normalized_control_value(double value, double min, double max);
double decius_knob_angle(double min, double max, double value);

// fonts / svg paint
void ensure_font_faces_registered(detail::DocumentImpl& impl,
                                  Painter& painter);
void paint_direct_child_svgs(detail::DocumentImpl& impl,
                             const Block& b,
                             const Rect& eff,
                             const detail::ComputedStyle& cs,
                             const detail::AnimatedStyle& an,
                             Painter& painter,
                             lxb_dom_element_t* parent_elem);

// text-control layout / selection
TextControlGeometry text_control_geometry(const detail::DocumentImpl& impl,
                                          int idx,
                                          Painter& painter);
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
bool has_text_selection(const Block& block);
std::pair<std::size_t, std::size_t>
normalized_selection(const Block& block);
#endif  // !AFFINEUI_STUB_BUILD

}  // namespace detail

#if !defined(AFFINEUI_STUB_BUILD)
// CSS animation sampling (defined at affineui scope in the animation section).
bool animation_progress_at(const detail::ResolvedStyle::CssAnimation& anim,
                           double elapsed_s,
                           float& out_t,
                           bool& out_applies);
Mat2x3 effective_transform_for(const detail::DocumentImpl& impl, int idx);
detail::AnimatedStyle sample_keyframes(detail::DocumentImpl& impl,
                                       const Block& block,
                                       float t);
#endif  // !AFFINEUI_STUB_BUILD

}  // namespace affineui
