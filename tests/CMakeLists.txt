include(FetchContent)

# doctest — single-header, fastest C++ test framework to compile.
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(doctest)

add_executable(affineui_tests
    test_main.cpp
    test_animation.cpp
    test_version.cpp
    test_callback.cpp
    test_object.cpp
    test_inspector.cpp
    test_components.cpp
    test_app_template.cpp
    test_document.cpp
    test_bootstrap.cpp
    test_imm_callsite.cpp
    test_imm_runtime.cpp
    test_view.cpp
    test_virtual_list.cpp
    test_skeuo.cpp
    test_cascade.cpp
    test_display_list.cpp
    test_layout.cpp
    test_lifecycle.cpp
    test_memory.cpp
    test_telemetry.cpp
    test_tools.cpp
    test_tools_client.cpp
    test_reconcile_bench.cpp
    test_engine3d.cpp
)
target_link_libraries(affineui_tests PRIVATE
    affineui::affineui
    affineui::skeuo
    affineui::tools
    doctest::doctest
)
# The app-template tests link the sidecar template library when examples are
# built. (affineui::app is defined under examples/common/app.)
if(TARGET affineui::app)
    target_link_libraries(affineui_tests PRIVATE affineui::app)
endif()
# Same deal for the example 3D engine (examples/core/3dengine).
if(TARGET affineui::engine3d)
    target_link_libraries(affineui_tests PRIVATE affineui::engine3d)
endif()
# Cascade test pokes at internal headers (StyleStore, ResolvedStyle).
# That's the appropriate level of intimacy for a cascade test — the
# public API doesn't expose ComputedStyle by design.
target_include_directories(affineui_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/examples/common
    ${CMAKE_SOURCE_DIR}/examples/core/3dengine
    ${CMAKE_SOURCE_DIR}/extras/browser_server
)
target_compile_definitions(affineui_tests PRIVATE
    AFFINEUI_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    AFFINEUI_EXAMPLES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/examples"
)
if(AFFINEUI_DEPS_AVAILABLE)
    target_include_directories(affineui_tests SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/external/lexbor/source)
    target_link_libraries(affineui_tests PRIVATE
        $<BUILD_INTERFACE:lexbor_static>)
endif()
set_target_properties(affineui_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(affineui_tests)
