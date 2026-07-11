# 3dengine — EXAMPLE / TEMPLATE code (not part of the AffineUI library).
# A small three.js-style 3D engine on sokol_gfx used by the showcase
# samples that need a real GPU viewport (game editor, DENDER). Included
# via "e3d.h". Statically linked by examples; the framework itself never
# depends on it.
add_library(affineui_engine3d STATIC
    e3d_geometry.cpp
    e3d_scene.cpp
    e3d_raycast.cpp
    e3d_controls_orbit.cpp
    e3d_controls_transform.cpp
)
add_library(affineui::engine3d ALIAS affineui_engine3d)

target_include_directories(affineui_engine3d PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(affineui_engine3d PUBLIC affineui::affineui)
# The GPU renderer needs sokol headers + a real (non-stub) sokol_gfx
# implementation in libaffineui; the CPU side (math/scene/raycast)
# builds everywhere so tests always run.
if(TARGET affineui::sokol AND AFFINEUI_DEPS_AVAILABLE)
    target_sources(affineui_engine3d PRIVATE e3d_renderer.cpp)
    target_link_libraries(affineui_engine3d PUBLIC affineui::sokol)
endif()
target_compile_features(affineui_engine3d PUBLIC cxx_std_20)
