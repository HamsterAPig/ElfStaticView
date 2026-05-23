#include "ui/main_window.hpp"

#include "analysis/address_bias.hpp"
#include "elf_static_view/project.hpp"
#include "platform/utf8.hpp"
#include "ui/file_dialogs.hpp"
#include "ui/filter_matcher.hpp"
#include "ui/version_check.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <fstream>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace elf_static_view::ui {

namespace {

constexpr char kMainDockspaceName[] = "MainDockSpace";
constexpr char kVariablesWindowName[] = "变量###Variables";
constexpr char kInspectorWindowName[] = "详情###Inspector";
constexpr char kLogWindowName[] = "日志###Log";
constexpr char kJsonPreviewWindowName[] = "JSON 预览###JSON Preview";
constexpr char kVariableSearchInputId[] = "##variable_name_query";
constexpr char kAboutDialogName[] = "关于 ElfStaticView###about_dialog";
constexpr char kShortcutsDialogName[] = "快捷键说明###shortcuts_dialog";
constexpr CopyAddressBase kCopyAddressBaseOptions[] = {
  CopyAddressBase::Hex,
  CopyAddressBase::Dec,
  CopyAddressBase::Oct,
  CopyAddressBase::Bin,
};

const char* localized_type_kind(const TypeKind value) {
  switch (value) {
    case TypeKind::Base:
      return "基础类型";
    case TypeKind::Pointer:
      return "指针";
    case TypeKind::Reference:
      return "引用";
    case TypeKind::Typedef:
      return "类型别名";
    case TypeKind::Qualified:
      return "限定类型";
    case TypeKind::Array:
      return "数组";
    case TypeKind::Struct:
      return "结构体";
    case TypeKind::Class:
      return "类";
    case TypeKind::Union:
      return "联合体";
    case TypeKind::Enum:
      return "枚举";
    case TypeKind::Subroutine:
      return "子程序";
    case TypeKind::Unknown:
      return "未知";
  }
  return "未知";
}

const char* localized_availability(const Availability value) {
  switch (value) {
    case Availability::StaticAddressKnown:
      return "静态地址已知";
    case Availability::StaticLayoutKnown:
      return "静态布局已知";
    case Availability::RuntimeOnly:
      return "仅运行时可用";
    case Availability::Unavailable:
      return "不可用";
    case Availability::OptimizedOut:
      return "已被优化移除";
  }
  return "未知";
}

void setup_default_dock_layout(ImGuiID dockspace_id,
                               const ImGuiViewport* viewport,
                               ImGuiDockNodeFlags dockspace_flags) {
  if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace_id);
      node != nullptr &&
      (node->ChildNodes[0] != nullptr || node->ChildNodes[1] != nullptr || node->Windows.Size > 0)) {
    return;
  }

  ImGui::DockBuilderRemoveNode(dockspace_id);
  ImGui::DockBuilderAddNode(dockspace_id, dockspace_flags | ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);

  ImGuiID center_id = dockspace_id;
  const ImGuiID left_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Left, 0.28F, nullptr, &center_id);
  const ImGuiID right_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.32F, nullptr, &center_id);
  const ImGuiID bottom_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Down, 0.30F, nullptr, &center_id);

  // 首次启动时给四个核心面板一个稳定分区，避免全部叠在同一个 dock tab 里。
  ImGui::DockBuilderDockWindow(kVariablesWindowName, left_id);
  ImGui::DockBuilderDockWindow(kInspectorWindowName, right_id);
  ImGui::DockBuilderDockWindow(kLogWindowName, bottom_id);
  ImGui::DockBuilderDockWindow(kJsonPreviewWindowName, center_id);
  ImGui::DockBuilderFinish(dockspace_id);
}

bool node_or_descendant_matches(const AppState& state, const ExpandedNode& node) {
  if (matches_filters(state, node)) {
    return true;
  }
  for (const auto& child : node.children) {
    if (node_or_descendant_matches(state, child)) {
      return true;
    }
  }
  return false;
}

