#include "app/command.h"

namespace app {

namespace {

template <typename T>
T value_or(const ArgValue* v, T fallback) {
    if (v == nullptr) return fallback;
    if (const auto* typed = std::get_if<T>(v)) return *typed;
    return fallback;
}

}  // namespace

Args& Args::set(std::string key, ArgValue value) {
    for (auto& entry : entries_) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return *this;
        }
    }
    entries_.emplace_back(std::move(key), std::move(value));
    return *this;
}

bool Args::has(std::string_view key) const noexcept {
    return find(key) != nullptr;
}

const ArgValue* Args::find(std::string_view key) const noexcept {
    for (const auto& entry : entries_) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

bool Args::get_bool(std::string_view key, bool fallback) const {
    return value_or<bool>(find(key), fallback);
}

long long Args::get_int(std::string_view key, long long fallback) const {
    return value_or<long long>(find(key), fallback);
}

double Args::get_double(std::string_view key, double fallback) const {
    // Accept an int stored where a double is asked for, the common case for
    // numeric arguments that happen to be whole numbers.
    if (const auto* v = find(key)) {
        if (const auto* d = std::get_if<double>(v)) return *d;
        if (const auto* i = std::get_if<long long>(v)) {
            return static_cast<double>(*i);
        }
    }
    return fallback;
}

std::string Args::get_string(std::string_view key,
                             std::string_view fallback) const {
    return value_or<std::string>(find(key), std::string(fallback));
}

}  // namespace app
