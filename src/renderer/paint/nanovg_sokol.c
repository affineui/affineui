/*
 * NanoVG-on-sokol_gfx backend implementation TU.
 *
 * This compiles the affineui_nanovg fork's sokol_gfx render backend
 * (nanovg_sokol.h). It replaces the old native GL3 backend (nanovg_gl.h,
 * formerly built via nanovg_impl.c) so all GPU work goes through
 * sokol_gfx — no raw GL/D3D/Metal in AffineUI's own code.
 *
 * sokol_gfx.h is included for its *declarations* only here (SOKOL_IMPL is
 * defined in sokol_impl.c). The active backend (SOKOL_D3D11 etc.) is set
 * by affineui_link_backend() as a PUBLIC compile definition.
 */
#if !defined(AFFINEUI_STUB_BUILD) && !defined(AFFINEUI_HOST_PROVIDES_NANOVG)

#    include "sokol_gfx.h"
#    include "nanovg.h"

#    define NANOVG_SOKOL_IMPLEMENTATION
#    include "nanovg_sokol.h"

#endif
