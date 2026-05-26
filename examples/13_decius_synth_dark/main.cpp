// decius_synth_dark - dark flat synthesizer UI using Decius controls.

#include "decius_interactions.h"

#include <affineui/affineui.h>

#include <sokol_log.h>

#include <sstream>
#include <string>
#include <unordered_map>

namespace dcs = demo::decius;

namespace {

struct SynthState {
    bool armed{true};
    bool sync{false};
    int  patch{0};
    std::unordered_map<std::string, float> values{
        {"shape", .66f}, {"detune", -.18f}, {"fold", .42f},
        {"sub", .54f}, {"phase", .28f}, {"cutoff", 9400.0f},
        {"res", .72f}, {"env", .15f}, {"drive", .63f},
        {"atk", 18.0f}, {"dec", 42.0f}, {"rel", 71.0f},
        {"fader-a", .28f}, {"fader-d", .58f}, {"fader-s", .76f},
        {"output", .82f},
    };
};

float value(const SynthState& s, std::string_view id, float fallback) {
    auto it = s.values.find(std::string(id));
    return it == s.values.end() ? fallback : it->second;
}

std::string render(const SynthState& s) {
    std::ostringstream h;
    h << R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<link rel="stylesheet" href="frameworks/css/decius-css-0.4.1.bundle.min.css">
<style>
body{margin:0;background:#101219}
.desk{min-height:100vh;background:radial-gradient(circle at 30% 0,#273044,#101219 64%);padding:22px;overflow:hidden}
.synth{height:820px;display:flex;flex-direction:column;gap:12px}
.topbar{display:flex;align-items:center;gap:12px}
.brand{font-size:24px;font-weight:700;letter-spacing:.08em;color:var(--dcs-text)}
.lcd{font-family:var(--dcs-font-mono);font-size:20px;color:#7dffb2;background:#101b17;border:1px solid #1f4f39;border-radius:var(--dcs-r-3);padding:8px 14px;box-shadow:inset 0 2px 8px rgba(0,0,0,.6),0 0 24px rgba(61,214,138,.14)}
.rack{flex:1;display:flex;gap:12px;min-height:0}
.module{background:rgba(32,35,43,.92);border:1px solid var(--dcs-line);border-radius:var(--dcs-r-3);padding:14px;box-shadow:var(--dcs-shadow-2)}
.module h2{margin:0 0 14px;font-size:12px;text-transform:uppercase;letter-spacing:.18em;color:var(--dcs-text-mute)}
.osc{flex:1}.filter{flex:1.15}.env{flex:.9}.mix{flex:.8}
.knobs{display:flex;gap:18px;flex-wrap:wrap}.slider-row{display:flex;align-items:center;gap:10px;margin:13px 0}.slider-row span{width:72px;color:var(--dcs-text-mute);font-size:var(--dcs-fs-xs);text-transform:uppercase}
.meters{display:flex;gap:8px;height:140px;align-items:end;padding:12px;background:var(--dcs-well);border:1px solid var(--dcs-line);border-radius:var(--dcs-r-2)}
.meter{flex:1;background:linear-gradient(180deg,var(--dcs-ok),var(--dcs-accent));border-radius:3px 3px 0 0;animation:pulse 900ms ease-in-out infinite alternate}
.m2{animation-delay:120ms}.m3{animation-delay:240ms}.m4{animation-delay:360ms}
@keyframes pulse{from{filter:brightness(.78);transform:scaleY(.84)}to{filter:brightness(1.15);transform:scaleY(1)}}
.keys{height:118px;display:flex;gap:3px}.key{flex:1;background:#e8e9ed;border:1px solid #9aa0aa;border-radius:0 0 6px 6px}.key.black{height:70px;background:#15171f;flex:.62;margin-left:-22px;margin-right:-22px;z-index:2}
</style></head>
<body class="dcs" data-dcs-density="comfortable" data-dcs-accent="green">
<div class="desk"><div class="synth">
  <div class="topbar"><div class="brand">EMBER-7</div><div class="lcd">PATCH )HTML" << (s.patch + 1) << R"HTML( / DENSE NATIVE LEAD</div><div style="flex:1"></div>)HTML"
      << dcs::button("Prev", "chevron-left", "dcs-btn--ghost", "prev")
      << dcs::button("Next", "chevron-right", "dcs-btn--primary", "next")
      << R"HTML(</div>
  <div class="rack">
    <section class="module osc"><h2>Oscillators</h2><div class="knobs">)HTML"
      << dcs::knob(0, 1, value(s, "shape", .66f), "Shape", false, 72, "shape")
      << dcs::knob(-1, 1, value(s, "detune", -.18f), "Detune", true, 72, "detune")
      << dcs::knob(0, 1, value(s, "fold", .42f), "Fold", false, 72, "fold")
      << R"HTML(</div><div class="slider-row"><span>Sub</span>)HTML" << dcs::slider(0, 1, value(s, "sub", .54f), false, true, "sub") << R"HTML(</div><div class="slider-row"><span>Phase</span>)HTML" << dcs::slider(-1, 1, value(s, "phase", .28f), true, true, "phase") << R"HTML(</div>)HTML"
      << dcs::check("Hard sync", s.sync, false, "sync")
      << R"HTML(</section>
    <section class="module filter"><h2>Filter</h2><div class="knobs">)HTML"
      << dcs::knob(20, 20000, value(s, "cutoff", 9400.0f), "Cutoff", false, 86, "cutoff")
      << dcs::knob(0, 1, value(s, "res", .72f), "Res", false, 86, "res")
      << dcs::knob(-1, 1, value(s, "env", .15f), "Env", true, 86, "env")
      << R"HTML(</div><div class="slider-row"><span>Drive</span>)HTML" << dcs::slider(0, 1, value(s, "drive", .63f), false, false, "drive") << R"HTML(</div>)HTML"
      << dcs::toggle("Voice armed", s.armed, "armed")
      << R"HTML(</section>
    <section class="module env"><h2>Envelope</h2><div class="knobs">)HTML"
      << dcs::knob(0, 100, value(s, "atk", 18.0f), "Atk", false, 0, "atk")
      << dcs::knob(0, 100, value(s, "dec", 42.0f), "Dec", false, 0, "dec")
      << dcs::knob(0, 100, value(s, "rel", 71.0f), "Rel", false, 0, "rel")
      << R"HTML(</div><div style="display:flex;gap:16px;margin-top:18px">)HTML" << dcs::fader(value(s, "fader-a", .28f), true, "fader-a") << dcs::fader(value(s, "fader-d", .58f), true, "fader-d") << dcs::fader(value(s, "fader-s", .76f), true, "fader-s") << R"HTML(</div></section>
    <section class="module mix"><h2>Output</h2><div class="meters"><div class="meter" style="height:64%"></div><div class="meter m2" style="height:86%"></div><div class="meter m3" style="height:72%"></div><div class="meter m4" style="height:54%"></div></div><div style="margin-top:16px">)HTML" << dcs::slider(0, 1, value(s, "output", .82f), false, true, "output") << R"HTML(</div></section>
  </div>
  <div class="keys"><div class="key"></div><div class="key black"></div><div class="key"></div><div class="key black"></div><div class="key"></div><div class="key"></div><div class="key black"></div><div class="key"></div><div class="key black"></div><div class="key"></div><div class="key black"></div><div class="key"></div></div>
</div></div></body></html>)HTML";
    return h.str();
}

}  // namespace