std::string build_log_panel_text(const AppState& state) {
  std::string text;
  for (const auto& line : state.log_messages) {
    text.append(line);
    text.push_back('\n');
  }
  if (!state.error_message.empty()) {
    text.append("当前错误: ");
    text.append(state.error_message);
  }
  return text;
}

void save_app_config_or_log(AppState& state) {
  try {
    save_app_config(state);
  } catch (const std::exception& error) {
    log_error(state, error.what());
  }
}

std::optional<std::string> format_selected_address_minus_bias(const AppState& state) {
  if (state.selected_node == nullptr) {
    return std::nullopt;
  }
  if (!state.selected_node->absolute_address.has_value()) {
    return std::nullopt;
  }

  const auto absolute_address = state.selected_node->absolute_address.value();
  if (state.address_bias < 0) {
    const auto magnitude = static_cast<std::uint64_t>(-(state.address_bias + 1)) + 1U;
    if (absolute_address > std::numeric_limits<std::uint64_t>::max() - magnitude) {
      return std::nullopt;
    }
  } else if (absolute_address < static_cast<std::uint64_t>(state.address_bias)) {
    return std::nullopt;
  }

  const std::uint64_t adjusted =
    state.address_bias < 0
      ? absolute_address + (static_cast<std::uint64_t>(-(state.address_bias + 1)) + 1U)
      : absolute_address - static_cast<std::uint64_t>(state.address_bias);
  return format_address_for_copy(adjusted, state);
}

std::optional<std::string> format_selected_raw_address(const AppState& state) {
  if (state.selected_node == nullptr || !state.selected_node->absolute_address.has_value()) {
    return std::nullopt;
  }
  return format_address_for_copy(state.selected_node->absolute_address.value(), state);
}

void copy_selected_address_minus_bias(AppState& state) {
  const auto address = format_selected_address_minus_bias(state);
  if (!address.has_value()) {
    log_error(state, "当前变量没有可复制的地址减偏移结果");
    return;
  }
  ImGui::SetClipboardText(address->c_str());
  log_info(state, "已复制当前变量地址减偏移结果: " + address.value());
}

void copy_selected_raw_address(AppState& state) {
  const auto address = format_selected_raw_address(state);
  if (!address.has_value()) {
    log_error(state, "当前变量没有可复制的原始地址");
    return;
  }
  ImGui::SetClipboardText(address->c_str());
  log_info(state, "已复制当前变量原始地址: " + address.value());
}

void handle_global_shortcuts(AppState& state) {
  const ImGuiIO& io = ImGui::GetIO();

  // 这里集中处理全局快捷键，避免每个面板自行抢键盘焦点。
  if (!io.WantTextInput &&
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, ImGuiInputFlags_RouteGlobal)) {
    state.focus_variable_search = true;
  }
  if (!io.WantTextInput &&
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, ImGuiInputFlags_RouteGlobal)) {
    copy_selected_address_minus_bias(state);
  }
  if (!io.WantTextInput &&
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, ImGuiInputFlags_RouteGlobal)) {
    copy_selected_raw_address(state);
  }
  if (ImGui::Shortcut(ImGuiKey_F1, ImGuiInputFlags_RouteGlobal)) {
    state.show_shortcuts_dialog = true;
  }
  if (ImGui::Shortcut(ImGuiKey_Escape, ImGuiInputFlags_RouteGlobal)) {
    state.focus_variable_search = false;
    ImGui::ClearActiveID();
  }
}

std::string read_all_text(const std::string& path) {
  std::ifstream input(platform::utf8_path(path), std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("无法打开文件: " + path);
  }
  std::ostringstream stream;
  stream << input.rdbuf();
  return stream.str();
}

void write_all_text(const std::string& path, const std::string& content) {
  std::ofstream output(platform::utf8_path(path), std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("无法写入文件: " + path);
  }
  output << content;
}

