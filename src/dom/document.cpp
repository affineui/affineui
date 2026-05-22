// affineui::Document — Phase 2A.
//
// Parses HTML with lexbor, attaches stylesheets (user-agent + user +
// any embedded `<style>` blocks) through lexbor's cascade, then for
// each block-level element collects a `ResolvedStyle` (ComputedStyle
// + AnimatedStyle) via the StyleResolver. The Phase 1 `style_for(tag)`
// fallback is gone — real CSS now drives every visible attribute we
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
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if !defined(AFFINEUI_STUB_BUILD)
#    include <lexbor/css/css.h>
#    include <lexbor/dom/dom.h>
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
// than its children — that lets paint walk the vector linearly and
// hit parents before children (correct z-order for the box-bg-then-
// text emit pattern).
struct Block {
    detail::ElementId id{};        // StyleStore handle
    std::string       tag;
    std::string       elem_id;     // value of the `id` attribute (for "#x" selectors)
    std::vector<std::string> classes;  // tokenized `class` attribute
    std::string       text;
    std::string       image_src;
    std::string       placeholder;
    int               parent_idx{-1};
    Rect              bounds{};
    // Scroll state. Only meaningful when ComputedStyle.overflow_y is
    // Scroll or Auto. content_h is the total height of descendant
    // content measured from this block's bounds.y — i.e. how far the
    // user can scroll before the bottom of the deepest descendant
    // clears the visible window.
    int               scroll_y{0};
    int               content_h{0};
    // Synthetic line-box: a Yoga flex-row wrapper inserted by
    // collect_blocks around runs of inline / inline-block siblings.
    // No DOM element backs it; paint skips bg/border/text (it's
    // transparent above its children). Click routing + hit-test
    // treat it normally — children sit on top anyway.
    bool              synthetic{false};
    bool              text_control{false};
    bool              placeholder_visible{false};
};

#if !defined(AFFINEUI_STUB_BUILD)
// CSS pseudo-class side-table entry, parsed out of attached
// stylesheets by scan_pseudo_rules(). Each compound is the AND of
// one-or-more simple identifiers (tag/class/id). The chain is
// `target` (must match the hovered/active element) plus zero-or-more
// `ancestors` walked deepest → root with descendant-combinator gaps.
//
// Today's grammar: simple selectors + descendant combinator only.
// `>`, `+`, `~`, attribute selectors, and functional pseudos are
// silently skipped at scan time.
struct SimpleSelector {
    enum class Kind : std::uint8_t { Tag, Class, Id };
    Kind        kind;
    std::string name;
};

struct CompoundSelector {
    std::vector<SimpleSelector> simples;  // AND'd together
};

struct PseudoRule {
    enum class Pseudo : std::uint8_t { Hover, Active, Focus };
    Pseudo                                        pseudo;
    CompoundSelector                              target;
    std::vector<CompoundSelector>                 ancestors;  // nearest → root
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
    // When non-zero, this fill is gated on the named pseudo-class
    // state bit (kHoverStateBit / kActiveStateBit / kFocusStateBit)
    // being set on the matched element.
    std::uint8_t                  state_bit{0};
};

// Per-element state bits in StyleStore::state_bits().
constexpr std::uint8_t kHoverStateBit  = 1u << 0;
constexpr std::uint8_t kActiveStateBit = 1u << 1;
constexpr std::uint8_t kFocusStateBit  = 1u << 2;
#endif

}  // namespace

namespace detail {

struct DocumentImpl {
    std::string               html;
    std::string               user_stylesheet;
    ResourceLoader            resource_loader;
    Size                      content_size{0, 0};
    std::vector<Block>        blocks;
    bool                      paint_dirty{true};  // Phase 2C flips this

    // Interaction state. -1 = no block (off-window or pointer not down).
    // Updated by Document::dispatch; read by App to drive cursor +
    // :hover / :active and click routing. The *_chain vectors hold the
    // deepest → root block indices for the currently-hovered (resp.
    // -pressed) element. Recomputed on every relevant event; diffed
    // against the previous chain to toggle the pseudo state bit per
    // affected element.
    int                       hovered_idx{-1};
    int                       active_idx{-1};
    int                       focused_idx{-1};
    std::vector<int>          hovered_chain;
    std::vector<int>          active_chain;
    Point                     last_mouse_pos{};

    // Immediate-mode runtime — lazily created on the first
    // set_imm_view() call. Holds state slots, click handlers, and the
    // view function across re-renders.
    std::unique_ptr<ImmRuntime> imm;

    // Per-element style + dirty bookkeeping. Lives across set_html()
    // calls; reset() inside set_html() recycles capacity.
    StyleStore                style_store;

#if !defined(AFFINEUI_STUB_BUILD)
    lxb_html_document_t*               doc{nullptr};
    std::vector<lxb_css_stylesheet_t*> sheets;
    // :hover / :active overlay rules — populated by scan_pseudo_rules()
    // during attach. Pointers in `decls` reference rule data owned by
    // the document's CSS memory pool; valid for the document's lifetime.
    std::vector<PseudoRule>            pseudo_rules;
    // Font-family fill rules populated by scan_rule_fills() from the raw
    // CSS source at attach time.
    std::vector<RuleFill>              rule_fills;
    std::unique_ptr<StyleResolver>     resolver;
    ResolvedStyle                      root_style{};  // inheritance root
#endif

    ~DocumentImpl() {
#if !defined(AFFINEUI_STUB_BUILD)
        resolver.reset();
        // Stylesheets are owned by the document's CSS memory pool once
        // attached — destroying the document tears them down. Just drop
        // our tracking refs.
        sheets.clear();
        if (doc) lxb_html_document_destroy(doc);
#endif
    }
};

}  // namespace detail

namespace {

#if !defined(AFFINEUI_STUB_BUILD)

// ── DOM utilities ───────────────────────────────────────────────────

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
                // CR or CRLF → LF (normalize line endings).
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
// ASCII letters only — full Unicode case-mapping is out of scope.
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
    // Inherited text features — propagate from parent.
    rs.computed.letter_spacing_x100  = parent.computed.letter_spacing_x100;
    rs.computed.white_space          = parent.computed.white_space;
    rs.computed.text_transform       = parent.computed.text_transform;
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
    impl.blocks.push_back(std::move(b));
}

