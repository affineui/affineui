// Virtual list & tree providers: windowing math, recycling invariants,
// selection model, and the builder's window/spacer output.

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "affineui/document.h"
#include "affineui/view.h"
#include "affineui/virtual_list.h"

using affineui::Axis;
using affineui::compute_window;
using affineui::DropPos;
using affineui::IndexSelection;
using affineui::SelectMod;
using affineui::View;
using affineui::VirtualListProvider;
using affineui::virtual_item_at;
using affineui::virtual_offset;
using affineui::VirtualTreeProvider;

namespace {

// A uniform-height provider over a synthetic count. Providers are non-movable
// (Trackable holds a weak slot bound to a stable address), so configure in
// place via a small helper that returns nothing and takes the target by ref.
void make_uniform(VirtualListProvider& p, std::size_t count, double row = 20.0) {
    p.on_item_count([count] { return count; }).default_item_size(row);
}

// Counts the mutations a rebuild pushes to the sink — the recycling invariant
// is "scroll re-window = text/attr diffs ONLY, zero creates/removes".
struct CountingSink final : affineui::ViewSink {
    int creates = 0, removes = 0, texts = 0, attrs = 0;
    void create_element(const affineui::WidgetNode&,
                        const affineui::WidgetNode*, std::size_t) override {
        ++creates;
    }
    void create_text(const affineui::WidgetNode&, const affineui::WidgetNode*,
                     std::size_t) override {
        ++creates;
    }
    void remove(const affineui::WidgetNode&) override { ++removes; }
    void set_text(const affineui::WidgetNode&, std::string_view) override {
        ++texts;
    }
    void set_attribute(const affineui::WidgetNode&, std::string_view,
                       std::string_view) override {
        ++attrs;
    }
    void remove_attribute(const affineui::WidgetNode&,
                          std::string_view) override {}
};

}  // namespace

TEST_CASE("virtual_offset / virtual_item_at: uniform sizes") {
    VirtualListProvider p; make_uniform(p, 1000, 20.0);
    CHECK(virtual_offset(p, 0, 1000) == doctest::Approx(0.0));
    CHECK(virtual_offset(p, 10, 1000) == doctest::Approx(200.0));
    CHECK(virtual_offset(p, 1000, 1000) == doctest::Approx(20000.0));
    // Clamps past the end.
    CHECK(virtual_offset(p, 5000, 1000) == doctest::Approx(20000.0));

    CHECK(virtual_item_at(p, 0.0, 1000) == 0);
    CHECK(virtual_item_at(p, 25.0, 1000) == 1);
    CHECK(virtual_item_at(p, 205.0, 1000) == 10);
    CHECK(virtual_item_at(p, 1e9, 1000) == 999);  // clamps to last
}

TEST_CASE("virtual_offset / virtual_item_at: variable sizes") {
    VirtualListProvider p;
    p.on_item_count([] { return std::size_t{5}; })
        .on_item_size([](std::size_t i) { return 10.0 * (i + 1); });  // 10,20,30,40,50
    CHECK(virtual_offset(p, 0, 5) == doctest::Approx(0.0));
    CHECK(virtual_offset(p, 1, 5) == doctest::Approx(10.0));
    CHECK(virtual_offset(p, 3, 5) == doctest::Approx(60.0));   // 10+20+30
    CHECK(virtual_offset(p, 5, 5) == doctest::Approx(150.0));  // total

    CHECK(virtual_item_at(p, 5.0, 5) == 0);    // within item 0 (0..10)
    CHECK(virtual_item_at(p, 15.0, 5) == 1);   // within item 1 (10..30)
    CHECK(virtual_item_at(p, 65.0, 5) == 3);   // within item 3 (60..100)
}

