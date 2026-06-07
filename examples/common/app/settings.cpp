#include "app/settings.h"

#include <fstream>
#include <sstream>
#include <string>

namespace app {

bool SettingsStore::get_bool(std::string_view key, bool fb) const {
    const auto* v = toml::find(root_, key);
    return v ? v->as_bool(fb) : fb;
}
long long SettingsStore::get_int(std::string_view key, long long fb) const {
    const auto* v = toml::find(root_, key);
    return v ? v->as_int(fb) : fb;
}
double SettingsStore::get_double(std::string_view key, double fb) const {
    const auto* v = toml::find(root_, key);
    return v ? v->as_double(fb) : fb;
}
std::string SettingsStore::get_string(std::string_view key,
                                      std::string_view fb) const {
    const auto* v = toml::find(root_, key);
    return v ? v->as_string(fb) : std::string(fb);
}
bool SettingsStore::has(std::string_view key) const {
    return toml::find(root_, key) != nullptr;
}

void SettingsStore::set_bool(std::string_view key, bool value) {
    toml::set(root_, key, toml::Value(value));
}
void SettingsStore::set_int(std::string_view key, long long value) {
    toml::set(root_, key, toml::Value(value));
}
void SettingsStore::set_double(std::string_view key, double value) {
    toml::set(root_, key, toml::Value(value));
}
void SettingsStore::set_string(std::string_view key, std::string_view value) {
    toml::set(root_, key, toml::Value(std::string(value)));
}

bool SettingsStore::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { root_.clear(); return false; }
    std::ostringstream ss;
    ss << in.rdbuf();
    root_ = toml::parse(ss.str());
    return true;
}

bool SettingsStore::save(const std::string& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << toml::dump(root_);
    return static_cast<bool>(out);
}

// ── Workspace conveniences ──────────────────────────────────────────────────

namespace {
std::string panel_key(std::string_view panel_id, std::string_view leaf) {
    // Workspace layout lives under a [panels] table: panels.<id>.<leaf>.
    return "panels." + std::string(panel_id) + "." + std::string(leaf);
}
}  // namespace

bool Workspace::panel_open(std::string_view panel_id, bool fb) const {
    return get_bool(panel_key(panel_id, "open"), fb);
}
void Workspace::set_panel_open(std::string_view panel_id, bool open) {
    set_bool(panel_key(panel_id, "open"), open);
}
int Workspace::panel_size(std::string_view panel_id, int fb) const {
    return static_cast<int>(get_int(panel_key(panel_id, "size"), fb));
}
void Workspace::set_panel_size(std::string_view panel_id, int size) {
    set_int(panel_key(panel_id, "size"), size);
}

}  // namespace app
