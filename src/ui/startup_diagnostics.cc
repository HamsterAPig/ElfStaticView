#include "ui/startup_diagnostics.hpp"

#include "platform/utf8.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <GLFW/glfw3.h>

namespace elf_static_view::ui {

namespace {

    std::mutex g_glfw_error_mutex;
    std::string g_last_glfw_error;
    constexpr unsigned int kGlShadingLanguageVersion = 0x8B8C;

    std::string path_to_utf8_lossy(const std::filesystem::path& path)
    {
        try {
            return platform::path_to_utf8(path);
        } catch (const std::exception&) {
            return path.string();
        }
    }

    std::string current_timestamp_local()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time_value = std::chrono::system_clock::to_time_t(now);
        std::tm local_time{};
#if defined(_WIN32)
        localtime_s(&local_time, &time_value);
#else
        localtime_r(&time_value, &local_time);
#endif
        std::ostringstream stream;
        stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
        return stream.str();
    }

#if defined(_WIN32)
    std::filesystem::path read_windows_environment_path(const wchar_t* name)
    {
        wchar_t buffer[MAX_PATH] = {};
        const DWORD length = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
        if (length == 0 || length >= std::size(buffer)) {
            return {};
        }
        return std::filesystem::path(buffer);
    }
#endif

    std::filesystem::path preferred_startup_log_path()
    {
#if defined(_WIN32)
        if (const std::filesystem::path local_app_data = read_windows_environment_path(L"LOCALAPPDATA");
            !local_app_data.empty()) {
            return local_app_data / "ElfStaticView" / "logs" / "startup.log";
        }
#else
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            return std::filesystem::path(home) / ".local" / "state" / "ElfStaticView" / "startup.log";
        }
#endif
        return std::filesystem::temp_directory_path() / "ElfStaticView" / "startup.log";
    }

    void startup_glfw_error_callback(const int code, const char* description)
    {
        std::ostringstream stream;
        stream << "GLFW error " << code;
        if (description != nullptr && description[0] != '\0') {
            stream << ": " << description;
        }

        {
            std::lock_guard<std::mutex> lock(g_glfw_error_mutex);
            g_last_glfw_error = stream.str();
        }
        append_startup_log(stream.str());
    }

    const char* safe_gl_string(const unsigned int name)
    {
        const auto* value = glGetString(name);
        if (value == nullptr) {
            return "";
        }
        return reinterpret_cast<const char*>(value);
    }

    GraphicsProbeResult probe_candidate(const OpenGlContextCandidate& candidate)
    {
        clear_startup_glfw_error();
        apply_opengl_window_hints(candidate, false);
        GLFWwindow* window = glfwCreateWindow(640, 480, "ElfStaticView graphics probe", nullptr, nullptr);
        if (window == nullptr) {
            std::string error = last_startup_glfw_error();
            if (error.empty()) {
                error = "glfwCreateWindow 失败";
            }
            return {.candidate = candidate, .success = false, .message = error};
        }

        glfwMakeContextCurrent(window);
        GraphicsProbeResult result{
            .candidate = candidate,
            .success = true,
            .message = "OK",
            .vendor = safe_gl_string(GL_VENDOR),
            .renderer = safe_gl_string(GL_RENDERER),
            .version = safe_gl_string(GL_VERSION),
            .glsl_version = safe_gl_string(kGlShadingLanguageVersion),
        };
        glfwDestroyWindow(window);
        return result;
    }

} // namespace

const std::vector<OpenGlContextCandidate>& opengl_context_candidates()
{
    static const std::vector<OpenGlContextCandidate> candidates{
        {.label = "OpenGL 3.3 Core", .major = 3, .minor = 3, .core_profile = true, .glsl_version = "#version 330"},
        {.label = "OpenGL 3.2 Core", .major = 3, .minor = 2, .core_profile = true, .glsl_version = "#version 150"},
        {.label = "OpenGL 3.0", .major = 3, .minor = 0, .core_profile = false, .glsl_version = "#version 130"},
        {.label = "OpenGL 2.1", .major = 2, .minor = 1, .core_profile = false, .glsl_version = "#version 120"},
    };
    return candidates;
}

