// Declarative docking — the DCC-style workspace: a center document pane with
// dockable panels around it, which the user can drag, tab, split, and tear off.
//
// A dock container resolves a FLAT set of panel declarations into a split-tree
// layout and emits the DOM, inserting splitters itself. Panels may be declared
// in any order — each carries a DockLocation naming its parent and side — so the
// result is deterministic.

using System.Runtime.InteropServices;

namespace AffineUI;

/// <summary>Which side of its parent a panel docks to.</summary>
public enum Dock
{
    Left = 0,
    Right = 1,
    Top = 2,
    Bottom = 3,
    /// <summary>Another tab of the parent's pane, rather than a new split.</summary>
    Tab = 4,
}

/// <summary>A panel's window state.</summary>
public enum DockState
{
    Docked = 0,
    /// <summary>Floating in its own window.</summary>
    Detached = 1,
    /// <summary>Torn off, and draggable back into the dock.</summary>
    Tearoff = 2,
}

/// <summary>The corner of the parent a floating panel is anchored to.</summary>
public enum DockCorner
{
    TopLeft = 0,
    TopRight = 1,
    BottomLeft = 2,
    BottomRight = 3,
}

/// <summary>
/// Everything about where a panel is (or starts).
///
/// <para>Build one with a factory (<see cref="Docked"/>, <see cref="AsTab"/>,
/// <see cref="Floating"/>, <see cref="TornOff"/>) and refine it with the
/// chainable setters — the same shape as the C++ and Python APIs.</para>
///
/// <para>Every field is genuinely optional. The C ABI cannot express that (C has
/// no nullable), so it uses a flat struct with has-flags; this type carries real
/// nullables and is marshalled down at the call. You never see a flag.</para>
/// </summary>
public sealed class DockLocation
{
    /// <summary>Docked side, or <see cref="Dock.Tab"/> to join the parent's tab strip.</summary>
    public Dock? Side { get; set; }

    /// <summary>Parent pane id — what <c>Document()</c> / <c>DockPanel()</c> returned.
    /// Null = the document pane.</summary>
    public string? Parent { get; set; }

    public DockState State { get; set; } = DockState.Docked;

    /// <summary>px flex-basis when docked.</summary>
    public int? Size { get; set; }

    /// <summary>Corner a float is anchored to.</summary>
    public DockCorner? Anchor { get; set; }

    /// <summary>Float position relative to the anchor.</summary>
    public (int X, int Y)? Offset { get; set; }

    /// <summary>Float size.</summary>
    public (int W, int H)? FloatSize { get; set; }

    /// <summary>Tearoff: id of the panel this one drags with.</summary>
    public string? DragWith { get; set; }

    /// <summary>Docked to a side of its parent.</summary>
    public static DockLocation Docked(Dock side) => new() { Side = side };

    /// <summary>Another tab of the parent's pane.</summary>
    public static DockLocation AsTab() => new() { Side = Dock.Tab };

    /// <summary>Floating, anchored to a corner of the parent.</summary>
    public static DockLocation Floating(DockCorner anchor, (int X, int Y) pos, (int W, int H) size) =>
        new()
        {
            State = DockState.Detached,
            Anchor = anchor,
            Offset = pos,
            FloatSize = size,
        };

    /// <summary>Like <see cref="Floating"/>, but draggable back into the dock.</summary>
    public static DockLocation TornOff(DockCorner anchor, (int X, int Y) pos, (int W, int H) size)
    {
        var l = Floating(anchor, pos, size);
        l.State = DockState.Tearoff;
        return l;
    }

    /// <summary>Parent this panel to another pane (by the id its builder returned).</summary>
    public DockLocation In(string parentPaneId) { Parent = parentPaneId; return this; }

    /// <summary>px flex-basis when docked.</summary>
    public DockLocation Sized(int px) { Size = px; return this; }

    /// <summary>The size a torn-off panel takes when it detaches.</summary>
    public DockLocation TearoutSize((int W, int H) size) { FloatSize = size; return this; }

    /// <summary>Tearoff: drag this panel along with another.</summary>
    public DockLocation DraggingWith(string panelId) { DragWith = panelId; return this; }

    /// <summary>
    /// Marshal into the flat C form. The UTF-8 buffers the struct points at are
    /// owned by the returned scope and must outlive the native call — hence the
    /// IDisposable rather than a bare struct.
    /// </summary>
    internal Scope ToNative() => new(this);

    internal readonly struct Scope : IDisposable
    {
        public readonly NativeMethods.affineui_dock_location Raw;
        private readonly IntPtr _parent;
        private readonly IntPtr _dragWith;

        public Scope(DockLocation l)
        {
            _parent = l.Parent is null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(l.Parent);
            _dragWith = l.DragWith is null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(l.DragWith);

            Raw = new NativeMethods.affineui_dock_location
            {
                HasSide = l.Side.HasValue ? 1 : 0,
                Side = (int)(l.Side ?? Dock.Left),
                Parent = _parent,
                State = (int)l.State,
                HasSize = l.Size.HasValue ? 1 : 0,
                Size = l.Size ?? 0,
                HasAnchor = l.Anchor.HasValue ? 1 : 0,
                Anchor = (int)(l.Anchor ?? DockCorner.TopLeft),
                HasOffset = l.Offset.HasValue ? 1 : 0,
                OffsetX = l.Offset?.X ?? 0,
                OffsetY = l.Offset?.Y ?? 0,
                HasFloatSize = l.FloatSize.HasValue ? 1 : 0,
                FloatW = l.FloatSize?.W ?? 0,
                FloatH = l.FloatSize?.H ?? 0,
                DragWith = _dragWith,
            };
        }

        public void Dispose()
        {
            if (_parent != IntPtr.Zero) Marshal.FreeCoTaskMem(_parent);
            if (_dragWith != IntPtr.Zero) Marshal.FreeCoTaskMem(_dragWith);
        }
    }
}

/// <summary>
/// A runtime placement override — where a panel ended up after the user dragged,
/// tabbed, or tore it off. Read these back with <c>Document.DockOverrides()</c>
/// to save a workspace, and return them from
/// <c>View.SetDockPlacementProvider</c> to restore one.
/// </summary>
public sealed class DockPlacement
{
    /// <summary>Torn off into a floating panel.</summary>
    public bool Floating { get; set; }

    /// <summary>Docked: target pane id (empty = the document pane).</summary>
    public string Parent { get; set; } = string.Empty;

    /// <summary>Docked: which side.</summary>
    public Dock Side { get; set; }

    /// <summary>Docked: px flex-basis (0 = default).</summary>
    public int Size { get; set; }

    /// <summary>Floating: rect in float-host px.</summary>
    public int X { get; set; }
    public int Y { get; set; }
    public int W { get; set; }
    public int H { get; set; }

    /// <summary>Null when the native side reports no override for this panel.</summary>
    internal static DockPlacement? FromNative(in NativeMethods.affineui_dock_placement raw)
    {
        if (raw.Present == 0) return null;
        return new DockPlacement
        {
            Floating = raw.Floating != 0,
            Parent = raw.Parent == IntPtr.Zero
                ? string.Empty
                : Marshal.PtrToStringUTF8(raw.Parent) ?? string.Empty,
            Side = (Dock)raw.Side,
            Size = raw.Size,
            X = raw.X,
            Y = raw.Y,
            W = raw.W,
            H = raw.H,
        };
    }
}
