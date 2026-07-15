#include "elf/dwarf_expression.hpp"

#include <limits>

namespace elf_static_view::elf {

namespace {

    class ExpressionCursor {
    public:
        ExpressionCursor(const std::uint8_t* data, const std::size_t size, const DwarfByteOrder byte_order)
            : data_(data), size_(size), byte_order_(byte_order)
        {
        }

        [[nodiscard]] bool empty() const noexcept { return offset_ == size_; }

        [[nodiscard]] std::optional<std::uint8_t> read_u8()
        {
            if (offset_ >= size_) {
                return std::nullopt;
            }
            return data_[offset_++];
        }

        [[nodiscard]] std::optional<std::uint64_t> read_unsigned(const std::size_t width)
        {
            if (width == 0 || width > sizeof(std::uint64_t) || width > size_ - offset_) {
                return std::nullopt;
            }
            std::uint64_t value = 0;
            for (std::size_t index = 0; index < width; ++index) {
                const auto source_index = byte_order_ == DwarfByteOrder::LittleEndian ? index : width - index - 1;
                value |= static_cast<std::uint64_t>(data_[offset_ + source_index]) << (index * 8U);
            }
            offset_ += width;
            return value;
        }

        [[nodiscard]] std::optional<std::uint64_t> read_uleb128()
        {
            std::uint64_t value = 0;
            unsigned int shift = 0;
            for (unsigned int index = 0; index < 10; ++index) {
                const auto byte = read_u8();
                if (!byte.has_value()) {
                    return std::nullopt;
                }
                const auto payload = static_cast<std::uint64_t>(byte.value() & 0x7fU);
                if (shift == 63U && payload > 1U) {
                    return std::nullopt;
                }
                value |= payload << shift;
                if ((byte.value() & 0x80U) == 0) {
                    return value;
                }
                shift += 7U;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::int64_t> read_sleb128()
        {
            std::uint64_t value = 0;
            unsigned int shift = 0;
            std::uint8_t byte = 0;
            for (unsigned int index = 0; index < 10; ++index) {
                const auto next = read_u8();
                if (!next.has_value()) {
                    return std::nullopt;
                }
                byte = next.value();
                const auto payload = static_cast<std::uint64_t>(byte & 0x7fU);
                if (shift == 63U && payload != 0U && payload != 0x7fU) {
                    return std::nullopt;
                }
                value |= payload << shift;
                shift += 7U;
                if ((byte & 0x80U) == 0) {
                    if (shift < 64U && (byte & 0x40U) != 0) {
                        value |= (~std::uint64_t{0}) << shift;
                    }
                    return static_cast<std::int64_t>(value);
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] bool skip(const std::size_t count)
        {
            if (count > size_ - offset_) {
                return false;
            }
            offset_ += count;
            return true;
        }

    private:
        const std::uint8_t* data_ = nullptr;
        std::size_t size_ = 0;
        std::size_t offset_ = 0;
        DwarfByteOrder byte_order_ = DwarfByteOrder::LittleEndian;
    };

    [[nodiscard]] bool read_unsigned_operand(ExpressionCursor& cursor, LocationOp& operation, const std::size_t width)
    {
        const auto value = cursor.read_unsigned(width);
        if (!value.has_value()) {
            return false;
        }
        operation.operand1 = value.value();
        return true;
    }

    [[nodiscard]] bool read_signed_operand(ExpressionCursor& cursor, LocationOp& operation, const std::size_t width)
    {
        const auto value = cursor.read_unsigned(width);
        if (!value.has_value()) {
            return false;
        }
        std::uint64_t extended = value.value();
        if (width < sizeof(std::uint64_t) && (extended & (std::uint64_t{1} << (width * 8U - 1U))) != 0) {
            extended |= (~std::uint64_t{0}) << (width * 8U);
        }
        operation.operand1 = extended;
        return true;
    }

    [[nodiscard]] bool read_uleb_operand(ExpressionCursor& cursor, Dwarf_Unsigned& operand)
    {
        const auto value = cursor.read_uleb128();
        if (!value.has_value()) {
            return false;
        }
        operand = value.value();
        return true;
    }

    [[nodiscard]] bool read_sleb_operand(ExpressionCursor& cursor, Dwarf_Unsigned& operand)
    {
        const auto value = cursor.read_sleb128();
        if (!value.has_value()) {
            return false;
        }
        operand = static_cast<Dwarf_Unsigned>(value.value());
        return true;
    }

    [[nodiscard]] bool checked_add(const std::int64_t left, const std::int64_t right, std::int64_t& result)
    {
        if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
            (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
            return false;
        }
        result = left + right;
        return true;
    }

    [[nodiscard]] bool checked_subtract(const std::int64_t left, const std::int64_t right, std::int64_t& result)
    {
        if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) ||
            (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right)) {
            return false;
        }
        result = left - right;
        return true;
    }

    struct SymbolicValue {
        bool contains_object_base = false;
        std::int64_t constant = 0;
    };

} // namespace

DecodedDwarfExpression decode_dwarf_expression(const std::uint8_t* data,
                                               const std::size_t length,
                                               const Dwarf_Half address_size,
                                               const Dwarf_Half offset_size,
                                               const DwarfByteOrder byte_order)
{
    DecodedDwarfExpression result;
    if (data == nullptr && length != 0) {
        return result;
    }

    ExpressionCursor cursor(data, length, byte_order);
    while (!cursor.empty()) {
        const auto opcode_value = cursor.read_u8();
        if (!opcode_value.has_value()) {
            return result;
        }

        LocationOp operation;
        operation.atom = opcode_value.value();
        const auto opcode = opcode_value.value();
        bool valid = true;

        if (opcode >= DW_OP_lit0 && opcode <= DW_OP_lit31) {
            operation.operand1 = opcode - DW_OP_lit0;
        } else if (opcode >= DW_OP_reg0 && opcode <= DW_OP_reg31) {
            operation.operand1 = opcode - DW_OP_reg0;
        } else if (opcode >= DW_OP_breg0 && opcode <= DW_OP_breg31) {
            operation.operand1 = opcode - DW_OP_breg0;
            valid = read_sleb_operand(cursor, operation.operand2);
        } else {
            switch (opcode) {
                case DW_OP_addr:
                    valid = read_unsigned_operand(cursor, operation, address_size);
                    break;
                case DW_OP_const1u:
                    valid = read_unsigned_operand(cursor, operation, 1);
                    break;
                case DW_OP_const1s:
                    valid = read_signed_operand(cursor, operation, 1);
                    break;
                case DW_OP_const2u:
                    valid = read_unsigned_operand(cursor, operation, 2);
                    break;
                case DW_OP_const2s:
                    valid = read_signed_operand(cursor, operation, 2);
                    break;
                case DW_OP_const4u:
                    valid = read_unsigned_operand(cursor, operation, 4);
                    break;
                case DW_OP_const4s:
                    valid = read_signed_operand(cursor, operation, 4);
                    break;
                case DW_OP_const8u:
                    valid = read_unsigned_operand(cursor, operation, 8);
                    break;
                case DW_OP_const8s:
                    valid = read_signed_operand(cursor, operation, 8);
                    break;
                case DW_OP_constu:
                case DW_OP_plus_uconst:
                case DW_OP_regx:
                case DW_OP_piece:
                case DW_OP_addrx:
                case DW_OP_constx:
                case DW_OP_GNU_addr_index:
                    valid = read_uleb_operand(cursor, operation.operand1);
                    break;
                case DW_OP_consts:
                case DW_OP_fbreg:
                    valid = read_sleb_operand(cursor, operation.operand1);
                    break;
                case DW_OP_bregx:
                    valid =
                        read_uleb_operand(cursor, operation.operand1) && read_sleb_operand(cursor, operation.operand2);
                    break;
                case DW_OP_bit_piece:
                    valid =
                        read_uleb_operand(cursor, operation.operand1) && read_uleb_operand(cursor, operation.operand2);
                    break;
                case DW_OP_deref_size:
                case DW_OP_xderef_size:
                case DW_OP_pick:
                    valid = read_unsigned_operand(cursor, operation, 1);
                    break;
                case DW_OP_bra:
                case DW_OP_skip:
                case DW_OP_call2:
                    valid = read_signed_operand(cursor, operation, 2);
                    break;
                case DW_OP_call4:
                    valid = read_unsigned_operand(cursor, operation, 4);
                    break;
                case DW_OP_call_ref:
                    valid = read_unsigned_operand(cursor, operation, offset_size);
                    break;
                case DW_OP_implicit_value:
                case DW_OP_entry_value: {
                    const auto block_length = cursor.read_uleb128();
                    valid = block_length.has_value() &&
                            block_length.value() <= std::numeric_limits<std::size_t>::max() &&
                            cursor.skip(static_cast<std::size_t>(block_length.value()));
                    if (block_length.has_value()) {
                        operation.operand1 = block_length.value();
                    }
                    break;
                }
                case DW_OP_deref:
                case DW_OP_dup:
                case DW_OP_drop:
                case DW_OP_over:
                case DW_OP_swap:
                case DW_OP_rot:
                case DW_OP_xderef:
                case DW_OP_abs:
                case DW_OP_and:
                case DW_OP_div:
                case DW_OP_minus:
                case DW_OP_mod:
                case DW_OP_mul:
                case DW_OP_neg:
                case DW_OP_not:
                case DW_OP_or:
                case DW_OP_plus:
                case DW_OP_shl:
                case DW_OP_shr:
                case DW_OP_shra:
                case DW_OP_xor:
                case DW_OP_eq:
                case DW_OP_ge:
                case DW_OP_gt:
                case DW_OP_le:
                case DW_OP_lt:
                case DW_OP_ne:
                case DW_OP_nop:
                case DW_OP_push_object_address:
                case DW_OP_call_frame_cfa:
                case DW_OP_form_tls_address:
                case DW_OP_stack_value:
                    break;
                default:
                    valid = false;
                    break;
            }
        }

        if (!valid) {
            return result;
        }
        result.operations.push_back(operation);
    }

    result.complete = true;
    return result;
}

std::optional<std::int64_t> evaluate_data_member_location(const DecodedDwarfExpression& expression)
{
    if (!expression.complete) {
        return std::nullopt;
    }

    std::vector<SymbolicValue> stack{{.contains_object_base = true, .constant = 0}};
    for (const auto& operation : expression.operations) {
        if (operation.atom >= DW_OP_lit0 && operation.atom <= DW_OP_lit31) {
            stack.push_back({.constant = static_cast<std::int64_t>(operation.atom - DW_OP_lit0)});
            continue;
        }

        switch (operation.atom) {
            case DW_OP_const1u:
            case DW_OP_const2u:
            case DW_OP_const4u:
            case DW_OP_const8u:
            case DW_OP_constu:
                if (operation.operand1 > static_cast<Dwarf_Unsigned>(std::numeric_limits<std::int64_t>::max())) {
                    return std::nullopt;
                }
                stack.push_back({.constant = static_cast<std::int64_t>(operation.operand1)});
                break;
            case DW_OP_const1s:
            case DW_OP_const2s:
            case DW_OP_const4s:
            case DW_OP_const8s:
            case DW_OP_consts:
                stack.push_back({.constant = static_cast<std::int64_t>(operation.operand1)});
                break;
            case DW_OP_plus_uconst: {
                if (stack.empty() ||
                    operation.operand1 > static_cast<Dwarf_Unsigned>(std::numeric_limits<std::int64_t>::max())) {
                    return std::nullopt;
                }
                std::int64_t value = 0;
                if (!checked_add(stack.back().constant, static_cast<std::int64_t>(operation.operand1), value)) {
                    return std::nullopt;
                }
                stack.back().constant = value;
                break;
            }
            case DW_OP_plus:
            case DW_OP_minus: {
                if (stack.size() < 2) {
                    return std::nullopt;
                }
                const auto right = stack.back();
                stack.pop_back();
                const auto left = stack.back();
                stack.pop_back();

                SymbolicValue value;
                if (operation.atom == DW_OP_plus) {
                    if (left.contains_object_base && right.contains_object_base) {
                        return std::nullopt;
                    }
                    value.contains_object_base = left.contains_object_base || right.contains_object_base;
                    if (!checked_add(left.constant, right.constant, value.constant)) {
                        return std::nullopt;
                    }
                } else {
                    if (!left.contains_object_base && right.contains_object_base) {
                        return std::nullopt;
                    }
                    value.contains_object_base = left.contains_object_base != right.contains_object_base;
                    if (!checked_subtract(left.constant, right.constant, value.constant)) {
                        return std::nullopt;
                    }
                }
                stack.push_back(value);
                break;
            }
            case DW_OP_nop:
                break;
            default:
                return std::nullopt;
        }
    }

    if (stack.size() != 1 || !stack.front().contains_object_base) {
        return std::nullopt;
    }
    return stack.front().constant;
}

} // namespace elf_static_view::elf
