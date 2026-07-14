# Window chrome and native menus

How an AffineUI app gets a real macOS menu bar, gives up the system title bar
and draws its own, and gets a veto over closing. Issues #60, #61, #62.

The layering rule is the one the IME intents already follow (see
`IME_ARCHITECTURE.md` and `EMBEDDING_DESIGN.md`): **the core states what it
wants in platform-neutral terms; the shell speaks to the OS.** Nothing in
`src/framework/` or `src/renderer/` knows what an `NSMenu` is.

```text
  App / View  ──── Menu model, TitleBarStyle, close intent ────┐
  (platform-neutral)                                           │
                                                        src/platform/
                                                        ├── platform.h        the seam
                                                        ├── macos/            NSMenu, NSWindow
                                                        └── platform_stub.cpp everywhere else
```

There is **no sokol patch**. There used to be: a minimal NSMenu built inside
`sokol_app.h`'s `applicationDidFinishLaunching`, marked TEMPORARY, whose only
job was to make Cmd-Q work. It is gone. sokol already exposes
`sapp_macos_get_window()`, and its `init_cb` runs *from inside*
`applicationDidFinishLaunching` — so by the time the app layer's `cb_init` runs,
`NSApp` and the `NSWindow` both exist and everything can be driven from our own
code.

## Menus (#61)

`App::set_menu()` takes a tree of `MenuItem`. The shape mirrors Electron's
`Menu.buildFromTemplate`, because that vocabulary is already in people's heads
and because the mapping to AppKit is the one Electron itself uses.

```cpp
app.set_menu({
    MenuItem::sub("", {                        // titled with the app's name
        MenuItem::role(MenuRole::About),
        MenuItem::separator(),
        MenuItem::role(MenuRole::Quit),
    }),
    MenuItem::sub("File", {
        MenuItem::item("New Scene", "CmdOrCtrl+N", [&] { new_scene(); }),
    }),
    MenuItem::sub("Edit", MenuItem::edit_menu()),
});
```

**Roles** are the load-bearing idea. A role item's label, accelerator and
behavior come from the platform, so the app never restates them per OS. They
split three ways:

| Role group | Serviced by |
| --- | --- |
| About, Services, Hide, Hide Others, Show All, Minimize, Zoom, Full Screen | AppKit's own selectors — the shell wires them and we do nothing |
| Cut, Copy, Paste, Select All, Undo, Redo | **the core.** These must act on the focused *DOM* text control, which is not in AppKit's responder chain. The native item owns the keystroke (its key equivalent fires before the view ever sees it), so the shell hands the role back and the core replays it as the chord the document already implements |
| Quit, Close | **the core**, because they must run the close-request intent (below) |
| Preferences | **the core.** AppKit has no standard action behind Settings…, so it comes back like Quit and Close — give the item an `on_select`, or handle the role |

**Accelerators** are Electron-style strings. `CmdOrCtrl` resolves to Command on
macOS and Control elsewhere, which is the whole point: an app declares a
shortcut once. `parse_accelerator()` is in the core, not the shell, so the drawn
menus and the tests can use it without AppKit.

**Custom-drawn rows.** A drawn menu can paint anything into a row; a native menu
cannot. Rather than reach for `NSMenuItem.view` and a second render path, the
model carries the *data* those rows were painting: `checked` (a check mark),
`icon`, and `swatch` (a solid color chip). Every custom row in the demos —
checked density items, submenu stubs, accent swatches — maps onto a real
`NSMenuItem` affordance. A row the model genuinely cannot express degrades to a
plain labelled row rather than disappearing.

**The drawn menubar.** `View::menu_bar` still works and still draws. Once the app
has supplied a native menu, the *application* menubar hides its menu **triggers**
— File/Edit/View now live at the top of the screen, and drawing them in the
window too would show the same menus twice. Everything else in the bar (brand,
spacer, status, `document_title`) keeps drawing: that bar is the window's title
bar now.

Three rules make this safe:

- The triggers hide only when `set_menu()` was actually called with a non-empty
  menu. Hiding them merely because the platform *could* show menus would delete
  the menus of every app that has not adopted `set_menu()` yet — its triggers
  would go and nothing would take their place in the system bar.

- Only the **first** menubar a build declares is the application menubar; it
  carries the `aui-menubar--app` class. A menubar nested elsewhere (a viewport's
  own View/Add strip) is a contextual in-window menu and always keeps drawing.
  It is marked on the node, not inferred from depth — a contextual bar can sit
  at the same depth as the app's.

