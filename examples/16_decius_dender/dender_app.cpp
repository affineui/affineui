#include "dender_app.h"

#include "dender_components.h"
#include "dender_styles.h"
#include "dender_view.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dender {

namespace {

const char* kPrefsPath = "dender.prefs.toml";
const char* kWorkspacePath = "dender.workspace.toml";

// App::set_view/rebuild_view — the persistent-View reconcile fast path.
constexpr bool kUseReconcileFastPath = true;

constexpr double kPi = 3.14159265358979323846;

// Channel display text for the live inspector write-back: two decimals,
// trailing zeros trimmed (what the combos themselves show).
std::string format_channel(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

bool chain_has_class(const std::vector<affineui::Document::HoverInfo>& chain,
                     std::string_view cls,
                     const affineui::Document::HoverInfo** out = nullptr) {
    for (const auto& info : chain) {
        for (const auto& c : info.classes) {
            if (c == cls) {
                if (out != nullptr) *out = &info;
                return true;
            }
        }
    }
    return false;
}

// ── DENDER-local scene mutation over app::Document ───────────────────────────
// The uniquify / spawn / child re-home / x+0.5 duplicate semantics that used to
// live on DenderDocument, reimplemented as helpers over the shared document.

std::string mint_id(int& counter) {
    return "obj_" + std::to_string(++counter);
}

app::Object* find_object(app::Document& doc, std::string_view id) {
    return doc.find(id);
}

}  // namespace

// ── Undoable scene commands (DENDER-local, editing ctx_.document()) ───────────

namespace {

/// Add a catalog primitive. Redo mints (or on redo-after-undo restores) the
/// object; undo removes it. Selection is driven separately by the app.
class AddObjectCmd final : public app::Command {
public:
    AddObjectCmd(app::Selection& sel, int& counter, std::string type)
        : Command("dender.addObject", "Add " + type),
          sel_(sel),
          counter_(counter),
          type_(std::move(type)) {}

    void redo(app::Document& doc) override {
        prev_active_ = std::string(sel_.active());
        if (find_primitive(type_) == nullptr) return;
        if (id_.empty()) id_ = mint_id(counter_);
        app::Object o;
        o.id = id_;
        o.type = type_;
        o.name = name_.empty() ? unique_name(doc, type_, id_) : name_;
        name_ = o.name;  // stable across redo
        doc.add(std::move(o));
        created_ = true;
        doc.touch();
        sel_.select(id_);
    }
    void undo(app::Document& doc) override {
        doc.remove(id_);
        doc.touch();
        if (!prev_active_.empty() && doc.find(prev_active_) != nullptr) {
            sel_.select(prev_active_);
        } else {
            sel_.clear();
        }
    }
    [[nodiscard]] bool modifies() const override { return created_; }

    [[nodiscard]] const std::string& id() const { return id_; }

private:
    app::Selection& sel_;
    int& counter_;
    std::string type_, name_, id_, prev_active_;
    bool created_{false};
};

/// Duplicate: copy at x+0.5 with a uniquified name (web Shift+D). The x+0.5
/// spawn offset is realized on the e3d node via the app's pending-spawn map.
class DuplicateObjectCmd final : public app::Command {
public:
    DuplicateObjectCmd(app::Selection& sel, int& counter, std::string source,
                       std::function<void(const std::string& id,
                                          const std::string& src)>
                           on_created)
        : Command("dender.duplicateObject", "Duplicate"),
          sel_(sel),
          counter_(counter),
          source_(std::move(source)),
          on_created_(std::move(on_created)) {}

    void redo(app::Document& doc) override {
        prev_active_ = std::string(sel_.active());
        const app::Object* src = doc.find(source_);
        if (src == nullptr) return;
        if (id_.empty()) id_ = mint_id(counter_);
        app::Object o = *src;
        o.id = id_;
        o.name = name_.empty() ? unique_name(doc, src->name, id_) : name_;
        name_ = o.name;
        doc.add(std::move(o));
        created_ = true;
        if (on_created_) on_created_(id_, source_);
        doc.touch();
        sel_.select(id_);
    }
    void undo(app::Document& doc) override {
        doc.remove(id_);
        doc.touch();
        if (!prev_active_.empty() && doc.find(prev_active_) != nullptr) {
            sel_.select(prev_active_);
        } else {
            sel_.clear();
        }
    }
    [[nodiscard]] bool modifies() const override { return created_; }

private:
    app::Selection& sel_;
    int& counter_;
    std::string source_, name_, id_, prev_active_;
    bool created_{false};
    std::function<void(const std::string&, const std::string&)> on_created_;
};

/// Delete: remove the object and re-home its children to its parent (web
/// VP.remove); undo restores the exact object + child parenting.
class DeleteObjectCmd final : public app::Command {
public:
    DeleteObjectCmd(app::Selection& sel, std::string id)
        : Command("dender.deleteObject", "Delete"),
          sel_(sel),
          id_(std::move(id)) {}

