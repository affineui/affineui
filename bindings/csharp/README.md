# AffineUI — C# / .NET binding

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_dender.png" width="720" alt="AffineUI running the Dender 3D-print slicer — the default Decius CSS look">

<img src="https://raw.githubusercontent.com/affineui/affineui/main/images/affineui_bootstrap.png" width="720" alt="AffineUI rendering a Bootstrap dashboard">

An idiomatic .NET wrapper over the AffineUI C ABI (`affineui_c`), shaped like
the Python binding: a gradio/imgui-style `View` builder, typed components,
safe widget handles, and hard-to-crash callbacks. Both operating modes ship in
one assembly (see `docs/LANGUAGE_BINDINGS.md` §5 for the full design):

- **Mode A — C# owns the system** (`AffineUI.App`): AffineUI creates the
  native window and runs the loop; `App.Run()` blocks the calling thread.
- **Mode B — embedded** (`AffineUI.Embedded.Ui`): the host engine owns the
  GPU device, window, loop, and input pump; AffineUI renders into
  render-target views the host supplies each frame and never presents.

**Status:** alpha. Broad standards coverage and real UIs run today, but
expect bugs and expect APIs to move.

> **Note:** this release carries no `-alpha` suffix in its version number,
> but AffineUI is alpha and pre-1.0. Features, APIs, and the set of supported
> platforms are all still in flux.
>
> Verified on: **Linux (Ubuntu, x86-64)**, **macOS (Apple Silicon)**, and
> **Windows (x86-64)** — the full suite (600+ tests) passes on each. Other
> platforms and architectures are not yet supported.

## Layout

```
bindings/csharp/
├── AffineUI/                 # the library (net8.0, LibraryImport P/Invoke)
│   ├── NativeMethods.cs      # raw FFI, 1:1 with c_api.h + c_api_app.h
│   ├── AffineUIRuntime.cs    # loader, ABI gate, callback trampolines
│   ├── App.cs View.cs Widget.cs Document.cs Types.cs Components.cs
│   └── Embedded/             # Ui.cs, GpuContext.cs, FrameTarget.cs
├── examples/Hello/           # Mode A console app (+ --headless verify path)
└── README.md
```

## Building

1. **Native library.** Configure the CMake build with
   `-DAFFINEUI_BUILD_C_SHARED=ON` and build the `affineui_c` target; in this
   checkout the result lands in `build/ninja/affineui_c.dll`.

2. **Managed library and example** (requires a .NET 8+ SDK):

   ```
   dotnet build bindings/csharp/AffineUI
   dotnet build bindings/csharp/examples/Hello
   ```

## Locating the native library

The wrapper resolves `affineui_c` in this order:

1. the `AFFINEUI_NATIVE_DIR` environment variable (a directory containing
   `affineui_c.dll` / `libaffineui_c.so` / `libaffineui_c.dylib`),
2. the application base directory,
3. default .NET probing (`runtimes/<rid>/native` for future NuGet packages).

For the dev loop, the Hello example copies the DLL beside the exe
automatically: its `AffineUINativeDir` MSBuild property defaults to
`../../../../build/ninja` (this checkout's CMake output) and can be overridden
with `dotnet build -p:AffineUINativeDir=<dir>`.

At first use the wrapper checks `affineui_c_abi_version()` against the version
it was written for (`AffineUIRuntime.ExpectedAbiVersion` = 5) and throws a
clear exception on mismatch.

## Running the example

```
dotnet run --project bindings/csharp/examples/Hello -- --headless
```

prints the built view's HTML fragment, lays it out in a headless `Document`,
and verifies the callback-lifetime contract (each handler's closure is
released exactly once when its view is destroyed). Without `--headless` it
opens a native window and runs the app loop.

## Usage sketch

```csharp
using AffineUI;

using var view = new View(Theme.Decius);
view.Build(v =>
{
    v.Heading(1, "Photo Edit");
    v.Toolbar("main", build: t =>
    {
        t.IconButton("save", key: "save").OnClick(() => Console.WriteLine("saved"));
    });
    v.Slider("Exposure", 0.5, 0, 1, key: "exposure")
     .OnChange(val => Console.WriteLine($"exposure = {val}"));
});

using var app = new App(new AppConfig { Title = "Demo", Width = 1280, Height = 800 });
app.LoadView(view);
app.Run();
```

Typed components (the `components.h` semantics — `Valid` / `WrongType` /
`NotPresent`, inert when invalid, never crash):

```csharp
Checkbox c = view.CheckboxAt("enabled");
if (c.IsValid && c.Checked) { ... }
```

Embedded mode (host-owned GPU; handles are opaque `IntPtr`s):

```csharp
using AffineUI.Embedded;

var ui = new Ui();
ui.Init(new InitDesc { Gpu = GpuContext.D3D11(devicePtr, contextPtr, color: PixelFormat.Bgra8) });
ui.SetHtml(html);
// per frame, inside the host's render loop:
if (ui.NeedsUpdate) { /* an on-demand host may skip idle frames */ }
ui.Render(FrameTarget.D3D11(rtvPtr, dsvPtr, width, height, dpiScale));
bool consumed = ui.Dispatch(in evt);   // host suppresses its own input if true
```

## Contracts encoded by the wrapper

- **Threading:** the whole surface is single-threaded — create, mutate,
  render, and dispatch on one thread (debug builds of the wrapper assert
  this).
- **Callbacks:** managed closures are boxed in a `GCHandle`; the core calls
  `user_free` exactly once when it drops the handler, which frees the handle —
  leak-free, no delegate bookkeeping. Exceptions thrown by callbacks never
  cross into native code; they are routed to
  `AffineUIRuntime.OnCallbackException` (default: stderr).
- **Lifetimes:** owned handles are `SafeHandle`s. A `Widget` is an invalidating
  handle and does not keep its `View` alive. A callback-provided `View` retains
  only an opaque native weak token; `IsAlive` becomes false with its owner and
  stale operations throw `ObjectDisposedException`. `App.Document` is borrowed
  — never destroyed, keeps the `App` alive, and degrades to a no-op after the
  app is disposed.
- **Hard to crash:** operations on a widget whose node is gone read defaults
  and no-op on writes, exactly like the C++ `WidgetRef`.

---

## Links

- **Repository:** <https://github.com/affineui/affineui>
- **Docs:** <https://github.com/affineui/affineui/tree/main/docs>
- **Issues:** <https://github.com/affineui/affineui/issues>
- **Discussion:** [r/affineui](https://www.reddit.com/r/affineui/) — questions,
  show-and-tell, and general chat
- **License:** MIT
