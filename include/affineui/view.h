#pragma once

// Remote-ready command tree for AffineUI applications.
//
// This is intentionally not a second renderer. The tree is an identity and
// reconciliation spine for app-level widget commands. Local native mode stores
// weak handles to real AffineUI DOM nodes; browser mode stores stable remote ids
// and emits DOM patch operations. The public command API can feel immediate,
// while the backing output stays retained and efficient.

#include <cstdint>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "affineui/types.h"

namespace affineui {

struct StableId {
    std::uint64_t value{0};

    friend bool operator==(StableId a, StableId b) noexcept {
        return a.value == b.value;
    }
    friend bool operator!=(StableId a, StableId b) noexcept {
        return !(a == b);
    }
};

enum class ViewTheme {
    Plain,
    Bootstrap,
    Decius,
};

enum class WidgetKind {
    Root,
    Container,
    Text,
    Heading,
    Panel,
    Button,
    Checkbox,
    Slider,
    Card,
};

struct WidgetAttribute {
    std::string name;
    std::string value;
};

struct WidgetNode {
    StableId id{};
    WidgetKind kind{WidgetKind::Container};
    std::string tag;
    std::string widget_name;
    std::string remote_id;
    DomHandle local_dom{};

    std::string text;
    std::vector<WidgetAttribute> attrs;
    std::vector<WidgetNode> children;

    // Reconcile cursor, reset at the start of each visit. Public so debug
    // inspectors can show the machinery plainly; user code should treat it as
    // internal state.
    std::size_t cursor{0};
};

enum class RemotePatchOp {
    CreateElement,
    CreateText,
    Remove,
    SetText,
    SetAttribute,
    RemoveAttribute,
};

struct RemotePatch {
    RemotePatchOp op{RemotePatchOp::CreateElement};
    std::string id;
    std::string parent_id;
    std::string tag;
    std::string name;
    std::string value;
    std::size_t index{0};
};

class RemotePatchQueue {
public:
    void clear();
    void push(RemotePatch patch);

    [[nodiscard]] bool empty() const noexcept { return patches_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return patches_.size(); }
    [[nodiscard]] const std::vector<RemotePatch>& patches() const noexcept {
        return patches_;
    }

    [[nodiscard]] std::string to_json() const;

private:
    std::vector<RemotePatch> patches_;
};

class ViewSink {
public:
    virtual ~ViewSink() = default;

    virtual void create_element(const WidgetNode& node,
                                const WidgetNode* parent,
                                std::size_t index) = 0;
    virtual void create_text(const WidgetNode& node,
                             const WidgetNode* parent,
                             std::size_t index) = 0;
    virtual void remove(const WidgetNode& node) = 0;
    virtual void set_text(const WidgetNode& node, std::string_view value) = 0;
    virtual void set_attribute(const WidgetNode& node,
                               std::string_view name,
                               std::string_view value) = 0;
    virtual void remove_attribute(const WidgetNode& node,
                                  std::string_view name) = 0;
};

class RemotePatchSink final : public ViewSink {
public:
    explicit RemotePatchSink(RemotePatchQueue* queue = nullptr) noexcept
        : queue_(queue) {}

    void reset(RemotePatchQueue* queue) noexcept { queue_ = queue; }

    void create_element(const WidgetNode& node,
                        const WidgetNode* parent,
                        std::size_t index) override;
    void create_text(const WidgetNode& node,
                     const WidgetNode* parent,
                     std::size_t index) override;
    void remove(const WidgetNode& node) override;
    void set_text(const WidgetNode& node, std::string_view value) override;
    void set_attribute(const WidgetNode& node,
                       std::string_view name,
                       std::string_view value) override;
    void remove_attribute(const WidgetNode& node,
                          std::string_view name) override;

private:
    RemotePatchQueue* queue_{nullptr};
};

class View;

class WidgetRef {
public:
    WidgetRef() noexcept = default;
    WidgetRef(View* owner,
              StableId panel_id,
              StableId id,
              std::string_view name = {});
    WidgetRef(const WidgetRef&) = default;
    WidgetRef& operator=(const WidgetRef&) = default;
    WidgetRef(WidgetRef&&) noexcept = default;
    WidgetRef& operator=(WidgetRef&&) noexcept = default;
    ~WidgetRef() = default;

    [[nodiscard]] explicit operator bool() const;
    [[nodiscard]] StableId id() const;
    [[nodiscard]] const WidgetNode* node() const;
    [[nodiscard]] std::string_view name() const;

    WidgetRef& named(std::string_view name);
    WidgetRef& clear();
    WidgetRef& text(std::string_view value);
    WidgetRef& attr(std::string_view name, std::string_view value);
    WidgetRef& remove_attr(std::string_view name);
    WidgetRef& cls(std::string_view classes);
    WidgetRef& append(const std::function<void(View&)>& build);
    WidgetRef& replace(const std::function<void(View&)>& build);

