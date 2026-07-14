// Mode A — C# owns the system: AffineUI creates the native window and runs
// the loop on the calling thread. Wraps affineui_app_*.

using System.Runtime.InteropServices;

namespace AffineUI;

/// <summary>
/// How the window's title bar is drawn. Named as in Electron's
/// <c>titleBarStyle</c> (plus <see cref="Frameless"/> for its
/// <c>frame: false</c>), so the vocabulary carries over. Mirrors
/// <c>affineui_titlebar_style</c>.
/// </summary>
public enum TitleBarStyle
{
    /// <summary>The OS draws its title bar and window buttons; the app draws
    /// below it.</summary>
    Default = 0,

    /// <summary>No system title bar — the content fills the window and the app
    /// draws its own bar. The OS window buttons (macOS traffic lights) are still
    /// shown and still work; move them with
    /// <see cref="AppConfig.TrafficLightPosition"/>.
    ///
    /// <para>A Hidden window needs the app to mark its own drag region, or the
    /// window cannot be moved: any element carrying the CSS declaration
    /// <c>--affineui-app-region: drag</c> behaves like a title bar. Interactive
    /// children inside it must opt back out with <c>no-drag</c>. This mirrors
    /// Electron's <c>-webkit-app-region</c>.</para></summary>
    Hidden = 1,

    /// <summary>As <see cref="Hidden"/>, with the macOS traffic lights inset a
    /// little further from the corner.</summary>
    HiddenInset = 2,

    /// <summary>No system title bar AND no system window buttons: the app draws
    /// close/minimize/maximize itself and drives them with <see cref="App.Close"/>
    /// / <see cref="App.Minimize"/> / <see cref="App.ToggleMaximize"/>. Don't pick
    /// this unless you are actually drawing the buttons — otherwise the window
    /// cannot be closed except with Cmd-Q.</summary>
    Frameless = 3,
}

/// <summary>
/// Application window configuration. Unset (null) properties keep the core's
/// defaults (filled by <c>affineui_app_config_init</c>).
/// </summary>
public sealed class AppConfig
{
    /// <summary>Window title. Default "AffineUI".</summary>
    public string? Title { get; set; }

    /// <summary>Window width in points. Default 1024.</summary>
    public int? Width { get; set; }

    /// <summary>Window height in points. Default 768.</summary>
    public int? Height { get; set; }

    /// <summary>Background clear color. Default (30, 30, 46, 255).</summary>
    public Color? ClearColor { get; set; }

    /// <summary>Request a high-DPI framebuffer. Default true.</summary>
    public bool? HighDpi { get; set; }

    /// <summary>Vertical sync. Default true.</summary>
    public bool? VSync { get; set; }

    /// <summary>Default UI font family. Default "sans-serif".</summary>
    public string? DefaultFontFamily { get; set; }

    /// <summary>Default UI font size. Default 16.</summary>
    public int? DefaultFontSize { get; set; }

    /// <summary>Folders searched for assets (stylesheets, images, fonts).
    /// Default { "." }.</summary>
    public IReadOnlyList<string>? AssetFolders { get; set; }

    /// <summary>Show the performance overlay. Default false.</summary>
    public bool? PerfOverlay { get; set; }

    /// <summary>Runtime opt-out for the compile-time bundled Decius
    /// resources. The bundle is ON by default (this is false); set to
    /// true to disable — no auto-applied stylesheet, no fallback-to-
    /// embedded on <c>frameworks/*</c> URLs — even when the bundle is
    /// compiled in. Ignored when affineui_c was built with
    /// <c>-DAFFINEUI_NO_BUNDLE_DECIUS</c>.</summary>
    public bool? NoBundleDecius { get; set; }

    // ── Platform chrome ──────────────────────────────────────────────────

