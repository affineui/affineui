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
#include <utility>
#include <vector>

namespace dender {

namespace {

const char* kPrefsPath = "dender.prefs.toml";
const char* kWorkspacePath = "dender.workspace.toml";

// App::set_view/rebuild_view — the persistent-View reconcile fast path.
// Measured 2026-07-03 (--profile) after Phase 0 (end-of-build coalescing,
// batched sink settle, content-derived dock identity, svg paint-only lane):
// clean rebuild 2.2 ms, camera-change interaction 8.6 ms — vs ~190 ms via
// legacy load_view and ~64 s on the pre-Phase-0 fast path. Flip to false
// only to A/B against the legacy reparse path.
constexpr bool kUseReconcileFastPath = true;

constexpr double kPi = 3.14159265358979323846;

// ── Undoable scene commands ─────────────────────────────────────────────────
// Built on the app template's agnostic Command base. The template stack passes
// its own (shadow) app::Document; these commands edit the captured
// DenderDocument instead — the base is agnostic about the undo strategy AND
// about the model being edited.

/// One TRS channel edit with drag coalescing. Channels 0-2 location, 3-5
/// rotation (radians), 6-8 scale.
class TransformEdit final : public app::Command {
public:
    TransformEdit(DenderDocument& scene, std::string id, int channel,
                  double value, std::string label)
        : Command("dender.setTransform", std::move(label)),
          scene_(scene),
          id_(std::move(id)),
          channel_(channel),
          new_(value) {
        args_.set("id", id_).set("channel", static_cast<long long>(channel_));
    }
    void redo(app::Document&) override {
        old_ = read();
        write(new_);
    }
    void undo(app::Document&) override { write(old_); }
    bool merge_with(const app::Command& next) override {
        const auto* o = dynamic_cast<const TransformEdit*>(&next);
        if (o == nullptr || o->id_ != id_ || o->channel_ != channel_) return false;
        new_ = o->new_;  // absorb the newer value, keep our captured old_
        return true;
    }

private:
    [[nodiscard]] double read() const {
        const SceneObject* obj = scene_.find(id_);
        if (obj == nullptr) return 0.0;
        const int axis = channel_ % 3;
        if (channel_ < 3) return DenderDocument::axis(obj->position, axis);
        if (channel_ < 6) return DenderDocument::axis(obj->rotation, axis);
        return DenderDocument::axis(obj->scale, axis);
    }
    void write(double value) {
        const int axis = channel_ % 3;
        if (channel_ < 3) scene_.set_position(id_, axis, value);
        else if (channel_ < 6) scene_.set_rotation(id_, axis, value);
        else scene_.set_scale(id_, axis, value);
    }

