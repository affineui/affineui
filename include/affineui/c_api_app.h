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
    int event_consumed;
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
    // Runtime opt-out for the compile-time embedded Decius bundle.
    // 0 (default) → bundle is used (auto-applied stylesheet + fallback
    // for `frameworks/*` URLs when asset_folders come back empty).
    // 1 → embed disabled at runtime; the app runs entirely against
    // user assets. Ignored when affineui_c was built with
    // -DAFFINEUI_NO_BUNDLE_DECIUS.
    int                no_bundle_decius;     // 0/1, default 0
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

// Register an app-global capture handler before focused-widget dispatch.
AFFINEUI_C_API void affineui_app_on_event_capture(
    affineui_app* app,
    affineui_event_capture_fn fn,
    void* user,
    affineui_user_free_fn user_free);

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

// Caret visibility half-cycle in milliseconds. Zero disables blinking.
AFFINEUI_C_API void affineui_document_set_caret_blink_interval(
    affineui_document* doc, double milliseconds);
AFFINEUI_C_API double affineui_document_caret_blink_interval(
    const affineui_document* doc);
// Advance caret timing for headless/custom Document hosts. Ui/App drivers do
// this automatically. Returns 1 only when a phase transition dirtied paint.
AFFINEUI_C_API int affineui_document_tick_caret_blink(affineui_document* doc);

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
// linear != 0 → constant step/pixel scrub (rotation degrees etc.);
// 0 → magnitude-accelerated scrub (the C++ default).
AFFINEUI_C_API affineui_widget* affineui_view_combo(affineui_view* view,
                                                    const char* label,
                                                    double value, double step,
                                                    const char* key,
                                                    int linear);
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

// ── Declarative docking ──────────────────────────────────────────────
//
// Mirrors the C++ / Python surface (View::document_view / document /
// dockpanel + the four dock providers). A dock container resolves a FLAT set
// of panel declarations into a split-tree layout and emits the DOM, inserting
// splitters itself. Panels may be declared in ANY ORDER — each carries a
// location naming its parent and side — so the result is deterministic.
//
// Typical shape (error handling elided):
//
//     void build_dock(void* user, affineui_view* v) {
//         affineui_view_document(v, doc_content, user, "Scene", "cube");
//
//         affineui_dock_location where;
//         affineui_dock_location_init(&where);          // defaults
//         where.side = AFFINEUI_DOCK_LEFT;
//         where.size = 280;
//         affineui_view_dockpanel(v, "Outliner", &where,
//                                 outliner_content, user, "list", "outliner");
//     }
//     affineui_view_document_view(view, "workspace", build_dock, user);
//
// The declared layout is a SEED. Wire the providers below and a saved (or
// user-rearranged) workspace wins over it.

// Mirrors affineui::Dock (values must match).
typedef enum affineui_dock {
    AFFINEUI_DOCK_LEFT   = 0,
    AFFINEUI_DOCK_RIGHT  = 1,
    AFFINEUI_DOCK_TOP    = 2,
    AFFINEUI_DOCK_BOTTOM = 3,
    AFFINEUI_DOCK_TAB    = 4   // another tab of the parent's pane
} affineui_dock;

// Mirrors affineui::DockState (values must match).
typedef enum affineui_dock_state {
    AFFINEUI_DOCK_DOCKED   = 0,
    AFFINEUI_DOCK_DETACHED = 1,  // floating
    AFFINEUI_DOCK_TEAROFF  = 2
} affineui_dock_state;

// Mirrors affineui::DockCorner (values must match).
typedef enum affineui_dock_corner {
    AFFINEUI_DOCK_CORNER_TOP_LEFT     = 0,
    AFFINEUI_DOCK_CORNER_TOP_RIGHT    = 1,
    AFFINEUI_DOCK_CORNER_BOTTOM_LEFT  = 2,
    AFFINEUI_DOCK_CORNER_BOTTOM_RIGHT = 3
} affineui_dock_corner;

