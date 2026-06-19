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
#    include <fstream>
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

    void build_resolver(int viewport_width_px = 0,
                        int viewport_height_px = 0) {
        resolver = affineui::detail::make_lexbor_resolver(
            doc, viewport_width_px, viewport_height_px);
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

std::string read_text_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
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

TEST_CASE("class selectors match whole class tokens, not BEM prefixes") {
    CssEnv env("<div class=\"dcs-slider__track\">x</div>");
    env.attach(".dcs-slider { height: 24px; }");
    env.build_resolver();

    auto* html = env.find("html");
    auto* body = env.find("body");
    auto* div = env.find("div");
    REQUIRE(html != nullptr);
    REQUIRE(body != nullptr);
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto html_rs = env.resolver->resolve(html, parent);
    const auto body_rs = env.resolver->resolve(body, html_rs);
    const auto rs = env.resolver->resolve(div, body_rs);
    CHECK(rs.computed.height == -1);
}

TEST_CASE("viewport units resolve against current CSS viewport") {
    CssEnv env("<div class=\"panel\"></div>");
    env.attach(
        ".panel { width: 50vw; height: 50vh; min-height: 25vmin; "
        "margin-left: calc(10vmax - 4px); }");
    env.build_resolver(/*viewport_width_px=*/800,
                       /*viewport_height_px=*/600);

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);
    CHECK(rs.computed.width == 400);
    CHECK(rs.computed.height == 300);
    CHECK(rs.computed.min_height == 150);
    CHECK(rs.computed.margin_left == 76);
}

TEST_CASE("Decius slider subpart keeps its explicit track height") {
    CssEnv env("<div class=\"dcs-slider__track\"></div>");
    env.attach(read_text_file(
        AFFINEUI_TEST_SOURCE_DIR
        "/conformance/cases/_decius/css/decius.bundle.min.css"));
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);
    CHECK(rs.computed.height == 4);
}

TEST_CASE("Decius direct-child slider rule does not match slider subparts") {
    CssEnv env(
        "<section class=\"dcs-props\">"
        "<main class=\"dcs-field\">"
        "<aside class=\"dcs-slider\">"
        "<div class=\"dcs-slider__track\"></div>"
        "</aside>"
        "</main>"
        "</section>");
    env.attach(
        ".dcs-props > .dcs-field > .dcs-slider { display: flex; align-items: center; height: 22px; }"
        ".dcs-slider__track { position: relative; height: 4px; }");
    env.build_resolver();

    auto* section = env.find("section");
    auto* main = env.find("main");
    auto* aside = env.find("aside");
    auto* div = env.find("div");
    REQUIRE(section != nullptr);
    REQUIRE(main != nullptr);
    REQUIRE(aside != nullptr);
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto section_rs = env.resolver->resolve(section, root);
    const auto main_rs = env.resolver->resolve(main, section_rs);
    const auto aside_rs = env.resolver->resolve(aside, main_rs);
    const auto track_rs = env.resolver->resolve(div, aside_rs);

    CHECK(aside_rs.computed.height == 22);
    CHECK(aside_rs.computed.display == affineui::detail::ComputedStyle::Display::Flex);
    CHECK(aside_rs.computed.align_items == affineui::detail::ComputedStyle::AlignItems::Center);
    CHECK(track_rs.computed.height == 4);
    CHECK(track_rs.computed.position == affineui::detail::ComputedStyle::Position::Relative);
}

TEST_CASE("logical box alignment start and end values parse for flex") {
    CssEnv env("<main><aside></aside></main>");
    env.attach(
        "main { display: flex; align-items: end; justify-content: end; }"
        "aside { display: flex; align-items: start; }");
    env.build_resolver();

    auto* main = env.find("main");
    auto* aside = env.find("aside");
    REQUIRE(main != nullptr);
    REQUIRE(aside != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto main_rs = env.resolver->resolve(main, root);
    const auto aside_rs = env.resolver->resolve(aside, main_rs);

    using CS = affineui::detail::ComputedStyle;
    CHECK(main_rs.computed.display == CS::Display::Flex);
    CHECK(main_rs.computed.align_items == CS::AlignItems::End);
    CHECK(main_rs.computed.justify_content == CS::JustifyContent::End);
    CHECK(aside_rs.computed.align_items == CS::AlignItems::Start);
}

TEST_CASE("Decius combo fill keeps bottom-anchored two pixel height") {
    CssEnv env("<div class=\"dcs-combo__fill\"></div>");
    env.attach(read_text_file(
        AFFINEUI_TEST_SOURCE_DIR
        "/conformance/cases/_decius/css/decius.bundle.min.css"));
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);
    CHECK(rs.computed.position == affineui::detail::ComputedStyle::Position::Absolute);
    CHECK(rs.computed.height == 2);
    CHECK(rs.computed.inset_has.bottom == 1);
    CHECK(rs.computed.inset_bottom == 0);
}

