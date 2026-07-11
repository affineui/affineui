// Recycling virtual lists & trees — idiomatic wrappers over the provider
// C ABI (include/affineui/c_api_app.h "Virtual lists & trees" section).
//
// Providers are STATELESS bridges of callbacks between your model and the
// recycling widget: they store no items. Keep the provider object alive as
// long as any view references it — the widget holds a weak reference and
// degrades to an empty list if the provider is disposed first (never a
// crash). Callback GCHandles are freed exactly once through user_free.

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace AffineUI;

/// <summary>Mirrors <c>affineui::SelectMod</c>: row-activation modifier intent.</summary>
public enum SelectMod
{
    Replace = 0,
    Toggle = 1,
    Range = 2,
}

/// <summary>Mirrors <c>affineui::Axis</c>.</summary>
public enum Axis
{
    Vertical = 0,
    Horizontal = 1,
}

internal static partial class NativeMethods
{
    [LibraryImport(Lib)] internal static partial IntPtr affineui_index_selection_create();
    [LibraryImport(Lib)] internal static partial void affineui_index_selection_destroy(IntPtr sel);
    [LibraryImport(Lib)] internal static partial void affineui_index_selection_apply(IntPtr sel, nuint index, int mode, nuint count);
    [LibraryImport(Lib)] internal static partial int affineui_index_selection_contains(IntPtr sel, nuint index);
    [LibraryImport(Lib)] internal static partial void affineui_index_selection_clear(IntPtr sel);
    [LibraryImport(Lib)] internal static partial nuint affineui_index_selection_size(IntPtr sel);
    [LibraryImport(Lib)] internal static partial nuint affineui_index_selection_anchor(IntPtr sel);
    [LibraryImport(Lib)] internal static partial void affineui_index_selection_on_change(IntPtr sel, IntPtr fn, IntPtr user, IntPtr userFree);

