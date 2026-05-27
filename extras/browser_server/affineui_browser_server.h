#pragma once

// Tiny browser-target support for AffineUI.
//
// The remote browser backend needs only a small static asset server plus a
// message transport. This header defines the built-in boot page, the browser
// patch applier, and a plain asset table a single-file C/C++ HTTP server can
// serve. The socket server implementation is deliberately optional so native
// users who never target a browser do not pick up networking dependencies.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace affineui::browser {

struct Asset {
    std::string path;
    std::string content_type;
    std::string body;
};

class AssetStore {
public:
    void add(std::string path, std::string content_type, std::string body) {
        assets_.push_back({std::move(path), std::move(content_type), std::move(body)});
    }

    const Asset* find(std::string_view path) const {
        for (const auto& asset : assets_) {
            if (asset.path == path) return &asset;
        }
        return nullptr;
    }

    const std::vector<Asset>& assets() const noexcept { return assets_; }

    bool add_file(std::string path,
                  std::string content_type,
                  const std::filesystem::path& file_path) {
        std::ifstream file{file_path, std::ios::binary};
        if (!file.good()) return false;
        std::stringstream bytes;
        bytes << file.rdbuf();
        add(std::move(path), std::move(content_type), bytes.str());
        return true;
    }

private:
    std::vector<Asset> assets_;
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

struct HttpResponse {
    int status{200};
    std::string content_type{"text/plain; charset=utf-8"};
    std::string body;
    bool keep_open{false};
};

inline std::string status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        default: return "OK";
    }
}

inline std::string http_response_bytes(const HttpResponse& response) {
    std::string out;
    out += "HTTP/1.1 " + std::to_string(response.status) + " " +
           status_text(response.status) + "\r\n";
    out += "content-type: " + response.content_type + "\r\n";
    out += "cache-control: no-cache\r\n";
    out += "connection: ";
    out += response.keep_open ? "keep-alive\r\n" : "close\r\n";
    out += "content-length: " + std::to_string(response.body.size()) + "\r\n";
    out += "\r\n";
    out += response.body;
    return out;
}

inline std::string sse_event(std::string_view name, std::string_view data) {
    std::string out;
    out += "event: ";
    out += name;
    out += "\n";
    out += "data: ";
    out += data;
    out += "\n\n";
    return out;
}

inline std::string sse_patches(std::string_view patches_json) {
    return sse_event("patches", patches_json);
}

inline AssetStore default_assets(std::string_view title = "AffineUI");

class TransportCore {
public:
    explicit TransportCore(AssetStore assets = default_assets())
        : assets_(std::move(assets)) {}

    AssetStore& assets() noexcept { return assets_; }
    const AssetStore& assets() const noexcept { return assets_; }

    HttpResponse handle_request(const HttpRequest& request) {
        if (request.method == "GET") {
            if (request.path == "/stream") {
                return {
                    200,
                    "text/event-stream; charset=utf-8",
                    pending_stream_.empty()
                        ? std::string{": affineui stream ready\n\n"}
                        : std::exchange(pending_stream_, {}),
                    true,
                };
            }
            if (const auto* asset = assets_.find(request.path)) {
                return {200, asset->content_type, asset->body, false};
            }
            return {404, "text/plain; charset=utf-8", "not found\n", false};
        }

        if (request.method == "POST" && request.path == "/events") {
            events_.push_back(request.body);
            return {202, "text/plain; charset=utf-8", "accepted\n", false};
        }

        return {405, "text/plain; charset=utf-8", "method not allowed\n", false};
    }

    void enqueue_patches_json(std::string_view patches_json) {
        pending_stream_ += sse_patches(patches_json);
    }

    std::vector<std::string> take_events() {
        return std::exchange(events_, {});
    }

private:
    AssetStore assets_;
    std::string pending_stream_;
    std::vector<std::string> events_;
};

