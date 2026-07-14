#include "dender_view.h"

#include "dender_components.h"
#include "dender_scene.h"
#include "dender_stats.h"
#include "dender_styles.h"

#include <array>
#include <string>
#include <vector>

namespace dender {

using affineui::bind;
using affineui::Dock;
using affineui::DockLocation;
using affineui::TreeRowOptions;
using affineui::View;

namespace {

// Lowercased, dash-separated widget key from a display name ("UV Sphere" ->
// "uv-sphere").
std::string slug(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c >= 'A' && c <= 'Z') out.push_back(static_cast<char>(c - 'A' + 'a'));
        else if (c == ' ') out.push_back('-');
        else out.push_back(c);
    }
    return out;
}

// A statusbar item with a bold value ("Verts **8**").
void stat_item(View& v, std::string_view label, std::string_view value,
               std::string_view key) {
    auto item = v.element("span", "dcs-statusbar__item", key);
    v.text(std::string(label) + " ", std::string(key) + "-label");
    v.element("b", {}, std::string(key) + "-value").text(value);
}

// A labeled drag-scrub combo row (dcs-field + bare combo) for panel sheets.
affineui::WidgetRef combo_field(View& v, std::string_view label, double value,
                                double step, std::string_view key,
                                std::string_view suffix = {}) {
    auto field = v.container("dcs-field", key);
    ui::text_span(v, label, "dcs-field__label", std::string(key) + "-label");
    auto combo = v.combo({}, value, step, std::string(key) + "-combo");
    if (!suffix.empty()) combo.attr("data-suffix", suffix);
    return combo;
}

}  // namespace

DenderView::DenderView(DenderApp& app)
    : app_(app), doc_(app.ctx().document()), ui_(app.ui()) {}

View DenderView::build() const {
    View v{affineui::ViewTheme::Decius};
    v.begin();
    build_into(v);
    v.end();
    return v;
}

void DenderView::build_into(View& v) const {
    v.set_theme(affineui::ViewTheme::Decius);
    v.set_framework_version(kDeciusVersion);
    v.selector(affineui::decius::selector::style,
               app_.style() == "3d" ? affineui::decius::style::three_d
                                     : affineui::decius::style::flat);
    v.selector(affineui::decius::selector::density, app_.density());
    v.selector(affineui::decius::selector::accent, app_.accent());

    // Saved sizes / live arrangement win over the declared dock seed
    // (game_editor pattern: the declared layout only seeds the first run).
    DenderApp* app = &app_;
    v.set_dock_size_provider(
        [app](std::string_view id) { return app->workspace_panel_size(id); });
    v.set_dock_layout_provider([app] { return app->dock_layout(); });
    v.set_dock_placement_provider(
        [app](std::string_view id) { return app->dock_override(id); });
    v.set_dock_active_tab_provider(
        [app](std::string_view id) { return app->dock_active_tab(id); });

    auto shell = v.container("dn-app", "dender");
    build_topbar(v);
    build_workarea(v);
    build_statusbar(v);
    build_shared_menus(v);
    build_tweaks_popover(v);
}

// ── Topbar ──────────────────────────────────────────────────────────────────

void DenderView::build_topbar(View& v) const {
    auto bar = v.container("dcs-toolbar dn-topbar", "topbar");

    {
        auto logo = v.container("dn-logo", "logo");
        logo.attr("data-dcs-tip", "DENDER \xE2\x80\x94 splash & about");
        ui::icon(v, "decius", "dn-logo__mark", "logo-mark");
        ui::text_span(v, "DENDER", "dn-logo__name", "logo-name");
    }

    {
        auto menubar = v.menu_bar("main-menubar");

        v.menu_button("File", [&](View& m) {
            m.menu_item("New", "image", "Ctrl N", "mi-file-new");
            m.menu_item("Open\xE2\x80\xA6", "folder", "Ctrl O", "mi-file-open");
            ui::submenu_stub(m, "Open Recent", {}, "mi-file-recent");
            m.menu_separator("mi-file-sep1");
            m.menu_item("Save", {}, "Ctrl S", "mi-file-save");
            m.menu_item("Save As\xE2\x80\xA6", {}, "Ctrl \xE2\x87\xA7 S",
                        "mi-file-saveas");
            m.menu_separator("mi-file-sep2");
            ui::submenu_stub(m, "Import", {}, "mi-file-import");
            ui::submenu_stub(m, "Export", {}, "mi-file-export");
            m.menu_separator("mi-file-sep3");
            m.menu_item("Quit", {}, "Ctrl Q", "mi-file-quit")
                .on_click(bind(&app_, &DenderApp::quit));
        }, "mb-file");

        v.menu_button("Edit", [&](View& m) {
            m.menu_item("Undo", {}, "Ctrl Z", "mi-edit-undo")
                .on_click(bind(&app_, &DenderApp::undo));
            m.menu_item("Redo", {}, "Ctrl \xE2\x87\xA7 Z", "mi-edit-redo")
                .on_click(bind(&app_, &DenderApp::redo));
            m.menu_item("Undo History", {}, {}, "mi-edit-history");
            m.menu_separator("mi-edit-sep1");
            m.menu_item("Menu Search\xE2\x80\xA6", {}, "F3", "mi-edit-search");
            m.menu_item("Rename Active Object\xE2\x80\xA6", {}, "F2",
                        "mi-edit-rename");
            m.menu_separator("mi-edit-sep2");
            m.menu_item("Preferences\xE2\x80\xA6", "cog", {}, "mi-edit-prefs");
        }, "mb-edit");

        v.menu_button("Render", [&](View& m) {
            m.menu_item("Render Image", "render", "F12", "mi-render-image");
            m.menu_item("Render Animation", {}, "Ctrl F12", "mi-render-anim");
            m.menu_separator("mi-render-sep");
            m.menu_item("View Render", {}, "F11", "mi-render-view");
            m.menu_item("View Animation", {}, "Ctrl F11", "mi-render-viewanim");
        }, "mb-render");

        v.menu_button("Window", [&](View& m) {
            m.menu_item("New Window", {}, {}, "mi-window-new");
            m.menu_item("New Main Window", {}, {}, "mi-window-newmain");
            m.menu_separator("mi-window-sep");
            m.menu_item("Toggle Window Fullscreen", "fullscreen", {},
                        "mi-window-fullscreen");
            m.menu_item("Save Screenshot\xE2\x80\xA6", {}, {},
                        "mi-window-screenshot");
        }, "mb-window");

        v.menu_button("Help", [&](View& m) {
            m.menu_item("Manual", {}, {}, "mi-help-manual");
            m.menu_item("Tutorials", {}, {}, "mi-help-tutorials");
            m.menu_item("Report a Bug", {}, {}, "mi-help-bug");
            m.menu_separator("mi-help-sep");
            m.menu_item("About DENDER", {}, {}, "mi-help-about");
        }, "mb-help");

        // The file being edited, centered — this strip is the window's title
        // bar, and that is what a title bar shows.
        v.document_title("untitled.blend", "mb-title");
    }

    v.toolbar_separator("topbar-sep1");
    ui::select_button(v, "Layout", {}, "menu-workspace", "workspace-pick",
                      "Workspace");
    v.find_widget("workspace-pick")
        .cls("dcs-select dcs-select--btn dn-workspace-pick");
    v.element_ref("span", "dcs-toolbar__spacer", "topbar-spacer");
    ui::select_button(v, "Scene", "cube", "menu-scene", "scene-pick", "Scene");
    ui::select_button(v, "ViewLayer", "layers", "menu-viewlayer",
                      "viewlayer-pick", "View Layer");
    v.toolbar_separator("topbar-sep2");
    v.icon_button("cog", "tweaks-btn")
        .attr("data-dcs-toggle", "popover")
        .attr("data-dcs-target", "#dn-tweaks")
        .attr("data-dcs-placement", "bottom-end")
        .attr("data-dcs-tip", "Theme tweaks");
}

