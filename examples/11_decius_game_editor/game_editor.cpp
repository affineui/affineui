#include "game_editor.h"

#include "game_editor_styles.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace ge {

using affineui::bind;
using affineui::View;

namespace {

// The Decius CSS version this app ships and was tested against. Declared in one
// place: used to load the matching bundle and stamped on the view so the
// renderer/interaction layer knows which Decius version is in play.
constexpr const char* kDeciusVersion = "0.6.2";

double parse_double(std::string_view s, double fallback) {
    // strtod, not std::from_chars: Apple's libc++ leaves the
    // floating-point from_chars overload `= delete`'d.
    if (s.empty()) return fallback;
    std::string tmp(s);
    char* end = nullptr;
    errno = 0;
    double out = std::strtod(tmp.c_str(), &end);
    return (end != tmp.c_str() && errno != ERANGE) ? out : fallback;
}

// A memento command for a single string/number property edit, with drag
// coalescing — exactly the shape a slider or text field wants. (The base
// app::Command is agnostic; this is the snapshot flavour.)
class SetProperty final : public app::Command {
public:
    SetProperty(std::string id, std::string prop, app::PropValue value,
                std::string label)
        : Command("obj.setProperty", std::move(label)),
          id_(std::move(id)),
          prop_(std::move(prop)),
          new_(std::move(value)) {
        args_.set("id", id_).set("prop", prop_);
    }
    void redo(app::Document& doc) override {
        old_ = doc.property(id_, prop_, new_);
        doc.set_property(id_, prop_, new_);
    }
    void undo(app::Document& doc) override {
        doc.set_property(id_, prop_, old_);
    }
    bool merge_with(const app::Command& next) override {
        const auto* o = dynamic_cast<const SetProperty*>(&next);
        if (!o || o->id_ != id_ || o->prop_ != prop_) return false;
        new_ = o->new_;  // absorb newer value, keep our captured old_
        return true;
    }

private:
    std::string    id_;
    std::string    prop_;
    app::PropValue new_;
    app::PropValue old_;
};

app::Object mesh(std::string id, std::string name, std::string type, int depth) {
    app::Object o;
    o.id = std::move(id);
    o.name = std::move(name);
    o.type = std::move(type);
    o.depth = depth;
    o.properties.push_back({"roughness", app::PropValue{0.62}});
    o.properties.push_back({"tint", app::PropValue{std::string{"#4d9fff"}}});
    o.properties.push_back({"castShadows", app::PropValue{true}});
    return o;
}

}  // namespace

GameEditor::GameEditor() : app_(config()) {
    // Seed the localization catalog with the standard command labels.
    app::Localizer::instance().add_locale("en",
                                          app::standard_command_labels_en());

    // Seed a small scene.
    auto& doc = ctx_.document();
    doc.set_title("ruins_arena_03");
    doc.add(mesh("world", "WorldRoot", "group", 0));
    doc.add(mesh("hero", "Hero_mesh_high", "mesh", 1));
    doc.add(mesh("key_light", "KeyLight", "light", 1));
    doc.add(mesh("spline", "SplineRig", "spline", 1));
    ctx_.selection().select("hero");

    // Standard edit commands (undo/redo/delete/duplicate/...) + accelerators.
    app::register_standard_commands(ctx_);

    // Restore durable preferences + last workspace state.
    load_settings();

    // Refresh the UI whenever the document or selection changes.
    doc.set_changed_handler(bind(this, &GameEditor::reload));
    ctx_.selection().set_changed_handler(bind(this, &GameEditor::reload));
    ctx_.stack().set_changed_handler(bind(this, &GameEditor::reload));
}

namespace {
// Where this sample keeps its settings (next to the working dir for the demo;
// a real app would use a platform config dir).
const char* kPrefsPath = "game_editor.prefs.toml";
const char* kWorkspacePath = "game_editor.workspace.toml";
}  // namespace

void GameEditor::load_settings() {
    prefs_.load(kPrefsPath);
    ws_.load(kWorkspacePath);
    accent_ = prefs_.get_string("ui.accent", "cyan");
    tool_ = ws_.get_string("last_tool", "select");
}

