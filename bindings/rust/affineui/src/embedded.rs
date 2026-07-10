//! Embedded mode: the host engine owns the GPU device, window, main
//! loop, and input pump; AffineUI renders into render-target views the
//! host supplies each frame and never presents.
//!
//! `init` and `render` are `unsafe`: the caller vouches that the raw
//! device/view pointers are live and match the backend AffineUI was
//! compiled for. Everything else is safe. See `docs/EMBEDDING.md` for
//! the ownership and state-clobber contract.
//!
//! ```no_run
//! use affineui::embedded::{FrameTarget, GpuContext, Ui};
//! # let (device, context, rtv, dsv) = (std::ptr::null(), std::ptr::null(), std::ptr::null(), std::ptr::null());
//! let ui = Ui::new();
//! unsafe { ui.init(Some(&GpuContext::d3d11(device, context)), None) };
//! ui.set_html("<button id='save'>Save</button>");
//! ui.on_click("#save", || println!("save"));
//! // per host frame:
//! if ui.needs_update() { /* host may skip idle frames */ }
//! unsafe { ui.render(&FrameTarget::d3d11(rtv, dsv, 1920, 1080, 1.0)) };
//! ```

use crate::event::{Color, Event};
use crate::sys;
use crate::util::{cstring, ensure_abi, NotThreadSafe};
use std::os::raw::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};

/// Concrete GPU backend (must match what AffineUI was compiled for).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum Backend {
    D3d11 = 0,
    Metal = 1,
    Gl = 2,
    Wgpu = 3,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum PixelFormat {
    #[default]
    Default = 0,
    Rgba8 = 1,
    Bgra8 = 2,
    Depth = 3,
    DepthStencil = 4,
}

/// Host graphics objects, supplied once at init (all-or-nothing
/// ownership: fill the fields for your backend).
#[derive(Clone, Copy, Debug)]
pub struct GpuContext {
    pub backend: Backend,
    pub d3d11_device: *const c_void,
    pub d3d11_device_context: *const c_void,
    pub metal_device: *const c_void,
    pub wgpu_device: *const c_void,
    pub color_format: PixelFormat,
    pub depth_format: PixelFormat,
    pub sample_count: i32,
}

impl GpuContext {
    fn empty(backend: Backend) -> GpuContext {
        GpuContext {
            backend,
            d3d11_device: std::ptr::null(),
            d3d11_device_context: std::ptr::null(),
            metal_device: std::ptr::null(),
            wgpu_device: std::ptr::null(),
            color_format: PixelFormat::Default,
            depth_format: PixelFormat::Default,
            sample_count: 1,
        }
    }

    /// `device` = `ID3D11Device*`, `context` = `ID3D11DeviceContext*`.
    pub fn d3d11(device: *const c_void, context: *const c_void) -> GpuContext {
        GpuContext { d3d11_device: device, d3d11_device_context: context, ..GpuContext::empty(Backend::D3d11) }
    }

    /// `device` = `id<MTLDevice>`.
    pub fn metal(device: *const c_void) -> GpuContext {
        GpuContext { metal_device: device, ..GpuContext::empty(Backend::Metal) }
    }

    pub fn gl() -> GpuContext {
        GpuContext::empty(Backend::Gl)
    }

    /// `device` = `WGPUDevice`.
    pub fn wgpu(device: *const c_void) -> GpuContext {
        GpuContext { wgpu_device: device, ..GpuContext::empty(Backend::Wgpu) }
    }

    pub fn color_format(mut self, f: PixelFormat) -> GpuContext {
        self.color_format = f;
        self
    }
    pub fn depth_format(mut self, f: PixelFormat) -> GpuContext {
        self.depth_format = f;
        self
    }
    pub fn sample_count(mut self, n: i32) -> GpuContext {
        self.sample_count = n;
        self
    }

