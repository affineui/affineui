#pragma once

// `StyleEngine` — runs the cascade pass for a Document.
//
// Inputs:
//   • DOM (lexbor)
//   • parsed stylesheets (lexbor CSS AST + our index built on top)
//   • RestyleQueue (set of dirty nodes)
//
// Output:
//   • per-element ComputedStyle, attached to the DOM node via user-data
//
// The interface is deliberately one method (`resolve_dirty`). All the
// optimization machinery (rule indexing § 2.1, bloom prefilter § 2.2,
// invalidation sets § 2.3, computed-style sharing § 2.4, custom-prop
// deps § 2.7) lives inside the impl. The rest of the engine doesn't
// know which optimizations are active in this build.

#include "affineui/computed_style.h"
#include "renderer/engine/restyle_queue.h"

#include <memory>
#include <string_view>

namespace affineui {

namespace detail {
struct DomHandle;  // opaque pointer to lexbor's document_t
}

struct StyleStats {
    std::uint32_t nodes_visited{0};
    std::uint32_t nodes_restyled{0};
    std::uint32_t nodes_shared_with_sibling{0};
    std::uint32_t rules_considered{0};
    std::uint32_t rules_matched{0};
    double        cpu_time_ms{0.0};
};

class StyleEngine {
public:
    StyleEngine();
    ~StyleEngine();

    StyleEngine(const StyleEngine&)            = delete;
    StyleEngine& operator=(const StyleEngine&) = delete;
    StyleEngine(StyleEngine&&) noexcept;
    StyleEngine& operator=(StyleEngine&&) noexcept;

    /// Bind to a DOM. Called once per Document. After this, stylesheet
    /// changes flow through `add_stylesheet` / `clear_stylesheets`.
    void attach_dom(detail::DomHandle& dom);

    /// Parse a stylesheet string and add it to the cascade. `origin`
    /// affects cascade priority (user-agent < author < user).
    enum class Origin : std::uint8_t { UserAgent, Author, User };
    void add_stylesheet(std::string_view css, Origin origin);

    void clear_stylesheets();

    /// Run the cascade pass for nodes the queue says are dirty.
    /// Returns the per-pass stats.
    StyleStats resolve_dirty(RestyleQueue& queue);

    /// Full re-cascade. Used after stylesheet changes; in normal
    /// operation, prefer `resolve_dirty`.
    StyleStats resolve_all();

    /// Read a node's current ComputedStyle. Returns a refcounted view;
    /// caller must call release() when done.
    const ComputedStyle* style_of(NodeId node) const;

    /// Animation-driven property override: write a computed value
    /// directly without going through cascade. Used by composite-only
    /// animations on `transform` / `opacity` etc. Returns false if the
    /// property requires re-paint (not composite-only).
    bool apply_animated_value(NodeId node, PropertyId prop, float value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace affineui
