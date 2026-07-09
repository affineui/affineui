#include <affineui/app.h>
#include <affineui/components.h>
#include <affineui/decius_bundle.h>
#include <affineui/document.h>
#include <affineui/tools.h>
#include <affineui/types.h>
#include <affineui/view.h>
#include <affineui/version.h>

#include "photo_core_py.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace py = pybind11;

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
                                      bool no_bundle_decius) {
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
    bind_photo_core(m);

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
        .value("Digit9", affineui::Key::Digit9);

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
        .value("FocusGained", affineui::EventType::FocusGained);

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
                      &affineui::DispatchResult::invalidate_view);

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
             "6=ew-resize 7=ns-resize 8=nwse-resize).");

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
                     (*callback)(&view);
                 });
             },
             py::return_value_policy::reference_internal)
        .def("replace",
             [](affineui::WidgetRef& ref, py::function build) -> affineui::WidgetRef& {
                 auto callback = keep_python_function(std::move(build));
                 return ref.replace([callback = std::move(callback)](affineui::View& view) {
                     py::gil_scoped_acquire gil;
                     (*callback)(&view);
                 });
             },
             py::return_value_policy::reference_internal)
        .def("find_widget",
             [](const affineui::WidgetRef& ref, const std::string& name) {
                 return ref.find_widget(name);
             },
             py::arg("name"),
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
             "Add a paragraph and return a stable WidgetRef tied to this View.")
        .def("text",
             [](affineui::View& view,
                const std::string& text,
                const std::string& key) {
                 return view.text(text, key);
             },
             py::arg("text"),
             py::arg("key") = "",
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
             "Add a button. The returned WidgetRef keeps the View alive.")
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
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
             "Add an on/off switch (checkbox semantics, slide presentation).")
        .def("combo",
             [](affineui::View& view,
                const std::string& label,
                double value,
                double step,
                const std::string& key) {
                 return view.combo(label, value, step, key);
             },
             py::arg("label"),
             py::arg("value"),
             py::arg("step") = 0.01,
             py::arg("key") = "",
             py::keep_alive<0, 1>(),
             "Add a bare drag-scrub numeric combo (no field/label wrapper).")
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
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
             "Add a mutually-exclusive button group and return a WidgetRef.")
        .def("virtual_list",
             [](affineui::View& view,
                const std::string& key,
                std::size_t item_count,
                py::function build,
                double item_size,
                std::size_t first_item,
                std::size_t visible_items,
                std::size_t overscan,
                const std::vector<double>& item_sizes,
                const std::string& classes) {
                 affineui::VirtualListOptions options{};
                 options.item_count = item_count;
                 options.first_item = first_item;
                 options.visible_items = visible_items;
                 options.overscan = overscan;
                 options.item_size = item_size;
                 options.item_sizes = item_sizes;
                 auto callback = keep_python_function(std::move(build));
                 return view.virtual_list(
                     key,
                     options,
                     [callback = std::move(callback)](affineui::View& child_view,
                                                       std::size_t index) {
                         py::gil_scoped_acquire gil;
                         (*callback)(&child_view, index);
                     },
                     classes);
             },
             py::arg("key"),
             py::arg("item_count"),
             py::arg("build"),
             py::arg("item_size") = 24.0,
             py::arg("first_item") = 0,
             py::arg("visible_items") = 16,
             py::arg("overscan") = 2,
             py::arg("item_sizes") = std::vector<double>{},
             py::arg("classes") = "",
             py::keep_alive<0, 1>(),
             "Add a virtualized fixed-size list. build(view, index) is called "
             "only for the materialized rows.")
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
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
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
                 build(&view);
                 return ref;
             },
             py::arg("classes") = "",
             py::arg("key") = "",
             py::arg("build") = py::none(),
             py::keep_alive<0, 1>(),
             "Add a generic container. If build is supplied it is called "
             "immediately with the same View.")
        .def("panel",
             [](affineui::View& view, const std::string& key, py::object build) {
                 if (build.is_none()) {
                     return view.panel_ref(key);
                 }
                 auto scope = view.panel(key);
                 auto ref = scope.ref();
                 build(&view);
                 return ref;
             },
             py::arg("key") = "",
             py::arg("build") = py::none(),
             py::keep_alive<0, 1>(),
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
                 build(&view);
                 return ref;
             },
             py::arg("tag"),
             py::arg("classes") = "",
             py::arg("key") = "",
             py::arg("build") = py::none(),
             py::keep_alive<0, 1>(),
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
                 if (!build.is_none()) build(&view);
                 return ref;
             },
             py::arg("title"),
             py::arg("classes") = "",
             py::arg("key") = "",
             py::arg("build") = py::none(),
             py::keep_alive<0, 1>(),
             "Add a titled card; fill it via the build callback.")
        .def("foldout",
             [](affineui::View& view,
                const std::string& title,
                bool expanded,
                const std::string& key,
                py::object build) {
                 auto scope = view.foldout(title, expanded, key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(&view);
                 return ref;
             },
             py::arg("title"),
             py::arg("expanded") = true,
             py::arg("key") = "",
             py::arg("build") = py::none(),
             py::keep_alive<0, 1>(),
             "Add a collapsible section (header + body); fill the body via "
             "build. Clicking the header toggles collapse.")
        .def("find_widget",
             [](affineui::View& view, const std::string& name) {
                 return view.find_widget(name);
             },
             py::arg("name"),
             py::keep_alive<0, 1>(),
             "Return a stable WidgetRef by user key. Empty refs are safe.")
        // ── App-shell / structural component builders ───────────────────
        // Scope builders take a Pythonic `build` callback (called immediately
        // with the same View) instead of exposing a raw RAII Scope to Python;
        // every returned WidgetRef keeps the View alive (keep_alive<0,1>).
        .def("toolbar",
             [](affineui::View& view, const std::string& key, py::object build) {
                 auto scope = view.toolbar(key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(&view);
                 return ref;
             },
             py::arg("key") = "", py::arg("build") = py::none(),
             py::keep_alive<0, 1>(),
             "Add a toolbar row; fill it via the build callback.")
        .def("toolbar_separator",
             [](affineui::View& view, const std::string& key) {
                 return view.toolbar_separator(key);
             },
             py::arg("key") = "", py::keep_alive<0, 1>(),
             "Add a separator inside a toolbar.")
        .def("icon_button",
             [](affineui::View& view, const std::string& icon,
                const std::string& key) {
                 return view.icon_button(icon, key);
             },
             py::arg("icon"), py::arg("key") = "", py::keep_alive<0, 1>(),
             "Add an icon-only ghost button (icon = Decius icon name).")
        .def("menu_bar",
             [](affineui::View& view, const std::string& key, py::object build) {
                 auto scope = view.menu_bar(key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(&view);
                 return ref;
             },
             py::arg("key") = "", py::arg("build") = py::none(),
             py::keep_alive<0, 1>(),
             "Add a menubar row; fill it with menu_button()s.")
        .def("menu_button",
             [](affineui::View& view, const std::string& label,
                const std::string& menu_id, const std::string& key) {
                 return view.menu_button(label, menu_id, key);
             },
             py::arg("label"), py::arg("menu_id"), py::arg("key") = "",
             py::keep_alive<0, 1>(),
             "Add a menubar button that opens the menu with id menu_id.")
        .def("menu_button",
             [](affineui::View& view, const std::string& label,
                py::function build, const std::string& key) {
                 return view.menu_button(
                     label,
                     [&build](affineui::View& v) { build(&v); },
                     key);
             },
             py::arg("label"), py::arg("build"), py::arg("key") = "",
             py::keep_alive<0, 1>(),
             "Add a menubar button that OWNS its dropdown: build(view) "
             "populates the menu inline (menu_item/menu_separator/submenu).")
        .def("menu",
             [](affineui::View& view, const std::string& menu_id,
                py::function build) {
                 return view.menu(
                     menu_id,
                     [&build](affineui::View& v) { build(&v); });
             },
             py::arg("menu_id"), py::arg("build"),
             py::keep_alive<0, 1>(),
             "Add a popup menu (hidden until a menu_button targets its id); "
             "build(view) populates it.")
        .def("menu_item",
             [](affineui::View& view, const std::string& label,
                const std::string& icon, const std::string& shortcut,
                const std::string& key) {
                 return view.menu_item(label, icon, shortcut, key);
             },
             py::arg("label"), py::arg("icon") = "", py::arg("shortcut") = "",
             py::arg("key") = "", py::keep_alive<0, 1>(),
             "Add a menu row (optional icon glyph + label + shortcut). Wire "
             "on_click for the action.")
        .def("menu_item_custom",
             [](affineui::View& view, const std::string& key, py::object build) {
                 auto scope = view.menu_item_custom(key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(&view);
                 return ref;
             },
             py::arg("key") = "", py::arg("build") = py::none(),
             py::keep_alive<0, 1>(),
             "Add a menu row whose content the caller composes via build; "
             "activation behaves like menu_item.")
        .def("menu_separator",
             [](affineui::View& view, const std::string& key) {
                 return view.menu_separator(key);
             },
             py::arg("key") = "", py::keep_alive<0, 1>(),
             "Add a separator line between menu groups.")
        .def("submenu",
             [](affineui::View& view, const std::string& label,
                py::function build, const std::string& icon,
                const std::string& key) {
                 return view.submenu(
                     label,
                     [&build](affineui::View& v) { build(&v); },
                     icon, key);
             },
             py::arg("label"), py::arg("build"), py::arg("icon") = "",
             py::arg("key") = "", py::keep_alive<0, 1>(),
             "Add a submenu row revealing nested items (build) on hover.")
        .def("menu_brand",
             [](affineui::View& view, const std::string& title,
                const std::string& icon, const std::string& key) {
                 return view.menu_brand(title, icon, key);
             },
             py::arg("title"), py::arg("icon") = "", py::arg("key") = "",
             py::keep_alive<0, 1>(),
             "Add the app brand (icon + title) at the start of a menubar.")
        .def("menu_spacer",
             [](affineui::View& view, const std::string& key) {
                 return view.menu_spacer(key);
             },
             py::arg("key") = "", py::keep_alive<0, 1>(),
             "Add a flexible spacer pushing following menubar items right.")
        .def("menu_meta",
             [](affineui::View& view, const std::string& text,
                const std::string& key) {
                 return view.menu_meta(text, key);
             },
             py::arg("text"), py::arg("key") = "", py::keep_alive<0, 1>(),
             "Add right-aligned status/meta text in a menubar.")
        .def("dock_panel",
             [](affineui::View& view, const std::string& title,
                const std::string& tabpanel_id, const std::string& classes,
                const std::string& key, py::object build) {
                 auto scope = view.dock_panel(title, tabpanel_id, classes, key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(&view);
                 return ref;
             },
             py::arg("title"), py::arg("tabpanel_id"), py::arg("classes") = "",
             py::arg("key") = "", py::arg("build") = py::none(),
             py::keep_alive<0, 1>(),
             "Add a dockable panel (titled tab + body); fill the body via build.")
        .def("splitter",
             [](affineui::View& view, bool horizontal, const std::string& key) {
                 return view.splitter(horizontal, key);
             },
             py::arg("horizontal") = false, py::arg("key") = "",
             py::keep_alive<0, 1>(),
             "Add a drag splitter between docked regions.")
        .def("tree",
             [](affineui::View& view, const std::string& key, py::object build) {
                 auto scope = view.tree(key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(&view);
                 return ref;
             },
             py::arg("key") = "", py::arg("build") = py::none(),
             py::keep_alive<0, 1>(),
             "Add a tree container; fill it with tree_row()s.")
        .def("tree_row",
             [](affineui::View& view, const std::string& label, bool selected,
                int depth, const std::string& key) {
                 return view.tree_row(label, selected, depth, key);
             },
             py::arg("label"), py::arg("selected") = false, py::arg("depth") = 0,
             py::arg("key") = "", py::keep_alive<0, 1>(),
             "Add a selectable tree row at the given depth.")
        .def("status_bar",
             [](affineui::View& view, const std::string& key, py::object build) {
                 auto scope = view.status_bar(key);
                 auto ref = scope.ref();
                 if (!build.is_none()) build(&view);
                 return ref;
             },
             py::arg("key") = "", py::arg("build") = py::none(),
             py::keep_alive<0, 1>(),
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
             py::keep_alive<0, 1>(),
             "Add a color field that opens a swatch picker popup.")
        .def("to_html_fragment", &affineui::View::to_html_fragment)
        .def("to_html_document", &affineui::View::to_html_document)
        // ── Strongly-typed component queries ────────────────────────────
        // Each returns a typed wrapper over a WidgetRef. Querying the wrong
        // type yields a wrapper whose .validity is WrongType (still attached,
        // but typed accessors are inert) and logs a diagnostic; a missing id
        // yields NotPresent. Never raises / never crashes. keep_alive<0,1>
        // ties the wrapper (and its inner ref) to this View.
        .def("button_at", &affineui::View::component<affineui::Button>,
             py::arg("name"), py::keep_alive<0, 1>())
        .def("checkbox_at", &affineui::View::component<affineui::Checkbox>,
             py::arg("name"), py::keep_alive<0, 1>())
        .def("text_field_at", &affineui::View::component<affineui::TextField>,
             py::arg("name"), py::keep_alive<0, 1>())
        .def("dropdown_at", &affineui::View::component<affineui::Dropdown>,
             py::arg("name"), py::keep_alive<0, 1>())
        .def("slider_at", &affineui::View::component<affineui::Slider>,
             py::arg("name"), py::keep_alive<0, 1>())
        .def("color_field_at", &affineui::View::component<affineui::ColorField>,
             py::arg("name"), py::keep_alive<0, 1>())
        .def("dock_panel_at", &affineui::View::component<affineui::DockPanel>,
             py::arg("name"), py::keep_alive<0, 1>())
        .def("foldout_at", &affineui::View::component<affineui::Foldout>,
             py::arg("name"), py::keep_alive<0, 1>());

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
                          bool no_bundle_decius) {
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
                                      no_bundle_decius));
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
              py::arg("no_bundle_decius") = false)
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
