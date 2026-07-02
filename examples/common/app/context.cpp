#include "app/context.h"

namespace app {

bool Context::run(CommandPtr command) {
    if (!command) return false;
    const bool recorded = stack_.push(std::move(command));
    if (recorded) {
        document_.mark_dirty();
    }
    return recorded;
}

bool Context::run(std::string_view command_id, const Args& args) {
    const CommandInfo* info = registry_.find(command_id);
    if (!info || !info->make) return false;
    // Respect command state: don't run a disabled command.
    if (info->state && !info->state(*this).enabled) return false;
    return run(info->make(*this, args));
}

}  // namespace app
