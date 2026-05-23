#include "ui/version_check.hpp"

#include "platform/utf8.hpp"

#include <yaml-cpp/yaml.h>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef ELF_STATIC_VIEW_VERSION
#define ELF_STATIC_VIEW_VERSION "0.0.0"
#endif

namespace elf_static_view::ui {

namespace {

constexpr char kConfigFileName[] = "elf-static-view.yaml";

std::string trim_copy(const std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }

  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

std::filesystem::path config_path_for_executable(const std::filesystem::path& executable_path) {
  const auto parent_path = executable_path.has_parent_path()
                             ? executable_path.parent_path()
                             : std::filesystem::current_path();
  return parent_path / kConfigFileName;
}

std::string path_to_log_text(const std::filesystem::path& path) {
  return platform::path_to_utf8(path);
}

std::string read_yaml_scalar(const YAML::Node& node, const char* key) {
  if (!node || !node[key]) {
    return {};
  }
  return trim_copy(node[key].as<std::string>());
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("无法打开配置文件: " + path_to_log_text(path));
  }
  std::ostringstream stream;
  stream << input.rdbuf();
  return stream.str();
}

std::string strip_version_prefix(std::string value) {
  value = trim_copy(value);
  if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) {
    value.erase(value.begin());
  }
  return value;
}

int compare_versions(const std::string& left, const std::string& right) {
  const std::string clean_left = strip_version_prefix(left);
  const std::string clean_right = strip_version_prefix(right);
  std::size_t left_cursor = 0;
  std::size_t right_cursor = 0;

  while (left_cursor < clean_left.size() || right_cursor < clean_right.size()) {
    std::uint64_t left_part = 0;
    while (left_cursor < clean_left.size() &&
           std::isdigit(static_cast<unsigned char>(clean_left[left_cursor])) != 0) {
      left_part = (left_part * 10U) + static_cast<std::uint64_t>(clean_left[left_cursor] - '0');
      ++left_cursor;
    }

    std::uint64_t right_part = 0;
    while (right_cursor < clean_right.size() &&
           std::isdigit(static_cast<unsigned char>(clean_right[right_cursor])) != 0) {
      right_part = (right_part * 10U) + static_cast<std::uint64_t>(clean_right[right_cursor] - '0');
      ++right_cursor;
    }

    if (left_part < right_part) {
      return -1;
    }
    if (left_part > right_part) {
      return 1;
    }

    while (left_cursor < clean_left.size() &&
           std::isdigit(static_cast<unsigned char>(clean_left[left_cursor])) == 0) {
      ++left_cursor;
    }
    while (right_cursor < clean_right.size() &&
           std::isdigit(static_cast<unsigned char>(clean_right[right_cursor])) == 0) {
      ++right_cursor;
    }
  }

  return 0;
}

#if defined(_WIN32)

struct ParsedUri {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
  bool https = true;
};

ParsedUri parse_http_uri(const std::string& uri_text) {
  const std::wstring uri = platform::utf8_to_wide(uri_text);
  URL_COMPONENTSW parts {};
  parts.dwStructSize = sizeof(parts);

  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  wchar_t extra[2048] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));

  if (WinHttpCrackUrl(uri.c_str(), static_cast<DWORD>(uri.size()), 0, &parts) == FALSE) {
    throw std::runtime_error("版本检查 URI 无法解析");
  }
  if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) {
    throw std::runtime_error("版本检查只支持 http 或 https URI");
  }

  ParsedUri parsed;
  parsed.host.assign(parts.lpszHostName, parts.dwHostNameLength);
  parsed.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  parsed.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  if (parsed.path.empty()) {
    parsed.path = L"/";
  }
  parsed.port = parts.nPort;
  parsed.https = parts.nScheme == INTERNET_SCHEME_HTTPS;
  return parsed;
}

