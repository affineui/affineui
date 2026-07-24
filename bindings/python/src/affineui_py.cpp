#include <affineui/app.h>
#include <affineui/components.h>
#include <affineui/decius_bundle.h>
#include <affineui/document.h>
#include <affineui/image.h>
#include <affineui/menu.h>
#include <affineui/painter.h>
#include <affineui/tools.h>
#include <affineui/types.h>
#include <affineui/view.h>
#include <affineui/version.h>
#include <affineui/virtual_list.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace detail {

// Dynamic images created from Python are owned HERE, keyed by their painter id.
// Python (and any native core it wires up) only ever sees the plain int id, so
// a core can draw app-owned images while linking none of the affineui runtime.
// Keyed by App so two Apps can't collide; entries die with the App.
inline std::unordered_map<std::uint32_t, affineui::ImageHandle>&
py_image_registry(affineui::App& app) {
    static std::unordered_map<const affineui::App*,
                              std::unordered_map<std::uint32_t,
                                                 affineui::ImageHandle>>
        by_app;
    return by_app[&app];
}

}  // namespace detail

namespace {

affineui::App::Config make_app_config(const std::string& title,
                                      int width,
                                      int height,
                                      affineui::Color clear_color,
                                      bool high_dpi,
                                      bool vsync,
                                      const std::string& default_font_family,
                                      int default_font_size,
                                      const std::vector<std::string>& asset_folders,
                                      bool perf_overlay,
                                      bool no_bundle_decius,
                                      bool native_menus,
                                      affineui::TitleBarStyle titlebar,
                                      affineui::Point traffic_light_position) {
    affineui::App::Config cfg{};
    cfg.title = title;
    cfg.width = width;
    cfg.height = height;
    cfg.clear_color = clear_color;
    cfg.high_dpi = high_dpi;
    cfg.vsync = vsync;
    cfg.default_font_family = default_font_family;
    cfg.default_font_size = default_font_size;
    cfg.asset_folders = asset_folders;
    cfg.perf_overlay = perf_overlay;
    cfg.no_bundle_decius = no_bundle_decius;
    cfg.native_menus = native_menus;
    cfg.titlebar = titlebar;
    cfg.traffic_light_position = traffic_light_position;
    return cfg;
}

void delete_python_function(py::function* fn) {
    if (fn == nullptr) return;
    if (!Py_IsInitialized()) {
        (void) fn->release();
        delete fn;
        return;
    }
    py::gil_scoped_acquire gil;
    delete fn;
}

std::shared_ptr<py::function> keep_python_function(py::function cb) {
    return {new py::function(std::move(cb)), delete_python_function};
}

[[noreturn]] void throw_stale_callback_view() {
    PyErr_SetString(
        PyExc_ReferenceError,
        "the native AffineUI View that supplied this callback object no longer exists");
    throw py::error_already_set();
}

// A Python callback must never receive pybind's ordinary non-owning View
// wrapper: Python can retain it after the native View is destroyed. Keep only
// the core's independently invalidating token and resolve it for the duration
// of each method call. The proxy is intentionally duck-compatible with View.
class PythonCallbackView {
public:
    explicit PythonCallbackView(affineui::View& view) : view_(&view) {}
    explicit PythonCallbackView(affineui::detail::WeakViewRef view)
        : view_(std::move(view)) {}

    [[nodiscard]] bool is_alive() const noexcept {
        return view_.get() != nullptr;
    }

    py::object getattr(const std::string& name) const {
        auto* view = view_.get();
        if (!view) throw_stale_callback_view();

        py::object target =
            py::cast(view, py::return_value_policy::reference);
        py::object attribute = target.attr(name.c_str());
        if (!PyCallable_Check(attribute.ptr())) return attribute;

        auto weak = view_;
        return py::cpp_function(
            [weak = std::move(weak), name](py::args args,
                                           py::kwargs kwargs) -> py::object {
                auto* live = weak.get();
                if (!live) throw_stale_callback_view();

                py::object current =
                    py::cast(live, py::return_value_policy::reference);
                py::object callable = current.attr(name.c_str());
                PyObject* result = PyObject_Call(
                    callable.ptr(), args.ptr(),
                    kwargs ? kwargs.ptr() : nullptr);
                if (!result) throw py::error_already_set();

                py::object out =
                    py::reinterpret_steal<py::object>(result);
                // Fluent View methods return View&. Do not let that return path
                // smuggle the temporary raw pybind wrapper past the weak proxy.
                if (py::isinstance<affineui::View>(out) &&
                    out.cast<affineui::View*>() == live) {
                    return py::cast(PythonCallbackView{weak});
                }
                return out;
            });
    }

private:
    affineui::detail::WeakViewRef view_{};
};

PythonCallbackView callback_view(affineui::View& view) {
    return PythonCallbackView{view};
}

template <typename... Args>
void call_python_function(const char* label,
                          const std::shared_ptr<py::function>& callback,
                          Args&&... args) noexcept {
    try {
        py::gil_scoped_acquire gil;
        (*callback)(std::forward<Args>(args)...);
    } catch (py::error_already_set& e) {
        e.discard_as_unraisable(label);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "AffineUI Python callback failed (%s): %s\n",
                     label, e.what());
    } catch (...) {
        std::fprintf(stderr, "AffineUI Python callback failed (%s)\n", label);
    }
}

}  // namespace