    void redo(app::Document& doc) override {
        const app::Object* o = doc.find(id_);
        if (o == nullptr) return;
        object_ = *o;
        // Record the removed object's index for exact restore.
        index_ = 0;
        for (const auto& obj : doc.objects()) {
            if (obj.id == id_) break;
            ++index_;
        }
        was_active_ = std::string(sel_.active());
        rehomed_.clear();
        // Re-home children to the removed object's parent.
        // (Applied by mutating the live objects via rename/reparent below.)
        parent_ = object_.parent;
        removed_ = true;
        // Collect children ids first (removal invalidates references).
        for (const auto& obj : doc.objects()) {
            if (obj.parent == id_) rehomed_.push_back(obj.id);
        }
        doc.remove(id_);
        for (const auto& child : rehomed_) {
            if (app::Object* c = doc.find(child)) c->parent = parent_;
        }
        doc.touch();
        if (was_active_ == id_) sel_.clear();
    }
    void undo(app::Document& doc) override {
        if (!removed_) return;
        // Re-insert (order approximated by index; app::Document has no
        // positional insert, so append then leave — outliner order is by
        // parent walk, not registry index, so this is visually identical).
        doc.add(object_);
        for (const auto& child : rehomed_) {
            if (app::Object* c = doc.find(child)) c->parent = id_;
        }
        doc.touch();
        if (!was_active_.empty() && doc.find(was_active_) != nullptr) {
            sel_.select(was_active_);
        }
    }
    [[nodiscard]] bool modifies() const override { return removed_; }

private:
    app::Selection& sel_;
    std::string id_, parent_, was_active_;
    app::Object object_{};
    std::size_t index_{0};
    std::vector<std::string> rehomed_;
    bool removed_{false};
};

/// Rename (uniquified against the other objects). Returns the applied name.
class RenameObjectCmd final : public app::Command {
public:
    RenameObjectCmd(std::string id, std::string name)
        : Command("dender.renameObject", "Rename"),
          id_(std::move(id)),
          requested_(std::move(name)) {}

