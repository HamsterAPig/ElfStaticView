#include "elf/elf_symbol_table.hpp"
#include "elf_static_view/project.hpp"
#include "platform/utf8.hpp"
#include "ui/filter_matcher.hpp"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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
  const auto reparsed = elf_static_view::parse_dump_json(json);
  expect_true(elf_static_view::render_dump_json(reparsed) == json, "dump JSON round-trip 应保持一致");

  elf_static_view::ProjectSnapshot snapshot;
  snapshot.source_file = fixture_path;
  snapshot.exported_at = "2026-05-23T12:00:00Z";
  snapshot.model = model;
  const auto snapshot_json = elf_static_view::render_snapshot_json(snapshot);
  const auto reparsed_snapshot = elf_static_view::parse_snapshot_json(snapshot_json);
  expect_true(reparsed_snapshot.source_file == fixture_path, "snapshot source_file 应保持一致");
  expect_true(elf_static_view::render_dump_json(reparsed_snapshot.model) == json,
              "snapshot 中的 model 应保持一致");
}

void verify_filter_rules() {
  elf_static_view::ui::AppState state;
  state.filters.form.path_rules_text = "demo::Derived::**\n!**::counter";
  state.filters.form.variable_name_query = "shared";
  elf_static_view::ui::compile_filter_rules(state.filters);
  expect_true(!state.filters.compile_error.has_value(), "路径规则应编译成功");

  elf_static_view::ExpandedNode shared_node{
    .path = "demo::Derived::shared",
    .display_name = "shared",
    .type_name = "int",
    .type_kind = elf_static_view::TypeKind::Base,
    .availability = elf_static_view::Availability::StaticAddressKnown,
    .absolute_address = 0x1000,
    .relative_offset = std::nullopt,
    .byte_size = std::nullopt,
    .array_count = std::nullopt,
    .array_stride = std::nullopt,
    .children = {},
  };
  elf_static_view::ExpandedNode counter_node{
    .path = "demo::Derived::counter",
    .display_name = "counter",
    .type_name = "int",
    .type_kind = elf_static_view::TypeKind::Base,
    .availability = elf_static_view::Availability::StaticAddressKnown,
    .absolute_address = 0x1010,
    .relative_offset = std::nullopt,
    .byte_size = std::nullopt,
    .array_count = std::nullopt,
    .array_stride = std::nullopt,
    .children = {},
  };
  elf_static_view::ExpandedNode holder_node{
    .path = "demo::Holder::shared",
    .display_name = "shared",
    .type_name = "int",
    .type_kind = elf_static_view::TypeKind::Base,
    .availability = elf_static_view::Availability::StaticAddressKnown,
    .absolute_address = 0x1020,
    .relative_offset = std::nullopt,
    .byte_size = std::nullopt,
    .array_count = std::nullopt,
    .array_stride = std::nullopt,
    .children = {},
  };

  expect_true(elf_static_view::ui::matches_filters(state, shared_node), "shared 节点应命中包含规则");
  expect_true(!elf_static_view::ui::matches_filters(state, counter_node), "counter 节点应被排除规则过滤");
  expect_true(!elf_static_view::ui::matches_filters(state, holder_node), "Holder 节点应因路径规则不匹配被过滤");
}

