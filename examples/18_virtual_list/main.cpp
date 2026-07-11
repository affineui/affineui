// virtual_list — a stress demo of the recycling virtual list & tree.
//
// The list holds 200,000 synthetic rows and the tree tens of thousands of
// nodes, yet only the rows under the viewport (plus a small overscan) are ever
// materialized. Scrolling — wheel, scrollbar drag, Home/End/arrows — re-windows
// automatically; the scrollbar reflects the full extent as if every row were
// real. Selection works with plain / Ctrl (toggle) / Shift (range) clicks; the
// tree's chevrons expand and collapse.
//
// Data lives in the app; the providers are stateless bridges of guarded
// callbacks, owned by the app via unique_ptr and held weakly by the widget. The
// tree is driven by a TreeFlattener over a weak-ref'd Scene: node handles (here
// an id into the Scene) are flattened to the visible set, and resolved to the
// live node pointer only at render — so a deleted node can never dangle.

#include <affineui/affineui.h>
#include <affineui/virtual_list.h>
#include <affineui/weak_ref.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

// A synthetic scene: 5000 group nodes, each with 4 children. Nodes are addressed
// by a stable id (the handle). This is the app-owned tree data, held weakly by
// the flattener. `resolve(id)` returns the live node or null.
struct Node {
    std::string          name;
    std::vector<std::uint64_t> children;
};

class Scene : public affineui::Trackable {
public:
    Scene() {
        constexpr int kRoots = 5000;
        constexpr int kChildren = 4;
        nodes_.resize(1);  // index 0 unused so id 0 == "none"
        roots_.reserve(kRoots);
        for (int r = 0; r < kRoots; ++r) {
            const auto root_id = add("Group " + std::to_string(r));
            roots_.push_back(root_id);
            for (int c = 0; c < kChildren; ++c) {
                const auto child_id =
                    add("Child " + std::to_string(r) + "." + std::to_string(c));
                nodes_[root_id].children.push_back(child_id);
            }
        }
    }

    const std::vector<std::uint64_t>& roots() const { return roots_; }
    Node* resolve(std::uint64_t id) {
        return id < nodes_.size() ? &nodes_[id] : nullptr;
    }

private:
    std::uint64_t add(std::string name) {
        nodes_.push_back({std::move(name), {}});
        return nodes_.size() - 1;
    }
    std::vector<Node>          nodes_;
    std::vector<std::uint64_t> roots_;
};

// The demo controller: owns the data, providers, and the tree flattener.
class Demo {
public:
    explicit Demo(affineui::App& app)
        : app_(app), tree_flat_(affineui::to_weak_ref(&scene_)) {
        // --- List: 200k synthetic rows, string-list convenience overload ---
        // Selection lives in list_sel_ (plain/Ctrl/Shift), driven straight from
        // the string overload — no provider boilerplate for the simple case.
        list_items_.reserve(200000);
        for (std::size_t i = 0; i < 200000; ++i) {
            list_items_.push_back("Row " + std::to_string(i));
        }
        list_sel_.on_change([this] { app_.rebuild_view(); });
        list_checked_.on_change([this] { app_.rebuild_view(); });

        // --- Tree: a TreeFlattener over the weak-ref'd Scene ----------------
        tree_ = std::make_unique<affineui::VirtualTreeProvider>();
        tree_flat_
            .on_roots([](Scene* s, std::vector<std::uint64_t>& out) {
                out = s->roots();
            })
            .on_children([](Scene* s, std::uint64_t id,
                            std::vector<std::uint64_t>& out) {
                if (Node* n = s->resolve(id)) out = n->children;
            })
            .on_has_children([](Scene* s, std::uint64_t id) {
                Node* n = s->resolve(id);
                return n && !n->children.empty();
            })
            .on_label([](Scene* s, std::uint64_t id) {
                Node* n = s->resolve(id);
                return n ? n->name : std::string{};
            })
            .on_changed([this] { app_.rebuild_view(); });
        // wire() also wires selection to the flattener's HANDLE-keyed model:
        // opening/closing nodes renumbers rows, but handles are stable, so
        // the selection stays on the same items.
        tree_flat_.wire(*tree_);
        tree_->default_item_size(24.0);
    }