    void redo(app::Document& doc) override {
        const app::Object* o = doc.find(id_);
        if (o == nullptr) return;
        old_ = o->name;
        applied_ = unique_name(doc, requested_.empty() ? o->type : requested_,
                               id_);
        doc.rename(id_, applied_);
        doc.touch();
    }
    void undo(app::Document& doc) override {
        doc.rename(id_, old_);
        doc.touch();
    }
    [[nodiscard]] bool modifies() const override {
        return !applied_.empty() && applied_ != old_;
    }

private:
    std::string id_, requested_, old_, applied_;
};

}  // namespace

// ── Construction ─────────────────────────────────────────────────────────────

DenderApp::DenderApp() : app_(make_config()) {
    load_settings();
    seed_scene();
    app::register_standard_commands(ctx_);
    bind_keys();

    // Any document / selection / stack change refreshes the whole view (and
    // mirrors the document into the 3D scene first — see reload()).
    ctx_.document().set_changed_handler(affineui::bind(this, &DenderApp::reload));
    ctx_.selection().set_changed_handler(affineui::bind(this, &DenderApp::reload));
    ctx_.stack().set_changed_handler(affineui::bind(this, &DenderApp::reload));

    // The shared reusable 3D viewport, configured with DENDER's DOM classes,
    // paint names, and a DENDER catalog -> e3d geometry factory.
    viewport3d::Viewport3D::Config cfg;
    cfg.scene_paint = "dender.scene";
    cfg.nav_paint = "dender.navball";
    cfg.canvas_class = "dn-vp-canvas";   // the float-host container
    cfg.stats_class = "dn-vp-stats";     // the click-through overlay
    cfg.scene_canvas_class = "dn-vp-3dcanvas";
    cfg.scene_canvas_key = "dn-vp-3d";
    cfg.nav_canvas_class = "dn-vp-navball";
    cfg.nav_canvas_key = "dn-vp-nav";
    cfg.make_node = [this](const app::Object& obj,
                           std::size_t /*index*/) -> e3d::ObjectPtr {
        return make_scene_node(obj);
    };
    viewport_ = std::make_unique<viewport3d::Viewport3D>(std::move(cfg));

    // Live inspector tracking (game_editor pattern): while a gizmo drag moves
    // the active object, write the changing transform straight into the
    // inspector's Transform channel widgets — in place, no view rebuild, no
    // change echo. The drag's end commits the undo entry, whose stack-changed
    // reload settles the final values. The inspector keys its vec channels
    // "loc/rot/scl-<axis>" (see dender_view.cpp), mapping to the reflection
    // prefixes position/rotation/scale.
    viewport_->on_node_changed = [this](const std::string& id) {
        if (!viewport_ || ctx_.selection().active() != id) return;
        e3d::Object3D* node = viewport_->node_of(id);
        if (node == nullptr) return;
        const affineui::ObjectClass& cls = get_class(*node);
        struct Chan { const char* prefix; const char* key; };
        static constexpr Chan kChans[3] = {
            {"position", "loc"}, {"rotation", "rot"}, {"scale", "scl"}};
        static constexpr const char* kAxis[3] = {".x", ".y", ".z"};
        for (const Chan& c : kChans) {
            for (int i = 0; i < 3; ++i) {
                const auto value = cls.get(node, std::string(c.prefix) + kAxis[i]);
                if (const double* d = std::get_if<double>(&value)) {
                    const std::string channel =
                        std::string(c.key) + "-" + std::to_string(i);
                    // Write both mini-inspectors' channels: the main inspector
                    // Transform ("insp-loc-0" …) and the Item N-panel ("np-loc-0"
                    // …). set_widget_value no-ops for whichever isn't present.
                    app_.set_widget_value("insp-" + channel, format_channel(*d));
                    app_.set_widget_value("np-" + channel, format_channel(*d));
                }
            }
        }
    };

    app_.on_event([this](const affineui::Event& ev,
                         const std::vector<affineui::Document::HoverInfo>& chain) {
        return handle_event(ev, chain);
    });

    viewport_->attach(app_, ctx_);
    viewport_->set_tool(ui_.active_tool);
}

void DenderApp::seed_scene() {
    // The web sample's exact initial scene (viewport.js boot): Cube at y=0.8
    // (active), a point light named "Light", and the camera. TRS lives on the
    // e3d node; the document carries id / name / type / parent only.
    auto& doc = ctx_.document();
    doc.set_title("untitled.blend");
    auto add = [&](std::string_view type, std::string_view name) {
        app::Object o;
        o.id = mint_id(id_counter_);
        o.type = std::string(type);
        o.name = std::string(name);
        return doc.add(std::move(o)).id;
    };
    const std::string cube = add("Cube", "Cube");
    add("Point Light", "Light");
    add("Camera", "Camera");
    ctx_.selection().select(cube);
}

// Regenerate a mesh primitive's geometry at the requested BASE dimensions
// (the Item panel's Dimensions edit = mesh rebuild, not a scale). Returns
// null for non-mesh types (lights/camera/empty have no editable dimensions).
// Round primitives map dimensions to their radius/height parameters (X/Z ->
// radius from the diameter, Y -> height), so a UV Sphere with dims 2,2,2 is
// radius 1; a Cylinder 2,3,2 is radius 1 height 3.
e3d::GeometryPtr make_primitive_geometry(std::string_view type,
                                         const e3d::Vec3& dims) {
    using namespace e3d;
    const float dx = std::max(dims.x, 1e-3f);
    const float dy = std::max(dims.y, 1e-3f);
    const float dz = std::max(dims.z, 1e-3f);
    if (type == "Cube") return make_box(dx, dy, dz);
    if (type == "UV Sphere") return make_sphere(dx * 0.5f, 24, 16);
    if (type == "Icosphere") return make_icosahedron(dx * 0.5f, 1);
    if (type == "Cylinder")
        return make_cylinder(dx * 0.5f, dz * 0.5f, dy, 24);
    if (type == "Cone") return make_cone(dx * 0.5f, dy, 24);
    if (type == "Torus") return make_torus(dx * 0.5f, dx * 0.15f, 12, 24);
    if (type == "Plane") return make_plane(dx, dz);
    return nullptr;
}

// ── DENDER catalog -> e3d node factory ───────────────────────────────────────

e3d::ObjectPtr DenderApp::make_scene_node(const app::Object& obj) {
    using namespace e3d;
    const Primitive* prim = find_primitive(obj.type);
    const dender::Vec3 spawn = prim != nullptr ? prim->spawn : dender::Vec3{};

    // A pending spawn (duplicate x+0.5, or a restored transform) wins over the
    // catalog default.
    dender::Vec3 pos = spawn;
    e3d::Vec3 rot{}, scl{1.0f, 1.0f, 1.0f};
    if (const auto it = pending_spawns_.find(obj.id);
        it != pending_spawns_.end()) {
        pos = it->second.position;
        rot = it->second.rotation;
        scl = it->second.scale;
        pending_spawns_.erase(it);
    }
    const e3d::Vec3 e_pos{static_cast<float>(pos.x), static_cast<float>(pos.y),
                          static_cast<float>(pos.z)};

    e3d::ObjectPtr node;
    // Meshes -> lit-solid standard material.
    if (obj.type == "Cube") {
        node = std::make_shared<Mesh>(make_box(1.6f, 1.6f, 1.6f),
                                      Material::standard(0x9aa1ad, 0.55f, 0.05f));
    } else if (obj.type == "UV Sphere") {
        node = std::make_shared<Mesh>(make_sphere(0.9f, 24, 16),
                                      Material::standard(0x9aa1ad, 0.5f, 0.05f));
    } else if (obj.type == "Icosphere") {
        node = std::make_shared<Mesh>(make_icosahedron(0.9f, 1),
                                      Material::standard(0x9aa1ad, 0.5f, 0.05f));
    } else if (obj.type == "Cylinder") {
        node = std::make_shared<Mesh>(make_cylinder(0.7f, 0.7f, 1.6f, 24),
                                      Material::standard(0x9aa1ad, 0.5f, 0.05f));
    } else if (obj.type == "Cone") {
        node = std::make_shared<Mesh>(make_cone(0.85f, 1.6f, 24),
                                      Material::standard(0x9aa1ad, 0.5f, 0.05f));
    } else if (obj.type == "Torus") {
        node = std::make_shared<Mesh>(make_torus(0.8f, 0.25f, 12, 24),
                                      Material::standard(0x4d9fff, 0.4f, 0.1f));
    } else if (obj.type == "Plane") {
        auto plane = std::make_shared<Mesh>(make_plane(2.0f, 2.0f),
                                            Material::standard(0x9aa1ad, 0.7f, 0.0f));
        plane->set_rotation({-static_cast<float>(kPi) / 2.0f, 0.0f, 0.0f});
        node = plane;
    } else if (obj.type == "Point Light" || obj.type == "Sun" ||
               obj.type == "Spot") {
        // A selectable light marker (the game editor's light case): a small
        // emissive nub in a wire cage. The real lighting rig is fixed in the
        // viewport's environment.
        auto group = std::make_shared<Group>();
        auto bulb = std::make_shared<Mesh>(make_octahedron(0.25f, 0),
                                           Material::basic(0xfff1c4));
        group->add(bulb);
        auto cage = std::make_shared<LineSegments>(
            make_edges(*make_icosahedron(0.4f, 0)),
            Material::line(0xfff1c4, 0.6f));
        group->add(cage);
        node = group;
    } else if (obj.type == "Camera" || obj.type == "Empty") {
        // Small marker: three axis-crossing line segments (an empty gizmo).
        std::vector<e3d::Vec3> pts = {
            {-0.3f, 0.0f, 0.0f}, {0.3f, 0.0f, 0.0f}, {0.0f, -0.3f, 0.0f},
            {0.0f, 0.3f, 0.0f},  {0.0f, 0.0f, -0.3f}, {0.0f, 0.0f, 0.3f}};
        node = std::make_shared<LineSegments>(make_from_points(pts),
                                              Material::line(0xb6bcc7, 0.9f));
    } else {
        return nullptr;
    }

    if (auto* mesh = dynamic_cast<Mesh*>(node.get())) {
        mesh->cast_shadow = true;
        mesh->receive_shadow = true;
    }
    node->position = e_pos;
    if (rot.x != 0.0f || rot.y != 0.0f || rot.z != 0.0f) {
        node->set_rotation({rot.x, rot.y, rot.z});
    }
    node->scale = scl;
    node->name = obj.id;  // node name == doc object id (picking / gizmo)
    return node;
}

affineui::App::Config DenderApp::make_config() {
    affineui::App::Config cfg;
    cfg.title = "AffineUI - DENDER mini DCC";
    cfg.width = 1440;
    cfg.height = 900;
    cfg.clear_color = affineui::Color{0x14, 0x16, 0x1c, 0xff};
    cfg.high_dpi = true;
    cfg.perf_overlay = false;
    cfg.asset_folders = {".", "examples"};
    cfg.on_layout_changed = [this] {
        save_dock_layout();
        reload();
    };
    return cfg;
}

void DenderApp::load_settings() {
    prefs_.load(kPrefsPath);
    ws_.load(kWorkspacePath);
    accent_ = prefs_.get_string("ui.accent", "orange");
    density_ = prefs_.get_string("ui.density", "comfortable");
    style_ = prefs_.get_string("ui.style", "flat");
}

void DenderApp::save_settings() {
    prefs_.set_string("ui.accent", accent_);
    prefs_.set_string("ui.density", density_);
    prefs_.set_string("ui.style", style_);
    prefs_.save(kPrefsPath);
}

void DenderApp::save_dock_layout() {
    for (const auto& [id, px] : app_.document().dock_pane_sizes()) {
        ws_.set_panel_size(id, px);
    }
    ws_.save(kWorkspacePath);
}

void DenderApp::bind_keys() {
    using K = affineui::Key;
    keymap_.bind({K::Z, true, false, false, false}, "edit.undo");
    keymap_.bind({K::Z, true, true, false, false}, "edit.redo");
    // Viewport-scoped (Blender semantics — see handle_key).
    keymap_.bind({K::G, false, false, false, false}, "dender.mode.translate");
    keymap_.bind({K::R, false, false, false, false}, "dender.mode.rotate");
    keymap_.bind({K::S, false, false, false, false}, "dender.mode.scale");
    keymap_.bind({K::X, false, false, false, false}, "dender.delete");
    keymap_.bind({K::Delete, false, false, false, false}, "dender.delete");
    keymap_.bind({K::D, false, true, false, false}, "dender.duplicate");
    keymap_.bind({K::F, false, false, false, false}, "dender.frame");
}

int DenderApp::run() {
    boot();
    return app_.run();
}

void DenderApp::boot() {
    std::string bundle_base;
    stylesheet_ = app::read_framework_bundle(affineui::ViewTheme::Decius,
                                             kDeciusVersion, bundle_base);
    stylesheet_ += "\n";
    stylesheet_ += native_css();
    stylesheet_base_ = std::move(bundle_base);
    if (kUseReconcileFastPath) {
        app_.set_view(
            [this](affineui::View& v) { DenderView{*this}.build_into(v); });
        app_.set_stylesheet(stylesheet_, stylesheet_base_);
    } else {
        app_.load_view(build_view());
        app_.set_stylesheet(stylesheet_, stylesheet_base_);
    }
}

void DenderApp::reload() {
    // Mirror document/selection changes into the 3D scene before the rebuilt
    // view paints (game_editor pattern).
    if (viewport_) {
        viewport_->sync_document();
        viewport_->sync_selection();
    }
    if (kUseReconcileFastPath) {
        app_.rebuild_view();
    } else {
        app_.load_view(build_view());
    }
}

void DenderApp::rebuild_shell() {
    if (!kUseReconcileFastPath) {
        reload();
        return;
    }
    if (stylesheet_.empty()) {
        reload();
        return;
    }
    app_.set_stylesheet(stylesheet_, stylesheet_base_);
}

int DenderApp::profile(int iterations) {
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };
    const auto stats = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return std::array<double, 3>{v.front(), v[v.size() / 2], v.back()};
    };

    {
        affineui::View pv;
        affineui::RemotePatchQueue q;
        pv.begin(&q);
        DenderView{*this}.build_into(pv);
        pv.end();
        const std::size_t first = q.size();
        q.clear();
        pv.begin(&q);
        DenderView{*this}.build_into(pv);
        pv.end();
        std::fprintf(stderr,
                     "[profile] diff probe: first build %zu patches, clean "
                     "rebuild %zu patches\n",
                     first, q.size());
        if (iterations <= 0) return 0;
    }

    std::string bundle_base;
    std::string css = app::read_framework_bundle(affineui::ViewTheme::Decius,
                                                 kDeciusVersion, bundle_base);
    css += "\n";
    css += native_css();
    app_.load_view(build_view());
    app_.set_stylesheet(css, bundle_base);
    (void)app_.document().find_element_rect("dn-vp-scenelayer");

    std::vector<double> t_build, t_load, t_layout;
    std::size_t html_bytes = 0;
    for (int i = 0; i < iterations; ++i) {
        const auto t0 = clock::now();
        affineui::View v = build_view();
        const auto t1 = clock::now();
        if (i == 0) html_bytes = v.to_html_document().size();
        const auto t2 = clock::now();
        app_.load_view(std::move(v));
        const auto t3 = clock::now();
        (void)app_.document().find_element_rect("dn-vp-scenelayer");
        const auto t4 = clock::now();
        t_build.push_back(ms(t1 - t0));
        t_load.push_back(ms(t3 - t2));
        t_layout.push_back(ms(t4 - t3));
    }

    app_.set_view(
        [this](affineui::View& v) { DenderView{*this}.build_into(v); });
    app_.set_stylesheet(css, bundle_base);
    app_.rebuild_view();

    std::vector<double> t_clean, t_camera, t_fast_layout;
    const int fast_iters = std::min(iterations, 1);
    for (int i = 0; i < fast_iters; ++i) {
        const auto t0 = clock::now();
        app_.rebuild_view();
        const auto t1 = clock::now();
        viewport_->dolly(i % 2 == 0 ? 1.01 : 1.0 / 1.01);
        const auto t2 = clock::now();
        (void)app_.document().find_element_rect("dn-vp-scenelayer");
        const auto t3 = clock::now();
        t_clean.push_back(ms(t1 - t0));
        t_camera.push_back(ms(t2 - t1));
        t_fast_layout.push_back(ms(t3 - t2));
    }

    const auto rect = app_.document().find_element_rect("dn-vp-scenelayer");
    const auto b = stats(t_build), l = stats(t_load), y = stats(t_layout);
    const auto fc = stats(t_clean), fm = stats(t_camera),
               fy = stats(t_fast_layout);
    std::fprintf(stderr, "[profile] iterations: %d, document: %zu KB\n",
                 iterations, html_bytes / 1024);
    std::fprintf(stderr,
                 "[profile]   view build       min %7.2f  med %7.2f  max %7.2f ms\n",
                 b[0], b[1], b[2]);
    std::fprintf(stderr,
                 "[profile]   load_view        min %7.2f  med %7.2f  max %7.2f ms\n",
                 l[0], l[1], l[2]);
    std::fprintf(stderr,
                 "[profile]   clean rebuild    min %7.2f  med %7.2f  max %7.2f ms\n",
                 fc[0], fc[1], fc[2]);
    std::fprintf(stderr,
                 "[profile]   camera change    min %7.2f  med %7.2f  max %7.2f ms\n",
                 fm[0], fm[1], fm[2]);
    std::fprintf(stderr,
                 "[profile]   per interaction: ~%.2f ms (scene layer rect %dx%d)\n",
                 fm[1] + fy[1], rect.w, rect.h);
    return 0;
}

