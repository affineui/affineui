//
// nanovg_sokol.h — NanoVG render backend on top of sokol_gfx.
//
// This is part of "affineui_nanovg", a maintained fork of NanoVG
// (memononen/nanovg). It implements NanoVG's NVGparams render callback
// interface using sokol_gfx (https://github.com/floooh/sokol), so the
// same NanoVG drawing API runs on every sokol_gfx backend — D3D11,
// Metal, OpenGL core/ES, WebGPU, (experimental) Vulkan — with no raw
// GL/D3D/Metal in this file.
//
// Design notes vs. the original nanovg_gl.h:
//   * No TRIANGLE_FAN. D3D11/Metal/WebGPU don't support fans, so NanoVG
//     fill fans are drawn as indexed triangle lists via a static
//     fan-pattern index buffer + sg_draw_ex(base_vertex).
//   * Render state (stencil/blend/color-mask/primitive) is immutable in
//     sokol_gfx pipeline objects, so instead of toggling glStencil*/
//     glBlend* we select a pre-built pipeline from a small lazy cache
//     keyed by {variant, blend factors}.
//   * Textures bind through sg_view objects (sokol's modern binding
//     model); per-frame geometry is uploaded with sg_append_buffer so
//     multiple nvgBeginFrame/EndFrame passes per frame are safe.
//   * Shaders are hand-written HLSL (D3D11) and GLSL (GLCORE), selected
//     at compile time. sokol-shdc is intentionally not used — a single
//     shader program doesn't justify the toolchain.
//
// Usage (one TU defines the implementation):
//   #define NANOVG_SOKOL_IMPLEMENTATION
//   #include "sokol_gfx.h"      // with SOKOL_IMPL in exactly one TU
//   #include "nanovg.h"
//   #include "nanovg_sokol.h"
//
//   NVGcontext* vg = nvgCreateSokol(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
//   ...
//   nvgDeleteSokol(vg);
//
// sokol_gfx must be set up (sg_setup) with a depth-STENCIL attachment
// available in the passes NanoVG renders into; the fill algorithm needs
// a stencil buffer.
//
// Original NanoVG (c) 2009-2013 Mikko Mononen. zlib license (see LICENSE.txt).
//
#ifndef NANOVG_SOKOL_H
#define NANOVG_SOKOL_H

// sg_image / sg_sampler appear in the public API below, so sokol_gfx.h
// must be visible. The consumer selects the sokol backend (SOKOL_D3D11,
// SOKOL_GLCORE, ...) before including this header. nanovg.h is likewise
// expected to have been included first (for NVGcontext).
#include "sokol_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create flags (classic nanovg GL-backend semantics; the sokol backend is
// the only one we ship, so they live here).
enum NVGcreateFlags {
    NVG_ANTIALIAS       = 1<<0,
    NVG_STENCIL_STROKES = 1<<1,
    NVG_DEBUG           = 1<<2,
};
enum NVGimageFlagsGL {
    NVG_IMAGE_NODELETE  = 1<<16, // Do not delete the injected sg_image.
};

NVGcontext* nvgCreateSokol(int flags);
void        nvgDeleteSokol(NVGcontext* ctx);

// Inject an existing sg_image (+ optional sampler) as a NanoVG image.
// Pass a zero sg_sampler to use the backend's default linear/clamp sampler.
int      nvsgCreateImageFromHandle(NVGcontext* ctx, sg_image image, sg_sampler smp, int w, int h, int flags);
sg_image nvsgImageHandle(NVGcontext* ctx, int image);

#ifdef __cplusplus
}
#endif

#endif // NANOVG_SOKOL_H

// ───────────────────────────────────────────────────────────────────────────
#ifdef NANOVG_SOKOL_IMPLEMENTATION

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include "nanovg.h"

#ifndef NANOVG_SOKOL_MAX_PIPELINES
#define NANOVG_SOKOL_MAX_PIPELINES 64
#endif

// Shader uniform "type" values (classic nanovg NSVG_SHADER_* semantics).
enum SGNVGshaderType {
    NSVG_SHADER_FILLGRAD,
    NSVG_SHADER_FILLIMG,
    NSVG_SHADER_SIMPLE,
    NSVG_SHADER_IMG
};

enum SGNVGcallType {
    SGNVG_NONE = 0,
    SGNVG_FILL,
    SGNVG_CONVEXFILL,
    SGNVG_STROKE,
    SGNVG_TRIANGLES,
};

// Pipeline "variants" — each bakes a distinct fixed-function state combo.
// Cull is NONE for all (NanoVG's 2D shapes don't need backface culling,
// and the stencil fill pass *requires* both faces; using NONE everywhere
// removes face-winding pitfalls across APIs).
enum SGNVGpipVariant {
    SGNVG_PIP_CONVEX_TRI = 0, // no stencil, color RGBA, TRIANGLES indexed (fan fill)
    SGNVG_PIP_CONVEX_STRIP,   // no stencil, color RGBA, TRIANGLE_STRIP   (fringe / plain stroke)
    SGNVG_PIP_FILL_STENCIL,   // stencil write, color OFF, TRIANGLES indexed
    SGNVG_PIP_FILL_AA,        // stencil==0 keep, color RGBA, STRIP       (also stroke AA)
    SGNVG_PIP_FILL_COVER,     // stencil!=0 ->zero, color RGBA, STRIP
    SGNVG_PIP_STROKE_BASE,    // stencil==0 ->incr, color RGBA, STRIP
    SGNVG_PIP_STROKE_CLEAR,   // stencil always ->zero, color OFF, STRIP
    SGNVG_PIP_TRIANGLES,      // no stencil, color RGBA, TRIANGLES        (text/images)
    SGNVG_PIP_VARIANT_COUNT
};

typedef struct SGNVGblend {
    sg_blend_factor srcRGB, dstRGB, srcAlpha, dstAlpha;
} SGNVGblend;

typedef struct SGNVGtexture {
    int        id;
    sg_image   img;
    sg_view    view;     // texture view used in bindings
    sg_sampler smp;
    int        width, height;
    int        type;     // NVG_TEXTURE_RGBA / NVG_TEXTURE_ALPHA
    int        flags;
    int        ownsImg;  // 0 for injected images flagged NVG_IMAGE_NODELETE
    int        dirty;    // shadow changed; needs a (coalesced) sg_update_image at flush
    unsigned char* shadow; // CPU shadow for partial updates (sokol has no sub-image update)
} SGNVGtexture;

typedef struct SGNVGcall {
    int        type;
    int        image;
    int        pathOffset;
    int        pathCount;
    int        triangleOffset;
    int        triangleCount;
    int        uniformOffset;
    SGNVGblend blendFunc;
} SGNVGcall;

typedef struct SGNVGpath {
    int fillOffset, fillCount;
    int strokeOffset, strokeCount;
} SGNVGpath;

// Fragment uniforms — identical layout to nanovg_gl.h's non-UBO union so
// glnvg__convertPaint logic ports verbatim. 11 vec4s = 176 bytes.
#define NANOVG_SG_UNIFORMARRAY_SIZE 11
typedef struct SGNVGfragUniforms {
    union {
        struct {
            float scissorMat[12];
            float paintMat[12];
            struct NVGcolor innerCol;
            struct NVGcolor outerCol;
            float scissorExt[2];
            float scissorScale[2];
            float extent[2];
            float radius;
            float feather;
            float strokeMult;
            float strokeThr;
            float texType;
            float type;
        };
        float uniformArray[NANOVG_SG_UNIFORMARRAY_SIZE][4];
    };
} SGNVGfragUniforms;

typedef struct SGNVGpipKey {
    int        variant;
    SGNVGblend blend;
} SGNVGpipKey;

typedef struct SGNVGcontext {
    sg_shader   shader;
    sg_buffer   vertBuf;     // stream vertex buffer (sg_append each flush)
    int         vertBufCap;  // capacity in vertices
    size_t      vertBufUsed; // bytes appended into vertBuf this frame
    // Grown-out vertex buffers are retired into a small ring instead of
    // destroyed immediately, so draws already recorded against them this
    // frame keep their data alive. Growth doubles, so the ring only ever
    // holds a handful of buffers over an app's lifetime.
    sg_buffer   retiredVertBufs[4];
    int         retiredVertCursor;
    sg_sampler  defSampler;  // default linear/clamp sampler

    SGNVGtexture* textures;
    int           ntextures, ctextures, textureId;
    int           dummyTex;

    // Lazy pipeline cache.
    SGNVGpipKey  pipKeys[NANOVG_SOKOL_MAX_PIPELINES];
    sg_pipeline  pips[NANOVG_SOKOL_MAX_PIPELINES];
    int          npips;

    float view[2];
    int   flags;
    int   edgeAA;

    // Per-frame CPU buffers (same growth strategy as nanovg_gl.h).
    SGNVGcall*    calls;    int ccalls, ncalls;
    SGNVGpath*    paths;    int cpaths, npaths;
    NVGvertex*    verts;    int cverts, nverts;
    unsigned char* uniforms; int cuniforms, nuniforms;
    int           fragSize;

    // State for the current flush.
    int          appendByteOffset; // byte offset of this flush's verts in vertBuf
    int          curPip;           // index of currently-applied pipeline (-1 = none)
} SGNVGcontext;

