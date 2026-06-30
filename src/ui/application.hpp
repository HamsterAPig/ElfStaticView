#pragma once

#include "ui/app_state.hpp"
#include "ui/startup_diagnostics.hpp"
#include "ui/ui_task_runner.hpp"

#include <chrono>
#include <cstdint>
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
    void start_elf_load(const std::string& path);
    void start_snapshot_import(const std::string& path);
    void start_snapshot_export(const std::string& path, const ExportOptions& options);
    void start_raw_dwarf_export(const std::string& source_path, const std::string& output_path);
    void start_json_preview_build();
    void start_version_check();
    void start_filter_build();
    void poll_background_tasks();
    void poll_opened_file_recreate();
    [[nodiscard]] std::string compute_json_preview_cache_key() const;
    [[nodiscard]] DumpOptions build_dump_options() const;
    void refresh_window_title();
    void request_redraw();
    void queue_ui_scale(float x_scale, float y_scale);
    void apply_pending_ui_scale();
    [[nodiscard]] std::chrono::steady_clock::duration frame_interval() const;
    void render_frame();

    static void glfw_drop_callback(GLFWwindow* window, int count, const char** paths);
    static void glfw_content_scale_callback(GLFWwindow* window, float x_scale, float y_scale);

    UiLaunchOptions options_;
    GLFWwindow* window_ = nullptr;
    AppState state_;
    float ui_scale_ = 1.0F;
    float pending_ui_scale_ = 1.0F;
    bool ui_scale_dirty_ = false;
    bool needs_redraw_ = true;
    std::uint64_t next_load_task_id_ = 1;
    std::uint64_t next_filter_task_id_ = 1;
    UiTaskRunner task_runner_;
    bool imgui_context_created_ = false;
    bool imgui_glfw_initialized_ = false;
    bool imgui_opengl_initialized_ = false;
};

} // namespace elf_static_view::ui
