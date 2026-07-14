//! Application menus (mirrors `affineui/menu.h`).
//!
//! A platform-neutral menu *model*: on macOS it becomes the real system menu
//! bar (which an app cannot draw itself), elsewhere it drives the in-window
//! bar. The app declares its menus once and hands them to [`App::set_menu`].
//!
//! The shape follows Electron's `Menu.buildFromTemplate`, as the C++ model
//! does:
//!
//! ```no_run
//! use affineui::{App, Config, Menu, MenuItem, MenuRole};
//!
//! let app = App::new(Config::default());
//! app.set_menu(
//!     Menu::new()
//!         .sub("File", Menu::new()
//!             .item("New Scene", "CmdOrCtrl+N", || println!("new"))
//!             .separator()
//!             .role(MenuRole::Quit))          // standard, auto-labelled
//!         .sub("Edit", Menu::edit_menu())     // Undo/Cut/Copy/Paste...
//!         .sub("View", Menu::new()
//!             .with(MenuItem::check("Grid", true, "CmdOrCtrl+G", || {}))),
//! );
//! ```
//!
//! Two things carry the weight: a [`MenuRole`] is a standard item whose label,
//! accelerator and behavior the platform supplies (this is how the macOS
//! application menu and a working Edit menu come out right without the app
//! restating them per platform), and an *accelerator* is an Electron-style
//! chord string (`"CmdOrCtrl+S"`) whose `CmdOrCtrl` resolves to Command on
//! macOS and Control elsewhere — so an app declares a shortcut once.
//!
//! [`App::set_menu`]: crate::App::set_menu

use crate::event::Color;
use crate::sys;
use crate::util::cstring;
use std::os::raw::{c_int, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};

// ── Callback trampolines ─────────────────────────────────────────────
// Same contract as `Widget::on_click`: the closure is boxed, the core owns
// the box, and `user_free` releases it exactly once. The C ABI holds the user
// data in a shared cell, so the closure outlives the builder handle we destroy
// right after `affineui_app_set_menu`.

unsafe extern "C" fn select_trampoline(user: *mut c_void) {
    let cb = &mut *(user as *mut Box<dyn FnMut()>);
    if catch_unwind(AssertUnwindSafe(&mut *cb)).is_err() {
        eprintln!("affineui: panic in menu on_select callback (suppressed)");
    }
}

unsafe extern "C" fn select_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut()>));
}

/// Standard items whose label/accelerator/behavior the platform supplies.
/// A role item needs no label and no callback.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MenuRole {
    None,
    // Application menu (macOS). Quit and About are universal; Services/Hide*
    // are macOS-only and are dropped from a drawn menu rather than shown dead.
    About,
    Services,
    Hide,
    HideOthers,
    Unhide,
    Preferences,
    Quit,
    // Edit. On macOS these get the standard AppKit selectors, so they act on
    // whatever control has focus (native text fields included) for free.
    Undo,
    Redo,
    Cut,
    Copy,
    Paste,
    SelectAll,
    // Window.
    Minimize,
    Zoom,
    Close,
    ToggleFullscreen,
}

impl MenuRole {
    fn to_raw(self) -> c_int {
        match self {
            MenuRole::None => sys::AFFINEUI_MENU_ROLE_NONE,
            MenuRole::About => sys::AFFINEUI_MENU_ROLE_ABOUT,
            MenuRole::Services => sys::AFFINEUI_MENU_ROLE_SERVICES,
            MenuRole::Hide => sys::AFFINEUI_MENU_ROLE_HIDE,
            MenuRole::HideOthers => sys::AFFINEUI_MENU_ROLE_HIDE_OTHERS,
            MenuRole::Unhide => sys::AFFINEUI_MENU_ROLE_UNHIDE,
            MenuRole::Preferences => sys::AFFINEUI_MENU_ROLE_PREFERENCES,
            MenuRole::Quit => sys::AFFINEUI_MENU_ROLE_QUIT,
            MenuRole::Undo => sys::AFFINEUI_MENU_ROLE_UNDO,
            MenuRole::Redo => sys::AFFINEUI_MENU_ROLE_REDO,
            MenuRole::Cut => sys::AFFINEUI_MENU_ROLE_CUT,
            MenuRole::Copy => sys::AFFINEUI_MENU_ROLE_COPY,
            MenuRole::Paste => sys::AFFINEUI_MENU_ROLE_PASTE,
            MenuRole::SelectAll => sys::AFFINEUI_MENU_ROLE_SELECT_ALL,
            MenuRole::Minimize => sys::AFFINEUI_MENU_ROLE_MINIMIZE,
            MenuRole::Zoom => sys::AFFINEUI_MENU_ROLE_ZOOM,
            MenuRole::Close => sys::AFFINEUI_MENU_ROLE_CLOSE,
            MenuRole::ToggleFullscreen => sys::AFFINEUI_MENU_ROLE_TOGGLE_FULLSCREEN,
        }
    }
}

