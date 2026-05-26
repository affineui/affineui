// decius_synth_skeuo - skeuomorphic modular synth panel using Decius CSS.

#include "decius_interactions.h"

#include <affineui/affineui.h>

#include <sokol_log.h>

#include <sstream>
#include <string>
#include <unordered_map>

namespace dcs = demo::decius;

namespace {

struct SkeuoState {
    bool drive_hot{true};
    int  bank{3};
    std::unordered_map<std::string, float> values{
        {"saw", .62f}, {"pwm", .38f}, {"fine", -.2f},
        {"cut", .74f}, {"res", .56f}, {"filter-send", .64f},
        {"drive-a", .38f}, {"drive-b", .68f},
        {"env-a", .18f}, {"env-d", .42f}, {"env-s", .71f}, {"env-r", .56f},
    };
};

float value(const SkeuoState& s, std::string_view id, float fallback) {
    auto it = s.values.find(std::string(id));
    return it == s.values.end() ? fallback : it->second;
}

std::string render(const SkeuoState& s) {
    std::ostringstream h;
    h << R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<link rel="stylesheet" href="frameworks/css/decius-css-0.4.1.bundle.min.css">
<style>
body{margin:0;background:#101219}
.bench{min-height:100vh;background:radial-gradient(circle at 48% -20%,#34302a,#0e1015 72%);padding:24px;overflow:hidden}
.case{position:relative;height:820px;border-radius:18px;background:linear-gradient(180deg,#211b16,#110e0c);box-shadow:inset 0 2px 0 rgba(255,255,255,.08),inset 0 -8px 24px rgba(0,0,0,.65),0 28px 70px rgba(0,0,0,.65);padding:28px;display:flex;gap:18px}
.module{position:relative;flex:1;padding:28px 20px 20px}
.module-wide{flex:1.35}.module-narrow{flex:.78}
.mod-title{display:flex;align-items:center;justify-content:space-between;margin-bottom:20px}
.grid{display:flex;flex-wrap:wrap;gap:18px;align-items:center}.fader-bank{display:flex;gap:22px;align-items:center;justify-content:center;margin-top:20px}
.jack-row{display:flex;gap:14px;margin-top:22px}.jack{width:28px;height:28px;border-radius:50%;background:radial-gradient(circle at 35% 30%,#0b0c0f,#30333a 56%,#050608 100%);box-shadow:inset 0 2px 3px rgba(0,0,0,.9),0 1px 0 rgba(255,255,255,.18)}
.mini-actions{display:flex;gap:8px;align-items:center}.mini-actions .dcs-btn{min-height:28px;padding:4px 9px}
.cable{position:absolute;height:8px;border-radius:999px;background:linear-gradient(180deg,#6ee7ff,#1686d9);box-shadow:0 8px 18px rgba(0,0,0,.45);transform-origin:left center;animation:swing 2200ms ease-in-out infinite alternate;z-index:4}
.c1{left:240px;top:610px;width:440px;transform:rotate(-10deg)}.c2{left:620px;top:646px;width:320px;background:linear-gradient(180deg,#ffcb6b,#d96b16);transform:rotate(8deg);animation-delay:300ms}.c3{left:900px;top:610px;width:260px;background:linear-gradient(180deg,#88ffbb,#1aaa68);transform:rotate(-14deg);animation-delay:620ms}
@keyframes swing{from{filter:brightness(.96)}to{filter:brightness(1.12)}}
.led{width:14px;height:14px;border-radius:50%;background:#45ff9c;box-shadow:0 0 18px #45ff9c,0 0 2px #fff;animation:blink 900ms steps(2,end) infinite}.led.red{background:#ff5d65;box-shadow:0 0 18px #ff5d65,0 0 2px #fff;animation-delay:250ms}.led.dim{opacity:.28;animation:none;box-shadow:0 0 4px rgba(69,255,156,.4)}
@keyframes blink{50%{opacity:.35}}
.readout{font-family:var(--dcs-font-mono);font-size:28px;letter-spacing:.12em;color:#ff6b55;background:#1a0806;border:1px solid rgba(255,107,85,.38);border-radius:6px;padding:8px 14px;text-shadow:0 0 14px rgba(255,107,85,.9);box-shadow:inset 0 2px 10px rgba(0,0,0,.8)}
</style></head>
<body class="dcs" data-dcs-density="comfortable" data-dcs-style="3d" data-dcs-accent="orange">
<div class="bench"><div class="case">
  <section class="module module-wide dcs-hw dcs-hw--brushed">)HTML" << dcs::hardware_screws() << R"HTML(
    <div class="mod-title"><span class="dcs-hw__label">dual oscillator</span><div class="mini-actions">)HTML"
      << dcs::button("bank " + std::to_string(s.bank), "sliders", "", "bank")
      << R"HTML(<div class="led"></div></div></div>
    <fieldset class="dcs-silk"><legend>waves</legend><div class="grid">)HTML"
      << dcs::knob(0, 1, value(s, "saw", .62f), "Saw", false, 82, "saw")
      << dcs::knob(0, 1, value(s, "pwm", .38f), "PWM", false, 82, "pwm")
      << dcs::knob(-1, 1, value(s, "fine", -.2f), "Fine", true, 82, "fine")
      << R"HTML(</div></fieldset>
    <div class="jack-row"><div class="jack"></div><div class="jack"></div><div class="jack"></div><div class="jack"></div></div>
  </section>
  <section class="module dcs-hw dcs-hw--cream">)HTML" << dcs::hardware_screws() << R"HTML(
    <div class="mod-title"><span class="dcs-hw__label">filter bank</span><div class="led red"></div></div>
    <fieldset class="dcs-silk"><legend>ladder</legend><div class="grid">)HTML"
      << dcs::knob(0, 1, value(s, "cut", .74f), "Cut", false, 88, "cut")
      << dcs::knob(0, 1, value(s, "res", .56f), "Res", false, 88, "res")
      << dcs::slider(0, 1, value(s, "filter-send", .64f), false, true, "filter-send")
      << R"HTML(</div></fieldset>
    <div class="jack-row"><div class="jack"></div><div class="jack"></div><div class="jack"></div></div>
  </section>
  <section class="module module-narrow dcs-hw dcs-hw--red">)HTML" << dcs::hardware_screws() << R"HTML(
    <div class="mod-title"><span class="dcs-hw__label">vca</span><div class="mini-actions">)HTML"
      << dcs::button(s.drive_hot ? "hot" : "idle", "zap",
                     s.drive_hot ? "dcs-btn--danger" : "", "drive",
                     s.drive_hot)
      << R"HTML(</div></div>
    <div class="readout">)HTML" << (s.drive_hot ? "7.42" : "2.18") << R"HTML(</div>
    <div class="fader-bank">)HTML"
      << dcs::fader(value(s, "drive-a", .38f), true, "drive-a")
      << dcs::fader(value(s, "drive-b", .68f), true, "drive-b")
      << R"HTML(</div>
    <div class="jack-row"><div class="jack"></div><div class="jack"></div></div>
  </section>
  <section class="module module-wide dcs-hw dcs-hw--lacquer">)HTML" << dcs::hardware_screws() << R"HTML(
    <div class="mod-title"><span class="dcs-hw__label">envelope / modulation</span><div class="led"></div></div>
    <fieldset class="dcs-silk"><legend>adsr</legend><div class="fader-bank">)HTML"
      << dcs::fader(value(s, "env-a", .18f), true, "env-a")
      << dcs::fader(value(s, "env-d", .42f), true, "env-d")
      << dcs::fader(value(s, "env-s", .71f), true, "env-s")
      << dcs::fader(value(s, "env-r", .56f), true, "env-r")
      << R"HTML(</div></fieldset>
    <div class="jack-row"><div class="jack"></div><div class="jack"></div><div class="jack"></div><div class="jack"></div></div>
  </section>
  <div class="cable c1"></div><div class="cable c2"></div><div class="cable c3"></div>
</div></div></body></html>)HTML";
    return h.str();
}

}  // namespace

int main() {
    affineui::Ui ui;
    demo::install_resource_loader(ui);
    SkeuoState state{};
    auto rerender = [&] { ui.html(render(state)); ui.mark_dirty(); };
    rerender();
    ui.on_click("#drive", [&] { state.drive_hot = !state.drive_hot; rerender(); });
    ui.on_click("#bank", [&] { state.bank = (state.bank % 8) + 1; rerender(); });
    dcs::install_value_interactions(ui, {
        [&](std::string_view id) { return value(state, id, 0.0f); },
        [&](std::string_view id, float v) { state.values[std::string(id)] = v; },
        rerender,
    });
    sapp_desc desc{};
    desc.width = 1440;
    desc.height = 900;
    desc.window_title = "AffineUI - Decius skeuomorphic synth";
    desc.high_dpi = true;
    desc.swap_interval = 0;
    desc.sample_count = 1;
    desc.logger.func = slog_func;
    affineui::sokol::wire(desc, ui, true);
    sapp_run(&desc);
    return 0;
}
