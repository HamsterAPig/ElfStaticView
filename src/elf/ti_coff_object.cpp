#include "elf/ti_coff_object.hpp"

#include "elf/dwarf_wrappers.hpp"
#include "platform/utf8.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace elf_static_view::elf {

namespace {

constexpr std::uint16_t kTiC2000CoffMagic = 0x00c2;
constexpr std::size_t kFileHeaderSize = 22;
constexpr std::size_t kSectionHeaderSize = 48;
constexpr std::size_t kSymbolRecordSize = 18;
constexpr std::size_t kObjectKindProbeSize = 4;

[[nodiscard]] std::uint16_t read_u16_le(const std::vector<std::uint8_t>& data,
                                        const std::size_t offset) {
  if (offset + 2 > data.size()) {
    throw DwarfError("TI-COFF 解析失败: 越界读取 u16");
  }
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(data[offset + 1] << 8U);
}

[[nodiscard]] std::uint32_t read_u32_le(const std::vector<std::uint8_t>& data,
                                        const std::size_t offset) {
  if (offset + 4 > data.size()) {
    throw DwarfError("TI-COFF 解析失败: 越界读取 u32");
  }
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

[[nodiscard]] std::vector<std::uint8_t> read_file_bytes(const std::string& file_path) {
  std::ifstream stream(platform::utf8_path(file_path), std::ios::binary);
  if (!stream) {
    throw DwarfError("无法打开对象文件: " + file_path);
  }
  stream.seekg(0, std::ios::end);
  const auto end = stream.tellg();
  if (end < 0) {
    throw DwarfError("无法读取对象文件大小: " + file_path);
  }
  stream.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
  if (!data.empty()) {
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!stream) {
      throw DwarfError("读取对象文件失败: " + file_path);
    }
  }
  return data;
}

[[nodiscard]] std::vector<std::uint8_t> read_file_header_bytes(const std::string& file_path,
                                                               const std::size_t size) {
  std::ifstream stream(platform::utf8_path(file_path), std::ios::binary);
  if (!stream) {
    throw DwarfError("无法打开对象文件: " + file_path);
  }
  std::vector<std::uint8_t> data(size);
  stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
  const auto bytes_read = stream.gcount();
  if (bytes_read < 0) {
    throw DwarfError("读取对象文件头失败: " + file_path);
  }
  if (!stream.good() && !stream.eof()) {
    throw DwarfError("读取对象文件头失败: " + file_path);
  }
  data.resize(static_cast<std::size_t>(bytes_read));
  return data;
}

[[nodiscard]] bool has_range(const std::vector<std::uint8_t>& data,
                             const std::uint32_t offset,
                             const std::uint32_t size) {
  const auto begin = static_cast<std::uint64_t>(offset);
  const auto length = static_cast<std::uint64_t>(size);
  return begin <= data.size() && length <= data.size() - begin;
}

[[nodiscard]] std::string read_null_terminated_string(const std::vector<std::uint8_t>& data,
                                                      const std::size_t offset) {
  if (offset >= data.size()) {
    return {};
  }
  std::size_t end = offset;
  while (end < data.size() && data[end] != 0) {
    ++end;
  }
  return std::string(reinterpret_cast<const char*>(data.data() + offset), end - offset);
}

[[nodiscard]] std::string read_short_name(const std::vector<std::uint8_t>& data,
                                          const std::size_t offset) {
  std::size_t length = 0;
  while (length < 8 && offset + length < data.size() && data[offset + length] != 0) {
    ++length;
  }
  return std::string(reinterpret_cast<const char*>(data.data() + offset), length);
}

[[nodiscard]] std::vector<std::uint8_t> read_string_table(const std::vector<std::uint8_t>& data,
                                                          const std::uint32_t symbol_offset,
                                                          const std::uint32_t symbol_count) {
  const auto table_offset =
    static_cast<std::uint64_t>(symbol_offset) +
    static_cast<std::uint64_t>(symbol_count) * kSymbolRecordSize;
  if (table_offset + 4 > data.size()) {
    return {};
  }
  const auto size = read_u32_le(data, static_cast<std::size_t>(table_offset));
  if (size < 4 || table_offset + size > data.size()) {
    return {};
  }
  return std::vector<std::uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(table_offset),
                                   data.begin() + static_cast<std::ptrdiff_t>(table_offset + size));
}

