#pragma once

// Central allocation facility.
//
// All of AffineUI's heap traffic is routed through ONE allocator so that:
//   • a host can supply memory (embedding — see affineui::Allocator), and
//   • debug builds can track every block for leak / double-free /
//     use-after-free detection on EVERY platform — notably Windows/MSVC, where
//     AddressSanitizer ships without a leak sanitizer.
//
// Every allocation carries a small self-describing header, so free()/realloc()
// work without the caller passing a size and so the debug layer can record each
// block. lexbor's global allocator (the DOM + CSS arenas — the bulk of our
// traffic, and where the docking memory bugs lived) is routed here via
// lexbor_memory_setup(); other subsystems (Yoga, NanoVG, sokol, our own C++)
// follow.

#include <cstddef>
#include <cstdint>

#include "affineui/embed.h"  // affineui::Allocator

namespace affineui::mem {

/// Install the active allocator (the host's, or a debug wrapper). Passing
/// nullptr resets to the built-in aligned malloc/free. Returns the previously
/// installed allocator (nullptr = built-in). Install before any allocations
/// exist (e.g. before the first Document); blocks already allocated under one
/// allocator must be freed under the same one.
const Allocator* set_allocator(const Allocator* allocator);

/// The active allocator (nullptr = built-in malloc/free).
const Allocator* current_allocator();

/// Route lexbor's global allocator (lexbor_memory_setup) through mem. Idempotent
/// and called automatically before the first Document is created; exposed so a
/// host can install its allocator + hooks explicitly at startup.
void install_lexbor_hooks();

// ── raw allocation ───────────────────────────────────────────────────────
/// `align` must be a power of two. Returns nullptr on failure.
[[nodiscard]] void* allocate(std::size_t size,
                             std::size_t align = alignof(std::max_align_t));
/// Grow/shrink a block previously returned by allocate()/reallocate().
/// `p == nullptr` behaves like allocate(); preserves min(old, new) bytes.
[[nodiscard]] void* reallocate(void* p, std::size_t new_size,
                               std::size_t align = alignof(std::max_align_t));
/// Free a block from allocate()/reallocate(). nullptr is a no-op.
void deallocate(void* p);

// ── statistics (always compiled; cheap atomics) ──────────────────────────
struct Stats {
    std::size_t   live_bytes{};    ///< currently allocated payload bytes
    std::size_t   live_blocks{};   ///< currently allocated block count
    std::size_t   peak_bytes{};    ///< high-water mark of live_bytes
    std::uint64_t total_allocs{};  ///< lifetime allocate() count
    std::uint64_t total_frees{};   ///< lifetime deallocate() count
};
Stats stats();

/// Number of blocks still live (0 == nothing leaked). With AFFINEUI_MEM_DEBUG
/// it also writes a per-block report (size + tag + sequence) to the log hook;
/// without it, just returns the live count. Call at shutdown / after a Document
/// is destroyed to assert "all memory freed".
std::size_t report_leaks();

/// RAII allocation tag: allocations made while a Tag is on the (thread-local)
/// stack are attributed to `name` in the leak report. No-op without
/// AFFINEUI_MEM_DEBUG.
struct Tag {
    explicit Tag(const char* name) noexcept;
    ~Tag();
    Tag(const Tag&) = delete;
    Tag& operator=(const Tag&) = delete;
};

}  // namespace affineui::mem
