//! The command-tree [`View`] builder and safe [`Widget`] handles.
//!
//! A `View` is a gradio/imgui-style component tree that AffineUI
//! reconciles into its retained DOM ([`crate::App::load_view`]). Builders
//! append widgets at the current insertion point; scope builders take a
//! closure that fills their children. Every builder returns a [`Widget`]
//! — an id-addressed handle that survives reconciliation and degrades
//! gracefully (reads return defaults, writes no-op) once its node is
//! gone.
//!
//! Lifetimes: a `Widget` owns only an invalidating native handle. It does not
//! retain its `View`; after the View or node is gone, reads return defaults and
//! writes safely no-op. A `View` received by a callback is also invalidating:
//! cloning or storing it is safe, but operations fail with a clear Rust panic
//! after the native owner destroys the View. Use [`View::is_alive`] to check
//! without panicking.

use crate::dock::{DockLocation, DockPlacement};
use crate::sys;
use crate::util::{cstring, ensure_abi, take_string, NotThreadSafe};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;

/// CSS framework personality (mirrors `affineui::ViewTheme`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum Theme {
    Plain = 0,
    #[default]
    Bootstrap = 1,
    Decius = 2,
}

/// Widget node kind (mirrors `affineui::WidgetKind`; `None` is the
/// "no node attached" sentinel).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum WidgetKind {
    None = -1,
    Root = 0,
    Container = 1,
    Text = 2,
    RawHtml = 3,
    Heading = 4,
    Panel = 5,
    Button = 6,
    Checkbox = 7,
    Slider = 8,
    Knob = 9,
    TextInput = 10,
    TextArea = 11,
    Dropdown = 12,
    ButtonGroup = 13,
    VirtualList = 14,
    Card = 15,
}

impl WidgetKind {
    fn from_raw(v: i32) -> WidgetKind {
        match v {
            0 => WidgetKind::Root,
            1 => WidgetKind::Container,
            2 => WidgetKind::Text,
            3 => WidgetKind::RawHtml,
            4 => WidgetKind::Heading,
            5 => WidgetKind::Panel,
            6 => WidgetKind::Button,
            7 => WidgetKind::Checkbox,
            8 => WidgetKind::Slider,
            9 => WidgetKind::Knob,
            10 => WidgetKind::TextInput,
            11 => WidgetKind::TextArea,
            12 => WidgetKind::Dropdown,
            13 => WidgetKind::ButtonGroup,
            14 => WidgetKind::VirtualList,
            15 => WidgetKind::Card,
            _ => WidgetKind::None,
        }
    }
}

#[derive(Clone, Copy)]
enum ViewHandle {
    Owned(*mut sys::affineui_view),
    // Callback-provided Views can escape because View is Clone. Never retain
    // their borrowed raw pointer: the native weak handle resolves it only while
    // the View's independent lifetime token is alive.
    Weak(*mut sys::affineui_weak_view),
}

pub(crate) struct ViewInner {
    handle: ViewHandle,
    _not_send: NotThreadSafe,
}

impl Drop for ViewInner {
    fn drop(&mut self) {
        unsafe {
            match self.handle {
                ViewHandle::Owned(raw) => sys::affineui_view_destroy(raw),
                ViewHandle::Weak(raw) => sys::affineui_weak_view_destroy(raw),
            }
        }
    }
}

/// A command-tree view. Cheap to clone (shared handle).
#[derive(Clone)]
pub struct View {
    pub(crate) inner: Rc<ViewInner>,
}

// ── Callback trampolines ─────────────────────────────────────────────
// Panics must never unwind across the FFI: every trampoline body is
// wrapped in catch_unwind.

unsafe extern "C" fn click_trampoline(user: *mut c_void) {
    let cb = &mut *(user as *mut Box<dyn FnMut()>);
    if catch_unwind(AssertUnwindSafe(&mut *cb)).is_err() {
        eprintln!("affineui: panic in on_click callback (suppressed)");
    }
}

unsafe extern "C" fn click_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut()>));
}

// ── Deferred build callbacks (docking) ───────────────────────────────
//
// The dock builders (document / dockpanel / dock_toolbar) do NOT run their
// closure before returning: the engine records it and invokes it later, when the
// container resolves and emits the layout. `with_build` is wrong for them — it
// parks a `BuildEnv` on the *caller's stack*, which is gone by then, and the
// engine dereferences it. (That is a real access violation, not a theoretical
// one; it is what the first cut of this binding did.)
//
// So: box the closure onto the heap, hand the engine a raw pointer, and give it
// a `user_free` to drop the box exactly once when it releases the callback.
type BoxedBuild = Box<dyn FnMut(&View)>;

