// affine_2600 — the "Affine 2600 / MR-30", an ARP 2600 / Korg MS-20
// inspired semi-modular synth face built from the skeuomorphic hardware
// kit (extras/skeuo): hardware chassis, silkscreen groups, patch jacks
// with drag-able hanging cables, backlit buttons, 7-segment LCDs, LED
// meters, faders and knobs — all rendered natively by AffineUI.
//
// Drag from one jack to another to patch a cable (Reason-style); grab a
// patched jack to re-route it; click a cable to remove it. The default
// patch is the classic subtractive voice: SEQ → VCO → VCF → VCA with
// LFO and ADSR modulation.

#include "affineui_skeuo.h"

#include <affineui/app.h>
#include <affineui/components.h>
#include <affineui/view.h>

#include <array>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>

namespace sk = affineui::skeuo;
using affineui::View;
using affineui::ViewTheme;

namespace {

struct SynthState {
    std::unordered_map<std::string, double> values{
        {"vco1-freq", .48},  {"vco1-fine", .5},   {"vco1-pwm", .35},
        {"vco2-freq", .52},  {"vco2-detune", .58}, {"vco2-shape", .3},
        {"vcf-cutoff", .62}, {"vcf-res", .34},    {"vcf-env", .5},
        {"lfo-rate", .38},
        {"vca-level", .72},  {"vca-pan", .5},
        {"clk-rate", .42},   {"clk-glide", .2},   {"gate-time", .6},
    };
    std::array<double, 8> step_vals{.42, .30, .58, .36, .66, .48, .24, .54};
    std::array<bool, 8>   steps{true, false, true, true, false, true,
                                false, true};
    int    num_steps{8};
    int    active_step{0};
    bool   playing{true};
    int    vco1_wave{0};   // 0 = saw, 1 = pulse
    int    lfo_wave{0};    // 0 = tri, 1 = sqr
    float  vu_l{0.55f};
    float  vu_r{0.48f};

