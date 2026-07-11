#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace affineui {

struct Color {
    std::uint8_t r{0}, g{0}, b{0}, a{255};
    static constexpr Color rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        return {r, g, b, 255};
    }
    static constexpr Color rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
        return {r, g, b, a};
    }
};

struct Size {
    int width{0};
    int height{0};
};

struct Point {
    int x{0};
    int y{0};
};

struct Rect {
    int x{0}, y{0}, w{0}, h{0};
};

struct DomHandle {
    std::uint32_t document_id{0};
    std::uint32_t node_slot{0};
    std::uint32_t generation{0};

    explicit operator bool() const noexcept {
        return document_id != 0 && generation != 0;
    }
};

enum class MouseButton : std::uint8_t { Left, Right, Middle };

/// Platform-independent key code carried by KeyDown / KeyUp events.
/// Only the keys AffineUI actively dispatches on are enumerated;
/// printable characters arrive via EventType::TextInput. Adapters
/// translate from their platform's native code (SAPP_KEYCODE_* /
/// SDLK_*) into one of these. Unknown keys are reported as `Unknown`.
enum class Key : std::uint16_t {
    Unknown = 0,
    Escape,
    Tab,
    Enter,
    Backspace,
    Delete,
    ArrowLeft,
    ArrowRight,
    ArrowUp,
    ArrowDown,
    Home,
    End,
    // Letters A–Z. Editing keys (A/C/V/X) live here too; DCC-style apps bind
    // the full set for commands and tools (e.g. Ctrl Z undo, G/R/S transforms).
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    // Top-row digits 0–9.
    Digit0, Digit1, Digit2, Digit3, Digit4,
    Digit5, Digit6, Digit7, Digit8, Digit9,
    // Editing / navigation punctuation DCC tools bind (space-pan, layer
    // reorder brackets, zoom +/-). Appended so existing values are stable.
    Space, Minus, Equal, BracketLeft, BracketRight,
};

enum class EventType : std::uint8_t {
    None,
    MouseMove,
    MouseDown,
    MouseUp,
    MouseWheel,
    KeyDown,
    KeyUp,
    TextInput,
    Resize,
    FocusLost,
    FocusGained,
    // IME preedit update: `text` holds the uncommitted composition string
    // (empty = composition ended/cancelled), shown inline at the caret but
    // never written to the control's value. Committed text still arrives
    // as TextInput. Appended so C-ABI numeric values stay stable.
    Composition,
};

struct Event {
    EventType   type{EventType::None};
    Point       pos{};
    MouseButton button{MouseButton::Left};
    float       wheel_dx{0.0f};
    float       wheel_dy{0.0f};
    Key         key{Key::Unknown};
    int         key_code{0};  // platform-native scancode (debug / passthrough)
    std::string text;  // valid for TextInput (committed) / Composition (preedit)
    // Composition only — byte offsets into `text` (clamped to UTF-8
    // boundaries on dispatch). The clause range marks the IME's active
    // segment (the one being converted); begin==end means none reported.
    int         composition_cursor{0};
    int         composition_clause_begin{0};
    int         composition_clause_end{0};
    bool        shift{false};
    bool        ctrl{false};
    bool        alt{false};
    bool        super{false};  // Command on macOS, Windows/Super elsewhere.
};

/// Result of an event dispatch. If `redraw_requested` is true, the next
/// frame should repaint; if `invalidate_view` is true, the imm view fn
/// should be re-invoked before the next paint.
struct DispatchResult {
    bool redraw_requested{false};
    bool invalidate_view{false};
    // Set while a native behavior script is still in the middle of a live
    // gesture. App should keep DOM-local changes live but defer calling
    // declarative view callbacks until the gesture reaches its finisher.
    bool defer_widget_changes{false};
    // Set when an interaction changed the dock layout (e.g. a splitter drag
    // finished) so the app can persist the new arrangement.
    bool layout_changed{false};
};

/// Resource loader hook. Given a URL ("app:///main.css", "https://...",
/// "data:image/png;base64,..."), returns the raw bytes or empty on miss.
using ResourceLoader = std::function<std::string(std::string_view url)>;

}  // namespace affineui
