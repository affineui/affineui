//! Raw FFI bindings to the AffineUI C ABI.
//!
//! Hand-written against `include/affineui/c_api.h` and
//! `include/affineui/c_api_app.h` (docs/LANGUAGE_BINDINGS.md §4.1 explains
//! why these are hand-written rather than bindgen-generated). Names, enum
//! values, and struct layouts mirror the headers exactly; the headers are
//! the source of truth and the C ABI version gate
//! ([`affineui_c_abi_version`]) protects against drift at load time.
//!
//! Everything here is `unsafe` and 1:1; use the `affineui` crate for the
//! safe, idiomatic surface.

#![allow(non_camel_case_types)]
#![no_std]

use core::ffi::{c_char, c_float, c_int, c_void};

pub const AFFINEUI_C_ABI_VERSION: c_int = 3;

// ── Opaque handles ───────────────────────────────────────────────────

macro_rules! opaque {
    ($name:ident) => {
        #[repr(C)]
        pub struct $name {
            _private: [u8; 0],
            _marker: core::marker::PhantomData<(*mut u8, core::marker::PhantomPinned)>,
        }
    };
}

opaque!(affineui_ui);
opaque!(affineui_app);
opaque!(affineui_document);
opaque!(affineui_view);
opaque!(affineui_widget);
opaque!(affineui_index_selection);
opaque!(affineui_vlist_provider);
opaque!(affineui_vtree_provider);
opaque!(affineui_tree_flattener);

// ── Shared enums (values locked to the C header) ─────────────────────

pub const AFFINEUI_EVENT_NONE: c_int = 0;
pub const AFFINEUI_EVENT_MOUSE_MOVE: c_int = 1;
pub const AFFINEUI_EVENT_MOUSE_DOWN: c_int = 2;
pub const AFFINEUI_EVENT_MOUSE_UP: c_int = 3;
pub const AFFINEUI_EVENT_MOUSE_WHEEL: c_int = 4;
pub const AFFINEUI_EVENT_KEY_DOWN: c_int = 5;
pub const AFFINEUI_EVENT_KEY_UP: c_int = 6;
pub const AFFINEUI_EVENT_TEXT_INPUT: c_int = 7;
pub const AFFINEUI_EVENT_RESIZE: c_int = 8;
pub const AFFINEUI_EVENT_FOCUS_LOST: c_int = 9;
pub const AFFINEUI_EVENT_FOCUS_GAINED: c_int = 10;

pub const AFFINEUI_MOUSE_LEFT: c_int = 0;
pub const AFFINEUI_MOUSE_RIGHT: c_int = 1;
pub const AFFINEUI_MOUSE_MIDDLE: c_int = 2;

// affineui_key: 0..=11 named keys, 12..=37 letters A..Z, 38..=47 digits.
pub const AFFINEUI_KEY_UNKNOWN: c_int = 0;
pub const AFFINEUI_KEY_ESCAPE: c_int = 1;
pub const AFFINEUI_KEY_TAB: c_int = 2;
pub const AFFINEUI_KEY_ENTER: c_int = 3;
pub const AFFINEUI_KEY_BACKSPACE: c_int = 4;
pub const AFFINEUI_KEY_DELETE: c_int = 5;
pub const AFFINEUI_KEY_ARROW_LEFT: c_int = 6;
pub const AFFINEUI_KEY_ARROW_RIGHT: c_int = 7;
pub const AFFINEUI_KEY_ARROW_UP: c_int = 8;
pub const AFFINEUI_KEY_ARROW_DOWN: c_int = 9;
pub const AFFINEUI_KEY_HOME: c_int = 10;
pub const AFFINEUI_KEY_END: c_int = 11;
pub const AFFINEUI_KEY_A: c_int = 12;
pub const AFFINEUI_KEY_Z: c_int = 37;
pub const AFFINEUI_KEY_DIGIT0: c_int = 38;
pub const AFFINEUI_KEY_DIGIT9: c_int = 47;

pub const AFFINEUI_THEME_PLAIN: c_int = 0;
pub const AFFINEUI_THEME_BOOTSTRAP: c_int = 1;
pub const AFFINEUI_THEME_DECIUS: c_int = 2;

pub const AFFINEUI_WIDGET_NONE: c_int = -1;
pub const AFFINEUI_WIDGET_ROOT: c_int = 0;
pub const AFFINEUI_WIDGET_CONTAINER: c_int = 1;
pub const AFFINEUI_WIDGET_TEXT: c_int = 2;
pub const AFFINEUI_WIDGET_RAW_HTML: c_int = 3;
pub const AFFINEUI_WIDGET_HEADING: c_int = 4;
pub const AFFINEUI_WIDGET_PANEL: c_int = 5;
pub const AFFINEUI_WIDGET_BUTTON: c_int = 6;
pub const AFFINEUI_WIDGET_CHECKBOX: c_int = 7;
pub const AFFINEUI_WIDGET_SLIDER: c_int = 8;
pub const AFFINEUI_WIDGET_KNOB: c_int = 9;
pub const AFFINEUI_WIDGET_TEXT_INPUT: c_int = 10;
pub const AFFINEUI_WIDGET_TEXT_AREA: c_int = 11;
pub const AFFINEUI_WIDGET_DROPDOWN: c_int = 12;
pub const AFFINEUI_WIDGET_BUTTON_GROUP: c_int = 13;
pub const AFFINEUI_WIDGET_VIRTUAL_LIST: c_int = 14;
pub const AFFINEUI_WIDGET_CARD: c_int = 15;

