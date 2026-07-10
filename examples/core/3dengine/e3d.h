// e3d.h — umbrella header for the example 3D engine.
//
// A small three.js-style 3D engine on sokol_gfx for AffineUI sample
// apps that need a real GPU viewport (the Decius game editor and
// DENDER). It is EXAMPLE / TEMPLATE code, statically linked by the
// samples — the AffineUI framework itself never depends on it.
//
// The API deliberately mirrors three.js r170 (the version the web
// reference apps run) so viewport code translates one-to-one:
//
//   auto scene  = std::make_shared<e3d::Scene>();
//   auto camera = std::make_shared<e3d::PerspectiveCamera>(38.f, 1.f,
//                                                          0.1f, 200.f);
//   auto cube   = std::make_shared<e3d::Mesh>(
//       e3d::make_box(1.6f, 1.6f, 1.6f),
//       e3d::Material::standard(0x9aa1ad, 0.55f, 0.05f));
//   scene->add(cube);
//
//   e3d::Renderer renderer;                    // offscreen, MSAA 4x
//   renderer.set_size(width_px, height_px);
//   renderer.render(*scene, *camera);          // outside any sg pass!
//
//   // In the custom-paint handler:
//   painter.draw_image(renderer.painter_image(painter), rect, rect);
//
// Picking goes through e3d::Raycaster, camera interaction through
// e3d::OrbitControls, and object manipulation through
// e3d::TransformControls (whose helper() object renders the gizmo).
#pragma once

#include "e3d_controls.h"
#include "e3d_geometry.h"
#include "e3d_material.h"
#include "e3d_math.h"
#include "e3d_raycast.h"
#include "e3d_renderer.h"
#include "e3d_scene.h"
