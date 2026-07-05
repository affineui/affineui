#ifndef AFFINEUI_C_API_APP_H
#define AFFINEUI_C_API_APP_H

// AffineUI C ABI — app-level surface.
//
// The extern "C" counterpart of the C++ application API (app.h, view.h,
// document.h, components.h) for language bindings: Rust, C#, Zig, Odin,
// plain C. The embedded-mode surface (host-owned GPU) lives in c_api.h;
// this header covers the mode where AffineUI owns the window and loop,
// plus the headless Document and the View command tree used by both.
//
// Contracts (see docs/LANGUAGE_BINDINGS.md for the full spec):
//   - Hard to crash: every function null-checks every handle/pointer and
//     no-ops (or returns a default) instead of faulting. Operations on a
//     widget whose node is gone follow WidgetRef semantics: reads return
//     defaults, writes no-op.
//   - Strings IN are UTF-8, null-terminated; NULL means "empty".
//   - Strings OUT are heap copies; free with affineui_string_free().
//   - Callbacks are (fn, user, user_free) triples. `user_free` is called
//     EXACTLY ONCE when the core drops its last reference to the handler
//     (handler replaced, view cleared/destroyed). Callbacks must not
//     unwind across this ABI.
//   - Threading: single-threaded. Everything on one thread.
//   - Widget handles are owned by the caller (affineui_widget_destroy)
//     but must not outlive the view / app that produced them.

#include "affineui/c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Opaque handles ───────────────────────────────────────────────────
typedef struct affineui_app      affineui_app;
typedef struct affineui_document affineui_document;
typedef struct affineui_view     affineui_view;
typedef struct affineui_widget   affineui_widget;

// ── Value types ──────────────────────────────────────────────────────

typedef struct affineui_color {
    uint8_t r, g, b, a;
} affineui_color;

// Mirrors affineui::ViewTheme (values must match).
typedef enum affineui_view_theme {
    AFFINEUI_THEME_PLAIN     = 0,
    AFFINEUI_THEME_BOOTSTRAP = 1,
    AFFINEUI_THEME_DECIUS    = 2
} affineui_view_theme;

// Mirrors affineui::WidgetKind (values must match).
// AFFINEUI_WIDGET_NONE is a C-only sentinel for "no node attached".
typedef enum affineui_widget_kind {
    AFFINEUI_WIDGET_NONE         = -1,
    AFFINEUI_WIDGET_ROOT         = 0,
    AFFINEUI_WIDGET_CONTAINER    = 1,
    AFFINEUI_WIDGET_TEXT         = 2,
    AFFINEUI_WIDGET_RAW_HTML     = 3,
    AFFINEUI_WIDGET_HEADING      = 4,
    AFFINEUI_WIDGET_PANEL        = 5,
    AFFINEUI_WIDGET_BUTTON       = 6,
    AFFINEUI_WIDGET_CHECKBOX     = 7,
    AFFINEUI_WIDGET_SLIDER       = 8,
    AFFINEUI_WIDGET_KNOB         = 9,
    AFFINEUI_WIDGET_TEXT_INPUT   = 10,
    AFFINEUI_WIDGET_TEXT_AREA    = 11,
    AFFINEUI_WIDGET_DROPDOWN     = 12,
    AFFINEUI_WIDGET_BUTTON_GROUP = 13,
    AFFINEUI_WIDGET_VIRTUAL_LIST = 14,
    AFFINEUI_WIDGET_CARD         = 15
} affineui_widget_kind;

// Mirrors affineui::DispatchResult.
typedef struct affineui_dispatch_result {
    int redraw_requested;
    int invalidate_view;
    int defer_widget_changes;
    int layout_changed;
} affineui_dispatch_result;

// ── Callback shapes ──────────────────────────────────────────────────
// (affineui_user_free_fn is declared in c_api.h.)
typedef void (*affineui_click_fn) (void* user);
typedef void (*affineui_change_fn)(void* user, const char* value);
// Build callbacks run SYNCHRONOUSLY inside the registering call; `view`
// is only valid for the duration of the invocation.
typedef void (*affineui_build_fn) (void* user, affineui_view* view);

// ── App ──────────────────────────────────────────────────────────────

// POD config mirroring affineui::App::Config. Zero it, then call
// affineui_app_config_init() to fill the defaults, then override fields.
typedef struct affineui_app_config {
    const char*        title;                // default "AffineUI"
    int                width, height;        // default 1024 x 768
    affineui_color     clear_color;          // default {30,30,46,255}
    int                high_dpi;             // 0/1, default 1
    int                vsync;                // 0/1, default 1
    const char*        default_font_family;  // default "sans-serif"
    int                default_font_size;    // default 16
    const char* const* asset_folders;        // default {"."}
    size_t             asset_folder_count;
    int                perf_overlay;         // 0/1, default 0
} affineui_app_config;

