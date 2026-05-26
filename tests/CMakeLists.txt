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
    test_document.cpp
    test_bootstrap.cpp
    test_imm_callsite.cpp
    test_imm_runtime.cpp
    test_cascade.cpp
    test_display_list.cpp
    test_layout.cpp
)
target_link_libraries(affineui_tests PRIVATE
    affineui::affineui
    doctest::doctest
)
# Cascade test pokes at internal headers (StyleStore, ResolvedStyle).
# That's the appropriate level of intimacy for a cascade test — the
# public API doesn't expose ComputedStyle by design.
target_include_directories(affineui_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/examples/common
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
