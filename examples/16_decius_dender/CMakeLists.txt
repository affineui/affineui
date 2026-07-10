affineui_add_sokol_example(decius_dender
    main.cpp
    dender_app.cpp
    dender_components.cpp
    dender_scene.cpp
    dender_stats.cpp
    dender_styles.cpp
    dender_view.cpp
)
# Built on the affineui_app DCC template (commands, undo stack, settings) plus
# the shared reusable 3D viewport (which pulls in the e3d engine).
target_link_libraries(decius_dender PRIVATE affineui::app
                                            affineui::viewport3d)
