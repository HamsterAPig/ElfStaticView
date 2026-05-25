#include "analysis/address_bias.hpp"
#include "elf/dwarf_wrappers.hpp"
#include "elf/elf_symbol_table.hpp"
#include "elf_static_view/project.hpp"
#include "platform/utf8.hpp"
#include "ui/filter_matcher.hpp"
#include "ui/version_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#ifndef ELF_STATIC_VIEW_DWARFDUMP_PATH
#error "ELF_STATIC_VIEW_DWARFDUMP_PATH 未定义"
#endif

void expect_true(bool condition, const std::string& message);

// 统一走构建产物中的 dwarfdump，避免依赖开发机本地安装路径。
std::string run_dwarfdump_to_temp(const std::string& dwarfdump_args,
                                  const std::string& fixture_path,
                                  const std::string& output_tag,
                                  const std::string& label) {
  const auto output_path =
    std::filesystem::temp_directory_path() /
    ("elf-static-view-" + output_tag + "-" +
     std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())) +
     ".txt");
  const auto command =
    std::string("\"") + ELF_STATIC_VIEW_DWARFDUMP_PATH + "\" " + dwarfdump_args + " \"" + fixture_path +
    "\" > \"" + output_path.string() + "\"";
  const int rc = std::system(command.c_str());
  expect_true(rc == 0, label + " 的 dwarfdump 应执行成功");
  return output_path.string();
}

std::string read_all(const std::string& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("无法打开文件: " + path);
  }
  std::ostringstream stream;
  stream << input.rdbuf();
  return stream.str();
}

void expect_true(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error("断言失败: " + message);
  }
}

void expect_contains(const std::string& content, const std::string& needle, const std::string& message) {
  if (content.find(needle) == std::string::npos) {
    throw std::runtime_error("断言失败: " + message + "，缺少片段: " + needle);
  }
}

std::vector<const elf_static_view::VariableRecord*> find_file_static_variables(
  const elf_static_view::ProjectModel& model,
  const std::string& name) {
  std::vector<const elf_static_view::VariableRecord*> matches;
  for (const auto& symbol : model.symbols) {
    if (symbol.name == name && symbol.variable_kind == elf_static_view::VariableKind::FileStatic) {
      matches.push_back(&symbol);
    }
  }
  return matches;
}

std::vector<const elf_static_view::VariableRecord*> find_variables_by_kind(
  const elf_static_view::ProjectModel& model,
  const std::string& name,
  const elf_static_view::VariableKind kind) {
  std::vector<const elf_static_view::VariableRecord*> matches;
  for (const auto& symbol : model.symbols) {
    if (symbol.name == name && symbol.variable_kind == kind) {
      matches.push_back(&symbol);
    }
  }
  return matches;
}

std::vector<const elf_static_view::VariableRecord*> find_variables_by_name(
  const elf_static_view::ProjectModel& model,
  const std::string& name) {
  std::vector<const elf_static_view::VariableRecord*> matches;
  for (const auto& symbol : model.symbols) {
    if (symbol.name == name) {
      matches.push_back(&symbol);
    }
  }
  return matches;
}

void verify_text_presence_via_dwarfdump(const std::string& fixture_path,
                                        const std::string& dwarfdump_kind,
                                        const std::string& expected_text,
                                        const std::string& label);

const elf_static_view::TypeNode* find_type_by_name(const elf_static_view::ProjectModel& model,
                                                   const std::string& name) {
  for (const auto& type : model.types) {
    if (type.name == name) {
      return &type;
    }
  }
  return nullptr;
}

void verify_fixture(const std::string& fixture_path, const std::string& expected_json_path) {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(fixture_path,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  const auto output = elf_static_view::render_dump_json(model);
  const auto expected = read_all(expected_json_path);
  std::istringstream lines(expected);
  std::string line;
  while (std::getline(lines, line)) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    expect_contains(output, line, expected_json_path);
  }
}

void verify_json_round_trip(const std::string& fixture_path) {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(fixture_path,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  const auto json = elf_static_view::render_dump_json(model);
  const auto parsed = elf_static_view::parse_dump_json(json);
  expect_true(parsed.file == model.file, "JSON 往返后文件路径应保持一致");
  expect_true(parsed.expanded.size() == model.expanded.size(), "JSON 往返后展开节点数量应保持一致");
}

void verify_dump_text_contains_elf_info(const std::string& fixture_path) {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(fixture_path,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "elf_class: ELF64", "dump 文本应包含 ELF class");
  expect_contains(output, "byte_order: LittleEndian", "dump 文本应包含大小端");
  expect_contains(output, "file_type: DYN", "dump 文本应包含文件类型");
  expect_contains(output, "machine: x86_64", "dump 文本应包含 machine");
  expect_contains(output, "os_abi: SystemV", "dump 文本应包含 OS ABI");
}

void verify_dump_text_contains_elf_info_any_class(const std::string& fixture_path,
                                                  const std::string& expected_class,
                                                  const std::string& expected_byte_order) {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(fixture_path,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "elf_class: " + expected_class, "dump 文本应包含目标 ELF class");
  expect_contains(output, "byte_order: " + expected_byte_order, "dump 文本应包含目标大小端");
  expect_contains(output, "file_type: ", "dump 文本应包含文件类型");
  expect_contains(output, "machine: ", "dump 文本应包含 machine");
  expect_contains(output, "os_abi: ", "dump 文本应包含 OS ABI");
}

void verify_bitfield_layout_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_C_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto* bitfield_type = find_type_by_name(model, "BitFieldStruct");
  expect_true(bitfield_type != nullptr, "C fixture 应包含 BitFieldStruct");
  expect_true(bitfield_type->members.size() == 3, "BitFieldStruct 应保留 3 个 bitfield 成员");

  const auto& enabled = bitfield_type->members[0];
  expect_true(enabled.name == "enabled", "第一个 bitfield 成员应为 enabled");
  expect_true(enabled.address.kind == elf_static_view::AddressKind::BitField,
              "enabled 应标记为 BitField");
  expect_true(enabled.address.bit_size.has_value() && enabled.address.bit_size.value() == 1,
              "enabled bit_size 应为 1");

  const auto& mode = bitfield_type->members[1];
  expect_true(mode.name == "mode", "第二个 bitfield 成员应为 mode");
  expect_true(mode.address.kind == elf_static_view::AddressKind::BitField,
              "mode 应标记为 BitField");
  expect_true(mode.address.bit_size.has_value() && mode.address.bit_size.value() == 3,
              "mode bit_size 应为 3");

  const auto& reserved = bitfield_type->members[2];
  expect_true(reserved.name == "reserved", "第三个 bitfield 成员应为 reserved");
  expect_true(reserved.address.kind == elf_static_view::AddressKind::BitField,
              "reserved 应标记为 BitField");
  expect_true(reserved.address.bit_size.has_value() && reserved.address.bit_size.value() == 4,
              "reserved bit_size 应为 4");

  const auto globals = find_variables_by_name(model, "global_value");
  expect_true(globals.size() == 1, "C fixture 应保留 global_value");
  expect_true(globals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
              "global_value 应解析成静态地址已知");
  expect_true(globals.front()->address.absolute_address.has_value(),
              "global_value 应解析出绝对地址");
  expect_true(globals.front()->address.location_description == "DW_OP_addrx",
              "global_value 应保留 DW_OP_addrx 位置描述");
}

void verify_member_pointer_type_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_CPP_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto* member_pointer_type = find_type_by_name(model, "MemberPointer");
  expect_true(member_pointer_type != nullptr, "C++ fixture 应包含 MemberPointer");
  expect_true(member_pointer_type->kind == elf_static_view::TypeKind::Typedef,
              "MemberPointer typedef 应保留 Typedef kind");
  expect_true(member_pointer_type->aliased_of.has_value(), "MemberPointer 应保留别名目标");

  const auto aliased_id = member_pointer_type->aliased_of->id;
  const auto member_pointer_iter =
    std::find_if(model.types.begin(), model.types.end(), [&](const elf_static_view::TypeNode& type) {
      return type.id == aliased_id;
    });
  expect_true(member_pointer_iter != model.types.end(), "MemberPointer 别名目标类型应存在");
  expect_true(member_pointer_iter->kind == elf_static_view::TypeKind::MemberPointer,
              "MemberPointer 别名目标应识别为 MemberPointer");

  const auto globals = find_variables_by_name(model, "global_item_member_ptr");
  expect_true(globals.size() == 1, "应解析出 global_item_member_ptr");
}

void verify_gnu_addr_index_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_GNU_ADDR_INDEX_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto globals = find_variables_by_name(model, "global_value");
  expect_true(globals.size() == 1, "GNU addr index fixture 应保留 global_value");
  expect_true(globals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
              "GNU addr index fixture 的 global_value 应解析成静态地址已知");
  expect_true(globals.front()->address.absolute_address.has_value(),
              "GNU addr index fixture 的 global_value 应解析出绝对地址");
  expect_true(globals.front()->address.location_description == "DW_OP_GNU_addr_index",
              "global_value 应保留 DW_OP_GNU_addr_index 位置描述");
}

void verify_ref_sig8_debug_types_fixture() {
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_REF_SIG8_DEBUG_TYPES_FIXTURE_PATH,
                                     "--debug-abbrev",
                                     "DW_AT_type\tDW_FORM_ref_sig8",
                                     "ref_sig8 fixture");
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_REF_SIG8_DEBUG_TYPES_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto globals = find_variables_by_name(model, "global_value");
  expect_true(globals.size() == 1, "ref_sig8 fixture 应保留 global_value");
  expect_true(globals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
              "ref_sig8 fixture 的 global_value 应解析成静态地址已知");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "global_value [StaticAddressKnown] RefTarget",
                  "ref_sig8 fixture 应把 global_value 解析回 RefTarget");
  expect_contains(output, "consume::local [RuntimeOnly] RefTarget",
                  "ref_sig8 fixture 应把 local 也解析回 RefTarget");
}

void verify_ref_sig8_indirect_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_REF_SIG8_DEBUG_TYPES_INDIRECT_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "global_value [StaticAddressKnown] RefTarget",
                  "indirect debug_types fixture 应保留 RefTarget 名称");
  expect_contains(output, "consume::local [RuntimeOnly] RefTarget",
                  "indirect debug_types fixture 应保留 local 的 RefTarget");
  expect_contains(output, "global_value.left [StaticLayoutKnown] int",
                  "indirect debug_types fixture 应保留 left 的 int 类型");
  expect_contains(output, "global_value.right [StaticLayoutKnown] int",
                  "indirect debug_types fixture 应保留 right 的 int 类型");
}

void verify_gcc_ref_sup4_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_GCC_DWARF5_REF_SUP4_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "sup_value [Unavailable] SupTarget",
                  "ref_sup4 fixture 应保留 sup_value 的 SupTarget 类型");
  expect_contains(output, "sup_value.value [StaticLayoutKnown] int",
                  "ref_sup4 fixture 应继续解析成员 int 类型");
}

void verify_gcc_ref_addr_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_GCC_DWARF5_REF_ADDR_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "sup_value [Unavailable] SupTarget",
                  "ref_addr fixture 应保留 sup_value 的 SupTarget 类型");
  expect_contains(output, "sup_value.value [StaticLayoutKnown] int",
                  "ref_addr fixture 应继续解析成员 int 类型");
}

void verify_gcc_small_ref_fixture(const std::string& fixture_path, const std::string& form_name) {
  verify_text_presence_via_dwarfdump(fixture_path,
                                     "--debug-abbrev",
                                     "DW_AT_type\tDW_FORM_" + form_name,
                                     form_name + " fixture");
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(fixture_path,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "sup_value [StaticAddressKnown] SupTarget",
                  form_name + " fixture 应保留 sup_value 的 SupTarget 类型");
  expect_contains(output, "sup_value.value [StaticLayoutKnown] int",
                  form_name + " fixture 应继续解析成员 int 类型");
}

void verify_gcc_ref_sup8_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_GCC_DWARF64_REF_SUP8_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "sup_value [Unavailable] SupTarget",
                  "ref_sup8 fixture 应保留 sup_value 的 SupTarget 类型");
  expect_contains(output, "sup_value.value [StaticLayoutKnown] int",
                  "ref_sup8 fixture 应继续解析成员 int 类型");
}

void verify_gcc_ref8_fixture() {
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_GCC_DWARF64_REF8_FIXTURE_PATH,
                                     "--debug-abbrev",
                                     "DW_AT_type\tDW_FORM_ref8",
                                     "ref8 fixture");

  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_GCC_DWARF64_REF8_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "sup_value [StaticAddressKnown] SupTarget",
                  "ref8 fixture 应保留 sup_value 的 SupTarget 类型");
  expect_contains(output, "sup_value.value [StaticLayoutKnown] int",
                  "ref8 fixture 应继续解析成员 int 类型");
}