    [[nodiscard]] WidgetRef find_widget(std::string_view name) const;

private:
    View* owner_{nullptr};
    StableId panel_id_{};
    mutable StableId id_{};
    std::string name_;

    friend class View;
};

class View {
public:
    class Scope {
    public:
        Scope() noexcept = default;
        Scope(View* owner, WidgetNode* node) noexcept;
        ~Scope();

        Scope(Scope&& other) noexcept;
        Scope& operator=(Scope&& other) noexcept;
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        explicit operator bool() const noexcept { return owner_ != nullptr; }

        WidgetRef ref() const;
        Scope& named(std::string_view name);
        Scope& attr(std::string_view name, std::string_view value);
        Scope& cls(std::string_view classes);
        Scope& text(std::string_view value);
        [[nodiscard]] WidgetRef find_widget(std::string_view name) const;

    private:
        View* owner_{nullptr};
        WidgetNode* node_{nullptr};
    };

    explicit View(ViewTheme theme = ViewTheme::Bootstrap);

    void clear();
    void set_mutation_sink(ViewSink* sink) noexcept { mutation_sink_ = sink; }
    void begin(ViewSink* sink = nullptr);
    void begin(RemotePatchQueue* remote_patches);
    void end();
    [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }
    void clear_diagnostics() { diagnostics_.clear(); }

    [[nodiscard]] ViewTheme theme() const noexcept { return theme_; }
    void set_theme(ViewTheme theme) noexcept { theme_ = theme; }
    [[nodiscard]] Color background_color() const noexcept;

    [[nodiscard]] const WidgetNode& root() const noexcept { return root_; }

    Scope container(std::string_view classes = {},
                    std::string_view key = {},
                    std::source_location here = std::source_location::current());
    Scope card(std::string_view title,
               std::string_view classes = {},
               std::string_view key = {},
               std::source_location here = std::source_location::current());

    WidgetRef heading(int level,
                      std::string_view text,
                      std::string_view classes = {},
                      std::string_view key = {},
                      std::source_location here = std::source_location::current());
    WidgetRef paragraph(std::string_view text,
                        std::string_view classes = {},
                        std::string_view key = {},
                        std::source_location here = std::source_location::current());
    WidgetRef text(std::string_view text,
                   std::string_view key = {},
                   std::source_location here = std::source_location::current());
    WidgetRef button(std::string_view label,
                     bool primary = false,
                     std::string_view key = {},
                     std::source_location here = std::source_location::current());
    WidgetRef checkbox(std::string_view label,
                       bool checked,
                       std::string_view key = {},
                       std::source_location here = std::source_location::current());
    WidgetRef slider(std::string_view label,
                     double value,
                     double min = 0.0,
                     double max = 1.0,
                     std::string_view key = {},
                     std::source_location here = std::source_location::current());
    WidgetRef container_ref(std::string_view classes = {},
                            std::string_view key = {},
                            std::source_location here = std::source_location::current());
    WidgetRef panel_ref(std::string_view key = {},
                        std::source_location here = std::source_location::current());

    [[nodiscard]] WidgetRef find_widget(std::string_view name);
    [[nodiscard]] const WidgetNode* find_remote(std::string_view remote_id) const;
    [[nodiscard]] std::string to_html_fragment() const;
    [[nodiscard]] std::string to_html_document() const;

private:
    friend class WidgetRef;

    WidgetNode& open_node(WidgetKind kind,
                          std::string_view tag,
                          std::string_view classes,
                          std::string_view key,
                          std::source_location here,
                          bool push_scope);
    void close_node();
    void set_attr(WidgetNode& node, std::string_view name, std::string_view value);
    void remove_attr(WidgetNode& node, std::string_view name);
    void set_text(WidgetNode& node, std::string_view value);
    void clear_children(WidgetNode& node);
    void set_widget_name(WidgetNode& node, std::string_view name);
    void unregister_tree(const WidgetNode& node);
    bool can_mutate_children(std::string_view operation);
    void build_children(WidgetNode& node,
                        const std::function<void(View&)>& build,
                        bool replace);
    [[nodiscard]] WidgetNode* find_id(StableId id);
    [[nodiscard]] const WidgetNode* find_id(StableId id) const;
    [[nodiscard]] WidgetRef ref_for_node(const WidgetNode& node,
                                         StableId panel_id,
                                         std::string_view name = {});
    [[nodiscard]] WidgetNode* find_widget_node_under(StableId root_id,
                                                     std::string_view name);
    [[nodiscard]] WidgetNode* resolve_widget_ref(const WidgetRef& ref);
    [[nodiscard]] ViewSink* current_sink() const noexcept;

    ViewTheme theme_{ViewTheme::Bootstrap};
    WidgetNode root_{};
    std::vector<WidgetNode*> stack_;
    std::vector<std::pair<std::string, StableId>> widget_names_;
    std::vector<std::string> diagnostics_;
    ViewSink* sink_{nullptr};
    ViewSink* mutation_sink_{nullptr};
    bool reconciling_{false};
    RemotePatchSink remote_patch_sink_{};
};

}  // namespace affineui
