// The platform-neutral menu model: accelerator parsing, and the close-request
// intent the menu's Quit routes through (affineui#60 / #61).

#include <doctest/doctest.h>

#include "affineui/app.h"
#include "affineui/menu.h"
#include "affineui/view.h"

using affineui::Accelerator;
using affineui::MenuItem;
using affineui::MenuRole;
using affineui::MenuItemType;
using affineui::parse_accelerator;

TEST_CASE("accelerator: CmdOrCtrl resolves to the platform's command key") {
    const Accelerator a = parse_accelerator("CmdOrCtrl+S");
    CHECK(a.valid());
    CHECK(a.key == "S");
#if defined(__APPLE__)
    // The whole point of the token: an app writes the shortcut once and it is
    // Command here, Control everywhere else.
    CHECK(a.super);
    CHECK_FALSE(a.ctrl);
#else
    CHECK(a.ctrl);
    CHECK_FALSE(a.super);
#endif
}

TEST_CASE("accelerator: modifiers stack, in any order and any case") {
    const Accelerator a = parse_accelerator("shift+ALT+Ctrl+z");
    CHECK(a.shift);
    CHECK(a.alt);
    CHECK(a.ctrl);
    CHECK(a.key == "Z");  // letters normalize to upper case
}

TEST_CASE("accelerator: aliases") {
    CHECK(parse_accelerator("Command+A").super);
    CHECK(parse_accelerator("Meta+A").super);
    CHECK(parse_accelerator("Option+A").alt);
    CHECK(parse_accelerator("Control+A").ctrl);
}

TEST_CASE("accelerator: named and punctuation keys survive") {
    CHECK(parse_accelerator("F5").key == "F5");
    CHECK(parse_accelerator("CmdOrCtrl+Enter").key == "Enter");
    CHECK(parse_accelerator("Shift+Delete").key == "Delete");
    // A trailing '+' is the KEY (zoom-in), not a dangling separator.
    CHECK(parse_accelerator("CmdOrCtrl++").key == "+");
}

TEST_CASE("accelerator: empty / modifier-only is not valid") {
    CHECK_FALSE(parse_accelerator("").valid());
    CHECK_FALSE(parse_accelerator("CmdOrCtrl").valid());
}

TEST_CASE("menu model: builders produce the shapes the shells translate") {
    CHECK(MenuItem::separator().type == MenuItemType::Separator);

    const MenuItem quit = MenuItem::role(MenuRole::Quit);
    CHECK(quit.item_role == MenuRole::Quit);
    // A role carries no label: the platform supplies it (with the app's name).
    CHECK(quit.label.empty());

    const MenuItem checked = MenuItem::check("Compact", true);
    CHECK(checked.type == MenuItemType::Checkbox);
    CHECK(checked.checked);

    const MenuItem sub = MenuItem::sub("File", {MenuItem::item("New")});
    CHECK(sub.submenu.size() == 1);

    // The standard Edit group is what wires Cut/Copy/Paste to the focused
    // control without the app restating them per platform.
    const auto edit = MenuItem::edit_menu();
    CHECK(edit.size() == 7);
    CHECK(edit.front().item_role == MenuRole::Undo);
}

TEST_CASE("menu model: a custom-drawn row's data survives as data") {
    // The three custom-drawn rows the demos use are checked items, submenus,
    // and accent swatches. None of them needs a custom view: each is data the
    // native menu can render itself.
    MenuItem accent = MenuItem::check("cyan", true);
    accent.swatch   = affineui::Color{0x00, 0xb8, 0xd4, 0xff};
    CHECK(accent.swatch.a != 0);  // a != 0 is what makes the chip render
    CHECK(accent.checked);
}

TEST_CASE("App::set_menu round-trips the model") {
    affineui::App::Config cfg;
    affineui::App app{cfg};
    app.set_menu({MenuItem::sub("File", {MenuItem::item("New", "CmdOrCtrl+N")})});
    REQUIRE(app.menu().size() == 1);
    CHECK(app.menu()[0].label == "File");
    CHECK(app.menu()[0].submenu[0].accelerator == "CmdOrCtrl+N");
}

namespace {
// How many .aui-menubar--app elements the fragment carries. Only the
// APPLICATION menubar gets the class; a menubar nested elsewhere is contextual.
std::size_t app_menubar_count(const std::string& html) {
    std::size_t n = 0;
    for (std::size_t at = html.find(affineui::kAppMenubarClass);
         at != std::string::npos;
         at = html.find(affineui::kAppMenubarClass, at + 1)) {
        ++n;
    }
    return n;
}
}  // namespace

TEST_CASE("only the application menubar is marked for the system bar") {
    // The mark is what the stylesheet keys on to stand the drawn triggers down.
    // It goes on the FIRST menubar a build declares — not "the one at depth 2":
    // apps wrap their shell in a root container, so the app menubar is usually
    // deeper, and a contextual strip can sit at the very same depth.
    affineui::View v{affineui::ViewTheme::Decius};
    v.begin();
    {
        auto shell = v.container("app", "app");
        {
            auto bar = v.menu_bar("menubar");  // the application menubar
            v.menu_button("File", [](affineui::View& m) { m.menu_item("New"); },
                          "mb-file");
        }
        {
            auto panel = v.container("viewport", "vp");
            {
                // A viewport's own View/Add strip: contextual, must keep drawing
                // on every platform, and at the same depth as the app's bar.
                auto strip = v.menu_bar("vp-menubar");
                v.menu_button("Add", [](affineui::View& m) { m.menu_item("Cube"); },
                              "vp-add");
            }
        }
    }
    v.end();
    const std::string html = v.to_html_fragment();

    CHECK(app_menubar_count(html) == 1);   // exactly one, and it is the first
    // Both bars still emit their triggers. Nothing is decided at build time, so
    // the same DOM is correct whether or not the menus went native — hiding is a
    // restyle, and that is what lets set_menu() run before OR after the view is
    // built. (A load_view() app builds its DOM exactly once; anything frozen in
    // at build time would be frozen at the wrong answer.)
    CHECK(html.find("mb-file") != std::string::npos);
    CHECK(html.find("vp-add") != std::string::npos);
}
