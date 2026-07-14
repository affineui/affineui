//! Declarative docking — the DCC-style workspace: a center document pane with
//! dockable panels around it, which the user can drag, tab, split, and tear off.
//!
//! A dock container resolves a FLAT set of panel declarations into a split-tree
//! layout and emits the DOM, inserting splitters itself. Panels may be declared
//! in **any order** — each carries a [`DockLocation`] naming its parent and side
//! — so the result is deterministic.
//!
//! ```no_run
//! use affineui::{App, Config, Dock, DockLocation, Theme, View};
//!
//! let view = View::new(Theme::Decius);
//! view.build(|v| {
//!     v.document_view("workspace", |v| {
//!         let doc = v.document("Scene", "cube", |v| {
//!             v.heading(1, "Viewport", "", "");
//!         });
//!
//!         v.dockpanel("Outliner", DockLocation::docked(Dock::Left).sized(280),
//!                     "list", "outliner", |v| {
//!             v.heading(2, "Objects", "", "");
//!         });
//!
//!         // Tabbed INTO the outliner, by the id it handed back.
//!         let _ = doc;
//!     });
//! });
//! ```
//!
//! The declared layout is a **seed**. Wire the providers on [`View`] (see
//! `set_dock_layout_from_document`) and a saved — or user-rearranged —
//! arrangement wins over it, so drag-to-dock and tearoff survive a rebuild.

use affineui_sys as sys;

/// Which side of its parent a panel docks to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Dock {
    Left,
    Right,
    Top,
    Bottom,
    /// Another tab of the parent's pane, rather than a new split.
    Tab,
}

impl Dock {
    fn raw(self) -> i32 {
        match self {
            Dock::Left => sys::AFFINEUI_DOCK_LEFT,
            Dock::Right => sys::AFFINEUI_DOCK_RIGHT,
            Dock::Top => sys::AFFINEUI_DOCK_TOP,
            Dock::Bottom => sys::AFFINEUI_DOCK_BOTTOM,
            Dock::Tab => sys::AFFINEUI_DOCK_TAB,
        }
    }

    fn from_raw(v: i32) -> Dock {
        match v {
            x if x == sys::AFFINEUI_DOCK_RIGHT => Dock::Right,
            x if x == sys::AFFINEUI_DOCK_TOP => Dock::Top,
            x if x == sys::AFFINEUI_DOCK_BOTTOM => Dock::Bottom,
            x if x == sys::AFFINEUI_DOCK_TAB => Dock::Tab,
            _ => Dock::Left,
        }
    }
}

/// A panel's window state.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum DockState {
    #[default]
    Docked,
    /// Floating in its own window.
    Detached,
    /// Torn off, and draggable back into the dock.
    Tearoff,
}

impl DockState {
    fn raw(self) -> i32 {
        match self {
            DockState::Docked => sys::AFFINEUI_DOCK_DOCKED,
            DockState::Detached => sys::AFFINEUI_DOCK_DETACHED,
            DockState::Tearoff => sys::AFFINEUI_DOCK_TEAROFF,
        }
    }
}

/// The corner of the parent a floating panel is anchored to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DockCorner {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
}

impl DockCorner {
    fn raw(self) -> i32 {
        match self {
            DockCorner::TopLeft => sys::AFFINEUI_DOCK_CORNER_TOP_LEFT,
            DockCorner::TopRight => sys::AFFINEUI_DOCK_CORNER_TOP_RIGHT,
            DockCorner::BottomLeft => sys::AFFINEUI_DOCK_CORNER_BOTTOM_LEFT,
            DockCorner::BottomRight => sys::AFFINEUI_DOCK_CORNER_BOTTOM_RIGHT,
        }
    }
}

/// Everything about where a panel is (or starts).
///
/// Build one with a factory ([`docked`](DockLocation::docked), [`tab`](DockLocation::tab),
/// [`floating`](DockLocation::floating), [`tearoff`](DockLocation::tearoff)) and refine it
/// with the chainable setters — the same shape as the C++ and Python APIs.
///
/// Every field is genuinely optional here. The C ABI cannot express that (C has
/// no `Option`), so it uses a flat struct with `has_*` flags; this type carries
/// real `Option`s and is marshalled down at the call. You never see a flag.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct DockLocation {
    /// Docked side, or `Tab` to join the parent's tab strip.
    pub side: Option<Dock>,
    /// Parent pane id — what `document()` / `dockpanel()` returned.
    /// `None` = the document pane.
    pub parent: Option<String>,
    pub state: DockState,
    /// px flex-basis when docked.
    pub size: Option<i32>,
    /// Corner a float is anchored to.
    pub anchor: Option<DockCorner>,
    /// Float position relative to the anchor.
    pub offset: Option<(i32, i32)>,
    /// Float size.
    pub float_size: Option<(i32, i32)>,
    /// Tearoff: id of the panel this one drags with.
    pub drag_with: Option<String>,
}

impl DockLocation {
    /// Docked to a side of its parent.
    pub fn docked(side: Dock) -> Self {
        Self { side: Some(side), ..Default::default() }
    }