fn deferred_build(
    f: impl FnMut(&View) + 'static,
) -> (sys::affineui_build_fn, *mut c_void, sys::affineui_user_free_fn) {
    let boxed: BoxedBuild = Box::new(f);
    let user = Box::into_raw(Box::new(boxed)) as *mut c_void;
    (
        Some(deferred_build_trampoline),
        user,
        Some(deferred_build_free),
    )
}

unsafe extern "C" fn deferred_build_trampoline(
    user: *mut c_void,
    raw_view: *mut sys::affineui_view,
) {
    let cb = &mut *(user as *mut BoxedBuild);
    let view = View::borrowed(raw_view);
    if catch_unwind(AssertUnwindSafe(|| cb(&view))).is_err() {
        eprintln!("affineui: panic in dock build callback (suppressed)");
    }
}

unsafe extern "C" fn deferred_build_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut BoxedBuild));
}

// ── Dock provider trampolines ────────────────────────────────────────
// Each owns its boxed closure and is released through `user_free` exactly once
// when the view drops the callback — the same contract as on_click. A leak or a
// double-free here is a real crash, so the free fns mirror the box types exactly.

unsafe fn cstr_arg<'a>(p: *const c_char) -> &'a str {
    if p.is_null() {
        ""
    } else {
        CStr::from_ptr(p).to_str().unwrap_or("")
    }
}

unsafe extern "C" fn dock_size_trampoline(user: *mut c_void, pane_id: *const c_char) -> c_int {
    let cb = &mut *(user as *mut Box<dyn FnMut(&str) -> i32>);
    // A panic must never unwind across the FFI boundary (UB / abort). On panic,
    // report the documented default: <= 0 means "no saved size; use the declared
    // one", which is exactly the safe fallback.
    catch_unwind(AssertUnwindSafe(|| cb(cstr_arg(pane_id)))).unwrap_or_else(|_| {
        eprintln!("affineui: panic in dock size provider (suppressed)");
        0
    })
}
unsafe extern "C" fn dock_size_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut(&str) -> i32>));
}

/// The active-tab callback returns a borrowed `const char*`. The engine copies it
/// immediately, but it must stay valid until the callback returns — so park it
/// here rather than dangling a temporary.
pub(crate) struct DockTabState {
    pub(crate) f: Box<dyn FnMut(&str) -> String>,
    pub(crate) last: CString,
}

unsafe extern "C" fn dock_tab_trampoline(
    user: *mut c_void,
    pane_id: *const c_char,
) -> *const c_char {
    let st = &mut *(user as *mut DockTabState);
    // On panic: the empty string, which the engine reads as "the primary panel".
    let s = catch_unwind(AssertUnwindSafe(|| (st.f)(cstr_arg(pane_id)))).unwrap_or_else(|_| {
        eprintln!("affineui: panic in dock active-tab provider (suppressed)");
        String::new()
    });
    st.last = CString::new(s).unwrap_or_else(|_| CString::new("").unwrap());
    st.last.as_ptr()
}
unsafe extern "C" fn dock_tab_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut DockTabState));
}

/// Same story for the placement callback's `parent` string.
pub(crate) struct DockPlacementState {
    pub(crate) f: Box<dyn FnMut(&str) -> Option<DockPlacement>>,
    pub(crate) last_parent: CString,
}

unsafe extern "C" fn dock_placement_trampoline(
    user: *mut c_void,
    panel_id: *const c_char,
    out: *mut sys::affineui_dock_placement,
) {
    // Zero the whole POD FIRST. A panic below leaves `out` fully initialised as
    // "no override" rather than partially written or untouched — the engine
    // reads every field, so a half-filled struct is worse than a defaulted one.
    *out = sys::affineui_dock_placement::default();

    let st = &mut *(user as *mut DockPlacementState);
    let result = catch_unwind(AssertUnwindSafe(|| (st.f)(cstr_arg(panel_id))));
    match result {
        Ok(Some(p)) => {
            st.last_parent =
                CString::new(p.parent.as_str()).unwrap_or_else(|_| CString::new("").unwrap());
            p.write_raw(out, &st.last_parent);
        }
        // present = 0 (already zeroed) => "no override; use the declared location".
        Ok(None) => {}
        Err(_) => eprintln!("affineui: panic in dock placement provider (suppressed)"),
    }
}
unsafe extern "C" fn dock_placement_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut DockPlacementState));
}

unsafe extern "C" fn change_trampoline(user: *mut c_void, value: *const c_char) {
    let cb = &mut *(user as *mut Box<dyn FnMut(&str)>);
    let value = if value.is_null() {
        String::new()
    } else {
        std::ffi::CStr::from_ptr(value).to_string_lossy().into_owned()
    };
    if catch_unwind(AssertUnwindSafe(|| cb(&value))).is_err() {
        eprintln!("affineui: panic in on_change callback (suppressed)");
    }
}

