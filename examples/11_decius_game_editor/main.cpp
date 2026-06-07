// decius_game_editor — a dense game/scene editor showcase.
//
// Built entirely on the AffineUI component API (strongly-typed components +
// structural builders) and the affineui_app DCC template (document, selection,
// undo/redo commands). There is no hand-written HTML: every widget is a typed
// component, and every callback is a safe affineui::bind() member.

#include "game_editor.h"
#include "game_editor_styles.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    ge::GameEditor editor;
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