void GameEditor::save_settings() {
    prefs_.set_string("ui.accent", accent_);
    prefs_.save(kPrefsPath);
    ws_.set_string("last_tool", tool_);
    ws_.save(kWorkspacePath);
}

affineui::App::Config GameEditor::config() {
    affineui::App::Config cfg;
    cfg.title = "AffineUI - Decius game editor";
    cfg.width = 1440;
    cfg.height = 900;
    cfg.clear_color = affineui::Color{0x14, 0x16, 0x1c, 0xff};
    cfg.high_dpi = true;
    // The build copies frameworks/ next to the exe (so "." works when run from
    // the build dir); "examples" also works when run from the repo root.
    cfg.asset_folders = {".", "examples"};
    // Persist the dock arrangement whenever an interaction changes it, and
    // rebuild so STRUCTURAL changes (a tearoff to a floating panel) re-resolve
    // from the saved placement. Size-only changes (splitter drags) rebuild to
    // the same layout, so this is seamless.
    cfg.on_layout_changed = [this] {
        save_dock_layout();
        reload();
    };
    return cfg;
}

void GameEditor::save_dock_layout() {
    for (const auto& [id, px] : app_.document().dock_pane_sizes()) {
        ws_.set_panel_size(id, px);
    }
    ws_.save(kWorkspacePath);
}

int GameEditor::run() {
    reload();
    // Load styles directly at init (the app template's versioned loader) rather
    // than relying on <link> resolution, which is sensitive to the working
    // directory. The Decius bundle for our declared version plus this app's
    // custom CSS become the user stylesheet.
    std::string bundle_base;
    std::string css = app::read_framework_bundle(affineui::ViewTheme::Decius,
                                                 kDeciusVersion, bundle_base);
    css += "\n";
    css += native_css();
    // Pass the bundle's base URL so its url()s (the decius-icons font, etc.)
    // resolve relative to the sheet — the same way a <link>ed sheet resolves.
    app_.set_stylesheet(css, bundle_base);
    return app_.run();
}

void GameEditor::reload() {
    app_.load_view(build());
}

std::string_view GameEditor::selected_id() const {
    return ctx_.selection().active();
}

const app::Object* GameEditor::selected() const {
    return ctx_.document().find(selected_id());
}

// ── Handlers ────────────────────────────────────────────────────────────────

void GameEditor::select_object(std::string_view id) {
    ctx_.selection().select(id);  // fires changed -> reload
}

void GameEditor::toggle_play() {
    playing_ = !playing_;
    reload();
}

void GameEditor::set_tool(std::string_view tool) {
    tool_ = std::string(tool);
    save_settings();  // remember the active tool across runs
    reload();
}

void GameEditor::rename_selected(std::string_view name) {
    const std::string id(selected_id());
    if (id.empty()) return;
    ctx_.run(std::make_unique<SetProperty>(id, "__name",
                                           app::PropValue{std::string(name)},
                                           "Rename"));
    ctx_.document().rename(id, name);
}

void GameEditor::set_roughness(std::string_view value) {
    const std::string id(selected_id());
    if (id.empty()) return;
    ctx_.stack().begin_coalescing();
    ctx_.run(std::make_unique<SetProperty>(
        id, "roughness", app::PropValue{parse_double(value, 0.62)},
        "Set Roughness"));
    ctx_.stack().end_coalescing();
}

void GameEditor::set_tint(std::string_view value) {
    const std::string id(selected_id());
    if (id.empty()) return;
    ctx_.run(std::make_unique<SetProperty>(
        id, "tint", app::PropValue{std::string(value)}, "Set Tint"));
}

void GameEditor::set_cast_shadows(std::string_view value) {
    const std::string id(selected_id());
    if (id.empty()) return;
    const bool on = (value == "true" || value == "1" || value == "on");
    ctx_.run(std::make_unique<SetProperty>(
        id, "castShadows", app::PropValue{on}, "Toggle Shadows"));
}

// Display-panel showcase controls. These keep local UI state (not document
// properties) — the interaction layer already flips the DOM, so the handlers
// just record the value so a rebuild reflects it.
namespace {
bool truthy(std::string_view v) {
    return v == "true" || v == "1" || v == "on";
}
}  // namespace

