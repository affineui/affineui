#include "affineui/memory.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#if !defined(AFFINEUI_STUB_BUILD)
#    include "lexbor/core/base.h"  // lxb_status_t + lexbor_memory_setup
#endif

// AFFINEUI_MEM_DEBUG: when defined, every block is tracked on an intrusive list
// (so report_leaks() can enumerate survivors), alloc/free poison their payload
// (0xCD fresh, 0xDD freed) and double-free is detected via a header magic. Zero
// of this is compiled in release builds; the live/peak counters below are always
// on (cheap atomics).
//
// AFFINEUI_MEM_POISON: when defined, alloc/free memset the payload with
// 0xCD/0xDD. It's OFF under AddressSanitizer because our blanket memset
// blindly writes over regions that vendored pools (lexbor mraw, etc.)
// have marked poisoned via ASAN's own API — the memset then trips ASAN
// on regions we don't logically own. ASAN's use-after-free / heap-overflow
// detection is stronger than the 0xDD marker anyway, so dropping it under
// ASAN loses nothing.
#if defined(__has_feature)
#    if __has_feature(address_sanitizer)
#        define AFFINEUI_ASAN_ACTIVE 1
#    endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#    define AFFINEUI_ASAN_ACTIVE 1
#endif

#if defined(AFFINEUI_MEM_DEBUG) && !defined(AFFINEUI_ASAN_ACTIVE)
#    define AFFINEUI_MEM_POISON 1
#endif

namespace affineui::mem {
namespace {

// Round `n` up to a multiple of `a` (a is a power of two).
inline std::size_t align_up(std::size_t n, std::size_t a) {
    return (n + (a - 1)) & ~(a - 1);
}

constexpr std::uint32_t kLiveMagic = 0xA11F0CEDu;  // "ALLOCED"
constexpr std::uint32_t kFreedMagic = 0xDEADBEEFu;

struct BlockHeader {
    void*         base;   // start of the underlying allocation (what we free)
    std::size_t   size;   // requested payload size
    std::uint32_t magic;  // kLiveMagic while live, kFreedMagic after free
    std::uint32_t align;  // payload alignment
#if defined(AFFINEUI_MEM_DEBUG)
    BlockHeader*  prev;
    BlockHeader*  next;
    const char*   tag;
    std::uint64_t seq;
#endif
};

// The active host allocator (nullptr == built-in malloc/free).
const Allocator* g_alloc = nullptr;

// Always-on counters.
std::atomic<std::size_t>   g_live_bytes{0};
std::atomic<std::size_t>   g_live_blocks{0};
std::atomic<std::size_t>   g_peak_bytes{0};
std::atomic<std::uint64_t> g_total_allocs{0};
std::atomic<std::uint64_t> g_total_frees{0};

void bump_peak(std::size_t live) {
    std::size_t prev = g_peak_bytes.load(std::memory_order_relaxed);
    while (live > prev &&
           !g_peak_bytes.compare_exchange_weak(prev, live,
                                               std::memory_order_relaxed)) {
    }
}

// ── underlying block alloc/free (host allocator or malloc) ────────────────
void* raw_alloc(std::size_t total, std::size_t align) {
    if (g_alloc && g_alloc->alloc) {
        return g_alloc->alloc(total, align, g_alloc->user);
    }
    return std::malloc(total);
}
void raw_free(void* base) {
    if (g_alloc && g_alloc->free) {
        g_alloc->free(base, g_alloc->user);
        return;
    }
    std::free(base);
}

#if defined(AFFINEUI_MEM_DEBUG)
// Intrusive list of live blocks + the tag stack. A recursive mutex would be
// overkill; the allocator is not re-entrant under the lock.
std::atomic<bool>      g_dbg_lock{false};
BlockHeader*           g_live_head = nullptr;
std::atomic<std::uint64_t> g_seq{0};
thread_local const char*   tl_tags[64];
thread_local int           tl_tag_depth = 0;

struct SpinGuard {
    SpinGuard() {
        bool expected = false;
        while (!g_dbg_lock.compare_exchange_weak(expected, true,
                                                 std::memory_order_acquire)) {
            expected = false;
        }
    }
    ~SpinGuard() { g_dbg_lock.store(false, std::memory_order_release); }
};

void dbg_link(BlockHeader* h) {
    SpinGuard lk;
    h->seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    h->tag = tl_tag_depth > 0 ? tl_tags[tl_tag_depth - 1] : nullptr;
    h->prev = nullptr;
    h->next = g_live_head;
    if (g_live_head) g_live_head->prev = h;
    g_live_head = h;
}
void dbg_unlink(BlockHeader* h) {
    SpinGuard lk;
    if (h->prev) h->prev->next = h->next;
    else g_live_head = h->next;
    if (h->next) h->next->prev = h->prev;
}
#endif  // AFFINEUI_MEM_DEBUG

}  // namespace

const Allocator* set_allocator(const Allocator* allocator) {
    const Allocator* prev = g_alloc;
    g_alloc = allocator;
    return prev;
}
const Allocator* current_allocator() { return g_alloc; }

void* allocate(std::size_t size, std::size_t align) {
    if (align < alignof(BlockHeader)) align = alignof(BlockHeader);
    // power-of-two round-up (callers pass powers of two, but be defensive)
    std::size_t a = alignof(BlockHeader);
    while (a < align) a <<= 1;
    align = a;

    const std::size_t total = sizeof(BlockHeader) + align + size;
    void* base = raw_alloc(total, align);
    if (!base) return nullptr;

    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(base);
    const std::uintptr_t payload =
        align_up(raw + sizeof(BlockHeader), align);
    auto* h = reinterpret_cast<BlockHeader*>(payload - sizeof(BlockHeader));
    h->base = base;
    h->size = size;
    h->magic = kLiveMagic;
    h->align = static_cast<std::uint32_t>(align);

    g_total_allocs.fetch_add(1, std::memory_order_relaxed);
    g_live_blocks.fetch_add(1, std::memory_order_relaxed);
    const std::size_t live =
        g_live_bytes.fetch_add(size, std::memory_order_relaxed) + size;
    bump_peak(live);

#if defined(AFFINEUI_MEM_DEBUG)
    dbg_link(h);
#endif
#if defined(AFFINEUI_MEM_POISON)
    std::memset(reinterpret_cast<void*>(payload), 0xCD, size);
#endif
    return reinterpret_cast<void*>(payload);
}

void deallocate(void* p) {
    if (!p) return;
    auto* h = reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(p) -
                                             sizeof(BlockHeader));
    if (h->magic == kFreedMagic) {
#if defined(AFFINEUI_MEM_DEBUG)
        std::fprintf(stderr, "[affineui][mem] double free of %p\n", p);
#endif
        return;  // refuse to free twice rather than corrupt the heap
    }
    if (h->magic != kLiveMagic) {
        // Not one of ours (or corrupted header). Don't touch it.
        return;
    }

