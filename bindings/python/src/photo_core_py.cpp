// Photo document core for the Python Decius Photo Edit sample.
//
// This is the document/state model behind bindings/python/examples/photo_edit.
// It mirrors the web reference (decius-css/samples/decius-photo engine.js) at
// the *document-state* level:
//
//   - Layers are rendered by the app as CSS-styled divs, so a layer's pixel
//     content is approximated by a CSS `style` string (background gradients,
//     filter functions, box-shadows). Where the web performs a real pixel
//     operation with no CSS analogue the callers mark it as Phase-B work.
//   - History is HONEST: every snapshot captures the full document state
//     (layer stack incl. style strings, order, active layer, doc dimensions)
//     and undo/redo/select_history restore that state. The web stores full
//     pixel snapshots; restoring our style/metadata state produces the same
//     observable behavior for CSS-rendered layers.
//
// Hard-to-crash contract: invalid ids/indices/values no-op (returning false
// or defaults); nothing here throws for bad input.

#include "photo_core_py.h"

#include <pybind11/stl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace {

struct PhotoLayerSnapshot {
    std::string id;
    std::string name;
    std::string kind;  // "pixel" | "text" | "adjustment"
    bool visible = true;
    bool locked = false;
    double opacity = 100.0;
    double fill = 100.0;
    std::string blend = "Normal";
    std::string style = "background:transparent";
};

double clamp_percent(double value) {
    if (!std::isfinite(value)) return 100.0;
    return std::clamp(value, 0.0, 100.0);
}

// ── CSS style-string helpers ────────────────────────────────────────────────
// Layer "pixels" live in a CSS declaration string. These helpers let ops
// merge into it without producing duplicate declarations (a second `filter:`
// declaration would silently override the first).

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::vector<std::pair<std::string, std::string>> parse_declarations(
    const std::string& style) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t start = 0;
    int paren_depth = 0;
    for (std::size_t i = 0; i <= style.size(); ++i) {
        const char c = i < style.size() ? style[i] : ';';
        if (c == '(') ++paren_depth;
        if (c == ')') --paren_depth;
        if (c == ';' && paren_depth <= 0) {
            const std::string decl = trim(style.substr(start, i - start));
            start = i + 1;
            if (decl.empty()) continue;
            const auto colon = decl.find(':');
            if (colon == std::string::npos) continue;
            out.emplace_back(trim(decl.substr(0, colon)),
                             trim(decl.substr(colon + 1)));
        }
    }
    return out;
}

std::string serialize_declarations(
    const std::vector<std::pair<std::string, std::string>>& decls) {
    std::string out;
    for (const auto& [name, value] : decls) {
        if (name.empty() || value.empty()) continue;
        if (!out.empty()) out += ";";
        out += name + ":" + value;
    }
    return out;
}

// Returns the value of a declaration ("" if absent).
std::string declaration_value(const std::string& style,
                              const std::string& name) {
    for (const auto& [decl_name, value] : parse_declarations(style)) {
        if (decl_name == name) return value;
    }
    return {};
}

// Replaces (or appends) one declaration, preserving the others.
std::string with_declaration(const std::string& style, const std::string& name,
                             const std::string& value) {
    auto decls = parse_declarations(style);
    bool replaced = false;
    for (auto& [decl_name, decl_value] : decls) {
        if (decl_name == name) {
            decl_value = value;
            replaced = true;
        }
    }
    if (!replaced) decls.emplace_back(name, value);
    return serialize_declarations(decls);
}

// A plain color is not a valid <bg-layer> inside a multi-background list;
// wrap non-gradient values so backgrounds can be stacked when merging.
std::string as_background_layer(const std::string& value) {
    if (value.empty() || value == "transparent" || value == "none") return {};
    if (value.find("gradient") != std::string::npos ||
        value.find("url(") != std::string::npos) {
        return value;
    }
    return "linear-gradient(" + value + "," + value + ")";
}