TEST_CASE("overflow does not inherit into scroll container descendants") {
    CssEnv env("<main><aside>child</aside></main>");
    env.attach("main { overflow: auto; }");
    env.build_resolver();

    auto* main = env.find("main");
    auto* aside = env.find("aside");
    REQUIRE(main != nullptr);
    REQUIRE(aside != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto parent_rs = env.resolver->resolve(main, root);
    const auto child_rs = env.resolver->resolve(aside, parent_rs);

    using O = affineui::detail::ComputedStyle::Overflow;
    CHECK(parent_rs.computed.overflow_y == O::Auto);
    CHECK(child_rs.computed.overflow_y == O::Visible);
}

TEST_CASE("min and max sizing properties do not inherit") {
    CssEnv env("<main><aside>child</aside></main>");
    env.attach("main { min-width: 11px; max-width: 123px; min-height: 24px; }");
    env.build_resolver();

    auto* main = env.find("main");
    auto* aside = env.find("aside");
    REQUIRE(main != nullptr);
    REQUIRE(aside != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto parent_rs = env.resolver->resolve(main, root);
    const auto child_rs = env.resolver->resolve(aside, parent_rs);

    CHECK(parent_rs.computed.min_width == 11);
    CHECK(parent_rs.computed.max_width == 123);
    CHECK(parent_rs.computed.min_height == 24);
    CHECK(child_rs.computed.min_width == -1);
    CHECK(child_rs.computed.max_width == -1);
    CHECK(child_rs.computed.min_height == 0);
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

TEST_CASE("transparent border side color remains an explicit override") {
    CssEnv env("<button>hi</button>");
    env.attach("button { --w: .25em; color: #112233;"
               "border: var(--w) solid currentColor;"
               "border-right-color: transparent; }");
    env.build_resolver();

    auto* button = env.find("button");
    REQUIRE(button != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(button, parent);
    CHECK(rs.animated.border_rgba == rgba(0x11, 0x22, 0x33));
    CHECK(rs.animated.border_right_rgba == rgba(0, 0, 0, 0));
    CHECK((rs.animated.border_color_set &
           affineui::detail::AnimatedStyle::BorderRightColorSet) != 0);
}

TEST_CASE("side border shorthand keeps only that side active after border-width utility") {
    CssEnv env("<div class=\"card\"></div>");
    env.attach(
        ".card { border: 0; border-left: var(--bs-border-width) solid #198754; "
        "border-width: 4px; }");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);

    using CS = affineui::detail::ComputedStyle;
    CHECK(rs.computed.border_top == 4);
    CHECK(rs.computed.border_right == 4);
    CHECK(rs.computed.border_bottom == 4);
    CHECK(rs.computed.border_left == 4);
    CHECK(rs.computed.used_border_top() == 0);
    CHECK(rs.computed.used_border_right() == 0);
    CHECK(rs.computed.used_border_bottom() == 0);
    CHECK(rs.computed.used_border_left() == 4);
    CHECK(rs.computed.border_style == CS::BorderStyle::Solid);
    CHECK(rs.computed.border_style_sides == CS::BorderLeftSide);
}

TEST_CASE("background-color currentColor resolves against element color") {
    CssEnv env("<div>hi</div>");
    env.attach("div { color: #0d6efd; background-color: currentColor; }");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);
    CHECK(rs.animated.background_rgba == rgba(0x0D, 0x6E, 0xFD));
}

TEST_CASE("background shorthand with var color resolves") {
    CssEnv env("<div class=\"dcs\">hi</div>");
    env.attach(":root { --dcs-bg: #2a2e38; }"
               ".dcs { background: var(--dcs-bg); }");
    env.build_resolver();

    auto* html = env.find("html");
    auto* body = env.find("body");
    auto* div = env.find("div");
    REQUIRE(html != nullptr);
    REQUIRE(body != nullptr);
    REQUIRE(div != nullptr);

    const auto html_rs = env.resolver->resolve(html, {});
    const auto body_rs = env.resolver->resolve(body, html_rs);
    const auto rs = env.resolver->resolve(div, body_rs);
    CHECK(rs.animated.background_rgba == rgba(0x2A, 0x2E, 0x38));
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

TEST_CASE("multi-layer box-shadow is preserved outside animated hot fields") {
    CssEnv env("<button>hi</button>");
    env.attach("button { box-shadow: inset 0 1px 0 rgba(255,255,255,.25), "
               "0 2px 6px rgba(0,0,0,.4); }");
    env.build_resolver();

    auto* button = env.find("button");
    REQUIRE(button != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(button, parent);

    REQUIRE(rs.box_shadows != nullptr);
    REQUIRE(rs.box_shadows->size() == 2);
    CHECK((*rs.box_shadows)[0].inset);
    CHECK((*rs.box_shadows)[0].offset_y == 1);
    CHECK((*rs.box_shadows)[0].rgba == rgba(255, 255, 255, 64));
    CHECK_FALSE((*rs.box_shadows)[1].inset);
    CHECK((*rs.box_shadows)[1].offset_y == 2);
    CHECK((*rs.box_shadows)[1].blur == 6);
    CHECK((*rs.box_shadows)[1].rgba == rgba(0, 0, 0, 102));
}

TEST_CASE("box-shadow parses hsla alpha colors") {
    CssEnv env("<div class=\"thumb\"></div>");
    env.attach(".thumb { color: #aab0bd; "
               "box-shadow: inset 0 1px 0 hsla(0,0%,100%,.07), "
               "inset 0 -1px 0 rgba(0,0,0,.25), "
               "0 2px 4px rgba(0,0,0,.4); }");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);

    REQUIRE(rs.box_shadows != nullptr);
    REQUIRE(rs.box_shadows->size() == 3);
    CHECK((*rs.box_shadows)[0].rgba == rgba(255, 255, 255, 18));
    CHECK((*rs.box_shadows)[1].rgba == rgba(0, 0, 0, 64));
    CHECK((*rs.box_shadows)[2].rgba == rgba(0, 0, 0, 102));
}

TEST_CASE("box-shadow custom properties preserve hsla bevel colors") {
    CssEnv env("<div class=\"thumb\"></div>");
    env.attach(":root { --bevel-up: inset 0 1px 0 hsla(0,0%,100%,.07), "
               "inset 0 -1px 0 rgba(0,0,0,.25); }"
               ".thumb { color: #aab0bd; box-shadow: var(--bevel-up), "
               "0 2px 4px rgba(0,0,0,.4); }");
    env.build_resolver();

    auto* html = env.find("html");
    auto* body = env.find("body");
    auto* div = env.find("div");
    REQUIRE(html != nullptr);
    REQUIRE(body != nullptr);
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto html_rs = env.resolver->resolve(html, parent);
    const auto body_rs = env.resolver->resolve(body, html_rs);
    const auto rs = env.resolver->resolve(div, body_rs);

    REQUIRE(body_rs.custom_props != nullptr);
    REQUIRE(body_rs.custom_props->count("--bevel-up") == 1);
    CHECK(body_rs.custom_props->at("--bevel-up").find("hsla") !=
          std::string::npos);
    REQUIRE(rs.box_shadows != nullptr);
    REQUIRE(rs.box_shadows->size() == 3);
    CHECK((*rs.box_shadows)[0].inset);
    CHECK((*rs.box_shadows)[0].offset_y == 1);
    CHECK((*rs.box_shadows)[0].rgba == rgba(255, 255, 255, 18));
    CHECK((*rs.box_shadows)[1].inset);
    CHECK((*rs.box_shadows)[1].offset_y == -1);
    CHECK((*rs.box_shadows)[1].rgba == rgba(0, 0, 0, 64));
    CHECK_FALSE((*rs.box_shadows)[2].inset);
    CHECK((*rs.box_shadows)[2].offset_y == 2);
    CHECK((*rs.box_shadows)[2].blur == 4);
    CHECK((*rs.box_shadows)[2].rgba == rgba(0, 0, 0, 102));
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
    CHECK(button_rs.computed.border_radius_top_left == 4);
    CHECK(button_rs.computed.border_radius_top_right == 4);
    CHECK(button_rs.computed.border_radius_bot_right == 4);
    CHECK(button_rs.computed.border_radius_bot_left == 4);
}

TEST_CASE("form control em sizing and float reach computed style") {
    CssEnv env("<input type=\"checkbox\">");
    env.attach("input { font-size: 16px; width: 1em; height: 1em; float: left; }");
    env.build_resolver();

    auto* input = env.find("input");
    REQUIRE(input != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(input, parent);
    CHECK(rs.computed.width == 16);
    CHECK(rs.computed.height == 16);
    CHECK(rs.computed.css_float == affineui::detail::ComputedStyle::Float::Left);
}

TEST_CASE("disabled input sibling selector reaches label") {
    CssEnv env("<input class=\"form-check-input\" disabled><label class=\"form-check-label\">x</label>");
    env.attach(".form-check-input[disabled] ~ .form-check-label { color: #999; opacity: .5; }");
    env.build_resolver();

    auto* label = env.find("label");
    REQUIRE(label != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(label, parent);
    CHECK(rs.animated.color_rgba == rgba(0x99, 0x99, 0x99));
    CHECK(rs.animated.opacity == doctest::Approx(0.5f));
}

TEST_CASE("transform functions reach animated style") {
    CssEnv env("<div class=\"mover\"></div>");
    env.attach(".mover { transform: translate(12px, 0.5rem) "
               "translate(-50%, 25%) scale(2, 3) rotate(90deg); }");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);

    CHECK(rs.animated.tx == doctest::Approx(12.0f));
    CHECK(rs.animated.ty == doctest::Approx(8.0f));
    CHECK(rs.animated.tx_pct == doctest::Approx(-50.0f));
    CHECK(rs.animated.ty_pct == doctest::Approx(25.0f));
    CHECK(rs.animated.scale_x == doctest::Approx(2.0f));
    CHECK(rs.animated.scale_y == doctest::Approx(3.0f));
    CHECK(rs.animated.rotation == doctest::Approx(1.570796f));
}

TEST_CASE("3D rotate transform functions are parsed (not dropped) and applied") {
    // rotateZ is an in-plane rotation; rotateX/Y are approximated as
    // foreshortening scale (2.5D). Crucially the whole transform must NOT be
    // discarded just because it contains a 3D function (the viewport cube uses
    // `rotateX(58deg) rotateZ(45deg)` — previously lexbor failed the declaration
    // and the element rendered flat).
    {
        CssEnv env("<div id=\"c\"></div>");
        env.attach("#c { transform: rotateZ(45deg); }");
        env.build_resolver();
        auto* c = env.find("div");
        REQUIRE(c != nullptr);
        const auto rs = env.resolver->resolve(c, affineui::detail::ResolvedStyle{});
        CHECK(rs.animated.rotation == doctest::Approx(0.7853982f));  // 45°
        CHECK(rs.animated.scale_y == doctest::Approx(1.0f));
    }
    {
        CssEnv env("<div id=\"c\"></div>");
        env.attach("#c { transform: rotateX(60deg); }");
        env.build_resolver();
        auto* c = env.find("div");
        REQUIRE(c != nullptr);
        const auto rs = env.resolver->resolve(c, affineui::detail::ResolvedStyle{});
        CHECK(rs.animated.scale_y == doctest::Approx(0.5f).epsilon(0.01));  // cos60
        CHECK(rs.animated.rotation == doctest::Approx(0.0f));
    }
    {
        CssEnv env("<div id=\"c\"></div>");
        env.attach("#c { transform: rotateX(58deg) rotateZ(45deg); }");
        env.build_resolver();
        auto* c = env.find("div");
        REQUIRE(c != nullptr);
        const auto rs = env.resolver->resolve(c, affineui::detail::ResolvedStyle{});
        CHECK(rs.animated.rotation == doctest::Approx(0.7853982f));  // rotateZ kept
        CHECK(rs.animated.scale_y < 0.95f);                          // rotateX foreshortened
    }
}

TEST_CASE("transform-origin reaches animated style") {
    CssEnv env("<div class=\"needle\"></div>");
    env.attach(".needle { transform-origin: 50% 100%; }");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);

    CHECK(rs.animated.origin_x == doctest::Approx(0.0f));
    CHECK(rs.animated.origin_y == doctest::Approx(0.0f));
    CHECK(rs.animated.origin_x_pct == doctest::Approx(50.0f));
    CHECK(rs.animated.origin_y_pct == doctest::Approx(100.0f));
}

TEST_CASE("animation shorthand reaches resolved animation style") {
    CssEnv env("<div class=\"spinner\"></div>");
    env.attach(".spinner { animation: .75s linear infinite spinner-border; }");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);

    CHECK(rs.animation.active);
    CHECK(rs.animation.name_hash != 0u);
    CHECK(rs.animation.duration_s == doctest::Approx(0.75f));
    CHECK(rs.animation.iteration_count == doctest::Approx(0.0f));
    CHECK(rs.animation.timing ==
          affineui::detail::ResolvedStyle::CssAnimation::Timing::Linear);
}

TEST_CASE("animation shorthand resolves custom property pieces") {
    CssEnv env("<html><body><div class=\"menu\"></div></body></html>");
    env.attach(":root { --fast: 80ms ease-out; }"
               ".menu { animation: menu-in var(--fast); }");
    env.build_resolver();

    auto* html = env.find("html");
    auto* body = env.find("body");
    auto* div = env.find("div");
    REQUIRE(html != nullptr);
    REQUIRE(body != nullptr);
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto html_rs = env.resolver->resolve(html, root);
    const auto body_rs = env.resolver->resolve(body, html_rs);
    const auto rs = env.resolver->resolve(div, body_rs);

    CHECK(rs.animation.active);
    CHECK(rs.animation.name_hash != 0u);
    CHECK(rs.animation.duration_s == doctest::Approx(0.08f));
    CHECK(rs.animation.timing ==
          affineui::detail::ResolvedStyle::CssAnimation::Timing::EaseOut);
}

TEST_CASE("animation longhands override shorthand pieces") {
    CssEnv env("<div class=\"spinner\"></div>");
    env.attach(".spinner { animation: 1s ease spin; animation-duration: 250ms;"
               "animation-direction: alternate-reverse; animation-play-state: paused; }");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);

    using Anim = affineui::detail::ResolvedStyle::CssAnimation;
    CHECK(rs.animation.active);
    CHECK(rs.animation.duration_s == doctest::Approx(0.25f));
    CHECK(rs.animation.direction == Anim::Direction::AlternateReverse);
    CHECK(rs.animation.play_state == Anim::PlayState::Paused);
}

TEST_CASE("background-image hard-stop gradient resolves as stripe pattern") {
    CssEnv env("<div></div>");
    env.attach(
        "div {"
        "  background-color: #dc3545;"
        "  background-image: linear-gradient(45deg,"
        "    rgba(255,255,255,.15) 25%, transparent 25%,"
        "    transparent 50%, rgba(255,255,255,.15) 50%,"
        "    rgba(255,255,255,.15) 75%, transparent 75%, transparent);"
        "}");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);
    CHECK(rs.animated.background_rgba == rgba(0xDC, 0x35, 0x45));
    CHECK(rs.animated.gradient_kind ==
          affineui::detail::AnimatedStyle::GradientKind::LinearStripes);
    CHECK(rs.animated.gradient_angle_deg == 45);
    CHECK(rs.animated.gradient_stop0_rgba == rgba(0xFF, 0xFF, 0xFF, 38));
}

