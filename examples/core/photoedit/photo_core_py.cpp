// pybind11 bindings for the raster photo core (photo_core.h).
//
// This is EXAMPLE-SUPPORT code, not part of the affineui library: it
// builds as its own `photo_core` extension module (see
// bindings/python/CMakeLists.txt), the C++ analog of examples/core/
// sidecar libraries like 3dengine. Python drives document operations
// and routes pointer input; the core owns pixels, compositing, and the
// custom-paint render path (attach()). Rect-like values cross the
// boundary as plain tuples / None so the Python side stays idiomatic.
// affineui types (App) resolve across modules through pybind11's
// shared internals, so `attach` accepts an affineui.App directly.

#include "photo_core.h"

#include <affineui/app.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <optional>
#include <tuple>

namespace py = pybind11;

namespace {

using photo::PhotoDoc;
using photo::RectI;

std::optional<std::tuple<int, int, int, int>> rect_or_none(const RectI& r) {
    if (r.empty()) return std::nullopt;
    return std::make_tuple(r.x, r.y, r.w, r.h);
}

}  // namespace

PYBIND11_MODULE(photo_core, m) {
    m.doc() = "Raster photo-editing core for the Decius Photo Edit "
              "example (not part of the affineui library).";
    py::class_<photo::LayerInfo>(m, "PhotoLayer",
                                 "Metadata snapshot of one raster layer "
                                 "(pixels stay inside the core).")
        .def_readonly("id", &photo::LayerInfo::id)
        .def_readonly("name", &photo::LayerInfo::name)
        .def_readonly("kind", &photo::LayerInfo::kind)
        .def_readonly("blend", &photo::LayerInfo::blend)
        .def_readonly("visible", &photo::LayerInfo::visible)
        .def_readonly("locked", &photo::LayerInfo::locked)
        .def_readonly("opacity", &photo::LayerInfo::opacity)
        .def_readonly("fill", &photo::LayerInfo::fill);

    py::class_<photo::HistoryEntry>(m, "PhotoHistoryEntry")
        .def_readonly("name", &photo::HistoryEntry::name)
        .def_readonly("icon", &photo::HistoryEntry::icon);

    py::class_<PhotoDoc>(
            m, "PhotoDocument",
            "Raster photo-editing document: real RGBA layers, canvas-style "
            "blend compositing, a brush engine, selections, adjustments, "
            "and pixel-snapshot history — the C++ core behind the Decius "
            "Photo Edit sample. attach(app) hands the whole render path "
            "(stage / navigator / layer thumbnails) to the core's "
            "custom-paint handlers.")
        .def(py::init<int, int>(), py::arg("width") = 1280,
             py::arg("height") = 800)
        // document
        .def("width", &PhotoDoc::width)
        .def("height", &PhotoDoc::height)
        .def("new_document", &PhotoDoc::new_document, py::arg("width"),
             py::arg("height"), py::arg("background_hex") = "",
             "Fresh single-layer document (empty hex → transparent). "
             "History restarts.")
        .def("load_sample_scene", &PhotoDoc::load_sample_scene)
        // view transform
        .def("zoom", &PhotoDoc::zoom)
        .def("pan_x", &PhotoDoc::pan_x)
        .def("pan_y", &PhotoDoc::pan_y)
        .def("set_pan", &PhotoDoc::set_pan, py::arg("px"), py::arg("py"))
        .def("set_zoom", &PhotoDoc::set_zoom, py::arg("zoom"))
        .def("set_zoom_at", &PhotoDoc::set_zoom_at, py::arg("zoom"),
             py::arg("client_x"), py::arg("client_y"),
             "Zoom keeping the document point under (client_x, client_y) "
             "stationary (ctrl-wheel / zoom-tool behavior).")
        .def("fit_to_screen", &PhotoDoc::fit_to_screen)
        .def("stage_rect",
             [](const PhotoDoc& doc) { return rect_or_none(doc.stage_rect()); },
             "Window-coord rect of the stage canvas as of the last paint "
             "(None before the first frame).")
        .def("screen_to_doc",
             [](const PhotoDoc& doc, double cx, double cy) {
                 double dx = 0, dy = 0;
                 doc.screen_to_doc(cx, cy, dx, dy);
                 return std::make_tuple(dx, dy);
             },
             py::arg("client_x"), py::arg("client_y"))
        .def("doc_origin",
             [](const PhotoDoc& doc) {
                 double x = 0, y = 0;
                 doc.doc_origin(x, y);
                 return std::make_tuple(x, y);
             },
             "Window coords of document (0, 0).")
        // layers
        .def("layers", &PhotoDoc::layers,
             "Layer metadata, bottom → top (web stack order).")
        .def("active_layer", &PhotoDoc::active_layer)
        .def("active_index", &PhotoDoc::active_index)
        .def("active_id", &PhotoDoc::active_id)
        .def("set_active_index", &PhotoDoc::set_active_index,
             py::arg("index"))
        .def("set_active_id", &PhotoDoc::set_active_id, py::arg("id"))
        .def("add_layer", &PhotoDoc::add_layer, py::arg("name") = "",
             py::arg("blend") = "source-over", py::arg("opacity") = 1.0,
             py::arg("label") = "New Layer", py::arg("icon") = "plus")
        .def("duplicate_active", &PhotoDoc::duplicate_active)
        .def("delete_active", &PhotoDoc::delete_active)
        .def("move_active", &PhotoDoc::move_active, py::arg("dir"))
        .def("reorder_layer", &PhotoDoc::reorder_layer, py::arg("id"),
             py::arg("new_index"))
        .def("rename_layer", &PhotoDoc::rename_layer, py::arg("id"),
             py::arg("name"))
        .def("toggle_visible", &PhotoDoc::toggle_visible, py::arg("id"))
        .def("set_active_opacity", &PhotoDoc::set_active_opacity,
             py::arg("value"),
             "0..1. No snapshot — call snapshot() when the scrub ends "
             "(web parity).")
        .def("set_active_fill", &PhotoDoc::set_active_fill, py::arg("value"))
        .def("set_active_blend", &PhotoDoc::set_active_blend,
             py::arg("blend"))
        .def("set_active_locked", &PhotoDoc::set_active_locked,
             py::arg("locked"))
        .def("merge_down", &PhotoDoc::merge_down)
        .def("flatten", &PhotoDoc::flatten)
        // selection
        .def("set_selection", &PhotoDoc::set_selection, py::arg("x"),
             py::arg("y"), py::arg("w"), py::arg("h"))
        .def("clear_selection", &PhotoDoc::clear_selection)
        .def("has_selection", &PhotoDoc::has_selection)
        .def("selection",
             [](const PhotoDoc& doc) { return rect_or_none(doc.selection()); })
        .def("select_all", &PhotoDoc::select_all)
        .def("delete_selection_pixels", &PhotoDoc::delete_selection_pixels)
        // brush engine
        .def("begin_stroke", &PhotoDoc::begin_stroke, py::arg("tool"),
             py::arg("x"), py::arg("y"), py::arg("size") = 24.0,
             py::arg("hardness") = 0.7, py::arg("opacity") = 1.0,
             py::arg("flow") = 1.0, py::arg("color") = "#000000",
             "Start a brush/pencil/eraser/clone/history/dodge/burn/smudge/"
             "blur stroke at document coords. False when the layer is "
             "locked (or clone has no source).")
        .def("stroke_to", &PhotoDoc::stroke_to, py::arg("x"), py::arg("y"))
        .def("end_stroke", &PhotoDoc::end_stroke)
        .def("cancel_stroke", &PhotoDoc::cancel_stroke)
        .def("stroking", &PhotoDoc::stroking)
        .def("set_clone_source", &PhotoDoc::set_clone_source, py::arg("x"),
             py::arg("y"))
        .def("has_clone_source", &PhotoDoc::has_clone_source)
        // move tool
        .def("begin_move", &PhotoDoc::begin_move)
        .def("move_to", &PhotoDoc::move_to, py::arg("dx"), py::arg("dy"))
        .def("end_move", &PhotoDoc::end_move)
        // click tools
        .def("fill_at", &PhotoDoc::fill_at, py::arg("x"), py::arg("y"),
             py::arg("color"), py::arg("tolerance") = 32.0,
             py::arg("contiguous") = true, py::arg("opacity") = 1.0)
        .def("wand_select",
             [](PhotoDoc& doc, double x, double y, double tol, bool contig) {
                 return rect_or_none(doc.wand_select(x, y, tol, contig));
             },
             py::arg("x"), py::arg("y"), py::arg("tolerance") = 32.0,
             py::arg("contiguous") = true,
             "Select the similar-color region's bbox on the composite; "
             "returns the selection rect or None.")
        .def("pick_color", &PhotoDoc::pick_color, py::arg("x"), py::arg("y"))
        .def("apply_gradient", &PhotoDoc::apply_gradient, py::arg("x0"),
             py::arg("y0"), py::arg("x1"), py::arg("y1"), py::arg("color"),
             py::arg("opacity") = 1.0)
        .def("place_type", &PhotoDoc::place_type, py::arg("x"), py::arg("y"),
             py::arg("text"), py::arg("size") = 64.0,
             py::arg("color") = "#000000", py::arg("bold") = true)
        .def("draw_shape", &PhotoDoc::draw_shape, py::arg("x0"),
             py::arg("y0"), py::arg("x1"), py::arg("y1"), py::arg("color"),
             py::arg("radius") = 0.0)
        .def("pen_add_point", &PhotoDoc::pen_add_point, py::arg("x"),
             py::arg("y"))
        .def("pen_clear", &PhotoDoc::pen_clear)
        .def("pen_count", &PhotoDoc::pen_count)
        // edit-menu ops
        .def("fill_layer", &PhotoDoc::fill_layer, py::arg("color"),
             py::arg("opacity") = 1.0, py::arg("blend") = "source-over",
             py::arg("label") = "Fill")
        .def("stroke_selection", &PhotoDoc::stroke_selection,
             py::arg("color"), py::arg("width") = 4.0,
             py::arg("location") = "center", py::arg("opacity") = 1.0)
        // document geometry
        .def("apply_crop", &PhotoDoc::apply_crop)
        .def("crop_to", &PhotoDoc::crop_to, py::arg("x"), py::arg("y"),
             py::arg("w"), py::arg("h"))
        .def("resize_image", &PhotoDoc::resize_image, py::arg("w"),
             py::arg("h"))
        .def("resize_canvas", &PhotoDoc::resize_canvas, py::arg("w"),
             py::arg("h"), py::arg("anchor") = "mc")
        // adjustments & filters
        .def("begin_preview", &PhotoDoc::begin_preview,
             "Capture the active layer's selection region as the live-"
             "preview baseline for an adjustment modal.")
        .def("preview_adjust", &PhotoDoc::preview_adjust, py::arg("kind"),
             py::arg("a") = 0.0, py::arg("b") = 0.0, py::arg("c") = 0.0)
        .def("commit_preview", &PhotoDoc::commit_preview,
             py::arg("label") = "Adjustment",
             py::arg("icon") = "color-grade")
        .def("cancel_preview", &PhotoDoc::cancel_preview)
        .def("previewing", &PhotoDoc::previewing)
        .def("apply_adjust", &PhotoDoc::apply_adjust, py::arg("kind"),
             py::arg("a") = 0.0, py::arg("label") = "",
             py::arg("icon") = "")
        // place embedded
        .def("place_asset", &PhotoDoc::place_asset, py::arg("asset"))
        // history
        .def("history_entries", &PhotoDoc::history_entries)
        .def("history_index", &PhotoDoc::history_index)
        .def("history_source", &PhotoDoc::history_source)
        .def("set_history_source", &PhotoDoc::set_history_source,
             py::arg("index"))
        .def("snapshot", &PhotoDoc::snapshot, py::arg("name"),
             py::arg("icon") = "")
        .def("undo", &PhotoDoc::undo)
        .def("redo", &PhotoDoc::redo)
        .def("jump_to", &PhotoDoc::jump_to, py::arg("index"))
        // export
        .def("export_png", &PhotoDoc::export_png, py::arg("path"),
             py::arg("scale") = 1.0, py::arg("opaque_white") = false)
        // UI integration
        .def("attach", &PhotoDoc::attach, py::arg("app"),
             py::keep_alive<1, 2>(),
             "Register the core's custom-paint handlers (\"ps-stage\", "
             "\"ps-nav\", \"ps-thumb-<id>\") with the app. The document "
             "then owns the whole canvas render path — no Python in the "
             "frame loop.")
        .def("detach", &PhotoDoc::detach)
        .def("request_repaint", &PhotoDoc::request_repaint)
        .def("thumb_paint_name", &PhotoDoc::thumb_paint_name,
             py::arg("layer_id"))
        .def("revision", &PhotoDoc::revision);
}
