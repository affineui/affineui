cmake_minimum_required(VERSION 3.20)

# CMake 4 removed compatibility with cmake_minimum_required < 3.5. Several
# bundled third-party CMake projects (doctest, etc.) still ship those old
# declarations, so we raise the policy floor here so they keep configuring.
# Applies to FetchContent'd / add_subdirectory'd targets too because the
# variable is read at every `cmake_minimum_required()` call.
if(POLICY CMP0000)
    cmake_policy(VERSION 3.20)
endif()
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

# ─── Project ─────────────────────────────────────────────────────────────────
project(affineui
    VERSION 0.0.1
    DESCRIPTION "Tiny GPU-accelerated HTML5/CSS UI renderer for C++"
    LANGUAGES C CXX
)
# sokol_app's macOS implementation is Objective-C (AppKit/Foundation).
# Enable OBJC only on Apple platforms so set_source_files_properties(LANGUAGE OBJC)
# in cmake/Dependencies.cmake takes effect there.
if(APPLE)
    enable_language(OBJC)
endif()

# Detect top-level invocation (so subprojects don't get our examples/tests)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(AFFINEUI_IS_TOPLEVEL ON)
else()
    set(AFFINEUI_IS_TOPLEVEL OFF)
endif()

# ─── Options ─────────────────────────────────────────────────────────────────
option(AFFINEUI_BUILD_EXAMPLES  "Build examples"               ${AFFINEUI_IS_TOPLEVEL})
option(AFFINEUI_BUILD_TESTS     "Build tests"                  ${AFFINEUI_IS_TOPLEVEL})
option(AFFINEUI_BUILD_TOOLS     "Build tools"                  ${AFFINEUI_IS_TOPLEVEL})
option(AFFINEUI_BUILD_PYTHON_BINDINGS "Build Python extension module" OFF)
option(AFFINEUI_BUILD_SHARED    "Build as shared library"      OFF)
option(AFFINEUI_INSTALL         "Install AffineUI library and CMake package config" ON)
option(AFFINEUI_ENABLE_IMM      "Enable immediate-mode layer"  ON)
option(AFFINEUI_ENABLE_C_API    "Build the extern \"C\" surface" ON)
option(AFFINEUI_BUILD_C_SHARED  "Build the affineui_c shared library for language bindings" OFF)
# Embed the Decius CSS framework + a small set of woff2 fonts into affineui
# so `View(ViewTheme::Decius)` renders with the intended look out of the
# box (no on-disk asset copying). ~140 KB of zlib-compressed data added
# to the compiled binary. The two-file drop-in defaults ON as well; the
# host opts out with -DAFFINEUI_NO_BUNDLE_DECIUS.
option(AFFINEUI_BUNDLE_DECIUS   "Embed the Decius CSS framework as compile-time data" ON)
option(AFFINEUI_WARNINGS_AS_ERRORS "Treat warnings as errors"  OFF)
option(AFFINEUI_HTML_ENTITIES_FULL "Use Lexbor's full HTML5 named entity table" OFF)
option(AFFINEUI_NO_EMBEDDED_FONTS  "Don't embed the default Roboto font (use host/system fonts)" OFF)
set(AFFINEUI_BACKEND "auto" CACHE STRING "GPU backend: auto|metal|d3d11|gl|wgpu")

# ── "Bring your own" options for hosts that already vendor these deps ────────
# When ON: AffineUI does NOT compile the dep's implementation TU, does NOT
# define its IMPL macro, and skips its CMake subproject if applicable. The
# host is responsible for providing the symbols. Header include paths must
# still resolve (typically via the host's own target_include_directories).
option(AFFINEUI_HOST_PROVIDES_SOKOL        "Host already implements sokol_*"     OFF)
option(AFFINEUI_HOST_PROVIDES_NANOVG       "Host already implements NanoVG"      OFF)
option(AFFINEUI_HOST_PROVIDES_STB_IMAGE    "Host already implements stb_image"   OFF)
option(AFFINEUI_HOST_PROVIDES_STB_TRUETYPE "Host already implements stb_truetype" OFF)
option(AFFINEUI_HOST_PROVIDES_FONTSTASH    "Host already implements fontstash"   OFF)
option(AFFINEUI_HOST_PROVIDES_LEXBOR       "Host already builds lexbor"          OFF)
option(AFFINEUI_HOST_PROVIDES_YOGA         "Host already builds Yoga"            OFF)

