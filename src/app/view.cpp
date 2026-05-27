#include "affineui/view.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <utility>

namespace affineui {

namespace {

std::uint64_t fnv1a_mix(std::uint64_t h, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) {
        h ^= (x >> (i * 8)) & 0xffu;
        h *= 0x100000001b3ull;
    }
    return h;
}

std::uint64_t fnv1a_bytes(std::uint64_t h, std::string_view text) {
    for (unsigned char c : text) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

StableId make_stable_id(StableId parent,
                        WidgetKind kind,
                        std::string_view key,
                        std::source_location here) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    h = fnv1a_mix(h, parent.value);
    h = fnv1a_mix(h, static_cast<std::uint64_t>(kind));
    if (!key.empty()) {
        h = fnv1a_bytes(h, key);
    } else {
        h = fnv1a_bytes(h, here.file_name());
        h = fnv1a_mix(h, here.line());
        h = fnv1a_mix(h, here.column());
    }
    return {h};
}

StableId make_duplicate_stable_id(StableId base, std::size_t index) {
    return {fnv1a_mix(base.value, index + 1)};
}

std::string remote_id(StableId id) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "aui-%016llx",
                  static_cast<unsigned long long>(id.value));
    return std::string{buf};
}

const char* patch_op_name(RemotePatchOp op) {
    switch (op) {
        case RemotePatchOp::CreateElement:   return "create_element";
        case RemotePatchOp::CreateText:      return "create_text";
        case RemotePatchOp::Remove:          return "remove";
        case RemotePatchOp::SetText:         return "set_text";
        case RemotePatchOp::SetAttribute:    return "set_attr";
        case RemotePatchOp::RemoveAttribute: return "remove_attr";
    }
    return "unknown";
}

