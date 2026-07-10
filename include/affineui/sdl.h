#pragma once

// SDL2 adapter — opt-in.
//
// Include via the umbrella header with the macro defined:
//
//     #define AFFINEUI_WITH_SDL
//     #include <affineui/affineui.h>
//
// Or include this header directly *after* SDL is included:
//
//     #include <SDL.h>
//     #include <affineui/sdl.h>
//
// What's in here:
//   • affineui::sdl::dispatch(Ui&, const SDL_Event&)
//       Translate an SDL event into affineui::Event, route it through
//       the Ui's hit-test + click-handler pipeline, and apply the OS
//       cursor via SDL_SetCursor. Returns true if the UI consumed
//       the event so your game can skip its own handling.
//
//   • affineui::sdl::render(Ui&, SDL_Window*)
//       Render the UI into the current GL framebuffer. Queries
//       drawable size + DPI from the SDL window. Call from inside
//       your render pass, with the right GL context current.
//
// Threading: same as Ui — same thread that owns the GL context.
//
// Assumed graphics stack: OpenGL 3 (via SDL_GL_CreateContext). Native
// Metal / D3D11 / Vulkan SDL paths need additional Renderer backends
// (Phase 3+) — not supported today.

#include "affineui/ui.h"

#include <SDL.h>

namespace affineui::sdl {

// ── Cursor mapping ──────────────────────────────────────────────────

inline SDL_SystemCursor cursor_to_sdl(int c) {
    switch (c) {
        case 1: return SDL_SYSTEM_CURSOR_HAND;
        case 2: return SDL_SYSTEM_CURSOR_IBEAM;
        case 3: return SDL_SYSTEM_CURSOR_CROSSHAIR;
        case 4: return SDL_SYSTEM_CURSOR_SIZEALL;
        case 5: return SDL_SYSTEM_CURSOR_NO;
        case 6: return SDL_SYSTEM_CURSOR_SIZEWE;
        case 7: return SDL_SYSTEM_CURSOR_SIZENS;
        case 8: return SDL_SYSTEM_CURSOR_SIZENWSE;
        case 9: return SDL_SYSTEM_CURSOR_SIZENESW;
        default: return SDL_SYSTEM_CURSOR_ARROW;
    }
}

namespace detail {
// Cache of system cursors. SDL_CreateSystemCursor allocates an SDL_Cursor*
// per kind; we lazily fill the slots so we only pay for the cursors we
// actually use. Pointers live for process lifetime.
inline SDL_Cursor* get_cached_cursor(int c) {
    static SDL_Cursor* cache[10] = {nullptr};
    const int idx = (c < 0 || c > 9) ? 0 : c;
    if (!cache[idx]) {
        cache[idx] = SDL_CreateSystemCursor(cursor_to_sdl(c));
    }
    return cache[idx];
}
}  // namespace detail

// ── Event translation + dispatch ────────────────────────────────────

// Map SDL keycodes to our platform-independent Key enum. Only the
// keys AffineUI dispatches on are listed; everything else is
// Key::Unknown and arrives at Document::dispatch with the raw
// SDLK_* value in Event::key_code for callers that want it.
inline Key key_to_affine(SDL_Keycode sym) {
    switch (sym) {
        case SDLK_ESCAPE:    return Key::Escape;
        case SDLK_TAB:       return Key::Tab;
        case SDLK_RETURN:    return Key::Enter;
        case SDLK_KP_ENTER:  return Key::Enter;
        case SDLK_BACKSPACE: return Key::Backspace;
        case SDLK_DELETE:    return Key::Delete;
        case SDLK_LEFT:      return Key::ArrowLeft;
        case SDLK_RIGHT:     return Key::ArrowRight;
        case SDLK_UP:        return Key::ArrowUp;
        case SDLK_DOWN:      return Key::ArrowDown;
        case SDLK_HOME:      return Key::Home;
        case SDLK_END:       return Key::End;
        case SDLK_SPACE:        return Key::Space;
        case SDLK_MINUS:        return Key::Minus;
        case SDLK_EQUALS:       return Key::Equal;
        case SDLK_LEFTBRACKET:  return Key::BracketLeft;
        case SDLK_RIGHTBRACKET: return Key::BracketRight;
        default: break;
    }
    // SDLK letters (a–z) and digits (0–9) are contiguous ASCII ranges, as are
    // the matching Key enum members; map by offset rather than per-key cases.
    if (sym >= SDLK_a && sym <= SDLK_z) {
        return static_cast<Key>(static_cast<int>(Key::A) + (sym - SDLK_a));
    }
    if (sym >= SDLK_0 && sym <= SDLK_9) {
        return static_cast<Key>(static_cast<int>(Key::Digit0) + (sym - SDLK_0));
    }
    return Key::Unknown;
}

inline void apply_key_modifiers(Event& out, SDL_Keymod mods) {
    out.shift = (mods & KMOD_SHIFT) != 0;
    out.ctrl  = (mods & KMOD_CTRL) != 0;
    out.alt   = (mods & KMOD_ALT) != 0;
    out.super = (mods & KMOD_GUI) != 0;
}

inline Event translate(const SDL_Event& ev) {
    Event out{};
    switch (ev.type) {
        case SDL_MOUSEMOTION: {
            out.type  = EventType::MouseMove;
            out.pos.x = ev.motion.x;   // SDL gives mouse coords in points
            out.pos.y = ev.motion.y;
            return out;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            out.type = (ev.type == SDL_MOUSEBUTTONDOWN)
                           ? EventType::MouseDown
                           : EventType::MouseUp;
            out.pos.x = ev.button.x;
            out.pos.y = ev.button.y;
            switch (ev.button.button) {
                case SDL_BUTTON_LEFT:   out.button = MouseButton::Left;   break;
                case SDL_BUTTON_RIGHT:  out.button = MouseButton::Right;  break;
                case SDL_BUTTON_MIDDLE: out.button = MouseButton::Middle; break;
                default: break;
            }
            return out;
        }
        case SDL_MOUSEWHEEL: {
            out.type     = EventType::MouseWheel;
            out.wheel_dx = static_cast<float>(ev.wheel.x);
            out.wheel_dy = static_cast<float>(ev.wheel.y);
            // SDL_MOUSEWHEEL doesn't carry the cursor position; use
            // the current mouse state so the wheel routes through the
            // hover chain Document::dispatch tracks.
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            out.pos = Point{mx, my};
            return out;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            out.type = (ev.type == SDL_KEYDOWN) ? EventType::KeyDown
                                                : EventType::KeyUp;
            out.key_code = static_cast<int>(ev.key.keysym.sym);
            out.key      = key_to_affine(ev.key.keysym.sym);
            apply_key_modifiers(out,
                                static_cast<SDL_Keymod>(ev.key.keysym.mod));
            return out;
        }
        case SDL_TEXTINPUT:
            out.type = EventType::TextInput;
            out.text = ev.text.text;
            return out;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_LEAVE) {
                out.type = EventType::MouseMove;
                out.pos  = Point{-1, -1};
                return out;
            }
            return out;
        default:
            return out;
    }
}