/// What a row *is*. Kept private: the [`MenuItem`] constructors are the API,
/// exactly as in the C++ model.
enum ItemKind {
    Normal,
    Separator,
    /// A check mark, initially on/off.
    Check(bool),
    Submenu(Menu),
    Role(MenuRole),
}

/// One row of a [`Menu`]. Build with the constructors ([`MenuItem::item`],
/// [`MenuItem::separator`], [`MenuItem::sub`], [`MenuItem::role`],
/// [`MenuItem::check`]) and decorate with the chained
/// [`swatch`](MenuItem::swatch) / [`enabled`](MenuItem::enabled).
pub struct MenuItem {
    kind: ItemKind,
    label: String,
    /// Electron-style chord: "CmdOrCtrl+S", "Shift+Alt+F". Empty for none.
    accelerator: String,
    enabled: bool,
    /// Only sent across when set: the C builder leaves items un-swatched.
    swatch: Option<Color>,
    on_select: Option<Box<dyn FnMut()>>,
}

impl MenuItem {
    /// A plain row. `accelerator` may be `""`.
    pub fn item(label: &str, accelerator: &str, on_select: impl FnMut() + 'static) -> MenuItem {
        MenuItem {
            kind: ItemKind::Normal,
            label: label.to_owned(),
            accelerator: accelerator.to_owned(),
            enabled: true,
            swatch: None,
            on_select: Some(Box::new(on_select)),
        }
    }

    pub fn separator() -> MenuItem {
        MenuItem::bare(ItemKind::Separator)
    }

    pub fn sub(label: &str, items: Menu) -> MenuItem {
        MenuItem { label: label.to_owned(), ..MenuItem::bare(ItemKind::Submenu(items)) }
    }

    /// A standard platform item; label and accelerator are the platform's.
    pub fn role(role: MenuRole) -> MenuItem {
        MenuItem::bare(ItemKind::Role(role))
    }

    /// A standard platform item with the label overridden. The accelerator and
    /// the behavior still come from the platform — only the wording is yours.
    pub fn role_labeled(role: MenuRole, label: &str) -> MenuItem {
        MenuItem { label: label.to_owned(), ..MenuItem::bare(ItemKind::Role(role)) }
    }

    /// A row with a check mark, initially `checked`. A menu that shows state is
    /// expected to be rebuilt and re-set as that state changes.
    pub fn check(
        label: &str,
        checked: bool,
        accelerator: &str,
        on_select: impl FnMut() + 'static,
    ) -> MenuItem {
        MenuItem { kind: ItemKind::Check(checked), ..MenuItem::item(label, accelerator, on_select) }
    }

    /// A solid color chip in the item's leading gutter — accent pickers, layer
    /// colors. Survives into a native menu as a real image.
    pub fn swatch(mut self, color: Color) -> MenuItem {
        self.swatch = Some(color);
        self
    }

    /// Grey the row out (enabled by default).
    pub fn enabled(mut self, on: bool) -> MenuItem {
        self.enabled = on;
        self
    }

    fn bare(kind: ItemKind) -> MenuItem {
        MenuItem {
            kind,
            label: String::new(),
            accelerator: String::new(),
            enabled: true,
            swatch: None,
            on_select: None,
        }
    }