// Parses "#rgb" / "#rrggbb" into components. Returns false on anything else.
bool parse_hex_color(const std::string& hex, int& r, int& g, int& b) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() == 4 && hex[0] == '#') {
        const int rn = nibble(hex[1]), gn = nibble(hex[2]), bn = nibble(hex[3]);
        if (rn < 0 || gn < 0 || bn < 0) return false;
        r = rn * 17;
        g = gn * 17;
        b = bn * 17;
        return true;
    }
    if (hex.size() == 7 && hex[0] == '#') {
        int v[6];
        for (int i = 0; i < 6; ++i) {
            v[i] = nibble(hex[1 + i]);
            if (v[i] < 0) return false;
        }
        r = v[0] * 16 + v[1];
        g = v[2] * 16 + v[3];
        b = v[4] * 16 + v[5];
        return true;
    }
    return false;
}

// ── Document ────────────────────────────────────────────────────────────────

class PhotoDocument {
public:
    explicit PhotoDocument(int width = 1280, int height = 800)
        : width_(std::max(1, width)), height_(std::max(1, height)) {
        reset_sample();
    }

    int width() const { return width_; }
    int height() const { return height_; }

    std::vector<PhotoLayerSnapshot> layers() const { return layers_; }
    int history_index() const { return history_index_; }
    std::string active_layer_id() const { return active_layer_id_; }

    std::vector<std::string> history() const {
        std::vector<std::string> labels;
        labels.reserve(history_.size());
        for (const auto& record : history_) labels.push_back(record.label);
        return labels;
    }

    // (label, icon) pairs, oldest first. Icons are decius `di-*` names.
    std::vector<std::pair<std::string, std::string>> history_entries() const {
        std::vector<std::pair<std::string, std::string>> entries;
        entries.reserve(history_.size());
        for (const auto& record : history_) {
            entries.emplace_back(record.label, record.icon);
        }
        return entries;
    }

    PhotoLayerSnapshot active_layer() const {
        const auto* layer = find_layer(active_layer_id_);
        if (layer != nullptr) return *layer;
        return layers_.empty() ? PhotoLayerSnapshot{} : layers_.back();
    }

    int active_index() const {
        for (std::size_t i = 0; i < layers_.size(); ++i) {
            if (layers_[i].id == active_layer_id_) return static_cast<int>(i);
        }
        return layers_.empty() ? -1 : static_cast<int>(layers_.size()) - 1;
    }

    bool set_active_layer(const std::string& id) {
        if (find_layer(id) == nullptr) return false;
        active_layer_id_ = id;
        return true;  // Selection is not a history state (matches the web).
    }

    // ── Layer stack ops ─────────────────────────────────────────────────────

    std::string add_layer(const std::string& name = "",
                          const std::string& kind = "pixel",
                          const std::string& style = "",
                          const std::string& blend = "Normal",
                          double opacity = 100.0,
                          const std::string& label = "New Layer",
                          const std::string& icon = "plus") {
        const int index = next_layer_index_++;
        PhotoLayerSnapshot layer;
        layer.id = make_unique_id("layer-" + std::to_string(index));
        layer.name = name.empty() ? "Layer " + std::to_string(index) : name;
        layer.kind = kind.empty() ? "pixel" : kind;
        layer.style = style.empty() ? "background:transparent" : style;
        layer.blend = blend.empty() ? "Normal" : blend;
        layer.opacity = clamp_percent(opacity);
        layers_.push_back(layer);  // New layers go to the top of the stack.
        active_layer_id_ = layer.id;
        snapshot(label.empty() ? "New Layer" : label,
                 icon.empty() ? "plus" : icon);
        return layer.id;
    }

    std::string duplicate_active_layer() {
        const int index = active_index();
        if (index < 0) return {};
        PhotoLayerSnapshot copy = layers_[index];
        copy.id = make_unique_id(copy.id + "-copy");
        copy.name += " copy";
        copy.locked = false;
        // The web inserts the duplicate directly above the source layer.
        layers_.insert(layers_.begin() + index + 1, copy);
        active_layer_id_ = copy.id;
        snapshot("Duplicate Layer", "duplicate");
        return copy.id;
    }

