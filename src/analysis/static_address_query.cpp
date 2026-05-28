#include "elf_static_view/project.hpp"

#include "analysis/expander.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace elf_static_view {

namespace {

struct PathRule {
  std::string raw_pattern;
  std::string lowered_pattern;
  bool exclude = false;
};

struct PreparedNodeText {
  std::string lowered_path;
  std::string lowered_display_name;
};

struct QuerySignature {
  std::string name_query_text;
  std::string path_rules_text;
  bool include_runtime_only = false;
  bool only_static_known = true;
  std::size_t max_array_elements = 0;

  [[nodiscard]] bool matches(const StaticAddressQueryOptions& options) const {
    return name_query_text == options.name_query_text &&
           path_rules_text == options.path_rules_text &&
           include_runtime_only == options.include_runtime_only &&
           only_static_known == options.only_static_known &&
           max_array_elements == options.max_array_elements;
  }
};

[[nodiscard]] std::string trim_copy(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

[[nodiscard]] std::string to_lower_copy(std::string_view text) {
  std::string lowered;
  lowered.reserve(text.size());
  for (const char ch : text) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered;
}

[[nodiscard]] PreparedNodeText prepare_node_text(const ExpandedNode& node) {
  return PreparedNodeText {
    .lowered_path = to_lower_copy(node.path),
    .lowered_display_name = to_lower_copy(node.display_name),
  };
}

[[nodiscard]] std::vector<std::string> split_name_query_tokens(const std::string& name_query_text) {
  std::vector<std::string> tokens;
  std::size_t begin = 0;
  while (begin <= name_query_text.size()) {
    const auto comma = name_query_text.find(',', begin);
    const auto segment = comma == std::string::npos
                             ? std::string_view(name_query_text).substr(begin)
                             : std::string_view(name_query_text).substr(begin, comma - begin);
    auto token = trim_copy(segment);
    if (!token.empty()) {
      tokens.push_back(to_lower_copy(token));
    }
    if (comma == std::string::npos) {
      break;
    }
    begin = comma + 1;
  }
  return tokens;
}

[[nodiscard]] std::vector<PathRule> compile_path_rules(const std::string& path_rules_text) {
  std::vector<PathRule> rules;
  std::size_t begin = 0;
  while (begin <= path_rules_text.size()) {
    const auto newline = path_rules_text.find('\n', begin);
    const auto segment = newline == std::string::npos
                             ? std::string_view(path_rules_text).substr(begin)
                             : std::string_view(path_rules_text).substr(begin, newline - begin);
    auto line = trim_copy(segment);
    if (!line.empty()) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!line.empty() && line.front() != '#') {
        PathRule rule;
        if (line.front() == '!') {
          rule.exclude = true;
          line.erase(line.begin());
          line = trim_copy(line);
        }
        if (!line.empty()) {
          rule.raw_pattern = line;
          rule.lowered_pattern = to_lower_copy(line);
          rules.push_back(std::move(rule));
        }
      }
    }
    if (newline == std::string::npos) {
      break;
    }
    begin = newline + 1;
  }
  return rules;
}

[[nodiscard]] bool glob_match_impl(const std::string& pattern,
                                   const std::string& text,
                                   std::size_t pattern_index,
                                   std::size_t text_index) {
  while (pattern_index < pattern.size()) {
    const char current = pattern[pattern_index];
    if (current == '*') {
      const bool recursive = pattern_index + 1 < pattern.size() && pattern[pattern_index + 1] == '*';
      pattern_index += recursive ? 2 : 1;
      if (pattern_index >= pattern.size()) {
        if (recursive) {
          return true;
        }
        return text.find('.', text_index) == std::string::npos;
      }
      for (std::size_t next = text_index; next <= text.size(); ++next) {
        if (!recursive && next > text_index && text[next - 1] == '.') {
          break;
        }
        if (glob_match_impl(pattern, text, pattern_index, next)) {
          return true;
        }
      }
      return false;
    }
    if (text_index >= text.size()) {
      return false;
    }
    if (current == '?') {
      if (text[text_index] == '.') {
        return false;
      }
      ++pattern_index;
      ++text_index;
      continue;
    }
    if (current != text[text_index]) {
      return false;
    }
    ++pattern_index;
    ++text_index;
  }
  return text_index == text.size();
}

[[nodiscard]] bool matches_path_rules(const std::vector<PathRule>& rules,
                                      const std::string& lowered_path) {
  if (rules.empty()) {
    return true;
  }

  bool matched_include = false;
  bool has_include = false;
  for (const auto& rule : rules) {
    if (!rule.exclude) {
      has_include = true;
    }
    if (!glob_match_impl(rule.lowered_pattern, lowered_path, 0, 0)) {
      continue;
    }
    if (rule.exclude) {
      return false;
    }
    matched_include = true;
  }
  if (!has_include) {
    return true;
  }
  return matched_include;
}

[[nodiscard]] bool matches_name_tokens(const std::vector<std::string>& tokens,
                                       const PreparedNodeText& prepared) {
  if (tokens.empty()) {
    return true;
  }
  return std::any_of(tokens.begin(), tokens.end(), [&](const std::string& token) {
    return prepared.lowered_display_name.find(token) != std::string::npos ||
           prepared.lowered_path.find(token) != std::string::npos;
  });
}

