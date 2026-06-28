#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(_WIN32)
// Prevent Windows headers from defining min/max macros in shared headers.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace elf_static_view::platform {

#if defined(_WIN32)

inline std::wstring utf8_to_wide(std::string_view text)
{
    if (text.empty()) {
        return {};
    }

    const int wide_length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide_length <= 0) {
        throw std::runtime_error("UTF-8 到宽字符转换失败");
    }

    std::wstring result(static_cast<std::size_t>(wide_length), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), wide_length);
    if (converted != wide_length) {
        throw std::runtime_error("UTF-8 到宽字符转换失败");
    }
    return result;
}

inline std::string wide_to_utf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }

    const int utf8_length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0) {
        throw std::runtime_error("宽字符到 UTF-8 转换失败");
    }

    std::string result(static_cast<std::size_t>(utf8_length), '\0');
    const int converted = WideCharToMultiByte(CP_UTF8,
                                              WC_ERR_INVALID_CHARS,
                                              text.data(),
                                              static_cast<int>(text.size()),
                                              result.data(),
                                              utf8_length,
                                              nullptr,
                                              nullptr);
    if (converted != utf8_length) {
        throw std::runtime_error("宽字符到 UTF-8 转换失败");
    }
    return result;
}

inline std::filesystem::path utf8_path(std::string_view path)
{
    return std::filesystem::path(utf8_to_wide(path));
}

inline std::string path_to_utf8(const std::filesystem::path& path)
{
    return wide_to_utf8(path.native());
}

#else

inline std::filesystem::path utf8_path(std::string_view path)
{
    return std::filesystem::path(path);
}

inline std::string path_to_utf8(const std::filesystem::path& path)
{
    return path.string();
}

#endif

} // namespace elf_static_view::platform
