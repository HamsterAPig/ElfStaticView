#include "elf_static_view/project.hpp"

#include "analysis/model_utils.hpp"

#include <yaml-cpp/yaml.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace elf_static_view {

namespace {

constexpr std::array<char, 8> kBinaryMagic {'E', 'S', 'V', 'E', 'X', 'P', '0', '1'};
constexpr std::uint32_t kBinaryVersion = 1;

[[nodiscard]] const char* to_wire_value(const ExportSource value) {
  switch (value) {
    case ExportSource::FullModel:
      return "full_model";
    case ExportSource::CurrentFilteredView:
      return "current_filtered_view";
  }
  return "full_model";
}

[[nodiscard]] const char* to_wire_value(const ExportPayloadKind value) {
  switch (value) {
    case ExportPayloadKind::FullSnapshot:
      return "full_snapshot";
    case ExportPayloadKind::VariableSummary:
      return "variable_summary";
  }
  return "full_snapshot";
}

[[nodiscard]] ExportSource parse_export_source(const std::string& value) {
  if (value == "full_model") {
    return ExportSource::FullModel;
  }
  if (value == "current_filtered_view") {
    return ExportSource::CurrentFilteredView;
  }
  throw std::runtime_error("未知导出来源: " + value);
}

[[nodiscard]] ExportPayloadKind parse_payload_kind(const std::string& value) {
  if (value == "full_snapshot") {
    return ExportPayloadKind::FullSnapshot;
  }
  if (value == "variable_summary") {
    return ExportPayloadKind::VariableSummary;
  }
  throw std::runtime_error("未知导出内容类型: " + value);
}

[[nodiscard]] std::string compact_json_text(const std::string& json) {
  std::string compact;
  compact.reserve(json.size());
  bool inside_string = false;
  bool escaped = false;
  for (const char value : json) {
    if (inside_string) {
      compact.push_back(value);
      if (escaped) {
        escaped = false;
      } else if (value == '\\') {
        escaped = true;
      } else if (value == '"') {
        inside_string = false;
      }
      continue;
    }
    if (value == '"') {
      inside_string = true;
      compact.push_back(value);
      continue;
    }
    if (value != ' ' && value != '\n' && value != '\r' && value != '\t') {
      compact.push_back(value);
    }
  }
  return compact;
}

void append_json_string(std::ostringstream& stream, const std::string& value) {
  stream << '"' << analysis::escape_json(value) << '"';
}

void append_lightweight_json(std::ostringstream& stream,
                             const LightweightExport& document,
                             const bool pretty,
                             const ExportOptions& options) {
  const char* newline = pretty ? "\n" : "";
  const char* one_indent = pretty ? "  " : "";
  const char* two_indent = pretty ? "    " : "";
  const char* after_colon = pretty ? ": " : ":";

  stream << "{" << newline;
  stream << one_indent << "\"schema_version\"" << after_colon << document.schema_version << "," << newline;
  stream << one_indent << "\"payload_kind\"" << after_colon;
  append_json_string(stream, to_wire_value(ExportPayloadKind::VariableSummary));
  stream << "," << newline;
  stream << one_indent << "\"source\"" << after_colon;
  append_json_string(stream, to_wire_value(document.source));
  stream << "," << newline;
  stream << one_indent << "\"include_sensitive_info\"" << after_colon
         << (options.include_sensitive_info ? "true" : "false") << "," << newline;
  stream << one_indent << "\"variables\"" << after_colon << "[" << newline;
  for (std::size_t index = 0; index < document.variables.size(); ++index) {
    const auto& variable = document.variables[index];
    stream << two_indent << "{" << newline;
    stream << two_indent << one_indent << "\"path\"" << after_colon;
    append_json_string(stream, variable.path);
    stream << "," << newline;
    stream << two_indent << one_indent << "\"name\"" << after_colon;
    append_json_string(stream, variable.name);
    stream << "," << newline;
    stream << two_indent << one_indent << "\"type_name\"" << after_colon;
    append_json_string(stream, variable.type_name);
    stream << "," << newline;
    stream << two_indent << one_indent << "\"address\"" << after_colon;
    if (variable.address.has_value()) {
      stream << variable.address.value();
    } else {
      stream << "null";
    }
    stream << newline << two_indent << "}";
    if (index + 1 < document.variables.size()) {
      stream << ",";
    }
    stream << newline;
  }
  stream << one_indent << "]" << newline << "}";
}

[[nodiscard]] LightweightVariableRecord parse_lightweight_variable(const YAML::Node& node) {
  LightweightVariableRecord record;
  record.path = node["path"].as<std::string>("");
  record.name = node["name"].as<std::string>("");
  record.type_name = node["type_name"].as<std::string>("");
  if (node["address"] && !node["address"].IsNull()) {
    record.address = node["address"].as<std::uint64_t>();
  }
  return record;
}

[[nodiscard]] LightweightExport parse_lightweight_json(const YAML::Node& root) {
  LightweightExport document;
  document.schema_version = root["schema_version"].as<std::uint64_t>();
  document.source = parse_export_source(root["source"].as<std::string>("full_model"));
  document.include_sensitive_info = root["include_sensitive_info"].as<bool>(true);
  for (const auto& item : root["variables"]) {
    document.variables.push_back(parse_lightweight_variable(item));
  }
  return document;
}

void append_u8(std::string& output, const std::uint8_t value) {
  output.push_back(static_cast<char>(value));
}

void append_u32(std::string& output, const std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    output.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

void append_u64(std::string& output, const std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    output.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

void append_string(std::string& output, const std::string& value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("导出字符串过长");
  }
  append_u32(output, static_cast<std::uint32_t>(value.size()));
  output.append(value);
}

[[nodiscard]] const TypeNode* find_type_by_id(const ProjectModel& model, const std::string& id) {
  for (const auto& type : model.types) {
    if (type.id == id) {
      return &type;
    }
  }
  return nullptr;
}

[[nodiscard]] std::string display_type_name(const ProjectModel& model, const TypeRef& type_ref) {
  const auto* type = find_type_by_id(model, type_ref.id);
  if (type == nullptr || type->name.empty()) {
    return type_ref.id;
  }
  return type->name;
}

[[nodiscard]] std::uint8_t binary_source_value(const ExportSource value) {
  return value == ExportSource::CurrentFilteredView ? 1 : 0;
}

[[nodiscard]] std::uint8_t binary_payload_value(const ExportPayloadKind value) {
  return value == ExportPayloadKind::VariableSummary ? 1 : 0;
}

[[nodiscard]] ExportSource parse_binary_source(const std::uint8_t value) {
  if (value == 0) {
    return ExportSource::FullModel;
  }
  if (value == 1) {
    return ExportSource::CurrentFilteredView;
  }
  throw std::runtime_error("未知二进制导出来源");
}

[[nodiscard]] ExportPayloadKind parse_binary_payload(const std::uint8_t value) {
  if (value == 0) {
    return ExportPayloadKind::FullSnapshot;
  }
  if (value == 1) {
    return ExportPayloadKind::VariableSummary;
  }
  throw std::runtime_error("未知二进制导出内容类型");
}

class BinaryReader {
public:
  explicit BinaryReader(std::string_view bytes) : bytes_(bytes) {}

  [[nodiscard]] std::uint8_t read_u8() {
    require(1);
    return static_cast<std::uint8_t>(bytes_[offset_++]);
  }

  [[nodiscard]] std::uint32_t read_u32() {
    require(4);
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[offset_++])) << shift;
    }
    return value;
  }

  [[nodiscard]] std::uint64_t read_u64() {
    require(8);
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[offset_++])) << shift;
    }
    return value;
  }

  [[nodiscard]] std::string read_string() {
    const auto size = read_u32();
    require(size);
    std::string value(bytes_.substr(offset_, size));
    offset_ += size;
    return value;
  }

  void require_finished() const {
    if (offset_ != bytes_.size()) {
      throw std::runtime_error("二进制导出文件尾部存在未知数据");
    }
  }

