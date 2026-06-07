#include "app/toml.h"

#include <charconv>
#include <cstdlib>
#include <sstream>

namespace app::toml {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Find a direct child slot in a table by key (creating none).
Value* child(Table& table, std::string_view key) {
    for (auto& [k, v] : table) {
        if (k == key) return &v;
    }
    return nullptr;
}

// Get-or-create a sub-table child, returning it.
Table& child_table(Table& table, std::string_view key) {
    if (Value* existing = child(table, key)) {
        if (!existing->is_table()) *existing = Value(Table{});
        return *existing->as_table();
    }
    table.emplace_back(std::string(key), Value(Table{}));
    return *table.back().second.as_table();
}

// Split a dotted key into segments.
std::vector<std::string_view> split_dotted(std::string_view key) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= key.size(); ++i) {
        if (i == key.size() || key[i] == '.') {
            parts.push_back(trim(key.substr(start, i - start)));
            start = i + 1;
        }
    }
    return parts;
}

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// Parse a scalar/array value token (the RHS of `key = ...`).
Value parse_value(std::string_view tok) {
    tok = trim(tok);
    if (tok.empty()) return Value{};

    if (tok.front() == '"') {  // string
        std::string out;
        for (std::size_t i = 1; i < tok.size(); ++i) {
            char c = tok[i];
            if (c == '\\' && i + 1 < tok.size()) { out.push_back(tok[++i]); continue; }
            if (c == '"') break;
            out.push_back(c);
        }
        return Value(out);
    }
    if (tok.front() == '[') {  // array (of scalars)
        Array arr;
        std::size_t i = 1;
        std::size_t depth = 1;
        std::string cur;
        auto flush = [&] {
            auto t = trim(cur);
            if (!t.empty()) arr.push_back(parse_value(t));
            cur.clear();
        };
        for (; i < tok.size() && depth > 0; ++i) {
            char c = tok[i];
            if (c == '[') { ++depth; cur.push_back(c); }
            else if (c == ']') { if (--depth == 0) break; cur.push_back(c); }
            else if (c == ',' && depth == 1) { flush(); }
            else cur.push_back(c);
        }
        flush();
        return Value(std::move(arr));
    }
    if (tok == "true") return Value(true);
    if (tok == "false") return Value(false);

    // Number: integer if no '.'/'e', else float.
    const bool floaty = tok.find('.') != std::string_view::npos ||
                        tok.find('e') != std::string_view::npos ||
                        tok.find('E') != std::string_view::npos;
    if (!floaty) {
        long long iv = 0;
        const auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), iv);
        if (ec == std::errc{} && p == tok.data() + tok.size()) return Value(iv);
    }
    // strtod for floats (from_chars<double> support is uneven across stdlibs).
    std::string s(tok);
    char* end = nullptr;
    double dv = std::strtod(s.c_str(), &end);
    if (end == s.c_str() + s.size()) return Value(dv);

    return Value(s);  // fall back to bare string
}

void dump_value(const Value& v, std::string& out);

void dump_array(const Array& arr, std::string& out) {
    out.push_back('[');
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (i) out += ", ";
        dump_value(arr[i], out);
    }
    out.push_back(']');
}

void dump_value(const Value& v, std::string& out) {
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, bool>) out += x ? "true" : "false";
        else if constexpr (std::is_same_v<T, long long>) out += std::to_string(x);
        else if constexpr (std::is_same_v<T, double>) {
            std::ostringstream ss; ss << x; out += ss.str();
        }
        else if constexpr (std::is_same_v<T, std::string>) out += quote(x);
        else if constexpr (std::is_same_v<T, Array>) dump_array(x, out);
        else { /* table / monostate handled by caller */ }
    }, v.storage());
}

// Emit a table's scalars/arrays, then recurse into sub-tables under a header.
void dump_table(const Table& table, const std::string& prefix, std::string& out) {
    for (const auto& [k, v] : table) {
        if (v.is_table()) continue;
        out += k;
        out += " = ";
        dump_value(v, out);
        out.push_back('\n');
    }
    for (const auto& [k, v] : table) {
        if (!v.is_table()) continue;
        const std::string header = prefix.empty() ? k : prefix + "." + k;
        out.push_back('\n');
        out += "[" + header + "]\n";
        dump_table(*v.as_table(), header, out);
    }
}

}  // namespace

bool Value::as_bool(bool fb) const {
    if (const auto* b = std::get_if<bool>(&data_)) return *b;
    return fb;
}
long long Value::as_int(long long fb) const {
    if (const auto* i = std::get_if<long long>(&data_)) return *i;
    if (const auto* d = std::get_if<double>(&data_)) return static_cast<long long>(*d);
    return fb;
}
double Value::as_double(double fb) const {
    if (const auto* d = std::get_if<double>(&data_)) return *d;
    if (const auto* i = std::get_if<long long>(&data_)) return static_cast<double>(*i);
    return fb;
}
std::string Value::as_string(std::string_view fb) const {
    if (const auto* s = std::get_if<std::string>(&data_)) return *s;
    return std::string(fb);
}

const Value* find(const Table& table, std::string_view dotted_key) {
    const auto parts = split_dotted(dotted_key);
    const Table* cur = &table;
    const Value* found = nullptr;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        found = nullptr;
        for (const auto& [k, v] : *cur) {
            if (k == parts[i]) { found = &v; break; }
        }
        if (!found) return nullptr;
        if (i + 1 < parts.size()) {
            cur = found->as_table();
            if (!cur) return nullptr;
        }
    }
    return found;
}

void set(Table& table, std::string_view dotted_key, Value value) {
    const auto parts = split_dotted(dotted_key);
    Table* cur = &table;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        cur = &child_table(*cur, parts[i]);
    }
    const std::string_view leaf = parts.back();
    if (Value* existing = child(*cur, leaf)) {
        *existing = std::move(value);
    } else {
        cur->emplace_back(std::string(leaf), std::move(value));
    }
}

Table parse(std::string_view text) {
    Table root;
    Table* current = &root;            // table the current key/values land in
    std::string current_path;          // for nested header creation

    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string_view line =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                          : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

        line = trim(line);
        if (line.empty() || line.front() == '#') continue;

        if (line.front() == '[') {  // table header [a.b.c]
            const std::size_t close = line.find(']');
            if (close == std::string_view::npos) continue;
            const std::string_view header = trim(line.substr(1, close - 1));
            // Walk/create the table path from the root.
            current = &root;
            for (auto part : split_dotted(header)) {
                current = &child_table(*current, part);
            }
            current_path = std::string(header);
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;  // skip malformed
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view val = trim(line.substr(eq + 1));
        if (key.empty()) continue;
        // Dotted keys land relative to the current table.
        set(*current, key, parse_value(val));
    }
    return root;
}

std::string dump(const Table& table) {
    std::string out;
    dump_table(table, {}, out);
    return out;
}

}  // namespace app::toml
