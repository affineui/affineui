# Tools are independent helper executables (preview, benchmarks).
# Each is optional; subdirectories are only added when their source
# is present, so the directory can grow incrementally.

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui-preview/main.cpp")
    add_subdirectory(ui-preview)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmark/main.cpp")
    add_subdirectory(benchmark)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/conformance/main.cpp")
    add_subdirectory(conformance)
endif()

if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/symprof/main.cpp")
    add_subdirectory(symprof)
endif()

# The devtools viewer (F12 target of affineui::tools_open_devtools).
if(TARGET affineui::tools AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/affinetools/main.cpp")
    add_subdirectory(affinetools)
endif()

# ─── Amalgamation ────────────────────────────────────────────────────────────
# `tools/amalgamate.py` concatenates the modular sources into a single
# `affineui.h` + `affineui.cpp` pair under ${CMAKE_BINARY_DIR}/dist/. That
# two-file form is the default integration story for end users (see
# docs/DESIGN.md, "Packaging plan"). This target *produces* those files;
# it does not try to compile them — compiling the amalgamation is the
# downstream consumer's job, and doing it here would re-introduce the
# modular build's platform wiring (Apple ObjC for sokol_app, the GL
# header pick per platform, etc.) that the two-file release deliberately
# decouples from.

find_package(Python3 COMPONENTS Interpreter)

# The generator stages Lexbor as C++ and repairs it from compiler diagnostics,
# which means clang specifically: it parses clang's diagnostic wording and
# passes clang-only flags. That has nothing to do with the compiler configured
# for *this* build — passing ${CMAKE_CXX_COMPILER} through hands the generator
# cl.exe on an MSVC configure and g++ on a GCC one, and it dies partway. Find a
# real clang instead, and say so plainly when there isn't one. (The *generated*
# affineui.cpp compiles under MSVC, GCC and clang alike; only the generator is
# clang-only, and it only ever runs on a release machine.)
find_program(AFFINEUI_AMALGAMATE_CLANG NAMES clang++ clang)

if(Python3_Interpreter_FOUND AND NOT AFFINEUI_AMALGAMATE_CLANG)
    message(STATUS
        "clang not found; `amalgamate` target disabled "
        "(the two-file SDK generator requires clang — install LLVM to enable it)")
endif()

if(Python3_Interpreter_FOUND AND AFFINEUI_AMALGAMATE_CLANG)
    set(_amalg_script "${CMAKE_CURRENT_SOURCE_DIR}/amalgamate.py")
    set(_amalg_outdir "${CMAKE_BINARY_DIR}/dist")
    set(_amalg_h      "${_amalg_outdir}/affineui.h")
    set(_amalg_cpp    "${_amalg_outdir}/affineui.cpp")
    set(_lexbor_cpp_script "${CMAKE_CURRENT_SOURCE_DIR}/lexbor_cpp_compat.py")
    set(_lexbor_cpp_outdir "${CMAKE_BINARY_DIR}/lexbor-cpp")

    # Glob the inputs that feed the amalgamator so re-running it is
    # gated on actual source changes. CONFIGURE_DEPENDS makes Ninja
    # re-glob on each build.
    file(GLOB_RECURSE _amalg_inputs CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/include/affineui/*.h"
        "${CMAKE_SOURCE_DIR}/src/*.h"
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.c"
        "${CMAKE_SOURCE_DIR}/external/lexbor/source/lexbor/*.h"
        "${CMAKE_SOURCE_DIR}/external/lexbor/source/lexbor/*.c"
        "${CMAKE_SOURCE_DIR}/external/yoga/yoga/*.h"
        "${CMAKE_SOURCE_DIR}/external/yoga/yoga/*.cpp"
        "${CMAKE_SOURCE_DIR}/external/nanovg/src/*.h"
        "${CMAKE_SOURCE_DIR}/external/nanovg/src/*.c"
        "${CMAKE_SOURCE_DIR}/external/sokol/*.h"
    )

    add_custom_command(
        OUTPUT  "${_amalg_h}" "${_amalg_cpp}"
        COMMAND "${Python3_EXECUTABLE}" "${_amalg_script}"
                --root "${CMAKE_SOURCE_DIR}"
                --out  "${_amalg_outdir}"
                --cxx  "${AFFINEUI_AMALGAMATE_CLANG}"
        DEPENDS "${_amalg_script}" "${_lexbor_cpp_script}" ${_amalg_inputs}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Producing affineui.h + affineui.cpp two-file release"
        VERBATIM
    )

    add_custom_target(amalgamate DEPENDS "${_amalg_h}" "${_amalg_cpp}")

    add_custom_target(lexbor_cpp_check
        COMMAND "${Python3_EXECUTABLE}" "${_lexbor_cpp_script}"
                --root "${CMAKE_SOURCE_DIR}"
                --out  "${_lexbor_cpp_outdir}"
                --check
                --cxx  "${AFFINEUI_AMALGAMATE_CLANG}"
        DEPENDS "${_lexbor_cpp_script}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Generating Lexbor C++ compatibility tree and compiling it"
        VERBATIM
    )
elseif(NOT Python3_Interpreter_FOUND)
    message(STATUS "Python3 interpreter not found; `amalgamate` target disabled")
endif()