void verify_address_bias() {
  elf_static_view::ExpandedNode node{
    .path = "demo::global_value",
    .display_name = "global_value",
    .type_name = "int",
    .type_kind = elf_static_view::TypeKind::Base,
    .availability = elf_static_view::Availability::StaticAddressKnown,
    .absolute_address = 0x1000,
    .relative_offset = 24,
    .byte_size = std::nullopt,
    .array_count = std::nullopt,
    .array_stride = std::nullopt,
    .children = {},
  };

  expect_true(elf_static_view::ui::apply_bias_to_absolute(node, 0x20).value() == 0x1020,
              "绝对地址偏移计算错误");
  expect_true(elf_static_view::ui::apply_bias_to_relative(node, -4).value() == 20,
              "相对地址偏移计算错误");
  expect_true(elf_static_view::ui::format_address_summary(node, 0x20) == "0x1020",
              "地址摘要应显示偏移后的绝对地址");

  elf_static_view::ExpandedNode high_address_node{
    .path = "demo::high_address",
    .display_name = "high_address",
    .type_name = "int",
    .type_kind = elf_static_view::TypeKind::Base,
    .availability = elf_static_view::Availability::StaticAddressKnown,
    .absolute_address = 0xffff'ffff'ffff'fff0ULL,
    .relative_offset = std::nullopt,
    .byte_size = std::nullopt,
    .array_count = std::nullopt,
    .array_stride = std::nullopt,
    .children = {},
  };

  expect_true(elf_static_view::ui::apply_bias_to_absolute(high_address_node, 0x0f).value() ==
                0xffff'ffff'ffff'ffffULL,
              "高地址加正偏移应保持 uint64_t 语义");
  expect_true(!elf_static_view::ui::apply_bias_to_absolute(high_address_node, 0x10).has_value(),
              "高地址上溢时应返回空结果");
  expect_true(elf_static_view::ui::apply_bias_to_absolute(high_address_node, -0x10).value() ==
                0xffff'ffff'ffff'ffe0ULL,
              "高地址减偏移应保持 uint64_t 语义");
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

void append_u16_le(std::vector<std::uint8_t>& bytes, const std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32_le(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void write_padding(std::vector<std::uint8_t>& bytes, const std::size_t target_size) {
  if (bytes.size() < target_size) {
    bytes.resize(target_size, 0U);
  }
}

void append_elf32_header(std::vector<std::uint8_t>& bytes, const std::uint32_t section_header_offset) {
  bytes.push_back(0x7fU);
  bytes.push_back(static_cast<std::uint8_t>('E'));
  bytes.push_back(static_cast<std::uint8_t>('L'));
  bytes.push_back(static_cast<std::uint8_t>('F'));
  bytes.push_back(1U);
  bytes.push_back(1U);
  bytes.push_back(1U);
  bytes.push_back(0U);
  bytes.resize(16, 0U);

  append_u16_le(bytes, 1U);
  append_u16_le(bytes, 40U);
  append_u32_le(bytes, 1U);
  append_u32_le(bytes, 0U);
  append_u32_le(bytes, 0U);
  append_u32_le(bytes, section_header_offset);
  append_u32_le(bytes, 0U);
  append_u16_le(bytes, 52U);
  append_u16_le(bytes, 0U);
  append_u16_le(bytes, 0U);
  append_u16_le(bytes, 40U);
  append_u16_le(bytes, 3U);
  append_u16_le(bytes, 0U);
}

void append_elf32_section_header(std::vector<std::uint8_t>& bytes,
                                 const std::uint32_t type,
                                 const std::uint32_t offset,
                                 const std::uint32_t size,
                                 const std::uint32_t link,
                                 const std::uint32_t entry_size) {
  append_u32_le(bytes, 0U);
  append_u32_le(bytes, type);
  append_u32_le(bytes, 0U);
  append_u32_le(bytes, 0U);
  append_u32_le(bytes, offset);
  append_u32_le(bytes, size);
  append_u32_le(bytes, link);
  append_u32_le(bytes, 0U);
  append_u32_le(bytes, 0U);
  append_u32_le(bytes, entry_size);
}

void append_elf32_symbol(std::vector<std::uint8_t>& bytes,
                         const std::uint32_t name_offset,
                         const std::uint32_t value,
                         const std::uint32_t size,
                         const std::uint8_t info,
                         const std::uint16_t section_index) {
  append_u32_le(bytes, name_offset);
  append_u32_le(bytes, value);
  append_u32_le(bytes, size);
  bytes.push_back(info);
  bytes.push_back(0U);
  append_u16_le(bytes, section_index);
}

void verify_elf32_symbol_table_support() {
  constexpr std::uint32_t symbol_table_offset = 0x40U;
  constexpr std::uint32_t string_table_offset = 0x70U;
  constexpr std::uint32_t section_header_offset = 0x90U;
  const std::string string_table = std::string("\0g_counter\0tls_counter\0", 23);

  std::vector<std::uint8_t> bytes;
  append_elf32_header(bytes, section_header_offset);
  write_padding(bytes, symbol_table_offset);

  append_elf32_symbol(bytes, 0U, 0U, 0U, 0U, 0U);
  append_elf32_symbol(bytes, 1U, 0x20000010U, 4U, 1U, 1U);
  append_elf32_symbol(bytes, 11U, 0x20000020U, 4U, 6U, 1U);

  write_padding(bytes, string_table_offset);
  bytes.insert(bytes.end(), string_table.begin(), string_table.end());

  write_padding(bytes, section_header_offset);
  append_elf32_section_header(bytes, 0U, 0U, 0U, 0U, 0U);
  append_elf32_section_header(bytes, 2U, symbol_table_offset, 48U, 2U, 16U);
  append_elf32_section_header(
    bytes,
    3U,
    string_table_offset,
    static_cast<std::uint32_t>(string_table.size()),
    0U,
    0U);

  const auto path = std::filesystem::temp_directory_path() / "elf_static_view_elf32_symtab_test.elf";
  {
    std::ofstream output(path, std::ios::binary);
    if (!output.is_open()) {
      throw std::runtime_error("无法写入 ELF32 测试夹具");
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      throw std::runtime_error("写入 ELF32 测试夹具失败");
    }
  }

  const auto table = elf_static_view::elf::ElfSymbolTable::load(path.string());
  const auto object_symbol = table.find("g_counter");
  expect_true(object_symbol.has_value(), "ELF32 对象符号应可被解析");
  expect_true(object_symbol->value == 0x20000010U, "ELF32 对象符号地址应保留");
  expect_true(object_symbol->size == 4U, "ELF32 对象符号大小应保留");
  expect_true(!object_symbol->is_thread_local, "普通对象符号不应被标记为 TLS");

  const auto tls_symbol = table.find("tls_counter");
  expect_true(tls_symbol.has_value(), "ELF32 TLS 符号应可被解析");
  expect_true(tls_symbol->is_thread_local, "TLS 符号应被标记为 TLS");

  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);
}

}  // namespace

int main() {
  try {
    verify_fixture(ELF_STATIC_VIEW_C_FIXTURE_PATH, ELF_STATIC_VIEW_C_EXPECTED_JSON);
    verify_fixture(ELF_STATIC_VIEW_CPP_FIXTURE_PATH, ELF_STATIC_VIEW_CPP_EXPECTED_JSON);
    verify_json_round_trip(ELF_STATIC_VIEW_CPP_FIXTURE_PATH);
    verify_filter_rules();
    verify_address_bias();
    verify_utf8_path_helpers();
    verify_elf32_symbol_table_support();
    std::cout << "all tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
