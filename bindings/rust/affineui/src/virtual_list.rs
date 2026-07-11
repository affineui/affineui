//! Recycling virtual lists & trees — safe wrappers over the provider C ABI.
//!
//! Providers are STATELESS bridges of callbacks between your model and the
//! recycling widget (see `include/affineui/virtual_list.h` for the design).
//! Keep the provider alive as long as any view references it: the widget
//! holds a weak reference and degrades to an empty list if the provider is
//! dropped first (never a crash).

use std::ffi::{c_char, c_int, c_void, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};

use crate::sys;
use crate::util::{cstring, ensure_abi, NotThreadSafe};
use crate::view::View;

/// Mirrors `affineui::SelectMod`: the modifier intent of a row activation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SelectMod {
    Replace = 0,
    Toggle = 1,
    Range = 2,
}

impl SelectMod {
    fn from_raw(v: c_int) -> SelectMod {
        match v {
            1 => SelectMod::Toggle,
            2 => SelectMod::Range,
            _ => SelectMod::Replace,
        }
    }
}

/// Mirrors `affineui::Axis`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Axis {
    Vertical = 0,
    Horizontal = 1,
}

// ── Trampolines (panics never unwind across the FFI) ─────────────────

macro_rules! suppress_panic {
    ($what:literal, $body:expr) => {
        if catch_unwind(AssertUnwindSafe(|| $body)).is_err() {
            eprintln!(concat!("affineui: panic in ", $what, " callback (suppressed)"));
        }
    };
}

unsafe extern "C" fn notify_trampoline(user: *mut c_void) {
    let cb = &mut *(user as *mut Box<dyn FnMut()>);
    suppress_panic!("notify", cb());
}
unsafe extern "C" fn notify_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut()>));
}

unsafe extern "C" fn count_trampoline(user: *mut c_void) -> usize {
    let cb = &mut *(user as *mut Box<dyn FnMut() -> usize>);
    catch_unwind(AssertUnwindSafe(|| cb())).unwrap_or(0)
}
unsafe extern "C" fn count_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut() -> usize>));
}

unsafe extern "C" fn size_trampoline(user: *mut c_void, index: usize) -> f64 {
    let cb = &mut *(user as *mut Box<dyn FnMut(usize) -> f64>);
    catch_unwind(AssertUnwindSafe(|| cb(index))).unwrap_or(0.0)
}
unsafe extern "C" fn size_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut(usize) -> f64>));
}

// Text callbacks return a pointer that must stay valid until the call
// returns; the env keeps the last CString alive between calls.
struct TextEnv {
    f: Box<dyn FnMut(usize) -> String>,
    scratch: CString,
}
unsafe extern "C" fn text_trampoline(user: *mut c_void, index: usize) -> *const c_char {
    let env = &mut *(user as *mut TextEnv);
    let text = catch_unwind(AssertUnwindSafe(|| (env.f)(index))).unwrap_or_default();
    env.scratch = CString::new(text).unwrap_or_default();
    env.scratch.as_ptr()
}
unsafe extern "C" fn text_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut TextEnv));
}

unsafe extern "C" fn flag_trampoline(user: *mut c_void, index: usize) -> c_int {
    let cb = &mut *(user as *mut Box<dyn FnMut(usize) -> bool>);
    (catch_unwind(AssertUnwindSafe(|| cb(index))).unwrap_or(false)) as c_int
}
unsafe extern "C" fn flag_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut(usize) -> bool>));
}

unsafe extern "C" fn activate_trampoline(user: *mut c_void, index: usize, select_mod: c_int) {
    let cb = &mut *(user as *mut Box<dyn FnMut(usize, SelectMod)>);
    suppress_panic!("on_activate", cb(index, SelectMod::from_raw(select_mod)));
}
unsafe extern "C" fn activate_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut(usize, SelectMod)>));
}

unsafe extern "C" fn toggle_trampoline(user: *mut c_void, index: usize) {
    let cb = &mut *(user as *mut Box<dyn FnMut(usize)>);
    suppress_panic!("on_toggle", cb(index));
}
unsafe extern "C" fn toggle_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut(usize)>));
}

