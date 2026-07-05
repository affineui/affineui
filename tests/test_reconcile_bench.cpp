// Reconcile-path benchmark: is the View builder fast + allocation-quiet
// enough to rebuild a full app view at display refresh (140Hz ≈ 7ms
// budget, target ≪1ms)?
//
// Measures, at "hardware synth face" scale (~900 nodes):
//   A. initial build of a persistent View
//   B. no-change rebuild into the same View (steady-state frame cost)
//   C. one-attribute-change rebuild (typical animation frame)
//   D. fresh-View-per-frame build (the current sample/dender pattern)
// with wall time and global heap alloc/free counts per scenario.
//
// Run:  affineui_tests.exe -tc="reconcile bench" -s
// (Numbers print via MESSAGE so they show with -s even on success.)

#include <doctest/doctest.h>

#include <affineui/view.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <string>

namespace {

std::atomic<long long> g_allocs{0};
std::atomic<long long> g_frees{0};
bool g_counting = false;

struct AllocCounterScope {
    AllocCounterScope() {
        g_allocs = 0;
        g_frees = 0;
        g_counting = true;
    }
    ~AllocCounterScope() { g_counting = false; }
};

}  // namespace

void* operator new(std::size_t n) {
    if (g_counting) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {
    if (g_counting) g_frees.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    if (g_counting) g_frees.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}

namespace {

using affineui::View;
using affineui::ViewTheme;

// A counting sink so we know exactly how many mutations a rebuild emits.
struct CountingSink final : affineui::ViewSink {
    int creates{0}, removes{0}, texts{0}, attrs{0}, attr_removes{0};
    void reset() { creates = removes = texts = attrs = attr_removes = 0; }
    void create_element(const affineui::WidgetNode&,
                        const affineui::WidgetNode*, std::size_t) override {
        ++creates;
    }
    void create_text(const affineui::WidgetNode&,
                     const affineui::WidgetNode*, std::size_t) override {
        ++creates;
    }
    void remove(const affineui::WidgetNode&) override { ++removes; }
    void set_text(const affineui::WidgetNode&, std::string_view) override {
        ++texts;
    }
    void set_attribute(const affineui::WidgetNode&, std::string_view,
                       std::string_view) override {
        ++attrs;
    }
    void remove_attribute(const affineui::WidgetNode&,
                          std::string_view) override {
        ++attr_removes;
    }
    int total() const {
        return creates + removes + texts + attrs + attr_removes;
    }
};

// Synth-face-shaped view: 6 modules × (title + silk + 3 knob-ish fields
// + 4 jack-ish stacks) + a sequencer strip with 8 steps + faders.
// `knob_angle` is the animated value; `frame` perturbs one module's
// knob when it changes.
void build_synth_view(View& v, double knob_angle) {
    for (int m = 0; m < 6; ++m) {
        const std::string mkey = "mod" + std::to_string(m);
        auto mod = v.element("div", "dcs-hw module", mkey);
        v.element_ref("div", "dcs-hw__label", "__t").text("MODULE");
        {
            auto silk = v.element("fieldset", "dcs-silk", "__silk");
            v.element_ref("legend", "dcs-silk__title", "__l").text("GROUP");
            for (int k = 0; k < 3; ++k) {
                const std::string kkey = "k" + std::to_string(k);
                auto field = v.element("div", "dcs-field", kkey);
                v.element_ref("span", "dcs-field__label", "__fl")
                    .text("CUTOFF");
                auto knob = v.element("div", "dcs-knob", "__knob");
                knob.attr("data-dcs-knob", "");
                knob.attr("data-value",
                          std::to_string(knob_angle + m * 3 + k));
                {
                    auto svg = v.element("svg", "dcs-knob__ring", "__ring");
                    svg.attr("viewBox", "0 0 24 24");
                    v.element_ref("path", {}, "__bg")
                        .attr("d", "M 4.5 19.5 A 10.5 10.5 0 1 1 19.5 19.5")
                        .attr("fill", "none")
                        .attr("stroke", "rgba(255,255,255,.08)");
                    v.element_ref("path", "dcs-knob__arc", "__arc")
                        .attr("d", "M 4.5 19.5 A 10.5 10.5 0 0 1 12 1.5")
                        .attr("fill", "none")
                        .attr("stroke", "var(--dcs-accent)");
                }
                v.element_ref("div", "dcs-knob__cap", "__cap");
                v.element_ref("div", "dcs-knob__indicator", "__ind")
                    .attr("style", "--angle:" +
                                       std::to_string(knob_angle) + "deg");
            }
        }
        {
            auto row = v.container("jack-row", "__jacks");
            for (int j = 0; j < 4; ++j) {
                const std::string jkey = "j" + std::to_string(j);
                auto jack = v.element("div", "dcs-jack", jkey);
                auto sock = v.element("div", "dcs-jack__socket", "__s");
                sock.attr("data-skeuo-jack", mkey + jkey);
                v.html("<svg viewBox=\"0 0 32 32\"><circle cx=\"16\" "
                       "cy=\"16\" r=\"11\"/></svg>",
                       "__art");
            }
        }
    }
    {
        auto seq = v.element("div", "dcs-hw seq", "seq");
        for (int s = 0; s < 8; ++s) {
            const std::string skey = "step" + std::to_string(s);
            v.element_ref("button", "dcs-litbtn", skey)
                .attr("data-aui-name", skey)
                .text(std::to_string(s + 1));
        }
        for (int f = 0; f < 8; ++f) {
            const std::string fkey = "fader" + std::to_string(f);
            auto fader = v.element("div", "dcs-fader", fkey);
            fader.attr("data-dcs-fader", "");
            fader.attr("data-value", "0.5");
            v.element_ref("div", "dcs-fader__track", "__t");
            v.element_ref("div", "dcs-fader__thumb", "__th");
        }
    }
}

int count_nodes(const affineui::WidgetNode& n) {
    int total = 1;
    for (const auto& c : n.children) total += count_nodes(c);
    return total;
}

struct BenchResult {
    double      us_per_iter{0.0};
    long long   allocs{0};
    long long   frees{0};
    int         sink_ops{0};
};

template <typename Fn>
BenchResult bench(int iters, CountingSink* sink, Fn&& fn) {
    using clock = std::chrono::steady_clock;
    // Warm-up.
    fn(0);
    if (sink) sink->reset();
    AllocCounterScope counter;
    const auto t0 = clock::now();
    for (int i = 1; i <= iters; ++i) fn(i);
    const auto t1 = clock::now();
    BenchResult r;
    r.us_per_iter =
        std::chrono::duration<double, std::micro>(t1 - t0).count() / iters;
    r.allocs = g_allocs.load() / iters;
    r.frees = g_frees.load() / iters;
    r.sink_ops = sink ? sink->total() / iters : 0;
    return r;
}

}  // namespace

TEST_CASE("reconcile bench") {
    constexpr int kIters = 200;
    CountingSink sink;

    // A: initial build (fresh view, sink attached).
    View persistent{ViewTheme::Decius};
    {
        persistent.begin(&sink);
        build_synth_view(persistent, 45.0);
        persistent.end();
    }
    const int nodes = count_nodes(persistent.root());
    MESSAGE("tree nodes: " << nodes
                           << "  (initial creates: " << sink.creates << ")");

    // B: no-change rebuild into the persistent view.
    sink.reset();
    const auto b = bench(kIters, &sink, [&](int) {
        persistent.begin(&sink);
        build_synth_view(persistent, 45.0);
        persistent.end();
    });
    MESSAGE("B no-change rebuild:   " << b.us_per_iter << " us/frame, "
            << b.allocs << " allocs, " << b.frees << " frees, "
            << b.sink_ops << " sink ops");

    // C: one animated value changes per frame (knob angle sweeps).
    sink.reset();
    const auto c = bench(kIters, &sink, [&](int i) {
        persistent.begin(&sink);
        build_synth_view(persistent, 45.0 + i);
        persistent.end();
    });
    MESSAGE("C 1-value rebuild:     " << c.us_per_iter << " us/frame, "
            << c.allocs << " allocs, " << c.frees << " frees, "
            << c.sink_ops << " sink ops");

    // D: fresh View each frame (the current sample/dender pattern),
    // including the HTML serialization load_view would then reparse.
    const auto d = bench(kIters, nullptr, [&](int i) {
        View fresh{ViewTheme::Decius};
        fresh.begin(static_cast<affineui::ViewSink*>(nullptr));
        build_synth_view(fresh, 45.0 + i);
        fresh.end();
        const auto html = fresh.to_html_document();
        DOCTEST_FAST_CHECK_GT(html.size(), 1000u);
    });
    MESSAGE("D fresh view + to_html: " << d.us_per_iter << " us/frame, "
            << d.allocs << " allocs, " << d.frees << " frees");

    // The steady-state rebuild must stay far inside a 140Hz frame and
    // emit zero mutations when nothing changed.
    CHECK(b.sink_ops == 0);
    CHECK(b.us_per_iter < 2000.0);
}
