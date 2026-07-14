#include "game_editor.h"

#include "game_editor_styles.h"

#include <cerrno>
#include <cstdio>
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

// Channel display text for live widget write-back: two decimals,
// trailing zeros trimmed (what the combos themselves show).
std::string format_channel(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
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

    // The real 3D viewport: an e3d scene mirrored from the document,
    // orbit camera, click picking and the transform gizmo (driven by
    // the tool rail).
    // The Config defaults reproduce this app's paint names, canvas/stats
    // classes and the 3-type (mesh/light/spline) node mapping exactly, so
    // an empty Config keeps game-editor output identical.
    viewport_ = std::make_unique<viewport3d::Viewport3D>();
    viewport_->attach(app_, ctx_);
    viewport_->set_tool(tool_);
    app_.on_event([this](const affineui::Event& ev,
                         const std::vector<affineui::Document::HoverInfo>&
                             chain) {
        return viewport_->handle_event(ev, chain);
    });
    // Track gizmo drags live: write the moving transform straight into
    // the inspector's channel widgets (in place, no view rebuild, no
    // change echo). The drag's end pushes the undo command, whose
    // stack-changed reload settles the final values.
    viewport_->on_node_changed = [this](const std::string& id) {
        if (!viewport_ || selected_id() != id) return;
        e3d::Object3D* node = viewport_->node_of(id);
        if (node == nullptr) return;
        const affineui::ObjectClass& cls = get_class(*node);
        static constexpr const char* kPrefix[3] = {"position", "rotation",
                                                   "scale"};
        static constexpr const char* kAxis[3] = {".x", ".y", ".z"};
        for (const char* prefix : kPrefix) {
            for (int i = 0; i < 3; ++i) {
                const auto value = cls.get(node, std::string(prefix) + kAxis[i]);
                if (const double* d = std::get_if<double>(&value)) {
                    app_.set_widget_value(
                        std::string(prefix) + "-" + std::to_string(i),
                        format_channel(*d));
                }
            }
        }
    };

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
    density_ = prefs_.get_string("ui.density", "compact");
    tool_ = ws_.get_string("last_tool", "select");
}

void GameEditor::save_settings() {
    prefs_.set_string("ui.accent", accent_);
    prefs_.set_string("ui.density", density_);
    prefs_.save(kPrefsPath);
    ws_.set_string("last_tool", tool_);
    ws_.save(kWorkspacePath);
}

void GameEditor::set_density(std::string_view density) {
    if (density_ == density) return;
    density_ = std::string(density);
    save_settings();
    build_native_menu();  // the check mark moved
    reload();
}

void GameEditor::set_accent(std::string_view accent) {
    if (accent_ == accent) return;
    accent_ = std::string(accent);
    save_settings();
    build_native_menu();  // the check mark moved
    reload();
}

affineui::App::Config GameEditor::config() {
    affineui::App::Config cfg;
    cfg.title = "AffineUI - Decius game editor";
    cfg.width = 1440;
    cfg.height = 900;
    cfg.clear_color = affineui::Color{0x14, 0x16, 0x1c, 0xff};
    cfg.high_dpi = true;
    // A DCC tool owns the whole window: no OS title bar, the drawn menubar IS
    // the title bar. It drags the window and pads itself around the traffic
    // lights automatically (--affineui-titlebar-inset-*), so nothing here has
    // to know where they are. Menus go to the macOS system bar; see set_menu()
    // in run(), and the drawn File/Edit/View triggers hide themselves there.
    cfg.titlebar = affineui::TitleBarStyle::HiddenInset;
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
    build_native_menu();
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
    // Save-on-exit. The one veto point: the traffic-light close button, Cmd-Q,
    // and the menu's Quit all run this before anything tears down.
    app_.on_close_request([this] {
        if (!ctx_.document().dirty()) return true;
        std::fprintf(stderr,
                     "[game_editor] unsaved changes — a real app would prompt "
                     "here and return false to cancel the quit\n");
        return true;
    });
    return app_.run();
}

