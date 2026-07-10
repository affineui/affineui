affineui_add_sokol_example(decius_game_editor
    main.cpp
    game_editor.cpp
    interactive_tests.cpp
    game_editor_styles.cpp
)
# Built on the affineui_app DCC template (commands, selection, document)
# plus the shared reusable 3D viewport (which pulls in the e3d engine).
target_link_libraries(decius_game_editor PRIVATE affineui::app
                                                 affineui::viewport3d)
