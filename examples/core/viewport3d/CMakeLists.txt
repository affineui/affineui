# viewport3d — EXAMPLE / TEMPLATE code (not part of the AffineUI library).
# A reusable DCC 3D viewport built on the e3d example engine and the
# affineui_app template: an orbit camera + click-picking + transform gizmo
# wired into an app::Context's document / selection / undo stack, composited
# through a custom-paint canvas. Shared by the samples that need a real GPU
# viewport (game editor, DENDER). Included via "viewport3d.h". Statically
# linked by examples; the framework itself never depends on it.
add_library(affineui_viewport3d STATIC
    viewport3d.cpp
)
add_library(affineui::viewport3d ALIAS affineui_viewport3d)

target_include_directories(affineui_viewport3d PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
# app template (Context/Document/Selection/CommandStack) + the e3d engine.
target_link_libraries(affineui_viewport3d PUBLIC affineui::app
                                                 affineui::engine3d)
target_compile_features(affineui_viewport3d PUBLIC cxx_std_20)