    DenderDocument& scene_;
    std::string id_;
    int channel_;
    double new_{0.0};
    double old_{0.0};
};

class RenameObject final : public app::Command {
public:
    RenameObject(DenderDocument& scene, std::string id, std::string name)
        : Command("dender.renameObject", "Rename"),
          scene_(scene),
          id_(std::move(id)),
          requested_(std::move(name)) {}
    void redo(app::Document&) override {
        const SceneObject* obj = scene_.find(id_);
        old_ = obj != nullptr ? obj->name : std::string{};
        applied_ = scene_.rename(id_, requested_);
    }
    void undo(app::Document&) override { scene_.rename(id_, old_); }
    [[nodiscard]] bool modifies() const override {
        return !applied_.empty() && applied_ != old_;
    }

private:
    DenderDocument& scene_;
    std::string id_, requested_, old_, applied_;
};

class AddObject final : public app::Command {
public:
    AddObject(DenderDocument& scene, std::string type)
        : Command("dender.addObject", "Add " + type),
          scene_(scene),
          type_(std::move(type)) {}
    void redo(app::Document&) override {
        prev_active_ = std::string(scene_.active_id());
        if (removed_.valid) {
            scene_.restore(removed_);  // redo after undo: identical id/name
            removed_ = {};
        } else {
            created_ = scene_.add(type_);
        }
        if (!created_.empty()) scene_.select(created_);
    }
    void undo(app::Document&) override {
        removed_ = scene_.remove(created_);
        if (!prev_active_.empty() && scene_.find(prev_active_) != nullptr) {
            scene_.select(prev_active_);
        } else {
            scene_.deselect();
        }
    }
    [[nodiscard]] bool modifies() const override { return !created_.empty(); }

private:
    DenderDocument& scene_;
    std::string type_, created_, prev_active_;
    DenderDocument::RemovedObject removed_{};
};

class DuplicateObject final : public app::Command {
public:
    DuplicateObject(DenderDocument& scene, std::string source)
        : Command("dender.duplicateObject", "Duplicate"),
          scene_(scene),
          source_(std::move(source)) {}
    void redo(app::Document&) override {
        prev_active_ = std::string(scene_.active_id());
        if (removed_.valid) {
            scene_.restore(removed_);
            removed_ = {};
        } else {
            created_ = scene_.duplicate(source_);
        }
        if (!created_.empty()) scene_.select(created_);
    }
    void undo(app::Document&) override {
        removed_ = scene_.remove(created_);
        if (!prev_active_.empty() && scene_.find(prev_active_) != nullptr) {
            scene_.select(prev_active_);
        } else {
            scene_.deselect();
        }
    }
    [[nodiscard]] bool modifies() const override { return !created_.empty(); }

private:
    DenderDocument& scene_;
    std::string source_, created_, prev_active_;
    DenderDocument::RemovedObject removed_{};
};

class DeleteObject final : public app::Command {
public:
    DeleteObject(DenderDocument& scene, std::string id)
        : Command("dender.deleteObject", "Delete"),
          scene_(scene),
          id_(std::move(id)) {}
    void redo(app::Document&) override { removed_ = scene_.remove(id_); }
    void undo(app::Document&) override { scene_.restore(removed_); }
    [[nodiscard]] bool modifies() const override { return removed_.valid; }

private:
    DenderDocument& scene_;
    std::string id_;
    DenderDocument::RemovedObject removed_{};
};

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

}  // namespace

DenderApp::DenderApp() : app_(make_config()) {
    load_settings();
    bind_keys();
    // Any stack change (push / undo / redo) refreshes the whole view.
    stack_.set_changed_handler(affineui::bind(this, &DenderApp::reload));
    app_.on_event([this](const affineui::Event& ev,
                         const std::vector<affineui::Document::HoverInfo>& chain) {
        return handle_event(ev, chain);
    });
    // Viewport runtime: orbit/pan/dolly interaction + the scene SVG cache.
    // Click-picks route back through the same selection path the outliner
    // uses; camera moves just ask for a rebuild (never undoable).
    viewport_.request_render = affineui::bind(this, &DenderApp::reload);
    viewport_.on_pick = [this](std::string_view id, bool shift) {
        pick_object(id, shift);
    };
    viewport_.attach(app_);
}

affineui::App::Config DenderApp::make_config() {
    affineui::App::Config cfg;
    cfg.title = "AffineUI - DENDER mini DCC";
    cfg.width = 1440;
    cfg.height = 900;
    cfg.clear_color = affineui::Color{0x14, 0x16, 0x1c, 0xff};
    cfg.high_dpi = true;
    // Perf HUD on by default while UI performance is being debugged
    // (user directive 2026-07-03): fps/ms graph + stage flags, top-right.
    cfg.perf_overlay = false;
    // frameworks/ is copied next to the exe; "examples" works from repo root.
    cfg.asset_folders = {".", "examples"};
    // Persist dock sizes after splitter drags and rebuild so structural
    // changes re-resolve from the saved placement (game_editor pattern).
    cfg.on_layout_changed = [this] {
        save_dock_layout();
        reload();
    };
    return cfg;
}

void DenderApp::load_settings() {
    prefs_.load(kPrefsPath);
    ws_.load(kWorkspacePath);
    document_.set_accent(prefs_.get_string("ui.accent", "orange"));
    document_.set_density(prefs_.get_string("ui.density", "comfortable"));
    document_.set_style(prefs_.get_string("ui.style", "flat"));
}

void DenderApp::save_settings() {
    prefs_.set_string("ui.accent", document_.theme().accent);
    prefs_.set_string("ui.density", document_.theme().density);
    prefs_.set_string("ui.style", document_.theme().style);
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
    // TODO(dender): Shift+A (open the Add menu at the cursor) and F2 (focus the
    // rename field) need programmatic menu-open / focus APIs the framework does
    // not expose yet.
}

int DenderApp::run() {
    boot();
    return app_.run();
}

void DenderApp::boot() {
    // Load styles directly (the app template's versioned loader) rather than
    // relying on <link> resolution; the bundle's base URL makes its url()s
    // (icon font) resolve like a <link>ed sheet.
    std::string bundle_base;
    stylesheet_ = app::read_framework_bundle(affineui::ViewTheme::Decius,
                                             kDeciusVersion, bundle_base);
    stylesheet_ += "\n";
    stylesheet_ += native_css();
    stylesheet_base_ = std::move(bundle_base);
    if (kUseReconcileFastPath) {
        // Persistent-View reconcile: interactions diff against the retained
        // view instead of reparsing the document (App::load_view becomes
        // bootstrap machinery only). set_stylesheet re-bootstraps the shell.
        app_.set_view(
            [this](affineui::View& v) { DenderView{*this}.build_into(v); });
        app_.set_stylesheet(stylesheet_, stylesheet_base_);
    } else {
        app_.load_view(build_view());
        app_.set_stylesheet(stylesheet_, stylesheet_base_);
    }
}

void DenderApp::reload() {
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
    // Theme selectors are document (body) attrs, which only apply when the
    // bootstrap shell is built — the reconcile sink does not diff document
    // attrs yet (framework T1 follow-up). Re-setting the stylesheet rebuilds
    // the shell with the current selectors; rare path (theme tweaks only).
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

    // ── Diff probe: is the View reconcile itself clean? ──────────────────
    // Two identical builds into a RemotePatchQueue. A sound diff emits zero
    // patches on the second pass; anything it emits is builder churn that
    // defeats reconcile (and multiplies whatever the document sink charges
    // per op).
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
        // Categorize ALL churn so the reconcile fix knows whether this is
        // one bug class (attr double-writes) or several.
        std::map<std::string, int> attr_hist;
        int structural = 0, text_ops = 0;
        for (const auto& p : q.patches()) {
            using Op = affineui::RemotePatchOp;
            if (p.op == Op::SetAttribute || p.op == Op::RemoveAttribute) {
                ++attr_hist[p.name];
            } else if (p.op == Op::SetText) {
                ++text_ops;
            } else {
                ++structural;
            }
        }
        for (const auto& [name, n] : attr_hist) {
            std::fprintf(stderr, "[profile]   churn attr '%s' x%d\n",
                         name.c_str(), n);
        }
        std::fprintf(stderr, "[profile]   churn structural=%d text=%d\n",
                     structural, text_ops);
        for (std::size_t i = 0; i < q.size(); ++i) {
            const auto& p = q.patches()[i];
            std::fprintf(stderr,
                         "[profile]   churn[%zu]: op=%d id=%s name=%s "
                         "value=%.60s\n",
                         i, static_cast<int>(p.op), p.id.c_str(),
                         p.name.c_str(), p.value.c_str());
        }
        std::fflush(stderr);
        if (iterations <= 0) return 0;  // --probe: churn analysis only
    }