TEST_CASE("radial gradient position and last color stop resolve") {
    CssEnv env("<div></div>");
    env.attach(
        "div {"
        "  background: radial-gradient(circle at 30% 25%,"
        "    #111111, #222222 55%, #333333 100%);"
        "}");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);
    CHECK(rs.animated.gradient_kind ==
          affineui::detail::AnimatedStyle::GradientKind::Radial);
    CHECK(rs.animated.gradient_center_x_pct == 30);
    CHECK(rs.animated.gradient_center_y_pct == 25);
    CHECK(rs.animated.gradient_stop0_rgba == rgba(0x11, 0x11, 0x11));
    CHECK(rs.animated.gradient_stop1_rgba == rgba(0x33, 0x33, 0x33));
}

TEST_CASE("layered background grid keeps tiled lines over base gradient") {
    CssEnv env("<div></div>");
    env.attach(
        "div {"
        "  background:"
        "    linear-gradient(90deg, rgba(255,255,255,.04) 1px, transparent 1px),"
        "    linear-gradient(rgba(255,255,255,.04) 1px, transparent 1px),"
        "    radial-gradient(circle at 52% 42%, #495064, #171a22 72%);"
        "  background-size: 24px 24px, 24px 24px, auto;"
        "}");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);
    CHECK(rs.animated.gradient_kind ==
          affineui::detail::AnimatedStyle::GradientKind::Radial);
    CHECK(rs.animated.gradient_center_x_pct == 52);
    CHECK(rs.animated.gradient_center_y_pct == 42);
    CHECK(rs.animated.gradient_stop0_rgba == rgba(0x49, 0x50, 0x64));
    CHECK(rs.animated.gradient_stop1_rgba == rgba(0x17, 0x1A, 0x22));
    CHECK(rs.animated.gradient_stop1_pos_pct == 72);
    CHECK(rs.animated.background_grid_rgba == rgba(0xFF, 0xFF, 0xFF, 10));
    CHECK(rs.animated.background_grid_size_px == 24);
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

