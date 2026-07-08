/*
 * stb_truetype + stb_image implementation TU — intentionally inert.
 *
 * NanoVG (external/nanovg/src/nanovg.c) bundles its own copies of
 * stb_truetype and stb_image and emits the implementation symbols
 * inside its TU. Defining the IMPLEMENTATION macros here as well
 * would cause duplicate-symbol link errors.
 *
 * Re-enable selectively if a future phase wants stb_image outside
 * the painter (e.g. for offline image decoding).
 */
#ifdef AFFINEUI_REENABLE_STB_IMPL
#    define STB_TRUETYPE_IMPLEMENTATION
#    define STB_IMAGE_IMPLEMENTATION
#    include "stb_truetype.h"
#    include "stb_image.h"
#endif