std::string escape_html(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string escape_json(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string number(double value) {
    char buf[64]{};
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    if (ec == std::errc{}) return std::string(buf, ptr);
    std::ostringstream out;
    out << value;
    return out.str();
}

double normalized_value(double value, double min, double max) {
    if (max <= min) return 0.0;
    return std::clamp((value - min) / (max - min), 0.0, 1.0);
}

std::string percent(double fraction) {
    return number(std::clamp(fraction, 0.0, 1.0) * 100.0) + "%";
}

double knob_angle(double value, double min, double max) {
    return -135.0 + normalized_value(value, min, max) * 270.0;
}

std::pair<double, double> knob_ring_point(double deg) {
    constexpr double r = 10.5;
    constexpr double pi = 3.14159265358979323846;
    const double rad = deg * pi / 180.0;
    return {12.0 + r * std::cos(rad), 12.0 + r * std::sin(rad)};
}

std::string knob_arc_path(double min, double max, double value, bool bipolar) {
    const double p = normalized_value(value, min, max);
    const double sweep_degrees = bipolar ? (p - 0.5) * 270.0 : p * 270.0;
    if (std::abs(sweep_degrees) <= 0.5) return {};

    const double start_degrees = bipolar ? -90.0 : -225.0;
    const double end_degrees = start_degrees + sweep_degrees;
    const auto [x0, y0] = knob_ring_point(start_degrees);
    const auto [x1, y1] = knob_ring_point(end_degrees);
    const int large = std::abs(sweep_degrees) > 180.0 ? 1 : 0;
    const int sweep = end_degrees >= start_degrees ? 1 : 0;

    return "M " + number(x0) + " " + number(y0) +
           " A 10.5 10.5 0 " + std::to_string(large) + " " +
           std::to_string(sweep) + " " + number(x1) + " " + number(y1);
}

void emit_remove_tree(ViewSink* sink, const WidgetNode& node) {
    if (!sink) return;
    sink->remove(node);
}

enum class FrameworkElement {
    Panel,
    Card,
    CardTitle,
    Button,
    CheckboxGroup,
    CheckboxInput,
    CheckboxLabel,
    SliderGroup,
    SliderLabel,
    SliderInput,
    KnobGroup,
    KnobLabel,
    KnobInput,
};

struct ElementRecipe {
    std::string_view tag;
    std::string_view classes;
};

class ViewFramework {
public:
    virtual ~ViewFramework() = default;

    [[nodiscard]] virtual std::string_view stylesheet_href() const noexcept = 0;
    [[nodiscard]] virtual std::string_view body_attrs() const noexcept = 0;
    [[nodiscard]] virtual Color background_color() const noexcept = 0;
    [[nodiscard]] virtual ElementRecipe element(FrameworkElement element,
                                                bool primary = false) const noexcept = 0;
};

class PlainFramework final : public ViewFramework {
public:
    [[nodiscard]] std::string_view stylesheet_href() const noexcept override {
        return {};
    }

    [[nodiscard]] std::string_view body_attrs() const noexcept override {
        return {};
    }

    [[nodiscard]] Color background_color() const noexcept override {
        return Color{30, 30, 46, 255};
    }

    [[nodiscard]] ElementRecipe element(FrameworkElement element,
                                        bool primary = false) const noexcept override {
        (void) primary;
        switch (element) {
            case FrameworkElement::Card:          return {"section", {}};
            case FrameworkElement::Button:        return {"button", {}};
            case FrameworkElement::CheckboxGroup: return {"label", {}};
            case FrameworkElement::CheckboxInput: return {"input", {}};
            case FrameworkElement::CheckboxLabel: return {"span", {}};
            case FrameworkElement::SliderGroup:   return {"label", {}};
            case FrameworkElement::SliderLabel:   return {"span", {}};
            case FrameworkElement::SliderInput:   return {"input", {}};
            case FrameworkElement::KnobGroup:     return {"label", {}};
            case FrameworkElement::KnobLabel:     return {"span", {}};
            case FrameworkElement::KnobInput:     return {"input", {}};
            case FrameworkElement::Panel:
            case FrameworkElement::CardTitle:
                return {"div", {}};
        }
        return {"div", {}};
    }
};

class BootstrapFramework final : public ViewFramework {
public:
    [[nodiscard]] std::string_view stylesheet_href() const noexcept override {
        return "frameworks/css/bootstrap-5.3.8.min.css";
    }

    [[nodiscard]] std::string_view body_attrs() const noexcept override {
        return {};
    }

    [[nodiscard]] Color background_color() const noexcept override {
        return Color{0xFF, 0xFF, 0xFF, 0xFF};
    }

    [[nodiscard]] ElementRecipe element(FrameworkElement element,
                                        bool primary = false) const noexcept override {
        switch (element) {
            case FrameworkElement::Panel:         return {"div", "container py-4"};
            case FrameworkElement::Card:          return {"section", "card shadow-sm"};
            case FrameworkElement::CardTitle:     return {"h3", "card-header h6 mb-0"};
            case FrameworkElement::Button:
                return {"button", primary ? "btn btn-primary" : "btn btn-outline-secondary"};
            case FrameworkElement::CheckboxGroup: return {"label", "form-check"};
            case FrameworkElement::CheckboxInput: return {"input", "form-check-input"};
            case FrameworkElement::CheckboxLabel: return {"span", "form-check-label"};
            case FrameworkElement::SliderGroup:   return {"div", "mb-3"};
            case FrameworkElement::SliderLabel:   return {"label", "form-label"};
            case FrameworkElement::SliderInput:   return {"input", "form-range"};
            case FrameworkElement::KnobGroup:     return {"div", "mb-3"};
            case FrameworkElement::KnobLabel:     return {"label", "form-label"};
            case FrameworkElement::KnobInput:     return {"input", "form-range"};
        }
        return {"div", {}};
    }
};

class DeciusFramework final : public ViewFramework {
public:
    [[nodiscard]] std::string_view stylesheet_href() const noexcept override {
        return "frameworks/css/decius-css-0.4.1.bundle.min.css";
    }

    [[nodiscard]] std::string_view body_attrs() const noexcept override {
        return " class=\"dcs\" data-dcs-density=\"compact\" data-dcs-style=\"3d\"";
    }

    [[nodiscard]] Color background_color() const noexcept override {
        return Color{0x1F, 0x22, 0x2A, 0xFF};
    }

    [[nodiscard]] ElementRecipe element(FrameworkElement element,
                                        bool primary = false) const noexcept override {
        switch (element) {
            case FrameworkElement::Panel:
                return {"section", "dcs-panel dcs-panel--bordered dcs-panel--raised"};
            case FrameworkElement::Card:
                return {"section", "dcs-panel dcs-panel--bordered"};
            case FrameworkElement::CardTitle:
                return {"h3", "dcs-panel__header"};
            case FrameworkElement::Button:
                return {"button", primary ? "dcs-btn dcs-btn--primary" : "dcs-btn"};
            case FrameworkElement::CheckboxGroup: return {"label", "dcs-check"};
            case FrameworkElement::CheckboxInput: return {"input", "dcs-check__input"};
            case FrameworkElement::CheckboxLabel: return {"span", "dcs-check__label"};
            case FrameworkElement::SliderGroup:   return {"div", "dcs-row"};
            case FrameworkElement::SliderLabel:   return {"span", {}};
            case FrameworkElement::SliderInput:   return {"div", "dcs-slider"};
            case FrameworkElement::KnobGroup:     return {"div", "dcs-knob"};
            case FrameworkElement::KnobLabel:     return {"div", "dcs-knob__label"};
            case FrameworkElement::KnobInput:     return {"div", "dcs-knob__body"};
        }
        return {"div", {}};
    }
};

const ViewFramework& framework_for(ViewTheme theme) {
    static const PlainFramework plain;
    static const BootstrapFramework bootstrap;
    static const DeciusFramework decius;

    switch (theme) {
        case ViewTheme::Bootstrap: return bootstrap;
        case ViewTheme::Decius:    return decius;
        case ViewTheme::Plain:     return plain;
    }
    return bootstrap;
}

std::string theme_link(ViewTheme theme) {
    const auto href = framework_for(theme).stylesheet_href();
    if (href.empty()) return {};
    std::string out = "<link rel=\"stylesheet\" href=\"";
    out += href;
    out += "\">";
    return out;
}

std::string body_attrs(ViewTheme theme) {
    return std::string{framework_for(theme).body_attrs()};
}

std::string default_class(ViewTheme theme,
                          FrameworkElement element,
                          bool primary = false) {
    return std::string{framework_for(theme).element(element, primary).classes};
}

ElementRecipe default_element(ViewTheme theme,
                              FrameworkElement element,
                              bool primary = false) {
    return framework_for(theme).element(element, primary);
}

Color default_background_color(ViewTheme theme) {
    return framework_for(theme).background_color();
}

bool is_void_tag(std::string_view tag) {
    return tag == "input" || tag == "img" || tag == "br" || tag == "hr" ||
           tag == "meta" || tag == "link";
}

void append_node_html(const WidgetNode& node, std::string& out) {
    if (node.kind == WidgetKind::Root) {
        for (const auto& child : node.children) append_node_html(child, out);
        return;
    }
    if (node.kind == WidgetKind::Text) {
        out += escape_html(node.text);
        return;
    }

    out += '<';
    out += node.tag;
    for (const auto& attr : node.attrs) {
        out += ' ';
        out += attr.name;
        if (!attr.value.empty()) {
            out += "=\"";
            out += escape_html(attr.value);
            out += '"';
        }
    }
    if (is_void_tag(node.tag)) {
        out += '>';
        return;
    }

    out += '>';
    out += escape_html(node.text);
    for (const auto& child : node.children) append_node_html(child, out);
    out += "</";
    out += node.tag;
    out += '>';
}

const WidgetNode* find_remote_impl(const WidgetNode& node,
                                   std::string_view id) {
    if (node.remote_id == id) return &node;
    for (const auto& child : node.children) {
        if (auto* found = find_remote_impl(child, id)) return found;
    }
    return nullptr;
}

WidgetNode* find_id_impl(WidgetNode& node, StableId id) {
    if (node.id == id) return &node;
    for (auto& child : node.children) {
        if (auto* found = find_id_impl(child, id)) return found;
    }
    return nullptr;
}

const WidgetNode* find_id_impl(const WidgetNode& node, StableId id) {
    if (node.id == id) return &node;
    for (const auto& child : node.children) {
        if (auto* found = find_id_impl(child, id)) return found;
    }
    return nullptr;
}

WidgetNode* find_widget_impl(WidgetNode& node, std::string_view name) {
    if (node.widget_name == name) return &node;
    for (auto& child : node.children) {
        if (auto* found = find_widget_impl(child, name)) return found;
    }
    return nullptr;
}

}  // namespace

void RemotePatchQueue::clear() {
    patches_.clear();
}

void RemotePatchQueue::push(RemotePatch patch) {
    patches_.push_back(std::move(patch));
}

std::string RemotePatchQueue::to_json() const {
    std::string out{"["};
    for (std::size_t i = 0; i < patches_.size(); ++i) {
        const auto& p = patches_[i];
        if (i != 0) out += ',';
        out += "{\"op\":\"";
        out += patch_op_name(p.op);
        out += "\",\"id\":\"";
        out += escape_json(p.id);
        out += "\"";
        if (!p.parent_id.empty()) {
            out += ",\"parent\":\"";
            out += escape_json(p.parent_id);
            out += "\"";
        }
        if (!p.tag.empty()) {
            out += ",\"tag\":\"";
            out += escape_json(p.tag);
            out += "\"";
        }
        if (!p.name.empty()) {
            out += ",\"name\":\"";
            out += escape_json(p.name);
            out += "\"";
        }
        if (!p.value.empty()) {
            out += ",\"value\":\"";
            out += escape_json(p.value);
            out += "\"";
        }
        out += ",\"index\":";
        out += std::to_string(p.index);
        out += '}';
    }
    out += ']';
    return out;
}

void RemotePatchSink::create_element(const WidgetNode& node,
                                     const WidgetNode* parent,
                                     std::size_t index) {
    if (!queue_) return;
    queue_->push(RemotePatch{
        RemotePatchOp::CreateElement,
        node.remote_id,
        parent ? parent->remote_id : std::string{},
        node.tag,
        {},
        {},
        index,
    });
}

void RemotePatchSink::create_text(const WidgetNode& node,
                                  const WidgetNode* parent,
                                  std::size_t index) {
    if (!queue_) return;
    queue_->push(RemotePatch{
        RemotePatchOp::CreateText,
        node.remote_id,
        parent ? parent->remote_id : std::string{},
        {},
        {},
        {},
        index,
    });
}

void RemotePatchSink::remove(const WidgetNode& node) {
    if (!queue_ || node.remote_id.empty()) return;
    queue_->push(RemotePatch{
        RemotePatchOp::Remove,
        node.remote_id,
    });
}

void RemotePatchSink::set_text(const WidgetNode& node, std::string_view value) {
    if (!queue_) return;
    queue_->push(RemotePatch{
        RemotePatchOp::SetText,
        node.remote_id,
        {},
        {},
        {},
        std::string(value),
    });
}

void RemotePatchSink::set_attribute(const WidgetNode& node,
                                    std::string_view name,
                                    std::string_view value) {
    if (!queue_) return;
    queue_->push(RemotePatch{
        RemotePatchOp::SetAttribute,
        node.remote_id,
        {},
        {},
        std::string(name),
        std::string(value),
    });
}

void RemotePatchSink::remove_attribute(const WidgetNode& node,
                                       std::string_view name) {
    if (!queue_) return;
    queue_->push(RemotePatch{
        RemotePatchOp::RemoveAttribute,
        node.remote_id,
        {},
        {},
        std::string(name),
    });
}

WidgetRef::WidgetRef(View* owner,
                     StableId panel_id,
                     StableId id,
                     std::string_view name)
    : owner_(owner), panel_id_(panel_id), id_(id), name_(name) {}

WidgetRef::operator bool() const {
    return owner_ != nullptr && owner_->resolve_widget_ref(*this) != nullptr;
}

StableId WidgetRef::id() const {
    if (owner_) {
        [[maybe_unused]] auto* resolved = owner_->resolve_widget_ref(*this);
    }
    return id_;
}

const WidgetNode* WidgetRef::node() const {
    return owner_ ? owner_->resolve_widget_ref(*this) : nullptr;
}

std::string_view WidgetRef::name() const {
    if (const auto* n = node()) return n->widget_name;
    return name_;
}

WidgetRef& WidgetRef::named(std::string_view name) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->set_widget_name(*n, name);
            name_ = n->widget_name;
        }
    }
    return *this;
}

