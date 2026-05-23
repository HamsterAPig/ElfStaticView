#include "elf/elf_symbol_table.hpp"

#include "platform/utf8.hpp"

#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace elf_static_view::elf {

namespace {

using ElfIdent = std::array<unsigned char, 16>;

enum class ByteOrder {
  LittleEndian,
  BigEndian,
};

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
constexpr unsigned char kElfDataBigEndian = 2;

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
std::array<unsigned char, sizeof(T)> read_bytes(std::ifstream& input) {
  std::array<unsigned char, sizeof(T)> bytes {};
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!input) {
    throw std::runtime_error("读取 ELF 结构失败");
  }
  return bytes;
}

template <typename T>
T read_integer(std::ifstream& input, const ByteOrder byte_order) {
  const auto bytes = read_bytes<T>(input);
  T value = 0;
  if (byte_order == ByteOrder::LittleEndian) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      value |= static_cast<T>(bytes[index]) << (index * 8U);
    }
    return value;
  }

  for (const auto byte : bytes) {
    value = static_cast<T>((value << 8U) | static_cast<T>(byte));
  }
  return value;
}

ElfIdent read_ident(std::ifstream& input) {
  ElfIdent ident {};
  input.read(reinterpret_cast<char*>(ident.data()), static_cast<std::streamsize>(ident.size()));
  if (!input) {
    throw std::runtime_error("读取 ELF 头失败");
  }
  return ident;
}

std::string read_c_string(const std::vector<char>& table, const std::uint32_t offset) {
  if (offset >= table.size()) {
    return {};
  }
  return std::string(table.data() + offset);
}

SectionDescriptor to_descriptor(const Elf32SectionHeader& section) {
  return SectionDescriptor {.type = section.type,
                            .offset = section.offset,
                            .size = section.size,
                            .link = section.link,
                            .entsize = section.entsize};
}

SectionDescriptor to_descriptor(const Elf64SectionHeader& section) {
  return SectionDescriptor {.type = section.type,
                            .offset = section.offset,
                            .size = section.size,
                            .link = section.link,
                            .entsize = section.entsize};
}

SymbolDescriptor to_descriptor(const Elf32Symbol& symbol) {
  return SymbolDescriptor {.name = symbol.name,
                           .info = symbol.info,
                           .shndx = symbol.shndx,
                           .value = symbol.value,
                           .size = symbol.size};
}

SymbolDescriptor to_descriptor(const Elf64Symbol& symbol) {
  return SymbolDescriptor {.name = symbol.name,
                           .info = symbol.info,
                           .shndx = symbol.shndx,
                           .value = symbol.value,
                           .size = symbol.size};
}

void validate_elf_magic(const ElfIdent& ident) {
  if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F') {
    throw std::runtime_error("目标文件不是 ELF");
  }
}

unsigned char parse_elf_class(const ElfIdent& ident) {
  if (ident[4] != kElfClass32 && ident[4] != kElfClass64) {
    throw std::runtime_error("不支持的 ELF class");
  }
  return ident[4];
}

ByteOrder parse_byte_order(const ElfIdent& ident) {
  if (ident[5] == kElfDataLittleEndian) {
    return ByteOrder::LittleEndian;
  }
  if (ident[5] == kElfDataBigEndian) {
    return ByteOrder::BigEndian;
  }
  throw std::runtime_error("不支持的 ELF 字节序");
}

template <typename Header>
Header read_header(std::ifstream& input, const ElfIdent& ident, ByteOrder byte_order);

