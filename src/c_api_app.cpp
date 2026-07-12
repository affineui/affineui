// extern "C" app-level surface: App / Document / View / WidgetRef.
//
// Thin translation over the C++ API, upholding the binding contracts in
// docs/LANGUAGE_BINDINGS.md: every function null-checks and no-ops rather
// than faults; out-strings are heap copies freed by affineui_string_free;
// callback user data is released through user_free exactly once.

#include "affineui/app.h"
#include "affineui/c_api_app.h"
#include "affineui/decius_bundle.h"
#include "affineui/document.h"
#include "affineui/types.h"
#include "affineui/view.h"
#include "affineui/virtual_list.h"

#include "c_api_util.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using affineui_c::dup_string;
using affineui_c::hold_user;
using affineui_c::sv;

namespace {

affineui::App*       to_app(affineui_app* a) { return reinterpret_cast<affineui::App*>(a); }
const affineui::App* to_app(const affineui_app* a) { return reinterpret_cast<const affineui::App*>(a); }
affineui::Document*       to_doc(affineui_document* d) { return reinterpret_cast<affineui::Document*>(d); }
const affineui::Document* to_doc(const affineui_document* d) { return reinterpret_cast<const affineui::Document*>(d); }
affineui::View*       to_view(affineui_view* v) { return reinterpret_cast<affineui::View*>(v); }
const affineui::View* to_view(const affineui_view* v) { return reinterpret_cast<const affineui::View*>(v); }
affineui::WidgetRef*       to_widget(affineui_widget* w) { return reinterpret_cast<affineui::WidgetRef*>(w); }
const affineui::WidgetRef* to_widget(const affineui_widget* w) { return reinterpret_cast<const affineui::WidgetRef*>(w); }

affineui_view* view_handle(affineui::View& v) {
    return reinterpret_cast<affineui_view*>(&v);
}

// Wrap a returned WidgetRef in a caller-owned handle.
affineui_widget* wrap(affineui::WidgetRef ref) {
    return reinterpret_cast<affineui_widget*>(new affineui::WidgetRef(std::move(ref)));
}

std::vector<std::string> to_strings(const char* const* items, size_t count) {
    std::vector<std::string> out;
    if (items == nullptr) return out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) out.emplace_back(items[i] ? items[i] : "");
    return out;
}

// A build callback invoked synchronously with the live view. `build` may
// be null (empty scope).
void run_build(affineui_build_fn build, void* user, affineui::View& view) {
    if (build) build(user, view_handle(view));
}

std::function<void(affineui::View&)> build_fn(affineui_build_fn build, void* user) {
    if (!build) return [](affineui::View&) {};
    return [build, user](affineui::View& v) { build(user, view_handle(v)); };
}

// ── ABI locks for the app-surface enums ──────────────────────────────
static_assert(AFFINEUI_THEME_PLAIN == static_cast<int>(affineui::ViewTheme::Plain));
static_assert(AFFINEUI_THEME_BOOTSTRAP == static_cast<int>(affineui::ViewTheme::Bootstrap));
static_assert(AFFINEUI_THEME_DECIUS == static_cast<int>(affineui::ViewTheme::Decius));
static_assert(AFFINEUI_WIDGET_ROOT == static_cast<int>(affineui::WidgetKind::Root));
static_assert(AFFINEUI_WIDGET_HEADING == static_cast<int>(affineui::WidgetKind::Heading));
static_assert(AFFINEUI_WIDGET_BUTTON == static_cast<int>(affineui::WidgetKind::Button));
static_assert(AFFINEUI_WIDGET_TEXT_INPUT == static_cast<int>(affineui::WidgetKind::TextInput));
static_assert(AFFINEUI_WIDGET_CARD == static_cast<int>(affineui::WidgetKind::Card));
static_assert(AFFINEUI_SELECT_REPLACE == static_cast<int>(affineui::SelectMod::Replace));
static_assert(AFFINEUI_SELECT_TOGGLE == static_cast<int>(affineui::SelectMod::Toggle));
static_assert(AFFINEUI_SELECT_RANGE == static_cast<int>(affineui::SelectMod::Range));
static_assert(AFFINEUI_AXIS_VERTICAL == static_cast<int>(affineui::Axis::Vertical));
static_assert(AFFINEUI_AXIS_HORIZONTAL == static_cast<int>(affineui::Axis::Horizontal));

}  // namespace

