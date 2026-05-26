#include "ui/version_check.hpp"

#include "elf_static_view/version.hpp"
#include "analysis/address_bias.hpp"
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

namespace elf_static_view::ui {

namespace {

constexpr char kConfigFileName[] = "elf-static-view.yaml";
constexpr char kDefaultRepositoryUrl[] = "https://github.com/HamsterAPig/ElfStaticView";
constexpr char kDefaultReleasesApiUrl[] =
  "https://api.github.com/repos/HamsterAPig/ElfStaticView/releases/latest";
constexpr char kDefaultAuthorName[] = "HamsterAPig";
constexpr char kDefaultAuthorEmail[] = "diyhome@outlook.com";

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

std::string first_non_empty(std::string first,
                            std::string second = {},
                            std::string third = {},
                            std::string fourth = {}) {
  if (!first.empty()) {
    return first;
  }
  if (!second.empty()) {
    return second;
  }
  if (!third.empty()) {
    return third;
  }
  return fourth;
}

bool read_yaml_bool(const YAML::Node& node, const char* key, const bool default_value) {
  if (!node || !node[key]) {
    return default_value;
  }
  return node[key].as<bool>();
}

std::optional<std::string> validate_compile_unit_path_rules_text(const std::string& rules_text) {
  std::istringstream lines(rules_text);
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(lines, line)) {
    ++line_number;
    std::string pattern = trim_copy(line);
    if (pattern.empty() || pattern.starts_with('#')) {
      continue;
    }
    if (pattern == "!") {
      return "第 " + std::to_string(line_number) + " 行只有 `!`，缺少实际路径模式";
    }
  }
  return std::nullopt;
}

std::optional<int> read_yaml_int(const YAML::Node& node, const char* key) {
  if (!node || !node[key]) {
    return std::nullopt;
  }
  return node[key].as<int>();
}

std::size_t read_yaml_size_t(const YAML::Node& node, const char* key, const std::size_t default_value) {
  if (!node || !node[key]) {
    return default_value;
  }
  return node[key].as<std::size_t>();
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
  if (const auto metadata_separator = value.find('+'); metadata_separator != std::string::npos) {
    value.erase(metadata_separator);
  }
  return value;
}

YAML::Node load_existing_config_root(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return YAML::Node(YAML::NodeType::Map);
  }
  YAML::Node root = YAML::Load(read_text_file(path));
  if (!root || !root.IsMap()) {
    return YAML::Node(YAML::NodeType::Map);
  }
  return root;
}

void write_config_root(const std::filesystem::path& path, const YAML::Node& root) {
  if (const auto parent = path.parent_path(); !parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("无法写入配置文件: " + path_to_log_text(path));
  }
  YAML::Emitter emitter;
  emitter.SetIndent(2);
  emitter << root;
  output << emitter.c_str();
}

