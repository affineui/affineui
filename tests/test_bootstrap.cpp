#include <doctest/doctest.h>

#include "affineui/document.h"
#include "affineui/painter.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct PaintRect {
    affineui::Rect rect;
    affineui::Color color;
};

struct TextRun {
    std::string text;
    affineui::Point pos;
    float max_width{0.0f};
    affineui::Painter::TextAlign align{affineui::Painter::TextAlign::Left};
};

class RecordingPainter final : public affineui::Painter {
public:
    std::vector<PaintRect> fills;
    std::vector<PaintRect> strokes;
    std::vector<TextRun> text_runs;

    void begin_frame(int, int, float) override {}
    void end_frame() override {}
    void fill_rect(const affineui::Rect& rect, affineui::Color color) override {
        fills.push_back({rect, color});
    }
    void stroke_rect(const affineui::Rect& rect, affineui::Color color, float) override {
        strokes.push_back({rect, color});
    }
    void stroke_line(float, float, float, float, affineui::Color color, float) override {
        strokes.push_back({{}, color});
    }
    void fill_circle(float, float, float, affineui::Color color) override {
        fills.push_back({{}, color});
    }
    void stroke_arc(float, float, float, float, float,
                    affineui::Color color, float) override {
        strokes.push_back({{}, color});
    }
    void fill_rounded_rect(const affineui::Rect& rect, float,
                           affineui::Color color) override {
        fills.push_back({rect, color});
    }
    void stroke_rounded_rect(const affineui::Rect& rect, float,
                             affineui::Color color, float) override {
        strokes.push_back({rect, color});
    }
    void fill_rounded_rect_varying(const affineui::Rect& rect, float, float,
                                   float, float,
                                   affineui::Color color) override {
        fills.push_back({rect, color});
    }
    void stroke_rounded_rect_varying(const affineui::Rect& rect, float, float,
                                     float, float,
                                     affineui::Color color, float) override {
        strokes.push_back({rect, color});
    }
    void fill_linear_gradient_rect(const affineui::Rect& rect, float,
                                   affineui::Color c0, affineui::Color,
                                   float, float, float, float) override {
        fills.push_back({rect, c0});
    }
    void fill_radial_gradient_rect(const affineui::Rect& rect,
                                   affineui::Color c0, affineui::Color,
                                   float, float, float, float,
                                   float = 50, float = 50, float = 100) override {
        fills.push_back({rect, c0});
    }
    void fill_box_shadow(const affineui::Rect& rect, float, affineui::Color c,
                         float, float, float, float, bool) override {
        fills.push_back({rect, c});
    }
    std::uint32_t resolve_font(std::string_view, int, int, bool) override {
        return 1;
    }
    int measure_text(std::uint32_t, std::string_view text) override {
        return static_cast<int>(text.size()) * 8;
    }
    TextMetrics text_metrics(std::uint32_t) override {
        return {12.0f, 4.0f, 18.0f};
    }
    void draw_text(std::uint32_t, const affineui::Point& pos,
                   std::string_view text, affineui::Color) override {
        text_runs.push_back({std::string{text}, pos});
    }
    affineui::Size measure_text_box(std::uint32_t, std::string_view text,
                                    float max_width, float, float) override {
        const int natural = static_cast<int>(text.size()) * 8;
        const int width = max_width > 0.0f
                              ? std::min(natural, static_cast<int>(max_width))
                              : natural;
        return {width, 18};
    }
    void draw_text_box(std::uint32_t, const affineui::Point& pos,
                       std::string_view text, affineui::Color,
                       float max_width, float, float,
                       affineui::Painter::TextAlign align) override {
        text_runs.push_back({std::string{text}, pos, max_width, align});
    }
    std::uint32_t load_image(std::string_view) override {
        return 0;
    }
    affineui::Size image_size(std::uint32_t) override {
        return {};
    }
    void draw_image(std::uint32_t, const affineui::Rect&,
                    const affineui::Rect&) override {}
    void push_clip(const affineui::Rect&) override {}
    void pop_clip() override {}
    void push_alpha(float) override {}
    void pop_alpha() override {}
};

