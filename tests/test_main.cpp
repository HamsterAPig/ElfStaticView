#include "analysis/address_bias.hpp"
#include "elf/elf_symbol_table.hpp"
#include "elf_static_view/project.hpp"
#include "platform/utf8.hpp"
#include "ui/filter_matcher.hpp"
#include "ui/version_check.hpp"

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
  snapshot.model.compile_units.push_back(
    {.id = "cu@0", .name = "D:/src/demo.cpp", .producer = "clang -g -O2", .language = "C++"});
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
  const auto temp_root =
    std::filesystem::temp_directory_path() / "elf-static-view-config-round-trip";
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
    verify_multi_cu_file_static_fixture();
    verify_json_round_trip(ELF_STATIC_VIEW_CPP_FIXTURE_PATH);
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
