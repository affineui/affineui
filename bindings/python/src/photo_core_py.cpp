#include "photo_core_py.h"

#include <pybind11/stl.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace py = pybind11;

namespace {

struct PhotoLayerSnapshot {
    std::string id;
    std::string name;
    std::string kind;
    bool visible = true;
    bool locked = false;
    double opacity = 100.0;
    double fill = 100.0;
    std::string blend = "Normal";
    std::string style =
        "background:linear-gradient(135deg,#345b9d,#6d9ed8)";
};

double clamp_percent(double value) {
    if (!std::isfinite(value)) return 100.0;
    return std::clamp(value, 0.0, 100.0);
}

class PhotoDocument {
public:
    explicit PhotoDocument(int width = 1280, int height = 800)
        : width_(std::max(1, width)), height_(std::max(1, height)) {
        reset_sample();
    }

    int width() const { return width_; }
    int height() const { return height_; }

    std::vector<PhotoLayerSnapshot> layers() const { return layers_; }
    std::vector<std::string> history() const { return history_; }
    int history_index() const { return history_index_; }
    std::string active_layer_id() const { return active_layer_id_; }

    PhotoLayerSnapshot active_layer() const {
        const auto* layer = find_layer(active_layer_id_);
        if (layer != nullptr) return *layer;
        return layers_.empty() ? PhotoLayerSnapshot{} : layers_.back();
    }

    bool set_active_layer(const std::string& id) {
        if (find_layer(id) == nullptr) return false;
        active_layer_id_ = id;
        return true;
    }

    std::string add_layer(const std::string& name = "",
                          const std::string& kind = "pixel",
                          const std::string& style = "") {
        const int index = next_layer_index_++;
        PhotoLayerSnapshot layer;
        layer.id = make_unique_id("layer-" + std::to_string(index));
        layer.name = name.empty() ? "Layer " + std::to_string(index) : name;
        layer.kind = kind.empty() ? "pixel" : kind;
        layer.style = style.empty()
                          ? "background:linear-gradient(135deg,rgba(79,134,214,.38),rgba(232,132,58,.32))"
                          : style;
        layers_.push_back(layer);
        active_layer_id_ = layer.id;
        snapshot("New Layer");
        return layer.id;
    }

    std::string duplicate_active_layer() {
        auto* layer = active_layer_ref();
        if (layer == nullptr) return {};
        PhotoLayerSnapshot copy = *layer;
        copy.id = make_unique_id(layer->id + "-copy-" +
                                 std::to_string(layers_.size()));
        copy.name = layer->name + " copy";
        copy.locked = false;
        layers_.push_back(copy);
        active_layer_id_ = copy.id;
        snapshot("Duplicate Layer");
        return copy.id;
    }

    bool delete_active_layer() {
        if (layers_.size() <= 1) return false;
        const auto active = active_layer_id_;
        auto it = std::find_if(layers_.begin(), layers_.end(),
                               [&](const auto& layer) {
                                   return layer.id == active;
                               });
        if (it == layers_.end()) return false;
        layers_.erase(it);
        active_layer_id_ = layers_.back().id;
        snapshot("Delete Layer");
        return true;
    }

    bool toggle_layer_visible(const std::string& id) {
        auto* layer = find_layer(id);
        if (layer == nullptr) return false;
        layer->visible = !layer->visible;
        snapshot("Layer Visibility");
        return true;
    }

    bool set_layer_visible(const std::string& id, bool visible) {
        auto* layer = find_layer(id);
        if (layer == nullptr) return false;
        layer->visible = visible;
        snapshot("Layer Visibility");
        return true;
    }

    bool set_active_opacity(double value) {
        auto* layer = active_layer_ref();
        if (layer == nullptr) return false;
        layer->opacity = clamp_percent(value);
        snapshot("Layer Opacity");
        return true;
    }

    bool set_active_fill(double value) {
        auto* layer = active_layer_ref();
        if (layer == nullptr) return false;
        layer->fill = clamp_percent(value);
        snapshot("Layer Fill");
        return true;
    }

    bool set_active_blend(const std::string& value) {
        auto* layer = active_layer_ref();
        if (layer == nullptr || value.empty()) return false;
        layer->blend = value;
        snapshot("Blend: " + value);
        return true;
    }

    bool set_active_locked(bool locked) {
        auto* layer = active_layer_ref();
        if (layer == nullptr) return false;
        layer->locked = locked;
        snapshot("Layer Lock");
        return true;
    }

    bool fill_active(const std::string& color) {
        auto* layer = active_layer_ref();
        if (layer == nullptr || color.empty()) return false;
        layer->style = "background:" + color;
        snapshot("Fill with Foreground");
        return true;
    }

    bool invert_active() {
        auto* layer = active_layer_ref();
        if (layer == nullptr) return false;
        layer->style += ";filter:invert(1)";
        snapshot("Invert");
        return true;
    }