[[nodiscard]] std::string resolve_section_name(const std::vector<std::uint8_t>& data,
                                               const std::vector<std::uint8_t>& string_table,
                                               const std::size_t offset) {
  const auto zeroes = read_u32_le(data, offset);
  if (zeroes == 0) {
    const auto string_offset = read_u32_le(data, offset + 4);
    if (string_offset < 4 || string_offset >= string_table.size()) {
      throw DwarfError("暂不支持的 TI-COFF section header 格式: section 长名称越界");
    }
    return read_null_terminated_string(string_table, string_offset);
  }
  return read_short_name(data, offset);
}

[[nodiscard]] bool is_debug_section_name(const std::string& name) {
  return name.rfind(".debug_", 0) == 0;
}

[[nodiscard]] bool has_section(const std::vector<TiCoffSection>& sections, const char* name) {
  return std::any_of(sections.begin(), sections.end(), [name](const TiCoffSection& section) {
    return section.name == name;
  });
}

[[nodiscard]] int get_section_info(void* object,
                                   const Dwarf_Unsigned section_index,
                                   Dwarf_Obj_Access_Section_a* return_section,
                                   int* error) {
  if (return_section == nullptr) {
    if (error != nullptr) {
      *error = DW_DLE_NDS;
    }
    return DW_DLV_NO_ENTRY;
  }
  *return_section = {};
  if (section_index == 0U) {
    // libdwarf 初始化阶段会按 ELF 约定访问 0 号空 section，必须返回一个可忽略的空段。
    return_section->as_name = "";
    return DW_DLV_OK;
  }

  const auto* coff_object = static_cast<TiCoffObject*>(object);
  const auto* section = coff_object->section_at(section_index);
  if (section == nullptr) {
    if (error != nullptr) {
      *error = DW_DLE_NDS;
    }
    return DW_DLV_NO_ENTRY;
  }
  return_section->as_name = section->name.c_str();
  return_section->as_addr = section->physical_address;
  return_section->as_offset = section->file_offset;
  return_section->as_size = section->size;
  return DW_DLV_OK;
}

[[nodiscard]] Dwarf_Small get_byte_order(void*) { return DW_END_little; }

[[nodiscard]] Dwarf_Small get_length_size(void*) { return 4; }

[[nodiscard]] Dwarf_Small get_pointer_size(void*) { return 4; }

[[nodiscard]] Dwarf_Unsigned get_filesize(void* object) {
  const auto* coff_object = static_cast<TiCoffObject*>(object);
  return coff_object->file_size();
}

[[nodiscard]] Dwarf_Unsigned get_section_count(void* object) {
  const auto* coff_object = static_cast<TiCoffObject*>(object);
  return coff_object->section_count();
}

[[nodiscard]] int load_section(void* object,
                               const Dwarf_Unsigned section_index,
                               Dwarf_Small** return_data,
                               int* error) {
  auto* coff_object = static_cast<TiCoffObject*>(object);
  try {
    auto data = coff_object->read_section_data(section_index);
    auto* buffer = static_cast<Dwarf_Small*>(std::malloc(data.size()));
    if (buffer == nullptr && !data.empty()) {
      if (error != nullptr) {
        *error = DW_DLE_ALLOC_FAIL;
      }
      return DW_DLV_ERROR;
    }
    if (!data.empty()) {
      std::memcpy(buffer, data.data(), data.size());
    }
    *return_data = buffer;
    return DW_DLV_OK;
  } catch (...) {
    if (error != nullptr) {
      *error = DW_DLE_NDS;
    }
    return DW_DLV_NO_ENTRY;
  }
}

[[nodiscard]] int relocate_section(void*,
                                   Dwarf_Unsigned,
                                   Dwarf_Debug,
                                   int*) {
  return DW_DLV_OK;
}

void finish_object(void*) {}

const Dwarf_Obj_Access_Methods_a kTiCoffAccessMethods = {
  get_section_info,
  get_byte_order,
  get_length_size,
  get_pointer_size,
  get_filesize,
  get_section_count,
  load_section,
  relocate_section,
  nullptr,
  finish_object,
};

}  // namespace

