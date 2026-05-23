#pragma once

#include "ui/app_state.hpp"

#include <filesystem>
#include <string>

namespace elf_static_view::ui {

struct ReleaseMetadata {
  std::string repository_url;
  std::string releases_api_url;
  std::string author_name;
  std::string author_email;
};

[[nodiscard]] const ReleaseMetadata& default_release_metadata();
[[nodiscard]] int compare_version_strings(const std::string& left, const std::string& right);
[[nodiscard]] std::string current_version_string();
[[nodiscard]] VersionCheckState resolve_version_check_state(const AppState& state);
[[nodiscard]] VersionCheckState parse_version_response_text(const std::string& response_text,
                                                            const std::string& check_uri,
                                                            const std::string& repository_url);
void load_app_config(AppState& state, const std::filesystem::path& executable_path);
void save_app_config(const AppState& state);
void check_for_new_version(AppState& state);

}  // namespace elf_static_view::ui