void render_tree_node(AppState& state, const ExpandedNode& node) {
  if (!node_or_descendant_matches(state, node)) {
    return;
  }

  const auto flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                     (node.children.empty() ? ImGuiTreeNodeFlags_Leaf : 0) |
                     ((state.selected_node == &node) ? ImGuiTreeNodeFlags_Selected : 0);

  const auto label =
    node.display_name + " [" + node.type_name + "] @ " +
    elf_static_view::format_address_summary(node, state.address_bias);
  const bool opened = ImGui::TreeNodeEx(static_cast<const void*>(&node), flags, "%s", label.c_str());
  if (ImGui::IsItemClicked()) {
    state.selected_node = &node;
  }
  if (ImGui::BeginPopupContextItem()) {
    state.selected_node = &node;
    if (ImGui::MenuItem("复制变量路径")) {
      ImGui::SetClipboardText(node.path.c_str());
    }
    if (node.absolute_address.has_value() && ImGui::MenuItem("复制原始地址")) {
      copy_selected_raw_address(state);
    }
    if (const auto adjusted = elf_static_view::apply_bias_to_absolute(node, state.address_bias);
        adjusted.has_value()) {
      if (ImGui::MenuItem("复制偏移后地址")) {
        const std::string copied_text = format_address_for_copy(adjusted.value(), state);
        ImGui::SetClipboardText(copied_text.c_str());
        log_info(state, "已复制当前变量偏移后地址: " + copied_text);
      }
    }
    if (node.relative_offset.has_value()) {
      const auto value = std::to_string(node.relative_offset.value());
      if (ImGui::MenuItem("复制相对偏移")) {
        ImGui::SetClipboardText(value.c_str());
      }
    }
    const auto json = render_expanded_node_json(node);
    if (ImGui::MenuItem("复制节点 JSON")) {
      ImGui::SetClipboardText(json.c_str());
    }
    ImGui::EndPopup();
  }

  if (opened) {
    for (const auto& child : node.children) {
      render_tree_node(state, child);
    }
    ImGui::TreePop();
  }
}

void load_elf_from_dialog(AppState& state) {
  const auto file_path = open_elf_file_dialog();
  if (!file_path.has_value()) {
    return;
  }
  ProjectLoader loader;
  set_loaded_project(state,
                     loader.dump(file_path.value(),
                                 {.include_runtime_only = true,
                                  .only_static_known = false,
                                  .symbol_name = std::nullopt,
                                  .expand_depth = 8}),
                     LoadedContentKind::ElfProject,
                     file_path.value());
  log_info(state, "已打开 ELF 文件: " + file_path.value());
}

void import_snapshot_from_dialog(AppState& state) {
  const auto file_path = open_snapshot_file_dialog();
  if (!file_path.has_value()) {
    return;
  }
  set_loaded_snapshot(state, parse_snapshot_json(read_all_text(file_path.value())), file_path.value());
  log_info(state, "已导入 JSON 快照: " + file_path.value());
}

void export_snapshot_from_dialog(AppState& state) {
  const auto snapshot = build_snapshot(state);
  if (!snapshot.has_value()) {
    log_error(state, "当前没有可导出的模型");
    return;
  }
  const auto file_path = save_snapshot_file_dialog("snapshot.json");
  if (!file_path.has_value()) {
    return;
  }
  auto value = snapshot.value();
  if (value.exported_at.empty()) {
    value.exported_at = "generated-by-ui";
  }
  write_all_text(file_path.value(), render_snapshot_json(value));
  log_info(state, "已导出 JSON 快照: " + file_path.value());
}