inline std::string_view bridge_js() {
    return R"JS(
(() => {
  const textNodes = new Map();
  const root = () => document.getElementById("aui-root");
  const byId = id => document.getElementById(id);
  const byAuiId = id => byId(id) || textNodes.get(id);
  const ensureParent = p => p ? byId(p) : root();
  const make = patch => {
    if (patch.op === "create_text") return document.createTextNode("");
    return document.createElement(patch.tag || "div");
  };
  const attach = (parent, child, index) => {
    if (!parent) return;
    const before = Number.isInteger(index) ? parent.childNodes[index] : null;
    parent.insertBefore(child, before || null);
  };
  window.affineuiApplyPatches = patches => {
    for (const patch of patches || []) {
      if (patch.op === "create_element" || patch.op === "create_text") {
        if (byAuiId(patch.id)) continue;
        const el = make(patch);
        if (patch.op !== "create_text") el.id = patch.id;
        else textNodes.set(patch.id, el);
        attach(ensureParent(patch.parent), el, patch.index);
      } else if (patch.op === "remove") {
        const el = byAuiId(patch.id);
        if (el && el.parentNode) el.parentNode.removeChild(el);
        textNodes.delete(patch.id);
      } else if (patch.op === "set_text") {
        const el = byAuiId(patch.id);
        if (el) el.textContent = patch.value || "";
      } else if (patch.op === "set_attr") {
        const el = byId(patch.id);
        if (el) el.setAttribute(patch.name, patch.value || "");
      } else if (patch.op === "remove_attr") {
        const el = byId(patch.id);
        if (el) el.removeAttribute(patch.name);
      }
    }
  };
  window.affineuiSendEvent = event => {
    if (window.affineuiPostEvent) return window.affineuiPostEvent(event);
    return fetch("/events", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(event)
    }).catch(err => console.debug("AffineUI event post failed", err));
  };
  window.affineuiConnect = (url = "/stream") => {
    if (!window.EventSource) return null;
    const source = new EventSource(url);
    source.addEventListener("patches", event => {
      try { window.affineuiApplyPatches(JSON.parse(event.data)); }
      catch (err) { console.error("AffineUI patch parse failed", err); }
    });
    source.onerror = err => console.debug("AffineUI stream error", err);
    return source;
  };
  document.addEventListener("click", event => {
    const target = event.target && event.target.closest && event.target.closest("[id^='aui-']");
    if (!target || target.id === "aui-root") return;
    window.affineuiSendEvent({ type: "click", id: target.id });
  });
  window.addEventListener("DOMContentLoaded", () => window.affineuiConnect());
})();
)JS";
}

inline std::string boot_html(std::string_view title = "AffineUI") {
    std::string html;
    html += "<!doctype html><html><head><meta charset=\"utf-8\">";
    html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    html += "<title>";
    html += title;
    html += "</title>";
    html += "<link rel=\"stylesheet\" href=\"/frameworks/css/bootstrap-5.3.8.min.css\">";
    html += "<script defer src=\"/affineui.js\"></script>";
    html += "</head><body><main id=\"aui-root\"></main></body></html>";
    return html;
}

inline AssetStore default_assets(std::string_view title) {
    AssetStore store;
    store.add("/", "text/html; charset=utf-8", boot_html(title));
    store.add("/index.html", "text/html; charset=utf-8", boot_html(title));
    store.add("/affineui.js", "text/javascript; charset=utf-8", std::string{bridge_js()});
    return store;
}

inline void add_framework_assets(AssetStore& store,
                                 const std::filesystem::path& frameworks_dir) {
    store.add_file("/frameworks/css/bootstrap-5.3.8.min.css",
                   "text/css; charset=utf-8",
                   frameworks_dir / "css" / "bootstrap-5.3.8.min.css");
    store.add_file("/frameworks/css/decius-css-0.4.1.bundle.min.css",
                   "text/css; charset=utf-8",
                   frameworks_dir / "css" / "decius-css-0.4.1.bundle.min.css");
    store.add_file("/frameworks/fonts/decius-icons.woff2",
                   "font/woff2",
                   frameworks_dir / "fonts" / "decius-icons.woff2");
    store.add_file("/frameworks/fonts/decius-icons.woff",
                   "font/woff",
                   frameworks_dir / "fonts" / "decius-icons.woff");
    store.add_file("/frameworks/fonts/decius-icons.ttf",
                   "font/ttf",
                   frameworks_dir / "fonts" / "decius-icons.ttf");
}

}  // namespace affineui::browser
