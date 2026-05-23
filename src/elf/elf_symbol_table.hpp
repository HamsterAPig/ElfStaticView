#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace elf_static_view::elf {

struct SymbolInfo {
  std::string name;
  std::uint64_t value = 0;
  std::uint64_t size = 0;
  bool is_thread_local = false;
};

class ElfSymbolTable {
public:
  [[nodiscard]] static ElfSymbolTable load(const std::string& file_path);
  [[nodiscard]] std::optional<SymbolInfo> find(const std::string& name) const;

private:
  std::unordered_map<std::string, SymbolInfo> symbols_;
};

}  // namespace elf_static_view::elf