extern "C" {

// ── App ──────────────────────────────────────────────────────────────

void affineui_app_config_init(affineui_app_config* cfg) {
    if (!cfg) return;
    static const affineui::App::Config defaults{};
    *cfg = affineui_app_config{};
    cfg->title       = "AffineUI";
    cfg->width       = defaults.width;
    cfg->height      = defaults.height;
    cfg->clear_color = affineui_color{defaults.clear_color.r, defaults.clear_color.g,
                                      defaults.clear_color.b, defaults.clear_color.a};
    cfg->high_dpi            = defaults.high_dpi ? 1 : 0;
    cfg->vsync               = defaults.vsync ? 1 : 0;
    cfg->default_font_family = "sans-serif";
    cfg->default_font_size   = defaults.default_font_size;
    cfg->asset_folders       = nullptr;  // create() interprets null as {"."}
    cfg->asset_folder_count  = 0;
    cfg->perf_overlay        = defaults.perf_overlay ? 1 : 0;
    cfg->no_bundle_decius    = defaults.no_bundle_decius ? 1 : 0;
}

affineui_app* affineui_app_create(const affineui_app_config* cfg) {
    affineui::App::Config cpp{};
    if (cfg) {
        if (cfg->title) cpp.title = cfg->title;
        if (cfg->width > 0) cpp.width = cfg->width;
        if (cfg->height > 0) cpp.height = cfg->height;
        cpp.clear_color = affineui::Color{cfg->clear_color.r, cfg->clear_color.g,
                                          cfg->clear_color.b, cfg->clear_color.a};
        cpp.high_dpi = cfg->high_dpi != 0;
        cpp.vsync    = cfg->vsync != 0;
        if (cfg->default_font_family) cpp.default_font_family = cfg->default_font_family;
        if (cfg->default_font_size > 0) cpp.default_font_size = cfg->default_font_size;
        if (cfg->asset_folders && cfg->asset_folder_count > 0) {
            cpp.asset_folders = to_strings(cfg->asset_folders, cfg->asset_folder_count);
        }
        cpp.perf_overlay = cfg->perf_overlay != 0;
        cpp.no_bundle_decius = cfg->no_bundle_decius != 0;
    }
    return reinterpret_cast<affineui_app*>(new affineui::App(std::move(cpp)));
}

void affineui_app_destroy(affineui_app* app) {
    delete to_app(app);
}

void affineui_app_load_html(affineui_app* app, const char* html) {
    if (!app) return;
    to_app(app)->load_html(sv(html));
}

int affineui_app_load_html_file(affineui_app* app, const char* path) {
    if (!app || !path) return 0;
    return to_app(app)->load_html_file(std::string_view{path}) ? 1 : 0;
}

void affineui_app_load_view(affineui_app* app, const affineui_view* view) {
    if (!app || !view) return;
    to_app(app)->load_view(*to_view(view));
}

void affineui_app_set_stylesheet(affineui_app* app, const char* css, const char* base_url) {
    if (!app) return;
    if (base_url) {
        to_app(app)->set_stylesheet(sv(css), std::string_view{base_url});
    } else {
        to_app(app)->set_stylesheet(sv(css));
    }
}

void affineui_app_invalidate(affineui_app* app) {
    if (app) to_app(app)->invalidate();
}

void affineui_app_set_perf_overlay_enabled(affineui_app* app, int enabled) {
    if (app) to_app(app)->set_perf_overlay_enabled(enabled != 0);
}

int affineui_app_perf_overlay_enabled(const affineui_app* app) {
    return app && to_app(app)->perf_overlay_enabled() ? 1 : 0;
}

int affineui_app_dispatch(affineui_app* app, const affineui_event* ev) {
    if (!app || !ev) return 0;
    return to_app(app)->dispatch(affineui_c::to_event(*ev)) ? 1 : 0;
}

void affineui_app_on_event_capture(affineui_app* app,
                                   affineui_event_capture_fn fn,
                                   void* user,
                                   affineui_user_free_fn user_free) {
    if (!app || !fn) {
        if (user_free) user_free(user);
        return;
    }
    auto data = hold_user(user, user_free);
    to_app(app)->on_event_capture(
        [fn, data = std::move(data)](
            const affineui::Event& ev,
            const std::vector<affineui::Document::HoverInfo>&) {
            const affineui_event c_ev = affineui_c::to_c_event(ev);
            return fn(data->user, &c_ev) != 0;
        });
}

int affineui_app_run(affineui_app* app) {
    if (!app) return 1;
    return to_app(app)->run();
}

void affineui_app_quit(affineui_app* app, int code) {
    if (app) to_app(app)->quit(code);
}

void affineui_app_window_size(const affineui_app* app, int* out_w, int* out_h) {
    affineui::Size size{};
    if (app) size = to_app(app)->window_size();
    if (out_w) *out_w = size.width;
    if (out_h) *out_h = size.height;
}

void affineui_app_framebuffer_size(const affineui_app* app, int* out_w, int* out_h) {
    affineui::Size size{};
    if (app) size = to_app(app)->framebuffer_size();
    if (out_w) *out_w = size.width;
    if (out_h) *out_h = size.height;
}

float affineui_app_dpi_scale(const affineui_app* app) {
    return app ? to_app(app)->dpi_scale() : 1.0f;
}

affineui_document* affineui_app_document(affineui_app* app) {
    if (!app) return nullptr;
    return reinterpret_cast<affineui_document*>(&to_app(app)->document());
}

// ── Document ─────────────────────────────────────────────────────────

affineui_document* affineui_document_create(void) {
    return reinterpret_cast<affineui_document*>(new affineui::Document());
}

void affineui_document_destroy(affineui_document* doc) {
    delete to_doc(doc);
}

void affineui_document_set_html(affineui_document* doc, const char* html) {
    if (doc) to_doc(doc)->set_html(sv(html));
}

void affineui_document_set_user_stylesheet(affineui_document* doc,
                                           const char* css,
                                           const char* base_url) {
    if (!doc) return;
    if (base_url) {
        to_doc(doc)->set_user_stylesheet(sv(css), std::string_view{base_url});
    } else {
        to_doc(doc)->set_user_stylesheet(sv(css));
    }
}

void affineui_document_reload_stylesheets(affineui_document* doc) {
    if (doc) to_doc(doc)->reload_stylesheets();
}

void affineui_document_layout(affineui_document* doc,
                              int viewport_width,
                              int viewport_height) {
    if (doc) to_doc(doc)->layout(viewport_width, viewport_height);
}

void affineui_document_content_size(const affineui_document* doc,
                                    int* out_w, int* out_h) {
    affineui::Size size{};
    if (doc) size = to_doc(doc)->content_size();
    if (out_w) *out_w = size.width;
    if (out_h) *out_h = size.height;
}

int affineui_document_set_attribute_by_id(affineui_document* doc,
                                          const char* elem_id,
                                          const char* name,
                                          const char* value) {
    if (!doc || !elem_id || !name) return 0;
    return to_doc(doc)->set_attribute_by_id(std::string_view{elem_id},
                                            std::string_view{name}, sv(value))
               ? 1
               : 0;
}

int affineui_document_remove_attribute_by_id(affineui_document* doc,
                                             const char* elem_id,
                                             const char* name) {
    if (!doc || !elem_id || !name) return 0;
    return to_doc(doc)->remove_attribute_by_id(std::string_view{elem_id},
                                               std::string_view{name})
               ? 1
               : 0;
}

int affineui_document_set_text_by_id(affineui_document* doc,
                                     const char* elem_id,
                                     const char* text) {
    if (!doc || !elem_id) return 0;
    return to_doc(doc)->set_text_by_id(std::string_view{elem_id}, sv(text)) ? 1 : 0;
}

void affineui_document_dispatch(affineui_document* doc,
                                const affineui_event* ev,
                                affineui_dispatch_result* out) {
    affineui::DispatchResult result{};
    if (doc && ev) result = to_doc(doc)->dispatch(affineui_c::to_event(*ev));
    if (out) {
        out->redraw_requested     = result.redraw_requested ? 1 : 0;
        out->invalidate_view      = result.invalidate_view ? 1 : 0;
        out->defer_widget_changes = result.defer_widget_changes ? 1 : 0;
        out->layout_changed       = result.layout_changed ? 1 : 0;
        out->event_consumed       = result.event_consumed ? 1 : 0;
    }
}

void affineui_document_set_caret_blink_interval(affineui_document* doc,
                                                 double milliseconds) {
    if (doc) to_doc(doc)->set_caret_blink_interval(milliseconds);
}

double affineui_document_caret_blink_interval(const affineui_document* doc) {
    return doc ? to_doc(doc)->caret_blink_interval() : 0.0;
}

int affineui_document_tick_caret_blink(affineui_document* doc) {
    return doc && to_doc(doc)->tick_caret_blink() ? 1 : 0;
}

void affineui_document_attach_script(affineui_document* doc, int script) {
    if (doc) to_doc(doc)->attach_script(static_cast<affineui::DocumentScript>(script));
}

void affineui_document_detach_script(affineui_document* doc, int script) {
    if (doc) to_doc(doc)->detach_script(static_cast<affineui::DocumentScript>(script));
}

int affineui_document_hovered_cursor(const affineui_document* doc) {
    return doc ? to_doc(doc)->hovered_cursor() : 0;
}

// ── View ─────────────────────────────────────────────────────────────

affineui_view* affineui_view_create(int theme) {
    return reinterpret_cast<affineui_view*>(
        new affineui::View(static_cast<affineui::ViewTheme>(theme)));
}

void affineui_view_destroy(affineui_view* view) {
    delete to_view(view);
}

void affineui_view_clear(affineui_view* view) {
    if (view) to_view(view)->clear();
}

void affineui_view_begin(affineui_view* view) {
    if (view) to_view(view)->begin();
}

void affineui_view_end(affineui_view* view) {
    if (view) to_view(view)->end();
}

void affineui_view_set_theme(affineui_view* view, int theme) {
    if (view) to_view(view)->set_theme(static_cast<affineui::ViewTheme>(theme));
}

int affineui_view_get_theme(const affineui_view* view) {
    return view ? static_cast<int>(to_view(view)->theme()) : AFFINEUI_THEME_PLAIN;
}

void affineui_view_set_framework_version(affineui_view* view, const char* version) {
    if (view) to_view(view)->set_framework_version(sv(version));
}

void affineui_view_selector(affineui_view* view, const char* name, const char* value) {
    if (view && name) to_view(view)->selector(std::string_view{name}, sv(value));
}

char* affineui_view_to_html_fragment(const affineui_view* view) {
    if (!view) return dup_string({});
    return dup_string(to_view(view)->to_html_fragment());
}

char* affineui_view_to_html_document(const affineui_view* view) {
    if (!view) return dup_string({});
    return dup_string(to_view(view)->to_html_document());
}

affineui_widget* affineui_view_heading(affineui_view* view, int level,
                                       const char* text, const char* classes,
                                       const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->heading(level, sv(text), sv(classes), sv(key)));
}