    g_total_frees.fetch_add(1, std::memory_order_relaxed);
    g_live_blocks.fetch_sub(1, std::memory_order_relaxed);
    g_live_bytes.fetch_sub(h->size, std::memory_order_relaxed);

#if defined(AFFINEUI_MEM_DEBUG)
    dbg_unlink(h);
#endif
#if defined(AFFINEUI_MEM_POISON)
    std::memset(p, 0xDD, h->size);  // poison payload to catch use-after-free
#endif
    h->magic = kFreedMagic;
    raw_free(h->base);
}

void* reallocate(void* p, std::size_t new_size, std::size_t align) {
    if (!p) return allocate(new_size, align);
    if (new_size == 0) {
        deallocate(p);
        return nullptr;
    }
    auto* h = reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(p) -
                                             sizeof(BlockHeader));
    if (h->magic != kLiveMagic) return nullptr;
    const std::size_t old_size = h->size;
    if (new_size <= old_size && h->align >= align) {
        return p;  // shrink/no-op in place (header keeps the original size band)
    }
    void* np = allocate(new_size, align);
    if (!np) return nullptr;
    std::memcpy(np, p, old_size < new_size ? old_size : new_size);
    deallocate(p);
    return np;
}

Stats stats() {
    Stats s;
    s.live_bytes = g_live_bytes.load(std::memory_order_relaxed);
    s.live_blocks = g_live_blocks.load(std::memory_order_relaxed);
    s.peak_bytes = g_peak_bytes.load(std::memory_order_relaxed);
    s.total_allocs = g_total_allocs.load(std::memory_order_relaxed);
    s.total_frees = g_total_frees.load(std::memory_order_relaxed);
    return s;
}

std::size_t report_leaks() {
    const std::size_t blocks = g_live_blocks.load(std::memory_order_relaxed);
#if defined(AFFINEUI_MEM_DEBUG)
    SpinGuard lk;
    std::size_t n = 0;
    for (BlockHeader* h = g_live_head; h; h = h->next, ++n) {
        std::fprintf(stderr,
                     "[affineui][mem] LEAK %zu bytes  tag=%s  seq=%llu\n",
                     h->size, h->tag ? h->tag : "(none)",
                     static_cast<unsigned long long>(h->seq));
    }
    if (n) {
        std::fprintf(stderr, "[affineui][mem] %zu live block(s), %zu bytes\n",
                     n, g_live_bytes.load(std::memory_order_relaxed));
    }
#endif
    return blocks;
}

Tag::Tag(const char* name) noexcept {
#if defined(AFFINEUI_MEM_DEBUG)
    if (tl_tag_depth < static_cast<int>(sizeof(tl_tags) / sizeof(tl_tags[0]))) {
        tl_tags[tl_tag_depth] = name;
    }
    ++tl_tag_depth;
#else
    (void) name;
#endif
}
Tag::~Tag() {
#if defined(AFFINEUI_MEM_DEBUG)
    if (tl_tag_depth > 0) --tl_tag_depth;
#endif
}

// ── lexbor wiring ─────────────────────────────────────────────────────────
#if !defined(AFFINEUI_STUB_BUILD)
namespace {
void* lx_malloc(std::size_t size) { return allocate(size); }
void* lx_realloc(void* dst, std::size_t size) { return reallocate(dst, size); }
void* lx_calloc(std::size_t num, std::size_t size) {
    const std::size_t total = num * size;
    void* p = allocate(total);
    if (p) std::memset(p, 0, total);
    return p;
}
void lx_free(void* dst) { deallocate(dst); }

std::atomic<bool> g_lexbor_installed{false};
}  // namespace

void install_lexbor_hooks() {
    bool expected = false;
    if (!g_lexbor_installed.compare_exchange_strong(expected, true)) return;
    (void) lexbor_memory_setup(lx_malloc, lx_realloc, lx_calloc, lx_free);
}
#else
void install_lexbor_hooks() {}
#endif

}  // namespace affineui::mem