    bool delete_active_layer() {
        if (layers_.size() <= 1) return false;  // Never delete the last layer.
        const int index = active_index();
        if (index < 0 || layers_[index].locked) return false;
        layers_.erase(layers_.begin() + index);
        const int next = std::min(index, static_cast<int>(layers_.size()) - 1);
        active_layer_id_ = layers_[next].id;
        snapshot("Delete Layer", "trash");
        return true;
    }

    bool rename_layer(const std::string& id, const std::string& name) {
        auto* layer = find_layer(id);
        const std::string trimmed = trim(name);
        if (layer == nullptr || layer->locked || trimmed.empty()) return false;
        if (layer->name == trimmed) return true;
        layer->name = trimmed;
        snapshot("Rename Layer", "edit");
        return true;
    }

    // dir > 0 moves the active layer up (towards the top), dir < 0 down.
    bool move_active(int dir) {
        const int index = active_index();
        if (index < 0 || dir == 0 || layers_[index].locked) return false;
        const int target = index + (dir > 0 ? 1 : -1);
        if (target < 0 || target >= static_cast<int>(layers_.size())) {
            return false;
        }
        if (layers_[target].locked) return false;
        std::swap(layers_[index], layers_[target]);
        snapshot("Reorder Layers", dir > 0 ? "chevron-up" : "chevron-down");
        return true;
    }

    // Arbitrary reorder (drag-and-drop). Index is bottom-based; clamps.
    bool reorder_layer(const std::string& id, int new_index) {
        auto it = std::find_if(layers_.begin(), layers_.end(),
                               [&](const auto& layer) {
                                   return layer.id == id;
                               });
        if (it == layers_.end() || it->locked) return false;
        new_index = std::clamp(new_index, 0,
                               static_cast<int>(layers_.size()) - 1);
        const int old_index = static_cast<int>(it - layers_.begin());
        if (new_index == old_index) return false;
        PhotoLayerSnapshot layer = std::move(*it);
        layers_.erase(it);
        layers_.insert(layers_.begin() + new_index, std::move(layer));
        snapshot("Reorder Layers", "layers");
        return true;
    }

    // ── Layer property ops ──────────────────────────────────────────────────
    // Visibility/opacity/fill/blend stay allowed on locked layers (the web
    // lock only guards pixel edits, reorder, rename, and delete).

    bool toggle_layer_visible(const std::string& id) {
        auto* layer = find_layer(id);
        if (layer == nullptr) return false;
        layer->visible = !layer->visible;
        snapshot("Layer Visibility", layer->visible ? "eye" : "eye-off");
        return true;
    }

    bool set_layer_visible(const std::string& id, bool visible) {
        auto* layer = find_layer(id);
        if (layer == nullptr || layer->visible == visible) return false;
        layer->visible = visible;
        snapshot("Layer Visibility", visible ? "eye" : "eye-off");
        return true;
    }

    bool set_active_opacity(double value) {
        auto* layer = active_layer_ref();
        if (layer == nullptr) return false;
        layer->opacity = clamp_percent(value);
        snapshot("Layer Opacity", "opacity");
        return true;
    }

    // Fill is tracked separately from opacity, like the web (effective layer
    // alpha = opacity * fill).
    bool set_active_fill_amount(double value) {
        auto* layer = active_layer_ref();
        if (layer == nullptr) return false;
        layer->fill = clamp_percent(value);
        snapshot("Layer Fill", "opacity");
        return true;
    }

    bool set_active_blend(const std::string& value) {
        auto* layer = active_layer_ref();
        if (layer == nullptr || value.empty()) return false;
        layer->blend = value;
        snapshot("Blend: " + value, "layers");
        return true;
    }

    bool set_active_locked(bool locked) {
        auto* layer = active_layer_ref();
        if (layer == nullptr || layer->locked == locked) return false;
        layer->locked = locked;
        snapshot(locked ? "Lock Layer" : "Unlock Layer", "lock");
        return true;
    }

    // ── "Pixel" ops (CSS-style approximations; guarded by layer lock) ──────