int ensure_inline_run(detail::DocumentImpl& impl, int parent_idx,
                      int& open_synth_idx) {
    if (open_synth_idx >= 0) return open_synth_idx;

    using Display = detail::ComputedStyle::Display;

    const auto sid = impl.style_store.acquire_synthetic();
    auto& synth_cs = impl.style_store.computed(sid);
    synth_cs.display        = Display::Flex;
    synth_cs.flex_direction = detail::ComputedStyle::FlexDirection::Row;
    synth_cs.flex_wrap      = detail::ComputedStyle::FlexWrap::Wrap;
    // Browser inline FFC aligns siblings at the text baseline. Yoga
    // supports YGAlignBaseline but only when each item exposes a baseline
    // via YGNodeSetBaselineFunc; our text leaves don't, so use Center until
    // the painter exposes per-font ascender/descender data for a real
    // baseline callback.
    synth_cs.align_items    = detail::ComputedStyle::AlignItems::Center;

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

// True if a (possibly value-less, boolean) attribute is present.
bool has_attr(lxb_dom_element_t* elem, std::string_view name) {
    return lxb_dom_element_has_attribute(
        elem, reinterpret_cast<const lxb_char_t*>(name.data()), name.size());
}

// The text a closed <select> shows: the `selected` <option>'s text, or
// the first option if none is marked. We render no popup/list — only the
// chosen option — matching a closed native control.
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

bool is_block_tag(const std::string& tag) {
    return tag == "h1" || tag == "h2" || tag == "h3" ||
           tag == "h4" || tag == "h5" || tag == "h6" ||
           tag == "p"  || tag == "div" ||
           tag == "section" || tag == "article" || tag == "header" ||
           tag == "footer"  || tag == "main"    || tag == "nav" ||
           // Form-ish + a few common containers. We treat them as
           // block-level for layout — Phase 3 inline layout splits
           // these into their proper inline / inline-block flow.
           tag == "button" || tag == "input"  || tag == "textarea" ||
           tag == "select" || tag == "label"  || tag == "form" ||
           tag == "ul"     || tag == "ol" ||
           tag == "li"     || tag == "a"      || tag == "span" ||
           tag == "img"    ||
           // CSS table model — each produces a box (display is set by the
           // UA stylesheet); the layout engine gives them table semantics.
           tag == "table"  || tag == "thead"  || tag == "tbody" ||
           tag == "tfoot"  || tag == "tr"     || tag == "td"    ||
           tag == "th"     || tag == "caption";
}

// ── Stylesheet extraction ──────────────────────────────────────────
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

void collect_author_stylesheets(lxb_dom_node_t* node,
                                const detail::DocumentImpl& impl,
                                std::string& out) {
    for (auto* c = lxb_dom_node_first_child(node); c;
         c = lxb_dom_node_next(c)) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            auto* el = lxb_dom_interface_element(c);
            const auto tag = tag_name(el);
            if (tag == "style") {
                size_t len = 0;
                if (auto* t = lxb_dom_node_text_content(c, &len); t && len) {
                    out.append(reinterpret_cast<const char*>(t), len);
                    out.push_back('\n');
                }
                continue;  // skip <style>'s descendants
            }
            if (tag == "link" && impl.resource_loader) {
                const auto rel = attr_string(el, "rel");
                const auto href = attr_string(el, "href");
                if (!href.empty() && rel_includes_stylesheet(rel)) {
                    if (auto css = impl.resource_loader(href); !css.empty()) {
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
//     "target" — the element whose state flips the rule on)
// Anything else (`>`, `+`, `~`, attribute selectors, functional
// pseudos, the pseudo on a non-target compound) is silently skipped.
// One compound matches when every one of its simples matches.
bool compound_matches(const CompoundSelector& compound,
                      std::string_view tag,
                      std::string_view elem_id,
                      const std::vector<std::string>& classes) {
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
        }
    }
    return true;
}

// Walk up `parent_idx` through `blocks`, greedy-matching each ancestor
// compound in order. Returns true when all ancestors have been
// satisfied (gaps are allowed — descendant combinator semantics).
bool ancestor_chain_matches(const std::vector<CompoundSelector>& ancestors,
                            int parent_idx,
                            const std::vector<Block>& blocks) {
    std::size_t i = 0;
    int idx = parent_idx;
    while (i < ancestors.size() && idx >= 0) {
        const auto& a = blocks[static_cast<std::size_t>(idx)];
        if (compound_matches(ancestors[i], a.tag, a.elem_id, a.classes)) {
            ++i;
        }
        idx = a.parent_idx;
    }
    return i == ancestors.size();
}

void scan_pseudo_rules(lxb_css_stylesheet_t* sst,
                       std::vector<PseudoRule>& out) {
    if (!sst || !sst->root) return;
    auto* rule_list = lxb_css_rule_list(sst->root);
    for (auto* r = rule_list->first; r != nullptr; r = r->next) {
        if (r->type != LXB_CSS_RULE_STYLE) continue;
        auto* style = lxb_css_rule_style(r);

        // `selector` is a comma-separated chain of compound chains;
        // walk each group independently — each becomes its own rule.
        for (auto* sl = style->selector; sl != nullptr; sl = sl->next) {
            // Build the chain of compounds for this group.
            std::vector<CompoundSelector> compounds;
            CompoundSelector              current;
            PseudoRule::Pseudo            pseudo{};
            bool                          has_pseudo  = false;
            bool                          pseudo_seen_in_last = false;
            bool                          ok          = true;

            for (auto* sel = sl->first; sel != nullptr; sel = sel->next) {
                const bool starts_new_compound =
                    (sel != sl->first) &&
                    (sel->combinator != LXB_CSS_SELECTOR_COMBINATOR_CLOSE);
                if (starts_new_compound) {
                    if (sel->combinator != LXB_CSS_SELECTOR_COMBINATOR_DESCENDANT) {
                        // `>`, `+`, `~` — not in MVP grammar.
                        ok = false; break;
                    }
                    if (pseudo_seen_in_last) {
                        // pseudo must be in the last compound only.
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
                            has_pseudo = true; pseudo_seen_in_last = true; break;
                        case LXB_CSS_SELECTOR_PSEUDO_CLASS_ACTIVE:
                            pseudo = PseudoRule::Pseudo::Active;
                            has_pseudo = true; pseudo_seen_in_last = true; break;
                        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FOCUS:
                            pseudo = PseudoRule::Pseudo::Focus;
                            has_pseudo = true; pseudo_seen_in_last = true; break;
                        default:
                            ok = false; break;
                    }
                    if (!ok) break;
                } else if (sel->type == LXB_CSS_SELECTOR_TYPE_ELEMENT ||
                           sel->type == LXB_CSS_SELECTOR_TYPE_CLASS   ||
                           sel->type == LXB_CSS_SELECTOR_TYPE_ID) {
                    SimpleSelector s;
                    switch (sel->type) {
                        case LXB_CSS_SELECTOR_TYPE_ELEMENT: s.kind = SimpleSelector::Kind::Tag;   break;
                        case LXB_CSS_SELECTOR_TYPE_CLASS:   s.kind = SimpleSelector::Kind::Class; break;
                        case LXB_CSS_SELECTOR_TYPE_ID:      s.kind = SimpleSelector::Kind::Id;    break;
                        default: ok = false; break;
                    }
                    if (!ok) break;
                    s.name.assign(
                        reinterpret_cast<const char*>(sel->name.data),
                        sel->name.length);
                    current.simples.push_back(std::move(s));
                } else {
                    ok = false; break;
                }
            }

            if (!ok || !has_pseudo) continue;
            // The current compound is the target. It must have at
            // least one identifier — `:hover { ... }` (universal)
            // alone is not supported in MVP.
            if (current.simples.empty()) continue;
            compounds.push_back(std::move(current));

            PseudoRule pr;
            pr.pseudo = pseudo;
            pr.target = std::move(compounds.back());
            compounds.pop_back();
            // compounds left over are the ancestor constraints, with
            // the OUTERMOST first in CSS source order. We want them
            // nearest → root (reverse).
            pr.ancestors.reserve(compounds.size());
            for (auto it = compounds.rbegin(); it != compounds.rend(); ++it) {
                pr.ancestors.push_back(std::move(*it));
            }
            pr.decls = style->declarations;
            out.push_back(std::move(pr));
        }
    }
}

// ── Font-family fill scanner ──────────────────────────────────────
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
// the body — it's just collecting the source range so other helpers
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

// Parse a static (no `>` / `+` / `~`, no attribute selector) selector
// text into a target compound + ancestor chain matching the shape
// scan_pseudo_rules builds. A single trailing pseudo-class
// (:hover / :active / :focus) on the last compound is allowed and
// returned via `out_state_bit`; any other pseudo (including anywhere
// but the last compound) causes a parse failure. Returns false on
// anything we don't support (the rule's other properties still apply
// through lexbor; we just won't fill the missing ones).
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
        if (sel[i] != '.' && sel[i] != '#' && sel[i] != ':') {
            const std::size_t s = i;
            while (i < sel.size() && sel[i] != '.' && sel[i] != '#' &&
                   sel[i] != ':' && !is_css_ws(sel[i])) ++i;
            if (s == i) return false;
            compound.simples.push_back(
                {SimpleSelector::Kind::Tag, std::string(sel.substr(s, i - s))});
        }
        // Then any number of `.name` / `#name` segments, optionally
        // followed by a single `:pseudo` recognized below.
        while (i < sel.size() && !is_css_ws(sel[i])) {
            if (sel[i] == ':') {
                if (pseudo_seen) return false;
                ++i;
                const std::size_t s = i;
                while (i < sel.size() && sel[i] != '.' && sel[i] != '#' &&
                       sel[i] != ':' && !is_css_ws(sel[i])) ++i;
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
                   sel[i] != ':' && !is_css_ws(sel[i])) ++i;
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

// Find `key: <token>` and return the first comma/semicolon-delimited
// token, with surrounding whitespace and any wrapping ' or " stripped.
// Used to extract the first family name from `font-family: <list>`.
std::string find_decl_first_ident(std::string_view decls,
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
        while (end < rest.size() && rest[end] != ',' && rest[end] != ';')
            ++end;
        std::string tok(trim_css_ws(rest.substr(0, end)));
        // Strip a wrapping pair of quotes (matching).
        if (tok.size() >= 2 &&
            ((tok.front() == '"'  && tok.back() == '"') ||
             (tok.front() == '\'' && tok.back() == '\''))) {
            tok = tok.substr(1, tok.size() - 2);
        }
        return tok;
    }
    return {};
}

// Walk the raw CSS source for each rule's font-family declaration.
// For rules whose selector(s) parse as static (no pseudo / no advanced
// combinators), append a RuleFill entry per comma-separated group.
void scan_rule_fills(lxb_css_stylesheet_t* sst,
                     std::string_view css,
                     std::vector<RuleFill>& out) {
    std::vector<const lxb_css_rule_style_t*> parsed_styles;
    if (sst && sst->root) {
        auto* rule_list = lxb_css_rule_list(sst->root);
        for (auto* r = rule_list->first; r != nullptr; r = r->next) {
            if (r->type != LXB_CSS_RULE_STYLE) continue;
            parsed_styles.push_back(lxb_css_rule_style(r));
        }
    }

    std::size_t raw_rule_index = 0;
    for (const auto& raw : split_css_rules(css)) {
        const lxb_css_rule_style_t* parsed_style =
            raw_rule_index < parsed_styles.size()
                ? parsed_styles[raw_rule_index]
                : nullptr;
        ++raw_rule_index;

        // font-family — only the first family name is honored. Real
        // browsers walk the fallback list and pick the first family
        // that has a face installed; our resolver does a similar
        // fallback inside resolve_font when the name doesn't match.
        const auto ff = find_decl_first_ident(raw.decls, "font-family");
        if (ff.empty()) continue;

        // Each comma-separated group becomes its own RuleFill.
        std::string_view sel_text = trim_css_ws(raw.selector);
        std::size_t s = 0;
        const lxb_css_selector_list_t* parsed_selector =
            parsed_style ? parsed_style->selector : nullptr;
        while (s <= sel_text.size()) {
            const auto comma = sel_text.find(',', s);
            const auto group = trim_css_ws(sel_text.substr(s,
                (comma == std::string_view::npos
                     ? sel_text.size() : comma) - s));
            const lxb_css_selector_specificity_t specificity =
                parsed_selector ? parsed_selector->specificity : 0;
            if (!group.empty()) {
                CompoundSelector target;
                std::vector<CompoundSelector> ancestors;
                std::uint8_t state_bit = 0;
                if (parse_static_selector(group, target, ancestors, state_bit)) {
                    RuleFill rf;
                    rf.target               = std::move(target);
                    rf.ancestors            = std::move(ancestors);
                    rf.specificity          = specificity;
                    rf.source_order         =
                        static_cast<std::uint32_t>(out.size());
                    rf.font_family          = ff;
                    rf.state_bit            = state_bit;
                    out.push_back(std::move(rf));
                }
            }
            if (comma == std::string_view::npos) break;
            s = comma + 1;
            if (parsed_selector) parsed_selector = parsed_selector->next;
        }
    }

    std::stable_sort(out.begin(), out.end(),
        [](const RuleFill& a, const RuleFill& b) {
            if (a.specificity != b.specificity)
                return a.specificity < b.specificity;
            return a.source_order < b.source_order;
        });
}

void attach_stylesheet(detail::DocumentImpl& impl, std::string_view css) {
    if (css.empty()) return;
    // Parse via the document's own CSS parser (pre-wired with the
    // document's memory pool + selectors engine). Parsing through a
    // standalone parser allocates rules in a foreign pool that the
    // document's ev_destroy hook can't safely tear down.
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
        // but RuleFill values copy what they need).
        scan_rule_fills(sst, css, impl.rule_fills);
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

// Map a `cursor` keyword onto our enum. Unknown values → Default.
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

    for (auto* child = lxb_dom_node_first_child(node); child;
         child = lxb_dom_node_next(child)) {
        if (node_is_collapsible_whitespace(child)) {
            if (open_synth_idx >= 0) pending_inline_space = true;
            continue;
        }

        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            auto text = apply_text_transform(
                node_text(child, parent_style.computed.white_space),
                parent_style.computed.text_transform);
            if (!text.empty()) {
                const int inline_parent_idx =
                    ensure_inline_run(impl, parent_idx, open_synth_idx);
                if (pending_inline_space) {
                    append_anonymous_inline_text(impl, parent_style,
                                                 inline_parent_idx, " ");
                    pending_inline_space = false;
                }
                append_anonymous_inline_text(impl, parent_style,
                                             inline_parent_idx,
                                             std::move(text));
            }
            continue;
        }

        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* elem = lxb_dom_interface_element(child);
        std::string tag = tag_name(elem);

        if (tag == "head" || tag == "script" || tag == "style" ||
            tag == "meta" || tag == "link"   || tag == "title")
            continue;

        // <option>/<optgroup> are not flow content — a <select> renders
        // only its chosen option's text (handled as the select's leaf
        // text below), so don't flatten every option into the box.
        if (tag == "option" || tag == "optgroup")
            continue;

        if (!is_block_tag(tag)) {
            auto rs_inline = impl.resolver->resolve(elem, parent_style);
            auto text = apply_text_transform(
                node_text(child, rs_inline.computed.white_space),
                rs_inline.computed.text_transform);
            if (!text.empty()) {
                const int inline_parent_idx =
                    ensure_inline_run(impl, parent_idx, open_synth_idx);
                if (pending_inline_space) {
                    append_anonymous_inline_text(impl, parent_style,
                                                 inline_parent_idx, " ");
                    pending_inline_space = false;
                }
                append_anonymous_inline_text(impl, rs_inline, inline_parent_idx,
                                             std::move(text));
            }
            continue;
        }

        // Resolve this element's style under the parent's resolved
        // style (so inheritance flows correctly down the tree).
        const auto id = impl.style_store.acquire(elem);
        auto rs = impl.resolver->resolve(elem, parent_style);

        // Pseudo-class overlay (:hover, :active) — at collect time the
        // bits are preserved from any prior interaction state (they
        // survive reset/acquire). dispatch() re-resolves affected
        // blocks when chains change, so collect-time work is the
        // steady-state path. The block's parent_idx is `parent_idx`
        // (function arg), and impl.blocks already contains everything
        // up to but not including this element — exactly what
        // ancestor_chain_matches needs to walk.
        const auto sb_at_collect = impl.style_store.state_bits(id);
        const auto elem_id_attr  = attr_string(elem, "id");
        const auto cls_attr      = split_classes(attr_string(elem, "class"));

        if (sb_at_collect != 0 && !impl.pseudo_rules.empty()) {
            for (const auto& pr : impl.pseudo_rules) {
                std::uint8_t bit;
                switch (pr.pseudo) {
                    case PseudoRule::Pseudo::Hover:  bit = kHoverStateBit;  break;
                    case PseudoRule::Pseudo::Active: bit = kActiveStateBit; break;
                    case PseudoRule::Pseudo::Focus:  bit = kFocusStateBit;  break;
                    default: continue;
                }
                if (!(sb_at_collect & bit)) continue;
                if (!compound_matches(pr.target, tag, elem_id_attr, cls_attr))
                    continue;
                if (!ancestor_chain_matches(pr.ancestors, parent_idx,
                                            impl.blocks)) continue;
                impl.resolver->apply_decl_list(pr.decls, rs);
            }
        }

        // Font-family fill overlay. Same selector grammar as pseudo
        // overlay; later rules win (the scan is in attach order, which
        // matches CSS source order).
        for (const auto& rf : impl.rule_fills) {
            // Pseudo-scoped fills only apply when the state bit is set.
            // Unscoped fills (state_bit == 0) always apply.
            if (rf.state_bit && !(sb_at_collect & rf.state_bit)) continue;
            if (!compound_matches(rf.target, tag, elem_id_attr, cls_attr))
                continue;
            if (!ancestor_chain_matches(rf.ancestors, parent_idx,
                                        impl.blocks)) continue;
            if (!rf.font_family.empty())
                rs.computed.font_id = impl.style_store.intern_font_family(
                    rf.font_family);
        }

        impl.style_store.computed(id) = rs.computed;
        impl.style_store.animated(id) = rs.animated;

        if (auto kw = scan_inline_keyword(elem, "cursor"); !kw.empty()) {
            impl.style_store.computed(id).cursor = parse_cursor_keyword(kw);
        }
        impl.style_store.dirty(id) &=
            static_cast<std::uint8_t>(~detail::StyleStore::DirtyStyle);

        // Inline-run wrapping. Consecutive inline / inline-block
        // siblings get a synthetic flex-row-wrap line-box around
        // them so they flow horizontally instead of stacking. A
        // block-level sibling breaks the run; the next inline
        // sibling opens a fresh line-box.
        //
        // Flex-item blockification (CSS Flexbox §4): the children of a
        // flex container are flex items, and an inline-level flex item is
        // blockified. So when the parent establishes a flex formatting
        // context we DON'T group inline children into a line-box — each
        // becomes a direct block-level flex item. This is what makes an
        // `<a class="nav-link">` inside a `display:flex` navbar lay out as
        // a flex item rather than collapsing into an inline run.
        using Display = detail::ComputedStyle::Display;
        const bool parent_is_flex =
            parent_style.computed.display == Display::Flex;
        const bool child_is_inline =
            !parent_is_flex &&
            (rs.computed.display == Display::Inline ||
             rs.computed.display == Display::InlineBlock);
        int effective_parent_idx;
        if (child_is_inline) {
            ensure_inline_run(impl, parent_idx, open_synth_idx);
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
        b.elem_id    = attr_string(elem, "id");
        b.classes    = split_classes(attr_string(elem, "class"));
        if (b.tag == "img") {
            b.image_src = attr_string(elem, "src");
        }
        if (b.tag == "input" || b.tag == "textarea") {
            b.text_control = true;
            b.placeholder = attr_string(elem, "placeholder");
        }
        b.parent_idx = effective_parent_idx;
        impl.blocks.push_back(std::move(b));

        // Recurse — children get my_idx as their parent. Track whether
        // any blocks were appended; if not, this block is a leaf and
        // gets the concatenated descendant text.
        const std::size_t before = impl.blocks.size();
        collect_blocks(impl, child, rs, my_idx);
        const bool is_leaf = (impl.blocks.size() == before);
        if (is_leaf) {
            auto& leaf = impl.blocks[static_cast<std::size_t>(my_idx)];
            if (leaf.tag == "input") {
                leaf.text = attr_string(elem, "value");
                if (!leaf.text.empty() && attr_string(elem, "type") == "password") {
                    leaf.text = mask_password(leaf.text);
                } else if (leaf.text.empty() && !leaf.placeholder.empty()) {
                    leaf.text = leaf.placeholder;
                    leaf.placeholder_visible = true;
                }
            } else if (leaf.tag == "select") {
                leaf.text = select_display_text(elem);
            } else if (leaf.tag == "textarea") {
                leaf.text = node_text(child, rs.computed.white_space);
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
                          - tcs.border_left - tcs.border_right;
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

        // Pin each cell: content width = column border-box width minus the
        // cell's own padding + border (Yoga adds them back, so the cell's
        // border box equals the column width and the columns line up).
        for (const auto& cells : row_cells) {
            for (std::size_t j = 0; j < cells.size(); ++j) {
                const auto& ccs = *inputs[static_cast<std::size_t>(cells[j])].style;
                int content = colw[j] - ccs.padding_left - ccs.padding_right
                            - ccs.border_left - ccs.border_right;
                if (content < 0) content = 0;
                inputs[static_cast<std::size_t>(cells[j])].intrinsic_w_px = content;
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
    impl_->html.assign(html);
    impl_->blocks.clear();
    impl_->style_store.reset();
    impl_->paint_dirty = true;
    impl_->content_size = Size{0, 0};

#if !defined(AFFINEUI_STUB_BUILD)
    // Tear down the previous document; its CSS pool owns the
    // attached stylesheets, so destroying doc tears them down too.
    impl_->resolver.reset();
    impl_->sheets.clear();
    impl_->pseudo_rules.clear();
    impl_->rule_fills.clear();
    impl_->hovered_chain.clear();
    impl_->active_chain.clear();
    impl_->active_idx = -1;
    impl_->focused_idx = -1;
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

    if (lxb_html_document_parse(
            impl_->doc,
            reinterpret_cast<const lxb_char_t*>(impl_->html.data()),
            impl_->html.size()) != LXB_STATUS_OK) {
        return;
    }

    // Cascade order (lower → higher specificity, ties to last):
    //   1. User-agent baseline
    //   2. Author <style> blocks from the page
    //   3. User stylesheet (App-supplied, often a theme override)
    attach_stylesheet(*impl_, theme::ua_default());

    std::string author_css;
    collect_author_stylesheets(lxb_dom_interface_node(impl_->doc), *impl_, author_css);
    attach_stylesheet(*impl_, author_css);

    attach_stylesheet(*impl_, impl_->user_stylesheet);

    // Resolver runs against the now fully-cascade-attached document.
    impl_->resolver = detail::make_lexbor_resolver(impl_->doc);

    // Establish a root inheritance baseline. Reasonable initial values
    // for the implicit document root — anything not overridden by CSS
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
        impl_->root_style = impl_->resolver->resolve(
            lxb_dom_interface_element(body_node), html_style);
    }
    collect_blocks(*impl_,
                   body ? lxb_dom_interface_node(body)
                        : lxb_dom_interface_node(impl_->doc),
                   impl_->root_style,
                   /*parent_idx=*/-1);
#endif
}

void Document::set_user_stylesheet(std::string_view css) {
    impl_->user_stylesheet.assign(css);
    // Re-cascade on next set_html. Live mutation of the attached
    // stylesheet without a full re-parse is a Phase 2E hot-reload
    // refinement.
    impl_->paint_dirty = true;
}

void Document::reload_stylesheets() {
    if (!impl_->html.empty()) set_html(impl_->html);
}

void Document::layout(int viewport_width, int viewport_height,
                      Painter* measurer) {
    // Layout delegates to Yoga via src/layout/yoga_adapter. Text
    // leaves get a Yoga measure callback that calls nvgTextBoxBounds
    // — Yoga asks "given width W, what height?" and we return the
    // *actually rendered* wrapped bbox. No metric heuristics; the
    // top/bottom padding ends up symmetric for free because the
    // content area matches what the painter will draw into.
    //
    // Page gutter is driven by body's CSS padding. The UA stylesheet
    // keeps body padding at the browser-compatible zero default;
    // demos or applications that want a gutter author it explicitly.
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

    std::vector<detail::BlockLayoutInput> inputs;
    inputs.reserve(impl_->blocks.size());
    for (auto& b : impl_->blocks) {
        const auto& cs = impl_->style_store.computed(b.id);
        detail::BlockLayoutInput in{};
        in.style          = &cs;
        in.parent_idx     = b.parent_idx;
        in.intrinsic_w_px = 0;  // let parent stretch on cross axis

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

        // Container blocks (no direct text — wrap child blocks) leave
        // intrinsic_h at 0 so Yoga sizes them from their children's
        // resolved heights + their own padding/border.
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
            using WS = detail::ComputedStyle::WhiteSpace;
            in.nowrap = (cs.white_space == WS::Nowrap ||
                         cs.white_space == WS::Pre);
            // Leave intrinsic_h_px = 0 — the measure callback supplies
            // the height instead.
        } else {
            in.intrinsic_h_px = cs.font_size_px;
        }
        inputs.push_back(in);
    }

    std::vector<Rect> out(impl_->blocks.size());
    // Yoga's root has no per-block padding of its own. We bake body's
    // padding in by shrinking the viewport handed to Yoga and
    // shifting frames back out below. Cleaner future: a real Box
    // tree where body is its own Yoga node.
    const int inner_w = viewport_width - pad_l - pad_r;
    detail::layout_blocks_with_yoga(inner_w, inputs, out, measurer);

    // Table column alignment. Yoga lays each row out independently, so
    // cells in column N of different rows wouldn't line up. Using the
    // natural cell widths from the pass above, resolve one width per
    // column (the widest cell, scaled to any explicit table width), pin
    // every cell to it via intrinsic_w_px, and re-run layout so columns
    // align. No-op (returns false) when the document has no tables.
    if (assign_table_column_widths(inputs, out)) {
        detail::layout_blocks_with_yoga(inner_w, inputs, out, measurer);
    }

    int max_bottom = 0;
    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        out[i].x += pad_l;
        out[i].y += pad_t;
        impl_->blocks[i].bounds = out[i];
        const int bottom = out[i].y + out[i].h;
        if (bottom > max_bottom) max_bottom = bottom;
    }
    // content_size = max(natural body height, viewport floor). The
    // floor ensures body's background fills the visible window even
    // when natural content is shorter.
    const int natural_h = max_bottom + pad_b;
    impl_->content_size = Size{viewport_width,
                               std::max(natural_h, viewport_height)};

    // Compute per-block content_h = max(child bottom edge) - own top.
    // Used by the scroll path: how far the user can scroll before the
    // last descendant clears the visible region. Iterate children in
    // doc order; each parent gets the max bottom of all its
    // descendants (transitive: child's content already reflects its
    // own descendants).
    for (auto& b : impl_->blocks) b.content_h = b.bounds.h;
    for (std::size_t i = impl_->blocks.size(); i-- > 0; ) {
        const auto& child = impl_->blocks[i];
        if (child.parent_idx < 0) continue;
        auto& parent = impl_->blocks[static_cast<std::size_t>(child.parent_idx)];
        const int child_bottom_in_parent =
            (child.bounds.y - parent.bounds.y) + child.bounds.h;
        if (child_bottom_in_parent > parent.content_h)
            parent.content_h = child_bottom_in_parent;
    }
}

// Forward decls — definitions live in the anonymous namespace below,
// alongside the dispatch helpers. Used by Document::draw to compute
// scroll offsets + clip rects for the paint walk.
namespace {
#if !defined(AFFINEUI_STUB_BUILD)
bool block_is_scrollable_y(const detail::DocumentImpl& impl, int idx);
bool block_clips_overflow(const detail::DocumentImpl& impl, int idx);
int  scroll_offset_y_for(const std::vector<Block>& blocks,
                         const detail::StyleStore& styles, int idx);
#endif
}  // namespace

void Document::draw(Painter& painter) {
    // Document::draw paints through *any* Painter — could be the real
    // NanoVGPainter, could be a DisplayListBuilder that records into
    // a DisplayList. The App layer decides which.
    //
    // This is the "paint" stage of the five-stage pipeline. It walks
    // the box tree, fetches per-element ResolvedStyle from the
    // StyleStore, and emits Painter calls. No GL calls happen here
    // directly — that's the rasterize stage's job.
    //
    // Scroll: per-block, sum ancestor scroll_y to get the effective
    // draw position. If any ancestor is a scrollable container, push
    // its bounds as the clip rect for the duration of this block's
    // draws so overflowing children stay inside the container.

    // Body background fills the page. <body> is the implicit root
    // and isn't in the block list (collect_blocks starts walking its
    // children), so its bg needs an explicit pre-pass. The clear
    // color is the window's, not the page's — without this, body's
    // bg-color silently does nothing.
#if !defined(AFFINEUI_STUB_BUILD)
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

    for (std::size_t i = 0; i < impl_->blocks.size(); ++i) {
        const auto& b  = impl_->blocks[i];
        // Synthetic line-boxes are layout-only. They don't carry
        // any visual style — skip the whole draw stanza.
        if (b.synthetic) continue;
        const auto& cs = impl_->style_store.computed(b.id);
        const auto& an = impl_->style_store.animated(b.id);

        // CSS `display:none` removes the element from layout AND from
        // paint — nothing is drawn, no space is reserved.
        if (cs.display == detail::ComputedStyle::Display::None) {
            continue;
        }

        // CSS `visibility:hidden` (or collapse): the box keeps its
        // layout space but paints nothing — neither this element nor
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

        // Find the nearest ancestor whose overflow clips children
        // (overflow: hidden | clip | scroll | auto). Push its bounds as
        // the scissor rect so overflowing descendants are masked.
        int clip_idx = b.parent_idx;
        while (clip_idx >= 0 && !block_clips_overflow(*impl_, clip_idx)) {
            clip_idx = impl_->blocks[static_cast<std::size_t>(clip_idx)].parent_idx;
        }
        const bool clipped = (clip_idx >= 0);
        if (clipped) {
            painter.push_clip(impl_->blocks[
                static_cast<std::size_t>(clip_idx)].bounds);
        }

        // CSS `opacity` — composite this element's entire subtree at a
        // group alpha. NanoVG's nvgGlobalAlpha multiplies onto whatever
        // alpha is currently set, so save/restore gives clean isolation.
        const bool has_opacity = (an.opacity < 1.0f - 1e-5f);
        if (has_opacity) painter.push_alpha(an.opacity);

        const float r_tl = static_cast<float>(cs.border_radius_top_left_px);
        const float r_tr = static_cast<float>(cs.border_radius_top_right_px);
        const float r_br = static_cast<float>(cs.border_radius_bot_right_px);
        const float r_bl = static_cast<float>(cs.border_radius_bot_left_px);
        const bool any_radius  = (r_tl > 0 || r_tr > 0 || r_br > 0 || r_bl > 0);
        const bool uniform_r   = (r_tl == r_tr && r_tr == r_br && r_br == r_bl);
        // Background: a gradient (if present) wins over the solid color.
        const bool has_gradient =
            (an.gradient_kind != detail::AnimatedStyle::GradientKind::None);
        const bool has_bg = !has_gradient && (an.background_rgba & 0xFFu) != 0;

        // Resolve effective per-side border color. When the per-side
        // override (set by `border-{side}:` shorthands) is non-zero,
        // use it; otherwise fall back to the uniform border_rgba (set by
        // the `border:` shorthand).
        using BS = detail::ComputedStyle::BorderStyle;
        auto side_rgba = [&](std::uint32_t per_side_rgba,
                             std::uint32_t fallback_rgba) -> std::uint32_t {
            return (per_side_rgba != 0) ? per_side_rgba : fallback_rgba;
        };

        const std::uint32_t c_top    = side_rgba(an.border_top_rgba,    an.border_rgba);
        const std::uint32_t c_right  = side_rgba(an.border_right_rgba,  an.border_rgba);
        const std::uint32_t c_bottom = side_rgba(an.border_bottom_rgba, an.border_rgba);
        const std::uint32_t c_left   = side_rgba(an.border_left_rgba,   an.border_rgba);

        // A side is visible if the element's border_style is non-None,
        // the side has a non-zero width, and its effective color is opaque.
        const bool style_active = cs.border_style != BS::None;
        auto side_visible = [&](int w, std::uint32_t rgba) -> bool {
            return style_active && w > 0 && (rgba & 0xFFu) != 0;
        };

        const bool vis_top    = side_visible(cs.border_top,    c_top);
        const bool vis_right  = side_visible(cs.border_right,  c_right);
        const bool vis_bottom = side_visible(cs.border_bottom, c_bottom);
        const bool vis_left   = side_visible(cs.border_left,   c_left);

        const bool has_border = vis_top || vis_right || vis_bottom || vis_left;

        // True when all visible sides share identical color and width, enabling
        // the fast-path stroke_rect / stroke_rounded_rect for the Solid style.
        const bool uniform_border =
            has_border
            && cs.border_style == BS::Solid
            && c_top == c_right && c_right == c_bottom && c_bottom == c_left
            && cs.border_top == cs.border_right && cs.border_right == cs.border_bottom
            && cs.border_bottom == cs.border_left;
        const bool has_shadow = (an.shadow_rgba & 0xFFu) != 0
            && (an.shadow_blur != 0 || an.shadow_spread != 0 ||
                an.shadow_offset_x != 0 || an.shadow_offset_y != 0);

        // Single radius for the shadow primitive (CSS box-shadow follows
        // the largest corner; per-corner shadow radii are not a thing).
        const float shadow_radius = any_radius
            ? std::max({r_tl, r_tr, r_br, r_bl}) : 0.0f;
        // Outset shadow paints BEHIND the background (CSS painting order).
        if (has_shadow && !an.shadow_inset) {
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
            if      (!any_radius)             painter.fill_rect(eff, bg);
            else if (uniform_r)               painter.fill_rounded_rect(eff, r_tl, bg);
            else                              painter.fill_rounded_rect_varying(
                                                  eff, r_tl, r_tr, r_br, r_bl, bg);
        }

        if (has_gradient) {
            const Color s0 = detail::unpack_rgba(an.gradient_stop0_rgba);
            const Color s1 = detail::unpack_rgba(an.gradient_stop1_rgba);
            if (an.gradient_kind == detail::AnimatedStyle::GradientKind::Linear) {
                painter.fill_linear_gradient_rect(
                    eff, static_cast<float>(an.gradient_angle_deg),
                    s0, s1, r_tl, r_tr, r_br, r_bl);
            } else {
                painter.fill_radial_gradient_rect(
                    eff, s0, s1, r_tl, r_tr, r_br, r_bl);
            }
        }

        // Inset shadow paints ON TOP of the background/gradient but under
        // the border and content (CSS painting order).
        if (has_shadow && an.shadow_inset) {
            painter.fill_box_shadow(
                eff, shadow_radius, detail::unpack_rgba(an.shadow_rgba),
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
                    eff.x + cs.border_left + cs.padding_left,
                    eff.y + cs.border_top + cs.padding_top,
                    eff.w - cs.border_left - cs.border_right
                          - cs.padding_left - cs.padding_right,
                    eff.h - cs.border_top - cs.border_bottom
                          - cs.padding_top - cs.padding_bottom,
                };
                if (content_r.w > 0 && content_r.h > 0) {
                    painter.draw_image(image, content_r,
                                       Rect{0, 0, sz.width, sz.height});
                }
            }
        }

        if (has_border) {
            if (uniform_border && !any_radius) {
                // Fast path: uniform solid border, no radius. One NVG stroke
                // (avoids 4 separate lineto calls).
                const float thickness = static_cast<float>(cs.border_top);
                const int   inset     = cs.border_top / 2;
                const Rect  stroke_r{
                    eff.x + inset, eff.y + inset,
                    eff.w - 2 * inset, eff.h - 2 * inset,
                };
                painter.stroke_rect(stroke_r, detail::unpack_rgba(c_top), thickness);
            } else if (uniform_border && any_radius) {
                // Fast path: uniform solid border with border-radius.
                const float thickness = static_cast<float>(cs.border_top);
                const int   inset     = cs.border_top / 2;
                const Rect  stroke_r{
                    eff.x + inset, eff.y + inset,
                    eff.w - 2 * inset, eff.h - 2 * inset,
                };
                const Color bc = detail::unpack_rgba(c_top);
                if (uniform_r) painter.stroke_rounded_rect(stroke_r, r_tl, bc, thickness);
                else           painter.stroke_rounded_rect_varying(
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
                // between the outer horizontal edge endpoints — matching
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

                // Helper: draw one edge segment with a given border style.
                // (ax,ay)→(bx,by) are the outer-edge start/end points.
                // `w` is the border width in px; style and color are side-specific.
                auto draw_edge = [&](float ax, float ay, float bx, float by,
                                     float w, BS style, Color color) {
                    if (w <= 0.0f) return;
                    // Centre of the border stripe, perpendicular to the edge.
                    // For horizontal edges: shift down by w/2. For vertical:
                    // shift right by w/2. Both are embedded in ax/ay already
                    // for our usage — the caller passes midpoint coordinates.
                    switch (style) {
                        case BS::Solid: {
                            painter.stroke_line(ax, ay, bx, by, color, w);
                            break;
                        }
                        case BS::Dashed: {
                            // CSS dashed: dash ≈ 3× border width, gap ≈ border width.
                            const float dash   = std::max(w * 3.0f, 1.0f);
                            const float gap    = std::max(w,         1.0f);
                            const float period = dash + gap;
                            const float dx = bx - ax;
                            const float dy = by - ay;
                            const float len = std::sqrt(dx * dx + dy * dy);
                            if (len <= 0.0f) break;
                            const float ux = dx / len;
                            const float uy = dy / len;
                            float t = 0.0f;
                            while (t < len) {
                                const float dash_end = std::min(t + dash, len);
                                painter.stroke_line(ax + ux * t,  ay + uy * t,
                                                    ax + ux * dash_end,
                                                    ay + uy * dash_end,
                                                    color, w);
                                t += period;
                            }
                            break;
                        }
                        case BS::Dotted: {
                            // CSS dotted: circular dots, diameter = border width,
                            // spaced at ~2× diameter (dot + equal gap).
                            const float radius = w * 0.5f;
                            const float period = w * 2.0f;
                            const float dx = bx - ax;
                            const float dy = by - ay;
                            const float len = std::sqrt(dx * dx + dy * dy);
                            if (len <= 0.0f) break;
                            const float ux = dx / len;
                            const float uy = dy / len;
                            float t = radius;  // first dot centred at w/2 from edge
                            while (t <= len) {
                                painter.fill_circle(ax + ux * t, ay + uy * t,
                                                    radius, color);
                                t += period;
                            }
                            break;
                        }
                        case BS::Double: {
                            // CSS double: outer stroke + gap + inner stroke.
                            // Each stripe is w/3; gap is w/3.
                            // Total border width w = outer(w/3) + gap(w/3) + inner(w/3).
                            // The outer stripe is tangent to the outer edge of the
                            // border box; the inner is tangent to the content box.
                            // The caller passed midpoints of the overall border stripe;
                            // to draw the outer/inner sub-stripes we need to offset
                            // perpendicular to the edge by ±w/3.
                            //
                            // For simplicity we approximate using two parallel
                            // stroke_lines at offsets computed from the edge direction.
                            // Horizontal edge (ay == by): offset in Y.
                            // Vertical edge (ax == bx): offset in X.
                            const float sub_w = std::max(1.0f, w / 3.0f);
                            const float offset = w / 3.0f;  // shift to outer / inner
                            const bool horiz = (std::abs(by - ay) < 0.5f);
                            if (horiz) {
                                // Outer stripe (closer to top outer edge)
                                painter.stroke_line(ax, ay - offset, bx, by - offset,
                                                    color, sub_w);
                                // Inner stripe (closer to content area)
                                painter.stroke_line(ax, ay + offset, bx, by + offset,
                                                    color, sub_w);
                            } else {
                                // Outer stripe (closer to left outer edge)
                                painter.stroke_line(ax - offset, ay, bx - offset, by,
                                                    color, sub_w);
                                // Inner stripe
                                painter.stroke_line(ax + offset, ay, bx + offset, by,
                                                    color, sub_w);
                            }
                            break;
                        }
                        default: break;
                    }
                };

                // All sides share the same style (per-side style variation
                // is Phase 2C+ — see computed_style.h comment).
                const BS bstyle = cs.border_style;

                // Top edge: runs full width from left edge to right edge.
                // Y midpoint = eff.y + border_top/2.
                if (vis_top) {
                    const float wt = static_cast<float>(cs.border_top);
                    const float my = ey + wt * 0.5f;
                    draw_edge(ex, my, ex + ew, my, wt, bstyle,
                              detail::unpack_rgba(c_top));
                }
                // Bottom edge: full width.
                if (vis_bottom) {
                    const float wb = static_cast<float>(cs.border_bottom);
                    const float my = ey + eh - wb * 0.5f;
                    draw_edge(ex, my, ex + ew, my, wb, bstyle,
                              detail::unpack_rgba(c_bottom));
                }
                // Left edge: between the top and bottom edges' outer boundaries.
                if (vis_left) {
                    const float wl  = static_cast<float>(cs.border_left);
                    const float mx  = ex + wl * 0.5f;
                    const float y0  = ey + static_cast<float>(cs.border_top);
                    const float y1  = ey + eh - static_cast<float>(cs.border_bottom);
                    draw_edge(mx, y0, mx, y1, wl, bstyle,
                              detail::unpack_rgba(c_left));
                }
                // Right edge: between top and bottom edges' outer boundaries.
                if (vis_right) {
                    const float wr  = static_cast<float>(cs.border_right);
                    const float mx  = ex + ew - wr * 0.5f;
                    const float y0  = ey + static_cast<float>(cs.border_top);
                    const float y1  = ey + eh - static_cast<float>(cs.border_bottom);
                    draw_edge(mx, y0, mx, y1, wr, bstyle,
                              detail::unpack_rgba(c_right));
                }
            }
        }

        if (!b.text.empty()) {
            const auto font = painter.resolve_font(
                impl_->style_store.font_family_of(cs.font_id), cs.font_size_px, cs.font_weight, cs.font_style != 0);
            const int text_y = eff.y + cs.border_top  + cs.padding_top;

            // Map ComputedStyle::TextAlign → Painter::TextAlign.
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
            int text_x    = eff.x + cs.border_left + cs.padding_left;
            float content_w = static_cast<float>(
                eff.w - cs.border_left - cs.border_right
                      - cs.padding_left - cs.padding_right);

            if (paint_align != Painter::TextAlign::Left && b.synthetic == false) {
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
                        const int al = ae.x + acs.border_left + acs.padding_left;
                        const float aw = static_cast<float>(
                            ae.w - acs.border_left - acs.border_right
                                 - acs.padding_left - acs.padding_right);
                        // Only use the ancestor geometry when it's
                        // meaningfully wider (i.e. the current block is
                        // narrower than the container). This avoids
                        // replacing a block leaf's own width with a
                        // wider ancestor unexpectedly.
                        if (aw > content_w + 1.0f) {
                            text_x    = al;
                            content_w = aw;
                        }
                        break;
                    }
                    anc = ab.parent_idx;
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
            // white-space: nowrap / pre — suppress line-wrapping by
            // passing a very large max-width to both measure and draw.
            using WS = detail::ComputedStyle::WhiteSpace;
            const bool is_nowrap = (cs.white_space == WS::Nowrap ||
                                    cs.white_space == WS::Pre);
            const float draw_max_w = is_nowrap ? 1e6f : content_w + 4.0f;
            const float letter_spacing_px =
                static_cast<float>(cs.letter_spacing_x100) / 100.0f;
            painter.draw_text_box(font, Point{text_x, text_y}, b.text,
                                  detail::unpack_rgba(an.color_rgba),
                                  draw_max_w,
                                  detail::effective_line_height_mult(cs),
                                  letter_spacing_px,
                                  paint_align);
        }

        // <select> dropdown indicator: a small chevron at the right edge.
        // Native selects draw one; Bootstrap's .form-select uses an SVG
        // background we don't rasterize, so the UA supplies it here.
        if (b.tag == "select") {
            const float cx = static_cast<float>(eff.x + eff.w) - 17.0f;
            const float cy = static_cast<float>(eff.y) +
                             static_cast<float>(eff.h) * 0.5f;
            const Color chev{0x34, 0x3a, 0x40, 0xFF};
            painter.stroke_line(cx - 5.0f, cy - 2.5f, cx, cy + 2.5f, chev, 1.5f);
            painter.stroke_line(cx, cy + 2.5f, cx + 5.0f, cy - 2.5f, chev, 1.5f);
        }

        if (has_opacity) painter.pop_alpha();
        if (clipped) painter.pop_clip();
    }

    // Scrollbar overlay — drawn last so it sits on top of any
    // clipped content. A simple right-side thumb showing how far
    // we've scrolled; track is transparent.
    for (const auto& b : impl_->blocks) {
        if (!block_is_scrollable_y(*impl_, static_cast<int>(&b - impl_->blocks.data())))
            continue;
        const int track_w  = 6;
        const int track_pad = 2;
        const int track_x  = b.bounds.x + b.bounds.w - track_w - track_pad;
        const int track_y  = b.bounds.y + track_pad;
        const int track_h  = b.bounds.h - 2 * track_pad;
        const float ratio  = static_cast<float>(b.bounds.h)
                           / static_cast<float>(b.content_h);
        const int thumb_h  = std::max(24, static_cast<int>(
                               static_cast<float>(track_h) * ratio));
        const int scroll_range = std::max(1, b.content_h - b.bounds.h);
        const int thumb_y_off  = static_cast<int>(
            static_cast<float>(track_h - thumb_h) *
            static_cast<float>(b.scroll_y) /
            static_cast<float>(scroll_range));
        const Rect thumb{track_x, track_y + thumb_y_off, track_w, thumb_h};
        // Catppuccin overlay0-ish, semi-transparent.
        painter.fill_rounded_rect(thumb, 3.0f, Color{0x9c, 0xa0, 0xb0, 0xC0});
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
    int p = (idx >= 0) ? blocks[static_cast<std::size_t>(idx)].parent_idx : -1;
    while (p >= 0) {
        const auto& pb = blocks[static_cast<std::size_t>(p)];
        const auto ov = styles.computed(pb.id).overflow_y;
        if ((ov == O::Scroll || ov == O::Auto) && pb.scroll_y != 0) {
            sum += pb.scroll_y;
        }
        p = pb.parent_idx;
    }
#else
    (void)blocks; (void)idx;
#endif
    return sum;
}

// Deepest block whose effective border-box (after applying any
// ancestor scroll offsets) contains (x, y), or -1 if none. Walk in
// DFS order — parents before children — so the *last* match wins.
int hit_test_blocks(const std::vector<Block>& blocks,
#if !defined(AFFINEUI_STUB_BUILD)
                    const detail::StyleStore& styles,
#endif
                    int x, int y) {
    int hit = -1;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
#if !defined(AFFINEUI_STUB_BUILD)
        const int dy = scroll_offset_y_for(blocks, styles, static_cast<int>(i));
#else
        const int dy = 0;
#endif
        Rect eff = blocks[i].bounds;
        eff.y -= dy;
        if (rect_contains(eff, x, y)) {
            hit = static_cast<int>(i);
        }
    }
    return hit;
}

}  // namespace

namespace {
#if !defined(AFFINEUI_STUB_BUILD)
// Build the ancestor chain (deepest → root) for the block at `idx`.
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
    return rs;
}

// Apply currently-active pseudo-class overlays (per the block's
// state_bits) on top of `rs`. Shared by restyle_block (dispatch
// path) and the equivalent collect-time path inline above.
void apply_pseudo_overlay(detail::DocumentImpl& impl, const Block& block,
                          detail::ResolvedStyle& rs) {
    const auto sb = impl.style_store.state_bits(block.id);
    if (sb == 0) return;
    for (const auto& pr : impl.pseudo_rules) {
        std::uint8_t bit;
        switch (pr.pseudo) {
            case PseudoRule::Pseudo::Hover:  bit = kHoverStateBit;  break;
            case PseudoRule::Pseudo::Active: bit = kActiveStateBit; break;
            case PseudoRule::Pseudo::Focus:  bit = kFocusStateBit;  break;
            default: continue;
        }
        if (!(sb & bit)) continue;
        if (!compound_matches(pr.target, block.tag, block.elem_id,
                              block.classes)) continue;
        if (!ancestor_chain_matches(pr.ancestors, block.parent_idx,
                                    impl.blocks)) continue;
        impl.resolver->apply_decl_list(pr.decls, rs);
    }
}

// Re-resolve one block's style (paint-only properties), applying any
// active pseudo overlays. We don't touch layout: pseudo overlays
// affecting layout would require relayout-on-state, which lands with
// the broader restyle queue in Phase 4. Bounds in the block stay put.
void restyle_block(detail::DocumentImpl& impl, int idx) {
    auto& block = impl.blocks[static_cast<std::size_t>(idx)];
    auto* elem  = impl.style_store.element_of(block.id);
    if (!elem) return;
    auto parent = parent_resolved(impl, idx);
    auto rs     = impl.resolver->resolve(elem, parent);
    apply_pseudo_overlay(impl, block, rs);
    // Re-apply font-family fills too: restyle_block runs on the dispatch
    // path (hover/active/focus toggles), and pseudo-scoped fills gate on
    // the matching state bit being set.
    const auto sb_rs = impl.style_store.state_bits(block.id);
    for (const auto& rf : impl.rule_fills) {
        if (rf.state_bit && !(sb_rs & rf.state_bit)) continue;
        if (!compound_matches(rf.target, block.tag, block.elem_id, block.classes))
            continue;
        if (!ancestor_chain_matches(rf.ancestors, block.parent_idx, impl.blocks))
            continue;
        if (!rf.font_family.empty())
            rs.computed.font_id = impl.style_store.intern_font_family(
                rf.font_family);
    }
    impl.style_store.computed(block.id) = rs.computed;
    impl.style_store.animated(block.id) = rs.animated;
    impl.paint_dirty = true;
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
    // Leaving blocks: clear bit + restyle.
    for (int old_idx : current_chain) {
        if (in(old_idx, new_chain)) continue;
        const auto id = impl.blocks[static_cast<std::size_t>(old_idx)].id;
        impl.style_store.state_bits(id) &= static_cast<std::uint8_t>(~bit);
        restyle_block(impl, old_idx);
    }
    // Entering blocks: set bit + restyle.
    for (int new_idx : new_chain) {
        if (in(new_idx, current_chain)) continue;
        const auto id = impl.blocks[static_cast<std::size_t>(new_idx)].id;
        impl.style_store.state_bits(id) |= bit;
        restyle_block(impl, new_idx);
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
// element rather than a chain — there is no inheritance up the
// ancestor list.
bool set_focus(detail::DocumentImpl& impl, int target_idx) {
    if (target_idx == impl.focused_idx) return false;
    const int old_idx = impl.focused_idx;
    impl.focused_idx  = target_idx;
    if (old_idx >= 0 && old_idx < static_cast<int>(impl.blocks.size())) {
        const auto id = impl.blocks[static_cast<std::size_t>(old_idx)].id;
        impl.style_store.state_bits(id) &= static_cast<std::uint8_t>(~kFocusStateBit);
        restyle_block(impl, old_idx);
    }
    if (target_idx >= 0 && target_idx < static_cast<int>(impl.blocks.size())) {
        const auto id = impl.blocks[static_cast<std::size_t>(target_idx)].id;
        impl.style_store.state_bits(id) |= kFocusStateBit;
        restyle_block(impl, target_idx);
    }
    return true;
}

// Walk up from `idx` looking for the nearest focusable element. A tag
// is focusable if it natively accepts keyboard input today — buttons,
// inputs, textareas, selects, and <a href>. Returns -1 when no such
// ancestor exists; callers should treat that as "click outside any
// focusable element → clear focus".
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
#else  // stub build — no DOM, no pseudo / scroll bookkeeping
bool refresh_hover_chain(detail::DocumentImpl&)  { return false; }
bool refresh_active_chain(detail::DocumentImpl&) { return false; }
bool set_focus(detail::DocumentImpl&, int)       { return false; }
int  focusable_ancestor(const detail::DocumentImpl&, int) { return -1; }
int  find_scrollable_y_ancestor(const detail::DocumentImpl&, int) { return -1; }
bool focused_text_control(detail::DocumentImpl&, Block*&) { return false; }
void remove_last_utf8_codepoint(std::string&) {}
#endif
}  // namespace

DispatchResult Document::dispatch(const Event& ev) {
    DispatchResult result{};
    switch (ev.type) {
        case EventType::MouseMove: {
            impl_->last_mouse_pos = ev.pos;
            const int new_hover = hit_test_blocks(impl_->blocks, impl_->style_store, ev.pos.x, ev.pos.y);
            if (new_hover != impl_->hovered_idx) {
                impl_->hovered_idx      = new_hover;
                result.redraw_requested = true;
            }
            // Refresh :hover chain even when hovered_idx didn't change —
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
            impl_->hovered_idx    = hit_test_blocks(impl_->blocks, impl_->style_store, ev.pos.x, ev.pos.y);
            // :active follows the press: set to whatever's under the
            // pointer right now, refresh the active chain so the bit
            // toggles on and an immediate restyle visualizes the press.
            impl_->active_idx     = impl_->hovered_idx;
            const bool h = refresh_hover_chain(*impl_);
            const bool a = refresh_active_chain(*impl_);
            // Focus moves to the nearest focusable ancestor of whatever
            // the press landed on. Clicking blank space (no focusable
            // ancestor) clears focus, matching browser behavior for
            // mousedown outside any form control.
            const int target = focusable_ancestor(*impl_, impl_->hovered_idx);
            const bool f = set_focus(*impl_, target);
            if (h || a || f) result.redraw_requested = true;
            break;
        }
        case EventType::MouseUp: {
            impl_->last_mouse_pos = ev.pos;
            impl_->hovered_idx    = hit_test_blocks(impl_->blocks, impl_->style_store, ev.pos.x, ev.pos.y);
            // Clear :active on every MouseUp — the press is over. We
            // don't try to be clever about "release outside the
            // pressed element" today; that nuance is part of the
            // click-state machinery to layer in later.
            impl_->active_idx     = -1;
            const bool h = refresh_hover_chain(*impl_);
            const bool a = refresh_active_chain(*impl_);
            if (h || a) result.redraw_requested = true;
            break;
        }
#if !defined(AFFINEUI_STUB_BUILD)
        case EventType::KeyDown: {
            // ESC clears focus, matching the convention browsers use for
            // dismissing a focused control.
            if (ev.key == Key::Escape) {
                if (set_focus(*impl_, -1)) result.redraw_requested = true;
            } else if (ev.key == Key::Backspace) {
                Block* control = nullptr;
                if (focused_text_control(*impl_, control)) {
                    if (control->placeholder_visible) {
                        control->text.clear();
                        control->placeholder_visible = false;
                    } else {
                        remove_last_utf8_codepoint(control->text);
                    }
                    if (control->text.empty() && !control->placeholder.empty()) {
                        control->text = control->placeholder;
                        control->placeholder_visible = true;
                    }
                    impl_->content_size = Size{0, 0};
                    impl_->paint_dirty = true;
                    result.redraw_requested = true;
                }
            }
            break;
        }
        case EventType::TextInput: {
            Block* control = nullptr;
            if (focused_text_control(*impl_, control) && !ev.text.empty()) {
                if (control->placeholder_visible) {
                    control->text.clear();
                    control->placeholder_visible = false;
                }
                control->text += ev.text;
                impl_->content_size = Size{0, 0};
                impl_->paint_dirty = true;
                result.redraw_requested = true;
            }
            break;
        }
        case EventType::MouseWheel: {
            // Route to the nearest scrollable-Y ancestor of whatever
            // the pointer is over. Convention: positive wheel_dy
            // scrolls content up (i.e. scroll position increases).
            // The platform adapter is responsible for normalizing
            // direction + step size before we get here.
            const int target = find_scrollable_y_ancestor(
                *impl_, impl_->hovered_idx);
            if (target < 0) break;
            auto& sb = impl_->blocks[static_cast<std::size_t>(target)];
            constexpr int kPxPerWheelStep = 24;
            const int delta = static_cast<int>(
                -ev.wheel_dy * kPxPerWheelStep);
            const int max_scroll = std::max(0, sb.content_h - sb.bounds.h);
            const int next       = std::clamp(sb.scroll_y + delta,
                                              0, max_scroll);
            if (next != sb.scroll_y) {
                sb.scroll_y             = next;
                impl_->paint_dirty      = true;
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
// cursor returns Default — so this walk is mostly belt-and-braces.
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
}  // namespace

/// Cursor the OS should display right now (under the last mouse pos).
/// Lives on the public Document surface so App can poll it once per
/// frame without taking a Painter-style dependency.
int Document::hovered_cursor() const {
    return static_cast<int>(
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
    return info;
}

void Document::set_resource_loader(ResourceLoader loader) {
    impl_->resource_loader = std::move(loader);
}

Size Document::content_size() const { return impl_->content_size; }

// ── Immediate mode ──────────────────────────────────────────────────

void Document::set_imm_view(std::function<void()> view_fn) {
    if (!impl_->imm) impl_->imm = std::make_unique<detail::ImmRuntime>();
#if !defined(AFFINEUI_STUB_BUILD)
    if (!impl_->doc) {
        // No DOM yet — establish a minimal empty document so the
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
    // 1. Run the view fn — it mutates lexbor's DOM directly via the
    //    runtime, replacing the body's children.
    impl_->imm->run_view_fn();

    // 2. Re-cascade + re-collect. This is the same tail as set_html
    //    after parsing — minus the stylesheet re-attach (those are
    //    still bound to impl_->doc from the original set_html).
    impl_->blocks.clear();
    impl_->style_store.reset();

    impl_->root_style                       = detail::ResolvedStyle{};
    impl_->root_style.animated.color_rgba   = 0xDCDCE6FFu;
    impl_->root_style.computed.font_size_px = 16;
    impl_->root_style.computed.font_weight  = 400;

    auto* body = lxb_html_document_body_element(impl_->doc);
    if (body && impl_->resolver) {
        impl_->root_style = impl_->resolver->resolve(
            lxb_dom_interface_element(lxb_dom_interface_node(body)),
            impl_->root_style);
    }
    collect_blocks(*impl_,
                   body ? lxb_dom_interface_node(body)
                        : lxb_dom_interface_node(impl_->doc),
                   impl_->root_style,
                   /*parent_idx=*/-1);

    // 3. Stale-state cleanup. Block indices have churned; hovered_idx
    //    + the cached *_chain vectors all point into the old vector.
    //    Reset; next MouseMove rebuilds correctly. (Without this, the
    //    chain held indices that could land on a different block
    //    after re-collect, leaving rows stuck on hover style.)
    impl_->hovered_idx   = -1;
    impl_->active_idx    = -1;
    impl_->focused_idx   = -1;
    impl_->hovered_chain.clear();
    impl_->active_chain.clear();
    impl_->content_size = Size{0, 0};   // forces relayout in Renderer
#endif
}

bool Document::invoke_imm_click(std::string_view elem_id) {
    return impl_->imm && impl_->imm->invoke_click(elem_id);
}

}  // namespace affineui
