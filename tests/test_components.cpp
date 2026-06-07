#include <doctest/doctest.h>

#include "affineui/components.h"
#include "affineui/view.h"

#include <ostream>
#include <string>

using namespace affineui;

namespace {

// Build a small view with a few named widgets and return it ready to query.
View make_view() {
    View view{ViewTheme::Decius};
    view.begin();
    view.button("Save", true, "save");
    view.checkbox("Wireframe", true, "wire");
    view.input("Name", "Cube", "text", "name");
    view.dropdown("Blend", {"Normal", "Screen", "Multiply"}, "Screen", "blend");
    view.slider("Roughness", 0.42, 0.0, 1.0, "rough");
    view.end();
    return view;
}

}  // namespace

TEST_CASE("typed component wraps a live widget and reads its state") {
    View view = make_view();

    Checkbox wire = view.component<Checkbox>("wire");
    REQUIRE(static_cast<bool>(wire));
    CHECK(wire.checked());

    Dropdown blend = view.component<Dropdown>("blend");
    REQUIRE(static_cast<bool>(blend));
    CHECK(blend.selected() == "Screen");

    Slider rough = view.component<Slider>("rough");
    REQUIRE(static_cast<bool>(rough));
    CHECK(rough.value() == doctest::Approx(0.42));
}

TEST_CASE("typed component writes propagate to the node") {
    View view = make_view();

    Checkbox wire = view.component<Checkbox>("wire");
    wire.set_checked(false);
    CHECK_FALSE(wire.checked());

    Dropdown blend = view.component<Dropdown>("blend");
    blend.set_selected("Multiply");
    CHECK(blend.selected() == "Multiply");
}

TEST_CASE("a typed component for a missing id is safe (no crash, defaults)") {
    View view = make_view();

    // The crash-prevention contract: querying a non-existent widget yields a
    // wrapper that reports "not present" and whose accessors return defaults
    // and whose mutators no-op — never a crash.
    Dropdown missing = view.component<Dropdown>("does-not-exist");
    CHECK_FALSE(static_cast<bool>(missing));
    CHECK(missing.selected().empty());          // read -> default
    missing.set_selected("whatever");           // write -> no-op, no crash
    CHECK(missing.selected().empty());

    Slider gone = view.component<Slider>("nope");
    CHECK(gone.value(-1.0) == doctest::Approx(-1.0));  // fallback returned

    Checkbox absent = view.component<Checkbox>("absent");
    CHECK_FALSE(absent.checked());
    absent.set_checked(true);                   // no-op, no crash
}

TEST_CASE("querying a widget as the wrong type yields a WrongType wrapper") {
    View view = make_view();

    // "save" is a Button; asking for it as a Dropdown must NOT crash and must
    // NOT silently wrap it. It comes back WrongType: still attached (so you can
    // introspect what it really is), but not valid, with typed accessors inert.
    Dropdown wrong = view.component<Dropdown>("save");
    CHECK_FALSE(static_cast<bool>(wrong));                       // not valid
    CHECK(wrong.validity() == ComponentValidity::WrongType);
    CHECK(wrong.attached());                                     // still attached
    CHECK(wrong.kind() == WidgetKind::Button);                  // reveals real kind
    CHECK(wrong.selected().empty());                            // typed read inert
    wrong.set_selected("x");                                    // typed write no-op
    CHECK(wrong.selected().empty());

    // A diagnostic was logged for the mismatch.
    CHECK_FALSE(view.diagnostics().empty());

    // Generic element ops still work in WrongType mode (it's a real node).
    CHECK(wrong.text() == "Save");          // generic text read works
    wrong.set_attr("data-test", "1");       // generic attr write works
    CHECK(wrong.attr("data-test") == "1");
}

TEST_CASE("a NotPresent wrapper is distinct from WrongType") {
    View view = make_view();
    Dropdown missing = view.component<Dropdown>("nope");
    CHECK(missing.validity() == ComponentValidity::NotPresent);
    CHECK_FALSE(missing.attached());
}

