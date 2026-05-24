#pragma once

#include "elf_static_view/project.hpp"

#include <filesystem>
#include <cstdint>
#include <optional>
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
[[nodiscard]] std::string format_address_for_copy(std::uint64_t value, const AppState& state);
[[nodiscard]] std::string build_window_title(const AppState& state);
void set_loaded_project(AppState& state,
                        ProjectModel model,
                        LoadedContentKind kind,
                        const std::string& source_path);
void set_loaded_snapshot(AppState& state, ProjectSnapshot snapshot, const std::string& snapshot_path);
std::optional<ProjectSnapshot> build_snapshot(const AppState& state);
std::optional<std::string> selected_node_json(const AppState& state);

}  // namespace elf_static_view::ui
