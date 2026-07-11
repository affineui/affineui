//! App-owned mode: AffineUI creates the native window and runs the main
//! loop on the calling thread.

use crate::document::Document;
use crate::event::{Color, Event};
use crate::sys;
use crate::util::{cstring, ensure_abi, NotThreadSafe};
use crate::view::View;
use std::os::raw::c_char;
use std::rc::Rc;

/// App configuration (mirrors `affineui::App::Config`). Build with
/// [`Config::default`] plus the chained setters.
#[derive(Clone, Debug)]
pub struct Config {
    pub title: String,
    pub width: i32,
    pub height: i32,
    pub clear_color: Color,
    pub high_dpi: bool,
    pub vsync: bool,
    pub default_font_family: String,
    pub default_font_size: i32,
    pub asset_folders: Vec<String>,
    pub perf_overlay: bool,
    /// Runtime opt-out for the compile-time bundled Decius resources.
    /// The bundle is ON by default (this field is `false`); set to `true`
    /// to disable — no auto-applied stylesheet, no fallback-to-embedded
    /// on `frameworks/*` URLs — even when the bundle is compiled in.
    /// Ignored when affineui_c was built with `-DAFFINEUI_NO_BUNDLE_DECIUS`.
    pub no_bundle_decius: bool,
}

impl Default for Config {
    fn default() -> Config {
        Config {
            title: "AffineUI".to_owned(),
            width: 1024,
            height: 768,
            clear_color: Color::rgba(30, 30, 46, 255),
            high_dpi: true,
            vsync: true,
            default_font_family: "sans-serif".to_owned(),
            default_font_size: 16,
            asset_folders: vec![".".to_owned()],
            perf_overlay: false,
            no_bundle_decius: false,
        }
    }
}

impl Config {
    pub fn title(mut self, title: &str) -> Config {
        self.title = title.to_owned();
        self
    }
    pub fn size(mut self, width: i32, height: i32) -> Config {
        self.width = width;
        self.height = height;
        self
    }
    pub fn clear_color(mut self, color: Color) -> Config {
        self.clear_color = color;
        self
    }
    pub fn font(mut self, family: &str, size: i32) -> Config {
        self.default_font_family = family.to_owned();
        self.default_font_size = size;
        self
    }
    pub fn asset_folders(mut self, folders: &[&str]) -> Config {
        self.asset_folders = folders.iter().map(|f| (*f).to_owned()).collect();
        self
    }
    pub fn perf_overlay(mut self, on: bool) -> Config {
        self.perf_overlay = on;
        self
    }
    /// Disable the compile-time embedded Decius bundle for this app.
    /// See the field docs on `Config::no_bundle_decius`.
    pub fn no_bundle_decius(mut self, on: bool) -> Config {
        self.no_bundle_decius = on;
        self
    }
}

pub(crate) struct AppInner {
    raw: *mut sys::affineui_app,
    _not_send: NotThreadSafe,
}

impl Drop for AppInner {
    fn drop(&mut self) {
        unsafe { sys::affineui_app_destroy(self.raw) };
    }
}

/// A native AffineUI application (window + main loop). Cheap to clone
/// (shared handle).
#[derive(Clone)]
pub struct App {
    inner: Rc<AppInner>,
}

impl App {
    pub fn new(cfg: Config) -> App {
        ensure_abi();
        let title = cstring(&cfg.title);
        let font = cstring(&cfg.default_font_family);
        let folders: Vec<_> = cfg.asset_folders.iter().map(|f| cstring(f)).collect();
        let folder_ptrs: Vec<*const c_char> = folders.iter().map(|f| f.as_ptr()).collect();
        let c_cfg = sys::affineui_app_config {
            title: title.as_ptr(),
            width: cfg.width,
            height: cfg.height,
            clear_color: sys::affineui_color {
                r: cfg.clear_color.r,
                g: cfg.clear_color.g,
                b: cfg.clear_color.b,
                a: cfg.clear_color.a,
            },
            high_dpi: cfg.high_dpi as i32,
            vsync: cfg.vsync as i32,
            default_font_family: font.as_ptr(),
            default_font_size: cfg.default_font_size,
            asset_folders: if folder_ptrs.is_empty() { std::ptr::null() } else { folder_ptrs.as_ptr() },
            asset_folder_count: folder_ptrs.len(),
            perf_overlay: cfg.perf_overlay as i32,
            no_bundle_decius: cfg.no_bundle_decius as i32,
        };
        let raw = unsafe { sys::affineui_app_create(&c_cfg) };
        App { inner: Rc::new(AppInner { raw, _not_send: NotThreadSafe::default() }) }
    }