unsafe extern "C" fn checked_trampoline(user: *mut c_void, index: usize, checked: c_int) {
    let cb = &mut *(user as *mut Box<dyn FnMut(usize, bool)>);
    suppress_panic!("on_set_checked", cb(index, checked != 0));
}
unsafe extern "C" fn checked_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut(usize, bool)>));
}

unsafe extern "C" fn depth_trampoline(user: *mut c_void, index: usize) -> c_int {
    let cb = &mut *(user as *mut Box<dyn FnMut(usize) -> i32>);
    catch_unwind(AssertUnwindSafe(|| cb(index))).unwrap_or(0)
}
unsafe extern "C" fn depth_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut(usize) -> i32>));
}

unsafe extern "C" fn item_build_trampoline(
    user: *mut c_void,
    view: *mut sys::affineui_view,
    index: usize,
) {
    let cb = &mut *(user as *mut Box<dyn FnMut(&View, usize)>);
    let view = View::borrowed(view);
    suppress_panic!("on_build_item", cb(&view, index));
}
unsafe extern "C" fn item_build_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn FnMut(&View, usize)>));
}

fn into_user<T>(value: T) -> *mut c_void {
    Box::into_raw(Box::new(value)) as *mut c_void
}

// ── IndexSelection ───────────────────────────────────────────────────

/// Replace / Ctrl-toggle / Shift-range selection with an INDEX anchor.
/// Suits flat lists whose indices are stable identities.
pub struct IndexSelection {
    raw: *mut sys::affineui_index_selection,
    _not_send: NotThreadSafe,
}

impl IndexSelection {
    pub fn new() -> IndexSelection {
        ensure_abi();
        IndexSelection {
            raw: unsafe { sys::affineui_index_selection_create() },
            _not_send: NotThreadSafe::default(),
        }
    }

    pub(crate) fn raw(&self) -> *mut sys::affineui_index_selection {
        self.raw
    }

    pub fn apply(&self, index: usize, mode: SelectMod, item_count: usize) {
        unsafe {
            sys::affineui_index_selection_apply(self.raw, index, mode as c_int, item_count)
        };
    }
    pub fn contains(&self, index: usize) -> bool {
        unsafe { sys::affineui_index_selection_contains(self.raw, index) != 0 }
    }
    pub fn clear(&self) {
        unsafe { sys::affineui_index_selection_clear(self.raw) };
    }
    pub fn len(&self) -> usize {
        unsafe { sys::affineui_index_selection_size(self.raw) }
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn anchor(&self) -> usize {
        unsafe { sys::affineui_index_selection_anchor(self.raw) }
    }
    pub fn on_change(&self, f: impl FnMut() + 'static) {
        let boxed: Box<dyn FnMut()> = Box::new(f);
        unsafe {
            sys::affineui_index_selection_on_change(
                self.raw,
                Some(notify_trampoline),
                into_user(boxed),
                Some(notify_free),
            )
        };
    }
}

impl Default for IndexSelection {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for IndexSelection {
    fn drop(&mut self) {
        unsafe { sys::affineui_index_selection_destroy(self.raw) };
    }
}

// ── Providers ────────────────────────────────────────────────────────

/// A stateless bridge of callbacks for a recycling virtual LIST.
pub struct VirtualListProvider {
    raw: *mut sys::affineui_vlist_provider,
    _not_send: NotThreadSafe,
}

impl VirtualListProvider {
    pub fn new() -> VirtualListProvider {
        ensure_abi();
        VirtualListProvider {
            raw: unsafe { sys::affineui_vlist_provider_create() },
            _not_send: NotThreadSafe::default(),
        }
    }

    pub(crate) fn raw(&self) -> *mut sys::affineui_vlist_provider {
        self.raw
    }