# ─── Defaults ────────────────────────────────────────────────────────────────
# We compile in C++23 mode to harden our own build: C++23 makes the standard
# library's string / string_view constructors from std::nullptr_t `= delete`,
# so passing nullptr where a string is expected becomes a COMPILE error instead
# of runtime UB (we hit exactly that footgun once). The code uses NO C++23
# language or library features, so the baseline standard is 20 — that's what
# CI verifies, what every wheel ships against, and what the amalgamated
# single-header SDK promises to consumers. Local development can opt into 23
# via -DAFFINEUI_DEV_CXX23=ON (the `dev` preset enables it by default) to
# get the extra nullptr-to-string_view safety net.
option(AFFINEUI_DEV_CXX23
    "Use C++23 locally for extra deleted-nullptr-ctor protection. CI and \
wheel builds stay on C++20." OFF)
if(AFFINEUI_DEV_CXX23)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release MinSizeRel RelWithDebInfo)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Helper modules ──────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(BackendDetect)
include(Dependencies)

# ─── Resolve backend ─────────────────────────────────────────────────────────
affineui_detect_backend(AFFINEUI_BACKEND_RESOLVED ${AFFINEUI_BACKEND})
message(STATUS "AffineUI backend: ${AFFINEUI_BACKEND_RESOLVED}")

# ─── Resolve vendored deps (gracefully no-ops when external/ is empty) ───────
affineui_check_vendored_deps(AFFINEUI_DEPS_AVAILABLE)
if(NOT AFFINEUI_DEPS_AVAILABLE)
    message(WARNING
        "Vendored dependencies not present under external/. The library will "
        "build a stub (compile-only) so CMake still configures and tests can "
        "run. Restore the checked-in external/ sources to enable real rendering.")
endif()

# ─── Library target ──────────────────────────────────────────────────────────
if(AFFINEUI_BUILD_SHARED)
    add_library(affineui SHARED)
else()
    add_library(affineui STATIC)
endif()
add_library(affineui::affineui ALIAS affineui)