    fn to_sys(self) -> sys::affineui_gpu_context {
        sys::affineui_gpu_context {
            backend: self.backend as i32,
            d3d11_device: self.d3d11_device,
            d3d11_device_context: self.d3d11_device_context,
            metal_device: self.metal_device,
            wgpu_device: self.wgpu_device,
            color_format: self.color_format as i32,
            depth_format: self.depth_format as i32,
            sample_count: self.sample_count,
        }
    }
}

/// Per-frame render-target views. AffineUI opens a pass into these,
/// draws, ends + commits; the host presents.
#[derive(Clone, Copy, Debug)]
pub struct FrameTarget {
    pub width: i32,
    pub height: i32,
    pub dpi_scale: f32,
    pub sample_count: i32,
    pub clear: bool,
    /// Optional sub-rect of the target in pixels.
    pub viewport: Option<(i32, i32, i32, i32)>,
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

impl FrameTarget {
    fn empty(width: i32, height: i32, dpi_scale: f32) -> FrameTarget {
        FrameTarget {
            width,
            height,
            dpi_scale,
            sample_count: 1,
            clear: true,
            viewport: None,
            d3d11_render_view: std::ptr::null(),
            d3d11_resolve_view: std::ptr::null(),
            d3d11_depth_stencil_view: std::ptr::null(),
            metal_current_drawable: std::ptr::null(),
            metal_depth_stencil_texture: std::ptr::null(),
            metal_msaa_color_texture: std::ptr::null(),
            wgpu_render_view: std::ptr::null(),
            wgpu_resolve_view: std::ptr::null(),
            wgpu_depth_stencil_view: std::ptr::null(),
            gl_framebuffer: 0,
        }
    }

    /// `rtv` = `ID3D11RenderTargetView*`, `dsv` =
    /// `ID3D11DepthStencilView*` (may be null).
    pub fn d3d11(
        rtv: *const c_void,
        dsv: *const c_void,
        width: i32,
        height: i32,
        dpi_scale: f32,
    ) -> FrameTarget {
        FrameTarget {
            d3d11_render_view: rtv,
            d3d11_depth_stencil_view: dsv,
            ..FrameTarget::empty(width, height, dpi_scale)
        }
    }

    /// `drawable` = `CAMetalDrawable`.
    pub fn metal(drawable: *const c_void, width: i32, height: i32, dpi_scale: f32) -> FrameTarget {
        FrameTarget { metal_current_drawable: drawable, ..FrameTarget::empty(width, height, dpi_scale) }
    }

    /// `framebuffer` = GL FBO name (0 = default framebuffer).
    pub fn gl(framebuffer: u32, width: i32, height: i32, dpi_scale: f32) -> FrameTarget {
        FrameTarget { gl_framebuffer: framebuffer, ..FrameTarget::empty(width, height, dpi_scale) }
    }

    /// `view` = `WGPUTextureView`.
    pub fn wgpu(view: *const c_void, width: i32, height: i32, dpi_scale: f32) -> FrameTarget {
        FrameTarget { wgpu_render_view: view, ..FrameTarget::empty(width, height, dpi_scale) }
    }

    pub fn viewport(mut self, x: i32, y: i32, w: i32, h: i32) -> FrameTarget {
        self.viewport = Some((x, y, w, h));
        self
    }
    pub fn sample_count(mut self, n: i32) -> FrameTarget {
        self.sample_count = n;
        self
    }
    pub fn clear(mut self, on: bool) -> FrameTarget {
        self.clear = on;
        self
    }

