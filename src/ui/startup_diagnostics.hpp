#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace elf_static_view::ui {

struct OpenGlContextCandidate {
    std::string label;
    int major = 0;
    int minor = 0;
    bool core_profile = false;
    std::string glsl_version;
};

struct GraphicsProbeResult {
    OpenGlContextCandidate candidate;
    bool success = false;
    std::string message;
    std::string vendor;
    std::string renderer;
    std::string version;
    std::string glsl_version;
};

struct GraphicsDiagnosticReport {
    bool glfw_initialized = false;
    std::string glfw_error;
    std::vector<GraphicsProbeResult> probes;
};

[[nodiscard]] const std::vector<OpenGlContextCandidate>& opengl_context_candidates();
void apply_opengl_window_hints(const OpenGlContextCandidate& candidate, bool visible);
[[nodiscard]] std::string describe_opengl_candidate(const OpenGlContextCandidate& candidate);

void install_startup_glfw_error_callback();
void clear_startup_glfw_error();
[[nodiscard]] std::string last_startup_glfw_error();

[[nodiscard]] std::filesystem::path startup_log_path();
void append_startup_log(std::string_view message);
void show_startup_error_dialog(std::string_view title, std::string_view message);

[[nodiscard]] GraphicsDiagnosticReport run_graphics_diagnostics();
[[nodiscard]] std::string render_graphics_diagnostics(const GraphicsDiagnosticReport& report);

} // namespace elf_static_view::ui