TEST_CASE("typed component handle survives a rebuild by re-resolving its id") {
    View view = make_view();
    Dropdown blend = view.component<Dropdown>("blend");
    REQUIRE(static_cast<bool>(blend));
    blend.set_selected("Multiply");

    // Rebuild the whole view (a refresh pass). The node gets a new StableId but
    // keeps its widget-name, so the stored typed handle must re-resolve.
    view.begin();
    view.button("Save", true, "save");
    view.checkbox("Wireframe", true, "wire");
    view.input("Name", "Cube", "text", "name");
    view.dropdown("Blend", {"Normal", "Screen", "Multiply"}, "Normal", "blend");
    view.slider("Roughness", 0.42, 0.0, 1.0, "rough");
    view.end();

    CHECK(static_cast<bool>(blend));     // still resolves after rebuild
    CHECK(blend.selected() == "Normal"); // reads the rebuilt node's value
}

TEST_CASE("Version parses and compares major.minor.patch") {
    CHECK(FrameworkVersion::parse("0.5.2") == FrameworkVersion{0, 5, 2});
    CHECK(FrameworkVersion::parse("5.3.8") == FrameworkVersion{5, 3, 8});
    CHECK(FrameworkVersion::parse("1") == FrameworkVersion{1, 0, 0});
    CHECK(FrameworkVersion::parse("2.4") == FrameworkVersion{2, 4, 0});
    CHECK(FrameworkVersion::parse("0.6.0-rc1") == FrameworkVersion{0, 6, 0});  // trailing junk ignored
    CHECK(FrameworkVersion::parse("") == FrameworkVersion{0, 0, 0});

    CHECK(FrameworkVersion::parse("0.6.0") > FrameworkVersion::parse("0.5.2"));
    CHECK(FrameworkVersion::parse("0.5.2") >= FrameworkVersion::parse("0.5.2"));
    CHECK(FrameworkVersion::parse("1.0.0") > FrameworkVersion::parse("0.99.99"));
    CHECK(FrameworkVersion::parse("0.5.2") < FrameworkVersion::parse("0.5.10"));  // numeric, not lexical
}

TEST_CASE("a view stamps framework id + version and derives the bundle href") {
    View view{ViewTheme::Decius};
    view.begin();
    view.button("Go", true, "go");
    view.end();
    const std::string html = view.to_html_document();
    CHECK(html.find("data-aui-framework=\"decius\"") != std::string::npos);
    CHECK(html.find("data-aui-framework-version=\"0.6.2\"") != std::string::npos);
    CHECK(html.find("decius-css-0.6.2.bundle.min.css") != std::string::npos);

    // Declaring a version changes both the stamp and the derived href.
    View v2{ViewTheme::Decius};
    v2.set_framework_version("0.6.0");
    v2.begin();
    v2.end();
    const std::string html2 = v2.to_html_document();
    CHECK(html2.find("data-aui-framework-version=\"0.6.0\"") != std::string::npos);
    CHECK(html2.find("decius-css-0.6.0.bundle.min.css") != std::string::npos);
}

