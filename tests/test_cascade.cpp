// Cascade tests — exercises the lexbor-backed StyleResolver directly,
// without involving the window or paint subsystems. These are the
// proof that Phase 2A's "real CSS works" claim is true.

#include <doctest/doctest.h>

#if !defined(AFFINEUI_STUB_BUILD)

#    include "internal/animated_style.h"
#    include "internal/computed_style.h"
#    include "internal/style_resolver.h"

#    include <lexbor/css/css.h>
#    include <lexbor/dom/dom.h>
#    include <lexbor/html/html.h>

#    include <cstring>
#    include <memory>
#    include <string>
#    include <vector>

namespace {

// Tiny RAII wrapper so test bodies stay readable.
struct CssEnv {
    lxb_html_document_t* doc{nullptr};
    lxb_css_parser_t*    parser{nullptr};
    std::vector<lxb_css_stylesheet_t*> sheets;
    std::unique_ptr<affineui::detail::StyleResolver> resolver;

    explicit CssEnv(std::string_view html) {
        doc = lxb_html_document_create();
        REQUIRE(doc != nullptr);
        REQUIRE(lxb_html_document_css_init(doc) == LXB_STATUS_OK);
        REQUIRE(lxb_html_document_parse(
            doc,
            reinterpret_cast<const lxb_char_t*>(html.data()),
            html.size()) == LXB_STATUS_OK);
        parser = lxb_css_parser_create();
        REQUIRE(lxb_css_parser_init(parser, nullptr) == LXB_STATUS_OK);
    }

    ~CssEnv() {
        resolver.reset();
        for (auto* s : sheets) lxb_css_stylesheet_destroy(s, true);
        if (parser) lxb_css_parser_destroy(parser, true);
        if (doc) lxb_html_document_destroy(doc);
    }

    void attach(std::string_view css) {
        auto* sst = lxb_css_stylesheet_parse(
            parser,
            reinterpret_cast<const lxb_char_t*>(css.data()),
            css.size());
        REQUIRE(sst != nullptr);
        REQUIRE(lxb_html_document_stylesheet_attach(doc, sst) == LXB_STATUS_OK);
        sheets.push_back(sst);
    }

    void build_resolver() {
        resolver = affineui::detail::make_lexbor_resolver(doc);
        REQUIRE(resolver != nullptr);
    }

    // Find the first element with the given tag. Linear scan — fine
    // for these tiny test fixtures.
    lxb_dom_element_t* find(const char* tag) const {
        auto find_in = [&](auto&& self, lxb_dom_node_t* node) -> lxb_dom_element_t* {
            for (auto* c = lxb_dom_node_first_child(node); c;
                 c = lxb_dom_node_next(c)) {
                if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                    auto* el = lxb_dom_interface_element(c);
                    size_t len = 0;
                    const auto* name = lxb_dom_element_qualified_name(el, &len);
                    if (name && len == std::strlen(tag) &&
                        std::memcmp(name, tag, len) == 0) {
                        return el;
                    }
                    if (auto* nested = self(self, c)) return nested;
                }
            }
            return nullptr;
        };
        return find_in(find_in, lxb_dom_interface_node(doc));
    }
};

// Helper to construct an RGBA color literal (matches AnimatedStyle's
// packed layout).
constexpr std::uint32_t rgba(std::uint8_t r, std::uint8_t g,
                             std::uint8_t b, std::uint8_t a = 0xFF) {
    return (std::uint32_t(r) << 24) | (std::uint32_t(g) << 16) |
           (std::uint32_t(b) <<  8) |  std::uint32_t(a);
}

}  // namespace

