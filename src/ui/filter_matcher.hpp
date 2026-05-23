#pragma once

#include "ui/app_state.hpp"

#include <optional>
#include <string>

namespace elf_static_view::ui {

void compile_filter_rules(FilterState& state);
bool matches_filters(const AppState& state, const ExpandedNode& node);

}  // namespace elf_static_view::ui
