#include "affineui/c_api_app.h"

#include <doctest/doctest.h>

namespace {

void capture_callback_view(void* user, affineui_view* view) {
    auto** captured = static_cast<affineui_weak_view**>(user);
    *captured = affineui_view_weak_ref(view);
}

}  // namespace

TEST_CASE("C callback Views can be retained as invalidating weak handles") {
    auto* view = affineui_view_create(AFFINEUI_THEME_PLAIN);
    REQUIRE(view != nullptr);

    affineui_weak_view* escaped = nullptr;
    affineui_view_begin(view);
    auto* panel = affineui_view_panel(view, "lifetime-panel", capture_callback_view, &escaped);
    affineui_view_end(view);

    REQUIRE(escaped != nullptr);
    CHECK(affineui_weak_view_get(escaped) == view);

    affineui_widget_destroy(panel);
    affineui_view_destroy(view);

    // The wrapper still exists, but it no longer exposes the destroyed C++
    // object. Passing the resolved null through the regular API is inert.
    CHECK(affineui_weak_view_get(escaped) == nullptr);
    affineui_view_clear(affineui_weak_view_get(escaped));
    char* html = affineui_view_to_html_fragment(affineui_weak_view_get(escaped));
    REQUIRE(html != nullptr);
    CHECK(html[0] == '\0');
    affineui_string_free(html);

    affineui_weak_view_destroy(escaped);
}

TEST_CASE("a weak handle created from null stays safely invalid") {
    auto* weak = affineui_view_weak_ref(nullptr);
    REQUIRE(weak != nullptr);
    CHECK(affineui_weak_view_get(weak) == nullptr);
    affineui_weak_view_destroy(weak);
    affineui_weak_view_destroy(nullptr);
}