void verify_gcc_gnu_alt_fixture() {
  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_GCC_GNU_ALT_FIXTURE_PATH);
  Dwarf_Debug debug = debug_handle.get();

  Dwarf_Unsigned cu_header_length = 0;
  Dwarf_Half version_stamp = 0;
  Dwarf_Off abbrev_offset = 0;
  Dwarf_Half address_size = 0;
  Dwarf_Half length_size = 0;
  Dwarf_Half extension_size = 0;
  Dwarf_Sig8 type_signature {};
  Dwarf_Unsigned type_offset = 0;
  Dwarf_Unsigned next_cu_header_offset = 0;
  Dwarf_Half header_cu_type = 0;
  Dwarf_Error error = nullptr;

  expect_true(
    dwarf_next_cu_header_d(debug,
                           true,
                           &cu_header_length,
                           &version_stamp,
                           &abbrev_offset,
                           &address_size,
                           &length_size,
                           &extension_size,
                           &type_signature,
                           &type_offset,
                           &next_cu_header_offset,
                           &header_cu_type,
                           &error) == DW_DLV_OK,
    "第一个 gcc gnu alt fixture CU 应可读取");
  expect_true(
    dwarf_next_cu_header_d(debug,
                           true,
                           &cu_header_length,
                           &version_stamp,
                           &abbrev_offset,
                           &address_size,
                           &length_size,
                           &extension_size,
                           &type_signature,
                           &type_offset,
                           &next_cu_header_offset,
                           &header_cu_type,
                           &error) == DW_DLV_OK,
    "第二个 gcc gnu alt fixture CU 应可读取");

  Dwarf_Die cu_die = nullptr;
  expect_true(dwarf_siblingof_b(debug, nullptr, true, &cu_die, &error) == DW_DLV_OK,
              "应能拿到当前 gcc gnu alt fixture 的第二个 CU DIE");

  auto name_attr = elf_static_view::elf::attribute_of(debug, cu_die, DW_AT_name);
  expect_true(name_attr.has_value(), "第二个 GNU alt CU 应存在 DW_AT_name");
  const auto name = elf_static_view::elf::string_attr(name_attr->get());
  expect_true(name.has_value(), "DW_FORM_GNU_strp_alt 的 name 应可被读取");
  expect_true(name->find("cygwin.S") != std::string::npos,
              "DW_FORM_GNU_strp_alt 的 name 应回到 alternate string");

  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_GCC_GNU_ALT_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "sup_value [StaticAddressKnown] SupTarget",
                  "GNU_ref_alt fixture 应保留 sup_value 的 SupTarget 类型");
  expect_contains(output, "sup_value.value [StaticLayoutKnown] int",
                  "GNU_ref_alt fixture 应继续解析成员 int 类型");
}

void verify_gcc_line_strp_fixture() {
  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_GCC_DWARF5_STRP_FIXTURE_PATH);
  Dwarf_Debug debug = debug_handle.get();

  Dwarf_Unsigned cu_header_length = 0;
  Dwarf_Half version_stamp = 0;
  Dwarf_Off abbrev_offset = 0;
  Dwarf_Half address_size = 0;
  Dwarf_Half length_size = 0;
  Dwarf_Half extension_size = 0;
  Dwarf_Sig8 type_signature {};
  Dwarf_Unsigned type_offset = 0;
  Dwarf_Unsigned next_cu_header_offset = 0;
  Dwarf_Half header_cu_type = 0;
  Dwarf_Error error = nullptr;

  expect_true(
    dwarf_next_cu_header_d(debug,
                           true,
                           &cu_header_length,
                           &version_stamp,
                           &abbrev_offset,
                           &address_size,
                           &length_size,
                           &extension_size,
                           &type_signature,
                           &type_offset,
                           &next_cu_header_offset,
                           &header_cu_type,
                           &error) == DW_DLV_OK,
    "第一个 gcc dwarf5 strp fixture CU 应可读取");

  Dwarf_Die cu_die = nullptr;
  expect_true(dwarf_siblingof_b(debug, nullptr, true, &cu_die, &error) == DW_DLV_OK,
              "应能拿到当前 gcc dwarf5 strp fixture 的第一个 CU DIE");

  auto name_attr = elf_static_view::elf::attribute_of(debug, cu_die, DW_AT_name);
  expect_true(name_attr.has_value(), "第一个 CU 应存在 DW_AT_name");
  const auto name = elf_static_view::elf::string_attr(name_attr->get());
  expect_true(name.has_value(), "DW_FORM_line_strp 的 name 应可被读取");
  expect_true(name->find("debug_sup_minimal.cpp") != std::string::npos,
              "DW_FORM_line_strp 的 name 应回到源码文件名");

  auto comp_dir_attr = elf_static_view::elf::attribute_of(debug, cu_die, DW_AT_comp_dir);
  expect_true(comp_dir_attr.has_value(), "第一个 CU 应存在 DW_AT_comp_dir");
  const auto comp_dir = elf_static_view::elf::string_attr(comp_dir_attr->get());
  expect_true(comp_dir.has_value(), "DW_FORM_line_strp 的 comp_dir 应可被读取");
  expect_true(comp_dir->find("build_mingw") != std::string::npos,
              "DW_FORM_line_strp 的 comp_dir 应回到构建目录");
}

void verify_dwarf5_strx1_fixture() {
  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_DWARF5_STRX1_FIXTURE_PATH);
  Dwarf_Debug debug = debug_handle.get();

  Dwarf_Unsigned cu_header_length = 0;
  Dwarf_Half version_stamp = 0;
  Dwarf_Off abbrev_offset = 0;
  Dwarf_Half address_size = 0;
  Dwarf_Half length_size = 0;
  Dwarf_Half extension_size = 0;
  Dwarf_Sig8 type_signature {};
  Dwarf_Unsigned type_offset = 0;
  Dwarf_Unsigned next_cu_header_offset = 0;
  Dwarf_Half header_cu_type = 0;
  Dwarf_Error error = nullptr;

  expect_true(
    dwarf_next_cu_header_d(debug,
                           true,
                           &cu_header_length,
                           &version_stamp,
                           &abbrev_offset,
                           &address_size,
                           &length_size,
                           &extension_size,
                           &type_signature,
                           &type_offset,
                           &next_cu_header_offset,
                           &header_cu_type,
                           &error) == DW_DLV_OK,
    "dwarf5 strx1 fixture 的第一个 unit 应可读取");

  Dwarf_Die root_die = nullptr;
  expect_true(dwarf_siblingof_b(debug, nullptr, true, &root_die, &error) == DW_DLV_OK,
              "应能拿到 dwarf5 strx1 fixture 的根 DIE");

  auto named_type = elf_static_view::elf::child_of(debug, root_die);
  expect_true(named_type.has_value(), "dwarf5 strx1 fixture 应存在 type unit 的首个子 DIE");

  auto name_attr = elf_static_view::elf::attribute_of(debug, named_type->get(), DW_AT_name);
  expect_true(name_attr.has_value(), "dwarf5 strx1 fixture 的 structure_type 应存在 DW_AT_name");
  const auto name = elf_static_view::elf::string_attr(name_attr->get());
  expect_true(name.has_value(), "DW_FORM_strx1 的 name 应可被读取");
  expect_true(name->find("FancyName") != std::string::npos,
              "DW_FORM_strx1 的 name 应回到 FancyName");
}

void verify_dwarf5_strx_form_fixture(const std::string& fixture_path,
                                     const std::string& expected_form_name,
                                     const std::string& label) {
  const auto output_path =
    run_dwarfdump_to_temp("--debug-abbrev", fixture_path, "strx-form-" + expected_form_name, label);
  const auto abbrev_text = read_all(output_path);
  std::filesystem::remove(output_path);

  expect_contains(abbrev_text,
                  "DW_AT_name\t" + expected_form_name,
                  label + " 应在 type unit 的 abbrev 中显式出现目标 DW_FORM_strx*");

  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(fixture_path,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "global_name [StaticAddressKnown] FancyName",
                  label + " 应继续解析出 FancyName");
  expect_contains(output, "global_name.value [StaticLayoutKnown] int",
                  label + " 应继续解析出成员 int");
  expect_contains(output, "use_name::local [RuntimeOnly] FancyName",
                  label + " 应继续解析出 local 的 FancyName");
}

void verify_dwarf5_addrx_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DWARF5_ADDRX_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto globals = find_variables_by_name(model, "global_name");
  expect_true(globals.size() == 1, "dwarf5 addrx fixture 应保留 global_name");
  expect_true(globals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
              "dwarf5 addrx fixture 的 global_name 应解析成静态地址已知");
  expect_true(globals.front()->address.absolute_address.has_value(),
              "dwarf5 addrx fixture 的 global_name 应解析出绝对地址");
  expect_true(globals.front()->address.location_description == "DW_OP_addrx",
              "dwarf5 addrx fixture 应保留 DW_OP_addrx 位置描述");
}

void verify_dwarf5_addrx_form_fixture(const std::string& fixture_path,
                                      const Dwarf_Half expected_form,
                                      const std::string& label) {
  const auto output_path =
    run_dwarfdump_to_temp("--debug-abbrev", fixture_path, "addrx-form-" + std::to_string(expected_form), label);
  const auto abbrev_text = read_all(output_path);
  std::filesystem::remove(output_path);

  const auto expected_form_name = [&]() -> std::string {
    switch (expected_form) {
      case DW_FORM_addrx1:
        return "DW_FORM_addrx1";
      case DW_FORM_addrx2:
        return "DW_FORM_addrx2";
      case DW_FORM_addrx3:
        return "DW_FORM_addrx3";
      case DW_FORM_addrx4:
        return "DW_FORM_addrx4";
      default:
        return "DW_FORM_addrx";
    }
  }();
  expect_contains(abbrev_text,
                  "DW_AT_low_pc\t" + expected_form_name,
                  label + " 应在 abbrev 中显式出现目标 DW_FORM_addrx*");
  expect_true(expected_form == DW_FORM_addrx1 || expected_form == DW_FORM_addrx2 ||
                expected_form == DW_FORM_addrx3 || expected_form == DW_FORM_addrx4,
              label + " 仅应用于 DW_FORM_addrx1/2/3/4");

  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(fixture_path,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "global_name [StaticAddressKnown] FancyName",
                  label + " 应继续解析出 FancyName");

  const auto globals = find_variables_by_name(model, "global_name");
  expect_true(globals.size() == 1, label + " 应保留 global_name");
  expect_true(globals.front()->address.absolute_address.has_value(),
              label + " 的 global_name 应仍有绝对地址");
}

void verify_text_presence_via_dwarfdump(const std::string& fixture_path,
                                        const std::string& dwarfdump_kind,
                                        const std::string& expected_text,
                                        const std::string& label) {
  const auto output_path =
    run_dwarfdump_to_temp(dwarfdump_kind, fixture_path, "dwarfdump-presence", label);
  const auto content = read_all(output_path);
  std::filesystem::remove(output_path);
  expect_contains(content, expected_text, label + " 应显式包含目标文本");
}

void verify_rnglistx_fixture() {
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTX_FIXTURE_PATH,
                                     "--debug-rnglists",
                                     "offset_entry_count = 0x00000001",
                                     "rnglistx fixture");
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTX_FIXTURE_PATH,
                                     "--debug-abbrev",
                                     "DW_AT_ranges\tDW_FORM_rnglistx",
                                     "rnglistx fixture");

  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_RNGLISTX_FIXTURE_PATH);
  Dwarf_Debug debug = debug_handle.get();
  Dwarf_Unsigned cu_header_length = 0;
  Dwarf_Half version_stamp = 0;
  Dwarf_Off abbrev_offset = 0;
  Dwarf_Half address_size = 0;
  Dwarf_Half length_size = 0;
  Dwarf_Half extension_size = 0;
  Dwarf_Sig8 type_signature {};
  Dwarf_Unsigned type_offset = 0;
  Dwarf_Unsigned next_cu_header_offset = 0;
  Dwarf_Half header_cu_type = 0;
  Dwarf_Error error = nullptr;
  expect_true(
    dwarf_next_cu_header_d(debug,
                           true,
                           &cu_header_length,
                           &version_stamp,
                           &abbrev_offset,
                           &address_size,
                           &length_size,
                           &extension_size,
                           &type_signature,
                           &type_offset,
                           &next_cu_header_offset,
                           &header_cu_type,
                           &error) == DW_DLV_OK,
    "rnglistx fixture 的第一个 unit 应可读取");
  auto cu_die = elf_static_view::elf::die_from_offset(debug, 0x0c, true);
  expect_true(cu_die.has_value(), "rnglistx fixture 应能按已知 offset 拿到 compile unit 根 DIE");

  auto ranges_attr = elf_static_view::elf::attribute_of(debug, cu_die->get(), DW_AT_ranges);
  expect_true(ranges_attr.has_value(), "rnglistx fixture 的 compile unit 应存在 DW_AT_ranges");

  const auto description = elf_static_view::elf::read_range_description(ranges_attr->get(), cu_die->get());
  expect_true(description.has_value(), "DW_FORM_rnglistx 的 ranges 应可被 read_range_description 读取");
  expect_true(description->kind == DW_FORM_rnglistx,
              "rnglistx fixture 应保留 DW_FORM_rnglistx 形式");
  expect_true(description->entry_count >= 2, "rnglistx fixture 应至少解析出两段 range entry");
  const auto concrete_entries =
    std::count_if(description->entries.begin(),
                  description->entries.end(),
                  [](const elf_static_view::elf::LocationDescription::Entry& entry) {
                    return (entry.cooked_low_pc.has_value() && entry.cooked_high_pc.has_value()) ||
                           (entry.raw_low_pc.has_value() && entry.raw_high_pc.has_value());
                  });
  expect_true(concrete_entries >= 2, "rnglistx fixture 应至少返回两条可用 ranges");

  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_RNGLISTX_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  expect_true(!model.compile_units.empty(), "rnglistx fixture 应至少保留一个 compile unit");
  expect_true(model.compile_units.front().address.location_entry_count.has_value(),
              "rnglistx fixture 的 compile unit 应记录 ranges entry 数量");
  expect_true(model.compile_units.front().address.location_entry_count.value() >= 2,
              "rnglistx fixture 的 compile unit 应保留两段以上 range entry");
  expect_true(model.compile_units.front().address.location_ranges.size() >= 2,
              "rnglistx fixture 的 compile unit 应把 ranges 写入模型");
}

