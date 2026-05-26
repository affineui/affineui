// bootstrap_dashboard - polished dashboard-template style Bootstrap demo.

#include "demo_assets.h"

#include <affineui/affineui.h>

#include <sokol_log.h>

#include <sstream>
#include <string>

namespace {

struct DashboardState {
    int  refresh_count{0};
    int  selected_region{0};
    bool compact_table{false};
};

std::string kpi(std::string_view title,
                std::string_view value,
                std::string_view trend,
                std::string_view tone) {
    std::ostringstream h;
    h << "<div class=\"col\">"
      << "<div class=\"card h-100 border-0 shadow-sm metric-card border-start border-4 border-" << tone << "\">"
      << "<div class=\"card-body\">"
      << "<div class=\"text-uppercase text-secondary small fw-semibold\">" << title << "</div>"
      << "<div class=\"d-flex align-items-baseline justify-content-between mt-2\">"
      << "<div class=\"display-6 fw-semibold\">" << value << "</div>"
      << "<span class=\"badge text-bg-" << tone << "\">" << trend << "</span>"
      << "</div></div></div></div>";
    return h.str();
}

std::string pipeline_row(int idx,
                         std::string_view account,
                         std::string_view owner,
                         std::string_view stage,
                         std::string_view value,
                         std::string_view tone,
                         bool selected) {
    std::ostringstream h;
    h << "<tr";
    if (selected) h << " class=\"table-primary\"";
    h << "><td><input class=\"form-check-input\" type=\"checkbox\"";
    if (selected) h << " checked";
    h << "></td><td class=\"fw-semibold\">" << account
      << "</td><td>" << owner
      << "</td><td><span class=\"badge rounded-pill text-bg-" << tone << "\">" << stage
      << "</span></td><td class=\"text-end font-monospace\">" << value
      << "</td><td class=\"text-end\"><button id=\"region-" << idx
      << "\" class=\"btn btn-sm btn-outline-primary\">Open</button></td></tr>";
    return h.str();
}

std::string render_dashboard(const DashboardState& s) {
    std::ostringstream h;
    h << R"HTML(
<div class="app-shell">
  <nav class="navbar navbar-dark sticky-top bg-dark flex-md-nowrap p-0 shadow">
    <a class="navbar-brand col-md-3 col-lg-2 me-0 px-3 fs-6" href="#">Northwind Console</a>
    <input class="form-control form-control-dark w-100 rounded-0 border-0" value="Search orders, accounts, or SKUs">
    <div class="navbar-nav flex-row">
      <button id="refresh" class="btn btn-dark px-3">Refresh</button>
      <button id="export" class="btn btn-primary px-3">Export</button>
    </div>
  </nav>
  <div class="container-fluid">
    <div class="row">
      <nav class="col-md-3 col-lg-2 d-md-block bg-body-tertiary sidebar collapse">
        <div class="position-sticky pt-3 sidebar-sticky">
          <h6 class="sidebar-heading d-flex justify-content-between align-items-center px-3 mt-3 mb-1 text-body-secondary text-uppercase">
            <span>Workspace</span><span class="badge text-bg-success">Live</span>
          </h6>
          <ul class="nav flex-column">
            <li class="nav-item"><a class="nav-link active" href="#">Dashboard</a></li>
            <li class="nav-item"><a class="nav-link" href="#">Orders</a></li>
            <li class="nav-item"><a class="nav-link" href="#">Customers</a></li>
            <li class="nav-item"><a class="nav-link" href="#">Inventory</a></li>
            <li class="nav-item"><a class="nav-link" href="#">Reports</a></li>
          </ul>
          <h6 class="sidebar-heading px-3 mt-4 mb-1 text-body-secondary text-uppercase">Saved views</h6>
          <ul class="nav flex-column mb-auto">
            <li class="nav-item"><a class="nav-link" href="#">Current month</a></li>
            <li class="nav-item"><a class="nav-link" href="#">Enterprise pipeline</a></li>
            <li class="nav-item"><a class="nav-link" href="#">Renewal risk</a></li>
          </ul>
        </div>
      </nav>
      <main class="col-md-9 ms-sm-auto col-lg-10 px-md-4 dashboard-main">
        <div class="d-flex justify-content-between flex-wrap flex-md-nowrap align-items-center pt-4 pb-3 mb-3 border-bottom">
          <div>
            <h1 class="h2 mb-1">Dashboard</h1>
            <div class="text-secondary">Quarter close command center. Last refresh )HTML"
      << s.refresh_count << R"HTML(.</div>
          </div>
          <div class="btn-toolbar gap-2">
            <button id="density" class="btn btn-sm btn-outline-secondary">Density</button>
            <button class="btn btn-sm btn-outline-secondary">This week</button>
          </div>
        </div>
        <div class="row row-cols-1 row-cols-xl-4 g-3 mb-4">
)HTML";
    h << kpi("Revenue", "$184.2k", "+12.8%", "success");
    h << kpi("Orders", "1,428", "+8.1%", "primary");
    h << kpi("Margin", "38.4%", "+2.4%", "info");
    h << kpi("Risk", "17", "-4", "warning");
    h << R"HTML(
        </div>
        <div class="row g-3 mb-4">
          <div class="col-xl-8">
            <div class="card border-0 shadow-sm">
              <div class="card-header bg-white d-flex justify-content-between align-items-center">
                <span class="fw-semibold">Revenue forecast</span>
                <span class="text-secondary small">Actual vs target</span>
              </div>
              <div class="card-body">
                <div class="chart-grid">
                  <div class="chart-bar bg-primary" style="height:44%"></div>
                  <div class="chart-bar bg-primary" style="height:58%"></div>
                  <div class="chart-bar bg-primary" style="height:52%"></div>
                  <div class="chart-bar bg-primary" style="height:74%"></div>
                  <div class="chart-bar bg-primary" style="height:68%"></div>
                  <div class="chart-bar bg-success" style="height:82%"></div>
                  <div class="chart-bar bg-success" style="height:76%"></div>
                  <div class="chart-bar bg-success" style="height:88%"></div>
                </div>
              </div>
            </div>
          </div>
          <div class="col-xl-4">
            <div class="card border-0 shadow-sm h-100">
              <div class="card-header bg-white fw-semibold">Region mix</div>
              <div class="card-body">
                <div class="d-flex align-items-center mb-2"><div class="region-dot bg-primary"></div><div class="flex-fill">North America</div><strong>47%</strong></div>
                <div class="progress mb-3"><div class="progress-bar" style="width:47%"></div></div>
                <div class="d-flex align-items-center mb-2"><div class="region-dot bg-info"></div><div class="flex-fill">Europe</div><strong>31%</strong></div>
                <div class="progress mb-3"><div class="progress-bar bg-info" style="width:31%"></div></div>
                <div class="d-flex align-items-center mb-2"><div class="region-dot bg-success"></div><div class="flex-fill">APAC</div><strong>22%</strong></div>
                <div class="progress"><div class="progress-bar bg-success" style="width:22%"></div></div>
              </div>
            </div>
          </div>
        </div>
        <div class="card border-0 shadow-sm">
          <div class="card-header bg-white d-flex justify-content-between align-items-center">
            <span class="fw-semibold">Enterprise pipeline</span>
            <span class="badge text-bg-light">)HTML"
      << (s.compact_table ? "Compact" : "Comfortable") << R"HTML(</span>
          </div>
          <div class="table-responsive">
            <table class="table table-hover align-middle mb-0 )HTML"
      << (s.compact_table ? "table-sm" : "") << R"HTML(">
              <thead><tr><th></th><th>Account</th><th>Owner</th><th>Stage</th><th class="text-end">Value</th><th></th></tr></thead>
              <tbody>
)HTML";
    h << pipeline_row(0, "Aster Freight", "Morgan", "Commit", "$42,600", "success", s.selected_region == 0);
    h << pipeline_row(1, "Orbit Labs", "Iris", "Legal", "$31,200", "primary", s.selected_region == 1);
    h << pipeline_row(2, "Vertex Medical", "Sam", "Security", "$27,900", "warning", s.selected_region == 2);
    h << pipeline_row(3, "Canopy Retail", "Noah", "Trial", "$18,400", "info", s.selected_region == 3);
    h << R"HTML(
              </tbody>
            </table>
          </div>
        </div>
      </main>
    </div>
  </div>
