#pragma once

#include "ui/app_state.hpp"

#include <optional>
#include <string>

namespace elf_static_view::ui {

void compile_filter_rules(FilterState& state);
bool matches_filters(const AppState& state, const ExpandedNode& node);
void rebuild_filter_cache(AppState& state);
bool is_filter_cache_visible(const AppState& state, const ExpandedNode& node);
[[nodiscard]] FilterBuildResult build_filter_cache(const ProjectModel* model,
                                                   const FilterRuleSet& rules,
                                                   std::size_t expand_depth,
                                                   bool lazy_expand_children);
void apply_filter_build_result(AppState& state, FilterBuildResult result);
[[nodiscard]] bool receive_filter_build_result(AppState& state, std::uint64_t task_id, FilterBuildResult result);
void mark_filter_text_changed(AppState& state, std::chrono::steady_clock::time_point now);
void mark_filter_options_changed(AppState& state, std::chrono::steady_clock::time_point now);
[[nodiscard]] bool filter_debounce_elapsed(const AppState& state, std::chrono::steady_clock::time_point now);
[[nodiscard]] bool should_show_filter_progress(const AppState& state);

} // namespace elf_static_view::ui