template <>
Elf32Header read_header<Elf32Header>(std::ifstream& input, const ElfIdent& ident, const ByteOrder byte_order) {
  Elf32Header header;
  header.ident = ident;
  header.type = read_integer<std::uint16_t>(input, byte_order);
  header.machine = read_integer<std::uint16_t>(input, byte_order);
  header.version = read_integer<std::uint32_t>(input, byte_order);
  header.entry = read_integer<std::uint32_t>(input, byte_order);
  header.phoff = read_integer<std::uint32_t>(input, byte_order);
  header.shoff = read_integer<std::uint32_t>(input, byte_order);
  header.flags = read_integer<std::uint32_t>(input, byte_order);
  header.ehsize = read_integer<std::uint16_t>(input, byte_order);
  header.phentsize = read_integer<std::uint16_t>(input, byte_order);
  header.phnum = read_integer<std::uint16_t>(input, byte_order);
  header.shentsize = read_integer<std::uint16_t>(input, byte_order);
  header.shnum = read_integer<std::uint16_t>(input, byte_order);
  header.shstrndx = read_integer<std::uint16_t>(input, byte_order);
  return header;
}

template <>
Elf64Header read_header<Elf64Header>(std::ifstream& input, const ElfIdent& ident, const ByteOrder byte_order) {
  Elf64Header header;
  header.ident = ident;
  header.type = read_integer<std::uint16_t>(input, byte_order);
  header.machine = read_integer<std::uint16_t>(input, byte_order);
  header.version = read_integer<std::uint32_t>(input, byte_order);
  header.entry = read_integer<std::uint64_t>(input, byte_order);
  header.phoff = read_integer<std::uint64_t>(input, byte_order);
  header.shoff = read_integer<std::uint64_t>(input, byte_order);
  header.flags = read_integer<std::uint32_t>(input, byte_order);
  header.ehsize = read_integer<std::uint16_t>(input, byte_order);
  header.phentsize = read_integer<std::uint16_t>(input, byte_order);
  header.phnum = read_integer<std::uint16_t>(input, byte_order);
  header.shentsize = read_integer<std::uint16_t>(input, byte_order);
  header.shnum = read_integer<std::uint16_t>(input, byte_order);
  header.shstrndx = read_integer<std::uint16_t>(input, byte_order);
  return header;
}

template <typename Section>
Section read_section_header(std::ifstream& input, ByteOrder byte_order);

template <>
Elf32SectionHeader read_section_header<Elf32SectionHeader>(std::ifstream& input,
                                                           const ByteOrder byte_order) {
  Elf32SectionHeader section;
  section.name = read_integer<std::uint32_t>(input, byte_order);
  section.type = read_integer<std::uint32_t>(input, byte_order);
  section.flags = read_integer<std::uint32_t>(input, byte_order);
  section.addr = read_integer<std::uint32_t>(input, byte_order);
  section.offset = read_integer<std::uint32_t>(input, byte_order);
  section.size = read_integer<std::uint32_t>(input, byte_order);
  section.link = read_integer<std::uint32_t>(input, byte_order);
  section.info = read_integer<std::uint32_t>(input, byte_order);
  section.addralign = read_integer<std::uint32_t>(input, byte_order);
  section.entsize = read_integer<std::uint32_t>(input, byte_order);
  return section;
}

template <>
Elf64SectionHeader read_section_header<Elf64SectionHeader>(std::ifstream& input,
                                                           const ByteOrder byte_order) {
  Elf64SectionHeader section;
  section.name = read_integer<std::uint32_t>(input, byte_order);
  section.type = read_integer<std::uint32_t>(input, byte_order);
  section.flags = read_integer<std::uint64_t>(input, byte_order);
  section.addr = read_integer<std::uint64_t>(input, byte_order);
  section.offset = read_integer<std::uint64_t>(input, byte_order);
  section.size = read_integer<std::uint64_t>(input, byte_order);
  section.link = read_integer<std::uint32_t>(input, byte_order);
  section.info = read_integer<std::uint32_t>(input, byte_order);
  section.addralign = read_integer<std::uint64_t>(input, byte_order);
  section.entsize = read_integer<std::uint64_t>(input, byte_order);
  return section;
}

template <typename Symbol>
Symbol read_symbol(std::ifstream& input, ByteOrder byte_order);

