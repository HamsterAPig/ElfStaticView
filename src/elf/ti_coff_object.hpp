#pragma once

#include <dwarf.h>
#include <libdwarf.h>

#include <cstdint>
#include <string>
#include <vector>

namespace elf_static_view::elf {

enum class ObjectFileKind {
  Unknown,
  Elf,
  TiCoff,
};

struct TiCoffSection {
  std::string name;
  std::uint32_t physical_address = 0;
  std::uint32_t virtual_address = 0;
  std::uint32_t size = 0;
  std::uint32_t file_offset = 0;
  std::uint32_t relocation_offset = 0;
  std::uint32_t line_number_offset = 0;
  std::uint16_t relocation_count = 0;
  std::uint16_t line_number_count = 0;
  std::uint16_t flags = 0;
};

class TiCoffObject {
public:
  explicit TiCoffObject(std::string file_path);

  [[nodiscard]] const std::string& file_path() const noexcept;
  [[nodiscard]] Dwarf_Unsigned file_size() const noexcept;
  [[nodiscard]] Dwarf_Unsigned section_count() const noexcept;
  [[nodiscard]] const TiCoffSection* section_at(Dwarf_Unsigned section_index) const noexcept;
  [[nodiscard]] std::vector<std::uint8_t> read_section_data(
    Dwarf_Unsigned section_index) const;
  [[nodiscard]] std::vector<std::string> missing_required_debug_sections() const;

private:
  std::string file_path_;
  Dwarf_Unsigned file_size_ = 0;
  std::vector<std::uint8_t> file_data_;
  std::vector<TiCoffSection> sections_;
};

[[nodiscard]] bool is_ti_c2000_coff_file(const std::string& file_path);
[[nodiscard]] bool is_elf_file(const std::string& file_path);
[[nodiscard]] ObjectFileKind detect_object_file_kind(const std::string& file_path);
[[nodiscard]] Dwarf_Obj_Access_Interface_a make_ti_coff_dwarf_access(TiCoffObject& object);

}  // namespace elf_static_view::elf
