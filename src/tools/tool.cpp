#include "tools/tool.hpp"

#include "tools/tool_use_context.hpp"

#include <mutex>

namespace cc {

Tool* find_tool_by_name(const Tools& tools, std::string_view name) {
    for (auto& tool : tools) {
        if (tool && tool->name() == name) {
            return tool.get();
        }
    }
    return nullptr;
}

void AbortSignal::on_abort(std::function<void()> cb) {
    if (!cb) {
        return;
    }
    {
        std::lock_guard lock(*mutex_);
        if (flag_->load(std::memory_order_acquire)) {
            // Already aborted: run the callback straight away.
        } else {
            listeners_->push_back(std::move(cb));
            return;
        }
    }
    cb();
}

} // namespace cc