void GameEditor::set_blend(std::string_view value) { blend_ = std::string(value); }
void GameEditor::set_visible(std::string_view value) { visible_ = truthy(value); }
void GameEditor::set_wireframe(std::string_view value) { wireframe_ = truthy(value); }
void GameEditor::set_notes(std::string_view value) { notes_ = std::string(value); }

void GameEditor::undo() { ctx_.stack().undo(); }
void GameEditor::redo() { ctx_.stack().redo(); }

void GameEditor::duplicate_selected() { ctx_.run(app::cmd::duplicate); }
void GameEditor::delete_selected() { ctx_.run(app::cmd::delete_); }

// ── View ──────────────────────────────────────────────────────────────────

affineui::View GameEditor::build() {
    View v{affineui::ViewTheme::Decius};
    v.set_framework_version(kDeciusVersion);  // pin the Decius version we target
    // Flat is the modern default look (3D/skeuomorphic is an opt-in variant).
    v.selector(affineui::decius::selector::style, affineui::decius::style::flat);
    v.selector(affineui::decius::selector::density, affineui::decius::density::compact);
    v.selector(affineui::decius::selector::accent, accent_);  // from Preferences

    // Restore saved dock-pane sizes from the workspace: a remembered size wins
    // over the size declared in each panel's DockLocation.
    v.set_dock_size_provider(
        [this](std::string_view id) { return ws_.panel_size(id, 0); });
    // Re-emit the LIVE dock arrangement (drag-to-dock / tearoff surgery) on
    // every rebuild — the declared layout above is only the first-run seed.
    v.set_dock_layout_provider([this] { return app_.document().dock_layout(); });
    // Structural overrides (a panel torn off to floating) survive view rebuilds:
    // resolve_dock re-seeds each panel's placement from the Document's overrides.
    v.set_dock_placement_provider([this](std::string_view id) {
        return app_.document().dock_override(id);
    });
    v.set_dock_active_tab_provider([this](std::string_view id) {
        return app_.document().dock_active_tab(id);
    });

    v.begin();
    {
        // App shell: a fixed inset:0 column (menubar / work area / status bar).
        auto app = v.container("ge-app", "app");
        build_menubar(v);

        // The work area is a declarative dock: the viewport is the center
        // document, with Hierarchy / Inspector / Assets docked around it. The
        // engine resolves the split tree and inserts splitters — declaration
        // order does not matter.
        v.document_view("workarea", [&](View& dv) {
            dv.document([&](View& doc) { build_viewport(doc); }, "Lit View",
                        "cube")
                .toolbar([&](View& tb) { build_viewport_toolbar(tb); });
            dv.dockpanel("Hierarchy",
                         affineui::DockLocation::docked(affineui::Dock::Left, 260),
                         [&](View& p) { build_outliner(p); }, "layers");
            dv.dockpanel("Inspector",
                         affineui::DockLocation::docked(affineui::Dock::Right, 320),
                         [&](View& p) { build_inspector(p); }, "cog");
            auto assets = dv.dockpanel(
                "Assets",
                affineui::DockLocation::docked(affineui::Dock::Bottom, 120),
                [&](View& p) { build_assets(p); }, "image");
            // A second tab in the bottom pane — exercises tab switching (click a
            // tab to swap the pane's body). Co-tabbed into Assets via Dock::Tab.
            dv.dockpanel("Console",
                         affineui::DockLocation::tab().in(assets),
                         [&](View& p) { build_console(p); }, "file");
        });

        build_statusbar(v);
    }
    v.end();
    return v;
}

