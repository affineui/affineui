// In-process sampling profiler (AFFINEUI_SAMPLER=1) — the "which call
// path is actually hot" tool the stage timers can't be. A watchdog
// thread samples the UI thread at ~1 kHz with SuspendThread +
// GetThreadContext + RtlVirtualUnwind (the OS x64 unwinder: exact, no
// frame pointers required). Each sample keeps only the frames inside
// the host executable (RVA-keyed) and the aggregated top stacks are
// appended to affineui_profile.txt every 5 seconds, so a run can be
// killed at any time without losing data. Symbolize offline:
//   llvm-symbolizer --obj=<exe> <0x140000000 + RVA>
//
// Part of the tracing/instrumentation data plane (S0) —
// docs/TRACING_AND_PERFORMANCE_LOGGING.md.

#include "internal/diag.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

// ── Chrome-trace span emitter (AFFINEUI_TRACE_JSON=1) ──────────────────
// Portable. One complete ("ph":"X") event per closed span, appended to
// affineui_trace.json. chrome://tracing and Perfetto both accept a bare,
// unterminated event array, so a killed process still yields a valid
// trace.
namespace affineui::detail {
namespace {

long long now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

FILE* trace_file() {
    static FILE* f = [] {
        const char* e = std::getenv("AFFINEUI_TRACE_JSON");
        if (e == nullptr || e[0] == '\0' || e[0] == '0') {
            return static_cast<FILE*>(nullptr);
        }
        FILE* out = nullptr;
#if defined(_WIN32)
        (void) fopen_s(&out, "affineui_trace.json", "w");
#else
        out = std::fopen("affineui_trace.json", "w");
#endif
        if (out != nullptr) std::fputs("[\n", out);
        return out;
    }();
    return f;
}

std::mutex& trace_mutex() {
    static std::mutex m;
    return m;
}

}  // namespace

bool trace_json_enabled() { return trace_file() != nullptr; }

void trace_json_complete(const char* name, long long ts_us,
                         long long dur_us) {
    FILE* f = trace_file();
    if (f == nullptr) return;
    const std::lock_guard<std::mutex> lock(trace_mutex());
    std::fprintf(f,
                 "{\"name\":\"%s\",\"ph\":\"X\",\"ts\":%lld,"
                 "\"dur\":%lld,\"pid\":1,\"tid\":1},\n",
                 name, ts_us, dur_us);
    std::fflush(f);  // the process is usually killed, never exits cleanly
}

TraceSpan::TraceSpan(const char* name) : name_(name) {
    if (trace_json_enabled()) {
        on_ = true;
        t0_us_ = now_us();
    }
}

TraceSpan::~TraceSpan() {
    if (!on_) return;
    const long long t1 = now_us();
    trace_json_complete(name_, t0_us_, t1 - t0_us_);
}

}  // namespace affineui::detail

#if defined(_WIN32) && !defined(AFFINEUI_STUB_BUILD)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <utility>
#include <vector>

namespace affineui::detail {
namespace {

struct SampleKey {
    std::uint32_t rva[16];  // exe frames only, leaf-first
    int           n{0};
    bool operator<(const SampleKey& o) const {
        if (n != o.n) return n < o.n;
        return std::memcmp(rva, o.rva,
                           sizeof(std::uint32_t) *
                               static_cast<std::size_t>(n)) < 0;
    }
};

void sampler_thread(HANDLE ui_thread) {
    {
        // Announce immediately so a missing file means "thread never ran",
        // not "first dump interval never elapsed".
        FILE* f = nullptr;
        (void) fopen_s(&f, "affineui_profile.txt", "a");
        if (f != nullptr) {
            std::fputs("==== sampler started ====\n", f);
            std::fclose(f);
        }
    }
    const auto exe_base =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    // Image size from the PE optional header (bounds for "our frame?").
    std::uintptr_t exe_size = 0;
    {
        const auto* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            exe_base + static_cast<std::uintptr_t>(dos->e_lfanew));
        exe_size = nt->OptionalHeader.SizeOfImage;
    }

    std::map<SampleKey, std::uint64_t> counts;
    std::uint64_t total = 0;
    std::uint64_t in_exe = 0;
    auto last_dump = GetTickCount64();

    DWORD64 raw[64];
    std::uint64_t suspend_fail = 0;
    for (;;) {
        Sleep(1);

        const auto now = GetTickCount64();
        if (now - last_dump >= 5000) {
            last_dump = now;
            std::vector<std::pair<std::uint64_t, const SampleKey*>> top;
            top.reserve(counts.size());
            for (const auto& [k, c] : counts) top.push_back({c, &k});
            std::sort(top.begin(), top.end(),
                      [](const auto& a, const auto& b) {
                          return a.first > b.first;
                      });
            FILE* f = nullptr;
            (void) fopen_s(&f, "affineui_profile.txt", "a");
            if (f != nullptr) {
                std::fprintf(
                    f,
                    "==== dump: %llu samples, %llu in exe, %llu suspend "
                    "failures ====\n",
                    static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(in_exe),
                    static_cast<unsigned long long>(suspend_fail));
                const std::size_t show =
                    top.size() < 25 ? top.size() : 25;
                for (std::size_t i = 0; i < show; ++i) {
                    std::fprintf(f, "%6llu x ",
                                 static_cast<unsigned long long>(
                                     top[i].first));
                    for (int j = 0; j < top[i].second->n; ++j) {
                        std::fprintf(f, "0x%X ", top[i].second->rva[j]);
                    }
                    std::fprintf(f, "\n");
                }
                std::fclose(f);
            }
        }

        int raw_n = 0;
        if (SuspendThread(ui_thread) == static_cast<DWORD>(-1)) {
            ++suspend_fail;
            continue;
        }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(ui_thread, &ctx)) {
            // NO allocations between suspend and resume — the suspended
            // thread may hold the heap lock.
            while (raw_n < 64 && ctx.Rip != 0) {
                raw[raw_n++] = ctx.Rip;
                DWORD64 image_base = 0;
                auto* fe =
                    RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
                if (fe == nullptr) {
                    // Leaf function: return address sits at [Rsp].
                    if (ctx.Rsp == 0) break;
                    ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                    continue;
                }
                PVOID handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip,
                                 fe, &ctx, &handler_data, &establisher,
                                 nullptr);
            }
        }
        ResumeThread(ui_thread);

        ++total;
        SampleKey key;
        for (int i = 0; i < raw_n && key.n < 16; ++i) {
            const auto a = static_cast<std::uintptr_t>(raw[i]);
            if (a >= exe_base && a < exe_base + exe_size) {
                key.rva[key.n++] =
                    static_cast<std::uint32_t>(a - exe_base);
            }
        }
        if (key.n > 0) {
            ++in_exe;
            ++counts[key];
        }
    }
}

}  // namespace

void maybe_start_stack_sampler() {
    static bool started = false;
    if (started) return;
    const char* e = std::getenv("AFFINEUI_SAMPLER");
    if (e == nullptr || e[0] == '\0' || e[0] == '0') return;
    HANDLE ui_thread = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &ui_thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        return;
    }
    started = true;
    std::remove("affineui_profile.txt");  // fresh run, fresh dumps
    std::thread(sampler_thread, ui_thread).detach();
}

}  // namespace affineui::detail

#else

namespace affineui::detail {
void maybe_start_stack_sampler() {}
}  // namespace affineui::detail

#endif
