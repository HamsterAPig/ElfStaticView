#include "ui/main_window.hpp"

#include "analysis/address_bias.hpp"
#include "analysis/expander.hpp"
#include "elf_static_view/project.hpp"
#include "ui/file_dialogs.hpp"
#include "ui/filter_matcher.hpp"
#include "ui/version_check.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cstdint>
#include <stdexcept>

namespace elf_static_view::ui {

namespace {

constexpr char kMainDockspaceName[] = "MainDockSpace";
constexpr char kVariablesWindowName[] = "变量###Variables";
constexpr char kInspectorWindowName[] = "详情###Inspector";
constexpr char kLogWindowName[] = "日志###Log";
constexpr char kJsonPreviewWindowName[] = "JSON 预览###JSON Preview";
constexpr char kVariableSearchInputId[] = "##variable_name_query";
constexpr char kExportDialogName[] = "导出选项###export_options_dialog";
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
    case TypeKind::MemberPointer:
      return "成员指针";
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
    case TypeKind::Atomic:
      return "原子类型";
    case TypeKind::Unspecified:
      return "未指定类型";
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

std::optional<std::string> format_selected_adjusted_address(const AppState& state) {
  const auto* selected_node = resolve_selected_node(state);
  if (selected_node == nullptr) {
    return std::nullopt;
  }
  return format_adjusted_address_for_copy(*selected_node, state);
}

std::optional<std::string> format_selected_raw_address(const AppState& state) {
  const auto* selected_node = resolve_selected_node(state);
  if (selected_node == nullptr || !selected_node->absolute_address.has_value()) {
    return std::nullopt;
  }
  return format_address_for_copy(selected_node->absolute_address.value(), state);
}

void copy_selected_adjusted_address(AppState& state) {
  const auto address = format_selected_adjusted_address(state);
  if (!address.has_value()) {
    log_error(state, "当前变量没有可复制的偏移后地址");
    return;
  }
  ImGui::SetClipboardText(address->c_str());
  log_info(state, "已复制当前变量偏移后地址: " + address.value());
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
    copy_selected_adjusted_address(state);
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

void render_tree_node(AppState& state, const ExpandedNode& node) {
  if (!is_filter_cache_visible(state, node)) {
    return;
  }

  std::vector<ExpandedNode> lazy_children;
  const bool has_children = node.children_lazy || !node.children.empty();
  const auto flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                     (!has_children ? ImGuiTreeNodeFlags_Leaf : 0) |
                     ((state.selected_node_path == node.path) ? ImGuiTreeNodeFlags_Selected : 0);

  const auto label =
    node.display_name + " [" + node.type_name + "] @ " +
    elf_static_view::format_address_summary(node, state.address_bias);
  // 懒加载子节点会在每帧重建，必须使用稳定路径作为 ImGui ID，避免展开状态抖动。
  const bool opened = ImGui::TreeNodeEx(node.path.c_str(), flags, "%s", label.c_str());
  if (ImGui::IsItemClicked()) {
    state.selected_node = nullptr;
    state.selected_node_path = node.path;
  }
  if (ImGui::BeginPopupContextItem()) {
    state.selected_node = nullptr;
    state.selected_node_path = node.path;
    if (ImGui::MenuItem("复制变量路径")) {
      ImGui::SetClipboardText(node.path.c_str());
    }
    if (node.absolute_address.has_value() && ImGui::MenuItem("复制原始地址")) {
      copy_selected_raw_address(state);
    }
    if (const auto copied_text = format_adjusted_address_for_copy(node, state);
        copied_text.has_value()) {
      if (ImGui::MenuItem("复制偏移后地址")) {
        ImGui::SetClipboardText(copied_text->c_str());
        log_info(state, "已复制当前变量偏移后地址: " + copied_text.value());
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
    const std::vector<ExpandedNode>* children = &node.children;
    if (node.children_lazy) {
      analysis::Expander expander(state.project_model->types,
                                  state.load_policy.expand_depth,
                                  state.load_policy.lazy_expand_children);
      lazy_children = expander.expand_children(node);
      children = &lazy_children;
    }
    for (const auto& child : *children) {
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
  state.pending_open_elf_path = file_path.value();
  log_info(state, "已选择 ELF 文件，准备开始后台分析: " + file_path.value());
}

void import_snapshot_from_dialog(AppState& state) {
  const auto file_path = open_snapshot_file_dialog();
  if (!file_path.has_value()) {
    return;
  }
  state.pending_import_snapshot_path = file_path.value();
  log_info(state, "已选择 JSON 快照，准备后台导入: " + file_path.value());
}

void export_snapshot_from_dialog(AppState& state) {
  if (!state.project_model) {
    log_error(state, "当前没有可导出的模型");
    return;
  }
  const auto suggested_name =
      state.export_dialog.options.format == ExportFormat::BinaryPrivate ? "snapshot.esv" : "snapshot.json";
  const auto file_path = save_export_file_dialog(suggested_name);
  if (!file_path.has_value()) {
    return;
  }
  state.pending_export_snapshot_path = file_path.value();
  state.pending_export_options = state.export_dialog.options;
  log_info(state, "已选择导出路径，准备后台导出: " + file_path.value());
}

void export_raw_dwarf_from_dialog(AppState& state) {
  auto source_path = state.current_file_path;
  if (source_path.empty()) {
    const auto selected_path = open_elf_file_dialog();
    if (!selected_path.has_value()) {
      return;
    }
    source_path = selected_path.value();
  }
  const auto file_path = save_raw_dwarf_file_dialog("raw-dwarf.json");
  if (!file_path.has_value()) {
    return;
  }
  state.pending_export_raw_dwarf_source_path = source_path;
  state.pending_export_raw_dwarf_output_path = file_path.value();
  log_info(state, "已选择原始 DWARF 导出路径，准备后台导出: " + file_path.value());
}

void render_menu_bar(AppState& state) {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }

  if (ImGui::BeginMenu("文件")) {
    const bool loading = state.background_load.status == BackgroundLoadStatus::Loading;
    if (loading) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("打开 ELF...")) {
      try {
        load_elf_from_dialog(state);
      } catch (const std::exception& error) {
        log_error(state, error.what());
      }
    }
    if (loading) {
      ImGui::EndDisabled();
    }
    if (state.import_snapshot_task.status == UiTaskStatus::Running) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("导入 JSON...")) {
      try {
        import_snapshot_from_dialog(state);
      } catch (const std::exception& error) {
        log_error(state, error.what());
      }
    }
    if (state.import_snapshot_task.status == UiTaskStatus::Running) {
      ImGui::EndDisabled();
    }
    if (state.export_snapshot_task.status == UiTaskStatus::Running) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("导出数据...")) {
      state.export_dialog.options.include_sensitive_info = state.export_sensitive_info;
      state.export_dialog.open = true;
    }
    if (state.export_snapshot_task.status == UiTaskStatus::Running) {
      ImGui::EndDisabled();
    }
    if (state.export_raw_dwarf_task.status == UiTaskStatus::Running) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("导出原始 DWARF JSON...")) {
      try {
        export_raw_dwarf_from_dialog(state);
      } catch (const std::exception& error) {
        log_error(state, error.what());
      }
    }
    if (state.export_raw_dwarf_task.status == UiTaskStatus::Running) {
      ImGui::EndDisabled();
    }
    ImGui::Separator();
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
    if (state.version_check_task.status == UiTaskStatus::Running) {
      ImGui::BeginDisabled();
    }
    if (ImGui::MenuItem("检查更新")) {
      try {
        state.pending_version_check = true;
      } catch (const std::exception& error) {
        log_error(state, error.what());
      }
    }
    if (state.version_check_task.status == UiTaskStatus::Running) {
      ImGui::EndDisabled();
    }
    if (state.version_check.has_value() && !state.version_check->message.empty()) {
      ImGui::TextDisabled("%s", state.version_check->message.c_str());
    }
    if (state.version_check_task.status == UiTaskStatus::Running) {
      ImGui::TextDisabled("更新检查进行中: %s", state.version_check_task.detail.c_str());
    } else if (state.version_check_task.status == UiTaskStatus::Failed &&
               !state.version_check_task.message.empty()) {
      ImGui::TextDisabled("更新检查失败: %s", state.version_check_task.message.c_str());
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
    mark_filter_text_changed(state, std::chrono::steady_clock::now());
  }

  ImGui::TextUnformatted("路径规则");
  ImGui::SameLine();
  ImGui::TextDisabled("支持 * / ** 通配；每行一条；! 前缀表示排除");
  // 路径规则输入框需要占满剩余宽度，但标签单独放在上方，避免左侧窄 dock 把右侧 label 挤出可视区。
  if (ImGui::InputTextMultiline("##path_rules_text",
                                &state.filters.form.path_rules_text,
                                ImVec2(ImGui::GetContentRegionAvail().x, 90.0F))) {
    mark_filter_text_changed(state, std::chrono::steady_clock::now());
  }
  if (ImGui::Checkbox("包含仅运行时可用项", &state.filters.form.include_runtime_only)) {
    mark_filter_options_changed(state, std::chrono::steady_clock::now());
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("仅静态地址可知", &state.filters.form.only_static_known)) {
    mark_filter_options_changed(state, std::chrono::steady_clock::now());
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

  if (state.background_load.status == BackgroundLoadStatus::Loading) {
    ImGui::Text("正在后台解析: %s", state.background_load.path.c_str());
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - state.background_load.started_at);
    ImGui::Text("已耗时: %lld 秒", static_cast<long long>(elapsed.count()));
    ImGui::Separator();
  } else if (state.background_load.status == BackgroundLoadStatus::Failed &&
             !state.background_load.error_message.empty()) {
    ImGui::TextWrapped("最近一次加载失败: %s", state.background_load.error_message.c_str());
    ImGui::Separator();
  }
  if (state.import_snapshot_task.status == UiTaskStatus::Running) {
    ImGui::Text("正在后台导入: %s", state.import_snapshot_task.detail.c_str());
    ImGui::Separator();
  } else if (state.import_snapshot_task.status == UiTaskStatus::Failed &&
             !state.import_snapshot_task.message.empty()) {
    ImGui::TextWrapped("最近一次导入失败: %s", state.import_snapshot_task.message.c_str());
    ImGui::Separator();
  }
  if (state.export_snapshot_task.status == UiTaskStatus::Running) {
    ImGui::Text("正在后台导出: %s", state.export_snapshot_task.detail.c_str());
    ImGui::Separator();
  } else if (state.export_snapshot_task.status == UiTaskStatus::Failed &&
             !state.export_snapshot_task.message.empty()) {
    ImGui::TextWrapped("最近一次导出失败: %s", state.export_snapshot_task.message.c_str());
    ImGui::Separator();
  }
  if (state.export_raw_dwarf_task.status == UiTaskStatus::Running) {
    ImGui::Text("正在后台导出原始 DWARF: %s", state.export_raw_dwarf_task.detail.c_str());
    ImGui::Separator();
  } else if (state.export_raw_dwarf_task.status == UiTaskStatus::Failed &&
             !state.export_raw_dwarf_task.message.empty()) {
    ImGui::TextWrapped("最近一次原始 DWARF 导出失败: %s", state.export_raw_dwarf_task.message.c_str());
    ImGui::Separator();
  }

  if (!state.project_model) {
    ImGui::TextUnformatted("拖拽 ELF 或 JSON 文件到窗口，或者使用“文件”菜单打开。");
    ImGui::End();
    return;
  }

  if (should_show_filter_progress(state)) {
    ImGui::TextUnformatted("筛选中...");
    ImGui::End();
    return;
  }
  if (state.filters.build_error.has_value()) {
    ImGui::TextWrapped("筛选失败: %s", state.filters.build_error->c_str());
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
  if (state.project_model) {
    const auto& elf_info = state.project_model->elf_info;
    const auto& metrics = state.project_model->metrics;
    ImGui::TextUnformatted("ELF 信息");
    ImGui::Separator();
    ImGui::Text("Class: %s", elf_info.object_class.c_str());
    ImGui::Text("Endian: %s", elf_info.byte_order.c_str());
    ImGui::Text("Type: %s", elf_info.file_type.c_str());
    ImGui::Text("Machine: %s", elf_info.machine.c_str());
    ImGui::Text("OS ABI: %s", elf_info.os_abi.c_str());
    if (state.load_policy.enable_parse_metrics) {
      ImGui::Separator();
      ImGui::TextUnformatted("解析指标");
      ImGui::Text("DWARF 加载: %llu ms", static_cast<unsigned long long>(metrics.dwarf_load_ms));
      ImGui::Text("符号表补址: %llu ms", static_cast<unsigned long long>(metrics.symbol_table_ms));
      ImGui::Text("变量去重: %llu ms", static_cast<unsigned long long>(metrics.deduplicate_ms));
      ImGui::Text("展开构树: %llu ms", static_cast<unsigned long long>(metrics.expand_ms));
      ImGui::Text("过滤前变量: %llu",
                  static_cast<unsigned long long>(metrics.variable_count_before_filter));
      ImGui::Text("过滤后变量: %llu",
                  static_cast<unsigned long long>(metrics.variable_count_after_filter));
      ImGui::Text("跳过 CU: %llu",
                  static_cast<unsigned long long>(metrics.skipped_compile_unit_count));
    }
    ImGui::Separator();
  }
  const auto* selected_node = resolve_selected_node(state);
  if (selected_node == nullptr) {
    ImGui::TextUnformatted("请选择一个节点。");
    ImGui::End();
    return;
  }

  const auto& node = *selected_node;
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
  if (state.json_preview_task.status == UiTaskStatus::Running) {
    ImGui::TextUnformatted("正在后台刷新 JSON 预览...");
    ImGui::End();
    return;
  }
  if (!state.json_preview_error.empty()) {
    ImGui::TextWrapped("预览构建失败: %s", state.json_preview_error.c_str());
    ImGui::End();
    return;
  }
  if (state.json_preview_text.empty()) {
    ImGui::TextUnformatted("暂无可预览内容。");
    ImGui::End();
    return;
  }

  ImGui::BeginChild("json_preview_scroll");
  ImGui::TextUnformatted(state.json_preview_text.c_str());
  ImGui::EndChild();
  ImGui::End();
}

void render_export_dialog(AppState& state) {
  if (!state.export_dialog.open) {
    return;
  }
  ImGui::OpenPopup(kExportDialogName);
  if (!ImGui::BeginPopupModal(kExportDialogName, &state.export_dialog.open, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }

  auto& options = state.export_dialog.options;
  int format = static_cast<int>(options.format);
  int source = static_cast<int>(options.source);
  int payload_kind = static_cast<int>(options.payload_kind);

  ImGui::TextUnformatted("导出格式");
  ImGui::RadioButton("易读 JSON", &format, static_cast<int>(ExportFormat::JsonPretty));
  ImGui::SameLine();
  ImGui::RadioButton("紧凑 JSON", &format, static_cast<int>(ExportFormat::JsonCompact));
  ImGui::SameLine();
  ImGui::RadioButton("私有二进制", &format, static_cast<int>(ExportFormat::BinaryPrivate));

  ImGui::TextUnformatted("数据来源");
  ImGui::RadioButton("完整模型", &source, static_cast<int>(ExportSource::FullModel));
  ImGui::SameLine();
  ImGui::RadioButton("当前筛选结果", &source, static_cast<int>(ExportSource::CurrentFilteredView));

  ImGui::TextUnformatted("导出内容");
  ImGui::RadioButton("完整快照", &payload_kind, static_cast<int>(ExportPayloadKind::FullSnapshot));
  ImGui::SameLine();
  ImGui::RadioButton("精简变量", &payload_kind, static_cast<int>(ExportPayloadKind::VariableSummary));

  options.format = static_cast<ExportFormat>(format);
  options.source = static_cast<ExportSource>(source);
  options.payload_kind = static_cast<ExportPayloadKind>(payload_kind);

  if (ImGui::Checkbox("包含敏感信息", &options.include_sensitive_info)) {
    state.export_sensitive_info = options.include_sensitive_info;
    state.json_preview_dirty = true;
  }
  ImGui::TextWrapped("敏感信息包含源文件路径、编译单元路径和编译器指纹；变量名、类型名、地址仍会导出。");

  ImGui::Separator();
  if (ImGui::Button("导出")) {
    state.export_sensitive_info = options.include_sensitive_info;
    try {
      export_snapshot_from_dialog(state);
      state.export_dialog.open = false;
      ImGui::CloseCurrentPopup();
    } catch (const std::exception& error) {
      log_error(state, error.what());
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("取消")) {
    state.export_dialog.open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
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
    ImGui::BulletText("Ctrl+C: 复制当前变量的偏移后地址");
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

bool MainWindow::render(AppState& state) {
  constexpr ImGuiDockNodeFlags kDockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
  const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
  const ImGuiID dockspace_id = ImGui::GetID(kMainDockspaceName);

  handle_global_shortcuts(state);
  const std::string previous_selected_path = state.selected_node_path;
  const bool previous_show_preview = state.show_json_preview_panel;
  const bool previous_export_sensitive = state.export_sensitive_info;
  const std::string previous_file_path = state.current_file_path;
  const std::string previous_snapshot_path = state.current_snapshot_path;
  const LoadedContentKind previous_loaded_kind = state.loaded_kind;
  render_menu_bar(state);
  ImGui::DockSpaceOverViewport(dockspace_id, main_viewport, kDockspaceFlags);
  setup_default_dock_layout(dockspace_id, main_viewport, kDockspaceFlags);
  render_variables_panel(state);
  render_inspector_panel(state);
  render_log_panel(state);
  render_json_preview_panel(state);
  render_export_dialog(state);
  render_shortcuts_dialog(state);
  render_about_dialog(state);

  const bool preview_related_changed = state.selected_node_path != previous_selected_path ||
                                       state.show_json_preview_panel != previous_show_preview ||
                                       state.export_sensitive_info != previous_export_sensitive ||
                                       state.current_file_path != previous_file_path ||
                                       state.current_snapshot_path != previous_snapshot_path ||
                                       state.loaded_kind != previous_loaded_kind;
  if (preview_related_changed) {
    state.json_preview_dirty = true;
  }
  return preview_related_changed;
}

}  // namespace elf_static_view::ui