// ── Work area (declarative dock) ────────────────────────────────────────────

void DenderView::build_workarea(View& v) const {
    v.document_view("workarea", [&](View& dv) {
        dv.document([&](View& doc) { build_viewport_body(doc); }, "3D Viewport",
                    "cube")
            .toolbar([&](View& tb) { build_viewport_toolbar(tb); });

        // Right column: Outliner over Inspector inside a 340px slot; Timeline
        // spans the bottom. NOTE: the web seeds the outliner at 240px tall,
        // but the dock resolver intentionally drops size seeds on panels
        // nested inside another panel's slot, so the column starts as an even
        // split; a saved splitter drag then persists via the layout replay.
        auto outliner = dv.dockpanel(
            "Outliner", DockLocation::docked(Dock::Right, 340),
            [&](View& p) { build_outliner_body(p); }, "folder-open",
            "outliner");
        outliner.toolbar([&](View& tb) { build_outliner_toolbar(tb); });

        auto inspector = dv.dockpanel(
            "Inspector", DockLocation::docked(Dock::Bottom).in(outliner),
            [&](View& p) { build_inspector_body(p); }, "cog", "props");
        inspector.toolbar([&](View& tb) { build_inspector_toolbar(tb); });

        auto timeline = dv.dockpanel(
            "Timeline", DockLocation::docked(Dock::Bottom, 140),
            [&](View& p) { build_timeline_body(p); }, "keyframe", "timeline");
        timeline.toolbar([&](View& tb) { build_timeline_toolbar(tb); });

        // Floating N-panel (web: .dn-npanel, right:8/top:8 of the viewport):
        // three DECLARED dockpanels seeded as one floating tab group. Being
        // declared (not raw chrome) is what lets a drag-to-dock placement —
        // e.g. Item split left of the Timeline — replay across rebuilds. The
        // seed offset counts inward from the workarea's top-right, clearing
        // the 340px outliner column so it lands over the viewport like the
        // web reference.
        auto npitem = dv.dockpanel(
            "Item",
            DockLocation::floating(affineui::DockCorner::TopRight, {356, 76},
                                   {220, 520}),
            [&](View& p) { build_npanel_item_body(p); }, "cube", "npanel-item");
        dv.dockpanel("Tool", DockLocation::tab().in(npitem),
                     [&](View& p) { ui::npanel_tool_body(p); }, "axes",
                     "npanel-tool");
        dv.dockpanel("View", DockLocation::tab().in(npitem),
                     [&](View& p) { ui::npanel_view_body(p); }, "camera",
                     "npanel-view");
    });
}

// ── Viewport ────────────────────────────────────────────────────────────────

void DenderView::build_viewport_toolbar(View& v) const {
    DenderApp* app = &app_;

    ui::select_button(v, "Object Mode", {}, "menu-mode", "vp-mode",
                      "Interaction Mode");
    v.toolbar_separator("vp-sep1");

    v.menu_button("View", [&](View& m) {
        m.menu_item("Frame All", {}, "Home", "mi-view-frameall");
        m.menu_item("Frame Selected", {}, "NumPad .", "mi-view-framesel");
        m.menu_separator("mi-view-sep");
        ui::submenu_stub(m, "Viewpoint", {}, "mi-view-viewpoint");
        ui::submenu_stub(m, "Navigation", {}, "mi-view-navigation");
        ui::submenu_stub(m, "Cameras", "camera", "mi-view-cameras");
    }, "vp-view");

    v.menu_button("Select", [&](View& m) {
        m.menu_item("All", {}, "A", "mi-sel-all");
        m.menu_item("None", {}, "Alt A", "mi-sel-none");
        m.menu_item("Invert", {}, "Ctrl I", "mi-sel-invert");
        m.menu_item("Box Select", {}, "B", "mi-sel-box");
    }, "vp-select");

    // The Add menu the web builds at runtime (Mesh / Light sections + Camera
    // and Empty), every item wired to add-and-select.
    v.menu_button("Add", [&](View& m) {
        std::string_view section;
        int index = 0;
        for (const Primitive& p : catalog()) {
            if (p.section != section) {
                if (index > 0) {
                    m.menu_separator("mi-add-sep-" + std::to_string(index));
                }
                if (!p.section.empty()) {
                    ui::menu_label(m, p.section, "mi-add-label-" + slug(p.section));
                }
                section = p.section;
            }
            const std::string type(p.type);
            m.menu_item(type, icon_for(p.type), {},
                        "mi-add-" + slug(p.type))
                .on_click(bind(&app_, &DenderApp::add_object, type));
            ++index;
        }
    }, "vp-add");

    v.menu_button("Object", [&](View& m) {
        ui::submenu_stub(m, "Set Origin", {}, "mi-obj-origin");
        m.menu_item("Shade Smooth", {}, {}, "mi-obj-smooth");
        m.menu_item("Shade Flat", {}, {}, "mi-obj-flat");
        m.menu_separator("mi-obj-sep");
        m.menu_item("Duplicate Objects", {}, "\xE2\x87\xA7 D", "mi-obj-dup")
            .on_click(bind(&app_, &DenderApp::duplicate_active));
        m.menu_item("Join", {}, "Ctrl J", "mi-obj-join");
        m.menu_item("Delete", {}, "X", "mi-obj-delete")
            .on_click(bind(&app_, &DenderApp::delete_active));
    }, "vp-object");

    v.toolbar_separator("vp-sep2");
    ui::select_button(v, "Global", {}, "menu-orient", "vp-orient",
                      "Transform Orientation");
    v.icon_button("pivot", "vp-pivot")
        .attr("data-dcs-toggle", "menu")
        .attr("data-dcs-target", "#menu-pivot")
        .attr("data-dcs-tip", "Transform Pivot Point");
    ui::icon_toggle(v, "magnet", ui_.snapping, "Snapping", "vp-snap")
        .on_click(bind(&app_, &DenderApp::toggle_flag, std::string("snapping")));
    ui::icon_toggle(v, "cross-target", ui_.proportional, "Proportional Editing",
                    "vp-proportional")
        .on_click(
            bind(&app_, &DenderApp::toggle_flag, std::string("proportional")));

    v.element_ref("span", "dcs-toolbar__spacer", "vp-spacer");

    ui::icon_toggle(v, "gizmo", ui_.show_gizmo, "Show Gizmo", "vp-gizmo-toggle")
        .on_click(bind(&app_, &DenderApp::toggle_flag, std::string("gizmo")));
    ui::icon_toggle(v, "view-bbox", ui_.show_overlays, "Show Overlays",
                    "vp-overlays-toggle")
        .on_click(bind(&app_, &DenderApp::toggle_flag, std::string("overlays")));
    ui::icon_toggle(v, "eye", ui_.xray, "Toggle X-Ray", "vp-xray")
        .on_click(bind(&app_, &DenderApp::toggle_flag, std::string("xray")));

    {
        auto group = v.container("dcs-btn-group dn-shademodes", "vp-shademodes");
        group.attr("data-dcs-tip", "Viewport Shading");
        struct Shade { const char* id; const char* icon; Shading mode; };
        static constexpr Shade kShades[] = {
            {"wire", "view-wire", Shading::Wireframe},
            {"solid", "view-solid", Shading::Solid},
            {"tex", "view-tex", Shading::Texture},
            {"render", "view-render", Shading::Rendered},
        };
        for (const Shade& s : kShades) {
            const std::string id(s.id);
            auto btn = v.icon_button(s.icon, "vp-shade-" + id);
            btn.cls("dcs-btn dcs-btn--icon")
                .attr("data-dcs-radio", "shade")
                .attr("aria-pressed",
                      app_.shading() == s.mode ? "true" : "false");
            btn.on_click(bind(&app_, &DenderApp::set_shading, id));
        }
    }
    v.icon_button("chevron-down", "vp-shadeopts")
        .attr("data-dcs-toggle", "menu")
        .attr("data-dcs-target", "#menu-vpoptions")
        .attr("data-dcs-tip", "Viewport Shading Options");

    v.toolbar_separator("vp-sep3");
    ui::text_span(v, "Render Engine", "dn-engine-label", "vp-engine-caption");
    ui::select_button(v, "Cycles", {}, "menu-engine", "vp-engine",
                      "Render Engine");
}