    // ── Legacy path: App::load_view reparses the whole document ──────────
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

    // ── Fast path: persistent-View reconcile (App::set_view) ─────────────
    // Measured regardless of kUseReconcileFastPath so the A/B stays honest;
    // camera renders are routed straight to rebuild_view for this phase.
    std::fprintf(stderr, "[profile] legacy loop done\n");
    std::fflush(stderr);
    app_.set_view(
        [this](affineui::View& v) { DenderView{*this}.build_into(v); });
    app_.set_stylesheet(css, bundle_base);
    viewport_.request_render = [this] { app_.rebuild_view(); };
    std::fprintf(stderr, "[profile] set_view bootstrap done\n");
    std::fflush(stderr);
    app_.rebuild_view();  // warm the retained view
    std::fprintf(stderr, "[profile] warm rebuild done\n");
    std::fflush(stderr);

    // Live-document diff probe: with the document laid out and the dock
    // providers returning real state, is the emission stable pass-to-pass?
    {
        affineui::View pv;
        affineui::RemotePatchQueue q;
        pv.begin(&q);
        DenderView{*this}.build_into(pv);
        pv.end();
        q.clear();
        pv.begin(&q);
        DenderView{*this}.build_into(pv);
        pv.end();
        std::fprintf(stderr, "[profile] LIVE probe: clean rebuild %zu patches\n",
                     q.size());
        const std::size_t show = q.size() < 30 ? q.size() : 30;
        for (std::size_t i = 0; i < show; ++i) {
            const auto& p = q.patches()[i];
            std::fprintf(stderr,
                         "[profile]   live[%zu]: op=%d id=%s parent=%s tag=%s "
                         "name=%s value=%.40s\n",
                         i, static_cast<int>(p.op), p.id.c_str(),
                         p.parent_id.c_str(), p.tag.c_str(), p.name.c_str(),
                         p.value.c_str());
        }
        std::fflush(stderr);
    }

