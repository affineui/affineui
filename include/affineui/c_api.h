#ifndef AFFINEUI_C_API_H
#define AFFINEUI_C_API_H

// AffineUI C ABI — a flat extern "C" surface mirroring the C++ embedding
// API, for engines and language bindings (Rust / Zig / Odin / C). Pass the
// host's native graphics objects as opaque void* handles; AffineUI renders
// into render-target views you supply each frame and never presents.
//
// See affineui/embed.h for the full semantics (all-or-nothing ownership,
// the D3D11/GL state-clobber contract, per-backend handle types).

#include <stddef.h>
#include <stdint.h>

// Export decoration. Defined to dllexport while building the affineui_c
// shared library on Windows; empty for static linking (the default) and on
// every other platform (the shared library exports by default there).
#ifndef AFFINEUI_C_API
#  if defined(_WIN32) && defined(AFFINEUI_C_BUILD_DLL)
#    define AFFINEUI_C_API __declspec(dllexport)
#  else
#    define AFFINEUI_C_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to an affineui::Ui instance.
typedef struct affineui_ui affineui_ui;

// Concrete GPU backend. Must match the backend AffineUI was compiled for.
// (Named to avoid colliding with the AFFINEUI_BACKEND_* compile macros.)
typedef enum affineui_backend {
    AFFINEUI_GPU_D3D11 = 0,
    AFFINEUI_GPU_METAL = 1,
    AFFINEUI_GPU_GL    = 2,
    AFFINEUI_GPU_WGPU  = 3
} affineui_backend;

typedef enum affineui_pixel_format {
    AFFINEUI_FORMAT_DEFAULT       = 0,
    AFFINEUI_FORMAT_RGBA8         = 1,
    AFFINEUI_FORMAT_BGRA8         = 2,
    AFFINEUI_FORMAT_DEPTH         = 3,
    AFFINEUI_FORMAT_DEPTH_STENCIL = 4
} affineui_pixel_format;

// Host graphics objects, supplied once at init (all-or-nothing). Fill the
// fields for your backend; leave the rest null.
typedef struct affineui_gpu_context {
    affineui_backend backend;
    const void* d3d11_device;          // ID3D11Device*
    const void* d3d11_device_context;  // ID3D11DeviceContext*
    const void* metal_device;          // id<MTLDevice>
    const void* wgpu_device;           // WGPUDevice
    int color_format;                  // affineui_pixel_format
    int depth_format;                  // affineui_pixel_format
    int sample_count;                  // MSAA sample count (>=1)
} affineui_gpu_context;

// Host allocator (optional). When set, AffineUI routes allocation through it.
typedef struct affineui_allocator {
    void* (*alloc)  (size_t size, size_t align, void* user);
    void* (*realloc)(void* p, size_t old_sz, size_t new_sz, size_t align, void* user);
    void  (*free)   (void* p, void* user);
    void*  user;
} affineui_allocator;

typedef enum affineui_log_level {
    AFFINEUI_LOG_DEBUG = 0,
    AFFINEUI_LOG_INFO  = 1,
    AFFINEUI_LOG_WARN  = 2,
    AFFINEUI_LOG_ERROR = 3
} affineui_log_level;

typedef void (*affineui_log_fn)(affineui_log_level level, const char* msg, void* user);

typedef struct affineui_init_desc {
    const affineui_gpu_context* gpu;   // non-null + complete = embedded mode
    const affineui_allocator*   allocator;  // optional host allocator
    affineui_log_fn             log;        // optional log sink
    void*                       log_user;
    const char* default_font_family;   // may be null
    int         default_font_size;     // 0 = use AffineUI default
} affineui_init_desc;