void verify_rnglists_start_end_fixture() {
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_START_END_FIXTURE_PATH,
                                     "--debug-rnglists --verbose",
                                     "DW_RLE_start_end",
                                     "rnglists start_end fixture");

  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_RNGLISTS_START_END_FIXTURE_PATH);
  Dwarf_Debug debug = debug_handle.get();
  auto cu_die = elf_static_view::elf::die_from_offset(debug, 0x0c, true);
  expect_true(cu_die.has_value(), "rnglists start_end fixture 应能拿到 compile unit 根 DIE");
  auto ranges_attr = elf_static_view::elf::attribute_of(debug, cu_die->get(), DW_AT_ranges);
  expect_true(ranges_attr.has_value(), "rnglists start_end fixture 应存在 DW_AT_ranges");
  const auto description = elf_static_view::elf::read_range_description(ranges_attr->get(), cu_die->get());
  expect_true(description.has_value(), "DW_RLE_start_end 应可被 read_range_description 读取");
  expect_true(description->entry_count >= 2, "DW_RLE_start_end 应至少保留两段 range entry");
  const auto concrete_entries =
    std::count_if(description->entries.begin(),
                  description->entries.end(),
                  [](const elf_static_view::elf::LocationDescription::Entry& entry) {
                    return (entry.cooked_low_pc.has_value() && entry.cooked_high_pc.has_value()) ||
                           (entry.raw_low_pc.has_value() && entry.raw_high_pc.has_value());
                  });
  expect_true(concrete_entries >= 2, "DW_RLE_start_end 应至少返回两条可用 range");
}

void verify_rnglists_offset_pair_fixture() {
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_OFFSET_PAIR_FIXTURE_PATH,
                                     "--debug-rnglists --verbose",
                                     "DW_RLE_offset_pair",
                                     "rnglists offset_pair fixture");
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_OFFSET_PAIR_FIXTURE_PATH,
                                     "--debug-rnglists --verbose",
                                     "DW_RLE_base_address",
                                     "rnglists offset_pair fixture");

  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_RNGLISTS_OFFSET_PAIR_FIXTURE_PATH);
  Dwarf_Debug debug = debug_handle.get();
  auto cu_die = elf_static_view::elf::die_from_offset(debug, 0x0c, true);
  expect_true(cu_die.has_value(), "rnglists offset_pair fixture 应能拿到 compile unit 根 DIE");
  auto ranges_attr = elf_static_view::elf::attribute_of(debug, cu_die->get(), DW_AT_ranges);
  expect_true(ranges_attr.has_value(), "rnglists offset_pair fixture 应存在 DW_AT_ranges");
  const auto description = elf_static_view::elf::read_range_description(ranges_attr->get(), cu_die->get());
  expect_true(description.has_value(), "DW_RLE_offset_pair 应可被 read_range_description 读取");
  expect_true(description->entry_count >= 3, "DW_RLE_offset_pair 应至少保留三条 range entry");
  const auto concrete_entries =
    std::count_if(description->entries.begin(),
                  description->entries.end(),
                  [](const elf_static_view::elf::LocationDescription::Entry& entry) {
                    return (entry.cooked_low_pc.has_value() && entry.cooked_high_pc.has_value()) ||
                           (entry.raw_low_pc.has_value() && entry.raw_high_pc.has_value());
                  });
  expect_true(concrete_entries >= 2, "DW_RLE_offset_pair 应至少返回两条可用 range");
}

void verify_rnglists_startx_length_fixture() {
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_STARTX_LENGTH_FIXTURE_PATH,
                                     "--debug-rnglists --verbose",
                                     "DW_RLE_startx_length",
                                     "rnglists startx_length fixture");

  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_RNGLISTS_STARTX_LENGTH_FIXTURE_PATH);
  Dwarf_Debug debug = debug_handle.get();
  auto cu_die = elf_static_view::elf::die_from_offset(debug, 0x0c, true);
  expect_true(cu_die.has_value(), "rnglists startx_length fixture 应能拿到 compile unit 根 DIE");
  auto ranges_attr = elf_static_view::elf::attribute_of(debug, cu_die->get(), DW_AT_ranges);
  expect_true(ranges_attr.has_value(), "rnglists startx_length fixture 应存在 DW_AT_ranges");
  const auto description = elf_static_view::elf::read_range_description(ranges_attr->get(), cu_die->get());
  expect_true(description.has_value(), "DW_RLE_startx_length 应可被 read_range_description 读取");
  expect_true(description->entry_count >= 2, "DW_RLE_startx_length 应至少保留两段 range entry");
  const auto concrete_entries =
    std::count_if(description->entries.begin(),
                  description->entries.end(),
                  [](const elf_static_view::elf::LocationDescription::Entry& entry) {
                    return (entry.cooked_low_pc.has_value() && entry.cooked_high_pc.has_value()) ||
                           (entry.raw_low_pc.has_value() && entry.raw_high_pc.has_value());
                  });
  expect_true(concrete_entries >= 2, "DW_RLE_startx_length 应至少返回两条可用 range");
}

void verify_rnglists_base_addressx_fixture() {
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_BASE_ADDRESSX_FIXTURE_PATH,
                                     "--debug-rnglists --verbose",
                                     "DW_RLE_base_addressx",
                                     "rnglists base_addressx fixture");
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_BASE_ADDRESSX_FIXTURE_PATH,
                                     "--debug-rnglists --verbose",
                                     "DW_RLE_offset_pair",
                                     "rnglists base_addressx fixture");

  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_RNGLISTS_BASE_ADDRESSX_FIXTURE_PATH);
  Dwarf_Debug debug = debug_handle.get();
  auto cu_die = elf_static_view::elf::die_from_offset(debug, 0x0c, true);
  expect_true(cu_die.has_value(), "rnglists base_addressx fixture 应能拿到 compile unit 根 DIE");
  auto ranges_attr = elf_static_view::elf::attribute_of(debug, cu_die->get(), DW_AT_ranges);
  expect_true(ranges_attr.has_value(), "rnglists base_addressx fixture 应存在 DW_AT_ranges");
  const auto description = elf_static_view::elf::read_range_description(ranges_attr->get(), cu_die->get());
  expect_true(description.has_value(), "DW_RLE_base_addressx 应可被 read_range_description 读取");
  expect_true(description->entry_count >= 3, "DW_RLE_base_addressx 应至少保留三条 range entry");
  const auto concrete_entries =
    std::count_if(description->entries.begin(),
                  description->entries.end(),
                  [](const elf_static_view::elf::LocationDescription::Entry& entry) {
                    return (entry.cooked_low_pc.has_value() && entry.cooked_high_pc.has_value()) ||
                           (entry.raw_low_pc.has_value() && entry.raw_high_pc.has_value());
                  });
  expect_true(concrete_entries >= 2, "DW_RLE_base_addressx 应至少返回两条可用 range");
  expect_true(std::any_of(description->entries.begin(),
                          description->entries.end(),
                          [](const elf_static_view::elf::LocationDescription::Entry& entry) {
                            return entry.debug_addr_unavailable;
                          }),
              "当前 base_addressx fixture 应显式暴露缺少 .debug_addr / addr_base 的样本边界");
}

void verify_rnglists_startx_endx_fixture() {
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_STARTX_ENDX_FIXTURE_PATH,
                                     "--debug-rnglists --verbose",
                                     "DW_RLE_startx_endx",
                                     "rnglists startx_endx fixture");

  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_RNGLISTS_STARTX_ENDX_FIXTURE_PATH);
  Dwarf_Debug debug = debug_handle.get();
  auto cu_die = elf_static_view::elf::die_from_offset(debug, 0x0c, true);
  expect_true(cu_die.has_value(), "rnglists startx_endx fixture 应能拿到 compile unit 根 DIE");
  auto ranges_attr = elf_static_view::elf::attribute_of(debug, cu_die->get(), DW_AT_ranges);
  expect_true(ranges_attr.has_value(), "rnglists startx_endx fixture 应存在 DW_AT_ranges");
  const auto description = elf_static_view::elf::read_range_description(ranges_attr->get(), cu_die->get());
  expect_true(description.has_value(), "DW_RLE_startx_endx 应可被 read_range_description 读取");
  expect_true(description->entry_count >= 2, "DW_RLE_startx_endx 应至少保留两段 range entry");
  const auto concrete_entries =
    std::count_if(description->entries.begin(),
                  description->entries.end(),
                  [](const elf_static_view::elf::LocationDescription::Entry& entry) {
                    return (entry.cooked_low_pc.has_value() && entry.cooked_high_pc.has_value()) ||
                           (entry.raw_low_pc.has_value() && entry.raw_high_pc.has_value());
                  });
  expect_true(concrete_entries >= 2, "DW_RLE_startx_endx 应至少返回两条可用 range");
}

void verify_implicit_const_fixture() {
  const auto output_path = run_dwarfdump_to_temp("--debug-abbrev",
                                                 ELF_STATIC_VIEW_GCC_GNU_ALT_FIXTURE_PATH,
                                                 "implicit-const",
                                                 "implicit_const fixture");
  const auto content = read_all(output_path);
  std::filesystem::remove(output_path);
  expect_contains(content, "DW_FORM_implicit_const", "fixture 应显式包含 DW_FORM_implicit_const");
  expect_contains(content, "DW_AT_decl_file", "implicit_const fixture 应保留 decl_file");
  expect_contains(content, "DW_AT_decl_column", "implicit_const fixture 应保留 decl_column");
}

void verify_dwarf5_strx_type_unit_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DWARF5_STRX_TYPE_UNIT_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "global_name [StaticAddressKnown] FancyName",
                  "dwarf5 strx type unit fixture 应把 global_name 解析成 FancyName");
  expect_contains(output, "use_name::local [RuntimeOnly] FancyName",
                  "dwarf5 strx type unit fixture 应把 local 解析成 FancyName");
  expect_contains(output, "global_name.value [StaticLayoutKnown] int",
                  "dwarf5 strx type unit fixture 应保留成员 int 类型");
}

void verify_dwarf5_strx_type_unit_abbrev_offset_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DWARF5_STRX_TYPE_UNIT_ABBREV_OFFSET_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "global_name [StaticAddressKnown] FancyName",
                  "abbrev_offset!=0 的 dwarf5 type unit 应把 global_name 解析成 FancyName");
  expect_contains(output, "use_name::local [RuntimeOnly] FancyName",
                  "abbrev_offset!=0 的 dwarf5 type unit 应把 local 解析成 FancyName");
  expect_contains(output, "global_name.value [StaticLayoutKnown] int",
                  "abbrev_offset!=0 的 dwarf5 type unit 应保留成员 int 类型");
}

void verify_split_dwarf_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_SPLIT_DWARF_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto globals = find_variables_by_name(model, "global_value");
  const auto locals = find_variables_by_name(model, "local");
  expect_true(globals.size() == 1, "split dwarf fixture 应解析出 global_value");
  expect_true(locals.size() == 1, "split dwarf fixture 应解析出 local");
  expect_true(globals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
              "split dwarf 的 global_value 应解析出静态地址");
  expect_true(locals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
              "split dwarf 的 local 应解析出静态地址");
  expect_true(globals.front()->type.id != "type@unknown", "split dwarf 的 global_value 不应退化成未知类型");
  expect_true(locals.front()->type.id != "type@unknown", "split dwarf 的 local 不应退化成未知类型");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "global_value [StaticAddressKnown] S",
                  "split dwarf dump 应包含 global_value");
  expect_contains(output, "main::local [StaticAddressKnown] S",
                  "split dwarf dump 应包含 local");
}

void verify_split_dwp_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto dwp_path = std::filesystem::path(ELF_STATIC_VIEW_SPLIT_DWARF_FIXTURE_PATH).replace_extension("dwp");
  const auto model = loader.dump(dwp_path.string(),
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto globals = find_variables_by_name(model, "global_value");
  const auto locals = find_variables_by_name(model, "local");
  expect_true(globals.size() == 1, "dwp fixture 应解析出 global_value");
  expect_true(locals.size() == 1, "dwp fixture 应解析出 local");
  expect_true(globals.front()->availability == elf_static_view::Availability::Unavailable,
              "纯 dwp fixture 当前没有 tied executable，global_value 应保留 Unavailable");
  expect_true(locals.front()->availability == elf_static_view::Availability::Unavailable,
              "纯 dwp fixture 当前没有 tied executable，local 应保留 Unavailable");
  expect_true(globals.front()->type.id != "type@unknown", "dwp 的 global_value 不应退化成未知类型");
  expect_true(locals.front()->type.id != "type@unknown", "dwp 的 local 不应退化成未知类型");
}

void verify_debug_sup_fixture() {
  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_DEBUG_SUP_FIXTURE_PATH);

  Dwarf_Half version = 0;
  Dwarf_Small is_supplementary = 0;
  char* filename = nullptr;
  Dwarf_Unsigned checksum_len = 0;
  Dwarf_Small* checksum = nullptr;
  Dwarf_Error error = nullptr;
  const int result = dwarf_get_debug_sup(debug_handle.get(),
                                         &version,
                                         &is_supplementary,
                                         &filename,
                                         &checksum_len,
                                         &checksum,
                                         &error);
  if (result != DW_DLV_OK) {
    std::string message = "debug_sup fixture 应可读出 .debug_sup";
    if (error != nullptr) {
      message += "，实际错误: ";
      message += dwarf_errmsg(error);
      dwarf_dealloc_error(nullptr, error);
    }
    throw std::runtime_error(message);
  }

  expect_true(version == 2, ".debug_sup version 应为 2");
  expect_true(is_supplementary == 0, ".debug_sup 样本应标记为主对象引用 supplementary");
  expect_true(filename != nullptr, ".debug_sup filename 不应为空");
  expect_true(std::string(filename) == "fake-file/fake-file-name",
              ".debug_sup filename 应与 dwarfgen 生成内容一致");
  expect_true(checksum_len == sizeof("fake checksum_content"),
              ".debug_sup checksum 长度应与 dwarfgen 固定内容一致");
  expect_true(checksum != nullptr, ".debug_sup checksum 不应为空");
  expect_true(
    std::string(reinterpret_cast<const char*>(checksum), static_cast<std::size_t>(checksum_len - 1)) ==
      "fake checksum_content",
    ".debug_sup checksum 内容应与 dwarfgen 固定内容一致");

  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DEBUG_SUP_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  expect_true(model.elf_info.object_class == "ELF32", "debug_sup fixture 应保留 dwarfgen 产物的 ELF32 元信息");
  expect_true(model.elf_info.file_type == "REL", "debug_sup fixture 当前应是 dwarfgen 生成的 REL 对象");
  expect_true(model.symbols.empty(),
              "debug_sup 最小样本当前只验证 section 可读与加载不崩溃，不要求保留原始静态变量");
}