WidgetRef& WidgetRef::clear() {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) owner_->clear_children(*n);
    }
    return *this;
}

WidgetRef& WidgetRef::text(std::string_view value) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) owner_->set_text(*n, value);
    }
    return *this;
}

WidgetRef& WidgetRef::attr(std::string_view name, std::string_view value) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->set_attr(*n, name, value);
        }
    }
    return *this;
}

WidgetRef& WidgetRef::remove_attr(std::string_view name) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) owner_->remove_attr(*n, name);
    }
    return *this;
}

WidgetRef& WidgetRef::cls(std::string_view classes) {
    return attr("class", classes);
}

WidgetRef& WidgetRef::on_click(std::function<void()> cb) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->set_click_handler(*n, std::move(cb));
        }
    }
    return *this;
}

WidgetRef& WidgetRef::on_change(std::function<void(std::string_view)> cb) {
    if (owner_) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->set_change_handler(*n, std::move(cb));
        }
    }
    return *this;
}

WidgetRef& WidgetRef::append(const std::function<void(View&)>& build) {
    if (owner_ && owner_->can_mutate_children("append")) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->build_children(*n, build, false);
        }
    }
    return *this;
}

WidgetRef& WidgetRef::replace(const std::function<void(View&)>& build) {
    if (owner_ && owner_->can_mutate_children("replace")) {
        if (auto* n = owner_->resolve_widget_ref(*this)) {
            owner_->build_children(*n, build, true);
        }
    }
    return *this;
}