void render_menu_bar(AppState& state) {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }

  if (ImGui::BeginMenu("文件")) {
    if (ImGui::MenuItem("打开 ELF...")) {
      try {
        load_elf_from_dialog(state);
      } catch (const std::exception& error) {
        log_error(state, error.what());
      }
    }
    if (ImGui::MenuItem("导入 JSON...")) {
      try {
        import_snapshot_from_dialog(state);
      } catch (const std::exception& error) {
        log_error(state, error.what());
      }
    }
    if (ImGui::MenuItem("导出 JSON...")) {
      try {
        export_snapshot_from_dialog(state);
      } catch (const std::exception& error) {
        log_error(state, error.what());
      }
    }
    if (ImGui::MenuItem("退出")) {
      state.request_exit = true;
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("视图")) {
    ImGui::MenuItem("显示日志面板", nullptr, &state.show_log_panel);
    ImGui::MenuItem("显示 JSON 预览", nullptr, &state.show_json_preview_panel);
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("工具")) {
    if (ImGui::MenuItem("检查更新")) {
      try {
        check_for_new_version(state);
      } catch (const std::exception& error) {
        log_error(state, error.what());
      }
    }
    if (state.version_check.has_value() && !state.version_check->message.empty()) {
      ImGui::TextDisabled("%s", state.version_check->message.c_str());
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("帮助")) {
    if (ImGui::MenuItem("快捷键说明", "F1")) {
      state.show_shortcuts_dialog = true;
    }
    if (ImGui::MenuItem("关于")) {
      state.show_about_dialog = true;
    }
    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
}

void render_filters(AppState& state) {
  ImGui::TextUnformatted("变量名搜索");
  ImGui::SetNextItemWidth(-1.0F);
  if (state.focus_variable_search) {
    ImGui::SetKeyboardFocusHere();
    state.focus_variable_search = false;
  }
  if (ImGui::InputTextWithHint(kVariableSearchInputId,
                               "匹配变量名或完整路径中的子串",
                               &state.filters.form.variable_name_query)) {
    compile_filter_rules(state.filters);
  }

  ImGui::TextUnformatted("路径规则");
  ImGui::SameLine();
  ImGui::TextDisabled("支持 * / ** 通配；每行一条；! 前缀表示排除");
  // 路径规则输入框需要占满剩余宽度，但标签单独放在上方，避免左侧窄 dock 把右侧 label 挤出可视区。
  if (ImGui::InputTextMultiline("##path_rules_text",
                                &state.filters.form.path_rules_text,
                                ImVec2(ImGui::GetContentRegionAvail().x, 90.0F))) {
    compile_filter_rules(state.filters);
  }
  if (ImGui::Checkbox("包含仅运行时可用项", &state.filters.form.include_runtime_only)) {
    compile_filter_rules(state.filters);
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("仅静态地址可知", &state.filters.form.only_static_known)) {
    compile_filter_rules(state.filters);
  }
  if (ImGui::InputText("地址偏移", &state.address_bias_input)) {
    try {
      state.address_bias = elf_static_view::parse_address_bias(state.address_bias_input);
      state.address_bias_error.reset();
      if (state.persist_address_bias_to_config) {
        save_app_config_or_log(state);
      }
    } catch (const std::exception& error) {
      state.address_bias_error = error.what();
    }
  }
  if (ImGui::Checkbox("将地址偏移写回配置文件", &state.persist_address_bias_to_config)) {
    save_app_config_or_log(state);
    log_info(state,
             state.persist_address_bias_to_config
               ? "已启用地址偏移写回配置文件"
               : "已禁用地址偏移写回配置文件");
  }
  if (ImGui::BeginCombo("复制进制", copy_address_base_label(state.copy_address_base))) {
    for (const CopyAddressBase candidate : kCopyAddressBaseOptions) {
      const bool selected = candidate == state.copy_address_base;
      if (ImGui::Selectable(copy_address_base_label(candidate), selected)) {
        state.copy_address_base = candidate;
        save_app_config_or_log(state);
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  const bool enable_hex_prefix_toggle = state.copy_address_base == CopyAddressBase::Hex;
  if (!enable_hex_prefix_toggle) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Checkbox("复制十六进制时移除 0x 前缀", &state.copy_hex_without_prefix)) {
    save_app_config_or_log(state);
  }
  if (!enable_hex_prefix_toggle) {
    ImGui::EndDisabled();
  }
  ImGui::TextUnformatted(elf_static_view::format_bias_value(state.address_bias).c_str());
  if (state.address_bias_error.has_value()) {
    ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.45F, 1.0F), "%s", state.address_bias_error->c_str());
  }
  if (state.filters.compile_error.has_value()) {
    ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.45F, 1.0F), "%s", state.filters.compile_error->c_str());
  }
}