static int sgnvg__maxi(int a, int b) { return a > b ? a : b; }

// ── Texture table ───────────────────────────────────────────────────────────
static SGNVGtexture* sgnvg__allocTexture(SGNVGcontext* sg) {
    SGNVGtexture* tex = NULL;
    for (int i = 0; i < sg->ntextures; i++) {
        if (sg->textures[i].id == 0) { tex = &sg->textures[i]; break; }
    }
    if (tex == NULL) {
        if (sg->ntextures + 1 > sg->ctextures) {
            int c = sgnvg__maxi(sg->ntextures + 1, 4) + sg->ctextures / 2;
            SGNVGtexture* t = (SGNVGtexture*)realloc(sg->textures, sizeof(SGNVGtexture) * c);
            if (t == NULL) return NULL;
            sg->textures = t;
            sg->ctextures = c;
        }
        tex = &sg->textures[sg->ntextures++];
    }
    memset(tex, 0, sizeof(*tex));
    tex->id = ++sg->textureId;
    return tex;
}

static SGNVGtexture* sgnvg__findTexture(SGNVGcontext* sg, int id) {
    for (int i = 0; i < sg->ntextures; i++)
        if (sg->textures[i].id == id) return &sg->textures[i];
    return NULL;
}

static void sgnvg__freeTextureResources(SGNVGtexture* tex) {
    if (tex->view.id != SG_INVALID_ID) sg_destroy_view(tex->view);
    if (tex->ownsImg && tex->img.id != SG_INVALID_ID) sg_destroy_image(tex->img);
    if (tex->smp.id != SG_INVALID_ID) sg_destroy_sampler(tex->smp);
    free(tex->shadow);
}

static int sgnvg__deleteTexture(SGNVGcontext* sg, int id) {
    for (int i = 0; i < sg->ntextures; i++) {
        if (sg->textures[i].id == id) {
            sgnvg__freeTextureResources(&sg->textures[i]);
            memset(&sg->textures[i], 0, sizeof(sg->textures[i]));
            return 1;
        }
    }
    return 0;
}

// ── Shaders ──────────────────────────────────────────────────────────────────
#if defined(SOKOL_D3D11) || defined(SOKOL_DUMMY_BACKEND)
static const char* sgnvg__vs_hlsl =
    "cbuffer vs_params : register(b0) { float2 viewSize; float2 _pad; };\n"
    "struct vs_in  { float2 vertex : POSITION; float2 tcoord : TEXCOORD0; };\n"
    "struct vs_out { float2 ftcoord : TEXCOORD0; float2 fpos : TEXCOORD1; float4 pos : SV_Position; };\n"
    "vs_out main(vs_in inp) {\n"
    "  vs_out o;\n"
    "  o.ftcoord = inp.tcoord;\n"
    "  o.fpos = inp.vertex;\n"
    "  o.pos = float4(2.0*inp.vertex.x/viewSize.x - 1.0, 1.0 - 2.0*inp.vertex.y/viewSize.y, 0, 1);\n"
    "  return o;\n"
    "}\n";
static const char* sgnvg__fs_hlsl =
    "cbuffer fs_params : register(b0) { float4 frag[11]; };\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct ps_in { float2 ftcoord : TEXCOORD0; float2 fpos : TEXCOORD1; };\n"
    "float sdroundrect(float2 pt, float2 ext, float rad) {\n"
    "  float2 ext2 = ext - float2(rad, rad);\n"
    "  float2 d = abs(pt) - ext2;\n"
    "  return min(max(d.x, d.y), 0.0) + length(max(d, float2(0,0))) - rad;\n"
    "}\n"
    "float3 xform(float2 p, float4 c0, float4 c1, float4 c2) {\n"
    "  return c0.xyz*p.x + c1.xyz*p.y + c2.xyz;\n" // mat3(cols)*vec3(p,1)
    "}\n"
    "float scissorMask(float2 p) {\n"
    "  float2 sc = abs(xform(p, frag[0], frag[1], frag[2]).xy) - frag[8].xy;\n"
    "  sc = float2(0.5,0.5) - sc * frag[8].zw;\n"
    "  return clamp(sc.x,0.0,1.0) * clamp(sc.y,0.0,1.0);\n"
    "}\n"
    "float glyphAlpha(float a, float4 paint) {\n"
    "  a = saturate(a);\n"
    "  float pa = max(paint.w, 0.0001);\n"
    "  float3 rgb = saturate(paint.xyz / pa);\n"
    "  float lum = dot(rgb, float3(0.2126, 0.7152, 0.0722));\n"
    "  float t = saturate((lum - 0.15) / 0.50);\n"
    "  t = t * t * (3.0 - 2.0 * t);\n"
    "  float dark = pow(a, 0.75);\n"
    "  float light = pow(a, 1.0/2.2);\n"
    "  return lerp(dark, light, t);\n"
    "}\n"
    "float4 main(ps_in inp) : SV_Target {\n"
    "  float strokeMult = frag[10].x; float strokeThr = frag[10].y;\n"
    "  int texType = (int)frag[10].z; int type = (int)frag[10].w;\n"
    "  float scissor = scissorMask(inp.fpos);\n"
    "  float strokeAlpha = min(1.0,(1.0-abs(inp.ftcoord.x*2.0-1.0))*strokeMult) * min(1.0,inp.ftcoord.y);\n"
    "  if (strokeAlpha < strokeThr) discard;\n"
    "  float4 result;\n"
    "  if (type == 0) {\n"               // gradient
    "    float2 pt = xform(inp.fpos, frag[3], frag[4], frag[5]).xy;\n"
    "    float d = clamp((sdroundrect(pt, frag[9].xy, frag[9].z) + frag[9].w*0.5)/frag[9].w, 0.0, 1.0);\n"
    "    float4 color = lerp(frag[6], frag[7], d);\n"
    "    result = color * (strokeAlpha * scissor);\n"
    "  } else if (type == 1) {\n"        // image
    "    float2 pt = xform(inp.fpos, frag[3], frag[4], frag[5]).xy / frag[9].xy;\n"
    "    float4 color = tex.Sample(smp, pt);\n"
    "    if (texType == 1) color = float4(color.xyz*color.w, color.w);\n"
    "    if (texType == 2) color = float4(color.x,color.x,color.x,color.x);\n"
    "    color *= frag[6];\n"
    "    result = color * (strokeAlpha * scissor);\n"
    "  } else if (type == 2) {\n"        // stencil simple
    "    result = float4(1,1,1,1);\n"
    "  } else {\n"                       // textured tris (text)
    "    float4 color = tex.Sample(smp, inp.ftcoord);\n"
    "    if (texType == 1) color = float4(color.xyz*color.w, color.w);\n"
    "    if (texType == 2) { float a = glyphAlpha(color.x, frag[6]); color = float4(a,a,a,a); }\n"
    "    color *= scissor;\n"
    "    result = color * frag[6];\n"
    "  }\n"
    "  return result;\n"
    "}\n";
#endif

#if defined(SOKOL_GLCORE)
static const char* sgnvg__vs_glsl =
    "#version 330\n"
    "uniform vec2 viewSize;\n"
    "layout(location=0) in vec2 vertex;\n"
    "layout(location=1) in vec2 tcoord;\n"
    "out vec2 ftcoord;\n"
    "out vec2 fpos;\n"
    "void main(void) {\n"
    "  ftcoord = tcoord;\n"
    "  fpos = vertex;\n"
    "  gl_Position = vec4(2.0*vertex.x/viewSize.x - 1.0, 1.0 - 2.0*vertex.y/viewSize.y, 0, 1);\n"
    "}\n";