affineui_widget* affineui_view_paragraph(affineui_view* view, const char* text,
                                         const char* classes, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->paragraph(sv(text), sv(classes), sv(key)));
}

affineui_widget* affineui_view_text(affineui_view* view, const char* text,
                                    const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->text(sv(text), sv(key)));
}

affineui_widget* affineui_view_html(affineui_view* view, const char* markup,
                                    const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->html(sv(markup), sv(key)));
}

affineui_widget* affineui_view_button(affineui_view* view, const char* label,
                                      int primary, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->button(sv(label), primary != 0, sv(key)));
}

affineui_widget* affineui_view_checkbox(affineui_view* view, const char* label,
                                        int checked, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->checkbox(sv(label), checked != 0, sv(key)));
}

affineui_widget* affineui_view_toggle(affineui_view* view, const char* label,
                                      int on, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->toggle(sv(label), on != 0, sv(key)));
}

affineui_widget* affineui_view_input(affineui_view* view, const char* label,
                                     const char* value, const char* type,
                                     const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->input(sv(label), sv(value),
                                     type ? std::string_view{type} : "text", sv(key)));
}

affineui_widget* affineui_view_password(affineui_view* view, const char* label,
                                        const char* value, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->password(sv(label), sv(value), sv(key)));
}

affineui_widget* affineui_view_textarea(affineui_view* view, const char* label,
                                        const char* value, int rows,
                                        const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->textarea(sv(label), sv(value), rows > 0 ? rows : 3, sv(key)));
}

affineui_widget* affineui_view_dropdown(affineui_view* view, const char* label,
                                        const char* const* options,
                                        size_t option_count,
                                        const char* selected, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->dropdown(sv(label), to_strings(options, option_count),
                                        sv(selected), sv(key)));
}