    fn to_sys(self) -> sys::affineui_frame_target {
        let (vx, vy, vw, vh) = self.viewport.unwrap_or((0, 0, 0, 0));
        sys::affineui_frame_target {
            width: self.width,
            height: self.height,
            dpi_scale: self.dpi_scale,
            sample_count: self.sample_count,
            clear: self.clear as i32,
            viewport_x: vx,
            viewport_y: vy,
            viewport_w: vw,
            viewport_h: vh,
            d3d11_render_view: self.d3d11_render_view,
            d3d11_resolve_view: self.d3d11_resolve_view,
            d3d11_depth_stencil_view: self.d3d11_depth_stencil_view,
            metal_current_drawable: self.metal_current_drawable,
            metal_depth_stencil_texture: self.metal_depth_stencil_texture,
            metal_msaa_color_texture: self.metal_msaa_color_texture,
            wgpu_render_view: self.wgpu_render_view,
            wgpu_resolve_view: self.wgpu_resolve_view,
            wgpu_depth_stencil_view: self.wgpu_depth_stencil_view,
            gl_framebuffer: self.gl_framebuffer,
        }
    }
}

unsafe extern "C" fn ui_click_trampoline(user: *mut c_void) {
    let cb = &mut *(user as *mut Box<dyn FnMut()>);
    if catch_unwind(AssertUnwindSafe(&mut *cb)).is_err() {
        eprintln!("affineui: panic in on_click callback (suppressed)");
    }
}

unsafe extern "C" fn ui_click_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut()>));
}

/// An embedded AffineUI instance rendering against host graphics
/// objects.
pub struct Ui {
    raw: *mut sys::affineui_ui,
    _not_send: NotThreadSafe,
}

impl Drop for Ui {
    fn drop(&mut self) {
        unsafe { sys::affineui_ui_destroy(self.raw) };
    }
}

impl Ui {
    pub fn new() -> Ui {
        ensure_abi();
        Ui { raw: unsafe { sys::affineui_ui_create() }, _not_send: NotThreadSafe::default() }
    }

    /// Initialize against host graphics objects. Call once before the
    /// first [`Ui::render`]. `gpu: None` applies only the non-GPU
    /// settings (fonts) and leaves GPU init to the lazy windowed path.
    ///
    /// # Safety
    /// The raw device pointers in `gpu` must be live and must match the
    /// backend AffineUI was compiled for.
    pub unsafe fn init(&self, gpu: Option<&GpuContext>, font: Option<(&str, i32)>) {
        let gpu_sys = gpu.map(|g| g.to_sys());
        let font_name = font.map(|(family, _)| cstring(family));
        let desc = sys::affineui_init_desc {
            gpu: gpu_sys.as_ref().map_or(std::ptr::null(), |g| g as *const _),
            allocator: std::ptr::null(),
            log: None,
            log_user: std::ptr::null_mut(),
            default_font_family: font_name.as_ref().map_or(std::ptr::null(), |f| f.as_ptr()),
            default_font_size: font.map_or(0, |(_, size)| size),
        };
        sys::affineui_ui_init(self.raw, &desc);
    }

    /// Replace the document HTML.
    pub fn set_html(&self, html: &str) {
        let html = cstring(html);
        unsafe { sys::affineui_ui_set_html(self.raw, html.as_ptr()) };
    }

    /// Set the user stylesheet.
    pub fn set_css(&self, css: &str) {
        let css = cstring(css);
        unsafe { sys::affineui_ui_set_css(self.raw, css.as_ptr()) };
    }

    /// Load HTML from a file (linked local resources resolve relative to
    /// its directory).
    pub fn load_file(&self, path: &str) -> bool {
        let path = cstring(path);
        unsafe { sys::affineui_ui_load_file(self.raw, path.as_ptr()) != 0 }
    }

    pub fn set_clear_color(&self, c: Color) {
        unsafe { sys::affineui_ui_set_clear_color(self.raw, c.r, c.g, c.b, c.a) };
    }

    /// Render one frame into host render-target views. AffineUI opens
    /// its own pass, draws, ends + commits; the host presents.
    ///
    /// # Safety
    /// The raw view pointers in `target` must be live, sized as
    /// declared, and belong to the device passed at init.
    pub unsafe fn render(&self, target: &FrameTarget) {
        let t = target.to_sys();
        sys::affineui_ui_render(self.raw, &t);
    }