static const char* sgnvg__fs_glsl =
    "#version 330\n"
    "uniform vec4 frag[11];\n"
    "uniform sampler2D tex;\n"
    "in vec2 ftcoord;\n"
    "in vec2 fpos;\n"
    "out vec4 outColor;\n"
    "#define scissorExt frag[8].xy\n"
    "#define scissorScale frag[8].zw\n"
    "#define extent frag[9].xy\n"
    "#define radius frag[9].z\n"
    "#define feather frag[9].w\n"
    "#define strokeMult frag[10].x\n"
    "#define strokeThr frag[10].y\n"
    "#define texType int(frag[10].z)\n"
    "#define type int(frag[10].w)\n"
    "vec3 xform(vec2 p, vec4 c0, vec4 c1, vec4 c2){ return c0.xyz*p.x + c1.xyz*p.y + c2.xyz; }\n"
    "float sdroundrect(vec2 pt, vec2 ext, float rad){\n"
    "  vec2 ext2 = ext - vec2(rad,rad); vec2 d = abs(pt)-ext2;\n"
    "  return min(max(d.x,d.y),0.0) + length(max(d,0.0)) - rad; }\n"
    "float scissorMask(vec2 p){\n"
    "  vec2 sc = abs(xform(p,frag[0],frag[1],frag[2]).xy) - scissorExt;\n"
    "  sc = vec2(0.5,0.5) - sc*scissorScale;\n"
    "  return clamp(sc.x,0.0,1.0)*clamp(sc.y,0.0,1.0); }\n"
    "float glyphAlpha(float a, vec4 paint){\n"
    "  a = clamp(a, 0.0, 1.0);\n"
    "  float pa = max(paint.w, 0.0001);\n"
    "  vec3 rgb = clamp(paint.xyz / pa, vec3(0.0), vec3(1.0));\n"
    "  float lum = dot(rgb, vec3(0.2126,0.7152,0.0722));\n"
    "  float t = clamp((lum - 0.15) / 0.50, 0.0, 1.0);\n"
    "  t = t * t * (3.0 - 2.0 * t);\n"
    "  float dark = pow(a, 0.75);\n"
    "  float light = pow(a, 1.0/2.2);\n"
    "  return mix(dark, light, t); }\n"
    "void main(void){\n"
    "  float scissor = scissorMask(fpos);\n"
    "  float strokeAlpha = min(1.0,(1.0-abs(ftcoord.x*2.0-1.0))*strokeMult)*min(1.0,ftcoord.y);\n"
    "  if (strokeAlpha < strokeThr) discard;\n"
    "  vec4 result;\n"
    "  if (type == 0) {\n"
    "    vec2 pt = xform(fpos,frag[3],frag[4],frag[5]).xy;\n"
    "    float d = clamp((sdroundrect(pt,extent,radius)+feather*0.5)/feather,0.0,1.0);\n"
    "    result = mix(frag[6],frag[7],d) * (strokeAlpha*scissor);\n"
    "  } else if (type == 1) {\n"
    "    vec2 pt = xform(fpos,frag[3],frag[4],frag[5]).xy / extent;\n"
    "    vec4 color = texture(tex, pt);\n"
    "    if (texType == 1) color = vec4(color.xyz*color.w, color.w);\n"
    "    if (texType == 2) color = vec4(color.x);\n"
    "    color *= frag[6];\n"
    "    result = color * (strokeAlpha*scissor);\n"
    "  } else if (type == 2) {\n"
    "    result = vec4(1,1,1,1);\n"
    "  } else {\n"
    "    vec4 color = texture(tex, ftcoord);\n"
    "    if (texType == 1) color = vec4(color.xyz*color.w, color.w);\n"
    "    if (texType == 2) color = vec4(glyphAlpha(color.x, frag[6]));\n"
    "    color *= scissor;\n"
    "    result = color * frag[6];\n"
    "  }\n"
    "  outColor = result;\n"
    "}\n";
#endif

#if defined(SOKOL_METAL)
static const char* sgnvg__vs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct vs_params { float2 viewSize; };\n"
    "struct vs_in { float2 vpos [[attribute(0)]]; float2 tcoord [[attribute(1)]]; };\n"
    "struct vs_out { float4 pos [[position]]; float2 ftcoord; float2 fpos; };\n"
    "vertex vs_out vs_main(vs_in inp [[stage_in]], constant vs_params& params [[buffer(0)]]) {\n"
    "  vs_out o;\n"
    "  o.ftcoord = inp.tcoord;\n"
    "  o.fpos = inp.vpos;\n"
    "  o.pos = float4(2.0*inp.vpos.x/params.viewSize.x - 1.0, 1.0 - 2.0*inp.vpos.y/params.viewSize.y, 0.0, 1.0);\n"
    "  return o;\n"
    "}\n";
static const char* sgnvg__fs_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct fs_params { float4 frag[11]; };\n"
    "struct fs_in { float4 pos [[position]]; float2 ftcoord; float2 fpos; };\n"
    "static float sdroundrect(float2 pt, float2 ext, float rad) {\n"
    "  float2 ext2 = ext - float2(rad, rad);\n"
    "  float2 d = abs(pt) - ext2;\n"
    "  return min(max(d.x, d.y), 0.0) + length(max(d, float2(0.0))) - rad;\n"
    "}\n"
    "static float3 xform(float2 p, float4 c0, float4 c1, float4 c2) {\n"
    "  return c0.xyz*p.x + c1.xyz*p.y + c2.xyz;\n"
    "}\n"
    "static float scissorMask(float2 p, constant fs_params& u) {\n"
    "  float2 sc = abs(xform(p, u.frag[0], u.frag[1], u.frag[2]).xy) - u.frag[8].xy;\n"
    "  sc = float2(0.5,0.5) - sc * u.frag[8].zw;\n"
    "  return clamp(sc.x,0.0,1.0) * clamp(sc.y,0.0,1.0);\n"
    "}\n"
    "static float glyphAlpha(float a, float4 paint) {\n"
    "  a = clamp(a, 0.0, 1.0);\n"
    "  float pa = max(paint.w, 0.0001);\n"
    "  float3 rgb = clamp(paint.xyz / pa, float3(0.0), float3(1.0));\n"
    "  float lum = dot(rgb, float3(0.2126, 0.7152, 0.0722));\n"
    "  float t = clamp((lum - 0.15) / 0.50, 0.0, 1.0);\n"
    "  t = t * t * (3.0 - 2.0 * t);\n"
    "  float dark = pow(a, 0.75);\n"
    "  float light = pow(a, 1.0/2.2);\n"
    "  return mix(dark, light, t);\n"
    "}\n"
    "fragment float4 fs_main(fs_in inp [[stage_in]], constant fs_params& u [[buffer(0)]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {\n"
    "  float strokeMult = u.frag[10].x; float strokeThr = u.frag[10].y;\n"
    "  int texType = int(u.frag[10].z); int type = int(u.frag[10].w);\n"
    "  float scissor = scissorMask(inp.fpos, u);\n"
    "  float strokeAlpha = min(1.0,(1.0-abs(inp.ftcoord.x*2.0-1.0))*strokeMult) * min(1.0,inp.ftcoord.y);\n"
    "  if (strokeAlpha < strokeThr) discard_fragment();\n"
    "  float4 result;\n"
    "  if (type == 0) {\n"                // gradient
    "    float2 pt = xform(inp.fpos, u.frag[3], u.frag[4], u.frag[5]).xy;\n"
    "    float d = clamp((sdroundrect(pt, u.frag[9].xy, u.frag[9].z) + u.frag[9].w*0.5)/u.frag[9].w, 0.0, 1.0);\n"
    "    float4 color = mix(u.frag[6], u.frag[7], d);\n"
    "    result = color * (strokeAlpha * scissor);\n"
    "  } else if (type == 1) {\n"         // image
    "    float2 pt = xform(inp.fpos, u.frag[3], u.frag[4], u.frag[5]).xy / u.frag[9].xy;\n"
    "    float4 color = tex.sample(smp, pt);\n"
    "    if (texType == 1) color = float4(color.xyz*color.w, color.w);\n"
    "    if (texType == 2) color = float4(color.x);\n"
    "    color *= u.frag[6];\n"
    "    result = color * (strokeAlpha * scissor);\n"
    "  } else if (type == 2) {\n"         // stencil simple
    "    result = float4(1.0);\n"
    "  } else {\n"                        // textured tris (text)
    "    float4 color = tex.sample(smp, inp.ftcoord);\n"
    "    if (texType == 1) color = float4(color.xyz*color.w, color.w);\n"
    "    if (texType == 2) color = float4(glyphAlpha(color.x, u.frag[6]));\n"
    "    color *= scissor;\n"
    "    result = color * u.frag[6];\n"
    "  }\n"
    "  return result;\n"
    "}\n";
#endif

