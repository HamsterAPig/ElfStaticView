#include "ui/application.hpp"

#include "elf_static_view/project.hpp"
#include "logging/logger.hpp"
#include "platform/utf8.hpp"
#include "ui/file_dialogs.hpp"
#include "ui/filter_matcher.hpp"
#include "ui/main_window.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace elf_static_view::ui {

namespace {

std::string read_all_text(const std::string& path) {
  std::ifstream input(platform::utf8_path(path), std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("无法打开文件: " + path);
  }
  std::ostringstream stream;
  stream << input.rdbuf();
  return stream.str();
}

void configure_ui_font(ImGuiIO& io) {
  io.Fonts->Clear();
#if defined(_WIN32)
  wchar_t windows_directory[MAX_PATH] = {};
  const UINT length = GetWindowsDirectoryW(windows_directory, MAX_PATH);
  if (length > 0) {
    // Windows 默认字体不带中文字形，这里优先挑系统里常见的简体中文字体。
    const std::filesystem::path fonts_directory =
      std::filesystem::path(windows_directory) / "Fonts";
    constexpr const wchar_t* kCandidateFonts[] = {
      L"msyh.ttc",
      L"msyhbd.ttc",
      L"simhei.ttf",
      L"simsun.ttc",
    };

    ImFontConfig font_config {};
    font_config.OversampleH = 2;
    font_config.PixelSnapH = true;

    for (const wchar_t* font_name : kCandidateFonts) {
      const auto font_path = fonts_directory / font_name;
      if (!std::filesystem::exists(font_path)) {
        continue;
      }

      const std::string utf8_font_path = platform::path_to_utf8(font_path);
      if (ImFont* font = io.Fonts->AddFontFromFileTTF(
            utf8_font_path.c_str(),
            18.0F,
            &font_config,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) {
        io.FontDefault = font;
        return;
      }
    }
  }
#endif
  io.FontDefault = io.Fonts->AddFontDefault();
}

void glfw_drop_callback(GLFWwindow* window, const int count, const char** paths) {
  auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
  if (state == nullptr || count <= 0 || paths == nullptr) {
    return;
  }

  try {
    const std::string path = paths[0];
    if (path.ends_with(".json")) {
      const auto snapshot = parse_snapshot_json(read_all_text(path));
      set_loaded_snapshot(*state, snapshot, path);
      log_info(*state, "已导入 JSON 快照: " + path);
      return;
    }

    ProjectLoader loader;
    const auto model = loader.dump(path,
                                   {.include_runtime_only = true,
                                    .only_static_known = false,
                                    .symbol_name = std::nullopt,
                                    .expand_depth = 8});
    set_loaded_project(*state, model, LoadedContentKind::ElfProject, path);
    log_info(*state, "已分析文件: " + path);
  } catch (const std::exception& error) {
    log_error(*state, error.what());
  }
}

}  // namespace

Application::Application(UiLaunchOptions options) : options_(std::move(options)) {}

Application::~Application() {
  shutdown();
}

int Application::run() {
  if (!initialize()) {
    return 1;
  }

  load_startup_content();
  while (window_ != nullptr && glfwWindowShouldClose(window_) == 0) {
    render_frame();
  }
  return 0;
}

bool Application::initialize() {
  if (glfwInit() == GLFW_FALSE) {
    logging::log(logging::Level::Error, "glfwInit 失败");
    return false;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

  window_ = glfwCreateWindow(1600, 960, "ElfStaticView", nullptr, nullptr);
  if (window_ == nullptr) {
    logging::log(logging::Level::Error, "glfwCreateWindow 失败");
    shutdown();
    return false;
  }

  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);
  glfwSetWindowUserPointer(window_, &state_);
  glfwSetDropCallback(window_, glfw_drop_callback);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  configure_ui_font(io);

  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 4.0F;
  style.FrameRounding = 4.0F;
  style.PopupRounding = 4.0F;
  style.GrabRounding = 4.0F;

  if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
    logging::log(logging::Level::Error, "ImGui_ImplGlfw_InitForOpenGL 失败");
    shutdown();
    return false;
  }
  if (!ImGui_ImplOpenGL3_Init("#version 330")) {
    logging::log(logging::Level::Error, "ImGui_ImplOpenGL3_Init 失败");
    shutdown();
    return false;
  }

  compile_filter_rules(state_.filters);
  log_info(state_, "UI 初始化完成");
  return true;
}

void Application::shutdown() {
  if (window_ != nullptr) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
  glfwTerminate();
}

void Application::load_startup_content() {
  if (!options_.startup_file.has_value()) {
    return;
  }

  try {
    if (options_.startup_file_is_snapshot) {
      set_loaded_snapshot(state_,
                          parse_snapshot_json(read_all_text(options_.startup_file.value())),
                          options_.startup_file.value());
      log_info(state_, "已加载启动快照: " + options_.startup_file.value());
      return;
    }

    ProjectLoader loader;
    set_loaded_project(state_,
                       loader.dump(options_.startup_file.value(),
                                   {.include_runtime_only = true,
                                    .only_static_known = false,
                                    .symbol_name = std::nullopt,
                                    .expand_depth = 8}),
                       LoadedContentKind::ElfProject,
                       options_.startup_file.value());
    log_info(state_, "已加载启动文件: " + options_.startup_file.value());
  } catch (const std::exception& error) {
    log_error(state_, error.what());
  }
}

void Application::render_frame() {
  glfwPollEvents();

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  MainWindow window;
  window.render(state_);
  if (state_.request_exit) {
    glfwSetWindowShouldClose(window_, GLFW_TRUE);
  }

  ImGui::Render();
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  glViewport(0, 0, width, height);
  glClearColor(0.09F, 0.10F, 0.12F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glfwSwapBuffers(window_);
}

}  // namespace elf_static_view::ui
