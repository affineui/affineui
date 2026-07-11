//! Safe, idiomatic Rust bindings for AffineUI.
//!
//! Two operating modes, mirroring the C++ core (docs/LANGUAGE_BINDINGS.md):
//!
//! - **App-owned mode** — AffineUI owns the window and main loop:
//!   [`App`] + [`View`] + [`Widget`] + [`Document`].
//! - **Embedded mode** — the host engine owns the GPU device, window,
//!   loop, and input pump: [`embedded::Ui`] renders into host-provided
//!   render targets and receives forwarded events.
//!
//! Contracts inherited from the core, encoded in the types:
//!
//! - *Hard to crash*: operations on widgets whose nodes are gone degrade
//!   (reads return defaults, writes no-op) instead of panicking.
//! - *Thread affinity*: every handle type is `!Send + !Sync`; create and
//!   use everything on one thread.
//! - *Callback safety*: closures are boxed and released exactly once via
//!   the C ABI's `user_free` contract; panics inside callbacks are caught
//!   at the FFI boundary and never unwind into C++.
//!
//! ```no_run
//! use affineui::{App, Config, Theme, View};
//!
//! let view = View::new(Theme::Bootstrap);
//! view.build(|v| {
//!     v.heading(1, "Hello", "", "");
//!     v.button("Click me", true, "go").on_click(|| println!("clicked"));
//! });
//! let app = App::new(Config::default().title("Demo"));
//! app.load_view(&view);
//! app.run();
//! ```

pub use affineui_sys as sys;

mod app;
mod components;
mod document;
mod event;
pub mod embedded;
mod util;
mod view;
mod virtual_list;

pub use app::{App, Config};
pub use components::{
    Button, Checkbox, ColorField, DockPanel, Dropdown, Foldout, Slider, TextField, Validity,
};
pub use document::{DispatchResult, Document, DocumentScript};
pub use event::{Color, Event, EventType, Key, MouseButton};
pub use view::{Theme, View, Widget, WidgetKind};
pub use virtual_list::{
    Axis, IndexSelection, SelectMod, TreeFlattener, TreeSource, VirtualListProvider,
    VirtualTreeProvider, INDEX_NONE,
};

/// The AffineUI core version string (e.g. `"0.0.1"`).
pub fn version() -> String {
    util::ensure_abi();
    let ptr = unsafe { sys::affineui_version() };
    if ptr.is_null() {
        return String::new();
    }
    unsafe { std::ffi::CStr::from_ptr(ptr) }.to_string_lossy().into_owned()
}

/// Convenience: open a native window showing `html` and run to completion.
pub fn run_html(html: &str) -> i32 {
    util::ensure_abi();
    let html = util::cstring(html);
    unsafe { sys::affineui_run_html(html.as_ptr()) }
}
