cmake_minimum_required(VERSION 3.24)
project(affineui_macos_input_queue_probe LANGUAGES C CXX OBJC OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "macos_input_queue_probe is macOS-only")
endif()
if(NOT DEFINED AFFINEUI_CHECKOUT)
    message(FATAL_ERROR "set AFFINEUI_CHECKOUT to the AffineUI checkout")
endif()

set(AFFINEUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AFFINEUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AFFINEUI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(AFFINEUI_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
add_subdirectory("${AFFINEUI_CHECKOUT}" affineui)

add_executable(macos_input_queue_probe probe.mm)
target_compile_features(macos_input_queue_probe PRIVATE cxx_std_20)
target_include_directories(macos_input_queue_probe PRIVATE
    "${AFFINEUI_CHECKOUT}/external/sokol")
target_link_libraries(macos_input_queue_probe PRIVATE affineui "-framework AppKit")
