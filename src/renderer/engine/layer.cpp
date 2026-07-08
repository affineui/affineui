#include "renderer/engine/layer.h"

#include <algorithm>

namespace affineui {

LayerTree::LayerTree() {
    // Root layer is always layer 0, and always present.
    layers_.emplace_back();
    auto& root = layers_.back();
    root.id     = 0;
    root.flags  = LF_RasterizeDirty | LF_CompositeDirty | LF_RootLayer;
    root_       = 0;
}

LayerId LayerTree::allocate(LayerId parent) {
    LayerId id;
    if (!free_list_.empty()) {
        id = free_list_.back();
        free_list_.pop_back();
        layers_[id] = Layer{};
    } else {
        id = static_cast<LayerId>(layers_.size());
        layers_.emplace_back();
    }
    auto& layer  = layers_[id];
    layer.id     = id;
    layer.parent = parent;
    layer.flags  = LF_RasterizeDirty | LF_CompositeDirty;

    if (parent != kInvalidLayer) {
        layers_[parent].children.push_back(id);
        layers_[parent].flags |= LF_CompositeDirty;
    }
    return id;
}

void LayerTree::free_layer(LayerId id) {
    if (id == kInvalidLayer || id >= layers_.size()) return;
    auto& layer = layers_[id];
    // Texture return-to-pool is delegated to the Rasterizer; that
    // happens via Engine before this call. Here we just unlink from
    // the tree.
    if (layer.parent != kInvalidLayer) {
        auto& parent = layers_[layer.parent];
        auto it = std::find(parent.children.begin(), parent.children.end(), id);
        if (it != parent.children.end()) parent.children.erase(it);
        parent.flags |= LF_CompositeDirty;
    }
    layer.children.clear();
    layer.texture = kInvalidTexture;
    free_list_.push_back(id);
}

void LayerTree::sort_z() {
    for (auto& layer : layers_) {
        std::stable_sort(layer.children.begin(), layer.children.end(),
            [this](LayerId a, LayerId b) {
                return layers_[a].z_index < layers_[b].z_index;
            });
    }
}

template <typename Visitor>
void LayerTree::walk_composite(Visitor&& visitor) {
    if (root_ == kInvalidLayer) return;
    auto walk = [&](auto& self, LayerId id, int depth) -> void {
        auto& layer = layers_[id];
        if (layer.offscreen()) return;
        visitor(layer, depth);
        for (auto child : layer.children) self(self, child, depth + 1);
    };
    walk(walk, root_, 0);
}

}  // namespace affineui
