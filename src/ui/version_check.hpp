#pragma once

#include "ui/app_state.hpp"

#include <filesystem>

namespace elf_static_view::ui {

[[nodiscard]] std::string current_version_string();
void load_app_config(AppState& state, const std::filesystem::path& executable_path);
void save_app_config(const AppState& state);
void check_for_new_version(AppState& state);

}  // namespace elf_static_view::ui
