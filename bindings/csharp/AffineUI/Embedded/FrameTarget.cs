// Mode B per-frame render target views. AffineUI opens a pass into these,
// draws, ends + commits; the host presents.

namespace AffineUI.Embedded;

/// <summary>
/// Per-frame render-target description. Fill the fields for your backend
/// (or use the static factories); handles are opaque <see cref="IntPtr"/>s.
/// Mirrors <c>affineui_frame_target</c>.
/// </summary>
public struct FrameTarget
{
    /// <summary>Target size in pixels.</summary>
    public int Width, Height;

    /// <summary>Pixels per CSS point (1.0, 2.0, ...).</summary>
    public float DpiScale;

    /// <summary>Must match the target (>= 1).</summary>
    public int SampleCount;

    /// <summary>Clear to the clear color before drawing.</summary>
    public bool Clear;

    /// <summary>Optional sub-rect of the target, in pixels (all-zero = whole
    /// target).</summary>
    public int ViewportX, ViewportY, ViewportWidth, ViewportHeight;

    public IntPtr D3D11RenderView;          // ID3D11RenderTargetView*
    public IntPtr D3D11ResolveView;         // ID3D11RenderTargetView* (optional)
    public IntPtr D3D11DepthStencilView;    // ID3D11DepthStencilView*

    public IntPtr MetalCurrentDrawable;     // CAMetalDrawable
    public IntPtr MetalDepthStencilTexture; // MTLTexture (optional)
    public IntPtr MetalMsaaColorTexture;    // MTLTexture (optional)

    public IntPtr WgpuRenderView;           // WGPUTextureView
    public IntPtr WgpuResolveView;          // WGPUTextureView (optional)
    public IntPtr WgpuDepthStencilView;     // WGPUTextureView (optional)

    /// <summary>GL FBO (0 = default framebuffer).</summary>
    public uint GlFramebuffer;

    public static FrameTarget D3D11(IntPtr renderTargetView, IntPtr depthStencilView,
                                    int width, int height, float dpiScale = 1.0f,
                                    int sampleCount = 1, bool clear = true,
                                    IntPtr resolveView = default) => new()
    {
        Width = width,
        Height = height,
        DpiScale = dpiScale,
        SampleCount = sampleCount,
        Clear = clear,
        D3D11RenderView = renderTargetView,
        D3D11DepthStencilView = depthStencilView,
        D3D11ResolveView = resolveView,
    };

    public static FrameTarget Metal(IntPtr currentDrawable, int width, int height,
                                    float dpiScale = 1.0f, int sampleCount = 1,
                                    bool clear = true,
                                    IntPtr depthStencilTexture = default,
                                    IntPtr msaaColorTexture = default) => new()
    {
        Width = width,
        Height = height,
        DpiScale = dpiScale,
        SampleCount = sampleCount,
        Clear = clear,
        MetalCurrentDrawable = currentDrawable,
        MetalDepthStencilTexture = depthStencilTexture,
        MetalMsaaColorTexture = msaaColorTexture,
    };

    public static FrameTarget OpenGL(uint framebuffer, int width, int height,
                                     float dpiScale = 1.0f, int sampleCount = 1,
                                     bool clear = true) => new()
    {
        Width = width,
        Height = height,
        DpiScale = dpiScale,
        SampleCount = sampleCount,
        Clear = clear,
        GlFramebuffer = framebuffer,
    };

    public static FrameTarget WebGpu(IntPtr renderView, int width, int height,
                                     float dpiScale = 1.0f, int sampleCount = 1,
                                     bool clear = true,
                                     IntPtr resolveView = default,
                                     IntPtr depthStencilView = default) => new()
    {
        Width = width,
        Height = height,
        DpiScale = dpiScale,
        SampleCount = sampleCount,
        Clear = clear,
        WgpuRenderView = renderView,
        WgpuResolveView = resolveView,
        WgpuDepthStencilView = depthStencilView,
    };

    internal readonly NativeFrameTarget ToNative() => new()
    {
        Width = Width,
        Height = Height,
        DpiScale = DpiScale,
        SampleCount = SampleCount,
        Clear = Clear ? 1 : 0,
        ViewportX = ViewportX,
        ViewportY = ViewportY,
        ViewportW = ViewportWidth,
        ViewportH = ViewportHeight,
        D3D11RenderView = D3D11RenderView,
        D3D11ResolveView = D3D11ResolveView,
        D3D11DepthStencilView = D3D11DepthStencilView,
        MetalCurrentDrawable = MetalCurrentDrawable,
        MetalDepthStencilTexture = MetalDepthStencilTexture,
        MetalMsaaColorTexture = MetalMsaaColorTexture,
        WgpuRenderView = WgpuRenderView,
        WgpuResolveView = WgpuResolveView,
        WgpuDepthStencilView = WgpuDepthStencilView,
        GlFramebuffer = GlFramebuffer,
    };
}
