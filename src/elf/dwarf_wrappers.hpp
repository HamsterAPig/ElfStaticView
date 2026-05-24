#pragma once

#include <dwarf.h>
#include <libdwarf.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace elf_static_view::elf {

class DwarfError : public std::runtime_error {
public:
  explicit DwarfError(const std::string& message);
};

class DebugHandle {
public:
  explicit DebugHandle(const std::string& file_path);
  ~DebugHandle();

  DebugHandle(const DebugHandle&) = delete;
  DebugHandle& operator=(const DebugHandle&) = delete;

  [[nodiscard]] Dwarf_Debug get() const noexcept;

private:
  Dwarf_Debug debug_ = nullptr;
};

class DieHandle {
public:
  DieHandle() = default;
  DieHandle(Dwarf_Debug debug, Dwarf_Die die);
  ~DieHandle();

  DieHandle(DieHandle&& other) noexcept;
  DieHandle& operator=(DieHandle&& other) noexcept;

  DieHandle(const DieHandle&) = delete;
  DieHandle& operator=(const DieHandle&) = delete;

  [[nodiscard]] Dwarf_Die get() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

private:
  void reset() noexcept;

  Dwarf_Debug debug_ = nullptr;
  Dwarf_Die die_ = nullptr;
};

class AttributeHandle {
public:
  AttributeHandle() = default;
  AttributeHandle(Dwarf_Debug debug, Dwarf_Attribute attribute);
  ~AttributeHandle();

  AttributeHandle(AttributeHandle&& other) noexcept;
  AttributeHandle& operator=(AttributeHandle&& other) noexcept;

  AttributeHandle(const AttributeHandle&) = delete;
  AttributeHandle& operator=(const AttributeHandle&) = delete;

  [[nodiscard]] Dwarf_Attribute get() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

private:
  void reset() noexcept;

  Dwarf_Debug debug_ = nullptr;
  Dwarf_Attribute attribute_ = nullptr;
};

struct CuHeader {
  bool is_info = true;
  Dwarf_Unsigned next_header_offset = 0;
};

[[nodiscard]] std::optional<DieHandle> child_of(Dwarf_Debug debug, Dwarf_Die die);
[[nodiscard]] std::optional<DieHandle> sibling_of(Dwarf_Debug debug,
                                                  Dwarf_Die die,
                                                  bool is_info);
[[nodiscard]] std::optional<AttributeHandle> attribute_of(Dwarf_Debug debug,
                                                          Dwarf_Die die,
                                                          Dwarf_Half attr);
[[nodiscard]] std::optional<std::string> die_name(Dwarf_Die die);
[[nodiscard]] Dwarf_Half die_tag(Dwarf_Die die);
[[nodiscard]] std::optional<Dwarf_Off> die_offset(Dwarf_Die die);
[[nodiscard]] std::optional<Dwarf_Off> global_type_offset(Dwarf_Attribute attr);
[[nodiscard]] std::optional<DieHandle> die_from_offset(Dwarf_Debug debug,
                                                       Dwarf_Off offset,
                                                       bool is_info = true);
[[nodiscard]] std::optional<Dwarf_Unsigned> unsigned_attr(Dwarf_Attribute attr);
[[nodiscard]] std::optional<Dwarf_Signed> signed_attr(Dwarf_Attribute attr);
[[nodiscard]] std::optional<bool> flag_attr(Dwarf_Attribute attr);
[[nodiscard]] std::optional<std::string> string_attr(Dwarf_Attribute attr);
[[nodiscard]] std::optional<Dwarf_Addr> address_attr(Dwarf_Attribute attr);

struct LocationOp {
  Dwarf_Small atom = 0;
  Dwarf_Unsigned operand1 = 0;
  Dwarf_Unsigned operand2 = 0;
  Dwarf_Unsigned operand3 = 0;
};

struct LocationDescription {
  unsigned int kind = 0;
  std::vector<LocationOp> operations;
};

[[nodiscard]] std::optional<Dwarf_Addr> indexed_address_from_die_location(
  Dwarf_Die die,
  const LocationDescription& location);
[[nodiscard]] std::optional<LocationDescription> read_location_description(
  Dwarf_Attribute attr);

}  // namespace elf_static_view::elf
