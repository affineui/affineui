#include "viewport3d.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "sokol_app.h"  // sapp_dpi_scale for crisp high-DPI targets

namespace viewport3d {

using affineui::Rect;
using namespace e3d;

namespace {

bool chain_entry_has(const affineui::Document::HoverInfo& info,
                     std::string_view cls) {
    for (const auto& c : info.classes) {
        if (c == cls) return true;
    }
    return false;
}

constexpr double kClickSlopPx = 4.0;

}  // namespace

Viewport3D::Viewport3D(Config config) : cfg_(std::move(config)) {
    scene_ = std::make_shared<Scene>();
    scene_->background = Color(0x232529);

    camera_ = std::make_shared<PerspectiveCamera>(38.0f, 1.6f, 0.1f, 200.0f);
    camera_->position = {5.2f, 3.4f, 6.4f};
    scene_->add(camera_);

    orbit_ = std::make_unique<OrbitControls>(camera_);
    orbit_->target = {0.0f, 0.6f, 0.0f};
    orbit_->min_distance = 2.0f;
    orbit_->max_distance = 40.0f;
    orbit_->max_polar_angle = kPi * 0.495f;  // stay above the ground
    // DCC convention: middle-drag pans (the wheel already dollies).
    orbit_->button_middle = OrbitControls::Action::Pan;
    orbit_->update();

    gizmo_ = std::make_unique<TransformControls>(camera_);
    scene_->add(gizmo_->helper());

    // Object-aligned selection outline (the web samples' orange box).
    selection_box_ = std::make_shared<SelectionBox>(0xe8943c);
    scene_->add(selection_box_);

    // Environment: hemisphere fill, shadowed key light, soft back fill —
    // the same rig as the DENDER web sample.
    scene_->add(std::make_shared<HemisphereLight>(0xe6efff, 0x1a1c20, 0.55f));
    auto key = std::make_shared<DirectionalLight>(0xffffff, 2.4f);
    key->position = {6.0f, 10.0f, 4.0f};
    key->cast_shadow = true;
    scene_->add(key);
    auto fill = std::make_shared<DirectionalLight>(0xbfd4ff, 0.5f);
    fill->position = {-6.0f, 4.0f, -5.0f};
    scene_->add(fill);

    // Grid + invisible shadow catcher.
    scene_->add(make_grid_helper(20.0f, 20, 0x40444c, 0x2a2d34, 0.55f));
    auto floor = std::make_shared<Mesh>(make_plane(60.0f, 60.0f),
                                        Material::shadow(0.32f));
    floor->set_rotation({-kPi / 2.0f, 0.0f, 0.0f});
    floor->receive_shadow = true;
    scene_->add(floor);
}

Viewport3D::~Viewport3D() = default;

void Viewport3D::attach(affineui::App& app, app::Context& ctx) {
    app_ = app.handle();
    ctx_ = affineui::to_weak_ref(&ctx);
    renderer_ = std::make_unique<Renderer>();
    const auto weak = affineui::to_weak_ref(this);

    app.set_custom_paint(cfg_.scene_paint,
                         [weak](affineui::Painter& p, const Rect& r) {
                             if (auto* self = weak.get()) self->paint(p, r);
                         });
    app.set_custom_paint(cfg_.nav_paint,
                         [weak](affineui::Painter& p, const Rect& r) {
                             if (auto* self = weak.get()) self->paint_navball(p, r);
                         });
    app.on_frame([weak](double dt) {
        if (auto* self = weak.get()) self->frame(dt);
    });

    gizmo_->on_dragging_changed = [this](bool dragging) {
        if (dragging) {
            if (const ObjectPtr& obj = gizmo_->object()) {
                start_position_ = obj->position;
                start_quaternion_ = obj->quaternion();
                start_scale_ = obj->scale;
            }
        } else {
            push_transform_command();
        }
    };
    gizmo_->on_object_change = [this] {
        mark_dirty();
        // The scene's objectChange: surface which document object moved
        // so the inspector can track the drag live.
        if (on_node_changed) {
            if (const ObjectPtr& obj = gizmo_->object()) {
                on_node_changed(obj->name);  // node name == doc object id
            }
        }
    };

    sync_document();
    sync_selection();
}

void Viewport3D::build(affineui::View& v) {
    v.canvas(cfg_.scene_paint, cfg_.scene_canvas_class, cfg_.scene_canvas_key);
    // Navigation axis ball (the web samples' orientation gizmo):
    // painter-drawn, repainted with the scene on camera moves; nub
    // clicks snap the camera down that axis.
    v.canvas(cfg_.nav_paint, cfg_.nav_canvas_class, cfg_.nav_canvas_key);
}

// ── Scene <-> app document ──────────────────────────────────────────

ObjectPtr Viewport3D::make_node(const app::Object& obj, std::size_t index) {
    if (cfg_.make_node) return cfg_.make_node(obj, index);
    return make_default_node(obj, index);
}

ObjectPtr Viewport3D::make_default_node(const app::Object& obj,
                                        std::size_t index) {
    // Deterministic primitive + placement per object type. Objects the
    // user adds later land on a ring so they never stack. This is the
    // game editor's original mapping, kept as the fallback for a host
    // that installs no factory.
    const float ring = 2.6f;
    const float angle =
        static_cast<float>(index) * (2.0f * kPi / 8.0f) + 0.7f;
    const Vec3 ring_pos{ring * std::cos(angle), 0.8f,
                        ring * std::sin(angle)};

    ObjectPtr node;
    if (obj.type == "mesh") {
        auto mesh = std::make_shared<Mesh>(
            make_box(1.6f, 1.6f, 1.6f),
            Material::standard(0x9aa1ad, 0.55f, 0.05f));
        mesh->cast_shadow = true;
        mesh->receive_shadow = true;
        mesh->position = obj.id == "hero" ? Vec3{0.0f, 0.8f, 0.0f}
                                          : ring_pos;
        node = mesh;
    } else if (obj.type == "light") {
        // A selectable marker; the actual key light is part of the
        // fixed environment rig.
        auto group = std::make_shared<Group>();
        auto bulb = std::make_shared<Mesh>(make_octahedron(0.25f, 0),
                                           Material::basic(0xfff1c4));
        group->add(bulb);
        auto cage = std::make_shared<LineSegments>(
            make_edges(*make_icosahedron(0.4f, 0)),
            Material::line(0xfff1c4, 0.6f));
        group->add(cage);
        group->position = {2.8f, 2.6f, 1.4f};
        node = group;
    } else if (obj.type == "spline") {
        auto torus = std::make_shared<Mesh>(
            make_torus(0.7f, 0.22f, 16, 48),
            Material::standard(0x4d9fff, 0.4f, 0.1f));
        torus->cast_shadow = true;
        torus->receive_shadow = true;
        torus->position = {-2.4f, 0.7f, -1.8f};
        torus->set_rotation({-kPi / 2.0f, 0.0f, 0.0f});
        node = torus;
    } else {
        return nullptr;  // groups etc. exist only in the outliner
    }
    node->name = obj.id;
    return node;
}

void Viewport3D::sync_document() {
    auto* context = ctx_.get();
    if (context == nullptr) return;
    const auto& objects = context->document().objects();

    // Drop nodes whose document object is gone.
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        const bool alive =
            std::any_of(objects.begin(), objects.end(),
                        [&](const app::Object& o) { return o.id == it->first; });
        if (!alive) {
            if (gizmo_->object() == it->second) gizmo_->detach();
            it->second->remove_from_parent();
            it = nodes_.erase(it);
        } else {
            ++it;
        }
    }
    // Add nodes for new objects.
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const app::Object& obj = objects[i];
        if (nodes_.contains(obj.id)) continue;
        if (ObjectPtr node = make_node(obj, i)) {
            scene_->add(node);
            nodes_.emplace(obj.id, std::move(node));
        }
    }
    // Re-apply persisted material properties (idempotent) — the live
    // set_material_* previews write the same values ahead of the
    // document commit.
    for (const app::Object& obj : objects) {
        if (!nodes_.contains(obj.id)) continue;
        const auto& doc = context->document();
        const app::PropValue tint_value =
            doc.property(obj.id, "tint", app::PropValue{std::string{}});
        if (const auto* tint = std::get_if<std::string>(&tint_value);
            tint != nullptr && !tint->empty()) {
            set_material_tint(obj.id, *tint);
        }
        const app::PropValue rough_value =
            doc.property(obj.id, "roughness", app::PropValue{-1.0});
        if (const auto* rough = std::get_if<double>(&rough_value);
            rough != nullptr && *rough >= 0.0) {
            set_material_roughness(obj.id, *rough);
        }
    }
    mark_dirty();
}

