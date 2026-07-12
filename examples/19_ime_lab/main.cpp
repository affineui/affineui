// ime_lab — the harness for verifying IME (Japanese / Chinese / Korean) input
// against a real platform IME.
//
// Real-IME behavior is the one thing we cannot test headlessly: the Imm* calls
// read from an IMM context the OS owns, so synthetic WM_IME_* messages have
// nothing to read (docs/IME_ARCHITECTURE.md §5). The composition protocol
// itself is covered by doctest; what needs a human is everything between the
// OS IME and the glass.
//
// So this sample does the one thing the unit tests can't: it puts the live IME
// state on screen next to the fields, so a tester can see at a glance whether
// the preedit, the clause range, the caret rect and the committed text all
// agree with what the IME is showing. Each check below maps to an item in
// IME_ARCHITECTURE.md §6 (PR B) that is marked "user IME test".
//
// Usage:
//   ime_lab                       # Japanese by default
//   ime_lab --lang=zh             # Chinese sample text / hints
//   ime_lab --lang=ko             # Korean
//   ime_lab --checklist           # print the manual test checklist, exit 0
//
// The tester needs an OS IME installed (Windows: Settings → Time & language →
// Language → add Japanese / Chinese, then switch with Win+Space).

#include <affineui/affineui.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Per-language sample text + the phrase to type. Kept as UTF-8 literals — this
// is exactly the surface that MSVC mangles without /utf-8 (set in the top-level
// CMakeLists), so if these render as mojibake rather than tofu, suspect the
// flag before suspecting the font fallback.
struct Lang {
    std::string_view code;
    std::string_view name;
    std::string_view type_this;   // what to type, in romaji/pinyin/etc.
    std::string_view expect;      // what should come out
    std::string_view sample;      // static text — exercises fallback + wrap
};

const Lang kLangs[] = {
    {"ja", "Japanese",
     "type  nihongo  then Space to convert",
     "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",  // 日本語
     // Long enough to wrap; includes kinsoku-relevant punctuation (。、」)
     // so a wrap must not orphan the closing marks onto a line start.
     "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x86\xE3\x82\xAD"
     "\xE3\x82\xB9\xE3\x83\x88\xE3\x81\xAF\xE3\x80\x81\xE7\xA9\xBA\xE7\x99\xBD"
     "\xE3\x81\xA7\xE5\x8D\x98\xE8\xAA\x9E\xE3\x82\x92\xE5\x8C\xBA\xE5\x88\x87"
     "\xE3\x82\x89\xE3\x81\xAA\xE3\x81\x84\xE3\x80\x82"},
    {"zh", "Chinese",
     "type  nihao  then pick a candidate",
     "\xE4\xBD\xA0\xE5\xA5\xBD",  // 你好
     "\xE4\xB8\xAD\xE6\x96\x87\xE6\x96\x87\xE6\x9C\xAC\xE6\xB2\xA1\xE6\x9C\x89"
     "\xE7\xA9\xBA\xE6\xA0\xBC\xEF\xBC\x8C\xE6\x8D\xA2\xE8\xA1\x8C\xE9\x9C\x80"
     "\xE8\xA6\x81\xE6\x8C\x89\xE5\xAD\x97\xE6\x96\xAD\xE5\xBC\x80\xE3\x80\x82"},
    {"ko", "Korean",
     "type  hangul  (2-beolsik)",
     "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4",  // 한국어
     "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4\x20\xED\x85\x8D\xEC\x8A\xA4\xED\x8A"
     "\xB8\xEC\x9E\x85\xEB\x8B\x88\xEB\x8B\xA4\x2E"},
};

const Lang& lang_from_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        std::string_view value;
        if (arg == "--lang" && i + 1 < argc) {
            value = argv[++i];
        } else if (arg.starts_with("--lang=")) {
            value = arg.substr(7);
        }
        if (value.empty()) continue;
        for (const auto& l : kLangs) {
            if (l.code == value) return l;
        }
    }
    return kLangs[0];
}