bool same_color(affineui::Color a, affineui::Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool saw_fill(const RecordingPainter& painter, affineui::Color color) {
    for (const auto& fill : painter.fills) {
        if (same_color(fill.color, color)) return true;
    }
    return false;
}

bool saw_stroke(const RecordingPainter& painter, affineui::Color color) {
    for (const auto& stroke : painter.strokes) {
        if (same_color(stroke.color, color)) return true;
    }
    return false;
}

std::string read_file(const char* path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.good()) return {};
    std::stringstream bytes;
    bytes << file.rdbuf();
    return bytes.str();
}

std::string read_file(const std::filesystem::path& path) {
    return read_file(path.string().c_str());
}

std::filesystem::path test_source_root() {
#if defined(AFFINEUI_TEST_SOURCE_DIR)
    return std::filesystem::path{AFFINEUI_TEST_SOURCE_DIR};
#else
    return std::filesystem::current_path();
#endif
}

std::filesystem::path example_dir(std::string_view example_name) {
    return test_source_root() / "examples" / std::string(example_name);
}

std::string bootstrap_css() {
#if defined(AFFINEUI_TEST_SOURCE_DIR)
    const std::string source_dir = AFFINEUI_TEST_SOURCE_DIR;
    auto css = read_file(
        (source_dir + "/examples/01_bootstrap/bootstrap-4.6.2.min.css")
            .c_str());
    if (!css.empty()) return css;
#endif

    for (const char* path : {
             "examples/01_bootstrap/bootstrap-4.6.2.min.css",
             "../examples/01_bootstrap/bootstrap-4.6.2.min.css",
             "../../examples/01_bootstrap/bootstrap-4.6.2.min.css",
         }) {
        auto css = read_file(path);
        if (!css.empty()) return css;
    }
    return {};
}

std::string bootstrap5_css() {
#if defined(AFFINEUI_TEST_SOURCE_DIR)
    const std::string source_dir = AFFINEUI_TEST_SOURCE_DIR;
    auto css = read_file(
        (source_dir + "/examples/frameworks/css/bootstrap-5.3.8.min.css")
            .c_str());
    if (!css.empty()) return css;
#endif

    for (const char* path : {
             "examples/frameworks/css/bootstrap-5.3.8.min.css",
             "../examples/frameworks/css/bootstrap-5.3.8.min.css",
             "../../examples/frameworks/css/bootstrap-5.3.8.min.css",
         }) {
        auto css = read_file(path);
        if (!css.empty()) return css;
    }
    return {};
}

bool require_bootstrap_css(std::string& css) {
    css = bootstrap_css();
    if (!css.empty()) return true;
    WARN("Bootstrap CSS fixture not found");
    return false;
}

bool require_bootstrap5_css(std::string& css) {
    css = bootstrap5_css();
    if (!css.empty()) return true;
    WARN("Bootstrap 5 CSS fixture not found");
    return false;
}

