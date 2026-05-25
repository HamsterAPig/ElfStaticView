#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <utility>
#include <string>
#include <cstdint>
#include <unordered_map>

namespace elf_static_view::ui {

class UiTaskRunner {
public:
  using TaskFn = std::function<std::pair<bool, std::string>()>;

  struct CompletedTask {
    std::uint64_t id = 0;
    bool success = false;
    std::string success_value;
    std::string error_message;
  };

  struct TaskState {
    std::uint64_t id = 0;
    bool running = false;
    std::string detail;
    std::chrono::steady_clock::time_point started_at {};
  };

  [[nodiscard]] std::uint64_t submit(const std::string& key, const std::string& detail, TaskFn task_fn);
  [[nodiscard]] std::optional<CompletedTask> poll(const std::string& key);

private:
  struct TaskSlot {
    std::uint64_t id = 0;
    std::string detail;
    std::chrono::steady_clock::time_point started_at {};
    std::future<std::pair<bool, std::string>> future;
  };

  std::uint64_t next_task_id_ = 1;
  std::unordered_map<std::string, TaskSlot> tasks_;
};

}  // namespace elf_static_view::ui