static sg_shader sgnvg__makeShader(void) {
    sg_shader_desc d;
    memset(&d, 0, sizeof(d));

    // Uniform block 0: vertex viewSize (16 bytes: vec2 + pad).
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    d.uniform_blocks[0].size = 16;
    d.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[0].hlsl_register_b_n = 0;
    d.uniform_blocks[0].msl_buffer_n = 0;          // MSL vertex [[buffer(0)]]
    d.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_FLOAT2;
    d.uniform_blocks[0].glsl_uniforms[0].array_count = 1;
    d.uniform_blocks[0].glsl_uniforms[0].glsl_name = "viewSize";

    // Uniform block 1: fragment frag[11] (176 bytes).
    d.uniform_blocks[1].stage = SG_SHADERSTAGE_FRAGMENT;
    d.uniform_blocks[1].size = (uint32_t)(NANOVG_SG_UNIFORMARRAY_SIZE * 16);
    d.uniform_blocks[1].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[1].hlsl_register_b_n = 0;
    d.uniform_blocks[1].msl_buffer_n = 0;          // MSL fragment [[buffer(0)]]
    d.uniform_blocks[1].glsl_uniforms[0].type = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[1].glsl_uniforms[0].array_count = NANOVG_SG_UNIFORMARRAY_SIZE;
    d.uniform_blocks[1].glsl_uniforms[0].glsl_name = "frag";

    // Texture view 0 + sampler 0 (fragment).
    d.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d.views[0].texture.image_type = SG_IMAGETYPE_2D;
    d.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    d.views[0].texture.hlsl_register_t_n = 0;
    d.views[0].texture.msl_texture_n = 0;          // MSL [[texture(0)]]
    d.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    d.samplers[0].hlsl_register_s_n = 0;
    d.samplers[0].msl_sampler_n = 0;               // MSL [[sampler(0)]]
    d.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[0].view_slot = 0;
    d.texture_sampler_pairs[0].sampler_slot = 0;
    d.texture_sampler_pairs[0].glsl_name = "tex";

    // Vertex attributes.
    d.attrs[0].glsl_name = "vertex";
    d.attrs[0].hlsl_sem_name = "POSITION"; d.attrs[0].hlsl_sem_index = 0;
    d.attrs[1].glsl_name = "tcoord";
    d.attrs[1].hlsl_sem_name = "TEXCOORD"; d.attrs[1].hlsl_sem_index = 0;

#if defined(SOKOL_D3D11) || defined(SOKOL_DUMMY_BACKEND)
    d.vertex_func.source = sgnvg__vs_hlsl;
    d.vertex_func.d3d11_target = "vs_4_0";
    d.fragment_func.source = sgnvg__fs_hlsl;
    d.fragment_func.d3d11_target = "ps_4_0";
#elif defined(SOKOL_GLCORE)
    d.vertex_func.source = sgnvg__vs_glsl;
    d.fragment_func.source = sgnvg__fs_glsl;
#elif defined(SOKOL_METAL)
    d.vertex_func.source = sgnvg__vs_msl;
    d.vertex_func.entry = "vs_main";
    d.fragment_func.source = sgnvg__fs_msl;
    d.fragment_func.entry = "fs_main";
#else
#error "nanovg_sokol: unsupported sokol backend (add a shader variant)"
#endif
    d.label = "nanovg_sokol_shader";
    return sg_make_shader(&d);
}

// ── Pipeline cache ────────────────────────────────────────────────────────────
static void sgnvg__variantStencil(int variant, sg_pipeline_desc* p, bool* colorOff) {
    *colorOff = false;
    sg_stencil_state* s = &p->stencil;
    switch (variant) {
        case SGNVG_PIP_FILL_STENCIL:
            s->enabled = true; s->read_mask = 0xff; s->write_mask = 0xff; s->ref = 0;
            s->front.compare = SG_COMPAREFUNC_ALWAYS; s->front.fail_op = SG_STENCILOP_KEEP;
            s->front.depth_fail_op = SG_STENCILOP_KEEP; s->front.pass_op = SG_STENCILOP_INCR_WRAP;
            s->back.compare = SG_COMPAREFUNC_ALWAYS; s->back.fail_op = SG_STENCILOP_KEEP;
            s->back.depth_fail_op = SG_STENCILOP_KEEP; s->back.pass_op = SG_STENCILOP_DECR_WRAP;
            *colorOff = true;
            break;
        case SGNVG_PIP_FILL_AA:
            s->enabled = true; s->read_mask = 0xff; s->write_mask = 0xff; s->ref = 0;
            s->front.compare = s->back.compare = SG_COMPAREFUNC_EQUAL;
            s->front.fail_op = s->back.fail_op = SG_STENCILOP_KEEP;
            s->front.depth_fail_op = s->back.depth_fail_op = SG_STENCILOP_KEEP;
            s->front.pass_op = s->back.pass_op = SG_STENCILOP_KEEP;
            break;
        case SGNVG_PIP_FILL_COVER:
            s->enabled = true; s->read_mask = 0xff; s->write_mask = 0xff; s->ref = 0;
            s->front.compare = s->back.compare = SG_COMPAREFUNC_NOT_EQUAL;
            s->front.fail_op = s->back.fail_op = SG_STENCILOP_ZERO;
            s->front.depth_fail_op = s->back.depth_fail_op = SG_STENCILOP_ZERO;
            s->front.pass_op = s->back.pass_op = SG_STENCILOP_ZERO;
            break;
        case SGNVG_PIP_STROKE_BASE:
            s->enabled = true; s->read_mask = 0xff; s->write_mask = 0xff; s->ref = 0;
            s->front.compare = s->back.compare = SG_COMPAREFUNC_EQUAL;
            s->front.fail_op = s->back.fail_op = SG_STENCILOP_KEEP;
            s->front.depth_fail_op = s->back.depth_fail_op = SG_STENCILOP_KEEP;
            s->front.pass_op = s->back.pass_op = SG_STENCILOP_INCR_CLAMP;
            break;
        case SGNVG_PIP_STROKE_CLEAR:
            s->enabled = true; s->read_mask = 0xff; s->write_mask = 0xff; s->ref = 0;
            s->front.compare = s->back.compare = SG_COMPAREFUNC_ALWAYS;
            s->front.fail_op = s->back.fail_op = SG_STENCILOP_ZERO;
            s->front.depth_fail_op = s->back.depth_fail_op = SG_STENCILOP_ZERO;
            s->front.pass_op = s->back.pass_op = SG_STENCILOP_ZERO;
            *colorOff = true;
            break;
        default: // CONVEX_TRI, CONVEX_STRIP, TRIANGLES — no stencil
            s->enabled = false;
            break;
    }
}

static sg_primitive_type sgnvg__variantPrim(int variant) {
    switch (variant) {
        case SGNVG_PIP_CONVEX_TRI:
        case SGNVG_PIP_FILL_STENCIL:
        case SGNVG_PIP_TRIANGLES:
            return SG_PRIMITIVETYPE_TRIANGLES;
        default:
            return SG_PRIMITIVETYPE_TRIANGLE_STRIP;
    }
}

static sg_pipeline sgnvg__getPipeline(SGNVGcontext* sg, int variant, SGNVGblend blend) {
    for (int i = 0; i < sg->npips; i++) {
        if (sg->pipKeys[i].variant == variant &&
            sg->pipKeys[i].blend.srcRGB == blend.srcRGB &&
            sg->pipKeys[i].blend.dstRGB == blend.dstRGB &&
            sg->pipKeys[i].blend.srcAlpha == blend.srcAlpha &&
            sg->pipKeys[i].blend.dstAlpha == blend.dstAlpha) {
            return sg->pips[i];
        }
    }
    sg_pipeline_desc p;
    memset(&p, 0, sizeof(p));
    p.shader = sg->shader;
    p.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2; // vertex
    p.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2; // tcoord
    p.layout.buffers[0].stride = (int)sizeof(NVGvertex);
    p.primitive_type = sgnvg__variantPrim(variant);
    p.index_type = SG_INDEXTYPE_NONE; // fans are expanded to triangle lists on the CPU
    p.cull_mode = SG_CULLMODE_NONE;
    // NanoVG always renders into a depth-STENCIL pass (the fill algorithm
    // needs the stencil buffer); depth test itself is disabled.
    p.depth.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    p.depth.compare = SG_COMPAREFUNC_ALWAYS;
    p.depth.write_enabled = false;

    bool colorOff = false;
    sgnvg__variantStencil(variant, &p, &colorOff);

    p.color_count = 1;
    p.colors[0].write_mask = colorOff ? SG_COLORMASK_NONE : SG_COLORMASK_RGBA;
    p.colors[0].blend.enabled = true;
    p.colors[0].blend.src_factor_rgb = blend.srcRGB;
    p.colors[0].blend.dst_factor_rgb = blend.dstRGB;
    p.colors[0].blend.op_rgb = SG_BLENDOP_ADD;
    p.colors[0].blend.src_factor_alpha = blend.srcAlpha;
    p.colors[0].blend.dst_factor_alpha = blend.dstAlpha;
    p.colors[0].blend.op_alpha = SG_BLENDOP_ADD;
    p.label = "nanovg_sokol_pipeline";

    sg_pipeline pip = sg_make_pipeline(&p);
    if (sg->npips < NANOVG_SOKOL_MAX_PIPELINES) {
        sg->pipKeys[sg->npips].variant = variant;
        sg->pipKeys[sg->npips].blend = blend;
        sg->pips[sg->npips] = pip;
        sg->npips++;
    }
    return pip;
}

// ── Paint → uniforms (ported from nanovg_gl.h) ───────────────────────────────
static void sgnvg__xformToMat3x4(float* m3, float* t) {
    m3[0]=t[0]; m3[1]=t[1]; m3[2]=0; m3[3]=0;
    m3[4]=t[2]; m3[5]=t[3]; m3[6]=0; m3[7]=0;
    m3[8]=t[4]; m3[9]=t[5]; m3[10]=1; m3[11]=0;
}
static NVGcolor sgnvg__premulColor(NVGcolor c) { c.r*=c.a; c.g*=c.a; c.b*=c.a; return c; }