// The application menu. Declared once, in the platform-neutral model: on macOS
// this becomes the system menu bar at the top of the screen, and the drawn
// File/Edit/View triggers in our own menubar hide themselves because these are
// the same menus. Roles (Quit, Undo, Cut/Copy/Paste, Minimize) carry their own
// standard labels and accelerators — we don't restate them.
void GameEditor::build_native_menu() {
    using affineui::MenuItem;
    using affineui::MenuRole;

    app_.set_menu({
        MenuItem::sub("", {MenuItem::role(MenuRole::About),
                           MenuItem::separator(),
                           MenuItem::role(MenuRole::Services),
                           MenuItem::separator(),
                           MenuItem::role(MenuRole::Hide),
                           MenuItem::role(MenuRole::HideOthers),
                           MenuItem::separator(),
                           MenuItem::role(MenuRole::Quit)}),
        MenuItem::sub(
            "File",
            {
                MenuItem::item("New Scene", "CmdOrCtrl+N", [this] { reload(); }),
                MenuItem::item("Open Scene…", "CmdOrCtrl+O"),
                MenuItem::item("Save Scene", "CmdOrCtrl+S"),
                MenuItem::separator(),
                MenuItem::sub("Export As", {MenuItem::item("glTF"),
                                            MenuItem::item("FBX"),
                                            MenuItem::item("USD")}),
            }),
        MenuItem::sub(
            "Edit",
            {
                // Undo/Redo are the APP's, not the document's: they drive the
                // editor's command stack (scene edits, gizmo drags), so they
                // stay explicit items rather than the Undo/Redo roles, which
                // replay into the focused DOM control.
                MenuItem::item("Undo", "CmdOrCtrl+Z", [this] { undo(); }),
                MenuItem::item("Redo", "Shift+CmdOrCtrl+Z", [this] { redo(); }),
                MenuItem::separator(),
                // The clipboard group IS the document's. As roles they carry the
                // platform's labels and accelerators and act on whatever text
                // control has focus (the inspector's name and notes fields) — a
                // native menu item owns its keystroke, so without these Cmd-C in
                // a focused field would do nothing at all.
                MenuItem::role(MenuRole::Cut),
                MenuItem::role(MenuRole::Copy),
                MenuItem::role(MenuRole::Paste),
                MenuItem::role(MenuRole::SelectAll),
                MenuItem::separator(),
                MenuItem::item("Duplicate", "CmdOrCtrl+D",
                               [this] { duplicate_selected(); }),
                MenuItem::item("Delete", "Delete",
                               [this] { delete_selected(); }),
            }),
        MenuItem::sub("View", build_view_menu()),
        MenuItem::sub("Window", MenuItem::window_menu()),
    });
}

// The View menu carries state, so it is rebuilt and re-set whenever that state
// changes — the check marks and the accent swatches ARE the state. This is the
// same shape as the drawn menu's custom rows: checked items and color chips,
// expressed as data, so they survive into a native NSMenu as real check marks
// and real images instead of needing a custom-drawn view.
std::vector<affineui::MenuItem> GameEditor::build_view_menu() {
    using affineui::MenuItem;
    std::vector<MenuItem> density;
    for (const auto d : {affineui::decius::density::compact,
                         affineui::decius::density::comfortable,
                         affineui::decius::density::spacious}) {
        density.push_back(MenuItem::check(std::string(d), density_ == d, {},
                                          [this, d] { set_density(d); }));
    }
    // The drawn menu paints its swatch with the framework's own --dcs-accent,
    // so the color always tracks decius.css. A native NSMenu cannot read CSS,
    // so the RGB has to cross the boundary as data — these are the bundle's
    // [data-dcs-accent=…] values. That duplication is the real cost of going
    // native, and it is confined to this table.
    struct Swatch {
        const char*     name;
        affineui::Color color;
    };
    static constexpr Swatch kAccents[] = {
        {"cyan", {0x00, 0xb8, 0xd4, 0xff}},
        {"teal", {0x2f, 0x9c, 0x93, 0xff}},
        {"green", {0x3d, 0xd6, 0x8a, 0xff}},
        {"orange", {0xff, 0x8a, 0x3a, 0xff}},
        {"purple", {0x84, 0x66, 0xcf, 0xff}},
        {"violet", {0x8b, 0x6d, 0xff, 0xff}},
    };
    std::vector<MenuItem> accents;
    for (const auto& [name, color] : kAccents) {
        MenuItem item = MenuItem::check(name, accent_ == name, {},
                                        [this, n = std::string(name)] {
                                            set_accent(n);
                                        });
        item.swatch = color;
        accents.push_back(std::move(item));
    }
    return {MenuItem::sub("Density", std::move(density)),
            MenuItem::sub("Accent", std::move(accents))};
}