// Where a panel is (or starts) — affineui::DockLocation as plain data.
//
// A VALUE, not a handle: no behaviour, no identity, nothing to manipulate. So a
// flat POD, like affineui_event / affineui_color / affineui_app_config, rather
// than an opaque + setters (which is what affineui_vlist_provider is, because
// that one owns callbacks and has a lifetime).
//
// C++ expresses the optional fields with std::optional, which C has no
// equivalent for — hence the explicit has_* flags. Every binding language DOES
// have optionals (Rust Option<T>, C# int?), so the idiomatic wrapper carries a
// real optional-bearing struct and marshals it down to this at the boundary;
// callers never see the flags.
//
// affineui_dock_location_init() zeroes everything, which reads as "docked,
// default side/size, parented to the document". Call it before setting fields
// (or use one of the four affineui_dock_location_* factories, which mirror the
// C++/Python ones) — a garbage has_* byte would otherwise make the engine read
// an uninitialised field.
typedef struct affineui_dock_location {
    int                  has_side;
    affineui_dock        side;

    // Parent panel id — what document()/dockpanel() returned.
    // NULL/"" = the document pane.
    const char*          parent;

    affineui_dock_state  state;

    int                  has_size;
    int                  size;                // px flex-basis when docked

    int                  has_anchor;
    affineui_dock_corner anchor;              // corner a float is anchored to

    int                  has_offset;
    int                  offset_x, offset_y;  // float pos relative to anchor

    int                  has_float_size;
    int                  float_w, float_h;    // float size

    // Tearoff: id of the panel this one drags with (NULL = none).
    const char*          drag_with;
} affineui_dock_location;

// Zero `loc` to the defaults: docked, no explicit side/size, document parent.
AFFINEUI_C_API void affineui_dock_location_init(affineui_dock_location* loc);

// The four shapes, mirroring the C++/Python factories. Each fully initialises
// `loc` (no _init() needed first).
AFFINEUI_C_API void affineui_dock_location_docked(affineui_dock_location* loc,
                                                  affineui_dock side, int size_px);
AFFINEUI_C_API void affineui_dock_location_tab(affineui_dock_location* loc);
AFFINEUI_C_API void affineui_dock_location_floating(affineui_dock_location* loc,
                                                    affineui_dock_corner anchor,
                                                    int x, int y, int w, int h);
AFFINEUI_C_API void affineui_dock_location_tearoff(affineui_dock_location* loc,
                                                   affineui_dock_corner anchor,
                                                   int x, int y, int w, int h);

// Declare a dock container. Inside `build`, call affineui_view_document() for
// the center pane and affineui_view_dockpanel() for the surrounding panels.
// The engine resolves the layout and emits it when `build` returns.
AFFINEUI_C_API affineui_widget* affineui_view_document_view(affineui_view* view,
                                                            const char* key,
                                                            affineui_build_fn build,
                                                            void* user);

// ── The three DEFERRED builders ──────────────────────────────────────
//
// IMPORTANT, and different from every other builder in this header: these three
// do NOT run `content` before they return. They RECORD it, and the dock engine
// invokes it later — when document_view()'s build returns and the container
// resolves and emits the layout.
//
// That is why they take a `user_free` and the immediate builders (panel, card,
// toolbar…) do not. `user` must stay alive until the engine is done with the
// callback; it may not point at the caller's stack frame. Pass heap state and a
// `user_free`, which the view calls exactly once when it drops the callback (the
// same contract as affineui_widget_on_click). A binding that passes a stack
// pointer here — the natural thing to do, since every other builder allows it —
// gets a use-after-free the moment the layout emits.

// The center/document pane's content. Only valid inside a document_view build.
// `icon` is a Decius icon-font glyph name (NULL/"" = none). Returns the pane id
// (owned by the caller — free with affineui_string_free), usable as a parent id
// in another panel's affineui_dock_location.parent.
AFFINEUI_C_API char* affineui_view_document(affineui_view* view,
                                            affineui_build_fn content,
                                            void* user,
                                            affineui_user_free_fn user_free,
                                            const char* title,
                                            const char* icon);

