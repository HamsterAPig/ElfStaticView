#include "ui/application.hpp"

#include "elf_static_view/project.hpp"
#include "logging/logger.hpp"
#include "platform/utf8.hpp"
#include "ui/file_dialogs.hpp"
#include "ui/filter_matcher.hpp"
#include "ui/main_window.hpp"
#include "ui/version_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <string_view>
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

constexpr float kBaseFontSize = 18.0F;

float sanitize_ui_scale(const float x_scale, const float y_scale) {
  const float candidate = std::max(x_scale, y_scale);
  if (!std::isfinite(candidate) || candidate <= 0.0F) {
    return 1.0F;
  }
  return std::clamp(candidate, 1.0F, 4.0F);
}

#if defined(_WIN32)
void enable_high_dpi_mode() {
  // 先争取 Per-Monitor V2，失败时再退回系统 DPI aware，避免 Windows 位图缩放把字体拉虚。
  using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
  if (const HMODULE user32 = LoadLibraryW(L"user32.dll"); user32 != nullptr) {
    const FARPROC raw_proc = GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    SetProcessDpiAwarenessContextFn set_dpi_awareness = nullptr;
    static_assert(sizeof(set_dpi_awareness) == sizeof(raw_proc));
    std::memcpy(&set_dpi_awareness, &raw_proc, sizeof(set_dpi_awareness));
    if (set_dpi_awareness != nullptr &&
        set_dpi_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
      FreeLibrary(user32);
      return;
    }
    FreeLibrary(user32);
  }
  SetProcessDPIAware();
}
#else
void enable_high_dpi_mode() {}
#endif

void configure_ui_font(ImGuiIO& io, const float ui_scale) {
  io.Fonts->Clear();
  ImFontConfig default_font_config {};
  default_font_config.SizePixels = std::round(kBaseFontSize * ui_scale);
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
    font_config.SizePixels = std::round(kBaseFontSize * ui_scale);
    font_config.OversampleH = 2;
    font_config.OversampleV = 1;

    for (const wchar_t* font_name : kCandidateFonts) {
      const auto font_path = fonts_directory / font_name;
      if (!std::filesystem::exists(font_path)) {
        continue;
      }

      const std::string utf8_font_path = platform::path_to_utf8(font_path);
      if (ImFont* font = io.Fonts->AddFontFromFileTTF(
            utf8_font_path.c_str(),
            font_config.SizePixels,
            &font_config,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) {
        io.FontDefault = font;
        return;
      }
    }
  }
#endif
  io.FontDefault = io.Fonts->AddFontDefault(&default_font_config);
}

void configure_ui_style(const float ui_scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  ImGui::StyleColorsDark(&style);
  style.WindowRounding = 4.0F;
  style.FrameRounding = 4.0F;
  style.PopupRounding = 4.0F;
  style.GrabRounding = 4.0F;
  style.ScaleAllSizes(ui_scale);
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
  auto next_frame_deadline = std::chrono::steady_clock::now();
  while (window_ != nullptr && glfwWindowShouldClose(window_) == 0) {
    poll_background_load();
    if (!needs_redraw_ && !ui_scale_dirty_) {
      // 空闲时阻塞等待事件；一旦收到事件，立刻补一帧把交互结果真正绘制出来。
      glfwWaitEvents();
      needs_redraw_ = true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < next_frame_deadline) {
      // 这里统一做 UI 限帧，避免拖动/滚动等高频事件把 ImGui 推到显示器满帧。
      std::this_thread::sleep_until(next_frame_deadline);
    }
    render_frame();
    next_frame_deadline = std::chrono::steady_clock::now() + frame_interval();
  }
  return 0;
}

bool Application::initialize() {
  enable_high_dpi_mode();
  if (glfwInit() == GLFW_FALSE) {
    logging::log(logging::Level::Error, "glfwInit 失败");
    return false;
  }

  glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

  const std::string window_title = "ElfStaticView " + current_version_string();
  window_ = glfwCreateWindow(1600, 960, window_title.c_str(), nullptr, nullptr);
  if (window_ == nullptr) {
    logging::log(logging::Level::Error, "glfwCreateWindow 失败");
    shutdown();
    return false;
  }

  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);
  glfwSetWindowUserPointer(window_, this);
  glfwSetDropCallback(window_, &Application::glfw_drop_callback);
  glfwSetWindowContentScaleCallback(window_, &Application::glfw_content_scale_callback);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  float x_scale = 1.0F;
  float y_scale = 1.0F;
  glfwGetWindowContentScale(window_, &x_scale, &y_scale);
  pending_ui_scale_ = sanitize_ui_scale(x_scale, y_scale);
  apply_pending_ui_scale();

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
  try {
    load_app_config(state_, options_.executable_path);
  } catch (const std::exception& error) {
    log_error(state_, error.what());
  }
  log_info(state_, "UI 初始化完成");
  request_redraw();
  return true;
}

