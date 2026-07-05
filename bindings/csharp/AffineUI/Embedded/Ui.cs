// Mode B — embedded: the host owns the GPU device, window, loop, and input
// pump; AffineUI renders into render-target views the host supplies each
// frame and never presents. Wraps affineui_ui_*. Nothing here touches
// sokol_app (all-or-nothing ownership).

using System.Runtime.InteropServices;

namespace AffineUI.Embedded;

/// <summary>
/// An embedded AffineUI instance. Per frame the host calls
/// <see cref="Render"/> from inside its own frame (state-clobber contract per
/// EMBEDDING_DESIGN.md) and forwards translated input via
/// <see cref="Dispatch"/>; <see cref="NeedsUpdate"/> lets an on-demand host
/// skip idle frames. Single-threaded: use on the thread that owns the
/// graphics context.
/// </summary>
public sealed class Ui : IDisposable
{
    internal sealed class UiSafeHandle : SafeHandle
    {
        public UiSafeHandle(IntPtr h) : base(IntPtr.Zero, ownsHandle: true) => SetHandle(h);
        public override bool IsInvalid => handle == IntPtr.Zero;
        protected override bool ReleaseHandle()
        {
            NativeMethods.affineui_ui_destroy(handle);
            return true;
        }
    }

    private readonly UiSafeHandle _handle;
    private GCHandle _logHandle; // held for the ui's lifetime; freed on Dispose

    public Ui()
    {
        AffineUIRuntime.EnsureLoaded();
        AffineUIRuntime.CheckThread();
        IntPtr h = NativeMethods.affineui_ui_create();
        if (h == IntPtr.Zero)
            throw new InvalidOperationException("affineui_ui_create failed.");
        _handle = new UiSafeHandle(h);
    }

    private IntPtr Handle => _handle.IsClosed ? IntPtr.Zero : _handle.DangerousGetHandle();

    public void Dispose()
    {
        _handle.Dispose();
        if (_logHandle.IsAllocated) _logHandle.Free();
    }

    // ── Lifecycle ────────────────────────────────────────────────────────

    /// <summary>
    /// Initializes against the host's graphics objects. The caller vouches
    /// that the raw device pointers in <see cref="InitDesc.Gpu"/> are live and
    /// match the compiled backend.
    /// </summary>
    public unsafe void Init(InitDesc desc)
    {
        ArgumentNullException.ThrowIfNull(desc);
        AffineUIRuntime.CheckThread();

        IntPtr font = desc.DefaultFontFamily is null
            ? IntPtr.Zero
            : Marshal.StringToCoTaskMemUTF8(desc.DefaultFontFamily);
        try
        {
            var native = new NativeInitDesc
            {
                DefaultFontFamily = font,
                DefaultFontSize = desc.DefaultFontSize,
            };

            if (desc.Log is not null)
            {
                if (_logHandle.IsAllocated) _logHandle.Free();
                _logHandle = GCHandle.Alloc(desc.Log);
                native.Log = Trampolines.Log;
                native.LogUser = GCHandle.ToIntPtr(_logHandle);
            }

            if (desc.Gpu is GpuContext gpu)
            {
                NativeGpuContext nativeGpu = gpu.ToNative();
                native.Gpu = (IntPtr)(&nativeGpu);
                NativeMethods.affineui_ui_init(Handle, in native);
            }
            else
            {
                NativeMethods.affineui_ui_init(Handle, in native);
            }
        }
        finally
        {
            if (font != IntPtr.Zero) Marshal.FreeCoTaskMem(font);
            GC.KeepAlive(this);
        }
    }

    /// <summary>Drops all content and handlers, back to the just-initialized
    /// state.</summary>
    public void Reset()
    {
        NativeMethods.affineui_ui_reset(Handle);
        GC.KeepAlive(this);
    }

    // ── Content ──────────────────────────────────────────────────────────

