#pragma once

#include "elf_static_view/project_types.hpp"

#include <string>

namespace elf_static_view::elf {

class DwarfReader {
public:
  [[nodiscard]] ProjectModel load(const std::string& file_path,
                                  const LoadPolicy& load_policy = {}) const;
};

}  // namespace elf_static_view::elf
