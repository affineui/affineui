affineui_add_sokol_example(decius_game_editor
    main.cpp
    game_editor.cpp
    ge_viewport.cpp
    interactive_tests.cpp
    game_editor_styles.cpp
)
# Built on the affineui_app DCC template (commands, selection, document)
# plus the e3d example engine for the real 3D viewport.
target_link_libraries(decius_game_editor PRIVATE affineui::app
                                                 affineui::engine3d)