void Application::glfw_drop_callback(GLFWwindow* window, const int count, const char** paths) {
  auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
  if (app == nullptr || count <= 0 || paths == nullptr) {
    return;
  }

  try {
    app->load_file_into_state(paths[0]);
  } catch (const std::exception& error) {
    log_error(app->state_, error.what());
    app->request_redraw();
  }
}

void Application::glfw_content_scale_callback(GLFWwindow* window, const float x_scale, const float y_scale) {
  auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
  if (app == nullptr) {
    return;
  }
  app->queue_ui_scale(x_scale, y_scale);
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

    load_file_into_state(options_.startup_file.value());
    log_info(state_, "已加载启动文件: " + options_.startup_file.value());
  } catch (const std::exception& error) {
    log_error(state_, error.what());
  }

  if (state_.window_title_dirty) {
    refresh_window_title();
  }
  request_redraw();
}

void Application::load_file_into_state(const std::string& path) {
  if (std::string_view(path).ends_with(".json")) {
    const auto snapshot = parse_snapshot_json(read_all_text(path));
    set_loaded_snapshot(state_, snapshot, path);
    log_info(state_, "已导入 JSON 快照: " + path);
    return;
  }

  start_elf_load(path);
}

DumpOptions Application::build_dump_options() const {
  DumpOptions options;
  options.include_runtime_only = !state_.load_policy.exclude_runtime_only_variables;
  options.only_static_known = state_.load_policy.static_storage_only;
  options.symbol_name = std::nullopt;
  options.expand_depth = state_.load_policy.expand_depth;
  options.load_policy = state_.load_policy;
  return options;
}

void Application::start_elf_load(const std::string& path) {
  const std::uint64_t task_id = next_load_task_id_++;
  begin_background_load(state_, task_id, path);
  log_info(state_, "开始分析 ELF: " + path);

  if (!state_.enable_background_loading) {
    try {
      ProjectLoader loader;
      finish_background_load(state_, task_id, loader.dump(path, build_dump_options()));
      log_info(state_, "已分析文件: " + path);
    } catch (const std::exception& error) {
      fail_background_load(state_, task_id, error.what());
    }
    request_redraw();
    return;
  }

  const DumpOptions options = build_dump_options();
  state_.background_load.future = std::async(std::launch::async, [path, options]() {
    ProjectLoader loader;
    return loader.dump(path, options);
  });
  request_redraw();
}

void Application::poll_background_load() {
  if (state_.background_load.status != BackgroundLoadStatus::Loading) {
    return;
  }
  if (!state_.background_load.future.valid()) {
    return;
  }
  if (state_.background_load.future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return;
  }

  const std::uint64_t task_id = state_.background_load.task_id;
  const std::string path = state_.background_load.path;
  try {
    auto model = state_.background_load.future.get();
    finish_background_load(state_, task_id, std::move(model));
    log_info(state_, "已分析文件: " + path);
  } catch (const std::exception& error) {
    fail_background_load(state_, task_id, error.what());
  }
  request_redraw();
}

void Application::refresh_window_title() {
  if (window_ == nullptr) {
    return;
  }

  const std::string title = build_window_title(state_);
  glfwSetWindowTitle(window_, title.c_str());
  state_.window_title_dirty = false;
}

void Application::request_redraw() {
  needs_redraw_ = true;
  if (window_ != nullptr) {
    glfwPostEmptyEvent();
  }
}

void Application::queue_ui_scale(const float x_scale, const float y_scale) {
  pending_ui_scale_ = sanitize_ui_scale(x_scale, y_scale);
  ui_scale_dirty_ = std::abs(pending_ui_scale_ - ui_scale_) > 0.01F;
  if (ui_scale_dirty_) {
    request_redraw();
  }
}

void Application::apply_pending_ui_scale() {
  ui_scale_ = pending_ui_scale_;
  ui_scale_dirty_ = false;

  ImGuiIO& io = ImGui::GetIO();
  configure_ui_font(io, ui_scale_);
  configure_ui_style(ui_scale_);

  if (ImGui::GetCurrentContext() != nullptr &&
      ImGui::GetIO().BackendRendererUserData != nullptr) {
    ImGui_ImplOpenGL3_DestroyDeviceObjects();
  }
}

std::chrono::steady_clock::duration Application::frame_interval() const {
  using namespace std::chrono;
  return duration_cast<steady_clock::duration>(
    duration<double>(1.0 / static_cast<double>(sanitize_ui_refresh_rate(state_.ui_refresh_rate))));
}

void Application::render_frame() {
  needs_redraw_ = false;
  if (ui_scale_dirty_) {
    // DPI 变化时在帧起点统一重建字体与样式，避免窗口跨屏后继续使用旧字形纹理。
    apply_pending_ui_scale();
  }

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  MainWindow window;
  window.render(state_);
  if (state_.pending_open_elf_path.has_value()) {
    const std::string path = *state_.pending_open_elf_path;
    state_.pending_open_elf_path.reset();
    try {
      start_elf_load(path);
    } catch (const std::exception& error) {
      log_error(state_, error.what());
    }
  }
  if (state_.window_title_dirty) {
    refresh_window_title();
    needs_redraw_ = true;
  }
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
