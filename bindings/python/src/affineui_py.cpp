#include <affineui/app.h>
#include <affineui/document.h>
#include <affineui/types.h>
#include <affineui/view.h>
#include <affineui/version.h>

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
                                      bool perf_overlay) {
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
        .value("A", affineui::Key::A)
        .value("C", affineui::Key::C)
        .value("V", affineui::Key::V)
        .value("X", affineui::Key::X);

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
             [](affineui::Document& doc, const std::string& css) {
                 doc.set_user_stylesheet(css);
             })
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
             "Return whether a DomHandle still resolves inside this document.");

    py::enum_<affineui::ViewTheme>(m, "ViewTheme")
        .value("Plain", affineui::ViewTheme::Plain)
        .value("Bootstrap", affineui::ViewTheme::Bootstrap)
        .value("Decius", affineui::ViewTheme::Decius);

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
             py::arg("theme") = affineui::ViewTheme::Bootstrap)
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
        .def("find_widget",
             [](affineui::View& view, const std::string& name) {
                 return view.find_widget(name);
             },
             py::arg("name"),
             py::keep_alive<0, 1>(),
             "Return a stable WidgetRef by user key. Empty refs are safe.")
        .def("to_html_fragment", &affineui::View::to_html_fragment)
        .def("to_html_document", &affineui::View::to_html_document);

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
                          bool perf_overlay) {
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
                                      perf_overlay));
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
              py::arg("perf_overlay") = false)
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
             [](affineui::App& app, const std::string& css) {
                 app.set_stylesheet(css);
             })
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
