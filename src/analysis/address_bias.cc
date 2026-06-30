#include "analysis/address_bias.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace elf_static_view {

namespace {

    std::string trim_copy(const std::string_view value)
    {
        std::size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
            ++start;
        }

        std::size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
            --end;
        }
        return std::string(value.substr(start, end - start));
    }

} // namespace

std::int64_t parse_address_bias(const std::string& text)
{
    const std::string trimmed = trim_copy(text);
    if (trimmed.empty()) {
        throw std::runtime_error("地址偏移不能为空");
    }

    bool negative = false;
    std::size_t cursor = 0;
    if (trimmed[cursor] == '+' || trimmed[cursor] == '-') {
        negative = trimmed[cursor] == '-';
        ++cursor;
    }
    if (cursor >= trimmed.size()) {
        throw std::runtime_error("地址偏移缺少数字部分");
    }

    int base = 10;
    if (trimmed.compare(cursor, 2, "0x") == 0 || trimmed.compare(cursor, 2, "0X") == 0) {
        base = 16;
        cursor += 2;
    }
    if (cursor >= trimmed.size()) {
        throw std::runtime_error("地址偏移缺少数字部分");
    }

    std::uint64_t magnitude = 0;
    const char* begin = trimmed.data() + static_cast<std::ptrdiff_t>(cursor);
    const char* end = trimmed.data() + static_cast<std::ptrdiff_t>(trimmed.size());
    const auto result = std::from_chars(begin, end, magnitude, base);
    if (result.ec == std::errc::invalid_argument || result.ptr != end) {
        throw std::runtime_error("地址偏移包含非法字符");
    }
    if (result.ec == std::errc::result_out_of_range) {
        throw std::runtime_error("地址偏移超出 int64 范围");
    }

    constexpr auto kInt64Max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    constexpr auto kInt64MinMagnitude = kInt64Max + 1U;
    if (!negative) {
        if (magnitude > kInt64Max) {
            throw std::runtime_error("地址偏移超出 int64 范围");
        }
        return static_cast<std::int64_t>(magnitude);
    }

    // 负数需要单独保留 INT64_MIN 这一格，避免中间转换时溢出。
    if (magnitude > kInt64MinMagnitude) {
        throw std::runtime_error("地址偏移超出 int64 范围");
    }
    if (magnitude == kInt64MinMagnitude) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
}

std::optional<std::uint64_t> apply_bias_to_absolute(const ExpandedNode& node, const std::int64_t bias)
{
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

    const auto negative_bias = static_cast<std::uint64_t>(-(bias + 1)) + 1U;
    if (base < negative_bias) {
        return std::nullopt;
    }
    return base - negative_bias;
}

std::optional<std::int64_t> apply_bias_to_relative(const ExpandedNode& node, const std::int64_t bias)
{
    if (!node.relative_offset.has_value()) {
        return std::nullopt;
    }
    if ((bias > 0 && node.relative_offset.value() > std::numeric_limits<std::int64_t>::max() - bias) ||
        (bias < 0 && node.relative_offset.value() < std::numeric_limits<std::int64_t>::min() - bias)) {
        return std::nullopt;
    }
    return node.relative_offset.value() + bias;
}

std::string format_address_summary(const ExpandedNode& node, const std::int64_t bias)
{
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

std::string format_bias_value(const std::int64_t bias)
{
    std::ostringstream stream;
    stream << bias << " (0x" << std::hex << bias << std::dec << ')';
    return stream.str();
}

} // namespace elf_static_view
