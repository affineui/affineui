// decius_dender - a native C++ port of the DENDER web sample (a compact
// Blender-style DCC): declarative docking, a real scene document with undo,
// and the full Decius chrome.

#include "dender_app.h"
#include "dender_styles.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
// TEMP DEBUG (AFFINEUI_VEH=1): report the FIRST access violation with a
// raw stack, before the win32 kernel-callback filter can swallow it
// (x64 WndProc callbacks silently eat SEH faults, so the app limps on
// over corrupted state and dies somewhere unrelated).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static LONG WINAPI dender_first_chance(PEXCEPTION_POINTERS x) {
    if (x->ExceptionRecord->ExceptionCode == 0xC0000005u) {
        void* frames[48];
        const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
        std::fprintf(stderr, "[veh] AV at %p accessing %p (exe base %p)\n",
                     x->ExceptionRecord->ExceptionAddress,
                     reinterpret_cast<void*>(
                         x->ExceptionRecord->ExceptionInformation[1]),
                     static_cast<void*>(GetModuleHandleW(nullptr)));
        for (USHORT i = 0; i < n; ++i) {
            std::fprintf(stderr, "[veh]   %p\n", frames[i]);
        }
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 42);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    if (std::getenv("AFFINEUI_VEH")) {
        AddVectoredExceptionHandler(1, dender_first_chance);
    }
#endif
    dender::DenderApp app;

    // Headless timing of the per-interaction reload path (no window).
    // --probe runs only the reconcile-churn analysis (fast).
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--profile") return app.profile(30);
        if (std::string(argv[i]) == "--probe") return app.profile(0);
    }

    // Headless escape hatch for conformance/CI: dump the generated document
    // exactly as the running app renders it — the framework bundle plus this
    // app's own CSS as the user stylesheet — for browser A/B comparison.
    // (AFFINEUI_DUMP_HTML=1)
    if (const char* dump = std::getenv("AFFINEUI_DUMP_HTML");
        dump != nullptr && dump[0] == '1') {
        affineui::View v = app.build_view();
        std::string doc = v.to_html_document();
        std::string css = app::read_framework_bundle(
            affineui::ViewTheme::Decius, dender::kDeciusVersion);
        css += "\n";
        css += dender::native_css();
        // The app shell is position:fixed;inset:0 — neutralise the View's
        // default .aui-root padding/min-height wrapper in the headless dump.
        css += "\n.aui-root{min-height:0;padding:0}\n";
        if (const auto pos = doc.find("</style>"); pos != std::string::npos) {
            doc.insert(pos, css);
        }
        for (const auto& diag : v.diagnostics()) {
            std::fprintf(stderr, "[view] %s\n", diag.c_str());
        }
        std::fputs(doc.c_str(), stdout);
        return 0;
    }
    return app.run();
}