void Viewport3D::sync_selection() {
    auto* context = ctx_.get();
    if (context == nullptr) return;
    ObjectPtr target;
    // The box + gizmo follow the ACTIVE object — the one the user last
    // clicked, in the 3D view or the hierarchy. This is the same
    // "current object" the inspector and the hierarchy highlight key
    // off (Selection::active), so all three views stay consistent (a
    // document-order "first selected" would drift from them under
    // multi-select).
    const std::string_view active = context->selection().active();
    if (!active.empty()) {
        const auto it = nodes_.find(std::string(active));
        if (it != nodes_.end()) target = it->second;
    }
    selected_node_ = target;
    if (target && tool_ != "select") {
        gizmo_->attach(target);
    } else {
        gizmo_->detach();
    }
    mark_dirty();
}

void Viewport3D::set_tool(std::string_view tool) {
    tool_ = tool;
    if (tool == "move") {
        gizmo_->set_mode(TransformControls::Mode::Translate);
    } else if (tool == "rotate") {
        gizmo_->set_mode(TransformControls::Mode::Rotate);
    } else if (tool == "scale") {
        gizmo_->set_mode(TransformControls::Mode::Scale);
    }
    sync_selection();  // attaches for gizmo tools, detaches for select
}

// ── Camera ops ──────────────────────────────────────────────────────