void apply_opengl_window_hints(const OpenGlContextCandidate& candidate, const bool visible)
{
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, candidate.major);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, candidate.minor);
    glfwWindowHint(GLFW_OPENGL_PROFILE, candidate.core_profile ? GLFW_OPENGL_CORE_PROFILE : GLFW_OPENGL_ANY_PROFILE);
#if defined(__APPLE__)
    if (candidate.core_profile) {
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    }
#endif
}

std::string describe_opengl_candidate(const OpenGlContextCandidate& candidate)
{
    std::ostringstream stream;
    stream << candidate.label << " / " << candidate.glsl_version;
    return stream.str();
}

void install_startup_glfw_error_callback()
{
    glfwSetErrorCallback(startup_glfw_error_callback);
}

void clear_startup_glfw_error()
{
    std::lock_guard<std::mutex> lock(g_glfw_error_mutex);
    g_last_glfw_error.clear();
}

std::string last_startup_glfw_error()
{
    std::lock_guard<std::mutex> lock(g_glfw_error_mutex);
    return g_last_glfw_error;
}

std::filesystem::path startup_log_path()
{
    static const std::filesystem::path path = [] {
        std::filesystem::path candidate = preferred_startup_log_path();
        std::error_code error;
        std::filesystem::create_directories(candidate.parent_path(), error);
        if (!error) {
            return candidate;
        }
        candidate = std::filesystem::temp_directory_path() / "ElfStaticView-startup.log";
        std::filesystem::create_directories(candidate.parent_path(), error);
        return candidate;
    }();
    return path;
}

void append_startup_log(const std::string_view message)
{
    std::error_code error;
    std::filesystem::create_directories(startup_log_path().parent_path(), error);

    std::ofstream output(startup_log_path(), std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        return;
    }
    output << '[' << current_timestamp_local() << "] " << message << '\n';
}

void show_startup_error_dialog(const std::string_view title, const std::string_view message)
{
    append_startup_log(message);
#if defined(_WIN32)
    const std::string text =
        std::string(message) + "\n\n日志文件:\n" + path_to_utf8_lossy(startup_log_path());
    try {
        const std::wstring wide_title = platform::utf8_to_wide(title);
        const std::wstring wide_text = platform::utf8_to_wide(text);
        MessageBoxW(nullptr, wide_text.c_str(), wide_title.c_str(), MB_OK | MB_ICONERROR);
    } catch (const std::exception&) {
        MessageBoxA(nullptr, text.c_str(), std::string(title).c_str(), MB_OK | MB_ICONERROR);
    }
#else
    (void)title;
#endif
}

GraphicsDiagnosticReport run_graphics_diagnostics()
{
    install_startup_glfw_error_callback();
    clear_startup_glfw_error();

    GraphicsDiagnosticReport report;
    if (glfwInit() == GLFW_FALSE) {
        report.glfw_error = last_startup_glfw_error();
        if (report.glfw_error.empty()) {
            report.glfw_error = "glfwInit 失败";
        }
        return report;
    }

    report.glfw_initialized = true;
    for (const auto& candidate : opengl_context_candidates()) {
        report.probes.push_back(probe_candidate(candidate));
    }
    glfwTerminate();
    return report;
}

std::string render_graphics_diagnostics(const GraphicsDiagnosticReport& report)
{
    std::ostringstream stream;
    stream << "ElfStaticView 图形诊断\n";
    stream << "日志文件: " << path_to_utf8_lossy(startup_log_path()) << "\n";
    stream << "GLFW 初始化: " << (report.glfw_initialized ? "OK" : "失败") << "\n";
    if (!report.glfw_error.empty()) {
        stream << "GLFW 错误: " << report.glfw_error << "\n";
    }
    for (const auto& probe : report.probes) {
        stream << "\n[" << (probe.success ? "OK" : "FAIL") << "] "
               << describe_opengl_candidate(probe.candidate) << "\n";
        stream << "  结果: " << probe.message << "\n";
        if (probe.success) {
            stream << "  GL_VENDOR: " << probe.vendor << "\n";
            stream << "  GL_RENDERER: " << probe.renderer << "\n";
            stream << "  GL_VERSION: " << probe.version << "\n";
            stream << "  GLSL_VERSION: " << probe.glsl_version << "\n";
        }
    }
    return stream.str();
}

} // namespace elf_static_view::ui