void DenderView::build_viewport_body(View& v) const {
    DenderApp* app = &app_;
    viewport3d::Viewport3D* vp = &app_.viewport();

    auto canvas = v.container("dn-vp-canvas", "vp-canvas");
    canvas.attr("data-dcs-float-host", "");
    canvas.attr("data-screen-label", "3D Viewport");

    // The shared viewport's scene canvas goes in first (inside a positioned
    // scene-layer wrapper so the DOM overlays stack above it); it is painted
    // by the "dender.scene" custom-paint handler (Viewport3D::attach).
    {
        auto layer = v.element("div", "dn-vp-scenelayer", "vp-scene-layer");
        layer.attr("id", "vp-scene");
        layer.attr("data-aui-name", "dn-vp-scenelayer");
        v.canvas("dender.scene", "dn-vp-3dcanvas", "dn-vp-3d")
            .attr("style", "position:absolute;inset:0");
    }

    const app::Object* active =
        app_.ctx().selection().active().empty()
            ? nullptr
            : doc_.find(app_.ctx().selection().active());
    ui::viewport_stats(v, active != nullptr ? std::string_view{active->name}
                                            : std::string_view{});
    ui::viewport_corner(v, scene_stats(doc_, app_.ctx().selection().active(),
                                       [this](std::string_view id) {
                                           return app_.ctx().selection().contains(
                                               id);
                                       }));

    ui::tool_rail(v, ui_.active_tool);
    for (const char* tool : {"tweak", "cursor", "move", "rotate", "scale",
                             "transform", "annotate", "measure"}) {
        const std::string id(tool);
        v.find_widget("rail-" + id).on_click(
            bind(&app_, &DenderApp::set_tool, id));
    }
    v.find_widget("rail-add-cube")
        .on_click(bind(&app_, &DenderApp::add_object, std::string("Cube")));

    ui::nav_cluster(v, vp->pan_mode());
    // Nav wiring: Zoom dollies in, Move View toggles the LMB between orbit and
    // pan, Camera View frames the selection; the Perspective/Ortho toggle
    // stays cosmetic like the web. Bound through the viewport's weak ref
    // (Viewport3D is Trackable) so these no-op if it's gone.
    v.find_widget("nav-zoom").on_click(
        bind(vp, &viewport3d::Viewport3D::dolly, 0.8));
    v.find_widget("nav-move").on_click(
        bind(vp, &viewport3d::Viewport3D::toggle_pan_mode));
    v.find_widget("nav-camera").on_click(
        bind(vp, &viewport3d::Viewport3D::frame_selected));

    // The N-panel is a declared floating dock group — see build_workarea.
}

// ── Outliner ────────────────────────────────────────────────────────────────

void DenderView::build_outliner_toolbar(View& v) const {
    ui::select_button(v, "View Layer", {}, "menu-displaymode", "outliner-mode",
                      "Display Mode");
    v.element_ref("input", "dcs-input", "outliner-search")
        .attr("type", "text")
        .attr("placeholder", "Search")
        .attr("style", "max-width:140px");
    v.icon_button("eq", "outliner-filter")
        .attr("data-dcs-toggle", "menu")
        .attr("data-dcs-target", "#menu-filter")
        .attr("data-dcs-tip", "Filter");
    v.icon_button("folder", "outliner-newcoll")
        .attr("data-dcs-tip", "New Collection");
}

void DenderView::build_outliner_body(View& v) const {
    // The tree is a data-dcs-select box: clicking a row emits a widget CHANGE
    // on the tree carrying the selected row's data-dcs-value, NOT a per-row
    // button click. So bind the tree's on_change (a per-row on_click never
    // fires here) and carry the object id as each row's value.
    auto tree = v.tree("outliner-tree");
    tree.attr("data-dcs-select", "multi");
    // Bind through the weak-ref path (like every other handler here): this
    // change handler outlives the transient DenderView it's built in, so a
    // raw `this` capture would dangle. app_ is a Trackable DenderApp, so
    // bind() holds a weak handle that no-ops if it's gone. select_object
    // ignores an empty id, so the "cleared selection" change is harmless.
    tree.ref().on_change(bind(&app_, &DenderApp::select_object));

    {
        TreeRowOptions opts;
        opts.depth = 0;
        opts.icon = "cube";
        opts.expandable = true;
        opts.expanded = true;
        opts.draggable = false;
        v.tree_row("Scene", opts, "row-scene");
    }
    {
        TreeRowOptions opts;
        opts.depth = 1;
        opts.icon = "folder";
        opts.expandable = true;
        opts.expanded = true;
        opts.draggable = false;
        opts.meta_icon = "eye";
        v.tree_row("Collection", opts, "row-collection");
    }
    for (const auto& entry : ordered_objects(doc_)) {
        const app::Object& obj = *entry.object;
        TreeRowOptions opts;
        opts.depth = 2 + entry.depth;
        opts.selected = app_.ctx().selection().contains(obj.id);
        opts.icon = icon_for(obj.type);
        v.tree_row(obj.name, opts, "row-" + obj.id)
            .attr("data-dcs-value", obj.id);
    }
    {
        TreeRowOptions opts;
        opts.depth = 0;
        opts.icon = "globe";
        opts.expandable = true;
        opts.expanded = false;
        opts.draggable = false;
        v.tree_row("World", opts, "row-world");
    }
}