void GameEditor::reload() {
    // Mirror document/selection changes into the 3D scene before the
    // rebuilt view paints (selection box, gizmo target, added/removed
    // objects all follow the app state).
    if (viewport_) {
        viewport_->sync_document();
        viewport_->sync_selection();
    }
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
    if (viewport_) viewport_->set_tool(tool_);
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

void GameEditor::set_debug_scope(std::string_view scope) {
    debug_scope_ = std::string(scope);
    if (debug_scope_ == "full" || debug_scope_ == "none") {
        debug_scope_.clear();
    }
}

// ── View ──────────────────────────────────────────────────────────────────

affineui::View GameEditor::build() {
    View v{affineui::ViewTheme::Decius};
    v.set_framework_version(kDeciusVersion);  // pin the Decius version we target
    // Flat is the modern default look (3D/skeuomorphic is an opt-in variant).
    v.selector(affineui::decius::selector::style, affineui::decius::style::flat);
    // Density is a live preference (View menu) so every density can be
    // eyeballed against the decius-css reference samples. Spacing comes
    // ENTIRELY from the framework bundle's density variables (the vec gap is
    // var(--dcs-s-1)) — nothing app- or engine-side defines it.
    v.selector(affineui::decius::selector::density, density_);
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

    const bool scoped = !debug_scope_.empty();
    const bool show_tree = !scoped || debug_scope_ == "tree";
    const bool show_inspector =
        !scoped || debug_scope_ == "inspector" || debug_scope_ == "color" ||
        debug_scope_ == "vector" || debug_scope_ == "checkbox";
    const bool show_tearout = !scoped || debug_scope_ == "tearout";

    v.begin();
    {
        // App shell: a fixed inset:0 column (menubar / work area / status bar).
        auto app = v.container("ge-app", "app");
        if (!scoped) {
            build_menubar(v);
        }

        // The work area is a declarative dock: the viewport is the center
        // document, with Hierarchy / Inspector / Assets docked around it. The
        // engine resolves the split tree and inserts splitters — declaration
        // order does not matter.
        v.document_view("workarea", [&](View& dv) {
            dv.document([&](View& doc) { build_viewport(doc); }, "Lit View",
                        "cube")
                .toolbar([&](View& tb) { build_viewport_toolbar(tb); });
            if (show_tree) {
                dv.dockpanel("Hierarchy",
                             affineui::DockLocation::docked(affineui::Dock::Left, 260),
                             [&](View& p) { build_outliner(p); }, "layers");
            }
            if (show_inspector) {
                dv.dockpanel("Inspector",
                             affineui::DockLocation::docked(affineui::Dock::Right, 320),
                             [&](View& p) { build_inspector(p); }, "cog");
            }
            if (show_tearout) {
                auto assets = dv.dockpanel(
                "Assets",
                affineui::DockLocation::docked(affineui::Dock::Bottom, 120),
                [&](View& p) { build_assets(p); }, "image");
            // A second tab in the bottom pane — exercises tab switching (click a
            // tab to swap the pane's body). Co-tabbed into Assets via Dock::Tab.
            dv.dockpanel("Console",
                         affineui::DockLocation::tab().in(assets),
                         [&](View& p) { build_console(p); }, "file");
            }
        });

        if (!scoped) {
            build_statusbar(v);
        }
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

    v.menu_button("View", [&](View& m) {
        // Appearance selectors — density and key color come straight from the
        // framework bundle's selector variables; the app only picks which one.
        m.submenu("Density", [&](View& s) {
            for (const auto d : {affineui::decius::density::compact,
                                 affineui::decius::density::comfortable,
                                 affineui::decius::density::spacious}) {
                s.menu_item(d, density_ == d ? "check" : "", {},
                            std::string("mi-dens-") + std::string(d))
                    .on_click([this, d] { set_density(d); });
            }
        }, "layers", "mi-density");
        m.submenu("Accent", [&](View& s) {
            for (const auto a : {"cyan", "teal", "green", "orange", "purple",
                                 "violet"}) {
                auto item =
                    s.menu_item_custom(std::string("mi-accent-") + a);
                {
                    // Gutter: a swatch chip painted by the framework's OWN
                    // accent variable — `data-dcs-accent` on the chip scopes
                    // the bundle's [data-dcs-accent=...] override to it, so
                    // the color always tracks decius.css, never the app.
                    auto gutter =
                        s.element("span", "dcs-menu__icon", "__icon");
                    auto chip = s.element("span", "dcs-swatch", "__chip");
                    chip.attr("data-dcs-accent", a);
                    chip.attr("style",
                              "background:var(--dcs-accent);width:12px;"
                              "height:12px;border-radius:3px;"
                              "align-self:center");
                }
                auto lbl =
                    s.element("span", "dcs-menu__label-text", "__label");
                lbl.text(a);
                if (accent_ == a) {
                    auto check =
                        s.element("span", "dcs-menu__shortcut", "__check");
                    s.element("i", "di di-check", "__check-glyph");
                }
                item.ref().on_click([this, a] { set_accent(a); });
            }
        }, "palette", "mi-accent");
    }, "mb-view");

    v.menu_button("Build", [&](View& m) {
        m.menu_item("Build Lighting", "bolt", {}, "mi-light");
        m.menu_item("Bake Navmesh", "grid", {}, "mi-navmesh");
    }, "mb-build");

    // The scene being edited, centered in the bar — this strip is the window's
    // title bar now, and that is what a title bar shows.
    v.document_title(std::string(ctx_.document().title()), "mb-title");

    v.menu_spacer("mb-spacer");
    v.menu_meta(ctx_.document().dirty() ? "Edited" : "", "mb-meta");
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
    // The tree is a data-dcs-select="single" box: clicking a row is a
    // SELECTION interaction that emits a widget CHANGE on the tree with the
    // selected row's data-dcs-value — not a button click. So bind the
    // tree's on_change (a per-row on_click never fires here) and carry the
    // object id as each row's value.
    auto tree = v.tree("scene-tree");
    tree.ref().on_change([this](std::string_view value) {
        // data-dcs-select="single" emits exactly one id (empty if the
        // click cleared the selection).
        if (!value.empty()) select_object(value);
    });
    for (const auto& obj : ctx_.document().objects()) {
        affineui::TreeRowOptions opts;
        opts.depth = obj.depth;
        opts.selected = ctx_.selection().contains(obj.id);
        opts.icon = type_icon(obj.type);
        opts.expandable = obj.type == "group";  // groups can hold children
        opts.expanded = true;
        opts.meta_icon = obj.type == "mesh" ? "eye" : std::string_view{};
        v.tree_row(obj.name, opts, "row-" + obj.id)
            .attr("data-dcs-value", obj.id);
    }
}

void GameEditor::build_viewport(View& v) {
    // Fills the center document pane body (the dock engine emits the pane and
    // marks it a data-dcs-float-host). The scene itself is a real 3D render:
    // the e3d engine draws into an offscreen GPU target that the custom-paint
    // canvas below composites (orbit camera, picking, gizmo — see
    // viewport3d::Viewport3D).
    auto canvas = v.container("ge-vp-canvas", "vp-canvas");
    canvas.attr("data-dcs-float-host", "");
    if (viewport_) viewport_->build(v);
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

        // Continuous controls split preview from commit: the live
        // stream (on_change) drives the 3D material directly — no doc
        // command, no view rebuild, so the drag survives — and the
        // gesture end (on_commit) pushes the one undoable property
        // command, whose reload settles everything.
        const double roughness =
            std::get<double>(doc.property(id, "roughness", app::PropValue{0.62}));
        v.slider("Roughness", roughness, 0.0, 1.0, "rough")
            .on_change([this, id](std::string_view value) {
                viewport_->set_material_roughness(
                    id, parse_double(value, 0.62));
            })
            .on_commit(bind(this, &GameEditor::set_roughness));

        const std::string tint = std::get<std::string>(
            doc.property(id, "tint", app::PropValue{std::string{"#4d9fff"}}));
        v.colorfield("Tint", tint, "tint")
            .on_change([this, id](std::string_view value) {
                viewport_->set_material_tint(id, value);
            })
            .on_commit(bind(this, &GameEditor::set_tint));

        const bool shadows =
            std::get<bool>(doc.property(id, "castShadows", app::PropValue{true}));
        v.checkbox("Cast shadows", shadows, "shadows")
            .on_change(bind(this, &GameEditor::set_cast_shadows));
    }

    // ── Transform / Object — reflection-driven fields ───────────────────
    // The selected object's 3D node is inspected through its ObjectClass
    // (e3d get_class): "<prefix>.x/.y/.z" double triples render as
    // 3-channel vec fields in the Transform foldout, standalone bools as
    // checkboxes in an Object foldout. Edits write back through the same
    // reflection mediator via the viewport's undoable property setter;
    // the stack-changed handler then reloads the UI with fresh values.
    if (viewport_) {
        if (e3d::Object3D* node = viewport_->node_of(id)) {
            const affineui::ObjectClass& cls = get_class(*node);
            {
                auto fold = v.foldout("Transform", true, "fold-xform");
                auto props = v.container("dcs-props", "xform-props");
                struct VecRow {
                    const char* prefix;
                    const char* label;   // Blender-style display name
                    const char* suffix;  // channel unit suffix
                    double      fallback;
                    double      step;
                    bool        linear;  // constant step/pixel scrub
                };
                static constexpr VecRow kRows[] = {
                    {"position", "Location", " m", 0.0, 0.01, false},
                    // Degrees are a fixed-scale quantity — a constant
                    // step/pixel feels right; magnitude acceleration does
                    // not (180° would scrub 10× faster than 18°).
                    {"rotation", "Rotation", "\xC2\xB0", 0.0, 0.5, true},
                    {"scale", "Scale", "", 1.0, 0.01, false},
                };
                static constexpr const char* kAxis[3] = {".x", ".y", ".z"};
                for (const VecRow& row : kRows) {
                    std::array<double, 3> ch{};
                    for (int i = 0; i < 3; ++i) {
                        ch[static_cast<std::size_t>(i)] = std::get<double>(
                            cls.get(node, std::string(row.prefix) + kAxis[i]));
                    }
                    v.vec(row.label, {"X", "Y", "Z"}, {ch[0], ch[1], ch[2]},
                          row.prefix, row.step, row.linear);
                    for (int i = 0; i < 3; ++i) {
                        auto field = v.find_widget(std::string(row.prefix) +
                                                   "-" + std::to_string(i));
                        if (*row.suffix != '\0') {
                            field.attr("data-suffix", row.suffix);
                        }
                        const std::string prop =
                            std::string(row.prefix) + kAxis[i];
                        const double fallback = row.fallback;
                        // Scrub = live preview on the node; gesture end
                        // (or a typed edit) commits one undo entry.
                        field.on_change(
                            [this, id, prop, fallback](std::string_view value) {
                                viewport_->preview_node_property(
                                    id, prop,
                                    affineui::PropertyValue{
                                        parse_double(value, fallback)});
                            });
                        field.on_commit(
                            [this, id, prop, fallback](std::string_view value) {
                                viewport_->commit_node_property(
                                    id, prop,
                                    affineui::PropertyValue{
                                        parse_double(value, fallback)});
                            });
                    }
                }
            }
            {
                // Every remaining writable bool property, rendered
                // generically (attr::Label supplies the display name).
                auto fold = v.foldout("Object", true, "fold-object");
                auto props = v.container("dcs-props", "object-props");
                for (std::size_t i = 0; i < cls.property_count(); ++i) {
                    const auto prop = cls.property_at(i);
                    if (!prop.valid() || !prop.set ||
                        prop.has<affineui::attr::ReadOnly>()) {
                        continue;
                    }
                    const auto value = prop.get(node);
                    if (!std::holds_alternative<bool>(value)) continue;
                    const std::string key = "obj-" + prop.name;
                    v.checkbox(std::string(prop.display_label()),
                               std::get<bool>(value), key)
                        .on_commit([this, id, name = prop.name](
                                       std::string_view value) {
                            const bool on = value == "true" ||
                                            value == "1" || value == "on";
                            viewport_->commit_node_property(
                                id, name, affineui::PropertyValue{on});
                        });
                }
            }
        }
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
