#include "analysis/expander.hpp"

#include "analysis/model_utils.hpp"

#include <algorithm>

namespace elf_static_view::analysis {

Expander::Expander(const std::vector<TypeNode>& types, std::size_t depth_limit)
  : depth_limit_(depth_limit) {
  for (const auto& type : types) {
    types_by_id_[type.id] = &type;
  }
}

std::vector<ExpandedNode> Expander::build(const std::vector<VariableRecord>& variables,
                                          bool include_runtime_only,
                                          bool only_static_known,
                                          const std::optional<std::string>& symbol_name) const {
  std::vector<ExpandedNode> nodes;
  for (const auto& variable : variables) {
    if (!should_emit_symbol(variable, include_runtime_only, only_static_known, symbol_name)) {
      continue;
    }
    nodes.push_back(expand_variable(variable));
  }
  std::vector<ExpandedNode> deduplicated;
  for (auto& node : nodes) {
    auto existing = std::find_if(deduplicated.begin(), deduplicated.end(), [&](const ExpandedNode& item) {
      return item.path == node.path;
    });
    if (existing == deduplicated.end()) {
      deduplicated.push_back(std::move(node));
      continue;
    }
    if (existing->availability != Availability::StaticAddressKnown &&
        node.availability == Availability::StaticAddressKnown) {
      *existing = std::move(node);
    }
  }
  return deduplicated;
}

ExpandedNode Expander::expand_variable(const VariableRecord& variable) const {
  const auto* type = find_type(variable.type.id);
  const auto path = join_scope(variable.scope_path, variable.name);
  return expand_type(path,
                     variable.name,
                     type,
                     variable.availability,
                     variable.address.absolute_address,
                     variable.address.relative_offset,
                     0);
}

ExpandedNode Expander::expand_type(const std::string& path,
                                   const std::string& display_name,
                                   const TypeNode* type,
                                   Availability availability,
                                   std::optional<std::uint64_t> absolute_address,
                                   std::optional<std::int64_t> relative_offset,
                                   std::size_t depth) const {
  ExpandedNode node;
  node.path = path;
  node.display_name = display_name;
  node.availability = availability;
  node.absolute_address = absolute_address;
  node.relative_offset = relative_offset;
  if (type == nullptr) {
    node.type_name = "<unknown>";
    node.type_kind = TypeKind::Unknown;
    return node;
  }

  node.type_name = display_type_name(type);
  node.type_kind = type->kind;
  node.byte_size = type->byte_size;

  if (depth >= depth_limit_) {
    return node;
  }

  if (type->kind == TypeKind::Array) {
    const auto* element_type = type->element_type ? find_type(type->element_type->id) : nullptr;
    const auto total_count = compute_array_count(*type);
    node.array_count = total_count;
    if (const auto element_size = resolve_byte_size(element_type); element_size.has_value()) {
      node.array_stride = element_size.value();
    } else if (type->byte_size.has_value() && total_count > 0) {
      node.array_stride = type->byte_size.value() / total_count;
    }
    if (element_type != nullptr) {
      const auto stride = node.array_stride.value_or(0);
      for (std::uint64_t index = 0; index < total_count; ++index) {
        std::optional<std::uint64_t> child_address;
        if (absolute_address.has_value()) {
          child_address = absolute_address.value() + stride * index;
        }
        node.children.push_back(expand_type(path + "[" + std::to_string(index) + "]",
                                            display_name + "[" + std::to_string(index) + "]",
                                            element_type,
                                            availability,
                                            child_address,
                                            static_cast<std::int64_t>(stride * index),
                                            depth + 1));
      }
    }
    return node;
  }

  // 这里统一展开聚合类型，保证 CLI 和后续 GUI 都消费同一棵结构树。
  if (type->kind == TypeKind::Struct || type->kind == TypeKind::Class ||
      type->kind == TypeKind::Union) {
    for (const auto& base : type->bases) {
      const auto* base_type = find_type(base.type.id);
      std::optional<std::uint64_t> child_address;
      if (absolute_address.has_value()) {
        child_address = absolute_address.value() + base.offset;
      }
      node.children.push_back(expand_type(path + "::<base>",
                                          "<base>",
                                          base_type,
                                          Availability::StaticLayoutKnown,
                                          child_address,
                                          static_cast<std::int64_t>(base.offset),
                                          depth + 1));
    }
    for (const auto& member : type->members) {
      const auto* member_type = find_type(member.type.id);
      std::optional<std::uint64_t> child_address;
      if (absolute_address.has_value() && member.address.relative_offset.has_value()) {
        child_address =
          absolute_address.value() + static_cast<std::uint64_t>(member.address.relative_offset.value());
      }
      node.children.push_back(expand_type(path + "." + member.name,
                                          member.name,
                                          member_type,
                                          member.availability,
                                          child_address,
                                          member.address.relative_offset,
                                          depth + 1));
    }
  }

  return node;
}

const TypeNode* Expander::find_type(const std::string& id) const {
  const auto iter = types_by_id_.find(id);
  if (iter == types_by_id_.end()) {
    return nullptr;
  }
  return iter->second;
}

std::string Expander::display_type_name(const TypeNode* type) const {
  if (type == nullptr) {
    return "<unknown>";
  }
  if (!type->name.empty() && type->name != type->id) {
    return type->name;
  }

  // 指针/引用/数组这类 DWARF 类型通常没有 DW_AT_name，需要从被引用类型拼出可读名称。
  if (type->kind == TypeKind::Pointer) {
    return display_type_name(type->pointee_type ? find_type(type->pointee_type->id) : nullptr) + "*";
  }
  if (type->kind == TypeKind::Reference) {
    return display_type_name(type->pointee_type ? find_type(type->pointee_type->id) : nullptr) + "&";
  }
  if (type->kind == TypeKind::Qualified) {
    return display_type_name(type->qualified_of ? find_type(type->qualified_of->id) : nullptr);
  }
  if (type->kind == TypeKind::Typedef) {
    return display_type_name(type->aliased_of ? find_type(type->aliased_of->id) : nullptr);
  }
  if (type->kind == TypeKind::Array) {
    auto name = display_type_name(type->element_type ? find_type(type->element_type->id) : nullptr);
    for (const auto dimension : type->array_dimensions) {
      name += "[" + std::to_string(dimension) + "]";
    }
    return name;
  }

  return type->name.empty() ? type->id : type->name;
}

std::uint64_t Expander::compute_array_count(const TypeNode& type) const {
  if (type.array_dimensions.empty()) {
    return 0;
  }
  std::uint64_t count = 1;
  for (const auto dimension : type.array_dimensions) {
    count *= dimension;
  }
  return count;
}

std::optional<std::uint64_t> Expander::resolve_byte_size(const TypeNode* type) const {
  if (type == nullptr) {
    return std::nullopt;
  }
  if (type->byte_size.has_value()) {
    return type->byte_size;
  }
  if (type->kind == TypeKind::Typedef && type->aliased_of.has_value()) {
    return resolve_byte_size(find_type(type->aliased_of->id));
  }
  if (type->kind == TypeKind::Qualified && type->qualified_of.has_value()) {
    return resolve_byte_size(find_type(type->qualified_of->id));
  }
  return std::nullopt;
}

}  // namespace elf_static_view::analysis
