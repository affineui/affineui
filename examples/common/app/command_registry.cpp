#include "app/command_registry.h"

#include "app/localization.h"

namespace app {

void CommandRegistry::add(CommandInfo info) {
    auto it = index_.find(info.id);
    if (it != index_.end()) {
        infos_[it->second] = std::move(info);  // replace existing
        return;
    }
    index_.emplace(info.id, infos_.size());
    infos_.push_back(std::move(info));
}

const CommandInfo* CommandRegistry::find(std::string_view id) const {
    auto it = index_.find(std::string(id));
    if (it == index_.end()) return nullptr;
    return &infos_[it->second];
}

std::string_view CommandRegistry::label(std::string_view id) const {
    const CommandInfo* info = find(id);
    if (!info) return id;
    return tr(info->label_key.empty() ? info->id : info->label_key);
}

CommandState CommandRegistry::state(std::string_view id,
                                    const Context& ctx) const {
    const CommandInfo* info = find(id);
    if (info && info->state) return info->state(ctx);
    return {};  // enabled, unchecked, visible
}

}  // namespace app