TEST_CASE("compute_window: only the visible block + overscan is built") {
    VirtualListProvider p; make_uniform(p, 200000, 20.0);
    // Viewport of 200px shows 10 rows; scroll to item ~1000.
    const auto w = compute_window(p, /*scroll_px=*/20000, /*viewport=*/200.0,
                                  /*overscan=*/2, /*default_visible=*/16);
    CHECK(w.first == 1000);
    CHECK(w.begin == 998);                 // first - overscan
    // ~10 visible + 2 partial-edge + 2 overscan; a small bounded window, never
    // the whole 200k list.
    CHECK((w.end - w.begin) < 32);
    CHECK(w.total_px == doctest::Approx(4000000.0));  // 200000 * 20
    // Leading spacer places the built block at its true offset.
    CHECK(w.lead_px == doctest::Approx(998 * 20.0));
    // lead + built rows + trail == total extent.
    const double built = (w.end - w.begin) * 20.0;
    CHECK(w.lead_px + built + w.trail_px == doctest::Approx(w.total_px));
}

TEST_CASE("compute_window: clamps scroll into the honest extent") {
    VirtualListProvider p; make_uniform(p, 1000, 20.0);
    // Scroll way past the end with a 100px viewport: window must sit at the
    // bottom, not run off the list.
    const auto w = compute_window(p, 999999999, 100.0, 2, 16);
    CHECK(w.end == 1000);
    CHECK(w.first <= 999);
    CHECK(w.trail_px == doctest::Approx(0.0));
}

TEST_CASE("compute_window: empty list is safe") {
    VirtualListProvider p; make_uniform(p, 0);
    const auto w = compute_window(p, 0, 200.0, 2, 16);
    CHECK(w.begin == 0);
    CHECK(w.end == 0);
    CHECK(w.total_px == doctest::Approx(0.0));
}

TEST_CASE("compute_window: sliding one row keeps a constant window size") {
    // Recycling relies on a stable number of built rows as the window slides.
    VirtualListProvider p; make_uniform(p, 100000, 20.0);
    const auto a = compute_window(p, 20000, 200.0, 2, 16);
    const auto b = compute_window(p, 20020, 200.0, 2, 16);  // +1 row
    CHECK(b.first == a.first + 1);
    CHECK((a.end - a.begin) == (b.end - b.begin));  // same count → recycles
}

TEST_CASE("IndexSelection: replace, toggle, range with an index anchor") {
    IndexSelection sel;
    int changes = 0;
    sel.on_change([&] { ++changes; });

    sel.apply(5, SelectMod::Replace);
    CHECK(sel.contains(5));
    CHECK(sel.size() == 1);
    CHECK(sel.anchor() == 5);

    sel.apply(8, SelectMod::Toggle);
    CHECK(sel.contains(5));
    CHECK(sel.contains(8));
    CHECK(sel.size() == 2);

    sel.apply(8, SelectMod::Toggle);  // toggles off; anchor is now 8
    CHECK_FALSE(sel.contains(8));
    CHECK(sel.size() == 1);
    CHECK(sel.anchor() == 8);  // toggle moves the anchor (standard behavior)

    // Shift-range extends from the anchor (8, the last touched row) to 10.
    sel.apply(10, SelectMod::Range);
    for (std::size_t i = 8; i <= 10; ++i) CHECK(sel.contains(i));
    CHECK(sel.size() == 3);         // {8, 9, 10}
    CHECK_FALSE(sel.contains(5));   // range replaces the prior selection

    CHECK(changes == 4);
}

TEST_CASE("IndexSelection: range clamps to count") {
    IndexSelection sel;
    sel.apply(3, SelectMod::Replace);
    sel.apply(100, SelectMod::Range, /*count=*/10);
    CHECK(sel.contains(3));
    CHECK(sel.contains(9));
    CHECK_FALSE(sel.contains(10));
    CHECK(sel.size() == 7);  // 3..9
}