    pub fn on_item_count(&self, f: impl FnMut() -> usize + 'static) -> &Self {
        let boxed: Box<dyn FnMut() -> usize> = Box::new(f);
        unsafe {
            sys::affineui_vlist_provider_on_item_count(
                self.raw, Some(count_trampoline), into_user(boxed), Some(count_free))
        };
        self
    }
    pub fn on_item_size(&self, f: impl FnMut(usize) -> f64 + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize) -> f64> = Box::new(f);
        unsafe {
            sys::affineui_vlist_provider_on_item_size(
                self.raw, Some(size_trampoline), into_user(boxed), Some(size_free))
        };
        self
    }
    pub fn on_item_text(&self, f: impl FnMut(usize) -> String + 'static) -> &Self {
        let env = TextEnv { f: Box::new(f), scratch: CString::default() };
        unsafe {
            sys::affineui_vlist_provider_on_item_text(
                self.raw, Some(text_trampoline), into_user(env), Some(text_free))
        };
        self
    }
    pub fn on_build_item(&self, f: impl FnMut(&View, usize) + 'static) -> &Self {
        let boxed: Box<dyn FnMut(&View, usize)> = Box::new(f);
        unsafe {
            sys::affineui_vlist_provider_on_build_item(
                self.raw, Some(item_build_trampoline), into_user(boxed),
                Some(item_build_free))
        };
        self
    }
    pub fn on_is_selected(&self, f: impl FnMut(usize) -> bool + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize) -> bool> = Box::new(f);
        unsafe {
            sys::affineui_vlist_provider_on_is_selected(
                self.raw, Some(flag_trampoline), into_user(boxed), Some(flag_free))
        };
        self
    }
    pub fn on_activate(&self, f: impl FnMut(usize, SelectMod) + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize, SelectMod)> = Box::new(f);
        unsafe {
            sys::affineui_vlist_provider_on_activate(
                self.raw, Some(activate_trampoline), into_user(boxed),
                Some(activate_free))
        };
        self
    }
    pub fn on_is_checked(&self, f: impl FnMut(usize) -> bool + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize) -> bool> = Box::new(f);
        unsafe {
            sys::affineui_vlist_provider_on_is_checked(
                self.raw, Some(flag_trampoline), into_user(boxed), Some(flag_free))
        };
        self
    }
    pub fn on_set_checked(&self, f: impl FnMut(usize, bool) + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize, bool)> = Box::new(f);
        unsafe {
            sys::affineui_vlist_provider_on_set_checked(
                self.raw, Some(checked_trampoline), into_user(boxed),
                Some(checked_free))
        };
        self
    }
    /// Show the per-row checkbox column (checked is a SECOND row state,
    /// independent of selection).
    pub fn checkboxes(&self, on: bool) -> &Self {
        unsafe { sys::affineui_vlist_provider_set_checkboxes(self.raw, on as c_int) };
        self
    }
    pub fn default_item_size(&self, px: f64) -> &Self {
        unsafe { sys::affineui_vlist_provider_set_default_item_size(self.raw, px) };
        self
    }
}

impl Default for VirtualListProvider {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for VirtualListProvider {
    fn drop(&mut self) {
        unsafe { sys::affineui_vlist_provider_destroy(self.raw) };
    }
}

/// A stateless bridge of callbacks for a recycling virtual TREE — a virtual
/// list over the flattened, currently-expanded rows.
pub struct VirtualTreeProvider {
    raw: *mut sys::affineui_vtree_provider,
    _not_send: NotThreadSafe,
}

impl VirtualTreeProvider {
    pub fn new() -> VirtualTreeProvider {
        ensure_abi();
        VirtualTreeProvider {
            raw: unsafe { sys::affineui_vtree_provider_create() },
            _not_send: NotThreadSafe::default(),
        }
    }

    pub(crate) fn raw(&self) -> *mut sys::affineui_vtree_provider {
        self.raw
    }