affineui_widget* affineui_view_button_group(affineui_view* view, const char* label,
                                            const char* const* options,
                                            size_t option_count,
                                            const char* selected, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->button_group(sv(label), to_strings(options, option_count),
                                            sv(selected), sv(key)));
}

affineui_widget* affineui_view_slider(affineui_view* view, const char* label,
                                      double value, double min, double max,
                                      const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->slider(sv(label), value, min, max, sv(key)));
}

affineui_widget* affineui_view_knob(affineui_view* view, const char* label,
                                    double value, double min, double max,
                                    int bipolar, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->knob(sv(label), value, min, max, bipolar != 0, sv(key)));
}

affineui_widget* affineui_view_combo(affineui_view* view, const char* label,
                                     double value, double step, const char* key,
                                     int linear) {
    if (!view) return nullptr;
    return wrap(to_view(view)->combo(sv(label), value, step, sv(key),
                                     linear != 0));
}

affineui_widget* affineui_view_color_field(affineui_view* view, const char* label,
                                           const char* value,
                                           const char* const* swatches,
                                           size_t swatch_count, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->color_field(sv(label), sv(value),
                                           to_strings(swatches, swatch_count), sv(key)));
}

affineui_widget* affineui_view_colorfield(affineui_view* view, const char* label,
                                          const char* value, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->colorfield(sv(label), sv(value), sv(key)));
}

affineui_widget* affineui_view_container(affineui_view* view, const char* classes,
                                         const char* key, affineui_build_fn build,
                                         void* user) {
    if (!view) return nullptr;
    if (!build) return wrap(to_view(view)->container_ref(sv(classes), sv(key)));
    auto scope = to_view(view)->container(sv(classes), sv(key));
    auto ref   = scope.ref();
    run_build(build, user, *to_view(view));
    return wrap(std::move(ref));
}

affineui_widget* affineui_view_element(affineui_view* view, const char* tag,
                                       const char* classes, const char* key,
                                       affineui_build_fn build, void* user) {
    if (!view || !tag) return nullptr;
    if (!build) return wrap(to_view(view)->element_ref(std::string_view{tag},
                                                       sv(classes), sv(key)));
    auto scope = to_view(view)->element(std::string_view{tag}, sv(classes), sv(key));
    auto ref   = scope.ref();
    run_build(build, user, *to_view(view));
    return wrap(std::move(ref));
}

affineui_widget* affineui_view_panel(affineui_view* view, const char* key,
                                     affineui_build_fn build, void* user) {
    if (!view) return nullptr;
    if (!build) return wrap(to_view(view)->panel_ref(sv(key)));
    auto scope = to_view(view)->panel(sv(key));
    auto ref   = scope.ref();
    run_build(build, user, *to_view(view));
    return wrap(std::move(ref));
}

affineui_widget* affineui_view_card(affineui_view* view, const char* title,
                                    const char* classes, const char* key,
                                    affineui_build_fn build, void* user) {
    if (!view) return nullptr;
    auto scope = to_view(view)->card(sv(title), sv(classes), sv(key));
    auto ref   = scope.ref();
    run_build(build, user, *to_view(view));
    return wrap(std::move(ref));
}

affineui_widget* affineui_view_toolbar(affineui_view* view, const char* key,
                                       affineui_build_fn build, void* user) {
    if (!view) return nullptr;
    auto scope = to_view(view)->toolbar(sv(key));
    auto ref   = scope.ref();
    run_build(build, user, *to_view(view));
    return wrap(std::move(ref));
}

affineui_widget* affineui_view_menu_bar(affineui_view* view, const char* key,
                                        affineui_build_fn build, void* user) {
    if (!view) return nullptr;
    auto scope = to_view(view)->menu_bar(sv(key));
    auto ref   = scope.ref();
    run_build(build, user, *to_view(view));
    return wrap(std::move(ref));
}

affineui_widget* affineui_view_status_bar(affineui_view* view, const char* key,
                                          affineui_build_fn build, void* user) {
    if (!view) return nullptr;
    auto scope = to_view(view)->status_bar(sv(key));
    auto ref   = scope.ref();
    run_build(build, user, *to_view(view));
    return wrap(std::move(ref));
}

affineui_widget* affineui_view_tree(affineui_view* view, const char* key,
                                    affineui_build_fn build, void* user) {
    if (!view) return nullptr;
    auto scope = to_view(view)->tree(sv(key));
    auto ref   = scope.ref();
    run_build(build, user, *to_view(view));
    return wrap(std::move(ref));
}

affineui_widget* affineui_view_foldout(affineui_view* view, const char* title,
                                       int expanded, const char* key,
                                       affineui_build_fn build, void* user) {
    if (!view) return nullptr;
    auto scope = to_view(view)->foldout(sv(title), expanded != 0, sv(key));
    auto ref   = scope.ref();
    run_build(build, user, *to_view(view));
    return wrap(std::move(ref));
}

affineui_widget* affineui_view_toolbar_separator(affineui_view* view, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->toolbar_separator(sv(key)));
}

affineui_widget* affineui_view_icon_button(affineui_view* view, const char* icon,
                                           const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->icon_button(sv(icon), sv(key)));
}

affineui_widget* affineui_view_menu_button(affineui_view* view, const char* label,
                                           affineui_build_fn build, void* user,
                                           const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->menu_button(sv(label), build_fn(build, user), sv(key)));
}

affineui_widget* affineui_view_menu_item(affineui_view* view, const char* label,
                                         const char* icon, const char* shortcut,
                                         const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->menu_item(sv(label), sv(icon), sv(shortcut), sv(key)));
}

affineui_widget* affineui_view_menu_separator(affineui_view* view, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->menu_separator(sv(key)));
}

