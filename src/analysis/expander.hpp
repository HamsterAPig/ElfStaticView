#pragma once

#include "elf_static_view/project_types.hpp"

#include <unordered_map>

namespace elf_static_view::analysis {

class Expander {
public:
  Expander(const std::vector<TypeNode>& types, std::size_t depth_limit);

  [[nodiscard]] std::vector<ExpandedNode> build(
    const std::vector<VariableRecord>& variables,
    bool include_runtime_only,
    bool only_static_known,
    const std::optional<std::string>& symbol_name) const;

private:
  [[nodiscard]] ExpandedNode expand_variable(const VariableRecord& variable) const;
  [[nodiscard]] ExpandedNode expand_type(const std::string& path,
                                         const std::string& display_name,
                                         const TypeNode* type,
                                         Availability availability,
                                         std::optional<std::uint64_t> absolute_address,
                                         std::optional<std::int64_t> relative_offset,
                                         std::size_t depth) const;
  [[nodiscard]] const TypeNode* find_type(const std::string& id) const;
  [[nodiscard]] std::uint64_t compute_array_count(const TypeNode& type) const;
  [[nodiscard]] std::optional<std::uint64_t> resolve_byte_size(const TypeNode* type) const;

  std::unordered_map<std::string, const TypeNode*> types_by_id_;
  std::size_t depth_limit_ = 0;
};

}  // namespace elf_static_view::analysis