/// Forward an SDL event to the Ui's dispatch pipeline. Applies the OS
/// cursor synchronously inside the mouse-event handler (the same
/// constraint Cocoa enforces — SDL on macOS is just a thin wrapper
/// around Cocoa). Returns true when the UI consumed the event.
inline bool dispatch(Ui& ui, const SDL_Event& ev) {
    const auto e = translate(ev);
    if (e.type == EventType::None) return false;
    const bool consumed = ui.dispatch(e);
    if (e.type == EventType::MouseMove) {
        SDL_SetCursor(detail::get_cached_cursor(ui.hovered_cursor()));
    }
    return consumed;
}

// ── Frame ───────────────────────────────────────────────────────────

/// Render the UI into the current GL framebuffer. Pulls drawable
/// size from SDL — `SDL_GL_GetDrawableSize` gives pixels, while
/// `SDL_GetWindowSize` gives points; the ratio is the DPI scale.
inline void render(Ui& ui, SDL_Window* window) {
    if (!window) return;
    int win_w = 0, win_h = 0, fb_w = 0, fb_h = 0;
    SDL_GetWindowSize(window, &win_w, &win_h);
    SDL_GL_GetDrawableSize(window, &fb_w, &fb_h);
    const float dpi = (win_w > 0)
                          ? static_cast<float>(fb_w) / static_cast<float>(win_w)
                          : 1.0f;
    ui.render(fb_w, fb_h, dpi);
}

}  // namespace affineui::sdl