WidgetRef WidgetRef::find_widget(std::string_view name) const {
    if (!owner_) return {};
    const auto* root = owner_->resolve_widget_ref(*this);
    if (!root) return WidgetRef{owner_, id_, {}, name};
    auto* found = owner_->find_widget_node_under(root->id, name);
    return found ? owner_->ref_for_node(*found, root->id, name)
                 : WidgetRef{owner_, root->id, {}, name};
}

View::Scope::Scope(View* owner, WidgetNode* node) noexcept
    : owner_(owner), node_(node) {}

View::Scope::~Scope() {
    if (owner_) owner_->close_node();
}

View::Scope::Scope(Scope&& other) noexcept
    : owner_(other.owner_), node_(other.node_) {
    other.owner_ = nullptr;
    other.node_ = nullptr;
}

View::Scope& View::Scope::operator=(Scope&& other) noexcept {
    if (this == &other) return *this;
    if (owner_) owner_->close_node();
    owner_ = other.owner_;
    node_ = other.node_;
    other.owner_ = nullptr;
    other.node_ = nullptr;
    return *this;
}

WidgetRef View::Scope::ref() const {
    return (owner_ && node_) ? owner_->ref_for_node(*node_, node_->id) : WidgetRef{};
}

View::Scope& View::Scope::named(std::string_view name) {
    if (owner_ && node_) owner_->set_widget_name(*node_, name);
    return *this;
}

View::Scope& View::Scope::attr(std::string_view name, std::string_view value) {
    if (owner_ && node_) owner_->set_attr(*node_, name, value);
    return *this;
}

View::Scope& View::Scope::cls(std::string_view classes) {
    if (owner_ && node_) owner_->set_attr(*node_, "class", classes);
    return *this;
}

View::Scope& View::Scope::text(std::string_view value) {
    if (owner_ && node_) owner_->set_text(*node_, value);
    return *this;
}

WidgetRef View::Scope::find_widget(std::string_view name) const {
    if (!owner_ || !node_) return {};
    auto* found = owner_->find_widget_node_under(node_->id, name);
    return found ? owner_->ref_for_node(*found, node_->id, name)
                 : WidgetRef{owner_, node_->id, {}, name};
}

StableId current_panel_id(const std::vector<WidgetNode*>& stack) {
    if (stack.empty()) return {};
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if ((*it)->kind == WidgetKind::Card ||
            (*it)->kind == WidgetKind::Container ||
            (*it)->kind == WidgetKind::Root) {
            return (*it)->id;
        }
    }
    return stack.back()->id;
}

View::View(ViewTheme theme) : theme_(theme) {
    root_.id = {0xcbf29ce484222325ull};
    root_.kind = WidgetKind::Root;
    root_.tag = "#root";
    root_.remote_id = "aui-root";
}

void View::clear() {
    root_.children.clear();
    root_.cursor = 0;
    stack_.clear();
    widget_names_.clear();
    click_handlers_.clear();
    change_handlers_.clear();
}

void View::begin(ViewSink* sink) {
    sink_ = sink;
    reconciling_ = true;
    root_.cursor = 0;
    stack_.clear();
    stack_.push_back(&root_);
}

void View::begin(RemotePatchQueue* remote_patches) {
    remote_patch_sink_.reset(remote_patches);
    begin(static_cast<ViewSink*>(&remote_patch_sink_));
}