    /// <summary>Native application menus — the macOS system menu bar. Default
    /// true: the menu given to <see cref="App.SetMenu"/> becomes the system bar
    /// and the drawn menubar hides its triggers. The default has to be ON
    /// because without a menu bar there is no Quit item, so Cmd-Q cannot work
    /// at all. Set false to opt out and keep the in-window bar the app draws
    /// itself (<see cref="View.MenuBar"/>).</summary>
    public bool? NativeMenus { get; set; }

    /// <summary>Window chrome. Default <see cref="TitleBarStyle.Default"/> (the
    /// OS draws its title bar).</summary>
    public TitleBarStyle? TitleBar { get; set; }

    /// <summary>macOS only: where the traffic lights sit, in logical points from
    /// the window's top-left. Unset (or (0,0)) means the platform default for the
    /// chosen <see cref="TitleBar"/>. Ignored when the style is
    /// <see cref="TitleBarStyle.Default"/>.</summary>
    public Point? TrafficLightPosition { get; set; }
}

/// <summary>
/// An AffineUI application that owns the native window and main loop.
/// <see cref="Run"/> blocks the calling thread until the app quits.
/// Single-threaded: create and use on one thread.
/// </summary>
public sealed partial class App : IDisposable
{
    internal sealed class AppSafeHandle : SafeHandle
    {
        public AppSafeHandle(IntPtr h) : base(IntPtr.Zero, ownsHandle: true) => SetHandle(h);
        public override bool IsInvalid => handle == IntPtr.Zero;
        protected override bool ReleaseHandle()
        {
            NativeMethods.affineui_app_destroy(handle);
            return true;
        }
    }

    private readonly AppSafeHandle _handle;
    private Document? _document;

    public App(AppConfig? config = null)
    {
        AffineUIRuntime.EnsureLoaded();
        AffineUIRuntime.CheckThread();

        NativeAppConfig native = default;
        NativeMethods.affineui_app_config_init(ref native);

        var temp = new List<IntPtr>();
        Utf8StringArray? folders = null;
        try
        {
            if (config is not null)
            {
                if (config.Title is not null) native.Title = Alloc(config.Title, temp);
                if (config.Width is int w) native.Width = w;
                if (config.Height is int h) native.Height = h;
                if (config.ClearColor is Color c)
                    native.ClearColor = new NativeColor { R = c.R, G = c.G, B = c.B, A = c.A };
                if (config.HighDpi is bool hd) native.HighDpi = hd ? 1 : 0;
                if (config.VSync is bool vs) native.Vsync = vs ? 1 : 0;
                if (config.DefaultFontFamily is not null)
                    native.DefaultFontFamily = Alloc(config.DefaultFontFamily, temp);
                if (config.DefaultFontSize is int fs) native.DefaultFontSize = fs;
                if (config.AssetFolders is not null)
                {
                    folders = new Utf8StringArray(config.AssetFolders);
                    native.AssetFolders = folders.Pointer;
                    native.AssetFolderCount = folders.Count;
                }
                if (config.PerfOverlay is bool po) native.PerfOverlay = po ? 1 : 0;
                if (config.NoBundleDecius is bool nb) native.NoBundleDecius = nb ? 1 : 0;
                if (config.NativeMenus is bool nm) native.NativeMenus = nm ? 1 : 0;
                if (config.TitleBar is TitleBarStyle tb) native.Titlebar = (int)tb;
                if (config.TrafficLightPosition is Point tl)
                {
                    native.TrafficLightX = tl.X;
                    native.TrafficLightY = tl.Y;
                }
            }

            // The config is copied by the core, so the temporaries can be
            // freed as soon as the call returns.
            IntPtr appHandle = NativeMethods.affineui_app_create(in native);
            if (appHandle == IntPtr.Zero)
                throw new InvalidOperationException("affineui_app_create failed.");
            _handle = new AppSafeHandle(appHandle);
        }
        finally
        {
            foreach (var p in temp) Marshal.FreeCoTaskMem(p);
            folders?.Dispose();
        }

        static IntPtr Alloc(string s, List<IntPtr> temp)
        {
            IntPtr p = Marshal.StringToCoTaskMemUTF8(s);
            temp.Add(p);
            return p;
        }
    }