affineui::View DenderApp::build_view() {
    DenderView view{*this};
    return view.build();
}

int DenderApp::view_width() const {
    return app_.window_size().width;
}

int DenderApp::workspace_panel_size(std::string_view id) const {
    return ws_.panel_size(id, 0);
}

affineui::Document::DockLayout DenderApp::dock_layout() const {
    return app_.document().dock_layout();
}

affineui::Document::DockPlacement DenderApp::dock_override(
    std::string_view id) const {
    return app_.document().dock_override(id);
}

std::string DenderApp::dock_active_tab(std::string_view id) const {
    return app_.document().dock_active_tab(id);
}

// ── Handlers ────────────────────────────────────────────────────────────────

void DenderApp::select_object(std::string_view id) {
    if (id.empty()) return;
    ctx_.selection().select(id);  // fires changed -> reload
}

void DenderApp::pick_object(std::string_view id, bool shift) {
    auto& sel = ctx_.selection();
    if (id.empty()) {
        if (!shift && !sel.active().empty()) sel.clear();
        return;
    }
    if (shift && !sel.active().empty()) {
        sel.toggle(id);  // additive multi-select membership
        return;
    }
    select_object(id);
}

void DenderApp::rename_active(std::string_view name) {
    const std::string id(ctx_.selection().active());
    if (id.empty()) return;
    ctx_.stack().push(
        std::make_unique<RenameObjectCmd>(id, std::string(name)));
}