    pub fn on_item_count(&self, f: impl FnMut() -> usize + 'static) -> &Self {
        let boxed: Box<dyn FnMut() -> usize> = Box::new(f);
        unsafe {
            sys::affineui_vtree_provider_on_item_count(
                self.raw, Some(count_trampoline), into_user(boxed), Some(count_free))
        };
        self
    }
    pub fn on_item_size(&self, f: impl FnMut(usize) -> f64 + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize) -> f64> = Box::new(f);
        unsafe {
            sys::affineui_vtree_provider_on_item_size(
                self.raw, Some(size_trampoline), into_user(boxed), Some(size_free))
        };
        self
    }
    pub fn on_item_text(&self, f: impl FnMut(usize) -> String + 'static) -> &Self {
        let env = TextEnv { f: Box::new(f), scratch: CString::default() };
        unsafe {
            sys::affineui_vtree_provider_on_item_text(
                self.raw, Some(text_trampoline), into_user(env), Some(text_free))
        };
        self
    }
    pub fn on_build_item(&self, f: impl FnMut(&View, usize) + 'static) -> &Self {
        let boxed: Box<dyn FnMut(&View, usize)> = Box::new(f);
        unsafe {
            sys::affineui_vtree_provider_on_build_item(
                self.raw, Some(item_build_trampoline), into_user(boxed),
                Some(item_build_free))
        };
        self
    }
    pub fn on_is_selected(&self, f: impl FnMut(usize) -> bool + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize) -> bool> = Box::new(f);
        unsafe {
            sys::affineui_vtree_provider_on_is_selected(
                self.raw, Some(flag_trampoline), into_user(boxed), Some(flag_free))
        };
        self
    }
    pub fn on_activate(&self, f: impl FnMut(usize, SelectMod) + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize, SelectMod)> = Box::new(f);
        unsafe {
            sys::affineui_vtree_provider_on_activate(
                self.raw, Some(activate_trampoline), into_user(boxed),
                Some(activate_free))
        };
        self
    }
    pub fn on_is_checked(&self, f: impl FnMut(usize) -> bool + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize) -> bool> = Box::new(f);
        unsafe {
            sys::affineui_vtree_provider_on_is_checked(
                self.raw, Some(flag_trampoline), into_user(boxed), Some(flag_free))
        };
        self
    }
    pub fn on_set_checked(&self, f: impl FnMut(usize, bool) + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize, bool)> = Box::new(f);
        unsafe {
            sys::affineui_vtree_provider_on_set_checked(
                self.raw, Some(checked_trampoline), into_user(boxed),
                Some(checked_free))
        };
        self
    }
    pub fn on_depth(&self, f: impl FnMut(usize) -> i32 + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize) -> i32> = Box::new(f);
        unsafe {
            sys::affineui_vtree_provider_on_depth(
                self.raw, Some(depth_trampoline), into_user(boxed), Some(depth_free))
        };
        self
    }
    pub fn on_is_expandable(&self, f: impl FnMut(usize) -> bool + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize) -> bool> = Box::new(f);
        unsafe {
            sys::affineui_vtree_provider_on_is_expandable(
                self.raw, Some(flag_trampoline), into_user(boxed), Some(flag_free))
        };
        self
    }
    pub fn on_is_expanded(&self, f: impl FnMut(usize) -> bool + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize) -> bool> = Box::new(f);
        unsafe {
            sys::affineui_vtree_provider_on_is_expanded(
                self.raw, Some(flag_trampoline), into_user(boxed), Some(flag_free))
        };
        self
    }
    pub fn on_toggle(&self, f: impl FnMut(usize) + 'static) -> &Self {
        let boxed: Box<dyn FnMut(usize)> = Box::new(f);
        unsafe {
            sys::affineui_vtree_provider_on_toggle(
                self.raw, Some(toggle_trampoline), into_user(boxed), Some(toggle_free))
        };
        self
    }
    pub fn checkboxes(&self, on: bool) -> &Self {
        unsafe { sys::affineui_vtree_provider_set_checkboxes(self.raw, on as c_int) };
        self
    }
    pub fn default_item_size(&self, px: f64) -> &Self {
        unsafe { sys::affineui_vtree_provider_set_default_item_size(self.raw, px) };
        self
    }
}