int main() {
    affineui::Ui ui;
    demo::install_resource_loader(ui);
    SynthState state;
    auto rerender = [&] { ui.html(render(state)); ui.mark_dirty(); };
    rerender();
    ui.on_click("#prev", [&] { state.patch = (state.patch + 7) % 8; rerender(); });
    ui.on_click("#next", [&] { state.patch = (state.patch + 1) % 8; rerender(); });
    ui.on_click("#sync", [&] {
        state.sync = !state.sync;
        if (!dcs::set_checked(ui, "sync", state.sync)) rerender();
    });
    ui.on_click("#armed", [&] {
        state.armed = !state.armed;
        if (!dcs::set_checked(ui, "armed", state.armed)) rerender();
    });
    dcs::install_value_interactions(ui, {
        [&](std::string_view id) { return value(state, id, 0.0f); },
        [&](std::string_view id, float v) { state.values[std::string(id)] = v; },
        rerender,
    });
    sapp_desc desc{};
    desc.width = 1280;
    desc.height = 860;
    desc.window_title = "AffineUI - Decius dark synth";
    desc.high_dpi = true;
    desc.swap_interval = 0;
    desc.sample_count = 1;
    desc.logger.func = slog_func;
    affineui::sokol::wire(desc, ui, true);
    sapp_run(&desc);
    return 0;
}