void DenderApp::run_transform_edit(std::string_view prop, std::string_view value,
                                   bool degrees, std::string_view label) {
    const std::string id(ctx_.selection().active());
    if (id.empty() || viewport_ == nullptr) return;
    if (viewport_->node_of(id) == nullptr) return;
    double v = parse_double_or(value, 0.0);
    // The e3d reflection stores rotation in DEGREES already, so no conversion
    // is needed (unlike the old DenderDocument which stored radians).
    (void)degrees;
    (void)label;
    ctx_.stack().begin_coalescing();
    viewport_->commit_node_property(id, prop, affineui::PropertyValue{v});
    ctx_.stack().end_coalescing();
}

void DenderApp::set_location(int axis, std::string_view value) {
    static const char* kProp[3] = {"position.x", "position.y", "position.z"};
    if (axis < 0 || axis > 2) return;
    run_transform_edit(kProp[axis], value, false, "Move");
}

void DenderApp::set_rotation_deg(int axis, std::string_view value) {
    static const char* kProp[3] = {"rotation.x", "rotation.y", "rotation.z"};
    if (axis < 0 || axis > 2) return;
    run_transform_edit(kProp[axis], value, true, "Rotate");
}

void DenderApp::set_scale(int axis, std::string_view value) {
    static const char* kProp[3] = {"scale.x", "scale.y", "scale.z"};
    if (axis < 0 || axis > 2) return;
    run_transform_edit(kProp[axis], value, false, "Scale");
}