TEST_CASE("margin shorthand mirrors auto onto both horizontal sides") {
    CssEnv env("<div>x</div>");
    env.attach("div { margin: 0 auto; }");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);
    CHECK(rs.computed.margin_top == 0);
    CHECK(rs.computed.margin_bottom == 0);
    CHECK(rs.computed.margin_auto.left == 1);
    CHECK(rs.computed.margin_auto.right == 1);
}

TEST_CASE("inset shorthand mirrors percentages and longhands override edges") {
    CssEnv env("<div>x</div>");
    env.attach("div { position: absolute; inset: 18%; left: 50%; bottom: auto; }");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);
    CHECK(rs.computed.position == affineui::detail::ComputedStyle::Position::Absolute);
    CHECK(rs.computed.inset_has.top == 1);
    CHECK(rs.computed.inset_has.top_pct == 1);
    CHECK(rs.computed.inset_top == 1800);
    CHECK(rs.computed.inset_has.right == 1);
    CHECK(rs.computed.inset_has.right_pct == 1);
    CHECK(rs.computed.inset_right == 1800);
    CHECK(rs.computed.inset_has.bottom == 0);
    CHECK(rs.computed.inset_has.bottom_pct == 0);
    CHECK(rs.computed.inset_has.left == 1);
    CHECK(rs.computed.inset_has.left_pct == 1);
    CHECK(rs.computed.inset_left == 5000);
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

