#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace elf_static_view::elf {

struct ElfFileMetadata {
  std::string object_class;
  std::string byte_order;
  std::string file_type;
  std::string machine;
  std::string os_abi;
};

struct SymbolInfo {
  std::string name;
  std::uint64_t value = 0;
  std::uint64_t size = 0;
  bool is_thread_local = false;
};

class ElfSymbolTable {
public:
  [[nodiscard]] static ElfSymbolTable load(const std::string& file_path);
  [[nodiscard]] static ElfFileMetadata inspect_file(const std::string& file_path);
  [[nodiscard]] static std::optional<std::vector<std::uint8_t>> read_section_bytes(
    const std::string& file_path,
    const std::string& section_name);
  [[nodiscard]] std::optional<SymbolInfo> find(const std::string& name) const;
  [[nodiscard]] const ElfFileMetadata& metadata() const;

private:
  ElfFileMetadata metadata_;
  std::unordered_map<std::string, SymbolInfo> symbols_;
};

}  // namespace elf_static_view::elf
