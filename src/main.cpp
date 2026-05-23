#include "elf_static_view/project.hpp"
#include "logging/logger.hpp"

#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

struct CliOptions {
  std::string command;
  std::string file;
  std::string format = "text";
  bool show_runtime_only = false;
  bool only_static_known = false;
  std::optional<std::string> symbol_name;
  std::size_t expand_depth = 8;
};

void print_usage() {
  std::cout
    << "usage:\n"
    << "  elf-static-view scan <file> [--show-runtime-only]\n"
    << "  elf-static-view dump <file> [--format text|json] [--show-runtime-only]\n"
    << "                        [--only-static-known] [--symbol <name>]\n"
    << "                        [--expand-depth <n>]\n";
}

CliOptions parse_arguments(const int argc, char** argv) {
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
    } else if (arg == "--only-static-known") {
      options.only_static_known = true;
    } else if (arg == "--symbol" && index + 1 < argc) {
      options.symbol_name = argv[++index];
    } else if (arg == "--expand-depth" && index + 1 < argc) {
      options.expand_depth = static_cast<std::size_t>(std::stoul(argv[++index]));
    } else {
      throw std::runtime_error("未知参数: " + arg);
    }
  }
  return options;
}

}  // namespace

int main(const int argc, char** argv) {
  try {
    const auto options = parse_arguments(argc, argv);
    elf_static_view::ProjectLoader loader;

    if (options.command == "scan") {
      const auto model =
        loader.scan(options.file, {.include_runtime_only = options.show_runtime_only});
      std::cout << elf_static_view::render_scan_text(model);
      return 0;
    }

    if (options.command == "dump") {
      const auto model = loader.dump(options.file,
                                     {.include_runtime_only = options.show_runtime_only,
                                      .only_static_known = options.only_static_known,
                                      .symbol_name = options.symbol_name,
                                      .expand_depth = options.expand_depth});
      if (options.format == "json") {
        std::cout << elf_static_view::render_dump_json(model);
      } else {
        std::cout << elf_static_view::render_dump_text(model);
      }
      return 0;
    }

    throw std::runtime_error("未知命令: " + options.command);
  } catch (const std::exception& error) {
    elf_static_view::logging::log(elf_static_view::logging::Level::Error, error.what());
    print_usage();
    return 1;
  }
}
