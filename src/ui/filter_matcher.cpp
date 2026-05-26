#include "ui/filter_matcher.hpp"

#include "analysis/expander.hpp"
#include "analysis/model_utils.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

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

[[nodiscard]] bool can_single_star_consume(const std::string_view value,
                                           const std::size_t from,
                                           const std::size_t to) {
  return std::find(value.begin() + static_cast<std::ptrdiff_t>(from),
                   value.begin() + static_cast<std::ptrdiff_t>(to),
                   ':') == value.begin() + static_cast<std::ptrdiff_t>(to);
}

[[nodiscard]] bool wildcard_match_lowered(const std::string_view value, const std::string_view pattern) {
  std::size_t value_index = 0;
  std::size_t pattern_index = 0;
  std::size_t star_pattern_index = std::string_view::npos;
  std::size_t star_value_index = 0;
  bool star_crosses_scope = false;

  while (value_index < value.size()) {
    if (pattern_index < pattern.size() &&
        (pattern[pattern_index] == '?' || pattern[pattern_index] == value[value_index])) {
      ++value_index;
      ++pattern_index;
      continue;
    }

    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      star_crosses_scope = pattern_index + 1 < pattern.size() && pattern[pattern_index + 1] == '*';
      star_pattern_index = pattern_index;
      pattern_index += star_crosses_scope ? 2 : 1;
      star_value_index = value_index;
      continue;
    }

    if (star_pattern_index != std::string_view::npos &&
        (star_crosses_scope || can_single_star_consume(value, star_value_index, value_index + 1))) {
      value_index = ++star_value_index;
      pattern_index = star_pattern_index + (star_crosses_scope ? 2 : 1);
      continue;
    }

    return false;
  }

  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    pattern_index += pattern_index + 1 < pattern.size() && pattern[pattern_index + 1] == '*' ? 2 : 1;
  }
  return pattern_index == pattern.size();
}

bool matches_name_query(const FilterState& state, const ExpandedNode& node) {
  if (state.lowered_name_query.empty()) {
    return true;
  }
  const auto lowered_name = to_lower_copy(node.display_name);
  const auto lowered_path = to_lower_copy(node.path);
  return lowered_name.find(state.lowered_name_query) != std::string::npos ||
         lowered_path.find(state.lowered_name_query) != std::string::npos;
}

bool matches_path_rules(const FilterState& state, const ExpandedNode& node) {
  if (state.rules.empty()) {
    return true;
  }

  const auto lowered_path = to_lower_copy(node.path);
  bool has_include_rule = false;
  bool included = false;
  for (const auto& rule : state.rules) {
    if (!rule.exclude) {
      has_include_rule = true;
      if (wildcard_match_lowered(lowered_path, rule.lowered_pattern)) {
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
    if (rule.exclude && wildcard_match_lowered(lowered_path, rule.lowered_pattern)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool filter_rule_set_equal(const FilterRuleSet& left, const FilterRuleSet& right) {
  return left.variable_name_query == right.variable_name_query &&
         left.path_rules_text == right.path_rules_text &&
         left.include_runtime_only == right.include_runtime_only &&
         left.only_static_known == right.only_static_known;
}

void remember_visible_path_and_ancestors(FilterCache& cache, const std::string& path) {
  cache.visible_paths.insert(path);
  for (std::size_t index = path.size(); index > 0;) {
    const std::size_t dot_index = path.rfind('.', index - 1);
    const std::size_t bracket_index = path.rfind('[', index - 1);
    const std::size_t scope_index = path.rfind("::", index - 1);
    std::size_t cut = std::string::npos;
    if (dot_index != std::string::npos) {
      cut = dot_index;
    }
    if (bracket_index != std::string::npos && (cut == std::string::npos || bracket_index > cut)) {
      cut = bracket_index;
    }
    if (scope_index != std::string::npos && (cut == std::string::npos || scope_index > cut)) {
      cut = scope_index;
    }
    if (cut == std::string::npos) {
      break;
    }
    cache.visible_paths.insert(path.substr(0, cut));
    index = cut;
  }
}

void collect_visible_paths(AppState& state,
                           const ExpandedNode& node,
                           const analysis::Expander* expander) {
  if (matches_filters(state, node)) {
    remember_visible_path_and_ancestors(state.filters.cache, node.path);
  }

  for (const auto& child : node.children) {
    collect_visible_paths(state, child, expander);
  }

  if (node.children_lazy && expander != nullptr) {
    // 筛选缓存重建时集中展开懒加载子节点，避免渲染阶段每帧递归扫描和重复展开。
    const auto lazy_children = expander->expand_children(node);
    for (const auto& child : lazy_children) {
      collect_visible_paths(state, child, expander);
    }
  }
}

}  // namespace

void compile_filter_rules(FilterState& state) {
  state.rules.clear();
  state.compile_error.reset();
  state.lowered_name_query = to_lower_copy(trim(state.form.variable_name_query));
  state.cache.valid = false;
  state.cache.model = nullptr;
  state.cache.visible_paths.clear();

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
    rule.lowered_pattern = to_lower_copy(rule.normalized_pattern);
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

void rebuild_filter_cache(AppState& state) {
  auto& cache = state.filters.cache;
  const ProjectModel* model = state.project_model.has_value() ? &state.project_model.value() : nullptr;
  if (cache.valid && cache.model == model &&
      cache.expand_depth == state.load_policy.expand_depth &&
      cache.lazy_expand_children == state.load_policy.lazy_expand_children &&
      filter_rule_set_equal(cache.signature, state.filters.form)) {
    return;
  }

  cache.signature = state.filters.form;
  cache.model = model;
  cache.expand_depth = state.load_policy.expand_depth;
  cache.lazy_expand_children = state.load_policy.lazy_expand_children;
  cache.visible_paths.clear();
  cache.valid = true;
  ++cache.rebuild_count;

  if (model == nullptr || state.filters.compile_error.has_value()) {
    return;
  }

  analysis::Expander expander(model->types,
                              state.load_policy.expand_depth,
                              state.load_policy.lazy_expand_children);
  for (const auto& node : model->expanded) {
    collect_visible_paths(state, node, &expander);
  }
}

bool is_filter_cache_visible(const AppState& state, const ExpandedNode& node) {
  if (!state.filters.cache.valid || state.filters.compile_error.has_value()) {
    return false;
  }
  return state.filters.cache.visible_paths.find(node.path) != state.filters.cache.visible_paths.end();
}

}  // namespace elf_static_view::ui
