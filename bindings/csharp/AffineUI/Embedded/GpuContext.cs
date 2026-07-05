// Mode B value types: the host's GPU objects, passed once at Init as opaque
// pointers (all-or-nothing ownership; see EMBEDDING_DESIGN.md).

namespace AffineUI.Embedded;

/// <summary>Concrete GPU backend. Must match the backend AffineUI was
/// compiled for. Mirrors <c>affineui_backend</c>.</summary>
public enum GpuBackend
{
    D3D11 = 0,
    Metal = 1,
    OpenGL = 2,
    WebGpu = 3,
}

/// <summary>Mirrors <c>affineui_pixel_format</c>.</summary>
public enum PixelFormat
{
    Default = 0,
    Rgba8 = 1,
    Bgra8 = 2,
    Depth = 3,
    DepthStencil = 4,
}

/// <summary>
/// Host graphics objects, supplied once at <see cref="Ui.Init"/>. All device
/// handles are opaque <see cref="IntPtr"/>s (e.g. <c>ID3D11Device*</c>); the
/// caller vouches they are live and match the compiled backend.
/// </summary>
public struct GpuContext
{
    public GpuBackend Backend;
    public IntPtr D3D11Device;          // ID3D11Device*
    public IntPtr D3D11DeviceContext;   // ID3D11DeviceContext*
    public IntPtr MetalDevice;          // id<MTLDevice>
    public IntPtr WgpuDevice;           // WGPUDevice
    public PixelFormat ColorFormat;
    public PixelFormat DepthFormat;
    /// <summary>MSAA sample count (>= 1).</summary>
    public int SampleCount;

    public static GpuContext D3D11(IntPtr device, IntPtr deviceContext,
                                   PixelFormat color = PixelFormat.Default,
                                   PixelFormat depth = PixelFormat.Default,
                                   int sampleCount = 1) => new()
    {
        Backend = GpuBackend.D3D11,
        D3D11Device = device,
        D3D11DeviceContext = deviceContext,
        ColorFormat = color,
        DepthFormat = depth,
        SampleCount = sampleCount,
    };

    public static GpuContext Metal(IntPtr device,
                                   PixelFormat color = PixelFormat.Default,
                                   PixelFormat depth = PixelFormat.Default,
                                   int sampleCount = 1) => new()
    {
        Backend = GpuBackend.Metal,
        MetalDevice = device,
        ColorFormat = color,
        DepthFormat = depth,
        SampleCount = sampleCount,
    };

    public static GpuContext OpenGL(PixelFormat color = PixelFormat.Default,
                                    PixelFormat depth = PixelFormat.Default,
                                    int sampleCount = 1) => new()
    {
        Backend = GpuBackend.OpenGL,
        ColorFormat = color,
        DepthFormat = depth,
        SampleCount = sampleCount,
    };

    public static GpuContext WebGpu(IntPtr device,
                                    PixelFormat color = PixelFormat.Default,
                                    PixelFormat depth = PixelFormat.Default,
                                    int sampleCount = 1) => new()
    {
        Backend = GpuBackend.WebGpu,
        WgpuDevice = device,
        ColorFormat = color,
        DepthFormat = depth,
        SampleCount = sampleCount,
    };

    internal readonly NativeGpuContext ToNative() => new()
    {
        Backend = (int)Backend,
        D3D11Device = D3D11Device,
        D3D11DeviceContext = D3D11DeviceContext,
        MetalDevice = MetalDevice,
        WgpuDevice = WgpuDevice,
        ColorFormat = (int)ColorFormat,
        DepthFormat = (int)DepthFormat,
        SampleCount = SampleCount,
    };
}

/// <summary>
/// Initialization descriptor for embedded mode. A complete <see cref="Gpu"/>
/// enables rendering against the host's device; leave it null for a
/// DOM/layout-only instance.
/// </summary>
public sealed class InitDesc
{
    public GpuContext? Gpu { get; set; }

    /// <summary>Default UI font family (null = AffineUI default).</summary>
    public string? DefaultFontFamily { get; set; }

    /// <summary>Default UI font size (0 = AffineUI default).</summary>
    public int DefaultFontSize { get; set; }

    /// <summary>Optional log sink. Held for the lifetime of the
    /// <see cref="Ui"/>; exceptions are routed to
    /// <see cref="AffineUIRuntime.OnCallbackException"/>.</summary>
    public Action<LogLevel, string>? Log { get; set; }
}