void Viewport3D::dolly(double factor) {
    // Advance the orbit distance by scaling the eye offset from the
    // target (the wheel path's effect), then let update() re-clamp and
    // re-aim like a wheel notch would.
    const Vec3 offset = camera_->position - orbit_->target;
    camera_->position = orbit_->target + offset * static_cast<float>(factor);
    orbit_->update();
    mark_dirty();
}

void Viewport3D::toggle_pan_mode() {
    pan_mode_ = !pan_mode_;
    orbit_->button_left = pan_mode_ ? OrbitControls::Action::Pan
                                    : OrbitControls::Action::Rotate;
}

void Viewport3D::frame_selected() {
    // Fit the camera to the active node's world bounds; fall back to a
    // reasonable framing of the whole document when nothing is selected.
    Box3 box;
    auto expand_from = [&](const ObjectPtr& node) {
        if (!node) return;
        node->update_world_matrix(true, true);
        node->traverse([&](Object3D& n) {
            if (n.kind() == ObjectKind::Mesh) {
                if (const auto& geo = static_cast<Mesh&>(n).geometry) {
                    box.union_with(
                        geo->bounding_box().transformed(n.matrix_world));
                }
            }
        });
    };
    if (selected_node_) {
        expand_from(selected_node_);
    } else {
        for (const auto& [id, node] : nodes_) expand_from(node);
    }

    if (box.empty()) {
        orbit_->target = {0.0f, 0.6f, 0.0f};
    } else {
        orbit_->target = box.center();
        const float radius = box.size().length() * 0.5f;
        const float dist = std::max(2.0f, radius * 2.6f);
        const Vec3 dir = (camera_->position - orbit_->target).length() > 1e-4f
                             ? (camera_->position - orbit_->target).normalized()
                             : Vec3{0.6f, 0.4f, 0.7f}.normalized();
        camera_->position = orbit_->target + dir * dist;
    }
    orbit_->update();
    mark_dirty();
}