// Declare a dockable panel. `where` is COPIED (reuse or discard it however you
// like after this returns); NULL means "docked, defaults, parented to the
// document". Returns the panel id (owned by the caller — free with
// affineui_string_free), usable as another panel's parent and as the `pane_id`
// the providers below are asked about.
AFFINEUI_C_API char* affineui_view_dockpanel(affineui_view* view,
                                             const char* title,
                                             const affineui_dock_location* where,
                                             affineui_build_fn content,
                                             void* user,
                                             affineui_user_free_fn user_free,
                                             const char* icon,
                                             const char* key);

// Declare the tab toolbar of the pane a document()/dockpanel() call just
// returned — the strip beside the tabs (filter buttons, a search field, a
// viewport's mode/tool controls). Call it immediately after, passing that id.
AFFINEUI_C_API void affineui_view_dock_toolbar(affineui_view* view,
                                               const char* pane_id,
                                               affineui_build_fn build,
                                               void* user,
                                               affineui_user_free_fn user_free);

// ── Dock providers (workspace persistence) ───────────────────────────
//
// The declared layout is a seed. These four let a SAVED or user-rearranged
// arrangement win over it, so drag-to-dock and tearoff survive view rebuilds.
// Each takes the (fn, user, user_free) triple: `user_free` is called exactly
// once when the view drops the callback. Pass fn = NULL to clear.

// Saved px size for a pane (return <= 0 to fall back to the declared size).
typedef int (*affineui_dock_size_fn)(void* user, const char* pane_id);

// Active tab of a dock leaf. The returned string must stay valid until the
// callback returns (it is copied immediately); NULL/"" selects the primary.
typedef const char* (*affineui_dock_active_tab_fn)(void* user, const char* pane_id);

// A saved placement override for one panel — Document::DockPlacement. Plain
// data (nine scalars, no optionals, no behaviour), so a flat POD like
// affineui_event / affineui_color rather than an opaque handle: there is
// nothing to manipulate, only to read and write. Contrast affineui_dock_location,
// which is a builder over optionals and is therefore opaque.
//
// Zeroed (present = 0) means "no override — use the declared location".
typedef struct affineui_dock_placement {
    int         present;    // 0 = no override for this panel
    int         floating;   // 1 = torn off into a floating panel
    const char* parent;     // docked: target pane id (NULL/"" = document)
    int         side;       // docked: affineui_dock
    int         size;       // docked: px flex-basis (0 = default)
    int         x, y, w, h; // floating: rect in float-host px
} affineui_dock_placement;

// Fill `out` with the panel's saved placement. Set out->present = 0 for none.
typedef void (*affineui_dock_placement_fn)(void* user, const char* panel_id,
                                           affineui_dock_placement* out);

AFFINEUI_C_API void affineui_view_set_dock_size_provider(
    affineui_view* view, affineui_dock_size_fn fn, void* user,
    affineui_user_free_fn user_free);

AFFINEUI_C_API void affineui_view_set_dock_active_tab_provider(
    affineui_view* view, affineui_dock_active_tab_fn fn, void* user,
    affineui_user_free_fn user_free);

AFFINEUI_C_API void affineui_view_set_dock_placement_provider(
    affineui_view* view, affineui_dock_placement_fn fn, void* user,
    affineui_user_free_fn user_free);

// The CURRENT arrangement is a recursive tree (Document::DockLayout), and every
// real caller wires it straight back from the document it is rebuilding — so
// rather than marshal that tree through the FFI, wire it here directly. The
// view then replays the live arrangement (splits, tab order, active tabs,
// floats) instead of the declared seed, and drag-to-dock / tearoff survive a
// rebuild. Pass doc = NULL to clear.
//
// The view holds a weak reference: if `doc` dies first the provider goes inert
// (the declared seed is used again) rather than dangling.
AFFINEUI_C_API void affineui_view_set_dock_layout_from_document(
    affineui_view* view, affineui_document* doc);

// ── Dock readback (workspace SAVE) ───────────────────────────────────
//
// The other half of persistence: read the arrangement the user dragged into
// existence so the app can serialize it. Mirrors Document::dock_overrides().
// (Document::dock_layout() is the recursive tree, which is what
// affineui_view_set_dock_layout_from_document wires directly — it is a
// round-trip token, never inspected, even in Python.)