// ── Inspector ───────────────────────────────────────────────────────────────

namespace {
struct PropTab {
    const char* id;
    const char* tip;
    const char* icon;
};
constexpr PropTab kPropTabs[] = {
    {"tool", "Active Tool", "axes"},
    {"render", "Render", "render"},
    {"output", "Output", "export"},
    {"viewlayer", "View Layer", "layers"},
    {"scene", "Scene", "cube"},
    {"world", "World", "globe"},
    {"object", "Object", "cube"},
    {"modifiers", "Modifiers", "cog"},
    {"particles", "Particles", "cube"},
    {"physics", "Physics", "bolt"},
    {"constraints", "Constraints", "link"},
    {"data", "Object Data", "mesh"},
    {"material", "Material", "droplet"},
    {"texture", "Texture", "texture"},
};
}  // namespace

void DenderView::build_inspector_toolbar(View& v) const {
    ui::select_button(v, {}, "cog", "menu-editortype", "props-editortype",
                      "Editor Type");
    {
        auto group = v.container("dcs-btn-group", "props-layout");
        group.attr("data-dcs-tip", "Inspector Layout");
        struct Layout { const char* icon; const char* tip; bool on; };
        static constexpr Layout kLayouts[] = {
            {"eq", "Row", false},
            {"layers", "Stack", true},
            {"grid", "Grid", false},
        };
        int i = 0;
        for (const Layout& l : kLayouts) {
            auto btn = v.icon_button(l.icon,
                                     "props-layout-" + std::to_string(i++));
            btn.cls("dcs-btn dcs-btn--icon")
                .attr("data-dcs-radio", "proplayout")
                .attr("data-dcs-tip", l.tip)
                .attr("aria-pressed", l.on ? "true" : "false");
        }
    }
    {
        auto search = v.container("dcs-search dn-props-search", "props-search");
        ui::icon(v, "search", {}, "props-search-icon");
        v.element_ref("input", "dcs-input dcs-input--sm", "props-search-input")
            .attr("type", "search")
            .attr("placeholder", "Search inspector\xE2\x80\xA6");
    }
}

void DenderView::build_inspector_body(View& v) const {
    DenderApp* app = &app_;

    // Left icon rail + sheet (the #props-body flex row comes from native_css).
    {
        auto rail = v.container(
            "dcs-tabs dcs-toolbar dcs-toolbar--v dcs-toolbar--sm dn-props-rail",
            "prop-rail");
        for (const PropTab& tab : kPropTabs) {
            const std::string id(tab.id);
            auto btn = v.element("button", "dcs-tab dcs-btn dcs-btn--icon dcs-btn--sm",
                                 "prop-tab-" + id);
            // The APP owns which tab is active (set_inspector_tab → reload
            // re-renders exactly one populated panel). So the buttons are
            // plain — NO data-dcs-target: that would ALSO drive the
            // renderer's client-side tabpanel toggle, which fights the
            // rebuild and lands on an empty panel (the blank-inspector bug).
            btn.attr("type", "button")
                .attr("data-dcs-tip", tab.tip)
                .attr("aria-selected",
                      ui_.inspector_tab == id ? "true" : "false");
            ui::icon(v, tab.icon, {}, "prop-tab-icon-" + id);
            btn.ref().on_click(
                bind(&app_, &DenderApp::set_inspector_tab, id));
        }
    }
    {
        // Only the active tab's sheet is built (populated); the rebuild is
        // the switch, so there are no hidden client-side tabpanels.
        auto sheet = v.container("dn-props__sheet", "prop-sheet");
        const std::string& active = ui_.inspector_tab;
        auto panel = v.container({}, "prop-" + active);
        panel.attr("id", "prop-" + active);
        if (active == "object") build_inspector_object_tab(v);
        else build_inspector_static_tab(v, active);
    }
}

void DenderView::build_inspector_object_tab(View& v) const {
    const std::string id(app_.ctx().selection().active());
    const app::Object* obj = id.empty() ? nullptr : doc_.find(id);

    {
        auto db = v.container("dcs-row dn-datablock", "insp-datablock");
        const std::string_view type =
            obj != nullptr ? std::string_view{obj->type} : std::string_view{};
        ui::icon(v, icon_for(type), "dn-datablock__icon", "insp-db-icon");
        v.element_ref("input", "dcs-input", "insp-name")
            .attr("type", "text")
            .attr("value", obj != nullptr ? obj->name : std::string{})
            .attr("style", "flex:1")
            .on_change(bind(&app_, &DenderApp::rename_active));
        v.icon_button("more-h", "insp-browse")
            .attr("data-dcs-tip", "Browse Object");
    }

    {
        auto fold = v.foldout("Transform", true, "fold-xform");
        auto props = v.container("dcs-props", "xform-props");
        build_transform_vecs(v, id, "insp");
    }

    { auto fold = v.foldout("Delta Transform", false, "fold-delta"); }

    {
        auto fold = v.foldout("Relations", true, "fold-relations");
        auto props = v.container("dcs-props", "rel-props");
        {
            auto field = v.container("dcs-field", "rel-parent");
            ui::text_span(v, "Parent", "dcs-field__label", "rel-parent-label");
            const app::Object* parent =
                obj != nullptr && !obj->parent.empty() ? doc_.find(obj->parent)
                                                       : nullptr;
            v.element_ref("input", "dcs-input", "rel-parent-input")
                .attr("type", "text")
                .attr("placeholder", "\xE2\x80\x94")
                .attr("value", parent != nullptr ? parent->name : std::string{});
        }
        {
            auto field = v.container("dcs-field", "rel-collection");
            ui::text_span(v, "Collection", "dcs-field__label",
                          "rel-collection-label");
            // Cosmetic select chrome (the web row has no menu either).
            auto sel = v.element("button", "dcs-select dcs-select--btn",
                                 "rel-collection-select");
            sel.attr("type", "button");
            ui::text_span(v, "Collection", "dcs-select__label",
                          "rel-collection-value");
            auto caret = v.container("dcs-select__caret", "rel-collection-caret");
            ui::icon(v, "chevron-down", {}, "rel-collection-caret-icon");
        }
    }

    { auto fold = v.foldout("Viewport Display", false, "fold-vpdisplay"); }
}

