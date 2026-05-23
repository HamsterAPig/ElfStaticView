#pragma once

#include <string_view>

namespace elf_static_view::logging {

enum class Level {
  Info,
  Warning,
  Error,
};

void log(Level level, std::string_view message);

}  // namespace elf_static_view::logging