AFFINEUI_C_API void affineui_app_config_init(affineui_app_config* cfg);

// `cfg` may be NULL for all-defaults. The config is copied.
AFFINEUI_C_API affineui_app* affineui_app_create(const affineui_app_config* cfg);
AFFINEUI_C_API void          affineui_app_destroy(affineui_app* app);

AFFINEUI_C_API void affineui_app_load_html(affineui_app* app, const char* html);
AFFINEUI_C_API int  affineui_app_load_html_file(affineui_app* app, const char* path);
// Copies the view (callbacks included) into the app; the app never
// borrows the caller's view handle.
AFFINEUI_C_API void affineui_app_load_view(affineui_app* app, const affineui_view* view);
// `base_url` (nullable) is the stylesheet's own location so its relative
// url()s resolve like a <link>ed sheet's.
AFFINEUI_C_API void affineui_app_set_stylesheet(affineui_app* app,
                                                const char* css,
                                                const char* base_url);
AFFINEUI_C_API void affineui_app_invalidate(affineui_app* app);
AFFINEUI_C_API void affineui_app_set_perf_overlay_enabled(affineui_app* app, int enabled);
AFFINEUI_C_API int  affineui_app_perf_overlay_enabled(const affineui_app* app);

// Returns 1 when the event was consumed (a command callback fired or the
// document requested a redraw).
AFFINEUI_C_API int  affineui_app_dispatch(affineui_app* app, const affineui_event* ev);

// Runs the native loop on the calling thread; returns the OS exit code.
AFFINEUI_C_API int  affineui_app_run(affineui_app* app);
AFFINEUI_C_API void affineui_app_quit(affineui_app* app, int code);

AFFINEUI_C_API void  affineui_app_window_size(const affineui_app* app, int* out_w, int* out_h);
AFFINEUI_C_API void  affineui_app_framebuffer_size(const affineui_app* app, int* out_w, int* out_h);
AFFINEUI_C_API float affineui_app_dpi_scale(const affineui_app* app);

// BORROWED handle, valid exactly as long as the app. Do NOT pass it to
// affineui_document_destroy.
AFFINEUI_C_API affineui_document* affineui_app_document(affineui_app* app);

// ── Document (headless-capable) ──────────────────────────────────────

AFFINEUI_C_API affineui_document* affineui_document_create(void);
// Only for documents from affineui_document_create.
AFFINEUI_C_API void affineui_document_destroy(affineui_document* doc);

AFFINEUI_C_API void affineui_document_set_html(affineui_document* doc, const char* html);
AFFINEUI_C_API void affineui_document_set_user_stylesheet(affineui_document* doc,
                                                          const char* css,
                                                          const char* base_url);
AFFINEUI_C_API void affineui_document_reload_stylesheets(affineui_document* doc);
AFFINEUI_C_API void affineui_document_layout(affineui_document* doc,
                                             int viewport_width,
                                             int viewport_height);
AFFINEUI_C_API void affineui_document_content_size(const affineui_document* doc,
                                                   int* out_w, int* out_h);

// Live DOM mutation; return 1 only when the document actually changed.
AFFINEUI_C_API int affineui_document_set_attribute_by_id(affineui_document* doc,
                                                         const char* elem_id,
                                                         const char* name,
                                                         const char* value);
AFFINEUI_C_API int affineui_document_remove_attribute_by_id(affineui_document* doc,
                                                            const char* elem_id,
                                                            const char* name);
AFFINEUI_C_API int affineui_document_set_text_by_id(affineui_document* doc,
                                                    const char* elem_id,
                                                    const char* text);

// `out` may be NULL when the caller only needs the side effects.
AFFINEUI_C_API void affineui_document_dispatch(affineui_document* doc,
                                               const affineui_event* ev,
                                               affineui_dispatch_result* out);

// Attach/detach optional behavior scripts (affineui::DocumentScript;
// 0 = UiControls).
AFFINEUI_C_API void affineui_document_attach_script(affineui_document* doc, int script);
AFFINEUI_C_API void affineui_document_detach_script(affineui_document* doc, int script);

// Cursor enum for the hovered element (same mapping as
// affineui_ui_hovered_cursor in c_api.h).
AFFINEUI_C_API int affineui_document_hovered_cursor(const affineui_document* doc);

// ── View (command tree) ──────────────────────────────────────────────
//
// Builder calls append widgets at the current insertion point. Scope
// builders take an affineui_build_fn that is invoked synchronously with
// the same view to fill the scope's children (pass NULL to leave it
// empty). Every builder returns a NEW caller-owned widget handle (free
// with affineui_widget_destroy); the handle stays safe after the node is
// gone, but must not outlive the view.