void DenderView::build_transform_vecs(View& v, std::string_view id,
                                      std::string_view key_prefix) const {
    // Live Location / Rotation / Scale vec fields over the object's e3d node
    // reflection (game-editor pattern): "<prefix>.x/.y/.z" doubles, rotation
    // in degrees. Edits preview on the node during a scrub and commit one undo
    // entry on gesture end. `key_prefix` disambiguates widget names so the
    // same builder can serve the main inspector AND the Item N-panel without
    // two widgets sharing a key.
    viewport3d::Viewport3D* vp = &app_.viewport();
    e3d::Object3D* node = id.empty() ? nullptr : vp->node_of(id);

    struct VecRow {
        const char* prefix;  // reflection property prefix
        const char* key;     // widget key stem
        const char* label;
        const char* suffix;
        double fallback;
        double step;
        bool linear;  // constant step/pixel scrub (rotation degrees)
    };
    static constexpr VecRow kRows[] = {
        {"position", "loc", "Location", " m", 0.0, 0.01, false},
        {"rotation", "rot", "Rotation", "\xC2\xB0", 0.0, 0.5, true},
        {"scale", "scl", "Scale", "", 1.0, 0.01, false},
    };
    static constexpr const char* kAxis[3] = {".x", ".y", ".z"};
    const std::string owner_id(id);
    for (const VecRow& row : kRows) {
        std::array<double, 3> ch{row.fallback, row.fallback, row.fallback};
        if (node != nullptr) {
            const affineui::ObjectClass& cls = get_class(*node);
            for (int i = 0; i < 3; ++i) {
                const auto value =
                    cls.get(node, std::string(row.prefix) + kAxis[i]);
                if (const double* d = std::get_if<double>(&value)) {
                    ch[static_cast<std::size_t>(i)] = *d;
                }
            }
        }
        const std::string key =
            std::string(key_prefix) + "-" + row.key;  // e.g. "insp-loc"
        v.vec(row.label, {"X", "Y", "Z"}, {ch[0], ch[1], ch[2]}, key, row.step,
              row.linear);
        for (int i = 0; i < 3; ++i) {
            auto field = v.find_widget(key + "-" + std::to_string(i));
            if (*row.suffix != '\0') field.attr("data-suffix", row.suffix);
            const std::string prop = std::string(row.prefix) + kAxis[i];
            const double fallback = row.fallback;
            // Bound (id, prop) PLUS the widget's runtime value, which bind()'s
            // bound-args form can't express (it yields a zero-arg callback).
            // So capture a WEAK ref to the viewport and lock() it: the handler
            // no-ops after the viewport dies — the crash-safety bind() gives.
            const auto vp_ref = affineui::to_weak_ref(vp);
            field.on_change(
                [vp_ref, owner_id, prop, fallback](std::string_view value) {
                    if (auto* v3 = vp_ref.get()) {
                        v3->preview_node_property(
                            owner_id, prop,
                            affineui::PropertyValue{
                                parse_double_or(value, fallback)});
                    }
                });
            field.on_commit(
                [vp_ref, owner_id, prop, fallback](std::string_view value) {
                    if (auto* v3 = vp_ref.get()) {
                        v3->commit_node_property(
                            owner_id, prop,
                            affineui::PropertyValue{
                                parse_double_or(value, fallback)});
                    }
                });
        }
    }
}

void DenderView::build_npanel_item_body(View& v) const {
    // The Item N-panel shows the object's CORE properties — not a second
    // transform. Two fields, per the web reference:
    //   Location   — mirrors the object's world position (live, editable;
    //                the same node position the inspector's Location edits).
    //   Dimensions — the object's BASE CREATION size: the mesh geometry's
    //                bounding-box extents at scale 1. Editing Dimensions is a
    //                mesh REGENERATION (rebuild the geometry at the new size),
    //                not a scale — preview swaps geometry live during a scrub,
    //                commit pushes one undoable resize at the gesture's end.
    const std::string id(app_.ctx().selection().active());
    e3d::Object3D* node = id.empty() ? nullptr : app_.viewport().node_of(id);

    auto fold = v.foldout("Item", true, "np-item");
    auto props = v.container("dcs-props", "np-item-props");

    // Location — live, over the node's position reflection.
    std::array<double, 3> loc{0.0, 0.0, 0.0};
    if (node != nullptr) {
        const affineui::ObjectClass& cls = get_class(*node);
        static constexpr const char* kAxis[3] = {"position.x", "position.y",
                                                  "position.z"};
        for (int i = 0; i < 3; ++i) {
            const affineui::PropertyValue value = cls.get(node, kAxis[i]);
            if (const double* d = std::get_if<double>(&value)) {
                loc[static_cast<std::size_t>(i)] = *d;
            }
        }
    }
    v.vec("Location", {"X", "Y", "Z"}, {loc[0], loc[1], loc[2]}, "np-loc",
          0.01, false);
    {
        viewport3d::Viewport3D* vp = &app_.viewport();
        const std::string owner_id(id);
        for (int i = 0; i < 3; ++i) {
            auto field = v.find_widget("np-loc-" + std::to_string(i));
            field.attr("data-suffix", " m");
            const std::string prop = std::string("position.") +
                                     static_cast<char>('x' + i);
            const auto vp_ref = affineui::to_weak_ref(vp);
            field.on_change([vp_ref, owner_id, prop](std::string_view value) {
                if (auto* v3 = vp_ref.get()) {
                    v3->preview_node_property(
                        owner_id, prop,
                        affineui::PropertyValue{parse_double_or(value, 0.0)});
                }
            });
            field.on_commit([vp_ref, owner_id, prop](std::string_view value) {
                if (auto* v3 = vp_ref.get()) {
                    v3->commit_node_property(
                        owner_id, prop,
                        affineui::PropertyValue{parse_double_or(value, 0.0)});
                }
            });
        }
    }

    // Dimensions — the base creation size. Editing rebuilds the mesh at the
    // new size (a REGENERATION, not a scale): preview swaps geometry live
    // during a scrub, commit pushes one undoable resize at the gesture's end.
    const e3d::Vec3 dim =
        id.empty() ? e3d::Vec3{} : app_.viewport().base_dimensions(id);
    v.vec("Dimensions", {"X", "Y", "Z"}, {dim.x, dim.y, dim.z}, "np-dim");
    {
        // preview/commit carry a bound axis AND the widget's value, which
        // bind()'s bound-args form can't express — so a weak ref to the app,
        // lock()ed before use (no-op after teardown, like bind()).
        const auto app_ref = affineui::to_weak_ref(&app_);
        for (int i = 0; i < 3; ++i) {
            auto field = v.find_widget("np-dim-" + std::to_string(i));
            field.attr("data-suffix", " m");
            field.on_change([app_ref, i](std::string_view value) {
                if (auto* a = app_ref.get()) a->preview_dimension(i, value);
            });
            field.on_commit([app_ref, i](std::string_view value) {
                if (auto* a = app_ref.get()) a->commit_dimension(i, value);
            });
        }
    }
}

