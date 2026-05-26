# conformance_test — headless AffineUI side of the conformance harness:
# renders a named test's HTML, replays steps, writes a PPM per snapshot.
# D3D11-only for now (uses ID3D11 offscreen render + readback).
if(WIN32 AND AFFINEUI_BACKEND_RESOLVED STREQUAL "d3d11")
    add_executable(conformance_test main.cpp)
    target_link_libraries(conformance_test PRIVATE affineui::affineui d3d11)
    set_target_properties(conformance_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/conformance")
endif()

add_executable(conformance_viewer viewer.cpp)
target_link_libraries(conformance_viewer PRIVATE affineui::sokol)
set_target_properties(conformance_viewer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/conformance")
