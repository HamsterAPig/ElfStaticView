#include "elf/elf_symbol_table.hpp"

#include "platform/utf8.hpp"

#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace elf_static_view::elf {

namespace {

using ElfIdent = std::array<unsigned char, 16>;

struct Elf32Header {
  ElfIdent ident {};
  std::uint16_t type = 0;
  std::uint16_t machine = 0;
  std::uint32_t version = 0;
  std::uint32_t entry = 0;
  std::uint32_t phoff = 0;
  std::uint32_t shoff = 0;
  std::uint32_t flags = 0;
  std::uint16_t ehsize = 0;
  std::uint16_t phentsize = 0;
  std::uint16_t phnum = 0;
  std::uint16_t shentsize = 0;
  std::uint16_t shnum = 0;
  std::uint16_t shstrndx = 0;
};

struct Elf64Header {
  ElfIdent ident {};
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

struct Elf32SectionHeader {
  std::uint32_t name = 0;
  std::uint32_t type = 0;
  std::uint32_t flags = 0;
  std::uint32_t addr = 0;
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
  std::uint32_t link = 0;
  std::uint32_t info = 0;
  std::uint32_t addralign = 0;
  std::uint32_t entsize = 0;
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

struct Elf32Symbol {
  std::uint32_t name = 0;
  std::uint32_t value = 0;
  std::uint32_t size = 0;
  unsigned char info = 0;
  unsigned char other = 0;
  std::uint16_t shndx = 0;
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
constexpr unsigned char kElfClass32 = 1;
constexpr unsigned char kElfClass64 = 2;
constexpr unsigned char kElfDataLittleEndian = 1;

struct SectionDescriptor {
  std::uint32_t type = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint32_t link = 0;
  std::uint64_t entsize = 0;
};

struct SymbolDescriptor {
  std::uint32_t name = 0;
  unsigned char info = 0;
  std::uint16_t shndx = 0;
  std::uint64_t value = 0;
  std::uint64_t size = 0;
};

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

SectionDescriptor to_descriptor(const Elf32SectionHeader& section) {
  return SectionDescriptor{.type = section.type,
                           .offset = section.offset,
                           .size = section.size,
                           .link = section.link,
                           .entsize = section.entsize};
}

SectionDescriptor to_descriptor(const Elf64SectionHeader& section) {
  return SectionDescriptor{.type = section.type,
                           .offset = section.offset,
                           .size = section.size,
                           .link = section.link,
                           .entsize = section.entsize};
}

SymbolDescriptor to_descriptor(const Elf32Symbol& symbol) {
  return SymbolDescriptor{.name = symbol.name,
                          .info = symbol.info,
                          .shndx = symbol.shndx,
                          .value = symbol.value,
                          .size = symbol.size};
}

SymbolDescriptor to_descriptor(const Elf64Symbol& symbol) {
  return SymbolDescriptor{.name = symbol.name,
                          .info = symbol.info,
                          .shndx = symbol.shndx,
                          .value = symbol.value,
                          .size = symbol.size};
}

void validate_elf_ident(const ElfIdent& ident) {
  if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F') {
    throw std::runtime_error("文件不是 ELF 格式");
  }
  if (ident[5] != kElfDataLittleEndian) {
    throw std::runtime_error("当前仅支持 little-endian ELF");
  }
  if (ident[4] != kElfClass32 && ident[4] != kElfClass64) {
    throw std::runtime_error("当前仅支持 ELF32/ELF64");
  }
}

template <typename Header, typename SectionHeader, typename Symbol>
std::unordered_map<std::string, SymbolInfo> load_symbol_table(std::ifstream& input) {
  const auto header = read_struct<Header>(input);
  std::vector<SectionDescriptor> sections;
  sections.reserve(header.shnum);

  input.seekg(static_cast<std::streamoff>(header.shoff), std::ios::beg);
  for (std::uint16_t index = 0; index < header.shnum; ++index) {
    sections.push_back(to_descriptor(read_struct<SectionHeader>(input)));
  }

  std::unordered_map<std::string, SymbolInfo> symbols;
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
      const auto symbol = to_descriptor(read_struct<Symbol>(input));
      const auto symbol_type = static_cast<unsigned char>(symbol.info & 0x0f);
      if ((symbol_type != kSymbolTypeObject && symbol_type != kSymbolTypeTls) || symbol.shndx == 0) {
        continue;
      }
      const auto name = read_c_string(string_table, symbol.name);
      if (name.empty()) {
        continue;
      }
      symbols[name] = SymbolInfo{.name = name,
                                 .value = symbol.value,
                                 .size = symbol.size,
                                 .is_thread_local = symbol_type == kSymbolTypeTls};
    }
  }

  return symbols;
}

}  // namespace

ElfSymbolTable ElfSymbolTable::load(const std::string& file_path) {
  std::ifstream input(platform::utf8_path(file_path), std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("无法打开 ELF 文件: " + file_path);
  }

  const auto ident = read_struct<ElfIdent>(input);
  validate_elf_ident(ident);
  input.seekg(0, std::ios::beg);

  ElfSymbolTable table;
  // 这里按 ELF class 分派具体结构体，避免 32 位交叉编译产物被误判为“不支持”。
  if (ident[4] == kElfClass32) {
    table.symbols_ = load_symbol_table<Elf32Header, Elf32SectionHeader, Elf32Symbol>(input);
    return table;
  }
  table.symbols_ = load_symbol_table<Elf64Header, Elf64SectionHeader, Elf64Symbol>(input);
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
