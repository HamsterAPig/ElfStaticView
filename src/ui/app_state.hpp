#pragma once

#include "elf_static_view/project.hpp"

#include <chrono>
#include <filesystem>
#include <cstdint>
#include <optional>
#include <future>
#include <string>
#include <string_view>
#include <vector>

namespace elf_static_view::ui {

enum class LoadedContentKind {
  None,
  ElfProject,
  Snapshot,
};

struct FilterRuleSet {
  std::string variable_name_query;
  std::string path_rules_text;
  bool include_runtime_only = false;
  bool only_static_known = false;
};

struct FilterRule {
  std::string raw_pattern;
  std::string normalized_pattern;
  bool exclude = false;
};

struct FilterState {
  FilterRuleSet form;
  std::vector<FilterRule> rules;
  std::optional<std::string> compile_error;
};

struct VersionCheckState {
  std::string repository_url;
  std::string check_uri;
  std::string latest_version;
  std::string release_url;
  std::string release_name;
  std::string release_notes;
  std::string message;
  bool has_new_version = false;
  bool check_uri_uses_default = false;
};

enum class BackgroundLoadStatus {
  Idle,
  Loading,
  Loaded,
  Failed,
};

struct BackgroundLoadState {
  BackgroundLoadStatus status = BackgroundLoadStatus::Idle;
  std::uint64_t task_id = 0;
  std::string path;
  std::string error_message;
  std::chrono::steady_clock::time_point started_at {};
  std::future<ProjectModel> future;
};

enum class UiTaskStatus {
  Idle,
  Running,
  Succeeded,
  Failed,
};

struct UiTaskState {
  UiTaskStatus status = UiTaskStatus::Idle;
  std::uint64_t task_id = 0;
  std::string detail;
  std::string message;
  std::chrono::steady_clock::time_point started_at {};
};

enum class CopyAddressBase {
  Hex,
  Dec,
  Oct,
  Bin,
};

struct AppState {
  LoadedContentKind loaded_kind = LoadedContentKind::None;
  std::string current_file_path;
  std::string current_snapshot_path;
  std::optional<ProjectModel> project_model;
  std::optional<ProjectSnapshot> snapshot;
  const ExpandedNode* selected_node = nullptr;
  std::string selected_node_path;
  FilterState filters;
  std::int64_t address_bias = 0;
  std::string address_bias_input = "0";
  std::optional<std::string> address_bias_error;
  std::filesystem::path config_path;
  CopyAddressBase copy_address_base = CopyAddressBase::Hex;
  bool copy_hex_without_prefix = false;
  int ui_refresh_rate = 30;
  bool persist_address_bias_to_config = false;
  bool window_title_dirty = false;
  bool show_log_panel = true;
  bool show_json_preview_panel = true;
  bool export_sensitive_info = false;
  bool show_about_dialog = false;
  bool show_shortcuts_dialog = false;
  bool request_exit = false;
  bool focus_variable_search = false;
  bool enable_background_loading = true;
  std::optional<std::string> pending_open_elf_path;
  std::optional<std::string> pending_import_snapshot_path;
  std::optional<std::string> pending_export_snapshot_path;
  std::optional<std::string> pending_export_raw_dwarf_source_path;
  std::optional<std::string> pending_export_raw_dwarf_output_path;
  bool pending_version_check = false;
  LoadPolicy load_policy;
  BackgroundLoadState background_load;
  UiTaskState import_snapshot_task;
  UiTaskState export_snapshot_task;
  UiTaskState export_raw_dwarf_task;
  UiTaskState json_preview_task;
  UiTaskState version_check_task;
  std::string json_preview_text;
  std::string json_preview_cache_key;
  std::string json_preview_error;
  bool json_preview_dirty = true;
  std::optional<VersionCheckState> version_check;
  std::vector<std::string> log_messages;
  std::string error_message;
};

void log_info(AppState& state, const std::string& message);
void log_error(AppState& state, const std::string& message);
[[nodiscard]] const char* copy_address_base_label(CopyAddressBase value);
[[nodiscard]] std::optional<CopyAddressBase> parse_copy_address_base(std::string_view value);
[[nodiscard]] std::string copy_address_base_to_config_value(CopyAddressBase value);
[[nodiscard]] int sanitize_ui_refresh_rate(int value);
void clear_selection(AppState& state);
[[nodiscard]] const ExpandedNode* resolve_selected_node(const AppState& state);
[[nodiscard]] std::string format_address_for_copy(std::uint64_t value, const AppState& state);
[[nodiscard]] std::string build_window_title(const AppState& state);
void set_loaded_project(AppState& state,
                        ProjectModel model,
                        LoadedContentKind kind,
                        const std::string& source_path);
void set_loaded_snapshot(AppState& state, ProjectSnapshot snapshot, const std::string& snapshot_path);
void begin_background_load(AppState& state, std::uint64_t task_id, const std::string& path);
void finish_background_load(AppState& state, std::uint64_t task_id, ProjectModel model);
void fail_background_load(AppState& state, std::uint64_t task_id, const std::string& message);
void begin_ui_task(UiTaskState& task, std::uint64_t task_id, const std::string& detail);
bool finish_ui_task(UiTaskState& task, std::uint64_t task_id, const std::string& message);
bool fail_ui_task(UiTaskState& task, std::uint64_t task_id, const std::string& message);
std::optional<ProjectSnapshot> build_snapshot(const AppState& state);
std::optional<std::string> selected_node_json(const AppState& state);

}  // namespace elf_static_view::ui