private:
  void require(const std::size_t size) const {
    if (bytes_.size() - offset_ < size) {
      throw std::runtime_error("二进制导出文件不完整");
    }
  }

  std::string_view bytes_;
  std::size_t offset_ = 0;
};

[[nodiscard]] std::string render_binary_variable_summary(const LightweightExport& document,
                                                         const ExportOptions& options) {
  std::string output;
  // 二进制容器固定小端编码，便于第三方以最少依赖解析。
  output.append(kBinaryMagic.data(), kBinaryMagic.size());
  append_u32(output, kBinaryVersion);
  append_u8(output, binary_payload_value(ExportPayloadKind::VariableSummary));
  append_u8(output, binary_source_value(document.source));
  append_u8(output, options.include_sensitive_info ? 1 : 0);
  append_u8(output, 0);
  if (document.variables.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("轻量导出记录数量过多");
  }
  append_u32(output, static_cast<std::uint32_t>(document.variables.size()));
  for (const auto& variable : document.variables) {
    append_string(output, variable.path);
    append_string(output, variable.name);
    append_string(output, variable.type_name);
    append_u8(output, variable.address.has_value() ? 1 : 0);
    append_u64(output, variable.address.value_or(0));
  }
  return output;
}

[[nodiscard]] std::string render_binary_full_snapshot(const ProjectSnapshot& snapshot,
                                                      const ExportOptions& options) {
  const auto json = render_snapshot_json(snapshot, {.include_sensitive_info = options.include_sensitive_info});
  std::string output;
  // 完整快照先复用现有 JSON 契约，再包进私有容器，避免复制一套全量模型编码。
  output.append(kBinaryMagic.data(), kBinaryMagic.size());
  append_u32(output, kBinaryVersion);
  append_u8(output, binary_payload_value(ExportPayloadKind::FullSnapshot));
  append_u8(output, binary_source_value(options.source));
  append_u8(output, options.include_sensitive_info ? 1 : 0);
  append_u8(output, 0);
  append_string(output, json);
  return output;
}