namespace {
// The active mesh geometry of an object's node (null for non-mesh nodes).
e3d::GeometryPtr current_geometry(viewport3d::Viewport3D& vp,
                                  std::string_view id) {
    e3d::GeometryPtr geo;
    if (e3d::Object3D* node = vp.node_of(id)) {
        node->traverse([&](e3d::Object3D& n) {
            if (!geo && n.kind() == e3d::ObjectKind::Mesh) {
                geo = static_cast<e3d::Mesh&>(n).geometry;
            }
        });
    }
    return geo;
}
}  // namespace

// Regenerate `id`'s mesh at its current base dimensions with `axis` replaced by
// `value`; returns the new geometry (null if the type has no editable dims or
// nothing changed). Shared by preview + commit so both size the mesh the same.
e3d::GeometryPtr DenderApp::regen_dimension(std::string_view id, int axis,
                                            std::string_view value) {
    const app::Object* obj = id.empty() ? nullptr : ctx_.document().find(id);
    if (obj == nullptr || viewport_ == nullptr) return nullptr;
    e3d::Vec3 dims = viewport_->base_dimensions(id);
    float* const d[3] = {&dims.x, &dims.y, &dims.z};
    *d[axis] = static_cast<float>(parse_double_or(value, 1.0));
    return make_primitive_geometry(obj->type, dims);
}

