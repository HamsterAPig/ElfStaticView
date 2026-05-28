#include "elf_static_view/project.hpp"

int main() {
  elf_static_view::StaticAddressResult result;
  result.key = "demo";
  result.value_type = "int";
  return result.key.empty() || result.value_type.empty() ? 1 : 0;
}
