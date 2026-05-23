#include "ui/app_state.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace elf_static_view::ui {

namespace {

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

}  // namespace

void log_info(AppState& state, const std::string& message) {
  state.log_messages.push_back("[info] " + message);
}

void log_error(AppState& state, const std::string& message) {
  state.error_message = message;
  state.log_messages.push_back("[error] " + message);
}

void clear_selection(AppState& state) {
  state.selected_node = nullptr;
}

void set_loaded_project(AppState& state,
                        ProjectModel model,
                        const LoadedContentKind kind,
                        const std::string& source_path) {
  state.loaded_kind = kind;
  state.current_file_path = source_path;
  state.current_snapshot_path.clear();
  state.project_model = std::move(model);
  state.snapshot.reset();
  state.error_message.clear();
  clear_selection(state);
}

void set_loaded_snapshot(AppState& state, ProjectSnapshot snapshot, const std::string& snapshot_path) {
  state.loaded_kind = LoadedContentKind::Snapshot;
  state.current_snapshot_path = snapshot_path;
  state.current_file_path = snapshot.source_file;
  state.project_model = snapshot.model;
  state.snapshot = std::move(snapshot);
  state.error_message.clear();
  clear_selection(state);
}

std::optional<ProjectSnapshot> build_snapshot(const AppState& state) {
  if (!state.project_model.has_value()) {
    return std::nullopt;
  }
  ProjectSnapshot snapshot;
  snapshot.source_file = infer_source_file(state);
  snapshot.model = state.project_model.value();
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
  if (state.selected_node == nullptr) {
    return std::nullopt;
  }
  return render_expanded_node_json(*state.selected_node);
}

}  // namespace elf_static_view::ui
