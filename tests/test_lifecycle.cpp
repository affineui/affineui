#include <doctest/doctest.h>

#include "affineui/app.h"
#include "affineui/renderer.h"
#include "affineui/ui.h"
#include "affineui/view.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

#if !defined(AFFINEUI_STUB_BUILD)
#    include "renderer/dom/document_impl.h"
#    include "renderer/style/style_store.h"

#    include <lexbor/dom/dom.h>
#    include <lexbor/html/html.h>
#endif

static_assert(!std::is_move_constructible_v<affineui::Renderer>);
static_assert(!std::is_move_assignable_v<affineui::Renderer>);

TEST_CASE("Ui handler iteration snapshots re-entrant registrations") {
    affineui::Ui ui;
    affineui::Event resize{};
    resize.type = affineui::EventType::Resize;

    int original_calls = 0;
    int late_calls = 0;
    ui.on_event([&](const affineui::Event&,
                    const std::vector<affineui::Document::HoverInfo>&) {
        ++original_calls;
        ui.on_event([&](const affineui::Event&,
                        const std::vector<affineui::Document::HoverInfo>&) {
            ++late_calls;
            return false;
        });
        return false;
    });

    ui.dispatch(resize);
    CHECK(original_calls == 1);
    CHECK(late_calls == 0);

    ui.dispatch(resize);
    CHECK(original_calls == 2);
    CHECK(late_calls == 1);
}

TEST_CASE("Ui frame callbacks snapshot re-entrant registrations") {
    affineui::Ui ui;
    int original_calls = 0;
    int late_calls = 0;
    ui.on_frame([&](double) {
        ++original_calls;
        ui.on_frame([&](double) { ++late_calls; });
    });

    ui.run_frame_callbacks(1.0 / 60.0);
    CHECK(original_calls == 1);
    CHECK(late_calls == 0);

    ui.run_frame_callbacks(1.0 / 60.0);
    CHECK(original_calls == 2);
    CHECK(late_calls == 1);
}

TEST_CASE("Ui click callbacks survive a re-entrant reset") {
    affineui::Ui ui;
    ui.html(R"(<button id="reset" style="display:block;width:80px;height:32px">Reset</button>)");
    ui.document().layout(120, 60);

    affineui::Event move{};
    move.type = affineui::EventType::MouseMove;
    move.pos = {16, 16};
    ui.dispatch(move);
    const auto chain = ui.document().hovered_info_chain();
    REQUIRE(std::any_of(chain.begin(), chain.end(), [](const auto& info) {
        return info.elem_id == "reset";
    }));

    int reset_calls = 0;
    int trailing_calls = 0;
    ui.on_click("#reset", [&] {
        ++reset_calls;
        ui.reset();
    });
    ui.on_click("#reset", [&] { ++trailing_calls; });

    affineui::Event up{};
    up.type = affineui::EventType::MouseUp;
    up.button = affineui::MouseButton::Left;
    up.pos = move.pos;
    CHECK(ui.dispatch(up));
    CHECK(reset_calls == 1);
    CHECK(trailing_calls == 1);
}

TEST_CASE("App handler iteration snapshots re-entrant registrations") {
    affineui::App app;
    affineui::Event resize{};
    resize.type = affineui::EventType::Resize;

    int original_calls = 0;
    int late_calls = 0;
    app.on_event([&](const affineui::Event&,
                     const std::vector<affineui::Document::HoverInfo>&) {
        ++original_calls;
        app.on_event([&](const affineui::Event&,
                         const std::vector<affineui::Document::HoverInfo>&) {
            ++late_calls;
            return false;
        });
        return false;
    });

    app.dispatch(resize);
    CHECK(original_calls == 1);
    CHECK(late_calls == 0);

    app.dispatch(resize);
    CHECK(original_calls == 2);
    CHECK(late_calls == 1);
}

TEST_CASE("App rebuild recovers after a throwing retained-view builder") {
    affineui::App app;
    bool fail = false;
    std::string element_id = "initial";
    app.set_view([&](affineui::View& view) {
        auto node = view.container({}, "lifecycle-node");
        node.attr("id", element_id);
        if (fail) throw std::runtime_error("expected rebuild failure");
        view.paragraph("tail", {}, "lifecycle-tail");
    });

    app.document().layout(160, 80);
    REQUIRE(app.document().find_element_rect("#initial").w > 0);

    fail = true;
    element_id = "partial";
    CHECK_THROWS_AS(app.rebuild_view(), std::runtime_error);

    fail = false;
    element_id = "recovered";
    CHECK_NOTHROW(app.rebuild_view());
    app.document().layout(160, 80);
    CHECK(app.document().find_element_rect("#recovered").w > 0);
}