// Number of runtime placement overrides recorded by dock gestures (tearoff,
// drag-to-dock, tab move).
AFFINEUI_C_API size_t affineui_document_dock_override_count(
    const affineui_document* doc);

// Read override `index` (< affineui_document_dock_override_count). Writes the
// panel id to `out_panel_id` and the placement to `out`. Returns 0 (writing
// nothing) if the index is out of range.
//
// BOTH `*out_panel_id` and `out->parent` are heap copies owned by the caller:
// free each with affineui_string_free. (out->parent is `const char*` for
// symmetry with the input struct; cast it to char* to free.)
AFFINEUI_C_API int affineui_document_dock_override_at(
    const affineui_document* doc, size_t index,
    char** out_panel_id, affineui_dock_placement* out);

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

// == Virtual lists & trees (recycling; see include/affineui/virtual_list.h) ==
//
// Providers are STATELESS bridges of callbacks between an app-owned model
// and the recycling widget: they store no items. Every (fn, user, user_free)
// triple follows the standard contract -- user_free runs exactly once when
// the provider drops the handler. The provider object must outlive every
// view that references it (the widget holds a weak reference and degrades
// to an empty list if it dies first -- never a crash).

// Mirrors affineui::SelectMod (values must match).
typedef enum affineui_select_mod {
    AFFINEUI_SELECT_REPLACE = 0,
    AFFINEUI_SELECT_TOGGLE  = 1,
    AFFINEUI_SELECT_RANGE   = 2
} affineui_select_mod;

// Mirrors affineui::Axis (values must match).
typedef enum affineui_axis {
    AFFINEUI_AXIS_VERTICAL   = 0,
    AFFINEUI_AXIS_HORIZONTAL = 1
} affineui_axis;

typedef struct affineui_index_selection affineui_index_selection;
typedef struct affineui_vlist_provider  affineui_vlist_provider;
typedef struct affineui_vtree_provider  affineui_vtree_provider;
typedef struct affineui_tree_flattener  affineui_tree_flattener;

typedef void        (*affineui_notify_fn)      (void* user);
typedef size_t      (*affineui_item_count_fn)  (void* user);
typedef double      (*affineui_item_size_fn)   (void* user, size_t index);
// Returned string must stay valid until the callback returns (it is copied
// immediately). Return NULL for an empty row.
typedef const char* (*affineui_item_text_fn)   (void* user, size_t index);
typedef int         (*affineui_item_flag_fn)   (void* user, size_t index);
typedef void        (*affineui_item_build_fn)  (void* user, affineui_view* view,
                                                size_t index);
typedef void        (*affineui_item_activate_fn)(void* user, size_t index,
                                                 int select_mod);
typedef void        (*affineui_item_toggle_fn) (void* user, size_t index);
typedef void        (*affineui_item_checked_fn)(void* user, size_t index,
                                                int checked);
typedef int         (*affineui_item_depth_fn)  (void* user, size_t index);

// -- IndexSelection: replace / Ctrl-toggle / Shift-range with an index
// anchor. Suits flat lists whose indices are stable identities; trees
// should key selection by HANDLE (see the tree flattener below).
AFFINEUI_C_API affineui_index_selection* affineui_index_selection_create(void);
AFFINEUI_C_API void affineui_index_selection_destroy(affineui_index_selection* sel);
AFFINEUI_C_API void affineui_index_selection_apply(affineui_index_selection* sel,
                                                   size_t index,
                                                   int select_mod,
                                                   size_t item_count);
AFFINEUI_C_API int    affineui_index_selection_contains(const affineui_index_selection* sel,
                                                        size_t index);
AFFINEUI_C_API void   affineui_index_selection_clear(affineui_index_selection* sel);
AFFINEUI_C_API size_t affineui_index_selection_size(const affineui_index_selection* sel);
AFFINEUI_C_API size_t affineui_index_selection_anchor(const affineui_index_selection* sel);
AFFINEUI_C_API void   affineui_index_selection_on_change(affineui_index_selection* sel,
                                                         affineui_notify_fn fn,
                                                         void* user,
                                                         affineui_user_free_fn user_free);

