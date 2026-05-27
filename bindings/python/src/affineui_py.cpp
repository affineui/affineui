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
                                      const std::vector<std::string>& asset_folders) {
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
        .def("clear_scripts", &affineui::Document::clear_scripts);

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
             });

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
             py::arg("key") = "")
        .def("paragraph",
             [](affineui::View& view,
                const std::string& text,
                const std::string& classes,
                const std::string& key) {
                 return view.paragraph(text, classes, key);
             },
             py::arg("text"),
             py::arg("classes") = "",
             py::arg("key") = "")
        .def("button",
             [](affineui::View& view,
                const std::string& label,
                bool primary,
                const std::string& key) {
                 return view.button(label, primary, key);
             },
             py::arg("label"),
             py::arg("primary") = false,
             py::arg("key") = "")
        .def("checkbox",
             [](affineui::View& view,
                const std::string& label,
                bool checked,
                const std::string& key) {
                 return view.checkbox(label, checked, key);
             },
             py::arg("label"),
             py::arg("checked"),
             py::arg("key") = "")
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
             py::arg("key") = "")
        .def("password",
             [](affineui::View& view,
                const std::string& label,
                const std::string& value,
                const std::string& key) {
                 return view.password(label, value, key);
             },
             py::arg("label"),
             py::arg("value") = "",
             py::arg("key") = "")
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
             py::arg("key") = "")
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
             py::arg("key") = "")
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
             py::arg("key") = "")
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
             py::arg("classes") = "")
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
             py::arg("key") = "")
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
             py::arg("key") = "")
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
             py::arg("build") = py::none())
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
             py::arg("build") = py::none())
        .def("find_widget",
             [](affineui::View& view, const std::string& name) {
                 return view.find_widget(name);
             })
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
                          const std::vector<std::string>& asset_folders) {
                  return std::make_unique<affineui::App>(
                      make_app_config(title,
                                      width,
                                     height,
                                     clear_color,
                                     high_dpi,
                                      vsync,
                                      default_font_family,
                                      default_font_size,
                                      asset_folders));
              }),
             py::arg("title") = "AffineUI",
             py::arg("width") = 1024,
             py::arg("height") = 768,
             py::arg("clear_color") = affineui::Color{30, 30, 46, 255},
             py::arg("high_dpi") = true,
              py::arg("vsync") = true,
              py::arg("default_font_family") = "sans-serif",
              py::arg("default_font_size") = 16,
              py::arg("asset_folders") = std::vector<std::string>{"."})
        .def("load_html", [](affineui::App& app, const std::string& html) {
            app.load_html(html);
        })
        .def("load_view", &affineui::App::load_view)
        .def("load_html_file",
             [](affineui::App& app, const std::string& path) {
                 return app.load_html_file(path);
             })
        .def("set_stylesheet",
             [](affineui::App& app, const std::string& css) {
                 app.set_stylesheet(css);
             })
        .def("invalidate", &affineui::App::invalidate)
        .def("quit", &affineui::App::quit, py::arg("code") = 0)
        .def("window_size", &affineui::App::window_size)
        .def("dpi_scale", &affineui::App::dpi_scale)
        .def("document",
             static_cast<affineui::Document& (affineui::App::*)()>(
                 &affineui::App::document),
             py::return_value_policy::reference_internal)
        .def("run",
             [](affineui::App& app) {
                 py::gil_scoped_release release;
                 return app.run();
             })
        .def("launch",
             [](affineui::App& app, bool native) {
                 if (!native) {
                     throw py::value_error(
                         "Only native=True is available in this POC; "
                         "remote browser transport is not implemented yet.");
                 }
                 py::gil_scoped_release release;
                 return app.run();
             },
             py::arg("native") = true);
}