void verify_debug_sup_sidecar_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DEBUG_SUP_SIDECAR_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "demo::global_object [StaticAddressKnown] Derived",
                  ".debug_sup sidecar 自动绑定后应保留主 ELF 的静态变量");
  expect_contains(output, "demo::Derived::shared [StaticAddressKnown] int",
                  ".debug_sup sidecar 自动绑定后应保留类静态成员");
}

void verify_gcc_strp_sup_fixture() {
  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_GCC_DWARF5_STRP_SUP_FIXTURE_PATH);
  Dwarf_Debug debug = debug_handle.get();

  Dwarf_Unsigned cu_header_length = 0;
  Dwarf_Half version_stamp = 0;
  Dwarf_Off abbrev_offset = 0;
  Dwarf_Half address_size = 0;
  Dwarf_Half length_size = 0;
  Dwarf_Half extension_size = 0;
  Dwarf_Sig8 type_signature {};
  Dwarf_Unsigned type_offset = 0;
  Dwarf_Unsigned next_cu_header_offset = 0;
  Dwarf_Half header_cu_type = 0;
  Dwarf_Error error = nullptr;

  expect_true(
    dwarf_next_cu_header_d(debug,
                           true,
                           &cu_header_length,
                           &version_stamp,
                           &abbrev_offset,
                           &address_size,
                           &length_size,
                           &extension_size,
                           &type_signature,
                           &type_offset,
                           &next_cu_header_offset,
                           &header_cu_type,
                           &error) == DW_DLV_OK,
    "第一个 gcc dwarf5 fixture CU 应可读取");
  expect_true(
    dwarf_next_cu_header_d(debug,
                           true,
                           &cu_header_length,
                           &version_stamp,
                           &abbrev_offset,
                           &address_size,
                           &length_size,
                           &extension_size,
                           &type_signature,
                           &type_offset,
                           &next_cu_header_offset,
                           &header_cu_type,
                           &error) == DW_DLV_OK,
    "第二个 gcc dwarf5 fixture CU 应可读取");

  Dwarf_Die cu_die = nullptr;
  expect_true(dwarf_siblingof_b(debug, nullptr, true, &cu_die, &error) == DW_DLV_OK,
              "应能拿到当前 gcc dwarf5 fixture 的第二个 CU DIE");

  auto name_attr = elf_static_view::elf::attribute_of(debug, cu_die, DW_AT_name);
  expect_true(name_attr.has_value(), "第二个 CU 应存在 DW_AT_name");
  const auto name = elf_static_view::elf::string_attr(name_attr->get());
  expect_true(name.has_value(), "DW_FORM_strp_sup 的 name 应可被读取");
  expect_true(name->find("cygwin.S") != std::string::npos,
              "DW_FORM_strp_sup 的 name 应回到 supplementary string");

  auto producer_attr = elf_static_view::elf::attribute_of(debug, cu_die, DW_AT_producer);
  expect_true(producer_attr.has_value(), "第二个 CU 应存在 DW_AT_producer");
  const auto producer = elf_static_view::elf::string_attr(producer_attr->get());
  expect_true(producer.has_value(), "DW_FORM_strp_sup 的 producer 应可被读取");
  expect_true(producer->find("GNU AS") != std::string::npos,
              "DW_FORM_strp_sup 的 producer 应回到 supplementary string");
}

void verify_current_supplementary_gap_notes() {
  // 这不是功能测试，而是把当前“已接线”和“已正式覆盖”的边界固定住，避免误判。
  // 备注：
  // - DW_FORM_indirect 已通过 ref_sig8_debug_types_indirect fixture 间接覆盖；
  // - DW_FORM_addrx / addrx1 / addrx2 / addrx3 / addrx4 已有显式 fixture；
  // - DW_FORM_strx1 / strx2 / strx3 / strx4 已有显式 fixture；
  // - 当前剩余更大的问题不再是 supplementary/alternate form，而是“DWARF 2.0-最新”
  //   口径下仍有一些 code-only form/loc op 只被实现、还没逐项做 form 级显式回归。
  expect_true(true,
              "当前已正式覆盖 .debug_sup section 读取、sidecar 自动绑定、"
              "DW_FORM_strp_sup、DW_FORM_ref_sup4、DW_FORM_ref_sup8、"
              "DW_FORM_GNU_strp_alt、DW_FORM_GNU_ref_alt。");
}

void verify_const_value_text_json_round_trip() {
  elf_static_view::ProjectModel model;
  model.file = "demo.elf";
  model.elf_info = {.object_class = "ELF64",
                    .byte_order = "LittleEndian",
                    .file_type = "DYN",
                    .machine = "x86_64",
                    .os_abi = "SystemV"};

  elf_static_view::VariableRecord symbol;
  symbol.id = "var@text";
  symbol.name = "const_blob";
  symbol.compile_unit_name = "demo.cpp";
  symbol.variable_kind = elf_static_view::VariableKind::FileStatic;
  symbol.availability = elf_static_view::Availability::StaticAddressKnown;
  symbol.address.kind = elf_static_view::AddressKind::Unknown;
  symbol.address.location_description = "DW_AT_const_value";
  symbol.type.id = "type@blob";
  symbol.const_value_text = "0x112233";
  symbol.has_static_storage = true;
  model.symbols.push_back(symbol);
  model.expanded.push_back(elf_static_view::ExpandedNode{
    .path = "const_blob",
    .display_name = "const_blob",
    .type_name = "Blob",
    .type_kind = elf_static_view::TypeKind::Struct,
    .availability = elf_static_view::Availability::StaticAddressKnown,
    .absolute_address = std::nullopt,
    .relative_offset = std::nullopt,
    .byte_size = std::nullopt,
    .array_count = std::nullopt,
    .array_stride = std::nullopt,
    .children = {},
  });

  const auto json = elf_static_view::render_dump_json(model);
  expect_contains(json, "\"const_value_text\": \"0x112233\"", "JSON 应输出 const_value_text");
  const auto parsed = elf_static_view::parse_dump_json(json);
  expect_true(parsed.symbols.size() == 1, "const_value_text JSON 往返应保留变量");
  expect_true(parsed.symbols.front().const_value_text.has_value(),
              "const_value_text JSON 往返应保留文本常量");
  expect_true(parsed.symbols.front().const_value_text.value() == "0x112233",
              "const_value_text JSON 往返值不正确");
  const auto text = elf_static_view::render_dump_text(parsed);
  expect_contains(text, "= 0x112233", "文本 dump 应展示 const_value_text");
}

void verify_value_expression_text_rendering() {
  elf_static_view::ProjectModel model;
  model.file = "demo.elf";
  model.elf_info = {.object_class = "ELF64",
                    .byte_order = "LittleEndian",
                    .file_type = "DYN",
                    .machine = "x86_64",
                    .os_abi = "SystemV"};

  elf_static_view::VariableRecord symbol;
  symbol.id = "var@value-expr";
  symbol.name = "runtime_value";
  symbol.compile_unit_name = "demo.cpp";
  symbol.variable_kind = elf_static_view::VariableKind::Local;
  symbol.availability = elf_static_view::Availability::RuntimeOnly;
  symbol.address.kind = elf_static_view::AddressKind::Unknown;
  symbol.address.location_description = "value expression";
  symbol.type.id = "type@int";
  model.symbols.push_back(symbol);
  model.expanded.push_back(elf_static_view::ExpandedNode{
    .path = "runtime_value",
    .display_name = "runtime_value",
    .type_name = "int",
    .type_kind = elf_static_view::TypeKind::Base,
    .availability = elf_static_view::Availability::RuntimeOnly,
    .absolute_address = std::nullopt,
    .relative_offset = std::nullopt,
    .byte_size = std::nullopt,
    .array_count = std::nullopt,
    .array_stride = std::nullopt,
    .children = {},
  });

  const auto json = elf_static_view::render_dump_json(model);
  expect_contains(json, "\"location_description\": \"value expression\"",
                  "JSON 应输出 value expression 描述");
  const auto parsed = elf_static_view::parse_dump_json(json);
  expect_true(parsed.symbols.size() == 1, "value expression JSON 往返应保留变量");
  expect_true(parsed.symbols.front().address.location_description == "value expression",
              "value expression JSON 往返后描述应保持一致");
  const auto text = elf_static_view::render_dump_text(parsed);
  expect_contains(text, "runtime_value", "文本 dump 应保留 value expression 变量");
}

void verify_named_location_op_rendering() {
  elf_static_view::ProjectModel model;
  model.file = "demo.elf";
  model.elf_info = {.object_class = "ELF64",
                    .byte_order = "LittleEndian",
                    .file_type = "DYN",
                    .machine = "x86_64",
                    .os_abi = "SystemV"};

  elf_static_view::VariableRecord symbol;
  symbol.id = "var@named-op";
  symbol.name = "frame_local";
  symbol.compile_unit_name = "demo.cpp";
  symbol.variable_kind = elf_static_view::VariableKind::Local;
  symbol.availability = elf_static_view::Availability::RuntimeOnly;
  symbol.address.kind = elf_static_view::AddressKind::Unknown;
  symbol.address.location_description = "DW_OP_breg20";
  symbol.type.id = "type@int";
  model.symbols.push_back(symbol);
  model.expanded.push_back(elf_static_view::ExpandedNode{
    .path = "frame_local",
    .display_name = "frame_local",
    .type_name = "int",
    .type_kind = elf_static_view::TypeKind::Base,
    .availability = elf_static_view::Availability::RuntimeOnly,
    .absolute_address = std::nullopt,
    .relative_offset = std::nullopt,
    .byte_size = std::nullopt,
    .array_count = std::nullopt,
    .array_stride = std::nullopt,
    .children = {},
  });

  const auto json = elf_static_view::render_dump_json(model);
  expect_contains(json, "\"location_description\": \"DW_OP_breg20\"",
                  "JSON 应输出命名后的 location op");
  const auto text = elf_static_view::render_dump_text(model);
  expect_contains(text, "frame_local", "文本 dump 应保留命名 location op 变量");
}

void verify_register_location_text_rendering() {
  elf_static_view::ProjectModel model;
  model.file = "demo.elf";
  model.elf_info = {.object_class = "ELF64",
                    .byte_order = "LittleEndian",
                    .file_type = "DYN",
                    .machine = "x86_64",
                    .os_abi = "SystemV"};

  elf_static_view::VariableRecord symbol;
  symbol.id = "var@reg-op";
  symbol.name = "arg0";
  symbol.compile_unit_name = "demo.cpp";
  symbol.variable_kind = elf_static_view::VariableKind::Parameter;
  symbol.availability = elf_static_view::Availability::RuntimeOnly;
  symbol.address.kind = elf_static_view::AddressKind::Unknown;
  symbol.address.location_description = "register-value DW_OP_reg0";
  symbol.type.id = "type@int";
  model.symbols.push_back(symbol);
  model.expanded.push_back(elf_static_view::ExpandedNode{
    .path = "arg0",
    .display_name = "arg0",
    .type_name = "int",
    .type_kind = elf_static_view::TypeKind::Base,
    .availability = elf_static_view::Availability::RuntimeOnly,
    .absolute_address = std::nullopt,
    .relative_offset = std::nullopt,
    .byte_size = std::nullopt,
    .array_count = std::nullopt,
    .array_stride = std::nullopt,
    .children = {},
  });

  const auto json = elf_static_view::render_dump_json(model);
  expect_contains(json, "\"location_description\": \"register-value DW_OP_reg0\"",
                  "JSON 应输出寄存器语义 location op");
}

void verify_register_address_text_rendering() {
  elf_static_view::ProjectModel model;
  model.file = "demo.elf";
  model.elf_info = {.object_class = "ELF64",
                    .byte_order = "LittleEndian",
                    .file_type = "DYN",
                    .machine = "x86_64",
                    .os_abi = "SystemV"};

  elf_static_view::VariableRecord symbol;
  symbol.id = "var@breg-op";
  symbol.name = "stack_slot";
  symbol.compile_unit_name = "demo.cpp";
  symbol.variable_kind = elf_static_view::VariableKind::Local;
  symbol.availability = elf_static_view::Availability::RuntimeOnly;
  symbol.address.kind = elf_static_view::AddressKind::Unknown;
  symbol.address.location_description = "register-address DW_OP_breg20 -2";
  symbol.type.id = "type@int";
  model.symbols.push_back(symbol);
  model.expanded.push_back(elf_static_view::ExpandedNode{
    .path = "stack_slot",
    .display_name = "stack_slot",
    .type_name = "int",
    .type_kind = elf_static_view::TypeKind::Base,
    .availability = elf_static_view::Availability::RuntimeOnly,
    .absolute_address = std::nullopt,
    .relative_offset = std::nullopt,
    .byte_size = std::nullopt,
    .array_count = std::nullopt,
    .array_stride = std::nullopt,
    .children = {},
  });

  const auto json = elf_static_view::render_dump_json(model);
  expect_contains(json, "\"location_description\": \"register-address DW_OP_breg20 -2\"",
                  "JSON 应输出寄存器寻址语义 location op");
}

void verify_unspecified_type_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_UNSPECIFIED_TYPE_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto* null_type_alias = find_type_by_name(model, "NullType");
  expect_true(null_type_alias != nullptr, "unspecified fixture 应包含 NullType");
  expect_true(null_type_alias->kind == elf_static_view::TypeKind::Typedef,
              "NullType 应保留 Typedef kind");
  expect_true(null_type_alias->aliased_of.has_value(), "NullType 应指向 decltype(nullptr) 底层类型");

  const auto aliased_id = null_type_alias->aliased_of->id;
  const auto unspecified_iter =
    std::find_if(model.types.begin(), model.types.end(), [&](const elf_static_view::TypeNode& type) {
      return type.id == aliased_id;
    });
  expect_true(unspecified_iter != model.types.end(), "decltype(nullptr) 类型节点应存在");
  expect_true(unspecified_iter->kind == elf_static_view::TypeKind::Unspecified,
              "decltype(nullptr) 应识别为 Unspecified");

  const auto globals = find_variables_by_name(model, "global_null");
  expect_true(globals.size() == 1, "应解析出 global_null");
}