    double tempo() const {
        return 40.0 + values.at("clk-rate") * 200.0;   // 40–240 BPM
    }
    double lfo_hz() const {
        return 0.1 + values.at("lfo-rate") * 19.9;
    }
};

std::string fixed1(double v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

const char* kNativeCss = R"CSS(
body { margin: 0; }
/* The synth face fills the window edge to edge — neutralise the View's
   default .aui-root gutter (used by the generic headless dump shell). */
.aui-root { padding: 0; min-height: 0; }
.bench {
  min-height: 100vh;
  background: radial-gradient(circle at 46% -20%, #383028, #0d0f14 74%);
  display: flex;
}
.case {
  display: flex;
  gap: 14px;
  flex: 1;
  align-items: stretch;
}
.rack { display: flex; flex-direction: column; gap: 8px; flex: 1; }
.bottom-row { display: flex; gap: 8px; align-items: stretch; }
.module { padding: 18px 4px 8px; display: flex; flex-direction: column;
          gap: 6px; flex: 1 1 0; min-width: 0; --knob-size: 38px; }
.mod-row { gap: 6px; }
.module-title { text-align: center; letter-spacing: .22em; font-size: 11px; }
/* Compact hardware-style knob fields: label + value stack around the
   knob instead of the default wide side-label form row. */
.module .dcs-field, .seq .dcs-field {
  flex-direction: column; align-items: center;
  gap: 2px; min-height: 0;
}
.module .dcs-field__label, .seq .dcs-field__label {
  flex: 0 0 auto; width: auto; min-width: 0; font-size: 9px;
  letter-spacing: .06em; text-transform: uppercase;
}
.module .dcs-silk, .seq .dcs-silk { padding: 5px 6px 8px; }
.jack-row { gap: 6px; }
.col { display: flex; flex-direction: column; align-items: center; gap: 6px; }
.row { display: flex; gap: 8px; align-items: flex-end; flex-wrap: wrap;
       justify-content: center; }
.row-c { display: flex; gap: 8px; align-items: center; flex-wrap: wrap;
         justify-content: center; }
.row-tight { display: flex; gap: 8px; align-items: flex-end;
             justify-content: center; }
.jack-row { display: flex; gap: 10px; justify-content: center;
            flex-wrap: wrap; margin-top: auto; padding-top: 10px; }
.knob-pad { padding-top: 14px; }
.bay { padding: 20px 8px 10px; display: flex;
       flex-direction: column; gap: 8px; }
.bay-row { display: flex; gap: 6px; align-items: flex-start; }
.bay-grid { display: flex; flex-wrap: wrap; gap: 6px;
            justify-content: center; width: 118px; }
.bay-grid .dcs-jack { width: 52px; }
.kbd { padding: 12px 10px 10px; }
.mod-row { display: flex; align-items: stretch; }
.seq { padding: 20px 10px 10px; display: flex; flex-direction: column;
       gap: 8px; flex: 1 1 0; min-width: 0; --knob-size: 38px; }
.seq-top { display: flex; gap: 8px; align-items: flex-end;
           justify-content: space-between; flex-wrap: wrap; }
/* One column per step — the trigger button sits directly over its level
   fader, like the hardware. The grid hugs the panel bottom. */
.steps-grid { display: flex; gap: 12px; justify-content: center;
              align-items: flex-start; margin-top: auto;
              padding-top: 8px; }
.step-col { display: flex; flex-direction: column; align-items: center;
            gap: 6px; }
.step-col .dcs-fader { width: 24px; }
)CSS";

class Affine2600 {
public:
    Affine2600() : app_(config()) {}

    int run() {
        bay_.attach(app_);
        bay_.request_render = [this] { reload(); };
        bay_.on_change = [this] { reload(); };
        default_patch();
        app_.on_frame([this](double dt) { tick(dt); });
        reload();
        return app_.run();
    }

private:
    static affineui::App::Config config() {
        affineui::App::Config cfg;
        cfg.title = "AffineUI — Affine 2600 / MR-30";
        // Logical points (sokol scales the win32 window by the monitor
        // DPI). Height matches the laid-out face exactly (measure with
        // AFFINEUI_LAYOUT_DUMP after layout changes) so the window opens
        // showing the whole synth.
        cfg.width = 1180;
        cfg.height = 1001;
        cfg.clear_color = affineui::Color{13, 15, 20, 255};
        cfg.high_dpi = true;
        cfg.asset_folders = {".", "examples"};
        return cfg;
    }

    void default_patch() {
        bay_.connect("seq-cv", "vco1-oct");
        bay_.connect("seq-gate", "adsr-gate");
        bay_.connect("vco1-saw", "vcf-in1");
        bay_.connect("vco2-tri", "vcf-in2");
        bay_.connect("lfo-tri", "vcf-cv");
        bay_.connect("adsr-out", "vca-cv");
        bay_.connect("vcf-out", "vca-in");
    }

    double value(std::string_view id) const {
        const auto it = state_.values.find(std::string(id));
        return it == state_.values.end() ? 0.0 : it->second;
    }

    void bind_value(affineui::WidgetRef ref, std::string id) {
        ref.on_change([this, id](std::string_view v) {
            state_.values[id] = std::strtod(std::string(v).c_str(), nullptr);
        });
    }

    // Rate / tempo knobs also refresh the LCDs.
    void bind_value_reload(affineui::WidgetRef ref, std::string id) {
        ref.on_change([this, id](std::string_view v) {
            state_.values[id] = std::strtod(std::string(v).c_str(), nullptr);
            reload();
        });
    }

    void tick(double dt) {
        if (!state_.playing) return;
        step_accum_ += dt;
        const double interval = 60.0 / (state_.tempo() * 2.0);  // 8ths
        if (step_accum_ < interval) return;
        while (step_accum_ >= interval) {
            step_accum_ -= interval;
            state_.active_step =
                (state_.active_step + 1) % std::max(1, state_.num_steps);
        }
        // VU wander, gated by the step pattern.
        const bool gate =
            state_.steps[static_cast<std::size_t>(state_.active_step % 8)];
        std::uniform_real_distribution<float> jitter(-0.18f, 0.18f);
        const float target = gate ? 0.55f + jitter(rng_) : 0.18f;
        state_.vu_l = std::clamp(target + jitter(rng_) * 0.4f, 0.05f, 0.98f);
        state_.vu_r = std::clamp(target + jitter(rng_) * 0.4f, 0.05f, 0.98f);
        reload();
    }

    // ── Modules ──────────────────────────────────────────────────────
    void module_vco1(View& v) {
        auto mod = sk::hw_panel(v, sk::HwFinish::Brushed, "module", "vco1");
        sk::hw_label(v, "OSCILLATOR 1", "__t").attr("class",
                                                    "dcs-hw__label module-title");
        {
            auto grp = sk::silk(v, "WAVE");
            auto row = v.container("row-c", "__waves");
            sk::lit_button(v, "vco1-saw-btn", "SAW", sk::LitColor::Amber,
                           state_.vco1_wave == 0)
                .on_click([this] { state_.vco1_wave = 0; reload(); });
            sk::lit_button(v, "vco1-pul-btn", "PUL", sk::LitColor::Amber,
                           state_.vco1_wave == 1)
                .on_click([this] { state_.vco1_wave = 1; reload(); });
        }
        {
            auto row = v.container("row knob-pad", "__knobs");
            bind_value(v.knob("FREQ", value("vco1-freq"), 0, 1, false,
                              "vco1-freq"),
                       "vco1-freq");
            bind_value(v.knob("FINE", value("vco1-fine"), 0, 1, true,
                              "vco1-fine"),
                       "vco1-fine");
            bind_value(v.knob("PWM", value("vco1-pwm"), 0, 1, false,
                              "vco1-pwm"),
                       "vco1-pwm");
        }
        {
            auto row = v.container("jack-row", "__jacks");
            bay_.jack(v, "vco1-oct", "1V/OCT");
            bay_.jack(v, "vco1-fm", "FM");
            bay_.jack(v, "vco1-saw", "SAW");
            bay_.jack(v, "vco1-pul", "PULSE");
        }
    }

    void module_vco2(View& v) {
        auto mod = sk::hw_panel(v, sk::HwFinish::Brushed, "module", "vco2");
        sk::hw_label(v, "OSCILLATOR 2", "__t").attr("class",
                                                    "dcs-hw__label module-title");
        {
            auto row = v.container("row knob-pad", "__knobs");
            bind_value(v.knob("FREQ", value("vco2-freq"), 0, 1, false,
                              "vco2-freq"),
                       "vco2-freq");
            bind_value(v.knob("DETUNE", value("vco2-detune"), 0, 1, true,
                              "vco2-detune"),
                       "vco2-detune");
            bind_value(v.knob("SHAPE", value("vco2-shape"), 0, 1, false,
                              "vco2-shape"),
                       "vco2-shape");
        }
        {
            auto row = v.container("jack-row", "__jacks");
            bay_.jack(v, "vco2-oct", "1V/OCT");
            bay_.jack(v, "vco2-sync", "SYNC");
            bay_.jack(v, "vco2-tri", "TRI");
            bay_.jack(v, "vco2-sqr", "SQR");
        }
    }

    void module_vcf(View& v) {
        auto mod = sk::hw_panel(v, sk::HwFinish::Red, "module", "vcf");
        sk::hw_label(v, "LADDER FILTER", "__t").attr("class",
                                                     "dcs-hw__label module-title");
        {
            auto grp = sk::silk(v, "VCF");
            auto row = v.container("row-c", "__knobs");
            bind_value(v.knob("CUTOFF", value("vcf-cutoff"), 0, 1, false,
                              "vcf-cutoff"),
                       "vcf-cutoff");
            bind_value(v.knob("RES", value("vcf-res"), 0, 1, false,
                              "vcf-res"),
                       "vcf-res");
            bind_value(v.knob("ENV", value("vcf-env"), 0, 1, true,
                              "vcf-env"),
                       "vcf-env");
            {
                auto col = v.container("col", "__vu");
                sk::led_meter(v, state_.vu_l, 14, true, "vcf-vu");
                sk::control_label(v, "DRIVE", "__l");
            }
        }
        {
            auto row = v.container("jack-row", "__jacks");
            bay_.jack(v, "vcf-in1", "IN 1");
            bay_.jack(v, "vcf-in2", "IN 2");
            bay_.jack(v, "vcf-cv", "CUTOFF CV");
            bay_.jack(v, "vcf-out", "OUT");
        }
    }

    void module_adsr(View& v) {
        auto mod = sk::hw_panel(v, sk::HwFinish::Lacquer, "module", "adsr");
        sk::hw_label(v, "ENVELOPE", "__t").attr("class",
                                                "dcs-hw__label module-title");
        {
            auto grp = sk::silk(v, "ADSR");
            auto row = v.container("row-tight", "__faders");
            const char* names[4] = {"env-a", "env-d", "env-s", "env-r"};
            const char* labels[4] = {"A", "D", "S", "R"};
            const double defaults[4] = {.18, .42, .71, .56};
            for (int i = 0; i < 4; ++i) {
                auto col = v.container("col", names[i]);
                const auto it = state_.values.find(names[i]);
                const double val =
                    it == state_.values.end() ? defaults[i] : it->second;
                bind_value(sk::fader(v, names[i], val, 0, 1, 84), names[i]);
                sk::control_label(v, labels[i], "__l");
            }
        }
        {
            auto row = v.container("jack-row", "__jacks");
            bay_.jack(v, "adsr-gate", "GATE");
            bay_.jack(v, "adsr-trig", "TRIG");
            bay_.jack(v, "adsr-out", "ENV OUT");
        }
    }

    void module_lfo(View& v) {
        // (Cream is the period-correct finish here, but the engine's
        // multi-layer `radial-gradient(... at 30% 0%)` background parse
        // gap renders it dark — track via the conformance harness.)
        auto mod = sk::hw_panel(v, sk::HwFinish::Lacquer, "module", "lfo");
        sk::hw_label(v, "LFO / CLOCK", "__t").attr("class",
                                                   "dcs-hw__label module-title");
        {
            auto grp = sk::silk(v, "LFO");
            auto row = v.container("row-c", "__body");
            bind_value_reload(v.knob("RATE", value("lfo-rate"), 0, 1, false,
                                     "lfo-rate"),
                              "lfo-rate");
            {
                auto col = v.container("col", "__lcd");
                sk::lcd(v, fixed1(state_.lfo_hz()), sk::LcdSize::Md,
                        sk::LcdColor::Green, 3, "lfo-lcd");
                sk::control_label(v, "HZ", "__l");
            }
            {
                auto col = v.container("col", "__waves");
                sk::lit_button(v, "lfo-tri-btn", "TRI", sk::LitColor::Green,
                               state_.lfo_wave == 0, true)
                    .on_click([this] { state_.lfo_wave = 0; reload(); });
                sk::lit_button(v, "lfo-sqr-btn", "SQR", sk::LitColor::Green,
                               state_.lfo_wave == 1, true)
                    .on_click([this] { state_.lfo_wave = 1; reload(); });
            }
        }
        {
            auto row = v.container("jack-row", "__jacks");
            bay_.jack(v, "lfo-tri", "TRI");
            bay_.jack(v, "lfo-sqr", "SQR");
            bay_.jack(v, "lfo-sync", "SYNC");
        }
    }

    void module_vca(View& v) {
        auto mod = sk::hw_panel(v, sk::HwFinish::Steel, "module", "vca");
        sk::hw_label(v, "VCA / OUTPUT", "__t").attr("class",
                                                    "dcs-hw__label module-title");
        {
            auto grp = sk::silk(v, "OUTPUT");
            auto row = v.container("row-c", "__body");
            bind_value(v.knob("LEVEL", value("vca-level"), 0, 1, false,
                              "vca-level"),
                       "vca-level");
            bind_value(v.knob("PAN", value("vca-pan"), 0, 1, true,
                              "vca-pan"),
                       "vca-pan");
            {
                auto col = v.container("col", "__vul");
                sk::led_meter(v, state_.vu_l, 14, true, "vu-l");
                sk::control_label(v, "L", "__l");
            }
            {
                auto col = v.container("col", "__vur");
                sk::led_meter(v, state_.vu_r, 14, true, "vu-r");
                sk::control_label(v, "R", "__l");
            }
        }
        {
            auto row = v.container("jack-row", "__jacks");
            bay_.jack(v, "vca-in", "IN");
            bay_.jack(v, "vca-cv", "CV");
            bay_.jack(v, "main-out", "MAIN OUT");
            bay_.jack(v, "phones", "PHONES");
        }
    }

    void module_sequencer(View& v) {
        auto mod = sk::hw_panel(v, sk::HwFinish::Red, "seq", "seq");
        sk::hw_label(v, "8 STEP SEQUENCER", "__t").attr("class",
                                                        "dcs-hw__label module-title");
        {
            auto top = v.container("seq-top", "__top");
            {
                auto transport = v.container("row", "__transport");
                {
                    auto col = v.container("col", "__stop");
                    sk::lit_button(v, "seq-stop", "STOP", sk::LitColor::Red,
                                   !state_.playing)
                        .on_click([this] {
                            state_.playing = false;
                            reload();
                        });
                    bay_.jack(v, "stop-in", "STOP");
                }
                {
                    auto col = v.container("col", "__run");
                    sk::lit_button(v, "seq-run", "RUN", sk::LitColor::Green,
                                   state_.playing)
                        .on_click([this] {
                            state_.playing = true;
                            reload();
                        });
                    bay_.jack(v, "start-in", "START");
                }
                {
                    auto col = v.container("col", "__adv");
                    sk::lit_button(v, "seq-adv", "ADV", sk::LitColor::Amber,
                                   false)
                        .on_click([this] {
                            state_.active_step = (state_.active_step + 1) %
                                                 std::max(1, state_.num_steps);
                            reload();
                        });
                    bay_.jack(v, "reset-in", "RESET");
                }
                {
                    auto col = v.container("col", "__ext");
                    sk::lit_button(v, "seq-ext", "EXT", sk::LitColor::Amber,
                                   false);
                    bay_.jack(v, "ext-clk", "EXT CLK");
                }
            }
            {
                auto steps = v.container("col", "__numsteps");
                {
                    auto row = v.container("row-c", "__ctl");
                    auto [up, down] = sk::step_pair(v, "steps-up",
                                                    "steps-down");
                    up.on_click([this] {
                        state_.num_steps = std::min(8, state_.num_steps + 1);
                        reload();
                    });
                    down.on_click([this] {
                        state_.num_steps = std::max(1, state_.num_steps - 1);
                        reload();
                    });
                    sk::lcd(v, std::to_string(state_.num_steps),
                            sk::LcdSize::Md, sk::LcdColor::Red, 2,
                            "steps-lcd");
                }
                sk::control_label(v, "NUM OF STEPS", "__l");
            }
            {
                auto tempo = v.container("col", "__tempo");
                sk::lcd(v, fixed1(state_.tempo()), sk::LcdSize::Md,
                        sk::LcdColor::Amber, 4, "tempo-lcd");
                sk::control_label(v, "TEMPO BPM", "__l");
            }
            {
                auto clock = sk::silk(v, "CLOCK");
                auto row = v.container("row-c", "__knobs");
                bind_value_reload(v.knob("RATE", value("clk-rate"), 0, 1,
                                         false, "clk-rate"),
                                  "clk-rate");
                bind_value(v.knob("GLIDE", value("clk-glide"), 0, 1, false,
                                  "clk-glide"),
                           "clk-glide");
            }
            {
                auto out = sk::silk(v, "OUTPUT");
                auto row = v.container("row-c", "__body");
                bind_value(v.knob("GATE", value("gate-time"), 0, 1, false,
                                  "gate-time"),
                           "gate-time");
                bay_.jack(v, "seq-gate", "GATE OUT");
                bay_.jack(v, "seq-cv", "CV OUT");
            }
        }
        {
            auto steps = v.container("steps-grid", "__steps");
            for (int i = 0; i < 8; ++i) {
                const std::string col_key = "col-" + std::to_string(i);
                auto col = v.container("step-col", col_key);
                const std::string name = "step-" + std::to_string(i);
                const bool current =
                    state_.playing && i == (state_.active_step % 8);
                auto ref = sk::lit_button(v, name, std::to_string(i + 1),
                                          sk::LitColor::Cyan,
                                          state_.steps[i], false, name);
                // The running step dims briefly, like the reference.
                // Declarative contract: attrs persist on retained nodes,
                // so the non-current branch must actively remove it.
                if (current) ref.attr("style", "opacity:.45");
                else ref.remove_attr("style");
                ref.on_click([this, i] {
                    state_.steps[static_cast<std::size_t>(i)] =
                        !state_.steps[static_cast<std::size_t>(i)];
                    reload();
                });
                const std::string fname = "stepv-" + std::to_string(i);
                auto fref = sk::fader(v, fname, state_.step_vals[i], 0, 1,
                                      72, fname);
                fref.on_change([this, i](std::string_view val) {
                    state_.step_vals[static_cast<std::size_t>(i)] =
                        std::strtod(std::string(val).c_str(), nullptr);
                });
            }
        }
    }

    void module_keyboard(View& v) {
        auto mod = sk::hw_panel(v, sk::HwFinish::Steel, "kbd", "kbd");
        sk::keyboard(v, "kbd", 4, [this](int midi) {
            // Visual gate: strike the meters like a played note (there
            // is no audio engine behind the face).
            const float level =
                0.55f + static_cast<float>(midi % 12) * 0.03f;
            state_.vu_l = std::min(0.98f, level);
            state_.vu_r = std::min(0.98f, level * 0.96f);
            reload();
        }, 2, "__strip");
    }

    void patch_bay_panel(View& v) {
        auto panel = sk::hw_panel(v, sk::HwFinish::Steel, "bay", "bay");
        sk::hw_label(v, "PATCH BAY · MR-30", "__t").attr("class",
                                                         "dcs-hw__label module-title");
        auto row = v.container("bay-row", "__row");
        {
            auto grp = sk::silk(v, "SOURCES");
            auto grid = v.container("bay-grid", "__g");
            bay_.jack(v, "noise-out", "NOISE");
            bay_.jack(v, "sh-out", "S&H");
            bay_.jack(v, "sh-clk", "S&H CLK");
            bay_.jack(v, "kybd-cv", "KYBD CV");
            bay_.jack(v, "kybd-gate", "KYBD GT");
            bay_.jack(v, "bend", "BEND");
        }
        {
            auto grp = sk::silk(v, "PROCESSORS");
            auto grid = v.container("bay-grid", "__g");
            bay_.jack(v, "ring-a", "RING A");
            bay_.jack(v, "ring-b", "RING B");
            bay_.jack(v, "ring-out", "RING OUT");
            bay_.jack(v, "inv-in", "INV IN");
            bay_.jack(v, "inv-out", "INV OUT");
            bay_.jack(v, "envfol", "ENV FOL");
        }
        {
            auto grp = sk::silk(v, "MIX / MULT");
            auto grid = v.container("bay-grid", "__g");
            bay_.jack(v, "mix-1", "MIX 1");
            bay_.jack(v, "mix-2", "MIX 2");
            bay_.jack(v, "mix-out", "MIX OUT");
            bay_.jack(v, "mult-1", "MULT 1");
            bay_.jack(v, "mult-2", "MULT 2");
            bay_.jack(v, "mult-3", "MULT 3");
        }
    }

    void build_view(View& v) {
        v.set_theme(ViewTheme::Decius);
        v.selector("style", "3d");
        v.selector("density", "comfortable");
        v.selector("accent", "orange");

        {
            auto bench = v.container("bench", "__bench");
            auto board = bay_.board(v, "case", "__case");
            {
                auto rack = v.container("rack", "__rack");
                {
                    auto row = v.container("mod-row", "__mods");
                    module_vco1(v);
                    module_vco2(v);
                    module_vcf(v);
                    module_adsr(v);
                    module_lfo(v);
                    module_vca(v);
                }
                {
                    auto bottom = v.container("bottom-row", "__bottom");
                    module_sequencer(v);
                    patch_bay_panel(v);
                }
                module_keyboard(v);
            }
            bay_.cables_layer(v);
        }
    }

    void reload() {
        if (reloading_) return;   // guard re-entry from change callbacks
        reloading_ = true;
        if (!view_installed_) {
            view_installed_ = true;
            // Stylesheet first: installing it later re-parses the
            // retained document (see Document::set_user_stylesheet).
            static const std::string css =
                std::string(kNativeCss) + std::string(sk::stylesheet());
            app_.set_stylesheet(css);
            app_.set_view([this](View& v) { build_view(v); });
        } else {
            // Reconciles: only actual differences reach the document.
            app_.rebuild_view();
        }
        reloading_ = false;
    }

    affineui::App app_;
    sk::PatchBay  bay_;
    SynthState    state_;
    double        step_accum_{0.0};
    bool          reloading_{false};
    bool          view_installed_{false};
    std::minstd_rand rng_{20260703};
};

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--perf") {
            // Same switch as AFFINEUI_PERF_OVERLAY=1.
            _putenv_s("AFFINEUI_PERF_OVERLAY", "1");
        }
    }
    Affine2600 synth;
    return synth.run();
}