int compare_versions_impl(const std::string& left, const std::string& right) {
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

std::string http_get_text_impl(const std::string& uri_text) {
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

std::string http_get_text_impl(const std::string&) {
  throw std::runtime_error("当前平台暂未实现版本检查 HTTP 客户端");
}

#endif

}  // namespace

std::string http_get_text(const std::string& uri_text) {
  return http_get_text_impl(uri_text);
}

const ReleaseMetadata& default_release_metadata() {
  static const ReleaseMetadata metadata {
    .repository_url = kDefaultRepositoryUrl,
    .releases_api_url = kDefaultReleasesApiUrl,
    .author_name = kDefaultAuthorName,
    .author_email = kDefaultAuthorEmail,
  };
  return metadata;
}

int compare_version_strings(const std::string& left, const std::string& right) {
  return compare_versions_impl(left, right);
}

std::string current_version_string() {
  return ELF_STATIC_VIEW_VERSION;
}

VersionCheckState resolve_version_check_state(const AppState& state) {
  const ReleaseMetadata& defaults = default_release_metadata();
  // 运行时始终先用内置 GitHub 元数据兜底，再叠加配置值和最近一次检查结果。
  VersionCheckState resolved {
    .repository_url = defaults.repository_url,
    .check_uri = defaults.releases_api_url,
    .latest_version = {},
    .release_url = {},
    .release_name = {},
    .release_notes = {},
    .message = {},
    .has_new_version = false,
    .check_uri_uses_default = true,
  };

  if (!state.version_check.has_value()) {
    return resolved;
  }

  const VersionCheckState& stored = state.version_check.value();
  if (!stored.repository_url.empty()) {
    resolved.repository_url = stored.repository_url;
  }
  if (!stored.check_uri.empty()) {
    resolved.check_uri = stored.check_uri;
    resolved.check_uri_uses_default = false;
  }
  if (!stored.latest_version.empty()) {
    resolved.latest_version = stored.latest_version;
  }
  if (!stored.release_url.empty()) {
    resolved.release_url = stored.release_url;
  }
  resolved.release_name = stored.release_name;
  resolved.release_notes = stored.release_notes;
  resolved.message = stored.message;
  resolved.has_new_version = stored.has_new_version;
  return resolved;
}

VersionCheckState parse_version_response_text(const std::string& response_text,
                                              const std::string& check_uri,
                                              const std::string& repository_url) {
  const YAML::Node root = YAML::Load(response_text);
  const ReleaseMetadata& defaults = default_release_metadata();

  VersionCheckState result;
  result.repository_url = repository_url.empty() ? defaults.repository_url : repository_url;
  result.check_uri = check_uri;
  result.check_uri_uses_default = check_uri == defaults.releases_api_url;
  // 这里同时兼容历史自定义 YAML 字段和 GitHub Releases JSON 字段。
  result.latest_version = first_non_empty(read_yaml_scalar(root, "version"),
                                          read_yaml_scalar(root, "latest_version"),
                                          read_yaml_scalar(root, "tag_name"));
  result.release_url = first_non_empty(read_yaml_scalar(root, "url"),
                                       read_yaml_scalar(root, "release_url"),
                                       read_yaml_scalar(root, "html_url"),
                                       result.repository_url);
  result.release_name = first_non_empty(read_yaml_scalar(root, "name"), result.latest_version);
  result.release_notes = first_non_empty(read_yaml_scalar(root, "body"),
                                         read_yaml_scalar(root, "notes"));

  if (result.latest_version.empty()) {
    throw std::runtime_error("版本检查响应缺少 version、latest_version 或 tag_name 字段");
  }

  result.has_new_version = compare_version_strings(current_version_string(), result.latest_version) < 0;
  std::ostringstream message;
  if (result.has_new_version) {
    message << "发现新版本 " << result.latest_version << "，当前版本 "
            << current_version_string();
  } else {
    message << "当前已是最新版本 " << current_version_string();
  }
  if (!result.release_name.empty() && result.release_name != result.latest_version) {
    message << "（" << result.release_name << "）";
  }
  result.message = message.str();
  return result;
}

LoadPolicy default_load_policy() {
  LoadPolicy policy;
  policy.compile_unit_path_rules_text =
    "# 默认排除常见通用库目录\n"
    "**/CMSIS/**\n"
    "**/HAL/**\n"
    "**/Drivers/**\n"
    "**/Middlewares/**\n"
    "**/gcc-arm-none-eabi/**\n"
    "**/arm-none-eabi/**\n"
    "!**/Core/**\n"
    "!**/App/**\n";
  return policy;
}

LoadPolicy load_cli_load_policy(const std::filesystem::path& executable_path) {
  // CLI 复用同一份配置读取逻辑，避免与 UI 默认策略分叉。
  AppState state;
  load_app_config(state, executable_path);
  return state.load_policy;
}

void load_app_config(AppState& state, const std::filesystem::path& executable_path) {
  state.config_path = config_path_for_executable(executable_path);
  state.enable_background_loading = true;
  state.filter_debounce_ms = sanitize_filter_debounce_ms(300);
  state.load_policy = default_load_policy();
  if (!std::filesystem::exists(state.config_path)) {
    log_info(state, "未找到配置文件: " + path_to_log_text(state.config_path));
    log_info(state, "未配置 updates.check_uri，版本检查将回退到默认 GitHub Releases API。");
    return;
  }

  const YAML::Node root = load_existing_config_root(state.config_path);
  const YAML::Node updates = root["updates"];
  const std::string check_uri = read_yaml_scalar(updates, "check_uri");
  const std::string repository_url = read_yaml_scalar(updates, "repository_url");
  if (!check_uri.empty() || !repository_url.empty()) {
    VersionCheckState version_check;
    version_check.repository_url = repository_url;
    version_check.check_uri = check_uri;
    version_check.check_uri_uses_default = check_uri.empty();
    state.version_check = version_check;
  }

  if (!check_uri.empty()) {
    log_info(state, "版本检查 URI 已加载: " + check_uri);
  } else {
    log_info(state, "未配置 updates.check_uri，版本检查将回退到默认 GitHub Releases API。");
  }
  if (!repository_url.empty()) {
    log_info(state, "仓库地址已加载: " + repository_url);
  } else {
    log_info(state, "未配置 updates.repository_url，About 将显示默认 GitHub 仓库地址。");
  }

  const YAML::Node copy = root["copy"];
  state.copy_hex_without_prefix = read_yaml_bool(copy, "strip_hex_prefix", false);
  const std::string stored_copy_address_base = read_yaml_scalar(copy, "address_base");
  if (!stored_copy_address_base.empty()) {
    if (const auto parsed = parse_copy_address_base(stored_copy_address_base); parsed.has_value()) {
      state.copy_address_base = parsed.value();
      log_info(state, "复制进制已加载: " + std::string(copy_address_base_label(parsed.value())));
    } else {
      log_error(state, "配置文件中的 copy.address_base 无效: " + stored_copy_address_base);
    }
  }

  const YAML::Node ui = root["ui"];
  try {
    if (const auto stored_refresh_rate = read_yaml_int(ui, "refresh_rate"); stored_refresh_rate.has_value()) {
      state.ui_refresh_rate = sanitize_ui_refresh_rate(stored_refresh_rate.value());
      log_info(state, "界面刷新率已加载: " + std::to_string(state.ui_refresh_rate) + " FPS");
    }
  } catch (const std::exception& error) {
    log_error(state, std::string("配置文件中的 ui.refresh_rate 无效: ") + error.what());
  }
  try {
    if (const auto stored_filter_debounce = read_yaml_int(ui, "filter_debounce_ms");
        stored_filter_debounce.has_value()) {
      const int sanitized = sanitize_filter_debounce_ms(stored_filter_debounce.value());
      state.filter_debounce_ms = sanitized;
      if (sanitized == stored_filter_debounce.value()) {
        log_info(state, "筛选输入延迟已加载: " + std::to_string(state.filter_debounce_ms) + " ms");
      } else {
        log_error(state, "配置文件中的 ui.filter_debounce_ms 超出范围，将回退到 300 ms。");
      }
    }
  } catch (const std::exception& error) {
    state.filter_debounce_ms = sanitize_filter_debounce_ms(300);
    log_error(state, std::string("配置文件中的 ui.filter_debounce_ms 无效: ") + error.what());
  }

  const YAML::Node load_policy = root["load_policy"];
  state.enable_background_loading =
    read_yaml_bool(load_policy, "enable_background_loading", true);
  state.load_policy.static_storage_only =
    read_yaml_bool(load_policy, "default_static_storage_only", true);
  state.load_policy.exclude_formal_parameters =
    read_yaml_bool(load_policy, "exclude_formal_parameters", true);
  state.load_policy.exclude_runtime_only_variables =
    read_yaml_bool(load_policy, "exclude_runtime_only_variables", true);
  state.load_policy.lazy_expand_children =
    read_yaml_bool(load_policy, "lazy_expand_children", true);
  state.load_policy.enable_parse_metrics =
    read_yaml_bool(load_policy, "enable_parse_metrics", true);
  state.load_policy.expand_depth = read_yaml_size_t(load_policy, "max_expand_depth", 6);
  if (const std::string rules = read_yaml_scalar(load_policy, "compile_unit_path_rules"); !rules.empty()) {
    if (const auto validation_error = validate_compile_unit_path_rules_text(rules);
        validation_error.has_value()) {
      log_error(state,
                "配置文件中的 load_policy.compile_unit_path_rules 无效: " + validation_error.value() +
                  "；将回退到默认规则。");
    } else {
      state.load_policy.compile_unit_path_rules_text = rules;
    }
  }

  const YAML::Node address_bias = root["address_bias"];
  state.persist_address_bias_to_config = read_yaml_bool(address_bias, "write_back", false);
  if (!state.persist_address_bias_to_config) {
    return;
  }

  const std::string stored_bias = read_yaml_scalar(address_bias, "value");
  if (stored_bias.empty()) {
    log_info(state, "配置文件已启用地址偏移写回，但未找到 address_bias.value。");
    return;
  }

  try {
    state.address_bias = elf_static_view::parse_address_bias(stored_bias);
    state.address_bias_input = stored_bias;
    state.address_bias_error.reset();
    log_info(state, "已从配置文件恢复地址偏移: " + stored_bias);
  } catch (const std::exception& error) {
    log_error(state, "配置文件中的地址偏移无效: " + std::string(error.what()));
  }
}

void save_app_config(const AppState& state) {
  if (state.config_path.empty()) {
    throw std::runtime_error("配置文件路径尚未初始化");
  }

  YAML::Node root = load_existing_config_root(state.config_path);
  if (state.version_check.has_value()) {
    if (!state.version_check->check_uri.empty()) {
      root["updates"]["check_uri"] = state.version_check->check_uri;
    }
    if (!state.version_check->repository_url.empty()) {
      root["updates"]["repository_url"] = state.version_check->repository_url;
    }
  }
  root["copy"]["address_base"] = copy_address_base_to_config_value(state.copy_address_base);
  root["copy"]["strip_hex_prefix"] = state.copy_hex_without_prefix;
  root["ui"]["refresh_rate"] = sanitize_ui_refresh_rate(state.ui_refresh_rate);
  root["ui"]["filter_debounce_ms"] = sanitize_filter_debounce_ms(state.filter_debounce_ms);
  root["load_policy"]["enable_background_loading"] = state.enable_background_loading;
  root["load_policy"]["default_static_storage_only"] = state.load_policy.static_storage_only;
  root["load_policy"]["exclude_formal_parameters"] = state.load_policy.exclude_formal_parameters;
  root["load_policy"]["exclude_runtime_only_variables"] =
    state.load_policy.exclude_runtime_only_variables;
  root["load_policy"]["compile_unit_path_rules"] = state.load_policy.compile_unit_path_rules_text;
  root["load_policy"]["max_expand_depth"] = state.load_policy.expand_depth;
  root["load_policy"]["lazy_expand_children"] = state.load_policy.lazy_expand_children;
  root["load_policy"]["enable_parse_metrics"] = state.load_policy.enable_parse_metrics;

  YAML::Node address_bias = root["address_bias"];
  address_bias["write_back"] = state.persist_address_bias_to_config;
  if (state.persist_address_bias_to_config && !state.address_bias_error.has_value()) {
    const std::string bias_text = trim_copy(state.address_bias_input);
    if (!bias_text.empty()) {
      address_bias["value"] = bias_text;
    }
  }

  write_config_root(state.config_path, root);
}

}  // namespace elf_static_view::ui