void DenderView::build_inspector_static_tab(View& v, std::string_view id) const {
    if (id == "tool") {
        v.paragraph("Active Tool & Workspace settings.", "dn-cap", "tool-cap");
        auto fold = v.foldout("Transform", true, "tool-fold");
        v.checkbox("Affect Only Origins", false, "tool-origins");
    } else if (id == "render") {
        auto fold = v.foldout("Sampling", true, "render-fold");
        auto props = v.container("dcs-props", "render-props");
        combo_field(v, "Render", 4096.0, 1.0, "render-samples");
        combo_field(v, "Viewport", 1024.0, 1.0, "render-viewport");
        v.checkbox("Denoise", false, "render-denoise");
    } else if (id == "output") {
        auto fold = v.foldout("Format", true, "output-fold");
        auto props = v.container("dcs-props", "output-props");
        combo_field(v, "Resolution X", 1920.0, 1.0, "output-resx", " px");
        combo_field(v, "Resolution Y", 1080.0, 1.0, "output-resy", " px");
        v.dropdown("Frame Rate", {"24 fps", "30 fps", "60 fps"}, "24 fps",
                   "output-fps");
    } else if (id == "viewlayer") {
        v.paragraph("View Layer passes & filters.", "dn-cap", "viewlayer-cap");
    } else if (id == "scene") {
        auto fold = v.foldout("Units", true, "scene-fold");
        auto props = v.container("dcs-props", "scene-props");
        v.dropdown("Unit System", {"Metric", "Imperial", "None"}, "Metric",
                   "scene-units");
    } else if (id == "world") {
        auto fold = v.foldout("Surface", true, "world-fold");
        auto props = v.container("dcs-props", "world-props");
        v.colorfield("Color", "#3a3d45", "world-color");
        combo_field(v, "Strength", 1.0, 0.01, "world-strength");
    } else if (id == "modifiers") {
        {
            auto add = v.element("button", "dcs-btn dcs-btn--lg dn-add-block",
                                 "mod-add");
            add.attr("type", "button");
            ui::icon(v, "plus", {}, "mod-add-icon");
            ui::text_span(v, " Add Modifier", {}, "mod-add-label");
        }
        auto card = v.container("dn-modifier", "mod-card");
        {
            auto head = v.container("dn-modifier__head", "mod-head");
            ui::icon(v, "chevron-down", {}, "mod-head-chevron");
            ui::icon(v, "cube", {}, "mod-head-icon");
            ui::text_span(v, "Bevel", "dn-modifier__title", "mod-title");
            v.icon_button("mesh", "mod-editmode")
                .cls("dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost")
                .attr("data-dcs-tip", "Edit-mode display");
            v.icon_button("view-solid", "mod-realtime")
                .cls("dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost")
                .attr("aria-pressed", "true")
                .attr("data-dcs-tip", "Realtime");
            v.icon_button("chevron-down", "mod-menu")
                .cls("dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost")
                .attr("data-dcs-toggle", "menu")
                .attr("data-dcs-target", "#menu-modifier");
            v.icon_button("close", "mod-delete")
                .cls("dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost")
                .attr("data-dcs-tip", "Delete");
        }
        {
            auto body = v.container("dn-modifier__body", "mod-body");
            v.button_group("Limit", {"Width", "Angle"}, "Width", "mod-limit");
            combo_field(v, "Amount", 0.1, 0.01, "mod-amount", " m");
            v.slider("Segments", 2.0, 1.0, 12.0, "mod-segments");
            v.checkbox("Clamp Overlap", false, "mod-clamp");
        }
    } else if (id == "material") {
        {
            auto row = v.container("dcs-row dn-datablock", "mat-row");
            v.element_ref("span", "dn-mat-preview", "mat-preview");
            v.element_ref("input", "dcs-input", "mat-name")
                .attr("type", "text")
                .attr("value", "Material")
                .attr("style", "flex:1");
            v.icon_button("plus", "mat-add");
            v.icon_button("minus", "mat-remove");
        }
        auto fold = v.foldout("Surface", true, "mat-fold");
        auto props = v.container("dcs-props", "mat-props");
        v.dropdown("Surface", {"Principled BSDF", "Diffuse BSDF", "Emission"},
                   "Principled BSDF", "mat-surface");
        v.colorfield("Base Color", "#cf6b3a", "mat-basecolor");
        v.slider("Metallic", 0.0, 0.0, 1.0, "mat-metallic");
        v.slider("Roughness", 0.45, 0.0, 1.0, "mat-roughness");
        combo_field(v, "IOR", 1.45, 0.01, "mat-ior");
    } else if (id == "particles") {
        v.paragraph("No particle systems. Press + to add.", "dn-cap",
                    "particles-cap");
    } else if (id == "physics") {
        auto col = v.container("dcs-props", "physics-props");
        for (const char* label : {"Rigid Body", "Collision", "Cloth", "Fluid"}) {
            auto btn = v.element("button", "dcs-btn dcs-btn--lg dn-add-block",
                                 "physics-" + slug(label));
            btn.attr("type", "button");
            v.text(label, "physics-" + slug(label) + "-label");
        }
    } else if (id == "constraints") {
        auto add = v.element("button", "dcs-btn dcs-btn--lg dn-add-block",
                             "constraints-add");
        add.attr("type", "button");
        ui::icon(v, "plus", {}, "constraints-add-icon");
        ui::text_span(v, " Add Object Constraint", {}, "constraints-add-label");
    } else if (id == "data") {
        {
            auto fold = v.foldout("Normals", true, "data-fold-normals");
            v.checkbox("Auto Smooth", false, "data-autosmooth");
        }
        {
            auto fold = v.foldout("UV Maps", true, "data-fold-uv");
            auto tree = v.tree("data-uv-tree");
            TreeRowOptions opts;
            opts.selected = true;
            opts.icon = "image";
            opts.draggable = false;
            v.tree_row("UVMap", opts, "data-uv-row");
        }
    } else if (id == "texture") {
        v.paragraph("No textures in active slot.", "dn-cap", "texture-cap");
    }
}

// ── Timeline ────────────────────────────────────────────────────────────────