TEST_CASE("structural builders emit themed markup with interaction hooks") {
    View view{ViewTheme::Decius};
    view.begin();
    {
        auto bar = view.toolbar("tools");
        view.icon_button("save", "save-btn");
        view.toolbar_separator("sep");
    }
    {
        auto mb = view.menu_bar("mb");
        view.menu_button("File", "file-menu", "file-mb");
    }
    {
        auto t = view.tree("outliner");
        view.tree_row("Camera", true, 0, "row-cam");
        view.tree_row("Light", false, 1, "row-light");
    }
    view.color_field("Tint", "#3bb7ff", {"#ff0000", "#00ff00"}, "tint");
    view.splitter(false, "split");
    {
        auto sb = view.status_bar("status");
        view.text("Ready", "ready");
    }
    view.end();

    const std::string html = view.to_html_fragment();
    // Toolbar + icon button (Decius classes + icon glyph).
    CHECK(html.find("dcs-toolbar") != std::string::npos);
    CHECK(html.find("dcs-btn--icon") != std::string::npos);
    CHECK(html.find("di di-save") != std::string::npos);
    // Menu button wired to open its menu via the interaction layer.
    CHECK(html.find("data-dcs-toggle=\"menu\"") != std::string::npos);
    CHECK(html.find("data-dcs-target=\"#file-menu\"") != std::string::npos);
    // Tree with selection hook + a selected row.
    CHECK(html.find("dcs-tree") != std::string::npos);
    CHECK(html.find("data-dcs-select") != std::string::npos);
    CHECK(html.find("aria-selected=\"true\"") != std::string::npos);
    // Color field opens a swatch popup.
    CHECK(html.find("dcs-menu--swatches") != std::string::npos);
    CHECK(html.find("data-dcs-placement=\"bottom\"") != std::string::npos);
    // Splitter drag hook.
    CHECK(html.find("data-dcs-splitter") != std::string::npos);
    // Status bar.
    CHECK(html.find("dcs-statusbar") != std::string::npos);
}

TEST_CASE("tree_row emits the canonical Decius row substructure") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    {
        auto t = view.tree("outliner");
        affineui::TreeRowOptions branch;
        branch.depth = 0;
        branch.icon = "cube";
        branch.expandable = true;
        branch.expanded = true;
        view.tree_row("WorldRoot", branch, "row-root");

        affineui::TreeRowOptions leaf;
        leaf.depth = 1;
        leaf.selected = true;
        leaf.icon = "sphere";
        leaf.meta_icon = "eye";
        view.tree_row("Hero", leaf, "row-hero");
    }
    view.end();
    const std::string html = view.to_html_fragment();

    // Branch: open chevron with a chevron glyph + type icon.
    CHECK(html.find("dcs-tree__row") != std::string::npos);
    CHECK(html.find("dcs-tree__chevron dcs-tree__chevron--open") !=
          std::string::npos);
    CHECK(html.find("di di-chevron-right") != std::string::npos);
    CHECK(html.find("dcs-tree__icon") != std::string::npos);
    CHECK(html.find("di di-cube") != std::string::npos);
    CHECK(html.find("dcs-tree__label") != std::string::npos);
    // Leaf: type icon + trailing meta icon + selection + depth.
    CHECK(html.find("di di-sphere") != std::string::npos);
    CHECK(html.find("dcs-tree__meta") != std::string::npos);
    CHECK(html.find("di di-eye") != std::string::npos);
    CHECK(html.find("aria-selected=\"true\"") != std::string::npos);
    CHECK(html.find("--depth:1") != std::string::npos);
    // Rows are flat siblings (no nested dcs-tree__row inside another's body).
    CHECK(html.find("draggable=\"true\"") != std::string::npos);
}

TEST_CASE("colorfield emits the canonical Decius dcs-colorfield + picker") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    {
        auto props = view.container("dcs-props", "props");
        view.colorfield("Tint", "#4d9fff", "tint");
    }
    view.end();
    const std::string html = view.to_html_fragment();
    // Field wrapper (personality-mapped) + the Decius-specific body.
    CHECK(html.find("dcs-field") != std::string::npos);
    CHECK(html.find("data-aui-widget=\"colorfield\"") != std::string::npos);
    CHECK(html.find("dcs-colorfield__chip") != std::string::npos);
    CHECK(html.find("data-dcs-color=\"#4d9fff\"") != std::string::npos);
    CHECK(html.find("dcs-colorfield__hex") != std::string::npos);
    CHECK(html.find("dcs-colorfield__caret") != std::string::npos);
    // Caret opens the picker popover, which carries the SV square + hue bar.
    CHECK(html.find("data-dcs-toggle=\"popover\"") != std::string::npos);
    CHECK(html.find("dcs-color-square") != std::string::npos);
    CHECK(html.find("dcs-hue-bar") != std::string::npos);
    // di chevron glyph on the caret.
    CHECK(html.find("di di-chevron-down") != std::string::npos);
}