void render_variables_panel(AppState& state) {
  if (!ImGui::Begin(kVariablesWindowName)) {
    ImGui::End();
    return;
  }

  render_filters(state);
  ImGui::Separator();

  if (!state.project_model.has_value()) {
    ImGui::TextUnformatted("拖拽 ELF 或 JSON 文件到窗口，或者使用“文件”菜单打开。");
    ImGui::End();
    return;
  }

  for (const auto& node : state.project_model->expanded) {
    render_tree_node(state, node);
  }
  ImGui::End();
}

void render_inspector_panel(AppState& state) {
  if (!ImGui::Begin(kInspectorWindowName)) {
    ImGui::End();
    return;
  }
  if (state.selected_node == nullptr) {
    ImGui::TextUnformatted("请选择一个节点。");
    ImGui::End();
    return;
  }

  const auto& node = *state.selected_node;
  ImGui::Text("路径: %s", node.path.c_str());
  ImGui::Text("名称: %s", node.display_name.c_str());
  ImGui::Text("类型: %s", node.type_name.c_str());
  ImGui::Text("类型类别: %s", localized_type_kind(node.type_kind));
  ImGui::Text("可用性: %s", localized_availability(node.availability));
  ImGui::Text("原始地址: %s", elf_static_view::format_address_summary(node, 0).c_str());
  ImGui::Text(
    "偏移后地址: %s", elf_static_view::format_address_summary(node, state.address_bias).c_str());
  if (node.byte_size.has_value()) {
    ImGui::Text("字节大小: %llu", static_cast<unsigned long long>(node.byte_size.value()));
  }
  if (node.array_count.has_value()) {
    ImGui::Text("数组长度: %llu", static_cast<unsigned long long>(node.array_count.value()));
  }
  if (node.array_stride.has_value()) {
    ImGui::Text("数组步长: %llu", static_cast<unsigned long long>(node.array_stride.value()));
  }
  ImGui::End();
}

void render_log_panel(AppState& state) {
  if (!state.show_log_panel) {
    return;
  }
  if (!ImGui::Begin(kLogWindowName, &state.show_log_panel)) {
    ImGui::End();
    return;
  }
  // 日志面板改成只读多行输入框，这样用户可以自由框选、复制任意片段。
  std::string log_text = build_log_panel_text(state);
  if (log_text.empty()) {
    log_text = "暂无日志。";
  }
  ImGui::InputTextMultiline("##log_panel_text",
                            &log_text,
                            ImVec2(-FLT_MIN, -FLT_MIN),
                            ImGuiInputTextFlags_ReadOnly);
  ImGui::End();
}

void render_json_preview_panel(AppState& state) {
  if (!state.show_json_preview_panel) {
    return;
  }
  if (!ImGui::Begin(kJsonPreviewWindowName, &state.show_json_preview_panel)) {
    ImGui::End();
    return;
  }
  std::string preview_text;
  if (const auto json = selected_node_json(state); json.has_value()) {
    preview_text = json.value();
  } else if (state.project_model.has_value()) {
    if (auto snapshot = build_snapshot(state); snapshot.has_value()) {
      preview_text = render_snapshot_json(snapshot.value());
    }
  }

  if (preview_text.empty()) {
    ImGui::TextUnformatted("暂无可预览内容。");
    ImGui::End();
    return;
  }

  ImGui::BeginChild("json_preview_scroll");
  ImGui::TextUnformatted(preview_text.c_str());
  ImGui::EndChild();
  ImGui::End();
}