[[nodiscard]] ExportDocument parse_binary_export(std::string_view bytes) {
  if (bytes.size() < kBinaryMagic.size() ||
      !std::equal(kBinaryMagic.begin(), kBinaryMagic.end(), bytes.begin())) {
    throw std::runtime_error("不是 ElfStaticView 私有导出文件");
  }
  BinaryReader reader(bytes.substr(kBinaryMagic.size()));
  const auto version = reader.read_u32();
  if (version != kBinaryVersion) {
    throw std::runtime_error("不支持的私有导出版本");
  }
  const auto payload_kind = parse_binary_payload(reader.read_u8());
  const auto source = parse_binary_source(reader.read_u8());
  const bool include_sensitive_info = reader.read_u8() != 0;
  static_cast<void>(reader.read_u8());
  if (payload_kind == ExportPayloadKind::FullSnapshot) {
    const auto json = reader.read_string();
    reader.require_finished();
    return ExportDocument {ExportPayloadKind::FullSnapshot, parse_snapshot_json(json)};
  }

  LightweightExport document;
  document.source = source;
  document.include_sensitive_info = include_sensitive_info;
  const auto count = reader.read_u32();
  document.variables.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    LightweightVariableRecord variable;
    variable.path = reader.read_string();
    variable.name = reader.read_string();
    variable.type_name = reader.read_string();
    const bool has_address = reader.read_u8() != 0;
    const auto address = reader.read_u64();
    if (has_address) {
      variable.address = address;
    }
    document.variables.push_back(std::move(variable));
  }
  reader.require_finished();
  return ExportDocument {ExportPayloadKind::VariableSummary, std::move(document)};
}

void collect_expanded_nodes(const std::vector<ExpandedNode>& nodes,
                            const bool include_sensitive_info,
                            std::vector<LightweightVariableRecord>& output) {
  for (const auto& node : nodes) {
    LightweightVariableRecord record;
    record.path = include_sensitive_info ? node.path : node.display_name;
    record.name = node.display_name;
    record.type_name = node.type_name;
    record.address = node.absolute_address;
    output.push_back(std::move(record));
    collect_expanded_nodes(node.children, include_sensitive_info, output);
  }
}

}  // namespace

LightweightExport build_lightweight_export(const ProjectModel& model, const ExportOptions& options) {
  LightweightExport document;
  document.source = options.source;
  document.include_sensitive_info = options.include_sensitive_info;
  for (const auto& symbol : model.symbols) {
    LightweightVariableRecord record;
    record.path = options.include_sensitive_info ? symbol.id : symbol.name;
    record.name = symbol.name;
    record.type_name = display_type_name(model, symbol.type);
    record.address = symbol.address.absolute_address;
    document.variables.push_back(std::move(record));
  }
  return document;
}

LightweightExport build_lightweight_export(const std::vector<ExpandedNode>& nodes,
                                           const ExportOptions& options) {
  LightweightExport document;
  document.source = options.source;
  document.include_sensitive_info = options.include_sensitive_info;
  collect_expanded_nodes(nodes, options.include_sensitive_info, document.variables);
  return document;
}

std::string render_export_document(const ExportDocument& document, const ExportOptions& options) {
  if (options.payload_kind == ExportPayloadKind::FullSnapshot) {
    const auto& snapshot = std::get<ProjectSnapshot>(document.payload);
    if (options.format == ExportFormat::BinaryPrivate) {
      return render_binary_full_snapshot(snapshot, options);
    }
    auto json = render_snapshot_json(snapshot, {.include_sensitive_info = options.include_sensitive_info});
    if (options.format == ExportFormat::JsonCompact) {
      return compact_json_text(json);
    }
    return json;
  }

  const auto& lightweight = std::get<LightweightExport>(document.payload);
  if (options.format == ExportFormat::BinaryPrivate) {
    return render_binary_variable_summary(lightweight, options);
  }
  std::ostringstream stream;
  append_lightweight_json(stream, lightweight, options.format == ExportFormat::JsonPretty, options);
  return stream.str();
}

ExportDocument parse_export_bytes(const std::string& bytes, const ExportFormat format) {
  if (format == ExportFormat::BinaryPrivate) {
    return parse_binary_export(bytes);
  }

  const auto root = YAML::Load(bytes);
  const auto payload_kind = parse_payload_kind(root["payload_kind"].as<std::string>("full_snapshot"));
  if (payload_kind == ExportPayloadKind::VariableSummary) {
    return ExportDocument {payload_kind, parse_lightweight_json(root)};
  }
  return ExportDocument {payload_kind, parse_snapshot_json(bytes)};
}

ExportDocument load_export_file(const std::string& file_path, const ExportFormat format) {
  std::ifstream input(file_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("无法打开导出文件: " + file_path);
  }
  std::ostringstream stream;
  stream << input.rdbuf();
  return parse_export_bytes(stream.str(), format);
}

}  // namespace elf_static_view