TEST_CASE("a hex color on a type selector reaches the resolved style") {
    CssEnv env("<h1>hi</h1>");
    env.attach("h1 { color: #f38ba8; }");
    env.build_resolver();

    auto* h1 = env.find("h1");
    REQUIRE(h1 != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(h1, parent);
    CHECK(rs.animated.color_rgba == rgba(0xF3, 0x8B, 0xA8));
}

TEST_CASE("color inherits from parent through the resolver call chain") {
    CssEnv env("<body><h1>hi</h1></body>");
    env.attach("body { color: #cdd6f4; }");
    env.build_resolver();

    auto* body = env.find("body");
    auto* h1   = env.find("h1");
    REQUIRE(body != nullptr);
    REQUIRE(h1   != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto body_rs = env.resolver->resolve(body, root);
    const auto h1_rs   = env.resolver->resolve(h1,  body_rs);

    // body's color won
    CHECK(body_rs.animated.color_rgba == rgba(0xCD, 0xD6, 0xF4));
    // h1 has no color of its own → must inherit body's.
    CHECK(h1_rs.animated.color_rgba   == rgba(0xCD, 0xD6, 0xF4));
}

TEST_CASE("a more-specific rule overrides a less-specific one") {
    CssEnv env("<body><h1>hi</h1></body>");
    env.attach("body { color: #cdd6f4; }"
               "h1   { color: #f38ba8; }");
    env.build_resolver();

    auto* body = env.find("body");
    auto* h1   = env.find("h1");
    REQUIRE(h1 != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto body_rs = env.resolver->resolve(body, root);
    const auto h1_rs   = env.resolver->resolve(h1,  body_rs);

    CHECK(h1_rs.animated.color_rgba == rgba(0xF3, 0x8B, 0xA8));
}

TEST_CASE("lexbor counts simple pseudo-classes in selector specificity") {
    CssEnv env("<button class=\"btn\">hi</button>");
    env.attach(".btn {} .btn:focus {}");

    auto* sheet = env.sheets.back();
    auto* rules = lxb_css_rule_list(sheet->root);
    REQUIRE(rules->first != nullptr);
    REQUIRE(rules->first->next != nullptr);

    auto* base_rule = lxb_css_rule_style(rules->first);
    auto* focus_rule = lxb_css_rule_style(rules->first->next);
    REQUIRE(base_rule->selector != nullptr);
    REQUIRE(focus_rule->selector != nullptr);

    CHECK(focus_rule->selector->specificity >
          base_rule->selector->specificity);
}

TEST_CASE("border side color longhands reach the resolved border color") {
    CssEnv env("<button>hi</button>");
    env.attach("button { border-top-color: #445566; }");
    env.build_resolver();

    auto* button = env.find("button");
    REQUIRE(button != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(button, parent);
    // border-top-color sets only the top side; the paint path uses the
    // per-side color in preference to the unified border_rgba fallback.
    CHECK(rs.animated.border_top_rgba == rgba(0x44, 0x55, 0x66));
}

TEST_CASE("rgba colors resolve to packed alpha") {
    CssEnv env("<button>hi</button>");
    env.attach("button { color: rgba(255, 255, 255, .5);"
               "border-color: rgba(0, 0, 0, .125); }");
    env.build_resolver();

    auto* button = env.find("button");
    REQUIRE(button != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(button, parent);
    CHECK(rs.animated.color_rgba == rgba(255, 255, 255, 128));
    CHECK(rs.animated.border_rgba == rgba(0, 0, 0, 32));
}

TEST_CASE("box-shadow reaches the resolved animated style") {
    CssEnv env("<button>hi</button>");
    env.attach("button { box-shadow: 0 0 0 .25rem rgba(13, 110, 253, .25); }");
    env.build_resolver();

    auto* button = env.find("button");
    REQUIRE(button != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(button, parent);
    CHECK(rs.animated.shadow_rgba == rgba(13, 110, 253, 64));
    CHECK(rs.animated.shadow_offset_x == 0);
    CHECK(rs.animated.shadow_offset_y == 0);
    CHECK(rs.animated.shadow_blur == 0);
    CHECK(rs.animated.shadow_spread == 4);
}

TEST_CASE("framework shorthands reach resolved style") {
    CssEnv env("<section><button class=\"btn\">hi</button></section>");
    env.attach("section { background: #fff url(example.png) right center / 8px 10px no-repeat; gap: 1.25rem 2rem; }"
               ".btn { border-radius: .25rem; }");
    env.build_resolver();

    auto* section = env.find("section");
    auto* button = env.find("button");
    REQUIRE(section != nullptr);
    REQUIRE(button != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto section_rs = env.resolver->resolve(section, parent);
    const auto button_rs = env.resolver->resolve(button, section_rs);

    CHECK(section_rs.animated.background_rgba == rgba(0xFF, 0xFF, 0xFF));
    CHECK(section_rs.computed.row_gap == 20);
    CHECK(section_rs.computed.column_gap == 32);
    CHECK(button_rs.computed.border_radius_top_left_px == 4);
    CHECK(button_rs.computed.border_radius_top_right_px == 4);
    CHECK(button_rs.computed.border_radius_bot_right_px == 4);
    CHECK(button_rs.computed.border_radius_bot_left_px == 4);
}

TEST_CASE("a longhand reset overrides an equal-specificity shorthand") {
    // The Bootstrap-Reboot pattern: the UA sheet sets a heading's box
    // with the `margin` shorthand, then a later author rule of EQUAL
    // specificity zeroes one side with the `margin-top` longhand. CSS
    // source order says the longhand wins. lexbor keeps the shorthand and
    // the longhand as separate per-property cascade winners, so the
    // resolver must apply the shorthand before the longhand for the reset
    // to take effect (see shorthand_rank in cascade.cpp).
    CssEnv env("<h6>hi</h6>");
    env.attach("h6 { margin: 16px; }");          // UA-like shorthand (first)
    env.attach("h6 { margin-top: 0; }");          // reset longhand (later)
    env.build_resolver();

    auto* h6 = env.find("h6");
    REQUIRE(h6 != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(h6, parent);
    CHECK(rs.computed.margin_top == 0);    // longhand reset won
    CHECK(rs.computed.margin_bottom == 16);
    CHECK(rs.computed.margin_left == 16);
    CHECK(rs.computed.margin_right == 16);
}

TEST_CASE("a more-specific longhand overrides a shorthand regardless of order") {
    // The shorthand is declared LATER but is less specific; the
    // higher-specificity longhand must still win for its side. This
    // exercises the specificity-ascending half of the apply ordering.
    CssEnv env("<h6 class=\"sub\">hi</h6>");
    env.attach(".sub { margin-top: 2px; }");      // longhand, spec (0,1,0)
    env.attach("h6 { margin: 16px; }");            // shorthand, spec (0,0,1)
    env.build_resolver();

    auto* h6 = env.find("h6");
    REQUIRE(h6 != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(h6, parent);
    CHECK(rs.computed.margin_top == 2);    // more-specific longhand won
    CHECK(rs.computed.margin_bottom == 16);
    CHECK(rs.computed.margin_left == 16);
}

TEST_CASE("var() color with alpha resolves through :root inheritance") {
    // Bootstrap .text-body-secondary pattern: a :root custom property
    // holds an rgba() with alpha, and a class colours text with
    // color:var(...)!important while a lower-specificity reboot rule also
    // sets color via a (here undefined) var. The important var() must win
    // and resolve to the alpha colour, not fall back to inherited text.
    CssEnv env("<html><body><h6 class=\"sub\">x</h6></body></html>");
    env.attach(":root { --c: rgba(33, 37, 41, 0.75); }");
    env.attach("body { color: #1f2328; }");
    env.attach("h6 { color: var(--undef-heading); }");      // reboot-like
    env.attach(".sub { color: var(--c) !important; }");      // wins
    env.build_resolver();

    auto* html = env.find("html");
    auto* body = env.find("body");
    auto* h6   = env.find("h6");
    REQUIRE(html != nullptr);
    REQUIRE(body != nullptr);
    REQUIRE(h6 != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto html_rs = env.resolver->resolve(html, root);
    const auto body_rs = env.resolver->resolve(body, html_rs);
    const auto h6_rs   = env.resolver->resolve(h6, body_rs);
    CHECK(h6_rs.animated.color_rgba == rgba(33, 37, 41, 191));  // .75*255
}

TEST_CASE("calc() evaluates length arithmetic, including with var()") {
    // The Bootstrap .card-subtitle pattern: a negative margin from
    // calc(-.5 * var(--spacer)). Also exercises mixed +/* and px/rem.
    CssEnv env("<div class=\"sub\">x</div>");
    env.attach(":root { --spacer: 0.5rem; }");          // 8px
    env.attach(".sub { margin-top: calc(-.5 * var(--spacer));"
               "       margin-left: calc(100px / 4 + 2px);"
               "       margin-right: calc(1rem + 4px); }");
    env.build_resolver();

    auto* html = env.find("html");
    auto* sub  = env.find("div");
    REQUIRE(html != nullptr);
    REQUIRE(sub != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto html_rs = env.resolver->resolve(html, root);
    const auto rs      = env.resolver->resolve(sub, html_rs);
    CHECK(rs.computed.margin_top   == -4);   // -.5 * 8px
    CHECK(rs.computed.margin_left  == 27);   // 100/4 + 2
    CHECK(rs.computed.margin_right == 20);   // 16 + 4
}

TEST_CASE("calc() with an unsupported percentage is dropped, not misapplied") {
    CssEnv env("<div class=\"sub\">x</div>");
    env.attach(".sub { margin-top: calc(100% - 10px); }");
    env.build_resolver();
    auto* sub = env.find("div");
    REQUIRE(sub != nullptr);
    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(sub, parent);
    // The calc() carries a % we can't resolve without layout context, so
    // it is left verbatim and dropped on re-parse — margin stays initial
    // (0) rather than being misparsed into a garbage value.
    CHECK(rs.computed.margin_top == 0);
}

TEST_CASE("universal selector applies box-sizing to an element") {
    // Bootstrap relies on `*,*::before,*::after{box-sizing:border-box}`.
    // Without it, a %-width flex column with gutter padding overflows and
    // wraps (cols are flex:0 0 auto, so they can't shrink).
    CssEnv env("<div class=\"col\">x</div>");
    env.attach("*{box-sizing:border-box}");
    env.build_resolver();
    auto* col = env.find("div");
    REQUIRE(col != nullptr);
    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(col, parent);
    CHECK(rs.computed.box_sizing ==
          affineui::detail::ComputedStyle::BoxSizing::BorderBox);
}

TEST_CASE("universal selector grouped with pseudo-elements still applies (Reboot)") {
    // Bootstrap Reboot's exact selector. If lexbor mishandles the
    // *::before / *::after members of the list, the `*` member must still
    // match real elements — otherwise box-sizing:border-box never lands
    // and %-width grid columns overflow.
    CssEnv env("<div class=\"col\">x</div>");
    env.attach("*,*::before,*::after{box-sizing:border-box}");
    env.build_resolver();
    auto* col = env.find("div");
    REQUIRE(col != nullptr);
    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(col, parent);
    CHECK(rs.computed.box_sizing ==
          affineui::detail::ComputedStyle::BoxSizing::BorderBox);
}

TEST_CASE("selector list with pseudo-element members still applies to elements") {
    // A CSS selector list is non-forgiving, but ::before/::after are
    // VALID selectors — they just never match in an engine with no
    // pseudo-element nodes. lexbor used to treat them as a parse error,
    // which discarded the whole rule (Bootstrap's
    // `*,*::before,*::after{box-sizing:border-box}` lost its reset).
    auto red = [](const char* css) {
        CssEnv env("<div class=\"col\">x</div>");
        env.attach(css);
        env.build_resolver();
        auto* col = env.find("div");
        const affineui::detail::ResolvedStyle parent{};
        return env.resolver->resolve(col, parent).animated.color_rgba ==
               rgba(0xFF, 0x00, 0x00);
    };
    CHECK(red("div,div::before{color:red}"));          // pseudo last
    CHECK(red("div::before,div{color:red}"));          // pseudo first
    CHECK(red("div,*::before,*::after{color:red}"));    // Reboot shape
    CHECK(red("div::after{color:blue} div{color:red}")); // pseudo-only rule is inert
}

TEST_CASE("border-radius longhands reach computed style") {
    CssEnv env("<button>hi</button>");
    env.attach("button { border-top-left-radius: 8px 12px;"
               "border-bottom-right-radius: 6px; }");
    env.build_resolver();

    auto* button = env.find("button");
    REQUIRE(button != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(button, parent);
    CHECK(rs.computed.border_radius_top_left_px == 8);
    CHECK(rs.computed.border_radius_bot_right_px == 6);
}

TEST_CASE("flex sizing properties reach computed style") {
    CssEnv env("<main><section>col</section><article>body</article></main>");
    env.attach("section { flex-basis: 0; flex-grow: 1; max-width: 42px; min-width: 0; }"
               "article { flex: 1 0 0%; }");
    env.build_resolver();

    auto* section = env.find("section");
    auto* article = env.find("article");
    REQUIRE(section != nullptr);
    REQUIRE(article != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto section_rs = env.resolver->resolve(section, parent);
    const auto article_rs = env.resolver->resolve(article, parent);

    CHECK(section_rs.computed.flex_basis == 0);
    CHECK(section_rs.computed.flex_grow == 1);
    CHECK(section_rs.computed.max_width == 42);
    CHECK(section_rs.computed.min_width == 0);
    CHECK(article_rs.computed.flex_grow == 1);
    CHECK(article_rs.computed.flex_shrink == 0);
    // `flex: 1 0 0%` — the basis is a *percentage*, so it lands in
    // flex_basis_pct (0), leaving the px field unset (-1 = pct governs).
    CHECK(article_rs.computed.flex_basis_pct == 0);
    CHECK(article_rs.computed.flex_basis == -1);
}

TEST_CASE("font-size in px lands in ComputedStyle, not AnimatedStyle") {
    CssEnv env("<h1>hi</h1>");
    env.attach("h1 { font-size: 42px; }");
    env.build_resolver();

    auto* h1 = env.find("h1");
    REQUIRE(h1 != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(h1, parent);
    CHECK(rs.computed.font_size_px == 42);
}

TEST_CASE("rem lengths resolve against the default root font size") {
    CssEnv env("<button>hi</button>");
    env.attach("button { padding: .5rem 1rem; }");
    env.build_resolver();

    auto* button = env.find("button");
    REQUIRE(button != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(button, parent);
    CHECK(rs.computed.padding_top == 8);
    CHECK(rs.computed.padding_right == 16);
    CHECK(rs.computed.padding_bottom == 8);
    CHECK(rs.computed.padding_left == 16);
}

TEST_CASE(":active overlay layered via apply_decl_list updates the color") {
    // Sibling of the :hover test below. The overlay primitive is
    // pseudo-class agnostic — :active rides the same machinery,
    // verified independently here so a regression on one doesn't
    // mask the other.
    CssEnv env("<button>hi</button>");
    env.attach("button { color: #00ff00; }");
    env.attach("button:active { color: #0000ff; }");
    env.build_resolver();

    auto* btn = env.find("button");
    REQUIRE(btn != nullptr);

    affineui::detail::ResolvedStyle rs = env.resolver->resolve(btn, {});
    CHECK(rs.animated.color_rgba == rgba(0x00, 0xFF, 0x00));

    auto* active_sheet = env.sheets.back();
    auto* rule_list    = lxb_css_rule_list(active_sheet->root);
    REQUIRE(rule_list->first != nullptr);
    auto* style_rule   = lxb_css_rule_style(rule_list->first);

    env.resolver->apply_decl_list(style_rule->declarations, rs);
    CHECK(rs.animated.color_rgba == rgba(0x00, 0x00, 0xFF));
}

TEST_CASE(":hover overlay layered via apply_decl_list updates the color") {
    // Two sheets: the first is the base (always matched), the second
    // is the :hover overlay (matched separately by our side-table).
    // Lexbor's cascade skips the :hover rule for the base resolve
    // because no `hover` HTML attribute is present on the element —
    // verified by the first CHECK below. Applying the overlay's
    // declarations via apply_decl_list then flips the color.
    CssEnv env("<button>hi</button>");
    env.attach("button { color: #00ff00; }");
    env.attach("button:hover { color: #ff0000; }");
    env.build_resolver();

    auto* btn = env.find("button");
    REQUIRE(btn != nullptr);

    affineui::detail::ResolvedStyle rs = env.resolver->resolve(btn, {});
    CHECK(rs.animated.color_rgba == rgba(0x00, 0xFF, 0x00));

    // Pluck the :hover rule's declarations out of the second sheet
    // and apply them as if the element had transitioned into the
    // hovered state.
    auto* hover_sheet = env.sheets.back();
    auto* rule_list   = lxb_css_rule_list(hover_sheet->root);
    REQUIRE(rule_list->first != nullptr);
    auto* style_rule  = lxb_css_rule_style(rule_list->first);

    env.resolver->apply_decl_list(style_rule->declarations, rs);
    CHECK(rs.animated.color_rgba == rgba(0xFF, 0x00, 0x00));
}

TEST_CASE("font shorthand sets font_size_px and line_height_x100") {
    CssEnv env("<p>hi</p>");
    env.attach("p { font: bold 32px/1.5 sans-serif; }");
    env.build_resolver();

    auto* p = env.find("p");
    REQUIRE(p != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(p, parent);
    // font-size: 32px
    CHECK(rs.computed.font_size_px == 32);
    // line-height: 1.5 → stored as 1.5 * 100 = 150
    CHECK(rs.computed.line_height_x100 == 150);
}

TEST_CASE("font shorthand without line-height sets font_size_px only") {
    CssEnv env("<p>hi</p>");
    env.attach("p { font: 14px Arial; }");
    env.build_resolver();

    auto* p = env.find("p");
    REQUIRE(p != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(p, parent);
    CHECK(rs.computed.font_size_px == 14);
    // line-height not set (normal) → 0
    CHECK(rs.computed.line_height_x100 == 0);
}

// ── Table cell selector matching diagnostics ─────────────────────────

// Helper: find a specific element with a given tag AND the Nth occurrence
// (0-based) among same-tag elements in document order.
lxb_dom_element_t* find_nth(lxb_dom_node_t* root, const char* tag, int nth) {
    int count = 0;
    std::function<lxb_dom_element_t*(lxb_dom_node_t*)> dfs =
        [&](lxb_dom_node_t* node) -> lxb_dom_element_t* {
        for (auto* c = lxb_dom_node_first_child(node); c;
             c = lxb_dom_node_next(c)) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                auto* el = lxb_dom_interface_element(c);
                size_t len = 0;
                const auto* name = lxb_dom_element_qualified_name(el, &len);
                if (name && len == std::strlen(tag) &&
                    std::memcmp(name, tag, len) == 0) {
                    if (count++ == nth) return el;
                }
                if (auto* nested = dfs(c)) return nested;
            }
        }
        return nullptr;
    };
    return dfs(root);
}

TEST_CASE("table child selector matches td for padding") {
    // Minimal table with .table class — tests that .table>:not(caption)>*>*
    // correctly selects <td> elements.
    CssEnv env(
        "<table class=\"table\"><thead><tr><th>A</th></tr></thead>"
        "<tbody><tr><td>B</td></tr></tbody></table>");
    env.attach(".table>:not(caption)>*>*{padding:8px 12px}");
    env.build_resolver();

    auto* td = env.find("td");
    REQUIRE(td != nullptr);

    const affineui::detail::ResolvedStyle root{};
    // We need a parent chain: table → tbody → tr → td
    auto* table_el = env.find("table");
    auto* tbody_el = env.find("tbody");
    auto* tr_el    = env.find("tr");
    REQUIRE(table_el != nullptr);
    REQUIRE(tbody_el != nullptr);
    REQUIRE(tr_el    != nullptr);

    const auto table_rs = env.resolver->resolve(table_el, root);
    const auto tbody_rs = env.resolver->resolve(tbody_el, table_rs);
    const auto tr_rs    = env.resolver->resolve(tr_el,    tbody_rs);
    const auto td_rs    = env.resolver->resolve(td,       tr_rs);

    // .table>:not(caption)>*>* should apply padding: 8px 12px
    CHECK(td_rs.computed.padding_top    == 8);
    CHECK(td_rs.computed.padding_bottom == 8);
    CHECK(td_rs.computed.padding_left   == 12);
    CHECK(td_rs.computed.padding_right  == 12);
}

TEST_CASE("nth-child selector applies background to even rows") {
    // Tests tbody tr:nth-child(even) td { background: #eceff1 }
    CssEnv env(
        "<table><tbody>"
        "<tr><td>A</td></tr>"
        "<tr><td>B</td></tr>"
        "<tr><td>C</td></tr>"
        "</tbody></table>");
    env.attach("tbody tr:nth-child(even) td { background-color: #eceff1; }");
    env.build_resolver();

    const affineui::detail::ResolvedStyle root{};
    // Find the first and second tr
    auto* table_el  = env.find("table");
    auto* tbody_el  = env.find("tbody");
    REQUIRE(table_el != nullptr);
    REQUIRE(tbody_el != nullptr);
    const auto table_rs = env.resolver->resolve(table_el, root);
    const auto tbody_rs = env.resolver->resolve(tbody_el, table_rs);

    // First tr (odd) — no background
    auto* tr1 = find_nth(lxb_dom_interface_node(env.doc), "tr", 0);
    auto* td1 = find_nth(lxb_dom_interface_node(env.doc), "td", 0);
    REQUIRE(tr1 != nullptr);
    REQUIRE(td1 != nullptr);
    const auto tr1_rs = env.resolver->resolve(tr1, tbody_rs);
    const auto td1_rs = env.resolver->resolve(td1, tr1_rs);
    CHECK(td1_rs.animated.background_rgba == 0x00000000u);  // no bg

    // Second tr (even) — should have #eceff1 background
    auto* tr2 = find_nth(lxb_dom_interface_node(env.doc), "tr", 1);
    auto* td2 = find_nth(lxb_dom_interface_node(env.doc), "td", 1);
    REQUIRE(tr2 != nullptr);
    REQUIRE(td2 != nullptr);
    const auto tr2_rs = env.resolver->resolve(tr2, tbody_rs);
    const auto td2_rs = env.resolver->resolve(td2, tr2_rs);
    // #eceff1 = r=0xec, g=0xef, b=0xf1, a=0xff
    CHECK(td2_rs.animated.background_rgba == rgba(0xEC, 0xEF, 0xF1));
}

TEST_CASE("border-bottom-width via border shorthand applies to cells") {
    // Tests that border: 1px solid #000 sets border_bottom correctly
    CssEnv env("<table><tbody><tr><td>A</td></tr></tbody></table>");
    env.attach("td { border: 1px solid #000; }");
    env.build_resolver();

    const affineui::detail::ResolvedStyle root{};
    auto* td = env.find("td");
    REQUIRE(td != nullptr);
    const auto td_rs = env.resolver->resolve(td, root);
    CHECK(td_rs.computed.border_top    == 1);
    CHECK(td_rs.computed.border_bottom == 1);
    CHECK(td_rs.computed.border_left   == 1);
    CHECK(td_rs.computed.border_right  == 1);
}

TEST_CASE("css custom property striping via var() on table cells") {
    // Mimics Bootstrap's table striping: .table sets --bs-table-striped-bg,
    // :nth-of-type(odd) overrides --bs-table-bg-type, cell uses var() for bg.
    // This exercises the full custom-property inheritance + var() substitution chain.
    CssEnv env(
        "<table class=\"table table-striped\">"
        "<tbody>"
        "<tr><td>A</td></tr>"
        "<tr><td>B</td></tr>"
        "</tbody></table>");
    env.attach(
        // Table sets the striped bg color
        ".table { --bs-table-striped-bg: #f2f2f2; --bs-table-bg: #fff; }"
        // Cell uses a chain of vars for its bg
        ".table>:not(caption)>*>* { background-color: var(--bs-table-bg-type, var(--bs-table-bg)); }"
        // Odd rows override --bs-table-bg-type
        ".table-striped>tbody>tr:nth-of-type(odd)>* { --bs-table-bg-type: var(--bs-table-striped-bg); }");
    env.build_resolver();

    const affineui::detail::ResolvedStyle root{};
    auto* table_el = env.find("table");
    auto* tbody_el = env.find("tbody");
    REQUIRE(table_el != nullptr);
    REQUIRE(tbody_el != nullptr);
    const auto table_rs = env.resolver->resolve(table_el, root);
    const auto tbody_rs = env.resolver->resolve(tbody_el, table_rs);

    // First tr (1st = odd in nth-of-type) — should have striped bg #f2f2f2
    auto* tr1 = find_nth(lxb_dom_interface_node(env.doc), "tr", 0);
    auto* td1 = find_nth(lxb_dom_interface_node(env.doc), "td", 0);
    REQUIRE(tr1 != nullptr);
    REQUIRE(td1 != nullptr);
    const auto tr1_rs = env.resolver->resolve(tr1, tbody_rs);
    const auto td1_rs = env.resolver->resolve(td1, tr1_rs);
    // #f2f2f2 = r=0xf2, g=0xf2, b=0xf2
    CHECK(td1_rs.animated.background_rgba == rgba(0xF2, 0xF2, 0xF2));

    // Second tr (2nd = even in nth-of-type) — should have table bg #fff
    auto* tr2 = find_nth(lxb_dom_interface_node(env.doc), "tr", 1);
    auto* td2 = find_nth(lxb_dom_interface_node(env.doc), "td", 1);
    REQUIRE(tr2 != nullptr);
    REQUIRE(td2 != nullptr);
    const auto tr2_rs = env.resolver->resolve(tr2, tbody_rs);
    const auto td2_rs = env.resolver->resolve(td2, tr2_rs);
    // #fff = r=0xff, g=0xff, b=0xff
    CHECK(td2_rs.animated.background_rgba == rgba(0xFF, 0xFF, 0xFF));
}

TEST_CASE("em length uses element font-size, not root 16px") {
    // Bootstrap .badge: font-size:0.75em → 12px (parent=16px).
    // Then padding:0.35em 0.65em must resolve against 12px, not 16px.
    // Expected: padding_top = round(0.35 * 12) = 4, padding_left = round(0.65 * 12) = 8.
    CssEnv env("<div class=\"badge\">X</div>");
    env.attach(
        // Parent (root) has 16px font-size (default).
        // .badge shrinks font-size to 0.75em → 12px,
        // then uses em-based padding against its OWN font-size.
        ".badge { font-size: 0.75em; padding: 0.35em 0.65em; }");
    env.build_resolver();

    const affineui::detail::ResolvedStyle root{};  // font_size_px = 16 (default)
    auto* div = env.find("div");
    REQUIRE(div != nullptr);
    const auto rs = env.resolver->resolve(div, root);

    // font-size: 0.75em against parent 16px = 12px
    CHECK(rs.computed.font_size_px == 12);
    // padding-top: 0.35em against own 12px = round(4.2) = 4
    CHECK(rs.computed.padding_top == 4);
    // padding-left: 0.65em against own 12px = round(7.8) = 8
    CHECK(rs.computed.padding_left == 8);
    // padding-right same as padding-left (2-value shorthand)
    CHECK(rs.computed.padding_right == 8);
    // padding-bottom same as padding-top
    CHECK(rs.computed.padding_bottom == 4);
}

TEST_CASE("em font-size inherits parent font-size for nested elements") {
    // h1{font-size:2em} inside body{font-size:16px} → h1 = 32px.
    // p inside h1 with font-size:0.5em → 16px (half of h1's 32px).
    CssEnv env("<body><h1><p class=\"sub\">text</p></h1></body>");
    env.attach(
        "body { font-size: 16px; }"
        "h1 { font-size: 2em; }"
        ".sub { font-size: 0.5em; padding-top: 1em; }");
    env.build_resolver();

    const affineui::detail::ResolvedStyle root{};
    auto* body = env.find("body");
    auto* h1   = env.find("h1");
    auto* p    = env.find("p");
    REQUIRE(body != nullptr);
    REQUIRE(h1   != nullptr);
    REQUIRE(p    != nullptr);

    const auto body_rs = env.resolver->resolve(body, root);
    CHECK(body_rs.computed.font_size_px == 16);

    const auto h1_rs = env.resolver->resolve(h1, body_rs);
    // 2em against parent 16px = 32px
    CHECK(h1_rs.computed.font_size_px == 32);

    const auto p_rs = env.resolver->resolve(p, h1_rs);
    // 0.5em against parent 32px = 16px
    CHECK(p_rs.computed.font_size_px == 16);
    // padding-top: 1em against own 16px = 16px
    CHECK(p_rs.computed.padding_top == 16);
}

#endif  // !AFFINEUI_STUB_BUILD
