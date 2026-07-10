// document_style.cpp — part of the AffineUI HTML5 renderer's document implementation.
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
// Defined later in this file (font-face scanning).
void scan_font_face_rules(std::string_view css,
                          std::string_view stylesheet_base_url,
                          std::vector<FontFaceRule>& out);
#endif
}  // namespace

namespace {  // (balance: opener stayed above split)
// â”€â”€ DOM utilities â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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
) {
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
        if (detail::is_html_ws(c)) {
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
                if (detail::is_html_ws(static_cast<unsigned char>(c))) {
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
        if (!detail::is_html_ws(static_cast<unsigned char>(raw[i]))) {
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
    const auto rs = detail::anonymous_text_style(parent_style);
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
    // A line box is transparent to inheritance: children of the run resolve
    // through it via parent_resolved() on the restyle path, so it must carry
    // the parent's custom-property scope. Without this, any element wrapped
    // in an anonymous inline run lost every var() (all --dcs-* colors) on
    // its first attribute-driven restyle — masked for years because those
    // restyles used to be full box rebuilds, which re-resolve through the
    // real ancestor chain instead.
    synth.custom_props = parent_style.custom_props;
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
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// Attribute value as a view into lexbor's storage — no copy. Only valid
// until the attribute mutates; for read-and-compare use exclusively.
std::string_view attr_view(lxb_dom_element_t* elem, std::string_view name) {
    size_t len = 0;
    const lxb_char_t* v = lxb_dom_element_get_attribute(
        elem,
        reinterpret_cast<const lxb_char_t*>(name.data()), name.size(),
        &len);
    if (!v || len == 0) return {};
    return {reinterpret_cast<const char*>(v), len};
}

// Element tag as a view into lexbor's qualified-name storage — no copy.
std::string_view tag_view(lxb_dom_element_t* elem) {
    size_t len = 0;
    const lxb_char_t* name = lxb_dom_element_qualified_name(elem, &len);
    if (!name || len == 0) return {};
    return {reinterpret_cast<const char*>(name), len};
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
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// True if a (possibly value-less, boolean) attribute is present.
bool has_attr(lxb_dom_element_t* elem, std::string_view name) {
    return lxb_dom_element_has_attribute(
        elem, reinterpret_cast<const lxb_char_t*>(name.data()), name.size());
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
// The text a closed <select> shows: the `selected` <option>'s text, or
// the first option if none is marked. We render no popup/list â€” only the
// chosen option â€” matching a closed native control.
std::string select_display_text(lxb_dom_element_t* select) {
    lxb_dom_node_t* first = nullptr;
    for (auto* c = lxb_dom_node_first_child(lxb_dom_interface_node(select));
         c != nullptr; c = lxb_dom_node_next(c)) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* el = lxb_dom_interface_element(c);
        if (detail::tag_name(el) != "option") continue;
        if (first == nullptr) first = c;
        if (detail::has_attr(el, "selected")) return detail::node_text(c);
    }
    return first ? detail::node_text(first) : std::string{};
}
}  // namespace detail
namespace {


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

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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

// Whitespace-token membership test on a raw class attribute value —
// the allocation-free form of `detail::split_classes(...) contains token`.
// Selector matching runs this per rule × element during collect, where
// materializing token vectors was a measured hot spot (knob-drag
// recollects: ~15% of all CPU samples were these temporaries).
bool class_tokens_contain(std::string_view s, std::string_view token) {
    if (token.empty()) return false;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
        if (i >= s.size()) break;
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\n') ++j;
        if (s.substr(i, j - i) == token) return true;
        i = j;
    }
    return false;
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
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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
    return detail::block_attr_value(block, name) != nullptr;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
int block_attr_int(const Block& block,
                   std::string_view name,
                   int fallback,
                   int min_value,
                   int max_value) {
    const auto* value = detail::block_attr_value(block, name);
    if (!value || value->empty()) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value->c_str(), &end, 10);
    if (end == value->c_str()) return fallback;
    return std::clamp(static_cast<int>(parsed), min_value, max_value);
}
}  // namespace detail
namespace {



}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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
        if (const auto* name = detail::block_attr_value(block, "data-aui-name")) {
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
        const auto* name = detail::block_attr_value(block, "data-aui-name");
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
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
double block_attr_double(const Block& block,
                         std::string_view name,
                         double fallback) {
    const auto* value = detail::block_attr_value(block, name);
    if (!value || value->empty()) return fallback;

    char* end = nullptr;
    const double parsed = std::strtod(value->c_str(), &end);
    if (end == value->c_str()) return fallback;
    return parsed;
}
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::string compact_number(double value, int places) {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%.*f", places, value);
    std::string out{buf};
    // Trim trailing zeros only in the FRACTIONAL part — otherwise
    // "50" (0 decimals) would lose its trailing zero and read "5".
    if (out.find('.') != std::string::npos) {
        while (out.size() > 1 && out.back() == '0') out.pop_back();
        if (!out.empty() && out.back() == '.') out.pop_back();
    }
    return out;
}

std::string percent_string(double fraction) {
    return detail::compact_number(std::clamp(fraction, 0.0, 1.0) * 100.0) + "%";
}
}  // namespace detail
namespace {


}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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
}  // namespace detail
namespace {


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

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void collect_author_stylesheets(lxb_dom_node_t* node,
                                detail::DocumentImpl& impl,
                                std::string& out) {
    for (auto* c = lxb_dom_node_first_child(node); c;
         c = lxb_dom_node_next(c)) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            auto* el = lxb_dom_interface_element(c);
            const auto tag = detail::tag_name(el);
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
                const auto rel = detail::attr_string(el, "rel");
                const auto href = detail::attr_string(el, "href");
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
        detail::collect_author_stylesheets(c, impl, out);
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
                          attrs) {
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
}  // namespace detail
namespace {


int collapse_vertical_margins(int a, int b) {
    if (a >= 0 && b >= 0) return std::max(a, b);
    if (a <= 0 && b <= 0) return std::min(a, b);
    return a + b;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::int16_t clamp_css_px(int value) {
    return static_cast<std::int16_t>(std::clamp(
        value,
        static_cast<int>(std::numeric_limits<std::int16_t>::min()),
        static_cast<int>(std::numeric_limits<std::int16_t>::max())));
}
}  // namespace detail
namespace {


bool is_block_flow_box(const Block& block,
                       const detail::ComputedStyle& style) {
    return !block.synthetic &&
           (style.display == detail::ComputedStyle::Display::Block ||
            style.display == detail::ComputedStyle::Display::ListItem);
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool is_flex_container_display(detail::ComputedStyle::Display display) {
    return display == detail::ComputedStyle::Display::Flex ||
           display == detail::ComputedStyle::Display::InlineFlex;
}
}  // namespace detail
namespace {


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

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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
            parent_style.margin_top = detail::clamp_css_px(collapse_vertical_margins(
                parent_style.margin_top, child_style.margin_top));
            child_style.margin_top = 0;
        }

        const int last = kids.back();
        auto& last_child_style = styles[static_cast<std::size_t>(last)];
        if (can_collapse_last_child_bottom_margin(
                blocks[static_cast<std::size_t>(pi)], parent_style,
                blocks[static_cast<std::size_t>(last)], last_child_style)) {
            parent_style.margin_bottom = detail::clamp_css_px(collapse_vertical_margins(
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
                current_style.margin_top = detail::clamp_css_px(
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
// compound in order. Descendant combinators allow gaps; a child combinator
// (`A > B`, CompoundSelector::direct_parent) must match the immediate parent.
// NB: synthetic blocks (#inline runs) sit between an element and its DOM
// children in the block tree; skip them so `>` means DOM-parent, not
// block-parent.
bool ancestor_chain_matches(const std::vector<CompoundSelector>& ancestors,
                            int parent_idx,
                            const std::vector<Block>& blocks) {
    auto next_real_parent = [&](int idx) {
        while (idx >= 0 &&
               blocks[static_cast<std::size_t>(idx)].synthetic) {
            idx = blocks[static_cast<std::size_t>(idx)].parent_idx;
        }
        return idx;
    };
    int idx = next_real_parent(parent_idx);
    for (const auto& compound : ancestors) {
        if (idx < 0) return false;
        if (compound.direct_parent) {
            const auto& a = blocks[static_cast<std::size_t>(idx)];
            if (!detail::compound_matches(compound, a.tag, a.elem_id, a.classes,
                                  &a.attrs)) {
                return false;
            }
            idx = next_real_parent(a.parent_idx);
            continue;
        }
        bool matched = false;
        while (idx >= 0) {
            const auto& a = blocks[static_cast<std::size_t>(idx)];
            const bool m = detail::compound_matches(compound, a.tag, a.elem_id,
                                            a.classes, &a.attrs);
            idx = next_real_parent(a.parent_idx);
            if (m) {
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    return true;
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
    if (!detail::compound_matches(state_target, block.tag, block.elem_id,
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
        if (detail::compound_matches(state_target, a.tag, a.elem_id, a.classes,
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
                                        detail::kMaxGridTrackHints>* grid_columns,
                             std::uint8_t* grid_column_count) {
    for (const auto& rf : impl.rule_fills) {
        // Pseudo-scoped fills only apply when the state bit is set.
        // Unscoped fills (state_bit == 0) always apply.
        if (rf.state_bit && !(state_bits & rf.state_bit)) continue;
        if (!detail::compound_matches(rf.target, tag, elem_id, classes)) continue;
        if (!detail::ancestor_chain_matches(rf.ancestors, parent_idx, impl.blocks))
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
        if (rf.has_background_clip) {
            rs.computed.set_background_clip(
                static_cast<detail::ComputedStyle::BackgroundClip>(
                    rf.background_clip));
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
}  // namespace detail
namespace {


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
            std::vector<bool>             preceded_by_child;  // parallel
            CompoundSelector              current;
            bool                          current_child = false;
            PseudoRule::Pseudo            pseudo{};
            bool                          has_pseudo  = false;
            std::size_t                   pseudo_compound_index = 0;
            bool                          ok          = true;

            for (auto* sel = sl->first; sel != nullptr; sel = sel->next) {
                const bool starts_new_compound =
                    (sel != sl->first) &&
                    (sel->combinator != LXB_CSS_SELECTOR_COMBINATOR_CLOSE);
                if (starts_new_compound) {
                    if (sel->combinator !=
                            LXB_CSS_SELECTOR_COMBINATOR_DESCENDANT &&
                        sel->combinator !=
                            LXB_CSS_SELECTOR_COMBINATOR_CHILD) {
                        // `+`, `~`, `||` — not in this grammar.
                        ok = false; break;
                    }
                    compounds.push_back(std::move(current));
                    preceded_by_child.push_back(current_child);
                    current_child =
                        sel->combinator == LXB_CSS_SELECTOR_COMBINATOR_CHILD;
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
            preceded_by_child.push_back(current_child);
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
            // nearest â†’ root. direct_parent on ancestors[j] = "must be the
            // DIRECT parent of the compound one step to its right" =
            // preceded_by_child of that right-hand compound (same convention
            // as parse_static_selector).
            const std::size_t n = compounds.size();  // ancestors only now
            pr.ancestors.reserve(n);
            for (std::size_t j = n; j-- > 0;) {
                CompoundSelector a = std::move(compounds[j]);
                a.direct_parent = preceded_by_child[j + 1];
                pr.ancestors.push_back(std::move(a));
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
}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::string_view trim_css_ws(std::string_view s) { return ltrim_ws(rtrim_ws(s)); }
}  // namespace detail
namespace {

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::string style_with_properties(
    std::string_view style,
    std::initializer_list<std::pair<std::string, std::string>> props) {
    std::vector<std::string_view> names;
    names.reserve(props.size());
    for (const auto& p : props) names.push_back(p.first);

    std::string out;
    std::size_t i = 0;
    while (i < style.size()) {
        const std::size_t semi = style.find(';', i);
        const std::string_view decl = style.substr(
            i, semi == std::string_view::npos ? semi : semi - i);
        i = (semi == std::string_view::npos) ? style.size() : semi + 1;
        const std::size_t colon = decl.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view prop = detail::trim_css_ws(decl.substr(0, colon));
            if (std::find(names.begin(), names.end(), prop) != names.end()) {
                continue;
            }
        }
        const std::string_view t = detail::trim_css_ws(decl);
        if (!t.empty()) {
            if (!out.empty()) out += ';';
            out += t;
        }
    }
    for (const auto& [name, value] : props) {
        if (!out.empty()) out += ';';
        out += name;
        out += ':';
        out += value;
    }
    return out;
}
}  // namespace detail
namespace {


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

    auto body = detail::trim_css_ws(sel.substr(i + 1, close - i - 1));
    if (body.empty()) return false;

    SimpleSelector s;
    s.kind = SimpleSelector::Kind::Attr;

    const auto eq = body.find('=');
    if (eq == std::string_view::npos) {
        s.name = std::string(detail::trim_css_ws(body));
    } else {
        const auto name = detail::trim_css_ws(body.substr(0, eq));
        auto value = detail::trim_css_ws(body.substr(eq + 1));
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
    // preceded_by_child[i]: a `>` combinator sits between compounds[i-1] and
    // compounds[i] — compounds[i-1] must be the DIRECT parent of compounds[i].
    std::vector<bool> preceded_by_child;
    bool pseudo_seen = false;
    bool pending_child = false;
    std::size_t i = 0;
    while (i < sel.size()) {
        while (i < sel.size() && is_css_ws(sel[i])) ++i;
        if (i < sel.size() && sel[i] == '>') {
            if (compounds.empty() || pending_child) return false;
            pending_child = true;
            ++i;
            while (i < sel.size() && is_css_ws(sel[i])) ++i;
        }
        if (i >= sel.size()) break;
        if (pseudo_seen) return false;  // pseudo must be on last compound
        CompoundSelector compound;
        // Optional leading tag (anything not starting with . # or :).
        if (sel[i] != '.' && sel[i] != '#' && sel[i] != ':' &&
            sel[i] != '[') {
            const std::size_t s = i;
            while (i < sel.size() && sel[i] != '.' && sel[i] != '#' &&
                   sel[i] != ':' && sel[i] != '[' && sel[i] != '>' &&
                   !is_css_ws(sel[i])) ++i;
            if (s == i) return false;
            compound.simples.push_back(
                {SimpleSelector::Kind::Tag, std::string(sel.substr(s, i - s))});
        }
        // Then any number of `.name` / `#name` segments, optionally
        // followed by a single `:pseudo` recognized below.
        while (i < sel.size() && !is_css_ws(sel[i]) && sel[i] != '>') {
            if (sel[i] == '[') {
                if (!parse_attribute_simple(sel, i, compound)) return false;
                continue;
            }
            if (sel[i] == ':') {
                if (pseudo_seen) return false;
                ++i;
                const std::size_t s = i;
                while (i < sel.size() && sel[i] != '.' && sel[i] != '#' &&
                       sel[i] != ':' && sel[i] != '[' && sel[i] != '>' &&
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
                   sel[i] != ':' && sel[i] != '[' && sel[i] != '>' &&
                   !is_css_ws(sel[i])) ++i;
            if (s == i) return false;
            compound.simples.push_back(
                {kind, std::string(sel.substr(s, i - s))});
        }
        if (compound.simples.empty()) return false;
        compounds.push_back(std::move(compound));
        preceded_by_child.push_back(pending_child);
        pending_child = false;
    }
    if (pending_child) return false;  // dangling `>`
    if (compounds.empty()) return false;
    // ancestors[j] holds the compound j+1 steps left of the target;
    // direct_parent on ancestors[j] = "ancestors[j] is the DIRECT parent of
    // the element matched one step to its right" = preceded_by_child of that
    // right-hand compound.
    const std::size_t n = compounds.size();
    target = std::move(compounds.back());
    ancestors.reserve(n - 1);
    for (std::size_t j = n - 1; j-- > 0;) {
        CompoundSelector a = std::move(compounds[j]);
        a.direct_parent = preceded_by_child[j + 1];
        ancestors.push_back(std::move(a));
    }
    return true;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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
        return std::string(detail::trim_css_ws(rest.substr(0, end)));
    }
    return {};
}
}  // namespace detail
namespace {


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
    value = detail::trim_css_ws(value);
    const auto comma = find_top_level_comma(value);
    if (comma != std::string_view::npos) value = value.substr(0, comma);
    const auto important = value.find('!');
    if (important != std::string_view::npos) {
        value = detail::trim_css_ws(value.substr(0, important));
    }
    return strip_css_quotes(std::string(detail::trim_css_ws(value)));
}

std::string css_keyword_value(std::string value) {
    value = std::string(detail::trim_css_ws(value));
    const auto important = value.find('!');
    if (important != std::string::npos) {
        value = std::string(detail::trim_css_ws(
            std::string_view(value).substr(0, important)));
    }
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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
}  // namespace detail
namespace {


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
        value = std::string(detail::trim_css_ws(
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
            auto tok = detail::trim_css_ws(value.substr(start, i - start));
            if (!tok.empty()) out.push_back(tok);
            start = std::string_view::npos;
        }
    }
    return out;
}

bool parse_css_number(std::string_view s, float& out) {
    s = detail::trim_css_ws(s);
    if (s.empty()) return false;
    std::string tmp(s);
    char* end = nullptr;
    out = std::strtof(tmp.c_str(), &end);
    return end != tmp.c_str();
}

std::string_view css_function_inner(std::string_view value,
                                    std::string_view name) {
    value = detail::trim_css_ws(value);
    if (!starts_with_ascii_ci(value, name)) return {};
    auto rest = detail::trim_css_ws(value.substr(name.size()));
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
        tok = detail::trim_css_ws(tok);
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
    tok = detail::trim_css_ws(tok);
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

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
std::uint8_t parse_grid_template_columns(
        std::string value,
        std::array<detail::GridTrackHint,
                   detail::kMaxGridTrackHints>& out) {
    value = strip_css_important(std::string(detail::trim_css_ws(value)));
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
}  // namespace detail
namespace {


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
    url = detail::trim_css_ws(url);
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
    auto arg = detail::trim_css_ws(s.substr(open + 1, close - open - 1));
    return strip_css_quotes(std::string(arg));
}

int parse_font_face_weight(std::string_view value) {
    value = detail::trim_css_ws(value);
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
        const auto item = detail::trim_css_ws(src.substr(
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
        face.family = first_font_family(detail::find_decl_value(decls, "font-family"));
        face.weight = parse_font_face_weight(detail::find_decl_value(decls, "font-weight"));
        face.italic = parse_font_face_style(detail::find_decl_value(decls, "font-style"));
        face.sources = parse_font_face_sources(
            detail::find_decl_value(decls, "src"), stylesheet_base_url);
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

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void ensure_font_faces_registered(detail::DocumentImpl& impl,
                                  Painter& painter) {
    static const bool font_trace =
        std::getenv("AFFINEUI_FONT_TRACE") != nullptr;
    if (!impl.resource_loader) {
        if (font_trace && !impl.font_faces.empty()) {
            std::fprintf(stderr,
                         "[font] %zu @font-face rules but NO resource loader "
                         "— none can load\n",
                         impl.font_faces.size());
        }
        return;
    }

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
            if (font_trace) {
                std::fprintf(stderr,
                             "[font] '%s' w%d src '%s' (%s): %zu bytes\n",
                             face.family.c_str(), face.weight,
                             source.url.c_str(), source.format.c_str(),
                             bytes.size());
            }
            if (bytes.empty()) continue;
            if (painter.register_font_face(face.family, face.weight,
                                           face.italic, bytes)) {
                face.loaded = true;
                if (font_trace) {
                    std::fprintf(stderr, "[font] '%s' w%d REGISTERED\n",
                                 face.family.c_str(), face.weight);
                }
                break;
            } else if (font_trace) {
                std::fprintf(stderr, "[font] '%s' w%d register FAILED\n",
                             face.family.c_str(), face.weight);
            }
        }
        face.attempted = !face.loaded;
        if (font_trace && !face.loaded) {
            std::fprintf(stderr, "[font] '%s' w%d NOT LOADED (%zu sources)\n",
                         face.family.c_str(), face.weight,
                         face.sources.size());
        }
    }
}
}  // namespace detail
namespace {

bool selector_list_contains_root(std::string_view selector) {
    while (!selector.empty()) {
        const auto comma = find_top_level_comma(selector);
        const auto group = detail::trim_css_ws(selector.substr(
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
    return std::string(detail::trim_css_ws(decls.substr(start, pos - start)));
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

        const auto args = detail::trim_css_ws(
            value.substr(var_pos + 4, close - (var_pos + 4)));
        const auto comma = find_top_level_comma(args);
        const auto name = std::string(detail::trim_css_ws(args.substr(
            0,
            comma == std::string_view::npos ? args.size() : comma)));

        const auto found = vars.find(name);
        if (found != vars.end()) {
            out.append(substitute_root_vars(found->second, vars, depth + 1));
        } else if (comma != std::string_view::npos) {
            out.append(substitute_root_vars(
                detail::trim_css_ws(args.substr(comma + 1)), vars, depth + 1));
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
        const auto ff_value = detail::find_decl_value(raw.decls, "font-family");
        auto ff = std::string(
            detail::trim_css_ws(substitute_root_vars(ff_value, root_vars)));
        const auto important = ff.find('!');
        if (important != std::string::npos) {
            ff = std::string(detail::trim_css_ws(
                std::string_view(ff).substr(0, important)));
        }

        const auto resize_value = detail::find_decl_value(raw.decls, "resize");
        const auto [resize, has_resize] = detail::parse_resize_keyword(
            substitute_root_vars(resize_value, root_vars));
        const auto cursor_value = std::string(detail::trim_css_ws(
            substitute_root_vars(detail::find_decl_value(raw.decls, "cursor"),
                                 root_vars)));
        const bool has_cursor = !cursor_value.empty();
        const auto cursor = has_cursor ? detail::parse_cursor_keyword(cursor_value)
                                       : detail::ComputedStyle::Cursor::Default;
        const auto bg_clip_value = std::string(detail::trim_css_ws(
            substitute_root_vars(
                detail::find_decl_value(raw.decls, "background-clip"), root_vars)));
        const bool has_background_clip = !bg_clip_value.empty();
        std::uint8_t background_clip = 0;  // border-box
        if (bg_clip_value == "padding-box") background_clip = 1;
        else if (bg_clip_value == "content-box") background_clip = 2;
        std::array<detail::GridTrackHint,
                   detail::kMaxGridTrackHints> grid_columns{};
        const auto grid_columns_value =
            detail::find_decl_value(raw.decls, "grid-template-columns");
        const std::uint8_t grid_column_count =
            detail::parse_grid_template_columns(
                substitute_root_vars(grid_columns_value, root_vars),
                grid_columns);
        if (ff.empty() && !has_resize && !has_cursor &&
            !has_background_clip && grid_column_count == 0) {
            continue;
        }

        // Each comma-separated group becomes its own RuleFill.
        std::string_view sel_text = detail::trim_css_ws(raw.selector);
        std::size_t s = 0;
        while (s <= sel_text.size()) {
            const auto comma = sel_text.find(',', s);
            const auto group = detail::trim_css_ws(sel_text.substr(s,
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
                    rf.background_clip      = background_clip;
                    rf.has_background_clip  = has_background_clip;
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
    selector = detail::trim_css_ws(selector);
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
    selector = detail::trim_css_ws(selector);
    return !selector.empty();
}

bool parse_generated_selector(std::string_view selector,
                              GeneratedContentRule& rule) {
    selector = detail::trim_css_ws(selector);
    if (!strip_generated_pseudo(selector, rule.position)) return false;

    const auto plus = find_top_level_char(selector, '+');
    if (plus != std::string_view::npos) {
        const auto previous = detail::trim_css_ws(selector.substr(0, plus));
        const auto target = detail::trim_css_ws(selector.substr(plus + 1));
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

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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
    value = std::string(detail::trim_css_ws(detail::substitute_style_vars(value, style)));
    if (value.empty() || value == "normal" || value == "none") return {};
    return decode_css_string_escapes(strip_css_quotes(std::move(value)));
}

bool generated_content_enabled(std::string value,
                               const detail::ResolvedStyle& style) {
    value = std::string(detail::trim_css_ws(detail::substitute_style_vars(value, style)));
    return !value.empty() && value != "normal" && value != "none";
}

int parse_generated_length_px(std::string value,
                              const detail::ResolvedStyle& style) {
    value = std::string(detail::trim_css_ws(detail::substitute_style_vars(value, style)));
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
}  // namespace detail
namespace {


int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
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
}  // namespace detail
namespace {

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
    const auto name = ascii_lower(detail::trim_css_ws(value.substr(0, open)));
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

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
bool parse_generated_color(std::string value,
                           const detail::ResolvedStyle& style,
                           std::uint32_t& out) {
    value = std::string(detail::trim_css_ws(detail::substitute_style_vars(value, style)));
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
    return detail::parse_hex_color(value, out) || parse_function_color(value, out);
}
}  // namespace detail
namespace {

// ═══════════════════ Inline SVG (general subset) ═══════════════════
// Renders inline <svg> children through the Painter's vector-path API.
// Supported subset (grown as the widget kits need it):
//   shapes    : path, circle, ellipse, rect, line, polyline, polygon
//   structure : g (recursive, attribute inheritance), defs
//   path data : M/m L/l H/h V/v C/c S/s Q/q T/t A/a Z/z
//   paint     : fill / stroke = none | <color> (var()-aware) |
//               url(#gradient); fill-opacity / stroke-opacity / opacity
//   stroke    : stroke-width, stroke-linecap, stroke-linejoin
//   transform : translate / scale / rotate / matrix
//   gradients : linearGradient / radialGradient, multi-stop (clamped to
//               PathPaint::kMaxStops), objectBoundingBox or
//               userSpaceOnUse units, stop-opacity
// Not supported: text, clipPath/mask/filter, pattern, use/symbol,
// stroke dashing, fill-rule:evenodd.

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

// SVG matrix(a b c d e f): x' = a·x + c·y + e ; y' = b·x + d·y + f.
struct SvgXf {
    double a{1.0}, b{0.0}, c{0.0}, d{1.0}, e{0.0}, f{0.0};

    // Compose so `inner` applies first, then this transform.
    SvgXf then(const SvgXf& inner) const {
        SvgXf m;
        m.a = a * inner.a + c * inner.b;
        m.b = b * inner.a + d * inner.b;
        m.c = a * inner.c + c * inner.d;
        m.d = b * inner.c + d * inner.d;
        m.e = a * inner.e + c * inner.f + e;
        m.f = b * inner.e + d * inner.f + f;
        return m;
    }
    void apply(double x, double y, float& ox, float& oy) const {
        ox = static_cast<float>(a * x + c * y + e);
        oy = static_cast<float>(b * x + d * y + f);
    }
    // Average length scale — used for stroke widths and radial radii.
    double uniform_scale() const {
        return (std::sqrt(a * a + b * b) + std::sqrt(c * c + d * d)) * 0.5;
    }
};

// Parse an SVG `transform` attribute (a whitespace-separated list of
// translate/scale/rotate/matrix commands, applied left-to-right).
bool parse_svg_transform(std::string_view s, SvgXf& out) {
    out = SvgXf{};
    std::size_t pos = 0;
    bool any = false;
    while (pos < s.size()) {
        while (pos < s.size() &&
               (is_css_ws(s[pos]) || s[pos] == ',')) {
            ++pos;
        }
        if (pos >= s.size()) break;
        const auto open = s.find('(', pos);
        if (open == std::string_view::npos) return any;
        const auto close = s.find(')', open + 1);
        if (close == std::string_view::npos) return any;
        const auto name = ascii_lower(detail::trim_css_ws(s.substr(pos, open - pos)));
        std::vector<double> n;
        parse_number_list(s.substr(open + 1, close - open - 1), n);
        SvgXf m;
        if (name == "translate" && !n.empty()) {
            m.e = n[0];
            m.f = n.size() >= 2 ? n[1] : 0.0;
        } else if (name == "scale" && !n.empty()) {
            m.a = n[0];
            m.d = n.size() >= 2 ? n[1] : n[0];
        } else if (name == "rotate" && !n.empty()) {
            const double rad = n[0] * 3.14159265358979323846 / 180.0;
            const double cs = std::cos(rad);
            const double sn = std::sin(rad);
            SvgXf r;
            r.a = cs; r.b = sn; r.c = -sn; r.d = cs;
            if (n.size() >= 3) {
                SvgXf to;   to.e = n[1];  to.f = n[2];
                SvgXf back; back.e = -n[1]; back.f = -n[2];
                r = to.then(r).then(back);
            }
            m = r;
        } else if (name == "matrix" && n.size() >= 6) {
            m.a = n[0]; m.b = n[1]; m.c = n[2];
            m.d = n[3]; m.e = n[4]; m.f = n[5];
        } else {
            pos = close + 1;
            continue;
        }
        out = out.then(m);
        any = true;
        pos = close + 1;
    }
    return any;
}

// Accumulates a device-space path command stream while tracking the
// user-space bounding box (for objectBoundingBox gradient mapping).
// A parsed path in LOCAL (viewBox / userSpace) coordinates — no view
// transform baked in. The command stream and bbox depend only on the
// element's shape attributes (d, cx/cy/r, points, …), so a static SVG
// (jack socket, LCD digit, chevron) parses ONCE and its result is
// cached per element; the per-paint work is just re-applying the
// current transform (transform_local_cmds), which is a matrix multiply
// per point — no string parsing. Position/scale changes reuse the cache.
struct SvgLocalPath {
    std::vector<float> cmds;   // kPath* + raw local coords
    double min_x{std::numeric_limits<double>::max()};
    double min_y{std::numeric_limits<double>::max()};
    double max_x{std::numeric_limits<double>::lowest()};
    double max_y{std::numeric_limits<double>::lowest()};
    bool has_bbox() const { return min_x <= max_x && min_y <= max_y; }
};

struct SvgPathSink {
    // The sink now records LOCAL coordinates; xf is applied later, in
    // transform_local_cmds, when emitting to the painter. Kept as a
    // member so bbox-relative gradient math (objectBoundingBox) can read
    // the transform via the render context.
    SvgLocalPath local;
    SvgXf xf;

    void grow(double x, double y) {
        local.min_x = std::min(local.min_x, x);
        local.min_y = std::min(local.min_y, y);
        local.max_x = std::max(local.max_x, x);
        local.max_y = std::max(local.max_y, y);
    }
    void emit(double x, double y) {
        local.cmds.push_back(static_cast<float>(x));
        local.cmds.push_back(static_cast<float>(y));
    }
    void move(double x, double y) {
        grow(x, y);
        local.cmds.push_back(kPathMove);
        emit(x, y);
    }
    void line(double x, double y) {
        grow(x, y);
        local.cmds.push_back(kPathLine);
        emit(x, y);
    }
    void cubic(double c1x, double c1y, double c2x, double c2y,
               double x, double y) {
        grow(c1x, c1y);
        grow(c2x, c2y);
        grow(x, y);
        local.cmds.push_back(kPathCubic);
        emit(c1x, c1y);
        emit(c2x, c2y);
        emit(x, y);
    }
    void close() { local.cmds.push_back(kPathClose); }

    bool has_bbox() const { return local.has_bbox(); }
};

// Apply a view transform to a cached local-space command stream,
// producing the device-space stream the painter consumes. Walks the
// kPath opcodes; each is followed by 1 (Move/Line), 3 (Cubic), or 0
// (Close) coordinate pairs.
void transform_local_cmds(const std::vector<float>& local, const SvgXf& xf,
                          std::vector<float>& out) {
    out.clear();
    out.reserve(local.size());
    std::size_t i = 0;
    while (i < local.size()) {
        const float op = local[i++];
        out.push_back(op);
        int pairs = 0;
        if (op == kPathMove || op == kPathLine) pairs = 1;
        else if (op == kPathCubic) pairs = 3;
        else if (op == kPathClose) pairs = 0;
        for (int p = 0; p < pairs && i + 1 < local.size(); ++p) {
            float px = 0.0f;
            float py = 0.0f;
            xf.apply(local[i], local[i + 1], px, py);
            out.push_back(px);
            out.push_back(py);
            i += 2;
        }
    }
}

// Elliptical arc (SVG A command) → cubic segments. Endpoint
// parameterisation converted per SVG spec F.6.5, sweep split into
// ≤ 90° segments each approximated with one cubic.
void svg_arc_to_cubics(SvgPathSink& sink,
                       double x1, double y1,
                       double rx, double ry, double rot_deg,
                       bool large_arc, bool sweep,
                       double x2, double y2) {
    constexpr double kPi = 3.14159265358979323846;
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx < 1e-9 || ry < 1e-9 ||
        (std::abs(x1 - x2) < 1e-12 && std::abs(y1 - y2) < 1e-12)) {
        sink.line(x2, y2);
        return;
    }
    const double phi = rot_deg * kPi / 180.0;
    const double cos_phi = std::cos(phi);
    const double sin_phi = std::sin(phi);
    const double dx2 = (x1 - x2) * 0.5;
    const double dy2 = (y1 - y2) * 0.5;
    const double x1p = cos_phi * dx2 + sin_phi * dy2;
    const double y1p = -sin_phi * dx2 + cos_phi * dy2;

    // Scale radii up if the endpoints cannot be joined by the ellipse.
    const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        const double s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
    }

    const double rxry = rx * rx * ry * ry;
    const double rxy1 = rx * rx * y1p * y1p;
    const double ryx1 = ry * ry * x1p * x1p;
    double factor = (rxry - rxy1 - ryx1) / (rxy1 + ryx1);
    if (factor < 0.0) factor = 0.0;
    double coef = std::sqrt(factor);
    if (large_arc == sweep) coef = -coef;
    const double cxp = coef * (rx * y1p / ry);
    const double cyp = coef * -(ry * x1p / rx);
    const double cx = cos_phi * cxp - sin_phi * cyp + (x1 + x2) * 0.5;
    const double cy = sin_phi * cxp + cos_phi * cyp + (y1 + y2) * 0.5;

    const auto angle_of = [&](double ux, double uy) {
        return std::atan2(uy, ux);
    };
    const double theta1 = angle_of((x1p - cxp) / rx, (y1p - cyp) / ry);
    const double theta2 = angle_of((-x1p - cxp) / rx, (-y1p - cyp) / ry);
    double dtheta = theta2 - theta1;
    if (sweep && dtheta < 0.0) {
        dtheta += 2.0 * kPi;
    } else if (!sweep && dtheta > 0.0) {
        dtheta -= 2.0 * kPi;
    }

    const int segments = std::max(
        1, static_cast<int>(std::ceil(std::abs(dtheta) / (kPi * 0.5))));
    const double delta = dtheta / segments;
    // Cubic control distance for a `delta` arc span.
    const double t = 4.0 / 3.0 * std::tan(delta * 0.25);

    double theta = theta1;
    for (int i = 0; i < segments; ++i) {
        const double next = theta + delta;
        const double cos_t = std::cos(theta);
        const double sin_t = std::sin(theta);
        const double cos_n = std::cos(next);
        const double sin_n = std::sin(next);

        const auto point = [&](double ct, double st, double& px, double& py) {
            const double ex = rx * ct;
            const double ey = ry * st;
            px = cos_phi * ex - sin_phi * ey + cx;
            py = sin_phi * ex + cos_phi * ey + cy;
        };
        const auto deriv = [&](double ct, double st, double& dx, double& dy) {
            const double ex = -rx * st;
            const double ey = ry * ct;
            dx = cos_phi * ex - sin_phi * ey;
            dy = sin_phi * ex + cos_phi * ey;
        };
        double p0x = 0.0, p0y = 0.0, p1x = 0.0, p1y = 0.0;
        double d0x = 0.0, d0y = 0.0, d1x = 0.0, d1y = 0.0;
        point(cos_t, sin_t, p0x, p0y);
        point(cos_n, sin_n, p1x, p1y);
        deriv(cos_t, sin_t, d0x, d0y);
        deriv(cos_n, sin_n, d1x, d1y);
        sink.cubic(p0x + t * d0x, p0y + t * d0y,
                   p1x - t * d1x, p1y - t * d1y,
                   p1x, p1y);
        theta = next;
    }
}

// Full SVG path-data parser. Returns false only when nothing at all
// could be consumed; a trailing malformed segment keeps what parsed.
bool build_svg_path_data(std::string_view d, SvgPathSink& sink) {
    std::size_t pos = 0;
    const auto skip_seps = [&] {
        while (pos < d.size() && (is_css_ws(d[pos]) || d[pos] == ',')) ++pos;
    };
    const auto read_number = [&](double& out) {
        skip_seps();
        if (pos >= d.size()) return false;
        std::string tail(d.substr(pos));
        char* end = nullptr;
        const double v = std::strtod(tail.c_str(), &end);
        if (end == tail.c_str()) return false;
        out = v;
        pos += static_cast<std::size_t>(end - tail.c_str());
        return true;
    };
    const auto read_flag = [&](bool& out) {
        skip_seps();
        if (pos >= d.size()) return false;
        if (d[pos] == '0') { out = false; ++pos; return true; }
        if (d[pos] == '1') { out = true;  ++pos; return true; }
        return false;
    };
    const auto next_is_number = [&] {
        skip_seps();
        if (pos >= d.size()) return false;
        const char ch = d[pos];
        return (ch >= '0' && ch <= '9') || ch == '-' || ch == '+' ||
               ch == '.';
    };

    double cx = 0.0, cy = 0.0;   // current point
    double sx = 0.0, sy = 0.0;   // subpath start
    double pcx = 0.0, pcy = 0.0; // previous cubic control (S reflection)
    double pqx = 0.0, pqy = 0.0; // previous quad control (T reflection)
    char cmd = 0;
    char prev = 0;
    bool any = false;

    while (true) {
        skip_seps();
        if (pos >= d.size()) break;
        const char ch = d[pos];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            cmd = ch;
            ++pos;
        } else if (cmd != 0) {
            // Implicit repeat; an implicit M repeat becomes L.
            if (cmd == 'M') cmd = 'L';
            else if (cmd == 'm') cmd = 'l';
        } else {
            break;
        }

        const bool rel = (cmd >= 'a' && cmd <= 'z');
        const char op = rel ? static_cast<char>(cmd - 'a' + 'A') : cmd;
        double n[7] = {};
        switch (op) {
            case 'M': {
                if (!read_number(n[0]) || !read_number(n[1])) return any;
                cx = rel ? cx + n[0] : n[0];
                cy = rel ? cy + n[1] : n[1];
                sx = cx;
                sy = cy;
                sink.move(cx, cy);
                any = true;
                break;
            }
            case 'L': {
                if (!read_number(n[0]) || !read_number(n[1])) return any;
                cx = rel ? cx + n[0] : n[0];
                cy = rel ? cy + n[1] : n[1];
                sink.line(cx, cy);
                any = true;
                break;
            }
            case 'H': {
                if (!read_number(n[0])) return any;
                cx = rel ? cx + n[0] : n[0];
                sink.line(cx, cy);
                any = true;
                break;
            }
            case 'V': {
                if (!read_number(n[0])) return any;
                cy = rel ? cy + n[0] : n[0];
                sink.line(cx, cy);
                any = true;
                break;
            }
            case 'C': {
                for (int i = 0; i < 6; ++i) {
                    if (!read_number(n[i])) return any;
                }
                const double c1x = rel ? cx + n[0] : n[0];
                const double c1y = rel ? cy + n[1] : n[1];
                const double c2x = rel ? cx + n[2] : n[2];
                const double c2y = rel ? cy + n[3] : n[3];
                cx = rel ? cx + n[4] : n[4];
                cy = rel ? cy + n[5] : n[5];
                sink.cubic(c1x, c1y, c2x, c2y, cx, cy);
                pcx = c2x;
                pcy = c2y;
                any = true;
                break;
            }
            case 'S': {
                for (int i = 0; i < 4; ++i) {
                    if (!read_number(n[i])) return any;
                }
                double c1x = cx;
                double c1y = cy;
                if (prev == 'C' || prev == 'c' || prev == 'S' ||
                    prev == 's') {
                    c1x = 2.0 * cx - pcx;
                    c1y = 2.0 * cy - pcy;
                }
                const double c2x = rel ? cx + n[0] : n[0];
                const double c2y = rel ? cy + n[1] : n[1];
                cx = rel ? cx + n[2] : n[2];
                cy = rel ? cy + n[3] : n[3];
                sink.cubic(c1x, c1y, c2x, c2y, cx, cy);
                pcx = c2x;
                pcy = c2y;
                any = true;
                break;
            }
            case 'Q': {
                for (int i = 0; i < 4; ++i) {
                    if (!read_number(n[i])) return any;
                }
                const double qx = rel ? cx + n[0] : n[0];
                const double qy = rel ? cy + n[1] : n[1];
                const double ex = rel ? cx + n[2] : n[2];
                const double ey = rel ? cy + n[3] : n[3];
                // Elevate quadratic to cubic.
                sink.cubic(cx + 2.0 / 3.0 * (qx - cx),
                           cy + 2.0 / 3.0 * (qy - cy),
                           ex + 2.0 / 3.0 * (qx - ex),
                           ey + 2.0 / 3.0 * (qy - ey),
                           ex, ey);
                cx = ex;
                cy = ey;
                pqx = qx;
                pqy = qy;
                any = true;
                break;
            }
            case 'T': {
                if (!read_number(n[0]) || !read_number(n[1])) return any;
                double qx = cx;
                double qy = cy;
                if (prev == 'Q' || prev == 'q' || prev == 'T' ||
                    prev == 't') {
                    qx = 2.0 * cx - pqx;
                    qy = 2.0 * cy - pqy;
                }
                const double ex = rel ? cx + n[0] : n[0];
                const double ey = rel ? cy + n[1] : n[1];
                sink.cubic(cx + 2.0 / 3.0 * (qx - cx),
                           cy + 2.0 / 3.0 * (qy - cy),
                           ex + 2.0 / 3.0 * (qx - ex),
                           ey + 2.0 / 3.0 * (qy - ey),
                           ex, ey);
                cx = ex;
                cy = ey;
                pqx = qx;
                pqy = qy;
                any = true;
                break;
            }
            case 'A': {
                bool large = false;
                bool sweep_flag = false;
                if (!read_number(n[0]) || !read_number(n[1]) ||
                    !read_number(n[2]) || !read_flag(large) ||
                    !read_flag(sweep_flag) || !read_number(n[5]) ||
                    !read_number(n[6])) {
                    return any;
                }
                const double ex = rel ? cx + n[5] : n[5];
                const double ey = rel ? cy + n[6] : n[6];
                svg_arc_to_cubics(sink, cx, cy, n[0], n[1], n[2],
                                  large, sweep_flag, ex, ey);
                cx = ex;
                cy = ey;
                any = true;
                break;
            }
            case 'Z': {
                sink.close();
                cx = sx;
                cy = sy;
                any = true;
                break;
            }
            default:
                return any;
        }
        prev = cmd;
        if (op != 'Z' && !next_is_number()) {
            // Next token is a command letter (or end); loop handles it.
        }
    }
    return any;
}

// ── Gradient definitions ────────────────────────────────────────────
struct SvgGradientStopDef {
    double      offset{0.0};
    std::string color;
    double      opacity{1.0};
};

struct SvgGradientDef {
    bool radial{false};
    bool user_space{false};                  // gradientUnits="userSpaceOnUse"
    double x1{0.0}, y1{0.0}, x2{1.0}, y2{0.0};  // linear axis
    double cx{0.5}, cy{0.5}, r{0.5};            // radial geometry
    std::vector<SvgGradientStopDef> stops;
};

double parse_svg_coord(lxb_dom_element_t* elem, std::string_view name,
                       double fallback) {
    const std::string v = detail::attr_string(elem, name);
    if (v.empty()) return fallback;
    char* end = nullptr;
    const double n = std::strtod(v.c_str(), &end);
    if (end == v.c_str()) return fallback;
    if (end != nullptr && *end == '%') return n / 100.0;
    return n;
}

void collect_svg_gradients(
    lxb_dom_element_t* parent,
    std::unordered_map<std::string, SvgGradientDef>& out) {
    for (auto* node = lxb_dom_node_first_child(
             lxb_dom_interface_node(parent));
         node != nullptr; node = lxb_dom_node_next(node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* elem = lxb_dom_interface_element(node);
        const auto tag = ascii_lower(detail::tag_name(elem));
        if (tag == "lineargradient" || tag == "radialgradient") {
            const std::string id = detail::attr_string(elem, "id");
            if (id.empty()) continue;
            SvgGradientDef def;
            def.radial = (tag == "radialgradient");
            def.user_space =
                detail::attr_string(elem, "gradientUnits") == "userSpaceOnUse";
            if (def.radial) {
                def.cx = parse_svg_coord(elem, "cx", 0.5);
                def.cy = parse_svg_coord(elem, "cy", 0.5);
                def.r  = parse_svg_coord(elem, "r", 0.5);
            } else {
                def.x1 = parse_svg_coord(elem, "x1", 0.0);
                def.y1 = parse_svg_coord(elem, "y1", 0.0);
                def.x2 = parse_svg_coord(elem, "x2", 1.0);
                def.y2 = parse_svg_coord(elem, "y2", 0.0);
            }
            for (auto* stop_node = lxb_dom_node_first_child(node);
                 stop_node != nullptr;
                 stop_node = lxb_dom_node_next(stop_node)) {
                if (stop_node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                auto* stop_elem = lxb_dom_interface_element(stop_node);
                if (ascii_lower(detail::tag_name(stop_elem)) != "stop") continue;
                SvgGradientStopDef stop;
                stop.offset =
                    std::clamp(parse_svg_coord(stop_elem, "offset", 0.0),
                               0.0, 1.0);
                stop.color = detail::attr_string(stop_elem, "stop-color");
                if (stop.color.empty()) stop.color = "black";
                stop.opacity =
                    std::clamp(parse_svg_coord(stop_elem, "stop-opacity",
                                               1.0),
                               0.0, 1.0);
                def.stops.push_back(std::move(stop));
            }
            if (!def.stops.empty()) {
                out.emplace(id, std::move(def));
            }
            continue;
        }
        // Recurse into <defs> and any other container.
        collect_svg_gradients(elem, out);
    }
}

// ── Paint resolution ────────────────────────────────────────────────
struct SvgRenderCtx {
    Painter& painter;
    const detail::ResolvedStyle& style;
    std::unordered_map<std::string, SvgGradientDef> gradients;
    // Per-element local-geometry parse cache (nullptr = parse every
    // paint, e.g. the stub build or tests without a Document). Owned by
    // DocumentImpl; keyed on element pointer + geometry-attr hash.
    std::unordered_map<const lxb_dom_element_t*,
                       detail::DocumentImpl::SvgPathCacheEntry>* path_cache{
        nullptr};
    // Reusable scratch for the transformed device-space stream, so the
    // per-paint transform apply doesn't allocate per shape.
    std::vector<float> xf_scratch;
};

// Presentation attributes inherited down the SVG element tree.
struct SvgInherited {
    std::string fill{"black"};   // SVG spec default paint
    std::string stroke{"none"};
    double opacity{1.0};
    double fill_opacity{1.0};
    double stroke_opacity{1.0};
    double stroke_width{1.0};
    LineCap  cap{LineCap::Butt};
    LineJoin join{LineJoin::Miter};
    SvgXf xf;
};

Color svg_color_with_alpha(std::uint32_t rgba, double alpha_mult) {
    Color c = detail::unpack_rgba(rgba);
    c.a = static_cast<std::uint8_t>(
        std::clamp(std::lround(static_cast<double>(c.a) * alpha_mult),
                   0L, 255L));
    return c;
}

// Resolve fill/stroke paint value → device-space PathPaint. `bbox_*`
// is the element's user-space bounding box (for objectBoundingBox
// gradient mapping); `xf` maps user space to device space.
bool resolve_svg_paint(const std::string& raw_value,
                       double alpha_mult,
                       const SvgPathSink& sink,
                       const SvgRenderCtx& ctx,
                       PathPaint& out) {
    const std::string value(detail::trim_css_ws(raw_value));
    if (value.empty()) return false;
    const auto lower = ascii_lower(value);
    if (lower == "none" || lower == "transparent") return false;

    if (lower.rfind("url(", 0) == 0) {
        const auto close = value.find(')');
        if (close == std::string::npos) return false;
        std::string ref(detail::trim_css_ws(
            std::string_view(value).substr(4, close - 4)));
        if (!ref.empty() && (ref.front() == '\'' || ref.front() == '"')) {
            ref = ref.substr(1, ref.size() >= 2 ? ref.size() - 2 : 0);
        }
        if (ref.empty() || ref.front() != '#') return false;
        const auto it = ctx.gradients.find(ref.substr(1));
        if (it == ctx.gradients.end() || !sink.has_bbox()) return false;
        const SvgGradientDef& def = it->second;

        out = PathPaint{};
        out.kind = def.radial ? PathPaint::Kind::Radial
                              : PathPaint::Kind::Linear;
        out.stop_count = 0;
        for (const auto& stop : def.stops) {
            if (out.stop_count >= PathPaint::kMaxStops) break;
            std::uint32_t rgba = 0;
            if (!detail::parse_generated_color(stop.color, ctx.style, rgba)) {
                continue;
            }
            out.offsets[out.stop_count] = static_cast<float>(stop.offset);
            out.colors[out.stop_count] =
                svg_color_with_alpha(rgba, stop.opacity * alpha_mult);
            ++out.stop_count;
        }
        if (out.stop_count == 0) return false;
        if (out.stop_count == 1) {
            // Degenerate gradient → solid.
            const Color only = out.colors[0];
            out = PathPaint::solid(only);
            return true;
        }

        const double bw = sink.local.max_x - sink.local.min_x;
        const double bh = sink.local.max_y - sink.local.min_y;
        if (def.radial) {
            double ucx = def.cx;
            double ucy = def.cy;
            double ur = def.r;
            if (!def.user_space) {
                ucx = sink.local.min_x + def.cx * bw;
                ucy = sink.local.min_y + def.cy * bh;
                // Spec: percentage radius resolves against the
                // normalised diagonal of the bounding box.
                ur = def.r * std::sqrt((bw * bw + bh * bh) * 0.5);
            }
            float dcx = 0.0f;
            float dcy = 0.0f;
            sink.xf.apply(ucx, ucy, dcx, dcy);
            out.x0 = dcx;
            out.y0 = dcy;
            out.r0 = 0.0f;
            out.r1 = static_cast<float>(
                std::max(0.1, ur * sink.xf.uniform_scale()));
        } else {
            double ux1 = def.x1;
            double uy1 = def.y1;
            double ux2 = def.x2;
            double uy2 = def.y2;
            if (!def.user_space) {
                ux1 = sink.local.min_x + def.x1 * bw;
                uy1 = sink.local.min_y + def.y1 * bh;
                ux2 = sink.local.min_x + def.x2 * bw;
                uy2 = sink.local.min_y + def.y2 * bh;
            }
            sink.xf.apply(ux1, uy1, out.x0, out.y0);
            sink.xf.apply(ux2, uy2, out.x1, out.y1);
        }
        return true;
    }

    std::uint32_t rgba = 0;
    if (!detail::parse_generated_color(value, ctx.style, rgba)) return false;
    const Color c = svg_color_with_alpha(rgba, alpha_mult);
    if (c.a == 0) return false;
    out = PathPaint::solid(c);
    return true;
}

double parse_svg_number_attr(lxb_dom_element_t* elem, std::string_view name,
                             double fallback) {
    return parse_svg_coord(elem, name, fallback);
}

// ── Shape geometry emitters ─────────────────────────────────────────
void emit_svg_ellipse(SvgPathSink& sink, double cx, double cy,
                      double rx, double ry) {
    // Cubic circle constant.
    constexpr double k = 0.5522847498307936;
    const double ox = rx * k;
    const double oy = ry * k;
    sink.move(cx + rx, cy);
    sink.cubic(cx + rx, cy + oy, cx + ox, cy + ry, cx, cy + ry);
    sink.cubic(cx - ox, cy + ry, cx - rx, cy + oy, cx - rx, cy);
    sink.cubic(cx - rx, cy - oy, cx - ox, cy - ry, cx, cy - ry);
    sink.cubic(cx + ox, cy - ry, cx + rx, cy - oy, cx + rx, cy);
    sink.close();
}

void emit_svg_rect(SvgPathSink& sink, double x, double y,
                   double w, double h, double rx, double ry) {
    if (w <= 0.0 || h <= 0.0) return;
    rx = std::clamp(rx, 0.0, w * 0.5);
    ry = std::clamp(ry, 0.0, h * 0.5);
    if (rx <= 0.0 || ry <= 0.0) {
        sink.move(x, y);
        sink.line(x + w, y);
        sink.line(x + w, y + h);
        sink.line(x, y + h);
        sink.close();
        return;
    }
    constexpr double k = 0.5522847498307936;
    const double ox = rx * (1.0 - k);
    const double oy = ry * (1.0 - k);
    sink.move(x + rx, y);
    sink.line(x + w - rx, y);
    sink.cubic(x + w - ox, y, x + w, y + oy, x + w, y + ry);
    sink.line(x + w, y + h - ry);
    sink.cubic(x + w, y + h - oy, x + w - ox, y + h, x + w - rx, y + h);
    sink.line(x + rx, y + h);
    sink.cubic(x + ox, y + h, x, y + h - oy, x, y + h - ry);
    sink.line(x, y + ry);
    sink.cubic(x, y + oy, x + ox, y, x + rx, y);
    sink.close();
}

bool emit_svg_poly(SvgPathSink& sink, lxb_dom_element_t* elem, bool close) {
    std::vector<double> pts;
    if (!parse_number_list(detail::attr_string(elem, "points"), pts) ||
        pts.size() < 4) {
        return false;
    }
    sink.move(pts[0], pts[1]);
    for (std::size_t i = 2; i + 1 < pts.size(); i += 2) {
        sink.line(pts[i], pts[i + 1]);
    }
    if (close) sink.close();
    return true;
}

// ── Element rendering ───────────────────────────────────────────────
void render_svg_element_tree(lxb_dom_element_t* parent,
                             SvgRenderCtx& ctx,
                             const SvgInherited& inherited) {
    for (auto* node = lxb_dom_node_first_child(
             lxb_dom_interface_node(parent));
         node != nullptr; node = lxb_dom_node_next(node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* elem = lxb_dom_interface_element(node);
        const auto tag = ascii_lower(detail::tag_name(elem));
        if (tag == "defs" || tag == "lineargradient" ||
            tag == "radialgradient" || tag == "clippath" ||
            tag == "mask" || tag == "symbol" || tag == "title" ||
            tag == "desc") {
            continue;
        }

        SvgInherited state = inherited;
        if (const std::string v = detail::attr_string(elem, "fill"); !v.empty()) {
            state.fill = v;
        }
        if (const std::string v = detail::attr_string(elem, "stroke"); !v.empty()) {
            state.stroke = v;
        }
        state.opacity *= std::clamp(
            parse_svg_number_attr(elem, "opacity", 1.0), 0.0, 1.0);
        state.fill_opacity = std::clamp(
            parse_svg_number_attr(elem, "fill-opacity",
                                  state.fill_opacity),
            0.0, 1.0);
        state.stroke_opacity = std::clamp(
            parse_svg_number_attr(elem, "stroke-opacity",
                                  state.stroke_opacity),
            0.0, 1.0);
        state.stroke_width = parse_svg_number_attr(elem, "stroke-width",
                                                   state.stroke_width);
        if (const std::string v = detail::attr_string(elem, "stroke-linecap");
            !v.empty()) {
            const auto lc = ascii_lower(v);
            state.cap = lc == "round"    ? LineCap::Round
                        : lc == "square" ? LineCap::Square
                                         : LineCap::Butt;
        }
        if (const std::string v = detail::attr_string(elem, "stroke-linejoin");
            !v.empty()) {
            const auto lj = ascii_lower(v);
            state.join = lj == "round"   ? LineJoin::Round
                         : lj == "bevel" ? LineJoin::Bevel
                                         : LineJoin::Miter;
        }
        if (const std::string v = detail::attr_string(elem, "transform");
            !v.empty()) {
            SvgXf local;
            if (parse_svg_transform(v, local)) {
                state.xf = state.xf.then(local);
            }
        }

        if (tag == "g" || tag == "a" || tag == "svg") {
            render_svg_element_tree(elem, ctx, state);
            continue;
        }

        SvgPathSink sink;
        sink.xf = state.xf;
        bool strokable = true;
        bool fillable = (tag != "line" && tag != "polyline");

        // ── Parse the LOCAL geometry (cached per element) ──────────────
        // Hash the shape-defining attributes; a cache hit skips string
        // parsing entirely and reuses the local command stream. Only the
        // view transform is re-applied below, so a moved/scaled static
        // SVG still hits the cache. `line` is cheap (2 attrs) and rarely
        // repeats, so it's parsed inline without a cache lookup.
        // Leak backstop: a single long-lived document that churns many
        // distinct SVG elements would otherwise grow the pointer-keyed
        // map without bound (removed elements never look up again). A
        // real face has hundreds of shapes, not tens of thousands — well
        // past that, drop the whole cache and let it refill. Correctness
        // is unaffected (the cache is a pure memo).
        if (ctx.path_cache != nullptr && ctx.path_cache->size() > 20000) {
            ctx.path_cache->clear();
        }
        auto* cache_slot =
            (ctx.path_cache != nullptr && elem != nullptr && tag != "line")
                ? &(*ctx.path_cache)[elem]
                : nullptr;
        bool have_local = false;
        if (cache_slot != nullptr) {
            std::size_t h = std::hash<std::string_view>{}(tag);
            const auto mix = [&](std::string_view v) {
                h = h * 1099511628211ULL ^
                    std::hash<std::string_view>{}(v);
            };
            for (const char* a :
                 {"d", "cx", "cy", "r", "rx", "ry", "x", "y", "width",
                  "height", "x1", "y1", "x2", "y2", "points"}) {
                mix(detail::attr_view(elem, a));
            }
            if (cache_slot->attr_hash == h &&
                !cache_slot->local_cmds.empty()) {
                sink.local.cmds = cache_slot->local_cmds;
                sink.local.min_x = cache_slot->min_x;
                sink.local.min_y = cache_slot->min_y;
                sink.local.max_x = cache_slot->max_x;
                sink.local.max_y = cache_slot->max_y;
                have_local = cache_slot->has_bbox || !sink.local.cmds.empty();
            } else {
                cache_slot->attr_hash = h;  // (re)fill below
            }
        }

        if (!have_local) {
            if (tag == "path") {
                if (!build_svg_path_data(detail::attr_string(elem, "d"), sink)) {
                    if (cache_slot) cache_slot->local_cmds.clear();
                    continue;
                }
            } else if (tag == "circle") {
                const double r = parse_svg_number_attr(elem, "r", 0.0);
                if (r <= 0.0) continue;
                emit_svg_ellipse(sink,
                                 parse_svg_number_attr(elem, "cx", 0.0),
                                 parse_svg_number_attr(elem, "cy", 0.0),
                                 r, r);
            } else if (tag == "ellipse") {
                const double rx = parse_svg_number_attr(elem, "rx", 0.0);
                const double ry = parse_svg_number_attr(elem, "ry", 0.0);
                if (rx <= 0.0 || ry <= 0.0) continue;
                emit_svg_ellipse(sink,
                                 parse_svg_number_attr(elem, "cx", 0.0),
                                 parse_svg_number_attr(elem, "cy", 0.0),
                                 rx, ry);
            } else if (tag == "rect") {
                double rx = parse_svg_number_attr(elem, "rx", -1.0);
                double ry = parse_svg_number_attr(elem, "ry", -1.0);
                if (rx < 0.0 && ry >= 0.0) rx = ry;
                if (ry < 0.0 && rx >= 0.0) ry = rx;
                emit_svg_rect(sink,
                              parse_svg_number_attr(elem, "x", 0.0),
                              parse_svg_number_attr(elem, "y", 0.0),
                              parse_svg_number_attr(elem, "width", 0.0),
                              parse_svg_number_attr(elem, "height", 0.0),
                              std::max(0.0, rx), std::max(0.0, ry));
                if (sink.local.cmds.empty()) continue;
            } else if (tag == "line") {
                sink.move(parse_svg_number_attr(elem, "x1", 0.0),
                          parse_svg_number_attr(elem, "y1", 0.0));
                sink.line(parse_svg_number_attr(elem, "x2", 0.0),
                          parse_svg_number_attr(elem, "y2", 0.0));
            } else if (tag == "polyline") {
                if (!emit_svg_poly(sink, elem, /*close=*/false)) continue;
            } else if (tag == "polygon") {
                if (!emit_svg_poly(sink, elem, /*close=*/true)) continue;
            } else {
                continue;
            }
            if (sink.local.cmds.empty()) continue;
            if (cache_slot != nullptr) {
                cache_slot->local_cmds = sink.local.cmds;
                cache_slot->min_x = sink.local.min_x;
                cache_slot->min_y = sink.local.min_y;
                cache_slot->max_x = sink.local.max_x;
                cache_slot->max_y = sink.local.max_y;
                cache_slot->has_bbox = sink.local.has_bbox();
            }
        }
        if (sink.local.cmds.empty()) continue;

        // ── Apply the view transform → device-space stream (no parse) ──
        transform_local_cmds(sink.local.cmds, state.xf, ctx.xf_scratch);
        const float* dev = ctx.xf_scratch.data();
        const std::size_t dev_n = ctx.xf_scratch.size();

        PathPaint paint{};
        if (fillable &&
            resolve_svg_paint(state.fill,
                              state.opacity * state.fill_opacity,
                              sink, ctx, paint)) {
            ctx.painter.fill_path(dev, dev_n, paint);
        }
        if (strokable && state.stroke_width > 0.0 &&
            resolve_svg_paint(state.stroke,
                              state.opacity * state.stroke_opacity,
                              sink, ctx, paint)) {
            const float device_w = static_cast<float>(
                state.stroke_width * state.xf.uniform_scale());
            ctx.painter.stroke_path(dev, dev_n,
                                    paint, std::max(0.1f, device_w),
                                    state.cap, state.join);
        }
    }
}

#if !defined(AFFINEUI_STUB_BUILD)
void paint_inline_svg(detail::DocumentImpl& impl,
                      const Block& b,
                      const Rect& eff,
                      const detail::ComputedStyle& cs,
                      const detail::AnimatedStyle& an,
                      Painter& painter,
                      lxb_dom_element_t* svg_elem) {
    double vb_x = 0.0;
    double vb_y = 0.0;
    double vb_w = static_cast<double>(std::max(1, eff.w));
    double vb_h = static_cast<double>(std::max(1, eff.h));
    (void) parse_svg_viewbox(detail::attr_string(svg_elem, "viewBox"),
                             vb_x, vb_y, vb_w, vb_h);
    const double sx = static_cast<double>(eff.w) / vb_w;
    const double sy = static_cast<double>(eff.h) / vb_h;

    detail::ResolvedStyle svg_style{};
    svg_style.computed = cs;
    svg_style.animated = an;
    svg_style.custom_props = b.custom_props;

    SvgRenderCtx ctx{painter, svg_style, {}};
    ctx.path_cache = &impl.svg_path_cache;
    collect_svg_gradients(svg_elem, ctx.gradients);

    SvgInherited base;
    base.xf.a = sx;
    base.xf.d = sy;
    base.xf.e = static_cast<double>(eff.x) - vb_x * sx;
    base.xf.f = static_cast<double>(eff.y) - vb_y * sy;
    render_svg_element_tree(svg_elem, ctx, base);
}

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void paint_direct_child_svgs(detail::DocumentImpl& impl,
                             const Block& b,
                             const Rect& eff,
                             const detail::ComputedStyle& cs,
                             const detail::AnimatedStyle& an,
                             Painter& painter,
                             lxb_dom_element_t* parent_elem) {
    for (auto* node = lxb_dom_node_first_child(lxb_dom_interface_node(parent_elem));
         node != nullptr; node = lxb_dom_node_next(node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        auto* elem = lxb_dom_interface_element(node);
        if (detail::tag_name(elem) == "svg") {
            paint_inline_svg(impl, b, eff, cs, an, painter, elem);
        }
    }
}
}  // namespace detail
namespace {

#endif

void scan_generated_content_rules(lxb_css_parser_t* parser,
                                  lxb_css_memory_t* memory,
                                  std::string_view css,
                                  std::vector<GeneratedContentRule>& out) {
    const auto raw_rules = split_css_rules(css);
    for (const auto& raw : raw_rules) {
        const auto content_value = detail::find_decl_value(raw.decls, "content");
        const auto display_value = detail::find_decl_value(raw.decls, "display");
        const auto color_value = detail::find_decl_value(raw.decls, "color");
        const auto background_value =
            detail::find_decl_value(raw.decls, "background");
        const auto background_color_value =
            detail::find_decl_value(raw.decls, "background-color");
        const auto padding_left_value =
            detail::find_decl_value(raw.decls, "padding-left");
        const auto padding_right_value =
            detail::find_decl_value(raw.decls, "padding-right");

        std::string_view sel_text = detail::trim_css_ws(raw.selector);
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
            const auto group = detail::trim_css_ws(sel_text.substr(
                0,
                comma == std::string_view::npos ? sel_text.size() : comma));
            if (!group.empty()) {
                GeneratedContentRule rule;
                if (parse_generated_selector(group, rule)) {
                    rule.specificity = generated_selector_specificity(rule);
                    rule.source_order =
                        static_cast<std::uint32_t>(out.size());
                    rule.content_value = content_value;
                    rule.display_value = display_value;
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
            sel_text = detail::trim_css_ws(sel_text.substr(comma + 1));
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
    token = detail::trim_css_ws(token);
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
    token = detail::trim_css_ws(token);
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
    selector = detail::trim_css_ws(selector);
    while (!selector.empty()) {
        const auto comma = find_top_level_comma(selector);
        const auto piece = detail::trim_css_ws(selector.substr(
            0, comma == std::string_view::npos ? selector.size() : comma));
        float offset = 0.0f;
        if (parse_keyframe_offset(piece, offset)) offsets.push_back(offset);
        if (comma == std::string_view::npos) break;
        selector = detail::trim_css_ws(selector.substr(comma + 1));
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

        auto name = strip_css_quotes(std::string(detail::trim_css_ws(std::string_view(
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

}  // namespace

// Cross-file document helpers — declared in internal/document_impl.h.
namespace detail {
void attach_media_block(detail::DocumentImpl& impl, const MediaBlock& mb) {
    auto* sst_media = lxb_css_stylesheet_parse(
        impl.doc->css.parser,
        reinterpret_cast<const lxb_char_t*>(mb.block_css.data()),
        mb.block_css.size());
    if (!sst_media) return;

    if (lxb_html_document_stylesheet_attach(impl.doc, sst_media)
            == LXB_STATUS_OK) {
        impl.sheets.push_back(sst_media);
        impl.attr_subtree_local_cache.clear();
        impl.attr_subject_confined_cache.clear();
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
                       std::string_view base_url) {
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
        impl.attr_subtree_local_cache.clear();
        impl.attr_subject_confined_cache.clear();
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
                    detail::attach_media_block(impl, impl.media_blocks[i]);
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
    return detail::find_decl_value(s, key);
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
}  // namespace detail
}  // namespace affineui
