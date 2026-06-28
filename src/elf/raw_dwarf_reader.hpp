#pragma once

#include "elf_static_view/project_types.hpp"

#include <string>

namespace elf_static_view::elf {

class RawDwarfReader {
public:
    [[nodiscard]] RawDwarfDocument load(const std::string& file_path) const;
};

} // namespace elf_static_view::elf
