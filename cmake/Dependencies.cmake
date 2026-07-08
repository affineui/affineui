# Vendored dependency wiring.
#
# external/ contains curated, checked-in source snapshots. The check
# below reports whether real rendering is buildable; when the deps are
# missing the library still compiles as a stub so CMake configures
# cleanly.
#
# Helpers we vendor:
#   external/sokol/        sokol_gfx.h, sokol_app.h, sokol_glue.h (zlib)
#   external/nanovg/       nanovg.h + nanovg.c + GL/Metal backends   (zlib)
#   external/stb/          stb_truetype.h, stb_image.h                (MIT)
#   external/nanovg/src/   NanoVG plus its bundled fontstash/stb deps  (zlib/MIT)
#   external/lexbor/       html, css, dom, selectors, core modules    (Apache-2)
#   external/yoga/         flexbox layout engine                      (MIT)

set(_AFFINEUI_EXT "${CMAKE_CURRENT_LIST_DIR}/../external")

# NanoVG (incl. our sokol_gfx backend nanovg_sokol.h) is vendored under
# external/nanovg, kept in sync from the affineui_nanovg fork (the source
# of truth). affineui builds self-contained from this vendored copy.

function(affineui_check_vendored_deps out)
    set(_ok TRUE)
    foreach(_h
        "${_AFFINEUI_EXT}/sokol/sokol_gfx.h"
        "${_AFFINEUI_EXT}/sokol/sokol_app.h"
        "${_AFFINEUI_EXT}/sokol/sokol_glue.h"
        "${_AFFINEUI_EXT}/nanovg/src/nanovg.h"
        "${_AFFINEUI_EXT}/nanovg/src/nanovg_sokol.h"
        "${_AFFINEUI_EXT}/stb/stb_image.h"
        "${_AFFINEUI_EXT}/stb/stb_truetype.h"
        "${_AFFINEUI_EXT}/lexbor/source/lexbor/html/html.h"
        "${_AFFINEUI_EXT}/lexbor/source/lexbor/css/css.h"
        "${_AFFINEUI_EXT}/lexbor/source/lexbor/dom/dom.h"
        "${_AFFINEUI_EXT}/lexbor/source/lexbor/selectors/selectors.h"
        "${_AFFINEUI_EXT}/yoga/yoga/Yoga.h"
    )
        if(NOT EXISTS "${_h}")
            set(_ok FALSE)
        endif()
    endforeach()
    set(${out} ${_ok} PARENT_SCOPE)
endfunction()

