#include "ui/filter_matcher.hpp"

#include "analysis/model_utils.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>

namespace elf_static_view::ui {

namespace {

std::string trim(const std::string& value) {
  std::size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  std::size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string to_lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool wildcard_match_impl(const std::string& value,
                         const std::string& pattern,
                         const std::size_t value_index,
                         const std::size_t pattern_index) {
  if (pattern_index == pattern.size()) {
    return value_index == value.size();
  }

  if (pattern[pattern_index] == '*') {
    const bool double_star =
      pattern_index + 1 < pattern.size() && pattern[pattern_index + 1] == '*';
    const std::size_t next_pattern_index = pattern_index + (double_star ? 2 : 1);
    for (std::size_t next_value_index = value_index; next_value_index <= value.size(); ++next_value_index) {
      if (!double_star && next_value_index > value_index && value[next_value_index - 1] == ':') {
        break;
      }
      if (wildcard_match_impl(value, pattern, next_value_index, next_pattern_index)) {
        return true;
      }
    }
    return false;
  }

  if (value_index >= value.size()) {
    return false;
  }

  if (pattern[pattern_index] == '?') {
    return wildcard_match_impl(value, pattern, value_index + 1, pattern_index + 1);
  }

  return value[value_index] == pattern[pattern_index] &&
         wildcard_match_impl(value, pattern, value_index + 1, pattern_index + 1);
}

bool wildcard_match(const std::string& value, const std::string& pattern) {
  return wildcard_match_impl(value, pattern, 0, 0);
}

bool matches_name_query(const FilterState& state, const ExpandedNode& node) {
  if (state.form.variable_name_query.empty()) {
    return true;
  }
  const auto query = to_lower_copy(state.form.variable_name_query);
  const auto display_name = to_lower_copy(node.display_name);
  const auto path = to_lower_copy(node.path);
  return display_name.find(query) != std::string::npos || path.find(query) != std::string::npos;
}

bool matches_path_rules(const FilterState& state, const ExpandedNode& node) {
  if (state.rules.empty()) {
    return true;
  }

  bool included = false;
  bool has_include_rule = false;
  for (const auto& rule : state.rules) {
    if (!rule.exclude) {
      has_include_rule = true;
      if (wildcard_match(node.path, rule.normalized_pattern)) {
        included = true;
      }
    }
  }
  if (!has_include_rule) {
    included = true;
  }
  if (!included) {
    return false;
  }
  for (const auto& rule : state.rules) {
    if (rule.exclude && wildcard_match(node.path, rule.normalized_pattern)) {
      return false;
    }
  }
  return true;
}

}  // namespace

void compile_filter_rules(FilterState& state) {
  state.rules.clear();
  state.compile_error.reset();

  std::istringstream lines(state.form.path_rules_text);
  std::string line;
  while (std::getline(lines, line)) {
    const auto trimmed = trim(line);
    if (trimmed.empty() || trimmed.starts_with('#')) {
      continue;
    }
    FilterRule rule;
    rule.exclude = trimmed.starts_with('!');
    rule.raw_pattern = trimmed;
    rule.normalized_pattern = rule.exclude ? trim(trimmed.substr(1)) : trimmed;
    if (rule.normalized_pattern.empty()) {
      state.compile_error = "路径规则不能为空";
      state.rules.clear();
      return;
    }
    state.rules.push_back(std::move(rule));
  }
}

bool matches_filters(const AppState& state, const ExpandedNode& node) {
  if (state.filters.compile_error.has_value()) {
    return false;
  }
  if (state.filters.form.only_static_known && node.availability != Availability::StaticAddressKnown) {
    return false;
  }
  if (!state.filters.form.include_runtime_only &&
      (node.availability == Availability::RuntimeOnly ||
       node.availability == Availability::Unavailable ||
       node.availability == Availability::OptimizedOut)) {
    return false;
  }
  return matches_name_query(state.filters, node) && matches_path_rules(state.filters, node);
}

std::optional<std::uint64_t> apply_bias_to_absolute(const ExpandedNode& node, const std::int64_t bias) {
  if (!node.absolute_address.has_value()) {
    return std::nullopt;
  }
  const auto base = node.absolute_address.value();
  if (bias >= 0) {
    const auto positive_bias = static_cast<std::uint64_t>(bias);
    if (base > std::numeric_limits<std::uint64_t>::max() - positive_bias) {
      return std::nullopt;
    }
    return base + positive_bias;
  }

  // 绝对地址本身是无符号值，负偏移需要按无符号范围单独检查，避免高地址先转成 int64_t 溢出。
  const auto negative_bias = static_cast<std::uint64_t>(-(bias + 1)) + 1;
  if (base < negative_bias) {
    return std::nullopt;
  }
  return base - negative_bias;
}

std::optional<std::int64_t> apply_bias_to_relative(const ExpandedNode& node, const std::int64_t bias) {
  if (!node.relative_offset.has_value()) {
    return std::nullopt;
  }
  if ((bias > 0 && node.relative_offset.value() > std::numeric_limits<std::int64_t>::max() - bias) ||
      (bias < 0 && node.relative_offset.value() < std::numeric_limits<std::int64_t>::min() - bias)) {
    return std::nullopt;
  }
  return node.relative_offset.value() + bias;
}

std::string format_address_summary(const ExpandedNode& node, const std::int64_t bias) {
  std::ostringstream stream;
  if (const auto absolute = apply_bias_to_absolute(node, bias); absolute.has_value()) {
    stream << "0x" << std::hex << absolute.value() << std::dec;
    return stream.str();
  }
  if (const auto relative = apply_bias_to_relative(node, bias); relative.has_value()) {
    stream << relative.value();
    return stream.str();
  }
  if (node.availability == Availability::RuntimeOnly) {
    return "runtime";
  }
  return "n/a";
}

std::string format_bias_value(const std::int64_t bias) {
  std::ostringstream stream;
  stream << bias << " (0x" << std::hex << bias << std::dec << ')';
  return stream.str();
}

}  // namespace elf_static_view::ui