// Per-frame render-target views. AffineUI opens a pass into these, draws,
// and ends+commits; the host presents. Fill the fields for your backend.
typedef struct affineui_frame_target {
    int   width;         // pixels
    int   height;        // pixels
    float dpi_scale;     // pixels per CSS point (1.0, 2.0, ...)
    int   sample_count;  // must match the target (>=1)
    int   clear;         // non-zero = clear to clear color first

    // Optional sub-rect of the target, in pixels (all-zero = whole target).
    int   viewport_x, viewport_y, viewport_w, viewport_h;

    const void* d3d11_render_view;          // ID3D11RenderTargetView*
    const void* d3d11_resolve_view;         // ID3D11RenderTargetView* (optional)
    const void* d3d11_depth_stencil_view;   // ID3D11DepthStencilView*

    const void* metal_current_drawable;     // CAMetalDrawable
    const void* metal_depth_stencil_texture;// MTLTexture (optional)
    const void* metal_msaa_color_texture;   // MTLTexture (optional)

    const void* wgpu_render_view;           // WGPUTextureView
    const void* wgpu_resolve_view;          // WGPUTextureView (optional)
    const void* wgpu_depth_stencil_view;    // WGPUTextureView (optional)

    uint32_t    gl_framebuffer;             // GL FBO (0 = default)
} affineui_frame_target;

// ── Shared value types (used by both the embedded and app surfaces) ──

// Mirrors affineui::EventType (values must match).
typedef enum affineui_event_type {
    AFFINEUI_EVENT_NONE         = 0,
    AFFINEUI_EVENT_MOUSE_MOVE   = 1,
    AFFINEUI_EVENT_MOUSE_DOWN   = 2,
    AFFINEUI_EVENT_MOUSE_UP     = 3,
    AFFINEUI_EVENT_MOUSE_WHEEL  = 4,
    AFFINEUI_EVENT_KEY_DOWN     = 5,
    AFFINEUI_EVENT_KEY_UP       = 6,
    AFFINEUI_EVENT_TEXT_INPUT   = 7,
    AFFINEUI_EVENT_RESIZE       = 8,
    AFFINEUI_EVENT_FOCUS_LOST   = 9,
    AFFINEUI_EVENT_FOCUS_GAINED = 10,
    // IME preedit update: `text` carries the uncommitted composition
    // string (empty/null = composition ended), displayed inline at the
    // caret but never written to the control's value. Committed text
    // still arrives as AFFINEUI_EVENT_TEXT_INPUT.
    AFFINEUI_EVENT_COMPOSITION  = 11
} affineui_event_type;

// Mirrors affineui::MouseButton (values must match).
typedef enum affineui_mouse_button {
    AFFINEUI_MOUSE_LEFT   = 0,
    AFFINEUI_MOUSE_RIGHT  = 1,
    AFFINEUI_MOUSE_MIDDLE = 2
} affineui_mouse_button;

// Mirrors affineui::Key (values must match; printable text arrives as
// AFFINEUI_EVENT_TEXT_INPUT, not as key codes).
typedef enum affineui_key {
    AFFINEUI_KEY_UNKNOWN     = 0,
    AFFINEUI_KEY_ESCAPE      = 1,
    AFFINEUI_KEY_TAB         = 2,
    AFFINEUI_KEY_ENTER       = 3,
    AFFINEUI_KEY_BACKSPACE   = 4,
    AFFINEUI_KEY_DELETE      = 5,
    AFFINEUI_KEY_ARROW_LEFT  = 6,
    AFFINEUI_KEY_ARROW_RIGHT = 7,
    AFFINEUI_KEY_ARROW_UP    = 8,
    AFFINEUI_KEY_ARROW_DOWN  = 9,
    AFFINEUI_KEY_HOME        = 10,
    AFFINEUI_KEY_END         = 11,
    AFFINEUI_KEY_A = 12, AFFINEUI_KEY_B = 13, AFFINEUI_KEY_C = 14,
    AFFINEUI_KEY_D = 15, AFFINEUI_KEY_E = 16, AFFINEUI_KEY_F = 17,
    AFFINEUI_KEY_G = 18, AFFINEUI_KEY_H = 19, AFFINEUI_KEY_I = 20,
    AFFINEUI_KEY_J = 21, AFFINEUI_KEY_K = 22, AFFINEUI_KEY_L = 23,
    AFFINEUI_KEY_M = 24, AFFINEUI_KEY_N = 25, AFFINEUI_KEY_O = 26,
    AFFINEUI_KEY_P = 27, AFFINEUI_KEY_Q = 28, AFFINEUI_KEY_R = 29,
    AFFINEUI_KEY_S = 30, AFFINEUI_KEY_T = 31, AFFINEUI_KEY_U = 32,
    AFFINEUI_KEY_V = 33, AFFINEUI_KEY_W = 34, AFFINEUI_KEY_X = 35,
    AFFINEUI_KEY_Y = 36, AFFINEUI_KEY_Z = 37,
    AFFINEUI_KEY_DIGIT0 = 38, AFFINEUI_KEY_DIGIT1 = 39,
    AFFINEUI_KEY_DIGIT2 = 40, AFFINEUI_KEY_DIGIT3 = 41,
    AFFINEUI_KEY_DIGIT4 = 42, AFFINEUI_KEY_DIGIT5 = 43,
    AFFINEUI_KEY_DIGIT6 = 44, AFFINEUI_KEY_DIGIT7 = 45,
    AFFINEUI_KEY_DIGIT8 = 46, AFFINEUI_KEY_DIGIT9 = 47
} affineui_key;

