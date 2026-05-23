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
  first_state.version_check = elf_static_view::ui::VersionCheckState {
    .check_uri = "https://example.com/releases.yaml",
    .latest_version = {},
    .release_url = {},
    .message = {},
    .has_new_version = false,
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
  expect_true(restored_state.version_check.has_value(),
              "版本检查 URI 应当从配置文件恢复");
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

  std::filesystem::remove_all(temp_root);
}

}  // namespace

int main() {
  try {
    verify_fixture(ELF_STATIC_VIEW_C_FIXTURE_PATH, ELF_STATIC_VIEW_C_EXPECTED_JSON);
    verify_fixture(ELF_STATIC_VIEW_CPP_FIXTURE_PATH, ELF_STATIC_VIEW_CPP_EXPECTED_JSON);
    verify_json_round_trip(ELF_STATIC_VIEW_CPP_FIXTURE_PATH);
    verify_filter_rules();
    verify_address_bias();
    verify_address_bias_parsing();
    verify_utf8_path_helpers();
    verify_elf_symbol_table_endian_matrix();
    verify_ui_config_round_trip();
    std::cout << "all tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