TiCoffObject::TiCoffObject(std::string file_path) : file_path_(std::move(file_path)) {
  file_data_ = read_file_bytes(file_path_);
  file_size_ = file_data_.size();
  if (file_data_.size() < kFileHeaderSize || read_u16_le(file_data_, 0) != kTiC2000CoffMagic) {
    throw DwarfError("不支持的对象格式: 不是 TI C2000 COFF v2");
  }

  const auto section_count = read_u16_le(file_data_, 2);
  const auto symbol_offset = read_u32_le(file_data_, 8);
  const auto symbol_count = read_u32_le(file_data_, 12);
  const auto optional_header_size = read_u16_le(file_data_, 16);
  const auto section_table_offset = kFileHeaderSize + optional_header_size;
  const auto section_table_size =
    static_cast<std::uint64_t>(section_count) * kSectionHeaderSize;
  if (section_table_offset + section_table_size > file_data_.size()) {
    throw DwarfError("暂不支持的 TI-COFF section header 格式: section table 越界");
  }

  const auto string_table = read_string_table(file_data_, symbol_offset, symbol_count);
  sections_.reserve(section_count);
  for (std::uint16_t index = 0; index < section_count; ++index) {
    const auto offset = section_table_offset + static_cast<std::size_t>(index) * kSectionHeaderSize;
    TiCoffSection section;
    // TI-COFF section header 固定 48 字节；首版只把原始 .debug_* section 暴露给
    // libdwarf，不处理 COFF relocation，确保 EABI/ELF 语义层无需分叉。
    section.name = resolve_section_name(file_data_, string_table, offset);
    section.physical_address = read_u32_le(file_data_, offset + 8);
    section.virtual_address = read_u32_le(file_data_, offset + 12);
    section.size = read_u32_le(file_data_, offset + 16);
    section.file_offset = read_u32_le(file_data_, offset + 20);
    section.relocation_offset = read_u32_le(file_data_, offset + 24);
    section.line_number_offset = read_u32_le(file_data_, offset + 28);
    section.relocation_count = read_u16_le(file_data_, offset + 32);
    section.line_number_count = read_u16_le(file_data_, offset + 34);
    section.flags = read_u16_le(file_data_, offset + 36);
    if (section.name.empty() || !has_range(file_data_, section.file_offset, section.size)) {
      throw DwarfError("暂不支持的 TI-COFF section header 格式: section 数据越界或名称异常");
    }
    if (is_debug_section_name(section.name)) {
      sections_.push_back(std::move(section));
    }
  }
}

const std::string& TiCoffObject::file_path() const noexcept { return file_path_; }

Dwarf_Unsigned TiCoffObject::file_size() const noexcept { return file_size_; }

Dwarf_Unsigned TiCoffObject::section_count() const noexcept { return sections_.size() + 1U; }

const TiCoffSection* TiCoffObject::section_at(const Dwarf_Unsigned section_index) const noexcept {
  // libdwarf 的 object access API 按 ELF section 语义保留 0 号空 section。
  if (section_index == 0U || section_index > sections_.size()) {
    return nullptr;
  }
  return &sections_[static_cast<std::size_t>(section_index - 1U)];
}

std::vector<std::uint8_t> TiCoffObject::read_section_data(
  const Dwarf_Unsigned section_index) const {
  if (section_index == 0U) {
    throw DwarfError("TI-COFF section 0 为 libdwarf 保留空段，不能读取数据");
  }
  const auto* section = section_at(section_index);
  if (section == nullptr) {
    throw DwarfError("TI-COFF section 索引不存在");
  }
  return std::vector<std::uint8_t>(
    file_data_.begin() + static_cast<std::ptrdiff_t>(section->file_offset),
    file_data_.begin() + static_cast<std::ptrdiff_t>(section->file_offset + section->size));
}

std::vector<std::string> TiCoffObject::missing_required_debug_sections() const {
  std::vector<std::string> missing;
  if (!has_section(sections_, ".debug_info")) {
    missing.emplace_back(".debug_info");
  }
  if (!has_section(sections_, ".debug_abbrev")) {
    missing.emplace_back(".debug_abbrev");
  }
  return missing;
}

bool is_ti_c2000_coff_file(const std::string& file_path) {
  return detect_object_file_kind(file_path) == ObjectFileKind::TiCoff;
}

bool is_elf_file(const std::string& file_path) {
  return detect_object_file_kind(file_path) == ObjectFileKind::Elf;
}

ObjectFileKind detect_object_file_kind(const std::string& file_path) {
  // 只读取极小文件头做判型，避免格式探测阶段重复整文件 I/O。
  const auto data = read_file_header_bytes(file_path, kObjectKindProbeSize);
  if (data.size() >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
    return ObjectFileKind::Elf;
  }
  if (data.size() >= 2) {
    const auto magic = static_cast<std::uint16_t>(data[0]) |
                       (static_cast<std::uint16_t>(data[1]) << 8U);
    if (magic == kTiC2000CoffMagic) {
      return ObjectFileKind::TiCoff;
    }
  }
  return ObjectFileKind::Unknown;
}

Dwarf_Obj_Access_Interface_a make_ti_coff_dwarf_access(TiCoffObject& object) {
  Dwarf_Obj_Access_Interface_a interface = {};
  interface.ai_object = &object;
  interface.ai_methods = &kTiCoffAccessMethods;
  return interface;
}

}  // namespace elf_static_view::elf