void DenderApp::preview_dimension(int axis, std::string_view value) {
    if (axis < 0 || axis > 2 || viewport_ == nullptr) return;
    const std::string id(ctx_.selection().active());
    // First preview of the gesture: snapshot the pre-gesture geometry so the
    // commit's undo restores the exact starting mesh.
    if (dim_gesture_id_ != id || !dim_gesture_geometry_) {
        dim_gesture_id_ = id;
        dim_gesture_geometry_ = current_geometry(*viewport_, id);
    }
    if (e3d::GeometryPtr next = regen_dimension(id, axis, value)) {
        viewport_->set_node_geometry(id, next);  // live, no undo entry
    }
}

void DenderApp::commit_dimension(int axis, std::string_view value) {
    if (axis < 0 || axis > 2 || viewport_ == nullptr) return;
    const std::string id(ctx_.selection().active());
    const app::Object* obj = id.empty() ? nullptr : ctx_.document().find(id);
    if (obj == nullptr) {
        dim_gesture_geometry_.reset();
        dim_gesture_id_.clear();
        return;
    }
    e3d::GeometryPtr next = regen_dimension(id, axis, value);
    // The pre-gesture geometry (from the first preview) is the undo target; if
    // there was no preview (a typed edit), snapshot now.
    e3d::GeometryPtr prev = (dim_gesture_id_ == id && dim_gesture_geometry_)
                                ? dim_gesture_geometry_
                                : current_geometry(*viewport_, id);
    dim_gesture_geometry_.reset();
    dim_gesture_id_.clear();
    if (!next || !prev || prev == next) return;

    viewport3d::Viewport3D* vp = viewport_.get();
    ctx_.stack().push(std::make_unique<app::LambdaCommand>(
        "dender.setDimensions", "Resize " + obj->name,
        [vp, id, next](app::Document&) { vp->set_node_geometry(id, next); },
        [vp, id, prev](app::Document&) { vp->set_node_geometry(id, prev); }));
}

void DenderApp::add_object(std::string_view type) {
    ctx_.stack().push(std::make_unique<AddObjectCmd>(
        ctx_.selection(), id_counter_, std::string(type)));
}

void DenderApp::delete_active() {
    const std::string id(ctx_.selection().active());
    if (id.empty()) return;
    ctx_.stack().push(std::make_unique<DeleteObjectCmd>(ctx_.selection(), id));
}

void DenderApp::duplicate_active() {
    const std::string id(ctx_.selection().active());
    if (id.empty()) return;
    ctx_.stack().push(std::make_unique<DuplicateObjectCmd>(
        ctx_.selection(), id_counter_, id,
        [this](const std::string& new_id, const std::string& src_id) {
            // Copy the source node's current transform, offset x by 0.5 (web
            // Shift+D), and hand it to make_scene_node via the pending map.
            NodeSpawn spawn;
            if (viewport_ != nullptr) {
                if (e3d::Object3D* node = viewport_->node_of(src_id)) {
                    node->update_world_matrix(true, false);
                    spawn.position = {node->position.x, node->position.y,
                                      node->position.z};
                    const e3d::Euler& e = node->rotation();
                    spawn.rotation = {e.x, e.y, e.z};
                    spawn.scale = node->scale;
                }
            }
            spawn.position.x += 0.5;  // web Shift+D offset
            pending_spawns_[new_id] = spawn;
        }));
}

void DenderApp::undo() { ctx_.stack().undo(); }
void DenderApp::redo() { ctx_.stack().redo(); }

void DenderApp::set_tool(std::string_view tool) {
    ui_.active_tool = std::string(tool);
    // Move/Rotate/Scale (and Transform -> translate) drive the gizmo mode; the
    // other rail tools (tweak/cursor/annotate/measure) are cosmetic no-ops.
    if (tool == "move" || tool == "transform") {
        mode_ = TransformMode::Translate;
        if (viewport_) viewport_->set_tool("move");
    } else if (tool == "rotate") {
        mode_ = TransformMode::Rotate;
        if (viewport_) viewport_->set_tool("rotate");
    } else if (tool == "scale") {
        mode_ = TransformMode::Scale;
        if (viewport_) viewport_->set_tool("scale");
    } else {
        // tweak / cursor / annotate / measure: no viewport transform tool.
        // GAP: these DENDER tools have no viewport analog yet (cosmetic only).
        if (viewport_) viewport_->set_tool("select");
    }
    reload();
}

void DenderApp::set_shading(std::string_view mode) {
    // GAP: Viewport3D renders lit-solid only. Wireframe / Texture / Rendered
    // are not distinct render modes here — the button state is cosmetic.
    if (mode == "wire") shading_ = Shading::Wireframe;
    else if (mode == "tex") shading_ = Shading::Texture;
    else if (mode == "render") shading_ = Shading::Rendered;
    else shading_ = Shading::Solid;
    reload();
}

void DenderApp::toggle_play() {
    playing_ = !playing_;
    reload();
}