static int sgnvg__convertPaint(SGNVGcontext* sg, SGNVGfragUniforms* frag, NVGpaint* paint,
                               NVGscissor* scissor, float width, float fringe, float strokeThr) {
    SGNVGtexture* tex = NULL;
    float invxform[6];
    memset(frag, 0, sizeof(*frag));
    frag->innerCol = sgnvg__premulColor(paint->innerColor);
    frag->outerCol = sgnvg__premulColor(paint->outerColor);

    if (scissor->extent[0] < -0.5f || scissor->extent[1] < -0.5f) {
        memset(frag->scissorMat, 0, sizeof(frag->scissorMat));
        frag->scissorExt[0] = 1.0f; frag->scissorExt[1] = 1.0f;
        frag->scissorScale[0] = 1.0f; frag->scissorScale[1] = 1.0f;
    } else {
        nvgTransformInverse(invxform, scissor->xform);
        sgnvg__xformToMat3x4(frag->scissorMat, invxform);
        frag->scissorExt[0] = scissor->extent[0];
        frag->scissorExt[1] = scissor->extent[1];
        frag->scissorScale[0] = sqrtf(scissor->xform[0]*scissor->xform[0] + scissor->xform[2]*scissor->xform[2]) / fringe;
        frag->scissorScale[1] = sqrtf(scissor->xform[1]*scissor->xform[1] + scissor->xform[3]*scissor->xform[3]) / fringe;
    }

    memcpy(frag->extent, paint->extent, sizeof(frag->extent));
    frag->strokeMult = (width*0.5f + fringe*0.5f) / fringe;
    frag->strokeThr = strokeThr;

    if (paint->image != 0) {
        tex = sgnvg__findTexture(sg, paint->image);
        if (tex == NULL) return 0;
        if ((tex->flags & NVG_IMAGE_FLIPY) != 0) {
            float m1[6], m2[6];
            nvgTransformTranslate(m1, 0.0f, frag->extent[1] * 0.5f);
            nvgTransformMultiply(m1, paint->xform);
            nvgTransformScale(m2, 1.0f, -1.0f);
            nvgTransformMultiply(m2, m1);
            nvgTransformTranslate(m1, 0.0f, -frag->extent[1] * 0.5f);
            nvgTransformMultiply(m1, m2);
            nvgTransformInverse(invxform, m1);
        } else {
            nvgTransformInverse(invxform, paint->xform);
        }
        frag->type = (float)NSVG_SHADER_FILLIMG;
        if (tex->type == NVG_TEXTURE_RGBA)
            frag->texType = (tex->flags & NVG_IMAGE_PREMULTIPLIED) ? 0.0f : 1.0f;
        else
            frag->texType = 2.0f;
    } else {
        frag->type = (float)NSVG_SHADER_FILLGRAD;
        frag->radius = paint->radius;
        frag->feather = paint->feather;
        nvgTransformInverse(invxform, paint->xform);
    }
    sgnvg__xformToMat3x4(frag->paintMat, invxform);
    return 1;
}

static SGNVGfragUniforms* sgnvg__fragUniformPtr(SGNVGcontext* sg, int i) {
    return (SGNVGfragUniforms*)&sg->uniforms[i];
}

// ── Per-draw helpers ─────────────────────────────────────────────────────────
static void sgnvg__applyPipeline(SGNVGcontext* sg, int variant, SGNVGblend blend) {
    sg_pipeline pip = sgnvg__getPipeline(sg, variant, blend);
    sg_apply_pipeline(pip);
    // viewSize (vertex UB) must be re-applied after every pipeline switch.
    float vs[4] = { sg->view[0], sg->view[1], 0, 0 };
    sg_range r = { vs, sizeof(vs) };
    sg_apply_uniforms(0, &r);
    sg->curPip = variant;
}

static void sgnvg__applyFrag(SGNVGcontext* sg, int uniformOffset) {
    SGNVGfragUniforms* frag = sgnvg__fragUniformPtr(sg, uniformOffset);
    sg_range r = { frag->uniformArray, sizeof(float) * NANOVG_SG_UNIFORMARRAY_SIZE * 4 };
    sg_apply_uniforms(1, &r);
}

static void sgnvg__bind(SGNVGcontext* sg, int image) {
    SGNVGtexture* tex = NULL;
    if (image != 0) tex = sgnvg__findTexture(sg, image);
    if (tex == NULL) tex = sgnvg__findTexture(sg, sg->dummyTex);
    sg_bindings b; memset(&b, 0, sizeof(b));
    b.vertex_buffers[0] = sg->vertBuf;
    b.vertex_buffer_offsets[0] = sg->appendByteOffset;
    if (tex) {
        b.views[0] = tex->view;
        b.samplers[0] = tex->smp.id != SG_INVALID_ID ? tex->smp : sg->defSampler;
    }
    sg_apply_bindings(&b);
}

// ── NanoVG render callbacks ──────────────────────────────────────────────────
static void sgnvg__fill(SGNVGcontext* sg, SGNVGcall* call) {
    SGNVGpath* paths = &sg->paths[call->pathOffset];
    int npaths = call->pathCount;

    // 1) Stencil pass (winding into stencil, color off).
    sgnvg__applyPipeline(sg, SGNVG_PIP_FILL_STENCIL, call->blendFunc);
    sgnvg__applyFrag(sg, call->uniformOffset);             // simple stencil shader
    sgnvg__bind(sg, 0);
    for (int i = 0; i < npaths; i++)
        if (paths[i].fillCount > 0)
            sg_draw(paths[i].fillOffset, paths[i].fillCount, 1);

    // 2) Anti-aliased fringes (if enabled).
    if (sg->flags & NVG_ANTIALIAS) {
        sgnvg__applyPipeline(sg, SGNVG_PIP_FILL_AA, call->blendFunc);
        sgnvg__applyFrag(sg, call->uniformOffset + sg->fragSize);
        sgnvg__bind(sg, call->image);
        for (int i = 0; i < npaths; i++)
            if (paths[i].strokeCount > 0)
                sg_draw(paths[i].strokeOffset, paths[i].strokeCount, 1);
    }

    // 3) Cover (draw fill where stencil != 0, then zero it).
    sgnvg__applyPipeline(sg, SGNVG_PIP_FILL_COVER, call->blendFunc);
    sgnvg__applyFrag(sg, call->uniformOffset + sg->fragSize);
    sgnvg__bind(sg, call->image);
    sg_draw(call->triangleOffset, call->triangleCount, 1);
}

static void sgnvg__convexFill(SGNVGcontext* sg, SGNVGcall* call) {
    SGNVGpath* paths = &sg->paths[call->pathOffset];
    int npaths = call->pathCount;

    sgnvg__applyPipeline(sg, SGNVG_PIP_CONVEX_TRI, call->blendFunc);
    sgnvg__applyFrag(sg, call->uniformOffset);
    sgnvg__bind(sg, call->image);
    for (int i = 0; i < npaths; i++)
        if (paths[i].fillCount > 0)
            sg_draw(paths[i].fillOffset, paths[i].fillCount, 1);

    // Fringes (TRIANGLE_STRIP).
    int hasFringe = 0;
    for (int i = 0; i < npaths; i++) if (paths[i].strokeCount > 0) { hasFringe = 1; break; }
    if (hasFringe) {
        sgnvg__applyPipeline(sg, SGNVG_PIP_CONVEX_STRIP, call->blendFunc);
        sgnvg__applyFrag(sg, call->uniformOffset);
        sgnvg__bind(sg, call->image);
        for (int i = 0; i < npaths; i++)
            if (paths[i].strokeCount > 0)
                sg_draw(paths[i].strokeOffset, paths[i].strokeCount, 1);
    }
}

static void sgnvg__stroke(SGNVGcontext* sg, SGNVGcall* call) {
    SGNVGpath* paths = &sg->paths[call->pathOffset];
    int npaths = call->pathCount;

    if (sg->flags & NVG_STENCIL_STROKES) {
        // Base.
        sgnvg__applyPipeline(sg, SGNVG_PIP_STROKE_BASE, call->blendFunc);
        sgnvg__applyFrag(sg, call->uniformOffset + sg->fragSize);
        sgnvg__bind(sg, call->image);
        for (int i = 0; i < npaths; i++)
            sg_draw(paths[i].strokeOffset, paths[i].strokeCount, 1);
        // AA.
        sgnvg__applyPipeline(sg, SGNVG_PIP_FILL_AA, call->blendFunc);
        sgnvg__applyFrag(sg, call->uniformOffset);
        sgnvg__bind(sg, call->image);
        for (int i = 0; i < npaths; i++)
            sg_draw(paths[i].strokeOffset, paths[i].strokeCount, 1);
        // Clear stencil.
        sgnvg__applyPipeline(sg, SGNVG_PIP_STROKE_CLEAR, call->blendFunc);
        sgnvg__applyFrag(sg, call->uniformOffset);
        sgnvg__bind(sg, call->image);
        for (int i = 0; i < npaths; i++)
            sg_draw(paths[i].strokeOffset, paths[i].strokeCount, 1);
    } else {
        sgnvg__applyPipeline(sg, SGNVG_PIP_CONVEX_STRIP, call->blendFunc);
        sgnvg__applyFrag(sg, call->uniformOffset);
        sgnvg__bind(sg, call->image);
        for (int i = 0; i < npaths; i++)
            sg_draw(paths[i].strokeOffset, paths[i].strokeCount, 1);
    }
}

