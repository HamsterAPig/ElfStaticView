#include "ui/file_dialogs.hpp"

#include "platform/utf8.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#endif

#include <array>

namespace elf_static_view::ui {

namespace {

#if defined(_WIN32)
std::optional<std::string> run_dialog(const DWORD flags,
                                      const wchar_t* title,
                                      const wchar_t* filter,
                                      const wchar_t* default_extension,
                                      const bool save_dialog) {
  std::array<wchar_t, 4096> buffer{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrFilter = filter;
  dialog.lpstrTitle = title;
  dialog.Flags = flags;
  dialog.lpstrDefExt = default_extension;

  const BOOL result = save_dialog ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
  if (result == FALSE) {
    return std::nullopt;
  }
  return platform::wide_to_utf8(dialog.lpstrFile);
}
#endif

}  // namespace

std::optional<std::string> open_elf_file_dialog() {
#if defined(_WIN32)
  return run_dialog(OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
                    L"打开 ELF 或二进制文件",
                    L"ELF 与二进制文件\0*.elf;*.o;*.so;*.out;*.exe\0所有文件\0*.*\0",
                    L"elf",
                    false);
#else
  return std::nullopt;
#endif
}

std::optional<std::string> open_snapshot_file_dialog() {
#if defined(_WIN32)
  return run_dialog(OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
                    L"导入 JSON 快照",
                    L"JSON 文件\0*.json\0所有文件\0*.*\0",
                    L"json",
                    false);
#else
  return std::nullopt;
#endif
}

std::optional<std::string> save_snapshot_file_dialog(const std::string& suggested_name) {
#if defined(_WIN32)
  auto path = run_dialog(OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
                         L"导出 JSON 快照",
                         L"JSON 文件\0*.json\0所有文件\0*.*\0",
                         L"json",
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

std::optional<std::string> save_raw_dwarf_file_dialog(const std::string& suggested_name) {
#if defined(_WIN32)
  auto path = run_dialog(OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
                         L"导出原始 DWARF JSON",
                         L"JSON 文件\0*.json\0所有文件\0*.*\0",
                         L"json",
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