template <>
Elf32Symbol read_symbol<Elf32Symbol>(std::ifstream& input, const ByteOrder byte_order) {
  Elf32Symbol symbol;
  symbol.name = read_integer<std::uint32_t>(input, byte_order);
  symbol.value = read_integer<std::uint32_t>(input, byte_order);
  symbol.size = read_integer<std::uint32_t>(input, byte_order);
  symbol.info = read_integer<unsigned char>(input, byte_order);
  symbol.other = read_integer<unsigned char>(input, byte_order);
  symbol.shndx = read_integer<std::uint16_t>(input, byte_order);
  return symbol;
}

template <>
Elf64Symbol read_symbol<Elf64Symbol>(std::ifstream& input, const ByteOrder byte_order) {
  Elf64Symbol symbol;
  symbol.name = read_integer<std::uint32_t>(input, byte_order);
  symbol.info = read_integer<unsigned char>(input, byte_order);
  symbol.other = read_integer<unsigned char>(input, byte_order);
  symbol.shndx = read_integer<std::uint16_t>(input, byte_order);
  symbol.value = read_integer<std::uint64_t>(input, byte_order);
  symbol.size = read_integer<std::uint64_t>(input, byte_order);
  return symbol;
}

template <typename Header, typename Section, typename Symbol>
std::unordered_map<std::string, SymbolInfo> load_symbol_table(std::ifstream& input,
                                                              const ElfIdent& ident,
                                                              const ByteOrder byte_order) {
  const Header header = read_header<Header>(input, ident, byte_order);
  input.seekg(static_cast<std::streamoff>(header.shoff), std::ios::beg);

  std::vector<SectionDescriptor> sections;
  sections.reserve(header.shnum);
  for (std::uint16_t index = 0; index < header.shnum; ++index) {
    sections.push_back(to_descriptor(read_section_header<Section>(input, byte_order)));
  }

  std::unordered_map<std::string, SymbolInfo> symbols;
  for (const auto& section : sections) {
    if (section.type != kSectionTypeSymtab || section.entsize == 0 || section.link >= sections.size()) {
      continue;
    }

    input.seekg(static_cast<std::streamoff>(sections[section.link].offset), std::ios::beg);
    std::vector<char> string_table(static_cast<std::size_t>(sections[section.link].size));
    input.read(string_table.data(), static_cast<std::streamsize>(string_table.size()));
    if (!input) {
      throw std::runtime_error("读取字符串表失败");
    }

    input.seekg(static_cast<std::streamoff>(section.offset), std::ios::beg);
    const std::uint64_t symbol_count = section.size / section.entsize;
    for (std::uint64_t index = 0; index < symbol_count; ++index) {
      const auto symbol = to_descriptor(read_symbol<Symbol>(input, byte_order));
      const auto symbol_type = static_cast<unsigned char>(symbol.info & 0x0fU);
      if ((symbol_type != kSymbolTypeObject && symbol_type != kSymbolTypeTls) || symbol.shndx == 0) {
        continue;
      }
      const auto name = read_c_string(string_table, symbol.name);
      if (name.empty()) {
        continue;
      }
      symbols[name] = SymbolInfo {.name = name,
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

  const auto ident = read_ident(input);
  validate_elf_magic(ident);
  const auto elf_class = parse_elf_class(ident);
  const auto byte_order = parse_byte_order(ident);

  ElfSymbolTable table;
  // 按 ELF class 与字节序分派读取逻辑，避免把文件布局误当成本机内存布局。
  if (elf_class == kElfClass32) {
    table.symbols_ = load_symbol_table<Elf32Header, Elf32SectionHeader, Elf32Symbol>(
      input, ident, byte_order);
    return table;
  }

  table.symbols_ = load_symbol_table<Elf64Header, Elf64SectionHeader, Elf64Symbol>(
    input, ident, byte_order);
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