std::string http_get_text(const std::string& uri_text) {
  const ParsedUri uri = parse_http_uri(uri_text);
  const HINTERNET session = WinHttpOpen(L"ElfStaticView/1.0",
                                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS,
                                        0);
  if (session == nullptr) {
    throw std::runtime_error("版本检查无法初始化 HTTP 会话");
  }

  const HINTERNET connection =
    WinHttpConnect(session, uri.host.c_str(), uri.port, 0);
  if (connection == nullptr) {
    WinHttpCloseHandle(session);
    throw std::runtime_error("版本检查无法连接服务器");
  }

  const DWORD flags = uri.https ? WINHTTP_FLAG_SECURE : 0;
  const HINTERNET request =
    WinHttpOpenRequest(connection, L"GET", uri.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (request == nullptr) {
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    throw std::runtime_error("版本检查无法创建请求");
  }

  if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE ||
      WinHttpReceiveResponse(request, nullptr) == FALSE) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    throw std::runtime_error("版本检查请求失败");
  }

  std::string response;
  DWORD available = 0;
  while (WinHttpQueryDataAvailable(request, &available) != FALSE && available > 0) {
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (WinHttpReadData(request, chunk.data(), available, &read) == FALSE) {
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connection);
      WinHttpCloseHandle(session);
      throw std::runtime_error("版本检查响应读取失败");
    }
    chunk.resize(read);
    response += chunk;
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return response;
}

#else

std::string http_get_text(const std::string&) {
  throw std::runtime_error("当前平台暂未实现版本检查 HTTP 客户端");
}

#endif

VersionCheckState parse_version_response(const std::string& response_text,
                                         const std::string& check_uri) {
  const YAML::Node root = YAML::Load(response_text);
  VersionCheckState result;
  result.check_uri = check_uri;
  result.latest_version = read_yaml_scalar(root, "version");
  if (result.latest_version.empty()) {
    result.latest_version = read_yaml_scalar(root, "latest_version");
  }
  result.release_url = read_yaml_scalar(root, "url");
  if (result.release_url.empty()) {
    result.release_url = read_yaml_scalar(root, "release_url");
  }

  if (result.latest_version.empty()) {
    throw std::runtime_error("版本检查响应缺少 version 或 latest_version 字段");
  }

  result.has_new_version = compare_versions(ELF_STATIC_VIEW_VERSION, result.latest_version) < 0;
  std::ostringstream message;
  if (result.has_new_version) {
    message << "发现新版本 " << result.latest_version << "，当前版本 "
            << ELF_STATIC_VIEW_VERSION;
  } else {
    message << "当前已是最新版本 " << ELF_STATIC_VIEW_VERSION;
  }
  result.message = message.str();
  return result;
}

}  // namespace

void load_version_check_config(AppState& state, const std::filesystem::path& executable_path) {
  const auto config_path = config_path_for_executable(executable_path);
  if (!std::filesystem::exists(config_path)) {
    log_info(state, "未找到配置文件: " + path_to_log_text(config_path));
    return;
  }

  const YAML::Node root = YAML::Load(read_text_file(config_path));
  const std::string check_uri = read_yaml_scalar(root["updates"], "check_uri");
  if (check_uri.empty()) {
    log_info(state, "配置文件未启用版本检查: " + path_to_log_text(config_path));
    return;
  }

  VersionCheckState version_check;
  version_check.check_uri = check_uri;
  version_check.message = "版本检查 URI 已加载";
  state.version_check = version_check;
  log_info(state, "版本检查 URI 已加载: " + check_uri);
}

void check_for_new_version(AppState& state) {
  if (!state.version_check.has_value() || state.version_check->check_uri.empty()) {
    log_error(state, "未配置版本检查 URI，请在 elf-static-view.yaml 中设置 updates.check_uri");
    return;
  }

  const std::string check_uri = state.version_check->check_uri;
  const std::string response_text = http_get_text(check_uri);
  state.version_check = parse_version_response(response_text, check_uri);
  log_info(state, state.version_check->message);
}

}  // namespace elf_static_view::ui