unsafe extern "C" fn change_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut(&str)>));
}

// Scope-builder env: the closure runs synchronously inside the builder
// call, so it borrows locals; the env only lives for that call.
struct BuildEnv<F: FnOnce(&View)> {
    f: Option<F>,
}

unsafe extern "C" fn build_trampoline<F: FnOnce(&View)>(
    user: *mut c_void,
    raw_view: *mut sys::affineui_view,
) {
    let env = &mut *(user as *mut BuildEnv<F>);
    if let Some(f) = env.f.take() {
        let view = View::borrowed(raw_view);
        if catch_unwind(AssertUnwindSafe(|| f(&view))).is_err() {
            eprintln!("affineui: panic in build callback (suppressed)");
        }
    }
}

impl View {
    pub fn new(theme: Theme) -> View {
        ensure_abi();
        let raw = unsafe { sys::affineui_view_create(theme as i32) };
        View {
            inner: Rc::new(ViewInner {
                handle: ViewHandle::Owned(raw),
                _not_send: NotThreadSafe::default(),
            }),
        }
    }

    /// Wrap a framework-owned View handed to a callback. Copy its native weak
    /// lifetime token while the borrowed pointer is live, so clones that escape
    /// the callback resolve to null rather than a freed View.
    pub(crate) fn borrowed(raw: *mut sys::affineui_view) -> View {
        let weak = unsafe { sys::affineui_view_weak_ref(raw) };
        View {
            inner: Rc::new(ViewInner {
                handle: ViewHandle::Weak(weak),
                _not_send: NotThreadSafe::default(),
            }),
        }
    }

    fn resolved_raw(&self) -> *mut sys::affineui_view {
        match self.inner.handle {
            ViewHandle::Owned(raw) => raw,
            ViewHandle::Weak(weak) => unsafe { sys::affineui_weak_view_get(weak) },
        }
    }

    /// Whether the native View is still alive. Owned Views remain alive until
    /// their last clone is dropped; callback Views become false after their
    /// framework owner is destroyed.
    pub fn is_alive(&self) -> bool {
        !self.resolved_raw().is_null()
    }

    pub(crate) fn raw(&self) -> *mut sys::affineui_view {
        let raw = self.resolved_raw();
        assert!(!raw.is_null(), "AffineUI callback View is no longer alive");
        raw
    }

    pub(crate) fn wrap_widget(&self, raw: *mut sys::affineui_widget) -> Widget {
        self.wrap(raw)
    }

    fn wrap(&self, raw: *mut sys::affineui_widget) -> Widget {
        Widget { raw, _not_send: NotThreadSafe::default() }
    }

    fn with_build<F: FnOnce(&View), R>(
        &self,
        f: F,
        call: impl FnOnce(sys::affineui_build_fn, *mut c_void) -> R,
    ) -> R {
        let mut env = BuildEnv { f: Some(f) };
        call(Some(build_trampoline::<F>), &mut env as *mut _ as *mut c_void)
    }

    // ── Lifecycle ────────────────────────────────────────────────────

    pub fn clear(&self) {
        unsafe { sys::affineui_view_clear(self.raw()) };
    }

    /// `begin()` + your builder closure + `end()` in one call.
    pub fn build(&self, f: impl FnOnce(&View)) {
        self.begin();
        f(self);
        self.end();
    }

    pub fn begin(&self) {
        unsafe { sys::affineui_view_begin(self.raw()) };
    }

    pub fn end(&self) {
        unsafe { sys::affineui_view_end(self.raw()) };
    }

    pub fn set_theme(&self, theme: Theme) {
        unsafe { sys::affineui_view_set_theme(self.raw(), theme as i32) };
    }

    pub fn theme(&self) -> Theme {
        match unsafe { sys::affineui_view_get_theme(self.raw()) } {
            0 => Theme::Plain,
            2 => Theme::Decius,
            _ => Theme::Bootstrap,
        }
    }

    /// Pin the CSS framework version this view targets (e.g. `"0.6.2"`
    /// for Decius). Empty = personality default.
    pub fn set_framework_version(&self, version: &str) {
        let version = cstring(version);
        unsafe { sys::affineui_view_set_framework_version(self.raw(), version.as_ptr()) };
    }

    /// Set a personality selector (e.g. Decius `"density"` = `"compact"`).
    pub fn selector(&self, name: &str, value: &str) -> &Self {
        let name = cstring(name);
        let value = cstring(value);
        unsafe { sys::affineui_view_selector(self.raw(), name.as_ptr(), value.as_ptr()) };
        self
    }

    pub fn to_html_fragment(&self) -> String {
        unsafe { take_string(sys::affineui_view_to_html_fragment(self.raw())) }
    }

