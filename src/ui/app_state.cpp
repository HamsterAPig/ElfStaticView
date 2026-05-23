#include "ui/app_state.hpp"

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

void clear_selection(AppState& state) {
  state.selected_node = nullptr;
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
  state.snapshot.reset();
  state.error_message.clear();
  state.window_title_dirty = true;
  clear_selection(state);
}

void set_loaded_snapshot(AppState& state, ProjectSnapshot snapshot, const std::string& snapshot_path) {
  state.loaded_kind = LoadedContentKind::Snapshot;
  state.current_snapshot_path = snapshot_path;
  state.current_file_path = snapshot.source_file;
  state.project_model = snapshot.model;
  state.snapshot = std::move(snapshot);
  state.error_message.clear();
  state.window_title_dirty = true;
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