void GameEditor::build_menubar(View& v) {
    auto bar = v.menu_bar("menubar");
    v.menu_brand("Decius Forge", "decius", "mb-brand");

    // Each menubar button owns its dropdown, declared inline.
    v.menu_button("File", [&](View& m) {
        m.menu_item("New Scene", "file", "Ctrl N", "mi-new")
            .on_click(bind(this, &GameEditor::reload));
        m.menu_item("Open Scene", "folder-open", "Ctrl O", "mi-open");
        m.menu_item("Save Scene", "save", "Ctrl S", "mi-save");
        m.menu_separator("mi-file-sep");
        m.submenu("Export As", [&](View& s) {
            s.menu_item("glTF", "cube", {}, "mi-exp-gltf");
            s.menu_item("FBX", "cube", {}, "mi-exp-fbx");
            s.menu_item("USD", "cube", {}, "mi-exp-usd");
        }, "share", "mi-export");
    }, "mb-file");

    v.menu_button("Edit", [&](View& m) {
        m.menu_item("Undo", "undo", "Ctrl Z", "mi-undo")
            .on_click(bind(this, &GameEditor::undo));
        m.menu_item("Redo", "redo", "Ctrl Y", "mi-redo")
            .on_click(bind(this, &GameEditor::redo));
        m.menu_separator("mi-edit-sep");
        m.menu_item("Duplicate", "copy", "Ctrl D", "mi-dup")
            .on_click(bind(this, &GameEditor::duplicate_selected));
        m.menu_item("Delete", "trash", "Del", "mi-del")
            .on_click(bind(this, &GameEditor::delete_selected));
    }, "mb-edit");

    v.menu_button("Build", [&](View& m) {
        m.menu_item("Build Lighting", "bolt", {}, "mi-light");
        m.menu_item("Bake Navmesh", "grid", {}, "mi-navmesh");
    }, "mb-build");

    v.menu_spacer("mb-spacer");
    v.menu_meta("Scene: " + std::string(ctx_.document().title()), "mb-meta");
}

namespace {
// Map a scene object's type to a Decius icon-font glyph.
std::string_view type_icon(std::string_view type) {
    if (type == "group")  return "cube";
    if (type == "mesh")   return "sphere";
    if (type == "light")  return "light";
    if (type == "spline") return "spline";
    return "dot";
}
}  // namespace

void GameEditor::build_outliner(View& v) {
    // Fills the Hierarchy pane body (the dock engine emits the pane itself).
    auto tree = v.tree("scene-tree");
    for (const auto& obj : ctx_.document().objects()) {
        affineui::TreeRowOptions opts;
        opts.depth = obj.depth;
        opts.selected = ctx_.selection().contains(obj.id);
        opts.icon = type_icon(obj.type);
        opts.expandable = obj.type == "group";  // groups can hold children
        opts.expanded = true;
        opts.meta_icon = obj.type == "mesh" ? "eye" : std::string_view{};
        // Bind the row's id into a zero-arg click handler — safe after destroy.
        v.tree_row(obj.name, opts, "row-" + obj.id)
            .on_click(bind(this, &GameEditor::select_object, obj.id));
    }
}

void GameEditor::build_viewport(View& v) {
    // Fills the center document pane body (the dock engine emits the pane and
    // marks it a data-dcs-float-host). The faux-3D canvas is app-custom content.
    auto canvas = v.container("ge-vp-canvas", "vp-canvas");
    canvas.attr("data-dcs-float-host", "");
    v.container_ref("ge-vp-grid", "vp-grid");
    v.container_ref("ge-cube", "vp-cube");
    {
        auto stats = v.container("ge-vp-stats", "vp-stats");
        v.element("b", {}, "vp-persp").text("User Perspective");
        v.element_ref("br", {}, "vp-br");
        v.text("(1) Collection | " + std::string(selected() ? selected()->name : ""),
               "vp-sel");
    }
    // Floating tool rail inside the viewport (a tear-off-style floating toolbar).
    {
        affineui::FloatingToolbarOptions rail_opts;
        rail_opts.vertical = true;
        rail_opts.small = true;
        rail_opts.drag_bounds = ".ge-vp-canvas";
        rail_opts.position = "left:8px;top:8px";
        auto rail = v.floating_toolbar(rail_opts, "toolrail");
        for (const char* tool : {"cross-target", "move", "rotate", "scale"}) {
            auto btn = v.icon_button(tool, std::string("tool-") + tool);
            btn.cls("dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost");
            if (tool_ == tool) btn.attr("aria-pressed", "true");
            btn.on_click(bind(this, &GameEditor::set_tool, std::string(tool)));
        }
    }
}

