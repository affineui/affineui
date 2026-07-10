# photoedit — EXAMPLE / TEMPLATE code (not part of the AffineUI library).
#
# The raster photo-editing core behind the Decius Photo Edit sample: real
# RGBA layers, canvas-style blend compositing, a brush engine, selections,
# adjustments, and pixel-snapshot history. It is the C++ analog of the
# 3dengine sidecar — statically linked by whatever front-end drives it, and
# the framework itself never depends on it.
#
# Two artifacts:
#   * affineui_photoedit  — the pure C++ raster core (photo_core.cpp).
#   * photo_core          — a pybind11 extension module wrapping it, built
#                           only when the Python bindings are (pybind11 is
#                           already in scope there). The Python sample
#                           `import photo_core` alongside `import affineui`.
add_library(affineui_photoedit STATIC
    photo_core.cpp
)
add_library(affineui::photoedit ALIAS affineui_photoedit)

target_include_directories(affineui_photoedit PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
# The brush/type tooling rasterizes glyphs with the vendored stb_truetype.
target_include_directories(affineui_photoedit PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../external/stb")
target_link_libraries(affineui_photoedit PUBLIC affineui::affineui)
target_compile_features(affineui_photoedit PUBLIC cxx_std_20)
# This static lib is linked into the photo_core SHARED python module, so its
# objects must be position-independent — otherwise the manylinux GCC link
# fails with "failed to set dynamic section sizes: bad value".
set_target_properties(affineui_photoedit PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

# Absolute path to the pybind sources, captured now: a function body sees the
# CALLER's CMAKE_CURRENT_SOURCE_DIR, so we cannot rely on it at call time.
set(AFFINEUI_PHOTOEDIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    CACHE INTERNAL "photoedit example source dir")

# The pybind11 module is built on demand by the caller (the Python bindings
# tree) via affineui_photoedit_python_module(); guarded so the pure C++ lib
# still configures in a plain example build with no pybind11 present.
function(affineui_photoedit_python_module)
    if(NOT COMMAND pybind11_add_module)
        message(FATAL_ERROR
            "affineui_photoedit_python_module() needs pybind11 in scope")
    endif()
    pybind11_add_module(photo_core MODULE
        "${AFFINEUI_PHOTOEDIT_DIR}/photo_core_py.cpp"
    )
    target_link_libraries(photo_core PRIVATE affineui::photoedit)
    set_target_properties(photo_core PROPERTIES
        # Land beside the affineui package so `import photo_core` resolves
        # from the same install root.
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()