    pub fn to_html_document(&self) -> String {
        unsafe { take_string(sys::affineui_view_to_html_document(self.raw())) }
    }

    // ── Content / form builders ──────────────────────────────────────
    // `classes` and `key` accept "" for none, mirroring the C++ defaults.

    pub fn heading(&self, level: i32, text: &str, classes: &str, key: &str) -> Widget {
        let (text, classes, key) = (cstring(text), cstring(classes), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_heading(self.raw(), level, text.as_ptr(), classes.as_ptr(), key.as_ptr())
        })
    }

    pub fn paragraph(&self, text: &str, classes: &str, key: &str) -> Widget {
        let (text, classes, key) = (cstring(text), cstring(classes), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_paragraph(self.raw(), text.as_ptr(), classes.as_ptr(), key.as_ptr())
        })
    }

    pub fn text(&self, text: &str, key: &str) -> Widget {
        let (text, key) = (cstring(text), cstring(key));
        self.wrap(unsafe { sys::affineui_view_text(self.raw(), text.as_ptr(), key.as_ptr()) })
    }

    /// Append trusted raw HTML (parsed when the view is loaded).
    pub fn html(&self, markup: &str, key: &str) -> Widget {
        let (markup, key) = (cstring(markup), cstring(key));
        self.wrap(unsafe { sys::affineui_view_html(self.raw(), markup.as_ptr(), key.as_ptr()) })
    }

    pub fn button(&self, label: &str, primary: bool, key: &str) -> Widget {
        let (label, key) = (cstring(label), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_button(self.raw(), label.as_ptr(), primary as i32, key.as_ptr())
        })
    }

    pub fn checkbox(&self, label: &str, checked: bool, key: &str) -> Widget {
        let (label, key) = (cstring(label), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_checkbox(self.raw(), label.as_ptr(), checked as i32, key.as_ptr())
        })
    }

    pub fn toggle(&self, label: &str, on: bool, key: &str) -> Widget {
        let (label, key) = (cstring(label), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_toggle(self.raw(), label.as_ptr(), on as i32, key.as_ptr())
        })
    }

    pub fn input(&self, label: &str, value: &str, input_type: &str, key: &str) -> Widget {
        let (label, value, ty, key) = (cstring(label), cstring(value), cstring(input_type), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_input(self.raw(), label.as_ptr(), value.as_ptr(), ty.as_ptr(), key.as_ptr())
        })
    }

    pub fn password(&self, label: &str, value: &str, key: &str) -> Widget {
        let (label, value, key) = (cstring(label), cstring(value), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_password(self.raw(), label.as_ptr(), value.as_ptr(), key.as_ptr())
        })
    }

    pub fn textarea(&self, label: &str, value: &str, rows: i32, key: &str) -> Widget {
        let (label, value, key) = (cstring(label), cstring(value), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_textarea(self.raw(), label.as_ptr(), value.as_ptr(), rows, key.as_ptr())
        })
    }

    pub fn dropdown(&self, label: &str, options: &[&str], selected: &str, key: &str) -> Widget {
        let (label, selected, key) = (cstring(label), cstring(selected), cstring(key));
        let opts: Vec<_> = options.iter().map(|o| cstring(o)).collect();
        let ptrs: Vec<*const c_char> = opts.iter().map(|o| o.as_ptr()).collect();
        self.wrap(unsafe {
            sys::affineui_view_dropdown(
                self.raw(), label.as_ptr(), ptrs.as_ptr(), ptrs.len(), selected.as_ptr(), key.as_ptr(),
            )
        })
    }

    pub fn button_group(&self, label: &str, options: &[&str], selected: &str, key: &str) -> Widget {
        let (label, selected, key) = (cstring(label), cstring(selected), cstring(key));
        let opts: Vec<_> = options.iter().map(|o| cstring(o)).collect();
        let ptrs: Vec<*const c_char> = opts.iter().map(|o| o.as_ptr()).collect();
        self.wrap(unsafe {
            sys::affineui_view_button_group(
                self.raw(), label.as_ptr(), ptrs.as_ptr(), ptrs.len(), selected.as_ptr(), key.as_ptr(),
            )
        })
    }

    pub fn slider(&self, label: &str, value: f64, min: f64, max: f64, key: &str) -> Widget {
        let (label, key) = (cstring(label), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_slider(self.raw(), label.as_ptr(), value, min, max, key.as_ptr())
        })
    }

    pub fn knob(&self, label: &str, value: f64, min: f64, max: f64, bipolar: bool, key: &str) -> Widget {
        let (label, key) = (cstring(label), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_knob(self.raw(), label.as_ptr(), value, min, max, bipolar as i32, key.as_ptr())
        })
    }

    /// Bare drag-scrub numeric combo (no field/label wrapper).
    /// Bare drag-scrub numeric combo. `linear` scrubs at a constant
    /// step/pixel (rotation degrees etc.); when false the scrub
    /// accelerates with the value's magnitude.
    pub fn combo(&self, label: &str, value: f64, step: f64, key: &str, linear: bool) -> Widget {
        let (label, key) = (cstring(label), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_combo(
                self.raw(), label.as_ptr(), value, step, key.as_ptr(), linear as i32,
            )
        })
    }

    /// Color field with a swatch-picker popup.
    pub fn color_field(&self, label: &str, value: &str, swatches: &[&str], key: &str) -> Widget {
        let (label, value, key) = (cstring(label), cstring(value), cstring(key));
        let sw: Vec<_> = swatches.iter().map(|s| cstring(s)).collect();
        let ptrs: Vec<*const c_char> = sw.iter().map(|s| s.as_ptr()).collect();
        self.wrap(unsafe {
            sys::affineui_view_color_field(
                self.raw(), label.as_ptr(), value.as_ptr(), ptrs.as_ptr(), ptrs.len(), key.as_ptr(),
            )
        })
    }

    /// Decius color field: chip + editable hex + picker popover.
    pub fn colorfield(&self, label: &str, value: &str, key: &str) -> Widget {
        let (label, value, key) = (cstring(label), cstring(value), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_colorfield(self.raw(), label.as_ptr(), value.as_ptr(), key.as_ptr())
        })
    }

    // ── Scope builders (closure fills the children) ──────────────────

    pub fn container(&self, classes: &str, key: &str, build: impl FnOnce(&View)) -> Widget {
        let (classes, key) = (cstring(classes), cstring(key));
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_container(self.raw(), classes.as_ptr(), key.as_ptr(), cb, user)
        });
        self.wrap(raw)
    }

    pub fn element(&self, tag: &str, classes: &str, key: &str, build: impl FnOnce(&View)) -> Widget {
        let (tag, classes, key) = (cstring(tag), cstring(classes), cstring(key));
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_element(self.raw(), tag.as_ptr(), classes.as_ptr(), key.as_ptr(), cb, user)
        });
        self.wrap(raw)
    }

    pub fn panel(&self, key: &str, build: impl FnOnce(&View)) -> Widget {
        let key = cstring(key);
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_panel(self.raw(), key.as_ptr(), cb, user)
        });
        self.wrap(raw)
    }

    // ── Declarative docking ──────────────────────────────────────────
    // See the `dock` module for the model. A container resolves a flat set of
    // panel declarations into a split tree and emits the DOM; panels may be
    // declared in any order because each names its own parent and side.

    /// Declare a dock container. Inside `build`, call [`View::document`] for the
    /// center pane and [`View::dockpanel`] for the panels around it.
    pub fn document_view(&self, key: &str, build: impl FnOnce(&View)) -> Widget {
        let key = cstring(key);
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_document_view(self.raw(), key.as_ptr(), cb, user)
        });
        self.wrap(raw)
    }

    /// The center/document pane. Only valid inside a [`View::document_view`] build.
    ///
    /// `icon` is a Decius icon-font glyph name (`""` for none). Returns the pane
    /// id — pass it to [`DockLocation::in_pane`] to parent a panel to it, or to
    /// [`View::dock_toolbar`] to give it a tab toolbar.
    ///
    /// `content` is **deferred**: the dock engine records it and runs it later,
    /// when the container resolves and emits the layout. It is therefore
    /// heap-owned and `'static`, unlike the immediate builders' closures.
    pub fn document(
        &self,
        title: &str,
        icon: &str,
        content: impl FnMut(&View) + 'static,
    ) -> String {
        let (title, icon) = (cstring(title), cstring(icon));
        let (cb, user, free) = deferred_build(content);
        let raw = unsafe {
            sys::affineui_view_document(self.raw(), cb, user, free, title.as_ptr(), icon.as_ptr())
        };
        unsafe { take_string(raw) }
    }

    /// Declare a dockable panel. Only valid inside a [`View::document_view`] build.
    ///
    /// `where_` carries the placement — use [`DockLocation::docked`],
    /// [`DockLocation::tab`], [`DockLocation::floating`], or
    /// [`DockLocation::tearoff`]. Returns the panel id, usable as another
    /// panel's parent and as the `pane_id` the dock providers are asked about.
    ///
    /// `content` is **deferred** — see [`View::document`].
    pub fn dockpanel(
        &self,
        title: &str,
        where_: DockLocation,
        icon: &str,
        key: &str,
        content: impl FnMut(&View) + 'static,
    ) -> String {
        let (title, icon, key) = (cstring(title), cstring(icon), cstring(key));
        // Holds the CStrings `loc.raw` points into for the duration of the call
        // (the engine copies the location before this returns).
        let loc = where_.to_raw();
        let (cb, user, free) = deferred_build(content);
        let raw = unsafe {
            sys::affineui_view_dockpanel(
                self.raw(),
                title.as_ptr(),
                &loc.raw as *const _,
                cb,
                user,
                free,
                icon.as_ptr(),
                key.as_ptr(),
            )
        };
        unsafe { take_string(raw) }
    }

    /// Give a dock pane its tab toolbar — the strip beside the tabs (filter
    /// buttons, a search field, a viewport's mode/tool controls).
    ///
    /// `pane_id` is what [`View::document`] or [`View::dockpanel`] returned.
    ///
    /// `build` is **deferred** — see [`View::document`].
    pub fn dock_toolbar(&self, pane_id: &str, build: impl FnMut(&View) + 'static) {
        let pane_id = cstring(pane_id);
        let (cb, user, free) = deferred_build(build);
        unsafe {
            sys::affineui_view_dock_toolbar(self.raw(), pane_id.as_ptr(), cb, user, free)
        };
    }

    // ── Dock providers: a saved workspace beats the declared seed ────

    /// Supply the saved px size of each pane (return `<= 0` to fall back to the
    /// size declared in its [`DockLocation`]).
    pub fn set_dock_size_provider(&self, f: impl FnMut(&str) -> i32 + 'static) {
        let boxed: Box<dyn FnMut(&str) -> i32> = Box::new(f);
        let user = Box::into_raw(Box::new(boxed)) as *mut c_void;
        unsafe {
            sys::affineui_view_set_dock_size_provider(
                self.raw(),
                Some(dock_size_trampoline),
                user,
                Some(dock_size_free),
            );
        }
    }

    /// Supply the active tab of each dock leaf (empty selects the primary panel).
    pub fn set_dock_active_tab_provider(&self, f: impl FnMut(&str) -> String + 'static) {
        // The C callback returns a borrowed `const char*` that the engine copies
        // immediately, so the trampoline parks the String in the closure's own
        // state to keep it alive across the return.
        let boxed: Box<DockTabState> = Box::new(DockTabState {
            f: Box::new(f),
            last: CString::new("").unwrap(),
        });
        let user = Box::into_raw(boxed) as *mut c_void;
        unsafe {
            sys::affineui_view_set_dock_active_tab_provider(
                self.raw(),
                Some(dock_tab_trampoline),
                user,
                Some(dock_tab_free),
            );
        }
    }

    /// Supply the saved placement of each panel — where the user dragged or tore
    /// it to. `None` means "no override; use the declared [`DockLocation`]".
    ///
    /// Read the values back out with [`crate::Document::dock_overrides`].
    pub fn set_dock_placement_provider(
        &self,
        f: impl FnMut(&str) -> Option<DockPlacement> + 'static,
    ) {
        let boxed: Box<DockPlacementState> = Box::new(DockPlacementState {
            f: Box::new(f),
            last_parent: CString::new("").unwrap(),
        });
        let user = Box::into_raw(boxed) as *mut c_void;
        unsafe {
            sys::affineui_view_set_dock_placement_provider(
                self.raw(),
                Some(dock_placement_trampoline),
                user,
                Some(dock_placement_free),
            );
        }
    }

    /// Replay the CURRENT arrangement — splits, tab order, active tabs, floats —
    /// straight from `doc`, instead of the declared seed. This is what makes
    /// drag-to-dock and tearoff survive a view rebuild.
    ///
    /// The live arrangement is a recursive tree that every caller round-trips
    /// back from the document it is rebuilding, so it is wired directly rather
    /// than marshalled through the FFI.
    pub fn set_dock_layout_from_document(&self, doc: &crate::Document) {
        unsafe { sys::affineui_view_set_dock_layout_from_document(self.raw(), doc.raw()) };
    }

    pub fn card(&self, title: &str, classes: &str, key: &str, build: impl FnOnce(&View)) -> Widget {
        let (title, classes, key) = (cstring(title), cstring(classes), cstring(key));
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_card(self.raw(), title.as_ptr(), classes.as_ptr(), key.as_ptr(), cb, user)
        });
        self.wrap(raw)
    }

    pub fn toolbar(&self, key: &str, build: impl FnOnce(&View)) -> Widget {
        let key = cstring(key);
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_toolbar(self.raw(), key.as_ptr(), cb, user)
        });
        self.wrap(raw)
    }

    pub fn menu_bar(&self, key: &str, build: impl FnOnce(&View)) -> Widget {
        let key = cstring(key);
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_menu_bar(self.raw(), key.as_ptr(), cb, user)
        });
        self.wrap(raw)
    }

    pub fn status_bar(&self, key: &str, build: impl FnOnce(&View)) -> Widget {
        let key = cstring(key);
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_status_bar(self.raw(), key.as_ptr(), cb, user)
        });
        self.wrap(raw)
    }

    pub fn tree(&self, key: &str, build: impl FnOnce(&View)) -> Widget {
        let key = cstring(key);
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_tree(self.raw(), key.as_ptr(), cb, user)
        });
        self.wrap(raw)
    }

    pub fn foldout(&self, title: &str, expanded: bool, key: &str, build: impl FnOnce(&View)) -> Widget {
        let (title, key) = (cstring(title), cstring(key));
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_foldout(self.raw(), title.as_ptr(), expanded as i32, key.as_ptr(), cb, user)
        });
        self.wrap(raw)
    }

    // ── Structural leaves ────────────────────────────────────────────

    pub fn toolbar_separator(&self, key: &str) -> Widget {
        let key = cstring(key);
        self.wrap(unsafe { sys::affineui_view_toolbar_separator(self.raw(), key.as_ptr()) })
    }

    /// Icon-only ghost button (`icon` = Decius icon name, e.g. `"save"`).
    pub fn icon_button(&self, icon: &str, key: &str) -> Widget {
        let (icon, key) = (cstring(icon), cstring(key));
        self.wrap(unsafe { sys::affineui_view_icon_button(self.raw(), icon.as_ptr(), key.as_ptr()) })
    }

    /// Menubar button that owns its dropdown; `build` fills the menu with
    /// [`View::menu_item`] / [`View::menu_separator`] / [`View::submenu`].
    pub fn menu_button(&self, label: &str, key: &str, build: impl FnOnce(&View)) -> Widget {
        let (label, key) = (cstring(label), cstring(key));
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_menu_button(self.raw(), label.as_ptr(), cb, user, key.as_ptr())
        });
        self.wrap(raw)
    }

    pub fn menu_item(&self, label: &str, icon: &str, shortcut: &str, key: &str) -> Widget {
        let (label, icon, shortcut, key) = (cstring(label), cstring(icon), cstring(shortcut), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_menu_item(self.raw(), label.as_ptr(), icon.as_ptr(), shortcut.as_ptr(), key.as_ptr())
        })
    }

    pub fn menu_separator(&self, key: &str) -> Widget {
        let key = cstring(key);
        self.wrap(unsafe { sys::affineui_view_menu_separator(self.raw(), key.as_ptr()) })
    }

    pub fn submenu(&self, label: &str, icon: &str, key: &str, build: impl FnOnce(&View)) -> Widget {
        let (label, icon, key) = (cstring(label), cstring(icon), cstring(key));
        let raw = self.with_build(build, |cb, user| unsafe {
            sys::affineui_view_submenu(self.raw(), label.as_ptr(), cb, user, icon.as_ptr(), key.as_ptr())
        });
        self.wrap(raw)
    }

    pub fn menu_brand(&self, title: &str, icon: &str, key: &str) -> Widget {
        let (title, icon, key) = (cstring(title), cstring(icon), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_menu_brand(self.raw(), title.as_ptr(), icon.as_ptr(), key.as_ptr())
        })
    }

    pub fn menu_spacer(&self, key: &str) -> Widget {
        let key = cstring(key);
        self.wrap(unsafe { sys::affineui_view_menu_spacer(self.raw(), key.as_ptr()) })
    }

    /// The document being edited, centered in the bar — what a title bar shows.
    pub fn document_title(&self, text: &str, key: &str) -> Widget {
        let (text, key) = (cstring(text), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_document_title(self.raw(), text.as_ptr(), key.as_ptr())
        })
    }

    pub fn menu_meta(&self, text: &str, key: &str) -> Widget {
        let (text, key) = (cstring(text), cstring(key));
        self.wrap(unsafe { sys::affineui_view_menu_meta(self.raw(), text.as_ptr(), key.as_ptr()) })
    }

    pub fn tree_row(&self, label: &str, selected: bool, depth: i32, key: &str) -> Widget {
        let (label, key) = (cstring(label), cstring(key));
        self.wrap(unsafe {
            sys::affineui_view_tree_row(self.raw(), label.as_ptr(), selected as i32, depth, key.as_ptr())
        })
    }

    pub fn splitter(&self, horizontal: bool, key: &str) -> Widget {
        let key = cstring(key);
        self.wrap(unsafe {
            sys::affineui_view_splitter(self.raw(), horizontal as i32, key.as_ptr())
        })
    }

    /// Stable lookup by user key. Always returns a handle; check
    /// [`Widget::is_valid`].
    pub fn find_widget(&self, name: &str) -> Widget {
        let name = cstring(name);
        self.wrap(unsafe { sys::affineui_view_find_widget(self.raw(), name.as_ptr()) })
    }
}