void DenderView::build_timeline_toolbar(View& v) const {
    DenderApp* app = &app_;
    const Timeline& tl = app_.timeline();

    ui::select_button(v, {}, "keyframe", "menu-editortype", "tl-editortype",
                      "Editor Type");

    v.menu_button("View", [&](View& m) {
        m.menu_item("Frame All", {}, {}, "mi-tlview-all");
        m.menu_item("Frame Playback Range", {}, {}, "mi-tlview-range");
        m.menu_item("Show Seconds", "check", {}, "mi-tlview-seconds");
    }, "tl-view");
    v.menu_button("Marker", [&](View& m) {
        m.menu_item("Add Marker", {}, "M", "mi-tlmarker-add");
        m.menu_item("Rename Marker", {}, {}, "mi-tlmarker-rename");
        m.menu_item("Delete Marker", {}, {}, "mi-tlmarker-delete");
    }, "tl-marker");

    v.toolbar_separator("tl-sep1");

    {
        auto transport = v.container("dcs-btn-group dn-tl-controls",
                                     "tl-transport");
        auto tbtn = [&](const char* icon, const char* tip,
                        const char* key) -> affineui::WidgetRef {
            auto btn = v.icon_button(icon, key);
            btn.cls("dcs-btn dcs-btn--icon").attr("data-dcs-tip", tip);
            return btn;
        };
        tbtn("skip-back", "Jump to Start", "tl-jump-start");
        tbtn("keyframe", "Previous Keyframe", "tl-prev-key");
        tbtn("rewind", "Play Reverse", "tl-play-rev");
        tbtn(app_.playing() ? "pause" : "play", "Play Animation", "tl-play")
            .attr("aria-pressed", "true")
            .on_click(bind(&app_, &DenderApp::toggle_play));
        tbtn("keyframe", "Next Keyframe", "tl-next-key");
        tbtn("skip-fwd", "Jump to End", "tl-jump-end");
    }

    v.combo({}, tl.frame, 1.0, "tl-frame")
        .attr("data-min", "1")
        .attr("data-max", "250")
        .attr("data-dec", "0")
        .attr("style", "width:72px")
        .attr("data-dcs-tip", "Current Frame")
        .on_change(bind(&app_, &DenderApp::set_frame));

    v.toolbar_separator("tl-sep2");

    {
        auto keying = v.element("button", "dcs-btn dcs-btn--ghost", "tl-keying");
        keying.attr("type", "button")
            .attr("data-dcs-toggle", "menu")
            .attr("data-dcs-target", "#menu-keying")
            .attr("data-dcs-tip", "Keying Set");
        ui::icon(v, "keyframe", {}, "tl-keying-icon");
        ui::text_span(v, " Keying", {}, "tl-keying-label");
    }
    ui::icon_toggle(v, "record", ui_.auto_key, "Auto Keying", "tl-autokey")
        .on_click(bind(&app_, &DenderApp::toggle_flag, std::string("autokey")));

    v.element_ref("span", "dcs-toolbar__spacer", "tl-spacer");

    {
        auto field = v.container("dcs-field", "tl-start-field");
        ui::text_span(v, "Start", "dcs-field__label", "tl-start-label");
        v.combo({}, tl.start, 1.0, "tl-start")
            .attr("data-min", "0")
            .attr("data-max", "9999")
            .attr("data-dec", "0")
            .attr("style", "width:72px")
            .on_change(bind(&app_, &DenderApp::set_start));
    }
    {
        auto field = v.container("dcs-field", "tl-end-field");
        ui::text_span(v, "End", "dcs-field__label", "tl-end-label");
        v.combo({}, tl.end, 1.0, "tl-end")
            .attr("data-min", "1")
            .attr("data-max", "9999")
            .attr("data-dec", "0")
            .attr("style", "width:72px")
            .on_change(bind(&app_, &DenderApp::set_end));
    }
}

void DenderView::build_timeline_body(View& v) const {
    const app::Object* active =
        app_.ctx().selection().active().empty()
            ? nullptr
            : doc_.find(app_.ctx().selection().active());
    ui::timeline_body(v, app_.timeline(),
                      active != nullptr ? std::string_view{active->name}
                                        : std::string_view{},
                      app_.view_width());
}

// ── Statusbar ───────────────────────────────────────────────────────────────

void DenderView::build_statusbar(View& v) const {
    auto bar = v.status_bar("statusbar");
    const std::string active_id(app_.ctx().selection().active());
    {
        auto item = v.element("span", "dcs-statusbar__item", "st-active");
        const app::Object* active =
            active_id.empty() ? nullptr : doc_.find(active_id);
        const std::string_view type =
            active != nullptr ? std::string_view{active->type}
                              : std::string_view{};
        ui::icon(v, icon_for(type), {}, "st-active-icon");
        v.text(" " + (active != nullptr ? active->name
                                        : std::string{"\xE2\x80\x94"}),
               "st-active-name");
    }
    v.element_ref("span", "dcs-statusbar__spacer", "st-spacer");
    // Live topology readout from the mesh tables (active mesh, else totals).
    const SceneStats stats =
        scene_stats(doc_, active_id, [this](std::string_view id) {
            return app_.ctx().selection().contains(id);
        });
    stat_item(v, "Verts", std::to_string(stats.verts), "st-verts");
    stat_item(v, "Faces", std::to_string(stats.faces), "st-faces");
    stat_item(v, "Tris", std::to_string(stats.tris), "st-tris");
    stat_item(v, "Objects",
              std::to_string(stats.selected) + "/" +
                  std::to_string(stats.total),
              "st-objects");
    stat_item(v, "Memory", "42.6 MB", "st-memory");
    v.element("span", "dcs-statusbar__item", "st-version")
        .text("v1.0 \xC2\xB7 DENDER");
}

// ── Shared popup menus (select-button targets) ──────────────────────────────

