#include "ge_viewport.h"

#include <algorithm>
#include <cmath>

#include "sokol_app.h"  // sapp_dpi_scale for crisp high-DPI targets

namespace ge {

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

GeViewport::GeViewport() {
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

GeViewport::~GeViewport() = default;

void GeViewport::attach(affineui::App& app, app::Context& ctx) {
    app_ = &app;
    ctx_ = &ctx;
    renderer_ = std::make_unique<Renderer>();

    app.set_custom_paint(kPaintName,
                         [this](affineui::Painter& p, const Rect& r) {
                             paint(p, r);
                         });
    app.on_frame([this](double dt) { frame(dt); });

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
    gizmo_->on_object_change = [this] { mark_dirty(); };

    sync_document();
    sync_selection();
}

void GeViewport::build(affineui::View& v) {
    v.canvas(kPaintName, "ge-vp-3dcanvas", "ge-vp-3d");
}

// ── Scene <-> app document ──────────────────────────────────────────

ObjectPtr GeViewport::make_node(const app::Object& obj, std::size_t index) {
    // Deterministic primitive + placement per object type. Objects the
    // user adds later land on a ring so they never stack.
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

void GeViewport::sync_document() {
    if (ctx_ == nullptr) return;
    const auto& objects = ctx_->document().objects();

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
    mark_dirty();
}

void GeViewport::sync_selection() {
    if (ctx_ == nullptr) return;
    ObjectPtr target;
    if (!ctx_->selection().empty()) {
        // Box + gizmo follow the primary (first) selected object.
        for (const auto& obj : ctx_->document().objects()) {
            if (ctx_->selection().contains(obj.id)) {
                const auto it = nodes_.find(obj.id);
                if (it != nodes_.end()) target = it->second;
                break;
            }
        }
    }
    selected_node_ = target;
    if (target && tool_ != "select") {
        gizmo_->attach(target);
    } else {
        gizmo_->detach();
    }
    mark_dirty();
}

void GeViewport::set_tool(std::string_view tool) {
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

// ── Undo ────────────────────────────────────────────────────────────

void GeViewport::apply_transform_command(const ObjectPtr& node,
                                         const Vec3& old_p,
                                         const Quat& old_q,
                                         const Vec3& old_s) {
    if (!node || ctx_ == nullptr) return;
    const Vec3 new_p = node->position;
    const Quat new_q = node->quaternion();
    const Vec3 new_s = node->scale;
    if (new_p == old_p && new_s == old_s &&
        new_q.angle_to(old_q) < 1e-6f) {
        return;  // no actual change
    }
    // The node is captured by shared_ptr so undo stays valid even after
    // the object is removed and re-added; `this` outlives the stack
    // (both are owned by the GameEditor controller).
    auto apply = [this, node](const Vec3& p, const Quat& q, const Vec3& s) {
        node->position = p;
        node->set_quaternion(q);
        node->scale = s;
        mark_dirty();
    };
    ctx_->stack().push(std::make_unique<app::LambdaCommand>(
        "obj.transform", "Transform " + node->name,
        [apply, new_p, new_q, new_s](app::Document&) {
            apply(new_p, new_q, new_s);
        },
        [apply, old_p, old_q, old_s](app::Document&) {
            apply(old_p, old_q, old_s);
        }));
}

void GeViewport::push_transform_command() {
    apply_transform_command(gizmo_->object(), start_position_,
                            start_quaternion_, start_scale_);
}

// ── Inspector bridge ────────────────────────────────────────────────

std::optional<GeViewport::ObjectTransform> GeViewport::transform_of(
    std::string_view id) const {
    const auto it = nodes_.find(std::string(id));
    if (it == nodes_.end()) return std::nullopt;
    const Object3D& n = *it->second;
    const Euler e = n.rotation();
    ObjectTransform t;
    t.location = n.position;
    t.rotation_deg = {rad_to_deg(e.x), rad_to_deg(e.y), rad_to_deg(e.z)};
    t.scale = n.scale;
    return t;
}

namespace {
float& vec_axis(Vec3& v, int axis) {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}
}  // namespace

void GeViewport::set_location(std::string_view id, int axis, double value) {
    const auto it = nodes_.find(std::string(id));
    if (it == nodes_.end() || axis < 0 || axis > 2) return;
    const ObjectPtr& node = it->second;
    const Vec3 old_p = node->position;
    Vec3 p = old_p;
    vec_axis(p, axis) = static_cast<float>(value);
    node->position = p;
    apply_transform_command(node, old_p, node->quaternion(), node->scale);
}

void GeViewport::set_rotation_deg(std::string_view id, int axis,
                                  double value) {
    const auto it = nodes_.find(std::string(id));
    if (it == nodes_.end() || axis < 0 || axis > 2) return;
    const ObjectPtr& node = it->second;
    const Quat old_q = node->quaternion();
    Euler e = node->rotation();
    float* const channel[3] = {&e.x, &e.y, &e.z};
    *channel[axis] = deg_to_rad(static_cast<float>(value));
    node->set_rotation(e);
    apply_transform_command(node, node->position, old_q, node->scale);
}

void GeViewport::set_scale(std::string_view id, int axis, double value) {
    const auto it = nodes_.find(std::string(id));
    if (it == nodes_.end() || axis < 0 || axis > 2) return;
    const ObjectPtr& node = it->second;
    const Vec3 old_s = node->scale;
    Vec3 s = old_s;
    vec_axis(s, axis) = static_cast<float>(value);
    node->scale = s;
    apply_transform_command(node, node->position, node->quaternion(), old_s);
}

// ── Frame / paint ───────────────────────────────────────────────────

void GeViewport::mark_dirty() {
    dirty_ = true;
    if (app_ != nullptr) app_->request_custom_repaint(kPaintName);
}

void GeViewport::frame(double /*dt*/) {
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
    if (app_ != nullptr) app_->request_custom_repaint(kPaintName);
}

void GeViewport::paint(affineui::Painter& p, const Rect& r) {
    if (r.w != canvas_rect_.w || r.h != canvas_rect_.h) dirty_ = true;
    canvas_rect_ = r;
    if (renderer_ == nullptr) return;
    const std::uint32_t image = renderer_->painter_image(p);
    if (image != 0) p.draw_image(image, r, r);
}

// ── Input ───────────────────────────────────────────────────────────

Vec2 GeViewport::to_ndc(double x, double y) const {
    const double w = std::max(1, canvas_rect_.w);
    const double h = std::max(1, canvas_rect_.h);
    return {static_cast<float>((x - canvas_rect_.x) / w * 2.0 - 1.0),
            static_cast<float>(-((y - canvas_rect_.y) / h * 2.0 - 1.0))};
}

void GeViewport::pick(double mx, double my, bool additive) {
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
    if (ctx_ == nullptr) return;
    if (hit_id.empty()) {
        if (!additive) ctx_->selection().clear();
    } else if (additive) {
        ctx_->selection().toggle(hit_id);
    } else {
        ctx_->selection().select(hit_id);
    }
}

bool GeViewport::handle_event(
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
            if (app_ != nullptr) app_->release_pointer();
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
            if (app_ != nullptr) app_->release_pointer();
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
        if (chain_entry_has(info, "ge-vp-stats")) continue;
        if (chain_entry_has(info, "dcs-btn") ||
            chain_entry_has(info, "dcs-toolbar")) {
            break;
        }
        if (chain_entry_has(info, "ge-vp-canvas")) {
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
        // Gizmo first: it consumes the press when a handle is hit.
        if (ev.button == affineui::MouseButton::Left &&
            gizmo_->pointer_down(to_ndc(mx, my))) {
            gizmo_dragging_ = true;
            if (app_ != nullptr) app_->capture_pointer();
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
        if (app_ != nullptr) app_->capture_pointer();
        return true;
    }
    return false;
}

}  // namespace ge