void verify_atomic_type_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_ATOMIC_TYPE_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto atomic_iter =
    std::find_if(model.types.begin(), model.types.end(), [&](const elf_static_view::TypeNode& type) {
      return type.kind == elf_static_view::TypeKind::Atomic;
    });
  expect_true(atomic_iter != model.types.end(), "atomic fixture 应包含 atomic 类型");
  expect_true(atomic_iter->qualified_of.has_value(), "atomic 类型应指向底层类型");

  const auto globals = find_variables_by_name(model, "global_atomic");
  expect_true(globals.size() == 1, "应解析出 global_atomic");
  expect_true(globals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
              "global_atomic 应解析出静态地址");
}

void verify_abstract_origin_const_value_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_ABSTRACT_ORIGIN_VALUES_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto folded_variables = find_variables_by_name(model, "folded");
  expect_true(!folded_variables.empty(), "abstract-origin fixture 应包含 folded");

  const auto folded_with_const =
    std::find_if(folded_variables.begin(), folded_variables.end(), [](const auto* symbol) {
      return symbol->const_value.has_value() && symbol->const_value.value() == 8;
    });
  expect_true(folded_with_const != folded_variables.end(),
              "至少一个 folded 实例应从 abstract/inlined 视图保留 const_value");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "main::call_helper::value [RuntimeOnly] int = 3",
                  "应保留具体实例 value 的 const_value");
  expect_contains(output, "main::call_helper::helper::seed [RuntimeOnly] int = 3",
                  "应保留具体实例 seed 的 const_value");
  expect_true(output.find("helper::seed [RuntimeOnly] int\n") == std::string::npos,
              "abstract-origin fixture 不应保留无值 seed 占位节点");
  expect_true(output.find("call_helper::value [RuntimeOnly] int\n") == std::string::npos,
              "abstract-origin fixture 不应保留无值 value 占位节点");
}

void verify_loader_error_contains_file_path() {
  elf_static_view::ProjectLoader loader;
  const std::string missing_path = "Z:/definitely-missing/fixture.elf";
  try {
    (void)loader.dump(missing_path,
                      {.include_runtime_only = true,
                       .only_static_known = false,
                       .symbol_name = std::nullopt,
                       .expand_depth = 8});
    throw std::runtime_error("断言失败: 缺失文件应抛出异常");
  } catch (const std::exception& error) {
    const std::string message = error.what();
    expect_contains(message, "文件分析失败: " + missing_path, "失败日志应包含输入文件路径");
  }
}

void verify_inline_scope_static_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_INLINE_SCOPE_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto file_statics = find_file_static_variables(model, "file_static");
  expect_true(file_statics.size() == 1, "inline scope fixture 应保留 file_static");

  const auto inline_statics =
    find_variables_by_kind(model, "inline_static", elf_static_view::VariableKind::FunctionStatic);
  expect_true(inline_statics.size() == 1, "inline scope fixture 应识别 helper 内的函数静态变量");
  expect_true(inline_statics.front()->address.absolute_address.has_value(),
              "inline_static 应解析出绝对地址");
  expect_true(inline_statics.front()->scope_path.size() == 1, "inline_static 应保留单层函数作用域");

  const auto block_statics =
    find_variables_by_kind(model, "block_static", elf_static_view::VariableKind::FunctionStatic);
  expect_true(block_statics.size() == 1, "inline scope fixture 应识别 lexical_block 内的静态变量");
  expect_true(block_statics.front()->address.absolute_address.has_value(),
              "block_static 应解析出绝对地址");
  expect_true(block_statics.front()->scope_path == inline_statics.front()->scope_path,
              "block_static 与 inline_static 应保持同一抽象函数作用域");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "inline_static", "inline scope fixture dump 应包含 inline_static");
  expect_contains(output, "block_static", "inline scope fixture dump 应包含 block_static");

  const auto seed_variables = find_variables_by_name(model, "seed");
  expect_true(!seed_variables.empty(), "inline scope fixture 应包含 seed 参数");
  const auto seed_with_fbreg =
    std::find_if(seed_variables.begin(), seed_variables.end(), [](const auto* symbol) {
      return symbol->address.location_description.starts_with("frame-base+");
    });
  expect_true(seed_with_fbreg != seed_variables.end(),
              "inline scope fixture 应把 DW_OP_fbreg 解释成 frame-base 偏移");
}

void verify_const_value_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_CONST_VALUE_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto file_statics = find_file_static_variables(model, "const_static");
  expect_true(file_statics.size() == 1, "const_value fixture 应解析出 const_static");
  expect_true(file_statics.front()->const_value.has_value(), "const_static 应保留 DW_AT_const_value");
  expect_true(file_statics.front()->const_value.value() == 123, "const_static 常量值应为 123");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "const_static", "const_value fixture dump 应包含 const_static");
  expect_contains(output, "= 123", "const_value fixture dump 应展示常量值");

  const auto pair_variables = find_variables_by_name(model, "pair");
  expect_true(pair_variables.size() == 1, "const_value fixture 应保留局部变量 pair");
  expect_true(pair_variables.front()->availability == elf_static_view::Availability::RuntimeOnly,
              "piecewise loclist 的 pair 应标记为 RuntimeOnly");
  expect_true(pair_variables.front()->address.location_entry_count.has_value(),
              "pair 应记录 loclist entry 数量");
  expect_true(pair_variables.front()->address.location_entry_count.value() >= 1,
              "pair 的 loclist entry 数量应至少为 1");
  expect_true(pair_variables.front()->address.location_ranges.size() >= 1,
              "pair 应保留至少一段有效 loclist 范围");
  expect_true(pair_variables.front()->address.location_ranges.front().cooked_low_pc.has_value(),
              "pair 的 loclist 范围应保留 cooked low pc");
  expect_true(pair_variables.front()->address.location_ranges.front().cooked_high_pc.has_value(),
              "pair 的 loclist 范围应保留 cooked high pc");
  expect_contains(pair_variables.front()->address.location_description,
                  "DW_OP_piece",
                  "pair 应保留 DW_OP_piece 描述");
  expect_contains(pair_variables.front()->address.location_description,
                  "DW_OP_stack_value",
                  "pair 应保留 DW_OP_stack_value 描述");
  expect_contains(output, "ranges=[0x", "const_value fixture dump 应展示 loclist 范围");
  expect_contains(output, "pair [RuntimeOnly]", "const_value fixture dump 应包含 pair");
}

void verify_piece_stack_value_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_PIECE_STACK_VALUE_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto seed_variables = find_variables_by_name(model, "seed");
  expect_true(seed_variables.size() == 1, "piece stack_value fixture 应保留参数 seed");
  expect_true(seed_variables.front()->availability == elf_static_view::Availability::RuntimeOnly,
              "seed 应标记为 RuntimeOnly");
  expect_true(seed_variables.front()->address.location_entry_count.has_value(),
              "seed 应记录 loclist entry 数量");
  expect_true(seed_variables.front()->address.location_entry_count.value() >= 2,
              "seed 的 loclist entry 数量应至少为 2");
  expect_true(seed_variables.front()->address.location_ranges.size() >= 2,
              "seed 应保留至少两段有效 loclist 范围");
  expect_contains(seed_variables.front()->address.location_description,
                  "DW_OP_entry_value",
                  "seed 应保留 DW_OP_entry_value 描述");
  expect_contains(seed_variables.front()->address.location_description,
                  "DW_OP_stack_value",
                  "seed 应保留 DW_OP_stack_value 描述");

  const auto bits_variables = find_variables_by_name(model, "bits");
  expect_true(bits_variables.size() == 1, "piece stack_value fixture 应保留局部变量 bits");
  expect_true(bits_variables.front()->availability == elf_static_view::Availability::RuntimeOnly,
              "bits 应标记为 RuntimeOnly");
  expect_true(bits_variables.front()->address.location_entry_count.has_value(),
              "bits 应记录 loclist entry 数量");
  expect_true(bits_variables.front()->address.location_entry_count.value() >= 2,
              "bits 的 loclist entry 数量应至少为 2");
  expect_true(bits_variables.front()->address.location_ranges.size() >= 2,
              "bits 应保留至少两段有效 loclist 范围");
  expect_true(bits_variables.front()->address.location_ranges.front().cooked_low_pc.has_value(),
              "bits 的 loclist 范围应保留 cooked low pc");
  expect_true(bits_variables.front()->address.location_ranges.front().cooked_high_pc.has_value(),
              "bits 的 loclist 范围应保留 cooked high pc");
  expect_contains(bits_variables.front()->address.location_description,
                  "DW_OP_piece",
                  "bits 应保留 DW_OP_piece 描述");
  expect_contains(bits_variables.front()->address.location_description,
                  "DW_OP_stack_value",
                  "bits 应保留 DW_OP_stack_value 描述");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "use_bits::seed [RuntimeOnly]", "piece stack_value fixture dump 应包含 seed");
  expect_contains(output, "use_bits::bits [RuntimeOnly]", "piece stack_value fixture dump 应包含 bits");
  expect_contains(output, "ranges=[0x", "piece stack_value fixture dump 应展示 loclist 范围");
}

void verify_bit_piece_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_BIT_PIECE_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto bits_variables = find_variables_by_name(model, "bits");
  expect_true(bits_variables.size() == 1, "bit piece fixture 应保留局部变量 bits");
  expect_true(bits_variables.front()->availability == elf_static_view::Availability::RuntimeOnly,
              "bits 应标记为 RuntimeOnly");
  expect_true(bits_variables.front()->address.location_entry_count.has_value(),
              "bits 应记录 loclist entry 数量");
  expect_true(bits_variables.front()->address.location_entry_count.value() >= 4,
              "bits 的 loclist entry 数量应至少为 4");
  expect_true(bits_variables.front()->address.location_ranges.size() >= 4,
              "bits 应保留至少四段 loclist 范围");
  expect_contains(bits_variables.front()->address.location_description,
                  "DW_OP_bit_piece",
                  "bits 应保留 DW_OP_bit_piece 描述");
  expect_contains(bits_variables.front()->address.location_description,
                  "DW_OP_stack_value",
                  "bits 应保留 DW_OP_stack_value 描述");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "bits [RuntimeOnly]", "bit piece fixture dump 应包含 bits");
  expect_contains(output, "ranges=[0x", "bit piece fixture dump 应展示 loclist 范围");
}

void verify_dwarf5_loclists_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DWARF5_LOCLISTS_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto locals = find_variables_by_name(model, "local");
  expect_true(locals.size() == 1, "dwarf5 loclists fixture 应解析出 local");
  expect_true(locals.front()->availability == elf_static_view::Availability::RuntimeOnly,
              "dwarf5 loclists 里的 local 应标记为 RuntimeOnly");
  expect_true(locals.front()->address.location_entry_count.has_value(),
              "dwarf5 loclists 里的 local 应保留 loclist entry 数量");
  expect_true(locals.front()->address.location_entry_count.value() >= 1,
              "dwarf5 loclists 里的 local 至少应有 1 条 loclist entry");
  expect_true(!locals.front()->address.location_ranges.empty(),
              "dwarf5 loclists 里的 local 应保留至少一段范围");
  expect_true(locals.front()->address.location_ranges.front().cooked_low_pc.has_value(),
              "dwarf5 loclists 的范围应保留 cooked low pc");
  expect_true(locals.front()->address.location_ranges.front().cooked_high_pc.has_value(),
              "dwarf5 loclists 的范围应保留 cooked high pc");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "local", "dwarf5 loclists fixture dump 应包含 local");
  expect_contains(output, "ranges=[0x", "dwarf5 loclists fixture dump 应展示范围");
}

void verify_dwarf5_loclists_base_default_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_DEFAULT_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto locals = find_variables_by_name(model, "local");
  expect_true(locals.size() == 1, "base/default loclists fixture 应保留 local");
  expect_true(locals.front()->address.location_entry_count.has_value(),
              "base/default loclists 应记录 loclist entry 数量");
  expect_true(locals.front()->address.location_entry_count.value() >= 4,
              "base/default loclists 应保留插入后的额外 entry");
  expect_true(locals.front()->address.location_ranges.size() >= 2,
              "base/default loclists 仍应保留有效范围");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "use_loclist::local [RuntimeOnly]", "base/default loclists dump 应包含 local");
  expect_contains(output, "ranges=[0x", "base/default loclists dump 应展示范围");
}

void verify_dwarf5_loclists_base_addressx_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_ADDRESSX_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto locals = find_variables_by_name(model, "local");
  expect_true(locals.size() == 1, "base_addressx loclists fixture 应保留 local");
  expect_true(locals.front()->address.location_entry_count.has_value(),
              "base_addressx loclists 应记录 loclist entry 数量");
  expect_true(locals.front()->address.location_entry_count.value() >= 4,
              "base_addressx loclists 应保留插入后的额外 entry");
  expect_true(locals.front()->address.location_ranges.size() >= 2,
              "base_addressx loclists 仍应保留有效范围");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "use_loclist::local [RuntimeOnly]", "base_addressx loclists dump 应包含 local");
  expect_contains(output, "ranges=[0x", "base_addressx loclists dump 应展示范围");
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_ADDRESSX_FIXTURE_PATH,
                                     "--debug-loclists",
                                     "DW_LLE_base_addressx",
                                     "base_addressx loclists fixture");
  verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_ADDRESSX_FIXTURE_PATH,
                                     "--debug-loclists",
                                     "DW_LLE_offset_pair",
                                     "base_addressx loclists fixture");

  elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_ADDRESSX_FIXTURE_PATH);
  Dwarf_Debug debug = debug_handle.get();
  auto local_die = elf_static_view::elf::die_from_offset(debug, 0x4f, true);
  expect_true(local_die.has_value(), "base_addressx loclists fixture 应能定位 local DIE");
  auto location_attr = elf_static_view::elf::attribute_of(debug, local_die->get(), DW_AT_location);
  expect_true(location_attr.has_value(), "base_addressx loclists fixture 的 local 应存在 DW_AT_location");
  const auto location = elf_static_view::elf::read_location_description(location_attr->get());
  expect_true(location.has_value(), "DW_FORM_loclistx 的 location 应可被 read_location_description 读取");
  expect_true(location->entry_count >= 4, "DW_FORM_loclistx 应至少保留四条 loclist entry");
  const auto concrete_entries =
    std::count_if(location->entries.begin(),
                  location->entries.end(),
                  [](const elf_static_view::elf::LocationDescription::Entry& entry) {
                    return (entry.cooked_low_pc.has_value() && entry.cooked_high_pc.has_value()) ||
                           (entry.raw_low_pc.has_value() && entry.raw_high_pc.has_value());
                  });
  expect_true(concrete_entries >= 2, "DW_FORM_loclistx 应至少返回两条可用 range");
}