    // Full style replacement (gradient tool, shape tool, generated content).
    bool set_active_style(const std::string& style, const std::string& label,
                          const std::string& icon = "brush") {
        auto* layer = unlocked_active();
        if (layer == nullptr || style.empty()) return false;
        layer->style = style;
        snapshot(label.empty() ? "Edit Layer" : label, icon);
        return true;
    }

    // Appends declarations (e.g. a stroke's inset box-shadow) on top of the
    // existing style. Same-name declarations are replaced, not duplicated.
    bool append_active_style(const std::string& css, const std::string& label,
                             const std::string& icon = "edit") {
        auto* layer = unlocked_active();
        if (layer == nullptr || css.empty()) return false;
        std::string merged = layer->style;
        for (const auto& [name, value] : parse_declarations(css)) {
            merged = with_declaration(merged, name, value);
        }
        layer->style = merged;
        snapshot(label.empty() ? "Edit Layer" : label, icon);
        return true;
    }

    // Stacks a new background layer on TOP of the current background (used
    // for brush stamps and partial fills). `background` must be a valid
    // <bg-layer> (gradient/url) or a plain color which gets wrapped.
    bool paint_active(const std::string& background, const std::string& label,
                      const std::string& icon = "brush") {
        auto* layer = unlocked_active();
        if (layer == nullptr) return false;
        const std::string top = as_background_layer(background);
        if (top.empty()) return false;
        const std::string below =
            as_background_layer(declaration_value(layer->style, "background"));
        const std::string merged =
            below.empty() ? top : top + "," + below;
        layer->style = with_declaration(layer->style, "background", merged);
        snapshot(label.empty() ? "Paint" : label, icon);
        return true;
    }

    // Merges CSS filter functions into the layer's existing filter list so
    // repeated adjustments compose instead of overriding each other.
    bool adjust_active(const std::string& filter_functions,
                       const std::string& label,
                       const std::string& icon = "color-grade") {
        auto* layer = unlocked_active();
        if (layer == nullptr || filter_functions.empty()) return false;
        const std::string existing = declaration_value(layer->style, "filter");
        const std::string merged =
            existing.empty() ? filter_functions
                             : existing + " " + filter_functions;
        layer->style = with_declaration(layer->style, "filter", merged);
        snapshot(label.empty() ? "Adjustment" : label, icon);
        return true;
    }

    // Fill (Edit > Fill / Paint Bucket). Full opacity replaces the layer
    // content; partial opacity overlays a translucent color above the
    // existing background. Blend-mode fills are Phase-B pixel work (a fill's
    // blend can't be expressed inside a single CSS background stack).
    bool fill_active(const std::string& color, double opacity = 100.0,
                     const std::string& label = "Fill") {
        auto* layer = unlocked_active();
        if (layer == nullptr || color.empty()) return false;
        opacity = clamp_percent(opacity);
        int r = 0, g = 0, b = 0;
        if (opacity < 99.5 && parse_hex_color(color, r, g, b)) {
            char rgba[48];
            std::snprintf(rgba, sizeof(rgba), "rgba(%d,%d,%d,%.3f)", r, g, b,
                          opacity / 100.0);
            const std::string overlay =
                "linear-gradient(" + std::string(rgba) + "," + rgba + ")";
            const std::string below = as_background_layer(
                declaration_value(layer->style, "background"));
            const std::string merged =
                below.empty() ? overlay : overlay + "," + below;
            layer->style =
                with_declaration(layer->style, "background", merged);
        } else {
            layer->style =
                with_declaration(layer->style, "background", color);
        }
        snapshot(label.empty() ? "Fill" : label, "fill");
        return true;
    }

    bool invert_active() {
        return adjust_active("invert(1)", "Invert", "mirror");
    }

    bool desaturate_active() {
        return adjust_active("grayscale(1)", "Desaturate", "mono");
    }

    // ── Document-level ops ──────────────────────────────────────────────────