    /// Another tab of the parent's pane.
    pub fn tab() -> Self {
        Self { side: Some(Dock::Tab), ..Default::default() }
    }

    /// Floating, anchored to a corner of the parent.
    pub fn floating(anchor: DockCorner, pos: (i32, i32), size: (i32, i32)) -> Self {
        Self {
            state: DockState::Detached,
            anchor: Some(anchor),
            offset: Some(pos),
            float_size: Some(size),
            ..Default::default()
        }
    }

    /// Like [`floating`](Self::floating), but draggable back into the dock.
    pub fn tearoff(anchor: DockCorner, pos: (i32, i32), size: (i32, i32)) -> Self {
        Self { state: DockState::Tearoff, ..Self::floating(anchor, pos, size) }
    }

    /// Parent this panel to another pane (by the id `document()`/`dockpanel()` returned).
    pub fn in_pane(mut self, parent_pane_id: impl Into<String>) -> Self {
        self.parent = Some(parent_pane_id.into());
        self
    }

    /// px flex-basis when docked.
    pub fn sized(mut self, px: i32) -> Self {
        self.size = Some(px);
        self
    }

    /// The size a torn-off panel takes when it detaches.
    pub fn tearout_size(mut self, size: (i32, i32)) -> Self {
        self.float_size = Some(size);
        self
    }

    /// Tearoff: drag this panel along with another.
    pub fn dragging_with(mut self, panel_id: impl Into<String>) -> Self {
        self.drag_with = Some(panel_id.into());
        self
    }
}

/// Marshal into the C form. The `CString`s must outlive the call, so they are
/// returned alongside the struct and dropped by the caller afterwards.
pub(crate) struct RawDockLocation {
    pub(crate) raw: sys::affineui_dock_location,
    _parent: Option<std::ffi::CString>,
    _drag_with: Option<std::ffi::CString>,
}

impl DockLocation {
    pub(crate) fn to_raw(&self) -> RawDockLocation {
        let parent = self
            .parent
            .as_deref()
            .and_then(|s| std::ffi::CString::new(s).ok());
        let drag_with = self
            .drag_with
            .as_deref()
            .and_then(|s| std::ffi::CString::new(s).ok());

        let mut raw = sys::affineui_dock_location {
            state: self.state.raw(),
            parent: parent.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            drag_with: drag_with.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            ..Default::default()
        };

        if let Some(side) = self.side {
            raw.has_side = 1;
            raw.side = side.raw();
        }
        if let Some(size) = self.size {
            raw.has_size = 1;
            raw.size = size;
        }
        if let Some(anchor) = self.anchor {
            raw.has_anchor = 1;
            raw.anchor = anchor.raw();
        }
        if let Some((x, y)) = self.offset {
            raw.has_offset = 1;
            raw.offset_x = x;
            raw.offset_y = y;
        }
        if let Some((w, h)) = self.float_size {
            raw.has_float_size = 1;
            raw.float_w = w;
            raw.float_h = h;
        }

        RawDockLocation { raw, _parent: parent, _drag_with: drag_with }
    }
}

/// A runtime placement override — where a panel ended up after the user dragged,
/// tabbed, or tore it off. Read these back with [`Document::dock_overrides`] to
/// save a workspace, and feed them to `View::set_dock_placement_provider` to
/// restore one.
///
/// [`Document::dock_overrides`]: crate::Document::dock_overrides
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DockPlacement {
    /// Torn off into a floating panel.
    pub floating: bool,
    /// Docked: target pane id (empty = the document pane).
    pub parent: String,
    /// Docked: which side.
    pub side: Dock,
    /// Docked: px flex-basis (0 = default).
    pub size: i32,
    /// Floating: rect in float-host px.
    pub rect: (i32, i32, i32, i32),
}

impl DockPlacement {
    pub(crate) fn from_raw(raw: &sys::affineui_dock_placement) -> Option<DockPlacement> {
        if raw.present == 0 {
            return None;
        }
        let parent = if raw.parent.is_null() {
            String::new()
        } else {
            unsafe { std::ffi::CStr::from_ptr(raw.parent) }
                .to_string_lossy()
                .into_owned()
        };
        Some(DockPlacement {
            floating: raw.floating != 0,
            parent,
            side: Dock::from_raw(raw.side),
            size: raw.size,
            rect: (raw.x, raw.y, raw.w, raw.h),
        })
    }

    pub(crate) fn write_raw(&self, out: *mut sys::affineui_dock_placement, parent: &std::ffi::CStr) {
        // SAFETY: `out` is the engine's stack slot, valid for this call.
        unsafe {
            (*out).present = 1;
            (*out).floating = self.floating as i32;
            (*out).parent = parent.as_ptr();
            (*out).side = self.side.raw();
            (*out).size = self.size;
            (*out).x = self.rect.0;
            (*out).y = self.rect.1;
            (*out).w = self.rect.2;
            (*out).h = self.rect.3;
        }
    }
}