// Value object mirroring affineui::Event. `text` is UTF-8, may be null,
// and is only read for AFFINEUI_EVENT_TEXT_INPUT.
typedef struct affineui_event {
    int         type;      // affineui_event_type
    int         x, y;      // pointer position (CSS/layout points)
    int         button;    // affineui_mouse_button
    float       wheel_dx;
    float       wheel_dy;
    int         key;       // affineui_key
    int         key_code;  // platform-native scancode (debug / passthrough)
    const char* text;      // UTF-8 text for TEXT_INPUT / COMPOSITION (may be null)
    int         shift, ctrl, alt, super_key;  // modifier flags (0/1)
    // COMPOSITION only — byte offsets into `text`. cursor: IME caret in
    // the preedit (negative = end); clause begin/end: the IME's active
    // segment (equal = none). Fields grow at the tail only, so binding
    // wrappers stay layout-compatible within a release.
    int         composition_cursor;
    int         composition_clause_begin;
    int         composition_clause_end;
} affineui_event;

// Frees a string returned by any affineui_* function documented as
// "caller frees". Null is a safe no-op.
typedef void (*affineui_user_free_fn)(void* user);

// ── Lifecycle ────────────────────────────────────────────────────────
AFFINEUI_C_API affineui_ui* affineui_ui_create(void);
AFFINEUI_C_API void         affineui_ui_destroy(affineui_ui* ui);

// Initialize for embedded mode against host graphics objects.
AFFINEUI_C_API void affineui_ui_init(affineui_ui* ui, const affineui_init_desc* desc);

// ── Content ──────────────────────────────────────────────────────────
AFFINEUI_C_API void affineui_ui_set_html(affineui_ui* ui, const char* html);
AFFINEUI_C_API void affineui_ui_set_css(affineui_ui* ui, const char* css);
AFFINEUI_C_API int  affineui_ui_load_file(affineui_ui* ui, const char* path);
AFFINEUI_C_API void affineui_ui_set_clear_color(affineui_ui* ui,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t a);

// ── Render (embedded) ────────────────────────────────────────────────
AFFINEUI_C_API void affineui_ui_render(affineui_ui* ui, const affineui_frame_target* target);

// ── Input (embedded — the host forwards translated events) ──────────
// Returns 1 when the UI consumed the event (the host should suppress its
// own handling of it).
AFFINEUI_C_API int  affineui_ui_dispatch(affineui_ui* ui, const affineui_event* ev);

// Click callback for elements matching a minimal CSS selector ("#id",
// ".cls", "tag", "a,b"). `user_free` (optional) is called when the handler
// is released so bindings can drop their closure state.
typedef void (*affineui_ui_click_fn)(void* user);
AFFINEUI_C_API void affineui_ui_on_click(affineui_ui* ui,
                                         const char* selector,
                                         affineui_ui_click_fn fn,
                                         void* user,
                                         affineui_user_free_fn user_free);