    // Merge the active layer down onto the one below it (metadata/CSS-level:
    // the two background stacks are combined; the active layer's filter and
    // blend are dropped — faithful per-pixel compositing is Phase-B work).
    bool merge_down() {
        const int index = active_index();
        if (index <= 0) return false;
        PhotoLayerSnapshot& above = layers_[index];
        PhotoLayerSnapshot& below = layers_[index - 1];
        if (above.locked || below.locked) return false;
        const std::string top =
            as_background_layer(declaration_value(above.style, "background"));
        const std::string bottom =
            as_background_layer(declaration_value(below.style, "background"));
        if (!top.empty()) {
            const std::string merged =
                bottom.empty() ? top : top + "," + bottom;
            below.style =
                with_declaration(below.style, "background", merged);
        }
        active_layer_id_ = below.id;
        layers_.erase(layers_.begin() + index);
        snapshot("Merge Down", "compress");
        return true;
    }

    // Flatten to a single locked Background layer whose background stacks
    // every visible layer's background (topmost first).
    bool flatten() {
        if (layers_.size() <= 1) return false;
        std::string merged;
        for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
            if (!it->visible) continue;
            const std::string bg = as_background_layer(
                declaration_value(it->style, "background"));
            if (bg.empty()) continue;
            if (!merged.empty()) merged += ",";
            merged += bg;
        }
        PhotoLayerSnapshot flattened;
        flattened.id = make_unique_id("background");
        flattened.name = "Background";
        flattened.kind = "pixel";
        flattened.locked = true;
        flattened.style =
            merged.empty() ? "background:#ffffff" : "background:" + merged;
        layers_ = {flattened};
        active_layer_id_ = flattened.id;
        snapshot("Flatten Image", "layers");
        return true;
    }

    // Image Size / Canvas Size / Crop all resolve to new document dimensions
    // at the state level (pixel resampling / anchoring is Phase-B work).
    bool resize_document(int width, int height,
                         const std::string& label = "Image Size",
                         const std::string& icon = "aspect") {
        if (width < 1 || height < 1) return false;
        if (width == width_ && height == height_) return false;
        width_ = width;
        height_ = height;
        snapshot(label.empty() ? "Image Size" : label, icon);
        return true;
    }

    // File > New: fresh single-layer document; history restarts.
    void new_document(int width, int height,
                      const std::string& background_style = "") {
        width_ = std::max(1, width);
        height_ = std::max(1, height);
        PhotoLayerSnapshot background;
        background.id = "background";
        background.name = "Background";
        background.style = background_style.empty()
                               ? "background:transparent"
                               : background_style;
        layers_ = {background};
        active_layer_id_ = background.id;
        next_layer_index_ = 1;
        history_.clear();
        history_index_ = -1;
        snapshot("New Document", "file");
    }

    // ── History ─────────────────────────────────────────────────────────────

    void snapshot(const std::string& label, const std::string& icon = "") {
        if (label.empty()) return;
        if (history_index_ >= 0 &&
            history_index_ < static_cast<int>(history_.size()) - 1) {
            history_.erase(history_.begin() + history_index_ + 1,
                           history_.end());
        }
        history_.push_back({label, icon.empty() ? "edit" : icon, capture()});
        while (history_.size() > kHistoryMax) {
            history_.erase(history_.begin());
        }
        history_index_ = static_cast<int>(history_.size()) - 1;
    }

    std::string undo() {
        if (history_index_ > 0) {
            --history_index_;
            restore(history_[history_index_].state);
        }
        return history_.empty() ? std::string{}
                                : history_[history_index_].label;
    }

    std::string redo() {
        if (history_index_ < static_cast<int>(history_.size()) - 1) {
            ++history_index_;
            restore(history_[history_index_].state);
        }
        return history_.empty() ? std::string{}
                                : history_[history_index_].label;
    }

    bool select_history(int index) {
        if (history_.empty()) return false;
        index = std::clamp(index, 0, static_cast<int>(history_.size()) - 1);
        if (index == history_index_) return false;
        history_index_ = index;
        restore(history_[history_index_].state);
        return true;
    }

    // ── Sample scenes ───────────────────────────────────────────────────────

    // Web-parity boot scene (decius-photo generateSampleScene), approximated
    // in CSS. Document dimensions are left untouched (the web regenerates the
    // sample at the current document size too). History restarts at "Open",
    // matching the web boot snapshot.
    void load_sample_scene() {
        layers_.clear();

        auto push = [&](const char* id, const char* name, const char* kind,
                        bool locked, double opacity, const char* blend,
                        std::string style) {
            PhotoLayerSnapshot layer;
            layer.id = id;
            layer.name = name;
            layer.kind = kind;
            layer.locked = locked;
            layer.opacity = opacity;
            layer.blend = blend;
            layer.style = std::move(style);
            layers_.push_back(std::move(layer));
        };

        // 1. Background — sunset sky gradient + a few stars in the top half.
        push("background", "Background", "pixel", true, 100.0, "Normal",
             "background:"
             "radial-gradient(1px 1px at 12% 9%,#fff 0 50%,transparent 55%),"
             "radial-gradient(1px 1px at 27% 21%,#fffa 0 50%,transparent 55%),"
             "radial-gradient(1px 1px at 44% 6%,#fffc 0 50%,transparent 55%),"
             "radial-gradient(1px 1px at 63% 15%,#fff9 0 50%,transparent 55%),"
             "radial-gradient(1px 1px at 78% 9%,#fff 0 50%,transparent 55%),"
             "radial-gradient(1px 1px at 91% 24%,#fffb 0 50%,transparent 55%),"
             "linear-gradient(180deg,#0b1437 0%,#1d2b66 38%,#6a4a86 62%,"
             "#d98a5a 82%,#f2c277 100%)");
        // 2. Sun — radial glow + disc, screen blend.
        push("sun", "Sun", "pixel", false, 100.0, "Screen",
             "background:radial-gradient(circle at 50% 62%,"
             "#ffe9c4 0%,#ffcf8a 5%,rgba(255,170,80,.55) 11%,"
             "rgba(255,140,60,.22) 22%,transparent 42%)");
        // 3-4. Mountain ridges (flat CSS ridges; jagged silhouettes are
        // Phase-B raster work).
        push("mountains-far", "Mountains · Far", "pixel", false, 85.0,
             "Normal",
             "background:linear-gradient(180deg,transparent 0 64%,"
             "#34305a 66%,#34305a 100%)");
        push("mountains-near", "Mountains · Near", "pixel", false, 100.0,
             "Normal",
             "background:linear-gradient(180deg,transparent 0 74%,"
             "#1a1730 76%,#1a1730 100%)");
        // 5. Water reflection band, overlay blend.
        push("water", "Water", "pixel", false, 55.0, "Overlay",
             "background:linear-gradient(180deg,transparent 0 82%,"
             "rgba(140,170,255,.55) 83%,rgba(70,100,190,.4) 100%)");
        // 6. Title (text layer; the app renders text layers as typography).
        push("decius-title", "DECIUS", "text", false, 100.0, "Normal",
             "background:transparent");
        // 7. Empty paint layer on top — active at boot.
        push("paint", "Paint", "pixel", false, 100.0, "Normal",
             "background:transparent");

        active_layer_id_ = "paint";
        next_layer_index_ = static_cast<int>(layers_.size()) + 1;
        history_.clear();
        history_index_ = -1;
        snapshot("Open", "file");
    }

    // Legacy demo scene kept for API stability (seeded multi-entry history).
    // The photo_edit app boots load_sample_scene() instead. Every seeded
    // entry captures real state, so undo genuinely restores (e.g. the final
    // "Layer Opacity" step drops the retouch layer from 100% to 82%).
    void reset_sample() {
        layers_ = {
            {"background", "Background", "pixel", true, true, 100.0, 100.0,
             "Normal",
             "background:linear-gradient(135deg,#f08c3c 0%,#e26b9c 42%,#7654d9 100%)"},
            {"retouch", "Hero retouch", "pixel", true, false, 100.0, 100.0,
             "Normal",
             "background:radial-gradient(circle at 68% 32%,rgba(255,240,200,.75),rgba(255,200,120,0) 34%)"},
            {"curves", "Adjustment - Curves", "adjustment", true, false, 64.0,
             100.0, "Soft Light",
             "background:linear-gradient(90deg,rgba(31,111,235,.5),rgba(255,122,184,.45))"},
            {"title", "Title type", "text", true, false, 94.0, 100.0,
             "Normal", "background:transparent"},
            {"vignette", "Vignette", "pixel", true, false, 68.0, 100.0,
             "Multiply",
             "background:radial-gradient(circle,rgba(0,0,0,0) 40%,rgba(0,0,0,.72) 100%)"},
        };
        active_layer_id_ = "retouch";
        next_layer_index_ = static_cast<int>(layers_.size()) + 1;
        history_.clear();
        history_index_ = -1;
        snapshot("Open Sample", "file");
        snapshot("Brush Stroke", "brush");
        snapshot("Hue / Saturation", "color-grade");
        if (auto* retouch = find_layer("retouch")) retouch->opacity = 82.0;
        snapshot("Layer Opacity", "opacity");
    }

