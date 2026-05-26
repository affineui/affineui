// conformance_viewer - visible AffineUI side of the conformance harness.
//
// Loads the same case HTML/case.json used by the A/B snapshot runner, opens
// a real window, and renders continuously so animation/interactions can be
// reviewed by eye. For cases with `animation_time_ms` steps, the viewer loops
// that deterministic animation timeline instead of letting one-shot CSS
// animations finish and stay frozen.

#include <affineui/affineui.h>

#include "json.h"

#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct Step {
    enum Kind { Click, Hover, Wait, AnimationTime, Snapshot } kind;
    int x = 0;
    int y = 0;
    int ms = 0;
    std::string name;
};

struct Args {
    std::string test;
    std::string cases_dir = "conformance/cases";
    std::string html;
    std::string script;
    int width = 0;
    int height = 0;
    float dpi = 0.0f;
    std::vector<Step> steps;
};

struct AppState {
    affineui::Ui ui;
    Args args;
    std::chrono::steady_clock::time_point start;
    bool first_frame_done{false};
    bool scripted_inputs_applied{false};
    int loop_ms{0};
};

bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto sval = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string();
        };
        if (s == "--test") {
            a.test = sval();
        } else if (s == "--cases-dir") {
            a.cases_dir = sval();
        } else if (s == "--html") {
            a.html = sval();
        } else if (s == "--script") {
            a.script = sval();
        } else if (s == "--width") {
            a.width = std::atoi(sval().c_str());
        } else if (s == "--height") {
            a.height = std::atoi(sval().c_str());
        } else if (s == "--dpi") {
            a.dpi = static_cast<float>(std::atof(sval().c_str()));
        } else {
            std::fprintf(stderr, "unknown option %s\n", s.c_str());
            return false;
        }
    }

    if (a.test.empty() && a.html.empty()) {
        std::fprintf(stderr,
            "usage: conformance_viewer --test <name> [--cases-dir DIR] "
            "[--width W] [--height H] [--dpi S]\n"
            "       conformance_viewer --html <file> [--script case.json]\n");
        return false;
    }
    if (a.html.empty()) a.html = a.cases_dir + "/" + a.test + "/index.html";
    if (a.script.empty() && !a.test.empty()) {
        a.script = a.cases_dir + "/" + a.test + "/case.json";
    }
    if (a.test.empty()) a.test = "case";
    return true;
}

std::string read_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        const std::size_t got = std::fread(&s[0], 1, s.size(), f);
        s.resize(got);
    }
    std::fclose(f);
    return s;
}

bool load_case(Args& a) {
    if (a.script.empty()) return true;
    const std::string text = read_file(a.script);
    if (text.empty()) return true;

    cjson::Value root;
    if (!cjson::parse(text, root) || root.type != cjson::Value::Obj) {
        std::fprintf(stderr, "warning: malformed %s; ignoring\n",
                     a.script.c_str());
        return true;
    }

    if (a.width == 0) {
        if (auto* v = root.find("width")) a.width = static_cast<int>(v->as_num(0));
    }
    if (a.height == 0) {
        if (auto* v = root.find("height")) a.height = static_cast<int>(v->as_num(0));
    }
    if (a.dpi == 0.0f) {
        if (auto* v = root.find("dpi")) a.dpi = static_cast<float>(v->as_num(0));
    }

    if (const cjson::Value* steps = root.find("steps");
        steps && steps->type == cjson::Value::Arr) {
        for (const cjson::Value& s : *steps->arr) {
            if (const cjson::Value* c = s.find("click")) {
                a.steps.push_back({Step::Click, c->at_int(0), c->at_int(1)});
            } else if (const cjson::Value* h = s.find("hover")) {
                a.steps.push_back({Step::Hover, h->at_int(0), h->at_int(1)});
            } else if (const cjson::Value* w = s.find("wait_ms")) {
                Step st{Step::Wait};
                st.ms = static_cast<int>(w->as_num());
                a.steps.push_back(st);
            } else if (const cjson::Value* t = s.find("animation_time_ms")) {
                Step st{Step::AnimationTime};
                st.ms = static_cast<int>(t->as_num());
                a.steps.push_back(st);
            } else if (const cjson::Value* n = s.find("snapshot")) {
                Step st{Step::Snapshot};
                st.name = n->as_str();
                a.steps.push_back(st);
            }
        }
    }
    return true;
}