// The manual checklist. Printed by --checklist so the tester (or a release
// runbook) has it without opening this file. Each line is a claim that only a
// real IME can settle.
constexpr std::string_view kChecklist =
    "IME manual verification — run with a real OS IME (Win+Space to switch)\n"
    "\n"
    "  1. preedit inline      Typing shows the preedit INSIDE the field, at the\n"
    "                         caret — not in a floating OS box. Underlined.\n"
    "  2. clause highlight    During conversion (Space), the active clause has a\n"
    "                         THICKER underline than the rest of the preedit.\n"
    "  3. candidate window    The OS candidate list is anchored AT THE CARET,\n"
    "                         not at the window/screen corner. Still correct\n"
    "                         after scrolling, and on a hi-DPI monitor.\n"
    "  4. commit              Enter replaces the preedit with committed text.\n"
    "                         'live value' below updates; 'preedit' clears.\n"
    "  5. continuous input    Typing straight into a new preedit right after a\n"
    "                         commit works (commit + new preedit can arrive in\n"
    "                         a single WM_IME_COMPOSITION).\n"
    "  6. Esc cancels         Esc drops the preedit WITHOUT committing. The\n"
    "                         field's live value must be unchanged.\n"
    "  7. click commits       Left-clicking elsewhere during composition commits\n"
    "                         the preedit (CPS_COMPLETE), then the click lands.\n"
    "  8. no hotkey swallow   With NO text field focused, the IME must be off:\n"
    "                         plain keys must not open a preedit. Click the\n"
    "                         background, then type — nothing should appear.\n"
    "  9. CJK renders         The static sample text below shows real glyphs,\n"
    "                         NOT tofu boxes (font fallback) and NOT mojibake\n"
    "                         (that would mean MSVC /utf-8 is missing).\n"
    " 10. CJK wraps + kinsoku The sample wraps inside its box, and no line STARTS\n"
    "                         with closing punctuation (。 、 」 ) — kinsoku.\n"
    " 11. caret in preedit    Arrow-keying within a long preedit keeps the caret\n"
    "                         rect tracking the IME cursor (watch 'caret').\n"
    " 12. multiline           All of the above also works in the textarea.\n";

class ImeLab {
public:
    explicit ImeLab(affineui::App& app, const Lang& lang)
        : app_{app}, lang_{lang} {}

    void build(affineui::View& v) {
        v.set_framework_version(affineui::decius::default_version);
        v.selector(affineui::decius::selector::style,
                   affineui::decius::style::flat);
        v.selector(affineui::decius::selector::density,
                   affineui::decius::density::comfortable);
        v.selector(affineui::decius::selector::accent, "cyan");

        auto page = v.container(
            "dcs-panel dcs-panel--bordered dcs-panel--raised ime-lab-page",
            "ime-lab-page");
        {
            auto header = v.container("dcs-panel__header", "ime-page-header");
            v.text(std::string{"IME lab — "} + std::string{lang_.name},
                   "ime-title").cls("dcs-panel__title");
        }
        auto page_body = v.container("dcs-panel__body dcs-form",
                                     "ime-page-body");

        v.paragraph(std::string{"Type "} + std::string{lang_.type_this} +
                        "  \xE2\x86\x92  expect " +
                        std::string{lang_.expect},
                    "dcs-note", "ime-instructions");

        // Input + textarea have separate caret tables. Standard form markup
        // supplies field sizing and density-controlled spacing.
        {
            auto card = v.card("Text entry", "aui-demo-section",
                               "ime-entry-card");
            auto body = v.container("dcs-panel__body dcs-form",
                                    "ime-entry-body");
            v.input("Single line", single_, "text", "ime-input")
                .on_change([this](std::string_view s) {
                    single_ = std::string{s};
                });
            v.textarea("Multiline", multi_, 4, "ime-textarea")
                .on_change([this](std::string_view s) {
                    multi_ = std::string{s};
                });
        }

        // Show exactly what reached the document so platform and core failures
        // remain distinguishable during a manual IME run.
        {
            auto card = v.card("Live IME state", "aui-demo-section",
                               "ime-state-card");
            auto body = v.container("dcs-panel__body dcs-form",
                                    "ime-state-body");
            const auto& doc = app_.document();
            const bool active = doc.text_input_active();
            const auto caret = doc.caret_rect();

            v.paragraph(std::string{"Input method: "} +
                            (active ? "enabled" : "off — focus a field"),
                        active ? "" : "dcs-note", "ime-active");
            v.paragraph("Caret: " + rect_str(caret) +
                            (caret.w <= 0 ? " (no focused field)" : ""),
                        {}, "ime-caret");
            v.paragraph("Preedit: " + (preedit_.empty()
                                           ? std::string{"(none)"}
                                           : "\"" + preedit_ + "\""),
                        {}, "ime-preedit");
            v.paragraph("Clause: [" + std::to_string(clause_begin_) + ", " +
                            std::to_string(clause_end_) + ")   cursor: " +
                            std::to_string(cursor_),
                        {}, "ime-clause");
            v.paragraph("Committed: \"" + single_ + "\"",
                        {}, "ime-committed");
            v.paragraph("Compositions: " + std::to_string(compositions_) +
                            "   commits: " + std::to_string(commits_),
                        "dcs-note", "ime-counts");
        }

        // Static rendering separates font fallback failures from platform IME
        // failures.
        {
            auto card = v.card("Static CJK rendering", "aui-demo-section",
                               "ime-static-card");
            auto body = v.container("dcs-panel__body dcs-form",
                                    "ime-static-body");
            v.paragraph(std::string{lang_.sample}, "ime-sample",
                        "ime-static-sample");
            v.paragraph("Expect real glyphs, normal wrapping, and no line "
                        "starting with closing punctuation.",
                        "dcs-note", "ime-static-note");
        }

        // A standard button is the deliberate non-text focus target.
        {
            auto card = v.card("Focus routing", "aui-demo-section",
                               "ime-focus-card");
            auto body = v.container("dcs-panel__body dcs-form",
                                    "ime-focus-body");
            v.paragraph("Focus this button, then type. No preedit should "
                        "appear and the input method status should be off.",
                        "dcs-note", "ime-focus-note");
            v.button("Move focus out of text entry", false, "ime-sink");
        }
    }