affineui_widget* affineui_view_submenu(affineui_view* view, const char* label,
                                       affineui_build_fn build, void* user,
                                       const char* icon, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->submenu(sv(label), build_fn(build, user), sv(icon), sv(key)));
}

affineui_widget* affineui_view_menu_brand(affineui_view* view, const char* title,
                                          const char* icon, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->menu_brand(sv(title), sv(icon), sv(key)));
}

affineui_widget* affineui_view_menu_spacer(affineui_view* view, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->menu_spacer(sv(key)));
}

affineui_widget* affineui_view_menu_meta(affineui_view* view, const char* text,
                                         const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->menu_meta(sv(text), sv(key)));
}

affineui_widget* affineui_view_tree_row(affineui_view* view, const char* label,
                                        int selected, int depth, const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->tree_row(sv(label), selected != 0, depth, sv(key)));
}

affineui_widget* affineui_view_splitter(affineui_view* view, int horizontal,
                                        const char* key) {
    if (!view) return nullptr;
    return wrap(to_view(view)->splitter(horizontal != 0, sv(key)));
}

affineui_widget* affineui_view_find_widget(affineui_view* view, const char* name) {
    if (!view) return nullptr;
    return wrap(to_view(view)->find_widget(sv(name)));
}

// ── Widget ───────────────────────────────────────────────────────────

void affineui_widget_destroy(affineui_widget* w) {
    delete to_widget(w);
}

int affineui_widget_valid(const affineui_widget* w) {
    return w && static_cast<bool>(*to_widget(w)) ? 1 : 0;
}

int affineui_widget_get_kind(const affineui_widget* w) {
    if (!w) return AFFINEUI_WIDGET_NONE;
    const affineui::WidgetNode* node = to_widget(w)->node();
    if (!node) return AFFINEUI_WIDGET_NONE;
    return static_cast<int>(node->kind);
}

char* affineui_widget_name(const affineui_widget* w) {
    return dup_string(w ? to_widget(w)->name() : std::string_view{});
}

char* affineui_widget_attr(const affineui_widget* w, const char* name,
                           const char* fallback) {
    if (!w || !name) return dup_string(sv(fallback));
    return dup_string(to_widget(w)->attr_value(std::string_view{name}, sv(fallback)));
}

char* affineui_widget_text(const affineui_widget* w) {
    return dup_string(w ? to_widget(w)->text_value() : std::string_view{});
}

int affineui_widget_has_attr(const affineui_widget* w, const char* name) {
    if (!w || !name) return 0;
    return to_widget(w)->has_attr(std::string_view{name}) ? 1 : 0;
}

void affineui_widget_set_text(affineui_widget* w, const char* text) {
    if (w) to_widget(w)->text(sv(text));
}

void affineui_widget_set_attr(affineui_widget* w, const char* name, const char* value) {
    if (w && name) to_widget(w)->attr(std::string_view{name}, sv(value));
}

void affineui_widget_remove_attr(affineui_widget* w, const char* name) {
    if (w && name) to_widget(w)->remove_attr(std::string_view{name});
}

void affineui_widget_set_selector(affineui_widget* w, const char* name,
                                  const char* value) {
    if (w && name) to_widget(w)->selector(std::string_view{name}, sv(value));
}

void affineui_widget_add_class(affineui_widget* w, const char* classes) {
    if (w) to_widget(w)->cls(sv(classes));
}

void affineui_widget_clear(affineui_widget* w) {
    if (w) to_widget(w)->clear();
}

void affineui_widget_on_click(affineui_widget* w, affineui_click_fn fn,
                              void* user, affineui_user_free_fn user_free) {
    if (!w || !fn) {
        // Uphold exactly-once user_free even for rejected registrations.
        if (user_free) user_free(user);
        return;
    }
    auto data = hold_user(user, user_free);
    to_widget(w)->on_click([fn, data = std::move(data)] { fn(data->user); });
}

void affineui_widget_on_change(affineui_widget* w, affineui_change_fn fn,
                               void* user, affineui_user_free_fn user_free) {
    if (!w || !fn) {
        if (user_free) user_free(user);
        return;
    }
    auto data = hold_user(user, user_free);
    to_widget(w)->on_change([fn, data = std::move(data)](std::string_view value) {
        std::string owned(value);  // guarantee null termination across the ABI
        fn(data->user, owned.c_str());
    });
}

void affineui_widget_append(affineui_widget* w, affineui_build_fn build, void* user) {
    if (w && build) to_widget(w)->append(build_fn(build, user));
}

void affineui_widget_replace(affineui_widget* w, affineui_build_fn build, void* user) {
    if (w && build) to_widget(w)->replace(build_fn(build, user));
}

affineui_widget* affineui_widget_find_widget(const affineui_widget* w, const char* name) {
    if (!w) return nullptr;
    return wrap(to_widget(w)->find_widget(sv(name)));
}

// ─── bundled Decius CSS framework ─────────────────────────────────────
// See include/affineui/decius_bundle.h for the C++ surface; this is the
// C ABI. When compiled with -DAFFINEUI_NO_BUNDLE_DECIUS, both return
// "not compiled in" values (0 / -1) so callers can degrade gracefully.

int affineui_decius_available(void) {
#ifdef AFFINEUI_NO_BUNDLE_DECIUS
    return 0;
#else
    return affineui::decius::available() ? 1 : 0;
#endif
}

int affineui_decius_apply(affineui_app* app) {
#ifdef AFFINEUI_NO_BUNDLE_DECIUS
    (void)app;
    return -1;
#else
    if (!app) return -1;
    affineui::decius::apply(*to_app(app));
    return 0;
#endif
}

}  // extern "C"