TEST_CASE("virtual_list builder: window rows, slot keys, selection stamp") {
    View view{affineui::ViewTheme::Bootstrap};
    VirtualListProvider p; make_uniform(p, 100000, 20.0);
    p.on_item_text([](std::size_t i) { return "Item " + std::to_string(i); })
        .on_is_selected([](std::size_t i) { return i == 3; });

    view.begin();
    // No scroll provider set → first build windows at the top.
    view.virtual_list("big", p);
    view.end();

    const auto html = view.to_html_fragment();
    // Bounded window: item 0 present, a far item absent.
    CHECK(html.find("Item 0") != std::string::npos);
    CHECK(html.find("Item 90000") == std::string::npos);
    // Full extent carried by the trailing spacer.
    CHECK(html.find("data-item-count=\"100000\"") != std::string::npos);
    // Selected row stamped from the model.
    CHECK(html.find("aria-selected=\"true\"") != std::string::npos);
    // Marked as a virtual list so native selection emits model activations.
    CHECK(html.find("data-aui-virtual=\"list\"") != std::string::npos);
}

namespace {

// A tiny tree data source for the flattener tests: nodes addressed by id.
struct MiniTree : affineui::Trackable {
    struct N { std::string name; std::vector<std::uint64_t> kids; };
    std::vector<N>             nodes{{"", {}}};  // id 0 unused
    std::vector<std::uint64_t> roots;
    std::uint64_t add(std::string name) {
        nodes.push_back({std::move(name), {}});
        return nodes.size() - 1;
    }
    N* resolve(std::uint64_t id) {
        return id < nodes.size() ? &nodes[id] : nullptr;
    }
};

std::unique_ptr<MiniTree> make_mini() {
    auto t = std::make_unique<MiniTree>();
    // 2 roots, each with 2 children.
    for (int r = 0; r < 2; ++r) {
        auto root = t->add("root" + std::to_string(r));
        t->roots.push_back(root);
        for (int c = 0; c < 2; ++c) {
            // add() may reallocate nodes — take the id first, then index, or
            // the kids reference dangles (order of evaluation is unspecified).
            const auto kid =
                t->add("child" + std::to_string(r) + std::to_string(c));
            t->nodes[root].kids.push_back(kid);
        }
    }
    return t;
}

using Flat = affineui::TreeFlattener<MiniTree, MiniTree::N, std::uint64_t>;

void wire_mini(Flat& f) {
    f.on_roots([](MiniTree* t, std::vector<std::uint64_t>& out) {
         out = t->roots;
     })
        .on_children([](MiniTree* t, std::uint64_t id,
                        std::vector<std::uint64_t>& out) {
            if (auto* n = t->resolve(id)) out = n->kids;
        })
        .on_has_children([](MiniTree* t, std::uint64_t id) {
            auto* n = t->resolve(id);
            return n && !n->kids.empty();
        })
        .on_label([](MiniTree* t, std::uint64_t id) {
            auto* n = t->resolve(id);
            return n ? n->name : std::string{};
        });
}

}  // namespace

TEST_CASE("TreeFlattener: collapsed shows only roots; expand reveals children") {
    auto tree = make_mini();
    Flat f{affineui::to_weak_ref(tree.get())};
    wire_mini(f);
    affineui::VirtualTreeProvider p;
    f.wire(p);

    // All collapsed → 2 roots.
    CHECK(p.item_count() == 2);
    CHECK(p.is_expandable(0));
    CHECK(f.handle_at(0) == tree->roots[0]);

    // Expand root 0 → 2 roots + 2 children = 4, children at depth 1.
    f.set_expanded(tree->roots[0], true);
    CHECK(p.item_count() == 4);
    CHECK(p.depth(0) == 0);
    CHECK(p.depth(1) == 1);
    CHECK(p.is_expanded(0));

    // Collapse again → back to 2.
    f.set_expanded(tree->roots[0], false);
    CHECK(p.item_count() == 2);
}

