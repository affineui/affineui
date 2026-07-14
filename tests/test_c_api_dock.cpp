// The C ABI docking surface (issue #119).
//
// Rust and C# reach AffineUI through affineui_c, and until now that ABI had no
// docking at all — 52 view functions, zero dock functions — so a dockable app
// simply could not be built in either language. These tests drive the docking
// surface the way a binding does: through the C functions only, no C++ types.
//
// They assert the emitted DOM, because that is the thing a user sees. A test
// that only checked "the call didn't crash" would have passed on an ABI that
// silently dropped every panel.

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "affineui/c_api.h"
#include "affineui/c_api_app.h"

namespace {

// Own the char* the ABI hands back (heap copies, freed with affineui_string_free).
struct CStr {
    char* p{nullptr};
    explicit CStr(char* s) : p(s) {}
    ~CStr() { affineui_string_free(p); }
    CStr(const CStr&)            = delete;
    CStr& operator=(const CStr&) = delete;
    [[nodiscard]] std::string str() const { return p ? std::string(p) : std::string{}; }
};

std::string html_of(affineui_view* v) {
    CStr s{affineui_view_to_html_fragment(v)};
    return s.str();
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Panel content builders. `user` carries a counter so we can prove the build
// callbacks actually ran (a dropped panel would leave it at 0).
struct Calls {
    int document{0};
    int left{0};
    int right{0};
    int toolbar{0};
};

void build_document(void* user, affineui_view* v) {
    static_cast<Calls*>(user)->document++;
    affineui_view_heading(v, 1, "Scene", nullptr, nullptr);
}
void build_left(void* user, affineui_view* v) {
    static_cast<Calls*>(user)->left++;
    affineui_view_heading(v, 2, "Outliner body", nullptr, nullptr);
}
void build_right(void* user, affineui_view* v) {
    static_cast<Calls*>(user)->right++;
    affineui_view_heading(v, 2, "Inspector body", nullptr, nullptr);
}
void build_toolbar(void* user, affineui_view* v) {
    static_cast<Calls*>(user)->toolbar++;
    affineui_view_icon_button(v, "cube", "tb-cube");
}

}  // namespace

TEST_CASE("c_api: a full docking view can be built through the C ABI") {
    affineui_view* v = affineui_view_create(AFFINEUI_THEME_DECIUS);
    REQUIRE(v != nullptr);

    Calls calls;
    std::string doc_id, left_id, right_id;

    affineui_view_begin(v);

    struct Ctx {
        Calls*       calls;
        std::string* doc_id;
        std::string* left_id;
        std::string* right_id;
    } ctx{&calls, &doc_id, &left_id, &right_id};

    affineui_view_document_view(
        v, "workspace",
        [](void* user, affineui_view* view) {
            auto* c = static_cast<Ctx*>(user);

            // The center pane.
            CStr d{affineui_view_document(view, build_document, c->calls, nullptr, "Scene", "cube")};
            *c->doc_id = d.str();

            // A left panel, sized.
            affineui_dock_location where;
            affineui_dock_location_docked(&where, AFFINEUI_DOCK_LEFT, 280);
            CStr l{affineui_view_dockpanel(view, "Outliner", &where, build_left,
                                           c->calls, nullptr, "list", "outliner")};
            *c->left_id = l.str();

            // A right panel.
            affineui_dock_location w2;
            affineui_dock_location_docked(&w2, AFFINEUI_DOCK_RIGHT, 320);
            CStr r{affineui_view_dockpanel(view, "Inspector", &w2, build_right,
                                           c->calls, nullptr, "sliders", "inspector")};
            *c->right_id = r.str();
        },
        &ctx);

    affineui_view_end(v);

    // Every declared pane got an id back — that id is what a binding uses as
    // another panel's parent and as the providers' pane_id.
    CHECK_FALSE(doc_id.empty());
    CHECK_FALSE(left_id.empty());
    CHECK_FALSE(right_id.empty());

    // Every content callback ran exactly once. A silently-dropped panel (the
    // failure mode an ABI bug would produce) leaves these at 0.
    CHECK(calls.document == 1);
    CHECK(calls.left == 1);
    CHECK(calls.right == 1);

    const std::string html = html_of(v);

    // The dock DOM was emitted, with the panels' titles and bodies in it.
    CHECK(has(html, "dcs-dock"));
    CHECK(has(html, "Outliner"));
    CHECK(has(html, "Inspector"));
    CHECK(has(html, "Outliner body"));
    CHECK(has(html, "Inspector body"));
    CHECK(has(html, "Scene"));

    affineui_view_destroy(v);
}

TEST_CASE("c_api: a NULL dock_location means 'docked, defaults'") {
    // NULL is the documented shorthand for the defaults and must not crash — the
    // binding's "caller didn't specify a location" path.
    affineui_view* v = affineui_view_create(AFFINEUI_THEME_DECIUS);
    affineui_view_begin(v);
    affineui_view_document_view(
        v, "ws",
        [](void*, affineui_view* view) {
            affineui_view_document(view, nullptr, nullptr, nullptr, "Doc", nullptr);
            CStr id{affineui_view_dockpanel(view, "Panel", /*where=*/nullptr,
                                            /*content=*/nullptr, /*user=*/nullptr,
                                            /*user_free=*/nullptr, /*icon=*/nullptr, "p")};
            CHECK_FALSE(id.str().empty());
        },
        nullptr);
    affineui_view_end(v);
    CHECK(has(html_of(v), "Panel"));
    affineui_view_destroy(v);
}

TEST_CASE("c_api: dock_location factories + setters, and nesting by parent id") {
    // The four factories mirror the C++/Python ones. Also proves a panel can
    // parent to ANOTHER panel by the id dockpanel() returned — that id round-trip
    // is the whole reason document()/dockpanel() hand one back.
    affineui_view* v = affineui_view_create(AFFINEUI_THEME_DECIUS);

    affineui_view_begin(v);
    affineui_view_document_view(
        v, "ws",
        [](void*, affineui_view* view) {
            affineui_view_document(view, nullptr, nullptr, nullptr, "Doc", nullptr);

            // Docked, size set on the struct rather than via the factory arg.
            affineui_dock_location left;
            affineui_dock_location_docked(&left, AFFINEUI_DOCK_LEFT, 0);
            left.has_size = 1;
            left.size     = 300;
            CStr outliner{affineui_view_dockpanel(view, "Outliner", &left, nullptr,
                                           nullptr, nullptr, nullptr, "outliner")};
            REQUIRE_FALSE(outliner.str().empty());

            // A second panel TABBED INTO the first, addressed by its returned id.
            affineui_dock_location tabbed;
            affineui_dock_location_tab(&tabbed);
            tabbed.parent = outliner.p;
            CStr layers{affineui_view_dockpanel(view, "Layers", &tabbed, nullptr,
                                           nullptr, nullptr, nullptr, "layers")};
            CHECK_FALSE(layers.str().empty());

            // A floating panel.
            affineui_dock_location fl;
            affineui_dock_location_floating(&fl, AFFINEUI_DOCK_CORNER_TOP_RIGHT,
                                            40, 60, 320, 240);
            CStr tools{affineui_view_dockpanel(view, "Tools", &fl, nullptr,
                                           nullptr, nullptr, nullptr, "tools")};
            CHECK_FALSE(tools.str().empty());

            // A torn-off panel that drags with another.
            affineui_dock_location to;
            affineui_dock_location_tearoff(&to, AFFINEUI_DOCK_CORNER_BOTTOM_LEFT,
                                           10, 10, 200, 160);
            to.drag_with = tools.p;
            CStr notes{affineui_view_dockpanel(view, "Notes", &to, nullptr,
                                           nullptr, nullptr, nullptr, "notes")};
            CHECK_FALSE(notes.str().empty());
        },
        nullptr);
    affineui_view_end(v);

    const std::string html = html_of(v);
    CHECK(has(html, "Outliner"));
    CHECK(has(html, "Layers"));   // tabbed into Outliner
    CHECK(has(html, "Tools"));    // floating
    CHECK(has(html, "Notes"));    // tearoff
    CHECK(has(html, "300px"));    // the setter took effect

    affineui_view_destroy(v);
}

TEST_CASE("c_api: a dock pane's tab toolbar is declared by pane id") {
    // DockHandle can't cross a C ABI (it carries a WeakRef), so the binding gets
    // the pane id back and attaches the toolbar with it. Verify that path emits.
    affineui_view* v = affineui_view_create(AFFINEUI_THEME_DECIUS);
    Calls calls;

    affineui_view_begin(v);
    affineui_view_document_view(
        v, "ws",
        [](void* user, affineui_view* view) {
            auto* c = static_cast<Calls*>(user);
            CStr d{affineui_view_document(view, build_document, c, nullptr, "Scene", nullptr)};
            affineui_view_dock_toolbar(view, d.p, build_toolbar, c, nullptr);
        },
        &calls);
    affineui_view_end(v);

    CHECK(calls.toolbar == 1);          // the toolbar builder ran
    CHECK(has(html_of(v), "tb-cube"));  // and its content is in the DOM
    affineui_view_destroy(v);
}

TEST_CASE("c_api: dock providers are invoked, and free their user data once") {
    // The providers are how a saved workspace beats the declared seed. They are
    // (fn, user, user_free) triples: user_free must be called EXACTLY once when
    // the view drops the callback — the same contract as on_click. A leak or a
    // double-free here is a real crash in Rust/C#.
    struct Owner {
        int  size_calls{0};
        int  tab_calls{0};
        int  placement_calls{0};
        int* freed;
    };
    int freed_count = 0;
    auto* owner     = new Owner{0, 0, 0, &freed_count};

    {
        affineui_view* v = affineui_view_create(AFFINEUI_THEME_DECIUS);

        affineui_view_set_dock_size_provider(
            v,
            [](void* u, const char*) -> int {
                static_cast<Owner*>(u)->size_calls++;
                return 400;  // a saved size that should beat the declared 280
            },
            owner,
            [](void* u) {
                auto* o = static_cast<Owner*>(u);
                (*o->freed)++;
                delete o;
            });

        affineui_view_set_dock_active_tab_provider(
            v,
            [](void* u, const char*) -> const char* {
                static_cast<Owner*>(u)->tab_calls++;
                return nullptr;  // "" = primary
            },
            owner, nullptr);  // no user_free: `owner` is owned by the size provider

        affineui_view_set_dock_placement_provider(
            v,
            [](void* u, const char*, affineui_dock_placement* out) {
                static_cast<Owner*>(u)->placement_calls++;
                out->present = 0;  // no override — use the declared location
            },
            owner, nullptr);

        affineui_view_begin(v);
        affineui_view_document_view(
            v, "ws",
            [](void*, affineui_view* view) {
                affineui_view_document(view, nullptr, nullptr, nullptr, "Doc", nullptr);
                affineui_dock_location where;
                affineui_dock_location_docked(&where, AFFINEUI_DOCK_LEFT, 280);
                CStr id{affineui_view_dockpanel(view, "Outliner", &where, nullptr,
                                           nullptr, nullptr, nullptr, "outliner")};
            },
            nullptr);
        affineui_view_end(v);

        // The resolver consulted the providers while emitting the dock tree.
        CHECK(owner->size_calls > 0);
        CHECK(owner->placement_calls > 0);

        // The saved size (400) won over the declared seed (280).
        CHECK(has(html_of(v), "400px"));

        CHECK(freed_count == 0);  // still alive while the view is
        affineui_view_destroy(v);
    }

    // Exactly once — not zero (leak), not twice (double-free).
    CHECK(freed_count == 1);
}