    void build(affineui::View& v) {
        // The virtual list element is ITSELF the scroll box — the scroll seam
        // (scroll -> re-window rebuild) listens on it. Never wrap it in another
        // overflow:auto box; instead give it a definite height, here via a
        // 100vh root -> flex column -> flex:1;min-height:0 chain.
        auto outer = v.container("vstack", "outer");
        outer.attr("style",
                   "display:flex;flex-direction:column;gap:6px;height:100vh;"
                   "overflow:hidden;padding:8px;box-sizing:border-box");
        {
            auto bar = v.container("hstack", "toolbar");
            bar.attr("style",
                     "flex:0 0 26px;height:26px;display:flex;"
                     "align-items:center;gap:8px;margin:0;padding:0;"
                     "overflow:hidden");
            // Checkbox MODE toggle: rows carry two independent states —
            // selected and checked. Flipping the mode is one structural
            // rebuild; scrolling stays attribute-only either way. Pin the
            // themed checkbox group compact so it doesn't inflate the bar.
            v.checkbox("Row checkboxes (list + tree)", checks_enabled_,
                       "cb-mode")
                .attr("style",
                      "margin:0;padding:0;min-height:0;height:auto;"
                      "display:inline-flex;align-items:center;gap:6px")
                .on_click([this] {
                    checks_enabled_ = !checks_enabled_;
                    tree_->checkboxes(checks_enabled_);
                    app_.rebuild_view();
                });
        }
        auto row = v.container("hstack", "root");
        row.attr("style",
                 "flex:1 1 0;min-height:0;display:flex;gap:8px");
        {
            auto lcol = v.container("vstack", "list-col");
            lcol.attr("style",
                      "flex:1;display:flex;flex-direction:column;"
                      "min-width:0;min-height:0");
            v.paragraph("200,000-row virtual list", "list-title")
                .attr("style", "margin:0 0 6px;font-weight:600;flex:0 0 auto");
            affineui::View::StringListOptions opts;
            opts.item_size = 26.0;
            opts.selection = &list_sel_;
            opts.checked = checks_enabled_ ? &list_checked_ : nullptr;
            // flex basis 0, NOT auto: basis auto = content size (5.2M px of
            // spacers), which makes the column's shrink phase crush the title
            // to zero height — its text then paints over the list.
            v.virtual_list("big-list", list_items_, opts)
                .attr("style",
                      "flex:1 1 0;min-height:0;border:1px solid #4448");
        }
        {
            auto tcol = v.container("vstack", "tree-col");
            tcol.attr("style",
                      "flex:1;display:flex;flex-direction:column;"
                      "min-width:0;min-height:0");
            v.paragraph("Virtual tree (click chevrons)", "tree-title")
                .attr("style", "margin:0 0 6px;font-weight:600;flex:0 0 auto");
            v.virtual_tree("big-tree", *tree_)
                .attr("style",
                      "flex:1 1 0;min-height:0;border:1px solid #4448");
        }
    }

private:
    affineui::App& app_;
    Scene          scene_;
    std::unique_ptr<affineui::VirtualTreeProvider> tree_;
    affineui::TreeFlattener<Scene, void, std::uint64_t> tree_flat_;
    std::vector<std::string> list_items_;
    affineui::IndexSelection list_sel_;
    affineui::IndexSelection list_checked_;
    bool                     checks_enabled_{false};
};

}  // namespace

int main(int, char**) {
    affineui::App::Config config;
    config.title = "AffineUI - virtual list (200k)";
    config.width = 900;
    config.height = 620;
    config.asset_folders = {"examples", "."};

    affineui::App app{config};
    // Full-window app: drop the document-style root gutter (same idiom as the
    // game-editor / DENDER samples) so the 100vh layout fits the window.
    app.set_stylesheet(".aui-root{min-height:0;padding:0}");

    Demo demo{app};
    app.set_view([&demo](affineui::View& v) { demo.build(v); });
    return app.run();
}