    public void SetHtml(string html)
    {
        NativeMethods.affineui_ui_set_html(Handle, html);
        GC.KeepAlive(this);
    }

    public void SetCss(string css)
    {
        NativeMethods.affineui_ui_set_css(Handle, css);
        GC.KeepAlive(this);
    }

    public bool LoadFile(string path)
    {
        bool ok = NativeMethods.affineui_ui_load_file(Handle, path) != 0;
        GC.KeepAlive(this);
        return ok;
    }

    public void SetClearColor(Color color)
    {
        NativeMethods.affineui_ui_set_clear_color(Handle, color.R, color.G, color.B, color.A);
        GC.KeepAlive(this);
    }

    // ── Render / update scheduling ───────────────────────────────────────

    /// <summary>Renders into the host's target views for this frame. Call
    /// from inside the host's frame, on the graphics thread.</summary>
    public void Render(in FrameTarget target)
    {
        AffineUIRuntime.CheckThread();
        NativeFrameTarget native = target.ToNative();
        NativeMethods.affineui_ui_render(Handle, in native);
        GC.KeepAlive(this);
    }

    /// <summary>True when a repaint is needed — an on-demand host may skip
    /// rendering otherwise.</summary>
    public bool NeedsUpdate
    {
        get
        {
            bool v = NativeMethods.affineui_ui_needs_update(Handle) != 0;
            GC.KeepAlive(this);
            return v;
        }
    }

    public void MarkDirty()
    {
        NativeMethods.affineui_ui_mark_dirty(Handle);
        GC.KeepAlive(this);
    }

    // ── Input ────────────────────────────────────────────────────────────

    /// <summary>Dispatches a translated host event. Returns true when the UI
    /// consumed it (the host should suppress its own handling).</summary>
    public bool Dispatch(in Event ev)
    {
        AffineUIRuntime.CheckThread();
        NativeEvent native = ev.ToNative(out IntPtr text);
        try
        {
            bool consumed = NativeMethods.affineui_ui_dispatch(Handle, in native) != 0;
            GC.KeepAlive(this);
            return consumed;
        }
        finally
        {
            if (text != IntPtr.Zero) Marshal.FreeCoTaskMem(text);
        }
    }

    /// <summary>Registers a click handler for elements matching a minimal CSS
    /// selector ("#id", ".cls", "tag", "a,b"). The closure is released
    /// exactly once when the core drops the handler.</summary>
    public void OnClick(string selector, Action handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        IntPtr user = GCHandle.ToIntPtr(GCHandle.Alloc(handler));
        NativeMethods.affineui_ui_on_click(Handle, selector, Trampolines.Click, user, Trampolines.FreeUser);
        GC.KeepAlive(this);
    }

    /// <summary>Cursor the OS should display under the last hovered
    /// position.</summary>
    public Cursor HoveredCursor
    {
        get
        {
            var c = (Cursor)NativeMethods.affineui_ui_hovered_cursor(Handle);
            GC.KeepAlive(this);
            return c;
        }
    }

    // ── Live DOM mutation (return true only when the document changed) ───

    public bool SetAttr(string elementId, string name, string value)
    {
        bool changed = NativeMethods.affineui_ui_set_attr(Handle, elementId, name, value) != 0;
        GC.KeepAlive(this);
        return changed;
    }

    public bool RemoveAttr(string elementId, string name)
    {
        bool changed = NativeMethods.affineui_ui_remove_attr(Handle, elementId, name) != 0;
        GC.KeepAlive(this);
        return changed;
    }

    public bool SetText(string elementId, string text)
    {
        bool changed = NativeMethods.affineui_ui_set_text(Handle, elementId, text) != 0;
        GC.KeepAlive(this);
        return changed;
    }

    /// <summary>Document content size after the last layout pass.</summary>
    public Size ContentSize
    {
        get
        {
            NativeMethods.affineui_ui_content_size(Handle, out int w, out int h);
            GC.KeepAlive(this);
            return new Size(w, h);
        }
    }
}