# Allocation tracking (affineui::mem): live-block list, per-block leak report,
# double-free detection, and alloc/free poison fills. Cheap counters are always
# on; this option adds the heavier per-block tracking. Default ON for Debug.
option(AFFINEUI_MEM_DEBUG "Track every allocation for leak/UAF/double-free detection" OFF)
if(AFFINEUI_MEM_DEBUG OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(affineui PUBLIC AFFINEUI_MEM_DEBUG)
endif()

target_sources(affineui PRIVATE
    src/core/affineui.cpp
    src/core/memory.cpp
    src/core/log.cpp
    src/core/diag/sampler.cpp
    src/core/diag/telemetry.cpp
    src/core/tools/tools_server.cpp
    external/tinyjson/tiny-json.c
    src/framework/ui.cpp
    src/framework/themes.cpp
    src/framework/app/app.cpp
    src/framework/app/event.cpp
    src/framework/app/automation.cpp
    src/framework/app/view.cpp
    # macOS-only helper: drain the NSEvent queue per app pulse (see file
    # header for rationale). Attached as a source unconditionally; empty
    # on non-Apple platforms via #ifdef __APPLE__ inside the file.
    $<$<PLATFORM_ID:Darwin>:src/framework/app/app_mac_native.mm>
    src/renderer/dom/document_core.cpp
    src/renderer/dom/document_controls.cpp
    src/renderer/dom/document_dispatch.cpp
    src/renderer/dom/document_draw.cpp
    src/renderer/dom/document_dock.cpp
    src/renderer/dom/document_layout.cpp
    src/renderer/dom/document_restyle.cpp
    src/renderer/dom/document_style.cpp
    src/renderer/dom/document_text.cpp
    src/renderer/dom/document_tools.cpp
    src/renderer/dom/dom_view.cpp
    src/renderer/style/cascade.cpp
    src/renderer/style/computed.cpp
    src/renderer/style/selector_bridge.cpp
    src/renderer/style/style_store.cpp
    src/renderer/style/units.cpp
    src/renderer/layout/box.cpp
    src/renderer/layout/block.cpp
    src/renderer/layout/inline.cpp
    src/renderer/layout/flex.cpp
    src/renderer/layout/yoga_adapter.cpp
    src/renderer/text/shaper.cpp
    src/renderer/text/line_breaker.cpp
    src/renderer/text/font_manager.cpp
    src/renderer/paint/paint_driver.cpp
    src/renderer/paint/image_loader.cpp
    # NOTE: src/renderer/paint/layer.cpp and src/renderer/paint/composite.cpp were the raw-GL
    # FBO/compositor path. They're temporarily out of the build while the
    # renderer paints NanoVG (via sokol_gfx) straight to the swapchain;
    # they'll be reintroduced on top of sokol_gfx render targets (M2).

    # Renderer — graphics-only, windowing-toolkit-independent. App
    # delegates frame rendering to this; future SDL/glfw adapters use
    # the same Renderer through the same public interface.
    src/renderer/render/renderer.cpp

    # Engine facade + foundational data structures (5-stage pipeline)
    src/renderer/engine/box.cpp
    src/renderer/engine/layer.cpp
    src/renderer/engine/display_list.cpp
    src/renderer/engine/restyle_queue.cpp
    src/renderer/engine/animation.cpp
    src/renderer/engine/style_engine.cpp
    src/renderer/engine/layout_engine.cpp
    src/renderer/engine/paint_engine.cpp
    src/renderer/engine/rasterizer.cpp
    src/renderer/engine/compositor.cpp
    src/renderer/engine/engine.cpp
)

if(AFFINEUI_DEPS_AVAILABLE)
    target_sources(affineui PRIVATE
        src/renderer/dom/lexbor_bridge.cpp
        src/renderer/paint/nanovg_painter.cpp
        src/renderer/paint/nanovg_sokol.c   # NanoVG-on-sokol_gfx backend (replaces nanovg_impl.c)
        src/renderer/paint/sokol_impl.c
        src/renderer/text/fontstash_impl.c
        src/renderer/text/stb_impl.c
    )
endif()

if(AFFINEUI_ENABLE_IMM)
    target_sources(affineui PRIVATE
        src/framework/imm/imm.cpp
        src/framework/imm/imm_runtime.cpp
        src/framework/imm/reconciler.cpp
        src/framework/imm/state_store.cpp
    )
endif()

if(AFFINEUI_ENABLE_C_API)
    target_sources(affineui PRIVATE src/c_api.cpp src/c_api_app.cpp)
endif()

# Compile-time bundled Decius CSS framework. The generated header lives
# at include/affineui/_decius_bundle.h (leading underscore = generated,
# gitignored). Regenerating it is driven by DEPENDS on the source assets
# so `cmake --build` picks up an updated CSS or icon font automatically.
if(AFFINEUI_BUNDLE_DECIUS)
    set(_decius_gen "${CMAKE_CURRENT_SOURCE_DIR}/include/affineui/_decius_bundle.h")
    set(_decius_src
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/frameworks/css/decius-css-0.6.2.bundle.min.css"
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/frameworks/fonts/decius-icons.ttf")
    # Ensure Python3 is on hand; the other python-driven build steps
    # (tools/amalgamate.py, etc.) also rely on it, so a simple find_
    # package here composes fine.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(
        OUTPUT  "${_decius_gen}"
        COMMAND ${CMAKE_COMMAND} -E echo "▸ embedding Decius bundle → ${_decius_gen}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_decius.py" "${_decius_gen}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_decius.py" ${_decius_src}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Generating Decius CSS bundle header")
    add_custom_target(affineui_decius_bundle_gen DEPENDS "${_decius_gen}")
    add_dependencies(affineui affineui_decius_bundle_gen)
    target_sources(affineui PRIVATE src/decius_bundle.cpp "${_decius_gen}")
endif()

target_include_directories(affineui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        # tiny-json: single-header JSON parser used by tools_server.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/external/tinyjson
)

target_compile_definitions(affineui
    PUBLIC
        AFFINEUI_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        AFFINEUI_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        AFFINEUI_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    PRIVATE
        $<$<NOT:$<BOOL:${AFFINEUI_ENABLE_IMM}>>:AFFINEUI_NO_IMM>
        $<$<NOT:$<BOOL:${AFFINEUI_ENABLE_C_API}>>:AFFINEUI_NO_C_API>
        $<$<NOT:$<BOOL:${AFFINEUI_BUNDLE_DECIUS}>>:AFFINEUI_NO_BUNDLE_DECIUS>
        $<$<NOT:$<BOOL:${AFFINEUI_DEPS_AVAILABLE}>>:AFFINEUI_STUB_BUILD>
)

affineui_set_warnings(affineui ${AFFINEUI_WARNINGS_AS_ERRORS})
affineui_link_backend(affineui ${AFFINEUI_BACKEND_RESOLVED})

if(AFFINEUI_DEPS_AVAILABLE)
    affineui_link_vendored_deps(affineui)
endif()

# ─── Embedded default font (Roboto) ──────────────────────────────────────────
# Embed the vendored font into the library so text renders identically on
# every platform with no runtime asset path. The .ttf files under
# assets/fonts are the committed source of truth; a tiny bin2c host tool
# (cmake/bin2c.c) turns them into byte-array headers in the build tree.
# Apps can register their own fonts on top; pass AFFINEUI_NO_EMBEDDED_FONTS
# to drop the default and fall back to host/system fonts.
if(AFFINEUI_DEPS_AVAILABLE AND NOT AFFINEUI_NO_EMBEDDED_FONTS)
    add_executable(affineui_bin2c cmake/bin2c.c)
    set(_affineui_fontgen "${CMAKE_BINARY_DIR}/generated/affineui_fonts")
    file(MAKE_DIRECTORY "${_affineui_fontgen}")

    function(affineui_embed_font ttf symbol header)
        add_custom_command(
            OUTPUT  "${_affineui_fontgen}/${header}"
            COMMAND affineui_bin2c
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/${ttf}"
                    "${_affineui_fontgen}/${header}"
                    "${symbol}"
            DEPENDS affineui_bin2c "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/${ttf}"
            VERBATIM)
    endfunction()

    affineui_embed_font("Roboto-Regular.ttf" "affineui_font_roboto_regular" "roboto_regular.h")
    affineui_embed_font("Roboto-Bold.ttf"    "affineui_font_roboto_bold"    "roboto_bold.h")

    add_custom_target(affineui_fonts_gen DEPENDS
        "${_affineui_fontgen}/roboto_regular.h"
        "${_affineui_fontgen}/roboto_bold.h")
    add_dependencies(affineui affineui_fonts_gen)
    target_include_directories(affineui PRIVATE "${_affineui_fontgen}")
    target_compile_definitions(affineui PRIVATE AFFINEUI_HAVE_EMBEDDED_FONTS)
endif()

# Platform link libs that sokol_app + NanoVG (GL backend) need.
# Frameworks/libs are picked to match the resolved backend.
if(APPLE)
    target_link_libraries(affineui PUBLIC
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework AudioToolbox"
    )
    if(AFFINEUI_BACKEND_RESOLVED STREQUAL "gl")
        target_link_libraries(affineui PUBLIC "-framework OpenGL")
    elseif(AFFINEUI_BACKEND_RESOLVED STREQUAL "metal")
        target_link_libraries(affineui PUBLIC
            "-framework Metal" "-framework MetalKit")
    endif()
elseif(UNIX)
    find_package(X11 REQUIRED)
    target_link_libraries(affineui PUBLIC
        ${X11_LIBRARIES} ${X11_Xi_LIB} ${X11_Xcursor_LIB}
        GL dl m pthread
    )
elseif(WIN32)
    # ws2_32: affinetools protocol server (src/core/tools/tools_server.cpp)
    target_link_libraries(affineui PUBLIC user32 gdi32 ole32 shell32 ws2_32)
    if(AFFINEUI_BACKEND_RESOLVED STREQUAL "gl")
        target_link_libraries(affineui PUBLIC opengl32)
    elseif(AFFINEUI_BACKEND_RESOLVED STREQUAL "d3d11")
        target_link_libraries(affineui PUBLIC d3d11 dxgi)
    endif()
endif()

# ─── Adapter interface targets ───────────────────────────────────────────────
# Adapters are opt-in — link the one matching your windowing toolkit.
# Each target adds the corresponding header path + defines the
# AFFINEUI_WITH_* macro the umbrella header looks for.
#
#   target_link_libraries(my_game PRIVATE affineui::sokol)
#   target_link_libraries(my_game PRIVATE affineui::sdl)    # Phase D
#
# sokol_app's implementation is already built into libaffineui (so
# both the core library and your game see the same sokol symbols).
add_library(affineui_sokol INTERFACE)
add_library(affineui::sokol ALIAS affineui_sokol)
target_link_libraries(affineui_sokol INTERFACE affineui::affineui)
target_compile_definitions(affineui_sokol INTERFACE AFFINEUI_WITH_SOKOL)
target_include_directories(affineui_sokol SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/sokol>)

# SDL2 adapter — only available when SDL2 is found on the host. Games
# using SDL2 link against affineui::sdl which transitively gives them
# the AFFINEUI_WITH_SDL macro + SDL2 link deps. Falls back gracefully
# when SDL2 isn't installed (the target just isn't created — examples
# that depend on it get skipped).
find_package(SDL2 QUIET CONFIG)
if(NOT SDL2_FOUND)
    find_package(SDL2 QUIET)
endif()
if(SDL2_FOUND)
    add_library(affineui_sdl INTERFACE)
    add_library(affineui::sdl ALIAS affineui_sdl)
    target_link_libraries(affineui_sdl INTERFACE affineui::affineui)
    target_compile_definitions(affineui_sdl INTERFACE AFFINEUI_WITH_SDL)
    # SDL2::SDL2 (from CONFIG) or SDL2_LIBRARIES (from FindSDL2):
    if(TARGET SDL2::SDL2)
        target_link_libraries(affineui_sdl INTERFACE SDL2::SDL2)
    else()
        target_link_libraries(affineui_sdl INTERFACE ${SDL2_LIBRARIES})
        target_include_directories(affineui_sdl SYSTEM INTERFACE ${SDL2_INCLUDE_DIRS})
    endif()
    set(AFFINEUI_HAVE_SDL2 ON)
    message(STATUS "AffineUI: SDL2 adapter enabled (SDL2 ${SDL2_VERSION})")
else()
    set(AFFINEUI_HAVE_SDL2 OFF)
    message(STATUS "AffineUI: SDL2 not found, affineui::sdl target unavailable")
endif()

# ─── affineui_c: shared C-ABI library for language bindings ──────────────────
# One artifact (affineui_c.dll / libaffineui_c.{so,dylib}) consumed by the
# Rust / C# / future wrappers (see docs/LANGUAGE_BINDINGS.md). Recompiles the
# extern "C" TUs with the dllexport decoration and links the static core.
if(AFFINEUI_BUILD_C_SHARED)
    add_library(affineui_c SHARED src/c_api.cpp src/c_api_app.cpp)
    add_library(affineui::c ALIAS affineui_c)
    target_link_libraries(affineui_c PRIVATE affineui::affineui)
    target_include_directories(affineui_c PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(affineui_c PRIVATE AFFINEUI_C_BUILD_DLL)
    affineui_set_warnings(affineui_c ${AFFINEUI_WARNINGS_AS_ERRORS})
endif()

# ─── Subdirectories ──────────────────────────────────────────────────────────
# Optional extras: the skeuomorphic hardware widget kit (jacks, patch
# cables, 7-seg LCDs, LED meters). Separate from the core widgets;
# examples link it explicitly.
add_subdirectory(extras/skeuo)
# The affinetools devtools client library (extras/tools) + viewer app
# (tools/affinetools) — see docs/AFFINETOOLS_DESIGN.md.
add_subdirectory(extras/tools)
if(AFFINEUI_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(AFFINEUI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(AFFINEUI_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(AFFINEUI_BUILD_PYTHON_BINDINGS)
    add_subdirectory(bindings/python)
endif()

# ─── Install / package config ────────────────────────────────────────────────
if(AFFINEUI_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS affineui
        EXPORT affineuiTargets
        LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    if(AFFINEUI_BUILD_C_SHARED)
        install(TARGETS affineui_c
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT affineuiTargets
        FILE affineuiTargets.cmake
        NAMESPACE affineui::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/affineui
    )
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/affineuiConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/affineuiConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/affineui
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/affineuiConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/affineuiConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/affineuiConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/affineui
    )
endif()

# ─── Summary ─────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "AffineUI ${PROJECT_VERSION}")
message(STATUS "  Backend:        ${AFFINEUI_BACKEND_RESOLVED}")
message(STATUS "  Deps available: ${AFFINEUI_DEPS_AVAILABLE}")
message(STATUS "  Imm mode:       ${AFFINEUI_ENABLE_IMM}")
message(STATUS "  C ABI:          ${AFFINEUI_ENABLE_C_API}")
message(STATUS "  C shared lib:   ${AFFINEUI_BUILD_C_SHARED}")
message(STATUS "  Examples:       ${AFFINEUI_BUILD_EXAMPLES}")
message(STATUS "  Tests:          ${AFFINEUI_BUILD_TESTS}")
message(STATUS "  Tools:          ${AFFINEUI_BUILD_TOOLS}")
message(STATUS "  Python:         ${AFFINEUI_BUILD_PYTHON_BINDINGS}")
message(STATUS "  Shared:         ${AFFINEUI_BUILD_SHARED}")
message(STATUS "  Install:        ${AFFINEUI_INSTALL}")
message(STATUS "")
