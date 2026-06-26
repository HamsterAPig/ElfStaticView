#include "elf/dwarf_reader.hpp"

#include "analysis/model_utils.hpp"
#include "elf/elf_symbol_table.hpp"
#include "elf/dwarf_wrappers.hpp"
#include "elf/ti_coff_object.hpp"

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace elf_static_view::elf {

namespace {

DieHandle take_die(std::optional<DieHandle>&& value) {
  return std::move(value).value();
}

struct ReaderContext {
  Dwarf_Debug debug = nullptr;
  std::string file_path;
  LoadPolicy load_policy;
  std::vector<TypeNode> types;
  std::vector<VariableRecord> variables;
  std::vector<CompileUnitRecord> compile_units;
  std::unordered_map<std::string, std::string> type_ids;
  std::unordered_map<std::string, std::string> signature_type_ids;
  std::unordered_set<std::string> recorded_type_ids;
  std::unordered_set<std::string> loading_type_ids;
  std::unordered_map<Dwarf_Off, std::vector<std::string>> variable_scope_by_offset;
  std::unordered_map<Dwarf_Off, bool> variable_static_member_by_offset;
  std::unordered_map<Dwarf_Off, std::vector<std::string>> class_scope_by_offset;
  std::vector<std::string> scope_stack;
  std::vector<Dwarf_Half> scope_tag_stack;
  std::string current_compile_unit_name;
  std::string current_compile_unit_source_path;
  Dwarf_Half current_address_size = 0;
  Dwarf_Half current_offset_size = 0;
  Dwarf_Half current_dwarf_version = 0;
  std::size_t skipped_compile_unit_count = 0;
};

constexpr Dwarf_Half kDwTagTiFarType = static_cast<Dwarf_Half>(0x4080);
constexpr Dwarf_Half kDwTagTiClone = static_cast<Dwarf_Half>(0x4088);
constexpr Dwarf_Half kDwTagTiCodeLabel = static_cast<Dwarf_Half>(0x4089);

struct ManualAbbrevEntry {
  Dwarf_Half tag = 0;
  std::vector<std::pair<Dwarf_Half, Dwarf_Half>> attributes;
};

struct ManualTypeDescriptor {
  Dwarf_Half tag = 0;
  std::optional<std::string> name;
  std::optional<std::string> referenced_signature_hex;
  std::vector<std::uint64_t> dimensions;
  std::optional<std::uint64_t> byte_size;
};

struct ManualTypeUnitContext {
  std::uint64_t version = 0;
  std::uint64_t unit_type = 0;
  std::uint64_t addr_size = 0;
  std::uint64_t str_offsets_base = 0;
};

[[nodiscard]] std::string make_manual_type_id(const std::string& signature_hex) {
  return "type@sig:" + signature_hex;
}

[[nodiscard]] std::string sig8_to_hex(const Dwarf_Sig8& signature) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::setfill('0');
  for (unsigned char byte : signature.signature) {
    stream << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return stream.str();
}

[[nodiscard]] std::uint16_t read_u16_le(const std::vector<std::uint8_t>& data, const std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         (static_cast<std::uint16_t>(data[offset + 1]) << 8U);
}

[[nodiscard]] std::uint32_t read_u32_le(const std::vector<std::uint8_t>& data, const std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

[[nodiscard]] std::uint64_t read_uleb128(const std::vector<std::uint8_t>& data, std::size_t& offset) {
  std::uint64_t value = 0;
  unsigned shift = 0;
  while (offset < data.size()) {
    const auto byte = data[offset++];
    value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0) {
      return value;
    }
    shift += 7;
  }
  throw DwarfError("手工解析 .debug_types 失败: uleb128 越界");
}

[[nodiscard]] std::int64_t read_sleb128(const std::vector<std::uint8_t>& data, std::size_t& offset) {
  std::int64_t value = 0;
  unsigned shift = 0;
  std::uint8_t byte = 0;
  while (offset < data.size()) {
    byte = data[offset++];
    value |= static_cast<std::int64_t>(byte & 0x7fU) << shift;
    shift += 7;
    if ((byte & 0x80U) == 0) {
      break;
    }
  }
  if (shift < 64 && (byte & 0x40U) != 0) {
    value |= -((static_cast<std::int64_t>(1)) << shift);
  }
  return value;
}

[[nodiscard]] std::unordered_map<std::uint64_t, ManualAbbrevEntry> load_manual_abbrev_entries(
  const std::vector<std::uint8_t>& abbrev,
  std::size_t offset = 0) {
  std::unordered_map<std::uint64_t, ManualAbbrevEntry> entries;
  while (offset < abbrev.size()) {
    const auto code = read_uleb128(abbrev, offset);
    if (code == 0) {
      if (!entries.empty()) {
        break;
      }
      continue;
    }
    ManualAbbrevEntry entry;
    entry.tag = static_cast<Dwarf_Half>(read_uleb128(abbrev, offset));
    if (offset < abbrev.size()) {
      ++offset;
    }
    while (offset < abbrev.size()) {
      const auto name = static_cast<Dwarf_Half>(read_uleb128(abbrev, offset));
      const auto form = static_cast<Dwarf_Half>(read_uleb128(abbrev, offset));
      if (name == 0 && form == 0) {
        break;
      }
      if (form == DW_FORM_implicit_const) {
        (void)read_sleb128(abbrev, offset);
      }
      entry.attributes.emplace_back(name, form);
    }
    entries.emplace(code, std::move(entry));
  }
  return entries;
}

[[nodiscard]] std::optional<std::string> manual_read_string(const std::vector<std::uint8_t>& debug_str,
                                                            const std::uint32_t strp_offset) {
  if (strp_offset >= debug_str.size()) {
    return std::nullopt;
  }
  return std::string(reinterpret_cast<const char*>(debug_str.data() + strp_offset));
}

[[nodiscard]] std::optional<std::uint64_t> read_fixed_width_unsigned(const std::vector<std::uint8_t>& data,
                                                                     const std::size_t offset,
                                                                     const std::size_t width) {
  if (offset + width > data.size()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < width; ++index) {
    value |= static_cast<std::uint64_t>(data[offset + index]) << (index * 8U);
  }
  return value;
}

[[nodiscard]] std::optional<std::uint64_t> read_form_unsigned_value(const Dwarf_Half form,
                                                                    const std::vector<std::uint8_t>& data,
                                                                    std::size_t& offset) {
  switch (form) {
    case DW_FORM_data1:
    case DW_FORM_flag:
    case DW_FORM_ref1:
    case DW_FORM_strx1:
    case DW_FORM_addrx1: {
      const auto value = read_fixed_width_unsigned(data, offset, 1);
      if (!value.has_value()) {
        return std::nullopt;
      }
      offset += 1;
      return value;
    }
    case DW_FORM_data2:
    case DW_FORM_ref2:
    case DW_FORM_strx2:
    case DW_FORM_addrx2: {
      const auto value = read_fixed_width_unsigned(data, offset, 2);
      if (!value.has_value()) {
        return std::nullopt;
      }
      offset += 2;
      return value;
    }
    case DW_FORM_data4:
    case DW_FORM_sec_offset:
    case DW_FORM_strp:
    case DW_FORM_line_strp:
    case DW_FORM_ref4:
    case DW_FORM_ref_sup4:
    case DW_FORM_strp_sup:
    case DW_FORM_GNU_ref_alt:
    case DW_FORM_GNU_strp_alt:
    case DW_FORM_strx4:
    case DW_FORM_addrx4: {
      const auto value = read_fixed_width_unsigned(data, offset, 4);
      if (!value.has_value()) {
        return std::nullopt;
      }
      offset += 4;
      return value;
    }
    case DW_FORM_strx3:
    case DW_FORM_addrx3: {
      const auto value = read_fixed_width_unsigned(data, offset, 3);
      if (!value.has_value()) {
        return std::nullopt;
      }
      offset += 3;
      return value;
    }
    case DW_FORM_data8:
    case DW_FORM_ref_sig8:
    case DW_FORM_ref8:
    case DW_FORM_ref_sup8: {
      const auto value = read_fixed_width_unsigned(data, offset, 8);
      if (!value.has_value()) {
        return std::nullopt;
      }
      offset += 8;
      return value;
    }
    case DW_FORM_udata:
    case DW_FORM_ref_udata:
    case DW_FORM_strx:
    case DW_FORM_addrx:
    case DW_FORM_loclistx:
    case DW_FORM_rnglistx:
      return read_uleb128(data, offset);
    case DW_FORM_implicit_const:
    case DW_FORM_flag_present:
      return 0;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::string> manual_read_strx_string(
  const std::vector<std::uint8_t>& debug_str_offsets,
  const std::vector<std::uint8_t>& debug_str,
  const ManualTypeUnitContext& unit_context,
  const std::uint64_t strx_index) {
  if (unit_context.version != 5) {
    return std::nullopt;
  }
  const std::size_t table_offset = static_cast<std::size_t>(unit_context.str_offsets_base);
  if (table_offset + 8 > debug_str_offsets.size()) {
    return std::nullopt;
  }
  const auto unit_length = read_u32_le(debug_str_offsets, table_offset);
  const auto table_end = table_offset + 4 + static_cast<std::size_t>(unit_length);
  if (table_end > debug_str_offsets.size()) {
    return std::nullopt;
  }
  const auto version = read_u16_le(debug_str_offsets, table_offset + 4);
  const auto padding = read_u16_le(debug_str_offsets, table_offset + 6);
  if (version != 5 || padding != 0) {
    return std::nullopt;
  }
  const std::size_t entry_offset = table_offset + 8 + static_cast<std::size_t>(strx_index) * 4U;
  if (entry_offset + 4 > table_end) {
    return std::nullopt;
  }
  const auto str_offset = read_u32_le(debug_str_offsets, entry_offset);
  return manual_read_string(debug_str, str_offset);
}

void manual_skip_form(const Dwarf_Half form,
                      const std::vector<std::uint8_t>& data,
                      std::size_t& offset) {
  if (form == DW_FORM_indirect) {
    const auto actual_form = static_cast<Dwarf_Half>(read_uleb128(data, offset));
    manual_skip_form(actual_form, data, offset);
    return;
  }
  switch (form) {
    case DW_FORM_addr:
    case DW_FORM_ref_addr:
      offset += 4;
      return;
    case DW_FORM_data1:
    case DW_FORM_flag:
    case DW_FORM_ref1:
    case DW_FORM_strx1:
    case DW_FORM_addrx1:
      offset += 1;
      return;
    case DW_FORM_data2:
    case DW_FORM_ref2:
    case DW_FORM_strx2:
    case DW_FORM_addrx2:
      offset += 2;
      return;
    case DW_FORM_data4:
    case DW_FORM_sec_offset:
    case DW_FORM_strp:
    case DW_FORM_line_strp:
    case DW_FORM_ref_sup4:
    case DW_FORM_strp_sup:
    case DW_FORM_GNU_ref_alt:
    case DW_FORM_GNU_strp_alt:
    case DW_FORM_ref4:
    case DW_FORM_strx4:
    case DW_FORM_addrx4:
      offset += 4;
      return;
    case DW_FORM_strx3:
    case DW_FORM_addrx3:
      offset += 3;
      return;
    case DW_FORM_data8:
    case DW_FORM_ref_sig8:
    case DW_FORM_ref8:
    case DW_FORM_ref_sup8:
      offset += 8;
      return;
    case DW_FORM_data16:
      offset += 16;
      return;
    case DW_FORM_string: {
      while (offset < data.size() && data[offset] != 0) {
        ++offset;
      }
      if (offset < data.size()) {
        ++offset;
      }
      return;
    }
    case DW_FORM_block1: {
      const auto length = static_cast<std::size_t>(data[offset]);
      offset += 1 + length;
      return;
    }
    case DW_FORM_block2: {
      const auto length = static_cast<std::size_t>(read_u16_le(data, offset));
      offset += 2 + length;
      return;
    }
    case DW_FORM_block4: {
      const auto length = static_cast<std::size_t>(read_u32_le(data, offset));
      offset += 4 + length;
      return;
    }
    case DW_FORM_block:
    case DW_FORM_exprloc: {
      const auto length = static_cast<std::size_t>(read_uleb128(data, offset));
      offset += length;
      return;
    }
    case DW_FORM_udata:
    case DW_FORM_ref_udata:
    case DW_FORM_strx:
    case DW_FORM_addrx:
    case DW_FORM_loclistx:
    case DW_FORM_rnglistx:
      (void)read_uleb128(data, offset);
      return;
    case DW_FORM_sdata:
      (void)read_sleb128(data, offset);
      return;
    case DW_FORM_flag_present:
      return;
    default:
      throw DwarfError("手工解析 .debug_types 失败: 暂不支持的 FORM " + std::to_string(form));
  }
}

[[nodiscard]] std::unordered_map<std::string, ManualTypeDescriptor>& manual_type_descriptors(
  const std::string& file_path) {
  static std::unordered_map<std::string, std::unordered_map<std::string, ManualTypeDescriptor>> cache;
  const auto found = cache.find(file_path);
  if (found != cache.end()) {
    return found->second;
  }

  std::unordered_map<std::string, ManualTypeDescriptor> descriptors;
  try {
    const auto debug_types =
        ElfSymbolTable::read_section_bytes(file_path, ".debug_types");
    const auto debug_abbrev =
        ElfSymbolTable::read_section_bytes(file_path, ".debug_abbrev");
    const auto debug_str =
        ElfSymbolTable::read_section_bytes(file_path, ".debug_str");
    const auto debug_str_offsets =
        ElfSymbolTable::read_section_bytes(file_path, ".debug_str_offsets");
    const auto debug_line_str =
        ElfSymbolTable::read_section_bytes(file_path, ".debug_line_str");
    if (debug_types.has_value() && debug_abbrev.has_value()) {
      const auto &types = debug_types.value();
      const auto empty_str = std::vector<std::uint8_t>{};
      const auto &strings =
          debug_str.has_value() ? debug_str.value() : empty_str;
      const auto &string_offsets =
          debug_str_offsets.has_value() ? debug_str_offsets.value() : empty_str;
      const auto &line_strings =
          debug_line_str.has_value() ? debug_line_str.value() : empty_str;
      std::size_t unit_offset = 0;
      while (unit_offset + 23 <= types.size()) {
        const auto current_unit_offset = unit_offset;
        const auto unit_length = read_u32_le(types, current_unit_offset);
        if (unit_length == 0) {
          unit_offset = current_unit_offset + 4;
          continue;
        }
        const auto unit_end = current_unit_offset + 4 + unit_length;
        if (unit_end > types.size()) {
          break;
        }
        try {
          const auto version = read_u16_le(types, current_unit_offset + 4);
          std::size_t header_offset = current_unit_offset + 6;
          ManualTypeUnitContext unit_context{};
          unit_context.version = version;
          std::uint32_t abbrev_offset = 0;
          Dwarf_Sig8 signature{};
          std::uint32_t type_offset = 0;
          if (version == 4) {
            abbrev_offset = read_u32_le(types, header_offset);
            if (abbrev_offset != 0) {
              unit_offset = unit_end;
              continue;
            }
            unit_context.addr_size = types[header_offset + 4];
            std::memcpy(signature.signature, types.data() + header_offset + 5,
                        sizeof(signature.signature));
            type_offset = read_u32_le(types, header_offset + 13);
          } else if (version == 5) {
            unit_context.unit_type = types[header_offset];
            abbrev_offset = read_u32_le(types, header_offset + 1);
            if (unit_context.unit_type != DW_UT_type) {
              unit_offset = unit_end;
              continue;
            }
            unit_context.addr_size = types[header_offset + 5];
            std::memcpy(signature.signature, types.data() + header_offset + 6,
                        sizeof(signature.signature));
            type_offset = read_u32_le(types, header_offset + 14);
          } else {
            unit_offset = unit_end;
            continue;
          }
          if (abbrev_offset >= debug_abbrev.value().size()) {
            unit_offset = unit_end;
            continue;
          }
          const auto abbrev_entries =
              load_manual_abbrev_entries(debug_abbrev.value(), abbrev_offset);
          const auto signature_hex = sig8_to_hex(signature);
          const auto die_offset = current_unit_offset + type_offset;
          if (die_offset >= unit_end) {
            unit_offset = unit_end;
            continue;
          }
          std::size_t cursor = die_offset;
          const auto abbrev_code = read_uleb128(types, cursor);
          const auto abbrev = abbrev_entries.find(abbrev_code);
          if (abbrev == abbrev_entries.end()) {
            unit_offset = unit_end;
            continue;
          }
          ManualTypeDescriptor descriptor;
          descriptor.tag = abbrev->second.tag;
          for (const auto &[attr, form] : abbrev->second.attributes) {
            auto effective_form = form;
            if (effective_form == DW_FORM_indirect) {
              effective_form =
                  static_cast<Dwarf_Half>(read_uleb128(types, cursor));
            }
            if (attr == DW_AT_str_offsets_base) {
              if (const auto value =
                      read_form_unsigned_value(effective_form, types, cursor);
                  value.has_value()) {
                unit_context.str_offsets_base = value.value();
              } else {
                manual_skip_form(effective_form, types, cursor);
              }
              continue;
            }
            if (attr == DW_AT_name && effective_form == DW_FORM_strp) {
              descriptor.name =
                  manual_read_string(strings, read_u32_le(types, cursor));
              cursor += 4;
              continue;
            }
            if (attr == DW_AT_name && effective_form == DW_FORM_line_strp) {
              descriptor.name =
                  manual_read_string(line_strings, read_u32_le(types, cursor));
              cursor += 4;
              continue;
            }
            if (attr == DW_AT_name && effective_form == DW_FORM_string) {
              const auto end =
                  std::find(types.begin() + static_cast<std::ptrdiff_t>(cursor),
                            types.end(), static_cast<std::uint8_t>(0));
              descriptor.name = std::string(
                  reinterpret_cast<const char *>(types.data() + cursor),
                  static_cast<std::size_t>(
                      end -
                      (types.begin() + static_cast<std::ptrdiff_t>(cursor))));
              cursor += descriptor.name->size() + 1;
              continue;
            }
            if (attr == DW_AT_name && (effective_form == DW_FORM_strx ||
                                       effective_form == DW_FORM_strx1 ||
                                       effective_form == DW_FORM_strx2 ||
                                       effective_form == DW_FORM_strx3 ||
                                       effective_form == DW_FORM_strx4)) {
              if (const auto value =
                      read_form_unsigned_value(effective_form, types, cursor);
                  value.has_value()) {
                descriptor.name = manual_read_strx_string(
                    string_offsets, strings, unit_context, value.value());
              } else {
                manual_skip_form(effective_form, types, cursor);
              }
              continue;
            }
            if (attr == DW_AT_type && effective_form == DW_FORM_ref_sig8) {
              Dwarf_Sig8 referenced{};
              std::memcpy(referenced.signature, types.data() + cursor,
                          sizeof(referenced.signature));
              descriptor.referenced_signature_hex = sig8_to_hex(referenced);
              cursor += 8;
              continue;
            }
            if (attr == DW_AT_byte_size) {
              if (auto val =
                      read_form_unsigned_value(effective_form, types, cursor)) {
                descriptor.byte_size = val.value();
              } else {
                manual_skip_form(effective_form, types, cursor);
              }
              continue;
            }
            manual_skip_form(effective_form, types, cursor);
          }
          // 在 descriptor 构建完成之后，emplace 之前加入：
          if (descriptor.tag == DW_TAG_array_type) {
            // 现在 cursor 指向根 DIE 之后，即子 DIE 序列的起始
            while (cursor < unit_end) {
              const auto child_code = read_uleb128(types, cursor);
              if (child_code == 0) {
                break; // 子 DIE 列表结束
              }
              auto child_it = abbrev_entries.find(child_code);
              if (child_it == abbrev_entries.end()) {
                // 遇到未知缩写码，放弃解析当前 unit 的子 DIE 维度
                break;
              }
              if (child_it->second.tag != DW_TAG_subrange_type) {
                // 跳过不感兴趣的子 DIE（如 DW_TAG_enumeration_type）
                for (const auto &[attr, form] : child_it->second.attributes) {
                  auto eff_form =
                      (form == DW_FORM_indirect)
                          ? static_cast<Dwarf_Half>(read_uleb128(types, cursor))
                          : form;
                  manual_skip_form(eff_form, types, cursor);
                }
                continue;
              }

              // 这是一个 subrange_type，提取 upper_bound 或 count
              std::optional<std::uint64_t> upper_bound;
              std::optional<std::uint64_t> count;
              for (const auto &[attr, form] : child_it->second.attributes) {
                auto eff_form =
                    (form == DW_FORM_indirect)
                        ? static_cast<Dwarf_Half>(read_uleb128(types, cursor))
                        : form;
                if (attr == DW_AT_upper_bound) {
                  if (auto val =
                          read_form_unsigned_value(eff_form, types, cursor)) {
                    upper_bound = val;
                  } else {
                    manual_skip_form(eff_form, types, cursor);
                  }
                } else if (attr == DW_AT_count) {
                  if (auto val =
                          read_form_unsigned_value(eff_form, types, cursor)) {
                    count = val;
                  } else {
                    manual_skip_form(eff_form, types, cursor);
                  }
                } else {
                  manual_skip_form(eff_form, types, cursor);
                }
              }

              if (upper_bound.has_value()) {
                descriptor.dimensions.push_back(upper_bound.value() + 1);
              } else if (count.has_value()) {
                descriptor.dimensions.push_back(count.value());
              } else {
                // 没有界限信息，填入 0 表示未知维度
                descriptor.dimensions.push_back(0);
              }
            }
          }
          descriptors.emplace(signature_hex, std::move(descriptor));
        } catch (...) {
          // TI 的 type unit 里会混入我们暂时不关心的 vendor / block / exprloc
          // 形式。 这里按 unit 粒度跳过，避免单个 unit
          // 失败把整张手工类型表清空。
          unit_offset = unit_end;
          continue;
        }
        unit_offset = unit_end;
      }
    }
  } catch (...) {
  }

  return cache.emplace(file_path, std::move(descriptors)).first->second;
}

[[nodiscard]] std::string make_type_id(const Dwarf_Off offset, const bool is_info) {
  if (is_info) {
    return "type@" + std::to_string(offset);
  }
  return "type@types:" + std::to_string(offset);
}

[[nodiscard]] std::string maybe_name(const std::optional<std::string>& value,
                                     const std::string& fallback) {
  if (value.has_value() && !value->empty()) {
    return value.value();
  }
  return fallback;
}

[[nodiscard]] std::string trim_copy(const std::string& value) {
  std::size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  std::size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

[[nodiscard]] std::string normalize_rule_path(std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  return value;
}

[[nodiscard]] bool wildcard_match_impl(const std::string& value,
                                       const std::string& pattern,
                                       const std::size_t value_index,
                                       const std::size_t pattern_index) {
  if (pattern_index == pattern.size()) {
    return value_index == value.size();
  }
  if (pattern[pattern_index] == '*') {
    const bool double_star =
      pattern_index + 1 < pattern.size() && pattern[pattern_index + 1] == '*';
    const std::size_t next_pattern_index = pattern_index + (double_star ? 2 : 1);
    for (std::size_t next_value_index = value_index; next_value_index <= value.size(); ++next_value_index) {
      if (!double_star && next_value_index > value_index && value[next_value_index - 1] == '/') {
        break;
      }
      if (wildcard_match_impl(value, pattern, next_value_index, next_pattern_index)) {
        return true;
      }
    }
    return false;
  }
  if (value_index >= value.size()) {
    return false;
  }
  if (pattern[pattern_index] == '?') {
    return value[value_index] != '/' &&
           wildcard_match_impl(value, pattern, value_index + 1, pattern_index + 1);
  }
  if (pattern[pattern_index] != value[value_index]) {
    return false;
  }
  return wildcard_match_impl(value, pattern, value_index + 1, pattern_index + 1);
}

[[nodiscard]] bool wildcard_match(const std::string& value, const std::string& pattern) {
  const std::string normalized_value = normalize_rule_path(value);
  const std::string normalized_pattern = normalize_rule_path(pattern);
  if (wildcard_match_impl(normalized_value, normalized_pattern, 0, 0)) {
    return true;
  }

  // gitignore 风格中 `**/foo.c` 既能匹配任意目录下的 foo.c，也能匹配根部的 foo.c。
  if (normalized_pattern.starts_with("**/")) {
    return wildcard_match_impl(normalized_value, normalized_pattern.substr(3), 0, 0);
  }
  return false;
}

[[nodiscard]] bool should_skip_compile_unit(const ReaderContext& context, const std::string& source_path) {
  if (source_path.empty() || context.load_policy.compile_unit_path_rules_text.empty()) {
    return false;
  }
  bool included = true;
  bool matched_any = false;
  std::istringstream lines(context.load_policy.compile_unit_path_rules_text);
  std::string line;
  while (std::getline(lines, line)) {
    std::string pattern = trim_copy(line);
    if (pattern.empty() || pattern.starts_with('#')) {
      continue;
    }
    bool exclude = true;
    if (pattern.starts_with('!')) {
      exclude = false;
      pattern.erase(pattern.begin());
    }
    if (pattern.empty() || !wildcard_match(source_path, pattern)) {
      continue;
    }
    matched_any = true;
    included = !exclude;
  }
  return matched_any && !included;
}

[[nodiscard]] bool should_trace_unknown_type_name(const std::string& name) {
  const char* raw = std::getenv("ELF_STATIC_VIEW_TRACE_UNKNOWN_NAMES");
  if (raw == nullptr || *raw == '\0') {
    return false;
  }
  const std::string filter = raw;
  if (filter == "1" || filter == "all" || filter == "*") {
    return true;
  }
  return filter.find(name) != std::string::npos;
}

void apply_symbol_addresses(const ElfSymbolTable& symbols, std::vector<VariableRecord>& variables) {
  for (auto& variable : variables) {
    // 多个 CU 里可能出现同名 file-static；如果 DWARF 已经给出精确绝对地址，
    // 这里不能再用按名称查到的 ELF 符号去覆盖，否则会把别的 CU 的同名局部符号地址串进来。
    if (variable.availability == Availability::StaticAddressKnown &&
        variable.address.kind == AddressKind::Absolute &&
        variable.address.absolute_address.has_value()) {
      continue;
    }

    std::vector<std::string> candidates;
    if (variable.linkage_name.has_value() && !variable.linkage_name->empty()) {
      candidates.push_back(variable.linkage_name.value());
    }
    candidates.push_back(analysis::join_scope(variable.scope_path, variable.name));
    candidates.push_back(variable.name);

    for (const auto& candidate : candidates) {
      const auto symbol = symbols.find(candidate);
      if (!symbol.has_value()) {
        continue;
      }
      variable.address.kind = AddressKind::Absolute;
      if (symbol->is_thread_local) {
        variable.is_thread_local = true;
        variable.variable_kind = VariableKind::ThreadLocal;
        variable.address.kind = AddressKind::Unknown;
        variable.address.absolute_address.reset();
        variable.address.location_description = "ELF symtab TLS";
        variable.availability = Availability::RuntimeOnly;
      } else {
        variable.address.absolute_address = symbol->value;
        variable.address.location_description = "ELF symtab";
        variable.availability = Availability::StaticAddressKnown;
        if (!variable.byte_size.has_value() && symbol->size > 0) {
          variable.byte_size = symbol->size;
        }
      }
      break;
    }
  }
}

[[nodiscard]] std::string variable_identity_name(const VariableRecord& variable) {
  const auto full_name = analysis::join_scope(variable.scope_path, variable.name);
  if (!full_name.empty() && variable.name != "<anon>") {
    return full_name;
  }
  if (variable.linkage_name.has_value() && !variable.linkage_name->empty()) {
    return variable.linkage_name.value();
  }
  return full_name;
}

void deduplicate_variables(std::vector<VariableRecord>& variables) {
  std::vector<VariableRecord> deduplicated;
  std::unordered_map<std::string, std::size_t> index_by_key;
  deduplicated.reserve(variables.size());
  for (auto& variable : variables) {
    const auto dedup_name = variable_identity_name(variable);
    const auto dedup_key = variable.compile_unit_name + "|" + dedup_name;
    const auto existing = index_by_key.find(dedup_key);
    if (existing == index_by_key.end()) {
      index_by_key.emplace(dedup_key, deduplicated.size());
      deduplicated.push_back(std::move(variable));
      continue;
    }

    const bool prefer_current =
      variable.availability == Availability::StaticAddressKnown &&
      deduplicated[existing->second].availability != Availability::StaticAddressKnown;
    if (prefer_current) {
      deduplicated[existing->second] = std::move(variable);
    }
  }
  variables = std::move(deduplicated);
}

[[nodiscard]] bool scope_path_is_suffix_of(const std::vector<std::string>& candidate_scope,
                                           const std::vector<std::string>& resolved_scope) {
  if (candidate_scope.size() >= resolved_scope.size()) {
    return false;
  }
  const auto offset = resolved_scope.size() - candidate_scope.size();
  for (std::size_t index = 0; index < candidate_scope.size(); ++index) {
    if (candidate_scope[index] != resolved_scope[index + offset]) {
      return false;
    }
  }
  return true;
}

void prune_unresolved_abstract_locals(std::vector<VariableRecord>& variables) {
  variables.erase(std::remove_if(variables.begin(),
                                 variables.end(),
                                 [](const VariableRecord& variable) {
                                   if (variable.availability != Availability::Unavailable) {
                                     return false;
                                   }
                                   if (variable.const_value.has_value() ||
                                       variable.const_value_text.has_value() ||
                                       variable.address.absolute_address.has_value()) {
                                     return false;
                                   }
                                   return variable.variable_kind == VariableKind::Local;
                                 }),
                  variables.end());
}

void prune_shadowed_unresolved_reference_placeholders(std::vector<VariableRecord>& variables) {
  variables.erase(std::remove_if(variables.begin(),
                                 variables.end(),
                                 [&](const VariableRecord& variable) {
                                   const bool is_candidate_kind =
                                     variable.variable_kind == VariableKind::Local ||
                                     variable.variable_kind == VariableKind::Parameter;
                                   if (!is_candidate_kind) {
                                     return false;
                                   }

                                   const bool has_materialized_value =
                                     variable.const_value.has_value() ||
                                     variable.const_value_text.has_value() ||
                                     variable.address.absolute_address.has_value();
                                   if (has_materialized_value) {
                                     return false;
                                   }

                                   const bool is_placeholder_location =
                                     variable.address.location_description == "missing" ||
                                     variable.address.location_description.rfind("op(", 0) == 0;
                                   if (!is_placeholder_location ||
                                       variable.availability != Availability::RuntimeOnly) {
                                     return false;
                                   }

                                   return std::any_of(variables.begin(),
                                                      variables.end(),
                                                      [&](const VariableRecord& other) {
                                                        if (&other == &variable) {
                                                          return false;
                                                        }
                                                        if (other.compile_unit_name !=
                                                              variable.compile_unit_name ||
                                                            other.name != variable.name ||
                                                            other.variable_kind !=
                                                              variable.variable_kind) {
                                                          return false;
                                                        }
                                                        const bool other_has_value =
                                                          other.const_value.has_value() ||
                                                          other.const_value_text.has_value() ||
                                                          other.address.absolute_address.has_value();
                                                        if (!other_has_value) {
                                                          return false;
                                                        }
                                                        return scope_path_is_suffix_of(
                                                          variable.scope_path, other.scope_path);
                                                      });
                                 }),
                  variables.end());
}

[[nodiscard]] TypeKind map_type_kind(const Dwarf_Half tag) {
  switch (tag) {
    case DW_TAG_base_type:
      return TypeKind::Base;
    case DW_TAG_pointer_type:
      return TypeKind::Pointer;
    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
      return TypeKind::Reference;
    case DW_TAG_ptr_to_member_type:
      return TypeKind::MemberPointer;
    case DW_TAG_typedef:
      return TypeKind::Typedef;
    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
    case kDwTagTiFarType:
      return TypeKind::Qualified;
    case DW_TAG_array_type:
      return TypeKind::Array;
    case DW_TAG_structure_type:
      return TypeKind::Struct;
    case DW_TAG_class_type:
      return TypeKind::Class;
    case DW_TAG_union_type:
      return TypeKind::Union;
    case DW_TAG_enumeration_type:
      return TypeKind::Enum;
    case DW_TAG_subroutine_type:
      return TypeKind::Subroutine;
    case DW_TAG_atomic_type:
      return TypeKind::Atomic;
    case DW_TAG_unspecified_type:
      return TypeKind::Unspecified;
    default:
      return TypeKind::Unknown;
  }
}

[[nodiscard]] bool is_class_scope_tag(const Dwarf_Half tag) {
  return tag == DW_TAG_class_type || tag == DW_TAG_structure_type || tag == DW_TAG_union_type;
}

[[nodiscard]] bool is_subprogram_scope_tag(const Dwarf_Half tag) {
  return tag == DW_TAG_subprogram || tag == DW_TAG_inlined_subroutine;
}

[[nodiscard]] bool scope_contains_subprogram(const std::vector<Dwarf_Half>& scope_tag_stack) {
  return std::any_of(scope_tag_stack.begin(), scope_tag_stack.end(), is_subprogram_scope_tag);
}

[[nodiscard]] bool scope_ends_with_class(const std::vector<Dwarf_Half>& scope_tag_stack) {
  return !scope_tag_stack.empty() && is_class_scope_tag(scope_tag_stack.back());
}

void index_class_declaration_scopes(ReaderContext& context,
                                    Dwarf_Die die,
                                    const bool is_info,
                                    const std::vector<std::string>& scope_stack,
                                    const std::vector<Dwarf_Half>& scope_tag_stack) {
  auto child = child_of(context.debug, die);
  if (!child.has_value()) {
    return;
  }

  auto current = take_die(std::move(child));
  while (current) {
    const auto tag = die_tag(current.get());
    if (tag == DW_TAG_variable && scope_ends_with_class(scope_tag_stack)) {
      if (const auto declaration_attr = attribute_of(context.debug, current.get(), DW_AT_declaration);
          declaration_attr.has_value() &&
          flag_attr(declaration_attr->get()).value_or(false)) {
        const auto declaration_offset = die_offset(current.get()).value_or(0);
        context.class_scope_by_offset[declaration_offset] = scope_stack;
        context.variable_static_member_by_offset[declaration_offset] = true;
      }
    }

    auto next_scope = scope_stack;
    auto next_tags = scope_tag_stack;
    if (tag == DW_TAG_namespace || tag == DW_TAG_class_type || tag == DW_TAG_structure_type) {
      next_scope.push_back(maybe_name(die_name(current.get()), "<anon>"));
      next_tags.push_back(tag);
      index_class_declaration_scopes(context, current.get(), is_info, next_scope, next_tags);
    }

    auto sibling = sibling_of(context.debug, current.get(), is_info);
    if (!sibling.has_value()) {
      break;
    }
    current = take_die(std::move(sibling));
  }
}

[[nodiscard]] std::string describe_location_ops(const std::vector<LocationOp>& operations) {
  if (operations.empty()) {
    return "missing";
  }

  const auto& first = operations.front();
  if (operations.size() == 1) {
    if (first.atom == DW_OP_fbreg) {
      std::ostringstream stream;
      stream << "frame-base+" << static_cast<std::int64_t>(first.operand1);
      return stream.str();
    }
    if (first.atom >= DW_OP_breg0 && first.atom <= DW_OP_breg31) {
      std::ostringstream stream;
      stream << "register-address " << first.name << ' '
             << static_cast<std::int64_t>(first.operand1);
      return stream.str();
    }
    if (first.atom == DW_OP_bregx) {
      std::ostringstream stream;
      stream << "register-address " << first.name << " reg(" << first.operand1 << ") "
             << static_cast<std::int64_t>(first.operand2);
      return stream.str();
    }
    if (first.atom >= DW_OP_reg0 && first.atom <= DW_OP_reg31) {
      return "register-value " + first.name;
    }
    if (first.atom == DW_OP_regx) {
      std::ostringstream stream;
      stream << "register-value " << first.name << " reg(" << first.operand1 << ")";
      return stream.str();
    }
    if (first.atom == DW_OP_call_frame_cfa) {
      return "call-frame-cfa";
    }
  }

  std::ostringstream stream;
  for (std::size_t index = 0; index < operations.size(); ++index) {
    const auto& op = operations[index];
    if (index > 0) {
      stream << ' ';
    }
    if (!op.name.empty()) {
      stream << op.name;
    } else {
      stream << "op(" << static_cast<unsigned int>(op.atom) << ")";
    }
  }
  return stream.str();
}

[[nodiscard]] std::optional<std::int64_t> data_member_location_offset(const ReaderContext& context,
                                                                      Dwarf_Attribute attr) {
  if (const auto offset_value = unsigned_attr(attr); offset_value.has_value()) {
    if (offset_value.value() <= static_cast<Dwarf_Unsigned>(std::numeric_limits<std::int64_t>::max())) {
      return static_cast<std::int64_t>(offset_value.value());
    }
    return std::nullopt;
  }
  if (const auto signed_value = signed_attr(attr); signed_value.has_value()) {
    return signed_value.value();
  }

  const auto location_desc = read_location_description(context.debug,
                                                       attr,
                                                       context.current_address_size,
                                                       context.current_offset_size,
                                                       context.current_dwarf_version);
  if (!location_desc.has_value() || location_desc->operations.size() != 1) {
    return std::nullopt;
  }

  const auto& operation = location_desc->operations.front();
  if (operation.atom != DW_OP_plus_uconst ||
      operation.operand1 > static_cast<Dwarf_Unsigned>(std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  // C2000 等工具链会把成员偏移编码为 DW_OP_plus_uconst，这里只接受单操作数表达式，避免复杂表达式被误算。
  return static_cast<std::int64_t>(operation.operand1);
}

[[nodiscard]] std::string language_name(const std::optional<Dwarf_Unsigned>& value) {
  if (!value.has_value()) {
    return "unknown";
  }
  switch (value.value()) {
    case DW_LANG_C:
      return "C";
    case DW_LANG_C_plus_plus:
    case DW_LANG_C_plus_plus_11:
    case DW_LANG_C_plus_plus_14:
    case DW_LANG_C_plus_plus_17:
    case DW_LANG_C_plus_plus_20:
      return "C++";
    default:
      return "other";
  }
}

[[nodiscard]] bool is_type_tag(const Dwarf_Half tag) {
  switch (tag) {
    case DW_TAG_base_type:
    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
    case DW_TAG_ptr_to_member_type:
    case DW_TAG_typedef:
    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
    case kDwTagTiFarType:
    case DW_TAG_array_type:
    case DW_TAG_structure_type:
    case DW_TAG_class_type:
    case DW_TAG_union_type:
    case DW_TAG_enumeration_type:
    case DW_TAG_subroutine_type:
    case DW_TAG_atomic_type:
    case DW_TAG_unspecified_type:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool is_ignored_vendor_tag(const Dwarf_Half tag) {
  switch (tag) {
    case kDwTagTiClone:
    case kDwTagTiCodeLabel:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] AddressInfo classify_location(const std::optional<LocationDescription>& location,
                                            const std::optional<Dwarf_Addr>& direct_address,
                                            Availability& availability) {
  AddressInfo info;
  if (location.has_value()) {
    info.location_entry_count = location->entry_count;
    for (const auto& entry : location->entries) {
      if (entry.raw_low_pc.has_value() || entry.raw_high_pc.has_value() ||
          entry.cooked_low_pc.has_value() || entry.cooked_high_pc.has_value()) {
        info.location_ranges.push_back({
          .raw_low_pc = entry.raw_low_pc,
          .raw_high_pc = entry.raw_high_pc,
          .cooked_low_pc = entry.cooked_low_pc,
          .cooked_high_pc = entry.cooked_high_pc,
          .debug_addr_unavailable = entry.debug_addr_unavailable,
        });
      }
    }
  }
  const bool has_indexed_absolute =
    location.has_value() &&
    location->kind == DW_LKIND_expression &&
    location->operations.size() == 1 &&
    (location->operations.front().atom == DW_OP_addrx ||
     location->operations.front().atom == DW_OP_GNU_addr_index);

  if (direct_address.has_value() && !has_indexed_absolute) {
    info.kind = AddressKind::Absolute;
    info.absolute_address = direct_address.value();
    info.location_description = "DW_FORM_addr";
    availability = Availability::StaticAddressKnown;
    return info;
  }

  if (!location.has_value() || location->operations.empty()) {
    info.kind = AddressKind::Unknown;
    info.location_description = "missing";
    availability = Availability::Unavailable;
    return info;
  }

  std::ostringstream description;
  description << describe_location_ops(location->operations);
  if (location->entry_count > 1) {
    description << " entries(" << location->entry_count << ")";
  }
  info.location_description = description.str();

  if (location->kind == DW_LKIND_expression && location->operations.size() == 1 &&
      location->operations.front().atom == DW_OP_addr) {
    info.kind = AddressKind::Absolute;
    info.absolute_address = location->operations.front().operand1;
    availability = Availability::StaticAddressKnown;
    return info;
  }

  if (direct_address.has_value() && has_indexed_absolute) {
    info.kind = AddressKind::Absolute;
    info.absolute_address = direct_address.value();
    info.location_description =
      location->operations.front().atom == DW_OP_GNU_addr_index ? "DW_OP_GNU_addr_index" : "DW_OP_addrx";
    availability = Availability::StaticAddressKnown;
    return info;
  }

  if (std::any_of(location->operations.begin(),
                  location->operations.end(),
                  [](const LocationOp& op) {
                    return op.atom == DW_OP_fbreg ||
                           (op.atom >= DW_OP_breg0 && op.atom <= DW_OP_breg31) ||
                           op.atom == DW_OP_bregx ||
                           (op.atom >= DW_OP_reg0 && op.atom <= DW_OP_reg31) ||
                           op.atom == DW_OP_regx || op.atom == DW_OP_call_frame_cfa;
                  })) {
    info.kind = AddressKind::Unknown;
    info.location_description = description.str();
    availability = Availability::RuntimeOnly;
    return info;
  }

  if (std::any_of(location->operations.begin(),
                  location->operations.end(),
                  [](const LocationOp& op) {
                     return op.atom == DW_OP_piece || op.atom == DW_OP_bit_piece ||
                            op.atom == DW_OP_stack_value;
                  })) {
    info.kind = AddressKind::Unknown;
    info.location_description = description.str();
    availability = Availability::RuntimeOnly;
    return info;
  }

  if (std::any_of(location->operations.begin(),
                  location->operations.end(),
                  [](const LocationOp& op) {
                    return op.atom == DW_OP_implicit_value || op.atom == DW_OP_entry_value ||
                           op.atom == DW_OP_const_type || op.atom == DW_OP_regval_type ||
                           op.atom == DW_OP_convert || op.atom == DW_OP_reinterpret;
                  })) {
    info.kind = AddressKind::Unknown;
    info.location_description = "value expression";
    availability = Availability::RuntimeOnly;
    return info;
  }

  info.kind = AddressKind::Unknown;
  availability = Availability::Unavailable;
  return info;
}

[[nodiscard]] std::optional<std::string> read_producer(Dwarf_Debug debug, Dwarf_Die die) {
  const auto attr = attribute_of(debug, die, DW_AT_producer);
  if (!attr.has_value()) {
    return std::nullopt;
  }
  return string_attr(attr->get());
}

[[nodiscard]] std::optional<std::string> read_compilation_name(Dwarf_Debug debug, Dwarf_Die die) {
  const auto attr = attribute_of(debug, die, DW_AT_name);
  if (!attr.has_value()) {
    return std::nullopt;
  }
  return string_attr(attr->get());
}

[[nodiscard]] std::optional<std::string> read_compilation_directory(Dwarf_Debug debug, Dwarf_Die die) {
  const auto attr = attribute_of(debug, die, DW_AT_comp_dir);
  if (!attr.has_value()) {
    return std::nullopt;
  }
  return string_attr(attr->get());
}

[[nodiscard]] std::string resolve_type_id(ReaderContext& context,
                                          const std::optional<ReferenceTarget>& target) {
  if (!target.has_value()) {
    return "type@unknown";
  }
  const auto key = make_type_id(target->offset, target->is_info);
  const auto iter = context.type_ids.find(key);
  if (iter != context.type_ids.end()) {
    return iter->second;
  }
  context.type_ids[key] = key;
  const auto& id = context.type_ids[key];
  return id;
}

[[nodiscard]] std::optional<ReferenceTarget> resolve_type_reference_target(ReaderContext& context,
                                                                           Dwarf_Attribute attr) {
  return type_reference_target(context.debug, attr, &context.file_path);
}

[[nodiscard]] std::optional<DieHandle> referenced_die(ReaderContext& context, Dwarf_Attribute attr) {
  const auto target = resolve_type_reference_target(context, attr);
  if (!target.has_value()) {
    return std::nullopt;
  }
  return die_from_offset(context.debug, target->offset, target->is_info);
}

void walk_die_tree(ReaderContext& context, Dwarf_Die die, bool is_info);

void ensure_type_recorded(ReaderContext& context, const ReferenceTarget& target) {
  const auto type_id = make_type_id(target.offset, target.is_info);
  if (context.recorded_type_ids.contains(type_id) || context.loading_type_ids.contains(type_id)) {
    return;
  }

  const auto die = die_from_offset(context.debug, target.offset, target.is_info);
  if (!die.has_value()) {
    return;
  }

  const char* raw = std::getenv("ELF_STATIC_VIEW_TRACE_TYPE_OFFSETS");
  if (raw != nullptr && *raw != '\0') {
    const std::string filter = raw;
    if (filter == "all" || filter == "*" || filter.find(std::to_string(target.offset)) != std::string::npos) {
      std::cerr << "[trace-type-offset] offset=" << target.offset << " is_info="
                << (target.is_info ? "1" : "0") << " tag="
                << static_cast<unsigned int>(die_tag(die->get())) << '\n';
    }
  }

  context.loading_type_ids.insert(type_id);
  walk_die_tree(context, die->get(), target.is_info);
  context.loading_type_ids.erase(type_id);
}

[[nodiscard]] std::optional<std::string> referenced_type_id(ReaderContext& context,
                                                            const std::optional<ReferenceTarget>& target) {
  if (!target.has_value()) {
    return std::nullopt;
  }
  ensure_type_recorded(context, target.value());
  return resolve_type_id(context, target);
}

[[nodiscard]] std::optional<std::string>
resolve_signature_type_id(ReaderContext &context, const Dwarf_Sig8 &signature) {
  const auto signature_hex = sig8_to_hex(signature);
  if (const auto found = context.signature_type_ids.find(signature_hex);
      found != context.signature_type_ids.end()) {
    return found->second;
  }
  const auto &manual = manual_type_descriptors(context.file_path);
  if (should_trace_unknown_type_name(signature_hex)) {
    std::cerr << "[trace-manual] sig=" << signature_hex
              << " present=" << (manual.contains(signature_hex) ? "1" : "0")
              << '\n';
  }
  if (const auto iter = manual.find(signature_hex); iter != manual.end()) {
    const auto type_id = make_manual_type_id(signature_hex);
    if (should_trace_unknown_type_name(signature_hex)) {
      std::cerr << "[trace-manual] use " << type_id
                << " tag=" << static_cast<unsigned int>(iter->second.tag)
                << '\n';
    }
    if (!context.recorded_type_ids.contains(type_id)) {
      TypeNode type;
      type.id = type_id;
      type.kind = map_type_kind(iter->second.tag);
      type.name = iter->second.name.value_or(type_id);
      if (iter->second.byte_size.has_value()) {
        type.byte_size = iter->second.byte_size;
      }
      if (iter->second.referenced_signature_hex.has_value()) {
        const auto referenced_id =
            make_manual_type_id(iter->second.referenced_signature_hex.value());
        if (type.kind == TypeKind::Pointer ||
            type.kind == TypeKind::Reference) {
          type.pointee_type = TypeRef{referenced_id};
        } else if (type.kind == TypeKind::Array) {
          type.element_type = TypeRef{referenced_id};
          type.array_dimensions = iter->second.dimensions; // 加上手工解析的维度
        } else if (type.kind == TypeKind::Typedef) {
          type.aliased_of = TypeRef{referenced_id};
        } else {
          type.qualified_of = TypeRef{referenced_id};
        }
      }
      context.type_ids[type_id] = type.id;
      context.recorded_type_ids.insert(type_id);
      context.types.push_back(std::move(type));
    }
    return type_id;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> referenced_type_id(ReaderContext& context,
                                                            Dwarf_Attribute attr) {
  const auto resolved = referenced_type_id(context, resolve_type_reference_target(context, attr));
  if (resolved.has_value()) {
    return resolved;
  }

  Dwarf_Half form = 0;
  Dwarf_Error error = nullptr;
  const int form_result = dwarf_whatform(attr, &form, &error);
  const char* trace_ref_fail = std::getenv("ELF_STATIC_VIEW_TRACE_REF_FAIL");
  const bool trace_ref = trace_ref_fail != nullptr && *trace_ref_fail != '\0';
  if (trace_ref) {
    std::cerr << "[trace-ref-fail] dwarf_whatform result=" << form_result
              << " form=" << static_cast<unsigned int>(form);
    if (error != nullptr) {
      std::cerr << " error=" << dwarf_errmsg(error);
    }
    std::cerr << '\n';
  }
  if (form_result == DW_DLV_OK && form == DW_FORM_ref_sig8) {
    Dwarf_Sig8 signature {};
    error = nullptr;
    const int sig_result = dwarf_formsig8(attr, &signature, &error);
    if (trace_ref) {
      std::cerr << "[trace-ref-fail] dwarf_formsig8 result=" << sig_result;
      if (sig_result == DW_DLV_OK) {
        std::cerr << " sig=" << sig8_to_hex(signature);
      }
      if (error != nullptr) {
        std::cerr << " error=" << dwarf_errmsg(error);
      }
      std::cerr << '\n';
    }
    if (sig_result == DW_DLV_OK) {
      if (const auto manual_type_id = resolve_signature_type_id(context, signature);
          manual_type_id.has_value()) {
        return manual_type_id;
      }
    }
  }
  if (error != nullptr) {
    dwarf_dealloc_error(nullptr, error);
  }
  return std::nullopt;
}

void inherit_variable_metadata_from_reference(ReaderContext& context,
                                              VariableRecord& variable,
                                              bool& is_static_member,
                                              Dwarf_Attribute reference_attr) {
  const auto reference_target = resolve_type_reference_target(context, reference_attr);
  if (!reference_target.has_value()) {
    return;
  }

  if (reference_target->is_info) {
    if (const auto found_scope = context.variable_scope_by_offset.find(reference_target->offset);
        found_scope != context.variable_scope_by_offset.end() && variable.scope_path.empty()) {
      variable.scope_path = found_scope->second;
    }
    if (const auto found_class_scope = context.class_scope_by_offset.find(reference_target->offset);
        found_class_scope != context.class_scope_by_offset.end()) {
      const auto& class_scope = found_class_scope->second;
      const bool can_refine_scope =
        variable.scope_path.empty() ||
        (variable.scope_path.size() < class_scope.size() &&
         std::equal(variable.scope_path.begin(),
                    variable.scope_path.end(),
                    class_scope.begin()));
      if (can_refine_scope) {
        variable.scope_path = class_scope;
      }
    }
  }
  if (reference_target->is_info) {
    if (const auto found_static_member =
          context.variable_static_member_by_offset.find(reference_target->offset);
        found_static_member != context.variable_static_member_by_offset.end()) {
      is_static_member = found_static_member->second;
    }
  }

  auto reference_die = die_from_offset(context.debug, reference_target->offset, reference_target->is_info);
  if (!reference_die.has_value()) {
    return;
  }

  if (variable.name == "<anon>") {
    variable.name = maybe_name(die_name(reference_die->get()), variable.name);
  }
  if (variable.type.id == "type@unknown") {
    if (const auto reference_type_attr = attribute_of(context.debug, reference_die->get(), DW_AT_type);
        reference_type_attr.has_value()) {
      variable.type =
        TypeRef{referenced_type_id(context, reference_type_attr->get()).value_or("type@unknown")};
    }
  }
  if (!variable.byte_size.has_value()) {
    if (const auto byte_size_attr = attribute_of(context.debug, reference_die->get(), DW_AT_byte_size);
        byte_size_attr.has_value()) {
      variable.byte_size = unsigned_attr(byte_size_attr->get());
    }
  }
  if (!variable.linkage_name.has_value() || variable.linkage_name->empty()) {
    if (const auto linkage_attr = attribute_of(context.debug, reference_die->get(), DW_AT_linkage_name);
        linkage_attr.has_value()) {
      variable.linkage_name = string_attr(linkage_attr->get());
    } else if (const auto mips_linkage_attr =
                 attribute_of(context.debug, reference_die->get(), DW_AT_MIPS_linkage_name);
               mips_linkage_attr.has_value()) {
      variable.linkage_name = string_attr(mips_linkage_attr->get());
    }
  }
  if (!variable.const_value.has_value() && !variable.const_value_text.has_value()) {
    if (const auto const_value_attr = attribute_of(context.debug, reference_die->get(), DW_AT_const_value);
        const_value_attr.has_value()) {
      variable.const_value = signed_const_attr(const_value_attr->get());
      variable.const_value_text = const_value_text_attr(const_value_attr->get());
      if (variable.const_value.has_value() || variable.const_value_text.has_value()) {
        variable.availability = Availability::StaticAddressKnown;
        variable.address.kind = AddressKind::Unknown;
        variable.address.location_description = "DW_AT_const_value";
      }
    }
  }
  if (variable.address.location_description == "missing" ||
      (variable.availability == Availability::Unavailable && !variable.address.absolute_address.has_value())) {
    if (const auto location_attr = attribute_of(context.debug, reference_die->get(), DW_AT_location);
        location_attr.has_value()) {
      const auto location_desc = read_location_description(context.debug,
                                                           location_attr->get(),
                                                           context.current_address_size,
                                                           context.current_offset_size,
                                                           context.current_dwarf_version);
      const auto direct_addr = address_attr(location_attr->get());
      const auto resolved_indexed_addr =
        location_desc.has_value() ? indexed_address_from_die_location(reference_die->get(), location_desc.value())
                                  : std::nullopt;
      auto inherited_availability = variable.availability;
      auto inherited_address = classify_location(location_desc,
                                                 direct_addr.has_value() ? direct_addr : resolved_indexed_addr,
                                                 inherited_availability);
      if (inherited_availability != Availability::Unavailable ||
          inherited_address.absolute_address.has_value() ||
          inherited_address.location_description != "missing") {
        variable.address = std::move(inherited_address);
        variable.availability = inherited_availability;
      }
    }
  }
}

void record_type(ReaderContext& context, Dwarf_Die die, const Dwarf_Half tag, const bool is_info) {
  const auto offset = die_offset(die).value_or(0);
  const auto type_id = make_type_id(offset, is_info);
  if (context.recorded_type_ids.contains(type_id)) {
    return;
  }

  bool is_declaration_only = false;
  if (const auto declaration_attr = attribute_of(context.debug, die, DW_AT_declaration);
      declaration_attr.has_value()) {
    is_declaration_only = flag_attr(declaration_attr->get()).value_or(false);
  }
  const auto declaration_name = die_name(die);
  if (const auto signature_attr = attribute_of(context.debug, die, DW_AT_signature);
      signature_attr.has_value()) {
    Dwarf_Sig8 signature {};
    Dwarf_Error error = nullptr;
    const int sig_result = dwarf_formsig8(signature_attr->get(), &signature, &error);
    if (sig_result == DW_DLV_OK) {
      if (const auto resolved_id = resolve_signature_type_id(context, signature);
          resolved_id.has_value()) {
        context.type_ids[type_id] = resolved_id.value();
        if (is_declaration_only || !declaration_name.has_value() || declaration_name->empty()) {
          context.recorded_type_ids.insert(type_id);
          return;
        }
      }
    }
    if (error != nullptr) {
      dwarf_dealloc_error(nullptr, error);
    }
  }

  auto type = TypeNode{};
  type.id = type_id;
  context.type_ids[type_id] = type.id;
  context.recorded_type_ids.insert(type_id);
  type.kind = map_type_kind(tag);
  type.name = maybe_name(declaration_name, type.id);

  if (const auto attr = attribute_of(context.debug, die, DW_AT_byte_size); attr.has_value()) {
    type.byte_size = unsigned_attr(attr->get());
  }
  if (const auto attr = attribute_of(context.debug, die, DW_AT_alignment); attr.has_value()) {
    type.alignment = unsigned_attr(attr->get());
  }
  if (const auto attr = attribute_of(context.debug, die, DW_AT_type); attr.has_value()) {
    const auto resolved_id = referenced_type_id(context, attr->get());
    if (resolved_id.has_value()) {
      if (type.kind == TypeKind::Pointer || type.kind == TypeKind::Reference) {
        type.pointee_type = TypeRef{resolved_id.value()};
      } else if (type.kind == TypeKind::MemberPointer) {
        type.pointee_type = TypeRef{resolved_id.value()};
      } else if (type.kind == TypeKind::Array) {
        type.element_type = TypeRef{resolved_id.value()};
      } else if (type.kind == TypeKind::Typedef) {
        type.aliased_of = TypeRef{resolved_id.value()};
      } else if (type.kind == TypeKind::Qualified) {
        type.qualified_of = TypeRef{resolved_id.value()};
      } else if (type.kind == TypeKind::Atomic) {
        type.qualified_of = TypeRef{resolved_id.value()};
      }
    }
  }

  if ((type.kind == TypeKind::Atomic || type.kind == TypeKind::Unspecified) &&
      type.name == type.id) {
    if (type.kind == TypeKind::Atomic && type.qualified_of.has_value()) {
      const auto iter = std::find_if(context.types.begin(), context.types.end(), [&](const TypeNode& item) {
        return item.id == type.qualified_of->id;
      });
      if (iter != context.types.end() && !iter->name.empty()) {
        type.name = "atomic " + iter->name;
      }
    } else if (type.kind == TypeKind::Unspecified) {
      type.name = "decltype(nullptr)";
    }
  }

  if (auto child = child_of(context.debug, die); child.has_value()) {
    auto current = take_die(std::move(child));
    while (current) {
      const auto child_tag = die_tag(current.get());
      if (type.kind == TypeKind::Array && child_tag == DW_TAG_subrange_type) {
        if (const auto upper_bound_attr = attribute_of(context.debug, current.get(), DW_AT_upper_bound);
            upper_bound_attr.has_value()) {
          if (const auto upper_bound = unsigned_attr(upper_bound_attr->get()); upper_bound.has_value()) {
            type.array_dimensions.push_back(upper_bound.value() + 1);
          }
        } else if (const auto count_attr = attribute_of(context.debug, current.get(), DW_AT_count);
                   count_attr.has_value()) {
          if (const auto count = unsigned_attr(count_attr->get()); count.has_value()) {
            type.array_dimensions.push_back(count.value());
          }
        }
      } else if ((type.kind == TypeKind::Struct || type.kind == TypeKind::Class ||
                  type.kind == TypeKind::Union) &&
                 child_tag == DW_TAG_member) {
        TypeMember member;
        member.name = maybe_name(die_name(current.get()), "<anon>");
        if (const auto member_type_attr = attribute_of(context.debug, current.get(), DW_AT_type);
            member_type_attr.has_value()) {
          member.type =
            TypeRef{referenced_type_id(context, member_type_attr->get()).value_or("type@unknown")};
        }
        member.availability = Availability::StaticLayoutKnown;
        member.address.kind = AddressKind::MemberOffset;
        bool declaration_only = false;
        if (const auto declaration_attr = attribute_of(context.debug, current.get(), DW_AT_declaration);
            declaration_attr.has_value()) {
          declaration_only = flag_attr(declaration_attr->get()).value_or(false);
        }
        if (const auto location_attr =
              attribute_of(context.debug, current.get(), DW_AT_data_member_location);
            location_attr.has_value()) {
          member.address.relative_offset = data_member_location_offset(context, location_attr->get());
        } else if (type.members.empty() && !declaration_only) {
          // DWARF 允许省略 0 偏移成员的位置属性；首个实例成员地址就是对象基址。
          member.address.relative_offset = 0;
        }
        if (const auto data_bit_offset_attr =
              attribute_of(context.debug, current.get(), DW_AT_data_bit_offset);
            data_bit_offset_attr.has_value()) {
          if (const auto bit_offset_value = unsigned_attr(data_bit_offset_attr->get());
              bit_offset_value.has_value()) {
            member.address.kind = AddressKind::BitField;
            member.address.bit_offset = bit_offset_value.value();
          }
        } else if (const auto bit_offset_attr =
                     attribute_of(context.debug, current.get(), DW_AT_bit_offset);
                   bit_offset_attr.has_value()) {
          if (const auto bit_offset_value = unsigned_attr(bit_offset_attr->get());
              bit_offset_value.has_value()) {
            member.address.kind = AddressKind::BitField;
            member.address.bit_offset = bit_offset_value.value();
          }
        }
        if (const auto bit_size_attr = attribute_of(context.debug, current.get(), DW_AT_bit_size);
            bit_size_attr.has_value()) {
          if (const auto bit_size_value = unsigned_attr(bit_size_attr->get()); bit_size_value.has_value()) {
            member.address.kind = AddressKind::BitField;
            member.address.bit_size = bit_size_value.value();
          }
        }
        type.members.push_back(std::move(member));
      } else if ((type.kind == TypeKind::Struct || type.kind == TypeKind::Class) &&
                 child_tag == DW_TAG_inheritance) {
        BaseRelation base;
        if (const auto base_type_attr = attribute_of(context.debug, current.get(), DW_AT_type);
            base_type_attr.has_value()) {
          base.type = TypeRef{referenced_type_id(context, base_type_attr->get()).value_or(
            "type@unknown")};
        }
        if (const auto offset_attr =
              attribute_of(context.debug, current.get(), DW_AT_data_member_location);
            offset_attr.has_value()) {
          if (const auto offset_value = unsigned_attr(offset_attr->get()); offset_value.has_value()) {
            base.offset = offset_value.value();
          }
        }
        type.bases.push_back(std::move(base));
      } else if (type.kind == TypeKind::Enum && child_tag == DW_TAG_enumerator) {
        if (const auto name = die_name(current.get()); name.has_value()) {
          type.enum_values.push_back(name.value());
        }
      }

      auto sibling = sibling_of(context.debug, current.get(), is_info);
      if (!sibling.has_value()) {
        break;
      }
      current = take_die(std::move(sibling));
    }
  }

  context.types.push_back(std::move(type));
}

[[nodiscard]] VariableKind classify_variable_kind(const std::vector<std::string>& scope_stack,
                                                  const std::vector<Dwarf_Half>& scope_tag_stack,
                                                  const bool external,
                                                  const bool has_static_storage,
                                                  const bool is_thread_local,
                                                  const bool is_static_member) {
  if (is_thread_local) {
    return VariableKind::ThreadLocal;
  }
  if (is_static_member || (has_static_storage && scope_ends_with_class(scope_tag_stack))) {
    return VariableKind::StaticMember;
  }
  if (scope_stack.empty()) {
    return external ? VariableKind::Global : VariableKind::FileStatic;
  }
  if (scope_contains_subprogram(scope_tag_stack)) {
    return has_static_storage ? VariableKind::FunctionStatic : VariableKind::Local;
  }
  return external ? VariableKind::Namespace : VariableKind::FileStatic;
}

[[nodiscard]] bool infer_static_storage(const std::optional<LocationDescription>& location_desc,
                                        const std::optional<std::int64_t>& const_value,
                                        const std::optional<std::string>& const_value_text) {
  if (const_value.has_value() || const_value_text.has_value()) {
    return true;
  }
  if (!location_desc.has_value()) {
    return false;
  }
  return std::any_of(location_desc->operations.begin(),
                     location_desc->operations.end(),
                     [](const LocationOp& op) {
                       return op.atom == DW_OP_addr || op.atom == DW_OP_addrx ||
                              op.atom == DW_OP_GNU_addr_index;
                     });
}

void record_variable(ReaderContext& context, Dwarf_Die die, const Dwarf_Half tag) {
  if (tag == DW_TAG_formal_parameter && context.load_policy.exclude_formal_parameters) {
    return;
  }
  VariableRecord variable;
  variable.id = "var@" + std::to_string(die_offset(die).value_or(0));
  variable.name = maybe_name(die_name(die), "<anon>");
  variable.compile_unit_name = context.current_compile_unit_name;
  variable.scope_path = context.scope_stack;
  auto scope_tag_stack = context.scope_tag_stack;
  bool is_static_member = scope_ends_with_class(scope_tag_stack);
  bool inherited_from_reference = false;
  variable.has_static_storage = false;

  if (const auto type_attr = attribute_of(context.debug, die, DW_AT_type); type_attr.has_value()) {
    const auto type_target = resolve_type_reference_target(context, type_attr->get());
    if (should_trace_unknown_type_name(variable.name)) {
      if (type_target.has_value()) {
        std::cerr << "[trace-target] " << variable.name << " -> offset=" << type_target->offset
                  << " is_info=" << (type_target->is_info ? "1" : "0") << '\n';
      } else {
        std::cerr << "[trace-target] " << variable.name << " -> <null>\n";
      }
    }
    // TI C2000 的部分变量类型会直接落到 DW_FORM_ref_sig8，libdwarf 解析 target 失败时
    // 这里必须回退到基于 attribute 的兜底逻辑，才能接上手工 .debug_types 类型图。
    auto resolved_type_id = referenced_type_id(context, type_target);
    if (!resolved_type_id.has_value()) {
      resolved_type_id = referenced_type_id(context, type_attr->get());
    }
    if (should_trace_unknown_type_name(variable.name)) {
      std::cerr << "[trace-type] " << variable.name << " -> "
                << resolved_type_id.value_or("type@unknown") << '\n';
    }
    variable.type = TypeRef{resolved_type_id.value_or("type@unknown")};
  } else {
    variable.type = TypeRef{"type@unknown"};
  }
  if (const auto byte_size_attr = attribute_of(context.debug, die, DW_AT_byte_size); byte_size_attr.has_value()) {
    variable.byte_size = unsigned_attr(byte_size_attr->get());
  }
  if (const auto linkage_attr = attribute_of(context.debug, die, DW_AT_linkage_name); linkage_attr.has_value()) {
    variable.linkage_name = string_attr(linkage_attr->get());
  } else if (const auto mips_linkage_attr =
               attribute_of(context.debug, die, DW_AT_MIPS_linkage_name);
             mips_linkage_attr.has_value()) {
    variable.linkage_name = string_attr(mips_linkage_attr->get());
  }

  const auto external_attr = attribute_of(context.debug, die, DW_AT_external);
  const bool external = external_attr.has_value() && flag_attr(external_attr->get()).value_or(false);
  const auto location_attr = attribute_of(context.debug, die, DW_AT_location);
  const auto location_desc = location_attr.has_value()
                               ? read_location_description(context.debug,
                                                           location_attr->get(),
                                                           context.current_address_size,
                                                           context.current_offset_size,
                                                           context.current_dwarf_version)
                               : std::nullopt;
  const auto direct_addr = location_attr.has_value()
                             ? address_attr(location_attr->get())
                             : std::nullopt;
  const auto resolved_indexed_addr =
    location_desc.has_value() ? indexed_address_from_die_location(die, location_desc.value()) : std::nullopt;
  const bool is_thread_local =
    location_desc.has_value() &&
    std::any_of(location_desc->operations.begin(),
                location_desc->operations.end(),
                [](const LocationOp& op) {
                  return op.atom == DW_OP_form_tls_address ||
                         op.atom == DW_OP_GNU_push_tls_address;
                });
  variable.is_thread_local = is_thread_local;
  // 这里先把位置表达式归一到统一模型，后面的 CLI 和测试都只依赖这个分类结果。
  variable.availability = Availability::Unavailable;
  variable.address = classify_location(location_desc,
                                       direct_addr.has_value() ? direct_addr : resolved_indexed_addr,
                                       variable.availability);
  if (const auto const_value_attr = attribute_of(context.debug, die, DW_AT_const_value);
      const_value_attr.has_value()) {
    variable.const_value = signed_const_attr(const_value_attr->get());
    variable.const_value_text = const_value_text_attr(const_value_attr->get());
    if (variable.const_value.has_value()) {
      variable.availability = Availability::StaticAddressKnown;
      variable.address.kind = AddressKind::Unknown;
      variable.address.location_description = "DW_AT_const_value";
    } else if (variable.const_value_text.has_value()) {
      variable.availability = Availability::StaticAddressKnown;
      variable.address.kind = AddressKind::Unknown;
      variable.address.location_description = "DW_AT_const_value";
    }
  }
  variable.has_static_storage =
    infer_static_storage(location_desc, variable.const_value, variable.const_value_text);

  if (is_thread_local) {
    variable.availability = Availability::RuntimeOnly;
    variable.address.kind = AddressKind::Unknown;
    variable.address.absolute_address.reset();
    variable.address.location_description = "thread_local";
  }

  context.variable_scope_by_offset[die_offset(die).value_or(0)] = variable.scope_path;

  if (tag == DW_TAG_formal_parameter) {
    variable.availability = Availability::RuntimeOnly;
    variable.has_static_storage = false;
  }

  if (const auto specification_attr = attribute_of(context.debug, die, DW_AT_specification);
      specification_attr.has_value()) {
    inherited_from_reference = true;
    inherit_variable_metadata_from_reference(
      context, variable, is_static_member, specification_attr->get());
  }
  if (const auto abstract_origin_attr = attribute_of(context.debug, die, DW_AT_abstract_origin);
      abstract_origin_attr.has_value()) {
    inherited_from_reference = true;
    inherit_variable_metadata_from_reference(
      context, variable, is_static_member, abstract_origin_attr->get());
  }

  if (variable.scope_path != context.scope_stack) {
    scope_tag_stack.clear();
    if (!variable.scope_path.empty() && !context.scope_tag_stack.empty() &&
        is_class_scope_tag(context.scope_tag_stack.back())) {
      is_static_member = true;
    }
  }
  variable.variable_kind =
    (tag == DW_TAG_formal_parameter)
      ? VariableKind::Parameter
      : classify_variable_kind(variable.scope_path,
                               scope_tag_stack,
                               external,
                               variable.has_static_storage,
                               is_thread_local,
                               is_static_member);

  bool is_declaration_only = false;
  if (const auto declaration_attr = attribute_of(context.debug, die, DW_AT_declaration);
      declaration_attr.has_value()) {
    is_declaration_only = flag_attr(declaration_attr->get()).value_or(false);
  }

  if (is_declaration_only &&
      !variable.address.absolute_address.has_value() &&
      variable.address.location_description == "missing") {
    // DW_AT_specification / 类内静态成员声明这类 DIE 只有声明语义，没有真实存储位置。
    // 对静态变量分析来说它们只是占位节点，应直接跳过，避免污染最终视图。
    if (!variable.scope_path.empty()) {
      context.class_scope_by_offset[die_offset(die).value_or(0)] = variable.scope_path;
    }
    return;
  }

  const bool unresolved_reference_placeholder =
    inherited_from_reference &&
    (variable.variable_kind == VariableKind::Local ||
     variable.variable_kind == VariableKind::Parameter ||
     variable.variable_kind == VariableKind::StaticMember) &&
    !variable.const_value.has_value() &&
    !variable.const_value_text.has_value() &&
    !variable.address.absolute_address.has_value() &&
    (variable.availability == Availability::Unavailable ||
     (variable.availability == Availability::RuntimeOnly &&
      (variable.address.location_description == "missing" ||
       variable.address.location_description.rfind("op(", 0) == 0)));
  if (unresolved_reference_placeholder) {
    // abstract_origin/specification 会带出纯占位的参数/局部节点；
    // 这些节点没有静态值，只会污染静态变量分析结果，这里直接跳过。
    return;
  }

  const auto variable_offset = die_offset(die).value_or(0);
  if (context.load_policy.static_storage_only &&
      variable.variable_kind != VariableKind::Global &&
      variable.variable_kind != VariableKind::Namespace &&
      variable.variable_kind != VariableKind::FileStatic &&
      variable.variable_kind != VariableKind::FunctionStatic &&
      variable.variable_kind != VariableKind::StaticMember &&
      variable.variable_kind != VariableKind::ThreadLocal) {
    return;
  }
  if (context.load_policy.exclude_runtime_only_variables &&
      (variable.availability == Availability::RuntimeOnly ||
       variable.availability == Availability::OptimizedOut)) {
    return;
  }
  context.variable_scope_by_offset[variable_offset] = variable.scope_path;
  context.variable_static_member_by_offset[variable_offset] = is_static_member;
  context.variables.push_back(std::move(variable));
}

void walk_children(ReaderContext& context, Dwarf_Die die, const bool is_info) {
  auto child = child_of(context.debug, die);
  if (!child.has_value()) {
    return;
  }
  auto current = take_die(std::move(child));
  while (current) {
    walk_die_tree(context, current.get(), is_info);
    auto sibling = sibling_of(context.debug, current.get(), is_info);
    if (!sibling.has_value()) {
      break;
    }
    current = take_die(std::move(sibling));
  }
}

void walk_die_tree(ReaderContext& context, Dwarf_Die die, const bool is_info) {
  const auto tag = die_tag(die);
  // TI C2000 样本里会出现 0x4088 / 0x4089 这类厂商私有 tag：
  // - 0x4088：附着在子程序下的 clone / abstract-name 标记
  // - 0x4089：汇编源码区间/代码标签样式的标记
  // 这两类节点当前不承载静态变量或类型图语义，显式跳过，避免后续误判成“未覆盖类型”。
  if (is_ignored_vendor_tag(tag)) {
    return;
  }

  const bool pushes_scope =
    tag == DW_TAG_subprogram || tag == DW_TAG_inlined_subroutine || tag == DW_TAG_namespace ||
    tag == DW_TAG_class_type || tag == DW_TAG_structure_type || tag == DW_TAG_lexical_block;
  if (pushes_scope) {
    std::string scope_name = maybe_name(die_name(die), "<anon>");
    if (tag == DW_TAG_lexical_block) {
      scope_name = "<block>";
    } else if (tag == DW_TAG_inlined_subroutine) {
      if (scope_name == "<anon>") {
        if (const auto abstract_origin_attr =
              attribute_of(context.debug, die, DW_AT_abstract_origin);
            abstract_origin_attr.has_value()) {
          if (auto abstract_origin_die = referenced_die(context, abstract_origin_attr->get());
              abstract_origin_die.has_value()) {
            scope_name = maybe_name(die_name(abstract_origin_die->get()), "<inlined>");
          }
        }
      }
      if (scope_name == "<anon>") {
        scope_name = "<inlined>";
      }
    }
    context.scope_stack.push_back(scope_name);
    context.scope_tag_stack.push_back(tag);
  }

  if (is_type_tag(tag)) {
    record_type(context, die, tag, is_info);
  } else if (tag == DW_TAG_variable || tag == DW_TAG_formal_parameter) {
    record_variable(context, die, tag);
  }

  walk_children(context, die, is_info);

  if (pushes_scope) {
    context.scope_tag_stack.pop_back();
    context.scope_stack.pop_back();
  }
}

}  // namespace

ProjectModel DwarfReader::load(const std::string& file_path, const LoadPolicy& load_policy) const {
  const auto dwarf_load_started_at = std::chrono::steady_clock::now();
  const ObjectFileKind file_kind = detect_object_file_kind(file_path);
  DebugHandle debug(file_path, file_kind);
  ReaderContext context;
  context.debug = debug.get();
  context.file_path = file_path;
  context.load_policy = load_policy;

  std::size_t index = 0;
  while (true) {
    Dwarf_Die cu_die_raw = nullptr;
    Dwarf_Unsigned cu_header_length = 0;
    Dwarf_Half version_stamp = 0;
    Dwarf_Off abbrev_offset = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Half length_size = 0;
    Dwarf_Half extension_size = 0;
    Dwarf_Sig8 type_signature {};
    Dwarf_Unsigned type_offset = 0;
    Dwarf_Unsigned next_cu_header = 0;
    Dwarf_Half header_cu_type = 0;
    Dwarf_Error error = nullptr;
    const int result = dwarf_next_cu_header_e(context.debug,
                                              true,
                                              &cu_die_raw,
                                              &cu_header_length,
                                              &version_stamp,
                                              &abbrev_offset,
                                              &address_size,
                                              &length_size,
                                              &extension_size,
                                              &type_signature,
                                              &type_offset,
                                              &next_cu_header,
                                              &header_cu_type,
                                              &error);
    if (result == DW_DLV_NO_ENTRY) {
      break;
    }
    if (result != DW_DLV_OK) {
      throw DwarfError("dwarf_next_cu_header_e failed: " + std::string(dwarf_errmsg(error)));
    }

    DieHandle cu_die(context.debug, cu_die_raw);
    if (header_cu_type == DW_UT_type) {
      if (auto type_child = child_of(context.debug, cu_die.get()); type_child.has_value()) {
        const auto signature_type_id = make_type_id(die_offset(type_child->get()).value_or(0), true);
        context.signature_type_ids[sig8_to_hex(type_signature)] = signature_type_id;
      }
    }
    CompileUnitRecord cu;
    cu.id = "cu@" + std::to_string(index);
    cu.name = maybe_name(read_compilation_name(context.debug, cu_die.get()), "<unknown>");
    cu.producer = maybe_name(read_producer(context.debug, cu_die.get()), "");
    if (const auto comp_dir = read_compilation_directory(context.debug, cu_die.get()); comp_dir.has_value() &&
        !comp_dir->empty() && !cu.name.empty()) {
      cu.source_path = normalize_rule_path(*comp_dir + "/" + cu.name);
    } else {
      cu.source_path = normalize_rule_path(cu.name);
    }
    context.current_address_size = address_size;
    context.current_offset_size = length_size;
    context.current_dwarf_version = version_stamp;
    if (const auto language_attr = attribute_of(context.debug, cu_die.get(), DW_AT_language);
        language_attr.has_value()) {
      cu.language = language_name(unsigned_attr(language_attr->get()));
    } else {
      cu.language = "unknown";
    }
    if (const auto ranges_attr = attribute_of(context.debug, cu_die.get(), DW_AT_ranges); ranges_attr.has_value()) {
      Availability availability = Availability::RuntimeOnly;
      cu.address =
        classify_location(read_range_description(ranges_attr->get(), cu_die.get()), std::nullopt, availability);
    } else {
      std::optional<LocationDescription> unit_location;
      LocationDescription::Entry entry;
      bool has_range = false;
      if (const auto low_pc_attr = attribute_of(context.debug, cu_die.get(), DW_AT_low_pc); low_pc_attr.has_value()) {
        if (const auto low_pc = address_attr(low_pc_attr->get()); low_pc.has_value()) {
          entry.cooked_low_pc = low_pc.value();
          has_range = true;
        }
      }
      if (const auto high_pc_attr = attribute_of(context.debug, cu_die.get(), DW_AT_high_pc);
          high_pc_attr.has_value()) {
        if (const auto high_pc_addr = address_attr(high_pc_attr->get()); high_pc_addr.has_value()) {
          entry.cooked_high_pc = high_pc_addr.value();
          has_range = true;
        } else if (const auto high_pc_offset = unsigned_attr(high_pc_attr->get()); high_pc_offset.has_value() &&
                   entry.cooked_low_pc.has_value()) {
          entry.cooked_high_pc = entry.cooked_low_pc.value() + high_pc_offset.value();
          has_range = true;
        }
      }
      if (has_range) {
        unit_location = LocationDescription {};
        unit_location->entry_count = 1;
        unit_location->entries.push_back(entry);
        Availability availability = Availability::RuntimeOnly;
        cu.address = classify_location(unit_location, std::nullopt, availability);
      }
    }
    context.current_compile_unit_name = cu.name;
    context.current_compile_unit_source_path = cu.source_path;
    if (!should_skip_compile_unit(context, cu.source_path)) {
      context.compile_units.push_back(cu);
      index_class_declaration_scopes(context, cu_die.get(), true, {}, {});
      walk_die_tree(context, cu_die.get(), true);
    } else {
      ++context.skipped_compile_unit_count;
    }
    ++index;
  }

  ProjectModel model;
  model.file = file_path;
  model.compile_units = std::move(context.compile_units);
  model.types = std::move(context.types);
  model.metrics.variable_count_before_filter = context.variables.size();
  model.symbols = std::move(context.variables);
  const auto symbol_table_started_at = std::chrono::steady_clock::now();
  if (file_kind == ObjectFileKind::Elf) {
    const auto symbol_table = ElfSymbolTable::load(file_path);
    model.metrics.symbol_table_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - symbol_table_started_at).count());
    model.elf_info = ElfFileInfo {.object_class = symbol_table.metadata().object_class,
                                  .byte_order = symbol_table.metadata().byte_order,
                                  .file_type = symbol_table.metadata().file_type,
                                  .machine = symbol_table.metadata().machine,
                                  .os_abi = symbol_table.metadata().os_abi};
    // DWARF 位置表达式不总能直接给出静态绝对地址，这里再用 ELF 符号表补一遍静态对象地址。
    apply_symbol_addresses(symbol_table, model.symbols);
  } else if (file_kind == ObjectFileKind::TiCoff) {
    model.metrics.symbol_table_ms = 0;
    model.elf_info = ElfFileInfo {.object_class = "TI-COFF",
                                  .byte_order = "Little endian",
                                  .file_type = "Executable",
                                  .machine = "TI C2000",
                                  .os_abi = "TI C2000 CGT"};
    // TI-COFF 变量地址依赖 DWARF location 表达式，暂不实现符号表补址。
  }
  const auto deduplicate_started_at = std::chrono::steady_clock::now();
  deduplicate_variables(model.symbols);
  model.metrics.deduplicate_ms = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - deduplicate_started_at).count());
  model.metrics.variable_count_after_filter = model.symbols.size();
  model.metrics.skipped_compile_unit_count = context.skipped_compile_unit_count;
  model.metrics.dwarf_load_ms = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - dwarf_load_started_at).count());
  prune_unresolved_abstract_locals(model.symbols);
  prune_shadowed_unresolved_reference_placeholders(model.symbols);
  return model;
}

}  // namespace elf_static_view::elf