TEST_CASE("TreeFlattener selection is handle-keyed: survives expand/collapse") {
    auto tree = make_mini();
    Flat f{affineui::to_weak_ref(tree.get())};
    wire_mini(f);
    affineui::VirtualTreeProvider p;
    f.wire(p);

    // Collapsed: rows are [root0, root1]. Select root1 via the wired path.
    REQUIRE(p.item_count() == 2);
    p.activate(1, SelectMod::Replace);
    CHECK(p.is_selected(1));
    CHECK(f.selected_contains(tree->roots[1]));

    // Expand root0: root1's INDEX moves from 1 to 3 — the selection must
    // follow the handle, not stick to whichever item now sits at index 1.
    f.set_expanded(tree->roots[0], true);
    REQUIRE(p.item_count() == 4);
    CHECK_FALSE(p.is_selected(1));  // child of root0 now at index 1
    CHECK(p.is_selected(3));        // root1, at its new index
    CHECK(f.selected_contains(tree->roots[1]));

    // Range from root1 (anchor) back to index 1 selects the visible span by
    // handle: [child00 at 1, child01 at 2, root1 at 3].
    p.activate(1, SelectMod::Range);
    CHECK(f.selected().size() == 3);
    CHECK(p.is_selected(1));
    CHECK(p.is_selected(2));
    CHECK(p.is_selected(3));
    CHECK_FALSE(p.is_selected(0));

    // Collapse root0: the hidden children stay selected by handle; root1
    // (back at index 1) is still selected.
    f.set_expanded(tree->roots[0], false);
    REQUIRE(p.item_count() == 2);
    CHECK(p.is_selected(1));
    CHECK(f.selected().size() == 3);
}

TEST_CASE("TreeFlattener: handle resolves to the live item; null when data dies") {
    auto tree = make_mini();
    Flat f{affineui::to_weak_ref(tree.get())};
    wire_mini(f);
    // Add a resolver so item_at works.
    f.on_resolve([](MiniTree* t, std::uint64_t id) {
        return t->resolve(id);
    });
    affineui::VirtualTreeProvider p;
    f.wire(p);

    auto* item = f.item_at(0);
    REQUIRE(item != nullptr);
    CHECK(item->name == "root0");

    // Destroy the data source: the flattener holds only a weak ref + handles,
    // so resolution returns null rather than dereferencing freed memory.
    tree.reset();
    CHECK(f.item_at(0) == nullptr);
    // A rebuild after the data is gone empties the flattened view.
    f.rebuild();
    CHECK(p.item_count() == 0);
}

TEST_CASE("TreeFlattener: wired provider safely outlives the flattener") {
    auto tree = make_mini();
    affineui::VirtualTreeProvider provider;
    {
        Flat flattener{affineui::to_weak_ref(tree.get())};
        wire_mini(flattener);
        flattener.wire(provider);
        REQUIRE(provider.item_count() == 2);
    }

    CHECK(provider.item_count() == 0);
    CHECK(provider.depth(0) == 0);
    CHECK_FALSE(provider.is_expandable(0));
    CHECK_FALSE(provider.is_expanded(0));
    CHECK_FALSE(provider.is_selected(0));
    CHECK_FALSE(provider.is_checked(0));
    CHECK_NOTHROW(provider.toggle(0));
    CHECK_NOTHROW(provider.activate(0, SelectMod::Replace));
    CHECK_NOTHROW(provider.set_checked(0, true));
    REQUIRE(static_cast<bool>(provider.item_text()));
    CHECK(provider.item_text()(0).empty());
}

TEST_CASE("TreeFlattener tolerates destruction from a model callback") {
    auto tree = make_mini();
    auto flattener = std::make_unique<Flat>(
        affineui::to_weak_ref(tree.get()));
    bool destroy_during_rebuild = false;
    flattener
        ->on_roots([&](MiniTree* source,
                       std::vector<std::uint64_t>& out) {
            out = source->roots;
            if (destroy_during_rebuild) flattener.reset();
        })
        .on_children([](MiniTree* source, std::uint64_t id,
                        std::vector<std::uint64_t>& out) {
            if (auto* node = source->resolve(id)) out = node->kids;
        })
        .on_has_children([](MiniTree* source, std::uint64_t id) {
            const auto* node = source->resolve(id);
            return node != nullptr && !node->kids.empty();
        })
        .on_label([](MiniTree* source, std::uint64_t id) {
            const auto* node = source->resolve(id);
            return node != nullptr ? node->name : std::string{};
        });

    affineui::VirtualTreeProvider provider;
    flattener->wire(provider);
    REQUIRE(provider.item_count() == 2);
    REQUIRE(provider.is_expandable(0));

    destroy_during_rebuild = true;
    CHECK_NOTHROW(provider.toggle(0));
    CHECK(flattener == nullptr);
    CHECK(provider.item_count() == 0);
}