// == Virtual lists & trees ==============================================
//
// Handle structs own the C++ objects; providers are Trackable (widget-side
// weak references), so they live on the heap behind stable addresses. Every
// callback holds its UserData via shared_ptr — user_free runs exactly once
// when the provider drops (or replaces) the handler.

struct affineui_index_selection { affineui::IndexSelection sel; };
struct affineui_vlist_provider  { affineui::VirtualListProvider p; };
struct affineui_vtree_provider  { affineui::VirtualTreeProvider p; };

namespace {

affineui::SelectMod to_select_mod(int m) {
    switch (m) {
        case AFFINEUI_SELECT_TOGGLE: return affineui::SelectMod::Toggle;
        case AFFINEUI_SELECT_RANGE:  return affineui::SelectMod::Range;
        default:                     return affineui::SelectMod::Replace;
    }
}

// Setter templates shared by both providers (CRTP base has the fluent
// setters; these adapt the C (fn,user,user_free) triple).
template <typename P>
void set_item_count(P& p, affineui_item_count_fn fn, void* user,
                    affineui_user_free_fn user_free) {
    auto data = hold_user(user, user_free);
    if (!fn) { p.on_item_count({}); return; }
    p.on_item_count([fn, data] { return fn(data->user); });
}
template <typename P>
void set_item_size(P& p, affineui_item_size_fn fn, void* user,
                   affineui_user_free_fn user_free) {
    auto data = hold_user(user, user_free);
    if (!fn) { p.on_item_size({}); return; }
    p.on_item_size([fn, data](std::size_t i) { return fn(data->user, i); });
}
template <typename P>
void set_item_text(P& p, affineui_item_text_fn fn, void* user,
                   affineui_user_free_fn user_free) {
    auto data = hold_user(user, user_free);
    if (!fn) { p.on_item_text({}); return; }
    p.on_item_text([fn, data](std::size_t i) {
        const char* text = fn(data->user, i);
        return std::string(text ? text : "");
    });
}
template <typename P>
void set_build_item(P& p, affineui_item_build_fn fn, void* user,
                    affineui_user_free_fn user_free) {
    auto data = hold_user(user, user_free);
    if (!fn) { p.on_build_item({}); return; }
    p.on_build_item([fn, data](affineui::View& v, std::size_t i) {
        fn(data->user, view_handle(v), i);
    });
}
template <typename P>
void set_is_selected(P& p, affineui_item_flag_fn fn, void* user,
                     affineui_user_free_fn user_free) {
    auto data = hold_user(user, user_free);
    if (!fn) { p.on_is_selected({}); return; }
    p.on_is_selected(
        [fn, data](std::size_t i) { return fn(data->user, i) != 0; });
}
template <typename P>
void set_activate(P& p, affineui_item_activate_fn fn, void* user,
                  affineui_user_free_fn user_free) {
    auto data = hold_user(user, user_free);
    if (!fn) { p.on_activate({}); return; }
    p.on_activate([fn, data](std::size_t i, affineui::SelectMod m) {
        fn(data->user, i, static_cast<int>(m));
    });
}
template <typename P>
void set_is_checked(P& p, affineui_item_flag_fn fn, void* user,
                    affineui_user_free_fn user_free) {
    auto data = hold_user(user, user_free);
    if (!fn) { p.on_is_checked({}); return; }
    p.on_is_checked(
        [fn, data](std::size_t i) { return fn(data->user, i) != 0; });
}
template <typename P>
void set_set_checked(P& p, affineui_item_checked_fn fn, void* user,
                     affineui_user_free_fn user_free) {
    auto data = hold_user(user, user_free);
    if (!fn) { p.on_set_checked({}); return; }
    p.on_set_checked([fn, data](std::size_t i, bool on) {
        fn(data->user, i, on ? 1 : 0);
    });
}

// The C tree source: a Trackable wrapper over the walk callbacks so the
// TreeFlattener's WeakRef guard works exactly as it does for C++ data.
struct CTreeSource : affineui::Trackable {
    affineui_tree_children_fn children{nullptr};
    affineui_tree_label_fn    label{nullptr};
    affineui_tree_flag_fn     has_children{nullptr};
    std::shared_ptr<affineui_c::UserData> data;
};

}  // namespace

struct affineui_tree_flattener {
    CTreeSource source;
    affineui::TreeFlattener<CTreeSource, void, std::uint64_t> flat;
    affineui_tree_flattener() : flat(affineui::to_weak_ref(&source)) {}
};

