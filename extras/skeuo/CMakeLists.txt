# affineui_skeuo — the skeuomorphic hardware widget kit.
#
# Patch jacks, drag-able patch cables (Reason-style, with hanging-cable
# physics), backlit plastic buttons, 7-segment LED displays, LED bargraph
# meters, hardware chassis / silkscreen group builders. Renders through
# the Decius CSS skeuomorphic classes (dcs-jack / dcs-litbtn / dcs-lcd /
# dcs-meter / dcs-hw / dcs-silk) plus the core inline-SVG renderer.
#
# Deliberately NOT part of the core library: link `affineui::skeuo` from
# apps that want hardware-emulation UI.

add_library(affineui_skeuo STATIC
    affineui_skeuo.cpp
)
add_library(affineui::skeuo ALIAS affineui_skeuo)

target_include_directories(affineui_skeuo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(affineui_skeuo PUBLIC affineui::affineui)
target_compile_features(affineui_skeuo PUBLIC cxx_std_23)