[[nodiscard]] bool should_emit_node(const ExpandedNode& node,
                                    const StaticAddressQueryOptions& options) {
  if (!node.absolute_address.has_value()) {
    return false;
  }
  if (options.only_static_known && node.availability != Availability::StaticAddressKnown) {
    return false;
  }
  if (!options.include_runtime_only && node.availability == Availability::RuntimeOnly) {
    return false;
  }
  if (node.availability == Availability::Unavailable || node.availability == Availability::OptimizedOut) {
    return false;
  }
  return true;
}

[[nodiscard]] std::string fallback_type_name(const ExpandedNode& node) {
  if (!node.type_name.empty()) {
    return node.type_name;
  }
  return "unknown";
}

void flatten_expanded_node(const analysis::Expander& expander,
                          const ExpandedNode& node,
                          std::vector<ExpandedNode>& output,
                          std::unordered_set<std::string>& visited,
                          std::size_t max_array_elements) {
  if (!visited.insert(node.path).second) {
    return;
  }

  output.push_back(node);

  std::vector<ExpandedNode> children = node.children;
  if (node.children_lazy) {
    children = expander.expand_children(node);
  }
  if (node.type_kind == TypeKind::Array && max_array_elements > 0 && children.size() > max_array_elements) {
    children.resize(max_array_elements);
  }
  for (const auto& child : children) {
    flatten_expanded_node(expander, child, output, visited, max_array_elements);
  }
}

[[nodiscard]] analysis::Expander make_expander(const ProjectModel& model) {
  return analysis::Expander(model.types, 0, false);
}

}  // namespace

struct StaticAddressQuerySession::Impl {
  QuerySignature signature;
  std::vector<std::string> name_tokens;
  std::vector<PathRule> path_rules;
  std::vector<StaticAddressResult> cached_results;
  bool valid = false;
};

StaticAddressQuerySession::StaticAddressQuerySession(const ProjectModel& model)
  : model_(&model),
    expanded_nodes_(model.expanded),
    impl_(std::make_unique<Impl>()) {}

StaticAddressQuerySession::~StaticAddressQuerySession() = default;

StaticAddressQuerySession::StaticAddressQuerySession(StaticAddressQuerySession&&) noexcept = default;

StaticAddressQuerySession&
StaticAddressQuerySession::operator=(StaticAddressQuerySession&&) noexcept = default;

std::vector<StaticAddressResult>
StaticAddressQuerySession::query(const StaticAddressQueryOptions& options) {
  if (model_ == nullptr) {
    return {};
  }

  if (options.max_array_elements == 0) {
    throw std::invalid_argument("max_array_elements 必须大于 0");
  }

  if (impl_->valid && impl_->signature.matches(options)) {
    return impl_->cached_results;
  }

  impl_->signature = QuerySignature {
      .name_query_text = options.name_query_text,
      .path_rules_text = options.path_rules_text,
      .include_runtime_only = options.include_runtime_only,
      .only_static_known = options.only_static_known,
      .max_array_elements = options.max_array_elements,
  };
  impl_->name_tokens = split_name_query_tokens(options.name_query_text);
  impl_->path_rules = compile_path_rules(options.path_rules_text);

  auto expander = make_expander(*model_);
  std::vector<ExpandedNode> flattened_nodes;
  flattened_nodes.reserve(expanded_nodes_.size());
  std::unordered_set<std::string> visited;
  for (const auto& node : expanded_nodes_) {
    flatten_expanded_node(expander, node, flattened_nodes, visited, options.max_array_elements);
  }

  std::vector<StaticAddressResult> results;
  results.reserve(flattened_nodes.size());
  for (const auto& node : flattened_nodes) {
    const PreparedNodeText prepared = prepare_node_text(node);
    if (!matches_name_tokens(impl_->name_tokens, prepared)) {
      continue;
    }
    if (!matches_path_rules(impl_->path_rules, prepared.lowered_path)) {
      continue;
    }
    if (!should_emit_node(node, options)) {
      continue;
    }
    results.push_back(StaticAddressResult {
        .key = node.path,
        .value = node.absolute_address.value(),
        .value_type = fallback_type_name(node),
    });
  }

  impl_->cached_results = results;
  impl_->valid = true;
  return results;
}

std::vector<StaticAddressResult> query_static_addresses(
    const ProjectModel& model,
    const StaticAddressQueryOptions& options) {
  StaticAddressQuerySession session(model);
  return session.query(options);
}

std::vector<StaticAddressResult> query_static_addresses_from_file(
    const std::string& elf_path,
    const StaticAddressQueryOptions& options,
    const LoadPolicy& load_policy) {
  ProjectLoader loader;
  ScanOptions scan_options;
  scan_options.include_runtime_only = options.include_runtime_only;
  scan_options.load_policy = load_policy;
  scan_options.load_policy.exclude_runtime_only_variables = !options.include_runtime_only;
  scan_options.load_policy.static_storage_only = options.only_static_known;
  scan_options.load_policy.expand_depth = 6;
  scan_options.load_policy.lazy_expand_children = true;
  auto model = loader.scan(elf_path, scan_options);
  return query_static_addresses(model, options);
}

}  // namespace elf_static_view
