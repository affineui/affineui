#pragma once

// Shared helpers for the extern "C" surface (src/c_api.cpp,
// src/c_api_app.cpp): string duplication, event conversion, and the
// callback user-data holder that guarantees the (fn, user, user_free)
// contract — user_free runs exactly once, when the core drops its last
// reference to the handler.

#include "affineui/c_api.h"
#include "affineui/types.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>

namespace affineui_c {

inline std::string_view sv(const char* s) {
    return s ? std::string_view{s} : std::string_view{};
}

// Heap copy for out-strings; freed by affineui_string_free (std::free).
inline char* dup_string(std::string_view s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out == nullptr) return nullptr;
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

// Holds a binding-side closure's user pointer. The destructor releases it
// through user_free; shared_ptr copies inside std::function keep it alive
// until the core drops the last handler reference.
struct UserData {
    void*                 user{nullptr};
    affineui_user_free_fn free_fn{nullptr};

    UserData(void* u, affineui_user_free_fn f) noexcept : user(u), free_fn(f) {}
    ~UserData() {
        if (free_fn) free_fn(user);
    }
    UserData(const UserData&)            = delete;
    UserData& operator=(const UserData&) = delete;
};

inline std::shared_ptr<UserData> hold_user(void* user, affineui_user_free_fn f) {
    return std::make_shared<UserData>(user, f);
}

inline affineui::Event to_event(const affineui_event& ev) {
    affineui::Event out{};
    out.type     = static_cast<affineui::EventType>(ev.type);
    out.pos      = affineui::Point{ev.x, ev.y};
    out.button   = static_cast<affineui::MouseButton>(ev.button);
    out.wheel_dx = ev.wheel_dx;
    out.wheel_dy = ev.wheel_dy;
    out.key      = static_cast<affineui::Key>(ev.key);
    out.key_code = ev.key_code;
    if (ev.text) out.text = ev.text;
    out.shift = ev.shift != 0;
    out.ctrl  = ev.ctrl != 0;
    out.alt   = ev.alt != 0;
    out.super = ev.super_key != 0;
    return out;
}

// ── ABI locks: the C enum values ARE the C++ enum values ─────────────
static_assert(AFFINEUI_EVENT_NONE == static_cast<int>(affineui::EventType::None));
static_assert(AFFINEUI_EVENT_MOUSE_MOVE == static_cast<int>(affineui::EventType::MouseMove));
static_assert(AFFINEUI_EVENT_MOUSE_WHEEL == static_cast<int>(affineui::EventType::MouseWheel));
static_assert(AFFINEUI_EVENT_TEXT_INPUT == static_cast<int>(affineui::EventType::TextInput));
static_assert(AFFINEUI_EVENT_FOCUS_GAINED == static_cast<int>(affineui::EventType::FocusGained));
static_assert(AFFINEUI_MOUSE_LEFT == static_cast<int>(affineui::MouseButton::Left));
static_assert(AFFINEUI_MOUSE_MIDDLE == static_cast<int>(affineui::MouseButton::Middle));
static_assert(AFFINEUI_KEY_UNKNOWN == static_cast<int>(affineui::Key::Unknown));
static_assert(AFFINEUI_KEY_END == static_cast<int>(affineui::Key::End));
static_assert(AFFINEUI_KEY_A == static_cast<int>(affineui::Key::A));
static_assert(AFFINEUI_KEY_Z == static_cast<int>(affineui::Key::Z));
static_assert(AFFINEUI_KEY_DIGIT0 == static_cast<int>(affineui::Key::Digit0));
static_assert(AFFINEUI_KEY_DIGIT9 == static_cast<int>(affineui::Key::Digit9));
static_assert(AFFINEUI_KEY_SPACE == static_cast<int>(affineui::Key::Space));
static_assert(AFFINEUI_KEY_MINUS == static_cast<int>(affineui::Key::Minus));
static_assert(AFFINEUI_KEY_EQUAL == static_cast<int>(affineui::Key::Equal));
static_assert(AFFINEUI_KEY_BRACKET_LEFT ==
              static_cast<int>(affineui::Key::BracketLeft));
static_assert(AFFINEUI_KEY_BRACKET_RIGHT ==
              static_cast<int>(affineui::Key::BracketRight));

}  // namespace affineui_c
