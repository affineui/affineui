// Document — headless-capable DOM + layout. Owned documents come from the
// parameterless constructor (destroyed on Dispose); borrowed documents come
// from App.Document (never destroyed, hold a strong reference to their App).

using System.Runtime.InteropServices;

namespace AffineUI;

/// <summary>
/// A DOM document with CSS layout. Usable fully headless (set HTML, lay out,
/// query content size, dispatch synthetic events) — no window or GPU needed.
/// </summary>
public sealed class Document : IDisposable
{
    internal sealed class DocumentSafeHandle : SafeHandle
    {
        public DocumentSafeHandle(IntPtr h, bool owns) : base(IntPtr.Zero, ownsHandle: owns)
        {
            SetHandle(h);
        }

        public override bool IsInvalid => handle == IntPtr.Zero;

        protected override bool ReleaseHandle()
        {
            NativeMethods.affineui_document_destroy(handle);
            return true;
        }
    }

    private readonly DocumentSafeHandle _handle;
    private readonly App? _owner; // borrowed documents keep their app alive

    /// <summary>Creates an owned, headless document.</summary>
    public Document()
    {
        AffineUIRuntime.EnsureLoaded();
        AffineUIRuntime.CheckThread();
        IntPtr h = NativeMethods.affineui_document_create();
        if (h == IntPtr.Zero)
            throw new InvalidOperationException("affineui_document_create failed.");
        _handle = new DocumentSafeHandle(h, owns: true);
    }

    private Document(App owner, IntPtr borrowed)
    {
        _owner = owner;
        _handle = new DocumentSafeHandle(borrowed, owns: false);
    }

    internal static Document Borrowed(App owner, IntPtr handle) => new(owner, handle);

    private IntPtr Handle
    {
        get
        {
            // A borrowed document is valid exactly as long as its app; after
            // the app is disposed we hand the native side a null handle, which
            // no-ops (hard-to-crash contract).
            if (_owner is { IsDisposed: true }) return IntPtr.Zero;
            return _handle.IsClosed ? IntPtr.Zero : _handle.DangerousGetHandle();
        }
    }

    /// <summary>Destroys an owned document; a no-op for the borrowed document
    /// of an <see cref="App"/>.</summary>
    public void Dispose() => _handle.Dispose();

    // ── Content ──────────────────────────────────────────────────────────

    public void SetHtml(string html)
    {
        NativeMethods.affineui_document_set_html(Handle, html);
        KeepAlive();
    }

    /// <summary><paramref name="baseUrl"/> (optional) is the stylesheet's own
    /// location so its relative url()s resolve like a linked sheet's.</summary>
    public void SetUserStylesheet(string css, string? baseUrl = null)
    {
        NativeMethods.affineui_document_set_user_stylesheet(Handle, css, baseUrl);
        KeepAlive();
    }

    public void ReloadStylesheets()
    {
        NativeMethods.affineui_document_reload_stylesheets(Handle);
        KeepAlive();
    }

    // ── Layout ───────────────────────────────────────────────────────────

    public void Layout(int viewportWidth, int viewportHeight)
    {
        NativeMethods.affineui_document_layout(Handle, viewportWidth, viewportHeight);
        KeepAlive();
    }

    /// <summary>Document content size after the last layout pass.</summary>
    public Size ContentSize
    {
        get
        {
            NativeMethods.affineui_document_content_size(Handle, out int w, out int h);
            KeepAlive();
            return new Size(w, h);
        }
    }

    // ── Live DOM mutation (return true only when the document changed) ───

    public bool SetAttributeById(string elementId, string name, string value)
    {
        bool changed = NativeMethods.affineui_document_set_attribute_by_id(Handle, elementId, name, value) != 0;
        KeepAlive();
        return changed;
    }

    public bool RemoveAttributeById(string elementId, string name)
    {
        bool changed = NativeMethods.affineui_document_remove_attribute_by_id(Handle, elementId, name) != 0;
        KeepAlive();
        return changed;
    }

    public bool SetTextById(string elementId, string text)
    {
        bool changed = NativeMethods.affineui_document_set_text_by_id(Handle, elementId, text) != 0;
        KeepAlive();
        return changed;
    }

    // ── Input / behavior ─────────────────────────────────────────────────

    /// <summary>Dispatches a (possibly synthetic) input event to the document
    /// and returns what it requested in response.</summary>
    public DispatchResult Dispatch(in Event ev)
    {
        AffineUIRuntime.CheckThread();
        NativeEvent native = ev.ToNative(out IntPtr text);
        try
        {
            NativeMethods.affineui_document_dispatch(Handle, in native, out NativeDispatchResult result);
            KeepAlive();
            return DispatchResult.FromNative(in result);
        }
        finally
        {
            if (text != IntPtr.Zero) Marshal.FreeCoTaskMem(text);
        }
    }

    /// <summary>Attaches an optional behavior script
    /// (e.g. <see cref="DocumentScript.UiControls"/>).</summary>
    public void AttachScript(DocumentScript script)
    {
        NativeMethods.affineui_document_attach_script(Handle, (int)script);
        KeepAlive();
    }

    public void DetachScript(DocumentScript script)
    {
        NativeMethods.affineui_document_detach_script(Handle, (int)script);
        KeepAlive();
    }

    /// <summary>Cursor the OS should display for the hovered element.</summary>
    public Cursor HoveredCursor
    {
        get
        {
            var c = (Cursor)NativeMethods.affineui_document_hovered_cursor(Handle);
            KeepAlive();
            return c;
        }
    }

    private void KeepAlive()
    {
        GC.KeepAlive(this);
        if (_owner is not null) GC.KeepAlive(_owner);
    }
}