- **Hiding is a restyle, not a build-time decision.** The bar always emits its
  triggers; App sets `data-affineui-native-menus` on the root, and the
  stylesheet does the rest:

  ```css
  :root[data-affineui-native-menus="1"] .aui-menubar--app > .dcs-menubar__item {
    display: none;
  }
  ```

  This is what makes `set_menu()` order-independent. Deciding it while *building*
  the DOM would freeze the answer at whatever it was during that build — and a
  `load_view()` app builds its DOM exactly once, so an app that declared its menu
  afterward would draw its menus twice, forever. As a restyle it simply cannot
  go wrong, and there is no ordering rule for anyone to get wrong.

## Close requests (#60)

```cpp
app.on_close_request([&] {
    if (!doc.dirty()) return true;
    prompt_save();   // async: cancel now, call app.quit() when resolved
    return false;    // cancels the close
});
```

One veto point, and every route runs it: the window's close button, Cmd-Q (which
is just the Quit item's key equivalent), the menu's Quit, `App::close()`, and a
Close button the app drew itself. `App::quit()` is the uncancellable form — the
app's own decision, so it does not ask itself.

Mechanically: sokol posts `SAPP_EVENTTYPE_QUIT_REQUESTED`, and the handler either
lets it proceed or calls `sapp_cancel_quit()`. It must never *re-enter*
`sapp_request_quit()` from inside that callback to say yes; not touching it is
what yes means.

## Window chrome (#62)

`Config::titlebar` is named as in Electron: `Hidden` and `HiddenInset` drop the
system title bar but keep the OS window buttons (positionable via
`traffic_light_position`); `Frameless` drops the buttons too, and the app draws
its own and drives them with `App::close/minimize/toggle_maximize/...`.

On macOS this is `NSWindowStyleMaskFullSizeContentView` + a transparent,
title-less title bar — *not* a borderless `NSWindow`, which would lose edge
resize, snapping and full-screen and force us to reimplement all three.

### Dragging: `--affineui-app-region`

A window with no title bar cannot be moved unless the app says which part of its
UI behaves like one.

```css
.dcs-menubar        { --affineui-app-region: drag; }     /* the bar moves the window */
.dcs-menubar__item  { --affineui-app-region: no-drag; }  /* …but not the buttons in it */
```

Same semantics as Electron's `-webkit-app-region`, and it is a real CSS custom
property, so it inherits — which is precisely what makes it work: the element
under the cursor already carries the resolved answer, so a `no-drag` button
inside a `drag` bar wins by being nearer. A press in a drag region is handed to
the platform (`performWindowDragWithEvent:` — native snapping and Spaces) and
never becomes a click in the DOM. A double-click zooms.

### The reserved band: what the engine publishes

The OS keeps drawing its window buttons *over* our content — that is what a
hidden title bar means — so a drawn bar has to keep its content out from under
them. The engine **measures** them and publishes the result to CSS. It publishes
facts, not pixels: no color, no padding, no opinion about how a bar should look.

| Published on `:root` | Meaning |
| --- | --- |
| `--affineui-titlebar-inset-left` / `-right` | the band the OS's buttons occupy. macOS puts them left; Windows will put them right. 0px when the window has a normal title bar |
| `--affineui-titlebar-area-x/y/width/height` | the same information in the shape of the web's Window Controls Overlay `env(titlebar-area-*)` |
| `data-affineui-platform` | `macos` / `windows` / `linux` — custom properties cannot drive selectors, so per-OS rules need an attribute |
| `data-affineui-titlebar` | `default` / `hidden` / `hidden-inset` / `frameless` |

Electron is inconsistent here: it implements Window Controls Overlay on
Windows/Linux, and on macOS gives you nothing, so every Electron app hardcodes a
~80px left padding. We own the engine, so we publish the vars on macOS too.

Anything *visual* built on those vars is a style-layer decision, not the
engine's. Decius binds them for its own `.dcs-menubar` / `menu_brand` (that black
app-title area is Decius branding); an app whose title strip is its own thing —
DENDER's logo bar — binds the same vars in its own stylesheet. The engine never
knows a logo exists.

## Platforms

macOS is implemented. Windows and Linux take the same `Config`, the same menu
model and the same CSS: `platform_stub.cpp` no-ops, the vars read 0px, and the
drawn menubar stays as-is — so a bar written against the vars is already correct
when the chrome work lands there (`WM_NCCALCSIZE` / `WM_NCHITTEST` for Windows,
motif hints for X11).