TEST_CASE("virtual_tree builder: depth, chevron state, flattened window") {
    View view{affineui::ViewTheme::Bootstrap};
    VirtualTreeProvider t;
    t.on_item_count([] { return std::size_t{50000}; })
        .default_item_size(22.0)
        .on_item_text([](std::size_t i) { return "Node " + std::to_string(i); })
        .on_depth([](std::size_t i) { return static_cast<int>(i % 4); })
        .on_is_expandable([](std::size_t i) { return i % 2 == 0; })
        .on_is_expanded([](std::size_t) { return true; });

    view.begin();
    view.virtual_tree("tree", t);
    view.end();

    const auto html = view.to_html_fragment();
    CHECK(html.find("data-aui-virtual=\"tree\"") != std::string::npos);
    CHECK(html.find("Node 0") != std::string::npos);
    CHECK(html.find("Node 40000") == std::string::npos);
    // Expandable rows carry a toggle-able chevron marker.
    CHECK(html.find("data-aui-chevron=\"0\"") != std::string::npos);
    // Depth drives an indent custom property.
    CHECK(html.find("--depth:") != std::string::npos);

    // Structural: every row and both spacers must be CHILDREN of the tree box.
    // (Regression: a spurious close_node() after the chevron leaf popped the
    // ROW instead, unwinding each later row one ancestor higher — rows escaped
    // the tree into outer containers.)
    const auto anchor = html.find("data-aui-virtual=\"tree\"");
    REQUIRE(anchor != std::string::npos);
    const auto start = html.rfind("<div", anchor);
    REQUIRE(start != std::string::npos);
    std::size_t divs = 0, pos = start, subtree_end = std::string::npos;
    while (pos < html.size()) {
        const auto next_open = html.find("<div", pos);
        const auto next_close = html.find("</div>", pos);
        if (next_close == std::string::npos) break;
        if (next_open != std::string::npos && next_open < next_close) {
            ++divs;
            pos = next_open + 4;
        } else {
            --divs;
            pos = next_close + 6;
            if (divs == 0) { subtree_end = pos; break; }
        }
    }
    REQUIRE(subtree_end != std::string::npos);
    const auto subtree = html.substr(start, subtree_end - start);
    const auto count_in = [](const std::string& s, std::string_view needle) {
        std::size_t n = 0;
        for (auto at = s.find(needle); at != std::string::npos;
             at = s.find(needle, at + 1)) ++n;
        return n;
    };
    const auto rows_total = count_in(html, "role=\"treeitem\"");
    CHECK(rows_total > 0);
    CHECK(count_in(subtree, "role=\"treeitem\"") == rows_total);
    CHECK(count_in(subtree, "aui-virtual-list__spacer") == 2);
}

TEST_CASE("spacer heights stay exact at multi-million px extents") {
    // %g-style formatting turns 5.2M px into "5.2e+06" — invalid CSS, the
    // spacer collapses, and the scroll extent lies (thumb grows/shrinks and
    // the list snaps near a phantom bottom). Spacer px must never use
    // scientific notation, at any magnitude.
    View view{affineui::ViewTheme::Bootstrap};
    VirtualListProvider p;
    make_uniform(p, 200000, 26.0);
    p.on_item_text([](std::size_t i) { return std::to_string(i); });
    view.begin();
    view.virtual_list("big", p);
    view.end();
    const auto html = view.to_html_fragment();
    CHECK(html.find("e+") == std::string::npos);
    // Trailing spacer carries the full remaining extent (7 digits, exact).
    CHECK(html.find("5197452px") != std::string::npos);
}