    /// Forward a translated host event. Returns true when the UI
    /// consumed it (the host should suppress its own handling).
    pub fn dispatch(&self, ev: &Event) -> bool {
        ev.with_sys(|sys_ev| unsafe { sys::affineui_ui_dispatch(self.raw, sys_ev) != 0 })
    }

    /// Click handler for elements matching a minimal CSS selector
    /// (`"#id"`, `".cls"`, `"tag"`, `"a,b"`).
    pub fn on_click(&self, selector: &str, f: impl FnMut() + 'static) {
        let selector = cstring(selector);
        let boxed: Box<dyn FnMut()> = Box::new(f);
        let user = Box::into_raw(Box::new(boxed)) as *mut c_void;
        unsafe {
            sys::affineui_ui_on_click(
                self.raw,
                selector.as_ptr(),
                Some(ui_click_trampoline),
                user,
                Some(ui_click_free),
            );
        }
    }

    /// Cursor the OS should display (same mapping as
    /// [`crate::Document::hovered_cursor`]).
    pub fn hovered_cursor(&self) -> i32 {
        unsafe { sys::affineui_ui_hovered_cursor(self.raw) }
    }

    /// True while an editable text control is focused — the host should
    /// enable platform text input / IME while this holds and disable it
    /// when it clears (see docs/IME_ARCHITECTURE.md).
    pub fn text_input_active(&self) -> bool {
        unsafe { sys::affineui_ui_text_input_active(self.raw) != 0 }
    }

    /// Caret rectangle `(x, y, w, h)` of the focused text control in
    /// panel-local CSS points, for IME candidate-window placement.
    /// `w == 0` when no text control is focused.
    pub fn caret_rect(&self) -> (i32, i32, i32, i32) {
        let (mut x, mut y, mut w, mut h) = (0, 0, 0, 0);
        unsafe {
            sys::affineui_ui_caret_rect(self.raw, &mut x, &mut y, &mut w, &mut h)
        };
        (x, y, w, h)
    }

    // Live DOM mutation; return true only when the document changed.

    pub fn set_attr(&self, elem_id: &str, name: &str, value: &str) -> bool {
        let (id, name, value) = (cstring(elem_id), cstring(name), cstring(value));
        unsafe { sys::affineui_ui_set_attr(self.raw, id.as_ptr(), name.as_ptr(), value.as_ptr()) != 0 }
    }

    pub fn remove_attr(&self, elem_id: &str, name: &str) -> bool {
        let (id, name) = (cstring(elem_id), cstring(name));
        unsafe { sys::affineui_ui_remove_attr(self.raw, id.as_ptr(), name.as_ptr()) != 0 }
    }

    pub fn set_text(&self, elem_id: &str, text: &str) -> bool {
        let (id, text) = (cstring(elem_id), cstring(text));
        unsafe { sys::affineui_ui_set_text(self.raw, id.as_ptr(), text.as_ptr()) != 0 }
    }

    /// Document content size after the last layout pass.
    pub fn content_size(&self) -> (i32, i32) {
        let (mut w, mut h) = (0, 0);
        unsafe { sys::affineui_ui_content_size(self.raw, &mut w, &mut h) };
        (w, h)
    }

    /// Whether the UI changed since the last render (advisory; hosts
    /// rendering on demand can skip idle frames).
    pub fn needs_update(&self) -> bool {
        unsafe { sys::affineui_ui_needs_update(self.raw) != 0 }
    }

    /// Force `needs_update()` true.
    pub fn mark_dirty(&self) {
        unsafe { sys::affineui_ui_mark_dirty(self.raw) };
    }

    /// Drop all content state (keeps the GPU binding).
    pub fn reset(&self) {
        unsafe { sys::affineui_ui_reset(self.raw) };
    }
}

impl Default for Ui {
    fn default() -> Ui {
        Ui::new()
    }
}