PYBIND11_MODULE(_affineui, m) {
    m.doc() = "AffineUI Python bindings";

    m.attr("__version__") = AFFINEUI_PY_VERSION;
    m.def("version", [] { return std::string{affineui::version_string()}; });
    m.def("native_backend", [] { return std::string{"sokol"}; });

    py::class_<PythonCallbackView>(m, "CallbackView")
        .def_property_readonly("is_alive", &PythonCallbackView::is_alive)
        .def("__bool__", &PythonCallbackView::is_alive)
        .def("__getattr__", &PythonCallbackView::getattr)
        .def("__repr__", [](const PythonCallbackView& view) {
            return view.is_alive()
                ? std::string{"<affineui.CallbackView alive>"}
                : std::string{"<affineui.CallbackView expired>"};
        });

    // Painter — an opaque handle to the live painter, plus the drawing calls.
    //
    // The handle is what a paint callback receives. The METHODS are what make it
    // usable from outside the affineui runtime: a native rendering core in its own
    // extension module (the photoedit raster core) drives its drawing through
    // these, rather than by taking a C++ `affineui::Painter&`.
    //
    // That indirection is deliberate, not incidental. A separately-compiled module
    // holding an `affineui::Painter*` would be pinned to this class's exact vtable
    // layout — reorder a virtual and it silently calls the wrong slot — and pybind
    // would need `typeid(affineui::Painter)`, whose typeinfo lives only in the
    // affineui runtime (Painter has a key function). Python loads extension modules
    // RTLD_LOCAL, so such a core cannot borrow it and fails to import outright.
    // A method call over the Python boundary has neither problem.
    //
    // Rects and colors cross as plain tuples so a caller needs no affineui type at
    // all: rect = (x, y, w, h), color = (r, g, b, a) with 0-255 components.
    using PyRect = std::tuple<int, int, int, int>;
    using PyColor = std::tuple<int, int, int, int>;
    const auto to_rect = [](const PyRect& t) {
        return affineui::Rect{std::get<0>(t), std::get<1>(t), std::get<2>(t),
                              std::get<3>(t)};
    };
    const auto to_color = [](const PyColor& t) {
        const auto clamp8 = [](int v) -> std::uint8_t {
            return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        };
        return affineui::Color{clamp8(std::get<0>(t)), clamp8(std::get<1>(t)),
                               clamp8(std::get<2>(t)), clamp8(std::get<3>(t))};
    };

    py::class_<affineui::Painter>(
        m, "Painter",
        "The live painter, passed to App.set_custom_paint handlers. Rects are "
        "(x, y, w, h) tuples; colors are (r, g, b, a) tuples, 0-255. Valid only "
        "for the duration of the paint callback.")
        .def(
            "draw_image",
            [to_rect](affineui::Painter& p, std::uint32_t image_id,
                      const PyRect& dst, const PyRect& src) {
                p.draw_image(image_id, to_rect(dst), to_rect(src));
            },
            py::arg("image_id"), py::arg("dst"), py::arg("src"),
            "Draw an image previously created with App.create_image_rgba.")
        .def(
            "fill_rect",
            [to_rect, to_color](affineui::Painter& p, const PyRect& r,
                                const PyColor& c) {
                p.fill_rect(to_rect(r), to_color(c));
            },
            py::arg("rect"), py::arg("color"))
        .def(
            "stroke_rect",
            [to_rect, to_color](affineui::Painter& p, const PyRect& r,
                                const PyColor& c, float width) {
                p.stroke_rect(to_rect(r), to_color(c), width);
            },
            py::arg("rect"), py::arg("color"), py::arg("width") = 1.0f)
        .def(
            "stroke_line",
            [to_color](affineui::Painter& p, float x0, float y0, float x1,
                       float y1, const PyColor& c, float width) {
                p.stroke_line(x0, y0, x1, y1, to_color(c), width);
            },
            py::arg("x0"), py::arg("y0"), py::arg("x1"), py::arg("y1"),
            py::arg("color"), py::arg("width") = 1.0f);

    m.def("embedded_font_data",
          [](bool bold) {
              const std::string_view data = affineui::embedded_font_data(bold);
              return py::bytes(data.data(), data.size());
          },
          py::arg("bold") = false,
          "Raw bytes of the embedded UI font — for a native core that "
          "rasterizes its own glyphs.");

    // ── Bundled Decius CSS framework ──────────────────────────────────
    // Exposed as module-level `_decius_apply` and `_decius_available`
    // (leading underscore = internal). The Python `class decius:` in
    // __init__.py adds static-method wrappers so users call
    // `ui.decius.apply(app)` / `ui.decius.available()` — no namespace
    // collision with the existing selector-constants class.
    m.def("_decius_available", [] {
        return affineui::decius::available();
    });
    m.def("_decius_apply", [](affineui::App& app) {
        if (!affineui::decius::available()) {
            throw std::runtime_error(
                "Decius bundle was not compiled into affineui_c "
                "(AFFINEUI_BUNDLE_DECIUS=OFF at build time). Load a "
                "stylesheet manually with App.set_stylesheet(...).");
        }
        affineui::decius::apply(app);
    }, py::arg("app"));

    // affinetools attach (docs/AFFINETOOLS_DESIGN.md §3): loopback-only
    // devtools server with token auth via <tempdir>/affineui-tools/. Lets
    // a Python host enable attach without a rebuild; no-ops when the
    // native library was compiled with AFFINEUI_PERF=0.
    m.def("tools_listen",
          [](int port) { return affineui::tools_listen(port); },
          py::arg("port") = 0,
          "Start the affinetools protocol server (idempotent). port=0 binds "
          "an ephemeral port; discovery/auth via the temp-dir session file.");
    m.def("tools_active", [] { return affineui::tools_active(); });
    m.def("tools_port", [] { return affineui::tools_port(); });
    m.def("tools_shutdown", [] { affineui::tools_shutdown(); });

    py::class_<affineui::Color>(m, "Color")
        .def(py::init([](int r, int g, int b, int a) {
                 return affineui::Color{
                     static_cast<std::uint8_t>(r),
                     static_cast<std::uint8_t>(g),
                     static_cast<std::uint8_t>(b),
                     static_cast<std::uint8_t>(a),
                 };
             }),
             py::arg("r") = 0,
             py::arg("g") = 0,
             py::arg("b") = 0,
             py::arg("a") = 255)
        .def_readwrite("r", &affineui::Color::r)
        .def_readwrite("g", &affineui::Color::g)
        .def_readwrite("b", &affineui::Color::b)
        .def_readwrite("a", &affineui::Color::a)
        .def("__repr__", [](const affineui::Color& c) {
            return "Color(r=" + std::to_string(c.r) + ", g=" +
                   std::to_string(c.g) + ", b=" + std::to_string(c.b) +
                   ", a=" + std::to_string(c.a) + ")";
        });

    py::class_<affineui::Size>(m, "Size")
        .def(py::init<int, int>(), py::arg("width") = 0, py::arg("height") = 0)
        .def_readwrite("width", &affineui::Size::width)
        .def_readwrite("height", &affineui::Size::height)
        .def("__repr__", [](const affineui::Size& s) {
            return "Size(width=" + std::to_string(s.width) +
                   ", height=" + std::to_string(s.height) + ")";
        });

    py::class_<affineui::Rect>(m, "Rect")
        .def(py::init<int, int, int, int>(),
             py::arg("x") = 0,
             py::arg("y") = 0,
             py::arg("w") = 0,
             py::arg("h") = 0)
        .def_readwrite("x", &affineui::Rect::x)
        .def_readwrite("y", &affineui::Rect::y)
        .def_readwrite("w", &affineui::Rect::w)
        .def_readwrite("h", &affineui::Rect::h)
        .def("__repr__", [](const affineui::Rect& r) {
            return "Rect(x=" + std::to_string(r.x) +
                   ", y=" + std::to_string(r.y) +
                   ", w=" + std::to_string(r.w) +
                   ", h=" + std::to_string(r.h) + ")";
        });

    py::class_<affineui::Point>(
            m,
            "Point",
            "Integer point in AffineUI CSS/layout coordinates.")
        .def(py::init<int, int>(), py::arg("x") = 0, py::arg("y") = 0)
        .def_readwrite("x", &affineui::Point::x)
        .def_readwrite("y", &affineui::Point::y)
        .def("__repr__", [](const affineui::Point& p) {
            return "Point(x=" + std::to_string(p.x) +
                   ", y=" + std::to_string(p.y) + ")";
        });

    py::class_<affineui::DomHandle>(
            m,
            "DomHandle",
            "Opaque weak handle to a native DOM node. It never owns the node; "
            "check Document.weak_handle_valid(handle) before relying on it.")
        .def(py::init<>())
        .def_readonly("document_id", &affineui::DomHandle::document_id)
        .def_readonly("node_slot", &affineui::DomHandle::node_slot)
        .def_readonly("generation", &affineui::DomHandle::generation)
        .def("__bool__", [](const affineui::DomHandle& handle) {
            return static_cast<bool>(handle);
        });

    py::enum_<affineui::MouseButton>(
            m,
            "MouseButton",
            "Mouse button carried by MouseDown/MouseUp events.")
        .value("Left", affineui::MouseButton::Left)
        .value("Right", affineui::MouseButton::Right)
        .value("Middle", affineui::MouseButton::Middle);

    py::enum_<affineui::Key>(
            m,
            "Key",
            "Portable non-text key code. Printable text arrives as TextInput.")
        .value("Unknown", affineui::Key::Unknown)
        .value("Escape", affineui::Key::Escape)
        .value("Tab", affineui::Key::Tab)
        .value("Enter", affineui::Key::Enter)
        .value("Backspace", affineui::Key::Backspace)
        .value("Delete", affineui::Key::Delete)
        .value("ArrowLeft", affineui::Key::ArrowLeft)
        .value("ArrowRight", affineui::Key::ArrowRight)
        .value("ArrowUp", affineui::Key::ArrowUp)
        .value("ArrowDown", affineui::Key::ArrowDown)
        .value("Home", affineui::Key::Home)
        .value("End", affineui::Key::End)
        .value("A", affineui::Key::A).value("B", affineui::Key::B)
        .value("C", affineui::Key::C).value("D", affineui::Key::D)
        .value("E", affineui::Key::E).value("F", affineui::Key::F)
        .value("G", affineui::Key::G).value("H", affineui::Key::H)
        .value("I", affineui::Key::I).value("J", affineui::Key::J)
        .value("K", affineui::Key::K).value("L", affineui::Key::L)
        .value("M", affineui::Key::M).value("N", affineui::Key::N)
        .value("O", affineui::Key::O).value("P", affineui::Key::P)
        .value("Q", affineui::Key::Q).value("R", affineui::Key::R)
        .value("S", affineui::Key::S).value("T", affineui::Key::T)
        .value("U", affineui::Key::U).value("V", affineui::Key::V)
        .value("W", affineui::Key::W).value("X", affineui::Key::X)
        .value("Y", affineui::Key::Y).value("Z", affineui::Key::Z)
        .value("Digit0", affineui::Key::Digit0)
        .value("Digit1", affineui::Key::Digit1)
        .value("Digit2", affineui::Key::Digit2)
        .value("Digit3", affineui::Key::Digit3)
        .value("Digit4", affineui::Key::Digit4)
        .value("Digit5", affineui::Key::Digit5)
        .value("Digit6", affineui::Key::Digit6)
        .value("Digit7", affineui::Key::Digit7)
        .value("Digit8", affineui::Key::Digit8)
        .value("Digit9", affineui::Key::Digit9)
        .value("Space", affineui::Key::Space)
        .value("Minus", affineui::Key::Minus)
        .value("Equal", affineui::Key::Equal)
        .value("BracketLeft", affineui::Key::BracketLeft)
        .value("BracketRight", affineui::Key::BracketRight);

    py::enum_<affineui::EventType>(
            m,
            "EventType",
            "Event kind accepted by App.dispatch and Document.dispatch.")
        .value("None", affineui::EventType::None)
        .value("MouseMove", affineui::EventType::MouseMove)
        .value("MouseDown", affineui::EventType::MouseDown)
        .value("MouseUp", affineui::EventType::MouseUp)
        .value("MouseWheel", affineui::EventType::MouseWheel)
        .value("KeyDown", affineui::EventType::KeyDown)
        .value("KeyUp", affineui::EventType::KeyUp)
        .value("TextInput", affineui::EventType::TextInput)
        .value("Resize", affineui::EventType::Resize)
        .value("FocusLost", affineui::EventType::FocusLost)
        .value("FocusGained", affineui::EventType::FocusGained)
        .value("Composition", affineui::EventType::Composition);

    py::class_<affineui::Event>(
            m,
            "Event",
            "Value object used to drive AffineUI from tests, custom hosts, "
            "or Python-native event loops.")
        .def(py::init<>())
        .def_readwrite("type", &affineui::Event::type)
        .def_readwrite("pos", &affineui::Event::pos)
        .def_readwrite("button", &affineui::Event::button)
        .def_readwrite("wheel_dx", &affineui::Event::wheel_dx)
        .def_readwrite("wheel_dy", &affineui::Event::wheel_dy)
        .def_readwrite("key", &affineui::Event::key)
        .def_readwrite("key_code", &affineui::Event::key_code)
        .def_readwrite("text", &affineui::Event::text)
        .def_readwrite("composition_cursor",
                       &affineui::Event::composition_cursor)
        .def_readwrite("composition_clause_begin",
                       &affineui::Event::composition_clause_begin)
        .def_readwrite("composition_clause_end",
                       &affineui::Event::composition_clause_end)
        .def_readwrite("shift", &affineui::Event::shift)
        .def_readwrite("ctrl", &affineui::Event::ctrl)
        .def_readwrite("alt", &affineui::Event::alt)
        .def_readwrite("super", &affineui::Event::super);

    py::class_<affineui::DispatchResult>(
            m,
            "DispatchResult",
            "Value returned from event dispatch. It is a snapshot, not a "
            "live reference into the document.")
        .def_readonly("redraw_requested",
                      &affineui::DispatchResult::redraw_requested)
        .def_readonly("invalidate_view",
                      &affineui::DispatchResult::invalidate_view)
        .def_readonly("event_consumed",
                      &affineui::DispatchResult::event_consumed);

    py::class_<affineui::Document::HoverInfo>(
            m,
            "HoverInfo",
            "Snapshot describing one hovered element in the DOM chain.")
        .def_readonly("valid", &affineui::Document::HoverInfo::valid)
        .def_readonly("tag", &affineui::Document::HoverInfo::tag)
        .def_readonly("elem_id", &affineui::Document::HoverInfo::elem_id)
        .def_readonly("classes", &affineui::Document::HoverInfo::classes)
        .def_readonly("attrs", &affineui::Document::HoverInfo::attrs)
        .def_readonly("bounds", &affineui::Document::HoverInfo::bounds);

    py::class_<affineui::Document>(m, "Document")
        .def(py::init<>())
        .def("set_html", [](affineui::Document& doc, const std::string& html) {
            doc.set_html(html);
        })
        .def("set_user_stylesheet",
             [](affineui::Document& doc, const std::string& css,
                py::object base_url) {
                 if (base_url.is_none()) {
                     doc.set_user_stylesheet(css);
                 } else {
                     doc.set_user_stylesheet(css, base_url.cast<std::string>());
                 }
             },
             py::arg("css"), py::arg("base_url") = py::none(),
             "Install the user stylesheet. base_url (optional) is the "
             "sheet's own location so its relative url()s resolve like a "
             "<link>ed sheet's.")
        .def("reload_stylesheets", &affineui::Document::reload_stylesheets)
        .def("layout",
             [](affineui::Document& doc, int width, int height) {
                 doc.layout(width, height);
             },
             py::arg("width"),
             py::arg("height") = 0)
        .def("content_size", &affineui::Document::content_size)
        .def("set_attribute_by_id",
             [](affineui::Document& doc,
                const std::string& id,
                const std::string& name,
                const std::string& value) {
                 return doc.set_attribute_by_id(id, name, value);
             })
        .def("remove_attribute_by_id",
             [](affineui::Document& doc,
                const std::string& id,
                const std::string& name) {
                 return doc.remove_attribute_by_id(id, name);
             })
        .def("set_text_by_id",
             [](affineui::Document& doc,
                const std::string& id,
                const std::string& text) {
                 return doc.set_text_by_id(id, text);
             })
        .def("take_dirty_rects", &affineui::Document::take_dirty_rects)
        .def("take_paint_dirty", &affineui::Document::take_paint_dirty)
        .def("has_active_animations",
             &affineui::Document::has_active_animations)
        .def("attach_script", &affineui::Document::attach_script)
        .def("detach_script", &affineui::Document::detach_script)
        .def("clear_scripts", &affineui::Document::clear_scripts)
        .def("dispatch",
             [](affineui::Document& doc, const affineui::Event& ev) {
                 return doc.dispatch(ev);
             },
             py::arg("event"),
             "Dispatch an event directly to this document and return a "
             "DispatchResult snapshot.")
        .def("hovered_info",
             [](const affineui::Document& doc) {
                 return doc.hovered_info();
             },
             "Return a HoverInfo snapshot for the deepest hovered element.")
        .def("hovered_info_chain",
             [](const affineui::Document& doc) {
                 return doc.hovered_info_chain();
             },
             "Return HoverInfo snapshots from deepest hovered element toward "
             "the root.")
        .def("weak_handle_for_id",
             [](affineui::Document& doc, const std::string& id) {
                 return doc.weak_handle_for_id(id);
             },
             py::arg("id"),
             "Return an opaque weak handle for an element id. Invalid handles "
             "are empty and safe to pass back to weak_handle_valid().")
        .def("weak_handle_valid",
             [](const affineui::Document& doc, affineui::DomHandle handle) {
                 return doc.weak_handle_valid(handle);
             },
             py::arg("handle"),
             "Return whether a DomHandle still resolves inside this document.")
        .def("hovered_cursor",
             &affineui::Document::hovered_cursor,
             "Cursor the OS should display under the hovered element "
             "(0=default 1=pointer 2=text 3=crosshair 4=move 5=not-allowed "
             "6=ew-resize 7=ns-resize 8=nwse-resize).")
        .def("text_input_active",
             &affineui::Document::text_input_active,
             "True while an editable text control is focused; the host "
             "should enable platform text input / IME while this holds "
             "(see docs/IME_ARCHITECTURE.md).")
        .def("caret_rect",
             py::overload_cast<>(&affineui::Document::caret_rect, py::const_),
             "Caret rectangle of the focused text control in document CSS "
             "points, for IME candidate-window placement (w<=0 when no "
             "text control is focused).")
        .def("set_caret_blink_interval",
             &affineui::Document::set_caret_blink_interval,
             py::arg("milliseconds"),
             "Set the caret visibility half-cycle in milliseconds; zero "
             "keeps the caret continuously visible.")
        .def("caret_blink_interval",
             &affineui::Document::caret_blink_interval)
        .def("tick_caret_blink",
             &affineui::Document::tick_caret_blink,
             "Advance caret timing for a custom/headless Document driver. "
             "App hosts do this automatically.")
        .def("text_editing_active",
             &affineui::Document::text_editing_active,
             "True while a text control has keyboard focus — app-level "
             "keyboard shortcuts should stand down.")
        .def("dock_layout", &affineui::Document::dock_layout,
             "Snapshot of the CURRENT dock arrangement, read live from the "
             "DOM. Opaque: pass it back via View.set_dock_layout_provider so "
             "rebuilds replay the user's arrangement (tearoffs, splits) "
             "instead of the declared seed.")
        .def("dock_overrides", &affineui::Document::dock_overrides,
             "Runtime placement overrides recorded by dock gestures, as a "
             "list of (panel_id, DockPlacement) pairs. Feed them back via "
             "View.set_dock_placement_provider.")
        .def("dock_override", &affineui::Document::dock_override,
             py::arg("panel_id"),
             "The placement override for ONE panel (.present is False when it "
             "has none). The single-panel form of dock_overrides().")
        .def("dock_active_tab", &affineui::Document::dock_active_tab,
             py::arg("pane_id"),
             "The active tab of a dock leaf ('' = the primary panel). Feed it "
             "back via View.set_dock_active_tab_provider to restore which tab "
             "was selected.")
        .def("take_dock_structure_changed",
             &affineui::Document::take_dock_structure_changed,
             "True once (consumes the flag) after a dock gesture "
             "restructured the DOM. Apps driving their own rebuild loop "
             "should rebuild the whole view when this fires.")
        .def("reset_dock_state", &affineui::Document::reset_dock_state,
             "Forget all runtime dock overrides and remembered active tabs "
             "(Reset Workspace). Rebuild once WITHOUT wiring the dock "
             "providers so the declared seed layout wins.");

    // ── Declarative docking (document_view / dockpanel) ─────────────────
    py::enum_<affineui::Dock>(m, "Dock")
        .value("Left", affineui::Dock::Left)
        .value("Right", affineui::Dock::Right)
        .value("Top", affineui::Dock::Top)
        .value("Bottom", affineui::Dock::Bottom)
        .value("Tab", affineui::Dock::Tab);

    py::enum_<affineui::DockCorner>(m, "DockCorner")
        .value("TopLeft", affineui::DockCorner::TopLeft)
        .value("TopRight", affineui::DockCorner::TopRight)
        .value("BottomLeft", affineui::DockCorner::BottomLeft)
        .value("BottomRight", affineui::DockCorner::BottomRight);

    py::class_<affineui::Document::DockPlacement>(m, "DockPlacement")
        .def(py::init<>())
        .def_readwrite("present", &affineui::Document::DockPlacement::present)
        .def_readwrite("floating",
                       &affineui::Document::DockPlacement::floating)
        .def_readwrite("parent", &affineui::Document::DockPlacement::parent)
        .def_readwrite("side", &affineui::Document::DockPlacement::side)
        .def_readwrite("size", &affineui::Document::DockPlacement::size)
        .def_readwrite("x", &affineui::Document::DockPlacement::x)
        .def_readwrite("y", &affineui::Document::DockPlacement::y)
        .def_readwrite("w", &affineui::Document::DockPlacement::w)
        .def_readwrite("h", &affineui::Document::DockPlacement::h);

    // Opaque: harvested from Document.dock_layout(), handed back verbatim to
    // View.set_dock_layout_provider. Apps never inspect the tree from Python.
    py::class_<affineui::Document::DockLayout>(m, "DockLayout")
        .def(py::init<>())
        .def_readonly("present", &affineui::Document::DockLayout::present);

    py::class_<affineui::DockHandle>(m, "DockHandle")
        .def_readonly("id", &affineui::DockHandle::id)
        .def("__bool__",
             [](const affineui::DockHandle& h) { return bool(h); })
        .def("toolbar",
             [](affineui::DockHandle& h, py::function build) {
                 auto cb = keep_python_function(std::move(build));
                 h.toolbar([cb = std::move(cb)](affineui::View& v) {
                     py::gil_scoped_acquire gil;
                     (*cb)(callback_view(v));
                 });
                 return h;
             },
             py::arg("build"),
             "Declare this pane's tab toolbar; build fills the strip.");

    py::class_<affineui::DockLocation>(m, "DockLocation")
        .def(py::init<>())
        .def_static("docked", &affineui::DockLocation::docked,
                    py::arg("side"), py::arg("px") = 0,
                    "Docked on a side of the document (or of .in_(panel)).")
        .def_static("tab", &affineui::DockLocation::tab,
                    "A tab sharing another panel's pane (chain .in_(panel)).")
        .def_static("floating", &affineui::DockLocation::floating,
                    py::arg("anchor"), py::arg("pos"), py::arg("size"),
                    "Floating, anchored to a corner; pos counts inward from "
                    "that corner, size is (w, h) px.")
        .def_static("tearoff", &affineui::DockLocation::tearoff,
                    py::arg("anchor"), py::arg("pos"), py::arg("size"))
        // `in` is a Python keyword — bind the fluent setter as in_().
        .def("in_",
             [](affineui::DockLocation& l, const affineui::DockHandle& h) {
                 return l.in(h);
             },
             py::arg("panel"), py::return_value_policy::reference_internal,
             "Place relative to another declared panel (chainable).")
        .def("sized", &affineui::DockLocation::sized, py::arg("px"),
             py::return_value_policy::reference_internal)
        .def("dragging_with",
             [](affineui::DockLocation& l, const affineui::DockHandle& h) {
                 return l.dragging_with(h);
             },
             py::arg("panel"), py::return_value_policy::reference_internal,
             "Tearoff: drag this panel along with another (chainable).")
        .def("tearout_size", &affineui::DockLocation::tearout_size,
             py::arg("px"), py::return_value_policy::reference_internal);

    py::enum_<affineui::ViewTheme>(m, "ViewTheme")
        .value("Plain", affineui::ViewTheme::Plain)
        .value("Bootstrap", affineui::ViewTheme::Bootstrap)
        .value("Decius", affineui::ViewTheme::Decius);

    py::enum_<affineui::WidgetKind>(m, "WidgetKind")
        .value("Root", affineui::WidgetKind::Root)
        .value("Container", affineui::WidgetKind::Container)
        .value("Text", affineui::WidgetKind::Text)
        .value("RawHtml", affineui::WidgetKind::RawHtml)
        .value("Heading", affineui::WidgetKind::Heading)
        .value("Panel", affineui::WidgetKind::Panel)
        .value("Button", affineui::WidgetKind::Button)
        .value("Checkbox", affineui::WidgetKind::Checkbox)
        .value("Slider", affineui::WidgetKind::Slider)
        .value("Knob", affineui::WidgetKind::Knob)
        .value("TextInput", affineui::WidgetKind::TextInput)
        .value("TextArea", affineui::WidgetKind::TextArea)
        .value("Dropdown", affineui::WidgetKind::Dropdown)
        .value("ButtonGroup", affineui::WidgetKind::ButtonGroup)
        .value("VirtualList", affineui::WidgetKind::VirtualList)
        .value("Card", affineui::WidgetKind::Card);

    py::enum_<affineui::DocumentScript>(m, "DocumentScript")
        .value("UiControls", affineui::DocumentScript::UiControls);

    py::enum_<affineui::RemotePatchOp>(m, "RemotePatchOp")
        .value("CreateElement", affineui::RemotePatchOp::CreateElement)
        .value("CreateText", affineui::RemotePatchOp::CreateText)
        .value("Remove", affineui::RemotePatchOp::Remove)
        .value("SetText", affineui::RemotePatchOp::SetText)
        .value("SetAttribute", affineui::RemotePatchOp::SetAttribute)
        .value("RemoveAttribute", affineui::RemotePatchOp::RemoveAttribute);

    py::class_<affineui::RemotePatch>(m, "RemotePatch")
        .def_readonly("op", &affineui::RemotePatch::op)
        .def_readonly("id", &affineui::RemotePatch::id)
        .def_readonly("parent_id", &affineui::RemotePatch::parent_id)
        .def_readonly("tag", &affineui::RemotePatch::tag)
        .def_readonly("name", &affineui::RemotePatch::name)
        .def_readonly("value", &affineui::RemotePatch::value)
        .def_readonly("index", &affineui::RemotePatch::index);

    py::class_<affineui::RemotePatchQueue>(m, "RemotePatchQueue")
        .def(py::init<>())
        .def("clear", &affineui::RemotePatchQueue::clear)
        .def("empty", &affineui::RemotePatchQueue::empty)
        .def("size", &affineui::RemotePatchQueue::size)
        .def("patches", &affineui::RemotePatchQueue::patches,
             py::return_value_policy::reference_internal)
        .def("to_json", &affineui::RemotePatchQueue::to_json);

    // ── Virtual list / tree providers ──────────────────────────────────────
    py::enum_<affineui::Axis>(m, "Axis")
        .value("Vertical", affineui::Axis::Vertical)
        .value("Horizontal", affineui::Axis::Horizontal);

    py::enum_<affineui::SelectMod>(m, "SelectMod")
        .value("Replace", affineui::SelectMod::Replace)
        .value("Toggle", affineui::SelectMod::Toggle)
        .value("Range", affineui::SelectMod::Range);

    py::enum_<affineui::DropPos>(m, "DropPos")
        .value("Before", affineui::DropPos::Before)
        .value("Into", affineui::DropPos::Into)
        .value("After", affineui::DropPos::After);

    py::class_<affineui::IndexSelection>(m, "IndexSelection")
        .def(py::init<>())
        .def("apply", &affineui::IndexSelection::apply, py::arg("index"),
             py::arg("mod"), py::arg("count") = 0)
        .def("clear", &affineui::IndexSelection::clear)
        .def("contains", &affineui::IndexSelection::contains)
        .def("size", &affineui::IndexSelection::size)
        .def("anchor", &affineui::IndexSelection::anchor)
        .def("on_change",
             [](affineui::IndexSelection& s, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 s.on_change([cb] { call_python_function("on_change", cb); });
             });

    // Providers are Trackable (non-copyable, non-movable): Python owns the
    // instance and must keep it alive as long as the widget uses it. Fluent
    // setters return the provider itself (reference) so chaining works from
    // Python too.
    py::class_<affineui::VirtualListProvider>(m, "VirtualListProvider")
        .def(py::init<>())
        .def("on_item_count",
             [](affineui::VirtualListProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_item_count([cb]() -> std::size_t {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)().cast<std::size_t>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_item_count"); return 0;
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_item_size",
             [](affineui::VirtualListProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_item_size([cb](std::size_t i) -> double {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)(i).cast<double>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_item_size"); return 0.0;
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_item_text",
             [](affineui::VirtualListProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_item_text([cb](std::size_t i) -> std::string {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)(i).cast<std::string>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_item_text"); return {};
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_build_item",
             [](affineui::VirtualListProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_build_item([cb](affineui::View& v, std::size_t i) {
                     call_python_function(
                         "on_build_item", cb, callback_view(v), i);
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_is_selected",
             [](affineui::VirtualListProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_is_selected([cb](std::size_t i) -> bool {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)(i).cast<bool>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_is_selected"); return false;
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_activate",
             [](affineui::VirtualListProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_activate([cb](std::size_t i, affineui::SelectMod m) {
                     call_python_function("on_activate", cb, i, m);
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("default_item_size",
             [](affineui::VirtualListProvider& p, double px) {
                 p.default_item_size(px);
                 return &p;
             }, py::return_value_policy::reference)
        .def("checkboxes",
             [](affineui::VirtualListProvider& p, bool on) {
                 p.checkboxes(on);
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_is_checked",
             [](affineui::VirtualListProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_is_checked([cb](std::size_t i) -> bool {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)(i).cast<bool>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_is_checked"); return false;
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_set_checked",
             [](affineui::VirtualListProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_set_checked([cb](std::size_t i, bool on) {
                     call_python_function("on_set_checked", cb, i, on);
                 });
                 return &p;
             }, py::return_value_policy::reference);

    py::class_<affineui::VirtualTreeProvider>(m, "VirtualTreeProvider")
        .def(py::init<>())
        .def("on_item_count",
             [](affineui::VirtualTreeProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_item_count([cb]() -> std::size_t {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)().cast<std::size_t>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_item_count"); return 0;
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_item_text",
             [](affineui::VirtualTreeProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_item_text([cb](std::size_t i) -> std::string {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)(i).cast<std::string>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_item_text"); return {};
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_depth",
             [](affineui::VirtualTreeProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_depth([cb](std::size_t i) -> int {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)(i).cast<int>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_depth"); return 0;
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_is_expandable",
             [](affineui::VirtualTreeProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_is_expandable([cb](std::size_t i) -> bool {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)(i).cast<bool>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_is_expandable"); return false;
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_is_expanded",
             [](affineui::VirtualTreeProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_is_expanded([cb](std::size_t i) -> bool {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)(i).cast<bool>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_is_expanded"); return false;
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_toggle",
             [](affineui::VirtualTreeProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_toggle([cb](std::size_t i) {
                     call_python_function("on_toggle", cb, i);
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_is_selected",
             [](affineui::VirtualTreeProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_is_selected([cb](std::size_t i) -> bool {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)(i).cast<bool>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_is_selected"); return false;
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_activate",
             [](affineui::VirtualTreeProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_activate([cb](std::size_t i, affineui::SelectMod m) {
                     call_python_function("on_activate", cb, i, m);
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("default_item_size",
             [](affineui::VirtualTreeProvider& p, double px) {
                 p.default_item_size(px);
                 return &p;
             }, py::return_value_policy::reference)
        .def("checkboxes",
             [](affineui::VirtualTreeProvider& p, bool on) {
                 p.checkboxes(on);
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_is_checked",
             [](affineui::VirtualTreeProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_is_checked([cb](std::size_t i) -> bool {
                     py::gil_scoped_acquire gil;
                     try { return (*cb)(i).cast<bool>(); }
                     catch (py::error_already_set& e) {
                         e.discard_as_unraisable("on_is_checked"); return false;
                     }
                 });
                 return &p;
             }, py::return_value_policy::reference)
        .def("on_set_checked",
             [](affineui::VirtualTreeProvider& p, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 p.on_set_checked([cb](std::size_t i, bool on) {
                     call_python_function("on_set_checked", cb, i, on);
                 });
                 return &p;
             }, py::return_value_policy::reference);

    py::class_<affineui::VirtualListOptions>(m, "VirtualListOptions")
        .def(py::init<>())
        .def_readwrite("item_count", &affineui::VirtualListOptions::item_count)
        .def_readwrite("first_item", &affineui::VirtualListOptions::first_item)
        .def_readwrite("visible_items", &affineui::VirtualListOptions::visible_items)
        .def_readwrite("overscan", &affineui::VirtualListOptions::overscan)
        .def_readwrite("item_size", &affineui::VirtualListOptions::item_size)
        .def_readwrite("item_sizes", &affineui::VirtualListOptions::item_sizes);

    py::class_<affineui::WidgetRef>(m, "WidgetRef")
        .def("__bool__", [](const affineui::WidgetRef& ref) {
            return static_cast<bool>(ref);
        })
        .def("valid", [](const affineui::WidgetRef& ref) {
            return static_cast<bool>(ref);
        })
        .def("id", [](const affineui::WidgetRef& ref) {
            return ref.id().value;
        })
        .def("name", [](const affineui::WidgetRef& ref) {
            return std::string{ref.name()};
        })
        .def("attr_value",
             [](const affineui::WidgetRef& ref, const std::string& name,
                const std::string& fallback) {
                 return std::string{ref.attr_value(name, fallback)};
             },
             py::arg("name"), py::arg("fallback") = "",
             "Read an attribute value (empty/fallback if the node is gone).")
        .def("text_value", [](const affineui::WidgetRef& ref) {
            return std::string{ref.text_value()};
        }, "Read the node's text (empty if the node is gone).")
        .def("has_attr", [](const affineui::WidgetRef& ref, const std::string& name) {
            return ref.has_attr(name);
        }, py::arg("name"))
        .def("named", [](affineui::WidgetRef& ref, const std::string& name) -> affineui::WidgetRef& {
            return ref.named(name);
        }, py::return_value_policy::reference_internal)
        .def("clear", [](affineui::WidgetRef& ref) -> affineui::WidgetRef& {
            return ref.clear();
        }, py::return_value_policy::reference_internal)
        .def("text", [](affineui::WidgetRef& ref, const std::string& text) -> affineui::WidgetRef& {
            return ref.text(text);
        }, py::return_value_policy::reference_internal)
        .def("attr",
             [](affineui::WidgetRef& ref,
                const std::string& name,
                const std::string& value) -> affineui::WidgetRef& {
                 return ref.attr(name, value);
             },
             py::return_value_policy::reference_internal)
        .def("remove_attr",
             [](affineui::WidgetRef& ref,
                const std::string& name) -> affineui::WidgetRef& {
                 return ref.remove_attr(name);
             },
             py::return_value_policy::reference_internal)
        .def("selector",
             [](affineui::WidgetRef& ref,
                const std::string& name,
                const std::string& value) -> affineui::WidgetRef& {
                 return ref.selector(name, value);
             },
             py::return_value_policy::reference_internal)
        .def("cls", [](affineui::WidgetRef& ref, const std::string& classes) -> affineui::WidgetRef& {
            return ref.cls(classes);
        }, py::return_value_policy::reference_internal)
        // Append one class, keeping what the framework already put there. cls()
        // REPLACES the list — use this when adding an app class to a framework
        // widget, or you will silently drop the classes it relies on.
        .def("add_class", [](affineui::WidgetRef& ref, const std::string& token)
                 -> affineui::WidgetRef& { return ref.add_class(token); },
             py::arg("token"), py::return_value_policy::reference_internal)
        .def("on_click",
             [](affineui::WidgetRef& ref, py::function cb) -> affineui::WidgetRef& {
                 auto callback = keep_python_function(std::move(cb));
                 return ref.on_click([callback = std::move(callback)] {
                     call_python_function("affineui.on_click", callback);
                 });
             },
             py::return_value_policy::reference_internal)
        .def("on_change",
             [](affineui::WidgetRef& ref, py::function cb) -> affineui::WidgetRef& {
                 auto callback = keep_python_function(std::move(cb));
                 return ref.on_change([callback = std::move(callback)](std::string_view value) {
                     call_python_function("affineui.on_change", callback,
                                          std::string(value));
                 });
             },
             py::return_value_policy::reference_internal)
        .def("append",
             [](affineui::WidgetRef& ref, py::function build) -> affineui::WidgetRef& {
                 auto callback = keep_python_function(std::move(build));
                 return ref.append([callback = std::move(callback)](affineui::View& view) {
                     py::gil_scoped_acquire gil;
                     (*callback)(callback_view(view));
                 });
             },
             py::return_value_policy::reference_internal)
        .def("replace",
             [](affineui::WidgetRef& ref, py::function build) -> affineui::WidgetRef& {
                 auto callback = keep_python_function(std::move(build));
                 return ref.replace([callback = std::move(callback)](affineui::View& view) {
                     py::gil_scoped_acquire gil;
                     (*callback)(callback_view(view));
                 });
             },
             py::return_value_policy::reference_internal)
        .def("find_widget",
             [](const affineui::WidgetRef& ref, const std::string& name) {
                 return ref.find_widget(name);
             },
             py::arg("name"),
             "Find a descendant widget by key. Empty refs are safe.");

    py::class_<affineui::View>(m, "View")
        .def(py::init<affineui::ViewTheme>(),
             // Default matches the C++ default in view.h — Decius is the
             // framework the compile-time bundle ships CSS + fonts for,
             // so a bare `ui.View()` produces a styled UI out of the box.
             py::arg("theme") = affineui::ViewTheme::Decius)
        .def("clear", &affineui::View::clear)
        .def("selector",
             [](affineui::View& view,
                const std::string& name,
                const std::string& value) -> affineui::View& {
                 return view.selector(name, value);
             },
             py::return_value_policy::reference_internal)
        .def("begin",
             [](affineui::View& view, affineui::RemotePatchQueue* queue) {
                 view.begin(queue);
             },
             py::arg("patches") = nullptr)
        .def("end", &affineui::View::end)
        .def("set_theme", &affineui::View::set_theme)
        .def("diagnostics", &affineui::View::diagnostics,
             py::return_value_policy::reference_internal)
        .def("clear_diagnostics", &affineui::View::clear_diagnostics)
        .def("heading",
             [](affineui::View& view,
                int level,
                const std::string& text,
                const std::string& classes,
                const std::string& key) {
                 return view.heading(level, text, classes, key);
             },
             py::arg("level"),
             py::arg("text"),
             py::arg("classes") = "",
             py::arg("key") = "",
             "Add a heading and return a stable WidgetRef tied to this View.")
        .def("paragraph",
             [](affineui::View& view,
                const std::string& text,
                const std::string& classes,
                const std::string& key) {
                 return view.paragraph(text, classes, key);
             },
             py::arg("text"),
             py::arg("classes") = "",
             py::arg("key") = "",
             "Add a paragraph and return a stable WidgetRef tied to this View.")
        .def("text",
             [](affineui::View& view,
                const std::string& text,
                const std::string& key) {
                 return view.text(text, key);
             },
             py::arg("text"),
             py::arg("key") = "",
             "Add a bare text node and return a WidgetRef tied to this View.")
        .def("set_framework_version",
             [](affineui::View& view, const std::string& version) {
                 view.set_framework_version(version);
             },
             py::arg("version"),
             "Pin the CSS framework version this view targets (e.g. \"0.6.2\" "
             "for Decius). Empty = personality default.")
        .def("framework_version",
             [](const affineui::View& view) {
                 return std::string{view.framework_version()};
             })
        .def("html",
             [](affineui::View& view,
                const std::string& markup,
                const std::string& key) {
                 return view.html(markup, key);
             },
             py::arg("markup"),
             py::arg("key") = "",
             "Append trusted raw HTML to the current View. The markup is "
             "parsed when the View is loaded into the App document.")
        .def("button",
             [](affineui::View& view,
                const std::string& label,
                bool primary,
                const std::string& key) {
                 return view.button(label, primary, key);
             },
             py::arg("label"),
             py::arg("primary") = false,
             py::arg("key") = "",
             "Add a button. The returned WidgetRef weakly invalidates when "
             "the View is destroyed.")
        .def("checkbox",
             [](affineui::View& view,
                const std::string& label,
                bool checked,
                const std::string& key) {
                 return view.checkbox(label, checked, key);
             },
             py::arg("label"),
             py::arg("checked"),
             py::arg("key") = "",
             "Add a checkbox and return a WidgetRef tied to this View.")
        .def("toggle",
             [](affineui::View& view,
                const std::string& label,
                bool on,
                const std::string& key) {
                 return view.toggle(label, on, key);
             },
             py::arg("label"),
             py::arg("on"),
             py::arg("key") = "",
             "Add an on/off switch (checkbox semantics, slide presentation).")
        .def("combo",
             [](affineui::View& view,
                const std::string& label,
                double value,
                double step,
                const std::string& key,
                bool linear) {
                 return view.combo(label, value, step, key, linear);
             },
             py::arg("label"),
             py::arg("value"),
             py::arg("step") = 0.01,
             py::arg("key") = "",
             py::arg("linear") = false,
             "Add a bare drag-scrub numeric combo (no field/label wrapper). "
             "linear=True scrubs at a constant step/pixel (for rotation "
             "degrees etc.); the default accelerates with the value's "
             "magnitude.")
        .def("colorfield",
             [](affineui::View& view,
                const std::string& label,
                const std::string& value,
                const std::string& key) {
                 return view.colorfield(label, value, key);
             },
             py::arg("label"),
             py::arg("value") = "",
             py::arg("key") = "",
             "Add the Decius color field (chip + editable hex + picker "
             "popover). Degrades to a native color input off-Decius.")
        .def("input",
             [](affineui::View& view,
                const std::string& label,
                const std::string& value,
                const std::string& type,
                const std::string& key) {
                 return view.input(label, value, type, key);
             },
             py::arg("label"),
             py::arg("value") = "",
             py::arg("type") = "text",
             py::arg("key") = "",
             "Add a text-like input and return a WidgetRef tied to this View.")
        .def("password",
             [](affineui::View& view,
                const std::string& label,
                const std::string& value,
                const std::string& key) {
                 return view.password(label, value, key);
             },
             py::arg("label"),
             py::arg("value") = "",
             py::arg("key") = "",
             "Add a password input and return a WidgetRef tied to this View.")
        .def("textarea",
             [](affineui::View& view,
                const std::string& label,
                const std::string& value,
                int rows,
                const std::string& key) {
                 return view.textarea(label, value, rows, key);
             },
             py::arg("label"),
             py::arg("value") = "",
             py::arg("rows") = 3,
             py::arg("key") = "",
             "Add a textarea and return a WidgetRef tied to this View.")
        .def("dropdown",
             [](affineui::View& view,
                const std::string& label,
                const std::vector<std::string>& options,
                const std::string& selected,
                const std::string& key) {
                 return view.dropdown(label, options, selected, key);
             },
             py::arg("label"),
             py::arg("options"),
             py::arg("selected") = "",
             py::arg("key") = "",
             "Add a dropdown/select and return a WidgetRef tied to this View.")
        .def("button_group",
             [](affineui::View& view,
                const std::string& label,
                const std::vector<std::string>& options,
                const std::string& selected,
                const std::string& key) {
                 return view.button_group(label, options, selected, key);
             },
             py::arg("label"),
             py::arg("options"),
             py::arg("selected") = "",
             py::arg("key") = "",
             "Add a mutually-exclusive button group and return a WidgetRef.")
        .def("virtual_list",
             [](affineui::View& view, const std::string& key,
                affineui::VirtualListProvider& provider, affineui::Axis axis,
                const std::string& classes) {
                 return view.virtual_list(key, provider, axis, classes);
             },
             py::arg("key"), py::arg("provider"),
             py::arg("axis") = affineui::Axis::Vertical,
             py::arg("classes") = "",
             py::keep_alive<1, 3>(),  // provider kept alive by the view
             "Add a recycling virtual list bridged to a VirtualListProvider. "
             "Only the rows under the viewport (plus overscan) are built; the "
             "list follows the scrollbar/wheel/keyboard automatically.")
        .def("virtual_tree",
             [](affineui::View& view, const std::string& key,
                affineui::VirtualTreeProvider& provider,
                const std::string& classes) {
                 return view.virtual_tree(key, provider, classes);
             },
             py::arg("key"), py::arg("provider"), py::arg("classes") = "",
             py::keep_alive<1, 3>(),
             "Add a recycling virtual tree over a VirtualTreeProvider's "
             "flattened, currently-expanded nodes.")
        .def("virtual_string_list",
             [](affineui::View& view, const std::string& key,
                const std::vector<std::string>& items, double item_size,
                affineui::IndexSelection* selection,
                affineui::IndexSelection* checked, const std::string& classes) {
                 affineui::View::StringListOptions opts;
                 opts.item_size = item_size;
                 opts.selection = selection;
                 opts.checked = checked;
                 opts.classes = classes;
                 return view.virtual_list(key, items, opts);
             },
             py::arg("key"), py::arg("items"), py::arg("item_size") = 24.0,
             py::arg("selection") = nullptr, py::arg("checked") = nullptr,
             py::arg("classes") = "",
             py::keep_alive<1, 5>(),
             py::keep_alive<1, 6>(),
             "Display an array of strings as a virtual list. Pass a selection "
             "IndexSelection for click selection, or checked for checkboxes.")
        .def("slider",
             [](affineui::View& view,
                const std::string& label,
                double value,
                double min,
                double max,
                const std::string& key) {
                 return view.slider(label, value, min, max, key);
             },
             py::arg("label"),
             py::arg("value"),
             py::arg("min") = 0.0,
             py::arg("max") = 1.0,
             py::arg("key") = "",
             "Add a slider and return a WidgetRef tied to this View.")
        .def("knob",
             [](affineui::View& view,
                const std::string& label,
                double value,
                double min,
                double max,
                bool bipolar,
                const std::string& key) {
                 return view.knob(label, value, min, max, bipolar, key);
             },
             py::arg("label"),
             py::arg("value"),
             py::arg("min") = 0.0,
             py::arg("max") = 1.0,
             py::arg("bipolar") = false,
             py::arg("key") = "",
             "Add a rotary knob and return a WidgetRef tied to this View.")
        .def("container",
             [](affineui::View& view,
                const std::string& classes,
                const std::string& key,
                py::object build) {
                 if (build.is_none()) {
                     return view.container_ref(classes, key);
                 }
                 auto scope = view.container(classes, key);
                 auto ref = scope.ref();
                 build(callback_view(view));
                 return ref;
             },
             py::arg("classes") = "",
             py::arg("key") = "",
             py::arg("build") = py::none(),
             "Add a generic container. If build is supplied it is called "
             "immediately with the same View.")
        .def("canvas",
             [](affineui::View& view,
                const std::string& paint_name,
                const std::string& classes,
                const std::string& key) {
                 return view.canvas(paint_name, classes, key);
             },
             py::arg("paint_name"),
             py::arg("classes") = "",
             py::arg("key") = "",
             "Add a custom-paint (canvas) surface. The handler registered "
             "under paint_name (App.set_custom_paint or a native core's "
             "attach) draws the element's content each frame; per-frame "
             "updates flow through App.request_custom_repaint without "
             "touching the DOM.")
        .def("panel",
             [](affineui::View& view, const std::string& key, py::object build) {
                 if (build.is_none()) {
                     return view.panel_ref(key);
                 }
                 auto scope = view.panel(key);
                 auto ref = scope.ref();
                 build(callback_view(view));
                 return ref;
             },
             py::arg("key") = "",
             py::arg("build") = py::none(),
             "Add a framework-default panel. If build is supplied it is "
             "called immediately with the same View.")
        .def("element",
             [](affineui::View& view,
                const std::string& tag,
                const std::string& classes,
                const std::string& key,
                py::object build) {
                 if (build.is_none()) {
                     return view.element_ref(tag, classes, key);
                 }
                 auto scope = view.element(tag, classes, key);
                 auto ref = scope.ref();
                 build(callback_view(view));
                 return ref;
             },
             py::arg("tag"),
             py::arg("classes") = "",
             py::arg("key") = "",
             py::arg("build") = py::none(),
             "Add an arbitrary element. If build is supplied it is called "
             "immediately with the same View.")
        .def("card",
             [](affineui::View& view,
                const std::string& title,
                const std::string& classes,
                const std::string& key,
                py::object build) {
                 auto scope = view.card(title, classes, key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(callback_view(view));
                 return ref;
             },
             py::arg("title"),
             py::arg("classes") = "",
             py::arg("key") = "",
             py::arg("build") = py::none(),
             "Add a titled card; fill it via the build callback.")
        .def("foldout",
             [](affineui::View& view,
                const std::string& title,
                bool expanded,
                const std::string& key,
                py::object build) {
                 auto scope = view.foldout(title, expanded, key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(callback_view(view));
                 return ref;
             },
             py::arg("title"),
             py::arg("expanded") = true,
             py::arg("key") = "",
             py::arg("build") = py::none(),
             "Add a collapsible section (header + body); fill the body via "
             "build. Clicking the header toggles collapse.")
        .def("find_widget",
             [](affineui::View& view, const std::string& name) {
                 return view.find_widget(name);
             },
             py::arg("name"),
             "Return a stable WidgetRef by user key. Empty refs are safe.")
        // ── App-shell / structural component builders ───────────────────
        // Scope builders take a Pythonic `build` callback (called immediately
        // with the same View) instead of exposing a raw RAII Scope to Python;
        // Returned WidgetRefs weakly invalidate; they do not retain the View.
        .def("toolbar",
             [](affineui::View& view, const std::string& key, py::object build) {
                 auto scope = view.toolbar(key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(callback_view(view));
                 return ref;
             },
             py::arg("key") = "", py::arg("build") = py::none(),
             "Add a toolbar row; fill it via the build callback.")
        .def("toolbar_separator",
             [](affineui::View& view, const std::string& key) {
                 return view.toolbar_separator(key);
             },
             py::arg("key") = "",
             "Add a separator inside a toolbar.")
        .def("icon_button",
             [](affineui::View& view, const std::string& icon,
                const std::string& key) {
                 return view.icon_button(icon, key);
             },
             py::arg("icon"), py::arg("key") = "",
             "Add an icon-only ghost button (icon = Decius icon name).")
        .def("menu_bar",
             [](affineui::View& view, const std::string& key, py::object build) {
                 auto scope = view.menu_bar(key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(callback_view(view));
                 return ref;
             },
             py::arg("key") = "", py::arg("build") = py::none(),
             "Add a menubar row; fill it with menu_button()s.")
        .def("menu_button",
             [](affineui::View& view, const std::string& label,
                const std::string& menu_id, const std::string& key) {
                 return view.menu_button(label, menu_id, key);
             },
             py::arg("label"), py::arg("menu_id"), py::arg("key") = "",
             "Add a menubar button that opens the menu with id menu_id.")
        .def("menu_button",
             [](affineui::View& view, const std::string& label,
                py::function build, const std::string& key) {
                 return view.menu_button(
                     label,
                     [&build](affineui::View& v) {
                         build(callback_view(v));
                     },
                     key);
             },
             py::arg("label"), py::arg("build"), py::arg("key") = "",
             "Add a menubar button that OWNS its dropdown: build(view) "
             "populates the menu inline (menu_item/menu_separator/submenu).")
        .def("menu",
             [](affineui::View& view, const std::string& menu_id,
                py::function build) {
                 return view.menu(
                     menu_id,
                     [&build](affineui::View& v) {
                         build(callback_view(v));
                     });
             },
             py::arg("menu_id"), py::arg("build"),
             "Add a popup menu (hidden until a menu_button targets its id); "
             "build(view) populates it.")
        .def("menu_item",
             [](affineui::View& view, const std::string& label,
                const std::string& icon, const std::string& shortcut,
                const std::string& key) {
                 return view.menu_item(label, icon, shortcut, key);
             },
             py::arg("label"), py::arg("icon") = "", py::arg("shortcut") = "",
             py::arg("key") = "",
             "Add a menu row (optional icon glyph + label + shortcut). Wire "
             "on_click for the action.")
        .def("menu_item_custom",
             [](affineui::View& view, const std::string& key, py::object build) {
                 auto scope = view.menu_item_custom(key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(callback_view(view));
                 return ref;
             },
             py::arg("key") = "", py::arg("build") = py::none(),
             "Add a menu row whose content the caller composes via build; "
             "activation behaves like menu_item.")
        .def("menu_separator",
             [](affineui::View& view, const std::string& key) {
                 return view.menu_separator(key);
             },
             py::arg("key") = "",
             "Add a separator line between menu groups.")
        .def("submenu",
             [](affineui::View& view, const std::string& label,
                py::function build, const std::string& icon,
                const std::string& key) {
                 return view.submenu(
                     label,
                     [&build](affineui::View& v) {
                         build(callback_view(v));
                     },
                     icon, key);
             },
             py::arg("label"), py::arg("build"), py::arg("icon") = "",
             py::arg("key") = "",
             "Add a submenu row revealing nested items (build) on hover.")
        .def("menu_brand",
             [](affineui::View& view, const std::string& title,
                const std::string& icon, const std::string& key) {
                 return view.menu_brand(title, icon, key);
             },
             py::arg("title"), py::arg("icon") = "", py::arg("key") = "",
             "Add the app brand (icon + title) at the start of a menubar.")
        .def("menu_spacer",
             [](affineui::View& view, const std::string& key) {
                 return view.menu_spacer(key);
             },
             py::arg("key") = "",
             "Add a flexible spacer pushing following menubar items right.")
        .def("menu_meta",
             [](affineui::View& view, const std::string& text,
                const std::string& key) {
                 return view.menu_meta(text, key);
             },
             py::arg("text"), py::arg("key") = "",
             "Add right-aligned status/meta text in a menubar.")
        .def("document_title",
             [](affineui::View& view, const std::string& text,
                const std::string& key) {
                 return view.document_title(text, key);
             },
             py::arg("text"), py::arg("key") = "",
             "The name of the document being edited, centered in the bar — a "
             "WINDOW-title concern that happens to live in the menubar, "
             "because that strip IS the title bar once the window has none of "
             "its own. Centered on the window, not between its neighbours.")
        .def("dock_panel",
             [](affineui::View& view, const std::string& title,
                const std::string& tabpanel_id, const std::string& classes,
                const std::string& key, py::object build) {
                 auto scope = view.dock_panel(title, tabpanel_id, classes, key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(callback_view(view));
                 return ref;
             },
             py::arg("title"), py::arg("tabpanel_id"), py::arg("classes") = "",
             py::arg("key") = "", py::arg("build") = py::none(),
             "Add a dockable panel (titled tab + body); fill the body via build.")
        // ── Declarative docking workspace ────────────────────────────────
        .def("document_view",
             [](affineui::View& view, const std::string& key,
                py::function build) {
                 auto cb = keep_python_function(std::move(build));
                 return view.document_view(
                     key, [cb = std::move(cb)](affineui::View& dv) {
                         py::gil_scoped_acquire gil;
                         (*cb)(callback_view(dv));
                     });
             },
             py::arg("key"), py::arg("build"),
             "The docking workspace: declare the document and dockpanels "
             "inside build; the layout resolves from the declared seed, or "
             "replays the live arrangement when a dock-layout provider is "
             "wired.")
        .def("document",
             [](affineui::View& view, py::function content,
                const std::string& title, const std::string& icon) {
                 auto cb = keep_python_function(std::move(content));
                 return view.document(
                     [cb = std::move(cb)](affineui::View& p) {
                         py::gil_scoped_acquire gil;
                         (*cb)(callback_view(p));
                     },
                     title, icon);
             },
             py::arg("content"), py::arg("title") = "",
             py::arg("icon") = "",
             "Declare the center document of a document_view.")
        .def("dockpanel",
             [](affineui::View& view, const std::string& title,
                const affineui::DockLocation& where, py::function content,
                const std::string& icon, const std::string& key) {
                 auto cb = keep_python_function(std::move(content));
                 return view.dockpanel(
                     title, where,
                     [cb = std::move(cb)](affineui::View& p) {
                         py::gil_scoped_acquire gil;
                         (*cb)(callback_view(p));
                     },
                     icon, key);
             },
             py::arg("title"), py::arg("where"), py::arg("content"),
             py::arg("icon") = "", py::arg("key") = "",
             "Declare a dockable panel at a DockLocation; returns a "
             "DockHandle usable as another panel's parent.")
        .def("set_dock_layout_provider",
             [](affineui::View& view, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 view.set_dock_layout_provider(
                     [cb = std::move(cb)]() -> affineui::Document::DockLayout {
                         py::gil_scoped_acquire gil;
                         return (*cb)().cast<affineui::Document::DockLayout>();
                     });
             },
             py::arg("fn"),
             "Wire () -> DockLayout (usually app.document().dock_layout) so "
             "rebuilds replay the live arrangement.")
        .def("set_dock_placement_provider",
             [](affineui::View& view, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 view.set_dock_placement_provider(
                     [cb = std::move(cb)](std::string_view id)
                         -> affineui::Document::DockPlacement {
                         py::gil_scoped_acquire gil;
                         return (*cb)(std::string(id))
                             .cast<affineui::Document::DockPlacement>();
                     });
             },
             py::arg("fn"),
             "Wire (panel_id) -> DockPlacement runtime overrides (tearoffs / "
             "drag-to-dock) that win over the declared DockLocation.")
        .def("set_dock_size_provider",
             [](affineui::View& view, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 view.set_dock_size_provider(
                     [cb = std::move(cb)](std::string_view id) -> int {
                         py::gil_scoped_acquire gil;
                         return (*cb)(std::string(id)).cast<int>();
                     });
             },
             py::arg("fn"),
             "Wire (panel_id) -> saved px size (0 = none); a saved size wins "
             "over the declared seed.")
        .def("set_dock_active_tab_provider",
             [](affineui::View& view, py::function fn) {
                 auto cb = keep_python_function(std::move(fn));
                 view.set_dock_active_tab_provider(
                     [cb = std::move(cb)](std::string_view id) -> std::string {
                         py::gil_scoped_acquire gil;
                         return (*cb)(std::string(id)).cast<std::string>();
                     });
             },
             py::arg("fn"),
             "Wire (pane_id) -> active tab panel id (empty = primary).")
        .def("splitter",
             [](affineui::View& view, bool horizontal, const std::string& key) {
                 return view.splitter(horizontal, key);
             },
             py::arg("horizontal") = false, py::arg("key") = "",
             "Add a drag splitter between docked regions.")
        .def("tree",
             [](affineui::View& view, const std::string& key, py::object build) {
                 auto scope = view.tree(key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(callback_view(view));
                 return ref;
             },
             py::arg("key") = "", py::arg("build") = py::none(),
             "Add a tree container; fill it with tree_row()s.")
        .def("tree_row",
             [](affineui::View& view, const std::string& label, bool selected,
                int depth, const std::string& key) {
                 return view.tree_row(label, selected, depth, key);
             },
             py::arg("label"), py::arg("selected") = false, py::arg("depth") = 0,
             py::arg("key") = "",
             "Add a selectable tree row at the given depth.")
        .def("status_bar",
             [](affineui::View& view, const std::string& key, py::object build) {
                 auto scope = view.status_bar(key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(callback_view(view));
                 return ref;
             },
             py::arg("key") = "", py::arg("build") = py::none(),
             "Add a status bar row; fill it via the build callback.")
        .def("color_field",
             [](affineui::View& view, const std::string& label,
                const std::string& value,
                const std::vector<std::string>& swatches,
                const std::string& key) {
                 return view.color_field(label, value, swatches, key);
             },
             py::arg("label"), py::arg("value") = "",
             py::arg("swatches") = std::vector<std::string>{}, py::arg("key") = "",
             "Add a color field that opens a swatch picker popup.")
        .def("to_html_fragment", &affineui::View::to_html_fragment)
        .def("to_html_document", &affineui::View::to_html_document)
        // ── Strongly-typed component queries ────────────────────────────
        // Each returns a typed wrapper over a WidgetRef. Querying the wrong
        // type yields a wrapper whose .validity is WrongType (still attached,
        // but typed accessors are inert) and logs a diagnostic; a missing id
        // yields NotPresent. Never raises / never crashes. The returned
        // component weakly invalidates with the View.
        // ties the wrapper (and its inner ref) to this View.
        .def("button_at", &affineui::View::component<affineui::Button>,
             py::arg("name"))
        .def("checkbox_at", &affineui::View::component<affineui::Checkbox>,
             py::arg("name"))
        .def("text_field_at", &affineui::View::component<affineui::TextField>,
             py::arg("name"))
        .def("dropdown_at", &affineui::View::component<affineui::Dropdown>,
             py::arg("name"))
        .def("slider_at", &affineui::View::component<affineui::Slider>,
             py::arg("name"))
        .def("color_field_at", &affineui::View::component<affineui::ColorField>,
             py::arg("name"))
        .def("dock_panel_at", &affineui::View::component<affineui::DockPanel>,
             py::arg("name"))
        .def("foldout_at", &affineui::View::component<affineui::Foldout>,
             py::arg("name"));

    // ── Typed component wrappers ────────────────────────────────────────
    py::enum_<affineui::ComponentValidity>(m, "ComponentValidity")
        .value("Valid", affineui::ComponentValidity::Valid)
        .value("WrongType", affineui::ComponentValidity::WrongType)
        .value("NotPresent", affineui::ComponentValidity::NotPresent);

    // Common surface shared by every typed component (validity + generic ops).
    // Bound per-class below via a helper that registers the shared methods.
    const auto bind_component_base = [](auto& cls) {
        using T = typename std::decay_t<decltype(cls)>::type;
        cls.def("__bool__", [](const T& c) { return static_cast<bool>(c); })
           .def_property_readonly("valid", [](const T& c) { return c.valid(); })
           .def_property_readonly("validity", [](const T& c) { return c.validity(); })
           .def_property_readonly("attached", [](const T& c) { return c.attached(); })
           .def_property_readonly("id", [](const T& c) { return std::string{c.id()}; })
           .def_property_readonly("kind", [](const T& c) { return c.kind(); })
           .def_property("visible",
                         [](const T& c) { return c.visible(); },
                         [](T& c, bool on) { c.set_visible(on); })
           .def("attr",
                [](const T& c, const std::string& n, const std::string& f) {
                    return std::string{c.attr(n, f)};
                }, py::arg("name"), py::arg("fallback") = "")
           .def("set_attr",
                [](T& c, const std::string& n, const std::string& v) {
                    c.set_attr(n, v);
                }, py::arg("name"), py::arg("value"))
           .def_property("text",
                         [](const T& c) { return std::string{c.text()}; },
                         [](T& c, const std::string& t) { c.set_text(t); });
    };

    {
        py::class_<affineui::Button> cls(m, "Button");
        cls.def(py::init<>());
        bind_component_base(cls);
        cls.def_property("label",
                         [](const affineui::Button& b) { return std::string{b.label()}; },
                         [](affineui::Button& b, const std::string& t) { b.set_label(t); })
           .def_property("enabled",
                         [](const affineui::Button& b) { return b.enabled(); },
                         [](affineui::Button& b, bool on) { b.set_enabled(on); })
           .def("on_click",
                [](affineui::Button& b, py::function cb) {
                    auto callback = keep_python_function(std::move(cb));
                    b.on_click([callback] {
                        call_python_function("Button.on_click", callback);
                    });
                }, py::arg("callback"));
    }
    {
        py::class_<affineui::Checkbox> cls(m, "Checkbox");
        cls.def(py::init<>());
        bind_component_base(cls);
        cls.def_property("checked",
                         [](const affineui::Checkbox& c) { return c.checked(); },
                         [](affineui::Checkbox& c, bool on) { c.set_checked(on); })
           .def("on_change",
                [](affineui::Checkbox& c, py::function cb) {
                    auto callback = keep_python_function(std::move(cb));
                    c.on_change([callback](std::string_view v) {
                        call_python_function("Checkbox.on_change", callback,
                                             std::string(v));
                    });
                }, py::arg("callback"));
    }
    {
        py::class_<affineui::TextField> cls(m, "TextField");
        cls.def(py::init<>());
        bind_component_base(cls);
        cls.def_property("value",
                         [](const affineui::TextField& t) { return t.value(); },
                         [](affineui::TextField& t, const std::string& v) { t.set_value(v); })
           .def("on_change",
                [](affineui::TextField& t, py::function cb) {
                    auto callback = keep_python_function(std::move(cb));
                    t.on_change([callback](std::string_view v) {
                        call_python_function("TextField.on_change", callback,
                                             std::string(v));
                    });
                }, py::arg("callback"));
    }
    {
        py::class_<affineui::Dropdown> cls(m, "Dropdown");
        cls.def(py::init<>());
        bind_component_base(cls);
        cls.def_property("selected",
                         [](const affineui::Dropdown& d) { return d.selected(); },
                         [](affineui::Dropdown& d, const std::string& v) { d.set_selected(v); })
           .def("on_change",
                [](affineui::Dropdown& d, py::function cb) {
                    auto callback = keep_python_function(std::move(cb));
                    d.on_change([callback](std::string_view v) {
                        call_python_function("Dropdown.on_change", callback,
                                             std::string(v));
                    });
                }, py::arg("callback"));
    }
    {
        py::class_<affineui::Slider> cls(m, "Slider");
        cls.def(py::init<>());
        bind_component_base(cls);
        cls.def("value",
                [](const affineui::Slider& s, double fallback) { return s.value(fallback); },
                py::arg("fallback") = 0.0)
           .def("on_change",
                [](affineui::Slider& s, py::function cb) {
                    auto callback = keep_python_function(std::move(cb));
                    s.on_change([callback](std::string_view v) {
                        call_python_function("Slider.on_change", callback,
                                             std::string(v));
                    });
                }, py::arg("callback"));
    }
    {
        py::class_<affineui::ColorField> cls(m, "ColorField");
        cls.def(py::init<>());
        bind_component_base(cls);
        cls.def_property("color",
                         [](const affineui::ColorField& c) { return c.color(); },
                         [](affineui::ColorField& c, const std::string& v) { c.set_color(v); })
           .def("on_change",
                [](affineui::ColorField& c, py::function cb) {
                    auto callback = keep_python_function(std::move(cb));
                    c.on_change([callback](std::string_view v) {
                        call_python_function("ColorField.on_change", callback,
                                             std::string(v));
                    });
                }, py::arg("callback"));
    }
    {
        py::class_<affineui::DockPanel> cls(m, "DockPanel");
        cls.def(py::init<>());
        bind_component_base(cls);
        cls.def_property("active_tab",
                         [](const affineui::DockPanel& d) { return std::string{d.active_tab()}; },
                         [](affineui::DockPanel& d, const std::string& t) { d.set_active_tab(t); });
    }
    {
        py::class_<affineui::Foldout> cls(m, "Foldout");
        cls.def(py::init<>());
        bind_component_base(cls);
        cls.def_property("open",
                         [](const affineui::Foldout& f) { return f.open(); },
                         [](affineui::Foldout& f, bool on) { f.set_open(on); });
    }

    // ── Application menus ───────────────────────────────────────────────
    // The platform-neutral menu MODEL (affineui/menu.h), mirroring Electron's
    // Menu.buildFromTemplate vocabulary. Declared once with App.set_menu; on
    // macOS it becomes the real system menu bar (NSApp.mainMenu — the bar at
    // the top of the SCREEN, which an app cannot draw itself) and the drawn
    // View.menu_bar triggers hide themselves, because they are the same menus.
    py::enum_<affineui::MenuRole>(
            m,
            "MenuRole",
            "A standard item whose label, accelerator and behavior the "
            "platform supplies — the way the macOS application menu (About / "
            "Services / Hide / Quit) and a working Edit menu come out right "
            "without the app restating them per platform.")
        // C++ spells the no-role case MenuRole::None; `None` is a Python
        // keyword, so it is NoRole here (it is also the default, so apps
        // rarely name it).
        .value("NoRole", affineui::MenuRole::None)
        .value("About", affineui::MenuRole::About)
        .value("Services", affineui::MenuRole::Services)
        .value("Hide", affineui::MenuRole::Hide)
        .value("HideOthers", affineui::MenuRole::HideOthers)
        .value("Unhide", affineui::MenuRole::Unhide)
        .value("Preferences", affineui::MenuRole::Preferences)
        .value("Quit", affineui::MenuRole::Quit)
        .value("Undo", affineui::MenuRole::Undo)
        .value("Redo", affineui::MenuRole::Redo)
        .value("Cut", affineui::MenuRole::Cut)
        .value("Copy", affineui::MenuRole::Copy)
        .value("Paste", affineui::MenuRole::Paste)
        .value("SelectAll", affineui::MenuRole::SelectAll)
        .value("Minimize", affineui::MenuRole::Minimize)
        .value("Zoom", affineui::MenuRole::Zoom)
        .value("Close", affineui::MenuRole::Close)
        .value("ToggleFullscreen", affineui::MenuRole::ToggleFullscreen);

    py::enum_<affineui::MenuItemType>(m, "MenuItemType")
        .value("Normal", affineui::MenuItemType::Normal)
        .value("Separator", affineui::MenuItemType::Separator)
        .value("Checkbox", affineui::MenuItemType::Checkbox)
        .value("Radio", affineui::MenuItemType::Radio);

    py::enum_<affineui::TitleBarStyle>(
            m,
            "TitleBarStyle",
            "How the window's title bar is drawn (App titlebar=...), named as "
            "in Electron's titleBarStyle.")
        .value("Default", affineui::TitleBarStyle::Default,
               "The OS draws its title bar; the app draws below it.")
        .value("Hidden", affineui::TitleBarStyle::Hidden,
               "No system title bar: the content fills the window and the app "
               "draws its own bar. The macOS traffic lights still float over "
               "it (move them with traffic_light_position). Mark the app's own "
               "bar draggable with CSS `--affineui-app-region: drag`, or the "
               "window cannot be moved.")
        .value("HiddenInset", affineui::TitleBarStyle::HiddenInset,
               "As Hidden, with the macOS traffic lights inset further from "
               "the corner.")
        .value("Frameless", affineui::TitleBarStyle::Frameless,
               "No system title bar AND no system window buttons: the app "
               "draws close/minimize/maximize itself and drives them with "
               "App.close/minimize/toggle_maximize.");

    py::class_<affineui::Accelerator>(
            m,
            "Accelerator",
            "A parsed accelerator. `key` is the normalized key token (\"S\", "
            "\"F5\", \"Enter\"), empty when the string had no key.")
        .def(py::init<>())
        .def_readwrite("ctrl", &affineui::Accelerator::ctrl)
        .def_readwrite("shift", &affineui::Accelerator::shift)
        .def_readwrite("alt", &affineui::Accelerator::alt)
        .def_readwrite("super", &affineui::Accelerator::super,
                       "Command on macOS, the Windows/Super key elsewhere.")
        .def_readwrite("key", &affineui::Accelerator::key)
        .def("valid", &affineui::Accelerator::valid)
        .def("__bool__", &affineui::Accelerator::valid);

    m.def("parse_accelerator",
          [](const std::string& spec) {
              return affineui::parse_accelerator(spec);
          },
          py::arg("spec"),
          "Parse an Electron-style accelerator (\"Shift+CmdOrCtrl+Z\"). "
          "CmdOrCtrl resolves to Command on macOS and Control elsewhere, so an "
          "app writes the shortcut once. The result's valid() is False when no "
          "key was found.");
    m.def("accelerator_text",
          [](const affineui::Accelerator& accel) {
              return affineui::accelerator_text(accel);
          },
          py::arg("accel"),
          "Human-readable text for a drawn menu's shortcut column: the glyph "
          "form (\"⇧⌘Z\") on macOS, the spelled form (\"Ctrl+Shift+Z\") "
          "elsewhere.");

    // One menu row. The builders (item/separator/sub/role/check) read the way
    // the C++ ones do; a Menu is just a list of these. Callbacks go through
    // keep_python_function/call_python_function like every other callback here,
    // so a menu selection fired from AppKit acquires the GIL and a raising
    // handler is reported rather than unwinding into native code.
    py::class_<affineui::MenuItem>(
            m,
            "MenuItem",
            "One row of a menu: a labelled item, a separator, a submenu, or a "
            "standard role. Pass a list of them to App.set_menu.")
        .def(py::init<>())
        .def_readwrite("label", &affineui::MenuItem::label)
        .def_readwrite("accelerator", &affineui::MenuItem::accelerator,
                       "Electron-style chord: \"CmdOrCtrl+S\", "
                       "\"Shift+Alt+F\". Empty for none.")
        .def_readwrite("item_role", &affineui::MenuItem::item_role)
        .def_readwrite("type", &affineui::MenuItem::type)
        .def_readwrite("enabled", &affineui::MenuItem::enabled)
        .def_readwrite("visible", &affineui::MenuItem::visible)
        .def_readwrite("checked", &affineui::MenuItem::checked,
                       "Check mark. Meaningful for Checkbox/Radio.")
        .def_readwrite("icon", &affineui::MenuItem::icon,
                       "Named glyph (the icon names the drawn menus use).")
        .def_readwrite("swatch", &affineui::MenuItem::swatch,
                       "Solid color chip in the item's leading gutter (accent "
                       "pickers, layer colors). Only drawn when swatch.a is "
                       "non-zero.")
        .def_readwrite("submenu", &affineui::MenuItem::submenu)
        .def("on_select",
             [](affineui::MenuItem& item, py::object cb) -> affineui::MenuItem& {
                 if (cb.is_none()) {
                     item.on_select = nullptr;
                     return item;
                 }
                 auto callback = keep_python_function(cb.cast<py::function>());
                 item.on_select = [callback = std::move(callback)] {
                     call_python_function("MenuItem.on_select", callback);
                 };
                 return item;
             },
             py::arg("callback"), py::return_value_policy::reference_internal,
             "Set the activation callback (chainable). None clears it.")
        .def_static("item",
                    [](const std::string& label,
                       const std::string& accelerator,
                       py::object on_select) {
                        auto m = affineui::MenuItem::item(label, accelerator);
                        if (!on_select.is_none()) {
                            auto callback = keep_python_function(
                                on_select.cast<py::function>());
                            m.on_select = [callback = std::move(callback)] {
                                call_python_function("MenuItem.on_select",
                                                     callback);
                            };
                        }
                        return m;
                    },
                    py::arg("label"), py::arg("accelerator") = "",
                    py::arg("on_select") = py::none(),
                    "A labelled item: MenuItem.item(\"Save\", \"CmdOrCtrl+S\", "
                    "self.save).")
        .def_static("separator", &affineui::MenuItem::separator)
        .def_static("sub",
                    [](const std::string& label,
                       const std::vector<affineui::MenuItem>& items) {
                        return affineui::MenuItem::sub(label, items);
                    },
                    py::arg("label"), py::arg("items"),
                    "A submenu (also how a top-level menu is declared).")
        .def_static("role",
                    [](affineui::MenuRole role, const std::string& label) {
                        return affineui::MenuItem::role(role, label);
                    },
                    py::arg("role"), py::arg("label") = "",
                    "A standard platform item. Label and accelerator are "
                    "supplied by the shell unless you override them.")
        .def_static("check",
                    [](const std::string& label, bool checked,
                       const std::string& accelerator, py::object on_select) {
                        auto m = affineui::MenuItem::check(label, checked,
                                                           accelerator);
                        if (!on_select.is_none()) {
                            auto callback = keep_python_function(
                                on_select.cast<py::function>());
                            m.on_select = [callback = std::move(callback)] {
                                call_python_function("MenuItem.on_select",
                                                     callback);
                            };
                        }
                        return m;
                    },
                    py::arg("label"), py::arg("checked"),
                    py::arg("accelerator") = "",
                    py::arg("on_select") = py::none(),
                    "A checkable item. Rebuild and re-set the menu when the "
                    "state changes, the same way the view is rebuilt.")
        .def_static("edit_menu", &affineui::MenuItem::edit_menu,
                    "The conventional Edit menu (Undo/Redo/Cut/Copy/Paste/"
                    "Select All). On macOS these carry the AppKit selectors, so "
                    "they act on the focused control with no app wiring.")
        .def_static("window_menu", &affineui::MenuItem::window_menu,
                    "The conventional Window menu (Minimize/Zoom/Close).");

    py::class_<affineui::App>(m, "App")
        .def(py::init([](const std::string& title,
                         int width,
                         int height,
                         affineui::Color clear_color,
                         bool high_dpi,
                          bool vsync,
                          const std::string& default_font_family,
                          int default_font_size,
                          const std::vector<std::string>& asset_folders,
                          bool perf_overlay,
                          bool no_bundle_decius,
                          bool native_menus,
                          affineui::TitleBarStyle titlebar,
                          affineui::Point traffic_light_position) {
                  return std::make_unique<affineui::App>(
                      make_app_config(title,
                                      width,
                                     height,
                                     clear_color,
                                     high_dpi,
                                      vsync,
                                      default_font_family,
                                      default_font_size,
                                      asset_folders,
                                      perf_overlay,
                                      no_bundle_decius,
                                      native_menus,
                                      titlebar,
                                      traffic_light_position));
              }),
             py::arg("title") = "AffineUI",
             py::arg("width") = 1024,
             py::arg("height") = 768,
             py::arg("clear_color") = affineui::Color{30, 30, 46, 255},
             py::arg("high_dpi") = true,
              py::arg("vsync") = true,
              py::arg("default_font_family") = "sans-serif",
              py::arg("default_font_size") = 16,
              py::arg("asset_folders") = std::vector<std::string>{"."},
              py::arg("perf_overlay") = false,
              py::arg("no_bundle_decius") = false,
              // ── Platform chrome ──────────────────────────────────────
              // native_menus is ON by default for the reason the C++ Config
              // is: on macOS the menu bar lives at the top of the SCREEN and
              // an app cannot draw it — and without one there is no Quit item,
              // so Cmd-Q would not work at all. Set False to keep only the
              // in-window bar the app draws itself (View.menu_bar).
              py::arg("native_menus") = true,
              py::arg("titlebar") = affineui::TitleBarStyle::Default,
              // macOS only: where the traffic lights sit, in logical points
              // from the window's top-left. Point(0, 0) = platform default.
              py::arg("traffic_light_position") = affineui::Point{})
        .def("load_html", [](affineui::App& app, const std::string& html) {
            app.load_html(html);
        })
        .def("load_view",
             [](affineui::App& app, const affineui::View& view) {
                 app.load_view(view);
             },
             py::arg("view"),
             "Copy a View into the native App. The App copies callbacks and "
             "does not borrow the Python View object.")
        .def("set_view",
             [](affineui::App& app, py::function builder) {
                 auto callback = keep_python_function(std::move(builder));
                 // The builder runs from the App's frame loop (and from the
                 // synchronous set_view()/rebuild_view() call below), so it
                 // must acquire the GIL before touching Python. Python receives
                 // an invalidating weak proxy for the App's persistent View;
                 // method calls still populate that View in place, but keeping
                 // the callback object after App destruction cannot retain a
                 // raw pointer. Exceptions propagate into rebuild_view() (which
                 // closes the mutation window and rethrows).
                 app.set_view([callback](affineui::View& view) {
                     py::gil_scoped_acquire gil;
                     try {
                         (*callback)(callback_view(view));
                     } catch (py::error_already_set& e) {
                         // Re-raise as a C++ exception so rebuild_view()'s
                         // mid-batch handler closes the mutation window before
                         // it unwinds; the message carries the Python trace.
                         throw std::runtime_error(e.what());
                     }
                 });
             },
             py::arg("builder"),
             "Install a view builder callable(View)->None and build once. "
             "The App owns a persistent View; each rebuild re-runs the builder "
             "into it and reconciles only the diff into the live document — "
             "the fast path for apps that rebuild per state change. Follow a "
             "state change with invalidate() (the frame loop coalesces and "
             "reconciles) or rebuild_view() (synchronous, e.g. headless).")
        .def("rebuild_view", &affineui::App::rebuild_view,
             "Re-run the installed set_view() builder and reconcile the result "
             "into the live document, synchronously. No-op until set_view() "
             "has been called. Use invalidate() instead when running under the "
             "frame loop so rebuilds coalesce to one per frame.")
        .def("load_html_file",
             [](affineui::App& app, const std::string& path) {
                 return app.load_html_file(path);
             })
        .def("set_stylesheet",
             [](affineui::App& app, const std::string& css,
                py::object base_url) {
                 if (base_url.is_none()) {
                     app.set_stylesheet(css);
                 } else {
                     app.set_stylesheet(css, base_url.cast<std::string>());
                 }
             },
             py::arg("css"), py::arg("base_url") = py::none(),
             "Install the user stylesheet. base_url (optional) is the "
             "sheet's own location so its relative url()s resolve like a "
             "<link>ed sheet's.")
        .def("invalidate", &affineui::App::invalidate)
        .def("set_perf_overlay_enabled",
             &affineui::App::set_perf_overlay_enabled,
             py::arg("enabled"),
             "Enable or disable the native performance overlay. The overlay "
             "is drawn outside the document and can be moved by clicking it.")
        .def("perf_overlay_enabled",
             &affineui::App::perf_overlay_enabled,
             "Return whether the native performance overlay is enabled.")
        .def("dispatch",
             [](affineui::App& app, const affineui::Event& ev) {
                 return app.dispatch(ev);
             },
             py::arg("event"),
             "Dispatch an Event through the loaded document. Useful for "
             "headless tests and custom Python hosts.")
        .def("quit", &affineui::App::quit, py::arg("code") = 0)
        // ── Menus ────────────────────────────────────────────────────────
        .def("set_menu",
             [](affineui::App& app, std::vector<affineui::MenuItem> menu) {
                 app.set_menu(std::move(menu));
             },
             py::arg("menu"),
             "Install (or replace) the application menu: a list of MenuItems, "
             "left to right. Safe to call at any time, including from a menu "
             "callback — a menu that shows checked/enabled state is meant to be "
             "rebuilt and re-set as that state changes, the same way the view "
             "is. On macOS this becomes the system menu bar (and the drawn "
             "View.menu_bar triggers hide themselves); ignored when the App was "
             "built with native_menus=False.")
        .def("menu",
             [](const affineui::App& app) { return app.menu(); },
             "The menu last passed to set_menu().")
        // ── Close requests ───────────────────────────────────────────────
        .def("on_close_request",
             [](affineui::App& app, py::function cb) {
                 auto callback = keep_python_function(std::move(cb));
                 app.on_close_request([callback]() -> bool {
                     try {
                         py::gil_scoped_acquire gil;
                         py::object result = (*callback)();
                         // A handler that returns nothing means "proceed" —
                         // only an explicit False cancels the close.
                         return result.is_none() ? true : result.cast<bool>();
                     } catch (py::error_already_set& e) {
                         e.discard_as_unraisable("App.on_close_request");
                     } catch (const std::exception& e) {
                         std::fprintf(stderr,
                                      "AffineUI Python callback failed "
                                      "(App.on_close_request): %s\n",
                                      e.what());
                     }
                     // A broken handler must not trap the user in the app.
                     return true;
                 });
             },
             py::arg("callback"),
             "Install the handler for an inbound close request — the window's "
             "close button, Cmd-Q / Alt-F4, the menu's Quit item, a Close "
             "control the app drew itself. Return False to CANCEL the close; "
             "this is the one veto point, so it is where an editor says \"you "
             "have unsaved changes\". With no handler a close always proceeds, "
             "and App.quit() bypasses it.")
        // ── Window controls ──────────────────────────────────────────────
        // What a title bar's buttons do. An app running with
        // TitleBarStyle.Frameless draws its own and wires them to these.
        .def("close", &affineui::App::close,
             "Ask to close the window. Runs the close-request handler first, "
             "so this is cancellable — an app-drawn close button gets "
             "save-on-exit for free. (quit() is the uncancellable form.)")
        .def("minimize", &affineui::App::minimize)
        .def("toggle_maximize", &affineui::App::toggle_maximize,
             "Toggle zoomed/maximized, as the OS's own button does.")
        .def("is_maximized", &affineui::App::is_maximized)
        .def("set_fullscreen", &affineui::App::set_fullscreen, py::arg("on"))
        .def("is_fullscreen", &affineui::App::is_fullscreen)
        .def("window_size",
             &affineui::App::window_size,
             "Return the current window size in logical CSS points.")
        .def("framebuffer_size",
             &affineui::App::framebuffer_size,
             "Return the current drawable framebuffer size in physical pixels.")
        .def("dpi_scale",
             &affineui::App::dpi_scale,
             "Return the render DPI scale: framebuffer pixels per CSS point.")
        .def("frame_telemetry",
             [](const affineui::App& app) {
                 // Keys mirror the telemetry.frame schema
                 // (docs/AFFINETOOLS_PROTOCOL.md) so dumps and this dict
                 // are the same shape.
                 const affineui::FrameTelemetry& t = app.frame_telemetry();
                 py::dict d;
                 d["v"] = t.v;
                 d["frame"] = t.frame;
                 d["t_ms"] = t.t_ms;
                 d["gap_ms"] = t.gap_ms;
                 d["cb_ms"] = t.cb_ms;
                 d["skipped"] = t.skipped;
                 d["fb"] = py::make_tuple(t.fb_w, t.fb_h);
                 d["dpi"] = t.dpi;
                 py::dict stage;
                 stage["prep"] = t.prep_us;
                 stage["layout"] = t.layout_us;
                 stage["dl"] = t.record_us;
                 stage["rast"] = t.raster_us;
                 stage["comp"] = t.composite_us;
                 d["stage_us"] = stage;
                 py::dict ops;
                 ops["cached"] = t.cached_ops;
                 ops["culled"] = t.culled_ops;
                 ops["changed"] = t.changed_ops;
                 ops["rects"] = t.dirty_rects;
                 ops["dirty_pct"] = t.dirty_pct_x100 / 100.0;
                 d["ops"] = ops;
                 py::dict flags;
                 flags["rec"] = t.recorded;
                 flags["dl"] = t.dl_changed;
                 flags["rast"] = t.rasterized;
                 flags["partial"] = t.partial;
                 flags["direct"] = t.direct;
                 flags["reused"] = t.layer_reused;
                 flags["anim"] = t.animations;
                 d["flags"] = flags;
                 py::dict mem;
                 mem["live"] = t.mem_live_bytes;
                 mem["blocks"] = t.mem_live_blocks;
                 mem["allocs"] = t.allocs;
                 mem["frees"] = t.frees;
                 d["mem"] = mem;
                 return d;
             },
             "The most recent presented frame's telemetry record as a dict "
             "(same shape as a telemetry.frame JSONL record). Zeroed until "
             "the first frame presents, or when compiled with "
             "AFFINEUI_PERF=0.")
        .def("on_event_capture",
             [](affineui::App& app, py::function cb) {
                 auto callback = keep_python_function(std::move(cb));
                 app.on_event_capture(
                     [callback](const affineui::Event& ev,
                                const std::vector<
                                    affineui::Document::HoverInfo>& hover)
                         -> bool {
                         try {
                             py::gil_scoped_acquire gil;
                             py::object result = (*callback)(ev, hover);
                             return result.is_none() ? false
                                                     : result.cast<bool>();
                         } catch (py::error_already_set& e) {
                             e.discard_as_unraisable("App.on_event_capture");
                         } catch (const std::exception& e) {
                             std::fprintf(stderr,
                                          "AffineUI Python callback failed "
                                          "(App.on_event_capture): %s\n",
                                          e.what());
                         }
                         return false;
                     });
             },
             py::arg("callback"),
             "Register a capture handler before focused-widget dispatch. "
             "Return True to consume the event, for example to override "
             "local text undo with an app-global undo command.")
        .def("on_event",
             [](affineui::App& app, py::function cb) {
                 auto callback = keep_python_function(std::move(cb));
                 app.on_event(
                     [callback](const affineui::Event& ev,
                                const std::vector<
                                    affineui::Document::HoverInfo>& hover)
                         -> bool {
                         try {
                             py::gil_scoped_acquire gil;
                             py::object result = (*callback)(ev, hover);
                             return result.is_none() ? false
                                                     : result.cast<bool>();
                         } catch (py::error_already_set& e) {
                             e.discard_as_unraisable("App.on_event");
                         } catch (const std::exception& e) {
                             std::fprintf(stderr,
                                          "AffineUI Python callback failed "
                                          "(App.on_event): %s\n",
                                          e.what());
                         }
                         return false;
                     });
             },
             py::arg("callback"),
             "Register a low-level native event handler. The callback "
             "receives (event, hover_chain) — the hover chain is hit-tested "
             "deepest-first — after document/widget dispatch. Events consumed "
             "by a focused editor do not reach this phase. The hook native "
             "widget kits (and canvas tools) build pointer-drag behaviors on.")
        .def("on_frame",
             [](affineui::App& app, py::function cb) {
                 auto callback = keep_python_function(std::move(cb));
                 app.on_frame([callback](double dt) {
                     call_python_function("App.on_frame", callback, dt);
                 });
             },
             py::arg("callback"),
             "Register a per-frame tick callback (dt seconds) — the "
             "requestAnimationFrame analog.")
        .def("capture_pointer", &affineui::App::capture_pointer,
             "Route MouseMove events to on_event handlers before DOM hover "
             "hit-testing (browser-style pointer capture for drags).")
        .def("release_pointer", &affineui::App::release_pointer)
        .def("pointer_captured", &affineui::App::pointer_captured)
        .def("request_custom_repaint",
             [](affineui::App& app, const std::string& name) {
                 app.request_custom_repaint(name);
             },
             py::arg("name"),
             "Repaint every data-aui-paint=name element on the next frame — "
             "no restyle, no layout, no reconcile.")
        // ── Custom paint + dynamic images ────────────────────────────────
        // The seam a NATIVE RENDERING CORE plugs into. A core (e.g. the
        // photoedit raster engine) receives these as plain callbacks, so it
        // links no affineui runtime — and therefore no second copy of sokol.
        // Ids are plain ints: the ImageHandle stays here, owned by the App.
        .def("set_custom_paint",
             [](affineui::App& app, const std::string& name, py::object fn) {
                 if (fn.is_none()) {
                     app.set_custom_paint(name, nullptr);
                     return;
                 }
                 auto cb = std::make_shared<py::object>(std::move(fn));
                 app.set_custom_paint(
                     name,
                     [cb](affineui::Painter& p, const affineui::Rect& r) {
                         py::gil_scoped_acquire gil;
                         (*cb)(py::cast(&p, py::return_value_policy::reference),
                               r);
                     });
             },
             py::arg("name"), py::arg("fn"),
             "Register a handler for every data-aui-paint=name element: "
             "fn(painter, rect) draws its content each frame. Pass None to "
             "unregister.")
        .def("create_image_rgba",
             [](affineui::App& app, int w, int h, py::buffer px) -> std::uint32_t {
                 const py::buffer_info info = px.request();
                 const auto* bytes =
                     static_cast<const std::uint8_t*>(info.ptr);
                 auto handle = app.create_image_rgba(
                     w, h,
                     std::span<const std::uint8_t>(
                         bytes, static_cast<std::size_t>(info.size *
                                                        info.itemsize)));
                 if (!handle.is_valid()) return 0;
                 const std::uint32_t id = handle.id();
                 detail::py_image_registry(app)[id] = std::move(handle);
                 return id;
             },
             py::arg("width"), py::arg("height"), py::arg("rgba"),
             "Create a renderer-owned dynamic RGBA8 image; returns its painter "
             "id (0 on failure). The App keeps the handle alive until "
             "destroy_image.")
        .def("update_image",
             [](affineui::App& app, std::uint32_t id, py::buffer px) -> bool {
                 auto& reg = detail::py_image_registry(app);
                 const auto it = reg.find(id);
                 if (it == reg.end()) return false;
                 const py::buffer_info info = px.request();
                 const auto* bytes =
                     static_cast<const std::uint8_t*>(info.ptr);
                 return it->second.update(std::span<const std::uint8_t>(
                     bytes,
                     static_cast<std::size_t>(info.size * info.itemsize)));
             },
             py::arg("image_id"), py::arg("rgba"),
             "Replace an image's pixels. The byte count must match "
             "width * height * 4 from creation.")
        .def("destroy_image",
             [](affineui::App& app, std::uint32_t id) {
                 detail::py_image_registry(app).erase(id);
             },
             py::arg("image_id"),
             "Release an image created with create_image_rgba.")
        .def("document",
             static_cast<affineui::Document& (affineui::App::*)()>(
                 &affineui::App::document),
             py::return_value_policy::reference_internal)
        .def("run",
             [](affineui::App& app) {
                 return app.run();
             },
             "Run the native app loop on the Python main thread. This binding "
             "intentionally keeps the GIL while native callbacks are still "
             "synchronous Python calls; a future queued callback bridge can "
             "release it safely.")
        .def("launch",
             [](affineui::App& app, bool native) {
                 if (!native) {
                     throw py::value_error(
                         "Only native=True is available in this POC; "
                         "remote browser transport is not implemented yet.");
                 }
                 return app.run();
             },
             py::arg("native") = true,
             "Launch the app. native=True opens the local AffineUI window; "
             "remote browser transport is planned but not implemented yet.");
}