TEST_CASE("virtual list in a real document: honest multi-million px extent") {
    // End-to-end through the ACTUAL runtime path: the builder's spacer px
    // strings -> the real CSS parser -> layout -> native wheel scroll. If the
    // spacer strings are corrupted (e.g. the %g scientific-notation bug) the
    // scroll clamps against a phantom bottom a few thousand px in; honest
    // content is 200000 * 26 = 5.2M px.
    View view{affineui::ViewTheme::Bootstrap};
    VirtualListProvider p;
    make_uniform(p, 200000, 26.0);
    p.on_item_text([](std::size_t i) { return "Row " + std::to_string(i); });
    view.begin();
    view.virtual_list("big", p);
    view.end();

    affineui::Document doc;
    doc.set_html(
        "<style>"
        "html,body{margin:0;padding:0}"
        ".aui-virtual-list{display:flex;flex-direction:column;height:520px;"
        "min-height:0;overflow:auto}"
        ".aui-virtual-list__spacer{flex:0 0 auto;min-height:0}"
        ".aui-virtual-list__row{flex:0 0 auto;min-height:0;overflow:hidden}"
        "</style>" +
        view.to_html_fragment());
    doc.layout(800, 600);

    auto g = doc.virtual_scroll_geometry("big");
    REQUIRE(g.known);
    CHECK(g.viewport == doctest::Approx(520.0).epsilon(0.05));

    // The wheel resolves its scroll target through the hover chain — hover
    // the list first (same choreography as the document scroll tests).
    affineui::Event hover{};
    hover.type = affineui::EventType::MouseMove;
    hover.pos = {50, 50};
    doc.dispatch(hover);

    affineui::Event wheel{};
    wheel.type = affineui::EventType::MouseWheel;
    wheel.pos = {50, 50};
    wheel.wheel_dy = -3.0f;
    doc.dispatch(wheel);
    g = doc.virtual_scroll_geometry("big");
    CHECK(g.offset > 0);
    // The scroll queued a re-window request for this widget.
    CHECK(!doc.take_widget_scrolls().empty());

    // Ride the wheel to the floor: a huge delta clamps at content - viewport.
    wheel.wheel_dy = -1e8f;
    doc.dispatch(wheel);
    g = doc.virtual_scroll_geometry("big");
    CHECK(g.offset > 4'000'000);
}

TEST_CASE("virtual list recycles on scroll: zero creates/removes, diffs only") {
    View view{affineui::ViewTheme::Bootstrap};
    VirtualListProvider p;
    make_uniform(p, 200000, 26.0);
    p.on_item_text([](std::size_t i) { return "Row " + std::to_string(i); });

    std::int64_t scroll = 26 * 1000;  // start deep so the window never clamps
    view.set_scroll_provider(
        [&scroll](std::string_view, Axis) -> View::ScrollGeometry {
            return {scroll, 780.0, true};
        });
    const auto build = [&](affineui::ViewSink* sink) {
        view.begin(sink);
        view.virtual_list("big", p);
        view.end();
    };

    build(nullptr);  // initial build populates the retained tree

    // Slide the window: sub-row tick, multi-row jump, and a huge jump. Every
    // rebuild must reuse the slot-keyed rows — content diffs only. A create or
    // remove here means recycling is broken (rows recreated every scroll).
    for (const std::int64_t delta : {13LL, 26LL * 7, 26LL * 50000}) {
        scroll += delta;
        CountingSink sink;
        build(&sink);
        CAPTURE(delta);
        CHECK(sink.creates == 0);
        CHECK(sink.removes == 0);
    }

    // Sanity: the window did move — a row jump rewrites row text.
    CountingSink sink;
    scroll += 26 * 3;
    build(&sink);
    CHECK(sink.creates == 0);
    CHECK(sink.removes == 0);
    CHECK(sink.texts > 0);
}
