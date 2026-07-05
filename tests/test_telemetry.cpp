// FrameTelemetry (data plane R0/R1): field mapping, per-frame deltas, and
// the JSONL record shapes. The string checks here are the enforcement arm
// of docs/AFFINETOOLS_PROTOCOL.md — if a key changes, that schema doc and
// kTelemetrySchemaVersion must move with it.

#include "affineui/renderer.h"
#include "affineui/telemetry.h"
#include "affineui/version.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

using namespace affineui;

namespace {

bool balanced_json_object(const std::string& s) {
    if (s.empty() || s.front() != '{' || s.back() != '}') return false;
    return std::count(s.begin(), s.end(), '{') ==
           std::count(s.begin(), s.end(), '}');
}

}  // namespace

TEST_CASE("telemetry: fill_from_render_stats maps the per-frame fields") {
    RenderStats rs{};
    rs.prepare_us_this_frame             = 10;
    rs.layout_us_this_frame              = 120;
    rs.display_list_record_us_this_frame = 80;
    rs.raster_us_this_frame              = 300;
    rs.composite_us_this_frame           = 90;
    rs.cached_ops                        = 412;
    rs.display_list_ops_culled_this_frame = 10;
    rs.display_list_diff_changed_ops     = 3;
    rs.dirty_rects                       = 2;
    rs.dirty_area_pct_x100               = 340;
    rs.recorded_this_frame               = true;
    rs.display_list_changed_this_frame   = true;
    rs.root_layer_rasterized_this_frame  = true;
    rs.root_layer_partial_this_frame     = false;
    rs.root_layer_direct_this_frame      = false;
    rs.root_layer_reused_this_frame      = true;
    rs.animations_active                 = false;

    FrameTelemetry t{};
    fill_from_render_stats(t, rs);

    CHECK(t.prep_us == 10);
    CHECK(t.layout_us == 120);
    CHECK(t.record_us == 80);
    CHECK(t.raster_us == 300);
    CHECK(t.composite_us == 90);
    CHECK(t.cached_ops == 412);
    CHECK(t.culled_ops == 10);
    CHECK(t.changed_ops == 3);
    CHECK(t.dirty_rects == 2);
    CHECK(t.dirty_pct_x100 == 340);
    CHECK(t.recorded);
    CHECK(t.dl_changed);
    CHECK(t.rasterized);
    CHECK_FALSE(t.partial);
    CHECK_FALSE(t.direct);
    CHECK(t.layer_reused);
    CHECK_FALSE(t.animations);
}

TEST_CASE("telemetry: fill_mem_delta reports live sizes and frame deltas") {
    mem::Stats prev{};
    prev.total_allocs = 1000;
    prev.total_frees  = 900;

    mem::Stats cur{};
    cur.live_bytes   = 9123456;
    cur.live_blocks  = 1284;
    cur.total_allocs = 1439;
    cur.total_frees  = 1320;

    FrameTelemetry t{};
    fill_mem_delta(t, prev, cur);

    CHECK(t.mem_live_bytes == 9123456);
    CHECK(t.mem_live_blocks == 1284);
    CHECK(t.allocs == 439);
    CHECK(t.frees == 420);
}

TEST_CASE("telemetry: frame JSONL matches the documented schema") {
    FrameTelemetry t{};
    t.frame          = 412;
    t.t_ms           = 6883.21;
    t.gap_ms         = 6.94;
    t.cb_ms          = 1.41;
    t.skipped        = 3;
    t.fb_w           = 1280;
    t.fb_h           = 720;
    t.dpi            = 1.25f;
    t.prep_us        = 10;
    t.layout_us      = 120;
    t.record_us      = 80;
    t.raster_us      = 300;
    t.composite_us   = 90;
    t.cached_ops     = 412;
    t.culled_ops     = 10;
    t.changed_ops    = 3;
    t.dirty_rects    = 2;
    t.dirty_pct_x100 = 340;
    t.recorded       = true;
    t.dl_changed     = true;
    t.rasterized     = true;
    t.layer_reused   = false;
    t.mem_live_bytes  = 9123456;
    t.mem_live_blocks = 1284;
    t.allocs          = 439;
    t.frees           = 420;

    char buf[512];
    const std::size_t len = format_frame_jsonl(t, buf, sizeof(buf));
    REQUIRE(len > 0);
    const std::string line(buf, len);

    CHECK(balanced_json_object(line));
    CHECK(line.find("\"v\":1") != std::string::npos);
    CHECK(line.find("\"type\":\"frame\"") != std::string::npos);
    CHECK(line.find("\"frame\":412") != std::string::npos);
    CHECK(line.find("\"t_ms\":6883.21") != std::string::npos);
    CHECK(line.find("\"gap_ms\":6.94") != std::string::npos);
    CHECK(line.find("\"cb_ms\":1.41") != std::string::npos);
    CHECK(line.find("\"skipped\":3") != std::string::npos);
    CHECK(line.find("\"fb\":[1280,720]") != std::string::npos);
    CHECK(line.find("\"dpi\":1.25") != std::string::npos);
    CHECK(line.find("\"stage_us\":{\"prep\":10,\"layout\":120,\"dl\":80,"
                    "\"rast\":300,\"comp\":90}") != std::string::npos);
    CHECK(line.find("\"ops\":{\"cached\":412,\"culled\":10,\"changed\":3,"
                    "\"rects\":2,\"dirty_pct\":3.40}") != std::string::npos);
    CHECK(line.find("\"flags\":{\"rec\":1,\"dl\":1,\"rast\":1,\"partial\":0,"
                    "\"direct\":0,\"reused\":0,\"anim\":0}") !=
          std::string::npos);
    CHECK(line.find("\"mem\":{\"live\":9123456,\"blocks\":1284,"
                    "\"allocs\":439,\"frees\":420}") != std::string::npos);
}

TEST_CASE("telemetry: idle JSONL heartbeat shape") {
    char buf[128];
    const std::size_t len = format_idle_jsonl(9120.5, 978, buf, sizeof(buf));
    REQUIRE(len > 0);
    const std::string line(buf, len);

    CHECK(balanced_json_object(line));
    CHECK(line.find("\"type\":\"idle\"") != std::string::npos);
    CHECK(line.find("\"t_ms\":9120.50") != std::string::npos);
    CHECK(line.find("\"skipped\":978") != std::string::npos);
}

TEST_CASE("telemetry: session preamble carries version and platform") {
    char buf[512];
    const std::size_t len = format_session_jsonl(buf, sizeof(buf));
    REQUIRE(len > 0);
    const std::string line(buf, len);

    CHECK(balanced_json_object(line));
    CHECK(line.find("\"type\":\"session\"") != std::string::npos);
    CHECK(line.find("\"schema\":\"telemetry/1\"") != std::string::npos);
    CHECK(line.find("\"affineui\":\"" AFFINEUI_VERSION_STRING "\"") !=
          std::string::npos);
    CHECK(line.find("\"platform\":\"") != std::string::npos);
    CHECK(line.find("\"pid\":") != std::string::npos);
    CHECK(line.find("\"t0_wall\":\"") != std::string::npos);
}

TEST_CASE("telemetry: formatters fail closed on undersized buffers") {
    FrameTelemetry t{};
    char tiny[16];
    CHECK(format_frame_jsonl(t, tiny, sizeof(tiny)) == 0);
    CHECK(format_idle_jsonl(0.0, 0, tiny, sizeof(tiny)) == 0);
    CHECK(format_session_jsonl(tiny, sizeof(tiny)) == 0);
    // A torn record must never be emitted: zero means "did not fit",
    // and the buffer contents are not part of the contract.
}