// GPU backend / pixel format (embedded mode).
pub const AFFINEUI_GPU_D3D11: c_int = 0;
pub const AFFINEUI_GPU_METAL: c_int = 1;
pub const AFFINEUI_GPU_GL: c_int = 2;
pub const AFFINEUI_GPU_WGPU: c_int = 3;

pub const AFFINEUI_FORMAT_DEFAULT: c_int = 0;
pub const AFFINEUI_FORMAT_RGBA8: c_int = 1;
pub const AFFINEUI_FORMAT_BGRA8: c_int = 2;
pub const AFFINEUI_FORMAT_DEPTH: c_int = 3;
pub const AFFINEUI_FORMAT_DEPTH_STENCIL: c_int = 4;

pub const AFFINEUI_LOG_DEBUG: c_int = 0;
pub const AFFINEUI_LOG_INFO: c_int = 1;
pub const AFFINEUI_LOG_WARN: c_int = 2;
pub const AFFINEUI_LOG_ERROR: c_int = 3;

// ── Shared value structs ─────────────────────────────────────────────

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct affineui_event {
    pub r#type: c_int,
    pub x: c_int,
    pub y: c_int,
    pub button: c_int,
    pub wheel_dx: c_float,
    pub wheel_dy: c_float,
    pub key: c_int,
    pub key_code: c_int,
    pub text: *const c_char,
    pub shift: c_int,
    pub ctrl: c_int,
    pub alt: c_int,
    pub super_key: c_int,
    // COMPOSITION only — byte offsets into `text` (see c_api.h).
    pub composition_cursor: c_int,
    pub composition_clause_begin: c_int,
    pub composition_clause_end: c_int,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct affineui_color {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct affineui_dispatch_result {
    pub redraw_requested: c_int,
    pub invalidate_view: c_int,
    pub defer_widget_changes: c_int,
    pub layout_changed: c_int,
    pub event_consumed: c_int,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct affineui_app_config {
    pub title: *const c_char,
    pub width: c_int,
    pub height: c_int,
    pub clear_color: affineui_color,
    pub high_dpi: c_int,
    pub vsync: c_int,
    pub default_font_family: *const c_char,
    pub default_font_size: c_int,
    pub asset_folders: *const *const c_char,
    pub asset_folder_count: usize,
    pub perf_overlay: c_int,
    pub no_bundle_decius: c_int,
}

// ── Embedded-mode structs (c_api.h) ──────────────────────────────────

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct affineui_gpu_context {
    pub backend: c_int,
    pub d3d11_device: *const c_void,
    pub d3d11_device_context: *const c_void,
    pub metal_device: *const c_void,
    pub wgpu_device: *const c_void,
    pub color_format: c_int,
    pub depth_format: c_int,
    pub sample_count: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct affineui_allocator {
    pub alloc: Option<unsafe extern "C" fn(size: usize, align: usize, user: *mut c_void) -> *mut c_void>,
    pub realloc: Option<
        unsafe extern "C" fn(p: *mut c_void, old_sz: usize, new_sz: usize, align: usize, user: *mut c_void) -> *mut c_void,
    >,
    pub free: Option<unsafe extern "C" fn(p: *mut c_void, user: *mut c_void)>,
    pub user: *mut c_void,
}

pub type affineui_log_fn = Option<unsafe extern "C" fn(level: c_int, msg: *const c_char, user: *mut c_void)>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct affineui_init_desc {
    pub gpu: *const affineui_gpu_context,
    pub allocator: *const affineui_allocator,
    pub log: affineui_log_fn,
    pub log_user: *mut c_void,
    pub default_font_family: *const c_char,
    pub default_font_size: c_int,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct affineui_frame_target {
    pub width: c_int,
    pub height: c_int,
    pub dpi_scale: c_float,
    pub sample_count: c_int,
    pub clear: c_int,
    pub viewport_x: c_int,
    pub viewport_y: c_int,
    pub viewport_w: c_int,
    pub viewport_h: c_int,
    pub d3d11_render_view: *const c_void,
    pub d3d11_resolve_view: *const c_void,
    pub d3d11_depth_stencil_view: *const c_void,
    pub metal_current_drawable: *const c_void,
    pub metal_depth_stencil_texture: *const c_void,
    pub metal_msaa_color_texture: *const c_void,
    pub wgpu_render_view: *const c_void,
    pub wgpu_resolve_view: *const c_void,
    pub wgpu_depth_stencil_view: *const c_void,
    pub gl_framebuffer: u32,
}

// ── Callback shapes ──────────────────────────────────────────────────

pub type affineui_user_free_fn = Option<unsafe extern "C" fn(user: *mut c_void)>;
pub type affineui_ui_click_fn = Option<unsafe extern "C" fn(user: *mut c_void)>;
pub type affineui_event_capture_fn = Option<
    unsafe extern "C" fn(user: *mut c_void, ev: *const affineui_event) -> c_int,
>;
pub type affineui_click_fn = Option<unsafe extern "C" fn(user: *mut c_void)>;
pub type affineui_change_fn = Option<unsafe extern "C" fn(user: *mut c_void, value: *const c_char)>;
pub type affineui_build_fn = Option<unsafe extern "C" fn(user: *mut c_void, view: *mut affineui_view)>;

// -- Declarative docking ----------------------------------------------

pub const AFFINEUI_DOCK_LEFT: c_int = 0;
pub const AFFINEUI_DOCK_RIGHT: c_int = 1;
pub const AFFINEUI_DOCK_TOP: c_int = 2;
pub const AFFINEUI_DOCK_BOTTOM: c_int = 3;
pub const AFFINEUI_DOCK_TAB: c_int = 4;

pub const AFFINEUI_DOCK_DOCKED: c_int = 0;
pub const AFFINEUI_DOCK_DETACHED: c_int = 1;
pub const AFFINEUI_DOCK_TEAROFF: c_int = 2;

pub const AFFINEUI_DOCK_CORNER_TOP_LEFT: c_int = 0;
pub const AFFINEUI_DOCK_CORNER_TOP_RIGHT: c_int = 1;
pub const AFFINEUI_DOCK_CORNER_BOTTOM_LEFT: c_int = 2;
pub const AFFINEUI_DOCK_CORNER_BOTTOM_RIGHT: c_int = 3;

/// `affineui::DockLocation` as flat C data. C++ uses `std::optional` for the
/// optional fields, which C cannot express — hence the `has_*` flags.
///
/// This is the RAW form. The safe `affineui` crate exposes an idiomatic
/// `DockLocation` with real `Option<T>`s and marshals it into this at the call;
/// nothing above `-sys` should ever see a `has_*` flag.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct affineui_dock_location {
    pub has_side: c_int,
    pub side: c_int,
    pub parent: *const c_char,
    pub state: c_int,
    pub has_size: c_int,
    pub size: c_int,
    pub has_anchor: c_int,
    pub anchor: c_int,
    pub has_offset: c_int,
    pub offset_x: c_int,
    pub offset_y: c_int,
    pub has_float_size: c_int,
    pub float_w: c_int,
    pub float_h: c_int,
    pub drag_with: *const c_char,
}

impl Default for affineui_dock_location {
    /// Same as `affineui_dock_location_init`: docked, no explicit side/size,
    /// parented to the document.
    fn default() -> Self {
        Self {
            has_side: 0,
            side: 0,
            parent: core::ptr::null(),
            state: AFFINEUI_DOCK_DOCKED,
            has_size: 0,
            size: 0,
            has_anchor: 0,
            anchor: 0,
            has_offset: 0,
            offset_x: 0,
            offset_y: 0,
            has_float_size: 0,
            float_w: 0,
            float_h: 0,
            drag_with: core::ptr::null(),
        }
    }
}

/// A saved placement override for one panel (flat `Document::DockPlacement`).
/// `present = 0` means "no override — use the declared location".
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct affineui_dock_placement {
    pub present: c_int,
    pub floating: c_int,
    pub parent: *const c_char,
    pub side: c_int,
    pub size: c_int,
    pub x: c_int,
    pub y: c_int,
    pub w: c_int,
    pub h: c_int,
}

impl Default for affineui_dock_placement {
    fn default() -> Self {
        Self {
            present: 0,
            floating: 0,
            parent: core::ptr::null(),
            side: 0,
            size: 0,
            x: 0,
            y: 0,
            w: 0,
            h: 0,
        }
    }
}

pub type affineui_dock_size_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, pane_id: *const c_char) -> c_int>;
pub type affineui_dock_active_tab_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, pane_id: *const c_char) -> *const c_char>;
pub type affineui_dock_placement_fn = Option<
    unsafe extern "C" fn(
        user: *mut c_void,
        panel_id: *const c_char,
        out: *mut affineui_dock_placement,
    ),
>;

// -- Virtual lists & trees --------------------------------------------

pub const AFFINEUI_SELECT_REPLACE: c_int = 0;
pub const AFFINEUI_SELECT_TOGGLE: c_int = 1;
pub const AFFINEUI_SELECT_RANGE: c_int = 2;

pub const AFFINEUI_AXIS_VERTICAL: c_int = 0;
pub const AFFINEUI_AXIS_HORIZONTAL: c_int = 1;

pub type affineui_notify_fn = Option<unsafe extern "C" fn(user: *mut c_void)>;
pub type affineui_item_count_fn = Option<unsafe extern "C" fn(user: *mut c_void) -> usize>;
pub type affineui_item_size_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, index: usize) -> f64>;
pub type affineui_item_text_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, index: usize) -> *const c_char>;
pub type affineui_item_flag_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, index: usize) -> c_int>;
pub type affineui_item_build_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, view: *mut affineui_view, index: usize)>;
pub type affineui_item_activate_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, index: usize, select_mod: c_int)>;
pub type affineui_item_toggle_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, index: usize)>;
pub type affineui_item_checked_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, index: usize, checked: c_int)>;
pub type affineui_item_depth_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, index: usize) -> c_int>;
pub type affineui_tree_emit_fn = Option<unsafe extern "C" fn(ctx: *mut c_void, child: u64)>;
pub type affineui_tree_children_fn = Option<
    unsafe extern "C" fn(user: *mut c_void, parent: u64, emit: affineui_tree_emit_fn, ctx: *mut c_void),
