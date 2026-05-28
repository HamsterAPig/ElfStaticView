#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace elf_static_view {

enum class AddressKind {
  Absolute,
  SectionRelative,
  MemberOffset,
  ArrayElementOffset,
  BitField,
  Unknown,
};

enum class Availability {
  StaticAddressKnown,
  StaticLayoutKnown,
  RuntimeOnly,
  Unavailable,
  OptimizedOut,
};

enum class TypeKind {
  Base,
  Pointer,
  Reference,
  MemberPointer,
  Typedef,
  Qualified,
  Array,
  Struct,
  Class,
  Union,
  Enum,
  Subroutine,
  Atomic,
  Unspecified,
  Unknown,
};

enum class VariableKind {
  Global,
  Namespace,
  FileStatic,
  FunctionStatic,
  StaticMember,
  Local,
  Parameter,
  ThreadLocal,
  Unknown,
};

struct AddressInfo {
  struct LocationRange {
    std::optional<std::uint64_t> raw_low_pc;
    std::optional<std::uint64_t> raw_high_pc;
    std::optional<std::uint64_t> cooked_low_pc;
    std::optional<std::uint64_t> cooked_high_pc;
    bool debug_addr_unavailable = false;
  };

  AddressKind kind = AddressKind::Unknown;
  std::optional<std::uint64_t> absolute_address;
  std::optional<std::int64_t> relative_offset;
  std::optional<std::string> section_name;
  std::optional<std::uint64_t> bit_offset;
  std::optional<std::uint64_t> bit_size;
  std::string location_description;
  std::optional<std::uint64_t> location_entry_count;
  std::vector<LocationRange> location_ranges;
};

struct TypeRef {
  std::string id;
};

struct BaseRelation {
  TypeRef type;
  std::uint64_t offset = 0;
};

struct TypeMember {
  std::string name;
  TypeRef type;
  AddressInfo address;
  Availability availability = Availability::Unavailable;
  std::optional<std::uint64_t> byte_size;
};

struct TypeNode {
  std::string id;
  TypeKind kind = TypeKind::Unknown;
  std::string name;
  std::optional<std::uint64_t> byte_size;
  std::optional<std::uint64_t> alignment;
  std::optional<TypeRef> qualified_of;
  std::optional<TypeRef> aliased_of;
  std::optional<TypeRef> pointee_type;
  std::optional<TypeRef> element_type;
  std::vector<std::uint64_t> array_dimensions;
  std::vector<TypeMember> members;
  std::vector<BaseRelation> bases;
  std::vector<std::string> enum_values;
};

struct VariableRecord {
  std::string id;
  std::string name;
  std::string compile_unit_name;
  std::optional<std::string> linkage_name;
  VariableKind variable_kind = VariableKind::Unknown;
  Availability availability = Availability::Unavailable;
  AddressInfo address;
  TypeRef type;
  std::vector<std::string> scope_path;
  std::optional<std::uint64_t> byte_size;
  std::optional<std::int64_t> const_value;
  std::optional<std::string> const_value_text;
  bool is_thread_local = false;
  bool has_static_storage = false;
};

struct CompileUnitRecord {
  std::string id;
  std::string name;
  std::string producer;
  std::string language;
  std::string source_path;
  AddressInfo address;
};

struct ElfFileInfo {
  std::string object_class;
  std::string byte_order;
  std::string file_type;
  std::string machine;
  std::string os_abi;
};

struct ExpandedNode {
  std::string path;
  std::string display_name;
  std::string type_name;
  std::string type_id;
  TypeKind type_kind = TypeKind::Unknown;
  Availability availability = Availability::Unavailable;
  std::optional<std::uint64_t> absolute_address;
  std::optional<std::int64_t> relative_offset;
  std::optional<std::uint64_t> byte_size;
  std::optional<std::uint64_t> array_count;
  std::optional<std::uint64_t> array_stride;
  std::size_t depth = 0;
  bool children_lazy = false;
  std::vector<ExpandedNode> children;
};

struct ProjectSummary {
  std::size_t compile_unit_count = 0;
  std::size_t type_count = 0;
  std::size_t symbol_count = 0;
  std::size_t static_address_known_count = 0;
  std::size_t runtime_only_count = 0;
  std::size_t unavailable_count = 0;
};

struct ParseMetrics {
  std::uint64_t dwarf_load_ms = 0;
  std::uint64_t symbol_table_ms = 0;
  std::uint64_t deduplicate_ms = 0;
  std::uint64_t expand_ms = 0;
  std::size_t variable_count_before_filter = 0;
  std::size_t variable_count_after_filter = 0;
  std::size_t skipped_compile_unit_count = 0;
};

struct ProjectModel {
  std::string file;
  ElfFileInfo elf_info;
  std::vector<CompileUnitRecord> compile_units;
  std::vector<TypeNode> types;
  std::vector<VariableRecord> symbols;
  std::vector<ExpandedNode> expanded;
  ParseMetrics metrics;
};

struct LoadPolicy {
  bool static_storage_only = true;
  bool exclude_formal_parameters = true;
  bool exclude_runtime_only_variables = true;
  std::string compile_unit_path_rules_text;
  std::size_t expand_depth = 6;
  bool lazy_expand_children = true;
  bool enable_parse_metrics = true;
};

struct ProjectSnapshot {
  std::uint64_t schema_version = 1;
  std::string source_kind = "elf-static-view";
  std::string source_file;
  std::string exported_at;
  ProjectModel model;
};

struct RawDwarfAttribute {
  std::string name;
  std::string form;
  std::string value;
};

struct RawDwarfDie {
  std::uint64_t offset = 0;
  std::string tag;
  std::string name;
  std::vector<RawDwarfAttribute> attributes;
  std::vector<RawDwarfDie> children;
};

struct RawDwarfCompileUnit {
  std::size_t index = 0;
  std::uint16_t version = 0;
  std::uint64_t header_length = 0;
  std::uint64_t abbrev_offset = 0;
  std::uint16_t address_size = 0;
  std::uint16_t length_size = 0;
  std::uint16_t extension_size = 0;
  std::uint64_t next_header_offset = 0;
  std::string unit_type;
  RawDwarfDie root;
};

struct RawDwarfDocument {
  std::uint64_t schema_version = 1;
  std::string source_file;
  std::string exported_at;
  std::string status = "ok";
  std::vector<RawDwarfCompileUnit> compile_units;
  std::vector<std::string> errors;
};

struct ScanOptions {
  bool include_runtime_only = false;
  LoadPolicy load_policy {};
};

struct DumpOptions {
  bool include_runtime_only = false;
  bool only_static_known = false;
  std::optional<std::string> symbol_name;
  std::size_t expand_depth = 8;
  LoadPolicy load_policy {};
};

struct StaticAddressQueryOptions {
  std::string name_query_text;
  std::string path_rules_text;
  bool include_runtime_only = false;
  bool only_static_known = true;
  std::size_t max_array_elements = 1024;
};

struct StaticAddressResult {
  std::string key;
  std::uint64_t value = 0;
  std::string value_type;
};

struct SnapshotExportOptions {
  bool include_sensitive_info = true;
};

std::string to_string(AddressKind value);
std::string to_string(Availability value);
std::string to_string(TypeKind value);
std::string to_string(VariableKind value);

}  // namespace elf_static_view
