// test_skeuo.cpp — skeuomorphic hardware kit (extras/skeuo).
//
// Pins the geometry contracts the patch-cable painter depends on: a
// plug must anchor to the CENTER OF THE SOCKET artwork, never to the
// socket+label group (the visible "plug hangs below the port" bug).

#include <doctest/doctest.h>

#include "affineui_skeuo.h"

#include <affineui/app.h>
#include <affineui/view.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

std::vector<std::string> skeuo_asset_folders() {
    return {
        std::string(AFFINEUI_TEST_SOURCE_DIR) + "/examples",
        std::string(AFFINEUI_TEST_SOURCE_DIR),
    };
}

}  // namespace

TEST_CASE("PatchBay jack rect resolves to the socket, not the labeled group") {
    namespace sk = affineui::skeuo;
    sk::PatchBay bay;

    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    {
        auto board = bay.board(view, "", "board");
        (void) board;
        bay.jack(view, "cv-out", "CV OUT");
        bay.cables_layer(view);
    }
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = skeuo_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(400, 300);

    // The socket query is what PatchBay::jack_center anchors plugs to.
    // It must be the bare 32x32 socket artwork — no label height folded
    // in (a taller box hangs every plug half a label below its port).
    const auto socket =
        app.document().find_element_rect("[data-skeuo-jack=cv-out]");
    REQUIRE(socket.w > 0);
    REQUIRE(socket.h > 0);
    CHECK(socket.w <= 34);
    CHECK(socket.h <= 34);

    // The jack's NAME resolves to the outer socket+label column (the
    // generous hit/drop target). It must contain the socket and extend
    // below it — the two queries deliberately answer different questions.
    const auto named = app.document().find_element_rect("cv-out");
    REQUIRE(named.w > 0);
    CHECK(named.y <= socket.y);
    CHECK(named.h > socket.h);
    CHECK(socket.x >= named.x);
    CHECK(socket.x + socket.w <= named.x + named.w);
    // Socket is horizontally centered in its column, so plug X == the
    // column's visual center.
    const int socket_cx = socket.x + socket.w / 2;
    const int named_cx = named.x + named.w / 2;
    CHECK(std::abs(socket_cx - named_cx) <= 1);
}

TEST_CASE("PatchBay callbacks may destroy an active drag safely") {
    namespace sk = affineui::skeuo;

    auto build_bay = [](sk::PatchBay& bay, affineui::View& view) {
        view.begin();
        {
            auto board = bay.board(view, "", "board");
            (void) board;
            bay.jack(view, "source", "SOURCE");
            bay.jack(view, "target", "TARGET");
            bay.cables_layer(view);
        }
        view.end();
    };
    auto pointer_event = [](affineui::EventType type,
                            const affineui::Rect& rect) {
        affineui::Event event{};
        event.type = type;
        event.button = affineui::MouseButton::Left;
        event.pos = {rect.x + rect.w / 2, rect.y + rect.h / 2};
        return event;
    };

    SUBCASE("picking up an existing cable does not strand pointer capture") {
        auto bay = std::make_unique<sk::PatchBay>();
        bay->connect("source", "target");

        affineui::View view{affineui::ViewTheme::Decius};
        build_bay(*bay, view);

        affineui::App::Config cfg;
        cfg.asset_folders = skeuo_asset_folders();
        affineui::App app{cfg};
        app.load_view(view);
        app.document().layout(400, 300);
        bay->attach(app);

        int renders = 0;
        bay->request_render = [&] { ++renders; };
        bay->on_change = [&] { bay.reset(); };

        const auto source = app.document().find_element_rect(
            "[data-skeuo-jack=source]");
        REQUIRE(source.w > 0);
        CHECK_NOTHROW(app.dispatch(
            pointer_event(affineui::EventType::MouseDown, source)));

        CHECK(bay == nullptr);
        CHECK_FALSE(app.pointer_captured());
        CHECK(renders == 1);
    }

    SUBCASE("completing a cable makes no access after destructive callback") {
        auto bay = std::make_unique<sk::PatchBay>();

        affineui::View view{affineui::ViewTheme::Decius};
        build_bay(*bay, view);

        affineui::App::Config cfg;
        cfg.asset_folders = skeuo_asset_folders();
        affineui::App app{cfg};
        app.load_view(view);
        app.document().layout(400, 300);
        bay->attach(app);

        int renders = 0;
        bay->request_render = [&] { ++renders; };
        bay->on_change = [&] { bay.reset(); };

        const auto source = app.document().find_element_rect(
            "[data-skeuo-jack=source]");
        const auto target = app.document().find_element_rect("target");
        REQUIRE(source.w > 0);
        REQUIRE(target.w > 0);
        REQUIRE(app.dispatch(
            pointer_event(affineui::EventType::MouseDown, source)));
        REQUIRE(app.pointer_captured());

        CHECK_NOTHROW(app.dispatch(
            pointer_event(affineui::EventType::MouseUp, target)));

        CHECK(bay == nullptr);
        CHECK_FALSE(app.pointer_captured());
        CHECK(renders == 2);
    }
}

TEST_CASE("step_pair renders two stacked chunky buttons, not a sliver") {
    namespace sk = affineui::skeuo;

    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    {
        auto row = view.container("", "row");
        (void) row;
        sk::step_pair(view, "steps-up", "steps-down");
    }
    view.end();

    affineui::App::Config cfg;
    cfg.asset_folders = skeuo_asset_folders();
    affineui::App app{cfg};
    app.load_view(view);
    app.document().layout(400, 300);

    const auto up = app.document().find_element_rect("steps-up");
    const auto down = app.document().find_element_rect("steps-down");
    REQUIRE(up.w > 0);
    REQUIRE(down.w > 0);
    // Each button is a 22x15 chunky plastic cap (inline-sized).
    CHECK(up.h >= 14);
    CHECK(down.h >= 14);
    CHECK(up.w >= 20);
    // Stacked vertically: down starts where up ends (1px border seam).
    CHECK(down.y >= up.y + up.h - 2);
    CHECK(std::abs(down.x - up.x) <= 1);
}