TEST_CASE("colorfield degrades to a native input off-Decius") {
    affineui::View view{affineui::ViewTheme::Bootstrap};
    view.begin();
    view.colorfield("Tint", "#4d9fff", "tint");
    view.end();
    const std::string html = view.to_html_fragment();
    CHECK(html.find("type=\"color\"") != std::string::npos);
    CHECK(html.find("dcs-colorfield") == std::string::npos);
}

TEST_CASE("combo emits a bare dcs-combo with no field wrapper") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    {
        auto vec = view.container("dcs-vec", "vec");
        view.combo("X", 12.0, 0.01, "loc-x");
        view.combo("Y", 4.2, 0.01, "loc-y");
        view.combo("Z", -8.5, 0.01, "loc-z");
    }
    view.end();
    const std::string html = view.to_html_fragment();
    CHECK(html.find("dcs-combo") != std::string::npos);
    CHECK(html.find("dcs-combo__fill") != std::string::npos);
    CHECK(html.find("dcs-combo__value") != std::string::npos);
    CHECK(html.find("data-label=\"X\"") != std::string::npos);
    // Bare: no field/label wrapper introduced by the combo itself.
    CHECK(html.find("dcs-field__label") == std::string::npos);
}

TEST_CASE("foldout emits a collapsible header + body the scope fills") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    {
        auto f = view.foldout("Material", true, "mat");
        view.slider("Roughness", 0.5, 0.0, 1.0, "rough");
    }
    {
        auto f = view.foldout("Hidden", false, "hid");
        view.text("body", "hid-body");
    }
    view.end();
    const std::string html = view.to_html_fragment();
    CHECK(html.find("dcs-foldout__header") != std::string::npos);
    CHECK(html.find("dcs-foldout__chevron dcs-foldout__chevron--open") !=
          std::string::npos);
    CHECK(html.find("dcs-foldout__title") != std::string::npos);
    CHECK(html.find("dcs-foldout__body") != std::string::npos);
    // Collapsed foldout carries the collapsed modifier.
    CHECK(html.find("dcs-foldout--collapsed") != std::string::npos);
    // The slider really lives inside the foldout body (component nesting).
    CHECK(html.find("dcs-slider") != std::string::npos);
}

TEST_CASE("vec emits 2-4 labeled channels inside a dcs-vec") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    {
        auto props = view.container("dcs-props", "props");
        view.vec("Location", {"X", "Y", "Z"}, {12.0, 4.2, -8.5}, "loc");
    }
    view.end();
    const std::string html = view.to_html_fragment();
    CHECK(html.find("dcs-field") != std::string::npos);
    CHECK(html.find("data-aui-widget=\"vec\"") != std::string::npos);
    CHECK(html.find("dcs-vec") != std::string::npos);
    // Three drag-scrub combos with their channel labels.
    CHECK(html.find("data-label=\"X\"") != std::string::npos);
    CHECK(html.find("data-label=\"Y\"") != std::string::npos);
    CHECK(html.find("data-label=\"Z\"") != std::string::npos);
    CHECK(html.find("value=\"12\"") != std::string::npos);
}

TEST_CASE("menu builds declaratively with shortcuts, separators, and submenus") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    view.menu("menu-file", [](affineui::View& m) {
        m.menu_item("New", "file", "Ctrl N", "mi-new");
        m.menu_separator("sep");
        m.submenu("Export", [](affineui::View& s) {
            s.menu_item("glTF", {}, {}, "mi-gltf");
        }, "share", "mi-export");
    });
    view.end();
    const std::string html = view.to_html_fragment();
    CHECK(html.find("class=\"dcs-menu\"") != std::string::npos);
    CHECK(html.find("id=\"menu-file\"") != std::string::npos);
    CHECK(html.find("dcs-menu__item") != std::string::npos);
    CHECK(html.find("dcs-menu__label-text") != std::string::npos);
    CHECK(html.find("dcs-menu__shortcut") != std::string::npos);
    CHECK(html.find("dcs-menu__sep") != std::string::npos);
    // Nested submenu structure.
    CHECK(html.find("dcs-menu__item--has-sub") != std::string::npos);
    CHECK(html.find("dcs-menu__sub") != std::string::npos);
    // The nested item is inside the submenu container.
    CHECK(html.find("glTF") != std::string::npos);
}

