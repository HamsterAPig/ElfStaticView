#if defined(_WIN32)

#include "entry.hpp"
#include "platform/utf8.hpp"
#include "ui/startup_diagnostics.hpp"

#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sstream>
#include <vector>

#include <io.h>
#include <shellapi.h>
#include <windows.h>

namespace {

bool should_attach_parent_console(const std::vector<std::string>& arguments)
{
    if (arguments.size() <= 1) {
        return false;
    }
    return arguments[1] != "ui";
}

bool bind_standard_handle_to_crt(const DWORD handle_id, const int crt_fd)
{
    const HANDLE raw_handle = GetStdHandle(handle_id);
    if (raw_handle == nullptr || raw_handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    HANDLE duplicated_handle = nullptr;
    const HANDLE current_process = GetCurrentProcess();
    if (DuplicateHandle(current_process,
                        raw_handle,
                        current_process,
                        &duplicated_handle,
                        0,
                        TRUE,
                        DUPLICATE_SAME_ACCESS) == FALSE) {
        return false;
    }

    const int os_fd = _open_osfhandle(reinterpret_cast<intptr_t>(duplicated_handle), _O_TEXT);
    if (os_fd == -1) {
        CloseHandle(duplicated_handle);
        return false;
    }

    const bool ok = _dup2(os_fd, crt_fd) == 0;
    _close(os_fd);
    return ok;
}

bool bind_inherited_standard_streams()
{
    const bool stdout_bound = bind_standard_handle_to_crt(STD_OUTPUT_HANDLE, _fileno(stdout));
    const bool stderr_bound = bind_standard_handle_to_crt(STD_ERROR_HANDLE, _fileno(stderr));
    const bool stdin_bound = bind_standard_handle_to_crt(STD_INPUT_HANDLE, _fileno(stdin));
    return stdout_bound || stderr_bound || stdin_bound;
}

void attach_parent_console_for_cli_mode()
{
    if (bind_inherited_standard_streams()) {
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) {
        return;
    }

    // GUI 子系统进程默认没有控制台流；命令行模式下显式接回父控制台。
    FILE* ignored = nullptr;
    freopen_s(&ignored, "CONOUT$", "w", stdout);
    freopen_s(&ignored, "CONOUT$", "w", stderr);
    freopen_s(&ignored, "CONIN$", "r", stdin);
}

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
    if (should_attach_parent_console(utf8_arguments)) {
        attach_parent_console_for_cli_mode();
    }
    for (auto& argument : utf8_arguments) {
        argv.push_back(argument.data());
    }
    LocalFree(wide_argv);
    return run_entry_with_seh_guard(argc, argv.data());
}

#endif
