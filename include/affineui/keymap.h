#pragma once

// Keybindings for AffineUI.
//
// A keymap maps key *chords* (a Key plus modifier flags) to opaque action ids
// (strings the application assigns meaning to), in layers: a default layer the
// app ships and a user layer that overrides it. Resolution checks the user
// layer first so user rebindings win. This is general UI infrastructure — any
// app, not just editors, wires keyboard accelerators — so it lives in the core
// library alongside the Key/Event types it builds on.
//
//   affineui::Keymap km;
//   km.bind({affineui::Key::Z, /*ctrl*/true}, "edit.undo");
//   if (ev.type == affineui::EventType::KeyDown) {
//       std::string_view id = km.command_for(affineui::chord_from_event(ev));
//       if (!id.empty()) run(id);
//   }
//
// Header-only: small enough that there is nothing to compile separately, and it
// stays self-contained in the single-file distribution.

#include <string>
#include <string_view>
#include <vector>

#include "affineui/types.h"

namespace affineui {

/// A key plus modifier state. Aggregate-initialisable: `{Key::Z, true}` binds
/// Ctrl+Z (modifiers are, in order, ctrl/shift/alt/super).
struct Chord {
    Key  key{Key::Unknown};
    bool ctrl{false};
    bool shift{false};
    bool alt{false};
    bool super{false};

    [[nodiscard]] bool operator==(const Chord& o) const noexcept {
        return key == o.key && ctrl == o.ctrl && shift == o.shift &&
               alt == o.alt && super == o.super;
    }
};

/// Build a chord from a key event, so callers can look up straight from input.
[[nodiscard]] inline Chord chord_from_event(const Event& ev) noexcept {
    return Chord{ev.key, ev.ctrl, ev.shift, ev.alt, ev.super};
}

class Keymap {
public:
    enum class Layer { Default, User };

    /// Bind a chord to an action id in a layer. Re-binding the same chord in
    /// the same layer replaces it.
    void bind(Chord chord, std::string action_id, Layer layer = Layer::Default) {
        for (auto& b : bindings_) {
            if (b.layer == layer && b.chord == chord) {
                b.action_id = std::move(action_id);
                return;
            }
        }
        bindings_.push_back({chord, std::move(action_id), layer});
    }

    /// The action id bound to `chord` (user layer wins), or empty.
    [[nodiscard]] std::string_view command_for(const Chord& chord) const {
        if (const Binding* b = lookup(chord, Layer::User)) return b->action_id;
        if (const Binding* b = lookup(chord, Layer::Default)) return b->action_id;
        return {};
    }

    /// Human-readable accelerator text for an action ("Ctrl Z"), for menus.
    /// Uses the first chord bound to the id, user layer first.
    [[nodiscard]] std::string shortcut_text(std::string_view action_id) const {
        const Binding* b = first_for(action_id, Layer::User);
        if (!b) b = first_for(action_id, Layer::Default);
        if (!b) return {};

        std::string out;
        auto add = [&](std::string_view part) {
            if (!out.empty()) out += ' ';
            out += part;
        };
        if (b->chord.ctrl)  add("Ctrl");
        if (b->chord.shift) add("Shift");
        if (b->chord.alt)   add("Alt");
        if (b->chord.super) add("Super");
        const std::string name = key_name(b->chord.key);
        if (!name.empty()) add(name);
        return out;
    }

    [[nodiscard]] static std::string key_name(Key key) {
        switch (key) {
            case Key::Escape:     return "Esc";
            case Key::Tab:        return "Tab";
            case Key::Enter:      return "Enter";
            case Key::Backspace:  return "Backspace";
            case Key::Delete:     return "Del";
            case Key::ArrowLeft:  return "Left";
            case Key::ArrowRight: return "Right";
            case Key::ArrowUp:    return "Up";
            case Key::ArrowDown:  return "Down";
            case Key::Home:       return "Home";
            case Key::End:        return "End";
            case Key::Space:        return "Space";
            case Key::Minus:        return "-";
            case Key::Equal:        return "=";
            case Key::BracketLeft:  return "[";
            case Key::BracketRight: return "]";
            default: break;
        }
        const int k = static_cast<int>(key);
        if (k >= static_cast<int>(Key::A) && k <= static_cast<int>(Key::Z)) {
            return std::string(1, static_cast<char>('A' + (k - static_cast<int>(Key::A))));
        }
        if (k >= static_cast<int>(Key::Digit0) &&
            k <= static_cast<int>(Key::Digit9)) {
            return std::string(1, static_cast<char>('0' + (k - static_cast<int>(Key::Digit0))));
        }
        return {};
    }

private:
    struct Binding {
        Chord       chord;
        std::string action_id;
        Layer       layer;
    };
    [[nodiscard]] const Binding* lookup(const Chord& chord, Layer layer) const {
        for (const auto& b : bindings_) {
            if (b.layer == layer && b.chord == chord) return &b;
        }
        return nullptr;
    }
    [[nodiscard]] const Binding* first_for(std::string_view id,
                                           Layer layer) const {
        for (const auto& b : bindings_) {
            if (b.layer == layer && b.action_id == id) return &b;
        }
        return nullptr;
    }

    std::vector<Binding> bindings_;
};

}  // namespace affineui
