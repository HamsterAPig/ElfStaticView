#include "elf_static_view/project.hpp"
#include "ui/app_state.hpp"
#include "ui/version_check.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void expect_true(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path create_temp_directory()
{
    const auto root = std::filesystem::temp_directory_path() / "elf_static_view_perf_tests";
    std::filesystem::create_directories(root);
    const auto unique_name =
        "case-" + std::to_string(std::filesystem::file_time_type::clock::now().time_since_epoch().count());
    const auto path = root / unique_name;
    std::filesystem::create_directories(path);
    return path;
}

void verify_default_load_policy()
{
    const auto policy = elf_static_view::ui::default_load_policy();
    expect_true(policy.static_storage_only, "默认策略应启用静态存储优先");
    expect_true(policy.exclude_formal_parameters, "默认策略应跳过形参");
    expect_true(policy.exclude_runtime_only_variables, "默认策略应跳过 runtime-only 变量");
    expect_true(policy.expand_depth == 6, "默认展开深度应为 6");
    expect_true(policy.lazy_expand_children, "默认策略应启用 lazy expand");
    expect_true(policy.enable_parse_metrics, "默认策略应启用解析指标");
    expect_true(policy.compile_unit_path_rules_text.find("**/Drivers/**") != std::string::npos,
                "默认策略应包含通用库目录过滤规则");
}

void verify_invalid_compile_unit_rules_fallback()
{
    const auto temp_dir = create_temp_directory();
    const auto executable_path = temp_dir / "elf-static-view.exe";
    const auto config_path = temp_dir / "elf-static-view.yaml";
    {
        std::ofstream output(config_path, std::ios::binary);
        output << "load_policy:\n";
        output << "  compile_unit_path_rules: |\n";
        output << "    !\n";
    }

    elf_static_view::ui::AppState state;
    elf_static_view::ui::load_app_config(state, executable_path);

    const auto expected = elf_static_view::ui::default_load_policy().compile_unit_path_rules_text;
    expect_true(state.load_policy.compile_unit_path_rules_text == expected, "非法编译单元规则应回退到默认规则");

    bool found_error = false;
    for (const auto& message : state.log_messages) {
        if (message.find("load_policy.compile_unit_path_rules 无效") != std::string::npos) {
            found_error = true;
            break;
        }
    }
    expect_true(found_error, "非法编译单元规则应输出明确错误日志");
}

void verify_cli_load_policy_uses_yaml_defaults()
{
    const auto temp_dir = create_temp_directory();
    const auto executable_path = temp_dir / "elf-static-view.exe";
    const auto config_path = temp_dir / "elf-static-view.yaml";
    {
        std::ofstream output(config_path, std::ios::binary);
        output << "load_policy:\n";
        output << "  default_static_storage_only: false\n";
        output << "  exclude_formal_parameters: false\n";
        output << "  exclude_runtime_only_variables: false\n";
        output << "  max_expand_depth: 3\n";
        output << "  lazy_expand_children: false\n";
        output << "  enable_parse_metrics: false\n";
        output << "  compile_unit_path_rules: |\n";
        output << "    **/Vendor/**\n";
        output << "    !**/App/**\n";
    }

    const auto policy = elf_static_view::ui::load_cli_load_policy(executable_path);
    expect_true(!policy.static_storage_only, "CLI 应读取 YAML 中的 default_static_storage_only");
    expect_true(!policy.exclude_formal_parameters, "CLI 应读取 YAML 中的 exclude_formal_parameters");
    expect_true(!policy.exclude_runtime_only_variables, "CLI 应读取 YAML 中的 exclude_runtime_only_variables");
    expect_true(policy.expand_depth == 3, "CLI 应读取 YAML 中的 max_expand_depth");
    expect_true(!policy.lazy_expand_children, "CLI 应读取 YAML 中的 lazy_expand_children");
    expect_true(!policy.enable_parse_metrics, "CLI 应读取 YAML 中的 enable_parse_metrics");
    expect_true(policy.compile_unit_path_rules_text.find("**/Vendor/**") != std::string::npos,
                "CLI 应读取 YAML 中的 compile_unit_path_rules");
}

void verify_resolve_selected_node_by_path()
{
    elf_static_view::ProjectModel model;
    elf_static_view::ExpandedNode child;
    child.path = "demo::value.inner";
    child.display_name = "inner";

    elf_static_view::ExpandedNode root;
    root.path = "demo::value";
    root.display_name = "value";
    root.children.push_back(child);
    model.expanded.push_back(root);

    elf_static_view::ui::AppState state;
    state.project_model = model;
    state.selected_node_path = "demo::value.inner";

    const auto* resolved = elf_static_view::ui::resolve_selected_node(state);
    expect_true(resolved != nullptr, "应能按稳定路径重新定位选中节点");
    expect_true(resolved->path == "demo::value.inner", "定位到的节点路径应与选中路径一致");
}

void verify_json_round_trip_preserves_metrics_and_lazy_nodes()
{
    elf_static_view::ProjectModel model;
    model.file = "demo.elf";
    model.metrics.dwarf_load_ms = 11;
    model.metrics.symbol_table_ms = 12;
    model.metrics.deduplicate_ms = 13;
    model.metrics.expand_ms = 14;
    model.metrics.variable_count_before_filter = 15;
    model.metrics.variable_count_after_filter = 16;
    model.metrics.skipped_compile_unit_count = 17;

    elf_static_view::ExpandedNode node;
    node.path = "demo::array";
    node.display_name = "array";
    node.type_id = "type@42";
    node.depth = 1;
    node.children_lazy = true;
    model.expanded.push_back(node);

    const std::string json_text = elf_static_view::render_dump_json(model);
    const auto parsed = elf_static_view::parse_dump_json(json_text);

    expect_true(parsed.metrics.dwarf_load_ms == model.metrics.dwarf_load_ms, "JSON 往返后应保留解析耗时");
    expect_true(parsed.metrics.skipped_compile_unit_count == model.metrics.skipped_compile_unit_count,
                "JSON 往返后应保留跳过编译单元计数");
    expect_true(parsed.expanded.size() == 1, "JSON 往返后应保留展开节点");
    expect_true(parsed.expanded.front().type_id == "type@42", "JSON 往返后应保留 type_id");
    expect_true(parsed.expanded.front().children_lazy, "JSON 往返后应保留 lazy 标记");
}

} // namespace

int main()
{
    try {
        verify_default_load_policy();
        verify_invalid_compile_unit_rules_fallback();
        verify_cli_load_policy_uses_yaml_defaults();
        verify_resolve_selected_node_by_path();
        verify_json_round_trip_preserves_metrics_and_lazy_nodes();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "perf policy tests failed: %s\n", error.what());
        return 1;
    }
}
