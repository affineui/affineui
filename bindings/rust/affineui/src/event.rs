//! Value types shared by both modes: colors, events, keys.

use crate::sys;
use crate::util::cstring;
use std::ffi::CStr;

/// RGBA color, 8 bits per channel.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Color {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

impl Color {
    pub const fn rgb(r: u8, g: u8, b: u8) -> Color {
        Color { r, g, b, a: 255 }
    }
    pub const fn rgba(r: u8, g: u8, b: u8, a: u8) -> Color {
        Color { r, g, b, a }
    }
}

impl Default for Color {
    fn default() -> Color {
        Color::rgba(0, 0, 0, 255)
    }
}

/// Event kind (mirrors `affineui::EventType`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum EventType {
    #[default]
    None = 0,
    MouseMove = 1,
    MouseDown = 2,
    MouseUp = 3,
    MouseWheel = 4,
    KeyDown = 5,
    KeyUp = 6,
    TextInput = 7,
    Resize = 8,
    FocusLost = 9,
    FocusGained = 10,
    /// IME preedit update: `text` carries the uncommitted composition
    /// string (empty = composition ended), shown inline at the caret but
    /// never written to the control's value. Committed text still
    /// arrives as [`EventType::TextInput`].
    Composition = 11,
}

/// Mouse button (mirrors `affineui::MouseButton`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum MouseButton {
    #[default]
    Left = 0,
    Right = 1,
    Middle = 2,
}

/// Portable non-text key code (mirrors `affineui::Key`). Printable text
/// arrives as [`EventType::TextInput`], not as key codes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum Key {
    #[default]
    Unknown = 0,
    Escape = 1,
    Tab = 2,
    Enter = 3,
    Backspace = 4,
    Delete = 5,
    ArrowLeft = 6,
    ArrowRight = 7,
    ArrowUp = 8,
    ArrowDown = 9,
    Home = 10,
    End = 11,
    A = 12, B = 13, C = 14, D = 15, E = 16, F = 17, G = 18, H = 19, I = 20,
    J = 21, K = 22, L = 23, M = 24, N = 25, O = 26, P = 27, Q = 28, R = 29,
    S = 30, T = 31, U = 32, V = 33, W = 34, X = 35, Y = 36, Z = 37,
    Digit0 = 38, Digit1 = 39, Digit2 = 40, Digit3 = 41, Digit4 = 42,
    Digit5 = 43, Digit6 = 44, Digit7 = 45, Digit8 = 46, Digit9 = 47,
    Space = 48, Minus = 49, Equal = 50, BracketLeft = 51, BracketRight = 52,
}

/// Input event, for driving AffineUI from a host loop, tests, or the
/// embedded mode's forwarded input.
#[derive(Clone, Debug, Default)]
pub struct Event {
    pub kind: EventType,
    pub x: i32,
    pub y: i32,
    pub button: MouseButton,
    pub wheel_dx: f32,
    pub wheel_dy: f32,
    pub key: Key,
    /// Platform-native scancode (debug / passthrough).
    pub key_code: i32,
    /// UTF-8 text, read for [`EventType::TextInput`] (committed) and
    /// [`EventType::Composition`] (preedit).
    pub text: String,
    /// Composition only — IME caret as a byte offset into `text`
    /// (negative = end of preedit).
    pub composition_cursor: i32,
    /// Composition only — the IME's active clause, byte offsets into
    /// `text` (equal = none reported).
    pub composition_clause_begin: i32,
    pub composition_clause_end: i32,
    pub shift: bool,
    pub ctrl: bool,
    pub alt: bool,
    /// Command on macOS, Windows/Super elsewhere.
    pub super_key: bool,
}

impl Event {
    /// Copy an event borrowed from a synchronous native callback.
    pub(crate) unsafe fn from_sys(ev: &sys::affineui_event) -> Event {
        Event {
            kind: std::mem::transmute::<i32, EventType>(ev.r#type),
            x: ev.x,
            y: ev.y,
            button: std::mem::transmute::<i32, MouseButton>(ev.button),
            wheel_dx: ev.wheel_dx,
            wheel_dy: ev.wheel_dy,
            key: std::mem::transmute::<i32, Key>(ev.key),
            key_code: ev.key_code,
            text: if ev.text.is_null() {
                String::new()
            } else {
                CStr::from_ptr(ev.text).to_string_lossy().into_owned()
            },
            composition_cursor: ev.composition_cursor,
            composition_clause_begin: ev.composition_clause_begin,
            composition_clause_end: ev.composition_clause_end,
            shift: ev.shift != 0,
            ctrl: ev.ctrl != 0,
            alt: ev.alt != 0,
            super_key: ev.super_key != 0,
        }
    }

    pub fn mouse_move(x: i32, y: i32) -> Event {
        Event { kind: EventType::MouseMove, x, y, ..Event::default() }
    }
    pub fn mouse_down(x: i32, y: i32, button: MouseButton) -> Event {
        Event { kind: EventType::MouseDown, x, y, button, ..Event::default() }
    }
    pub fn mouse_up(x: i32, y: i32, button: MouseButton) -> Event {
        Event { kind: EventType::MouseUp, x, y, button, ..Event::default() }
    }
    pub fn mouse_wheel(x: i32, y: i32, dx: f32, dy: f32) -> Event {
        Event {
            kind: EventType::MouseWheel,
            x,
            y,
            wheel_dx: dx,
            wheel_dy: dy,
            ..Event::default()
        }
    }
    pub fn key_down(key: Key) -> Event {
        Event { kind: EventType::KeyDown, key, ..Event::default() }
    }
    pub fn key_up(key: Key) -> Event {
        Event { kind: EventType::KeyUp, key, ..Event::default() }
    }
    pub fn text_input(text: &str) -> Event {
        Event { kind: EventType::TextInput, text: text.to_owned(), ..Event::default() }
    }
    /// IME preedit update; `cursor` is a byte offset into `preedit`
    /// (pass a negative value for "end of preedit").
    pub fn composition(preedit: &str, cursor: i32) -> Event {
        Event {
            kind: EventType::Composition,
            text: preedit.to_owned(),
            composition_cursor: cursor,
            ..Event::default()
        }
    }

    /// Build the C event (with a live text CString) and hand it to `f`.
    pub(crate) fn with_sys<R>(&self, f: impl FnOnce(&sys::affineui_event) -> R) -> R {
        let text = cstring(&self.text);
        let ev = sys::affineui_event {
            r#type: self.kind as i32,
            x: self.x,
            y: self.y,
            button: self.button as i32,
            wheel_dx: self.wheel_dx,
            wheel_dy: self.wheel_dy,
            key: self.key as i32,
            key_code: self.key_code,
            text: if self.text.is_empty() { std::ptr::null() } else { text.as_ptr() },
            shift: self.shift as i32,
            ctrl: self.ctrl as i32,
            alt: self.alt as i32,
            super_key: self.super_key as i32,
            composition_cursor: self.composition_cursor,
            composition_clause_begin: self.composition_clause_begin,
            composition_clause_end: self.composition_clause_end,
        };
        f(&ev)
    }
}
