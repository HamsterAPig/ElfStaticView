#pragma once

#include "elf/dwarf_wrappers.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace elf_static_view::elf {

struct DecodedDwarfExpression {
    std::vector<LocationOp> operations;
    bool complete = false;
};

[[nodiscard]] DecodedDwarfExpression decode_dwarf_expression(const std::uint8_t* data,
                                                             std::size_t length,
                                                             Dwarf_Half address_size,
                                                             Dwarf_Half offset_size,
                                                             DwarfByteOrder byte_order);

[[nodiscard]] std::optional<std::int64_t> evaluate_data_member_location(const DecodedDwarfExpression& expression);

} // namespace elf_static_view::elf
