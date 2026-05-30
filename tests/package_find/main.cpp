#include "elf_static_view/project.hpp"

int main() {
  elf_static_view::LightweightExport lightweight;
  lightweight.variables.push_back({"demo", "demo", "int", 0x1000});
  elf_static_view::ExportOptions options {
    elf_static_view::ExportFormat::BinaryPrivate,
    elf_static_view::ExportSource::FullModel,
    elf_static_view::ExportPayloadKind::VariableSummary,
    true,
  };
  elf_static_view::ExportDocument document {
    elf_static_view::ExportPayloadKind::VariableSummary,
    lightweight,
  };
  const auto bytes = elf_static_view::render_export_document(document, options);
  const auto parsed = elf_static_view::parse_export_bytes(bytes, options.format);
  return parsed.payload_kind == elf_static_view::ExportPayloadKind::VariableSummary ? 0 : 1;
}
