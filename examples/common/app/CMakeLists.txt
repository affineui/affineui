# App template — EXAMPLE / TEMPLATE code (not part of the AffineUI library).
# A compact command-based DCC-style editing spine that sample apps build on,
# included via "affineui_app.h". Linked by examples; never linked into the
# framework itself.
add_library(affineui_app STATIC
    command.cpp
    command_stack.cpp
    command_registry.cpp
    context.cpp
    document.cpp
    selection.cpp
    gesture.cpp
    localization.cpp
    standard_commands.cpp
    toml.cpp
    settings.cpp
    styles.cpp
)
add_library(affineui::app ALIAS affineui_app)

target_include_directories(affineui_app PUBLIC
    "${AFFINEUI_EXAMPLES_ROOT}/common")
target_link_libraries(affineui_app PUBLIC affineui::affineui)
target_compile_features(affineui_app PUBLIC cxx_std_20)