/// Invalidating handle over a widget in a [`View`]. It does not retain the
/// View and becomes safely inert after either the View or node is gone.
pub struct Widget {
    raw: *mut sys::affineui_widget,
    _not_send: NotThreadSafe,
}

impl Drop for Widget {
    fn drop(&mut self) {
        unsafe { sys::affineui_widget_destroy(self.raw) };
    }
}

impl Widget {
    /// True when the handle resolves to a live node.
    pub fn is_valid(&self) -> bool {
        unsafe { sys::affineui_widget_valid(self.raw) != 0 }
    }

    pub fn kind(&self) -> WidgetKind {
        WidgetKind::from_raw(unsafe { sys::affineui_widget_get_kind(self.raw) })
    }

    pub fn name(&self) -> String {
        unsafe { take_string(sys::affineui_widget_name(self.raw)) }
    }

    pub fn attr(&self, name: &str, fallback: &str) -> String {
        let (name, fallback) = (cstring(name), cstring(fallback));
        unsafe { take_string(sys::affineui_widget_attr(self.raw, name.as_ptr(), fallback.as_ptr())) }
    }

    pub fn text(&self) -> String {
        unsafe { take_string(sys::affineui_widget_text(self.raw)) }
    }

    pub fn has_attr(&self, name: &str) -> bool {
        let name = cstring(name);
        unsafe { sys::affineui_widget_has_attr(self.raw, name.as_ptr()) != 0 }
    }