impl Default for VirtualTreeProvider {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for VirtualTreeProvider {
    fn drop(&mut self) {
        unsafe { sys::affineui_vtree_provider_destroy(self.raw) };
    }
}

// ── Tree flattener ───────────────────────────────────────────────────

/// Callbacks describing an app tree as opaque `u64` HANDLES. A handle must
/// be UNIQUE TO THE ITEM for its lifetime (id, stable pointer, map key) —
/// never recycled onto another item, and never 0 (reserved for "roots").
pub struct TreeSource {
    /// Children of `parent` (0 = roots), in order.
    pub children: Box<dyn FnMut(u64, &mut Vec<u64>)>,
    /// Row label for a handle.
    pub label: Box<dyn FnMut(u64) -> String>,
    /// Whether the handle can expand.
    pub has_children: Box<dyn FnMut(u64) -> bool>,
}

struct TreeSourceEnv {
    source: TreeSource,
    scratch: CString,
}

unsafe extern "C" fn tree_children_trampoline(
    user: *mut c_void,
    parent: u64,
    emit: sys::affineui_tree_emit_fn,
    ctx: *mut c_void,
) {
    let env = &mut *(user as *mut TreeSourceEnv);
    let mut out = Vec::new();
    suppress_panic!("tree children", (env.source.children)(parent, &mut out));
    if let Some(emit) = emit {
        for child in out {
            emit(ctx, child);
        }
    }
}
unsafe extern "C" fn tree_label_trampoline(user: *mut c_void, handle: u64) -> *const c_char {
    let env = &mut *(user as *mut TreeSourceEnv);
    let text = catch_unwind(AssertUnwindSafe(|| (env.source.label)(handle)))
        .unwrap_or_default();
    env.scratch = CString::new(text).unwrap_or_default();
    env.scratch.as_ptr()
}
unsafe extern "C" fn tree_flag_trampoline(user: *mut c_void, handle: u64) -> c_int {
    let env = &mut *(user as *mut TreeSourceEnv);
    (catch_unwind(AssertUnwindSafe(|| (env.source.has_children)(handle)))
        .unwrap_or(false)) as c_int
}
unsafe extern "C" fn tree_source_free(user: *mut c_void) {
    drop(Box::from_raw(user as *mut TreeSourceEnv));
}

/// Flattens a handle-tree into the visible rows and owns expanded /
/// HANDLE-keyed selection / HANDLE-keyed checked state — all of which
/// survive expand/collapse renumbering. Wire it to a [`VirtualTreeProvider`].
pub struct TreeFlattener {
    raw: *mut sys::affineui_tree_flattener,
    _not_send: NotThreadSafe,
}

pub const INDEX_NONE: usize = usize::MAX;

impl TreeFlattener {
    pub fn new(source: TreeSource) -> TreeFlattener {
        ensure_abi();
        let env = TreeSourceEnv { source, scratch: CString::default() };
        let raw = unsafe {
            sys::affineui_tree_flattener_create(
                Some(tree_children_trampoline),
                Some(tree_label_trampoline),
                Some(tree_flag_trampoline),
                into_user(env),
                Some(tree_source_free),
            )
        };
        TreeFlattener { raw, _not_send: NotThreadSafe::default() }
    }

