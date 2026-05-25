#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <dwarf.h>
#include <libdwarf.h>
#include <libdwarfp.h>

namespace {

struct Options {
  std::string output_path;
  std::string sup_filename = "placeholder.sup";
};

std::vector<std::string> g_section_names;

bool parse_args(const int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--output" && index + 1 < argc) {
      options.output_path = argv[++index];
      continue;
    }
    if (arg == "--sup-filename" && index + 1 < argc) {
      options.sup_filename = argv[++index];
      continue;
    }
    std::cerr << "未知参数: " << arg << '\n';
    return false;
  }
  return !options.output_path.empty();
}

int writer_callback(const char* name,
                    int,
                    Dwarf_Unsigned,
                    Dwarf_Unsigned,
                    Dwarf_Unsigned,
                    Dwarf_Unsigned,
                    Dwarf_Unsigned* section_symbol_index,
                    void*,
                    int*) {
  static Dwarf_Unsigned next_section_index = 1;
  if (section_symbol_index != nullptr) {
    *section_symbol_index = next_section_index;
  }
  if (g_section_names.size() <= next_section_index) {
    g_section_names.resize(static_cast<std::size_t>(next_section_index + 1));
  }
  g_section_names[static_cast<std::size_t>(next_section_index)] = std::string(name ? name : "");
  return static_cast<int>(next_section_index++);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_args(argc, argv, options)) {
    std::cerr << "用法: debug_sup_form_builder --output <file>\n";
    return 2;
  }

  Dwarf_P_Debug dbg = nullptr;
  Dwarf_Error error = nullptr;
  constexpr unsigned long flags =
    DW_DLC_TARGET_LITTLEENDIAN | DW_DLC_POINTER64 | DW_DLC_OFFSET32 | DW_DLC_ELF_OFFSET_SIZE_64 |
    DW_DLC_SYMBOLIC_RELOCATIONS;
  int result = dwarf_producer_init(flags,
                                   writer_callback,
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   const_cast<char*>("x86"),
                                   const_cast<char*>("V5"),
                                   const_cast<char*>(""),
                                   &dbg,
                                   &error);
  if (result != DW_DLV_OK || dbg == nullptr) {
    std::cerr << "dwarf_producer_init failed\n";
    return 1;
  }

  const char checksum[] = "placeholder";
  result = dwarf_add_debug_sup(dbg,
                               2,
                               0,
                               const_cast<char*>(options.sup_filename.c_str()),
                               sizeof(checksum),
                               reinterpret_cast<Dwarf_Small*>(const_cast<char*>(checksum)),
                               &error);
  if (result != DW_DLV_OK) {
    std::cerr << "dwarf_add_debug_sup failed\n";
    dwarf_producer_finish_a(dbg, &error);
    return 1;
  }

  Dwarf_Unsigned section_count = 0;
  Dwarf_Error finish_error = nullptr;
  result = dwarf_transform_to_disk_form_a(dbg, &section_count, &finish_error);
  if (result != DW_DLV_OK) {
    std::cerr << "dwarf_transform_to_disk_form_a failed\n";
    dwarf_producer_finish_a(dbg, &error);
    return 1;
  }

  std::optional<std::vector<std::uint8_t>> debug_sup_bytes;
  for (Dwarf_Unsigned section_index = 0; section_index < section_count; ++section_index) {
    Dwarf_Unsigned elf_section_index = 0;
    Dwarf_Unsigned length = 0;
    Dwarf_Ptr bytes = nullptr;
    if (dwarf_get_section_bytes_a(
          dbg, static_cast<Dwarf_Signed>(section_index), &elf_section_index, &length, &bytes, &error) !=
        DW_DLV_OK) {
      std::cerr << "dwarf_get_section_bytes_a failed\n";
      dwarf_producer_finish_a(dbg, &error);
      return 1;
    }
    const auto mapped_index = static_cast<std::size_t>(elf_section_index);
    const auto section_name =
      mapped_index < g_section_names.size() ? g_section_names[mapped_index] : std::string();
    if (section_name == ".debug_sup" && bytes != nullptr && length > 0) {
      debug_sup_bytes = std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes),
        reinterpret_cast<const std::uint8_t*>(bytes) + length);
    }
  }
  dwarf_producer_finish_a(dbg, &error);

  if (!debug_sup_bytes.has_value()) {
    std::cerr << "未找到 .debug_sup section bytes\n";
    return 1;
  }

  std::ofstream output(options.output_path, std::ios::binary);
  if (!output.is_open()) {
    std::cerr << "无法写出文件: " << options.output_path << '\n';
    return 1;
  }
  output.write(reinterpret_cast<const char*>(debug_sup_bytes->data()),
               static_cast<std::streamsize>(debug_sup_bytes->size()));
  if (!output.good()) {
    std::cerr << "写出失败: " << options.output_path << '\n';
    return 1;
  }
  return 0;
}