void DenderApp::set_frame(std::string_view value) {
    const int f = static_cast<int>(
        std::lround(parse_double_or(value, timeline_.frame)));
    timeline_.frame = f;
    if (timeline_.start > f) timeline_.start = f;
    if (timeline_.end < f) timeline_.end = f;
    reload();
}

void DenderApp::set_start(std::string_view value) {
    const int s = static_cast<int>(
        std::lround(parse_double_or(value, timeline_.start)));
    timeline_.start = s;
    if (timeline_.end < s) timeline_.end = s;
    if (timeline_.frame < s) timeline_.frame = s;
    reload();
}

void DenderApp::set_end(std::string_view value) {
    const int e = static_cast<int>(
        std::lround(parse_double_or(value, timeline_.end)));
    timeline_.end = e;
    if (timeline_.start > e) timeline_.start = e;
    if (timeline_.frame > e) timeline_.frame = e;
    reload();
}

void DenderApp::set_accent(std::string_view accent) {
    accent_ = std::string(accent);
    save_settings();
    rebuild_shell();
}

void DenderApp::set_density(std::string_view density) {
    density_ = std::string(density);
    save_settings();
    rebuild_shell();
}

void DenderApp::set_style(std::string_view style) {
    style_ = std::string(style);
    save_settings();
    rebuild_shell();
}

void DenderApp::toggle_flag(std::string_view which) {
    if (which == "snapping") ui_.snapping = !ui_.snapping;
    else if (which == "proportional") ui_.proportional = !ui_.proportional;
    else if (which == "gizmo") ui_.show_gizmo = !ui_.show_gizmo;
    else if (which == "overlays") ui_.show_overlays = !ui_.show_overlays;
    else if (which == "xray") ui_.xray = !ui_.xray;
    else if (which == "autokey") ui_.auto_key = !ui_.auto_key;
    reload();
}

void DenderApp::set_inspector_tab(std::string_view id) {
    ui_.inspector_tab = std::string(id);
    reload();
}

void DenderApp::quit() { app_.quit(); }

// ── Native events: keyboard + timeline scrub ────────────────────────────────

bool DenderApp::handle_key(
    const affineui::Event& ev,
    const std::vector<affineui::Document::HoverInfo>& chain) {
    const std::string_view action =
        keymap_.command_for(affineui::chord_from_event(ev));
    if (action.empty()) return false;
    if (action == "edit.undo") { undo(); return true; }
    if (action == "edit.redo") { redo(); return true; }
    // Bare-letter commands act only while the pointer is over the 3D viewport.
    if (!chain_has_class(chain, "dn-vp-canvas")) return false;
    if (action == "dender.mode.translate") { set_tool("move"); return true; }
    if (action == "dender.mode.rotate") { set_tool("rotate"); return true; }
    if (action == "dender.mode.scale") { set_tool("scale"); return true; }
    if (action == "dender.delete") { delete_active(); return true; }
    if (action == "dender.duplicate") { duplicate_active(); return true; }
    if (action == "dender.frame") {
        if (viewport_) viewport_->frame_selected();
        return true;
    }
    return false;
}

bool DenderApp::handle_event(
    const affineui::Event& ev,
    const std::vector<affineui::Document::HoverInfo>& chain) {
    using ET = affineui::EventType;

    if (ev.type == ET::Resize) {
        reload();  // the timeline ruler scale derives from the window width
        return false;
    }
    if (ev.type == ET::KeyDown) return handle_key(ev, chain);

    // Give the shared viewport first crack (orbit / pan / dolly / pick / nav
    // gizmo). If it consumes the event, we are done.
    if (viewport_ != nullptr && viewport_->handle_event(ev, chain)) return true;

    // Otherwise run DENDER's timeline-scrub gesture.
    if (scrubbing_) {
        if (ev.type == ET::MouseMove) {
            const auto scale = ui::TimelineScale::for_width(scrub_bounds_.w);
            const int frame = scale.frame_at(ev.pos.x - scrub_bounds_.x);
            const int clamped =
                std::clamp(frame, timeline_.start, timeline_.end);
            if (clamped != timeline_.frame) {
                timeline_.frame = clamped;
                reload();
            }
            return true;
        }
        if (ev.type == ET::MouseUp) {
            scrubbing_ = false;
            app_.release_pointer();
            return true;
        }
        return false;
    }

    if (ev.type == ET::MouseDown && ev.button == affineui::MouseButton::Left) {
        const affineui::Document::HoverInfo* body = nullptr;
        if (chain_has_class(chain, "dn-timeline-body", &body)) {
            scrubbing_ = true;
            scrub_bounds_ = body->bounds;
            app_.capture_pointer();
            const auto scale = ui::TimelineScale::for_width(scrub_bounds_.w);
            timeline_.frame = std::clamp(
                scale.frame_at(ev.pos.x - scrub_bounds_.x), timeline_.start,
                timeline_.end);
            reload();
            return true;
        }
    }
    return false;
}

}  // namespace dender
