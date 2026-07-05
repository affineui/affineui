// Mode A — C# owns the system: AffineUI creates the native window and runs
// the loop on the calling thread. Wraps affineui_app_*.

using System.Runtime.InteropServices;

namespace AffineUI;

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
}

/// <summary>
/// An AffineUI application that owns the native window and main loop.
/// <see cref="Run"/> blocks the calling thread until the app quits.
/// Single-threaded: create and use on one thread.
/// </summary>
public sealed class App : IDisposable
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
    private IntPtr Handle => _handle.IsClosed ? IntPtr.Zero : _handle.DangerousGetHandle();

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

    /// <summary>Runs the native loop on the calling thread; returns the OS
    /// exit code. Blocks until the app quits.</summary>
    public int Run()
    {
        AffineUIRuntime.CheckThread();
        int code = NativeMethods.affineui_app_run(Handle);
        GC.KeepAlive(this);
        return code;
    }

    public void Quit(int exitCode = 0)
    {
        NativeMethods.affineui_app_quit(Handle, exitCode);
        GC.KeepAlive(this);
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
