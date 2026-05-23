#include "elf_static_view/project.hpp"

#include "analysis/model_utils.hpp"

#include <sstream>

namespace elf_static_view {

namespace {

void append_indent(std::ostringstream& stream, const int level) {
  for (int i = 0; i < level; ++i) {
    stream << "  ";
  }
}

void append_string_field(std::ostringstream& stream,
                         const std::string& key,
                         const std::string& value,
                         const int level,
                         const bool trailing_comma = true) {
  append_indent(stream, level);
  stream << '"' << key << "\": \"" << analysis::escape_json(value) << '"';
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

template <typename T>
void append_number_field(std::ostringstream& stream,
                         const std::string& key,
                         const std::optional<T>& value,
                         const int level,
                         const bool trailing_comma = true) {
  append_indent(stream, level);
  stream << '"' << key << "\": ";
  if (value.has_value()) {
    stream << value.value();
  } else {
    stream << "null";
  }
  if (trailing_comma) {
    stream << ',';
  }
  stream << '\n';
}

void append_expanded_node(std::ostringstream& stream, const ExpandedNode& node, const int level) {
  append_indent(stream, level);
  stream << "{\n";
  append_string_field(stream, "path", node.path, level + 1);
  append_string_field(stream, "display_name", node.display_name, level + 1);
  append_string_field(stream, "type_name", node.type_name, level + 1);
  append_string_field(stream, "type_kind", to_string(node.type_kind), level + 1);
  append_string_field(stream, "availability", to_string(node.availability), level + 1);
  append_number_field(stream, "absolute_address", node.absolute_address, level + 1);
  append_number_field(stream, "relative_offset", node.relative_offset, level + 1);
  append_number_field(stream, "byte_size", node.byte_size, level + 1);
  append_number_field(stream, "array_count", node.array_count, level + 1);
  append_number_field(stream, "array_stride", node.array_stride, level + 1);
  append_indent(stream, level + 1);
  stream << "\"children\": [\n";
  for (std::size_t index = 0; index < node.children.size(); ++index) {
    append_expanded_node(stream, node.children[index], level + 2);
    if (index + 1 < node.children.size()) {
      stream << ',';
    }
    stream << '\n';
  }
  append_indent(stream, level + 1);
  stream << "]\n";
  append_indent(stream, level);
  stream << '}';
}

}  // namespace

std::string render_dump_json(const ProjectModel& model) {
  std::ostringstream stream;
  stream << "{\n";
  append_string_field(stream, "file", model.file, 1);

  append_indent(stream, 1);
  stream << "\"compile_units\": [\n";
  for (std::size_t index = 0; index < model.compile_units.size(); ++index) {
    const auto& cu = model.compile_units[index];
    append_indent(stream, 2);
    stream << "{\n";
    append_string_field(stream, "id", cu.id, 3);
    append_string_field(stream, "name", cu.name, 3);
    append_string_field(stream, "producer", cu.producer, 3);
    append_string_field(stream, "language", cu.language, 3, false);
    append_indent(stream, 2);
    stream << '}';
    if (index + 1 < model.compile_units.size()) {
      stream << ',';
    }
    stream << '\n';
  }
  append_indent(stream, 1);
  stream << "],\n";

  append_indent(stream, 1);
  stream << "\"symbols\": [\n";
  for (std::size_t index = 0; index < model.symbols.size(); ++index) {
    const auto& symbol = model.symbols[index];
    append_indent(stream, 2);
    stream << "{\n";
    append_string_field(stream, "id", symbol.id, 3);
    append_string_field(stream, "name", symbol.name, 3);
    append_string_field(stream, "kind", to_string(symbol.variable_kind), 3);
    append_string_field(stream, "availability", to_string(symbol.availability), 3);
    append_string_field(stream, "type_id", symbol.type.id, 3);
    append_number_field(stream, "absolute_address", symbol.address.absolute_address, 3);
    append_number_field(stream, "relative_offset", symbol.address.relative_offset, 3);
    append_string_field(stream, "location_description", symbol.address.location_description, 3);
    append_indent(stream, 3);
    stream << "\"scope_path\": [";
    for (std::size_t scope_index = 0; scope_index < symbol.scope_path.size(); ++scope_index) {
      stream << '"' << analysis::escape_json(symbol.scope_path[scope_index]) << '"';
      if (scope_index + 1 < symbol.scope_path.size()) {
        stream << ", ";
      }
    }
    stream << "]\n";
    append_indent(stream, 2);
    stream << '}';
    if (index + 1 < model.symbols.size()) {
      stream << ',';
    }
    stream << '\n';
  }
  append_indent(stream, 1);
  stream << "],\n";

  append_indent(stream, 1);
  stream << "\"types\": [\n";
  for (std::size_t index = 0; index < model.types.size(); ++index) {
    const auto& type = model.types[index];
    append_indent(stream, 2);
    stream << "{\n";
    append_string_field(stream, "id", type.id, 3);
    append_string_field(stream, "name", type.name, 3);
    append_string_field(stream, "kind", to_string(type.kind), 3);
    append_number_field(stream, "byte_size", type.byte_size, 3, false);
    append_indent(stream, 2);
    stream << '}';
    if (index + 1 < model.types.size()) {
      stream << ',';
    }
    stream << '\n';
  }
  append_indent(stream, 1);
  stream << "],\n";

  append_indent(stream, 1);
  stream << "\"expanded\": [\n";
  for (std::size_t index = 0; index < model.expanded.size(); ++index) {
    append_expanded_node(stream, model.expanded[index], 2);
    if (index + 1 < model.expanded.size()) {
      stream << ',';
    }
    stream << '\n';
  }
  append_indent(stream, 1);
  stream << "]\n";
  stream << "}\n";
  return stream.str();
}

}  // namespace elf_static_view