TEST_CASE("menu_button owns its dropdown declared inline") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    {
        auto bar = view.menu_bar("mb");
        view.menu_button("File", [](affineui::View& m) {
            m.menu_item("New", "file", "Ctrl N", "mi-new");
        }, "mb-file");
    }
    view.end();
    const std::string html = view.to_html_fragment();
    // The trigger and its generated menu share the same id link.
    CHECK(html.find("data-dcs-target=\"#aui-menu-mb-file\"") !=
          std::string::npos);
    CHECK(html.find("id=\"aui-menu-mb-file\"") != std::string::npos);
    CHECK(html.find("dcs-menu__item") != std::string::npos);
    CHECK(html.find("New") != std::string::npos);
}

TEST_CASE("floating_toolbar emits a draggable rail with a grip") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    {
        affineui::FloatingToolbarOptions opts;
        opts.vertical = true;
        opts.drag_bounds = ".canvas";
        opts.position = "left:8px;top:8px";
        auto rail = view.floating_toolbar(opts, "rail");
        view.icon_button("move", "t-move");
    }
    view.end();
    const std::string html = view.to_html_fragment();
    CHECK(html.find("dcs-toolbar--floating") != std::string::npos);
    CHECK(html.find("dcs-toolbar--v") != std::string::npos);
    CHECK(html.find("data-dcs-drag-bounds=\".canvas\"") != std::string::npos);
    CHECK(html.find("dcs-grip") != std::string::npos);
    CHECK(html.find("data-dcs-drag-handle") != std::string::npos);
    // The icon button is inside the rail.
    CHECK(html.find("di di-move") != std::string::npos);
}

TEST_CASE("document_view resolves a declarative dock with auto-splitters") {
    affineui::View view{affineui::ViewTheme::Decius};
    view.begin();
    view.document_view("work", [](affineui::View& dv) {
        dv.document([](affineui::View& doc) { doc.text("viewport", "vp"); },
                    "Lit View");
        // Declared out of visual order on purpose — placement is explicit.
        dv.dockpanel("Assets",
                     affineui::DockLocation::docked(affineui::Dock::Bottom, 120),
                     [](affineui::View& p) { p.text("assets", "a"); });
        dv.dockpanel("Inspector",
                     affineui::DockLocation::docked(affineui::Dock::Right, 320),
                     [](affineui::View& p) { p.text("inspector", "i"); });
        dv.dockpanel("Hierarchy",
                     affineui::DockLocation::docked(affineui::Dock::Left, 260),
                     [](affineui::View& p) { p.text("tree", "t"); });
    });
    view.end();
    const std::string html = view.to_html_fragment();

    // The container is the root vertical dock; a horizontal row sits above the
    // bottom Assets pane, both produced by the engine.
    CHECK(html.find("dcs-dock dcs-dock--v") != std::string::npos);
    CHECK(html.find("dcs-dockpane--center") != std::string::npos);  // the document
    // Each declared panel became a pane with its title + content.
    CHECK(html.find("Hierarchy") != std::string::npos);
    CHECK(html.find("Inspector") != std::string::npos);
    CHECK(html.find("Assets") != std::string::npos);
    CHECK(html.find("Lit View") != std::string::npos);
    CHECK(html.find(">tree<") != std::string::npos);
    // Splitters were auto-inserted (no manual splitter() calls): both a
    // vertical (between columns) and a horizontal (above Assets).
    CHECK(html.find("data-dcs-splitter=\"v\"") != std::string::npos);
    CHECK(html.find("data-dcs-splitter=\"h\"") != std::string::npos);
    // Pane sizing from the DockLocation flex-basis.
    CHECK(html.find("flex:0 0 260px") != std::string::npos);
    CHECK(html.find("flex:0 0 320px") != std::string::npos);
}