// Cursor the OS should display under the last hovered position
// (0=default 1=pointer 2=text 3=crosshair 4=move 5=not-allowed
//  6=ew-resize 7=ns-resize 8=nwse-resize).
AFFINEUI_C_API int  affineui_ui_hovered_cursor(const affineui_ui* ui);

// ── Text input / IME intents (see docs/IME_ARCHITECTURE.md) ─────────
// 1 while an editable text control is focused: the host should enable
// platform text input / IME while this holds and disable it when clear.
AFFINEUI_C_API int  affineui_ui_text_input_active(const affineui_ui* ui);
// Caret rectangle of the focused text control in panel-local CSS points,
// for IME candidate-window placement / soft-keyboard avoidance. Writes
// zeros when no text control is focused. Out params may be null.
AFFINEUI_C_API void affineui_ui_caret_rect(const affineui_ui* ui,
                                           int* out_x, int* out_y,
                                           int* out_w, int* out_h);

// ── Live DOM mutation (embedded) ─────────────────────────────────────
// Return 1 only when the document actually changed.
AFFINEUI_C_API int affineui_ui_set_attr(affineui_ui* ui, const char* elem_id,
                                        const char* name, const char* value);
AFFINEUI_C_API int affineui_ui_remove_attr(affineui_ui* ui, const char* elem_id,
                                           const char* name);
AFFINEUI_C_API int affineui_ui_set_text(affineui_ui* ui, const char* elem_id,
                                        const char* text);

// Document content size after the last layout pass.
AFFINEUI_C_API void affineui_ui_content_size(const affineui_ui* ui,
                                             int* out_w, int* out_h);

// ── Update scheduling / lifecycle ────────────────────────────────────
AFFINEUI_C_API int  affineui_ui_needs_update(const affineui_ui* ui);  // 1 = repaint needed
AFFINEUI_C_API void affineui_ui_mark_dirty(affineui_ui* ui);
AFFINEUI_C_API void affineui_ui_reset(affineui_ui* ui);

// ── affinetools attach (perf/devtools protocol server) ───────────────
// Loopback-only devtools server (docs/AFFINETOOLS_DESIGN.md §3;
// discovery + token auth via <tempdir>/affineui-tools/<pid>.json). Lets
// a host built against a prebuilt binary enable attach without a
// rebuild. All return 0 / no-op when the library was compiled with
// AFFINEUI_PERF=0.

// Start the server (idempotent). port 0 = ephemeral. 1 = running.
AFFINEUI_C_API int  affineui_tools_listen(int port);
// 1 while the server is running (listening or serving a client).
AFFINEUI_C_API int  affineui_tools_active(void);
// Bound port, 0 when not listening.
AFFINEUI_C_API int  affineui_tools_port(void);
// Stop the server: close sockets, join the thread, remove discovery file.
AFFINEUI_C_API void affineui_tools_shutdown(void);

// ── Misc ─────────────────────────────────────────────────────────────

// C ABI version. Bumped on ANY breaking change to this header or
// c_api_app.h (enum meaning, struct layout, function semantics).
// Additive changes (new functions, appended enum values) do not bump it.
// Language wrappers check it at load and fail fast on mismatch.
#define AFFINEUI_C_ABI_VERSION 1

AFFINEUI_C_API int         affineui_c_abi_version(void);

// Borrowed static string — do NOT free (the one exception to the
// "out-strings are heap copies" rule; affineui_string_free would crash).
AFFINEUI_C_API const char* affineui_version(void);

// Convenience: open a native window, show `html`, run to completion.
AFFINEUI_C_API int affineui_run_html(const char* html);

// Free a heap string returned by an affineui_* function documented as
// "caller frees with affineui_string_free". Null is a safe no-op.
AFFINEUI_C_API void affineui_string_free(char* s);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AFFINEUI_C_API_H