void DenderView::build_shared_menus(View& v) const {
    v.menu("menu-workspace", [&](View& m) {
        struct Ws { const char* label; bool checked; };
        static constexpr Ws kSpaces[] = {
            {"Layout", true},       {"Modeling", false},
            {"Sculpting", false},   {"UV Editing", false},
            {"Texture Paint", false}, {"Shading", false},
            {"Animation", false},   {"Rendering", false},
            {"Compositing", false}, {"Geometry Nodes", false},
            {"Scripting", false},
        };
        for (const Ws& w : kSpaces) {
            ui::check_menu_item(m, w.label, {}, {}, w.checked,
                                "mi-ws-" + slug(w.label));
        }
        m.menu_separator("mi-ws-sep");
        m.menu_item("Add Workspace\xE2\x80\xA6", "plus", {}, "mi-ws-add");
    });

    v.menu("menu-scene", [&](View& m) {
        ui::check_menu_item(m, "Scene", "cube", {}, true, "mi-scene-current");
        m.menu_separator("mi-scene-sep");
        m.menu_item("New Scene", "plus", {}, "mi-scene-new");
    });

    v.menu("menu-viewlayer", [&](View& m) {
        ui::check_menu_item(m, "ViewLayer", "layers", {}, true,
                            "mi-vl-current");
        m.menu_separator("mi-vl-sep");
        m.menu_item("Add View Layer", "plus", {}, "mi-vl-add");
    });

    v.menu("menu-mode", [&](View& m) {
        m.menu_item("Object Mode", "cube", "Tab", "mi-mode-object");
        m.menu_item("Edit Mode", "mesh", {}, "mi-mode-edit");
        m.menu_item("Sculpt Mode", {}, {}, "mi-mode-sculpt");
        m.menu_item("Vertex Paint", {}, {}, "mi-mode-vpaint");
        m.menu_item("Weight Paint", {}, {}, "mi-mode-wpaint");
        m.menu_item("Texture Paint", {}, {}, "mi-mode-tpaint");
    });

    v.menu("menu-orient", [&](View& m) {
        ui::check_menu_item(m, "Global", {}, {}, true, "mi-orient-global");
        m.menu_item("Local", {}, {}, "mi-orient-local");
        m.menu_item("Normal", {}, {}, "mi-orient-normal");
        m.menu_item("Gimbal", {}, {}, "mi-orient-gimbal");
        m.menu_item("View", {}, {}, "mi-orient-view");
    });

    v.menu("menu-pivot", [&](View& m) {
        ui::check_menu_item(m, "Median Point", "pivot", {}, true,
                            "mi-pivot-median");
        m.menu_item("3D Cursor", {}, {}, "mi-pivot-cursor");
        m.menu_item("Individual Origins", {}, {}, "mi-pivot-individual");
        m.menu_item("Active Element", {}, {}, "mi-pivot-active");
        m.menu_item("Bounding Box Center", {}, {}, "mi-pivot-bbox");
    });

    v.menu("menu-vpoptions", [&](View& m) {
        ui::menu_label(m, "Lighting", "mi-vpo-label");
        ui::check_menu_item(m, "Studio", {}, {}, true, "mi-vpo-studio");
        m.menu_item("MatCap", {}, {}, "mi-vpo-matcap");
        m.menu_item("Flat", {}, {}, "mi-vpo-flat");
        m.menu_separator("mi-vpo-sep");
        m.menu_item("Backface Culling", {}, {}, "mi-vpo-backface");
        m.menu_item("Cavity", {}, {}, "mi-vpo-cavity");
        ui::check_menu_item(m, "Shadow", {}, {}, true, "mi-vpo-shadow");
    });

    v.menu("menu-engine", [&](View& m) {
        m.menu_item("Eevee", {}, {}, "mi-engine-eevee");
        m.menu_item("Workbench", {}, {}, "mi-engine-workbench");
        ui::check_menu_item(m, "Cycles", {}, {}, true, "mi-engine-cycles");
    });

    v.menu("menu-displaymode", [&](View& m) {
        m.menu_item("Scene Collection", {}, {}, "mi-dm-scenecoll");
        ui::check_menu_item(m, "View Layer", {}, {}, true, "mi-dm-viewlayer");
        m.menu_item("Blender File", {}, {}, "mi-dm-file");
        m.menu_item("Data API", {}, {}, "mi-dm-dataapi");
        m.menu_item("Orphan Data", {}, {}, "mi-dm-orphan");
    });

    v.menu("menu-filter", [&](View& m) {
        ui::menu_label(m, "Restriction Toggles", "mi-filter-label");
        m.menu_item("Visibility", "eye", {}, "mi-filter-visibility");
        m.menu_item("Disable in Renders", "render", {}, "mi-filter-renders");
        m.menu_item("Selectable", "lock", {}, "mi-filter-selectable");
        m.menu_separator("mi-filter-sep");
        m.menu_item("Filter by Type", {}, {}, "mi-filter-type");
    });

    v.menu("menu-editortype", [&](View& m) {
        ui::menu_label(m, "General", "mi-et-general");
        m.menu_item("3D Viewport", "cube", {}, "mi-et-viewport");
        m.menu_item("Image Editor", "image", {}, "mi-et-image");
        m.menu_item("Shader Editor", "view-tex", {}, "mi-et-shader");
        ui::menu_label(m, "Animation", "mi-et-animation");
        m.menu_item("Timeline", "keyframe", {}, "mi-et-timeline");
        m.menu_item("Dope Sheet", "eq", {}, "mi-et-dopesheet");
        m.menu_item("Graph Editor", "spline", {}, "mi-et-graph");
        ui::menu_label(m, "Data", "mi-et-data");
        m.menu_item("Outliner", "folder", {}, "mi-et-outliner");
        m.menu_item("Inspector", "cog", {}, "mi-et-inspector");
    });

    v.menu("menu-keying", [&](View& m) {
        m.menu_item("Location", {}, {}, "mi-key-loc");
        m.menu_item("Rotation", {}, {}, "mi-key-rot");
        m.menu_item("Scale", {}, {}, "mi-key-scale");
        ui::check_menu_item(m, "Location, Rotation & Scale", {}, {}, true,
                            "mi-key-lrs");
    });

    v.menu("menu-modifier", [&](View& m) {
        m.menu_item("Apply", {}, "Ctrl A", "mi-mod-apply");
        m.menu_item("Duplicate", {}, {}, "mi-mod-duplicate");
        m.menu_item("Copy to Selected", {}, {}, "mi-mod-copy");
        m.menu_separator("mi-mod-sep");
        m.menu_item("Move to First", {}, {}, "mi-mod-first");
        m.menu_item("Move to Last", {}, {}, "mi-mod-last");
    });
}

// ── Theme tweaks popover ────────────────────────────────────────────────────

void DenderView::build_tweaks_popover(View& v) const {
    DenderApp* app = &app_;
    const std::string_view accent = app_.accent();
    const std::string_view density = app_.density();
    const std::string_view style = app_.style();

    auto pop = v.container("dcs-popover dn-tweaks-popover", "dn-tweaks");
    pop.attr("id", "dn-tweaks").attr("hidden", "");
    {
        auto head = v.container("dcs-popover__header", "tweaks-head");
        ui::icon(v, "cog", {}, "tweaks-head-icon");
        ui::text_span(v, " Theme tweaks", {}, "tweaks-head-title");
    }
    auto body = v.container("dcs-popover__body dcs-form", "tweaks-body");

    {
        auto field = v.container("dcs-field", "tweaks-density");
        ui::text_span(v, "Density", "dcs-field__label", "tweaks-density-label");
        auto group = v.container("dcs-btn-group", "tweaks-density-group");
        struct Density { const char* label; const char* value; };
        static constexpr Density kDensities[] = {
            {"S", "compact"}, {"M", "comfortable"}, {"L", "spacious"},
        };
        for (const Density& d : kDensities) {
            const std::string value(d.value);
            auto btn = v.element("button", "dcs-btn",
                                 "tweaks-density-" + value);
            btn.attr("type", "button")
                .attr("aria-pressed", density == value ? "true" : "false")
                .text(d.label);
            btn.ref().on_click(bind(&app_, &DenderApp::set_density, value));
        }
    }
    {
        auto field = v.container("dcs-field", "tweaks-style");
        ui::text_span(v, "Style", "dcs-field__label", "tweaks-style-label");
        auto group = v.container("dcs-btn-group", "tweaks-style-group");
        struct Style { const char* label; const char* value; };
        static constexpr Style kStyles[] = {{"Flat", "flat"}, {"3D", "3d"}};
        for (const Style& s : kStyles) {
            const std::string value(s.value);
            auto btn = v.element("button", "dcs-btn", "tweaks-style-" + value);
            btn.attr("type", "button")
                .attr("aria-pressed", style == value ? "true" : "false")
                .text(s.label);
            btn.ref().on_click(bind(&app_, &DenderApp::set_style, value));
        }
    }
    {
        auto field = v.container("dcs-field", "tweaks-accent");
        ui::text_span(v, "Accent", "dcs-field__label", "tweaks-accent-label");
        auto row = v.container("dn-accent-row", "tweaks-accent-row");
        struct Accent { const char* name; const char* color; };
        static constexpr Accent kAccents[] = {
            {"blue", "#4f86d6"},   {"orange", "#e8843a"}, {"green", "#4e9e54"},
            {"purple", "#8466cf"}, {"teal", "#2f9c93"},
        };
        for (const Accent& a : kAccents) {
            const std::string name(a.name);
            auto dot = v.element("button", "dn-accent-dot",
                                 "tweaks-accent-" + name);
            dot.attr("type", "button")
                .attr("aria-pressed", accent == name ? "true" : "false")
                .attr("data-dcs-tip", name)
                .attr("style", std::string("background:") + a.color +
                                   ";color:" + a.color);
            dot.ref().on_click(bind(&app_, &DenderApp::set_accent, name));
        }
    }
}

}  // namespace dender
