#include "entry.hpp"

#include "analysis/address_bias.hpp"
#include "elf_static_view/project.hpp"
#include "logging/logger.hpp"
#include "ui/application.hpp"
#include "ui/version_check.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

struct CliOptions {
  std::string command;
  std::string file;
  std::string format = "text";
  bool show_runtime_only = false;
  bool show_runtime_only_specified = false;
  bool only_static_known = false;
  bool only_static_known_specified = false;
  std::optional<std::string> symbol_name;
  std::size_t expand_depth = 8;
  bool expand_depth_specified = false;
  std::optional<std::int64_t> address_bias;
};

std::filesystem::path resolve_executable_path(char** argv) {
  std::filesystem::path executable_path = argv[0];
#if defined(_WIN32)
  wchar_t module_path[MAX_PATH] = {};
  const DWORD module_path_length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  if (module_path_length > 0 && module_path_length < MAX_PATH) {
    executable_path = std::filesystem::path(module_path);
  }
#endif
  return executable_path;
}

void print_usage() {
  std::cout
    << "用法:\n"
    << "  elf-static-view ui [file]                         打开图形界面，可选传入启动文件\n"
    << "  elf-static-view scan <file> [--show-runtime-only] 扫描变量概览\n"
    << "  elf-static-view dump <file> [--format text|json] [--show-runtime-only]\n"
    << "                        [--only-static-known] [--symbol <name>]\n"
    << "                        [--expand-depth <n>] [--address-bias <value>]\n"
    << "                        --expand-depth 0 表示不限展开层深\n";
}

CliOptions parse_cli_arguments(const int argc, char** argv) {
  if (argc < 3) {
    throw std::runtime_error("参数不足");
  }

  CliOptions options;
  options.command = argv[1];
  options.file = argv[2];

  for (int index = 3; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--format" && index + 1 < argc) {
      options.format = argv[++index];
    } else if (arg == "--show-runtime-only") {
      options.show_runtime_only = true;
      options.show_runtime_only_specified = true;
    } else if (arg == "--only-static-known") {
      options.only_static_known = true;
      options.only_static_known_specified = true;
    } else if (arg == "--symbol" && index + 1 < argc) {
      options.symbol_name = argv[++index];
    } else if (arg == "--expand-depth" && index + 1 < argc) {
      options.expand_depth = static_cast<std::size_t>(std::stoul(argv[++index]));
      options.expand_depth_specified = true;
    } else if (arg == "--address-bias" && index + 1 < argc) {
      options.address_bias = elf_static_view::parse_address_bias(argv[++index]);
    } else {
      throw std::runtime_error("未知参数: " + arg);
    }
  }

  if (options.command != "scan" && options.command != "dump") {
    throw std::runtime_error("未知命令: " + options.command);
  }
  if (options.command == "scan" && options.format != "text") {
    throw std::runtime_error("scan 命令不支持 --format");
  }
  return options;
}

int run_cli(const int argc, char** argv) {
  const CliOptions options = parse_cli_arguments(argc, argv);
  const std::filesystem::path executable_path = resolve_executable_path(argv);
  elf_static_view::ProjectLoader loader;
  elf_static_view::LoadPolicy load_policy = elf_static_view::ui::load_cli_load_policy(executable_path);

  // CLI 显式参数优先；未显式传入时沿用配置文件默认值。
  if (options.expand_depth_specified) {
    load_policy.expand_depth = options.expand_depth;
  }
  if (options.show_runtime_only_specified) {
    load_policy.exclude_runtime_only_variables = !options.show_runtime_only;
  }
  if (options.only_static_known_specified) {
    load_policy.static_storage_only = options.only_static_known;
  }

  const bool include_runtime_only =
    options.show_runtime_only_specified ? options.show_runtime_only
                                        : !load_policy.exclude_runtime_only_variables;
  const bool only_static_known =
    options.only_static_known_specified ? options.only_static_known
                                        : load_policy.static_storage_only;
  const std::size_t expand_depth =
    options.expand_depth_specified ? options.expand_depth
                                   : load_policy.expand_depth;

  if (options.command == "scan") {
    const auto model = loader.scan(options.file,
                                   {.include_runtime_only = include_runtime_only,
                                    .load_policy = load_policy});
    std::cout << elf_static_view::render_scan_text(model);
    return 0;
  }

  if (options.command == "dump") {
    const auto model = loader.dump(options.file,
                                   {.include_runtime_only = include_runtime_only,
                                    .only_static_known = only_static_known,
                                    .symbol_name = options.symbol_name,
                                    .expand_depth = expand_depth,
                                    .load_policy = load_policy});
    if (options.format == "json") {
      std::cout << elf_static_view::render_dump_json(model);
    } else if (options.address_bias.has_value()) {
      std::cout << elf_static_view::render_dump_text(model, options.address_bias.value());
    } else {
      std::cout << elf_static_view::render_dump_text(model);
    }
    return 0;
  }

  throw std::runtime_error("未知命令: " + options.command);
}

int run_ui(const int argc, char** argv) {
  std::optional<std::string> input_path;
  bool input_path_is_snapshot = false;
  if (argc >= 3) {
    input_path = argv[2];
    input_path_is_snapshot = input_path->ends_with(".json");
  }

  const std::filesystem::path executable_path = resolve_executable_path(argv);

  elf_static_view::ui::Application app(
    {.startup_file = input_path,
     .executable_path = executable_path,
     .startup_file_is_snapshot = input_path_is_snapshot});
  return app.run();
}

}  // namespace

int elf_static_view_entry(const int argc, char** argv) {
  try {
    if (argc == 1) {
      return run_ui(argc, argv);
    }
    const std::string command = argv[1];
    if (command == "ui") {
      return run_ui(argc, argv);
    }
    if (command == "scan" || command == "dump") {
      return run_cli(argc, argv);
    }
    throw std::runtime_error("未知命令: " + command);
  } catch (const std::exception& error) {
    elf_static_view::logging::log(elf_static_view::logging::Level::Error, error.what());
    print_usage();
    return 1;
  }
}