void Viewport3D::snap_to_axis(int axis) { snap_camera_to_axis(axis); }

// ── Undo ────────────────────────────────────────────────────────────

void Viewport3D::apply_transform_command(const ObjectPtr& node,
                                         const Vec3& old_p,
                                         const Quat& old_q,
                                         const Vec3& old_s) {
    auto* context = ctx_.get();
    if (!node || context == nullptr) return;
    const Vec3 new_p = node->position;
    const Quat new_q = node->quaternion();
    const Vec3 new_s = node->scale;
    if (new_p == old_p && new_s == old_s &&
        new_q.angle_to(old_q) < 1e-6f) {
        return;  // no actual change
    }
    // The node is captured by shared_ptr so undo stays valid even after
    // the object is removed and re-added; `this` outlives the stack
    // (both are owned by the host controller).
    const auto weak = affineui::to_weak_ref(this);
    auto apply = [weak, node](const Vec3& p, const Quat& q, const Vec3& s) {
        node->position = p;
        node->set_quaternion(q);
        node->scale = s;
        if (auto* self = weak.get()) self->mark_dirty();
    };
    context->stack().push(std::make_unique<app::LambdaCommand>(
        "obj.transform", "Transform " + node->name,
        [apply, new_p, new_q, new_s](app::Document&) {
            apply(new_p, new_q, new_s);
        },
        [apply, old_p, old_q, old_s](app::Document&) {
            apply(old_p, old_q, old_s);
        }));
}

void Viewport3D::push_transform_command() {
    apply_transform_command(gizmo_->object(), start_position_,
                            start_quaternion_, start_scale_);
}

// ── Inspector bridge ────────────────────────────────────────────────

Object3D* Viewport3D::node_of(std::string_view id) const {
    const auto it = nodes_.find(std::string(id));
    return it != nodes_.end() ? it->second.get() : nullptr;
}

namespace {
std::string gesture_key(std::string_view id, std::string_view prop) {
    std::string key(id);
    key.push_back('\x1f');
    key.append(prop);
    return key;
}
}  // namespace

void Viewport3D::preview_node_property(std::string_view id,
                                       std::string_view prop,
                                       const affineui::PropertyValue& value) {
    const auto it = nodes_.find(std::string(id));
    if (it == nodes_.end()) return;
    const ObjectPtr& node = it->second;
    const affineui::ObjectClass& cls = get_class(*node);
    // First preview of the gesture: remember the pre-gesture value the
    // eventual commit's undo will restore.
    gesture_old_.try_emplace(gesture_key(id, prop),
                             cls.get(node.get(), prop));
    cls.set(node.get(), prop, value);
    mark_dirty();
}

void Viewport3D::commit_node_property(std::string_view id,
                                      std::string_view prop,
                                      const affineui::PropertyValue& value) {
    const auto it = nodes_.find(std::string(id));
    auto* context = ctx_.get();
    if (it == nodes_.end() || context == nullptr) return;
    const ObjectPtr& node = it->second;
    const affineui::ObjectClass& cls = get_class(*node);
    // Undo target: the pre-gesture value when previews ran, else the
    // current value (a typed edit with no live phase).
    affineui::PropertyValue old = cls.get(node.get(), prop);
    const auto gesture = gesture_old_.find(gesture_key(id, prop));
    if (gesture != gesture_old_.end()) {
        old = gesture->second;
        gesture_old_.erase(gesture);
    }
    if (old == value) {
        cls.set(node.get(), prop, value);  // normalise display value
        mark_dirty();
        return;  // no actual change — nothing to undo
    }
    // The node is captured by shared_ptr (undo stays valid after
    // remove/re-add) and the property by name, so redo/undo both go
    // through the same reflection mediator the inspector reads.
    const auto weak = affineui::to_weak_ref(this);
    auto apply = [weak, node, prop = std::string(prop)](
                     const affineui::PropertyValue& v) {
        get_class(*node).set(node.get(), prop, v);
        if (auto* self = weak.get()) self->mark_dirty();
    };
    context->stack().push(std::make_unique<app::LambdaCommand>(
        "obj.prop", "Edit " + node->name,
        [apply, value](app::Document&) { apply(value); },
        [apply, old](app::Document&) { apply(old); }));
}

