// affinetools client (extras/tools): discovery, attach, live model, and
// dump summaries — exercised against the real in-process server. This is
// the C++ twin of what tools/affinetools/main.cpp does in its window.

#include "affineui/log.h"
#include "affineui/telemetry.h"
#include "affineui/tools.h"
#include "affineui_tools.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

using namespace affineui;

namespace {

/// Poll until `pred` holds or ~3 s pass.
template <typename Pred>
bool eventually(Pred pred) {
    for (int i = 0; i < 300; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

TEST_CASE("tools client: discover, attach, live telemetry, detach") {
    REQUIRE(tools_listen(0));
    const int self = TEST_GETPID();

    // Discovery sees this process.
    bool found = false;
    for (const tools::TargetInfo& t : tools::discover_targets()) {
        if (t.pid == self) {
            found = true;
            CHECK(t.port == tools_port());
            CHECK(t.token.size() == 32);
        }
    }
    REQUIRE(found);

    tools::Client client;
    REQUIRE(client.attach_pid(self));
    REQUIRE(client.attached());
    {
        const tools::ClientStatus s = client.status();
        CHECK(s.connected);
        CHECK(s.pid == self);
        CHECK(s.session_id.size() == 16);
        CHECK_FALSE(s.have_frame);
    }

    // Simulate the target app presenting a frame.
    FrameTelemetry t{};
    t.frame = 777;
    t.gap_ms = 7.25;
    t.fb_w = 800;
    t.fb_h = 600;
    t.layout_us = 321;
    t.mem_live_bytes = 1234567;
    tools::push_frame(t);
    REQUIRE(eventually([&] { return client.status().have_frame; }));
    {
        const tools::ClientStatus s = client.status();
        CHECK(s.last_frame.frame == 777);
        CHECK(s.last_frame.gap_ms == doctest::Approx(7.25));
        CHECK(s.last_frame.fb_w == 800);
        CHECK(s.last_frame.layout_us == 321);
        CHECK(s.last_frame.mem_live_bytes == 1234567);
        CHECK_FALSE(s.last_was_idle);
    }

    // Idle heartbeat updates the model too.
    tools::push_idle(5555.5, 42);
    REQUIRE(eventually([&] { return client.status().last_was_idle; }));
    CHECK(client.status().idle_skipped == 42);

    const std::uint64_t rev = client.revision();
    CHECK(rev >= 2);  // at least one bump per event

    // Log flow: a diagnostic emitted through the core facility must reach
    // the client's log ring, frame-stamped, over the wire. (The frame
    // source isn't installed in this headless test, so `frame` is 0 — the
    // point is that the whole log path carries level + text + frame.)
    affineui::log(affineui::LogLevel::warn, "test warning line");
    REQUIRE(eventually([&] {
        std::vector<tools::LogEntry> ls;
        client.log_history(ls, 64);
        return !ls.empty();
    }));
    {
        std::vector<tools::LogEntry> ls;
        client.log_history(ls, 64);
        REQUIRE_FALSE(ls.empty());
        const tools::LogEntry& last = ls.back();
        CHECK(last.text == "test warning line");
        CHECK(last.level == 2);  // warn
    }

    client.detach();
    CHECK_FALSE(client.attached());
    tools_shutdown();
}

TEST_CASE("tools client: dump summary math") {
    const std::string path = "affinetools_test_dump.jsonl";
    {
        std::ofstream f(path, std::ios::binary);
        f << R"({"v":1,"type":"session","schema":"telemetry/1","affineui":"9.9.9","platform":"test","pid":1,"t0_wall":"2026-07-04T00:00:00Z"})"
          << "\n";
        // gaps: 4, 6, 8, 10 → avg 7, max 10; cb: 1 each.
        for (const double gap : {4.0, 6.0, 8.0, 10.0}) {
            f << "{\"v\":1,\"type\":\"frame\",\"frame\":1,\"t_ms\":1.0,"
              << "\"gap_ms\":" << gap << ",\"cb_ms\":1.0,\"skipped\":0}"
              << "\n";
        }
        f << R"({"v":1,"type":"idle","t_ms":99.0,"skipped":12})" << "\n";
    }
    const tools::DumpSummary s = tools::summarize_dump(path);
    std::remove(path.c_str());

    REQUIRE(s.ok);
    CHECK(s.affineui_version == "9.9.9");
    CHECK(s.frames == 4);
    CHECK(s.idles == 1);
    CHECK(s.avg_gap_ms == doctest::Approx(7.0));
    CHECK(s.max_gap_ms == doctest::Approx(10.0));
    CHECK(s.avg_cb_ms == doctest::Approx(1.0));

    const tools::DumpSummary missing = tools::summarize_dump("no_such.jsonl");
    CHECK_FALSE(missing.ok);
}

TEST_CASE("tools client: compute_range_stats over a frame span") {
    std::vector<affineui::FrameTelemetry> h;
    auto mk = [](double gap_ms, double cb_ms, unsigned layout_us,
                 unsigned raster_us, unsigned comp_us) {
        affineui::FrameTelemetry t{};
        t.gap_ms = gap_ms;
        t.cb_ms = cb_ms;
        t.layout_us = layout_us;
        t.raster_us = raster_us;
        t.composite_us = comp_us;
        return t;
    };
    // 5 frames. gaps 4/6/8/10/12; budget 6.94 → 3 over (8,10,12).
    h.push_back(mk(4.0,  1.0, 500, 300, 200));   // stages 1.0ms, other 3.0
    h.push_back(mk(6.0,  1.5, 500, 300, 200));
    h.push_back(mk(8.0,  2.0, 1000, 1000, 1000)); // stages 3.0ms
    h.push_back(mk(10.0, 2.5, 1000, 1000, 1000));
    h.push_back(mk(12.0, 3.0, 1000, 1000, 1000));

    // Whole span.
    const tools::RangeStats all = tools::compute_range_stats(h, 0, h.size());
    CHECK(all.frames == 5);
    CHECK(all.avg_gap_ms == doctest::Approx(8.0));      // (4+6+8+10+12)/5
    CHECK(all.min_gap_ms == doctest::Approx(4.0));
    CHECK(all.max_gap_ms == doctest::Approx(12.0));
    CHECK(all.avg_fps == doctest::Approx(1000.0 / 8.0));
    CHECK(all.over_budget == 3);                        // 8,10,12 > 6.94
    // Per-component averages (ms):
    //   layout  (0.5+0.5+1+1+1)/5 = 0.8
    //   raster  (0.3+0.3+1+1+1)/5 = 0.72
    //   comp    (0.2+0.2+1+1+1)/5 = 0.68
    CHECK(all.avg_layout_ms == doctest::Approx(0.8));
    CHECK(all.avg_raster_ms == doctest::Approx(0.72));
    CHECK(all.avg_composite_ms == doctest::Approx(0.68));
    CHECK(all.avg_cb_ms == doctest::Approx(2.0));

    // Sub-range [2,5): the three over-budget frames only.
    const tools::RangeStats sub = tools::compute_range_stats(h, 2, 5);
    CHECK(sub.frames == 3);
    CHECK(sub.avg_gap_ms == doctest::Approx(10.0));     // (8+10+12)/3
    CHECK(sub.over_budget == 3);

    // A single frame (point selection shape).
    const tools::RangeStats one = tools::compute_range_stats(h, 4, 5);
    CHECK(one.frames == 1);
    CHECK(one.avg_gap_ms == doctest::Approx(12.0));
    CHECK(one.min_gap_ms == doctest::Approx(12.0));
    CHECK(one.max_gap_ms == doctest::Approx(12.0));

    // Empty / degenerate spans.
    CHECK(tools::compute_range_stats(h, 3, 3).frames == 0);
    CHECK(tools::compute_range_stats(h, 4, 99).frames == 1);  // end clamped
    CHECK(tools::compute_range_stats({}, 0, 10).frames == 0);
}
