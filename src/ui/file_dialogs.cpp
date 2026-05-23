#include "ui/file_dialogs.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#endif

#include <array>

namespace elf_static_view::ui {

namespace {

#if defined(_WIN32)
std::optional<std::string> run_dialog(const DWORD flags,
                                      const std::string& title,
                                      const char* filter,
                                      const char* default_extension,
                                      const bool save_dialog) {
  std::array<char, MAX_PATH> buffer{};
  OPENFILENAMEA dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrFilter = filter;
  dialog.lpstrTitle = title.c_str();
  dialog.Flags = flags;
  dialog.lpstrDefExt = default_extension;

  const BOOL result = save_dialog ? GetSaveFileNameA(&dialog) : GetOpenFileNameA(&dialog);
  if (result == FALSE) {
    return std::nullopt;
  }
  return std::string(dialog.lpstrFile);
}
#endif

}  // namespace

std::optional<std::string> open_elf_file_dialog() {
#if defined(_WIN32)
  return run_dialog(OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
                    "Open ELF or Binary",
                    "ELF and Binary\0*.elf;*.o;*.so;*.out;*.exe\0All Files\0*.*\0",
                    "elf",
                    false);
#else
  return std::nullopt;
#endif
}

std::optional<std::string> open_snapshot_file_dialog() {
#if defined(_WIN32)
  return run_dialog(OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
                    "Import JSON Snapshot",
                    "JSON Files\0*.json\0All Files\0*.*\0",
                    "json",
                    false);
#else
  return std::nullopt;
#endif
}

std::optional<std::string> save_snapshot_file_dialog(const std::string& suggested_name) {
#if defined(_WIN32)
  auto path = run_dialog(OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
                         "Export JSON Snapshot",
                         "JSON Files\0*.json\0All Files\0*.*\0",
                         "json",
                         true);
  if (path.has_value()) {
    return path;
  }
  if (!suggested_name.empty()) {
    return std::nullopt;
  }
  return std::nullopt;
#else
  static_cast<void>(suggested_name);
  return std::nullopt;
#endif
}

}  // namespace elf_static_view::ui