    internal bool IsDisposed => _handle.IsClosed;
    internal IntPtr Handle => _handle.IsClosed ? IntPtr.Zero : _handle.DangerousGetHandle();

    public void Dispose() => _handle.Dispose();

    // ── Content ──────────────────────────────────────────────────────────

    public void LoadHtml(string html)
    {
        NativeMethods.affineui_app_load_html(Handle, html);
        GC.KeepAlive(this);
    }

    public bool LoadHtmlFile(string path)
    {
        bool ok = NativeMethods.affineui_app_load_html_file(Handle, path) != 0;
        GC.KeepAlive(this);
        return ok;
    }

    /// <summary>Loads a <see cref="View"/> into the app. The view is copied
    /// (callbacks included); the app never borrows the caller's view.</summary>
    public void LoadView(View view)
    {
        ArgumentNullException.ThrowIfNull(view);
        NativeMethods.affineui_app_load_view(Handle, view.Handle);
        GC.KeepAlive(this);
        GC.KeepAlive(view);
    }

    /// <summary>Sets the user stylesheet. <paramref name="baseUrl"/> (optional)
    /// is the stylesheet's own location, so its relative url()s resolve like a
    /// linked sheet's.</summary>
    public void SetStylesheet(string css, string? baseUrl = null)
    {
        NativeMethods.affineui_app_set_stylesheet(Handle, css, baseUrl);
        GC.KeepAlive(this);
    }

    public void Invalidate()
    {
        NativeMethods.affineui_app_invalidate(Handle);
        GC.KeepAlive(this);
    }

    public bool PerfOverlayEnabled
    {
        get
        {
            bool v = NativeMethods.affineui_app_perf_overlay_enabled(Handle) != 0;
            GC.KeepAlive(this);
            return v;
        }
        set
        {
            NativeMethods.affineui_app_set_perf_overlay_enabled(Handle, value ? 1 : 0);
            GC.KeepAlive(this);
        }
    }

    // ── Loop / input ─────────────────────────────────────────────────────

    /// <summary>Dispatches an input event. Returns true when the UI consumed
    /// it.</summary>
    public bool Dispatch(in Event ev)
    {
        AffineUIRuntime.CheckThread();
        NativeEvent native = ev.ToNative(out IntPtr text);
        try
        {
            bool consumed = NativeMethods.affineui_app_dispatch(Handle, in native) != 0;
            GC.KeepAlive(this);
            return consumed;
        }
        finally
        {
            if (text != IntPtr.Zero) Marshal.FreeCoTaskMem(text);
        }
    }

    /// <summary>Registers an app-global capture handler before focused-widget
    /// dispatch. Return true to consume the event, for example to route
    /// Ctrl/Cmd+Z to a global undo stack instead of the active text field.</summary>
    public void OnEventCapture(Func<Event, bool> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        IntPtr user = GCHandle.ToIntPtr(GCHandle.Alloc(handler));
        NativeMethods.affineui_app_on_event_capture(
            Handle, Trampolines.EventCapture, user, Trampolines.FreeUser);
        GC.KeepAlive(this);
    }

    /// <summary>Runs the native loop on the calling thread; returns the OS
    /// exit code. Blocks until the app quits.</summary>
    public int Run()
    {
        AffineUIRuntime.CheckThread();
        int code = NativeMethods.affineui_app_run(Handle);
        GC.KeepAlive(this);
        return code;
    }

    /// <summary>Quit unconditionally. Does NOT run the close-request handler:
    /// quit means quit. Use <see cref="Close"/> for the cancellable path.</summary>
    public void Quit(int exitCode = 0)
    {
        NativeMethods.affineui_app_quit(Handle, exitCode);
        GC.KeepAlive(this);
    }

    // ── Close requests ───────────────────────────────────────────────────

