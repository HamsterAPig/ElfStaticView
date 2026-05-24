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
  Typedef,
  Qualified,
  Array,
  Struct,
  Class,
  Union,
  Enum,
  Subroutine,
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
  AddressKind kind = AddressKind::Unknown;
  std::optional<std::uint64_t> absolute_address;
  std::optional<std::int64_t> relative_offset;
  std::optional<std::string> section_name;
  std::optional<std::uint64_t> bit_offset;
  std::optional<std::uint64_t> bit_size;
  std::string location_description;
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
  bool is_thread_local = false;
  bool has_static_storage = false;
};

struct CompileUnitRecord {
  std::string id;
  std::string name;
  std::string producer;
  std::string language;
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
  TypeKind type_kind = TypeKind::Unknown;
  Availability availability = Availability::Unavailable;
  std::optional<std::uint64_t> absolute_address;
  std::optional<std::int64_t> relative_offset;
  std::optional<std::uint64_t> byte_size;
  std::optional<std::uint64_t> array_count;
  std::optional<std::uint64_t> array_stride;
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

struct ProjectModel {
  std::string file;
  ElfFileInfo elf_info;
  std::vector<CompileUnitRecord> compile_units;
  std::vector<TypeNode> types;
  std::vector<VariableRecord> symbols;
  std::vector<ExpandedNode> expanded;
};

struct ProjectSnapshot {
  std::uint64_t schema_version = 1;
  std::string source_kind = "elf-static-view";
  std::string source_file;
  std::string exported_at;
  ProjectModel model;
};

struct ScanOptions {
  bool include_runtime_only = false;
};

struct DumpOptions {
  bool include_runtime_only = false;
  bool only_static_known = false;
  std::optional<std::string> symbol_name;
  std::size_t expand_depth = 8;
};

struct SnapshotExportOptions {
  bool include_sensitive_info = true;
};

std::string to_string(AddressKind value);
std::string to_string(Availability value);
std::string to_string(TypeKind value);
std::string to_string(VariableKind value);

}  // namespace elf_static_view