bool text_contains(const RecordingPainter& painter, std::string_view needle) {
    for (const auto& run : painter.text_runs) {
        if (run.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::optional<affineui::Point>
point_for_text(const RecordingPainter& painter, std::string_view needle) {
    for (const auto& run : painter.text_runs) {
        if (run.text.find(needle) != std::string::npos) return run.pos;
    }
    return std::nullopt;
}

const TextRun* text_run_for(const RecordingPainter& painter,
                            std::string_view needle) {
    for (const auto& run : painter.text_runs) {
        if (run.text.find(needle) != std::string::npos) return &run;
    }
    return nullptr;
}

std::string text_run_summary(const RecordingPainter& painter) {
    std::ostringstream out;
    const auto count = std::min<std::size_t>(painter.text_runs.size(), 16);
    out << "text runs (" << painter.text_runs.size() << "):";
    for (std::size_t i = 0; i < count; ++i) {
        out << " [" << painter.text_runs[i].text << " @ "
            << painter.text_runs[i].pos.x << ","
            << painter.text_runs[i].pos.y << "]";
    }
    return out.str();
}

RecordingPainter render_bootstrap(std::string_view html, int width = 960,
                                  int height = 0) {
    std::string css;
    REQUIRE(require_bootstrap_css(css));

    affineui::Document doc;
    RecordingPainter painter;

    doc.set_user_stylesheet(css);
    doc.set_html(html);
    doc.layout(width, height, &painter);
    doc.draw(painter);

    return painter;
}

RecordingPainter render_bootstrap5(std::string_view html, int width = 960,
                                   int height = 0) {
    std::string css;
    REQUIRE(require_bootstrap5_css(css));

    affineui::Document doc;
    RecordingPainter painter;

    doc.set_user_stylesheet(css);
    doc.set_html(html);
    doc.layout(width, height, &painter);
    doc.draw(painter);

    return painter;
}

RecordingPainter render_example(std::string_view example_name,
                                std::string_view html,
                                int width,
                                int height) {
    const auto root = example_dir(example_name);

    affineui::Document doc;
    RecordingPainter painter;

    doc.set_resource_loader([root](std::string_view url) -> std::string {
        if (url.empty() || url.find("://") != std::string_view::npos ||
            url.rfind("data:", 0) == 0) {
            return {};
        }
        const auto path =
            (root / std::filesystem::path{std::string(url)}).lexically_normal();
        return read_file(path);
    });
    doc.set_html(html);
    doc.layout(width, height, &painter);
    doc.draw(painter);

    return painter;
}

}  // namespace

TEST_CASE("Bootstrap grid columns stay on the same row") {
    auto painter = render_bootstrap(R"HTML(
        <main class="container py-4">
            <div class="row">
                <div class="col">
                    <div class="card">
                        <div class="card-body">
                            <h5 class="card-title">Left card</h5>
                            <p class="card-text">First column</p>
                        </div>
                    </div>
                </div>
                <div class="col">
                    <div class="card">
                        <div class="card-body">
                            <h5 class="card-title">Right card</h5>
                            <p class="card-text">Second column</p>
                        </div>
                    </div>
                </div>
            </div>
        </main>
    )HTML");

    const auto left = point_for_text(painter, "Left card");
    const auto right = point_for_text(painter, "Right card");
    REQUIRE(left.has_value());
    REQUIRE(right.has_value());

    CHECK(right->x > left->x + 120);
    CHECK(right->y >= left->y - 4);
    CHECK(right->y <= left->y + 4);
}

TEST_CASE("Bootstrap 5 dashboard sidebar uses desktop grid width") {
    auto painter = render_bootstrap5(R"HTML(
        <div class="container-fluid">
            <div class="row">
                <nav class="col-md-3 col-lg-2 d-md-block bg-body-tertiary sidebar collapse">
                    <h6 class="px-3">Workspace</h6>
                </nav>
                <main class="col-md-9 ms-sm-auto col-lg-10 px-md-4 dashboard-main">
                    <h1 class="h2">Dashboard</h1>
                </main>
            </div>
        </div>
    )HTML", 1440, 920);

    const auto sidebar = point_for_text(painter, "Workspace");
    const auto main = point_for_text(painter, "Dashboard");
    REQUIRE(sidebar.has_value());
    REQUIRE(main.has_value());

    CHECK(main->x > sidebar->x + 180);
    CHECK(main->y >= sidebar->y - 4);
    CHECK(main->y <= sidebar->y + 40);
}

TEST_CASE("Bootstrap 5 dashboard sidebar is not squeezed by wide main content") {
    auto painter = render_bootstrap5(R"HTML(
        <style>
        .sidebar { min-height: calc(100vh - 44px); }
        .chart-grid { height: 250px; display: flex; align-items: end; gap: 18px; }
        .chart-bar { width: 100%; min-width: 32px; }
        </style>
        <div class="container-fluid">
            <div class="row">
                <nav class="col-md-3 col-lg-2 d-md-block bg-body-tertiary sidebar collapse">
                    <h6 class="px-3">Workspace</h6>
                    <ul class="nav flex-column">
                        <li class="nav-item"><a class="nav-link active" href="#">Dashboard</a></li>
                        <li class="nav-item"><a class="nav-link" href="#">Orders</a></li>
                    </ul>
                </nav>
                <main class="col-md-9 ms-sm-auto col-lg-10 px-md-4 dashboard-main">
                    <div class="row row-cols-1 row-cols-xl-4 g-3 mb-4">
                      <div class="col"><div class="card"><div class="card-body"><div class="display-6">184.2k</div></div></div></div>
                      <div class="col"><div class="card"><div class="card-body"><div class="display-6">1,428</div></div></div></div>
                      <div class="col"><div class="card"><div class="card-body"><div class="display-6">38.4%</div></div></div></div>
                      <div class="col"><div class="card"><div class="card-body"><div class="display-6">17</div></div></div></div>
                    </div>
                    <div class="chart-grid">
                      <div class="chart-bar bg-primary"></div><div class="chart-bar bg-primary"></div>
                      <div class="chart-bar bg-primary"></div><div class="chart-bar bg-primary"></div>
                      <div class="chart-bar bg-success"></div><div class="chart-bar bg-success"></div>
                    </div>
                    <table class="table">
                      <tbody>
                        <tr><td>Aster Freight</td><td>Morgan</td><td><button class="btn btn-sm btn-outline-primary">Open</button></td></tr>
                        <tr><td>Orbit Labs</td><td>Iris</td><td><button class="btn btn-sm btn-outline-primary">Open</button></td></tr>
                      </tbody>
                    </table>
                </main>
            </div>
        </div>
    )HTML", 1440, 920);

    const auto sidebar = point_for_text(painter, "Workspace");
    const auto main = point_for_text(painter, "184.2k");
    REQUIRE(sidebar.has_value());
    REQUIRE(main.has_value());

    CHECK(main->x > sidebar->x + 180);

    std::optional<affineui::Rect> sidebar_fill;
    const auto tertiary = affineui::Color::rgb(0xf8, 0xf9, 0xfa);
    for (const auto& fill : painter.fills) {
        if (!same_color(fill.color, tertiary)) continue;
        if (fill.rect.h < 100) continue;
        if (!sidebar_fill || fill.rect.w > sidebar_fill->w) {
            sidebar_fill = fill.rect;
        }
    }
    REQUIRE(sidebar_fill.has_value());
    CHECK(sidebar_fill->w >= 220);
}

TEST_CASE("Bootstrap dashboard example loads linked CSS and keeps sidebar wide") {
    const auto html =
        read_file(example_dir("10_bootstrap_dashboard") / "index.html");
    REQUIRE(!html.empty());

    auto painter =
        render_example("10_bootstrap_dashboard", html, 1440, 920);

    const auto sidebar = point_for_text(painter, "WORKSPACE");
    const auto main = point_for_text(painter, "Revenue");
    INFO(text_run_summary(painter));
    REQUIRE(sidebar.has_value());
    REQUIRE(main.has_value());

    CHECK(sidebar->x < 80);
    CHECK(main->x > sidebar->x + 220);

    std::optional<affineui::Rect> sidebar_fill;
    const auto tertiary = affineui::Color::rgb(0xf8, 0xf9, 0xfa);
    for (const auto& fill : painter.fills) {
        if (!same_color(fill.color, tertiary)) continue;
        if (fill.rect.h < 100) continue;
        if (!sidebar_fill || fill.rect.w > sidebar_fill->w) {
            sidebar_fill = fill.rect;
        }
    }
    REQUIRE(sidebar_fill.has_value());
    CHECK(sidebar_fill->w >= 220);
}

TEST_CASE("Bootstrap button row preserves inline spacing and paints focusable variants") {
    auto painter = render_bootstrap(R"HTML(
        <main class="container py-4">
            <div class="jumbotron">
                <button class="btn btn-primary">Primary action</button>
                <button class="btn btn-secondary">Secondary</button>
                <button class="btn btn-outline-primary">Outline</button>
            </div>
        </main>
    )HTML");

    const auto primary = point_for_text(painter, "Primary action");
    const auto secondary = point_for_text(painter, "Secondary");
    const auto outline = point_for_text(painter, "Outline");
    REQUIRE(primary.has_value());
    REQUIRE(secondary.has_value());
    REQUIRE(outline.has_value());

    CHECK(secondary->x > primary->x + 100);
    CHECK(outline->x > secondary->x + 70);
    CHECK(saw_fill(painter, affineui::Color::rgb(0x00, 0x7b, 0xff)));
    CHECK(saw_fill(painter, affineui::Color::rgb(0x6c, 0x75, 0x7d)));
    CHECK(saw_stroke(painter, affineui::Color::rgb(0x00, 0x7b, 0xff)));
}

TEST_CASE("Centered auto-sized labels draw without tight wrap rebreak") {
    auto painter = render_bootstrap5(R"HTML(
        <style>
        .tight-centered {
            display: inline-block;
            text-align: center;
            padding: 0;
            border: 0;
            font-size: 16px;
        }
        </style>
        <button class="tight-centered">Primary action</button>
    )HTML", 320, 80);

    const auto* label = text_run_for(painter, "Primary action");
    REQUIRE(label != nullptr);
    CHECK(label->align == affineui::Painter::TextAlign::Left);
    CHECK(label->max_width > 60000.0f);
}

TEST_CASE("Bootstrap static JavaScript component states are renderable markup") {
    auto painter = render_bootstrap(R"HTML(
        <main class="container py-4">
            <div class="alert alert-success alert-dismissible fade show" role="alert">
                <strong>Notification:</strong> store entitlement refreshed.
                <button type="button" class="close" aria-label="Close">
                    <span aria-hidden="true">&times;</span>
                </button>
            </div>
            <div class="toast show mb-3" role="alert" aria-live="assertive" aria-atomic="true">
                <div class="toast-header">
                    <strong class="mr-auto">Toast</strong>
                    <small>now</small>
                    <button type="button" class="ml-2 mb-1 close" aria-label="Close">
                        <span aria-hidden="true">&times;</span>
                    </button>
                </div>
                <div class="toast-body">Friend request accepted.</div>
            </div>
            <div class="modal d-block position-static" tabindex="-1" role="dialog">
                <div class="modal-dialog modal-sm" role="document">
                    <div class="modal-content">
                        <div class="modal-header">
                            <h5 class="modal-title">Modal title</h5>
                            <button type="button" class="close" aria-label="Close">
                                <span aria-hidden="true">&times;</span>
                            </button>
                        </div>
                        <div class="modal-body">
                            <p>Static modal content.</p>
                        </div>
                    </div>
                </div>
            </div>
            <div class="dropdown show mb-3">
                <button class="btn btn-secondary dropdown-toggle">Static dropdown</button>
                <div class="dropdown-menu show">
                    <a class="dropdown-item" href="#">Action</a>
                </div>
            </div>
            <div class="collapse show">
                <div class="card card-body">Static collapse panel</div>
            </div>
        </main>
    )HTML");

    CHECK(text_contains(painter, "Notification:"));
    CHECK(text_contains(painter, "store entitlement refreshed."));
    CHECK(text_contains(painter, "Toast"));
    CHECK(text_contains(painter, "Friend request accepted."));
    CHECK(text_contains(painter, "Modal title"));
    CHECK(text_contains(painter, "Static modal content."));
    CHECK(text_contains(painter, "Static dropdown"));
    CHECK(text_contains(painter, "Action"));
    CHECK(text_contains(painter, "Static collapse panel"));
    CHECK(text_contains(painter, "\xC3\x97"));
}

TEST_CASE("Bootstrap status components paint framework colors") {
    auto painter = render_bootstrap(R"HTML(
        <main class="container py-4">
            <div class="alert alert-primary">Primary alert</div>
            <span class="badge badge-success">Badge</span>
            <button class="btn btn-secondary">Secondary</button>
            <button class="btn btn-outline-primary">Outline</button>
            <ul class="list-group">
                <li class="list-group-item active">Active list item</li>
            </ul>
            <div class="progress">
                <div class="progress-bar" style="width: 66%">66%</div>
            </div>
        </main>
    )HTML");

    CHECK(saw_fill(painter, affineui::Color::rgb(0xcc, 0xe5, 0xff)));
    CHECK(saw_fill(painter, affineui::Color::rgb(0x28, 0xa7, 0x45)));
    CHECK(saw_fill(painter, affineui::Color::rgb(0x6c, 0x75, 0x7d)));
    CHECK(saw_fill(painter, affineui::Color::rgb(0x00, 0x7b, 0xff)));
    CHECK(saw_stroke(painter, affineui::Color::rgb(0x00, 0x7b, 0xff)));
}
