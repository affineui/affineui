cmake_minimum_required(VERSION 3.20)

project(affineui_python
    VERSION 0.0.1
    DESCRIPTION "Python bindings for AffineUI"
    LANGUAGES CXX
)

find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

if(NOT TARGET affineui)
    set(AFFINEUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(AFFINEUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(AFFINEUI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(AFFINEUI_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
    set(AFFINEUI_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../.."
        "${CMAKE_CURRENT_BINARY_DIR}/affineui-core"
    )
endif()

pybind11_add_module(_affineui MODULE
    src/affineui_py.cpp
)

target_link_libraries(_affineui PRIVATE affineui::affineui)
target_compile_features(_affineui PRIVATE cxx_std_20)
target_compile_definitions(_affineui PRIVATE
    AFFINEUI_PY_VERSION="${PROJECT_VERSION}"
)

install(TARGETS _affineui
    LIBRARY DESTINATION affineui
    RUNTIME DESTINATION affineui
)

install(FILES
    "${CMAKE_CURRENT_LIST_DIR}/../../extras/browser_server/affineui_browser_server.h"
    DESTINATION affineui/include
)