    [LibraryImport(Lib)] internal static partial IntPtr affineui_vlist_provider_create();
    [LibraryImport(Lib)] internal static partial void affineui_vlist_provider_destroy(IntPtr p);
    [LibraryImport(Lib)] internal static partial void affineui_vlist_provider_on_item_count(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vlist_provider_on_item_size(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vlist_provider_on_item_text(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vlist_provider_on_build_item(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vlist_provider_on_is_selected(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vlist_provider_on_activate(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vlist_provider_on_is_checked(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vlist_provider_on_set_checked(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vlist_provider_set_checkboxes(IntPtr p, int on);
    [LibraryImport(Lib)] internal static partial void affineui_vlist_provider_set_default_item_size(IntPtr p, double px);

    [LibraryImport(Lib)] internal static partial IntPtr affineui_vtree_provider_create();
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_destroy(IntPtr p);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_item_count(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_item_size(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_item_text(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_build_item(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_is_selected(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_activate(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_is_checked(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_set_checked(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_depth(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_is_expandable(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_is_expanded(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_on_toggle(IntPtr p, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_set_checkboxes(IntPtr p, int on);
    [LibraryImport(Lib)] internal static partial void affineui_vtree_provider_set_default_item_size(IntPtr p, double px);

    [LibraryImport(Lib)] internal static partial IntPtr affineui_tree_flattener_create(IntPtr children, IntPtr label, IntPtr hasChildren, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_tree_flattener_destroy(IntPtr f);
    [LibraryImport(Lib)] internal static partial void affineui_tree_flattener_wire(IntPtr f, IntPtr p);
    [LibraryImport(Lib)] internal static partial void affineui_tree_flattener_rebuild(IntPtr f);
    [LibraryImport(Lib)] internal static partial void affineui_tree_flattener_on_changed(IntPtr f, IntPtr fn, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_tree_flattener_set_expanded(IntPtr f, ulong handle, int open);
    [LibraryImport(Lib)] internal static partial int affineui_tree_flattener_is_expanded(IntPtr f, ulong handle);
    [LibraryImport(Lib)] internal static partial void affineui_tree_flattener_set_selected(IntPtr f, ulong handle, int on);
    [LibraryImport(Lib)] internal static partial int affineui_tree_flattener_selected_contains(IntPtr f, ulong handle);
    [LibraryImport(Lib)] internal static partial void affineui_tree_flattener_clear_selection(IntPtr f);
    [LibraryImport(Lib)] internal static partial void affineui_tree_flattener_set_checked(IntPtr f, ulong handle, int on);
    [LibraryImport(Lib)] internal static partial int affineui_tree_flattener_checked_contains(IntPtr f, ulong handle);
    [LibraryImport(Lib)] internal static partial nuint affineui_tree_flattener_size(IntPtr f);
    [LibraryImport(Lib)] internal static partial ulong affineui_tree_flattener_handle_at(IntPtr f, nuint index);
    [LibraryImport(Lib)] internal static partial nuint affineui_tree_flattener_index_of(IntPtr f, ulong handle);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_virtual_list(IntPtr view, string? key, IntPtr provider, int axis, string? classes);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_virtual_tree(IntPtr view, string? key, IntPtr provider, string? classes);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_virtual_string_list(IntPtr view, string? key, IntPtr items, nuint itemCount, double itemSize, IntPtr selection, IntPtr checkedSel, string? classes);

    [LibraryImport(Lib)] internal static partial void affineui_app_set_view(IntPtr app, IntPtr build, IntPtr user, IntPtr userFree);
    [LibraryImport(Lib)] internal static partial void affineui_app_rebuild_view(IntPtr app);
}

// ── Provider callback trampolines ─────────────────────────────────────
// Same GCHandle discipline as Trampolines in AffineUIRuntime.cs; the text
// trampolines pin a scratch UTF-8 buffer per closure (the C side copies the
// returned pointer before the call returns).

internal sealed class TextScratch
{
    public Func<nuint, string>? ByIndex;
    public IntPtr Buffer = IntPtr.Zero;

    public IntPtr Fill(string text)
    {
        if (Buffer != IntPtr.Zero) Marshal.FreeCoTaskMem(Buffer);
        Buffer = Marshal.StringToCoTaskMemUTF8(text);
        return Buffer;
    }

    ~TextScratch()
    {
        if (Buffer != IntPtr.Zero) Marshal.FreeCoTaskMem(Buffer);
    }
}

internal static unsafe class VirtualTrampolines
{
    internal static readonly IntPtr Notify =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&OnNotify;
    internal static readonly IntPtr Count =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, nuint>)&OnCount;
    internal static readonly IntPtr Size =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, nuint, double>)&OnSize;
    internal static readonly IntPtr Text =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, nuint, IntPtr>)&OnText;
    internal static readonly IntPtr Flag =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, nuint, int>)&OnFlag;
    internal static readonly IntPtr BuildItem =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, void*, nuint, void>)&OnBuildItem;
    internal static readonly IntPtr Activate =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, nuint, int, void>)&OnActivate;
    internal static readonly IntPtr Toggle =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, nuint, void>)&OnToggle;
    internal static readonly IntPtr SetChecked =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, nuint, int, void>)&OnSetChecked;
    internal static readonly IntPtr Depth =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, nuint, int>)&OnDepth;
    internal static readonly IntPtr TreeChildren =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, ulong, IntPtr, void*, void>)&OnTreeChildren;
    internal static readonly IntPtr TreeLabel =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, ulong, IntPtr>)&OnTreeLabel;
    internal static readonly IntPtr TreeFlag =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, ulong, int>)&OnTreeFlag;

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnNotify(void* user)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Action action) action();
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static nuint OnCount(void* user)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Func<nuint> f) return f();
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static double OnSize(void* user, nuint index)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Func<nuint, double> f)
                return f(index);
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
        return 0.0;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static IntPtr OnText(void* user, nuint index)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is TextScratch scratch &&
                scratch.ByIndex is not null)
                return scratch.Fill(scratch.ByIndex(index));
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
        return IntPtr.Zero;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int OnFlag(void* user, nuint index)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Func<nuint, bool> f)
                return f(index) ? 1 : 0;
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnBuildItem(void* user, void* view, nuint index)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Action<View, nuint> f)
                f(View.Borrowed((IntPtr)view), index);
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnActivate(void* user, nuint index, int mode)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Action<nuint, SelectMod> f)
                f(index, (SelectMod)mode);
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnToggle(void* user, nuint index)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Action<nuint> f) f(index);
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnSetChecked(void* user, nuint index, int isChecked)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Action<nuint, bool> f)
                f(index, isChecked != 0);
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int OnDepth(void* user, nuint index)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Func<nuint, int> f)
                return f(index);
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnTreeChildren(void* user, ulong parent, IntPtr emit, void* ctx)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is not TreeSource source) return;
            var children = source.Children(parent);
            if (emit == IntPtr.Zero || children is null) return;
            var emitFn = (delegate* unmanaged[Cdecl]<void*, ulong, void>)emit;
            foreach (var child in children) emitFn(ctx, child);
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static IntPtr OnTreeLabel(void* user, ulong handle)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is TreeSource source)
                return source.Scratch.Fill(source.Label(handle));
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
        return IntPtr.Zero;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int OnTreeFlag(void* user, ulong handle)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is TreeSource source)
                return source.HasChildren(handle) ? 1 : 0;
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
        return 0;
    }

    internal static IntPtr Pin(object target) =>
        GCHandle.ToIntPtr(GCHandle.Alloc(target));
}