// ── Materials ───────────────────────────────────────────────────────

namespace {
/// The first mesh in the node's subtree — the surface the inspector's
/// Material foldout edits (light markers keep their bulb mesh first).
Mesh* first_mesh(Object3D* node) {
    Mesh* found = nullptr;
    if (node == nullptr) return nullptr;
    node->traverse([&](Object3D& n) {
        if (found == nullptr && n.kind() == ObjectKind::Mesh) {
            found = static_cast<Mesh*>(&n);
        }
    });
    return found;
}

bool parse_hex_rgb(std::string_view s, std::uint32_t& out) {
    if (!s.empty() && s.front() == '#') s.remove_prefix(1);
    if (s.size() != 6) return false;
    std::uint32_t v = 0;
    for (const char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A') + 10u;
        else return false;
    }
    out = v;
    return true;
}
}  // namespace

void Viewport3D::set_material_tint(std::string_view id,
                                   std::string_view hex) {
    Mesh* mesh = first_mesh(node_of(id));
    std::uint32_t rgb = 0;
    if (mesh == nullptr || !mesh->material || !parse_hex_rgb(hex, rgb)) {
        return;
    }
    mesh->material->color = Color(rgb);  // sRGB hex → linear
    mark_dirty();
}

void Viewport3D::set_material_roughness(std::string_view id,
                                        double roughness) {
    Mesh* mesh = first_mesh(node_of(id));
    if (mesh == nullptr || !mesh->material) return;
    mesh->material->roughness =
        std::clamp(static_cast<float>(roughness), 0.0f, 1.0f);
    mark_dirty();
}

Vec3 Viewport3D::base_dimensions(std::string_view id) const {
    // The mesh geometry's bounding-box size, at scale 1 — the object's
    // intrinsic dimensions independent of its node scale transform.
    Mesh* mesh = first_mesh(node_of(id));
    if (mesh == nullptr || !mesh->geometry) return Vec3{};
    return mesh->geometry->bounding_box().size();
}

void Viewport3D::set_node_geometry(std::string_view id, GeometryPtr geometry) {
    Mesh* mesh = first_mesh(node_of(id));
    if (mesh == nullptr || !geometry) return;
    mesh->geometry = std::move(geometry);  // new ptr → renderer re-uploads
    mark_dirty();
}

// ── Frame / paint ───────────────────────────────────────────────────

void Viewport3D::mark_dirty() {
    dirty_ = true;
    app_.request_custom_repaint(cfg_.scene_paint);
}

void Viewport3D::frame(double /*dt*/) {
    if (renderer_ == nullptr) return;
    if (orbit_->update()) dirty_ = true;
    if (canvas_rect_.w <= 0 || canvas_rect_.h <= 0 || !dirty_) return;

    const float dpi = sapp_isvalid() ? sapp_dpi_scale() : 1.0f;
    renderer_->set_size(
        static_cast<int>(static_cast<float>(canvas_rect_.w) * dpi),
        static_cast<int>(static_cast<float>(canvas_rect_.h) * dpi));
    gizmo_->update();
    // Refit the selection outline (the object may just have moved).
    scene_->update_matrix_world();
    selection_box_->update_from(selected_node_.get());
    renderer_->render(*scene_, *camera_);
    dirty_ = false;
    if (app_) {
        app_.request_custom_repaint(cfg_.scene_paint);
        // The axis ball tracks the camera; repaint it with the scene.
        app_.request_custom_repaint(cfg_.nav_paint);
    }
}