private:
    // Full document state captured per history record (the CSS-level
    // equivalent of the web's per-layer pixel dataURL snapshots).
    struct DocState {
        std::vector<PhotoLayerSnapshot> layers;
        std::string active_layer_id;
        int width = 0;
        int height = 0;
        int next_layer_index = 1;
    };

    struct HistoryRecord {
        std::string label;
        std::string icon;
        DocState state;
    };

    static constexpr std::size_t kHistoryMax = 40;  // Matches the web cap.

    DocState capture() const {
        return {layers_, active_layer_id_, width_, height_,
                next_layer_index_};
    }

    void restore(const DocState& state) {
        layers_ = state.layers;
        active_layer_id_ = state.active_layer_id;
        width_ = state.width;
        height_ = state.height;
        next_layer_index_ = state.next_layer_index;
        if (find_layer(active_layer_id_) == nullptr && !layers_.empty()) {
            active_layer_id_ = layers_.back().id;
        }
    }

    PhotoLayerSnapshot* find_layer(const std::string& id) {
        auto it = std::find_if(layers_.begin(), layers_.end(),
                               [&](const auto& layer) {
                                   return layer.id == id;
                               });
        return it == layers_.end() ? nullptr : &*it;
    }

    const PhotoLayerSnapshot* find_layer(const std::string& id) const {
        auto it = std::find_if(layers_.begin(), layers_.end(),
                               [&](const auto& layer) {
                                   return layer.id == id;
                               });
        return it == layers_.end() ? nullptr : &*it;
    }

    PhotoLayerSnapshot* active_layer_ref() {
        return find_layer(active_layer_id_);
    }

    // Lock guard used by every "pixel" mutation.
    PhotoLayerSnapshot* unlocked_active() {
        auto* layer = active_layer_ref();
        return (layer == nullptr || layer->locked) ? nullptr : layer;
    }

    std::string make_unique_id(const std::string& base) const {
        if (find_layer(base) == nullptr) return base;
        for (int i = 2;; ++i) {
            const std::string candidate = base + "-" + std::to_string(i);
            if (find_layer(candidate) == nullptr) return candidate;
        }
    }

    int width_ = 1280;
    int height_ = 800;
    std::vector<PhotoLayerSnapshot> layers_;
    std::string active_layer_id_;
    std::vector<HistoryRecord> history_;
    int history_index_ = -1;
    int next_layer_index_ = 1;
};

}  // namespace

