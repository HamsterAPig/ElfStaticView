#if defined(_WIN32)

#include "entry.hpp"
#include "platform/utf8.hpp"

#include <shellapi.h>
#include <windows.h>

#include <string>
#include <vector>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  int argc = 0;
  LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (wide_argv == nullptr || argc <= 0) {
    return elf_static_view_entry(0, nullptr);
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
  return elf_static_view_entry(argc, argv.data());
}

#endif