void GameEditor::build_viewport_toolbar(View& v) {
    // The document pane's tab toolbar (declared via the document handle's
    // .toolbar()): a mode selector, the View/Add action menus, then the gizmo
    // toggle and shading-mode group pushed to the right. App-specific viewport
    // chrome composed from the toolbar primitives.

    // Mode selector — a boxed select-button that opens a menu.
    {
        auto sel = v.container("dcs-select dcs-select--btn", "vp-mode");
        sel.attr("data-dcs-toggle", "menu")
            .attr("data-dcs-target", "#vp-mode-menu")
            .attr("data-dcs-tip", "Mode");
        v.element("span", "dcs-select__label", "vp-mode-label")
            .text("Object Mode");
        auto caret = v.container("dcs-select__caret", "vp-mode-caret");
        v.element_ref("i", "di di-chevron-down", "vp-mode-chev");
    }
    v.menu("vp-mode-menu", [&](View& m) {
        m.menu_item("Object Mode", {}, {}, "mode-object");
        m.menu_item("Edit Mode", {}, {}, "mode-edit");
        m.menu_item("Sculpt Mode", {}, {}, "mode-sculpt");
    });

    v.toolbar_separator("vp-sep");

    // View / Add action menus.
    v.menu_button("View", [&](View& m) {
        m.menu_item("Frame Selected", "cross-target", "Num .", "view-frame");
        m.menu_item("Frame All", "cube", "Home", "view-all");
    }, "vp-view");
    v.menu_button("Add", [&](View& m) {
        m.menu_item("Cube", "cube", {}, "add-cube");
        m.menu_item("Light", "light", {}, "add-light");
        m.menu_item("Camera", "camera", {}, "add-camera");
    }, "vp-add");

    // Spacer pushes the tool buttons to the trailing edge.
    v.element_ref("span", "dcs-toolbar__spacer", "vp-spacer");

    // Gizmo toggle.
    v.icon_button("gizmo", "vp-gizmo")
        .cls("dcs-btn dcs-btn--icon dcs-btn--ghost")
        .attr("aria-pressed", "true")
        .attr("data-dcs-tip", "Gizmo");

    // Shading-mode group (wireframe / solid / rendered).
    {
        auto grp = v.container("dcs-btn-group", "vp-shading");
        grp.attr("data-dcs-tip", "Shading");
        struct Shade { const char* icon; bool on; };
        int i = 0;
        for (const Shade& s : {Shade{"view-wire", false}, Shade{"view-solid", true},
                               Shade{"view-render", false}}) {
            auto b = v.icon_button(s.icon, "vp-shade-" + std::to_string(i++));
            b.cls("dcs-btn dcs-btn--icon").attr("data-dcs-radio", "shade");
            if (s.on) b.attr("aria-pressed", "true");
        }
    }
}

