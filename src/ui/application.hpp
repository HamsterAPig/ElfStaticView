#pragma once

#include "ui/app_state.hpp"

#include <filesystem>
#include <optional>
#include <string>

struct GLFWwindow;

namespace elf_static_view::ui {

struct UiLaunchOptions {
  std::optional<std::string> startup_file;
  std::filesystem::path executable_path;
  bool startup_file_is_snapshot = false;
};

class Application {
public:
  explicit Application(UiLaunchOptions options);
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  int run();

private:
  bool initialize();
  void shutdown();
  void load_startup_content();
  void render_frame();

  UiLaunchOptions options_;
  GLFWwindow* window_ = nullptr;
  AppState state_;
};

}  // namespace elf_static_view::ui
