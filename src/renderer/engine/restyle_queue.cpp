#include "renderer/engine/restyle_queue.h"

#include <algorithm>

namespace affineui {

void RestyleQueue::enqueue(const Mutation& m) {
    pending_.push_back(m);
}

void RestyleQueue::enqueue_subtree(NodeId root) {
    subtrees_.push_back(root);
}

std::vector<NodeId> RestyleQueue::drain_to_dirty_nodes() {
    // Phase 2 implementation: every node in any flagged subtree becomes
    // dirty. Per-mutation invalidation-set lookups happen later (see
    // docs/OPTIMIZATION.md § 2.3); the queue's interface is shaped so
    // that change is invisible to consumers.
    std::vector<NodeId> result;
    result.reserve(pending_.size() + subtrees_.size());

    for (const auto& m : pending_) {
        result.push_back(m.node);
    }
    for (auto root : subtrees_) {
        result.push_back(root);
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    pending_.clear();
    subtrees_.clear();
    return result;
}

void RestyleQueue::clear() noexcept {
    pending_.clear();
    subtrees_.clear();
}

}  // namespace affineui