    /// Install a persistent view builder: the app retains a View, re-runs
    /// the builder on every rebuild, and reconciles only the diff into the
    /// live document. REQUIRED for recycling virtual lists to follow the
    /// scrollbar. Follow state changes with [`App::invalidate`] (coalesced)
    /// or [`App::rebuild_view`] (synchronous).
    pub fn set_view(&self, mut builder: impl FnMut(&crate::View) + 'static) {
        use std::ffi::c_void;
        unsafe extern "C" fn build_trampoline(user: *mut c_void, view: *mut sys::affineui_view) {
            let cb = &mut *(user as *mut Box<dyn FnMut(&crate::View)>);
            let view = crate::View::borrowed(view);
            if std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| cb(&view))).is_err() {
                eprintln!("affineui: panic in set_view builder (suppressed)");
            }
        }
        unsafe extern "C" fn build_free(user: *mut c_void) {
            drop(Box::from_raw(user as *mut Box<dyn FnMut(&crate::View)>));
        }
        let boxed: Box<dyn FnMut(&crate::View)> = Box::new(move |v| builder(v));
        let user = Box::into_raw(Box::new(boxed)) as *mut c_void;
        unsafe {
            sys::affineui_app_set_view(self.raw(), Some(build_trampoline), user, Some(build_free))
        };
    }

    /// Re-run the installed set_view builder and reconcile synchronously.
    pub fn rebuild_view(&self) {
        unsafe { sys::affineui_app_rebuild_view(self.raw()) };
    }

    fn raw(&self) -> *mut sys::affineui_app {
        self.inner.raw
    }

    /// Load an HTML string (retained-mode entry point).
    pub fn load_html(&self, html: &str) {
        let html = cstring(html);
        unsafe { sys::affineui_app_load_html(self.raw(), html.as_ptr()) };
    }

    pub fn load_html_file(&self, path: &str) -> bool {
        let path = cstring(path);
        unsafe { sys::affineui_app_load_html_file(self.raw(), path.as_ptr()) != 0 }
    }

    /// Copy a [`View`] (callbacks included) into the app. The app never
    /// borrows the caller's view.
    pub fn load_view(&self, view: &View) {
        unsafe { sys::affineui_app_load_view(self.raw(), view.raw()) };
    }

    /// Install/replace the user stylesheet. `base_url` (if given) is the
    /// sheet's own location for relative `url()` resolution.
    pub fn set_stylesheet(&self, css: &str, base_url: Option<&str>) {
        let css = cstring(css);
        let base = base_url.map(cstring);
        unsafe {
            sys::affineui_app_set_stylesheet(
                self.raw(),
                css.as_ptr(),
                base.as_ref().map_or(std::ptr::null(), |b| b.as_ptr()),
            )
        };
    }

    pub fn invalidate(&self) {
        unsafe { sys::affineui_app_invalidate(self.raw()) };
    }

    pub fn set_perf_overlay_enabled(&self, enabled: bool) {
        unsafe { sys::affineui_app_set_perf_overlay_enabled(self.raw(), enabled as i32) };
    }

    pub fn perf_overlay_enabled(&self) -> bool {
        unsafe { sys::affineui_app_perf_overlay_enabled(self.raw()) != 0 }
    }

    /// Dispatch an event through the loaded document. Returns true when
    /// consumed (a command callback fired or a redraw was requested).
    pub fn dispatch(&self, ev: &Event) -> bool {
        ev.with_sys(|sys_ev| unsafe { sys::affineui_app_dispatch(self.raw(), sys_ev) != 0 })
    }

    /// Run the native main loop on the calling thread. Returns the OS
    /// exit code.
    pub fn run(&self) -> i32 {
        unsafe { sys::affineui_app_run(self.raw()) }
    }

    /// Request a clean exit after the current frame.
    pub fn quit(&self, code: i32) {
        unsafe { sys::affineui_app_quit(self.raw(), code) };
    }

    /// Window size in logical CSS points.
    pub fn window_size(&self) -> (i32, i32) {
        let (mut w, mut h) = (0, 0);
        unsafe { sys::affineui_app_window_size(self.raw(), &mut w, &mut h) };
        (w, h)
    }

    /// Drawable framebuffer size in physical pixels.
    pub fn framebuffer_size(&self) -> (i32, i32) {
        let (mut w, mut h) = (0, 0);
        unsafe { sys::affineui_app_framebuffer_size(self.raw(), &mut w, &mut h) };
        (w, h)
    }

    /// Framebuffer pixels per CSS point.
    pub fn dpi_scale(&self) -> f32 {
        unsafe { sys::affineui_app_dpi_scale(self.raw()) }
    }

    /// The app's retained document. The returned handle keeps the app
    /// alive, so it can never dangle.
    pub fn document(&self) -> Document {
        let raw = unsafe { sys::affineui_app_document(self.raw()) };
        Document::borrowed(raw, Rc::clone(&self.inner))
    }
}
