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
  void load_file_into_state(const std::string& path);
  void refresh_window_title();
  void queue_ui_scale(float x_scale, float y_scale);
  void apply_pending_ui_scale();
  void render_frame();

  static void glfw_drop_callback(GLFWwindow* window, int count, const char** paths);
  static void glfw_content_scale_callback(GLFWwindow* window, float x_scale, float y_scale);

  UiLaunchOptions options_;
  GLFWwindow* window_ = nullptr;
  AppState state_;
  float ui_scale_ = 1.0F;
  float pending_ui_scale_ = 1.0F;
  bool ui_scale_dirty_ = false;
};

}  // namespace elf_static_view::ui
