// decius_game_editor — a dense game/scene editor showcase.
//
// Built entirely on the AffineUI component API (strongly-typed components +
// structural builders) and the affineui_app DCC template (document, selection,
// undo/redo commands). There is no hand-written HTML: every widget is a typed
// component, and every callback is a safe affineui::bind() member.

#include "game_editor.h"
#include "game_editor_styles.h"
#include "interactive_tests.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

struct Args {
    std::string_view lab;
    std::string_view game_scope;
    bool help{false};
};

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i] ? argv[i] : "";
        if (arg == "--lab-help" || arg == "--help-labs") {
            args.help = true;
        } else if (arg == "--lab" && i + 1 < argc) {
            args.lab = argv[++i];
        } else if (arg.rfind("--lab=", 0) == 0) {
            args.lab = arg.substr(6);
        } else if (arg == "--game-scope" && i + 1 < argc) {
            args.game_scope = argv[++i];
        } else if (arg.rfind("--game-scope=", 0) == 0) {
            args.game_scope = arg.substr(13);
        }
    }
    return args;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    if (args.help) {
        ge::print_interactive_test_usage();
        return 0;
    }
    if (!args.lab.empty()) {
        return ge::run_interactive_test(args.lab);
    }

    ge::GameEditor editor;
    if (!args.game_scope.empty()) {
        editor.set_debug_scope(args.game_scope);
    }

    // Headless escape hatch for conformance/CI: dump the generated document
    // EXACTLY as the running app renders it — the framework bundle plus this
    // app's own CSS as the user stylesheet (not the View's default boilerplate),
    // so the A/B render matches what the window shows. (AFFINEUI_DUMP_HTML=1)
    if (const char* dump = std::getenv("AFFINEUI_DUMP_HTML"); dump && dump[0] == '1') {
        // Start from the View's full document (correct <body> framework attrs:
        // theme/version + data-dcs-style/density/accent selectors), then inject
        // the SAME stylesheet the running app applies — the framework bundle
        // plus this app's own CSS — so the headless A/B matches the window.
        affineui::View v = editor.build();
        std::string doc = v.to_html_document();
        std::string css =
            app::read_framework_bundle(affineui::ViewTheme::Decius, "0.6.2");
        css += "\n";
        css += ge::native_css();
        // The app shell is position:fixed; inset:0, so neutralise the View's
        // default .aui-root padding/min-height wrapper used in the headless dump.
        css += "\n.aui-root{min-height:0;padding:0}\n";

        // Splice our CSS in just before </style> (after the command-widget
        // boilerplate, so our rules win on equal specificity).
        if (const auto pos = doc.find("</style>"); pos != std::string::npos) {
            doc.insert(pos, css);
        }
        std::fputs(doc.c_str(), stdout);
        return 0;
    }
    return editor.run();
}