    pub fn set_text(&self, text: &str) -> &Self {
        let text = cstring(text);
        unsafe { sys::affineui_widget_set_text(self.raw, text.as_ptr()) };
        self
    }

    pub fn set_attr(&self, name: &str, value: &str) -> &Self {
        let (name, value) = (cstring(name), cstring(value));
        unsafe { sys::affineui_widget_set_attr(self.raw, name.as_ptr(), value.as_ptr()) };
        self
    }

    pub fn remove_attr(&self, name: &str) -> &Self {
        let name = cstring(name);
        unsafe { sys::affineui_widget_remove_attr(self.raw, name.as_ptr()) };
        self
    }

    pub fn set_selector(&self, name: &str, value: &str) -> &Self {
        let (name, value) = (cstring(name), cstring(value));
        unsafe { sys::affineui_widget_set_selector(self.raw, name.as_ptr(), value.as_ptr()) };
        self
    }

    pub fn add_class(&self, classes: &str) -> &Self {
        let classes = cstring(classes);
        unsafe { sys::affineui_widget_add_class(self.raw, classes.as_ptr()) };
        self
    }

    /// Remove all children.
    pub fn clear(&self) -> &Self {
        unsafe { sys::affineui_widget_clear(self.raw) };
        self
    }

    /// Register a click handler. The closure is released exactly once
    /// when the core drops the handler (replaced, or view destroyed).
    pub fn on_click(&self, f: impl FnMut() + 'static) -> &Self {
        let boxed: Box<dyn FnMut()> = Box::new(f);
        let user = Box::into_raw(Box::new(boxed)) as *mut c_void;
        unsafe {
            sys::affineui_widget_on_click(self.raw, Some(click_trampoline), user, Some(click_free));
        }
        self
    }