AFFINEUI_C_API affineui_view* affineui_view_create(int theme /*affineui_view_theme*/);
AFFINEUI_C_API void           affineui_view_destroy(affineui_view* view);

AFFINEUI_C_API void affineui_view_clear(affineui_view* view);
AFFINEUI_C_API void affineui_view_begin(affineui_view* view);
AFFINEUI_C_API void affineui_view_end(affineui_view* view);
AFFINEUI_C_API void affineui_view_set_theme(affineui_view* view, int theme);
AFFINEUI_C_API int  affineui_view_get_theme(const affineui_view* view);
AFFINEUI_C_API void affineui_view_set_framework_version(affineui_view* view,
                                                        const char* version);
AFFINEUI_C_API void affineui_view_selector(affineui_view* view,
                                           const char* name, const char* value);

// Caller frees with affineui_string_free.
AFFINEUI_C_API char* affineui_view_to_html_fragment(const affineui_view* view);
AFFINEUI_C_API char* affineui_view_to_html_document(const affineui_view* view);

// Text / content widgets.
AFFINEUI_C_API affineui_widget* affineui_view_heading(affineui_view* view, int level,
                                                      const char* text,
                                                      const char* classes,
                                                      const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_paragraph(affineui_view* view,
                                                        const char* text,
                                                        const char* classes,
                                                        const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_text(affineui_view* view,
                                                   const char* text,
                                                   const char* key);
// Trusted raw HTML, parsed when the view is loaded into a document.
AFFINEUI_C_API affineui_widget* affineui_view_html(affineui_view* view,
                                                   const char* markup,
                                                   const char* key);

// Form widgets.
AFFINEUI_C_API affineui_widget* affineui_view_button(affineui_view* view,
                                                     const char* label,
                                                     int primary,
                                                     const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_checkbox(affineui_view* view,
                                                       const char* label,
                                                       int checked,
                                                       const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_toggle(affineui_view* view,
                                                     const char* label,
                                                     int on,
                                                     const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_input(affineui_view* view,
                                                    const char* label,
                                                    const char* value,
                                                    const char* type, // "text" etc.
                                                    const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_password(affineui_view* view,
                                                       const char* label,
                                                       const char* value,
                                                       const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_textarea(affineui_view* view,
                                                       const char* label,
                                                       const char* value,
                                                       int rows,
                                                       const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_dropdown(affineui_view* view,
                                                       const char* label,
                                                       const char* const* options,
                                                       size_t option_count,
                                                       const char* selected,
                                                       const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_button_group(affineui_view* view,
                                                           const char* label,
                                                           const char* const* options,
                                                           size_t option_count,
                                                           const char* selected,
                                                           const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_slider(affineui_view* view,
                                                     const char* label,
                                                     double value, double min, double max,
                                                     const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_knob(affineui_view* view,
                                                   const char* label,
                                                   double value, double min, double max,
                                                   int bipolar,
                                                   const char* key);
// Bare drag-scrub numeric combo (no field/label wrapper).
AFFINEUI_C_API affineui_widget* affineui_view_combo(affineui_view* view,
                                                    const char* label,
                                                    double value, double step,
                                                    const char* key);
// Swatch-popup color field.
AFFINEUI_C_API affineui_widget* affineui_view_color_field(affineui_view* view,
                                                          const char* label,
                                                          const char* value,
                                                          const char* const* swatches,
                                                          size_t swatch_count,
                                                          const char* key);
// Decius chip + hex + picker-popover color field.
AFFINEUI_C_API affineui_widget* affineui_view_colorfield(affineui_view* view,
                                                         const char* label,
                                                         const char* value,
                                                         const char* key);

// Scope builders (build may be NULL).
AFFINEUI_C_API affineui_widget* affineui_view_container(affineui_view* view,
                                                        const char* classes,
                                                        const char* key,
                                                        affineui_build_fn build,
                                                        void* user);
AFFINEUI_C_API affineui_widget* affineui_view_element(affineui_view* view,
                                                      const char* tag,
                                                      const char* classes,
                                                      const char* key,
                                                      affineui_build_fn build,
                                                      void* user);
AFFINEUI_C_API affineui_widget* affineui_view_panel(affineui_view* view,
                                                    const char* key,
                                                    affineui_build_fn build,
                                                    void* user);
AFFINEUI_C_API affineui_widget* affineui_view_card(affineui_view* view,
                                                   const char* title,
                                                   const char* classes,
                                                   const char* key,
                                                   affineui_build_fn build,
                                                   void* user);
AFFINEUI_C_API affineui_widget* affineui_view_toolbar(affineui_view* view,
                                                      const char* key,
                                                      affineui_build_fn build,
                                                      void* user);
AFFINEUI_C_API affineui_widget* affineui_view_menu_bar(affineui_view* view,
                                                       const char* key,
                                                       affineui_build_fn build,
                                                       void* user);
AFFINEUI_C_API affineui_widget* affineui_view_status_bar(affineui_view* view,
                                                         const char* key,
                                                         affineui_build_fn build,
                                                         void* user);
AFFINEUI_C_API affineui_widget* affineui_view_tree(affineui_view* view,
                                                   const char* key,
                                                   affineui_build_fn build,
                                                   void* user);
AFFINEUI_C_API affineui_widget* affineui_view_foldout(affineui_view* view,
                                                      const char* title,
                                                      int expanded,
                                                      const char* key,
                                                      affineui_build_fn build,
                                                      void* user);

// Structural leaves.
AFFINEUI_C_API affineui_widget* affineui_view_toolbar_separator(affineui_view* view,
                                                                const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_icon_button(affineui_view* view,
                                                          const char* icon,
                                                          const char* key);
// Menubar button that OWNS its dropdown; `build` fills the menu with
// menu_item()/menu_separator()/submenu().
AFFINEUI_C_API affineui_widget* affineui_view_menu_button(affineui_view* view,
                                                          const char* label,
                                                          affineui_build_fn build,
                                                          void* user,
                                                          const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_menu_item(affineui_view* view,
                                                        const char* label,
                                                        const char* icon,
                                                        const char* shortcut,
                                                        const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_menu_separator(affineui_view* view,
                                                             const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_submenu(affineui_view* view,
                                                      const char* label,
                                                      affineui_build_fn build,
                                                      void* user,
                                                      const char* icon,
                                                      const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_menu_brand(affineui_view* view,
                                                         const char* title,
                                                         const char* icon,
                                                         const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_menu_spacer(affineui_view* view,
                                                          const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_menu_meta(affineui_view* view,
                                                        const char* text,
                                                        const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_tree_row(affineui_view* view,
                                                       const char* label,
                                                       int selected,
                                                       int depth,
                                                       const char* key);
AFFINEUI_C_API affineui_widget* affineui_view_splitter(affineui_view* view,
                                                       int horizontal,
                                                       const char* key);

// Stable lookup by user key. Always returns a handle; check
// affineui_widget_valid.
AFFINEUI_C_API affineui_widget* affineui_view_find_widget(affineui_view* view,
                                                          const char* name);

// ── Widget (safe handle over a WidgetRef) ────────────────────────────

AFFINEUI_C_API void affineui_widget_destroy(affineui_widget* w);

AFFINEUI_C_API int  affineui_widget_valid(const affineui_widget* w);
// affineui_widget_kind of the attached node; AFFINEUI_WIDGET_NONE when
// no node resolves. Language wrappers use this for typed-component
// validity checks (components.h semantics).
AFFINEUI_C_API int  affineui_widget_get_kind(const affineui_widget* w);

// Caller frees results with affineui_string_free.
AFFINEUI_C_API char* affineui_widget_name(const affineui_widget* w);
AFFINEUI_C_API char* affineui_widget_attr(const affineui_widget* w,
                                          const char* name,
                                          const char* fallback);
AFFINEUI_C_API char* affineui_widget_text(const affineui_widget* w);
AFFINEUI_C_API int   affineui_widget_has_attr(const affineui_widget* w, const char* name);

AFFINEUI_C_API void affineui_widget_set_text(affineui_widget* w, const char* text);
AFFINEUI_C_API void affineui_widget_set_attr(affineui_widget* w,
                                             const char* name, const char* value);
AFFINEUI_C_API void affineui_widget_remove_attr(affineui_widget* w, const char* name);
AFFINEUI_C_API void affineui_widget_set_selector(affineui_widget* w,
                                                 const char* name, const char* value);
AFFINEUI_C_API void affineui_widget_add_class(affineui_widget* w, const char* classes);
AFFINEUI_C_API void affineui_widget_clear(affineui_widget* w);

AFFINEUI_C_API void affineui_widget_on_click(affineui_widget* w,
                                             affineui_click_fn fn,
                                             void* user,
                                             affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_widget_on_change(affineui_widget* w,
                                              affineui_change_fn fn,
                                              void* user,
                                              affineui_user_free_fn user_free);

// Append to / replace the widget's children; build runs synchronously.
AFFINEUI_C_API void affineui_widget_append(affineui_widget* w,
                                           affineui_build_fn build, void* user);
AFFINEUI_C_API void affineui_widget_replace(affineui_widget* w,
                                            affineui_build_fn build, void* user);

// Find a descendant widget by key. Always returns a handle; check
// affineui_widget_valid.
AFFINEUI_C_API affineui_widget* affineui_widget_find_widget(const affineui_widget* w,
                                                            const char* name);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AFFINEUI_C_API_APP_H