void View::end() {
    while (stack_.size() > 1) close_node();
    if (!stack_.empty()) {
        auto* root = stack_.back();
        for (std::size_t i = root->cursor; i < root->children.size(); ++i) {
            unregister_tree(root->children[i]);
            emit_remove_tree(sink_, root->children[i]);
        }
        root->children.erase(root->children.begin() +
                             static_cast<std::ptrdiff_t>(root->cursor),
                             root->children.end());
    }
    stack_.clear();
    sink_ = nullptr;
    reconciling_ = false;
    remote_patch_sink_.reset(nullptr);
}

Color View::background_color() const noexcept {
    return default_background_color(theme_);
}

View::Scope View::container(std::string_view classes,
                            std::string_view key,
                            std::source_location here) {
    auto& node = open_node(WidgetKind::Container, "div", classes, key, here, true);
    return Scope{this, &node};
}

View::Scope View::card(std::string_view title,
                       std::string_view classes,
                       std::string_view key,
                       std::source_location here) {
    std::string cls = default_class(theme_, FrameworkElement::Card);
    if (!classes.empty()) {
        if (!cls.empty()) cls += ' ';
        cls += classes;
    }
    const auto recipe = default_element(theme_, FrameworkElement::Card);
    auto& node = open_node(WidgetKind::Card, recipe.tag, cls, key, here, true);
    if (!title.empty()) {
        heading(3, title, default_class(theme_, FrameworkElement::CardTitle),
                "__title", here);
    }
    return Scope{this, &node};
}

