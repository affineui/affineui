#pragma once

// ── DCC template: tiny TOML reader/writer ───────────────────────────────────
//
// EXAMPLE / TEMPLATE CODE. A small, dependency-free TOML subset — enough for
// app preferences and saved workspace layout, which is what TOML is great for
// (human-readable, hand-editable, comments, nested tables). It is intentionally
// NOT a full TOML implementation: it supports string / integer / float /
// boolean values, arrays of those, `[table]` and `[a.b.c]` headers, and dotted
// keys (`a.b = 1`). No dates, no inline tables, no multi-line strings. Extend
// as your app needs; swap for a full TOML library if you outgrow it.

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace app::toml {

class Value;
using Array = std::vector<Value>;
// Ordered map so round-tripping a file keeps a stable, diff-friendly order.
using Table = std::vector<std::pair<std::string, Value>>;

/// A TOML value: one of the scalar types, an array, or a sub-table.
class Value {
public:
    using Storage =
        std::variant<std::monostate, bool, long long, double, std::string,
                     Array, Table>;

    Value() = default;
    Value(bool v) : data_(v) {}                       // NOLINT
    Value(long long v) : data_(v) {}                  // NOLINT
    Value(int v) : data_(static_cast<long long>(v)) {}// NOLINT
    Value(double v) : data_(v) {}                     // NOLINT
    Value(std::string v) : data_(std::move(v)) {}     // NOLINT
    Value(const char* v) : data_(std::string(v)) {}   // NOLINT
    Value(Array v) : data_(std::move(v)) {}           // NOLINT
    Value(Table v) : data_(std::move(v)) {}           // NOLINT

    [[nodiscard]] bool is_table() const { return std::holds_alternative<Table>(data_); }
    [[nodiscard]] bool is_array() const { return std::holds_alternative<Array>(data_); }

    [[nodiscard]] bool        as_bool(bool fb = false) const;
    [[nodiscard]] long long   as_int(long long fb = 0) const;
    [[nodiscard]] double      as_double(double fb = 0.0) const;
    [[nodiscard]] std::string as_string(std::string_view fb = {}) const;
    [[nodiscard]] const Array* as_array() const {
        return std::get_if<Array>(&data_);
    }
    [[nodiscard]] Table*       as_table() { return std::get_if<Table>(&data_); }
    [[nodiscard]] const Table* as_table() const { return std::get_if<Table>(&data_); }

    [[nodiscard]] const Storage& storage() const noexcept { return data_; }
    [[nodiscard]] Storage&       storage() noexcept { return data_; }

private:
    Storage data_;
};

// ── Table helpers (dotted-path get/set over the ordered table) ──────────────

/// Find a value by dotted path ("ui.dock.outliner.width"), or null.
[[nodiscard]] const Value* find(const Table& table, std::string_view dotted_key);

/// Set a value by dotted path, creating intermediate sub-tables as needed.
void set(Table& table, std::string_view dotted_key, Value value);

// ── Serialisation ───────────────────────────────────────────────────────────

/// Parse TOML text into a root table. On a malformed line the parser skips it
/// (best-effort, never throws) so a partially-corrupt settings file still
/// loads what it can.
[[nodiscard]] Table parse(std::string_view text);

/// Serialise a root table back to TOML text. Scalars first, then `[tables]`.
[[nodiscard]] std::string dump(const Table& table);

}  // namespace app::toml
