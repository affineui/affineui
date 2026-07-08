# affineui_tools — the affinetools sidecar library (devtools CLIENT half:
# discovery, protocol client, telemetry model, dump summaries; panels
# accrete here as S2+ land). The probed target needs only core; link
# `affineui::tools` from viewer/tooling processes. Single-file oriented —
# stays amalgamatable to one affineui_tools.h/.cpp pair beside affineui.h.

add_library(affineui_tools STATIC
    affineui_tools.cpp
)
add_library(affineui::tools ALIAS affineui_tools)

target_include_directories(affineui_tools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Core-internal minimal JSON reader (src/core/tools/json_reader.h) — same
# strict parser the target-side server uses; not public API. Path relative
# to this file rather than CMAKE_SOURCE_DIR — the wheel build's outermost
# CMakeLists lives at bindings/python, so an absolute CMAKE_SOURCE_DIR/src
# wrongly resolves to bindings/python/src there.
target_include_directories(affineui_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(affineui_tools PUBLIC affineui::affineui)
target_compile_features(affineui_tools PUBLIC cxx_std_23)
