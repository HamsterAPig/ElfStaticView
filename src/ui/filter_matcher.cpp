#include "ui/filter_matcher.hpp"

#include "analysis/model_utils.hpp"

#include <algorithm>
#include <cctype>
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
  return wildcard_match_impl(to_lower_copy(value), to_lower_copy(pattern), 0, 0);
}

bool matches_name_query(const FilterState& state, const ExpandedNode& node) {
  if (state.form.variable_name_query.empty()) {
    return true;
  }
  const auto lowered_query = to_lower_copy(state.form.variable_name_query);
  const auto lowered_name = to_lower_copy(node.display_name);
  const auto lowered_path = to_lower_copy(node.path);
  return lowered_name.find(lowered_query) != std::string::npos ||
         lowered_path.find(lowered_query) != std::string::npos;
}

bool matches_path_rules(const FilterState& state, const ExpandedNode& node) {
  if (state.rules.empty()) {
    return true;
  }

  bool has_include_rule = false;
  bool included = false;
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
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed.starts_with('#')) {
      continue;
    }

    FilterRule rule;
    rule.exclude = trimmed.starts_with('!');
    rule.raw_pattern = trimmed;
    rule.normalized_pattern = rule.exclude ? trim(trimmed.substr(1)) : trimmed;
    if (rule.normalized_pattern.empty()) {
      state.compile_error = "路径规则中的排除模式不能为空";
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
      (node.availability == Availability::RuntimeOnly || node.availability == Availability::OptimizedOut)) {
    return false;
  }
  return matches_name_query(state.filters, node) && matches_path_rules(state.filters, node);
}

}  // namespace elf_static_view::ui