function(affineui_link_vendored_deps target)
    # ── lexbor: build as static lib from its own CMake project ────────
    # Guarded so a host using lexbor doesn't get a duplicate target.
    if(EXISTS "${_AFFINEUI_EXT}/lexbor/CMakeLists.txt"
       AND NOT AFFINEUI_HOST_PROVIDES_LEXBOR
       AND NOT TARGET lexbor_static)
        set(LEXBOR_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
        set(LEXBOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(LEXBOR_BUILD_UTILS    OFF CACHE BOOL "" FORCE)
        set(LEXBOR_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
        set(LEXBOR_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
        add_subdirectory("${_AFFINEUI_EXT}/lexbor"
                         "${CMAKE_BINARY_DIR}/_deps/lexbor" EXCLUDE_FROM_ALL)
    endif()
    if(TARGET lexbor_static)
        if(NOT AFFINEUI_HTML_ENTITIES_FULL)
            target_compile_definitions(lexbor_static PRIVATE
                LXB_HTML_TOKENIZER_BASIC_ENTITIES)
        endif()
        # BUILD_INTERFACE only — the static dep is embedded into our
        # archive at build time and isn't a runtime requirement for
        # downstream consumers, so we keep it out of the install EXPORT.
        target_link_libraries(${target} PRIVATE
            $<BUILD_INTERFACE:lexbor_static>)
    endif()
    target_include_directories(${target} SYSTEM PRIVATE
        "${_AFFINEUI_EXT}/lexbor/source")

    # ── Yoga: same shape — guarded, static ────────────────────────────
    if(EXISTS "${_AFFINEUI_EXT}/yoga/yoga/CMakeLists.txt"
       AND NOT AFFINEUI_HOST_PROVIDES_YOGA
       AND NOT TARGET yogacore)
        set(YOGA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${_AFFINEUI_EXT}/yoga/yoga"
                         "${CMAKE_BINARY_DIR}/_deps/yoga" EXCLUDE_FROM_ALL)
    endif()
    if(TARGET yogacore)
        target_link_libraries(${target} PRIVATE
            $<BUILD_INTERFACE:yogacore>)
    endif()
    target_include_directories(${target} SYSTEM PRIVATE
        "${_AFFINEUI_EXT}/yoga")

    # ── Header-only deps: sokol / NanoVG / stb / fontstash ────────────
    # These have no library targets — they live as headers + impl TUs
    # under src/. The host-provides flags propagate to the implementing
    # TUs (see Compile flags below) so we don't define IMPL macros that
    # would duplicate the host's symbols.
    target_include_directories(${target} SYSTEM PRIVATE
        "${_AFFINEUI_EXT}/sokol"
        "${_AFFINEUI_EXT}/nanovg/src"
        "${_AFFINEUI_EXT}/stb"
    )

    # NanoVG's core C source. Bundles fontstash + stb_truetype +
    # stb_image inside its TU — we don't need separate impl TUs for
    # those.
    if(NOT AFFINEUI_HOST_PROVIDES_NANOVG
       AND EXISTS "${_AFFINEUI_EXT}/nanovg/src/nanovg.c")
        target_sources(${target} PRIVATE
            "${_AFFINEUI_EXT}/nanovg/src/nanovg.c")
        set_source_files_properties("${_AFFINEUI_EXT}/nanovg/src/nanovg.c"
            PROPERTIES COMPILE_FLAGS
            "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
    endif()

    # Propagate host-provides into the compilation. When a flag is ON,
    # the matching impl TU compiles to an empty object file (its body
    # is guarded by `#ifndef AFFINEUI_HOST_PROVIDES_<DEP>`).
    target_compile_definitions(${target} PRIVATE
        $<$<BOOL:${AFFINEUI_HOST_PROVIDES_SOKOL}>:AFFINEUI_HOST_PROVIDES_SOKOL>
        $<$<BOOL:${AFFINEUI_HOST_PROVIDES_NANOVG}>:AFFINEUI_HOST_PROVIDES_NANOVG>
        $<$<BOOL:${AFFINEUI_HOST_PROVIDES_STB_IMAGE}>:AFFINEUI_HOST_PROVIDES_STB_IMAGE>
        $<$<BOOL:${AFFINEUI_HOST_PROVIDES_STB_TRUETYPE}>:AFFINEUI_HOST_PROVIDES_STB_TRUETYPE>
        $<$<BOOL:${AFFINEUI_HOST_PROVIDES_FONTSTASH}>:AFFINEUI_HOST_PROVIDES_FONTSTASH>
    )

    # Silence vendored-code warnings without polluting project flags.
    foreach(_tu
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/paint/nanovg_painter.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/paint/nanovg_sokol.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/paint/sokol_impl.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/text/fontstash_impl.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/text/stb_impl.c"
    )
        if(EXISTS "${_tu}")
            set_source_files_properties("${_tu}" PROPERTIES COMPILE_FLAGS
                "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
        endif()
    endforeach()

    # On Apple platforms, sokol_app's implementation TU includes
    # AppKit / Foundation, which means it must be compiled as
    # Objective-C, not plain C. The same applies to anything else
    # that ends up pulling in sokol_gfx Metal backend in later phases.
    if(APPLE)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/paint/sokol_impl.c"
            PROPERTIES LANGUAGE OBJC)
    endif()
endfunction()