extern "C" {

// -- IndexSelection ----------------------------------------------------

affineui_index_selection* affineui_index_selection_create(void) {
    return new affineui_index_selection{};
}
void affineui_index_selection_destroy(affineui_index_selection* sel) {
    delete sel;
}
void affineui_index_selection_apply(affineui_index_selection* sel, size_t index,
                                    int select_mod, size_t item_count) {
    if (sel) sel->sel.apply(index, to_select_mod(select_mod), item_count);
}
int affineui_index_selection_contains(const affineui_index_selection* sel,
                                      size_t index) {
    return sel && sel->sel.contains(index) ? 1 : 0;
}
void affineui_index_selection_clear(affineui_index_selection* sel) {
    if (sel) sel->sel.clear();
}
size_t affineui_index_selection_size(const affineui_index_selection* sel) {
    return sel ? sel->sel.size() : 0;
}
size_t affineui_index_selection_anchor(const affineui_index_selection* sel) {
    return sel ? sel->sel.anchor() : 0;
}
void affineui_index_selection_on_change(affineui_index_selection* sel,
                                        affineui_notify_fn fn, void* user,
                                        affineui_user_free_fn user_free) {
    if (!sel) {
        if (user_free) user_free(user);
        return;
    }
    auto data = hold_user(user, user_free);
    if (!fn) { sel->sel.on_change({}); return; }
    sel->sel.on_change([fn, data] { fn(data->user); });
}

// -- List provider ------------------------------------------------------

affineui_vlist_provider* affineui_vlist_provider_create(void) {
    return new affineui_vlist_provider{};
}
void affineui_vlist_provider_destroy(affineui_vlist_provider* p) { delete p; }

#define AFFINEUI_GUARD_PROVIDER(p)          \
    if (!(p)) {                             \
        if (user_free) user_free(user);     \
        return;                             \
    }

void affineui_vlist_provider_on_item_count(affineui_vlist_provider* p,
    affineui_item_count_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_item_count(p->p, fn, user, user_free);
}
void affineui_vlist_provider_on_item_size(affineui_vlist_provider* p,
    affineui_item_size_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_item_size(p->p, fn, user, user_free);
}
void affineui_vlist_provider_on_item_text(affineui_vlist_provider* p,
    affineui_item_text_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_item_text(p->p, fn, user, user_free);
}
void affineui_vlist_provider_on_build_item(affineui_vlist_provider* p,
    affineui_item_build_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_build_item(p->p, fn, user, user_free);
}
void affineui_vlist_provider_on_is_selected(affineui_vlist_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_is_selected(p->p, fn, user, user_free);
}
void affineui_vlist_provider_on_activate(affineui_vlist_provider* p,
    affineui_item_activate_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_activate(p->p, fn, user, user_free);
}
void affineui_vlist_provider_on_is_checked(affineui_vlist_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_is_checked(p->p, fn, user, user_free);
}
void affineui_vlist_provider_on_set_checked(affineui_vlist_provider* p,
    affineui_item_checked_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_set_checked(p->p, fn, user, user_free);
}
void affineui_vlist_provider_set_checkboxes(affineui_vlist_provider* p, int on) {
    if (p) p->p.checkboxes(on != 0);
}
void affineui_vlist_provider_set_default_item_size(affineui_vlist_provider* p,
                                                   double px) {
    if (p) p->p.default_item_size(px);
}

// -- Tree provider --------------------------------------------------------

affineui_vtree_provider* affineui_vtree_provider_create(void) {
    return new affineui_vtree_provider{};
}
void affineui_vtree_provider_destroy(affineui_vtree_provider* p) { delete p; }

void affineui_vtree_provider_on_item_count(affineui_vtree_provider* p,
    affineui_item_count_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_item_count(p->p, fn, user, user_free);
}
void affineui_vtree_provider_on_item_size(affineui_vtree_provider* p,
    affineui_item_size_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_item_size(p->p, fn, user, user_free);
}
void affineui_vtree_provider_on_item_text(affineui_vtree_provider* p,
    affineui_item_text_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_item_text(p->p, fn, user, user_free);
}
void affineui_vtree_provider_on_build_item(affineui_vtree_provider* p,
    affineui_item_build_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_build_item(p->p, fn, user, user_free);
}
void affineui_vtree_provider_on_is_selected(affineui_vtree_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_is_selected(p->p, fn, user, user_free);
}
void affineui_vtree_provider_on_activate(affineui_vtree_provider* p,
    affineui_item_activate_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_activate(p->p, fn, user, user_free);
}
void affineui_vtree_provider_on_is_checked(affineui_vtree_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_is_checked(p->p, fn, user, user_free);
}
void affineui_vtree_provider_on_set_checked(affineui_vtree_provider* p,
    affineui_item_checked_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p) set_set_checked(p->p, fn, user, user_free);
}
void affineui_vtree_provider_on_depth(affineui_vtree_provider* p,
    affineui_item_depth_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p)
    auto data = hold_user(user, user_free);
    if (!fn) { p->p.on_depth({}); return; }
    p->p.on_depth([fn, data](std::size_t i) { return fn(data->user, i); });
}
void affineui_vtree_provider_on_is_expandable(affineui_vtree_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p)
    auto data = hold_user(user, user_free);
    if (!fn) { p->p.on_is_expandable({}); return; }
    p->p.on_is_expandable(
        [fn, data](std::size_t i) { return fn(data->user, i) != 0; });
}
void affineui_vtree_provider_on_is_expanded(affineui_vtree_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p)
    auto data = hold_user(user, user_free);
    if (!fn) { p->p.on_is_expanded({}); return; }
    p->p.on_is_expanded(
        [fn, data](std::size_t i) { return fn(data->user, i) != 0; });
}
void affineui_vtree_provider_on_toggle(affineui_vtree_provider* p,
    affineui_item_toggle_fn fn, void* user, affineui_user_free_fn user_free) {
    AFFINEUI_GUARD_PROVIDER(p)
    auto data = hold_user(user, user_free);
    if (!fn) { p->p.on_toggle({}); return; }
    p->p.on_toggle([fn, data](std::size_t i) { fn(data->user, i); });
}
void affineui_vtree_provider_set_checkboxes(affineui_vtree_provider* p, int on) {
    if (p) p->p.checkboxes(on != 0);
}
void affineui_vtree_provider_set_default_item_size(affineui_vtree_provider* p,
                                                   double px) {
    if (p) p->p.default_item_size(px);
}

#undef AFFINEUI_GUARD_PROVIDER

// -- Tree flattener --------------------------------------------------------

