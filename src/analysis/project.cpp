#include "elf_static_view/project.hpp"

#include "analysis/address_bias.hpp"
#include "analysis/expander.hpp"
#include "analysis/model_utils.hpp"
#include "elf/dwarf_reader.hpp"

#include <sstream>

namespace elf_static_view {

std::string to_string(AddressKind value) {
  switch (value) {
    case AddressKind::Absolute:
      return "Absolute";
    case AddressKind::SectionRelative:
      return "SectionRelative";
    case AddressKind::MemberOffset:
      return "MemberOffset";
    case AddressKind::ArrayElementOffset:
      return "ArrayElementOffset";
    case AddressKind::BitField:
      return "BitField";
    case AddressKind::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

std::string to_string(Availability value) {
  switch (value) {
    case Availability::StaticAddressKnown:
      return "StaticAddressKnown";
    case Availability::StaticLayoutKnown:
      return "StaticLayoutKnown";
    case Availability::RuntimeOnly:
      return "RuntimeOnly";
    case Availability::Unavailable:
      return "Unavailable";
    case Availability::OptimizedOut:
      return "OptimizedOut";
  }
  return "Unavailable";
}

std::string to_string(TypeKind value) {
  switch (value) {
    case TypeKind::Base:
      return "Base";
    case TypeKind::Pointer:
      return "Pointer";
    case TypeKind::Reference:
      return "Reference";
    case TypeKind::Typedef:
      return "Typedef";
    case TypeKind::Qualified:
      return "Qualified";
    case TypeKind::Array:
      return "Array";
    case TypeKind::Struct:
      return "Struct";
    case TypeKind::Class:
      return "Class";
    case TypeKind::Union:
      return "Union";
    case TypeKind::Enum:
      return "Enum";
    case TypeKind::Subroutine:
      return "Subroutine";
    case TypeKind::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

std::string to_string(VariableKind value) {
  switch (value) {
    case VariableKind::Global:
      return "Global";
    case VariableKind::Namespace:
      return "Namespace";
    case VariableKind::FileStatic:
      return "FileStatic";
    case VariableKind::FunctionStatic:
      return "FunctionStatic";
    case VariableKind::StaticMember:
      return "StaticMember";
    case VariableKind::Local:
      return "Local";
    case VariableKind::Parameter:
      return "Parameter";
    case VariableKind::ThreadLocal:
      return "ThreadLocal";
    case VariableKind::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

ProjectModel ProjectLoader::scan(const std::string& file_path, const ScanOptions& options) const {
  elf::DwarfReader reader;
  auto model = reader.load(file_path);
  analysis::Expander expander(model.types, 6);
  model.expanded = expander.build(model.symbols, options.include_runtime_only, false, std::nullopt);
  return model;
}

ProjectModel ProjectLoader::dump(const std::string& file_path, const DumpOptions& options) const {
  elf::DwarfReader reader;
  auto model = reader.load(file_path);
  analysis::Expander expander(model.types, options.expand_depth);
  model.expanded = expander.build(model.symbols,
                                  options.include_runtime_only,
                                  options.only_static_known,
                                  options.symbol_name);
  return model;
}

std::string render_scan_text(const ProjectModel& model) {
  const auto summary = summarize(model);
  std::ostringstream stream;
  stream << "file: " << model.file << '\n';
  stream << "compile_units: " << summary.compile_unit_count << '\n';
  stream << "types: " << summary.type_count << '\n';
  stream << "symbols: " << summary.symbol_count << '\n';
  stream << "static_address_known: " << summary.static_address_known_count << '\n';
  stream << "runtime_only: " << summary.runtime_only_count << '\n';
  stream << "unavailable: " << summary.unavailable_count << '\n';
  return stream.str();
}

namespace {

void render_expanded_text(const ExpandedNode& node,
                          const int level,
                          const std::optional<std::int64_t> address_bias,
                          std::ostringstream& stream) {
  for (int i = 0; i < level; ++i) {
    stream << "  ";
  }
  stream << "- " << node.path << " [" << to_string(node.availability) << "] "
         << node.type_name;
  if (node.absolute_address.has_value()) {
    if (address_bias.has_value()) {
      stream << " @" << format_address_summary(node, address_bias.value());
    } else {
      stream << " @0x" << std::hex << node.absolute_address.value() << std::dec;
    }
  }
  stream << '\n';
  for (const auto& child : node.children) {
    render_expanded_text(child, level + 1, address_bias, stream);
  }
}

std::string render_dump_text_with_bias(const ProjectModel& model,
                                       const std::optional<std::int64_t> address_bias) {
  std::ostringstream stream;
  stream << "file: " << model.file << '\n';
  for (const auto& node : model.expanded) {
    render_expanded_text(node, 0, address_bias, stream);
  }
  return stream.str();
}

}  // namespace

std::string render_dump_text(const ProjectModel& model) {
  return render_dump_text_with_bias(model, std::nullopt);
}

std::string render_dump_text(const ProjectModel& model, const std::int64_t address_bias) {
  return render_dump_text_with_bias(model, std::optional<std::int64_t>(address_bias));
}

}  // namespace elf_static_view
