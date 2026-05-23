#pragma once

#include "ui/app_state.hpp"

#include <optional>
#include <string>

namespace elf_static_view::ui {

void compile_filter_rules(FilterState& state);
bool matches_filters(const AppState& state, const ExpandedNode& node);
std::optional<std::uint64_t> apply_bias_to_absolute(const ExpandedNode& node, std::int64_t bias);
std::optional<std::int64_t> apply_bias_to_relative(const ExpandedNode& node, std::int64_t bias);
std::string format_address_summary(const ExpandedNode& node, std::int64_t bias);
std::string format_bias_value(std::int64_t bias);

}  // namespace elf_static_view::ui