#if !defined(AFFINEUI_STUB_BUILD)

TEST_CASE("StyleStore release invalidates and safely recycles a slot") {
    auto* doc = lxb_html_document_create();
    REQUIRE(doc != nullptr);
    auto* element = lxb_dom_document_create_element(
        lxb_dom_interface_document(doc),
        reinterpret_cast<const lxb_char_t*>("div"), 3, nullptr);
    REQUIRE(element != nullptr);

    affineui::detail::StyleStore store;
    const auto first = store.acquire(element);
    store.state_bits(first) = 0x07;
    store.release(element);
    CHECK_FALSE(store.lookup(element).valid());
    CHECK(store.element_of(first) == nullptr);

    const auto recycled = store.acquire(element);
    CHECK(recycled.index == first.index);
    CHECK(recycled.generation != first.generation);
    CHECK(store.state_bits(recycled) == 0);
    CHECK(store.element_of(first) == nullptr);
    CHECK(store.element_of(recycled) == element);

    auto current = recycled;
    for (int i = 0; i < 1024; ++i) {
        store.release(element);
        const auto next = store.acquire(element);
        CHECK(next.index == first.index);
        CHECK(next.generation != current.generation);
        current = next;
    }
    CHECK(store.size() == 1);

    while (current.generation <
           std::numeric_limits<std::uint16_t>::max()) {
        store.release(element);
        current = store.acquire(element);
    }
    store.release(element);
    const auto after_retirement = store.acquire(element);
    CHECK(after_retirement.index != current.index);
    CHECK(store.element_of(current) == nullptr);
    CHECK(store.size() == 2);

    lxb_html_document_destroy(doc);
}

TEST_CASE("DOM invalidation clears pointer-keyed text and gesture state") {
    auto* doc = lxb_html_document_create();
    REQUIRE(doc != nullptr);
    auto* element = lxb_dom_document_create_element(
        lxb_dom_interface_document(doc),
        reinterpret_cast<const lxb_char_t*>("div"), 3, nullptr);
    REQUIRE(element != nullptr);
    auto* node = lxb_dom_interface_node(element);

    affineui::detail::DocumentImpl impl;
    impl.text_layout_signatures[node] = 42;
    impl.splitter_drag.block_idx = 1;
    impl.splitter_drag.prev = element;
    impl.splitter_drag.next = element;
    impl.float_drag.elem = element;
    impl.float_resize.elem = element;
    impl.tab_drag.tab = element;
    impl.tab_drag.pane = element;
    impl.tab_drag_ghost = element;
    impl.tab_drag_ghost_block_idx = 2;
    impl.pressed_dcs_menu_item = element;
    impl.pressed_button = element;
    impl.colorfield_drag.kind =
        affineui::detail::DocumentImpl::ColorfieldDrag::Kind::Chip;
    impl.colorfield_drag.field = element;
    impl.colorfield_drag.part = element;
    impl.tree_drag.tree = element;
    impl.tree_drag.row = element;
    impl.tree_drag.target = element;
    impl.tree_drag.select_box = element;
    impl.tree_drag.select_row = element;
    impl.live_drag.kind = affineui::LiveControlKind::RangeInput;
    impl.live_drag.elem = element;
    const auto style_id = impl.style_store.acquire(element);

    affineui::detail::invalidate_dom_subtree_on_destroy(impl, node);

    CHECK(impl.text_layout_signatures.empty());
    CHECK(impl.splitter_drag.block_idx == -1);
    CHECK(impl.float_drag.elem == nullptr);
    CHECK(impl.float_resize.elem == nullptr);
    CHECK(impl.tab_drag.tab == nullptr);
    CHECK(impl.tab_drag.pane == nullptr);
    CHECK(impl.tab_drag_ghost == nullptr);
    CHECK(impl.tab_drag_ghost_block_idx == -1);
    CHECK(impl.pressed_dcs_menu_item == nullptr);
    CHECK(impl.pressed_button == nullptr);
    CHECK(impl.colorfield_drag.kind ==
          affineui::detail::DocumentImpl::ColorfieldDrag::Kind::None);
    CHECK(impl.tree_drag.row == nullptr);
    CHECK(impl.live_drag.kind == affineui::LiveControlKind::None);
    CHECK(impl.style_store.element_of(style_id) == nullptr);

    lxb_html_document_destroy(doc);
}

#endif