</div>
)HTML";
    return h.str();
}

}  // namespace

int main() {
    affineui::Ui ui;
    DashboardState state;

    ui.css(demo::read_first_existing({
        "frameworks/css/bootstrap-5.3.8.min.css",
        "examples/frameworks/css/bootstrap-5.3.8.min.css",
    }) + R"CSS(
        body { margin: 0; background-color: #f4f6f9; }
        .navbar-brand { font-weight: 700; letter-spacing: .02em; }
        .form-control-dark { color: #fff; background-color: rgba(255,255,255,.08); }
        .sidebar { min-height: calc(100vh - 44px); box-shadow: inset -1px 0 0 rgba(0,0,0,.08); }
        .sidebar .nav-link { color: #495057; font-weight: 500; }
        .sidebar .nav-link.active { color: #0d6efd; }
        .dashboard-main { min-height: calc(100vh - 44px); }
        .metric-card { border-radius: 10px; }
        .chart-grid { height: 250px; display: flex; align-items: end; gap: 18px; padding: 24px 12px 4px; border-bottom: 1px solid #dee2e6; }
        .chart-bar { width: 100%; min-width: 32px; border-radius: 6px 6px 0 0; opacity: .88; }
        .region-dot { width: 10px; height: 10px; border-radius: 50%; margin-right: 8px; }
        .table > :not(caption) > * > * { padding-top: .8rem; padding-bottom: .8rem; }
        .table-sm > :not(caption) > * > * { padding-top: .45rem; padding-bottom: .45rem; }
    )CSS");

    auto rerender = [&] {
        ui.html(render_dashboard(state));
        ui.mark_dirty();
    };
    rerender();

    ui.on_click("#refresh", [&] { ++state.refresh_count; rerender(); });
    ui.on_click("#density", [&] { state.compact_table = !state.compact_table; rerender(); });
    ui.on_click("#region-0", [&] { state.selected_region = 0; rerender(); });
    ui.on_click("#region-1", [&] { state.selected_region = 1; rerender(); });
    ui.on_click("#region-2", [&] { state.selected_region = 2; rerender(); });
    ui.on_click("#region-3", [&] { state.selected_region = 3; rerender(); });

    sapp_desc desc{};
    desc.width         = 1440;
    desc.height        = 920;
    desc.window_title  = "AffineUI - Bootstrap dashboard";
    desc.high_dpi      = true;
    desc.swap_interval = 0;
    desc.sample_count  = 1;
    desc.logger.func   = slog_func;
    affineui::sokol::wire(desc, ui, true);
    sapp_run(&desc);
    return 0;
}