void GameEditor::build_inspector(View& v) {
    // Fills the Inspector pane body (the dock engine emits the pane).
    const app::Object* obj = selected();
    const std::string id(selected_id());
    const std::string name = obj ? obj->name : std::string{"(nothing selected)"};
    const auto& doc = ctx_.document();

    // Datablock header: type icon + editable name + browse.
    {
        auto db = v.container("dcs-row dn-datablock", "insp-datablock");
        db.attr("style", "gap:var(--dcs-s-2);align-items:center;padding:var(--dcs-s-2)");
        v.element_ref("i",
                      "di di-" + std::string(type_icon(obj ? obj->type : "")),
                      "insp-db-icon")
            .attr("style", "color:var(--dcs-accent)");
        // A bare name field (not a labeled dcs-field — this is the datablock's
        // inline title input).
        v.element_ref("input", "dcs-input", "insp-name")
            .attr("type", "text").attr("value", name).attr("style", "flex:1")
            .on_change(bind(this, &GameEditor::rename_selected));
        v.icon_button("more-h", "insp-browse");
    }

    auto foldouts = v.container("dcs-foldouts", "insp-foldouts");

    // ── Material — slider / color field / checkbox components ───────────
    {
        auto fold = v.foldout("Material", true, "fold-material");
        auto props = v.container("dcs-props", "material-props");

        const double roughness =
            std::get<double>(doc.property(id, "roughness", app::PropValue{0.62}));
        v.slider("Roughness", roughness, 0.0, 1.0, "rough")
            .on_change(bind(this, &GameEditor::set_roughness));

        const std::string tint = std::get<std::string>(
            doc.property(id, "tint", app::PropValue{std::string{"#4d9fff"}}));
        v.colorfield("Tint", tint, "tint")
            .on_change(bind(this, &GameEditor::set_tint));

        const bool shadows =
            std::get<bool>(doc.property(id, "castShadows", app::PropValue{true}));
        v.checkbox("Cast shadows", shadows, "shadows")
            .on_change(bind(this, &GameEditor::set_cast_shadows));
    }

    // ── Transform — a 3-channel vector field ────────────────────────────
    {
        auto fold = v.foldout("Transform", true, "fold-xform");
        auto props = v.container("dcs-props", "xform-props");
        v.vec("Location", {"X", "Y", "Z"}, {12.0, 4.2, -8.5}, "loc");
    }

    // ── Display — showcases the remaining channel controls: a multi-button
    // (segmented group), a checkbox, an on/off switch, and a text area ──────
    {
        auto fold = v.foldout("Display", true, "fold-display");
        auto props = v.container("dcs-props", "display-props");
        v.button_group("Blend", {"Normal", "Add", "Multiply"}, blend_, "blend")
            .on_change(bind(this, &GameEditor::set_blend));
        v.checkbox("Visible", visible_, "visible")
            .on_change(bind(this, &GameEditor::set_visible));
        v.toggle("Wireframe", wireframe_, "wireframe")
            .on_change(bind(this, &GameEditor::set_wireframe));
        v.textarea("Notes", notes_, 3, "notes")
            .on_change(bind(this, &GameEditor::set_notes));
    }

    {
        auto row = v.container("dcs-btn-row", "insp-actions");
        row.attr("style", "padding:var(--dcs-s-3);gap:var(--dcs-s-2)");
        v.button("Duplicate", false, "insp-dup")
            .on_click(bind(this, &GameEditor::duplicate_selected));
        v.button("Delete", false, "insp-del")
            .on_click(bind(this, &GameEditor::delete_selected));
    }
}

void GameEditor::build_assets(View& v) {
    // Fills the Assets pane body (the dock engine emits the pane).
    auto strip = v.container("ge-asset-strip", "asset-strip");
    struct Asset { const char* label; const char* icon; };
    for (const Asset& a : {Asset{"Hero", "cube"}, Asset{"Albedo", "image"},
                           Asset{"Rig", "bone"}, Asset{"Shot A", "camera"}}) {
        auto card = v.container("ge-asset", std::string("asset-") + a.label);
        v.element_ref("i", std::string("di di-") + a.icon,
                      std::string("asset-icon-") + a.label);
        v.paragraph(a.label, "ge-asset__label",
                    std::string("asset-label-") + a.label);
    }
}

void GameEditor::build_console(View& v) {
    // The second tab of the bottom pane — a simple log so clicking the
    // Assets / Console tabs visibly swaps the pane body.
    auto log = v.container("ge-console", "console-log");
    log.attr("style",
             "padding:var(--dcs-s-3);display:flex;flex-direction:column;"
             "gap:var(--dcs-s-1);font-size:12px");
    struct Line { const char* cls; const char* msg; };
    int i = 0;
    for (const Line& l : {
             Line{"dcs-statusbar__item--ok", "Scene 'ruins_arena_03' loaded — 3 objects"},
             Line{"", "Baked lighting cache is up to date"},
             Line{"", "Hero_mesh_high: 2 ngons triangulated on import"},
             Line{"dcs-statusbar__item--ok", "Ready"}}) {
        v.element("div", l.cls, "cl-" + std::to_string(i++)).text(l.msg);
    }
}

void GameEditor::build_statusbar(View& v) {
    auto bar = v.status_bar("statusbar");
    v.element("span", "dcs-statusbar__item dcs-statusbar__item--ok", "st-ready")
        .text("READY");
    v.element("span", "dcs-statusbar__item", "st-fps").text("60.0 FPS");
    v.element_ref("span", "dcs-statusbar__spacer", "st-spacer");
    // Undo/redo state reflected live from the command stack.
    const auto undo_state = ctx_.registry().state(app::cmd::undo, ctx_);
    v.element("span", "dcs-statusbar__item dcs-statusbar__item--accent", "st-undo")
        .text(undo_state.enabled ? "Undo ready" : "Nothing to undo");
}

}  // namespace ge
