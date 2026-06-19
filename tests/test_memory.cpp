// Central-allocator tests: proves AffineUI's heap traffic is routed through
// affineui::mem and that a Document frees everything it allocates — the
// cross-platform "no leaks / no zombies" gate (Windows/MSVC AddressSanitizer
// has no leak sanitizer, so these counters are how we enforce it there).

#include <doctest/doctest.h>

#if !defined(AFFINEUI_STUB_BUILD)

#    include "affineui/document.h"
#    include "affineui/memory.h"

namespace {
constexpr const char* kDocHtml =
    "<style>"
    "  .x { color: red; padding: 4px; font: 0/0 a; }"
    "  .y { display: flex; margin: 2px 3px; }"
    "</style>"
    "<div class='x'>hi <b class='y'>there</b> "
    "<span style='margin:2px'>s</span></div>";
}  // namespace

TEST_CASE("mem: lexbor allocations are routed through affineui::mem") {
    const auto before = affineui::mem::stats();
    affineui::Document doc;
    doc.set_html(kDocHtml);
    const auto after = affineui::mem::stats();
    // Parsing a document must have allocated through mem (lexbor hooks live).
    CHECK(after.total_allocs > before.total_allocs);
    CHECK(after.live_blocks > before.live_blocks);
}

TEST_CASE("mem: a Document frees ALL its allocations on destruction") {
    // Warm-up: the first Document installs the lexbor hooks and allocates any
    // one-time global state; measure the steady state after that so the gate is
    // about THIS document's blocks, not process-wide one-time setup.
    { affineui::Document warm; warm.set_html(kDocHtml); }

    const auto before = affineui::mem::stats();
    {
        affineui::Document doc;
        doc.set_html(kDocHtml);
        // a reparse must not leak the previous tree either
        doc.set_html(kDocHtml);
    }
    const auto after = affineui::mem::stats();

    // No block this scope allocated may outlive it (no leaks, no zombies).
    CHECK(after.live_blocks == before.live_blocks);
    CHECK(after.live_bytes == before.live_bytes);
    // Sanity: the scope really did exercise the allocator.
    CHECK(after.total_allocs > before.total_allocs);
    CHECK(after.total_frees > before.total_frees);
}

TEST_CASE("mem: counters are self-consistent") {
    const auto s = affineui::mem::stats();
    CHECK(s.total_allocs >= s.total_frees);
    CHECK(s.live_blocks == s.total_allocs - s.total_frees);
    // report_leaks() reflects the live-block count (and, under AFFINEUI_MEM_DEBUG,
    // logs each survivor). Non-fatal here — other tests may hold live documents.
    CHECK(affineui::mem::report_leaks() == s.live_blocks);
}

#endif  // !AFFINEUI_STUB_BUILD
