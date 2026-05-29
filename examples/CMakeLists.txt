set(AFFINEUI_EXAMPLES_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(AFFINEUI_EXAMPLES_BINARY_ROOT "${CMAKE_BINARY_DIR}/examples")

add_custom_target(affineui_example_browser_assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${AFFINEUI_EXAMPLES_ROOT}/frameworks"
            "${AFFINEUI_EXAMPLES_BINARY_ROOT}/frameworks"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${AFFINEUI_EXAMPLES_BINARY_ROOT}/common"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${AFFINEUI_EXAMPLES_ROOT}/common/decius_demo_interactions.js"
            "${AFFINEUI_EXAMPLES_BINARY_ROOT}/common/decius_demo_interactions.js"
    VERBATIM)

function(affineui_copy_example_browser_files name)
    add_dependencies(${name} affineui_example_browser_assets)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${AFFINEUI_EXAMPLES_ROOT}/frameworks"
                "$<TARGET_FILE_DIR:${name}>/frameworks"
        VERBATIM)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/index.html")
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
                    "$<TARGET_FILE_DIR:${name}>/index.html"
            VERBATIM)
    endif()
endfunction()

# Example apps. Each subdirectory builds one executable.

function(affineui_add_example name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE affineui::affineui)
    target_include_directories(${name} PRIVATE "${AFFINEUI_EXAMPLES_ROOT}/common")
    target_compile_definitions(${name} PRIVATE
        AFFINEUI_EXAMPLES_SOURCE_DIR="${AFFINEUI_EXAMPLES_ROOT}")
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/${name}")
    if(WIN32)
        set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE TRUE)
        if(MSVC)
            target_link_options(${name} PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()
    affineui_copy_example_browser_files(${name})
endfunction()

# Sokol-flavored example - same as above but also pulls in the sokol
# adapter (gets AFFINEUI_WITH_SOKOL + sokol_app.h include path).
function(affineui_add_sokol_example name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE affineui::sokol)
    target_include_directories(${name} PRIVATE "${AFFINEUI_EXAMPLES_ROOT}/common")
    target_compile_definitions(${name} PRIVATE
        AFFINEUI_EXAMPLES_SOURCE_DIR="${AFFINEUI_EXAMPLES_ROOT}")
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/${name}")
    if(WIN32)
        set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE TRUE)
        if(MSVC)
            target_link_options(${name} PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()
    affineui_copy_example_browser_files(${name})
endfunction()

# SDL2 example. Skipped silently when SDL2 isn't available so the
# rest of the build still works.
function(affineui_add_sdl_example name)
    if(NOT TARGET affineui::sdl)
        return()
    endif()
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE affineui::sdl)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/${name}")
    if(WIN32)
        set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE TRUE)
        if(MSVC)
            target_link_options(${name} PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()
    affineui_copy_example_browser_files(${name})
endfunction()

add_subdirectory(00_hello)
add_subdirectory(01_bootstrap)
add_subdirectory(02_hello_sdl)
add_subdirectory(03_media)
add_subdirectory(04_imm_counter)
add_subdirectory(05_imm_todo)
add_subdirectory(06_text_flow)
add_subdirectory(07_forms_focus)
add_subdirectory(08_bootstrap_kitchen)
add_subdirectory(09_embed_d3d11)
add_subdirectory(10_bootstrap_dashboard)
add_subdirectory(11_decius_game_editor)
add_subdirectory(12_decius_video_editor)
add_subdirectory(13_decius_synth_dark)
add_subdirectory(14_decius_synth_skeuo)
add_subdirectory(15_command_panel)
add_subdirectory(16_decius_dender)
