#include "elf/elf_symbol_table.hpp"

#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace elf_static_view::elf {

namespace {

struct Elf64Header {
  std::array<unsigned char, 16> ident {};
  std::uint16_t type = 0;
  std::uint16_t machine = 0;
  std::uint32_t version = 0;
  std::uint64_t entry = 0;
  std::uint64_t phoff = 0;
  std::uint64_t shoff = 0;
  std::uint32_t flags = 0;
  std::uint16_t ehsize = 0;
  std::uint16_t phentsize = 0;
  std::uint16_t phnum = 0;
  std::uint16_t shentsize = 0;
  std::uint16_t shnum = 0;
  std::uint16_t shstrndx = 0;
};

struct Elf64SectionHeader {
  std::uint32_t name = 0;
  std::uint32_t type = 0;
  std::uint64_t flags = 0;
  std::uint64_t addr = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint32_t link = 0;
  std::uint32_t info = 0;
  std::uint64_t addralign = 0;
  std::uint64_t entsize = 0;
};

struct Elf64Symbol {
  std::uint32_t name = 0;
  unsigned char info = 0;
  unsigned char other = 0;
  std::uint16_t shndx = 0;
  std::uint64_t value = 0;
  std::uint64_t size = 0;
};

constexpr std::uint32_t kSectionTypeSymtab = 2;
constexpr unsigned char kSymbolTypeObject = 1;
constexpr unsigned char kSymbolTypeTls = 6;
constexpr unsigned char kElfClass64 = 2;
constexpr unsigned char kElfDataLittleEndian = 1;

template <typename T>
T read_struct(std::ifstream& input) {
  T value {};
  input.read(reinterpret_cast<char*>(&value), sizeof(T));
  if (!input) {
    throw std::runtime_error("读取 ELF 结构失败");
  }
  return value;
}

std::string read_c_string(const std::vector<char>& table, const std::uint32_t offset) {
  if (offset >= table.size()) {
    return {};
  }
  return std::string(table.data() + offset);
}

}  // namespace

ElfSymbolTable ElfSymbolTable::load(const std::string& file_path) {
  std::ifstream input(file_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("无法打开 ELF 文件: " + file_path);
  }

  const auto header = read_struct<Elf64Header>(input);
  if (header.ident[0] != 0x7f || header.ident[1] != 'E' || header.ident[2] != 'L' ||
      header.ident[3] != 'F' || header.ident[4] != kElfClass64 ||
      header.ident[5] != kElfDataLittleEndian) {
    throw std::runtime_error("当前仅支持 ELF64 little-endian");
  }

  input.seekg(static_cast<std::streamoff>(header.shoff), std::ios::beg);
  std::vector<Elf64SectionHeader> sections;
  sections.reserve(header.shnum);
  for (std::uint16_t index = 0; index < header.shnum; ++index) {
    sections.push_back(read_struct<Elf64SectionHeader>(input));
  }

  ElfSymbolTable table;
  for (const auto& section : sections) {
    if (section.type != kSectionTypeSymtab || section.entsize == 0 || section.link >= sections.size()) {
      continue;
    }

    const auto& string_table_section = sections[section.link];
    std::vector<char> string_table(static_cast<std::size_t>(string_table_section.size), '\0');
    input.seekg(static_cast<std::streamoff>(string_table_section.offset), std::ios::beg);
    input.read(string_table.data(), static_cast<std::streamsize>(string_table.size()));
    if (!input) {
      throw std::runtime_error("读取 ELF 字符串表失败");
    }

    const auto symbol_count = section.size / section.entsize;
    input.seekg(static_cast<std::streamoff>(section.offset), std::ios::beg);
    for (std::uint64_t index = 0; index < symbol_count; ++index) {
      const auto symbol = read_struct<Elf64Symbol>(input);
      const auto symbol_type = static_cast<unsigned char>(symbol.info & 0x0f);
      if ((symbol_type != kSymbolTypeObject && symbol_type != kSymbolTypeTls) || symbol.shndx == 0) {
        continue;
      }
      const auto name = read_c_string(string_table, symbol.name);
      if (name.empty()) {
        continue;
      }
      table.symbols_[name] = SymbolInfo{.name = name,
                                        .value = symbol.value,
                                        .size = symbol.size,
                                        .is_thread_local = symbol_type == kSymbolTypeTls};
    }
  }

  return table;
}

std::optional<SymbolInfo> ElfSymbolTable::find(const std::string& name) const {
  const auto iter = symbols_.find(name);
  if (iter == symbols_.end()) {
    return std::nullopt;
  }
  return iter->second;
}

}  // namespace elf_static_view::elf
