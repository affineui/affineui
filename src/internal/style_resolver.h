#pragma once

#include "internal/animated_style.h"
#include "internal/computed_style.h"

#include <memory>
#include <string>
#include <unordered_map>

// Forward declarations so callers don't drag in lexbor headers.
struct lxb_html_document;
typedef struct lxb_html_document lxb_html_document_t;
struct lxb_dom_element;
typedef struct lxb_dom_element lxb_dom_element_t;
struct lxb_css_rule_declaration_list;
typedef struct lxb_css_rule_declaration_list lxb_css_rule_declaration_list_t;

namespace affineui::detail {

/// CSS custom properties (`--name: value`) in effect for an element,
/// keyed by name (including the leading `--`) with the raw, unresolved
/// value string. They inherit, so the cascade carries them down the
/// tree; `var()` references are substituted against this map at
/// resolve time. Stored behind a shared_ptr so inheritance is a cheap
/// pointer copy — only an element that *declares* custom properties
/// clones the map (copy-on-write).
using CustomPropMap = std::unordered_map<std::string, std::string>;

/// The two-struct bundle the cascade resolves into. Splitting them
/// pays off downstream: layout reads ComputedStyle only, paint reads
/// AnimatedStyle only, composite reads only the transform/opacity
/// fields out of AnimatedStyle. See docs/DESIGN.md §
/// "Real-time render architecture."
struct ResolvedStyle {
    ComputedStyle computed{};
    AnimatedStyle animated{};
    /// Inherited custom-property scope (null = none in effect).
    std::shared_ptr<const CustomPropMap> custom_props;
};

/// Abstract style resolver. Phase 2 ships one impl (lexbor-backed)
/// that delegates to lexbor's cascade via `lxb_html_element_style_walk`.
/// Future phases can swap in a custom matcher with bloom filters,
/// invalidation sets, computed-style sharing — the rest of the
/// engine sees only this interface.
class StyleResolver {
public:
    virtual ~StyleResolver() = default;

    /// Resolve the full style for `element`, merging in inherited
    /// properties from `parent`. Phase 2 is uncached; Phase 3 adds
    /// the dirty-bit caching described in DESIGN.md.
    virtual ResolvedStyle resolve(lxb_dom_element_t* element,
                                  const ResolvedStyle& parent) = 0;

    /// Apply every declaration in `list` to `out` in source order.
    /// Used by the :hover overlay pass to layer state-dependent rules
    /// on top of an already-resolved base style without re-running the
    /// full cascade. Caller is responsible for whether the rule's
    /// selector should be applied at all.
    virtual void apply_decl_list(
        const lxb_css_rule_declaration_list_t* list,
        ResolvedStyle& out) = 0;

    /// Mark an element's style cache entry dirty so the next
    /// `resolve()` call recomputes. Phase 2 doesn't yet propagate
    /// to descendants.
    virtual void invalidate(lxb_dom_element_t* element) = 0;

    /// Drop all caches. Called by Document::set_html when the DOM
    /// is wholesale replaced.
    virtual void clear() = 0;
};

/// Construct the default resolver, backed by lexbor's cascade.
std::unique_ptr<StyleResolver> make_lexbor_resolver(lxb_html_document_t* doc);

}  // namespace affineui::detail
