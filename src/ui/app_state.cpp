#include "ui/app_state.hpp"

#include "analysis/address_bias.hpp"
#include "ui/version_check.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <filesystem>
#include <sstream>
#include <utility>
#include <vector>

namespace elf_static_view::ui {

namespace {

constexpr int kDefaultUiRefreshRate = 30;
constexpr int kMinUiRefreshRate = 1;
constexpr int kMaxUiRefreshRate = 240;
constexpr int kDefaultFilterDebounceMs = 300;
constexpr int kMinFilterDebounceMs = 0;
constexpr int kMaxFilterDebounceMs = 2000;
constexpr auto kOpenedFileCheckInterval = std::chrono::milliseconds(1000);
constexpr auto kOpenedFileRecreateStableDelay = std::chrono::milliseconds(300);

std::string infer_source_file(const AppState& state) {
  if (!state.current_file_path.empty()) {
    return state.current_file_path;
  }
  if (state.snapshot.has_value()) {
    return state.snapshot->source_file;
  }
  return {};
}

std::string current_timestamp_utc() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time_value = std::chrono::system_clock::to_time_t(now);
  std::tm utc_time{};
#if defined(_WIN32)
  gmtime_s(&utc_time, &time_value);
#else
  gmtime_r(&time_value, &utc_time);
#endif
  std::ostringstream stream;
  stream << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
  return stream.str();
}

std::string trim_ascii_copy(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }

  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }

  return std::string(value.substr(begin, end - begin));
}