void verify_dwarf5_loclists_startx_length_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DWARF5_LOCLISTS_STARTX_LENGTH_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  const auto locals = find_variables_by_name(model, "local");
  expect_true(locals.size() == 1, "startx_length loclists fixture 应保留 local");
  expect_true(locals.front()->address.location_entry_count.has_value(),
              "startx_length loclists 应记录 loclist entry 数量");
  expect_true(locals.front()->address.location_entry_count.value() >= 4,
              "startx_length loclists 应保留四条以上 entry");
  expect_true(locals.front()->address.location_ranges.size() >= 2,
              "startx_length loclists 应保留至少两段有效范围");
}

void verify_dwarf5_loclists_startx_endx_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DWARF5_LOCLISTS_STARTX_ENDX_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  const auto locals = find_variables_by_name(model, "local");
  expect_true(locals.size() == 1, "startx_endx loclists fixture 应保留 local");
  expect_true(locals.front()->address.location_entry_count.has_value(),
              "startx_endx loclists 应记录 loclist entry 数量");
  expect_true(locals.front()->address.location_entry_count.value() >= 4,
              "startx_endx loclists 应保留四条以上 entry");
  expect_true(locals.front()->address.location_ranges.size() >= 2,
              "startx_endx loclists 应保留至少两段有效范围");
}

void verify_dwarf5_loclists_start_length_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DWARF5_LOCLISTS_START_LENGTH_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  const auto locals = find_variables_by_name(model, "local");
  expect_true(locals.size() == 1, "start_length loclists fixture 应保留 local");
  expect_true(locals.front()->address.location_entry_count.has_value(),
              "start_length loclists 应记录 loclist entry 数量");
  expect_true(locals.front()->address.location_entry_count.value() >= 4,
              "start_length loclists 应保留四条以上 entry");
  expect_true(locals.front()->address.location_ranges.size() >= 2,
              "start_length loclists 应保留至少两段有效范围");
}

void verify_dwarf5_loclists_start_end_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_DWARF5_LOCLISTS_START_END_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});
  const auto locals = find_variables_by_name(model, "local");
  expect_true(locals.size() == 1, "start_end loclists fixture 应保留 local");
  expect_true(locals.front()->address.location_entry_count.has_value(),
              "start_end loclists 应记录 loclist entry 数量");
  expect_true(locals.front()->address.location_entry_count.value() >= 4,
              "start_end loclists 应保留四条以上 entry");
  expect_true(locals.front()->address.location_ranges.size() >= 2,
              "start_end loclists 应保留至少两段有效范围");
}

void verify_specification_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_SPECIFICATION_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto values = find_variables_by_name(model, "value");
  expect_true(values.size() == 1, "specification fixture 应只保留真实定义节点");

  expect_true(values.front()->availability == elf_static_view::Availability::StaticAddressKnown,
              "specification fixture 应保留静态地址已知的定义节点");
  expect_true(values.front()->address.absolute_address.has_value(),
              "specification fixture 应解析出定义节点的绝对地址");
  expect_true(values.front()->address.location_description == "DW_OP_addrx",
              "specification fixture 应保留 DW_OP_addrx 位置描述");
  expect_true(values.front()->type.id != "type@unknown", "definition 节点不应退化成未知类型");
  expect_true(values.front()->variable_kind == elf_static_view::VariableKind::FileStatic ||
                values.front()->variable_kind == elf_static_view::VariableKind::StaticMember,
              "specification 定义节点应保留为真实静态变量，而不是 declaration 占位节点");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "value [StaticAddressKnown]", "specification fixture dump 应包含真实定义节点");
}

void verify_thread_local_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_THREAD_LOCAL_TLS_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  const auto tls_values = find_variables_by_name(model, "tls_value");
  expect_true(tls_values.size() == 1, "TLS fixture 应解析出 tls_value");
  expect_true(tls_values.front()->is_thread_local, "tls_value 应标记为 thread-local");
  expect_true(tls_values.front()->variable_kind == elf_static_view::VariableKind::ThreadLocal,
              "tls_value 应归类为 ThreadLocal");
  expect_true(tls_values.front()->availability == elf_static_view::Availability::RuntimeOnly,
              "tls_value 应标记为 RuntimeOnly");
  expect_true(tls_values.front()->address.location_description == "thread_local" ||
                tls_values.front()->address.location_description == "ELF symtab TLS",
              "tls_value 应保留 TLS 位置描述");

  const auto plain_values = find_variables_by_name(model, "plain_value");
  expect_true(plain_values.size() == 1, "TLS fixture 应解析出 plain_value");
  expect_true(!plain_values.front()->is_thread_local, "plain_value 不应被误判成 TLS");
  expect_true(plain_values.front()->availability == elf_static_view::Availability::StaticAddressKnown,
              "plain_value 应保留静态地址");

  const auto output = elf_static_view::render_dump_text(model);
  expect_contains(output, "tls_value [RuntimeOnly]", "TLS fixture dump 应包含 tls_value");
}

void verify_multi_cu_file_static_fixture() {
  elf_static_view::ProjectLoader loader;
  const auto model = loader.dump(ELF_STATIC_VIEW_MULTI_CU_FIXTURE_PATH,
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8});

  // 这里故意在两个源文件里放同名 file-static 变量，验证解析器不会把不同 CU 的局部符号串成一个。
  expect_true(model.compile_units.size() >= 2, "多 CU fixture 至少应包含两个编译单元");
  const auto shared_file_statics = find_file_static_variables(model, "shared_value");
  expect_true(shared_file_statics.size() == 2, "应解析出两个同名 file-static 变量");
  expect_true(shared_file_statics[0]->compile_unit_name != shared_file_statics[1]->compile_unit_name,
              "两个同名变量应来自不同编译单元");
  expect_true(shared_file_statics[0]->address.absolute_address.has_value(), "第一个变量应有绝对地址");
  expect_true(shared_file_statics[1]->address.absolute_address.has_value(), "第二个变量应有绝对地址");
  expect_true(shared_file_statics[0]->address.absolute_address.value() !=
                shared_file_statics[1]->address.absolute_address.value(),
              "两个变量的绝对地址应不同");

  const auto private_file_statics = find_file_static_variables(model, "unit_private_value");
  expect_true(private_file_statics.size() == 2, "每个 CU 都应解析出自己的 unit_private_value");
  expect_true(private_file_statics[0]->compile_unit_name != private_file_statics[1]->compile_unit_name,
              "同名 unit_private_value 不应丢失编译单元边界");
  expect_true(private_file_statics[0]->address.absolute_address.has_value() &&
                private_file_statics[1]->address.absolute_address.has_value(),
              "同名 unit_private_value 应能解析出绝对地址");
  expect_true(private_file_statics[0]->address.absolute_address.value() !=
                private_file_statics[1]->address.absolute_address.value(),
              "不同 CU 的 unit_private_value 地址不应相同");

  const auto function_statics =
    find_variables_by_kind(model, "shared_counter", elf_static_view::VariableKind::FunctionStatic);
  expect_true(function_statics.size() == 2, "每个 CU 的函数静态变量都应被解析出来");
  expect_true(function_statics[0]->compile_unit_name != function_statics[1]->compile_unit_name,
              "同名函数静态变量应保留编译单元边界");
  expect_true(function_statics[0]->scope_path != function_statics[1]->scope_path,
              "同名函数静态变量应保留各自函数作用域");
  expect_true(function_statics[0]->address.absolute_address.has_value() &&
                function_statics[1]->address.absolute_address.has_value(),
              "函数静态变量应解析出绝对地址");

  const auto globals = find_variables_by_kind(model, "shared_global", elf_static_view::VariableKind::Global);
  expect_true(!globals.empty(), "多 CU fixture 应包含一个可见的全局变量定义");
  expect_true(globals.front()->address.absolute_address.has_value(), "全局变量定义应解析出绝对地址");
}

elf_static_view::ExpandedNode make_node(const std::string& path,
                                        const std::string& display_name,
                                        const std::string& type_name,
                                        const elf_static_view::Availability availability,
                                        const std::optional<std::uint64_t> absolute_address,
                                        const std::optional<std::int64_t> relative_offset) {
  return elf_static_view::ExpandedNode {.path = path,
                                        .display_name = display_name,
                                        .type_name = type_name,
                                        .type_kind = elf_static_view::TypeKind::Base,
                                        .availability = availability,
                                        .absolute_address = absolute_address,
                                        .relative_offset = relative_offset,
                                        .byte_size = std::nullopt,
                                        .array_count = std::nullopt,
                                        .array_stride = std::nullopt,
                                        .children = {}};
}

void verify_filter_rules() {
  elf_static_view::ui::AppState state;
  state.filters.form.variable_name_query = "shared";
  state.filters.form.path_rules_text = "demo::**\n!demo::counter";
  state.filters.form.include_runtime_only = false;
  state.filters.form.only_static_known = false;
  elf_static_view::ui::compile_filter_rules(state.filters);
  expect_true(!state.filters.compile_error.has_value(), "路径规则编译不应报错");

  const auto shared_node = make_node(
    "demo::shared", "shared", "int", elf_static_view::Availability::StaticAddressKnown, 0x1000, std::nullopt);
  const auto counter_node = make_node(
    "demo::counter", "counter", "int", elf_static_view::Availability::StaticAddressKnown, 0x1020, std::nullopt);
  const auto holder_node = make_node("other::Holder",
                                     "Holder",
                                     "int",
                                     elf_static_view::Availability::StaticAddressKnown,
                                     0x1040,
                                     std::nullopt);

  expect_true(elf_static_view::ui::matches_filters(state, shared_node), "shared 节点应命中过滤规则");
  expect_true(!elf_static_view::ui::matches_filters(state, counter_node), "counter 节点应被排除规则过滤");
  expect_true(!elf_static_view::ui::matches_filters(state, holder_node), "Holder 节点应被路径规则过滤");
}

void verify_address_bias() {
  const auto node = make_node("demo::global_value",
                              "global_value",
                              "int",
                              elf_static_view::Availability::StaticAddressKnown,
                              0x1000,
                              24);

  expect_true(elf_static_view::apply_bias_to_absolute(node, 0x20).value() == 0x1020,
              "绝对地址偏移计算错误");
  expect_true(elf_static_view::apply_bias_to_relative(node, -4).value() == 20, "相对偏移计算错误");
  expect_true(elf_static_view::format_address_summary(node, 0x20) == "0x1020",
              "地址摘要应展示偏移后的绝对地址");

  const auto high_address_node = make_node("demo::high_address",
                                           "high_address",
                                           "int",
                                           elf_static_view::Availability::StaticAddressKnown,
                                           std::numeric_limits<std::uint64_t>::max() - 0x0fU,
                                           std::nullopt);
  expect_true(elf_static_view::apply_bias_to_absolute(high_address_node, 0x0f).value() ==
                std::numeric_limits<std::uint64_t>::max(),
              "高地址加上最后一个偏移后应到达上限");
  expect_true(!elf_static_view::apply_bias_to_absolute(high_address_node, 0x10).has_value(),
              "绝对地址正向溢出应返回空");
  expect_true(elf_static_view::apply_bias_to_absolute(high_address_node, -0x10).value() ==
                std::numeric_limits<std::uint64_t>::max() - 0x1fU,
              "绝对地址负向偏移应能正常计算");
}

void expect_parse_failure(const std::string& value, const std::string& message) {
  bool failed = false;
  try {
    static_cast<void>(elf_static_view::parse_address_bias(value));
  } catch (const std::exception&) {
    failed = true;
  }
  expect_true(failed, message);
}

void verify_address_bias_parsing() {
  expect_true(elf_static_view::parse_address_bias("0") == 0, "0 应按十进制解析");
  expect_true(elf_static_view::parse_address_bias("123") == 123, "十进制偏移解析错误");
  expect_true(elf_static_view::parse_address_bias("-16") == -16, "负十进制偏移解析错误");
  expect_true(elf_static_view::parse_address_bias("0x10") == 16, "十六进制偏移解析错误");
  expect_true(elf_static_view::parse_address_bias("0X20") == 32, "大写前缀十六进制解析错误");
  expect_true(elf_static_view::parse_address_bias("-0x10") == -16, "负十六进制偏移解析错误");
  expect_true(elf_static_view::parse_address_bias(" 42 ") == 42, "偏移解析应忽略首尾空白");
  expect_true(elf_static_view::parse_address_bias("-0x8000000000000000") ==
                std::numeric_limits<std::int64_t>::min(),
              "INT64_MIN 十六进制值应能被解析");

  expect_parse_failure("", "空字符串应被拒绝");
  expect_parse_failure("0x", "只有十六进制前缀应被拒绝");
  expect_parse_failure("-0x", "只有负号和十六进制前缀应被拒绝");
  expect_parse_failure("abc", "非法字符输入应被拒绝");
  expect_parse_failure("12abc", "混合非法字符应被拒绝");
  expect_parse_failure("9223372036854775808", "超过 int64 正向范围应被拒绝");
  expect_parse_failure("-0x8000000000000001", "超过 int64 负向范围应被拒绝");
}

