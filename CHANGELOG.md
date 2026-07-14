# Changelog

All notable changes to AffineUI are recorded here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
once it reaches 1.0.

## [Unreleased]

### Added

- **Native application menus, and a platform-neutral menu model** (#61).
  `App::set_menu()` takes a tree of `MenuItem`s — label, Electron-style
  accelerator (`"CmdOrCtrl+S"`, resolving to Command on macOS and Control
  elsewhere), `role`, checkbox state, icon, and a color `swatch`. On macOS it
  becomes a real `NSMenu` installed as the system menu bar. `role` items
  (Quit, About, Services, Hide, the Cut/Copy/Paste/Undo group, Minimize, Zoom,
  Close) take their label, accelerator and behavior from the platform, so an
  app does not restate them per OS. Off by config with
  `Config::native_menus = false`.

  The drawn in-window menubar (`View::menu_bar`) keeps working, and keeps
  drawing everything except its menu *triggers* once the app has supplied a
  native menu — the bar still carries the brand and the status bits, because it
  doubles as the window's title bar. A menubar nested elsewhere in the UI (a
  viewport's own View/Add strip) is contextual, not the application menu, and is
  never touched.

- **Close-request handling** (#60). `App::on_close_request(cb)` — return false
  to cancel. Every way of closing runs it: the window button, Cmd-Q, the menu's
  Quit, a Close control the app drew itself, and `App::close()`. This is what
  makes save-on-exit possible; `App::quit()` remains the uncancellable form.

- **Windows that own their chrome** (#62, macOS). `Config::titlebar` —
  `Hidden` / `HiddenInset` / `Frameless`, named as in Electron's
  `titleBarStyle` — removes the system title bar so the app draws its own. The
  window stays resizable, snappable and full-screen-able.
  - `--affineui-app-region: drag` marks an element as a title bar (drag moves
    the window, double-click zooms); `no-drag` opts a child back out. The
    inheriting-custom-property semantics match Electron's `-webkit-app-region`.
  - The engine measures the OS's window buttons and publishes the reserved band
    to CSS as `--affineui-titlebar-inset-left` / `-right` and the
    `--affineui-titlebar-area-*` set (the web's `env(titlebar-area-*)`, except
    published on macOS too, where Electron leaves apps to hardcode ~80px). It
    also stamps `data-affineui-platform` and `data-affineui-titlebar` on `:root`
    so stylesheets can select per-OS.
  - `App::close/minimize/toggle_maximize/is_maximized/set_fullscreen/is_fullscreen`
    — what an app-drawn window button calls.
  - Windows and Linux take the same config and CSS; the chrome work there is
    still to come, and the vars read 0px, so a bar written against them is
    already correct.

- `View::document_title()` — the edited document's name, centered on the window.

### Removed

- The `AFFINEUI PATCH (menu)` in vendored `sokol_app.h` — the temporary,
  app-invisible NSMenu that existed only so Cmd-Q would work. macOS menus now
  live in `src/platform/macos/`, outside the vendored tree, and no sokol patch
  is needed at all: sokol already exposes `sapp_macos_get_window()`, and its
  init callback runs inside `applicationDidFinishLaunching`, by which point
  `NSApp` and the window both exist. An app that never calls `set_menu()` still
  gets a standard application menu synthesized for it, so Cmd-Q keeps working.

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