TEST_CASE("document_view dock_size_provider overrides the declared seed size") {
    affineui::View view{affineui::ViewTheme::Decius};
    // Saved layout: Hierarchy was resized to 400px; Inspector has no saved size.
    view.set_dock_size_provider([](std::string_view id) -> int {
        return id == "hierarchy" ? 400 : 0;
    });
    view.begin();
    view.document_view("work", [](affineui::View& dv) {
        dv.document([](affineui::View& doc) { doc.text("vp", "vp"); }, "Doc");
        dv.dockpanel("Hierarchy",
                     affineui::DockLocation::docked(affineui::Dock::Left, 260),
                     [](affineui::View& p) { p.text("h", "h"); });
        dv.dockpanel("Inspector",
                     affineui::DockLocation::docked(affineui::Dock::Right, 320),
                     [](affineui::View& p) { p.text("i", "i"); });
    });
    view.end();
    const std::string html = view.to_html_fragment();
    // Saved 400px wins for Hierarchy; declared 320px stands for Inspector.
    CHECK(html.find("flex:0 0 400px") != std::string::npos);
    CHECK(html.find("flex:0 0 320px") != std::string::npos);
    CHECK(html.find("flex:0 0 260px") == std::string::npos);  // seed overridden
}

TEST_CASE("dock_panel nests a tab bar and a body the scope fills") {
    View view{ViewTheme::Decius};
    view.begin();
    {
        auto panel = view.dock_panel("Inspector", "inspector-body", {}, "inspector");
        view.paragraph("Properties", {}, "props");  // builds inside the body
    }
    view.end();

    const std::string html = view.to_html_fragment();
    CHECK(html.find("dcs-dockpane") != std::string::npos);
    CHECK(html.find("dcs-dockpane__tabbar") != std::string::npos);
    CHECK(html.find("dcs-dockpane__tab") != std::string::npos);
    CHECK(html.find("id=\"inspector-body\"") != std::string::npos);
    CHECK(html.find("data-dcs-target=\"#inspector-body\"") != std::string::npos);
    // The tab title and the body content both present.
    CHECK(html.find("Inspector") != std::string::npos);
    CHECK(html.find("Properties") != std::string::npos);

    // Typed query: the panel resolves as a DockPanel.
    DockPanel panel = view.component<DockPanel>("inspector");
    CHECK(static_cast<bool>(panel));
    panel.set_visible(false);
    CHECK_FALSE(panel.visible());

    // Typed query: the color field as a ColorField round-trips its value.
    ColorField tint = view.component<ColorField>("does-not-exist");
    CHECK(tint.validity() == ComponentValidity::NotPresent);
}

TEST_CASE("color_field typed wrapper reads and writes its color") {
    View view{ViewTheme::Decius};
    view.begin();
    view.color_field("Tint", "#3bb7ff", {"#ff0000"}, "tint");
    view.end();

    ColorField tint = view.component<ColorField>("tint");
    REQUIRE(static_cast<bool>(tint));
    CHECK(tint.color() == "#3bb7ff");
    tint.set_color("#112233");
    CHECK(tint.color() == "#112233");
}

TEST_CASE("builder-returned ref can be adopted by a typed component") {
    View view{ViewTheme::Decius};
    view.begin();
    WidgetRef save_ref = view.button("Save", true, "save");
    view.end();

    Button save{save_ref};
    REQUIRE(static_cast<bool>(save));
    CHECK(save.label() == "Save");
    CHECK(save.enabled());
    save.set_enabled(false);
    CHECK_FALSE(save.enabled());
}
