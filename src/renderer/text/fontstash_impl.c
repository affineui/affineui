/*
 * fontstash implementation TU — intentionally inert.
 *
 * NanoVG (compiled in via external/nanovg/src/nanovg.c) bundles its
 * own copy of fontstash and emits the implementation symbols there.
 * Defining FONTSTASH_IMPLEMENTATION here as well would cause a
 * duplicate-symbol link error.
 *
 * This file exists so the build graph stays stable; future phases
 * that want fontstash *outside* of NanoVG (e.g. for an alternate
 * painter) can re-enable it.
 */
#ifdef AFFINEUI_REENABLE_FONTSTASH_IMPL
#    define FONTSTASH_IMPLEMENTATION
#    include "fontstash.h"
#endif
