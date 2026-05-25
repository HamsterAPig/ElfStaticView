#include "elf/raw_dwarf_reader.hpp"

#include "elf/dwarf_wrappers.hpp"

#include <dwarf.h>
#include <libdwarf.h>

#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace elf_static_view::elf {

namespace {

[[nodiscard]] std::string current_timestamp_utc() {
  const auto now = std::time(nullptr);
  std::tm utc_time {};
#if defined(_WIN32)
  gmtime_s(&utc_time, &now);
#else
  gmtime_r(&now, &utc_time);
#endif
  std::ostringstream stream;
  stream << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
  return stream.str();
}

[[nodiscard]] std::string hex_value(const std::uint64_t value) {
  std::ostringstream stream;
  stream << "0x" << std::hex << value;
  return stream.str();
}

[[nodiscard]] std::string bytes_to_hex(const void* data, const Dwarf_Unsigned size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::ostringstream stream;
  stream << "0x" << std::hex << std::setfill('0');
  for (Dwarf_Unsigned index = 0; index < size; ++index) {
    stream << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  }
  return stream.str();
}

[[nodiscard]] std::string dwarf_error_text(const std::string& prefix, Dwarf_Error error) {
  std::ostringstream stream;
  stream << prefix;
  if (error != nullptr) {
    stream << ": " << dwarf_errmsg(error);
    dwarf_dealloc_error(nullptr, error);
  }
  return stream.str();
}

[[nodiscard]] std::string dwarf_name_or_hex(const unsigned int value,
                                            int (*resolver)(unsigned int, const char**)) {
  const char* name = nullptr;
  if (resolver(value, &name) == DW_DLV_OK && name != nullptr) {
    return name;
  }
  return hex_value(value);
}

[[nodiscard]] std::string form_class_name(const Dwarf_Form_Class form_class) {
  switch (form_class) {
    case DW_FORM_CLASS_ADDRESS:
      return "address";
    case DW_FORM_CLASS_BLOCK:
      return "block";
    case DW_FORM_CLASS_CONSTANT:
      return "constant";
    case DW_FORM_CLASS_EXPRLOC:
      return "exprloc";
    case DW_FORM_CLASS_FLAG:
      return "flag";
    case DW_FORM_CLASS_LINEPTR:
      return "lineptr";
    case DW_FORM_CLASS_LOCLISTPTR:
      return "loclistptr";
    case DW_FORM_CLASS_MACPTR:
      return "macptr";
    case DW_FORM_CLASS_RANGELISTPTR:
      return "rangelistptr";
    case DW_FORM_CLASS_REFERENCE:
      return "reference";
    case DW_FORM_CLASS_STRING:
      return "string";
    case DW_FORM_CLASS_FRAMEPTR:
      return "frameptr";
    case DW_FORM_CLASS_MACROPTR:
      return "macroptr";
    case DW_FORM_CLASS_ADDRPTR:
      return "addrptr";
    case DW_FORM_CLASS_LOCLIST:
      return "loclist";
    case DW_FORM_CLASS_LOCLISTSPTR:
      return "loclistsptr";
    case DW_FORM_CLASS_RNGLIST:
      return "rnglist";
    case DW_FORM_CLASS_RNGLISTSPTR:
      return "rnglistsptr";
    case DW_FORM_CLASS_STROFFSETSPTR:
      return "stroffsetsptr";
    case DW_FORM_CLASS_UNKNOWN:
    default:
      return "unknown";
  }
}

void record_local_error(RawDwarfDocument& document,
                        const std::size_t cu_index,
                        const Dwarf_Off die_offset_value,
                        const std::string& message) {
  std::ostringstream stream;
  stream << "cu@" << cu_index << " die@" << hex_value(die_offset_value) << ": " << message;
  document.errors.push_back(stream.str());
  document.status = "partial";
}

// 按 DWARF form class 读取最通用的可读值。失败时返回错误文本，
// 但不抛异常，避免单个 attribute 阻断整份 raw DWARF 导出。
[[nodiscard]] std::string read_attribute_value(Dwarf_Debug debug,
                                               Dwarf_Attribute attribute,
                                               const Dwarf_Half version,
                                               const Dwarf_Half offset_size,
                                               const Dwarf_Half attr_code,
                                               const Dwarf_Half form) {
  Dwarf_Error error = nullptr;
  const Dwarf_Form_Class form_class = dwarf_get_form_class(version, attr_code, offset_size, form);

  if (form_class == DW_FORM_CLASS_STRING) {
    char* value = nullptr;
    const int result = dwarf_formstring(attribute, &value, &error);
    if (result == DW_DLV_OK && value != nullptr) {
      return value;
    }
    return dwarf_error_text("string unavailable", error);
  }

  if (form_class == DW_FORM_CLASS_ADDRESS) {
    Dwarf_Addr value = 0;
    const int result = dwarf_formaddr(attribute, &value, &error);
    if (result == DW_DLV_OK) {
      return hex_value(value);
    }
    return dwarf_error_text("address unavailable", error);
  }

  if (form_class == DW_FORM_CLASS_FLAG) {
    Dwarf_Bool value = false;
    const int result = dwarf_formflag(attribute, &value, &error);
    if (result == DW_DLV_OK) {
      return value ? "true" : "false";
    }
    return dwarf_error_text("flag unavailable", error);
  }

  if (form_class == DW_FORM_CLASS_REFERENCE) {
    Dwarf_Off offset = 0;
    Dwarf_Bool is_info = true;
    const int result = dwarf_global_formref_b(attribute, &offset, &is_info, &error);
    if (result == DW_DLV_OK) {
      return std::string(is_info ? ".debug_info@" : ".debug_types@") + hex_value(offset);
    }
    return dwarf_error_text("reference unavailable", error);
  }

  if (form_class == DW_FORM_CLASS_EXPRLOC) {
    Dwarf_Unsigned expr_length = 0;
    Dwarf_Ptr expr_data = nullptr;
    const int result = dwarf_formexprloc(attribute, &expr_length, &expr_data, &error);
    if (result == DW_DLV_OK) {
      return bytes_to_hex(expr_data, expr_length);
    }
    return dwarf_error_text("exprloc unavailable", error);
  }

  if (form_class == DW_FORM_CLASS_BLOCK) {
    Dwarf_Block* block = nullptr;
    const int result = dwarf_formblock(attribute, &block, &error);
    if (result == DW_DLV_OK && block != nullptr) {
      const auto value = bytes_to_hex(block->bl_data, block->bl_len);
      dwarf_dealloc(debug, block, DW_DLA_BLOCK);
      return value;
    }
    return dwarf_error_text("block unavailable", error);
  }

  if (form_class == DW_FORM_CLASS_CONSTANT ||
      form_class == DW_FORM_CLASS_LINEPTR ||
      form_class == DW_FORM_CLASS_LOCLISTPTR ||
      form_class == DW_FORM_CLASS_MACPTR ||
      form_class == DW_FORM_CLASS_RANGELISTPTR ||
      form_class == DW_FORM_CLASS_FRAMEPTR ||
      form_class == DW_FORM_CLASS_MACROPTR ||
      form_class == DW_FORM_CLASS_ADDRPTR ||
      form_class == DW_FORM_CLASS_LOCLIST ||
      form_class == DW_FORM_CLASS_LOCLISTSPTR ||
      form_class == DW_FORM_CLASS_RNGLIST ||
      form_class == DW_FORM_CLASS_RNGLISTSPTR ||
      form_class == DW_FORM_CLASS_STROFFSETSPTR) {
    Dwarf_Unsigned unsigned_value = 0;
    const int unsigned_result = dwarf_formudata(attribute, &unsigned_value, &error);
    if (unsigned_result == DW_DLV_OK) {
      return std::to_string(unsigned_value);
    }
    if (error != nullptr) {
      dwarf_dealloc_error(nullptr, error);
      error = nullptr;
    }
    Dwarf_Signed signed_value = 0;
    const int signed_result = dwarf_formsdata(attribute, &signed_value, &error);
    if (signed_result == DW_DLV_OK) {
      return std::to_string(signed_value);
    }
    return dwarf_error_text(form_class_name(form_class) + " unavailable", error);
  }

  return "unhandled form_class=" + form_class_name(form_class);
}

// attrlist 返回的属性列表需要逐项释放；这里把每个 attribute 都转成
// name / form / value 三元组，保证上层 JSON 不依赖静态变量模型。
[[nodiscard]] std::vector<RawDwarfAttribute> read_attributes(Dwarf_Debug debug,
                                                             Dwarf_Die die,
                                                             const Dwarf_Half version,
                                                             const Dwarf_Half offset_size,
                                                             RawDwarfDocument& document,
                                                             const std::size_t cu_index,
                                                             const Dwarf_Off die_offset_value) {
  std::vector<RawDwarfAttribute> attributes;
  Dwarf_Attribute* attribute_list = nullptr;
  Dwarf_Signed attribute_count = 0;
  Dwarf_Error error = nullptr;
  const int result = dwarf_attrlist(die, &attribute_list, &attribute_count, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return attributes;
  }
  if (result != DW_DLV_OK) {
    record_local_error(document,
                       cu_index,
                       die_offset_value,
                       dwarf_error_text("dwarf_attrlist failed", error));
    return attributes;
  }

  for (Dwarf_Signed index = 0; index < attribute_count; ++index) {
    Dwarf_Attribute attribute = attribute_list[index];
    RawDwarfAttribute raw_attribute;
    Dwarf_Half attr_code = 0;
    error = nullptr;
    if (dwarf_whatattr(attribute, &attr_code, &error) == DW_DLV_OK) {
      raw_attribute.name = dwarf_name_or_hex(attr_code, dwarf_get_AT_name);
    } else {
      raw_attribute.name = dwarf_error_text("dwarf_whatattr failed", error);
    }

    Dwarf_Half form = 0;
    error = nullptr;
    if (dwarf_whatform(attribute, &form, &error) == DW_DLV_OK) {
      raw_attribute.form = dwarf_name_or_hex(form, dwarf_get_FORM_name);
      raw_attribute.value = read_attribute_value(debug, attribute, version, offset_size, attr_code, form);
    } else {
      raw_attribute.form = dwarf_error_text("dwarf_whatform failed", error);
      raw_attribute.value = "<unavailable>";
    }

    attributes.push_back(std::move(raw_attribute));
    dwarf_dealloc(debug, attribute, DW_DLA_ATTR);
  }
  dwarf_dealloc(debug, attribute_list, DW_DLA_LIST);
  return attributes;
}

// 递归遍历 DIE 树。child/sibling 读取失败只记录到 errors，
// 已经读出的 DIE 与兄弟/子节点尽量保留下来，符合 best-effort 策略。
[[nodiscard]] RawDwarfDie read_die_tree(Dwarf_Debug debug,
                                        Dwarf_Die die,
                                        const bool is_info,
                                        const Dwarf_Half version,
                                        const Dwarf_Half offset_size,
                                        RawDwarfDocument& document,
                                        const std::size_t cu_index) {
  RawDwarfDie raw_die;
  raw_die.offset = die_offset(die).value_or(0);
  raw_die.tag = dwarf_name_or_hex(die_tag(die), dwarf_get_TAG_name);
  raw_die.name = die_name(die).value_or("");
  raw_die.attributes = read_attributes(debug,
                                       die,
                                       version,
                                       offset_size,
                                       document,
                                       cu_index,
                                       raw_die.offset);

  try {
    auto child = child_of(debug, die);
    while (child.has_value()) {
      raw_die.children.push_back(read_die_tree(debug,
                                               child->get(),
                                               is_info,
                                               version,
                                               offset_size,
                                               document,
                                               cu_index));
      child = sibling_of(debug, child->get(), is_info);
    }
  } catch (const std::exception& error) {
    record_local_error(document, cu_index, raw_die.offset, error.what());
  }

  return raw_die;
}

}  // namespace