/// <summary>
/// Replace / Ctrl-toggle / Shift-range selection with an INDEX anchor.
/// Suits flat lists whose indices are stable identities; trees should key
/// selection by HANDLE (see <see cref="TreeFlattener"/>).
/// </summary>
public sealed class IndexSelection : IDisposable
{
    internal IntPtr Handle { get; private set; }

    public IndexSelection()
    {
        AffineUIRuntime.EnsureLoaded();
        Handle = NativeMethods.affineui_index_selection_create();
    }

    public void Apply(nuint index, SelectMod mode, nuint itemCount) =>
        NativeMethods.affineui_index_selection_apply(Handle, index, (int)mode, itemCount);

    public bool Contains(nuint index) =>
        NativeMethods.affineui_index_selection_contains(Handle, index) != 0;

    public void Clear() => NativeMethods.affineui_index_selection_clear(Handle);

    public nuint Count => NativeMethods.affineui_index_selection_size(Handle);

    public nuint Anchor => NativeMethods.affineui_index_selection_anchor(Handle);

    public void OnChange(Action handler) =>
        NativeMethods.affineui_index_selection_on_change(
            Handle, VirtualTrampolines.Notify, VirtualTrampolines.Pin(handler),
            Trampolines.FreeUser);

    public void Dispose()
    {
        if (Handle == IntPtr.Zero) return;
        NativeMethods.affineui_index_selection_destroy(Handle);
        Handle = IntPtr.Zero;
        GC.SuppressFinalize(this);
    }

    ~IndexSelection() => Dispose();
}

/// <summary>A stateless bridge of callbacks for a recycling virtual LIST.</summary>
public sealed class VirtualListProvider : IDisposable
{
    internal IntPtr Handle { get; private set; }

    public VirtualListProvider()
    {
        AffineUIRuntime.EnsureLoaded();
        Handle = NativeMethods.affineui_vlist_provider_create();
    }

