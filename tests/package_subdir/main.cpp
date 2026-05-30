#include "elf_static_view/project.hpp"

int main() {
  const std::string json =
      R"({"schema_version":1,"payload_kind":"variable_summary","source":"full_model","include_sensitive_info":true,"variables":[{"path":"demo","name":"demo","type_name":"int","address":4096}]})";
  const auto parsed = elf_static_view::parse_export_bytes(json, elf_static_view::ExportFormat::JsonCompact);
  const auto lightweight = std::get<elf_static_view::LightweightExport>(parsed.payload);
  return lightweight.variables.empty() || lightweight.variables.front().type_name != "int" ? 1 : 0;
}