static void sgnvg__triangles(SGNVGcontext* sg, SGNVGcall* call) {
    sgnvg__applyPipeline(sg, SGNVG_PIP_TRIANGLES, call->blendFunc);
    sgnvg__applyFrag(sg, call->uniformOffset);
    sgnvg__bind(sg, call->image);
    sg_draw(call->triangleOffset, call->triangleCount, 1);
}

static sg_blend_factor sgnvg__blendFactor(int factor) {
    switch (factor) {
        case NVG_ZERO:                return SG_BLENDFACTOR_ZERO;
        case NVG_ONE:                 return SG_BLENDFACTOR_ONE;
        case NVG_SRC_COLOR:           return SG_BLENDFACTOR_SRC_COLOR;
        case NVG_ONE_MINUS_SRC_COLOR: return SG_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
        case NVG_DST_COLOR:           return SG_BLENDFACTOR_DST_COLOR;
        case NVG_ONE_MINUS_DST_COLOR: return SG_BLENDFACTOR_ONE_MINUS_DST_COLOR;
        case NVG_SRC_ALPHA:           return SG_BLENDFACTOR_SRC_ALPHA;
        case NVG_ONE_MINUS_SRC_ALPHA: return SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        case NVG_DST_ALPHA:           return SG_BLENDFACTOR_DST_ALPHA;
        case NVG_ONE_MINUS_DST_ALPHA: return SG_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
        case NVG_SRC_ALPHA_SATURATE:  return SG_BLENDFACTOR_SRC_ALPHA_SATURATED;
        default:                      return SG_BLENDFACTOR_ONE; // safe fallback
    }
}

static SGNVGblend sgnvg__blendComposite(NVGcompositeOperationState op) {
    SGNVGblend b;
    b.srcRGB = sgnvg__blendFactor(op.srcRGB);
    b.dstRGB = sgnvg__blendFactor(op.dstRGB);
    b.srcAlpha = sgnvg__blendFactor(op.srcAlpha);
    b.dstAlpha = sgnvg__blendFactor(op.dstAlpha);
    return b;
}

static void sgnvg__renderViewport(void* uptr, float width, float height, float devicePixelRatio) {
    (void)devicePixelRatio;
    SGNVGcontext* sg = (SGNVGcontext*)uptr;
    sg->view[0] = width;
    sg->view[1] = height;
    // New nvg frame: the stream buffer's append cursor restarts.
    sg->vertBufUsed = 0;
}

// Ensure the stream vertex buffer has room for `bytes` more this frame.
// sokol's sg_append_buffer silently drops EVERYTHING once a buffer
// overflows for the frame, so a too-small buffer means a fully blank
// frame. Grow by doubling; the old buffer is retired (not destroyed)
// because draws recorded earlier this frame still reference it.
static void sgnvg__ensureVertBufRoom(SGNVGcontext* sg, size_t bytes) {
    const size_t cap = (size_t)sg->vertBufCap * sizeof(NVGvertex);
    if (sg->vertBufUsed + bytes <= cap) return;

    int newCap = sg->vertBufCap;
    while ((size_t)newCap * sizeof(NVGvertex) < bytes) newCap *= 2;
    if (newCap == sg->vertBufCap) newCap *= 2;

    // Retire the old buffer into the ring (destroying the oldest entry).
    sg_buffer* slot = &sg->retiredVertBufs[sg->retiredVertCursor];
    sg->retiredVertCursor = (sg->retiredVertCursor + 1) %
        (int)(sizeof(sg->retiredVertBufs) / sizeof(sg->retiredVertBufs[0]));
    if (slot->id != SG_INVALID_ID) sg_destroy_buffer(*slot);
    *slot = sg->vertBuf;

    sg_buffer_desc vbd; memset(&vbd, 0, sizeof(vbd));
    vbd.size = (size_t)newCap * sizeof(NVGvertex);
    vbd.usage.vertex_buffer = true; vbd.usage.stream_update = true;
    vbd.label = "nanovg_sokol_verts";
    sg->vertBuf = sg_make_buffer(&vbd);
    sg->vertBufCap = newCap;
    sg->vertBufUsed = 0;
}

static void sgnvg__renderCancel(void* uptr) {
    SGNVGcontext* sg = (SGNVGcontext*)uptr;
    sg->nverts = 0; sg->npaths = 0; sg->ncalls = 0; sg->nuniforms = 0;
}

static void sgnvg__renderFlush(void* uptr) {
    SGNVGcontext* sg = (SGNVGcontext*)uptr;
    if (sg->ncalls > 0) {
        // Coalesced texture uploads. sokol allows only one sg_update_image
        // per image per frame, but NanoVG's font atlas can be touched many
        // times per frame (glyph additions). So create/update only write
        // the CPU shadow + mark dirty; here we do a single upload each,
        // before any draw samples them.
        for (int i = 0; i < sg->ntextures; i++) {
            SGNVGtexture* t = &sg->textures[i];
            if (t->id != 0 && t->dirty && t->shadow != NULL && t->img.id != SG_INVALID_ID) {
                int bpp = (t->type == NVG_TEXTURE_RGBA) ? 4 : 1;
                sg_image_data idata; memset(&idata, 0, sizeof(idata));
                idata.mip_levels[0].ptr = t->shadow;
                idata.mip_levels[0].size = (size_t)t->width * t->height * bpp;
                sg_update_image(t->img, &idata);
                t->dirty = 0;
            }
        }

        // Upload this frame's vertices (append → safe across multiple
        // flushes per frame; offset feeds bindings.vertex_buffer_offsets).
        sg_range vr = { sg->verts, (size_t)sg->nverts * sizeof(NVGvertex) };
        sgnvg__ensureVertBufRoom(sg, vr.size);
        sg->appendByteOffset = sg_append_buffer(sg->vertBuf, &vr);
        sg->vertBufUsed = (size_t)sg->appendByteOffset + vr.size;
        sg->curPip = -1;

        for (int i = 0; i < sg->ncalls; i++) {
            SGNVGcall* call = &sg->calls[i];
            switch (call->type) {
                case SGNVG_FILL:       sgnvg__fill(sg, call); break;
                case SGNVG_CONVEXFILL: sgnvg__convexFill(sg, call); break;
                case SGNVG_STROKE:     sgnvg__stroke(sg, call); break;
                case SGNVG_TRIANGLES:  sgnvg__triangles(sg, call); break;
                default: break;
            }
        }
    }
    sg->nverts = 0; sg->npaths = 0; sg->ncalls = 0; sg->nuniforms = 0;
}

// ── Per-frame CPU buffer growth (ported from nanovg_gl.h) ─────────────────────
static int sgnvg__maxVertCount(const NVGpath* paths, int npaths) {
    int count = 0;
    for (int i = 0; i < npaths; i++) { count += paths[i].nfill; count += paths[i].nstroke; }
    return count;
}
static SGNVGcall* sgnvg__allocCall(SGNVGcontext* sg) {
    if (sg->ncalls + 1 > sg->ccalls) {
        int c = sgnvg__maxi(sg->ncalls + 1, 128) + sg->ccalls / 2;
        SGNVGcall* calls = (SGNVGcall*)realloc(sg->calls, sizeof(SGNVGcall) * c);
        if (calls == NULL) return NULL;
        sg->calls = calls; sg->ccalls = c;
    }
    SGNVGcall* ret = &sg->calls[sg->ncalls++];
    memset(ret, 0, sizeof(SGNVGcall));
    return ret;
}
static int sgnvg__allocPaths(SGNVGcontext* sg, int n) {
    if (sg->npaths + n > sg->cpaths) {
        int c = sgnvg__maxi(sg->npaths + n, 128) + sg->cpaths / 2;
        SGNVGpath* paths = (SGNVGpath*)realloc(sg->paths, sizeof(SGNVGpath) * c);
        if (paths == NULL) return -1;
        sg->paths = paths; sg->cpaths = c;
    }
    int ret = sg->npaths; sg->npaths += n; return ret;
}
static int sgnvg__allocVerts(SGNVGcontext* sg, int n) {
    if (sg->nverts + n > sg->cverts) {
        int c = sgnvg__maxi(sg->nverts + n, 4096) + sg->cverts / 2;
        NVGvertex* verts = (NVGvertex*)realloc(sg->verts, sizeof(NVGvertex) * c);
        if (verts == NULL) return -1;
        sg->verts = verts; sg->cverts = c;
    }
    int ret = sg->nverts; sg->nverts += n; return ret;
}
static int sgnvg__allocFragUniforms(SGNVGcontext* sg, int n) {
    int structSize = sg->fragSize;
    if (sg->nuniforms + n > sg->cuniforms) {
        int c = sgnvg__maxi(sg->nuniforms + n, 128) + sg->cuniforms / 2;
        unsigned char* u = (unsigned char*)realloc(sg->uniforms, structSize * c);
        if (u == NULL) return -1;
        sg->uniforms = u; sg->cuniforms = c;
    }
    int ret = sg->nuniforms * structSize; sg->nuniforms += n; return ret;
}
static void sgnvg__vset(NVGvertex* v, float x, float y, float u, float w) {
    v->x = x; v->y = y; v->u = u; v->v = w;
}

