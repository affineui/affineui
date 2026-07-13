# Changelog

All notable changes to AffineUI are recorded here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
once it reaches 1.0.

## [Unreleased]

### Changed

- `WidgetRef`, `View::Scope`, and `DockHandle` now use an invalidating View
  identity. They do not keep a View alive; reads return defaults and writes
  no-op after the View or referenced node is gone. Moving a View invalidates
  handles issued by its previous identity.
- `Ui` is non-copyable and non-movable. Retained helpers use `AppHandle`
  instead of keeping `App*`, `Document*`, `Renderer*`, or `Painter*` values.
- `WeakRef` now carries the registry that issued its versioned slot, so handles
  remain correct when AffineUI is statically linked into multiple modules.
  Bound handles can only be created from a live trackable object.
- Dynamic RGBA images use reference-counted `ImageHandle` values. The last
  handle releases the GPU image, renderer shutdown invalidates all handles,
  and invalid update/draw operations safely do nothing.
- Attached `WidgetRef` mutations and handler replacement now update the live
  document immediately through a scoped mutation transaction.
- Exact interaction/caret relayout borrows a Painter only for the duration of
  the dispatch/query; Documents no longer retain a Painter from an old frame.
- TinyJSON was removed. The tools server and amalgamation use AffineUI's
  existing `json_reader.h` implementation.

### Added

- `App::handle()` and `AppHandle`, an invalidating non-owning capability for
  retained controllers.
- `App::create_image_rgba`, `Renderer::create_image_rgba`, and `ImageHandle`.
- `guard(owner, callable)` for retained callbacks that need captures or a
  non-void result. The callable receives the live owner as its first argument.
- Regression coverage for View/queue/controller teardown, re-entrant App/Ui
  destruction, post-build mutation, handler replacement, and View moves.
- Project scaffolding: CMake build, documentation, dependency fetch
  script, CI skeleton, public API sketch.
- Public C++17 API surface: `affineui::App`, `affineui::Document`,
  `affineui::imm::*`.
- Architecture and design documentation covering retained + immediate
  mode dual front-ends.