    /// <summary>
    /// The one veto point for "close this app": the window's close button, Cmd-Q,
    /// the menu's Quit, and a close button the app drew itself all run it.
    /// Return false to CANCEL the close, true to let it proceed (the Electron
    /// <c>e.preventDefault()</c> shape) — so a dirty document can prompt first.
    /// Pass null to clear the handler.
    /// </summary>
    public void OnCloseRequest(Func<bool>? handler)
    {
        if (handler is null)
        {
            NativeMethods.affineui_app_on_close_request(Handle, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        }
        else
        {
            // The GCHandle is released by the core's user_free (exactly once, when
            // the handler is replaced or the app is destroyed) — never here.
            IntPtr user = GCHandle.ToIntPtr(GCHandle.Alloc(handler));
            NativeMethods.affineui_app_on_close_request(
                Handle, Trampolines.CloseRequest, user, Trampolines.FreeUser);
        }
        GC.KeepAlive(this);
    }

    // ── Window controls ──────────────────────────────────────────────────
    // What a title bar's buttons do. An app drawing its own close/minimize/
    // maximize (TitleBarStyle.Frameless) wires them to these.

    /// <summary>Ask to close: runs the <see cref="OnCloseRequest"/> handler, so
    /// this is cancellable.</summary>
    public void Close()
    {
        NativeMethods.affineui_app_close(Handle);
        GC.KeepAlive(this);
    }

    public void Minimize()
    {
        NativeMethods.affineui_app_minimize(Handle);
        GC.KeepAlive(this);
    }

    public void ToggleMaximize()
    {
        NativeMethods.affineui_app_toggle_maximize(Handle);
        GC.KeepAlive(this);
    }

    public bool IsMaximized
    {
        get
        {
            bool v = NativeMethods.affineui_app_is_maximized(Handle) != 0;
            GC.KeepAlive(this);
            return v;
        }
    }

    public void SetFullscreen(bool on)
    {
        NativeMethods.affineui_app_set_fullscreen(Handle, on ? 1 : 0);
        GC.KeepAlive(this);
    }

    public bool IsFullscreen
    {
        get
        {
            bool v = NativeMethods.affineui_app_is_fullscreen(Handle) != 0;
            GC.KeepAlive(this);
            return v;
        }
    }

    // ── Application menu ─────────────────────────────────────────────────

    /// <summary>
    /// Install (or replace) the application menu. The menu is COPIED into the
    /// core, so it is safe to call at any time — a menu showing checked/enabled
    /// state is expected to be rebuilt and re-set as that state changes. Pass
    /// null to clear.
    ///
    /// <para>Ignored when <see cref="AppConfig.NativeMenus"/> is false (and off
    /// macOS today, where the drawn <see cref="View.MenuBar"/> is still the only
    /// bar).</para>
    /// </summary>
    public void SetMenu(Menu? menu)
    {
        AffineUIRuntime.CheckThread();

        // Build-then-install (the C ABI's builder shape: the model is a tree and
        // C is not). affineui_app_set_menu copies the builder — each item's
        // closure is shared with the copy, not moved — so the builder is
        // destroyed here while the app's copy keeps every GCHandle alive.
        IntPtr root = NativeMethods.affineui_menu_create();
        if (root == IntPtr.Zero)
            throw new InvalidOperationException("affineui_menu_create failed.");
        try
        {
            if (menu is not null)
            {
                foreach (MenuItem item in menu) Emit(root, item);
            }
            NativeMethods.affineui_app_set_menu(Handle, menu is null ? IntPtr.Zero : root);
        }
        finally
        {
            NativeMethods.affineui_menu_destroy(root);
        }
        GC.KeepAlive(this);
    }

    /// <summary>
    /// One row of the model into the C builder, depth-first. The select closure's
    /// GCHandle is NOT freed here: the core holds it (the installed copy shares
    /// it with the builder) and releases it through user_free exactly once, when
    /// the menu is replaced or the app dies. Freeing it on the way out — the
    /// BuildScope pattern — would hand the menu a collected delegate.
    /// </summary>
    private static void Emit(IntPtr menu, MenuItem item)
    {
        switch (item.ItemKind)
        {
            case MenuItem.Kind.Separator:
                NativeMethods.affineui_menu_add_separator(menu);
                return;  // nothing to decorate

            case MenuItem.Kind.Role:
                // The platform supplies label, accelerator and behavior; a role
                // item takes no callback (affineui_menu_add_role has no slot for
                // one), so MenuItem.OnSelect is ignored here by design.
                NativeMethods.affineui_menu_add_role(menu, (int)item.ItemRole);
                // …but the caller MAY override the label (MenuItem.Role(role,
                // label)). Without this the override was silently dropped and
                // the item quietly showed the platform's wording instead.
                if (!string.IsNullOrEmpty(item.Label))
                    NativeMethods.affineui_menu_set_label(menu, item.Label);
                break;

            case MenuItem.Kind.Submenu:
            {
                IntPtr sub = NativeMethods.affineui_menu_add_submenu(menu, item.Label);
                // `sub` is owned by `menu` — fill it, never destroy it.
                if (sub != IntPtr.Zero)
                {
                    foreach (MenuItem child in item.Submenu) Emit(sub, child);
                }
                break;
            }

            case MenuItem.Kind.Check:
            {
                var (fn, user, free) = Select(item.OnSelect);
                NativeMethods.affineui_menu_add_check(
                    menu, item.Label, item.Checked ? 1 : 0, item.Accelerator, fn, user, free);
                break;
            }

            default:
            {
                var (fn, user, free) = Select(item.OnSelect);
                NativeMethods.affineui_menu_add_item(
                    menu, item.Label, item.Accelerator, fn, user, free);
                break;
            }
        }

        // set_swatch / set_enabled decorate the LAST item added — which is this
        // one, since a submenu's children go into `sub`, not into `menu`.
        if (item.Swatch is Color c)
        {
            NativeMethods.affineui_menu_set_swatch(
                menu, new NativeColor { R = c.R, G = c.G, B = c.B, A = c.A });
        }
        if (!item.Enabled) NativeMethods.affineui_menu_set_enabled(menu, 0);
    }

    /// <summary>
    /// A menu item's (fn, user, user_free) triple. All three are null when there
    /// is no handler: the core only takes ownership of `user` when `fn` is
    /// non-null, so allocating a GCHandle without one would leak it forever.
    /// The select callback is the same shape as a click, hence Trampolines.Click.
    /// </summary>
    private static (IntPtr Fn, IntPtr User, IntPtr Free) Select(Action? onSelect)
    {
        if (onSelect is null) return (IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        return (Trampolines.Click,
                GCHandle.ToIntPtr(GCHandle.Alloc(onSelect)),
                Trampolines.FreeUser);
    }

    // ── Metrics ──────────────────────────────────────────────────────────

    public Size WindowSize
    {
        get
        {
            NativeMethods.affineui_app_window_size(Handle, out int w, out int h);
            GC.KeepAlive(this);
            return new Size(w, h);
        }
    }

    public Size FramebufferSize
    {
        get
        {
            NativeMethods.affineui_app_framebuffer_size(Handle, out int w, out int h);
            GC.KeepAlive(this);
            return new Size(w, h);
        }
    }

    public float DpiScale
    {
        get
        {
            float s = NativeMethods.affineui_app_dpi_scale(Handle);
            GC.KeepAlive(this);
            return s;
        }
    }

    /// <summary>The app's document — a borrowed handle, valid exactly as long
    /// as the app. It is never destroyed by the wrapper and it keeps the app
    /// alive.</summary>
    public Document Document
    {
        get
        {
            if (_document is null)
            {
                IntPtr doc = NativeMethods.affineui_app_document(Handle);
                GC.KeepAlive(this);
                _document = Document.Borrowed(this, doc);
            }
            return _document;
        }
    }
}
