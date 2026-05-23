#pragma once

#include <optional>
#include <string>

namespace elf_static_view::ui {

std::optional<std::string> open_elf_file_dialog();
std::optional<std::string> open_snapshot_file_dialog();
std::optional<std::string> save_snapshot_file_dialog(const std::string& suggested_name);

}  // namespace elf_static_view::ui
