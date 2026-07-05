// Widget — a safe handle over the C ABI's heap-copied WidgetRef.
// Hard-to-crash: a widget whose node is gone reads defaults and no-ops on
// writes; a disposed widget degrades the same way (its native handle becomes
// null, and every C entry point no-ops on null).

using System.Runtime.InteropServices;

namespace AffineUI;

/// <summary>
/// An id-addressed handle to a widget in a <see cref="View"/>. Survives
/// reconciliation (re-finds its node by key) and degrades gracefully when the
/// node is gone. Every widget holds a strong reference to its
/// <see cref="View"/>, and the native view is kept alive until all widget
/// handles are released, so use-after-free cannot occur in either order.
/// </summary>
public sealed class Widget : IDisposable
{
    internal sealed class WidgetSafeHandle : SafeHandle
    {
        private readonly SafeHandle? _viewHandle;
        private readonly bool _addRefed;

        public WidgetSafeHandle(IntPtr h, SafeHandle viewHandle) : base(IntPtr.Zero, ownsHandle: true)
        {
            if (h != IntPtr.Zero)
            {
                bool ok = false;
                viewHandle.DangerousAddRef(ref ok);
                _viewHandle = viewHandle;
                _addRefed = ok;
            }
            SetHandle(h);
        }

        public override bool IsInvalid => handle == IntPtr.Zero;

        protected override bool ReleaseHandle()
        {
            NativeMethods.affineui_widget_destroy(handle);
            if (_addRefed) _viewHandle!.DangerousRelease();
            return true;
        }
    }

    private readonly WidgetSafeHandle _handle;

    /// <summary>The view this widget belongs to (kept alive by this widget).</summary>
    public View View { get; }

    internal Widget(IntPtr handle, View view)
    {
        View = view;
        _handle = new WidgetSafeHandle(handle, view.SafeHandleForWidgets);
    }

    internal WidgetSafeHandle NativeHandle => _handle;

    private IntPtr Handle => _handle.IsClosed ? IntPtr.Zero : _handle.DangerousGetHandle();

    /// <summary>Releases the native widget handle early. Optional — the GC
    /// finalizes it otherwise.</summary>
    public void Dispose() => _handle.Dispose();

    // ── Reads ────────────────────────────────────────────────────────────

    /// <summary>True when the handle currently resolves to a live node.</summary>
    public bool IsValid
    {
        get
        {
            bool v = NativeMethods.affineui_widget_valid(Handle) != 0;
            GC.KeepAlive(this);
            return v;
        }
    }

    /// <summary>Kind of the attached node; <see cref="WidgetKind.None"/> when
    /// no node resolves. Used for typed-component validity checks.</summary>
    public WidgetKind Kind
    {
        get
        {
            var k = (WidgetKind)NativeMethods.affineui_widget_get_kind(Handle);
            GC.KeepAlive(this);
            return k;
        }
    }

    /// <summary>The widget's stable key (widget-name), or empty.</summary>
    public string Name
    {
        get
        {
            var s = AffineUIRuntime.TakeString(NativeMethods.affineui_widget_name(Handle));
            GC.KeepAlive(this);
            return s;
        }
    }

    /// <summary>Reads an attribute value, or <paramref name="fallback"/> when
    /// absent (or the node is gone).</summary>
    public string Attr(string name, string fallback = "")
    {
        var s = AffineUIRuntime.TakeString(
            NativeMethods.affineui_widget_attr(Handle, name, fallback), fallback);
        GC.KeepAlive(this);
        return s;
    }

    /// <summary>The widget's text content.</summary>
    public string Text
    {
        get
        {
            var s = AffineUIRuntime.TakeString(NativeMethods.affineui_widget_text(Handle));
            GC.KeepAlive(this);
            return s;
        }
    }

    public bool HasAttr(string name)
    {
        bool v = NativeMethods.affineui_widget_has_attr(Handle, name) != 0;
        GC.KeepAlive(this);
        return v;
    }

    // ── Writes (fluent; no-op when the node is gone) ─────────────────────

    public Widget SetText(string text)
    {
        NativeMethods.affineui_widget_set_text(Handle, text);
        return this;
    }

    public Widget SetAttr(string name, string value)
    {
        NativeMethods.affineui_widget_set_attr(Handle, name, value);
        return this;
    }

    public Widget RemoveAttr(string name)
    {
        NativeMethods.affineui_widget_remove_attr(Handle, name);
        return this;
    }

    public Widget SetSelector(string name, string value)
    {
        NativeMethods.affineui_widget_set_selector(Handle, name, value);
        return this;
    }

    public Widget AddClass(string classes)
    {
        NativeMethods.affineui_widget_add_class(Handle, classes);
        return this;
    }

    /// <summary>Removes all of the widget's children.</summary>
    public Widget Clear()
    {
        NativeMethods.affineui_widget_clear(Handle);
        return this;
    }

    // ── Callbacks ────────────────────────────────────────────────────────
    //
    // The managed closure is boxed in a GCHandle passed as `user`; the core
    // calls `user_free` (a static trampoline that frees the GCHandle) exactly
    // once when it drops its last reference to the handler — including when
    // the registration is rejected (null handle) — so this is leak-free with
    // no delegate bookkeeping.

    /// <summary>Registers a click handler. Exceptions it throws are routed to
    /// <see cref="AffineUIRuntime.OnCallbackException"/>.</summary>
    public Widget OnClick(Action handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        var user = GCHandle.ToIntPtr(GCHandle.Alloc(handler));
        NativeMethods.affineui_widget_on_click(Handle, Trampolines.Click, user, Trampolines.FreeUser);
        return this;
    }

    /// <summary>Registers a change handler; receives the new value as a
    /// string.</summary>
    public Widget OnChange(Action<string> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        var user = GCHandle.ToIntPtr(GCHandle.Alloc(handler));
        NativeMethods.affineui_widget_on_change(Handle, Trampolines.Change, user, Trampolines.FreeUser);
        return this;
    }

    // ── Children ─────────────────────────────────────────────────────────

    /// <summary>Appends children built by <paramref name="build"/> (runs
    /// synchronously).</summary>
    public Widget Append(Action<View> build)
    {
        ArgumentNullException.ThrowIfNull(build);
        using var scope = BuildScope.Create(build);
        NativeMethods.affineui_widget_append(Handle, scope.Fn, scope.User);
        return this;
    }

    /// <summary>Replaces the widget's children with the tree built by
    /// <paramref name="build"/> (runs synchronously).</summary>
    public Widget Replace(Action<View> build)
    {
        ArgumentNullException.ThrowIfNull(build);
        using var scope = BuildScope.Create(build);
        NativeMethods.affineui_widget_replace(Handle, scope.Fn, scope.User);
        return this;
    }

    /// <summary>Finds a descendant widget by key. Always returns a handle;
    /// check <see cref="IsValid"/>.</summary>
    public Widget FindWidget(string name)
    {
        var found = View.Wrap(NativeMethods.affineui_widget_find_widget(Handle, name));
        GC.KeepAlive(this);
        return found;
    }
}