    bool desaturate_active() {
        auto* layer = active_layer_ref();
        if (layer == nullptr) return false;
        layer->style += ";filter:grayscale(1)";
        snapshot("Desaturate");
        return true;
    }

    bool flatten() {
        if (layers_.size() <= 1) return false;
        PhotoLayerSnapshot flattened;
        flattened.id = "background";
        flattened.name = "Background";
        flattened.kind = "pixel";
        flattened.locked = true;
        flattened.style =
            "background:linear-gradient(135deg,#f08c3c 0%,#e26b9c 42%,#7654d9 100%)";
        layers_ = {flattened};
        active_layer_id_ = flattened.id;
        snapshot("Flatten Image");
        return true;
    }

    void snapshot(const std::string& label) {
        if (label.empty()) return;
        if (history_index_ >= 0 &&
            history_index_ < static_cast<int>(history_.size()) - 1) {
            history_.erase(history_.begin() + history_index_ + 1,
                           history_.end());
        }
        history_.push_back(label);
        while (history_.size() > 24) {
            history_.erase(history_.begin());
        }
        history_index_ = static_cast<int>(history_.size()) - 1;
    }

    std::string undo() {
        if (history_index_ > 0) --history_index_;
        return history_.empty() ? std::string{} : history_[history_index_];
    }

    std::string redo() {
        if (history_index_ < static_cast<int>(history_.size()) - 1) {
            ++history_index_;
        }
        return history_.empty() ? std::string{} : history_[history_index_];
    }

    bool select_history(int index) {
        if (history_.empty()) return false;
        history_index_ = std::clamp(index, 0,
                                    static_cast<int>(history_.size()) - 1);
        return true;
    }

    void reset_sample() {
        layers_ = {
            {"background", "Background", "pixel", true, true, 100.0, 100.0,
             "Normal",
             "background:linear-gradient(135deg,#f08c3c 0%,#e26b9c 42%,#7654d9 100%)"},
            {"retouch", "Hero retouch", "pixel", true, false, 82.0, 100.0,
             "Normal",
             "background:radial-gradient(circle at 68% 32%,rgba(255,240,200,.75),rgba(255,200,120,0) 34%)"},
            {"curves", "Adjustment - Curves", "adjustment", true, false, 64.0,
             100.0, "Soft Light",
             "background:linear-gradient(90deg,rgba(31,111,235,.5),rgba(255,122,184,.45))"},
            {"title", "Title type", "type", true, false, 94.0, 100.0,
             "Normal", "background:transparent"},
            {"vignette", "Vignette", "pixel", true, false, 68.0, 100.0,
             "Multiply",
             "background:radial-gradient(circle,rgba(0,0,0,0) 40%,rgba(0,0,0,.72) 100%)"},
        };
        active_layer_id_ = "retouch";
        history_ = {"Open Sample", "Brush Stroke", "Hue / Saturation",
                    "Layer Opacity"};
        history_index_ = static_cast<int>(history_.size()) - 1;
        next_layer_index_ = static_cast<int>(layers_.size()) + 1;
    }

private:
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
    std::vector<std::string> history_;
    int history_index_ = 0;
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
        .def("history_index", &PhotoDocument::history_index)
        .def("active_layer_id", &PhotoDocument::active_layer_id)
        .def("active_layer", &PhotoDocument::active_layer)
        .def("set_active_layer", &PhotoDocument::set_active_layer,
             py::arg("id"))
        .def("add_layer", &PhotoDocument::add_layer, py::arg("name") = "",
             py::arg("kind") = "pixel", py::arg("style") = "")
        .def("duplicate_active_layer", &PhotoDocument::duplicate_active_layer)
        .def("delete_active_layer", &PhotoDocument::delete_active_layer)
        .def("toggle_layer_visible", &PhotoDocument::toggle_layer_visible,
             py::arg("id"))
        .def("set_layer_visible", &PhotoDocument::set_layer_visible,
             py::arg("id"), py::arg("visible"))
        .def("set_active_opacity", &PhotoDocument::set_active_opacity,
             py::arg("value"))
        .def("set_active_fill", &PhotoDocument::set_active_fill,
             py::arg("value"))
        .def("set_active_blend", &PhotoDocument::set_active_blend,
             py::arg("value"))
        .def("set_active_locked", &PhotoDocument::set_active_locked,
             py::arg("locked"))
        .def("fill_active", &PhotoDocument::fill_active, py::arg("color"))
        .def("invert_active", &PhotoDocument::invert_active)
        .def("desaturate_active", &PhotoDocument::desaturate_active)
        .def("flatten", &PhotoDocument::flatten)
        .def("snapshot", &PhotoDocument::snapshot, py::arg("label"))
        .def("undo", &PhotoDocument::undo)
        .def("redo", &PhotoDocument::redo)
        .def("select_history", &PhotoDocument::select_history,
             py::arg("index"))
        .def("reset_sample", &PhotoDocument::reset_sample);
}