// Expand a NanoVG triangle-FAN (v0,v1,v2,...) into a triangle LIST. sokol
// has no TRIANGLE_FAN primitive, so fills are stored as triangle lists.
// Returns the number of vertices written = (n-2)*3 for n>=3, else 0.
static int sgnvg__expandFan(NVGvertex* dst, const NVGvertex* fan, int n) {
    int o = 0;
    for (int k = 1; k + 1 < n; k++) {
        dst[o++] = fan[0];
        dst[o++] = fan[k];
        dst[o++] = fan[k + 1];
    }
    return o;
}

static void sgnvg__renderFill(void* uptr, NVGpaint* paint, NVGcompositeOperationState op,
                              NVGscissor* scissor, float fringe, const float* bounds,
                              const NVGpath* paths, int npaths) {
    SGNVGcontext* sg = (SGNVGcontext*)uptr;
    SGNVGcall* call = sgnvg__allocCall(sg);
    if (call == NULL) return;

    // Declared without initializers before the first `goto error` so the
    // amalgamated (C++) build is well-formed: C++ forbids a goto that
    // crosses an initialized scalar's scope, but permits crossing one
    // declared without an initializer.
    int maxverts;
    int offset;

    call->type = SGNVG_FILL;
    call->triangleCount = 4;
    call->pathOffset = sgnvg__allocPaths(sg, npaths);
    if (call->pathOffset == -1) goto error;
    call->pathCount = npaths;
    call->image = paint->image;
    call->blendFunc = sgnvg__blendComposite(op);

    if (npaths == 1 && paths[0].convex) {
        call->type = SGNVG_CONVEXFILL;
        call->triangleCount = 0;
    }

    // Worst-case vertex budget: each fill fan of n verts expands to (n-2)*3.
    maxverts = call->triangleCount;
    for (int i = 0; i < npaths; i++) {
        if (paths[i].nfill >= 3) maxverts += (paths[i].nfill - 2) * 3;
        maxverts += paths[i].nstroke;
    }
    offset = sgnvg__allocVerts(sg, maxverts);
    if (offset == -1) goto error;

    for (int i = 0; i < npaths; i++) {
        SGNVGpath* copy = &sg->paths[call->pathOffset + i];
        const NVGpath* path = &paths[i];
        memset(copy, 0, sizeof(SGNVGpath));
        if (path->nfill >= 3) {
            copy->fillOffset = offset;
            int n = sgnvg__expandFan(&sg->verts[offset], path->fill, path->nfill);
            copy->fillCount = n;
            offset += n;
        }
        if (path->nstroke > 0) {
            copy->strokeOffset = offset;
            copy->strokeCount = path->nstroke;
            memcpy(&sg->verts[offset], path->stroke, sizeof(NVGvertex) * path->nstroke);
            offset += path->nstroke;
        }
    }

    if (call->type == SGNVG_FILL) {
        call->triangleOffset = offset;
        NVGvertex* quad = &sg->verts[call->triangleOffset];
        sgnvg__vset(&quad[0], bounds[2], bounds[3], 0.5f, 1.0f);
        sgnvg__vset(&quad[1], bounds[2], bounds[1], 0.5f, 1.0f);
        sgnvg__vset(&quad[2], bounds[0], bounds[3], 0.5f, 1.0f);
        sgnvg__vset(&quad[3], bounds[0], bounds[1], 0.5f, 1.0f);

        call->uniformOffset = sgnvg__allocFragUniforms(sg, 2);
        if (call->uniformOffset == -1) goto error;
        SGNVGfragUniforms* frag = sgnvg__fragUniformPtr(sg, call->uniformOffset);
        memset(frag, 0, sizeof(*frag));
        frag->strokeThr = -1.0f;
        frag->type = (float)NSVG_SHADER_SIMPLE;
        sgnvg__convertPaint(sg, sgnvg__fragUniformPtr(sg, call->uniformOffset + sg->fragSize),
                            paint, scissor, fringe, fringe, -1.0f);
    } else {
        call->uniformOffset = sgnvg__allocFragUniforms(sg, 1);
        if (call->uniformOffset == -1) goto error;
        sgnvg__convertPaint(sg, sgnvg__fragUniformPtr(sg, call->uniformOffset),
                            paint, scissor, fringe, fringe, -1.0f);
    }
    return;
error:
    if (sg->ncalls > 0) sg->ncalls--;
}

static void sgnvg__renderStroke(void* uptr, NVGpaint* paint, NVGcompositeOperationState op,
                                NVGscissor* scissor, float fringe, float strokeWidth,
                                const NVGpath* paths, int npaths) {
    SGNVGcontext* sg = (SGNVGcontext*)uptr;
    SGNVGcall* call = sgnvg__allocCall(sg);
    if (call == NULL) return;

    // See sgnvg__renderFill: split decl/init so the goto is C++-legal.
    int maxverts;
    int offset;

    call->type = SGNVG_STROKE;
    call->pathOffset = sgnvg__allocPaths(sg, npaths);
    if (call->pathOffset == -1) goto error;
    call->pathCount = npaths;
    call->image = paint->image;
    call->blendFunc = sgnvg__blendComposite(op);

    maxverts = sgnvg__maxVertCount(paths, npaths);
    offset = sgnvg__allocVerts(sg, maxverts);
    if (offset == -1) goto error;

    for (int i = 0; i < npaths; i++) {
        SGNVGpath* copy = &sg->paths[call->pathOffset + i];
        const NVGpath* path = &paths[i];
        memset(copy, 0, sizeof(SGNVGpath));
        if (path->nstroke) {
            copy->strokeOffset = offset;
            copy->strokeCount = path->nstroke;
            memcpy(&sg->verts[offset], path->stroke, sizeof(NVGvertex) * path->nstroke);
            offset += path->nstroke;
        }
    }

    if (sg->flags & NVG_STENCIL_STROKES) {
        call->uniformOffset = sgnvg__allocFragUniforms(sg, 2);
        if (call->uniformOffset == -1) goto error;
        sgnvg__convertPaint(sg, sgnvg__fragUniformPtr(sg, call->uniformOffset),
                            paint, scissor, strokeWidth, fringe, -1.0f);
        sgnvg__convertPaint(sg, sgnvg__fragUniformPtr(sg, call->uniformOffset + sg->fragSize),
                            paint, scissor, strokeWidth, fringe, 1.0f - 0.5f/255.0f);
    } else {
        call->uniformOffset = sgnvg__allocFragUniforms(sg, 1);
        if (call->uniformOffset == -1) goto error;
        sgnvg__convertPaint(sg, sgnvg__fragUniformPtr(sg, call->uniformOffset),
                            paint, scissor, strokeWidth, fringe, -1.0f);
    }
    return;
error:
    if (sg->ncalls > 0) sg->ncalls--;
}

static void sgnvg__renderTriangles(void* uptr, NVGpaint* paint, NVGcompositeOperationState op,
                                   NVGscissor* scissor, const NVGvertex* verts, int nverts, float fringe) {
    SGNVGcontext* sg = (SGNVGcontext*)uptr;
    SGNVGcall* call = sgnvg__allocCall(sg);
    if (call == NULL) return;

    // See sgnvg__renderFill: split decl/init so the goto is C++-legal.
    SGNVGfragUniforms* frag;

    call->type = SGNVG_TRIANGLES;
    call->image = paint->image;
    call->blendFunc = sgnvg__blendComposite(op);

    call->triangleOffset = sgnvg__allocVerts(sg, nverts);
    if (call->triangleOffset == -1) goto error;
    call->triangleCount = nverts;
    memcpy(&sg->verts[call->triangleOffset], verts, sizeof(NVGvertex) * nverts);

    call->uniformOffset = sgnvg__allocFragUniforms(sg, 1);
    if (call->uniformOffset == -1) goto error;
    frag = sgnvg__fragUniformPtr(sg, call->uniformOffset);
    sgnvg__convertPaint(sg, frag, paint, scissor, 1.0f, fringe, -1.0f);
    frag->type = (float)NSVG_SHADER_IMG;
    return;
error:
    if (sg->ncalls > 0) sg->ncalls--;
}

// ── Texture callbacks ────────────────────────────────────────────────────────
static sg_sampler sgnvg__makeSampler(int imageFlags) {
    sg_sampler_desc sd; memset(&sd, 0, sizeof(sd));
    int nearest = imageFlags & NVG_IMAGE_NEAREST;
    sd.min_filter = nearest ? SG_FILTER_NEAREST : SG_FILTER_LINEAR;
    sd.mag_filter = nearest ? SG_FILTER_NEAREST : SG_FILTER_LINEAR;
    sd.mipmap_filter = (imageFlags & NVG_IMAGE_GENERATE_MIPMAPS)
                       ? (nearest ? SG_FILTER_NEAREST : SG_FILTER_LINEAR) : SG_FILTER_NEAREST;
    sd.wrap_u = (imageFlags & NVG_IMAGE_REPEATX) ? SG_WRAP_REPEAT : SG_WRAP_CLAMP_TO_EDGE;
    sd.wrap_v = (imageFlags & NVG_IMAGE_REPEATY) ? SG_WRAP_REPEAT : SG_WRAP_CLAMP_TO_EDGE;
    return sg_make_sampler(&sd);
}