    // Observe every composition/commit so the readout reflects what the engine
    // actually saw, not what we hope it saw. Wired through App::on_event, which
    // hands us the event after the document has already routed it.
    //
    // ALWAYS returns false: we are a passive observer. Consuming the event here
    // would swallow the very IME input we exist to display.
    //
    // The rebuild is deferred to the next frame rather than issued from inside
    // dispatch — rebuilding the view mid-dispatch reenters the reconciler while
    // it is walking the tree that raised the event.
    bool note(const affineui::Event& ev) {
        if (ev.type == affineui::EventType::Composition) {
            preedit_ = ev.text;
            cursor_ = ev.composition_cursor;
            clause_begin_ = ev.composition_clause_begin;
            clause_end_ = ev.composition_clause_end;
            if (!preedit_.empty()) ++compositions_;
            dirty_ = true;
        } else if (ev.type == affineui::EventType::TextInput) {
            preedit_.clear();
            cursor_ = clause_begin_ = clause_end_ = 0;
            ++commits_;
            dirty_ = true;
        }
        return false;
    }

    // Rebuild once per frame if an IME event moved the readout. Cheap: the
    // reconciler diffs, so an unchanged frame emits no patches.
    void tick() {
        if (!dirty_) return;
        dirty_ = false;
        app_.rebuild_view();
    }

private:
    static std::string rect_str(const affineui::Rect& r) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "x=%.1f y=%.1f w=%.1f h=%.1f",
                      static_cast<double>(r.x), static_cast<double>(r.y),
                      static_cast<double>(r.w), static_cast<double>(r.h));
        return buf;
    }

    affineui::App& app_;
    const Lang&    lang_;

    std::string single_;
    std::string multi_;

    std::string preedit_;
    int  cursor_{0};
    int  clause_begin_{0};
    int  clause_end_{0};
    int  compositions_{0};
    int  commits_{0};
    bool dirty_{false};
};

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--checklist") {
            std::fputs(kChecklist.data(), stdout);
            return 0;
        }
    }

    const Lang& lang = lang_from_args(argc, argv);

    affineui::App::Config config;
    config.title = std::string{"AffineUI - IME lab ("} +
                   std::string{lang.name} + ")";
    config.width = 900;
    config.height = 760;
    config.asset_folders = {"examples", "."};

    affineui::App app{config};
    // Layout and spacing come from the standard Decius panel/form components
    // and the selected density. The one app-specific rule makes the test page
    // the window's scroll surface.
    app.set_stylesheet(
        ".aui-root{height:100vh;min-height:0}"
        ".ime-lab-page .dcs-field>.dcs-input{flex:1;min-width:0}"
        ".ime-sample{max-width:22em;line-height:1.9;font-size:1.25rem}");

    ImeLab lab{app, lang};
    app.set_view([&lab](affineui::View& v) { lab.build(v); });
    // Without these two the readout never updates — and a readout that never
    // updates is worse than none, because it reads as "the engine saw nothing."
    app.on_event([&lab](const affineui::Event& ev,
                        const std::vector<affineui::Document::HoverInfo>&) {
        return lab.note(ev);
    });
    app.on_frame([&lab](double) { lab.tick(); });

    std::fputs(kChecklist.data(), stdout);
    std::fflush(stdout);

    return app.run();
}