TEST_CASE("calc() preserves pure percentage values for layout resolution") {
    CssEnv env("<div class=\"sub\">x</div>");
    env.attach(".sub { --fill: 28%; width: calc(var(--fill, 0%)); }");
    env.build_resolver();

    auto* sub = env.find("div");
    REQUIRE(sub != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(sub, parent);
    CHECK(rs.computed.width == -1);
    CHECK(rs.computed.width_pct_x100 == 2800);
}

TEST_CASE("calc() resolves viewport width units when viewport is known") {
    // Bootstrap headings use e.g. h4 { font-size: calc(1.275rem + .3vw); }.
    // At 800px viewport width: 1.275rem = 20.4px and .3vw = 2.4px,
    // rounded by the existing length path to 23px.
    CssEnv env("<h4>Heading</h4>");
    env.attach("h4 { font-size: calc(1.275rem + .3vw); }");
    env.build_resolver(800);

    auto* h4 = env.find("h4");
    REQUIRE(h4 != nullptr);
    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(h4, parent);
    CHECK(rs.computed.font_size_px == 23);
}

TEST_CASE("grouped heading rule applies line-height to h4") {
    CssEnv env("<h4>Heading</h4>");
    env.attach("body{line-height:1.5}"
               ".h1,.h2,.h3,.h4,.h5,.h6,h1,h2,h3,h4,h5,h6{line-height:1.2}");
    env.build_resolver();

    auto* h4 = env.find("h4");
    REQUIRE(h4 != nullptr);

    affineui::detail::ResolvedStyle body{};
    body.computed.line_height_x100 = 150;
    const auto rs = env.resolver->resolve(h4, body);
    CHECK(rs.computed.line_height_x100 == 120);
}

TEST_CASE("Bootstrap heading line-height wins over body line-height") {
    CssEnv env("<html><body><h4>Heading</h4></body></html>");
    const auto css = read_text_file(
        AFFINEUI_TEST_SOURCE_DIR
        "/conformance/cases/_bootstrap/bootstrap.min.css");
    env.attach(css);
    env.build_resolver(800);

    auto* html = env.find("html");
    auto* body = env.find("body");
    auto* h4   = env.find("h4");
    REQUIRE(html != nullptr);
    REQUIRE(body != nullptr);
    REQUIRE(h4 != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto html_rs = env.resolver->resolve(html, root);
    const auto body_rs = env.resolver->resolve(body, html_rs);
    const auto h4_rs   = env.resolver->resolve(h4, body_rs);

    CHECK(body_rs.computed.line_height_x100 == 150);
    CHECK(h4_rs.computed.font_size_px == 23);
    CHECK(h4_rs.computed.line_height_x100 == 120);
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

TEST_CASE("vertical-align reaches computed style for inline layout") {
    CssEnv env("<button>hi</button><span>new</span>");
    env.attach("button { vertical-align: middle; }"
               "span { vertical-align: text-top; }");
    env.build_resolver();

    auto* button = env.find("button");
    auto* span   = env.find("span");
    REQUIRE(button != nullptr);
    REQUIRE(span != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto button_rs = env.resolver->resolve(button, parent);
    const auto span_rs   = env.resolver->resolve(span, parent);

    using VA = affineui::detail::ComputedStyle::VerticalAlign;
    CHECK(button_rs.computed.vertical_align == VA::Middle);
    CHECK(span_rs.computed.vertical_align == VA::TextTop);
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
    CHECK(rs.computed.border_radius_top_left == 8);
    CHECK(rs.computed.border_radius_bot_right == 6);
}

TEST_CASE("percentage border-radius is retained until box size is known") {
    CssEnv env("<div class=\"circle\"></div>");
    env.attach(".circle { border-radius: 50%; }");
    env.build_resolver();

    auto* div = env.find("div");
    REQUIRE(div != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(div, parent);
    CHECK(affineui::detail::border_radius_is_percent(
        rs.computed.border_radius_top_left));
    CHECK(affineui::detail::resolve_border_radius_px(
        rs.computed.border_radius_top_left, 80, 80) == doctest::Approx(40.0f));
}

TEST_CASE("text-decoration line and color reach resolved style") {
    CssEnv env("<a>link</a>");
    env.attach("a { text-decoration: underline; text-decoration-color: #d32f2f; }");
    env.build_resolver();

    auto* link = env.find("a");
    REQUIRE(link != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(link, parent);
    CHECK((rs.computed.text_decoration_line &
           affineui::detail::ComputedStyle::DecorationUnderline) != 0);
    CHECK(rs.animated.text_decoration_rgba == rgba(0xd3, 0x2f, 0x2f));
}

TEST_CASE("list-style-type inherits to list-item boxes") {
    CssEnv env("<ul class=\"square\"><li>one</li></ul>");
    env.attach("li { display: list-item; } .square { list-style-type: square; }");
    env.build_resolver();

    auto* ul = env.find("ul");
    auto* li = env.find("li");
    REQUIRE(ul != nullptr);
    REQUIRE(li != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto ul_rs = env.resolver->resolve(ul, root);
    const auto li_rs = env.resolver->resolve(li, ul_rs);

    CHECK(ul_rs.computed.list_style_type ==
          affineui::detail::ComputedStyle::ListStyleType::Square);
    CHECK(li_rs.computed.list_style_type ==
          affineui::detail::ComputedStyle::ListStyleType::Square);
    CHECK(li_rs.computed.display ==
          affineui::detail::ComputedStyle::Display::ListItem);
}

TEST_CASE("list-style shorthand sets marker type") {
    CssEnv env("<ul class=\"plain\"><li>one</li></ul>");
    env.attach("li { display: list-item; } .plain { list-style: none; }");
    env.build_resolver();

    auto* ul = env.find("ul");
    auto* li = env.find("li");
    REQUIRE(ul != nullptr);
    REQUIRE(li != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto ul_rs = env.resolver->resolve(ul, root);
    const auto li_rs = env.resolver->resolve(li, ul_rs);

    CHECK(ul_rs.computed.list_style_type ==
          affineui::detail::ComputedStyle::ListStyleType::None);
    CHECK(li_rs.computed.list_style_type ==
          affineui::detail::ComputedStyle::ListStyleType::None);
}

TEST_CASE("grid and inline-flex display values reach computed style") {
    CssEnv env(
        "<div class=\"stack\"></div>"
        "<span class=\"inline\"></span>"
        "<nav class=\"tools\"></nav>");
    env.attach(".stack { display: grid; }"
               ".inline { display: inline-grid; }"
               ".tools { display: inline-flex; }");
    env.build_resolver();

    auto* div = env.find("div");
    auto* span = env.find("span");
    auto* nav = env.find("nav");
    REQUIRE(div != nullptr);
    REQUIRE(span != nullptr);
    REQUIRE(nav != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto div_rs = env.resolver->resolve(div, root);
    const auto span_rs = env.resolver->resolve(span, root);
    const auto nav_rs = env.resolver->resolve(nav, root);

    CHECK(div_rs.computed.display ==
          affineui::detail::ComputedStyle::Display::Grid);
    CHECK(span_rs.computed.display ==
          affineui::detail::ComputedStyle::Display::InlineGrid);
    CHECK(nav_rs.computed.display ==
          affineui::detail::ComputedStyle::Display::InlineFlex);
}

TEST_CASE("text-indent length reaches computed style") {
    CssEnv env("<p class=\"ind\">Indented</p>");
    env.attach(".ind { text-indent: 48px; }");
    env.build_resolver();

    auto* p = env.find("p");
    REQUIRE(p != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto rs = env.resolver->resolve(p, root);

    CHECK(rs.computed.text_indent_value == 48);
    CHECK(rs.computed.text_indent_is_pct == 0);
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

TEST_CASE("single-number flex shorthand uses zero percent basis") {
    CssEnv env("<main><section>one</section></main>");
    env.attach("section { flex: 1; }");
    env.build_resolver();

    auto* section = env.find("section");
    REQUIRE(section != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(section, parent);

    CHECK(rs.computed.flex_grow == 1);
    CHECK(rs.computed.flex_shrink == 1);
    CHECK(rs.computed.flex_basis_pct == 0);
    CHECK(rs.computed.flex_basis == -1);
}

TEST_CASE("three-part flex shorthand keeps a pixel basis and zero shrink") {
    CssEnv env("<main><section>fixed</section><article>grow</article></main>");
    env.attach("section { flex: 0 0 110px; }"
               "article { flex: 1 1 0px; }");
    env.build_resolver();

    auto* section = env.find("section");
    auto* article = env.find("article");
    REQUIRE(section != nullptr);
    REQUIRE(article != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(section, parent);
    const auto article_rs = env.resolver->resolve(article, parent);

    CHECK(rs.computed.flex_grow == 0);
    CHECK(rs.computed.flex_shrink == 0);
    CHECK(rs.computed.flex_basis_pct == -1);
    CHECK(rs.computed.flex_basis == 110);

    CHECK(article_rs.computed.flex_grow == 1);
    CHECK(article_rs.computed.flex_shrink == 1);
    CHECK(article_rs.computed.flex_basis_pct == -1);
    CHECK(article_rs.computed.flex_basis == 0);
}

TEST_CASE("inline styles outrank class rules for layout properties") {
    CssEnv env("<main><section class=\"dock\" style=\"height:0;flex:1 1 0px\">"
               "dock</section></main>");
    env.attach(".dock { height: 100%; flex: 0 0 200px; }");
    env.build_resolver();

    auto* section = env.find("section");
    REQUIRE(section != nullptr);

    const affineui::detail::ResolvedStyle parent{};
    const auto rs = env.resolver->resolve(section, parent);

    CHECK(rs.computed.height == 0);
    CHECK(rs.computed.height_pct == -1);
    CHECK(rs.computed.flex_grow == 1);
    CHECK(rs.computed.flex_shrink == 1);
    CHECK(rs.computed.flex_basis == 0);
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

TEST_CASE("font:0/0 shorthand (Bootstrap icon-reset) parses safely") {
    // `font:0/0 a` is Bootstrap 4's icon-font reset hack: size 0, line-height
    // 0, family "a". The leading 0 must be read as the font-SIZE, not consumed
    // as a font-weight candidate — doing the latter recycled the token and then
    // read it again (use-after-free under ASAN) AND dropped the 0 size. A bare
    // number is a weight only when in 1..1000.
    CssEnv env("<p>hi</p>");
    env.attach("p { font: 0/0 a; }");
    env.build_resolver();

    auto* p = env.find("p");
    REQUIRE(p != nullptr);

    affineui::detail::ResolvedStyle parent{};
    parent.computed.font_size_px = 40;  // distinctive inherited size
    const auto rs = env.resolver->resolve(p, parent);
    // The real regression teeth: parsing `font:0/0` must not crash (the leading
    // 0 was read after token recycle — a use-after-free caught under ASAN, also
    // exercised by test_bootstrap's real Bootstrap CSS). Behaviourally, a 0
    // font-size is intentionally ignored by the resolver (px > 0 guard), so the
    // inherited size survives — proving the shorthand parsed cleanly to
    // completion rather than corrupting the cascade.
    CHECK(rs.computed.font_size_px == 40);
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

TEST_CASE("higher-specificity font inherit shorthand beats form longhands") {
    CssEnv env("<body class=\"dcs\"><label><input class=\"dcs-input\"></label></body>");
    env.attach(
        ".dcs { font-size: 12px; line-height: 1.45; font-weight: 500; }"
        ".dcs-input { font-size: 11px; font-weight: 400; }"
        ".dcs input { font: inherit; }");
    env.build_resolver();

    auto* html = env.find("html");
    auto* body = env.find("body");
    auto* label = env.find("label");
    auto* input = env.find("input");
    REQUIRE(html != nullptr);
    REQUIRE(body != nullptr);
    REQUIRE(label != nullptr);
    REQUIRE(input != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto html_rs = env.resolver->resolve(html, root);
    const auto body_rs = env.resolver->resolve(body, html_rs);
    const auto label_rs = env.resolver->resolve(label, body_rs);
    const auto input_rs = env.resolver->resolve(input, label_rs);

    CHECK(input_rs.computed.font_size_px == 12);
    CHECK(input_rs.computed.line_height_x100 == 145);
    CHECK(input_rs.computed.font_weight == 500);
}

TEST_CASE("Decius form controls inherit the framework base font") {
    CssEnv env("<body class=\"dcs\"><label><input class=\"dcs-input\"></label></body>");
    env.attach(read_text_file(
        AFFINEUI_TEST_SOURCE_DIR
        "/conformance/cases/_decius/css/decius.bundle.min.css"));
    env.build_resolver();

    auto* html = env.find("html");
    auto* body = env.find("body");
    auto* label = env.find("label");
    auto* input = env.find("input");
    REQUIRE(html != nullptr);
    REQUIRE(body != nullptr);
    REQUIRE(label != nullptr);
    REQUIRE(input != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto html_rs = env.resolver->resolve(html, root);
    const auto body_rs = env.resolver->resolve(body, html_rs);
    const auto label_rs = env.resolver->resolve(label, body_rs);
    const auto input_rs = env.resolver->resolve(input, label_rs);

    CHECK(body_rs.computed.font_size_px == 12);
    CHECK(body_rs.computed.line_height_x100 == 145);
    CHECK(input_rs.computed.font_size_px == 12);
    CHECK(input_rs.computed.line_height_x100 == 145);
    CHECK(input_rs.computed.font_weight == 400);
}

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

TEST_CASE("class argument :not selector controls display") {
    CssEnv env("<nav class=\"collapse\">Hidden</nav>"
               "<nav class=\"collapse show\">Visible</nav>");
    env.attach(".collapse:not(.show){display:none}");
    env.build_resolver();

    const affineui::detail::ResolvedStyle root{};
    auto* hidden = find_nth(lxb_dom_interface_node(env.doc), "nav", 0);
    auto* visible = find_nth(lxb_dom_interface_node(env.doc), "nav", 1);
    REQUIRE(hidden != nullptr);
    REQUIRE(visible != nullptr);

    const auto hidden_rs = env.resolver->resolve(hidden, root);
    const auto visible_rs = env.resolver->resolve(visible, root);

    CHECK(hidden_rs.computed.display ==
          affineui::detail::ComputedStyle::Display::None);
    CHECK(visible_rs.computed.display !=
          affineui::detail::ComputedStyle::Display::None);
}

TEST_CASE("Bootstrap 5 collapse selector controls display") {
    CssEnv env("<nav class=\"col-md-3 col-lg-2 d-md-block bg-body-tertiary sidebar collapse\">Hidden</nav>");
    env.attach(read_text_file(
        AFFINEUI_TEST_SOURCE_DIR
        "/examples/frameworks/css/bootstrap-5.3.8.min.css"));
    env.build_resolver(740, 920);

    auto* nav = env.find("nav");
    REQUIRE(nav != nullptr);

    const affineui::detail::ResolvedStyle root{};
    const auto rs = env.resolver->resolve(nav, root);
    CHECK(rs.computed.display ==
          affineui::detail::ComputedStyle::Display::None);
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

TEST_CASE("letter-spacing preserves fractional em lengths") {
    CssEnv env("<div class=\"badge\">ACTIVE</div>");
    env.attach(".badge { font-size: 10px; letter-spacing: .04em; }");
    env.build_resolver();

    const affineui::detail::ResolvedStyle root{};
    auto* div = env.find("div");
    REQUIRE(div != nullptr);
    const auto rs = env.resolver->resolve(div, root);

    CHECK(rs.computed.font_size_px == 10);
    CHECK(rs.computed.letter_spacing_x100 == 40);
}

TEST_CASE("letter-spacing preserves fractional px lengths") {
    CssEnv env("<div class=\"badge\">ACTIVE</div>");
    env.attach(".badge { letter-spacing: .25px; }");
    env.build_resolver();

    const affineui::detail::ResolvedStyle root{};
    auto* div = env.find("div");
    REQUIRE(div != nullptr);
    const auto rs = env.resolver->resolve(div, root);

    CHECK(rs.computed.letter_spacing_x100 == 25);
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

TEST_CASE("lexbor parses content strings and preserves var content declarations") {
    CssEnv env("<div class=\"item\">x</div>");
    env.attach(".item::before { content: \"/\"; }"
               ".item::after { content: var(--suffix, \"!\"); }");

    auto* sheet = env.sheets.back();
    auto* rules = lxb_css_rule_list(sheet->root);
    REQUIRE(rules != nullptr);
    REQUIRE(rules->first != nullptr);
    REQUIRE(rules->first->next != nullptr);

    auto* before_rule = lxb_css_rule_style(rules->first);
    REQUIRE(before_rule != nullptr);
    REQUIRE(before_rule->declarations != nullptr);
    REQUIRE(before_rule->declarations->first != nullptr);
    auto* before_decl = reinterpret_cast<lxb_css_rule_declaration_t*>(
        before_rule->declarations->first);
    CHECK(before_decl->type == LXB_CSS_PROPERTY_CONTENT);
    REQUIRE(before_decl->u.content != nullptr);
    CHECK(before_decl->u.content->type == LXB_CSS_CONTENT_STRING);
    CHECK(before_decl->u.content->value.length == 1);
    REQUIRE(before_decl->u.content->value.data != nullptr);
    CHECK(before_decl->u.content->value.data[0] == '/');

    auto* after_rule = lxb_css_rule_style(rules->first->next);
    REQUIRE(after_rule != nullptr);
    REQUIRE(after_rule->declarations != nullptr);
    REQUIRE(after_rule->declarations->first != nullptr);
    auto* after_decl = reinterpret_cast<lxb_css_rule_declaration_t*>(
        after_rule->declarations->first);
    CHECK(after_decl->type == LXB_CSS_PROPERTY__UNDEF);
    REQUIRE(after_decl->u.undef != nullptr);
    CHECK(after_decl->u.undef->type == LXB_CSS_PROPERTY_CONTENT);
}

#endif  // !AFFINEUI_STUB_BUILD