void verify_copy_address_formatting() {
  elf_static_view::ui::AppState state;
  expect_true(elf_static_view::ui::format_address_for_copy(0x1234U, state) == "0x1234",
              "默认复制格式应为带 0x 前缀的十六进制");

  state.copy_hex_without_prefix = true;
  expect_true(elf_static_view::ui::format_address_for_copy(0x1234U, state) == "1234",
              "启用前缀移除后，十六进制复制结果不应带 0x");

  state.copy_address_base = elf_static_view::ui::CopyAddressBase::Dec;
  expect_true(elf_static_view::ui::format_address_for_copy(0x1234U, state) == "4660",
              "十进制复制结果不正确");

  state.copy_address_base = elf_static_view::ui::CopyAddressBase::Oct;
  expect_true(elf_static_view::ui::format_address_for_copy(0x1234U, state) == "11064",
              "八进制复制结果不正确");

  state.copy_address_base = elf_static_view::ui::CopyAddressBase::Bin;
  expect_true(elf_static_view::ui::format_address_for_copy(0x1234U, state) == "1001000110100",
              "二进制复制结果不正确");
}

void verify_window_title_formatting() {
  elf_static_view::ui::AppState state;
  expect_true(elf_static_view::ui::build_window_title(state) ==
                "ElfStaticView " + elf_static_view::ui::current_version_string(),
              "未加载文件时应显示默认标题");

  state.current_file_path = "workspace/demo/build/bin/app.elf";
  expect_true(elf_static_view::ui::build_window_title(state) ==
                "ElfStaticView - .../build/bin/app.elf",
              "ELF 文件标题应显示缩写路径和文件名");

  state.current_snapshot_path = "snapshots/run/app.snapshot.json";
  expect_true(elf_static_view::ui::build_window_title(state) ==
                "ElfStaticView - snapshots/run/app.snapshot.json",
              "快照标题应优先显示当前打开的快照路径");
}

void verify_utf8_path_helpers() {
#if defined(_WIN32)
  constexpr auto sample_literal = u8"中文/静态变量.json";
  const std::string sample(reinterpret_cast<const char*>(sample_literal), sizeof(sample_literal) - 1);
  const std::wstring wide = elf_static_view::platform::utf8_to_wide(sample);
  expect_true(elf_static_view::platform::wide_to_utf8(wide) == sample, "UTF-8 与宽字符转换应可往返");

  const auto path = elf_static_view::platform::utf8_path(sample);
  expect_true(elf_static_view::platform::path_to_utf8(path) == sample, "UTF-8 路径转换应保留原始内容");
#endif
}

enum class FixtureClass {
  Elf32,
  Elf64,
};

enum class FixtureEndian {
  Little,
  Big,
};