std::string lowercase_ascii_copy(std::string value) {
  std::transform(value.begin(),
                 value.end(),
                 value.begin(),
                 [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string format_binary(std::uint64_t value) {
  if (value == 0U) {
    return "0";
  }

  std::string digits;
  while (value > 0U) {
    digits.push_back((value & 1U) == 0U ? '0' : '1');
    value >>= 1U;
  }
  std::reverse(digits.begin(), digits.end());
  return digits;
}

std::string abbreviate_directory_for_title(const std::filesystem::path& directory_path) {
  const std::string generic_path = directory_path.lexically_normal().generic_string();
  if (generic_path.empty() || generic_path == ".") {
    return {};
  }

  std::string root_prefix;
  std::size_t cursor = 0;
  if (generic_path.size() >= 2 &&
      std::isalpha(static_cast<unsigned char>(generic_path[0])) != 0 &&
      generic_path[1] == ':') {
    root_prefix = generic_path.substr(0, 2);
    cursor = 2;
    if (cursor < generic_path.size() && generic_path[cursor] == '/') {
      root_prefix += '/';
      ++cursor;
    }
  } else if (!generic_path.empty() && generic_path.front() == '/') {
    root_prefix = "/";
    cursor = 1;
  }

  std::vector<std::string> parts;
  std::string current_part;
  while (cursor <= generic_path.size()) {
    if (cursor == generic_path.size() || generic_path[cursor] == '/') {
      if (!current_part.empty()) {
        parts.push_back(current_part);
        current_part.clear();
      }
      ++cursor;
      continue;
    }
    current_part.push_back(generic_path[cursor]);
    ++cursor;
  }

  if (parts.size() <= 2U) {
    return generic_path;
  }

  std::ostringstream stream;
  if (!root_prefix.empty()) {
    stream << root_prefix;
  }
  stream << ".../" << parts[parts.size() - 2] << '/' << parts.back();
  return stream.str();
}

std::string preferred_title_source_path(const AppState& state) {
  if (!state.current_snapshot_path.empty()) {
    return state.current_snapshot_path;
  }
  return state.current_file_path;
}

}  // namespace

void log_info(AppState& state, const std::string& message) {
  state.log_messages.push_back("[info] " + message);
}

void log_error(AppState& state, const std::string& message) {
  state.error_message = message;
  state.log_messages.push_back("[error] " + message);
}

const char* copy_address_base_label(const CopyAddressBase value) {
  switch (value) {
    case CopyAddressBase::Hex:
      return "十六进制";
    case CopyAddressBase::Dec:
      return "十进制";
    case CopyAddressBase::Oct:
      return "八进制";
    case CopyAddressBase::Bin:
      return "二进制";
  }
  return "十六进制";
}

std::optional<CopyAddressBase> parse_copy_address_base(const std::string_view value) {
  const std::string normalized = lowercase_ascii_copy(trim_ascii_copy(value));
  if (normalized.empty()) {
    return std::nullopt;
  }
  if (normalized == "hex") {
    return CopyAddressBase::Hex;
  }
  if (normalized == "dec") {
    return CopyAddressBase::Dec;
  }
  if (normalized == "oct") {
    return CopyAddressBase::Oct;
  }
  if (normalized == "bin") {
    return CopyAddressBase::Bin;
  }
  return std::nullopt;
}

std::string copy_address_base_to_config_value(const CopyAddressBase value) {
  switch (value) {
    case CopyAddressBase::Hex:
      return "hex";
    case CopyAddressBase::Dec:
      return "dec";
    case CopyAddressBase::Oct:
      return "oct";
    case CopyAddressBase::Bin:
      return "bin";
  }
  return "hex";
}

int sanitize_ui_refresh_rate(const int value) {
  if (value <= 0) {
    return kDefaultUiRefreshRate;
  }
  return std::clamp(value, kMinUiRefreshRate, kMaxUiRefreshRate);
}

int sanitize_filter_debounce_ms(const int value) {
  if (value < kMinFilterDebounceMs || value > kMaxFilterDebounceMs) {
    return kDefaultFilterDebounceMs;
  }
  return value;
}

void clear_selection(AppState& state) {
  state.selected_node = nullptr;
  state.selected_node_path.clear();
}

namespace {

const ExpandedNode* find_node_by_path(const ExpandedNode& node, const std::string& path) {
  if (node.path == path) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (const auto* resolved = find_node_by_path(child, path); resolved != nullptr) {
      return resolved;
    }
  }
  return nullptr;
}

}  // namespace

const ExpandedNode* resolve_selected_node(const AppState& state) {
  if (state.selected_node != nullptr) {
    return state.selected_node;
  }
  if (state.selected_node_path.empty() || !state.project_model) {
    return nullptr;
  }
  for (const auto& node : state.project_model->expanded) {
    if (const auto* resolved = find_node_by_path(node, state.selected_node_path); resolved != nullptr) {
      return resolved;
    }
  }
  return nullptr;
}

std::string format_address_for_copy(const std::uint64_t value, const AppState& state) {
  switch (state.copy_address_base) {
    case CopyAddressBase::Hex: {
      std::ostringstream stream;
      if (!state.copy_hex_without_prefix) {
        stream << "0x";
      }
      stream << std::hex << value;
      return stream.str();
    }
    case CopyAddressBase::Dec:
      return std::to_string(value);
    case CopyAddressBase::Oct: {
      std::ostringstream stream;
      stream << std::oct << value;
      return stream.str();
    }
    case CopyAddressBase::Bin:
      return format_binary(value);
  }
  return {};
}

std::optional<std::string> format_adjusted_address_for_copy(const ExpandedNode& node,
                                                            const AppState& state) {
  // 复制逻辑必须和树/详情面板共用同一套偏移规则，避免展示和剪贴板结果漂移。
  const auto adjusted = elf_static_view::apply_bias_to_absolute(node, state.address_bias);
  if (!adjusted.has_value()) {
    return std::nullopt;
  }
  return format_address_for_copy(adjusted.value(), state);
}

std::string build_window_title(const AppState& state) {
  const std::string default_title = "ElfStaticView " + current_version_string();
  const std::string source_path = preferred_title_source_path(state);
  if (source_path.empty()) {
    return default_title;
  }

  const std::filesystem::path source_file_path(source_path);
  const std::string file_name = source_file_path.filename().generic_string();
  if (file_name.empty()) {
    return default_title;
  }

  const std::string abbreviated_directory = abbreviate_directory_for_title(source_file_path.parent_path());
  if (abbreviated_directory.empty()) {
    return "ElfStaticView - " + file_name;
  }
  if (abbreviated_directory.ends_with('/')) {
    return "ElfStaticView - " + abbreviated_directory + file_name;
  }
  return "ElfStaticView - " + abbreviated_directory + '/' + file_name;
}

void set_loaded_project(AppState& state,
                        ProjectModel model,
                        const LoadedContentKind kind,
                        const std::string& source_path) {
  state.loaded_kind = kind;
  state.current_file_path = source_path;
  state.current_snapshot_path.clear();
  state.project_model = std::move(model);
  state.filters.cache.valid = false;
  state.filters.has_pending_form = true;
  state.filters.last_input_at = std::chrono::steady_clock::now() -
                                std::chrono::milliseconds(state.filter_debounce_ms);
  state.snapshot.reset();
  state.error_message.clear();
  state.window_title_dirty = true;
  state.background_load.status = BackgroundLoadStatus::Loaded;
  state.background_load.error_message.clear();
  state.background_load.path = source_path;
  state.json_preview_dirty = true;
  state.json_preview_error.clear();
  watch_opened_file(state, kind, source_path);
  clear_selection(state);
}

void set_loaded_snapshot(AppState& state, ProjectSnapshot snapshot, const std::string& snapshot_path) {
  state.loaded_kind = LoadedContentKind::Snapshot;
  state.current_snapshot_path = snapshot_path;
  state.current_file_path = snapshot.source_file;
  state.project_model = std::move(snapshot.model);
  state.filters.cache.valid = false;
  state.filters.has_pending_form = true;
  state.filters.last_input_at = std::chrono::steady_clock::now() -
                                std::chrono::milliseconds(state.filter_debounce_ms);
  state.snapshot = std::move(snapshot);
  state.error_message.clear();
  state.window_title_dirty = true;
  state.json_preview_dirty = true;
  state.json_preview_error.clear();
  watch_opened_file(state, LoadedContentKind::Snapshot, snapshot_path);
  clear_selection(state);
}

void begin_background_load(AppState& state, const std::uint64_t task_id, const std::string& path) {
  state.background_load.status = BackgroundLoadStatus::Loading;
  state.background_load.task_id = task_id;
  state.background_load.path = path;
  state.background_load.error_message.clear();
  state.background_load.started_at = std::chrono::steady_clock::now();
}

void finish_background_load(AppState& state, const std::uint64_t task_id, ProjectModel model) {
  if (state.background_load.task_id != task_id) {
    return;
  }
  set_loaded_project(state, std::move(model), LoadedContentKind::ElfProject, state.background_load.path);
  state.background_load.status = BackgroundLoadStatus::Loaded;
  state.background_load.future = {};
}

void fail_background_load(AppState& state, const std::uint64_t task_id, const std::string& message) {
  if (state.background_load.task_id != task_id) {
    return;
  }
  state.background_load.status = BackgroundLoadStatus::Failed;
  state.background_load.error_message = message;
  state.background_load.future = {};
  log_error(state, message);
}

void watch_opened_file(AppState& state, const LoadedContentKind kind, const std::string& path) {
  if (path.empty() || kind == LoadedContentKind::None) {
    state.opened_file_monitor = {};
    return;
  }

  // 成功打开后才建立监听；快照监听 JSON 本身，不监听快照内记录的 source_file。
  state.opened_file_monitor = OpenedFileMonitorState {
    .path = path,
    .content_kind = kind,
    .has_seen_existing = true,
  };
}

bool has_opened_file_monitor(const AppState& state) {
  return !state.opened_file_monitor.path.empty() &&
         state.opened_file_monitor.content_kind != LoadedContentKind::None;
}

bool opened_file_monitor_check_due(AppState& state, const std::chrono::steady_clock::time_point now) {
  if (!has_opened_file_monitor(state)) {
    return false;
  }

  auto& monitor = state.opened_file_monitor;
  if (monitor.last_checked_at != std::chrono::steady_clock::time_point {} &&
      now - monitor.last_checked_at < kOpenedFileCheckInterval) {
    return false;
  }

  monitor.last_checked_at = now;
  return true;
}

std::optional<std::string> observe_opened_file_presence(AppState& state,
                                                        const bool exists,
                                                        const std::chrono::steady_clock::time_point now,
                                                        const bool reload_busy) {
  if (!has_opened_file_monitor(state)) {
    return std::nullopt;
  }

  auto& monitor = state.opened_file_monitor;
  if (!exists) {
    // 删除期间只记录缺失状态，绝不清空旧模型或旧快照，保证界面继续展示最后一次解析结果。
    if (monitor.has_seen_existing) {
      monitor.missing = true;
    }
    monitor.reload_pending = false;
    monitor.recreated_at = {};
    return std::nullopt;
  }

  if (!monitor.has_seen_existing) {
    monitor.has_seen_existing = true;
    monitor.missing = false;
    return std::nullopt;
  }

  if (!monitor.missing) {
    return std::nullopt;
  }

  if (!monitor.reload_pending) {
    monitor.reload_pending = true;
    monitor.recreated_at = now;
    return std::nullopt;
  }

  if (now - monitor.recreated_at < kOpenedFileRecreateStableDelay || reload_busy) {
    return std::nullopt;
  }

  const std::string reload_path = monitor.path;
  monitor.missing = false;
  monitor.reload_pending = false;
  monitor.recreated_at = {};
  return reload_path;
}

void begin_ui_task(UiTaskState& task, const std::uint64_t task_id, const std::string& detail) {
  task.status = UiTaskStatus::Running;
  task.task_id = task_id;
  task.detail = detail;
  task.message.clear();
  task.started_at = std::chrono::steady_clock::now();
}

bool finish_ui_task(UiTaskState& task, const std::uint64_t task_id, const std::string& message) {
  if (task.task_id != task_id) {
    return false;
  }
  task.status = UiTaskStatus::Succeeded;
  task.message = message;
  return true;
}

bool fail_ui_task(UiTaskState& task, const std::uint64_t task_id, const std::string& message) {
  if (task.task_id != task_id) {
    return false;
  }
  task.status = UiTaskStatus::Failed;
  task.message = message;
  return true;
}

std::optional<ProjectSnapshot> build_snapshot(const AppState& state) {
  if (!state.project_model) {
    return std::nullopt;
  }
  ProjectSnapshot snapshot;
  snapshot.source_file = infer_source_file(state);
  snapshot.model = *state.project_model;
  if (state.snapshot.has_value()) {
    snapshot.schema_version = state.snapshot->schema_version;
    snapshot.source_kind = state.snapshot->source_kind;
    snapshot.exported_at = state.snapshot->exported_at;
  }
  if (snapshot.exported_at.empty()) {
    snapshot.exported_at = current_timestamp_utc();
  }
  return snapshot;
}

std::optional<std::string> selected_node_json(const AppState& state) {
  const auto* selected_node = resolve_selected_node(state);
  if (selected_node == nullptr) {
    return std::nullopt;
  }
  return render_expanded_node_json(*selected_node);
}

}  // namespace elf_static_view::ui
