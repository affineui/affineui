// Accelerator parsing for the platform-neutral menu model.
//
// Kept out of the macOS shell on purpose: the drawn menus need the same
// parse (to render a shortcut column) and the tests need it without AppKit.

#include "affineui/menu.h"

#include <algorithm>
#include <cctype>

namespace affineui {
namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

// True when the token names a modifier, folding it into `accel`.
bool apply_modifier(std::string_view tok, Accelerator& accel) {
    if (iequals(tok, "CmdOrCtrl") || iequals(tok, "CommandOrControl")) {
#if defined(__APPLE__)
        accel.super = true;
#else
        accel.ctrl = true;
#endif
        return true;
    }
    if (iequals(tok, "Cmd") || iequals(tok, "Command") || iequals(tok, "Super") ||
        iequals(tok, "Meta")) {
        accel.super = true;
        return true;
    }
    if (iequals(tok, "Ctrl") || iequals(tok, "Control")) {
        accel.ctrl = true;
        return true;
    }
    if (iequals(tok, "Alt") || iequals(tok, "Option")) {
        accel.alt = true;
        return true;
    }
    if (iequals(tok, "Shift")) {
        accel.shift = true;
        return true;
    }
    return false;
}

std::string normalize_key(std::string_view tok) {
    // Single letters uppercase so "cmd+s" and "Cmd+S" agree; everything else
    // (F5, Enter, Left, "[") passes through as written.
    if (tok.size() == 1) {
        const auto c = static_cast<unsigned char>(tok[0]);
        return std::string(1, static_cast<char>(std::toupper(c)));
    }
    return std::string(tok);
}

}  // namespace

Accelerator parse_accelerator(std::string_view spec) {
    Accelerator accel;
    std::size_t start = 0;
    while (start <= spec.size()) {
        std::size_t plus = spec.find('+', start);
        // A trailing "+" is the key itself ("CmdOrCtrl++" zooms in), not a
        // separator — only treat it as one when something follows.
        if (plus == start && plus + 1 >= spec.size()) plus = std::string_view::npos;
        const std::string_view tok =
            spec.substr(start, plus == std::string_view::npos
                                   ? std::string_view::npos
                                   : plus - start);
        if (!tok.empty() && !apply_modifier(tok, accel)) {
            accel.key = normalize_key(tok);
        }
        if (plus == std::string_view::npos) break;
        start = plus + 1;
    }
    return accel;
}

std::string accelerator_text(const Accelerator& accel) {
    if (!accel.valid()) return {};
#if defined(__APPLE__)
    // macOS orders the glyphs Control, Option, Shift, Command and uses no
    // separator — that ordering is a platform convention, not a preference.
    std::string out;
    if (accel.ctrl) out += "⌃";   // ⌃
    if (accel.alt) out += "⌥";    // ⌥
    if (accel.shift) out += "⇧";  // ⇧
    if (accel.super) out += "⌘";  // ⌘
    out += accel.key;
    return out;
#else
    std::string out;
    const auto add = [&out](const char* s) {
        if (!out.empty()) out += "+";
        out += s;
    };
    if (accel.ctrl) add("Ctrl");
    if (accel.super) add("Super");
    if (accel.alt) add("Alt");
    if (accel.shift) add("Shift");
    if (!out.empty()) out += "+";
    out += accel.key;
    return out;
#endif
}

}  // namespace affineui