void dispatch_hover(affineui::Ui& ui, int x, int y) {
    affineui::Event e{};
    e.type = affineui::EventType::MouseMove;
    e.pos = {x, y};
    ui.dispatch(e);
}

void dispatch_click(affineui::Ui& ui, int x, int y) {
    affineui::Event e{};
    e.pos = {x, y};
    e.button = affineui::MouseButton::Left;
    e.type = affineui::EventType::MouseMove;
    ui.dispatch(e);
    e.type = affineui::EventType::MouseDown;
    ui.dispatch(e);
    e.type = affineui::EventType::MouseUp;
    ui.dispatch(e);
}

void apply_scripted_inputs(AppState& app) {
    for (const Step& step : app.args.steps) {
        switch (step.kind) {
            case Step::Click:
                dispatch_click(app.ui, step.x, step.y);
                break;
            case Step::Hover:
                dispatch_hover(app.ui, step.x, step.y);
                break;
            default:
                break;
        }
    }
}

int max_animation_time_ms(const std::vector<Step>& steps) {
    int out = 0;
    for (const Step& step : steps) {
        if (step.kind == Step::AnimationTime) out = std::max(out, step.ms);
    }
    return out;
}

void init_cb(void* user) {
    auto& app = *static_cast<AppState*>(user);
    sg_desc d{};
    d.environment = sglue_environment();
    d.logger.func = slog_func;
    sg_setup(&d);

    if (!app.ui.load(app.args.html)) {
        std::fprintf(stderr, "failed to load %s\n", app.args.html.c_str());
        sapp_request_quit();
        return;
    }

    app.loop_ms = max_animation_time_ms(app.args.steps);
    app.start = std::chrono::steady_clock::now();
}

void frame_cb(void* user) {
    auto& app = *static_cast<AppState*>(user);

    if (app.first_frame_done && !app.scripted_inputs_applied) {
        apply_scripted_inputs(app);
        app.scripted_inputs_applied = true;
        app.start = std::chrono::steady_clock::now();
    }

    if (app.loop_ms > 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - app.start).count();
        const double t = std::fmod(static_cast<double>(elapsed),
                                   static_cast<double>(app.loop_ms)) / 1000.0;
        app.ui.document().set_animation_time_for_testing(t);
    }

    const affineui::Color c = app.ui.clear_color();
    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value.r = c.r / 255.0f;
    pass.action.colors[0].clear_value.g = c.g / 255.0f;
    pass.action.colors[0].clear_value.b = c.b / 255.0f;
    pass.action.colors[0].clear_value.a = c.a / 255.0f;
    pass.action.depth.load_action = SG_LOADACTION_CLEAR;
    pass.action.depth.clear_value = 1.0f;
    pass.action.stencil.load_action = SG_LOADACTION_CLEAR;
    pass.action.stencil.clear_value = 0;
    pass.swapchain = sglue_swapchain();

    sg_begin_pass(&pass);
    affineui::sokol::render(app.ui);
    sg_end_pass();
    sg_commit();

    app.first_frame_done = true;
}

void event_cb(const sapp_event* ev, void* user) {
    auto& app = *static_cast<AppState*>(user);
    affineui::sokol::dispatch(app.ui, ev);
}

void cleanup_cb(void* user) {
    auto& app = *static_cast<AppState*>(user);
    app.ui.renderer().shutdown();
    sg_shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    static AppState app;
    if (!parse_args(argc, argv, app.args)) return 2;
    if (!load_case(app.args)) return 2;
    if (app.args.width <= 0) app.args.width = 1024;
    if (app.args.height <= 0) app.args.height = 768;
    if (app.args.dpi <= 0.0f) app.args.dpi = 1.0f;

    sapp_desc desc{};
    desc.width = app.args.width;
    desc.height = app.args.height;
    desc.window_title = "AffineUI conformance viewer";
    desc.high_dpi = true;
    desc.swap_interval = 1;
    desc.sample_count = 1;
    desc.user_data = &app;
    desc.init_userdata_cb = init_cb;
    desc.frame_userdata_cb = frame_cb;
    desc.event_userdata_cb = event_cb;
    desc.cleanup_userdata_cb = cleanup_cb;
    desc.logger.func = slog_func;
    if (desc.gl.major_version == 0) {
        desc.gl.major_version = 4;
        desc.gl.minor_version = 1;
    }
    sapp_run(&desc);
    return 0;
}