    /// Register a value-change handler (serialized value string).
    pub fn on_change(&self, f: impl FnMut(&str) + 'static) -> &Self {
        let boxed: Box<dyn FnMut(&str)> = Box::new(f);
        let user = Box::into_raw(Box::new(boxed)) as *mut c_void;
        unsafe {
            sys::affineui_widget_on_change(self.raw, Some(change_trampoline), user, Some(change_free));
        }
        self
    }

    /// Append children built by the closure (runs synchronously).
    pub fn append<F: FnOnce(&View)>(&self, build: F) -> &Self {
        let mut env = BuildEnv { f: Some(build) };
        unsafe {
            sys::affineui_widget_append(
                self.raw,
                Some(build_trampoline::<F>),
                &mut env as *mut _ as *mut c_void,
            );
        }
        self
    }

    /// Replace children with ones built by the closure.
    pub fn replace<F: FnOnce(&View)>(&self, build: F) -> &Self {
        let mut env = BuildEnv { f: Some(build) };
        unsafe {
            sys::affineui_widget_replace(
                self.raw,
                Some(build_trampoline::<F>),
                &mut env as *mut _ as *mut c_void,
            );
        }
        self
    }

    /// Find a descendant widget by key. Always returns a handle; check
    /// [`Widget::is_valid`].
    pub fn find_widget(&self, name: &str) -> Widget {
        let name = cstring(name);
        let raw = unsafe { sys::affineui_widget_find_widget(self.raw, name.as_ptr()) };
        Widget { raw, _not_send: NotThreadSafe::default() }
    }
}
