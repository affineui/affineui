/*
 * Single TU that defines sokol's implementation symbols.
 *
 * AffineUI renders through sokol_gfx (NanoVG-on-sokol_gfx backend, see
 * affineui_nanovg/src/nanovg_sokol.h) — no raw GL/D3D/Metal in our code.
 * This TU emits the implementations for sokol_app (window + input),
 * sokol_gfx (GPU abstraction), and sokol_glue (bridges the two into an
 * sg_environment / sg_swapchain).
 *
 * Backend (SOKOL_GLCORE / SOKOL_METAL / SOKOL_D3D11 / ...) comes in via
 * affineui_link_backend(); see cmake/BackendDetect.cmake.
 */
#if !defined(AFFINEUI_STUB_BUILD) && !defined(AFFINEUI_HOST_PROVIDES_SOKOL)

#    if defined(__APPLE__)
#        define _DARWIN_C_SOURCE
#    endif

#    define SOKOL_IMPL
     /*
      * AffineUI is a retained UI renderer, so stale framebuffer dimensions
      * during Win32's modal resize loop are immediately visible as frozen
      * layout. Enable the local sokol_app opt-in below so the swapchain is
      * resized before each resize-loop frame.
      */
#    define SOKOL_WIN32_LIVE_RESIZE
     /*
      * Keep the first visible frame from being an OS-cleared blank window.
      * Our local sokol_app patch delays the Win32 ShowWindow call until after
      * the first app frame has been rendered and presented.
      */
#    define SOKOL_AFFINEUI_DEFER_WIN32_SHOW
#    include "sokol_log.h"
#    include "sokol_gfx.h"
#    include "sokol_app.h"
#    include "sokol_glue.h"

#endif
