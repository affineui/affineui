//! Retained [`Document`]: parsed HTML + CSS + layout + event state.
//! Usable headless (tests, layout probes) or borrowed from an [`crate::App`].

use crate::app::AppInner;
use crate::event::Event;
use crate::sys;
use crate::util::{cstring, ensure_abi, NotThreadSafe};
use std::rc::Rc;

/// Result of an event dispatch (a snapshot, not a live reference).
#[derive(Clone, Copy, Debug, Default)]
pub struct DispatchResult {
    pub redraw_requested: bool,
    pub invalidate_view: bool,
    pub defer_widget_changes: bool,
    pub layout_changed: bool,
    pub event_consumed: bool,
}

/// Optional native behavior scripts (mirrors `affineui::DocumentScript`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum DocumentScript {
    /// Stock behavior for native form controls and framework widgets.
    UiControls = 0,
}

enum Owner {
    /// Created by [`Document::new`]; destroyed on drop.
    Owned,
    /// Borrowed from an App; the Rc keeps the App (and thus the raw
    /// document) alive for as long as this handle exists.
    App(#[allow(dead_code)] Rc<AppInner>),
}

/// A parsed HTML document with its associated CSS, layout, and event
/// state.
pub struct Document {
    raw: *mut sys::affineui_document,
    owner: Owner,
    _not_send: NotThreadSafe,
}

impl Drop for Document {
    fn drop(&mut self) {
        if matches!(self.owner, Owner::Owned) {
            unsafe { sys::affineui_document_destroy(self.raw) };
        }
    }
}

impl Document {
    /// A headless document (no app, no window) — for layout probes and
    /// tests.
    pub fn new() -> Document {
        ensure_abi();
        Document {
            raw: unsafe { sys::affineui_document_create() },
            owner: Owner::Owned,
            _not_send: NotThreadSafe::default(),
        }
    }

    pub(crate) fn borrowed(raw: *mut sys::affineui_document, app: Rc<AppInner>) -> Document {
        Document { raw, owner: Owner::App(app), _not_send: NotThreadSafe::default() }
    }

    /// Parse and replace the document body.
    pub fn set_html(&self, html: &str) {
        let html = cstring(html);
        unsafe { sys::affineui_document_set_html(self.raw, html.as_ptr()) };
    }

    /// Install/replace the user stylesheet. `base_url` (if given) is the
    /// sheet's own location so its relative `url()`s resolve like a
    /// `<link>`ed sheet's.
    pub fn set_user_stylesheet(&self, css: &str, base_url: Option<&str>) {
        let css = cstring(css);
        let base = base_url.map(cstring);
        unsafe {
            sys::affineui_document_set_user_stylesheet(
                self.raw,
                css.as_ptr(),
                base.as_ref().map_or(std::ptr::null(), |b| b.as_ptr()),
            )
        };
    }

    pub fn reload_stylesheets(&self) {
        unsafe { sys::affineui_document_reload_stylesheets(self.raw) };
    }

    /// Run a layout pass. `viewport_height` of 0 disables the
    /// fill-the-viewport floor.
    pub fn layout(&self, viewport_width: i32, viewport_height: i32) {
        unsafe { sys::affineui_document_layout(self.raw, viewport_width, viewport_height) };
    }

    /// Document content size after the last layout pass.
    pub fn content_size(&self) -> (i32, i32) {
        let (mut w, mut h) = (0, 0);
        unsafe { sys::affineui_document_content_size(self.raw, &mut w, &mut h) };
        (w, h)
    }

    /// Returns true only when the attribute value actually changed.
    pub fn set_attribute_by_id(&self, elem_id: &str, name: &str, value: &str) -> bool {
        let (id, name, value) = (cstring(elem_id), cstring(name), cstring(value));
        unsafe {
            sys::affineui_document_set_attribute_by_id(self.raw, id.as_ptr(), name.as_ptr(), value.as_ptr()) != 0
        }
    }

    pub fn remove_attribute_by_id(&self, elem_id: &str, name: &str) -> bool {
        let (id, name) = (cstring(elem_id), cstring(name));
        unsafe { sys::affineui_document_remove_attribute_by_id(self.raw, id.as_ptr(), name.as_ptr()) != 0 }
    }

    pub fn set_text_by_id(&self, elem_id: &str, text: &str) -> bool {
        let (id, text) = (cstring(elem_id), cstring(text));
        unsafe { sys::affineui_document_set_text_by_id(self.raw, id.as_ptr(), text.as_ptr()) != 0 }
    }

    /// Dispatch an event directly to this document.
    pub fn dispatch(&self, ev: &Event) -> DispatchResult {
        let mut out = sys::affineui_dispatch_result::default();
        ev.with_sys(|sys_ev| unsafe { sys::affineui_document_dispatch(self.raw, sys_ev, &mut out) });
        DispatchResult {
            redraw_requested: out.redraw_requested != 0,
            invalidate_view: out.invalidate_view != 0,
            defer_widget_changes: out.defer_widget_changes != 0,
            layout_changed: out.layout_changed != 0,
            event_consumed: out.event_consumed != 0,
        }
    }

    /// Set the caret visibility half-cycle in milliseconds. Zero keeps the
    /// focused caret continuously visible.
    pub fn set_caret_blink_interval(&self, milliseconds: f64) {
        unsafe { sys::affineui_document_set_caret_blink_interval(self.raw, milliseconds) };
    }

    pub fn caret_blink_interval(&self) -> f64 {
        unsafe { sys::affineui_document_caret_blink_interval(self.raw) }
    }

    /// Advance caret timing for a custom/headless Document driver. App and Ui
    /// hosts do this automatically.
    pub fn tick_caret_blink(&self) -> bool {
        unsafe { sys::affineui_document_tick_caret_blink(self.raw) != 0 }
    }

    pub fn attach_script(&self, script: DocumentScript) {
        unsafe { sys::affineui_document_attach_script(self.raw, script as i32) };
    }

    pub fn detach_script(&self, script: DocumentScript) {
        unsafe { sys::affineui_document_detach_script(self.raw, script as i32) };
    }

    /// Cursor the OS should display under the hovered element
    /// (0=default 1=pointer 2=text 3=crosshair 4=move 5=not-allowed
    ///  6=ew-resize 7=ns-resize 8=nwse-resize).
    pub fn hovered_cursor(&self) -> i32 {
        unsafe { sys::affineui_document_hovered_cursor(self.raw) }
    }
}

impl Default for Document {
    fn default() -> Document {
        Document::new()
    }
}