WidgetRef View::heading(int level,
                        std::string_view value,
                        std::string_view classes,
                        std::string_view key,
                        std::source_location here) {
    if (level < 1) level = 1;
    if (level > 6) level = 6;
    const std::string tag = "h" + std::to_string(level);
    auto& node = open_node(WidgetKind::Heading, tag, classes, key, here, false);
    set_text(node, value);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::paragraph(std::string_view value,
                          std::string_view classes,
                          std::string_view key,
                          std::source_location here) {
    auto& node = open_node(WidgetKind::Container, "p", classes, key, here, false);
    set_text(node, value);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::text(std::string_view value,
                     std::string_view key,
                     std::source_location here) {
    auto& node = open_node(WidgetKind::Text, "#text", {}, key, here, false);
    set_text(node, value);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::button(std::string_view label,
                       bool primary,
                       std::string_view key,
                       std::source_location here) {
    const auto recipe = default_element(theme_, FrameworkElement::Button, primary);
    auto& node = open_node(WidgetKind::Button, recipe.tag, recipe.classes,
                           key, here, false);
    set_attr(node, "type", "button");
    set_text(node, label);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::checkbox(std::string_view label,
                         bool checked,
                         std::string_view key,
                         std::source_location here) {
    const auto group_recipe = default_element(theme_, FrameworkElement::CheckboxGroup);
    auto& group = open_node(WidgetKind::Checkbox, group_recipe.tag,
                            group_recipe.classes, key, here, true);
    set_attr(group, "data-aui-widget", "checkbox");
    if (checked) set_attr(group, "aria-checked", "true");
    else remove_attr(group, "aria-checked");

    const auto input_recipe = default_element(theme_, FrameworkElement::CheckboxInput);
    auto& input = open_node(WidgetKind::Checkbox, input_recipe.tag,
                            input_recipe.classes,
                            "__input", here, false);
    set_attr(input, "type", "checkbox");
    if (checked) set_attr(input, "checked", "checked");
    else remove_attr(input, "checked");

    const auto label_recipe = default_element(theme_, FrameworkElement::CheckboxLabel);
    auto& span = open_node(WidgetKind::Container, label_recipe.tag,
                           label_recipe.classes,
                           "__label", here, false);
    set_text(span, label);
    close_node();
    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::slider(std::string_view label,
                       double value,
                       double min,
                       double max,
                       std::string_view key,
                       std::source_location here) {
    const auto group_recipe = default_element(theme_, FrameworkElement::SliderGroup);
    auto& group = open_node(WidgetKind::Slider, group_recipe.tag,
                            group_recipe.classes,
                            key, here, true);
    set_attr(group, "data-aui-widget", "slider");
    set_attr(group, "role", "slider");
    set_attr(group, "aria-valuemin", number(min));
    set_attr(group, "aria-valuemax", number(max));
    set_attr(group, "aria-valuenow", number(value));

    const auto label_recipe = default_element(theme_, FrameworkElement::SliderLabel);
    auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                 label_recipe.classes, "__label", here, false);
    set_text(label_node, label);

    const auto input_recipe = default_element(theme_, FrameworkElement::SliderInput);
    auto& input = open_node(WidgetKind::Slider, input_recipe.tag,
                            input_recipe.classes,
                            "__input", here, theme_ == ViewTheme::Decius);
    set_attr(input, "min", number(min));
    set_attr(input, "max", number(max));
    set_attr(input, "value", number(value));
    if (theme_ == ViewTheme::Decius) {
        const double pos = normalized_value(value, min, max);
        set_attr(input, "data-dcs-slider", "");
        set_attr(input, "data-min", number(min));
        set_attr(input, "data-max", number(max));
        set_attr(input, "data-value", number(value));

        auto& track = open_node(WidgetKind::Container, "div",
                                "dcs-slider__track", "__track", here, true);
        (void) track;
        auto& fill = open_node(WidgetKind::Container, "div",
                               "dcs-slider__fill", "__fill", here, false);
        set_attr(fill, "style", "width:" + percent(pos));
        auto& thumb = open_node(WidgetKind::Container, "div",
                                "dcs-slider__thumb", "__thumb", here, false);
        set_attr(thumb, "style", "left:" + percent(pos));
        close_node();
        close_node();
    } else {
        set_attr(input, "type", "range");
    }
    close_node();
    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::knob(std::string_view label,
                     double value,
                     double min,
                     double max,
                     bool bipolar,
                     std::string_view key,
                     std::source_location here) {
    if (max <= min) max = min + 1.0;
    const double clamped = std::clamp(value, min, max);

    const auto group_recipe = default_element(theme_, FrameworkElement::KnobGroup);
    auto& group = open_node(WidgetKind::Knob, group_recipe.tag,
                            group_recipe.classes, key, here,
                            true);
    set_attr(group, "data-aui-widget", "knob");
    set_attr(group, "role", "slider");
    set_attr(group, "aria-valuemin", number(min));
    set_attr(group, "aria-valuemax", number(max));
    set_attr(group, "aria-valuenow", number(clamped));

    if (theme_ != ViewTheme::Decius) {
        const auto label_recipe = default_element(theme_, FrameworkElement::KnobLabel);
        auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                     label_recipe.classes, "__label", here, false);
        set_text(label_node, label);

        const auto input_recipe = default_element(theme_, FrameworkElement::KnobInput);
        auto& input = open_node(WidgetKind::Knob, input_recipe.tag,
                                input_recipe.classes, "__input", here, false);
        set_attr(input, "type", "range");
        set_attr(input, "min", number(min));
        set_attr(input, "max", number(max));
        set_attr(input, "value", number(clamped));
        close_node();
        return ref_for_node(group, current_panel_id(stack_));
    }

    set_attr(group, "data-dcs-knob", "");
    set_attr(group, "data-min", number(min));
    set_attr(group, "data-max", number(max));
    set_attr(group, "data-value", number(clamped));
    set_attr(group, "value", number(clamped));
    if (bipolar) set_attr(group, "data-bipolar", "");
    else remove_attr(group, "data-bipolar");

    const auto [bg_x0, bg_y0] = knob_ring_point(-225.0);
    const auto [bg_x1, bg_y1] = knob_ring_point(45.0);

    auto& svg = open_node(WidgetKind::Container, "svg", "dcs-knob__ring",
                          "__ring", here, true);
    set_attr(svg, "viewBox", "0 0 24 24");

    auto& bg = open_node(WidgetKind::Container, "path", {}, "__ring-bg",
                         here, false);
    set_attr(bg, "d", "M " + number(bg_x0) + " " + number(bg_y0) +
                     " A 10.5 10.5 0 1 1 " + number(bg_x1) + " " +
                     number(bg_y1));
    set_attr(bg, "fill", "none");
    set_attr(bg, "stroke", "rgba(255,255,255,.08)");
    set_attr(bg, "stroke-width", "1.5");
    set_attr(bg, "stroke-linecap", "round");

    auto& arc = open_node(WidgetKind::Container, "path", "dcs-knob__arc",
                          "__arc", here, false);
    const auto path = knob_arc_path(min, max, clamped, bipolar);
    if (!path.empty()) set_attr(arc, "d", path);
    else remove_attr(arc, "d");
    set_attr(arc, "fill", "none");
    set_attr(arc, "stroke", "var(--dcs-accent)");
    set_attr(arc, "stroke-width", "1.75");
    set_attr(arc, "stroke-linecap", "round");
    close_node();

    const auto body_recipe = default_element(theme_, FrameworkElement::KnobInput);
    auto& body = open_node(WidgetKind::Container, body_recipe.tag,
                           body_recipe.classes, "__body", here, true);
    (void) body;

    auto& indicator = open_node(WidgetKind::Container, "div",
                                "dcs-knob__indicator", "__indicator",
                                here, false);
    set_attr(indicator, "style",
             "--angle:" + number(knob_angle(clamped, min, max)) + "deg");

    auto& value_node = open_node(WidgetKind::Container, "div",
                                 "dcs-knob__value", "__value", here, false);
    set_text(value_node, number(clamped));

    const auto label_recipe = default_element(theme_, FrameworkElement::KnobLabel);
    auto& label_node = open_node(WidgetKind::Container, label_recipe.tag,
                                 label_recipe.classes, "__label", here, false);
    set_text(label_node, label);
    close_node();
    close_node();

    return ref_for_node(group, current_panel_id(stack_));
}

WidgetRef View::container_ref(std::string_view classes,
                              std::string_view key,
                              std::source_location here) {
    auto& node = open_node(WidgetKind::Container, "div", classes, key, here, false);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::panel_ref(std::string_view key, std::source_location here) {
    const auto recipe = default_element(theme_, FrameworkElement::Panel);
    auto& node = open_node(WidgetKind::Panel, recipe.tag, recipe.classes,
                           key, here, false);
    return ref_for_node(node, current_panel_id(stack_));
}

WidgetRef View::find_widget(std::string_view name) {
    for (auto it = widget_names_.rbegin(); it != widget_names_.rend(); ++it) {
        if (it->first != name) continue;
        if (auto* node = find_id(it->second)) {
            return ref_for_node(*node, root_.id, name);
        }
    }
    auto* found = find_widget_node_under(root_.id, name);
    return found ? ref_for_node(*found, root_.id, name)
                 : WidgetRef{this, root_.id, {}, name};
}

std::vector<WidgetClickBinding> View::click_bindings() const {
    std::vector<WidgetClickBinding> out;
    out.reserve(click_handlers_.size());
    for (const auto& [id, handler] : click_handlers_) {
        const auto* node = find_id(id);
        if (!node || node->widget_name.empty() || !handler) continue;
        out.push_back({node->widget_name, handler});
    }
    return out;
}

std::vector<WidgetChangeBinding> View::change_bindings() const {
    std::vector<WidgetChangeBinding> out;
    out.reserve(change_handlers_.size());
    for (const auto& [id, handler] : change_handlers_) {
        const auto* node = find_id(id);
        if (!node || node->widget_name.empty() || !handler) continue;
        out.push_back({node->widget_name, handler});
    }
    return out;
}

const WidgetNode* View::find_remote(std::string_view id) const {
    return find_remote_impl(root_, id);
}

std::string View::to_html_fragment() const {
    std::string out;
    for (const auto& child : root_.children) append_node_html(child, out);
    return out;
}

std::string View::to_html_document() const {
    std::string out;
    out += "<!doctype html><html><head><meta charset=\"utf-8\">";
    out += theme_link(theme_);
    out += "<style>body{margin:0}.aui-root{min-height:100vh;padding:24px;box-sizing:border-box}</style>";
    out += "</head><body";
    out += body_attrs(theme_);
    out += "><main id=\"aui-root\" class=\"aui-root\">";
    out += to_html_fragment();
    out += "</main></body></html>";
    return out;
}

WidgetNode& View::open_node(WidgetKind kind,
                            std::string_view tag,
                            std::string_view classes,
                            std::string_view key,
                            std::source_location here,
                            bool push_scope) {
    if (stack_.empty()) begin(static_cast<ViewSink*>(nullptr));
    auto* parent = stack_.back();
    const std::size_t index = parent->cursor++;
    StableId id = make_stable_id(parent->id, kind, key, here);
    if (!key.empty()) {
        for (std::size_t i = 0; i < index && i < parent->children.size(); ++i) {
            const auto& sibling = parent->children[i];
            if (sibling.id == id || sibling.widget_name == key) {
                id = make_duplicate_stable_id(id, index);
                break;
            }
        }
    }

    WidgetNode* node = nullptr;
    bool created = false;
    if (index < parent->children.size() &&
        parent->children[index].id == id &&
        parent->children[index].kind == kind &&
        parent->children[index].tag == tag) {
        node = &parent->children[index];
    } else {
        for (std::size_t i = index; i < parent->children.size(); ++i) {
            unregister_tree(parent->children[i]);
            emit_remove_tree(sink_, parent->children[i]);
        }
        parent->children.erase(parent->children.begin() +
                               static_cast<std::ptrdiff_t>(index),
                               parent->children.end());
        parent->children.push_back({});
        node = &parent->children.back();
        node->id = id;
        node->kind = kind;
        node->remote_id = remote_id(id);
        created = true;
    }

    node->tag = std::string(tag);
    if (!key.empty() && (created || node->widget_name.empty())) {
        set_widget_name(*node, key);
    }
    node->cursor = 0;

    if (created && sink_) {
        if (kind == WidgetKind::Text) {
            sink_->create_text(*node, parent == &root_ ? nullptr : parent, index);
        } else {
            sink_->create_element(*node, parent == &root_ ? nullptr : parent, index);
        }
    }
    if (!classes.empty()) set_attr(*node, "class", classes);
    else remove_attr(*node, "class");

    if (push_scope) {
        stack_.push_back(node);
    } else if (!node->children.empty()) {
        clear_children(*node);
    }
    return *node;
}

void View::close_node() {
    if (stack_.size() <= 1) return;
    auto* node = stack_.back();
    for (std::size_t i = node->cursor; i < node->children.size(); ++i) {
        unregister_tree(node->children[i]);
        emit_remove_tree(sink_, node->children[i]);
    }
    node->children.erase(node->children.begin() +
                         static_cast<std::ptrdiff_t>(node->cursor),
                         node->children.end());
    stack_.pop_back();
}

void View::set_attr(WidgetNode& node,
                    std::string_view name,
                    std::string_view value) {
    auto it = std::find_if(node.attrs.begin(), node.attrs.end(),
        [&](const WidgetAttribute& attr) { return attr.name == name; });
    if (it != node.attrs.end()) {
        if (it->value == value) return;
        it->value = std::string(value);
    } else {
        node.attrs.push_back({std::string(name), std::string(value)});
    }
    if (auto* sink = current_sink()) sink->set_attribute(node, name, value);
}

void View::remove_attr(WidgetNode& node, std::string_view name) {
    auto it = std::find_if(node.attrs.begin(), node.attrs.end(),
        [&](const WidgetAttribute& attr) { return attr.name == name; });
    if (it == node.attrs.end()) return;
    node.attrs.erase(it);
    if (auto* sink = current_sink()) sink->remove_attribute(node, name);
}

void View::set_text(WidgetNode& node, std::string_view value) {
    if (node.text == value) return;
    node.text = std::string(value);
    if (auto* sink = current_sink()) sink->set_text(node, value);
}

void View::set_click_handler(WidgetNode& node, std::function<void()> cb) {
    auto it = std::find_if(click_handlers_.begin(), click_handlers_.end(),
        [&](const auto& entry) { return entry.first == node.id; });
    if (it != click_handlers_.end()) {
        it->second = std::move(cb);
    } else {
        click_handlers_.push_back({node.id, std::move(cb)});
    }
}

void View::set_change_handler(WidgetNode& node,
                              std::function<void(std::string_view)> cb) {
    auto it = std::find_if(change_handlers_.begin(), change_handlers_.end(),
        [&](const auto& entry) { return entry.first == node.id; });
    if (it != change_handlers_.end()) {
        it->second = std::move(cb);
    } else {
        change_handlers_.push_back({node.id, std::move(cb)});
    }
}

void View::clear_children(WidgetNode& node) {
    for (const auto& child : node.children) {
        unregister_tree(child);
        emit_remove_tree(current_sink(), child);
    }
    node.children.clear();
    node.cursor = 0;
}

void View::set_widget_name(WidgetNode& node, std::string_view name) {
    const std::string old_name = node.widget_name;
    if (!old_name.empty()) {
        widget_names_.erase(
            std::remove_if(widget_names_.begin(), widget_names_.end(),
                [&](const auto& entry) {
                    return entry.first == old_name && entry.second == node.id;
                }),
            widget_names_.end());
    }

    node.widget_name = std::string(name);
    if (!node.widget_name.empty()) {
        auto it = std::find_if(widget_names_.begin(), widget_names_.end(),
            [&](const auto& entry) {
                return entry.first == node.widget_name;
            });
        if (it != widget_names_.end()) {
            if (it->second != node.id || find_id(it->second) != &node) {
                diagnostics_.push_back("Duplicate widget id: " + node.widget_name);
            }
            it->second = node.id;
        } else {
            widget_names_.push_back({node.widget_name, node.id});
        }
        set_attr(node, "data-aui-name", node.widget_name);
    } else {
        remove_attr(node, "data-aui-name");
    }
}

void View::unregister_tree(const WidgetNode& node) {
    click_handlers_.erase(
        std::remove_if(click_handlers_.begin(), click_handlers_.end(),
            [&](const auto& entry) {
                return entry.first == node.id;
            }),
        click_handlers_.end());
    change_handlers_.erase(
        std::remove_if(change_handlers_.begin(), change_handlers_.end(),
            [&](const auto& entry) {
                return entry.first == node.id;
            }),
        change_handlers_.end());
    if (!node.widget_name.empty()) {
        widget_names_.erase(
            std::remove_if(widget_names_.begin(), widget_names_.end(),
                [&](const auto& entry) {
                    return entry.first == node.widget_name && entry.second == node.id;
                }),
            widget_names_.end());
    }
    for (const auto& child : node.children) unregister_tree(child);
}

bool View::can_mutate_children(std::string_view operation) {
    if (!reconciling_) return true;
    std::string message{"Illegal WidgetRef::"};
    message += operation;
    message += " during view generation";
    diagnostics_.push_back(std::move(message));
    return false;
}

void View::build_children(WidgetNode& node,
                          const std::function<void(View&)>& build,
                          bool replace) {
    if (!build) return;
    if (replace) clear_children(node);

    const auto old_stack = std::move(stack_);
    const bool old_reconciling = reconciling_;
    ViewSink* old_sink = sink_;

    reconciling_ = false;
    sink_ = nullptr;
    stack_.clear();
    node.cursor = node.children.size();
    stack_.push_back(&root_);
    stack_.push_back(&node);

    build(*this);

    while (stack_.size() > 2) close_node();
    if (stack_.size() == 2) stack_.pop_back();
    if (stack_.size() == 1) stack_.pop_back();

    stack_ = old_stack;
    reconciling_ = old_reconciling;
    sink_ = old_sink;
}

WidgetNode* View::find_id(StableId id) {
    return find_id_impl(root_, id);
}

const WidgetNode* View::find_id(StableId id) const {
    return find_id_impl(root_, id);
}

WidgetRef View::ref_for_node(const WidgetNode& node,
                             StableId panel_id,
                             std::string_view name) {
    const std::string_view ref_name = !name.empty() ? name : node.widget_name;
    if (ref_name.empty()) return {};
    return WidgetRef{this, panel_id, node.id, ref_name};
}

WidgetNode* View::find_widget_node_under(StableId root_id, std::string_view name) {
    auto* root = find_id(root_id);
    if (!root) return nullptr;
    auto* found = find_widget_impl(*root, name);
    return found ? found : nullptr;
}

WidgetNode* View::resolve_widget_ref(const WidgetRef& ref) {
    auto* node = find_id(ref.id_);
    if (node != nullptr) return node;
    if (ref.name_.empty()) return nullptr;

    if (auto* found = find_widget_node_under(ref.panel_id_, ref.name_)) {
        ref.id_ = found->id;
        return found;
    }
    if (auto* found = find_widget_node_under(root_.id, ref.name_)) {
        ref.id_ = found->id;
        return found;
    }
    return nullptr;
}

ViewSink* View::current_sink() const noexcept {
    return reconciling_ ? sink_ : mutation_sink_;
}

}  // namespace affineui