    std::vector<double> t_clean, t_camera, t_fast_layout;
    const int fast_iters = std::min(iterations, 1);
    for (int i = 0; i < fast_iters; ++i) {
        const auto t0 = clock::now();
        app_.rebuild_view();  // no state changed: pure diff cost
        const auto t1 = clock::now();
        // A camera nudge regenerates the scene SVG and reconciles the patch —
        // dolly() itself triggers the rebuild, so this times one real
        // orbit/dolly drag-move end to end.
        viewport_.dolly(i % 2 == 0 ? 1.01 : 1.0 / 1.01);
        const auto t2 = clock::now();
        (void)app_.document().find_element_rect("dn-vp-scenelayer");
        const auto t3 = clock::now();
        t_clean.push_back(ms(t1 - t0));
        t_camera.push_back(ms(t2 - t1));
        t_fast_layout.push_back(ms(t3 - t2));
        std::fprintf(stderr,
                     "[profile] iter %d: clean %.2f  camera %.2f  layout %.2f ms\n",
                     i, t_clean.back(), t_camera.back(), t_fast_layout.back());
        std::fflush(stderr);
    }

    const auto rect = app_.document().find_element_rect("dn-vp-scenelayer");
    const auto b = stats(t_build), l = stats(t_load), y = stats(t_layout);
    const auto fc = stats(t_clean), fm = stats(t_camera),
               fy = stats(t_fast_layout);
    std::fprintf(stderr, "[profile] iterations: %d, document: %zu KB\n",
                 iterations, html_bytes / 1024);
    std::fprintf(stderr, "[profile] LEGACY load_view (reparse) path:\n");
    std::fprintf(stderr,
                 "[profile]   view build       min %7.2f  med %7.2f  max %7.2f ms\n",
                 b[0], b[1], b[2]);
    std::fprintf(stderr,
                 "[profile]   load_view        min %7.2f  med %7.2f  max %7.2f ms\n",
                 l[0], l[1], l[2]);
    std::fprintf(stderr,
                 "[profile]   layout (forced)  min %7.2f  med %7.2f  max %7.2f ms\n",
                 y[0], y[1], y[2]);
    std::fprintf(stderr, "[profile]   per interaction: ~%.2f ms\n",
                 b[1] + l[1] + y[1]);
    std::fprintf(stderr, "[profile] FAST set_view/rebuild_view path:\n");
    std::fprintf(stderr,
                 "[profile]   clean rebuild    min %7.2f  med %7.2f  max %7.2f ms\n",
                 fc[0], fc[1], fc[2]);
    std::fprintf(stderr,
                 "[profile]   camera change    min %7.2f  med %7.2f  max %7.2f ms\n",
                 fm[0], fm[1], fm[2]);
    std::fprintf(stderr,
                 "[profile]   layout (forced)  min %7.2f  med %7.2f  max %7.2f ms\n",
                 fy[0], fy[1], fy[2]);
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
    document_.select(id);
    reload();
}

void DenderApp::pick_object(std::string_view id, bool shift) {
    if (id.empty()) {
        // Clicked empty space: deselect, unless shift (web discrimination).
        if (!shift && !document_.active_id().empty()) {
            document_.deselect();
            reload();
        }
        return;
    }
    if (shift && !document_.active_id().empty()) {
        document_.toggle_multi(id);  // additive multi-select membership
        reload();
        return;
    }
    select_object(id);
}

void DenderApp::rename_active(std::string_view name) {
    const SceneObject* obj = document_.active();
    if (obj == nullptr) return;
    stack_.push(std::make_unique<RenameObject>(document_, obj->id,
                                               std::string(name)));
}

void DenderApp::run_transform_edit(int axis, std::string_view value,
                                   int channel_base, std::string_view label) {
    const SceneObject* obj = document_.active();
    if (obj == nullptr || axis < 0 || axis > 2) return;
    double v = parse_double_or(value, 0.0);
    if (channel_base == 3) v = v * kPi / 180.0;  // inspector shows degrees
    stack_.begin_coalescing();
    stack_.push(std::make_unique<TransformEdit>(
        document_, obj->id, channel_base + axis, v, std::string(label)));
    stack_.end_coalescing();
}