void bind_photo_core(py::module_& m) {
    py::class_<PhotoLayerSnapshot>(m, "PhotoLayerSnapshot")
        .def_readonly("id", &PhotoLayerSnapshot::id)
        .def_readonly("name", &PhotoLayerSnapshot::name)
        .def_readonly("kind", &PhotoLayerSnapshot::kind)
        .def_readonly("visible", &PhotoLayerSnapshot::visible)
        .def_readonly("locked", &PhotoLayerSnapshot::locked)
        .def_readonly("opacity", &PhotoLayerSnapshot::opacity)
        .def_readonly("fill", &PhotoLayerSnapshot::fill)
        .def_readonly("blend", &PhotoLayerSnapshot::blend)
        .def_readonly("style", &PhotoLayerSnapshot::style);

    py::class_<PhotoDocument>(m, "PhotoDocument")
        .def(py::init<int, int>(), py::arg("width") = 1280,
             py::arg("height") = 800)
        .def("width", &PhotoDocument::width)
        .def("height", &PhotoDocument::height)
        .def("layers", &PhotoDocument::layers)
        .def("history", &PhotoDocument::history)
        .def("history_entries", &PhotoDocument::history_entries)
        .def("history_index", &PhotoDocument::history_index)
        .def("active_layer_id", &PhotoDocument::active_layer_id)
        .def("active_layer", &PhotoDocument::active_layer)
        .def("active_index", &PhotoDocument::active_index)
        .def("set_active_layer", &PhotoDocument::set_active_layer,
             py::arg("id"))
        .def("add_layer", &PhotoDocument::add_layer, py::arg("name") = "",
             py::arg("kind") = "pixel", py::arg("style") = "",
             py::arg("blend") = "Normal", py::arg("opacity") = 100.0,
             py::arg("label") = "New Layer", py::arg("icon") = "plus")
        .def("duplicate_active_layer", &PhotoDocument::duplicate_active_layer)
        .def("delete_active_layer", &PhotoDocument::delete_active_layer)
        .def("rename_layer", &PhotoDocument::rename_layer, py::arg("id"),
             py::arg("name"))
        .def("move_active", &PhotoDocument::move_active, py::arg("dir"))
        .def("reorder_layer", &PhotoDocument::reorder_layer, py::arg("id"),
             py::arg("new_index"))
        .def("toggle_layer_visible", &PhotoDocument::toggle_layer_visible,
             py::arg("id"))
        .def("set_layer_visible", &PhotoDocument::set_layer_visible,
             py::arg("id"), py::arg("visible"))
        .def("set_active_opacity", &PhotoDocument::set_active_opacity,
             py::arg("value"))
        .def("set_active_fill_amount",
             &PhotoDocument::set_active_fill_amount, py::arg("value"))
        // Back-compat alias for the original fill-amount setter name.
        .def("set_active_fill", &PhotoDocument::set_active_fill_amount,
             py::arg("value"))
        .def("set_active_blend", &PhotoDocument::set_active_blend,
             py::arg("value"))
        .def("set_active_locked", &PhotoDocument::set_active_locked,
             py::arg("locked"))
        .def("set_active_style", &PhotoDocument::set_active_style,
             py::arg("style"), py::arg("label"), py::arg("icon") = "brush")
        .def("append_active_style", &PhotoDocument::append_active_style,
             py::arg("css"), py::arg("label"), py::arg("icon") = "edit")
        .def("paint_active", &PhotoDocument::paint_active,
             py::arg("background"), py::arg("label"),
             py::arg("icon") = "brush")
        .def("adjust_active", &PhotoDocument::adjust_active,
             py::arg("filter_functions"), py::arg("label"),
             py::arg("icon") = "color-grade")
        .def("fill_active", &PhotoDocument::fill_active, py::arg("color"),
             py::arg("opacity") = 100.0, py::arg("label") = "Fill")
        .def("invert_active", &PhotoDocument::invert_active)
        .def("desaturate_active", &PhotoDocument::desaturate_active)
        .def("merge_down", &PhotoDocument::merge_down)
        .def("flatten", &PhotoDocument::flatten)
        .def("resize_document", &PhotoDocument::resize_document,
             py::arg("width"), py::arg("height"),
             py::arg("label") = "Image Size", py::arg("icon") = "aspect")
        .def("new_document", &PhotoDocument::new_document, py::arg("width"),
             py::arg("height"), py::arg("background_style") = "")
        .def("snapshot", &PhotoDocument::snapshot, py::arg("label"),
             py::arg("icon") = "")
        .def("undo", &PhotoDocument::undo)
        .def("redo", &PhotoDocument::redo)
        .def("select_history", &PhotoDocument::select_history,
             py::arg("index"))
        .def("load_sample_scene", &PhotoDocument::load_sample_scene)
        .def("reset_sample", &PhotoDocument::reset_sample);
}
