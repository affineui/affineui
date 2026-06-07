#pragma once

#include <string>

namespace ge {

// The small amount of genuinely-custom CSS this app needs: the editor shell
// layout and the faux-3D viewport/gizmo. Everything else (panels, toolbar,
// tree, inspector controls, menus, status bar) is standard Decius styling
// supplied by the component library — so this stays short.
std::string native_css();

}  // namespace ge