void Viewport3D::paint(affineui::Painter& p, const Rect& r) {
    if (r.w != canvas_rect_.w || r.h != canvas_rect_.h) dirty_ = true;
    canvas_rect_ = r;
    if (renderer_ == nullptr) return;
    renderer_->draw_to(p, r);
}

// ── Navigation axis ball ────────────────────────────────────────────
// Port of the web samples' orientation gizmo (dender app.js
// buildGizmo / DENDER's paint_gizmo): the six world axis directions
// rotated into camera space, depth-sorted so near nubs paint over far
// ones, back-facing nubs dimmed; positive axes are filled nubs with a
// letter and a spoke, negative axes hollow rings.

namespace {

constexpr affineui::Color kNavAxisColor[3] = {
    affineui::Color{0xd8, 0x47, 0x5a, 0xff},   // X
    affineui::Color{0x6f, 0xb7, 0x4a, 0xff},   // Y
    affineui::Color{0x3f, 0x7a, 0xd0, 0xff}};  // Z

affineui::Color nav_alpha(affineui::Color c, double a01) {
    c.a = static_cast<std::uint8_t>(
        std::clamp(static_cast<long>(a01 * 255.0 + 0.5), 0L, 255L));
    return c;
}

constexpr e3d::Vec3 kNavDirs[6] = {{1, 0, 0},  {0, 1, 0},  {0, 0, 1},
                                   {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};

}  // namespace

void Viewport3D::paint_navball(affineui::Painter& p, const Rect& r) {
    nav_rect_ = r;
    if (r.w <= 0 || r.h <= 0) return;
    // Drawn in the web gizmo's 72x72 local space, scaled to the block.
    const double s = std::min(r.w, r.h) / 72.0;
    auto gx = [&](double v) {
        return static_cast<float>(static_cast<double>(r.x) + v * s);
    };
    auto gy = [&](double v) {
        return static_cast<float>(static_cast<double>(r.y) + v * s);
    };
    const auto sw = [&](double v) { return static_cast<float>(v * s); };

    // Camera basis: nubs project onto the screen right/up axes; the
    // depth axis (+Z of the camera frame) points toward the viewer.
    const Quat q = camera_->quaternion();
    const Vec3 right = Vec3::unit_x().applied(q);
    const Vec3 up = Vec3::unit_y().applied(q);
    const Vec3 back = Vec3::unit_z().applied(q);

    struct Nub {
        double x, y, depth;
        int axis;
    };
    std::array<Nub, 6> nubs{};
    for (int i = 0; i < 6; ++i) {
        const Vec3& d = kNavDirs[i];
        const double px = d.dot(right);
        const double py = d.dot(up);
        nubs[static_cast<std::size_t>(i)] = {36.0 + px * 26.0,
                                             36.0 - py * 26.0,
                                             static_cast<double>(d.dot(back)),
                                             i};
        // Click hit-testing stays in 72-space (handle_event scales the
        // pointer back to it).
        nav_nubs_[static_cast<std::size_t>(i)] = {36.0 + px * 26.0,
                                                  36.0 - py * 26.0, i};
    }
    std::sort(nubs.begin(), nubs.end(),
              [](const Nub& a, const Nub& n) { return a.depth < n.depth; });

    static const affineui::Color kLetterColor{0x10, 0x13, 0x1a, 0xff};
    static const affineui::Color kRingFill{0x22, 0x26, 0x2d, 0xff};
    for (const auto& n : nubs) {
        const int color_axis = n.axis % 3;
        const bool positive = n.axis < 3;
        const double alpha = n.depth < -0.05 ? 0.55 : 1.0;
        const affineui::Color axis_col =
            nav_alpha(kNavAxisColor[color_axis], alpha);
        if (positive) {
            // Spoke from the ball center to the nub, then the filled
            // nub with its letter (drawn as strokes, like the web's).
            p.stroke_line(gx(36.0), gy(36.0), gx(n.x), gy(n.y), axis_col,
                          sw(2.0));
            p.fill_circle(gx(n.x), gy(n.y), sw(7.5), axis_col);
            std::vector<float> d;
            auto rel = [&](double dx, double dy, bool move) {
                d.push_back(move ? affineui::kPathMove : affineui::kPathLine);
                d.push_back(gx(n.x + dx));
                d.push_back(gy(n.y + dy));
            };
            if (color_axis == 0) {  // X
                rel(-2.3, -2.7, true);
                rel(2.3, 2.7, false);
                rel(2.3, -2.7, true);
                rel(-2.3, 2.7, false);
            } else if (color_axis == 1) {  // Y
                rel(-2.3, -2.7, true);
                rel(0.0, 0.0, false);
                rel(2.3, -2.7, true);
                rel(0.0, 0.0, false);
                rel(0.0, 2.7, false);
            } else {  // Z
                rel(-2.2, -2.6, true);
                rel(2.2, -2.6, false);
                rel(-2.2, 2.6, false);
                rel(2.2, 2.6, false);
            }
            p.stroke_path(d.data(), d.size(),
                          affineui::PathPaint::solid(
                              nav_alpha(kLetterColor, alpha)),
                          sw(1.3));
        } else {
            // Negative axes: hollow ring.
            p.fill_circle(gx(n.x), gy(n.y), sw(6.0),
                          nav_alpha(kRingFill, alpha));
            p.stroke_arc(gx(n.x), gy(n.y), sw(6.0), 0.0f, 360.0f, axis_col,
                         sw(1.5));
        }
    }
}

void Viewport3D::snap_camera_to_axis(int axis) {
    if (axis < 0 || axis > 5) return;
    // Web snap(): put the camera `dist` down the axis from the orbit
    // target; OrbitControls.update() re-aims it and its pole clamp
    // keeps the straight up/down views from gimbal-flipping.
    const Vec3 dir = kNavDirs[axis];
    float dist = (camera_->position - orbit_->target).length();
    if (dist <= 0.0f) dist = 8.0f;
    camera_->position = orbit_->target + dir * dist;
    orbit_->update();
    mark_dirty();
}

// ── Input ───────────────────────────────────────────────────────────

Vec2 Viewport3D::to_ndc(double x, double y) const {
    const double w = std::max(1, canvas_rect_.w);
    const double h = std::max(1, canvas_rect_.h);
    return {static_cast<float>((x - canvas_rect_.x) / w * 2.0 - 1.0),
            static_cast<float>(-((y - canvas_rect_.y) / h * 2.0 - 1.0))};
}

void Viewport3D::pick(double mx, double my, bool additive) {
    Raycaster rc;
    rc.set_from_camera(to_ndc(mx, my), *camera_);
    std::vector<ObjectPtr> roots;
    roots.reserve(nodes_.size());
    for (const auto& [id, node] : nodes_) roots.push_back(node);
    const auto hits = rc.intersect_objects(roots);

    std::string hit_id;
    if (!hits.empty()) {
        // Walk up to the registered root (light markers are groups).
        const Object3D* n = hits[0].object;
        while (n != nullptr) {
            if (nodes_.contains(n->name)) {
                hit_id = n->name;
                break;
            }
            n = n->parent;
        }
    }
    auto* context = ctx_.get();
    if (context == nullptr) return;
    if (hit_id.empty()) {
        if (!additive) context->selection().clear();
    } else if (additive) {
        context->selection().toggle(hit_id);
    } else {
        context->selection().select(hit_id);
    }
}

bool Viewport3D::handle_event(
    const affineui::Event& ev,
    const std::vector<affineui::Document::HoverInfo>& chain) {
    using ET = affineui::EventType;
    const double mx = ev.pos.x;
    const double my = ev.pos.y;

    // Active drags own the pointer regardless of hover.
    if (gizmo_dragging_) {
        if (ev.type == ET::MouseMove) {
            gizmo_->pointer_move(to_ndc(mx, my));
            return true;
        }
        if (ev.type == ET::MouseUp) {
            gizmo_dragging_ = false;
            gizmo_->pointer_up();
            app_.release_pointer();
            mark_dirty();
            return true;
        }
        return false;
    }
    if (cam_dragging_) {
        if (ev.type == ET::MouseMove) {
            if (std::abs(mx - down_x_) > kClickSlopPx ||
                std::abs(my - down_y_) > kClickSlopPx) {
                drag_moved_ = true;
            }
            orbit_->pointer_move(static_cast<float>(mx),
                                 static_cast<float>(my));
            mark_dirty();
            return true;
        }
        if (ev.type == ET::MouseUp) {
            cam_dragging_ = false;
            orbit_->pointer_up();
            app_.release_pointer();
            if (drag_left_ && !drag_moved_) pick(mx, my, ev.shift);
            return true;
        }
        return false;
    }

    // Route by hover: only events over the viewport canvas (stats and
    // the floating tool rail decline to the normal UI path). The chain
    // carries element CLASSES (deepest first), not widget keys.
    bool over_scene = false;
    for (const auto& info : chain) {
        if (chain_entry_has(info, cfg_.stats_class)) continue;
        if (chain_entry_has(info, "dcs-btn") ||
            chain_entry_has(info, "dcs-toolbar")) {
            break;
        }
        if (chain_entry_has(info, cfg_.canvas_class)) {
            over_scene = true;
            break;
        }
    }
    if (!over_scene) return false;

    if (ev.type == ET::MouseMove) {
        // Gizmo hover highlight.
        if (gizmo_->pointer_hover(to_ndc(mx, my))) mark_dirty();
        return false;  // hover isn't consumed
    }
    if (ev.type == ET::MouseWheel) {
        orbit_->set_viewport(static_cast<float>(canvas_rect_.w),
                             static_cast<float>(canvas_rect_.h));
        orbit_->wheel(static_cast<float>(-ev.wheel_dy) * 100.0f);
        mark_dirty();
        return true;
    }
    if (ev.type == ET::MouseDown) {
        // Nav-ball nub click: snap the camera down that axis (empty
        // ball space falls through to the orbit drag, like the web's
        // pointer-events:none gizmo svg).
        if (ev.button == affineui::MouseButton::Left && nav_rect_.w > 0 &&
            mx >= nav_rect_.x && mx < nav_rect_.x + nav_rect_.w &&
            my >= nav_rect_.y && my < nav_rect_.y + nav_rect_.h) {
            const double s = std::min(nav_rect_.w, nav_rect_.h) / 72.0;
            const double lx = (mx - nav_rect_.x) / s;
            const double ly = (my - nav_rect_.y) / s;
            for (const NavNub& n : nav_nubs_) {
                const double dx = lx - n.x;
                const double dy = ly - n.y;
                if (dx * dx + dy * dy <= 10.0 * 10.0) {
                    snap_camera_to_axis(n.axis);
                    return true;
                }
            }
        }
        // Gizmo next: it consumes the press when a handle is hit.
        if (ev.button == affineui::MouseButton::Left &&
            gizmo_->pointer_down(to_ndc(mx, my))) {
            gizmo_dragging_ = true;
            app_.capture_pointer();
            mark_dirty();
            return true;
        }
        int button = -1;
        switch (ev.button) {
        case affineui::MouseButton::Left: button = 0; break;
        case affineui::MouseButton::Middle: button = 1; break;
        case affineui::MouseButton::Right: button = 2; break;
        default: break;
        }
        if (button < 0) return false;
        orbit_->set_viewport(static_cast<float>(canvas_rect_.w),
                             static_cast<float>(canvas_rect_.h));
        orbit_->pointer_down(static_cast<float>(mx),
                             static_cast<float>(my), button, ev.shift);
        cam_dragging_ = true;
        drag_left_ = button == 0;
        drag_moved_ = false;
        down_x_ = mx;
        down_y_ = my;
        app_.capture_pointer();
        return true;
    }
    return false;
}

}  // namespace viewport3d