affineui_tree_flattener* affineui_tree_flattener_create(
    affineui_tree_children_fn children,
    affineui_tree_label_fn label,
    affineui_tree_flag_fn has_children,
    void* user,
    affineui_user_free_fn user_free) {
    auto* f = new affineui_tree_flattener{};
    f->source.children     = children;
    f->source.label        = label;
    f->source.has_children = has_children;
    f->source.data         = hold_user(user, user_free);
    f->flat
        .on_roots([](CTreeSource* s, std::vector<std::uint64_t>& out) {
            if (!s->children) return;
            s->children(
                s->data->user, 0,
                [](void* ctx, std::uint64_t child) {
                    static_cast<std::vector<std::uint64_t>*>(ctx)->push_back(
                        child);
                },
                &out);
        })
        .on_children([](CTreeSource* s, std::uint64_t parent,
                        std::vector<std::uint64_t>& out) {
            if (!s->children) return;
            s->children(
                s->data->user, parent,
                [](void* ctx, std::uint64_t child) {
                    static_cast<std::vector<std::uint64_t>*>(ctx)->push_back(
                        child);
                },
                &out);
        })
        .on_label([](CTreeSource* s, std::uint64_t handle) {
            const char* text =
                s->label ? s->label(s->data->user, handle) : nullptr;
            return std::string(text ? text : "");
        })
        .on_has_children([](CTreeSource* s, std::uint64_t handle) {
            return s->has_children &&
                   s->has_children(s->data->user, handle) != 0;
        });
    return f;
}
void affineui_tree_flattener_destroy(affineui_tree_flattener* f) { delete f; }
void affineui_tree_flattener_wire(affineui_tree_flattener* f,
                                  affineui_vtree_provider* p) {
    if (f && p) f->flat.wire(p->p);
}
void affineui_tree_flattener_rebuild(affineui_tree_flattener* f) {
    if (f) f->flat.rebuild();
}
void affineui_tree_flattener_on_changed(affineui_tree_flattener* f,
                                        affineui_notify_fn fn, void* user,
                                        affineui_user_free_fn user_free) {
    if (!f) {
        if (user_free) user_free(user);
        return;
    }
    auto data = hold_user(user, user_free);
    if (!fn) { f->flat.on_changed({}); return; }
    f->flat.on_changed([fn, data] { fn(data->user); });
}
void affineui_tree_flattener_set_expanded(affineui_tree_flattener* f,
                                          uint64_t handle, int open) {
    if (f) f->flat.set_expanded(handle, open != 0);
}
int affineui_tree_flattener_is_expanded(const affineui_tree_flattener* f,
                                        uint64_t handle) {
    return f && f->flat.is_expanded(handle) ? 1 : 0;
}
void affineui_tree_flattener_set_selected(affineui_tree_flattener* f,
                                          uint64_t handle, int on) {
    if (f) f->flat.set_selected(handle, on != 0);
}
int affineui_tree_flattener_selected_contains(const affineui_tree_flattener* f,
                                              uint64_t handle) {
    return f && f->flat.selected_contains(handle) ? 1 : 0;
}
void affineui_tree_flattener_clear_selection(affineui_tree_flattener* f) {
    if (f) f->flat.clear_selection();
}
void affineui_tree_flattener_set_checked(affineui_tree_flattener* f,
                                         uint64_t handle, int on) {
    if (f) f->flat.set_checked(handle, on != 0);
}
int affineui_tree_flattener_checked_contains(const affineui_tree_flattener* f,
                                             uint64_t handle) {
    return f && f->flat.checked_contains(handle) ? 1 : 0;
}
size_t affineui_tree_flattener_size(const affineui_tree_flattener* f) {
    return f ? f->flat.size() : 0;
}
uint64_t affineui_tree_flattener_handle_at(const affineui_tree_flattener* f,
                                           size_t index) {
    return f ? f->flat.handle_at(index) : 0;
}
size_t affineui_tree_flattener_index_of(const affineui_tree_flattener* f,
                                        uint64_t handle) {
    if (!f) return static_cast<size_t>(-1);
    return f->flat.index_of(handle);
}

// -- View builders -----------------------------------------------------

affineui_widget* affineui_view_virtual_list(affineui_view* view,
                                            const char* key,
                                            affineui_vlist_provider* provider,
                                            int axis,
                                            const char* classes) {
    if (!view || !provider) return wrap(affineui::WidgetRef{});
    return wrap(to_view(view)->virtual_list(
        sv(key), provider->p,
        axis == AFFINEUI_AXIS_HORIZONTAL ? affineui::Axis::Horizontal
                                         : affineui::Axis::Vertical,
        sv(classes)));
}
affineui_widget* affineui_view_virtual_tree(affineui_view* view,
                                            const char* key,
                                            affineui_vtree_provider* provider,
                                            const char* classes) {
    if (!view || !provider) return wrap(affineui::WidgetRef{});
    return wrap(to_view(view)->virtual_tree(sv(key), provider->p, sv(classes)));
}
affineui_widget* affineui_view_virtual_string_list(
    affineui_view* view, const char* key, const char* const* items,
    size_t item_count, double item_size, affineui_index_selection* selection,
    affineui_index_selection* checked, const char* classes) {
    if (!view) return wrap(affineui::WidgetRef{});
    const auto strings = to_strings(items, item_count);
    affineui::View::StringListOptions options;
    if (item_size > 0.0) options.item_size = item_size;
    options.selection = selection ? &selection->sel : nullptr;
    options.checked   = checked ? &checked->sel : nullptr;
    options.classes   = sv(classes);
    return wrap(to_view(view)->virtual_list(sv(key), strings, options));
}

// -- Retained-view rebuild loop -----------------------------------------

void affineui_app_set_view(affineui_app* app, affineui_build_fn build,
                           void* user, affineui_user_free_fn user_free) {
    if (!app || !build) {
        if (user_free) user_free(user);
        return;
    }
    auto data = hold_user(user, user_free);
    to_app(app)->set_view([build, data](affineui::View& v) {
        build(data->user, view_handle(v));
    });
}
void affineui_app_rebuild_view(affineui_app* app) {
    if (app) to_app(app)->rebuild_view();
}

}  // extern "C"
