#include "elf/dwarf_wrappers.hpp"

#include <sstream>

namespace elf_static_view::elf {

namespace {

[[nodiscard]] std::string build_error_message(const std::string& prefix,
                                              Dwarf_Error error) {
  std::ostringstream stream;
  stream << prefix;
  if (error != nullptr) {
    stream << ": " << dwarf_errmsg(error);
  }
  return stream.str();
}

void destroy_error(Dwarf_Error error) {
  if (error != nullptr) {
    dwarf_dealloc_error(nullptr, error);
  }
}

}  // namespace

DwarfError::DwarfError(const std::string& message) : std::runtime_error(message) {}

DebugHandle::DebugHandle(const std::string& file_path) {
  Dwarf_Error error = nullptr;
  char actual_path_buffer[2048] = {};
  char* actual_path = nullptr;
  const int result = dwarf_init_path(file_path.c_str(),
                                     actual_path_buffer,
                                     sizeof(actual_path_buffer),
                                     DW_GROUPNUMBER_ANY,
                                     nullptr,
                                     nullptr,
                                     &debug_,
                                     &error);
  if (result != DW_DLV_OK) {
    const auto message = build_error_message("dwarf_init_path failed", error);
    destroy_error(error);
    throw DwarfError(message);
  }
  (void)actual_path;
}

DebugHandle::~DebugHandle() {
  if (debug_ != nullptr) {
    dwarf_finish(debug_);
  }
}

Dwarf_Debug DebugHandle::get() const noexcept { return debug_; }

DieHandle::DieHandle(Dwarf_Debug debug, Dwarf_Die die) : debug_(debug), die_(die) {}

DieHandle::~DieHandle() { reset(); }

DieHandle::DieHandle(DieHandle&& other) noexcept : debug_(other.debug_), die_(other.die_) {
  other.debug_ = nullptr;
  other.die_ = nullptr;
}

DieHandle& DieHandle::operator=(DieHandle&& other) noexcept {
  if (this != &other) {
    reset();
    debug_ = other.debug_;
    die_ = other.die_;
    other.debug_ = nullptr;
    other.die_ = nullptr;
  }
  return *this;
}

Dwarf_Die DieHandle::get() const noexcept { return die_; }

DieHandle::operator bool() const noexcept { return die_ != nullptr; }

void DieHandle::reset() noexcept {
  if (debug_ != nullptr && die_ != nullptr) {
    dwarf_dealloc_die(die_);
  }
  debug_ = nullptr;
  die_ = nullptr;
}

AttributeHandle::AttributeHandle(Dwarf_Debug debug, Dwarf_Attribute attribute)
  : debug_(debug), attribute_(attribute) {}

AttributeHandle::~AttributeHandle() { reset(); }

AttributeHandle::AttributeHandle(AttributeHandle&& other) noexcept
  : debug_(other.debug_), attribute_(other.attribute_) {
  other.debug_ = nullptr;
  other.attribute_ = nullptr;
}

AttributeHandle& AttributeHandle::operator=(AttributeHandle&& other) noexcept {
  if (this != &other) {
    reset();
    debug_ = other.debug_;
    attribute_ = other.attribute_;
    other.debug_ = nullptr;
    other.attribute_ = nullptr;
  }
  return *this;
}

Dwarf_Attribute AttributeHandle::get() const noexcept { return attribute_; }

AttributeHandle::operator bool() const noexcept { return attribute_ != nullptr; }

void AttributeHandle::reset() noexcept {
  if (debug_ != nullptr && attribute_ != nullptr) {
    dwarf_dealloc_attribute(attribute_);
  }
  debug_ = nullptr;
  attribute_ = nullptr;
}

std::optional<DieHandle> child_of(Dwarf_Debug debug, Dwarf_Die die) {
  Dwarf_Die child = nullptr;
  Dwarf_Error error = nullptr;
  const int result = dwarf_child(die, &child, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    const auto message = build_error_message("dwarf_child failed", error);
    destroy_error(error);
    throw DwarfError(message);
  }
  return DieHandle(debug, child);
}

std::optional<DieHandle> sibling_of(Dwarf_Debug debug, Dwarf_Die die, bool is_info) {
  Dwarf_Die sibling = nullptr;
  Dwarf_Error error = nullptr;
  const int result = dwarf_siblingof_b(debug, die, is_info, &sibling, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    const auto message = build_error_message("dwarf_siblingof_b failed", error);
    destroy_error(error);
    throw DwarfError(message);
  }
  return DieHandle(debug, sibling);
}

std::optional<AttributeHandle> attribute_of(Dwarf_Debug debug, Dwarf_Die die, Dwarf_Half attr) {
  Dwarf_Attribute attribute = nullptr;
  Dwarf_Error error = nullptr;
  const int result = dwarf_attr(die, attr, &attribute, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    const auto message = build_error_message("dwarf_attr failed", error);
    destroy_error(error);
    throw DwarfError(message);
  }
  return AttributeHandle(debug, attribute);
}

std::optional<std::string> die_name(Dwarf_Die die) {
  char* name = nullptr;
  Dwarf_Error error = nullptr;
  const int result = dwarf_diename(die, &name, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    const auto message = build_error_message("dwarf_diename failed", error);
    destroy_error(error);
    throw DwarfError(message);
  }
  return std::string(name);
}

Dwarf_Half die_tag(Dwarf_Die die) {
  Dwarf_Half tag = 0;
  Dwarf_Error error = nullptr;
  const int result = dwarf_tag(die, &tag, &error);
  if (result != DW_DLV_OK) {
    const auto message = build_error_message("dwarf_tag failed", error);
    destroy_error(error);
    throw DwarfError(message);
  }
  return tag;
}

std::optional<Dwarf_Off> die_offset(Dwarf_Die die) {
  Dwarf_Off offset = 0;
  Dwarf_Error error = nullptr;
  const int result = dwarf_dieoffset(die, &offset, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    const auto message = build_error_message("dwarf_dieoffset failed", error);
    destroy_error(error);
    throw DwarfError(message);
  }
  return offset;
}

std::optional<Dwarf_Off> global_type_offset(Dwarf_Attribute attr) {
  Dwarf_Off offset = 0;
  Dwarf_Error error = nullptr;
  const int result = dwarf_global_formref(attr, &offset, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    const auto message = build_error_message("dwarf_global_formref failed", error);
    destroy_error(error);
    throw DwarfError(message);
  }
  return offset;
}

std::optional<DieHandle> die_from_offset(Dwarf_Debug debug, Dwarf_Off offset, bool is_info) {
  Dwarf_Die die = nullptr;
  Dwarf_Error error = nullptr;
  const int result = dwarf_offdie_b(debug, offset, is_info, &die, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    const auto message = build_error_message("dwarf_offdie_b failed", error);
    destroy_error(error);
    throw DwarfError(message);
  }
  return DieHandle(debug, die);
}

std::optional<Dwarf_Unsigned> unsigned_attr(Dwarf_Attribute attr) {
  Dwarf_Unsigned value = 0;
  Dwarf_Error error = nullptr;
  const int result = dwarf_formudata(attr, &value, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    return std::nullopt;
  }
  return value;
}

std::optional<Dwarf_Signed> signed_attr(Dwarf_Attribute attr) {
  Dwarf_Signed value = 0;
  Dwarf_Error error = nullptr;
  const int result = dwarf_formsdata(attr, &value, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    return std::nullopt;
  }
  return value;
}

std::optional<bool> flag_attr(Dwarf_Attribute attr) {
  Dwarf_Bool value = false;
  Dwarf_Error error = nullptr;
  const int result = dwarf_formflag(attr, &value, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    return std::nullopt;
  }
  return value != 0;
}

std::optional<std::string> string_attr(Dwarf_Attribute attr) {
  char* value = nullptr;
  Dwarf_Error error = nullptr;
  const int result = dwarf_formstring(attr, &value, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    return std::nullopt;
  }
  return std::string(value);
}

std::optional<Dwarf_Addr> address_attr(Dwarf_Attribute attr) {
  Dwarf_Addr value = 0;
  Dwarf_Error error = nullptr;
  const int result = dwarf_formaddr(attr, &value, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    return std::nullopt;
  }
  return value;
}

std::optional<LocationDescription> read_location_description(Dwarf_Attribute attr) {
  Dwarf_Loc_Head_c head = nullptr;
  Dwarf_Unsigned entry_count = 0;
  Dwarf_Error error = nullptr;
  const int result = dwarf_get_loclist_c(attr, &head, &entry_count, &error);
  if (result == DW_DLV_NO_ENTRY) {
    return std::nullopt;
  }
  if (result != DW_DLV_OK) {
    return std::nullopt;
  }

  LocationDescription description;
  dwarf_get_loclist_head_kind(head, &description.kind, &error);
  if (entry_count > 0) {
    Dwarf_Small lle_value = 0;
    Dwarf_Addr raw_lowpc = 0;
    Dwarf_Addr raw_hipc = 0;
    Dwarf_Bool debug_addr_unavailable = false;
    Dwarf_Addr lowpc_cooked = 0;
    Dwarf_Addr hipc_cooked = 0;
    Dwarf_Unsigned loclist_expr_op_count = 0;
    Dwarf_Locdesc_c locdesc = nullptr;
    Dwarf_Small loclist_source = 0;
    Dwarf_Unsigned expression_offset = 0;
    Dwarf_Unsigned locdesc_offset = 0;
    const int entry_result = dwarf_get_locdesc_entry_d(head,
                                                       0,
                                                       &lle_value,
                                                       &raw_lowpc,
                                                       &raw_hipc,
                                                       &debug_addr_unavailable,
                                                       &lowpc_cooked,
                                                       &hipc_cooked,
                                                       &loclist_expr_op_count,
                                                       &locdesc,
                                                       &loclist_source,
                                                       &expression_offset,
                                                       &locdesc_offset,
                                                       &error);
    if (entry_result == DW_DLV_OK) {
      for (Dwarf_Unsigned index = 0; index < loclist_expr_op_count; ++index) {
        LocationOp op;
        Dwarf_Unsigned offset_for_branch = 0;
        const int op_result =
          dwarf_get_location_op_value_c(locdesc,
                                        index,
                                        &op.atom,
                                        &op.operand1,
                                        &op.operand2,
                                        &op.operand3,
                                        &offset_for_branch,
                                        &error);
        if (op_result == DW_DLV_OK) {
          description.operations.push_back(op);
        }
      }
    }
  }
  dwarf_dealloc_loc_head_c(head);
  return description;
}

}  // namespace elf_static_view::elf