    /// Append this item to a raw C builder handle. Consumes the item: the
    /// boxed `on_select` is handed to the core, which frees it via `user_free`.
    unsafe fn build_into(self, raw: *mut sys::affineui_menu) {
        let (label, accel) = (cstring(&self.label), cstring(&self.accelerator));
        // NULL user data when there is no closure, so the C side records no
        // callback at all rather than one that does nothing.
        let (cb, user, free): (sys::affineui_menu_select_fn, *mut c_void, sys::affineui_user_free_fn) =
            match self.on_select {
                Some(f) => {
                    let boxed: Box<dyn FnMut()> = f;
                    (
                        Some(select_trampoline),
                        Box::into_raw(Box::new(boxed)) as *mut c_void,
                        Some(select_free),
                    )
                }
                None => (None, std::ptr::null_mut(), None),
            };

        match self.kind {
            ItemKind::Normal => {
                sys::affineui_menu_add_item(raw, label.as_ptr(), accel.as_ptr(), cb, user, free);
            }
            ItemKind::Check(checked) => {
                sys::affineui_menu_add_check(
                    raw,
                    label.as_ptr(),
                    checked as c_int,
                    accel.as_ptr(),
                    cb,
                    user,
                    free,
                );
            }
            ItemKind::Separator => sys::affineui_menu_add_separator(raw),
            ItemKind::Role(role) => {
                sys::affineui_menu_add_role(raw, role.to_raw());
                // A role takes the platform's label unless the caller overrode
                // it (MenuItem::role_labeled); set_label decorates the item just
                // added, so it has to follow the add.
                if !self.label.is_empty() {
                    sys::affineui_menu_set_label(raw, label.as_ptr());
                }
            }
            ItemKind::Submenu(items) => {
                // The returned handle is owned by `raw`; never destroyed here.
                let sub = sys::affineui_menu_add_submenu(raw, label.as_ptr());
                if !sub.is_null() {
                    items.build_into(sub);
                }
            }
        }

        // Both decorate the LAST item added, so they follow the add_* call.
        if let Some(c) = self.swatch {
            sys::affineui_menu_set_swatch(
                raw,
                sys::affineui_color { r: c.r, g: c.g, b: c.b, a: c.a },
            );
        }
        if !self.enabled {
            sys::affineui_menu_set_enabled(raw, 0);
        }
    }
}

/// A menu: its items, top to bottom. Passed to [`App::set_menu`](crate::App::set_menu)
/// it is a menu *bar*, and its items are the bar's menus left to right — on
/// macOS the first one is the application menu and takes the app's name whatever
/// its label says (a platform rule, not a choice).
///
/// The chained methods are the common case; [`Menu::with`] takes a decorated
/// [`MenuItem`].
#[derive(Default)]
pub struct Menu {
    items: Vec<MenuItem>,
}

impl Menu {
    pub fn new() -> Menu {
        Menu { items: Vec::new() }
    }

    /// Append a prebuilt (and possibly decorated) item.
    pub fn with(mut self, item: MenuItem) -> Menu {
        self.items.push(item);
        self
    }

    /// Append a plain row. `accelerator` may be `""`.
    pub fn item(self, label: &str, accelerator: &str, on_select: impl FnMut() + 'static) -> Menu {
        self.with(MenuItem::item(label, accelerator, on_select))
    }

    pub fn separator(self) -> Menu {
        self.with(MenuItem::separator())
    }

    pub fn sub(self, label: &str, items: Menu) -> Menu {
        self.with(MenuItem::sub(label, items))
    }

    pub fn role(self, role: MenuRole) -> Menu {
        self.with(MenuItem::role(role))
    }

    pub fn check(
        self,
        label: &str,
        checked: bool,
        accelerator: &str,
        on_select: impl FnMut() + 'static,
    ) -> Menu {
        self.with(MenuItem::check(label, checked, accelerator, on_select))
    }

    /// Append every item of `other` — how a standard group ([`Menu::edit_menu`])
    /// is spliced into a menu that has more of its own.
    pub fn extend(mut self, other: Menu) -> Menu {
        self.items.extend(other.items);
        self
    }

    /// The conventional Edit menu. On macOS these carry the AppKit selectors,
    /// so they operate on the focused control without app wiring.
    pub fn edit_menu() -> Menu {
        Menu::new()
            .role(MenuRole::Undo)
            .role(MenuRole::Redo)
            .separator()
            .role(MenuRole::Cut)
            .role(MenuRole::Copy)
            .role(MenuRole::Paste)
            .role(MenuRole::SelectAll)
    }

    /// The conventional Window menu.
    pub fn window_menu() -> Menu {
        Menu::new()
            .role(MenuRole::Minimize)
            .role(MenuRole::Zoom)
            .separator()
            .role(MenuRole::Close)
    }

    pub fn len(&self) -> usize {
        self.items.len()
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    /// Fill a raw C builder handle with this menu's items, depth first.
    unsafe fn build_into(self, raw: *mut sys::affineui_menu) {
        for item in self.items {
            item.build_into(raw);
        }
    }

    /// Build the C-side tree and hand it to `f` (which copies it), then destroy
    /// the builder. The closures survive: the C ABI holds each item's user data
    /// in a shared cell that the copied menu keeps alive.
    pub(crate) unsafe fn with_raw<R>(self, f: impl FnOnce(*const sys::affineui_menu) -> R) -> R {
        let raw = sys::affineui_menu_create();
        self.build_into(raw);
        let out = f(raw);
        sys::affineui_menu_destroy(raw);
        out
    }
}

impl FromIterator<MenuItem> for Menu {
    fn from_iter<I: IntoIterator<Item = MenuItem>>(iter: I) -> Menu {
        Menu { items: iter.into_iter().collect() }
    }
}
