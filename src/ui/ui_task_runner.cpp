#include "ui/ui_task_runner.hpp"

#include <exception>
#include <stdexcept>

namespace elf_static_view::ui {

namespace {

    std::string describe_unknown_exception()
    {
        try {
            throw;
        } catch (const std::exception& error) {
            return error.what();
        } catch (...) {
            return "发生未知异常";
        }
    }

} // namespace

std::uint64_t UiTaskRunner::submit(const std::string& key, const std::string& detail, TaskFn task_fn)
{
    const std::uint64_t task_id = next_task_id_++;
    TaskSlot slot;
    slot.id = task_id;
    slot.detail = detail;
    slot.started_at = std::chrono::steady_clock::now();
    slot.future = std::async(std::launch::async, [task_fn = std::move(task_fn)]() mutable {
        try {
            return task_fn();
        } catch (...) {
            return std::pair<bool, std::string>{false, describe_unknown_exception()};
        }
    });
    tasks_[key] = std::move(slot);
    return task_id;
}

std::optional<UiTaskRunner::CompletedTask> UiTaskRunner::poll(const std::string& key)
{
    auto iter = tasks_.find(key);
    if (iter == tasks_.end()) {
        return std::nullopt;
    }
    TaskSlot& slot = iter->second;
    if (!slot.future.valid()) {
        return std::nullopt;
    }
    if (slot.future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return std::nullopt;
    }

    CompletedTask result;
    result.id = slot.id;
    try {
        auto payload = slot.future.get();
        if (payload.first) {
            result.success = true;
            result.success_value = std::move(payload.second);
        } else {
            result.success = false;
            result.error_message = std::move(payload.second);
        }
    } catch (...) {
        result.success = false;
        result.error_message = describe_unknown_exception();
    }

    tasks_.erase(iter);
    return result;
}

} // namespace elf_static_view::ui