void append_integer(std::vector<std::uint8_t>& bytes,
                    const std::uint64_t value,
                    const std::size_t size,
                    const FixtureEndian endian) {
  if (endian == FixtureEndian::Little) {
    for (std::size_t index = 0; index < size; ++index) {
      bytes.push_back(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }
    return;
  }

  for (std::size_t index = 0; index < size; ++index) {
    const auto shift = static_cast<unsigned int>((size - index - 1U) * 8U);
    bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void append_u16(std::vector<std::uint8_t>& bytes, const std::uint16_t value, const FixtureEndian endian) {
  append_integer(bytes, value, sizeof(value), endian);
}

void append_u32(std::vector<std::uint8_t>& bytes, const std::uint32_t value, const FixtureEndian endian) {
  append_integer(bytes, value, sizeof(value), endian);
}

void append_u64(std::vector<std::uint8_t>& bytes, const std::uint64_t value, const FixtureEndian endian) {
  append_integer(bytes, value, sizeof(value), endian);
}

void write_padding(std::vector<std::uint8_t>& bytes, const std::size_t target_size) {
  if (bytes.size() < target_size) {
    bytes.resize(target_size, 0U);
  }
}

void append_elf_header(std::vector<std::uint8_t>& bytes,
                       const FixtureClass elf_class,
                       const FixtureEndian endian,
                       const std::uint64_t section_header_offset,
                       const std::uint16_t section_header_size) {
  bytes.push_back(0x7fU);
  bytes.push_back(static_cast<std::uint8_t>('E'));
  bytes.push_back(static_cast<std::uint8_t>('L'));
  bytes.push_back(static_cast<std::uint8_t>('F'));
  bytes.push_back(elf_class == FixtureClass::Elf32 ? 1U : 2U);
  bytes.push_back(endian == FixtureEndian::Little ? 1U : 2U);
  bytes.push_back(1U);
  bytes.push_back(0U);
  bytes.resize(16, 0U);

  append_u16(bytes, 1U, endian);
  append_u16(bytes, elf_class == FixtureClass::Elf32 ? 3U : 62U, endian);
  append_u32(bytes, 1U, endian);
  if (elf_class == FixtureClass::Elf32) {
    append_u32(bytes, 0U, endian);
    append_u32(bytes, 0U, endian);
    append_u32(bytes, static_cast<std::uint32_t>(section_header_offset), endian);
    append_u32(bytes, 0U, endian);
    append_u16(bytes, 52U, endian);
  } else {
    append_u64(bytes, 0U, endian);
    append_u64(bytes, 0U, endian);
    append_u64(bytes, section_header_offset, endian);
    append_u32(bytes, 0U, endian);
    append_u16(bytes, 64U, endian);
  }
  append_u16(bytes, 0U, endian);
  append_u16(bytes, 0U, endian);
  append_u16(bytes, section_header_size, endian);
  append_u16(bytes, 3U, endian);
  append_u16(bytes, 0U, endian);
}

void append_section_header(std::vector<std::uint8_t>& bytes,
                           const FixtureClass elf_class,
                           const FixtureEndian endian,
                           const std::uint32_t type,
                           const std::uint64_t offset,
                           const std::uint64_t size,
                           const std::uint32_t link,
                           const std::uint64_t entry_size) {
  append_u32(bytes, 0U, endian);
  append_u32(bytes, type, endian);
  if (elf_class == FixtureClass::Elf32) {
    append_u32(bytes, 0U, endian);
    append_u32(bytes, 0U, endian);
    append_u32(bytes, static_cast<std::uint32_t>(offset), endian);
    append_u32(bytes, static_cast<std::uint32_t>(size), endian);
    append_u32(bytes, link, endian);
    append_u32(bytes, 0U, endian);
    append_u32(bytes, 0U, endian);
    append_u32(bytes, static_cast<std::uint32_t>(entry_size), endian);
    return;
  }

  append_u64(bytes, 0U, endian);
  append_u64(bytes, 0U, endian);
  append_u64(bytes, offset, endian);
  append_u64(bytes, size, endian);
  append_u32(bytes, link, endian);
  append_u32(bytes, 0U, endian);
  append_u64(bytes, 0U, endian);
  append_u64(bytes, entry_size, endian);
}

void append_symbol(std::vector<std::uint8_t>& bytes,
                   const FixtureClass elf_class,
                   const FixtureEndian endian,
                   const std::uint32_t name_offset,
                   const std::uint64_t value,
                   const std::uint64_t size,
                   const std::uint8_t info,
                   const std::uint16_t section_index) {
  append_u32(bytes, name_offset, endian);
  if (elf_class == FixtureClass::Elf32) {
    append_u32(bytes, static_cast<std::uint32_t>(value), endian);
    append_u32(bytes, static_cast<std::uint32_t>(size), endian);
    bytes.push_back(info);
    bytes.push_back(0U);
    append_u16(bytes, section_index, endian);
    return;
  }

  bytes.push_back(info);
  bytes.push_back(0U);
  append_u16(bytes, section_index, endian);
  append_u64(bytes, value, endian);
  append_u64(bytes, size, endian);
}

std::vector<std::uint8_t> build_symbol_table_fixture(const FixtureClass elf_class,
                                                     const FixtureEndian endian,
                                                     const std::uint64_t symbol_value) {
  const std::size_t header_size = elf_class == FixtureClass::Elf32 ? 52U : 64U;
  const std::size_t symbol_entry_size = elf_class == FixtureClass::Elf32 ? 16U : 24U;
  const std::size_t section_header_size = elf_class == FixtureClass::Elf32 ? 40U : 64U;
  const std::vector<std::uint8_t> string_table = {0U, 'd', 'e', 'm', 'o', '_', 'v', 'a', 'l', 'u', 'e', 0U};
  const std::size_t symbol_table_offset = header_size;
  const std::size_t string_table_offset = symbol_table_offset + symbol_entry_size;
  const std::size_t section_header_offset = string_table_offset + string_table.size();

  std::vector<std::uint8_t> bytes;
  append_elf_header(bytes,
                    elf_class,
                    endian,
                    static_cast<std::uint64_t>(section_header_offset),
                    static_cast<std::uint16_t>(section_header_size));
  write_padding(bytes, symbol_table_offset);
  append_symbol(bytes,
                elf_class,
                endian,
                1U,
                symbol_value,
                elf_class == FixtureClass::Elf32 ? 4U : 8U,
                1U,
                1U);
  write_padding(bytes, string_table_offset);
  bytes.insert(bytes.end(), string_table.begin(), string_table.end());
  write_padding(bytes, section_header_offset);
  append_section_header(bytes, elf_class, endian, 0U, 0U, 0U, 0U, 0U);
  append_section_header(bytes,
                        elf_class,
                        endian,
                        2U,
                        static_cast<std::uint64_t>(symbol_table_offset),
                        static_cast<std::uint64_t>(symbol_entry_size),
                        2U,
                        static_cast<std::uint64_t>(symbol_entry_size));
  append_section_header(bytes,
                        elf_class,
                        endian,
                        3U,
                        static_cast<std::uint64_t>(string_table_offset),
                        static_cast<std::uint64_t>(string_table.size()),
                        0U,
                        0U);
  return bytes;
}

void verify_elf_symbol_table_endian_matrix() {
  struct Case {
    FixtureClass elf_class;
    FixtureEndian endian;
    std::uint64_t value;
  };

  const std::vector<Case> cases = {
    {FixtureClass::Elf32, FixtureEndian::Little, 0x10203040ULL},
    {FixtureClass::Elf32, FixtureEndian::Big, 0x55667788ULL},
    {FixtureClass::Elf64, FixtureEndian::Little, 0x0102030405060708ULL},
    {FixtureClass::Elf64, FixtureEndian::Big, 0x8877665544332211ULL},
  };

  for (const auto& test_case : cases) {
    const auto bytes = build_symbol_table_fixture(test_case.elf_class, test_case.endian, test_case.value);
    const auto path = std::filesystem::temp_directory_path() /
                      ("elf_static_view_" + std::to_string(static_cast<int>(test_case.elf_class)) + "_" +
                       std::to_string(static_cast<int>(test_case.endian)) + ".bin");

    {
      std::ofstream output(path, std::ios::binary);
      if (!output.is_open()) {
        throw std::runtime_error("无法写入 ELF matrix fixture: " + path.string());
      }
      output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if (!output) {
        throw std::runtime_error("写入 ELF matrix fixture 失败: " + path.string());
      }
    }

    std::error_code remove_error;
    try {
      const auto table = elf_static_view::elf::ElfSymbolTable::load(path.string());
      const auto symbol = table.find("demo_value");
      expect_true(symbol.has_value(), "ELF matrix fixture 应能读到 demo_value");
      expect_true(symbol->value == test_case.value, "ELF matrix fixture 符号值解码错误");
    } catch (...) {
      std::filesystem::remove(path, remove_error);
      throw;
    }
    std::filesystem::remove(path, remove_error);
  }
}

void verify_elf_symbol_table_metadata() {
  const auto bytes = build_symbol_table_fixture(FixtureClass::Elf64, FixtureEndian::Big, 0x1020ULL);
  const auto path = std::filesystem::temp_directory_path() / "elf_static_view_metadata.bin";
  {
    std::ofstream output(path, std::ios::binary);
    if (!output.is_open()) {
      throw std::runtime_error("无法写入 ELF metadata fixture: " + path.string());
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  std::error_code remove_error;
  try {
    const auto metadata = elf_static_view::elf::ElfSymbolTable::inspect_file(path.string());
    expect_true(metadata.object_class == "ELF64", "应识别 ELF64");
    expect_true(metadata.byte_order == "BigEndian", "应识别大端");
    expect_true(metadata.file_type == "REL", "应识别 REL 类型");
  } catch (...) {
    std::filesystem::remove(path, remove_error);
    throw;
  }
  std::filesystem::remove(path, remove_error);
}

void verify_snapshot_export_redaction() {
  elf_static_view::ProjectSnapshot snapshot;
  snapshot.source_file = "D:/secret/demo.elf";
  snapshot.exported_at = "2026-05-24T10:00:00Z";
  snapshot.model.file = "D:/secret/demo.elf";
  snapshot.model.elf_info = {.object_class = "ELF64",
                             .byte_order = "LittleEndian",
                             .file_type = "EXEC",
                             .machine = "x86_64",
                             .os_abi = "SystemV"};
  elf_static_view::CompileUnitRecord compile_unit;
  compile_unit.id = "cu@0";
  compile_unit.name = "D:/src/demo.cpp";
  compile_unit.producer = "clang -g -O2";
  compile_unit.language = "C++";
  snapshot.model.compile_units.push_back(std::move(compile_unit));
  elf_static_view::VariableRecord symbol;
  symbol.id = "sym@0";
  symbol.name = "demo";
  symbol.compile_unit_name = "D:/src/demo.cpp";
  symbol.variable_kind = elf_static_view::VariableKind::Global;
  symbol.availability = elf_static_view::Availability::StaticAddressKnown;
  symbol.address.kind = elf_static_view::AddressKind::Absolute;
  symbol.address.absolute_address = 0x401000;
  symbol.type.id = "type@0";
  symbol.scope_path = {"demo"};
  symbol.has_static_storage = true;
  snapshot.model.symbols.push_back(symbol);

  const auto redacted = elf_static_view::build_export_snapshot(
    snapshot, {.include_sensitive_info = false});
  expect_true(redacted.source_file == "<redacted>", "脱敏快照应隐藏 source_file");
  expect_true(redacted.model.file == "<redacted>", "脱敏快照应隐藏 model.file");
  expect_true(redacted.model.compile_units.front().producer.empty(), "脱敏快照应清空 producer");
  expect_true(redacted.model.compile_units.front().name.empty(), "脱敏快照应清空 CU 名称");
  expect_true(redacted.model.symbols.front().compile_unit_name.empty(),
              "脱敏快照应清空变量 compile_unit_name");
  expect_true(redacted.model.symbols.front().address.absolute_address == 0x401000,
              "脱敏快照应保留地址信息");
}

void verify_ui_config_round_trip() {
  const auto temp_root = std::filesystem::temp_directory_path() /
                         ("elf-static-view-config-round-trip-" +
                          std::to_string(static_cast<unsigned long long>(
                            std::chrono::steady_clock::now().time_since_epoch().count())));
  std::filesystem::remove_all(temp_root);
  std::filesystem::create_directories(temp_root);
  const auto executable_path = temp_root / "elf-static-view.exe";

  elf_static_view::ui::AppState first_state;
  elf_static_view::ui::load_app_config(first_state, executable_path);
  first_state.persist_address_bias_to_config = true;
  first_state.address_bias_input = "0x123";
  first_state.address_bias = elf_static_view::parse_address_bias(first_state.address_bias_input);
  first_state.copy_address_base = elf_static_view::ui::CopyAddressBase::Bin;
  first_state.copy_hex_without_prefix = true;
  first_state.ui_refresh_rate = 15;
  first_state.version_check = elf_static_view::ui::VersionCheckState {
    .repository_url = "https://example.com/project",
    .check_uri = "https://example.com/releases.yaml",
    .latest_version = {},
    .release_url = {},
    .release_name = {},
    .release_notes = {},
    .message = {},
    .has_new_version = false,
    .check_uri_uses_default = false,
  };
  elf_static_view::ui::save_app_config(first_state);

  elf_static_view::ui::AppState restored_state;
  elf_static_view::ui::load_app_config(restored_state, executable_path);
  expect_true(restored_state.persist_address_bias_to_config,
              "地址偏移写回应当从配置文件恢复");
  expect_true(restored_state.address_bias == elf_static_view::parse_address_bias("0x123"),
              "地址偏移数值应当从配置文件恢复");
  expect_true(restored_state.address_bias_input == "0x123",
              "地址偏移输入框文本应当从配置文件恢复");
  expect_true(restored_state.copy_address_base == elf_static_view::ui::CopyAddressBase::Bin,
              "复制进制应当从配置文件恢复");
  expect_true(restored_state.copy_hex_without_prefix,
              "十六进制复制前缀设置应当从配置文件恢复");
  expect_true(restored_state.ui_refresh_rate == 15,
              "界面刷新率应当从配置文件恢复");
  expect_true(restored_state.version_check.has_value(),
              "版本检查 URI 应当从配置文件恢复");
  expect_true(restored_state.version_check->repository_url == "https://example.com/project",
              "仓库地址恢复值不正确");
  expect_true(restored_state.version_check->check_uri == "https://example.com/releases.yaml",
              "版本检查 URI 恢复值不正确");

  restored_state.persist_address_bias_to_config = false;
  restored_state.address_bias_input = "0x456";
  restored_state.address_bias = elf_static_view::parse_address_bias(restored_state.address_bias_input);
  elf_static_view::ui::save_app_config(restored_state);

  elf_static_view::ui::AppState disabled_state;
  elf_static_view::ui::load_app_config(disabled_state, executable_path);
  expect_true(!disabled_state.persist_address_bias_to_config,
              "关闭地址偏移写回后，下次启动不应再启用");
  expect_true(disabled_state.address_bias == 0, "关闭地址偏移写回后，不应恢复旧地址偏移");
  expect_true(disabled_state.address_bias_input == "0",
              "关闭地址偏移写回后，地址偏移输入应保持默认值");
  expect_true(disabled_state.copy_address_base == elf_static_view::ui::CopyAddressBase::Bin,
              "关闭地址偏移写回不应影响复制进制恢复");
  expect_true(disabled_state.copy_hex_without_prefix,
              "关闭地址偏移写回不应影响十六进制复制前缀设置");
  expect_true(disabled_state.ui_refresh_rate == 15,
              "关闭地址偏移写回不应影响界面刷新率恢复");

  std::filesystem::remove_all(temp_root);
}

void verify_version_check_resolution() {
  const auto& defaults = elf_static_view::ui::default_release_metadata();

  elf_static_view::ui::AppState default_state;
  const auto resolved_default = elf_static_view::ui::resolve_version_check_state(default_state);
  expect_true(resolved_default.repository_url == defaults.repository_url,
              "默认仓库地址应当回退到 GitHub 仓库");
  expect_true(resolved_default.check_uri == defaults.releases_api_url,
              "默认版本检查地址应当回退到 GitHub Releases API");
  expect_true(resolved_default.check_uri_uses_default,
              "默认版本检查来源应当标记为默认 GitHub");

  elf_static_view::ui::AppState custom_state;
  custom_state.version_check = elf_static_view::ui::VersionCheckState {
    .repository_url = "https://example.com/custom-repo",
    .check_uri = {},
    .latest_version = {},
    .release_url = {},
    .release_name = {},
    .release_notes = {},
    .message = {},
    .has_new_version = false,
    .check_uri_uses_default = true,
  };
  const auto resolved_custom = elf_static_view::ui::resolve_version_check_state(custom_state);
  expect_true(resolved_custom.repository_url == "https://example.com/custom-repo",
              "配置仓库地址后，应当优先显示配置值");
  expect_true(resolved_custom.check_uri == defaults.releases_api_url,
              "仅配置仓库地址时，版本检查地址仍应回退到默认 GitHub API");
}

void verify_version_response_parsing() {
  const auto custom_result = elf_static_view::ui::parse_version_response_text(
    "latest_version: 1.2.3\nrelease_url: https://example.com/releases/1.2.3\nname: Example Release\n"
    "body: Example Notes\n",
    "https://example.com/releases.yaml",
    "https://example.com/project");
  expect_true(custom_result.latest_version == "1.2.3",
              "自定义版本响应应当解析 latest_version");
  expect_true(custom_result.release_url == "https://example.com/releases/1.2.3",
              "自定义版本响应应当解析 release_url");
  expect_true(custom_result.release_name == "Example Release",
              "自定义版本响应应当解析 name");
  expect_true(custom_result.release_notes == "Example Notes",
              "自定义版本响应应当解析 body");

  const auto github_result = elf_static_view::ui::parse_version_response_text(
    R"({"tag_name":"v2.0.0","html_url":"https://github.com/HamsterAPig/ElfStaticView/releases/tag/v2.0.0","name":"v2.0.0 Windows","body":"GitHub Release Notes"})",
    "https://api.github.com/repos/HamsterAPig/ElfStaticView/releases/latest",
    "https://github.com/HamsterAPig/ElfStaticView");
  expect_true(github_result.latest_version == "v2.0.0",
              "GitHub Releases JSON 应当解析 tag_name");
  expect_true(github_result.release_url ==
                "https://github.com/HamsterAPig/ElfStaticView/releases/tag/v2.0.0",
              "GitHub Releases JSON 应当解析 html_url");
  expect_true(github_result.release_name == "v2.0.0 Windows",
              "GitHub Releases JSON 应当解析 name");
  expect_true(github_result.release_notes == "GitHub Release Notes",
              "GitHub Releases JSON 应当解析 body");
}

void verify_version_compare_rules() {
  expect_true(elf_static_view::ui::compare_version_strings("v0.1.0", "0.1.0") == 0,
              "版本比较应当兼容可选的 v 前缀");
  expect_true(elf_static_view::ui::compare_version_strings("0.1.0+abcd1234", "0.1.0") == 0,
              "开发态版本不应被误判成比同基线 tag 更新");
  expect_true(elf_static_view::ui::compare_version_strings("0.1.0+abcd1234", "v0.1.1") < 0,
              "开发态版本仍应识别到更高的发布版本");
}

}  // namespace

int main() {
  try {
    verify_fixture(ELF_STATIC_VIEW_C_FIXTURE_PATH, ELF_STATIC_VIEW_C_EXPECTED_JSON);
    verify_fixture(ELF_STATIC_VIEW_CPP_FIXTURE_PATH, ELF_STATIC_VIEW_CPP_EXPECTED_JSON);
    verify_dump_text_contains_elf_info_any_class(ELF_STATIC_VIEW_DEBUG_SUP_FIXTURE_PATH, "ELF32", "LittleEndian");
    verify_bitfield_layout_fixture();
    verify_gnu_addr_index_fixture();
    verify_ref_sig8_debug_types_fixture();
    verify_ref_sig8_indirect_fixture();
    verify_gcc_ref_addr_fixture();
    verify_gcc_small_ref_fixture(ELF_STATIC_VIEW_GCC_DWARF5_REF1_FIXTURE_PATH, "ref1");
    verify_gcc_small_ref_fixture(ELF_STATIC_VIEW_GCC_DWARF5_REF2_FIXTURE_PATH, "ref2");
    verify_gcc_small_ref_fixture(ELF_STATIC_VIEW_GCC_DWARF5_REF_UDATA_FIXTURE_PATH, "ref_udata");
    verify_gcc_ref_sup4_fixture();
    verify_gcc_ref8_fixture();
    verify_gcc_ref_sup8_fixture();
    verify_gcc_gnu_alt_fixture();
    verify_gcc_line_strp_fixture();
    verify_dwarf5_strx1_fixture();
    verify_dwarf5_strx_form_fixture(ELF_STATIC_VIEW_DWARF5_STRX2_FIXTURE_PATH, "DW_FORM_strx2", "dwarf5 strx2 fixture");
    verify_dwarf5_strx_form_fixture(ELF_STATIC_VIEW_DWARF5_STRX3_FIXTURE_PATH, "DW_FORM_strx3", "dwarf5 strx3 fixture");
    verify_dwarf5_strx_form_fixture(ELF_STATIC_VIEW_DWARF5_STRX4_FIXTURE_PATH, "DW_FORM_strx4", "dwarf5 strx4 fixture");
    verify_dwarf5_addrx_fixture();
    verify_dwarf5_addrx_form_fixture(ELF_STATIC_VIEW_DWARF5_ADDRX1_FIXTURE_PATH, DW_FORM_addrx1, "dwarf5 addrx1 fixture");
    verify_dwarf5_addrx_form_fixture(ELF_STATIC_VIEW_DWARF5_ADDRX2_FIXTURE_PATH, DW_FORM_addrx2, "dwarf5 addrx2 fixture");
    verify_dwarf5_addrx_form_fixture(ELF_STATIC_VIEW_DWARF5_ADDRX3_FIXTURE_PATH, DW_FORM_addrx3, "dwarf5 addrx3 fixture");
    verify_dwarf5_addrx_form_fixture(ELF_STATIC_VIEW_DWARF5_ADDRX4_FIXTURE_PATH, DW_FORM_addrx4, "dwarf5 addrx4 fixture");
    verify_rnglistx_fixture();
    verify_rnglists_start_end_fixture();
    verify_rnglists_offset_pair_fixture();
    verify_rnglists_base_addressx_fixture();
    verify_rnglists_startx_endx_fixture();
    verify_rnglists_startx_length_fixture();
    verify_implicit_const_fixture();
    verify_dwarf5_strx_type_unit_fixture();
    verify_dwarf5_strx_type_unit_abbrev_offset_fixture();
    verify_split_dwarf_fixture();
    verify_split_dwp_fixture();
    verify_debug_sup_fixture();
    verify_debug_sup_sidecar_fixture();
    verify_gcc_strp_sup_fixture();
    verify_current_supplementary_gap_notes();
    verify_member_pointer_type_fixture();
    verify_const_value_text_json_round_trip();
    verify_value_expression_text_rendering();
    verify_named_location_op_rendering();
    verify_register_location_text_rendering();
    verify_register_address_text_rendering();
    verify_unspecified_type_fixture();
    verify_atomic_type_fixture();
    verify_abstract_origin_const_value_fixture();
    verify_multi_cu_file_static_fixture();
    verify_inline_scope_static_fixture();
    verify_const_value_fixture();
    verify_piece_stack_value_fixture();
    verify_bit_piece_fixture();
    verify_dwarf5_loclists_fixture();
    verify_dwarf5_loclists_base_default_fixture();
    verify_dwarf5_loclists_base_addressx_fixture();
    verify_dwarf5_loclists_start_end_fixture();
    verify_dwarf5_loclists_startx_endx_fixture();
    verify_dwarf5_loclists_startx_length_fixture();
    verify_dwarf5_loclists_start_length_fixture();
    verify_specification_fixture();
    verify_thread_local_fixture();
    verify_json_round_trip(ELF_STATIC_VIEW_CPP_FIXTURE_PATH);
    verify_dump_text_contains_elf_info(ELF_STATIC_VIEW_CPP_FIXTURE_PATH);
    verify_loader_error_contains_file_path();
    verify_filter_rules();
    verify_address_bias();
    verify_address_bias_parsing();
    verify_copy_address_formatting();
    verify_window_title_formatting();
    verify_utf8_path_helpers();
    verify_elf_symbol_table_endian_matrix();
    verify_elf_symbol_table_metadata();
    verify_snapshot_export_redaction();
    verify_ui_config_round_trip();
    verify_version_check_resolution();
    verify_version_response_parsing();
    verify_version_compare_rules();
    std::cout << "all tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
