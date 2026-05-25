#include "elf_static_view/project.hpp"

#include "analysis/model_utils.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <sstream>
#include <stdexcept>

namespace elf_static_view {

namespace {

template <typename T>
void append_optional_number_field(std::ostringstream& stream,
                                  const std::string& key,
                                  const std::optional<T>& value,
                                  const int level,
                                  const bool trailing_comma = true) {
  for (int i = 0; i < level; ++i) {
    stream << "  ";
  }
  stream << '"' << key << "\": ";
  if (value.has_value()) {
    stream << value.value();
  } else {
    stream << "null";
  }
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

void append_indent(std::ostringstream& stream, const int level) {
  for (int i = 0; i < level; ++i) {
    stream << "  ";
  }
}

void append_string(std::ostringstream& stream, const std::string& value) {
  stream << '"' << analysis::escape_json(value) << '"';
}

void append_string_field(std::ostringstream& stream,
                         const std::string& key,
                         const std::string& value,
                         const int level,
                         const bool trailing_comma = true) {
  append_indent(stream, level);
  append_string(stream, key);
  stream << ": ";
  append_string(stream, value);
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

void append_optional_string_field(std::ostringstream& stream,
                                  const std::string& key,
                                  const std::optional<std::string>& value,
                                  const int level,
                                  const bool trailing_comma = true) {
  append_indent(stream, level);
  append_string(stream, key);
  stream << ": ";
  if (value.has_value()) {
    append_string(stream, value.value());
  } else {
    stream << "null";
  }
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

template <typename T>
void append_number_field(std::ostringstream& stream,
                         const std::string& key,
                         const T value,
                         const int level,
                         const bool trailing_comma = true) {
  append_indent(stream, level);
  append_string(stream, key);
  stream << ": " << value;
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

void append_bool_field(std::ostringstream& stream,
                       const std::string& key,
                       const bool value,
                       const int level,
                       const bool trailing_comma = true) {
  append_indent(stream, level);
  append_string(stream, key);
  stream << ": " << (value ? "true" : "false");
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

template <typename T>
void append_optional_number_value(std::ostringstream& stream, const std::optional<T>& value) {
  if (value.has_value()) {
    stream << value.value();
  } else {
    stream << "null";
  }
}

template <typename Writer>
void append_array(std::ostringstream& stream,
                  const std::string& key,
                  const std::size_t size,
                  const int level,
                  Writer&& writer,
                  const bool trailing_comma = true) {
  append_indent(stream, level);
  append_string(stream, key);
  stream << ": [\n";
  for (std::size_t index = 0; index < size; ++index) {
    writer(index, level + 1);
    if (index + 1 < size) {
      stream << ',';
    }
    stream << '\n';
  }
  append_indent(stream, level);
  stream << ']';
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

void append_string_array_inline(std::ostringstream& stream,
                                const std::string& key,
                                const std::vector<std::string>& values,
                                const int level,
                                const bool trailing_comma = true) {
  append_indent(stream, level);
  append_string(stream, key);
  stream << ": [";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index > 0) {
      stream << ", ";
    }
    append_string(stream, values[index]);
  }
  stream << ']';
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

template <typename T>
std::optional<T> parse_optional_number(const YAML::Node& node, const std::string& field_name) {
  const auto field = node[field_name];
  if (!field || field.IsNull()) {
    return std::nullopt;
  }
  return field.as<T>();
}

std::optional<std::string> parse_optional_string(const YAML::Node& node, const std::string& field_name) {
  const auto field = node[field_name];
  if (!field || field.IsNull()) {
    return std::nullopt;
  }
  return field.as<std::string>();
}

AddressKind parse_address_kind(const std::string& value) {
  if (value == "Absolute") {
    return AddressKind::Absolute;
  }
  if (value == "SectionRelative") {
    return AddressKind::SectionRelative;
  }
  if (value == "MemberOffset") {
    return AddressKind::MemberOffset;
  }
  if (value == "ArrayElementOffset") {
    return AddressKind::ArrayElementOffset;
  }
  if (value == "BitField") {
    return AddressKind::BitField;
  }
  if (value == "Unknown") {
    return AddressKind::Unknown;
  }
  throw std::runtime_error("未知 AddressKind: " + value);
}

Availability parse_availability(const std::string& value) {
  if (value == "StaticAddressKnown") {
    return Availability::StaticAddressKnown;
  }
  if (value == "StaticLayoutKnown") {
    return Availability::StaticLayoutKnown;
  }
  if (value == "RuntimeOnly") {
    return Availability::RuntimeOnly;
  }
  if (value == "Unavailable") {
    return Availability::Unavailable;
  }
  if (value == "OptimizedOut") {
    return Availability::OptimizedOut;
  }
  throw std::runtime_error("未知 Availability: " + value);
}

TypeKind parse_type_kind(const std::string& value) {
  if (value == "Base") {
    return TypeKind::Base;
  }
  if (value == "Pointer") {
    return TypeKind::Pointer;
  }
  if (value == "Reference") {
    return TypeKind::Reference;
  }
  if (value == "MemberPointer") {
    return TypeKind::MemberPointer;
  }
  if (value == "Typedef") {
    return TypeKind::Typedef;
  }
  if (value == "Qualified") {
    return TypeKind::Qualified;
  }
  if (value == "Array") {
    return TypeKind::Array;
  }
  if (value == "Struct") {
    return TypeKind::Struct;
  }
  if (value == "Class") {
    return TypeKind::Class;
  }
  if (value == "Union") {
    return TypeKind::Union;
  }
  if (value == "Enum") {
    return TypeKind::Enum;
  }
  if (value == "Subroutine") {
    return TypeKind::Subroutine;
  }
  if (value == "Atomic") {
    return TypeKind::Atomic;
  }
  if (value == "Unspecified") {
    return TypeKind::Unspecified;
  }
  if (value == "Unknown") {
    return TypeKind::Unknown;
  }
  throw std::runtime_error("未知 TypeKind: " + value);
}

VariableKind parse_variable_kind(const std::string& value) {
  if (value == "Global") {
    return VariableKind::Global;
  }
  if (value == "Namespace") {
    return VariableKind::Namespace;
  }
  if (value == "FileStatic") {
    return VariableKind::FileStatic;
  }
  if (value == "FunctionStatic") {
    return VariableKind::FunctionStatic;
  }
  if (value == "StaticMember") {
    return VariableKind::StaticMember;
  }
  if (value == "Local") {
    return VariableKind::Local;
  }
  if (value == "Parameter") {
    return VariableKind::Parameter;
  }
  if (value == "ThreadLocal") {
    return VariableKind::ThreadLocal;
  }
  if (value == "Unknown") {
    return VariableKind::Unknown;
  }
  throw std::runtime_error("未知 VariableKind: " + value);
}

void append_address_info(std::ostringstream& stream,
                         const AddressInfo& address,
                         const int level,
                         const bool trailing_comma = true) {
  append_indent(stream, level);
  stream << "{\n";
  append_string_field(stream, "kind", to_string(address.kind), level + 1);
  append_optional_number_field(stream, "absolute_address", address.absolute_address, level + 1);
  append_optional_number_field(stream, "relative_offset", address.relative_offset, level + 1);
  append_indent(stream, level + 1);
  append_string(stream, "section_name");
  stream << ": ";
  if (address.section_name.has_value()) {
    append_string(stream, address.section_name.value());
  } else {
    stream << "null";
  }
  stream << ",\n";
  append_optional_number_field(stream, "bit_offset", address.bit_offset, level + 1);
  append_optional_number_field(stream, "bit_size", address.bit_size, level + 1);
  append_optional_number_field(stream, "location_entry_count", address.location_entry_count, level + 1);
  append_indent(stream, level + 1);
  stream << "\"location_ranges\": [\n";
  for (std::size_t index = 0; index < address.location_ranges.size(); ++index) {
    const auto& range = address.location_ranges[index];
    append_indent(stream, level + 2);
    stream << "{\n";
    append_indent(stream, level + 3);
    stream << "\"raw_low_pc\": ";
    append_optional_number_value(stream, range.raw_low_pc);
    stream << ",\n";
    append_indent(stream, level + 3);
    stream << "\"raw_high_pc\": ";
    append_optional_number_value(stream, range.raw_high_pc);
    stream << ",\n";
    append_indent(stream, level + 3);
    stream << "\"cooked_low_pc\": ";
    append_optional_number_value(stream, range.cooked_low_pc);
    stream << ",\n";
    append_indent(stream, level + 3);
    stream << "\"cooked_high_pc\": ";
    append_optional_number_value(stream, range.cooked_high_pc);
    stream << ",\n";
    append_bool_field(stream,
                      "debug_addr_unavailable",
                      range.debug_addr_unavailable,
                      level + 3,
                      false);
    append_indent(stream, level + 2);
    stream << '}';
    if (index + 1 < address.location_ranges.size()) {
      stream << ',';
    }
    stream << '\n';
  }
  append_indent(stream, level + 1);
  stream << "],\n";
  append_string_field(stream, "location_description", address.location_description, level + 1, false);
  append_indent(stream, level);
  stream << '}';
  if (trailing_comma) {
    stream << ',';
  }
}

void append_type_ref(std::ostringstream& stream,
                     const std::string& key,
                     const TypeRef& ref,
                     const int level,
                     const bool trailing_comma = true) {
  append_indent(stream, level);
  append_string(stream, key);
  stream << ": {\n";
  append_string_field(stream, "id", ref.id, level + 1, false);
  append_indent(stream, level);
  stream << '}';
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

void append_optional_type_ref(std::ostringstream& stream,
                              const std::string& key,
                              const std::optional<TypeRef>& ref,
                              const int level,
                              const bool trailing_comma = true) {
  append_indent(stream, level);
  append_string(stream, key);
  stream << ": ";
  if (ref.has_value()) {
    stream << "{\n";
    append_string_field(stream, "id", ref->id, level + 1, false);
    append_indent(stream, level);
    stream << '}';
  } else {
    stream << "null";
  }
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

void append_type_member(std::ostringstream& stream, const TypeMember& member, const int level) {
  append_indent(stream, level);
  stream << "{\n";
  append_string_field(stream, "name", member.name, level + 1);
  append_type_ref(stream, "type", member.type, level + 1);
  append_indent(stream, level + 1);
  append_string(stream, "address");
  stream << ": ";
  append_address_info(stream, member.address, level + 1);
  stream << '\n';
  append_string_field(stream, "availability", to_string(member.availability), level + 1);
  append_optional_number_field(stream, "byte_size", member.byte_size, level + 1, false);
  append_indent(stream, level);
  stream << '}';
}

void append_base_relation(std::ostringstream& stream, const BaseRelation& relation, const int level) {
  append_indent(stream, level);
  stream << "{\n";
  append_type_ref(stream, "type", relation.type, level + 1);
  append_number_field(stream, "offset", relation.offset, level + 1, false);
  append_indent(stream, level);
  stream << '}';
}

void append_type_node(std::ostringstream& stream, const TypeNode& type, const int level) {
  append_indent(stream, level);
  stream << "{\n";
  append_string_field(stream, "id", type.id, level + 1);
  append_string_field(stream, "kind", to_string(type.kind), level + 1);
  append_string_field(stream, "name", type.name, level + 1);
  append_optional_number_field(stream, "byte_size", type.byte_size, level + 1);
  append_optional_number_field(stream, "alignment", type.alignment, level + 1);
  append_optional_type_ref(stream, "qualified_of", type.qualified_of, level + 1);
  append_optional_type_ref(stream, "aliased_of", type.aliased_of, level + 1);
  append_optional_type_ref(stream, "pointee_type", type.pointee_type, level + 1);
  append_optional_type_ref(stream, "element_type", type.element_type, level + 1);
  append_array(stream, "array_dimensions", type.array_dimensions.size(), level + 1, [&](const std::size_t index, const int item_level) {
    append_indent(stream, item_level);
    stream << type.array_dimensions[index];
  });
  append_array(stream, "members", type.members.size(), level + 1, [&](const std::size_t index, const int item_level) {
    append_type_member(stream, type.members[index], item_level);
  });
  append_array(stream, "bases", type.bases.size(), level + 1, [&](const std::size_t index, const int item_level) {
    append_base_relation(stream, type.bases[index], item_level);
  });
  append_array(stream, "enum_values", type.enum_values.size(), level + 1, [&](const std::size_t index, const int item_level) {
    append_indent(stream, item_level);
    append_string(stream, type.enum_values[index]);
  }, false);
  append_indent(stream, level);
  stream << '}';
}

void append_variable_record(std::ostringstream& stream, const VariableRecord& record, const int level) {
  append_indent(stream, level);
  stream << "{\n";
  append_string_field(stream, "id", record.id, level + 1);
  append_string_field(stream, "name", record.name, level + 1);
  append_string_field(stream, "compile_unit_name", record.compile_unit_name, level + 1);
  append_indent(stream, level + 1);
  append_string(stream, "linkage_name");
  stream << ": ";
  if (record.linkage_name.has_value()) {
    append_string(stream, record.linkage_name.value());
  } else {
    stream << "null";
  }
  stream << ",\n";
  append_string_field(stream, "kind", to_string(record.variable_kind), level + 1);
  append_string_field(stream, "availability", to_string(record.availability), level + 1);
  append_indent(stream, level + 1);
  append_string(stream, "address");
  stream << ": ";
  append_address_info(stream, record.address, level + 1);
  stream << '\n';
  append_type_ref(stream, "type", record.type, level + 1);
  append_string_array_inline(stream, "scope_path", record.scope_path, level + 1);
  append_optional_number_field(stream, "byte_size", record.byte_size, level + 1);
  append_optional_number_field(stream, "const_value", record.const_value, level + 1);
  append_optional_string_field(stream, "const_value_text", record.const_value_text, level + 1);
  append_bool_field(stream, "is_thread_local", record.is_thread_local, level + 1);
  append_bool_field(stream, "has_static_storage", record.has_static_storage, level + 1, false);
  append_indent(stream, level);
  stream << '}';
}

void append_compile_unit_record(std::ostringstream& stream, const CompileUnitRecord& unit, const int level) {
  append_indent(stream, level);
  stream << "{\n";
  append_string_field(stream, "id", unit.id, level + 1);
  append_string_field(stream, "name", unit.name, level + 1);
  append_string_field(stream, "producer", unit.producer, level + 1);
  append_string_field(stream, "language", unit.language, level + 1);
  append_indent(stream, level + 1);
  append_string(stream, "address");
  stream << ": ";
  append_address_info(stream, unit.address, level + 1);
  stream << '\n';
  append_indent(stream, level);
  stream << '}';
}

void append_elf_file_info(std::ostringstream& stream,
                          const ElfFileInfo& info,
                          const int level,
                          const bool trailing_comma = true) {
  append_indent(stream, level);
  stream << "{\n";
  append_string_field(stream, "object_class", info.object_class, level + 1);
  append_string_field(stream, "byte_order", info.byte_order, level + 1);
  append_string_field(stream, "file_type", info.file_type, level + 1);
  append_string_field(stream, "machine", info.machine, level + 1);
  append_string_field(stream, "os_abi", info.os_abi, level + 1, false);
  append_indent(stream, level);
  stream << '}';
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

void append_expanded_node(std::ostringstream& stream, const ExpandedNode& node, const int level) {
  append_indent(stream, level);
  stream << "{\n";
  append_string_field(stream, "path", node.path, level + 1);
  append_string_field(stream, "display_name", node.display_name, level + 1);
  append_string_field(stream, "type_name", node.type_name, level + 1);
  append_string_field(stream, "type_id", node.type_id, level + 1);
  append_string_field(stream, "type_kind", to_string(node.type_kind), level + 1);
  append_string_field(stream, "availability", to_string(node.availability), level + 1);
  append_optional_number_field(stream, "absolute_address", node.absolute_address, level + 1);
  append_optional_number_field(stream, "relative_offset", node.relative_offset, level + 1);
  append_optional_number_field(stream, "byte_size", node.byte_size, level + 1);
  append_optional_number_field(stream, "array_count", node.array_count, level + 1);
  append_optional_number_field(stream, "array_stride", node.array_stride, level + 1);
  append_number_field(stream, "depth", node.depth, level + 1);
  append_bool_field(stream, "children_lazy", node.children_lazy, level + 1);
  append_array(stream, "children", node.children.size(), level + 1, [&](const std::size_t index, const int item_level) {
    append_expanded_node(stream, node.children[index], item_level);
  }, false);
  append_indent(stream, level);
  stream << '}';
}

void append_raw_dwarf_attribute(std::ostringstream& stream,
                                const RawDwarfAttribute& attribute,
                                const int level) {
  append_indent(stream, level);
  stream << "{\n";
  append_string_field(stream, "name", attribute.name, level + 1);
  append_string_field(stream, "form", attribute.form, level + 1);
  append_string_field(stream, "value", attribute.value, level + 1, false);
  append_indent(stream, level);
  stream << '}';
}

void append_raw_dwarf_die(std::ostringstream& stream, const RawDwarfDie& die, const int level) {
  append_indent(stream, level);
  stream << "{\n";
  append_number_field(stream, "offset", die.offset, level + 1);
  append_string_field(stream, "tag", die.tag, level + 1);
  append_string_field(stream, "name", die.name, level + 1);
  append_array(stream,
               "attributes",
               die.attributes.size(),
               level + 1,
               [&](const std::size_t index, const int item_level) {
                 append_raw_dwarf_attribute(stream, die.attributes[index], item_level);
               });
  append_array(stream,
               "children",
               die.children.size(),
               level + 1,
               [&](const std::size_t index, const int item_level) {
                 append_raw_dwarf_die(stream, die.children[index], item_level);
               },
               false);
  append_indent(stream, level);
  stream << '}';
}

void append_raw_dwarf_compile_unit(std::ostringstream& stream,
                                   const RawDwarfCompileUnit& compile_unit,
                                   const int level) {
  append_indent(stream, level);
  stream << "{\n";
  append_number_field(stream, "index", compile_unit.index, level + 1);
  append_number_field(stream, "version", compile_unit.version, level + 1);
  append_number_field(stream, "header_length", compile_unit.header_length, level + 1);
  append_number_field(stream, "abbrev_offset", compile_unit.abbrev_offset, level + 1);
  append_number_field(stream, "address_size", compile_unit.address_size, level + 1);
  append_number_field(stream, "length_size", compile_unit.length_size, level + 1);
  append_number_field(stream, "extension_size", compile_unit.extension_size, level + 1);
  append_number_field(stream, "next_header_offset", compile_unit.next_header_offset, level + 1);
  append_string_field(stream, "unit_type", compile_unit.unit_type, level + 1);
  append_indent(stream, level + 1);
  append_string(stream, "root");
  stream << ": ";
  append_raw_dwarf_die(stream, compile_unit.root, level + 1);
  stream << '\n';
  append_indent(stream, level);
  stream << '}';
}

void append_project_model(std::ostringstream& stream,
                          const ProjectModel& model,
                          const int level,
                          const bool trailing_comma = true) {
  append_indent(stream, level);
  stream << "{\n";
  append_string_field(stream, "file", model.file, level + 1);
  append_indent(stream, level + 1);
  append_string(stream, "elf_info");
  stream << ": ";
  append_elf_file_info(stream, model.elf_info, level + 1);
  append_array(stream, "compile_units", model.compile_units.size(), level + 1, [&](const std::size_t index, const int item_level) {
    append_compile_unit_record(stream, model.compile_units[index], item_level);
  });
  append_array(stream, "types", model.types.size(), level + 1, [&](const std::size_t index, const int item_level) {
    append_type_node(stream, model.types[index], item_level);
  });
  append_array(stream, "symbols", model.symbols.size(), level + 1, [&](const std::size_t index, const int item_level) {
    append_variable_record(stream, model.symbols[index], item_level);
  });
  append_array(stream, "expanded", model.expanded.size(), level + 1, [&](const std::size_t index, const int item_level) {
    append_expanded_node(stream, model.expanded[index], item_level);
  });
  append_indent(stream, level + 1);
  append_string(stream, "metrics");
  stream << ": {\n";
  append_number_field(stream, "dwarf_load_ms", model.metrics.dwarf_load_ms, level + 2);
  append_number_field(stream, "symbol_table_ms", model.metrics.symbol_table_ms, level + 2);
  append_number_field(stream, "deduplicate_ms", model.metrics.deduplicate_ms, level + 2);
  append_number_field(stream, "expand_ms", model.metrics.expand_ms, level + 2);
  append_number_field(stream,
                      "variable_count_before_filter",
                      model.metrics.variable_count_before_filter,
                      level + 2);
  append_number_field(stream,
                      "variable_count_after_filter",
                      model.metrics.variable_count_after_filter,
                      level + 2);
  append_number_field(stream,
                      "skipped_compile_unit_count",
                      model.metrics.skipped_compile_unit_count,
                      level + 2,
                      false);
  append_indent(stream, level + 1);
  stream << "}\n";
  append_indent(stream, level);
  stream << '}';
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

AddressInfo parse_address_info(const YAML::Node& node) {
  AddressInfo info;
  info.kind = parse_address_kind(node["kind"].as<std::string>());
  info.absolute_address = parse_optional_number<std::uint64_t>(node, "absolute_address");
  info.relative_offset = parse_optional_number<std::int64_t>(node, "relative_offset");
  info.section_name = parse_optional_string(node, "section_name");
  info.bit_offset = parse_optional_number<std::uint64_t>(node, "bit_offset");
  info.bit_size = parse_optional_number<std::uint64_t>(node, "bit_size");
  info.location_entry_count = parse_optional_number<std::uint64_t>(node, "location_entry_count");
  if (const auto ranges = node["location_ranges"]; ranges && ranges.IsSequence()) {
    for (const auto& range_node : ranges) {
      AddressInfo::LocationRange range;
      range.raw_low_pc = parse_optional_number<std::uint64_t>(range_node, "raw_low_pc");
      range.raw_high_pc = parse_optional_number<std::uint64_t>(range_node, "raw_high_pc");
      range.cooked_low_pc = parse_optional_number<std::uint64_t>(range_node, "cooked_low_pc");
      range.cooked_high_pc = parse_optional_number<std::uint64_t>(range_node, "cooked_high_pc");
      const auto debug_addr_unavailable = range_node["debug_addr_unavailable"];
      range.debug_addr_unavailable =
        debug_addr_unavailable ? debug_addr_unavailable.as<bool>() : false;
      info.location_ranges.push_back(std::move(range));
    }
  }
  info.location_description = node["location_description"].as<std::string>();
  return info;
}

TypeRef parse_type_ref(const YAML::Node& node) {
  return {.id = node["id"].as<std::string>()};
}

TypeMember parse_type_member(const YAML::Node& node) {
  TypeMember member;
  member.name = node["name"].as<std::string>();
  member.type = parse_type_ref(node["type"]);
  member.address = parse_address_info(node["address"]);
  member.availability = parse_availability(node["availability"].as<std::string>());
  member.byte_size = parse_optional_number<std::uint64_t>(node, "byte_size");
  return member;
}

BaseRelation parse_base_relation(const YAML::Node& node) {
  BaseRelation relation;
  relation.type = parse_type_ref(node["type"]);
  relation.offset = node["offset"].as<std::uint64_t>();
  return relation;
}

TypeNode parse_type_node(const YAML::Node& node) {
  TypeNode type;
  type.id = node["id"].as<std::string>();
  type.kind = parse_type_kind(node["kind"].as<std::string>());
  type.name = node["name"].as<std::string>();
  type.byte_size = parse_optional_number<std::uint64_t>(node, "byte_size");
  type.alignment = parse_optional_number<std::uint64_t>(node, "alignment");
  if (node["qualified_of"] && !node["qualified_of"].IsNull()) {
    type.qualified_of = parse_type_ref(node["qualified_of"]);
  }
  if (node["aliased_of"] && !node["aliased_of"].IsNull()) {
    type.aliased_of = parse_type_ref(node["aliased_of"]);
  }
  if (node["pointee_type"] && !node["pointee_type"].IsNull()) {
    type.pointee_type = parse_type_ref(node["pointee_type"]);
  }
  if (node["element_type"] && !node["element_type"].IsNull()) {
    type.element_type = parse_type_ref(node["element_type"]);
  }
  for (const auto& item : node["array_dimensions"]) {
    type.array_dimensions.push_back(item.as<std::uint64_t>());
  }
  for (const auto& item : node["members"]) {
    type.members.push_back(parse_type_member(item));
  }
  for (const auto& item : node["bases"]) {
    type.bases.push_back(parse_base_relation(item));
  }
  for (const auto& item : node["enum_values"]) {
    type.enum_values.push_back(item.as<std::string>());
  }
  return type;
}

VariableRecord parse_variable_record(const YAML::Node& node) {
  VariableRecord record;
  record.id = node["id"].as<std::string>();
  record.name = node["name"].as<std::string>();
  record.compile_unit_name = node["compile_unit_name"].as<std::string>();
  record.linkage_name = parse_optional_string(node, "linkage_name");
  if (node["kind"]) {
    record.variable_kind = parse_variable_kind(node["kind"].as<std::string>());
  } else {
    record.variable_kind = parse_variable_kind(node["variable_kind"].as<std::string>());
  }
  record.availability = parse_availability(node["availability"].as<std::string>());
  record.address = parse_address_info(node["address"]);
  record.type = parse_type_ref(node["type"]);
  for (const auto& item : node["scope_path"]) {
    record.scope_path.push_back(item.as<std::string>());
  }
  record.byte_size = parse_optional_number<std::uint64_t>(node, "byte_size");
  record.const_value = parse_optional_number<std::int64_t>(node, "const_value");
  record.const_value_text = parse_optional_string(node, "const_value_text");
  record.is_thread_local = node["is_thread_local"].as<bool>();
  record.has_static_storage = node["has_static_storage"].as<bool>();
  return record;
}

CompileUnitRecord parse_compile_unit_record(const YAML::Node& node) {
  CompileUnitRecord unit;
  unit.id = node["id"].as<std::string>();
  unit.name = node["name"].as<std::string>();
  unit.producer = node["producer"].as<std::string>();
  unit.language = node["language"].as<std::string>();
  if (const auto address = node["address"]; address) {
    unit.address = parse_address_info(address);
  }
  return unit;
}

ElfFileInfo parse_elf_file_info(const YAML::Node& node) {
  ElfFileInfo info;
  if (!node) {
    return info;
  }
  info.object_class = node["object_class"].as<std::string>();
  info.byte_order = node["byte_order"].as<std::string>();
  info.file_type = node["file_type"].as<std::string>();
  info.machine = node["machine"].as<std::string>();
  info.os_abi = node["os_abi"].as<std::string>();
  return info;
}

ExpandedNode parse_expanded_node(const YAML::Node& node) {
  ExpandedNode expanded;
  expanded.path = node["path"].as<std::string>();
  expanded.display_name = node["display_name"].as<std::string>();
  expanded.type_name = node["type_name"].as<std::string>();
  expanded.type_id = node["type_id"] ? node["type_id"].as<std::string>() : std::string {};
  expanded.type_kind = parse_type_kind(node["type_kind"].as<std::string>());
  expanded.availability = parse_availability(node["availability"].as<std::string>());
  expanded.absolute_address = parse_optional_number<std::uint64_t>(node, "absolute_address");
  expanded.relative_offset = parse_optional_number<std::int64_t>(node, "relative_offset");
  expanded.byte_size = parse_optional_number<std::uint64_t>(node, "byte_size");
  expanded.array_count = parse_optional_number<std::uint64_t>(node, "array_count");
  expanded.array_stride = parse_optional_number<std::uint64_t>(node, "array_stride");
  expanded.depth = parse_optional_number<std::size_t>(node, "depth").value_or(0);
  expanded.children_lazy = node["children_lazy"] ? node["children_lazy"].as<bool>() : false;
  for (const auto& child : node["children"]) {
    expanded.children.push_back(parse_expanded_node(child));
  }
  return expanded;
}

ProjectModel parse_project_model(const YAML::Node& node) {
  ProjectModel model;
  model.file = node["file"].as<std::string>();
  model.elf_info = parse_elf_file_info(node["elf_info"]);
  for (const auto& item : node["compile_units"]) {
    model.compile_units.push_back(parse_compile_unit_record(item));
  }
  for (const auto& item : node["types"]) {
    model.types.push_back(parse_type_node(item));
  }
  for (const auto& item : node["symbols"]) {
    model.symbols.push_back(parse_variable_record(item));
  }
  for (const auto& item : node["expanded"]) {
    model.expanded.push_back(parse_expanded_node(item));
  }
  if (const auto metrics = node["metrics"]; metrics) {
    model.metrics.dwarf_load_ms = parse_optional_number<std::uint64_t>(metrics, "dwarf_load_ms").value_or(0);
    model.metrics.symbol_table_ms =
      parse_optional_number<std::uint64_t>(metrics, "symbol_table_ms").value_or(0);
    model.metrics.deduplicate_ms =
      parse_optional_number<std::uint64_t>(metrics, "deduplicate_ms").value_or(0);
    model.metrics.expand_ms = parse_optional_number<std::uint64_t>(metrics, "expand_ms").value_or(0);
    model.metrics.variable_count_before_filter =
      parse_optional_number<std::size_t>(metrics, "variable_count_before_filter").value_or(0);
    model.metrics.variable_count_after_filter =
      parse_optional_number<std::size_t>(metrics, "variable_count_after_filter").value_or(0);
    model.metrics.skipped_compile_unit_count =
      parse_optional_number<std::size_t>(metrics, "skipped_compile_unit_count").value_or(0);
  }
  return model;
}

}  // namespace

std::string render_dump_json(const ProjectModel& model) {
  std::ostringstream stream;
  append_project_model(stream, model, 0, false);
  return stream.str();
}

ProjectModel parse_dump_json(const std::string& json_text) {
  return parse_project_model(YAML::Load(json_text));
}

ProjectSnapshot build_export_snapshot(const ProjectSnapshot& snapshot,
                                     const SnapshotExportOptions& options) {
  ProjectSnapshot exported = snapshot;
  if (options.include_sensitive_info) {
    return exported;
  }

  // 仅隐藏路径和编译器指纹等敏感文本，保留地址、类型和展开树，避免影响离线地址分析。
  exported.source_file = "<redacted>";
  exported.model.file = "<redacted>";
  for (auto& unit : exported.model.compile_units) {
    unit.name.clear();
    unit.producer.clear();
  }
  for (auto& symbol : exported.model.symbols) {
    symbol.compile_unit_name.clear();
  }
  return exported;
}

std::string render_snapshot_json(const ProjectSnapshot& snapshot,
                                 const SnapshotExportOptions& options) {
  const auto exported = build_export_snapshot(snapshot, options);
  std::ostringstream stream;
  stream << "{\n";
  append_number_field(stream, "schema_version", exported.schema_version, 1);
  append_string_field(stream, "source_kind", exported.source_kind, 1);
  append_string_field(stream, "source_file", exported.source_file, 1);
  append_string_field(stream, "exported_at", exported.exported_at, 1);
  append_indent(stream, 1);
  append_string(stream, "model");
  stream << ": ";
  append_project_model(stream, exported.model, 1, false);
  stream << "}\n";
  return stream.str();
}

std::string render_raw_dwarf_json(const RawDwarfDocument& document) {
  std::ostringstream stream;
  stream << "{\n";
  append_number_field(stream, "schema_version", document.schema_version, 1);
  append_string_field(stream, "source_file", document.source_file, 1);
  append_string_field(stream, "exported_at", document.exported_at, 1);
  append_string_field(stream, "status", document.status, 1);
  append_array(stream,
               "compile_units",
               document.compile_units.size(),
               1,
               [&](const std::size_t index, const int item_level) {
                 append_raw_dwarf_compile_unit(stream, document.compile_units[index], item_level);
               });
  append_array(stream,
               "errors",
               document.errors.size(),
               1,
               [&](const std::size_t index, const int item_level) {
                 append_indent(stream, item_level);
                 append_string(stream, document.errors[index]);
               },
               false);
  stream << "}\n";
  return stream.str();
}

ProjectSnapshot parse_snapshot_json(const std::string& json_text) {
  const auto root = YAML::Load(json_text);
  ProjectSnapshot snapshot;
  snapshot.schema_version = root["schema_version"].as<std::uint64_t>();
  snapshot.source_kind = root["source_kind"].as<std::string>();
  snapshot.source_file = root["source_file"].as<std::string>();
  snapshot.exported_at = root["exported_at"].as<std::string>();
  snapshot.model = parse_project_model(root["model"]);
  return snapshot;
}

std::string render_expanded_node_json(const ExpandedNode& node) {
  std::ostringstream stream;
  append_expanded_node(stream, node, 0);
  return stream.str();
}

}  // namespace elf_static_view