>;
pub type affineui_tree_label_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, handle: u64) -> *const c_char>;
pub type affineui_tree_flag_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, handle: u64) -> c_int>;

// ── Functions ────────────────────────────────────────────────────────

extern "C" {
    // Misc.
    pub fn affineui_c_abi_version() -> c_int;
    pub fn affineui_version() -> *const c_char;
    pub fn affineui_run_html(html: *const c_char) -> c_int;
    pub fn affineui_string_free(s: *mut c_char);

    // Embedded-mode Ui.
    pub fn affineui_ui_create() -> *mut affineui_ui;
    pub fn affineui_ui_destroy(ui: *mut affineui_ui);
    pub fn affineui_ui_init(ui: *mut affineui_ui, desc: *const affineui_init_desc);
    pub fn affineui_ui_set_html(ui: *mut affineui_ui, html: *const c_char);
    pub fn affineui_ui_set_css(ui: *mut affineui_ui, css: *const c_char);
    pub fn affineui_ui_load_file(ui: *mut affineui_ui, path: *const c_char) -> c_int;
    pub fn affineui_ui_set_clear_color(ui: *mut affineui_ui, r: u8, g: u8, b: u8, a: u8);
    pub fn affineui_ui_render(ui: *mut affineui_ui, target: *const affineui_frame_target);
    pub fn affineui_ui_dispatch(ui: *mut affineui_ui, ev: *const affineui_event) -> c_int;
    pub fn affineui_ui_on_event_capture(
        ui: *mut affineui_ui,
        fn_: affineui_event_capture_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_ui_on_click(
        ui: *mut affineui_ui,
        selector: *const c_char,
        fn_: affineui_ui_click_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_ui_hovered_cursor(ui: *const affineui_ui) -> c_int;
    pub fn affineui_ui_text_input_active(ui: *const affineui_ui) -> c_int;
    pub fn affineui_ui_caret_rect(
        ui: *const affineui_ui,
        out_x: *mut c_int,
        out_y: *mut c_int,
        out_w: *mut c_int,
        out_h: *mut c_int,
    );
    pub fn affineui_ui_set_caret_blink_interval(ui: *mut affineui_ui, milliseconds: f64);
    pub fn affineui_ui_caret_blink_interval(ui: *const affineui_ui) -> f64;
    pub fn affineui_ui_set_attr(
        ui: *mut affineui_ui,
        elem_id: *const c_char,
        name: *const c_char,
        value: *const c_char,
    ) -> c_int;
    pub fn affineui_ui_remove_attr(ui: *mut affineui_ui, elem_id: *const c_char, name: *const c_char) -> c_int;
    pub fn affineui_ui_set_text(ui: *mut affineui_ui, elem_id: *const c_char, text: *const c_char) -> c_int;
    pub fn affineui_ui_content_size(ui: *const affineui_ui, out_w: *mut c_int, out_h: *mut c_int);
    pub fn affineui_ui_needs_update(ui: *const affineui_ui) -> c_int;
    pub fn affineui_ui_mark_dirty(ui: *mut affineui_ui);
    pub fn affineui_ui_reset(ui: *mut affineui_ui);

    // App.
    pub fn affineui_app_config_init(cfg: *mut affineui_app_config);
    pub fn affineui_app_create(cfg: *const affineui_app_config) -> *mut affineui_app;
    pub fn affineui_app_destroy(app: *mut affineui_app);
    pub fn affineui_app_load_html(app: *mut affineui_app, html: *const c_char);
    pub fn affineui_app_load_html_file(app: *mut affineui_app, path: *const c_char) -> c_int;
    pub fn affineui_app_load_view(app: *mut affineui_app, view: *const affineui_view);
    pub fn affineui_app_set_stylesheet(app: *mut affineui_app, css: *const c_char, base_url: *const c_char);
    pub fn affineui_app_invalidate(app: *mut affineui_app);
    pub fn affineui_app_set_perf_overlay_enabled(app: *mut affineui_app, enabled: c_int);
    pub fn affineui_app_perf_overlay_enabled(app: *const affineui_app) -> c_int;
    pub fn affineui_app_dispatch(app: *mut affineui_app, ev: *const affineui_event) -> c_int;
    pub fn affineui_app_on_event_capture(
        app: *mut affineui_app,
        fn_: affineui_event_capture_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_app_run(app: *mut affineui_app) -> c_int;
    pub fn affineui_app_quit(app: *mut affineui_app, code: c_int);
    pub fn affineui_app_window_size(app: *const affineui_app, out_w: *mut c_int, out_h: *mut c_int);
    pub fn affineui_app_framebuffer_size(app: *const affineui_app, out_w: *mut c_int, out_h: *mut c_int);
    pub fn affineui_app_dpi_scale(app: *const affineui_app) -> c_float;
    pub fn affineui_app_document(app: *mut affineui_app) -> *mut affineui_document;

    // Document.
    pub fn affineui_document_create() -> *mut affineui_document;
    pub fn affineui_document_destroy(doc: *mut affineui_document);
    pub fn affineui_document_set_html(doc: *mut affineui_document, html: *const c_char);
    pub fn affineui_document_set_user_stylesheet(
        doc: *mut affineui_document,
        css: *const c_char,
        base_url: *const c_char,
    );
    pub fn affineui_document_reload_stylesheets(doc: *mut affineui_document);
    pub fn affineui_document_layout(doc: *mut affineui_document, viewport_width: c_int, viewport_height: c_int);
    pub fn affineui_document_content_size(doc: *const affineui_document, out_w: *mut c_int, out_h: *mut c_int);
    pub fn affineui_document_set_attribute_by_id(
        doc: *mut affineui_document,
        elem_id: *const c_char,
        name: *const c_char,
        value: *const c_char,
    ) -> c_int;
    pub fn affineui_document_remove_attribute_by_id(
        doc: *mut affineui_document,
        elem_id: *const c_char,
        name: *const c_char,
    ) -> c_int;
    pub fn affineui_document_set_text_by_id(
        doc: *mut affineui_document,
        elem_id: *const c_char,
        text: *const c_char,
    ) -> c_int;
    pub fn affineui_document_dispatch(
        doc: *mut affineui_document,
        ev: *const affineui_event,
        out: *mut affineui_dispatch_result,
    );
    pub fn affineui_document_set_caret_blink_interval(
        doc: *mut affineui_document,
        milliseconds: f64,
    );
    pub fn affineui_document_caret_blink_interval(doc: *const affineui_document) -> f64;
    pub fn affineui_document_tick_caret_blink(doc: *mut affineui_document) -> c_int;
    pub fn affineui_document_attach_script(doc: *mut affineui_document, script: c_int);
    pub fn affineui_document_detach_script(doc: *mut affineui_document, script: c_int);
    pub fn affineui_document_hovered_cursor(doc: *const affineui_document) -> c_int;

    // View.
    pub fn affineui_view_create(theme: c_int) -> *mut affineui_view;
    pub fn affineui_view_destroy(view: *mut affineui_view);
    pub fn affineui_view_clear(view: *mut affineui_view);
    pub fn affineui_view_begin(view: *mut affineui_view);
    pub fn affineui_view_end(view: *mut affineui_view);
    pub fn affineui_view_set_theme(view: *mut affineui_view, theme: c_int);
    pub fn affineui_view_get_theme(view: *const affineui_view) -> c_int;
    pub fn affineui_view_set_framework_version(view: *mut affineui_view, version: *const c_char);
    pub fn affineui_view_selector(view: *mut affineui_view, name: *const c_char, value: *const c_char);
    pub fn affineui_view_to_html_fragment(view: *const affineui_view) -> *mut c_char;
    pub fn affineui_view_to_html_document(view: *const affineui_view) -> *mut c_char;

    pub fn affineui_view_heading(
        view: *mut affineui_view,
        level: c_int,
        text: *const c_char,
        classes: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_paragraph(
        view: *mut affineui_view,
        text: *const c_char,
        classes: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_text(view: *mut affineui_view, text: *const c_char, key: *const c_char)
        -> *mut affineui_widget;
    pub fn affineui_view_html(view: *mut affineui_view, markup: *const c_char, key: *const c_char)
        -> *mut affineui_widget;
    pub fn affineui_view_button(
        view: *mut affineui_view,
        label: *const c_char,
        primary: c_int,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_checkbox(
        view: *mut affineui_view,
        label: *const c_char,
        checked: c_int,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_toggle(
        view: *mut affineui_view,
        label: *const c_char,
        on: c_int,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_input(
        view: *mut affineui_view,
        label: *const c_char,
        value: *const c_char,
        r#type: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_password(
        view: *mut affineui_view,
        label: *const c_char,
        value: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_textarea(
        view: *mut affineui_view,
        label: *const c_char,
        value: *const c_char,
        rows: c_int,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_dropdown(
        view: *mut affineui_view,
        label: *const c_char,
        options: *const *const c_char,
        option_count: usize,
        selected: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_button_group(
        view: *mut affineui_view,
        label: *const c_char,
        options: *const *const c_char,
        option_count: usize,
        selected: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_slider(
        view: *mut affineui_view,
        label: *const c_char,
        value: f64,
        min: f64,
        max: f64,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_knob(
        view: *mut affineui_view,
        label: *const c_char,
        value: f64,
        min: f64,
        max: f64,
        bipolar: c_int,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_combo(
        view: *mut affineui_view,
        label: *const c_char,
        value: f64,
        step: f64,
        key: *const c_char,
        linear: c_int,
    ) -> *mut affineui_widget;
    pub fn affineui_view_color_field(
        view: *mut affineui_view,
        label: *const c_char,
        value: *const c_char,
        swatches: *const *const c_char,
        swatch_count: usize,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_colorfield(
        view: *mut affineui_view,
        label: *const c_char,
        value: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;

    pub fn affineui_view_container(
        view: *mut affineui_view,
        classes: *const c_char,
        key: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
    ) -> *mut affineui_widget;
    pub fn affineui_view_element(
        view: *mut affineui_view,
        tag: *const c_char,
        classes: *const c_char,
        key: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
    ) -> *mut affineui_widget;
    pub fn affineui_view_panel(
        view: *mut affineui_view,
        key: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
    ) -> *mut affineui_widget;
    pub fn affineui_view_card(
        view: *mut affineui_view,
        title: *const c_char,
        classes: *const c_char,
        key: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
    ) -> *mut affineui_widget;
    pub fn affineui_view_toolbar(
        view: *mut affineui_view,
        key: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
    ) -> *mut affineui_widget;
    pub fn affineui_view_menu_bar(
        view: *mut affineui_view,
        key: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
    ) -> *mut affineui_widget;
    pub fn affineui_view_status_bar(
        view: *mut affineui_view,
        key: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
    ) -> *mut affineui_widget;
    pub fn affineui_view_tree(
        view: *mut affineui_view,
        key: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
    ) -> *mut affineui_widget;
    pub fn affineui_view_foldout(
        view: *mut affineui_view,
        title: *const c_char,
        expanded: c_int,
        key: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
    ) -> *mut affineui_widget;

    // -- Declarative docking --

    pub fn affineui_dock_location_init(loc: *mut affineui_dock_location);
    pub fn affineui_dock_location_docked(
        loc: *mut affineui_dock_location,
        side: c_int,
        size_px: c_int,
    );
    pub fn affineui_dock_location_tab(loc: *mut affineui_dock_location);
    pub fn affineui_dock_location_floating(
        loc: *mut affineui_dock_location,
        anchor: c_int,
        x: c_int,
        y: c_int,
        w: c_int,
        h: c_int,
    );
    pub fn affineui_dock_location_tearoff(
        loc: *mut affineui_dock_location,
        anchor: c_int,
        x: c_int,
        y: c_int,
        w: c_int,
        h: c_int,
    );

    pub fn affineui_view_document_view(
        view: *mut affineui_view,
        key: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
    ) -> *mut affineui_widget;
    /// DEFERRED: the engine records `content` and invokes it later, when the
    /// container emits. `user` must outlive the call — pass heap state and a
    /// `user_free`, never a stack pointer. Returns the pane id; free with
    /// `affineui_string_free`.
    pub fn affineui_view_document(
        view: *mut affineui_view,
        content: affineui_build_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
        title: *const c_char,
        icon: *const c_char,
    ) -> *mut c_char;
    /// DEFERRED — see `affineui_view_document`. Returns the panel id; free with
    /// `affineui_string_free`.
    pub fn affineui_view_dockpanel(
        view: *mut affineui_view,
        title: *const c_char,
        where_: *const affineui_dock_location,
        content: affineui_build_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
        icon: *const c_char,
        key: *const c_char,
    ) -> *mut c_char;
    /// DEFERRED — see `affineui_view_document`.
    pub fn affineui_view_dock_toolbar(
        view: *mut affineui_view,
        pane_id: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );

    pub fn affineui_view_set_dock_size_provider(
        view: *mut affineui_view,
        fn_: affineui_dock_size_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_view_set_dock_active_tab_provider(
        view: *mut affineui_view,
        fn_: affineui_dock_active_tab_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_view_set_dock_placement_provider(
        view: *mut affineui_view,
        fn_: affineui_dock_placement_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_view_set_dock_layout_from_document(
        view: *mut affineui_view,
        doc: *mut affineui_document,
    );

    /// `out.parent` is a heap copy — free with `affineui_string_free`.
    pub fn affineui_document_dock_override(
        doc: *const affineui_document,
        panel_id: *const c_char,
        out: *mut affineui_dock_placement,
    );
    /// Caller frees with `affineui_string_free`.
    pub fn affineui_document_dock_active_tab(
        doc: *const affineui_document,
        pane_id: *const c_char,
    ) -> *mut c_char;
    pub fn affineui_document_take_dock_structure_changed(doc: *mut affineui_document) -> c_int;
    pub fn affineui_document_reset_dock_state(doc: *mut affineui_document);

    pub fn affineui_document_dock_override_count(doc: *const affineui_document) -> usize;
    /// Both `*out_panel_id` and `out.parent` are heap copies — free each with
    /// `affineui_string_free`.
    pub fn affineui_document_dock_override_at(
        doc: *const affineui_document,
        index: usize,
        out_panel_id: *mut *mut c_char,
        out: *mut affineui_dock_placement,
    ) -> c_int;

    pub fn affineui_view_toolbar_separator(view: *mut affineui_view, key: *const c_char) -> *mut affineui_widget;
    pub fn affineui_view_icon_button(
        view: *mut affineui_view,
        icon: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_menu_button(
        view: *mut affineui_view,
        label: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_menu_item(
        view: *mut affineui_view,
        label: *const c_char,
        icon: *const c_char,
        shortcut: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_menu_separator(view: *mut affineui_view, key: *const c_char) -> *mut affineui_widget;
    pub fn affineui_view_submenu(
        view: *mut affineui_view,
        label: *const c_char,
        build: affineui_build_fn,
        user: *mut c_void,
        icon: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_menu_brand(
        view: *mut affineui_view,
        title: *const c_char,
        icon: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_menu_spacer(view: *mut affineui_view, key: *const c_char) -> *mut affineui_widget;
    pub fn affineui_view_menu_meta(
        view: *mut affineui_view,
        text: *const c_char,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_tree_row(
        view: *mut affineui_view,
        label: *const c_char,
        selected: c_int,
        depth: c_int,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_splitter(
        view: *mut affineui_view,
        horizontal: c_int,
        key: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_find_widget(view: *mut affineui_view, name: *const c_char) -> *mut affineui_widget;

    // Widget.
    pub fn affineui_widget_destroy(w: *mut affineui_widget);
    pub fn affineui_widget_valid(w: *const affineui_widget) -> c_int;
    pub fn affineui_widget_get_kind(w: *const affineui_widget) -> c_int;
    pub fn affineui_widget_name(w: *const affineui_widget) -> *mut c_char;
    pub fn affineui_widget_attr(
        w: *const affineui_widget,
        name: *const c_char,
        fallback: *const c_char,
    ) -> *mut c_char;
    pub fn affineui_widget_text(w: *const affineui_widget) -> *mut c_char;
    pub fn affineui_widget_has_attr(w: *const affineui_widget, name: *const c_char) -> c_int;
    pub fn affineui_widget_set_text(w: *mut affineui_widget, text: *const c_char);
    pub fn affineui_widget_set_attr(w: *mut affineui_widget, name: *const c_char, value: *const c_char);
    pub fn affineui_widget_remove_attr(w: *mut affineui_widget, name: *const c_char);
    pub fn affineui_widget_set_selector(w: *mut affineui_widget, name: *const c_char, value: *const c_char);
    pub fn affineui_widget_add_class(w: *mut affineui_widget, classes: *const c_char);
    pub fn affineui_widget_clear(w: *mut affineui_widget);
    pub fn affineui_widget_on_click(
        w: *mut affineui_widget,
        fn_: affineui_click_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_widget_on_change(
        w: *mut affineui_widget,
        fn_: affineui_change_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_widget_append(w: *mut affineui_widget, build: affineui_build_fn, user: *mut c_void);
    pub fn affineui_widget_replace(w: *mut affineui_widget, build: affineui_build_fn, user: *mut c_void);
    pub fn affineui_widget_find_widget(w: *const affineui_widget, name: *const c_char) -> *mut affineui_widget;

    // -- Virtual lists & trees ------------------------------------------

    pub fn affineui_index_selection_create() -> *mut affineui_index_selection;
    pub fn affineui_index_selection_destroy(sel: *mut affineui_index_selection);
    pub fn affineui_index_selection_apply(
        sel: *mut affineui_index_selection,
        index: usize,
        select_mod: c_int,
        item_count: usize,
    );
    pub fn affineui_index_selection_contains(sel: *const affineui_index_selection, index: usize) -> c_int;
    pub fn affineui_index_selection_clear(sel: *mut affineui_index_selection);
    pub fn affineui_index_selection_size(sel: *const affineui_index_selection) -> usize;
    pub fn affineui_index_selection_anchor(sel: *const affineui_index_selection) -> usize;
    pub fn affineui_index_selection_on_change(
        sel: *mut affineui_index_selection,
        fn_: affineui_notify_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );

    pub fn affineui_vlist_provider_create() -> *mut affineui_vlist_provider;
    pub fn affineui_vlist_provider_destroy(p: *mut affineui_vlist_provider);
    pub fn affineui_vlist_provider_on_item_count(
        p: *mut affineui_vlist_provider,
        fn_: affineui_item_count_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vlist_provider_on_item_size(
        p: *mut affineui_vlist_provider,
        fn_: affineui_item_size_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vlist_provider_on_item_text(
        p: *mut affineui_vlist_provider,
        fn_: affineui_item_text_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vlist_provider_on_build_item(
        p: *mut affineui_vlist_provider,
        fn_: affineui_item_build_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vlist_provider_on_is_selected(
        p: *mut affineui_vlist_provider,
        fn_: affineui_item_flag_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vlist_provider_on_activate(
        p: *mut affineui_vlist_provider,
        fn_: affineui_item_activate_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vlist_provider_on_is_checked(
        p: *mut affineui_vlist_provider,
        fn_: affineui_item_flag_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vlist_provider_on_set_checked(
        p: *mut affineui_vlist_provider,
        fn_: affineui_item_checked_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vlist_provider_set_checkboxes(p: *mut affineui_vlist_provider, on: c_int);
    pub fn affineui_vlist_provider_set_default_item_size(p: *mut affineui_vlist_provider, px: f64);

    pub fn affineui_vtree_provider_create() -> *mut affineui_vtree_provider;
    pub fn affineui_vtree_provider_destroy(p: *mut affineui_vtree_provider);
    pub fn affineui_vtree_provider_on_item_count(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_count_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_on_item_size(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_size_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_on_item_text(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_text_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_on_build_item(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_build_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_on_is_selected(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_flag_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_on_activate(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_activate_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_on_is_checked(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_flag_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_on_set_checked(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_checked_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_on_depth(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_depth_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_on_is_expandable(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_flag_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_on_is_expanded(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_flag_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_on_toggle(
        p: *mut affineui_vtree_provider,
        fn_: affineui_item_toggle_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_vtree_provider_set_checkboxes(p: *mut affineui_vtree_provider, on: c_int);
    pub fn affineui_vtree_provider_set_default_item_size(p: *mut affineui_vtree_provider, px: f64);

    pub fn affineui_tree_flattener_create(
        children: affineui_tree_children_fn,
        label: affineui_tree_label_fn,
        has_children: affineui_tree_flag_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    ) -> *mut affineui_tree_flattener;
    pub fn affineui_tree_flattener_destroy(f: *mut affineui_tree_flattener);
    pub fn affineui_tree_flattener_wire(f: *mut affineui_tree_flattener, p: *mut affineui_vtree_provider);
    pub fn affineui_tree_flattener_rebuild(f: *mut affineui_tree_flattener);
    pub fn affineui_tree_flattener_on_changed(
        f: *mut affineui_tree_flattener,
        fn_: affineui_notify_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_tree_flattener_set_expanded(f: *mut affineui_tree_flattener, handle: u64, open: c_int);
    pub fn affineui_tree_flattener_is_expanded(f: *const affineui_tree_flattener, handle: u64) -> c_int;
    pub fn affineui_tree_flattener_set_selected(f: *mut affineui_tree_flattener, handle: u64, on: c_int);
    pub fn affineui_tree_flattener_selected_contains(f: *const affineui_tree_flattener, handle: u64) -> c_int;
    pub fn affineui_tree_flattener_clear_selection(f: *mut affineui_tree_flattener);
    pub fn affineui_tree_flattener_set_checked(f: *mut affineui_tree_flattener, handle: u64, on: c_int);
    pub fn affineui_tree_flattener_checked_contains(f: *const affineui_tree_flattener, handle: u64) -> c_int;
    pub fn affineui_tree_flattener_size(f: *const affineui_tree_flattener) -> usize;
    pub fn affineui_tree_flattener_handle_at(f: *const affineui_tree_flattener, index: usize) -> u64;
    pub fn affineui_tree_flattener_index_of(f: *const affineui_tree_flattener, handle: u64) -> usize;

    pub fn affineui_view_virtual_list(
        view: *mut affineui_view,
        key: *const c_char,
        provider: *mut affineui_vlist_provider,
        axis: c_int,
        classes: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_virtual_tree(
        view: *mut affineui_view,
        key: *const c_char,
        provider: *mut affineui_vtree_provider,
        classes: *const c_char,
    ) -> *mut affineui_widget;
    pub fn affineui_view_virtual_string_list(
        view: *mut affineui_view,
        key: *const c_char,
        items: *const *const c_char,
        item_count: usize,
        item_size: f64,
        selection: *mut affineui_index_selection,
        checked: *mut affineui_index_selection,
        classes: *const c_char,
    ) -> *mut affineui_widget;

    pub fn affineui_app_set_view(
        app: *mut affineui_app,
        build: affineui_build_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_app_rebuild_view(app: *mut affineui_app);
}
