#pragma once

#include "elf_static_view/project.hpp"

#include <optional>
#include <string>
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

struct AppState {
  LoadedContentKind loaded_kind = LoadedContentKind::None;
  std::string current_file_path;
  std::string current_snapshot_path;
  std::optional<ProjectModel> project_model;
  std::optional<ProjectSnapshot> snapshot;
  const ExpandedNode* selected_node = nullptr;
  FilterState filters;
  std::int64_t address_bias = 0;
  bool show_log_panel = true;
  bool show_json_preview_panel = true;
  bool show_about_dialog = false;
  bool request_exit = false;
  std::vector<std::string> log_messages;
  std::string error_message;
};

void log_info(AppState& state, const std::string& message);
void log_error(AppState& state, const std::string& message);
void clear_selection(AppState& state);
void set_loaded_project(AppState& state,
                        ProjectModel model,
                        LoadedContentKind kind,
                        const std::string& source_path);
void set_loaded_snapshot(AppState& state, ProjectSnapshot snapshot, const std::string& snapshot_path);
std::optional<ProjectSnapshot> build_snapshot(const AppState& state);
std::optional<std::string> selected_node_json(const AppState& state);

}  // namespace elf_static_view::ui