RawDwarfDocument RawDwarfReader::load(const std::string& file_path) const {
  DebugHandle debug(file_path);
  RawDwarfDocument document;
  document.source_file = file_path;
  document.exported_at = current_timestamp_utc();

  // raw 导出只依赖 libdwarf 基础 CU/DIE 遍历，不调用 DwarfReader::load()，
  // 因此静态变量解析失败不会影响这条导出通道。
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
    const int result = dwarf_next_cu_header_e(debug.get(),
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
      document.errors.push_back(dwarf_error_text("dwarf_next_cu_header_e failed", error));
      document.status = "partial";
      break;
    }

    try {
      DieHandle cu_die(debug.get(), cu_die_raw);
      RawDwarfCompileUnit compile_unit;
      compile_unit.index = index;
      compile_unit.version = version_stamp;
      compile_unit.header_length = cu_header_length;
      compile_unit.abbrev_offset = abbrev_offset;
      compile_unit.address_size = address_size;
      compile_unit.length_size = length_size;
      compile_unit.extension_size = extension_size;
      compile_unit.next_header_offset = next_cu_header;
      compile_unit.unit_type = dwarf_name_or_hex(header_cu_type, dwarf_get_UT_name);
      compile_unit.root = read_die_tree(debug.get(),
                                        cu_die.get(),
                                        true,
                                        version_stamp,
                                        length_size,
                                        document,
                                        index);
      document.compile_units.push_back(std::move(compile_unit));
    } catch (const std::exception& error) {
      document.errors.push_back("cu@" + std::to_string(index) + ": " + error.what());
      document.status = "partial";
    }

    ++index;
  }

  if (!document.errors.empty()) {
    document.status = "partial";
  }
  return document;
}

}  // namespace elf_static_view::elf