static int sgnvg__bytesPerPixel(int type) { return type == NVG_TEXTURE_RGBA ? 4 : 1; }

static int sgnvg__renderCreateTexture(void* uptr, int type, int w, int h, int imageFlags, const unsigned char* data) {
    SGNVGcontext* sg = (SGNVGcontext*)uptr;
    SGNVGtexture* tex = sgnvg__allocTexture(sg);
    if (tex == NULL) return 0;

    tex->width = w; tex->height = h; tex->type = type; tex->flags = imageFlags; tex->ownsImg = 1;

    int bpp = sgnvg__bytesPerPixel(type);
    size_t bytes = (size_t)w * (size_t)h * (size_t)bpp;
    tex->shadow = (unsigned char*)malloc(bytes);
    if (data) memcpy(tex->shadow, data, bytes); else memset(tex->shadow, 0, bytes);

    sg_image_desc id; memset(&id, 0, sizeof(id));
    id.type = SG_IMAGETYPE_2D;
    id.width = w; id.height = h;
    id.num_mipmaps = 1;
    id.pixel_format = (type == NVG_TEXTURE_RGBA) ? SG_PIXELFORMAT_RGBA8 : SG_PIXELFORMAT_R8;
    id.usage.dynamic_update = true;   // NanoVG updates the font atlas over time
    id.label = "nanovg_sokol_texture";
    tex->img = sg_make_image(&id);

    // Defer the initial upload to the next flush (one sg_update_image per
    // image per frame; see sgnvg__renderFlush).
    tex->dirty = 1;

    sg_view_desc vd; memset(&vd, 0, sizeof(vd));
    vd.texture.image = tex->img;
    vd.label = "nanovg_sokol_texview";
    tex->view = sg_make_view(&vd);

    tex->smp = sgnvg__makeSampler(imageFlags);
    return tex->id;
}

static int sgnvg__renderDeleteTexture(void* uptr, int image) {
    return sgnvg__deleteTexture((SGNVGcontext*)uptr, image);
}

static int sgnvg__renderUpdateTexture(void* uptr, int image, int x, int y, int w, int h, const unsigned char* data) {
    SGNVGcontext* sg = (SGNVGcontext*)uptr;
    SGNVGtexture* tex = sgnvg__findTexture(sg, image);
    if (tex == NULL || tex->shadow == NULL) return 0;
    (void)x; (void)w;
    int bpp = sgnvg__bytesPerPixel(tex->type);
    // sokol can only replace the whole image, so patch the CPU shadow with
    // the dirty sub-rect (full rows) and re-upload the whole texture.
    // NanoVG passes the source pointer at the (0,y) origin already.
    for (int row = y; row < y + h; row++) {
        size_t dstOff = (size_t)row * tex->width * bpp;
        size_t srcOff = (size_t)row * tex->width * bpp;
        memcpy(tex->shadow + dstOff, data + srcOff, (size_t)tex->width * bpp);
    }
    // Coalesce: actual sg_update_image happens once per frame in flush.
    tex->dirty = 1;
    return 1;
}

static int sgnvg__renderGetTextureSize(void* uptr, int image, int* w, int* h) {
    SGNVGtexture* tex = sgnvg__findTexture((SGNVGcontext*)uptr, image);
    if (tex == NULL) return 0;
    *w = tex->width; *h = tex->height; return 1;
}

static int sgnvg__renderCreate(void* uptr) {
    SGNVGcontext* sg = (SGNVGcontext*)uptr;

    sg->shader = sgnvg__makeShader();
    sg->fragSize = (int)sizeof(SGNVGfragUniforms);

    // Stream vertex buffer (filled via sg_append_buffer each flush).
    sg->vertBufCap = 1 << 16; // 65536 verts; grows on overflow.
    sg_buffer_desc vbd; memset(&vbd, 0, sizeof(vbd));
    vbd.size = (size_t)sg->vertBufCap * sizeof(NVGvertex);
    vbd.usage.vertex_buffer = true; vbd.usage.stream_update = true;
    vbd.label = "nanovg_sokol_verts";
    sg->vertBuf = sg_make_buffer(&vbd);

    sg_sampler_desc dsd; memset(&dsd, 0, sizeof(dsd));
    dsd.min_filter = SG_FILTER_LINEAR; dsd.mag_filter = SG_FILTER_LINEAR;
    dsd.wrap_u = SG_WRAP_CLAMP_TO_EDGE; dsd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    sg->defSampler = sg_make_sampler(&dsd);

    // 1x1 dummy alpha texture for draws that don't sample an image.
    unsigned char zero = 0;
    sg->dummyTex = sgnvg__renderCreateTexture(sg, NVG_TEXTURE_ALPHA, 1, 1, 0, &zero);

    sg->curPip = -1;
    return 1;
}

static void sgnvg__renderDelete(void* uptr) {
    SGNVGcontext* sg = (SGNVGcontext*)uptr;
    if (sg == NULL) return;
    for (int i = 0; i < sg->npips; i++) sg_destroy_pipeline(sg->pips[i]);
    if (sg->shader.id != SG_INVALID_ID) sg_destroy_shader(sg->shader);
    if (sg->vertBuf.id != SG_INVALID_ID) sg_destroy_buffer(sg->vertBuf);
    for (size_t i = 0;
         i < sizeof(sg->retiredVertBufs) / sizeof(sg->retiredVertBufs[0]);
         i++) {
        if (sg->retiredVertBufs[i].id != SG_INVALID_ID)
            sg_destroy_buffer(sg->retiredVertBufs[i]);
    }
    if (sg->defSampler.id != SG_INVALID_ID) sg_destroy_sampler(sg->defSampler);
    for (int i = 0; i < sg->ntextures; i++)
        if (sg->textures[i].id != 0) sgnvg__freeTextureResources(&sg->textures[i]);
    free(sg->textures);
    free(sg->paths);
    free(sg->verts);
    free(sg->uniforms);
    free(sg->calls);
    free(sg);
}

// ── Public API ───────────────────────────────────────────────────────────────
NVGcontext* nvgCreateSokol(int flags) {
    NVGparams params;
    NVGcontext* ctx = NULL;
    SGNVGcontext* sg = (SGNVGcontext*)malloc(sizeof(SGNVGcontext));
    if (sg == NULL) goto error;
    memset(sg, 0, sizeof(SGNVGcontext));

    memset(&params, 0, sizeof(params));
    params.renderCreate        = sgnvg__renderCreate;
    params.renderCreateTexture = sgnvg__renderCreateTexture;
    params.renderDeleteTexture = sgnvg__renderDeleteTexture;
    params.renderUpdateTexture = sgnvg__renderUpdateTexture;
    params.renderGetTextureSize= sgnvg__renderGetTextureSize;
    params.renderViewport      = sgnvg__renderViewport;
    params.renderCancel        = sgnvg__renderCancel;
    params.renderFlush         = sgnvg__renderFlush;
    params.renderFill          = sgnvg__renderFill;
    params.renderStroke        = sgnvg__renderStroke;
    params.renderTriangles     = sgnvg__renderTriangles;
    params.renderDelete        = sgnvg__renderDelete;
    params.userPtr             = sg;
    params.edgeAntiAlias       = (flags & NVG_ANTIALIAS) ? 1 : 0;

    sg->flags = flags;
    sg->edgeAA = (flags & NVG_ANTIALIAS) ? 1 : 0;

    ctx = nvgCreateInternal(&params);
    if (ctx == NULL) goto error;
    return ctx;

error:
    if (ctx != NULL) nvgDeleteInternal(ctx);
    return NULL;
}

void nvgDeleteSokol(NVGcontext* ctx) {
    nvgDeleteInternal(ctx);
}

int nvsgCreateImageFromHandle(NVGcontext* ctx, sg_image image, sg_sampler smp, int w, int h, int flags) {
    SGNVGcontext* sg = (SGNVGcontext*)nvgInternalParams(ctx)->userPtr;
    SGNVGtexture* tex = sgnvg__allocTexture(sg);
    if (tex == NULL) return 0;
    tex->type = NVG_TEXTURE_RGBA;
    tex->img = image;
    tex->flags = flags;
    tex->width = w; tex->height = h;
    tex->ownsImg = (flags & NVG_IMAGE_NODELETE) ? 0 : 1;
    tex->smp = smp; // may be {0}; sgnvg__bind falls back to defSampler
    sg_view_desc vd; memset(&vd, 0, sizeof(vd));
    vd.texture.image = image;
    tex->view = sg_make_view(&vd);
    return tex->id;
}

sg_image nvsgImageHandle(NVGcontext* ctx, int image) {
    SGNVGcontext* sg = (SGNVGcontext*)nvgInternalParams(ctx)->userPtr;
    SGNVGtexture* tex = sgnvg__findTexture(sg, image);
    if (tex == NULL) { sg_image inv; inv.id = SG_INVALID_ID; return inv; }
    return tex->img;
}

#endif // NANOVG_SOKOL_IMPLEMENTATION