    public VirtualListProvider OnItemCount(Func<nuint> f)
    {
        NativeMethods.affineui_vlist_provider_on_item_count(
            Handle, VirtualTrampolines.Count, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualListProvider OnItemSize(Func<nuint, double> f)
    {
        NativeMethods.affineui_vlist_provider_on_item_size(
            Handle, VirtualTrampolines.Size, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualListProvider OnItemText(Func<nuint, string> f)
    {
        var scratch = new TextScratch { ByIndex = f };
        NativeMethods.affineui_vlist_provider_on_item_text(
            Handle, VirtualTrampolines.Text, VirtualTrampolines.Pin(scratch),
            Trampolines.FreeUser);
        return this;
    }

    public VirtualListProvider OnBuildItem(Action<View, nuint> f)
    {
        NativeMethods.affineui_vlist_provider_on_build_item(
            Handle, VirtualTrampolines.BuildItem, VirtualTrampolines.Pin(f),
            Trampolines.FreeUser);
        return this;
    }

    public VirtualListProvider OnIsSelected(Func<nuint, bool> f)
    {
        NativeMethods.affineui_vlist_provider_on_is_selected(
            Handle, VirtualTrampolines.Flag, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualListProvider OnActivate(Action<nuint, SelectMod> f)
    {
        NativeMethods.affineui_vlist_provider_on_activate(
            Handle, VirtualTrampolines.Activate, VirtualTrampolines.Pin(f),
            Trampolines.FreeUser);
        return this;
    }

    public VirtualListProvider OnIsChecked(Func<nuint, bool> f)
    {
        NativeMethods.affineui_vlist_provider_on_is_checked(
            Handle, VirtualTrampolines.Flag, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualListProvider OnSetChecked(Action<nuint, bool> f)
    {
        NativeMethods.affineui_vlist_provider_on_set_checked(
            Handle, VirtualTrampolines.SetChecked, VirtualTrampolines.Pin(f),
            Trampolines.FreeUser);
        return this;
    }

    /// <summary>Show the per-row checkbox column (checked is a second,
    /// selection-independent row state).</summary>
    public VirtualListProvider Checkboxes(bool on)
    {
        NativeMethods.affineui_vlist_provider_set_checkboxes(Handle, on ? 1 : 0);
        return this;
    }

    public VirtualListProvider DefaultItemSize(double px)
    {
        NativeMethods.affineui_vlist_provider_set_default_item_size(Handle, px);
        return this;
    }

    public void Dispose()
    {
        if (Handle == IntPtr.Zero) return;
        NativeMethods.affineui_vlist_provider_destroy(Handle);
        Handle = IntPtr.Zero;
        GC.SuppressFinalize(this);
    }

    ~VirtualListProvider() => Dispose();
}

/// <summary>A stateless bridge of callbacks for a recycling virtual TREE —
/// a virtual list over the flattened, currently-expanded rows.</summary>
public sealed class VirtualTreeProvider : IDisposable
{
    internal IntPtr Handle { get; private set; }

    public VirtualTreeProvider()
    {
        AffineUIRuntime.EnsureLoaded();
        Handle = NativeMethods.affineui_vtree_provider_create();
    }

    public VirtualTreeProvider OnItemCount(Func<nuint> f)
    {
        NativeMethods.affineui_vtree_provider_on_item_count(
            Handle, VirtualTrampolines.Count, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider OnItemSize(Func<nuint, double> f)
    {
        NativeMethods.affineui_vtree_provider_on_item_size(
            Handle, VirtualTrampolines.Size, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider OnItemText(Func<nuint, string> f)
    {
        var scratch = new TextScratch { ByIndex = f };
        NativeMethods.affineui_vtree_provider_on_item_text(
            Handle, VirtualTrampolines.Text, VirtualTrampolines.Pin(scratch),
            Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider OnBuildItem(Action<View, nuint> f)
    {
        NativeMethods.affineui_vtree_provider_on_build_item(
            Handle, VirtualTrampolines.BuildItem, VirtualTrampolines.Pin(f),
            Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider OnIsSelected(Func<nuint, bool> f)
    {
        NativeMethods.affineui_vtree_provider_on_is_selected(
            Handle, VirtualTrampolines.Flag, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider OnActivate(Action<nuint, SelectMod> f)
    {
        NativeMethods.affineui_vtree_provider_on_activate(
            Handle, VirtualTrampolines.Activate, VirtualTrampolines.Pin(f),
            Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider OnIsChecked(Func<nuint, bool> f)
    {
        NativeMethods.affineui_vtree_provider_on_is_checked(
            Handle, VirtualTrampolines.Flag, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider OnSetChecked(Action<nuint, bool> f)
    {
        NativeMethods.affineui_vtree_provider_on_set_checked(
            Handle, VirtualTrampolines.SetChecked, VirtualTrampolines.Pin(f),
            Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider OnDepth(Func<nuint, int> f)
    {
        NativeMethods.affineui_vtree_provider_on_depth(
            Handle, VirtualTrampolines.Depth, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider OnIsExpandable(Func<nuint, bool> f)
    {
        NativeMethods.affineui_vtree_provider_on_is_expandable(
            Handle, VirtualTrampolines.Flag, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider OnIsExpanded(Func<nuint, bool> f)
    {
        NativeMethods.affineui_vtree_provider_on_is_expanded(
            Handle, VirtualTrampolines.Flag, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider OnToggle(Action<nuint> f)
    {
        NativeMethods.affineui_vtree_provider_on_toggle(
            Handle, VirtualTrampolines.Toggle, VirtualTrampolines.Pin(f), Trampolines.FreeUser);
        return this;
    }

    public VirtualTreeProvider Checkboxes(bool on)
    {
        NativeMethods.affineui_vtree_provider_set_checkboxes(Handle, on ? 1 : 0);
        return this;
    }

    public VirtualTreeProvider DefaultItemSize(double px)
    {
        NativeMethods.affineui_vtree_provider_set_default_item_size(Handle, px);
        return this;
    }

    public void Dispose()
    {
        if (Handle == IntPtr.Zero) return;
        NativeMethods.affineui_vtree_provider_destroy(Handle);
        Handle = IntPtr.Zero;
        GC.SuppressFinalize(this);
    }

    ~VirtualTreeProvider() => Dispose();
}

/// <summary>
/// Describes an app tree as opaque <c>ulong</c> HANDLES. A handle must be
/// UNIQUE TO THE ITEM for its lifetime (id, stable pointer, map key) —
/// never recycled onto another item, and never 0 (reserved for "roots").
/// </summary>
public sealed class TreeSource
{
    /// <summary>Children of <paramref name="parent"/> (0 = roots), in order.</summary>
    public required Func<ulong, IReadOnlyList<ulong>> Children { get; init; }

    /// <summary>Row label for a handle.</summary>
    public required Func<ulong, string> Label { get; init; }

    /// <summary>Whether the handle can expand.</summary>
    public required Func<ulong, bool> HasChildren { get; init; }

    internal TextScratch Scratch { get; } = new();
}

/// <summary>
/// Flattens a handle-tree into the visible rows and owns expanded /
/// HANDLE-keyed selection / HANDLE-keyed checked state — all survive
/// expand/collapse renumbering. Wire it to a <see cref="VirtualTreeProvider"/>.
/// </summary>
public sealed class TreeFlattener : IDisposable
{
    public static readonly nuint IndexNone = unchecked((nuint)(-1));

    internal IntPtr Handle { get; private set; }

    public TreeFlattener(TreeSource source)
    {
        AffineUIRuntime.EnsureLoaded();
        Handle = NativeMethods.affineui_tree_flattener_create(
            VirtualTrampolines.TreeChildren, VirtualTrampolines.TreeLabel,
            VirtualTrampolines.TreeFlag, VirtualTrampolines.Pin(source),
            Trampolines.FreeUser);
    }

    /// <summary>Point <paramref name="provider"/> at this flattener (call once).</summary>
    public void Wire(VirtualTreeProvider provider) =>
        NativeMethods.affineui_tree_flattener_wire(Handle, provider.Handle);

    /// <summary>Re-flatten after the underlying tree STRUCTURE changed.</summary>
    public void Rebuild() => NativeMethods.affineui_tree_flattener_rebuild(Handle);

    public void OnChanged(Action handler) =>
        NativeMethods.affineui_tree_flattener_on_changed(
            Handle, VirtualTrampolines.Notify, VirtualTrampolines.Pin(handler),
            Trampolines.FreeUser);

    public void SetExpanded(ulong handle, bool open) =>
        NativeMethods.affineui_tree_flattener_set_expanded(Handle, handle, open ? 1 : 0);

    public bool IsExpanded(ulong handle) =>
        NativeMethods.affineui_tree_flattener_is_expanded(Handle, handle) != 0;

    public void SetSelected(ulong handle, bool on) =>
        NativeMethods.affineui_tree_flattener_set_selected(Handle, handle, on ? 1 : 0);

    public bool SelectedContains(ulong handle) =>
        NativeMethods.affineui_tree_flattener_selected_contains(Handle, handle) != 0;

    public void ClearSelection() =>
        NativeMethods.affineui_tree_flattener_clear_selection(Handle);

    public void SetChecked(ulong handle, bool on) =>
        NativeMethods.affineui_tree_flattener_set_checked(Handle, handle, on ? 1 : 0);

    public bool CheckedContains(ulong handle) =>
        NativeMethods.affineui_tree_flattener_checked_contains(Handle, handle) != 0;

    public nuint Count => NativeMethods.affineui_tree_flattener_size(Handle);

    public ulong HandleAt(nuint index) =>
        NativeMethods.affineui_tree_flattener_handle_at(Handle, index);

    /// <summary>The current flattened index of a handle, or
    /// <see cref="IndexNone"/> when it is not visible.</summary>
    public nuint IndexOf(ulong handle) =>
        NativeMethods.affineui_tree_flattener_index_of(Handle, handle);

    public void Dispose()
    {
        if (Handle == IntPtr.Zero) return;
        NativeMethods.affineui_tree_flattener_destroy(Handle);
        Handle = IntPtr.Zero;
        GC.SuppressFinalize(this);
    }

    ~TreeFlattener() => Dispose();
}

public partial class View
{
    /// <summary>A recycling virtual list driven by <paramref name="provider"/>.
    /// The provider is held weakly by the widget — keep it alive alongside
    /// the view.</summary>
    public Widget VirtualList(string key, VirtualListProvider provider,
                              Axis axis = Axis.Vertical, string classes = "") =>
        Wrap(NativeMethods.affineui_view_virtual_list(
            Handle, key, provider.Handle, (int)axis, classes));

    /// <summary>A recycling virtual tree driven by <paramref name="provider"/>.</summary>
    public Widget VirtualTree(string key, VirtualTreeProvider provider,
                              string classes = "") =>
        Wrap(NativeMethods.affineui_view_virtual_tree(
            Handle, key, provider.Handle, classes));

    /// <summary>Convenience: an ALL-VIRTUAL list of strings.
    /// <paramref name="selection"/> / <paramref name="checkedState"/>
    /// (optional) must outlive the view.</summary>
    public Widget VirtualStringList(string key, IReadOnlyList<string> items,
                                    double itemSize = 24.0,
                                    IndexSelection? selection = null,
                                    IndexSelection? checkedState = null,
                                    string classes = "")
    {
        var ptrs = new IntPtr[items.Count];
        try
        {
            for (int i = 0; i < items.Count; i++)
                ptrs[i] = Marshal.StringToCoTaskMemUTF8(items[i]);
            unsafe
            {
                fixed (IntPtr* p = ptrs)
                {
                    return Wrap(NativeMethods.affineui_view_virtual_string_list(
                        Handle, key, (IntPtr)p, (nuint)items.Count, itemSize,
                        selection?.Handle ?? IntPtr.Zero,
                        checkedState?.Handle ?? IntPtr.Zero, classes));
                }
            }
        }
        finally
        {
            foreach (var p in ptrs)
                if (p != IntPtr.Zero) Marshal.FreeCoTaskMem(p);
        }
    }
}

public partial class App
{
    /// <summary>Install a persistent view builder: the app retains a View,
    /// re-runs the builder on every rebuild, and reconciles only the diff.
    /// REQUIRED for recycling virtual lists to follow the scrollbar. Follow
    /// state changes with <see cref="Invalidate"/> (coalesced) or
    /// <see cref="RebuildView"/> (synchronous).</summary>
    public void SetView(Action<View> builder) =>
        NativeMethods.affineui_app_set_view(
            Handle, Trampolines.Build, VirtualTrampolines.Pin(builder),
            Trampolines.FreeUser);

    /// <summary>Re-run the installed SetView builder synchronously.</summary>
    public void RebuildView() => NativeMethods.affineui_app_rebuild_view(Handle);
}