void render_about_dialog(AppState& state) {
  if (!state.show_about_dialog) {
    return;
  }
  ImGui::OpenPopup(kAboutDialogName);
  if (ImGui::BeginPopupModal(kAboutDialogName, &state.show_about_dialog, ImGuiWindowFlags_AlwaysAutoResize)) {
    const ReleaseMetadata& release_metadata = default_release_metadata();
    const VersionCheckState effective_version_check = resolve_version_check_state(state);

    ImGui::TextUnformatted("ElfStaticView");
    ImGui::Separator();
    ImGui::Text("版本: %s", current_version_string().c_str());
    ImGui::Text("作者: %s", release_metadata.author_name.c_str());
    ImGui::Text("邮箱: %s", release_metadata.author_email.c_str());
    ImGui::TextUnformatted("GLFW + OpenGL3 + Dear ImGui");
    ImGui::TextUnformatted("离线 ELF / DWARF 静态变量浏览器");
    ImGui::TextUnformatted("依赖项：ImGui、GLFW、libdwarf、yaml-cpp");
    ImGui::Separator();
    ImGui::TextUnformatted("仓库：");
    ImGui::SameLine();
    ImGui::TextLinkOpenURL(effective_version_check.repository_url.c_str(),
                           effective_version_check.repository_url.c_str());
    ImGui::TextWrapped("更新来源: %s",
                       effective_version_check.check_uri_uses_default
                         ? "默认 GitHub Releases API"
                         : "已配置的 updates.check_uri");
    ImGui::TextWrapped("更新 URI: %s", effective_version_check.check_uri.c_str());
    if (!effective_version_check.latest_version.empty()) {
      ImGui::Text("最近检测到的版本: %s", effective_version_check.latest_version.c_str());
    }
    if (!effective_version_check.release_name.empty()) {
      ImGui::TextWrapped("最近发布名称: %s", effective_version_check.release_name.c_str());
    }
    if (!effective_version_check.release_url.empty()) {
      ImGui::TextUnformatted("最近发布页面：");
      ImGui::SameLine();
      ImGui::TextLinkOpenURL(effective_version_check.release_url.c_str(),
                             effective_version_check.release_url.c_str());
    }
    if (!effective_version_check.message.empty()) {
      ImGui::Separator();
      ImGui::TextWrapped("%s", effective_version_check.message.c_str());
    }
    if (ImGui::Button("关闭")) {
      state.show_about_dialog = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void render_shortcuts_dialog(AppState& state) {
  if (!state.show_shortcuts_dialog) {
    return;
  }
  ImGui::OpenPopup(kShortcutsDialogName);
  if (ImGui::BeginPopupModal(kShortcutsDialogName,
                             &state.show_shortcuts_dialog,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("快捷键说明");
    ImGui::Separator();
    ImGui::BulletText("Ctrl+F: 聚焦变量名搜索框");
    ImGui::BulletText("Ctrl+C: 复制当前变量地址减去地址偏移后的结果");
    ImGui::BulletText("Ctrl+B: 复制当前变量的原始地址值");
    ImGui::BulletText("F1: 打开快捷键帮助");
    ImGui::BulletText("Esc: 清理当前输入焦点");
    ImGui::Separator();
    ImGui::TextWrapped("版本检查 URI 来自可执行程序同目录的 elf-static-view.yaml，字段为 updates.check_uri。");
    ImGui::TextWrapped("地址偏移配置位于 address_bias.write_back 和 address_bias.value。");
    if (ImGui::Button("关闭")) {
      state.show_shortcuts_dialog = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

}  // namespace

void MainWindow::render(AppState& state) {
  constexpr ImGuiDockNodeFlags kDockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
  const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
  const ImGuiID dockspace_id = ImGui::GetID(kMainDockspaceName);

  handle_global_shortcuts(state);
  render_menu_bar(state);
  ImGui::DockSpaceOverViewport(dockspace_id, main_viewport, kDockspaceFlags);
  setup_default_dock_layout(dockspace_id, main_viewport, kDockspaceFlags);
  render_variables_panel(state);
  render_inspector_panel(state);
  render_log_panel(state);
  render_json_preview_panel(state);
  render_shortcuts_dialog(state);
  render_about_dialog(state);
}

}  // namespace elf_static_view::ui
