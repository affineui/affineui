//! Boundary helpers: string conversion, the ABI version gate, and the
//! marker type that makes every handle `!Send + !Sync`.

use crate::sys;
use std::ffi::{CStr, CString};
use std::marker::PhantomData;
use std::os::raw::c_char;

/// Embedded in every handle type: raw pointers are `!Send + !Sync`, which
/// encodes the single-thread contract at compile time.
pub(crate) type NotThreadSafe = PhantomData<*const ()>;

/// UTF-8 → C string. Interior NULs cannot cross the ABI, so they are
/// stripped (degrade, don't panic — the binding-level expression of the
/// hard-to-crash contract).
pub(crate) fn cstring(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|_| {
        CString::new(s.replace('\0', "")).expect("NUL-free after strip")
    })
}

/// Take ownership of a `char*` returned by the C ABI ("caller frees").
pub(crate) unsafe fn take_string(ptr: *mut c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let out = CStr::from_ptr(ptr).to_string_lossy().into_owned();
    sys::affineui_string_free(ptr);
    out
}

/// Fail fast (once) if the loaded affineui_c library speaks a different
/// ABI version than this crate was written against.
pub(crate) fn ensure_abi() {
    use std::sync::Once;
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let got = unsafe { sys::affineui_c_abi_version() };
        assert_eq!(
            got,
            sys::AFFINEUI_C_ABI_VERSION,
            "AffineUI C ABI mismatch: affineui_c reports version {got}, this \
             crate expects {}. Rebuild the native library or update the crate.",
            sys::AFFINEUI_C_ABI_VERSION
        );
    });
}
