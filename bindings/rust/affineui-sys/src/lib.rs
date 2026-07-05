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

pub const AFFINEUI_C_ABI_VERSION: c_int = 1;

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
pub type affineui_click_fn = Option<unsafe extern "C" fn(user: *mut c_void)>;
pub type affineui_change_fn = Option<unsafe extern "C" fn(user: *mut c_void, value: *const c_char)>;
pub type affineui_build_fn = Option<unsafe extern "C" fn(user: *mut c_void, view: *mut affineui_view)>;

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
    pub fn affineui_ui_on_click(
        ui: *mut affineui_ui,
        selector: *const c_char,
        fn_: affineui_ui_click_fn,
        user: *mut c_void,
        user_free: affineui_user_free_fn,
    );
    pub fn affineui_ui_hovered_cursor(ui: *const affineui_ui) -> c_int;
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
}
