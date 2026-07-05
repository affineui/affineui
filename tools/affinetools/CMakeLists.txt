# affinetools — the AffineUI devtools viewer executable (thin shell over
# extras/tools' affineui::tools client library). F12 in any App-based app
# finds this exe (beside the app, in the build tree, or on PATH) and
# launches it attached — see affineui::tools_open_devtools().

add_executable(affinetools main.cpp)
target_link_libraries(affinetools PRIVATE affineui::affineui affineui::tools)
target_compile_features(affinetools PRIVATE cxx_std_23)

# A GUI tool must not flash a console window. Build against the Windows
# subsystem (WIN32_EXECUTABLE) and keep plain main() as the entry point
# (/ENTRY:mainCRTStartup) — the exact recipe the example apps use. Also
# embed the per-monitor-v2 DPI manifest so the window is crisp on hi-dpi.
if(WIN32)
    set_target_properties(affinetools PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(affinetools PRIVATE "/ENTRY:mainCRTStartup")
        target_sources(affinetools PRIVATE
            "${CMAKE_SOURCE_DIR}/examples/affineui_dpi.manifest")
    endif()
endif()

# Self-contained assets: embed the decius bundle CSS and the icon font
# directly in the exe (bin2c → byte-array headers), so the viewer needs no
# frameworks/ directory beside it. The icon TTF is raw SFNT (no woff2
# decode needed); the IBM Plex text fonts the bundle references are left to
# fall back to core's embedded Roboto. main.cpp serves these bytes through
# an in-memory resource loader.
if(TARGET affineui_bin2c)
    set(_aft_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_aft_gen}")
    set(_aft_css "${CMAKE_SOURCE_DIR}/examples/frameworks/css/decius-css-0.6.2.bundle.min.css")
    set(_aft_ttf "${CMAKE_SOURCE_DIR}/examples/frameworks/fonts/decius-icons.ttf")
    add_custom_command(
        OUTPUT "${_aft_gen}/decius_bundle_css.h"
        COMMAND affineui_bin2c "${_aft_css}"
                "${_aft_gen}/decius_bundle_css.h" "aft_decius_bundle_css"
        DEPENDS affineui_bin2c "${_aft_css}" VERBATIM)
    add_custom_command(
        OUTPUT "${_aft_gen}/decius_icons_ttf.h"
        COMMAND affineui_bin2c "${_aft_ttf}"
                "${_aft_gen}/decius_icons_ttf.h" "aft_decius_icons_ttf"
        DEPENDS affineui_bin2c "${_aft_ttf}" VERBATIM)
    add_custom_target(affinetools_assets DEPENDS
        "${_aft_gen}/decius_bundle_css.h"
        "${_aft_gen}/decius_icons_ttf.h")
    add_dependencies(affinetools affinetools_assets)
    target_include_directories(affinetools PRIVATE "${_aft_gen}")
    target_compile_definitions(affinetools PRIVATE AFT_EMBEDDED_ASSETS)
else()
    message(WARNING "affinetools: affineui_bin2c unavailable; the viewer "
                    "will look for a frameworks/ dir beside the exe.")
endif()