void DenderApp::set_location(int axis, std::string_view value) {
    run_transform_edit(axis, value, 0, "Move");
}

void DenderApp::set_rotation_deg(int axis, std::string_view value) {
    run_transform_edit(axis, value, 3, "Rotate");
}

void DenderApp::set_scale(int axis, std::string_view value) {
    run_transform_edit(axis, value, 6, "Scale");
}

void DenderApp::add_object(std::string_view type) {
    stack_.push(std::make_unique<AddObject>(document_, std::string(type)));
}

void DenderApp::delete_active() {
    const SceneObject* obj = document_.active();
    if (obj == nullptr) return;
    stack_.push(std::make_unique<DeleteObject>(document_, obj->id));
}

void DenderApp::duplicate_active() {
    const SceneObject* obj = document_.active();
    if (obj == nullptr) return;
    stack_.push(std::make_unique<DuplicateObject>(document_, obj->id));
}

void DenderApp::undo() { stack_.undo(); }
void DenderApp::redo() { stack_.redo(); }

void DenderApp::set_tool(std::string_view tool) {
    ui_.active_tool = std::string(tool);
    // The web's MODE_FOR_TIP mapping: Move/Rotate/Scale drive the gizmo mode;
    // Transform maps to translate. Other rail tools leave the mode alone.
    if (tool == "move" || tool == "transform") {
        document_.set_transform_mode(TransformMode::Translate);
    } else if (tool == "rotate") {
        document_.set_transform_mode(TransformMode::Rotate);
    } else if (tool == "scale") {
        document_.set_transform_mode(TransformMode::Scale);
    }
    reload();
}

void DenderApp::set_shading(std::string_view mode) {
    if (mode == "wire") document_.set_shading(Shading::Wireframe);
    else if (mode == "tex") document_.set_shading(Shading::Texture);
    else if (mode == "render") document_.set_shading(Shading::Rendered);
    else document_.set_shading(Shading::Solid);
    reload();
}

void DenderApp::toggle_play() {
    document_.set_playing(!document_.playing());
    reload();
}

void DenderApp::set_frame(std::string_view value) {
    document_.set_frame(static_cast<int>(
        std::lround(parse_double_or(value, document_.timeline().frame))));
    reload();
}

void DenderApp::set_start(std::string_view value) {
    document_.set_start(static_cast<int>(
        std::lround(parse_double_or(value, document_.timeline().start))));
    reload();
}

void DenderApp::set_end(std::string_view value) {
    document_.set_end(static_cast<int>(
        std::lround(parse_double_or(value, document_.timeline().end))));
    reload();
}

void DenderApp::set_accent(std::string_view accent) {
    document_.set_accent(accent);
    save_settings();
    rebuild_shell();
}

void DenderApp::set_density(std::string_view density) {
    document_.set_density(density);
    save_settings();
    rebuild_shell();
}

void DenderApp::set_style(std::string_view style) {
    document_.set_style(style);
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
    reload();  // the selected tab's sheet content is built on demand
}

void DenderApp::set_npanel_tab(std::string_view id) {
    // The interaction layer already switched the visible tabpanel; recording
    // the choice keeps it across rebuilds.
    ui_.npanel_tab = std::string(id);
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
    // Bare-letter commands act only while the pointer is over the 3D viewport
    // (Blender semantics) — which also keeps them away from text editing in
    // the panels.
    if (!chain_has_class(chain, "dn-vp-canvas")) return false;
    if (action == "dender.mode.translate") { set_tool("move"); return true; }
    if (action == "dender.mode.rotate") { set_tool("rotate"); return true; }
    if (action == "dender.mode.scale") { set_tool("scale"); return true; }
    if (action == "dender.delete") { delete_active(); return true; }
    if (action == "dender.duplicate") { duplicate_active(); return true; }
    if (action == "dender.frame") { viewport_.frame_selected(); return true; }
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

    if (scrubbing_) {
        if (ev.type == ET::MouseMove) {
            const auto scale = ui::TimelineScale::for_width(scrub_bounds_.w);
            const int frame = scale.frame_at(ev.pos.x - scrub_bounds_.x);
            if (frame != document_.timeline().frame) {
                document_.scrub_frame(frame);
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
            document_.scrub_frame(
                scale.frame_at(ev.pos.x - scrub_bounds_.x));
            reload();
            return true;
        }
    }
    return false;
}

}  // namespace dender
