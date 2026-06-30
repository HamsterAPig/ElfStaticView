#if defined(_WIN32)

#include "entry.hpp"
#include "platform/utf8.hpp"
#include "ui/startup_diagnostics.hpp"

#include <string>
#include <sstream>
#include <vector>

#include <shellapi.h>
#include <windows.h>

namespace {

int handle_seh_guard_failure(const DWORD code)
{
    std::ostringstream stream;
    stream << "程序启动时触发 Windows 结构化异常，异常码: 0x" << std::hex
           << static_cast<unsigned long long>(code);
    const std::string message = stream.str();
    elf_static_view::ui::append_startup_log(message);
    elf_static_view::ui::show_startup_error_dialog("ElfStaticView 启动失败", message);
    return 1;
}

int run_entry_with_seh_guard(const int argc, char** argv)
{
#if defined(_MSC_VER)
    __try {
        return elf_static_view_entry(argc, argv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return handle_seh_guard_failure(GetExceptionCode());
    }
#else
    return elf_static_view_entry(argc, argv);
#endif
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    int argc = 0;
    LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wide_argv == nullptr || argc <= 0) {
        return run_entry_with_seh_guard(0, nullptr);
    }

    std::vector<std::string> utf8_arguments;
    utf8_arguments.reserve(static_cast<std::size_t>(argc));
    std::vector<char*> argv;
    argv.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        utf8_arguments.push_back(elf_static_view::platform::wide_to_utf8(wide_argv[index]));
    }
    for (auto& argument : utf8_arguments) {
        argv.push_back(argument.data());
    }
    LocalFree(wide_argv);
    return run_entry_with_seh_guard(argc, argv.data());
}

#endif
