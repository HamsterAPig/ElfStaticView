#pragma once

#include "elf_static_view/project_types.hpp"

#include <optional>
#include <string>

namespace elf_static_view::analysis {

[[nodiscard]] std::string escape_json(const std::string& value);
[[nodiscard]] std::string join_scope(const std::vector<std::string>& scope_path, const std::string& name);
[[nodiscard]] bool should_emit_symbol(const VariableRecord& record,
                                      bool include_runtime_only,
                                      bool only_static_known,
                                      const std::optional<std::string>& symbol_name);

} // namespace elf_static_view::analysis