    /// Point `provider` at this flattener (call once, after creating both).
    pub fn wire(&self, provider: &VirtualTreeProvider) {
        unsafe { sys::affineui_tree_flattener_wire(self.raw, provider.raw()) };
    }
    /// Re-flatten after the underlying tree STRUCTURE changed.
    pub fn rebuild(&self) {
        unsafe { sys::affineui_tree_flattener_rebuild(self.raw) };
    }
    pub fn on_changed(&self, f: impl FnMut() + 'static) {
        let boxed: Box<dyn FnMut()> = Box::new(f);
        unsafe {
            sys::affineui_tree_flattener_on_changed(
                self.raw, Some(notify_trampoline), into_user(boxed), Some(notify_free))
        };
    }
    pub fn set_expanded(&self, handle: u64, open: bool) {
        unsafe { sys::affineui_tree_flattener_set_expanded(self.raw, handle, open as c_int) };
    }
    pub fn is_expanded(&self, handle: u64) -> bool {
        unsafe { sys::affineui_tree_flattener_is_expanded(self.raw, handle) != 0 }
    }
    pub fn set_selected(&self, handle: u64, on: bool) {
        unsafe { sys::affineui_tree_flattener_set_selected(self.raw, handle, on as c_int) };
    }
    pub fn selected_contains(&self, handle: u64) -> bool {
        unsafe { sys::affineui_tree_flattener_selected_contains(self.raw, handle) != 0 }
    }
    pub fn clear_selection(&self) {
        unsafe { sys::affineui_tree_flattener_clear_selection(self.raw) };
    }
    pub fn set_checked(&self, handle: u64, on: bool) {
        unsafe { sys::affineui_tree_flattener_set_checked(self.raw, handle, on as c_int) };
    }
    pub fn checked_contains(&self, handle: u64) -> bool {
        unsafe { sys::affineui_tree_flattener_checked_contains(self.raw, handle) != 0 }
    }
    pub fn len(&self) -> usize {
        unsafe { sys::affineui_tree_flattener_size(self.raw) }
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn handle_at(&self, index: usize) -> u64 {
        unsafe { sys::affineui_tree_flattener_handle_at(self.raw, index) }
    }
    /// The current flattened index of `handle`, or [`INDEX_NONE`] when it is
    /// not visible.
    pub fn index_of(&self, handle: u64) -> usize {
        unsafe { sys::affineui_tree_flattener_index_of(self.raw, handle) }
    }
}

impl Drop for TreeFlattener {
    fn drop(&mut self) {
        unsafe { sys::affineui_tree_flattener_destroy(self.raw) };
    }
}

// ── View builders ────────────────────────────────────────────────────

impl View {
    /// A recycling virtual list driven by `provider`. The provider is held
    /// weakly by the widget — keep it alive alongside the view.
    pub fn virtual_list(
        &self,
        key: &str,
        provider: &VirtualListProvider,
        axis: Axis,
        classes: &str,
    ) -> crate::view::Widget {
        let (key, classes) = (cstring(key), cstring(classes));
        let raw = unsafe {
            sys::affineui_view_virtual_list(
                self.raw(), key.as_ptr(), provider.raw(), axis as c_int, classes.as_ptr())
        };
        self.wrap_widget(raw)
    }

    /// A recycling virtual tree driven by `provider`.
    pub fn virtual_tree(
        &self,
        key: &str,
        provider: &VirtualTreeProvider,
        classes: &str,
    ) -> crate::view::Widget {
        let (key, classes) = (cstring(key), cstring(classes));
        let raw = unsafe {
            sys::affineui_view_virtual_tree(
                self.raw(), key.as_ptr(), provider.raw(), classes.as_ptr())
        };
        self.wrap_widget(raw)
    }

    /// Convenience: an ALL-VIRTUAL list of strings. `selection` / `checked`
    /// must outlive the view.
    pub fn virtual_string_list(
        &self,
        key: &str,
        items: &[&str],
        item_size: f64,
        selection: Option<&IndexSelection>,
        checked: Option<&IndexSelection>,
        classes: &str,
    ) -> crate::view::Widget {
        let key = cstring(key);
        let classes = cstring(classes);
        let owned: Vec<CString> = items.iter().map(|s| cstring(s)).collect();
        let ptrs: Vec<*const c_char> = owned.iter().map(|c| c.as_ptr()).collect();
        let raw = unsafe {
            sys::affineui_view_virtual_string_list(
                self.raw(),
                key.as_ptr(),
                ptrs.as_ptr(),
                ptrs.len(),
                item_size,
                selection.map_or(std::ptr::null_mut(), |s| s.raw()),
                checked.map_or(std::ptr::null_mut(), |s| s.raw()),
                classes.as_ptr(),
            )
        };
        self.wrap_widget(raw)
    }
}
