#pragma once

// `RestyleQueue` — the invalidation seam.
//
// All DOM / class / attr / custom-property mutations route through this
// queue. Style computation drains the queue once per frame (or as
// requested) and produces a list of elements whose ComputedStyle needs
// rebuilding.
//
// This shape exists so the Phase 2 implementation (naive: re-cascade
// everything in the queue) can be replaced by the Phase 4 implementation
// (invalidation sets, docs/OPTIMIZATION.md § 2.3) without changing the
// rest of the engine. The *consumer* of the queue stays the same; only
// the *producer* gets smarter.

#include "affineui/style.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace affineui {

using NodeId = std::uint32_t;

/// What changed about an element. Producers report this; the queue
/// decides which subtree(s) to mark dirty.
enum class MutationKind : std::uint8_t {
    ClassAdded,
    ClassRemoved,
    AttrSet,
    AttrRemoved,
    InlineStyleSet,
    NodeInserted,
    NodeRemoved,
    NodeMoved,
    CustomPropertyChanged,
    PseudoClassToggled,    // :hover / :active / :focus
    UserStylesheetChanged,
    ViewportChanged,
};

struct Mutation {
    NodeId       node{0};
    MutationKind kind{MutationKind::AttrSet};
    // Identifier of the affected name (class, attribute, custom prop).
    // Interned u32; meaning depends on `kind`.
    std::uint32_t name_id{0};
};

class RestyleQueue {
public:
    void enqueue(const Mutation& m);
    void enqueue_subtree(NodeId root);

    /// True when the next frame needs a restyle pass.
    bool dirty() const noexcept { return !pending_.empty() || !subtrees_.empty(); }

    /// Drain and return the set of nodes whose ComputedStyle should be
    /// rebuilt. In Phase 2 this is "everything in the dirty subtrees,
    /// recursive." In Phase 4 it consults invalidation sets and emits
    /// a much smaller list.
    std::vector<NodeId> drain_to_dirty_nodes();

    void clear() noexcept;

private:
    std::vector<Mutation> pending_;
    std::vector<NodeId>   subtrees_;
};

}  // namespace affineui
