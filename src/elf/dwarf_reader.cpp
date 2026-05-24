#include "elf/dwarf_reader.hpp"

#include "analysis/model_utils.hpp"
#include "elf/elf_symbol_table.hpp"
#include "elf/dwarf_wrappers.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace elf_static_view::elf {

namespace {

DieHandle take_die(std::optional<DieHandle>&& value) {
  return std::move(value).value();
}

struct ReaderContext {
  Dwarf_Debug debug = nullptr;
  std::vector<TypeNode> types;
  std::vector<VariableRecord> variables;
  std::vector<CompileUnitRecord> compile_units;
  std::unordered_map<Dwarf_Off, std::string> type_ids;
  std::unordered_map<Dwarf_Off, std::vector<std::string>> variable_scope_by_offset;
  std::unordered_map<Dwarf_Off, bool> variable_static_member_by_offset;
  std::vector<std::string> scope_stack;
  std::vector<Dwarf_Half> scope_tag_stack;
  std::string current_compile_unit_name;
};

[[nodiscard]] std::string make_type_id(const Dwarf_Off offset) {
  return "type@" + std::to_string(offset);
}

[[nodiscard]] std::string maybe_name(const std::optional<std::string>& value,
                                     const std::string& fallback) {
  if (value.has_value() && !value->empty()) {
    return value.value();
  }
  return fallback;
}

void apply_symbol_addresses(const ElfSymbolTable& symbols, std::vector<VariableRecord>& variables) {
  for (auto& variable : variables) {
    // 多个 CU 里可能出现同名 file-static；如果 DWARF 已经给出精确绝对地址，
    // 这里不能再用按名称查到的 ELF 符号去覆盖，否则会把别的 CU 的同名局部符号地址串进来。
    if (variable.availability == Availability::StaticAddressKnown &&
        variable.address.kind == AddressKind::Absolute &&
        variable.address.absolute_address.has_value()) {
      continue;
    }

    std::vector<std::string> candidates;
    if (variable.linkage_name.has_value() && !variable.linkage_name->empty()) {
      candidates.push_back(variable.linkage_name.value());
    }
    candidates.push_back(analysis::join_scope(variable.scope_path, variable.name));
    candidates.push_back(variable.name);

    for (const auto& candidate : candidates) {
      const auto symbol = symbols.find(candidate);
      if (!symbol.has_value()) {
        continue;
      }
      variable.address.kind = AddressKind::Absolute;
      if (symbol->is_thread_local) {
        variable.is_thread_local = true;
        variable.variable_kind = VariableKind::ThreadLocal;
        variable.address.kind = AddressKind::Unknown;
        variable.address.absolute_address.reset();
        variable.address.location_description = "ELF symtab TLS";
        variable.availability = Availability::RuntimeOnly;
      } else {
        variable.address.absolute_address = symbol->value;
        variable.address.location_description = "ELF symtab";
        variable.availability = Availability::StaticAddressKnown;
        if (!variable.byte_size.has_value() && symbol->size > 0) {
          variable.byte_size = symbol->size;
        }
      }
      break;
    }
  }
}

[[nodiscard]] std::string variable_identity_name(const VariableRecord& variable) {
  const auto full_name = analysis::join_scope(variable.scope_path, variable.name);
  if (!full_name.empty() && variable.name != "<anon>") {
    return full_name;
  }
  if (variable.linkage_name.has_value() && !variable.linkage_name->empty()) {
    return variable.linkage_name.value();
  }
  return full_name;
}

void deduplicate_variables(std::vector<VariableRecord>& variables) {
  std::vector<VariableRecord> deduplicated;
  for (auto& variable : variables) {
    const auto dedup_name = variable_identity_name(variable);
    const auto dedup_key = variable.compile_unit_name + "|" + dedup_name;
    auto existing = std::find_if(deduplicated.begin(), deduplicated.end(), [&](const VariableRecord& item) {
      const auto existing_name = variable_identity_name(item);
      const auto existing_key = item.compile_unit_name + "|" + existing_name;
      return existing_key == dedup_key;
    });

    if (existing == deduplicated.end()) {
      deduplicated.push_back(std::move(variable));
      continue;
    }

    const bool prefer_current =
      variable.availability == Availability::StaticAddressKnown &&
      existing->availability != Availability::StaticAddressKnown;
    if (prefer_current) {
      *existing = std::move(variable);
    }
  }
  variables = std::move(deduplicated);
}

[[nodiscard]] TypeKind map_type_kind(const Dwarf_Half tag) {
  switch (tag) {
    case DW_TAG_base_type:
      return TypeKind::Base;
    case DW_TAG_pointer_type:
      return TypeKind::Pointer;
    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
      return TypeKind::Reference;
    case DW_TAG_typedef:
      return TypeKind::Typedef;
    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
      return TypeKind::Qualified;
    case DW_TAG_array_type:
      return TypeKind::Array;
    case DW_TAG_structure_type:
      return TypeKind::Struct;
    case DW_TAG_class_type:
      return TypeKind::Class;
    case DW_TAG_union_type:
      return TypeKind::Union;
    case DW_TAG_enumeration_type:
      return TypeKind::Enum;
    case DW_TAG_subroutine_type:
      return TypeKind::Subroutine;
    default:
      return TypeKind::Unknown;
  }
}

[[nodiscard]] bool is_class_scope_tag(const Dwarf_Half tag) {
  return tag == DW_TAG_class_type || tag == DW_TAG_structure_type || tag == DW_TAG_union_type;
}

[[nodiscard]] bool is_subprogram_scope_tag(const Dwarf_Half tag) {
  return tag == DW_TAG_subprogram;
}

[[nodiscard]] bool scope_contains_subprogram(const std::vector<Dwarf_Half>& scope_tag_stack) {
  return std::any_of(scope_tag_stack.begin(), scope_tag_stack.end(), is_subprogram_scope_tag);
}

[[nodiscard]] bool scope_ends_with_class(const std::vector<Dwarf_Half>& scope_tag_stack) {
  return !scope_tag_stack.empty() && is_class_scope_tag(scope_tag_stack.back());
}

[[nodiscard]] std::string language_name(const std::optional<Dwarf_Unsigned>& value) {
  if (!value.has_value()) {
    return "unknown";
  }
  switch (value.value()) {
    case DW_LANG_C:
      return "C";
    case DW_LANG_C_plus_plus:
    case DW_LANG_C_plus_plus_11:
    case DW_LANG_C_plus_plus_14:
    case DW_LANG_C_plus_plus_17:
    case DW_LANG_C_plus_plus_20:
      return "C++";
    default:
      return "other";
  }
}

[[nodiscard]] bool is_type_tag(const Dwarf_Half tag) {
  switch (tag) {
    case DW_TAG_base_type:
    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
    case DW_TAG_typedef:
    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
    case DW_TAG_array_type:
    case DW_TAG_structure_type:
    case DW_TAG_class_type:
    case DW_TAG_union_type:
    case DW_TAG_enumeration_type:
    case DW_TAG_subroutine_type:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] AddressInfo classify_location(const std::optional<LocationDescription>& location,
                                            const std::optional<Dwarf_Addr>& direct_address,
                                            Availability& availability) {
  AddressInfo info;
  if (direct_address.has_value()) {
    info.kind = AddressKind::Absolute;
    info.absolute_address = direct_address.value();
    info.location_description = "DW_FORM_addr";
    availability = Availability::StaticAddressKnown;
    return info;
  }

  if (!location.has_value() || location->operations.empty()) {
    info.kind = AddressKind::Unknown;
    info.location_description = "missing";
    availability = Availability::Unavailable;
    return info;
  }

  std::ostringstream description;
  for (std::size_t index = 0; index < location->operations.size(); ++index) {
    const auto& op = location->operations[index];
    if (index > 0) {
      description << ' ';
    }
    description << "op(" << static_cast<unsigned int>(op.atom) << ")";
  }
  info.location_description = description.str();

  if (location->kind == DW_LKIND_expression && location->operations.size() == 1 &&
      location->operations.front().atom == DW_OP_addr) {
    info.kind = AddressKind::Absolute;
    info.absolute_address = location->operations.front().operand1;
    availability = Availability::StaticAddressKnown;
    return info;
  }

  if (std::any_of(location->operations.begin(),
                  location->operations.end(),
                  [](const LocationOp& op) {
                    return op.atom == DW_OP_fbreg || op.atom == DW_OP_breg0 ||
                           op.atom == DW_OP_bregx || op.atom == DW_OP_reg0 ||
                           op.atom == DW_OP_regx || op.atom == DW_OP_call_frame_cfa;
                  })) {
    info.kind = AddressKind::Unknown;
    availability = Availability::RuntimeOnly;
    return info;
  }

  info.kind = AddressKind::Unknown;
  availability = Availability::Unavailable;
  return info;
}

[[nodiscard]] std::optional<std::string> read_producer(Dwarf_Debug debug, Dwarf_Die die) {
  const auto attr = attribute_of(debug, die, DW_AT_producer);
  if (!attr.has_value()) {
    return std::nullopt;
  }
  return string_attr(attr->get());
}

[[nodiscard]] std::optional<std::string> read_compilation_name(Dwarf_Debug debug, Dwarf_Die die) {
  const auto attr = attribute_of(debug, die, DW_AT_name);
  if (!attr.has_value()) {
    return std::nullopt;
  }
  return string_attr(attr->get());
}

[[nodiscard]] std::string resolve_type_id(ReaderContext& context,
                                          const std::optional<Dwarf_Off>& type_offset) {
  if (!type_offset.has_value()) {
    return "type@unknown";
  }
  const auto iter = context.type_ids.find(type_offset.value());
  if (iter != context.type_ids.end()) {
    return iter->second;
  }
  const auto id = make_type_id(type_offset.value());
  context.type_ids[type_offset.value()] = id;
  return id;
}

void walk_die_tree(ReaderContext& context, Dwarf_Die die, bool is_info);

void record_type(ReaderContext& context, Dwarf_Die die, const Dwarf_Half tag) {
  const auto offset = die_offset(die).value_or(0);
  auto type = TypeNode{};
  type.id = make_type_id(offset);
  context.type_ids[offset] = type.id;
  type.kind = map_type_kind(tag);
  type.name = maybe_name(die_name(die), type.id);

  if (const auto attr = attribute_of(context.debug, die, DW_AT_byte_size); attr.has_value()) {
    type.byte_size = unsigned_attr(attr->get());
  }
  if (const auto attr = attribute_of(context.debug, die, DW_AT_alignment); attr.has_value()) {
    type.alignment = unsigned_attr(attr->get());
  }
  if (const auto attr = attribute_of(context.debug, die, DW_AT_type); attr.has_value()) {
    const auto offset_value = global_type_offset(attr->get());
    if (offset_value.has_value()) {
      if (type.kind == TypeKind::Pointer || type.kind == TypeKind::Reference) {
        type.pointee_type = TypeRef{resolve_type_id(context, offset_value)};
      } else if (type.kind == TypeKind::Array) {
        type.element_type = TypeRef{resolve_type_id(context, offset_value)};
      } else if (type.kind == TypeKind::Typedef) {
        type.aliased_of = TypeRef{resolve_type_id(context, offset_value)};
      } else if (type.kind == TypeKind::Qualified) {
        type.qualified_of = TypeRef{resolve_type_id(context, offset_value)};
      }
    }
  }

  if (auto child = child_of(context.debug, die); child.has_value()) {
    auto current = take_die(std::move(child));
    while (current) {
      const auto child_tag = die_tag(current.get());
      if (type.kind == TypeKind::Array && child_tag == DW_TAG_subrange_type) {
        if (const auto upper_bound_attr = attribute_of(context.debug, current.get(), DW_AT_upper_bound);
            upper_bound_attr.has_value()) {
          if (const auto upper_bound = unsigned_attr(upper_bound_attr->get()); upper_bound.has_value()) {
            type.array_dimensions.push_back(upper_bound.value() + 1);
          }
        } else if (const auto count_attr = attribute_of(context.debug, current.get(), DW_AT_count);
                   count_attr.has_value()) {
          if (const auto count = unsigned_attr(count_attr->get()); count.has_value()) {
            type.array_dimensions.push_back(count.value());
          }
        }
      } else if ((type.kind == TypeKind::Struct || type.kind == TypeKind::Class ||
                  type.kind == TypeKind::Union) &&
                 child_tag == DW_TAG_member) {
        TypeMember member;
        member.name = maybe_name(die_name(current.get()), "<anon>");
        if (const auto member_type_attr = attribute_of(context.debug, current.get(), DW_AT_type);
            member_type_attr.has_value()) {
          member.type = TypeRef{resolve_type_id(context, global_type_offset(member_type_attr->get()))};
        }
        member.availability = Availability::StaticLayoutKnown;
        member.address.kind = AddressKind::MemberOffset;
        if (const auto location_attr =
              attribute_of(context.debug, current.get(), DW_AT_data_member_location);
            location_attr.has_value()) {
          if (const auto offset_value = unsigned_attr(location_attr->get()); offset_value.has_value()) {
            member.address.relative_offset = static_cast<std::int64_t>(offset_value.value());
          } else if (const auto signed_value = signed_attr(location_attr->get());
                     signed_value.has_value()) {
            member.address.relative_offset = signed_value.value();
          }
        }
        type.members.push_back(std::move(member));
      } else if ((type.kind == TypeKind::Struct || type.kind == TypeKind::Class) &&
                 child_tag == DW_TAG_inheritance) {
        BaseRelation base;
        if (const auto base_type_attr = attribute_of(context.debug, current.get(), DW_AT_type);
            base_type_attr.has_value()) {
          base.type = TypeRef{resolve_type_id(context, global_type_offset(base_type_attr->get()))};
        }
        if (const auto offset_attr =
              attribute_of(context.debug, current.get(), DW_AT_data_member_location);
            offset_attr.has_value()) {
          if (const auto offset_value = unsigned_attr(offset_attr->get()); offset_value.has_value()) {
            base.offset = offset_value.value();
          }
        }
        type.bases.push_back(std::move(base));
      } else if (type.kind == TypeKind::Enum && child_tag == DW_TAG_enumerator) {
        if (const auto name = die_name(current.get()); name.has_value()) {
          type.enum_values.push_back(name.value());
        }
      }

      auto sibling = sibling_of(context.debug, current.get(), true);
      if (!sibling.has_value()) {
        break;
      }
      current = take_die(std::move(sibling));
    }
  }

  context.types.push_back(std::move(type));
}

[[nodiscard]] VariableKind classify_variable_kind(const std::vector<std::string>& scope_stack,
                                                  const std::vector<Dwarf_Half>& scope_tag_stack,
                                                  const bool external,
                                                  const bool has_static_storage,
                                                  const bool is_thread_local,
                                                  const bool is_static_member) {
  if (is_thread_local) {
    return VariableKind::ThreadLocal;
  }
  if (is_static_member || (has_static_storage && scope_ends_with_class(scope_tag_stack))) {
    return VariableKind::StaticMember;
  }
  if (scope_stack.empty()) {
    return external ? VariableKind::Global : VariableKind::FileStatic;
  }
  if (scope_contains_subprogram(scope_tag_stack)) {
    return has_static_storage ? VariableKind::FunctionStatic : VariableKind::Local;
  }
  return external ? VariableKind::Namespace : VariableKind::FileStatic;
}

void record_variable(ReaderContext& context, Dwarf_Die die, const Dwarf_Half tag) {
  VariableRecord variable;
  variable.id = "var@" + std::to_string(die_offset(die).value_or(0));
  variable.name = maybe_name(die_name(die), "<anon>");
  variable.compile_unit_name = context.current_compile_unit_name;
  variable.scope_path = context.scope_stack;
  auto scope_tag_stack = context.scope_tag_stack;
  bool is_static_member = scope_ends_with_class(scope_tag_stack);
  variable.has_static_storage = tag == DW_TAG_variable;

  if (const auto type_attr = attribute_of(context.debug, die, DW_AT_type); type_attr.has_value()) {
    variable.type = TypeRef{resolve_type_id(context, global_type_offset(type_attr->get()))};
  } else {
    variable.type = TypeRef{"type@unknown"};
  }
  if (const auto byte_size_attr = attribute_of(context.debug, die, DW_AT_byte_size); byte_size_attr.has_value()) {
    variable.byte_size = unsigned_attr(byte_size_attr->get());
  }
  if (const auto linkage_attr = attribute_of(context.debug, die, DW_AT_linkage_name); linkage_attr.has_value()) {
    variable.linkage_name = string_attr(linkage_attr->get());
  } else if (const auto mips_linkage_attr =
               attribute_of(context.debug, die, DW_AT_MIPS_linkage_name);
             mips_linkage_attr.has_value()) {
    variable.linkage_name = string_attr(mips_linkage_attr->get());
  }

  const auto external_attr = attribute_of(context.debug, die, DW_AT_external);
  const bool external = external_attr.has_value() && flag_attr(external_attr->get()).value_or(false);
  const auto location_attr = attribute_of(context.debug, die, DW_AT_location);
  const auto location_desc =
    location_attr.has_value() ? read_location_description(location_attr->get()) : std::nullopt;
  const auto direct_addr = location_attr.has_value()
                             ? address_attr(location_attr->get())
                             : std::nullopt;
  const auto resolved_indexed_addr =
    location_desc.has_value() ? indexed_address_from_die_location(die, location_desc.value()) : std::nullopt;
  const bool is_thread_local =
    location_desc.has_value() &&
    std::any_of(location_desc->operations.begin(),
                location_desc->operations.end(),
                [](const LocationOp& op) { return op.atom == DW_OP_form_tls_address; });
  variable.is_thread_local = is_thread_local;
  // 这里先把位置表达式归一到统一模型，后面的 CLI 和测试都只依赖这个分类结果。
  variable.availability = Availability::Unavailable;
  variable.address = classify_location(location_desc,
                                       direct_addr.has_value() ? direct_addr : resolved_indexed_addr,
                                       variable.availability);

  if (is_thread_local) {
    variable.availability = Availability::RuntimeOnly;
    variable.address.kind = AddressKind::Unknown;
    variable.address.absolute_address.reset();
    variable.address.location_description = "thread_local";
  }

  context.variable_scope_by_offset[die_offset(die).value_or(0)] = variable.scope_path;

  if (tag == DW_TAG_formal_parameter) {
    variable.availability = Availability::RuntimeOnly;
  }

  if (const auto specification_attr = attribute_of(context.debug, die, DW_AT_specification);
      specification_attr.has_value()) {
    if (const auto specification_offset = global_type_offset(specification_attr->get());
        specification_offset.has_value()) {
      if (const auto found_scope = context.variable_scope_by_offset.find(specification_offset.value());
          found_scope != context.variable_scope_by_offset.end()) {
        variable.scope_path = found_scope->second;
      }
      if (const auto found_static_member =
            context.variable_static_member_by_offset.find(specification_offset.value());
          found_static_member != context.variable_static_member_by_offset.end()) {
        is_static_member = found_static_member->second;
      }
      if (variable.name == "<anon>") {
        if (auto specification_die = die_from_offset(context.debug, specification_offset.value(), true);
            specification_die.has_value()) {
          variable.name = maybe_name(die_name(specification_die->get()), variable.name);
          if (variable.type.id == "type@unknown") {
            if (const auto spec_type_attr =
                  attribute_of(context.debug, specification_die->get(), DW_AT_type);
                spec_type_attr.has_value()) {
              variable.type =
                TypeRef{resolve_type_id(context, global_type_offset(spec_type_attr->get()))};
            }
          }
        }
      }
    }
  }

  if (variable.scope_path != context.scope_stack) {
    scope_tag_stack.clear();
  }
  variable.variable_kind =
    (tag == DW_TAG_formal_parameter)
      ? VariableKind::Parameter
      : classify_variable_kind(variable.scope_path,
                               scope_tag_stack,
                               external,
                               variable.has_static_storage,
                               is_thread_local,
                               is_static_member);

  const auto variable_offset = die_offset(die).value_or(0);
  context.variable_scope_by_offset[variable_offset] = variable.scope_path;
  context.variable_static_member_by_offset[variable_offset] = is_static_member;
  context.variables.push_back(std::move(variable));
}

void walk_children(ReaderContext& context, Dwarf_Die die, const bool is_info) {
  auto child = child_of(context.debug, die);
  if (!child.has_value()) {
    return;
  }
  auto current = take_die(std::move(child));
  while (current) {
    walk_die_tree(context, current.get(), is_info);
    auto sibling = sibling_of(context.debug, current.get(), is_info);
    if (!sibling.has_value()) {
      break;
    }
    current = take_die(std::move(sibling));
  }
}

void walk_die_tree(ReaderContext& context, Dwarf_Die die, const bool is_info) {
  const auto tag = die_tag(die);
  const bool pushes_scope =
    tag == DW_TAG_subprogram || tag == DW_TAG_namespace || tag == DW_TAG_class_type ||
    tag == DW_TAG_structure_type;
  if (pushes_scope) {
    context.scope_stack.push_back(maybe_name(die_name(die), "<anon>"));
    context.scope_tag_stack.push_back(tag);
  }

  if (is_type_tag(tag)) {
    record_type(context, die, tag);
  } else if (tag == DW_TAG_variable || tag == DW_TAG_formal_parameter) {
    record_variable(context, die, tag);
  }

  walk_children(context, die, is_info);

  if (pushes_scope) {
    context.scope_tag_stack.pop_back();
    context.scope_stack.pop_back();
  }
}

}  // namespace

ProjectModel DwarfReader::load(const std::string& file_path) const {
  DebugHandle debug(file_path);
  ReaderContext context;
  context.debug = debug.get();

  std::size_t index = 0;
  while (true) {
    Dwarf_Die cu_die_raw = nullptr;
    Dwarf_Unsigned cu_header_length = 0;
    Dwarf_Half version_stamp = 0;
    Dwarf_Off abbrev_offset = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Half length_size = 0;
    Dwarf_Half extension_size = 0;
    Dwarf_Sig8 type_signature {};
    Dwarf_Unsigned type_offset = 0;
    Dwarf_Unsigned next_cu_header = 0;
    Dwarf_Half header_cu_type = 0;
    Dwarf_Error error = nullptr;
    const int result = dwarf_next_cu_header_e(context.debug,
                                              true,
                                              &cu_die_raw,
                                              &cu_header_length,
                                              &version_stamp,
                                              &abbrev_offset,
                                              &address_size,
                                              &length_size,
                                              &extension_size,
                                              &type_signature,
                                              &type_offset,
                                              &next_cu_header,
                                              &header_cu_type,
                                              &error);
    if (result == DW_DLV_NO_ENTRY) {
      break;
    }
    if (result != DW_DLV_OK) {
      throw DwarfError("dwarf_next_cu_header_e failed: " + std::string(dwarf_errmsg(error)));
    }

    DieHandle cu_die(context.debug, cu_die_raw);
    CompileUnitRecord cu;
    cu.id = "cu@" + std::to_string(index);
    cu.name = maybe_name(read_compilation_name(context.debug, cu_die.get()), "<unknown>");
    cu.producer = maybe_name(read_producer(context.debug, cu_die.get()), "");
    if (const auto language_attr = attribute_of(context.debug, cu_die.get(), DW_AT_language);
        language_attr.has_value()) {
      cu.language = language_name(unsigned_attr(language_attr->get()));
    } else {
      cu.language = "unknown";
    }
    context.current_compile_unit_name = cu.name;
    context.compile_units.push_back(cu);
    walk_die_tree(context, cu_die.get(), true);
    ++index;
  }

  ProjectModel model;
  model.file = file_path;
  model.compile_units = std::move(context.compile_units);
  model.types = std::move(context.types);
  model.symbols = std::move(context.variables);
  // DWARF 位置表达式不总能直接给出静态绝对地址，这里再用 ELF 符号表补一遍静态对象地址。
  apply_symbol_addresses(ElfSymbolTable::load(file_path), model.symbols);
  deduplicate_variables(model.symbols);
  return model;
}

}  // namespace elf_static_view::elf
