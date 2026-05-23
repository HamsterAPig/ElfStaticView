#pragma once

#include "ui/app_state.hpp"

#include <filesystem>

namespace elf_static_view::ui {

void load_version_check_config(AppState& state, const std::filesystem::path& executable_path);
void check_for_new_version(AppState& state);

}  // namespace elf_static_view::ui
