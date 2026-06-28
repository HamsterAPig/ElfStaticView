#pragma once

#include "elf_static_view/project_types.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace elf_static_view {

[[nodiscard]] std::int64_t parse_address_bias(const std::string& text);
[[nodiscard]] std::optional<std::uint64_t> apply_bias_to_absolute(const ExpandedNode& node, std::int64_t bias);
[[nodiscard]] std::optional<std::int64_t> apply_bias_to_relative(const ExpandedNode& node, std::int64_t bias);
[[nodiscard]] std::string format_address_summary(const ExpandedNode& node, std::int64_t bias);
[[nodiscard]] std::string format_bias_value(std::int64_t bias);

} // namespace elf_static_view
