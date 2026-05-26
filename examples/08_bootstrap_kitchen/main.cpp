// bootstrap_kitchen — broader Bootstrap CSS compatibility coverage.
//
// JavaScript-backed Bootstrap components are represented in static
// states. AffineUI-native C++ behavior modules can later bind the same
// markup/classes without embedding a JavaScript engine.

#include <affineui/affineui.h>

#include <sokol_log.h>

#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

namespace {

std::string read_first_existing(std::initializer_list<const char*> paths) {
    for (const char* path : paths) {
        std::ifstream file{path, std::ios::binary};
        if (!file.good()) continue;
        std::stringstream bytes;
        bytes << file.rdbuf();
        return bytes.str();
    }
    return {};
}

}  // namespace

int main() {
    affineui::Ui ui;
    ui.set_clear_color(affineui::Color::rgb(0xff, 0xff, 0xff));
    ui.css(read_first_existing({
        "examples/08_bootstrap_kitchen/bootstrap-4.6.2.min.css",
        "bootstrap-4.6.2.min.css",
        "examples/01_bootstrap/bootstrap-4.6.2.min.css",
    }));

    ui.html(R"HTML(
        <nav class="navbar navbar-expand navbar-dark bg-dark">
            <a class="navbar-brand" href="#">AffineUI</a>
            <div class="navbar-nav">
                <a class="nav-link active" href="#">Kitchen</a>
                <a class="nav-link" href="#">Forms</a>
                <a class="nav-link" href="#">Panels</a>
            </div>
        </nav>

        <main class="container py-4">
            <div class="alert alert-primary" role="alert">
                Bootstrap alert, badge, nav, form, table, progress, list-group,
                toast, modal, toggle, and static JS-component states.
                <span class="badge badge-light">4.6</span>
            </div>

            <ul class="nav nav-tabs mb-3">
                <li class="nav-item"><a class="nav-link active" href="#">Active tab</a></li>
                <li class="nav-item"><a class="nav-link" href="#">Other tab</a></li>
                <li class="nav-item"><a class="nav-link disabled" href="#">Disabled</a></li>
            </ul>

            <div class="row mb-3">
                <div class="col">
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
                        <div class="toast-body">
                            Friend request accepted.
                        </div>
                    </div>
                </div>

                <div class="col">
                    <div class="btn-group btn-group-toggle mb-3">
                        <button class="btn btn-outline-primary active" aria-pressed="true">On</button>
                        <button class="btn btn-outline-primary" aria-pressed="false">Off</button>
                    </div>
                    <div class="custom-control custom-switch mb-3">
                        <input type="checkbox" class="custom-control-input" id="kitchen-switch" checked>
                        <label class="custom-control-label" for="kitchen-switch">Custom switch</label>
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
                                <div class="modal-footer">
                                    <button class="btn btn-primary">Confirm</button>
                                    <button class="btn btn-secondary">Cancel</button>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>
            </div>

            <div class="row">
                <div class="col">
                    <div class="card mb-3">
                        <div class="card-header">Form controls</div>
                        <div class="card-body">
                            <div class="form-group">
                                <label for="email">Email address</label>
                                <input id="email" class="form-control" value="designer@example.com">
                            </div>
                            <div class="form-group">
                                <label for="kind">Select</label>
                                <select id="kind" class="form-control">
                                    <option>Store panel</option>
                                    <option>Debug tool</option>
                                </select>
                            </div>
                            <button class="btn btn-primary">Save</button>
                            <button class="btn btn-outline-secondary">Cancel</button>
                        </div>
                    </div>

                    <div class="list-group">
                        <a href="#" class="list-group-item list-group-item-action active">
                            Active list item
                        </a>
                        <a href="#" class="list-group-item list-group-item-action">
                            Regular list item
                        </a>
                        <a href="#" class="list-group-item list-group-item-action disabled">
                            Disabled list item
                        </a>
                    </div>
                </div>

                <div class="col">
                    <table class="table table-sm table-striped table-bordered">
                        <thead class="thead-dark">
                            <tr><th>Component</th><th>Status</th></tr>
                        </thead>
                        <tbody>
                            <tr><td>Buttons</td><td><span class="badge badge-success">on</span></td></tr>
                            <tr><td>Forms</td><td><span class="badge badge-warning">partial</span></td></tr>
                            <tr><td>Images</td><td><span class="badge badge-info">next</span></td></tr>
                        </tbody>
                    </table>

                    <div class="progress mb-3">
                        <div class="progress-bar" style="width: 66%">66%</div>
                    </div>

                    <div class="dropdown show mb-3">
                        <button class="btn btn-secondary dropdown-toggle">Static dropdown</button>
                        <div class="dropdown-menu show">
                            <a class="dropdown-item" href="#">Action</a>
                            <a class="dropdown-item" href="#">Another action</a>
                        </div>
                    </div>

                    <div class="collapse show">
                        <div class="card card-body">
                            Static collapse panel shown with Bootstrap classes.
                        </div>
                    </div>
                </div>
            </div>
        </main>
    )HTML");

    sapp_desc desc{};
    desc.width         = 1280;
    desc.height        = 900;
    desc.window_title  = "AffineUI — Bootstrap kitchen";
    desc.high_dpi      = true;
    desc.swap_interval = 1;
    desc.sample_count  = 1;
    desc.logger.func   = slog_func;
    affineui::sokol::wire(desc, ui);
    sapp_run(&desc);
    return 0;
}
