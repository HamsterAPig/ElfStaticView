#include "elf_static_view/project.hpp"

int main() {
  elf_static_view::StaticAddressQueryOptions options;
  options.name_query_text = "demo";
  return options.name_query_text.empty() ? 1 : 0;
}
