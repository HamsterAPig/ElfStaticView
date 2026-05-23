#include "logging/logger.hpp"

#include <iostream>

namespace elf_static_view::logging {

void log(Level level, std::string_view message) {
  const char* prefix = "[info]";
  if (level == Level::Warning) {
    prefix = "[warn]";
  } else if (level == Level::Error) {
    prefix = "[error]";
  }

  std::cerr << prefix << ' ' << message << '\n';
}

}  // namespace elf_static_view::logging