// -- List provider ----------------------------------------------------
AFFINEUI_C_API affineui_vlist_provider* affineui_vlist_provider_create(void);
AFFINEUI_C_API void affineui_vlist_provider_destroy(affineui_vlist_provider* p);
AFFINEUI_C_API void affineui_vlist_provider_on_item_count(affineui_vlist_provider* p,
    affineui_item_count_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vlist_provider_on_item_size(affineui_vlist_provider* p,
    affineui_item_size_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vlist_provider_on_item_text(affineui_vlist_provider* p,
    affineui_item_text_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vlist_provider_on_build_item(affineui_vlist_provider* p,
    affineui_item_build_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vlist_provider_on_is_selected(affineui_vlist_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vlist_provider_on_activate(affineui_vlist_provider* p,
    affineui_item_activate_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vlist_provider_on_is_checked(affineui_vlist_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vlist_provider_on_set_checked(affineui_vlist_provider* p,
    affineui_item_checked_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vlist_provider_set_checkboxes(affineui_vlist_provider* p,
                                                           int on);
AFFINEUI_C_API void affineui_vlist_provider_set_default_item_size(
    affineui_vlist_provider* p, double px);

// -- Tree provider (a virtual list over the flattened-expanded rows) --
AFFINEUI_C_API affineui_vtree_provider* affineui_vtree_provider_create(void);
AFFINEUI_C_API void affineui_vtree_provider_destroy(affineui_vtree_provider* p);
AFFINEUI_C_API void affineui_vtree_provider_on_item_count(affineui_vtree_provider* p,
    affineui_item_count_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_on_item_size(affineui_vtree_provider* p,
    affineui_item_size_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_on_item_text(affineui_vtree_provider* p,
    affineui_item_text_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_on_build_item(affineui_vtree_provider* p,
    affineui_item_build_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_on_is_selected(affineui_vtree_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_on_activate(affineui_vtree_provider* p,
    affineui_item_activate_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_on_is_checked(affineui_vtree_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_on_set_checked(affineui_vtree_provider* p,
    affineui_item_checked_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_on_depth(affineui_vtree_provider* p,
    affineui_item_depth_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_on_is_expandable(affineui_vtree_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_on_is_expanded(affineui_vtree_provider* p,
    affineui_item_flag_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_on_toggle(affineui_vtree_provider* p,
    affineui_item_toggle_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_vtree_provider_set_checkboxes(affineui_vtree_provider* p,
                                                           int on);
AFFINEUI_C_API void affineui_vtree_provider_set_default_item_size(
    affineui_vtree_provider* p, double px);

// -- Tree flattener: opaque uint64 HANDLES -> flattened rows ----------
// The app supplies the walk (children of a handle; parent 0 = roots) and
// per-node label/has-children; the flattener owns the expanded set plus
// HANDLE-keyed selection and checked state, so both survive expand/
// collapse renumbering. A handle must be UNIQUE TO THE ITEM for its
// lifetime (id, stable pointer, map key) -- never recycled onto another
// item, and never 0 (reserved for "roots"). Wire it to a tree provider
// and every tree question is answered from the flattened view.
typedef void (*affineui_tree_emit_fn)(void* ctx, uint64_t child);
// Enumerate children of `parent` (0 = roots) by calling emit(ctx, child)
// once per child, in order.
typedef void (*affineui_tree_children_fn)(void* user, uint64_t parent,
                                          affineui_tree_emit_fn emit, void* ctx);
// Returned string must stay valid until the callback returns (copied).
typedef const char* (*affineui_tree_label_fn)(void* user, uint64_t handle);
typedef int (*affineui_tree_flag_fn)(void* user, uint64_t handle);

AFFINEUI_C_API affineui_tree_flattener* affineui_tree_flattener_create(
    affineui_tree_children_fn children,
    affineui_tree_label_fn    label,
    affineui_tree_flag_fn     has_children,
    void* user,
    affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_tree_flattener_destroy(affineui_tree_flattener* f);
// Point a tree provider at this flattener (item_count/depth/expand/
// selection/checked all delegate). Call once, after creating both.
AFFINEUI_C_API void affineui_tree_flattener_wire(affineui_tree_flattener* f,
                                                 affineui_vtree_provider* p);
// Re-flatten after the underlying tree STRUCTURE changed.
AFFINEUI_C_API void affineui_tree_flattener_rebuild(affineui_tree_flattener* f);
AFFINEUI_C_API void affineui_tree_flattener_on_changed(affineui_tree_flattener* f,
    affineui_notify_fn fn, void* user, affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_tree_flattener_set_expanded(affineui_tree_flattener* f,
                                                         uint64_t handle, int open);
AFFINEUI_C_API int  affineui_tree_flattener_is_expanded(const affineui_tree_flattener* f,
                                                        uint64_t handle);
AFFINEUI_C_API void affineui_tree_flattener_set_selected(affineui_tree_flattener* f,
                                                         uint64_t handle, int on);
AFFINEUI_C_API int  affineui_tree_flattener_selected_contains(
    const affineui_tree_flattener* f, uint64_t handle);
AFFINEUI_C_API void affineui_tree_flattener_clear_selection(affineui_tree_flattener* f);
AFFINEUI_C_API void affineui_tree_flattener_set_checked(affineui_tree_flattener* f,
                                                        uint64_t handle, int on);
AFFINEUI_C_API int  affineui_tree_flattener_checked_contains(
    const affineui_tree_flattener* f, uint64_t handle);
AFFINEUI_C_API size_t   affineui_tree_flattener_size(const affineui_tree_flattener* f);
AFFINEUI_C_API uint64_t affineui_tree_flattener_handle_at(
    const affineui_tree_flattener* f, size_t index);
// SIZE_MAX when the handle is not currently visible.
AFFINEUI_C_API size_t   affineui_tree_flattener_index_of(
    const affineui_tree_flattener* f, uint64_t handle);

// -- View builders -----------------------------------------------------
// The provider is held WEAKLY by the widget; the caller keeps it alive
// (destroying it while views reference it degrades to an empty list).
AFFINEUI_C_API affineui_widget* affineui_view_virtual_list(affineui_view* view,
                                                           const char* key,
                                                           affineui_vlist_provider* provider,
                                                           int axis,
                                                           const char* classes);
AFFINEUI_C_API affineui_widget* affineui_view_virtual_tree(affineui_view* view,
                                                           const char* key,
                                                           affineui_vtree_provider* provider,
                                                           const char* classes);
// Convenience: an ALL-VIRTUAL list of strings. `selection` / `checked`
// (both optional, may be NULL) must outlive the view.
AFFINEUI_C_API affineui_widget* affineui_view_virtual_string_list(
    affineui_view* view,
    const char* key,
    const char* const* items,
    size_t item_count,
    double item_size,
    affineui_index_selection* selection,
    affineui_index_selection* checked,
    const char* classes);

// -- Retained-view rebuild loop -----------------------------------------
// Install a persistent builder: the app owns a retained View, re-runs the
// builder on every rebuild, and reconciles only the diff into the live
// document. REQUIRED for virtual lists to follow the scrollbar (the
// framework re-windows by rebuilding). Follow state changes with
// affineui_app_invalidate() (coalesced, from the frame loop) or
// affineui_app_rebuild_view() (synchronous).
AFFINEUI_C_API void affineui_app_set_view(affineui_app* app,
                                          affineui_build_fn build,
                                          void* user,
                                          affineui_user_free_fn user_free);
AFFINEUI_C_API void affineui_app_rebuild_view(affineui_app* app);

// ─── bundled Decius CSS framework ─────────────────────────────────────
// Compile-time embedded CSS bundle so the component API ships with a
// working default look and no on-disk asset copying. Present only when
// affineui_c was built without -DAFFINEUI_NO_BUNDLE_DECIUS (cmake option
// AFFINEUI_BUNDLE_DECIUS=OFF).
//
// affineui_decius_available()   — 1 if the bundle was compiled in.
// affineui_decius_apply(app)    — load the embedded CSS into `app`.
//                                 Returns 0 on success, -1 if the bundle
//                                 wasn't compiled in.
AFFINEUI_C_API int affineui_decius_available(void);
AFFINEUI_C_API int affineui_decius_apply(affineui_app* app);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AFFINEUI_C_API_APP_H
