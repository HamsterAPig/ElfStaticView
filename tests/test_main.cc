#include "analysis/address_bias.hpp"

#include "elf/dwarf_wrappers.hpp"
#include "elf/elf_symbol_table.hpp"
#include "elf/ti_coff_object.hpp"

#include "elf_static_view/project.hpp"
#include "platform/utf8.hpp"
#include "ui/app_state.hpp"
#include "ui/filter_matcher.hpp"
#include "ui/version_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace {

#ifndef ELF_STATIC_VIEW_DWARFDUMP_PATH
#error "ELF_STATIC_VIEW_DWARFDUMP_PATH 未定义"
#endif

void expect_true(bool condition, const std::string& message);

std::string resolve_dwarfdump_path()
{
    std::filesystem::path configured = ELF_STATIC_VIEW_DWARFDUMP_PATH;
    if (std::filesystem::exists(configured)) {
        return configured.string();
    }

    std::vector<std::filesystem::path> search_roots;
    for (std::filesystem::path cursor = std::filesystem::current_path(); !cursor.empty();
         cursor = cursor.parent_path()) {
        search_roots.push_back(cursor);
        if (cursor == cursor.parent_path()) {
            break;
        }
    }

    for (const auto& root : search_roots) {
        const auto direct_candidate =
            root / "3rdparty" / "libdwarf-code" / "src" / "bin" / "dwarfdump" / "Release" / "dwarfdump.exe";
        if (std::filesystem::exists(direct_candidate)) {
            return direct_candidate.string();
        }

        const auto nested_candidate = root / "build-vs" / "3rdparty" / "libdwarf-code" / "src" / "bin" / "dwarfdump" /
                                      "Release" / "dwarfdump.exe";
        if (std::filesystem::exists(nested_candidate)) {
            return nested_candidate.string();
        }
    }

    if (configured.filename() == "dwarfdump.exe") {
        configured = configured.parent_path().parent_path() / "Release" / "dwarfdump.exe";
        if (std::filesystem::exists(configured)) {
            return configured.string();
        }
    }

    return configured.string();
}

std::string describe_last_error(const DWORD error_code)
{
    std::wstring message(512, L'\0');
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr,
                                        error_code,
                                        0,
                                        message.data(),
                                        static_cast<DWORD>(message.size()),
                                        nullptr);
    if (length == 0) {
        return "Windows 错误码 " + std::to_string(error_code);
    }

    message.resize(length);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return elf_static_view::platform::wide_to_utf8(message);
}

// 统一走构建产物中的 dwarfdump，避免依赖开发机本地安装路径。
std::string run_dwarfdump_to_temp(const std::string& dwarfdump_args,
                                  const std::string& fixture_path,
                                  const std::string& output_tag,
                                  const std::string& label)
{
    const auto output_path =
        std::filesystem::temp_directory_path() /
        ("elf-static-view-" + output_tag + "-" +
         std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())) + ".txt");
    const std::string dwarfdump_path = resolve_dwarfdump_path();
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    const auto output_handle = CreateFileW(elf_static_view::platform::utf8_path(output_path.string()).c_str(),
                                           GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           &security_attributes,
                                           CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL,
                                           nullptr);
    expect_true(output_handle != INVALID_HANDLE_VALUE,
                label + " 无法创建 dwarfdump 输出文件: " + output_path.string() +
                    "，原因: " + describe_last_error(GetLastError()));

    // 直接用 CreateProcessW 重定向标准输出，避免依赖 cmd.exe 的重定向和当前 shell 环境。
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = output_handle;
    startup_info.hStdError = output_handle;

    const std::string command_utf8 =
        std::string("\"") + dwarfdump_path + "\" " + dwarfdump_args + " \"" + fixture_path + "\"";
    std::wstring command_line = elf_static_view::platform::utf8_to_wide(command_utf8);
    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    PROCESS_INFORMATION process_info{};
    const BOOL started = CreateProcessW(nullptr,
                                        mutable_command_line.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &startup_info,
                                        &process_info);
    const DWORD start_error = GetLastError();
    CloseHandle(output_handle);
    expect_true(started != FALSE,
                label + " 的 dwarfdump 启动失败: " + dwarfdump_path + "，原因: " + describe_last_error(start_error));

    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 0;
    const BOOL got_exit_code = GetExitCodeProcess(process_info.hProcess, &exit_code);
    const DWORD exit_error = GetLastError();
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    expect_true(got_exit_code != FALSE, label + " 无法获取 dwarfdump 退出码，原因: " + describe_last_error(exit_error));
    expect_true(exit_code == 0,
                label + " 的 dwarfdump 应执行成功，实际退出码: " + std::to_string(exit_code) +
                    "，输出文件: " + output_path.string());
    return output_path.string();
}

std::string read_all(const std::string& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("无法打开文件: " + path);
    }
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

std::string normalize_path_separators(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

void expect_true(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error("断言失败: " + message);
    }
}

void expect_contains(const std::string& content, const std::string& needle, const std::string& message)
{
    if (content.find(needle) == std::string::npos) {
        throw std::runtime_error("断言失败: " + message + "，缺少片段: " + needle);
    }
}

[[nodiscard]] bool lazy_path_covers(const elf_static_view::ExpandedNode& node, const std::string& expected_path)
{
    if (node.children_lazy && expected_path.starts_with(node.path) && expected_path.size() > node.path.size()) {
        const char separator = expected_path[node.path.size()];
        if (separator == '.' || separator == '[') {
            return true;
        }
    }
    for (const auto& child : node.children) {
        if (lazy_path_covers(child, expected_path)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool expanded_path_exists_or_is_lazy(const elf_static_view::ProjectModel& model,
                                                   const std::string& expected_path)
{
    for (const auto& node : model.expanded) {
        if (node.path == expected_path || lazy_path_covers(node, expected_path)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] const elf_static_view::ExpandedNode* find_expanded_path(
    const std::vector<elf_static_view::ExpandedNode>& nodes, const std::string& expected_path)
{
    for (const auto& node : nodes) {
        if (node.path == expected_path) {
            return &node;
        }
        if (const auto* child = find_expanded_path(node.children, expected_path); child != nullptr) {
            return child;
        }
    }
    return nullptr;
}

[[nodiscard]] bool expanded_path_exists(const elf_static_view::ProjectModel& model, const std::string& expected_path)
{
    return find_expanded_path(model.expanded, expected_path) != nullptr;
}

[[nodiscard]] bool static_address_result_exists(const std::vector<elf_static_view::StaticAddressResult>& results,
                                                const std::string& expected_key)
{
    return std::any_of(results.begin(), results.end(), [&](const auto& result) { return result.key == expected_key; });
}

[[nodiscard]] bool contains_lazy_node(const std::vector<elf_static_view::ExpandedNode>& nodes)
{
    for (const auto& node : nodes) {
        if (node.children_lazy || contains_lazy_node(node.children)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<std::string> parse_expected_path_line(const std::string& line)
{
    constexpr std::string_view prefix = "\"path\": \"";
    if (!line.starts_with(prefix)) {
        return std::nullopt;
    }
    const auto end = line.find('"', prefix.size());
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return line.substr(prefix.size(), end - prefix.size());
}

std::vector<const elf_static_view::VariableRecord*> find_file_static_variables(
    const elf_static_view::ProjectModel& model, const std::string& name)
{
    std::vector<const elf_static_view::VariableRecord*> matches;
    for (const auto& symbol : model.symbols) {
        if (symbol.name == name && symbol.variable_kind == elf_static_view::VariableKind::FileStatic) {
            matches.push_back(&symbol);
        }
    }
    return matches;
}

std::vector<const elf_static_view::VariableRecord*> find_variables_by_kind(const elf_static_view::ProjectModel& model,
                                                                           const std::string& name,
                                                                           const elf_static_view::VariableKind kind)
{
    std::vector<const elf_static_view::VariableRecord*> matches;
    for (const auto& symbol : model.symbols) {
        if (symbol.name == name && symbol.variable_kind == kind) {
            matches.push_back(&symbol);
        }
    }
    return matches;
}

std::vector<const elf_static_view::VariableRecord*> find_variables_by_name(const elf_static_view::ProjectModel& model,
                                                                           const std::string& name)
{
    std::vector<const elf_static_view::VariableRecord*> matches;
    for (const auto& symbol : model.symbols) {
        if (symbol.name == name) {
            matches.push_back(&symbol);
        }
    }
    return matches;
}

void verify_text_presence_via_dwarfdump(const std::string& fixture_path,
                                        const std::string& dwarfdump_kind,
                                        const std::string& expected_text,
                                        const std::string& label);
void verify_abbrev_attribute_form_via_dwarfdump(const std::string& fixture_path,
                                                const std::string& attribute_name,
                                                const std::string& form_name,
                                                const std::string& label);
void verify_fixture_contains_compile_unit_ranges(const std::string& fixture_path,
                                                 std::size_t min_range_count,
                                                 const std::string& label);

const elf_static_view::TypeNode* find_type_by_name(const elf_static_view::ProjectModel& model, const std::string& name)
{
    for (const auto& type : model.types) {
        if (type.name == name) {
            return &type;
        }
    }
    return nullptr;
}

void verify_fixture(const std::string& fixture_path, const std::string& expected_json_path)
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        fixture_path,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto output = elf_static_view::render_dump_json(model);
    const auto expected = read_all(expected_json_path);
    std::istringstream lines(expected);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        if (const auto expected_path = parse_expected_path_line(line); expected_path.has_value()) {
            expect_true(expanded_path_exists_or_is_lazy(model, expected_path.value()),
                        expected_json_path + "，缺少路径或 lazy 父节点: " + expected_path.value());
            continue;
        }
        expect_contains(output, line, expected_json_path);
    }
}

void verify_json_round_trip(const std::string& fixture_path)
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        fixture_path,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto json = elf_static_view::render_dump_json(model);
    const auto parsed = elf_static_view::parse_dump_json(json);
    expect_true(parsed.file == model.file, "JSON 往返后文件路径应保持一致");
    expect_true(parsed.expanded.size() == model.expanded.size(), "JSON 往返后展开节点数量应保持一致");
    expect_true(parsed.metrics.dwarf_load_ms == model.metrics.dwarf_load_ms, "JSON 往返后应保留解析指标");
    expect_true(!parsed.expanded.empty() && parsed.expanded.front().type_id == model.expanded.front().type_id,
                "JSON 往返后应保留展开节点 type_id");
}

void verify_raw_dwarf_json_export(const std::string& fixture_path)
{
    elf_static_view::ProjectLoader loader;
    const auto raw_json = loader.dump_raw_dwarf_json(fixture_path);
    const auto parsed = YAML::Load(raw_json);
    expect_true(parsed["schema_version"].as<int>() == 1, "raw DWARF JSON 应包含 schema_version");
    expect_true(parsed["source_file"].as<std::string>() == fixture_path, "raw DWARF JSON 应保留源文件路径");
    const auto status = parsed["status"].as<std::string>();
    expect_true(status == "ok" || status == "partial", "raw DWARF JSON status 应为 ok 或 partial");
    expect_true(parsed["compile_units"].IsSequence() && parsed["compile_units"].size() > 0,
                "raw DWARF JSON 应包含 compile_units");
    const auto root = parsed["compile_units"][0]["root"];
    expect_true(root["tag"].as<std::string>().find("DW_TAG_compile_unit") != std::string::npos,
                "raw DWARF JSON 根 DIE 应是 compile_unit");
    const auto attributes = root["attributes"];
    expect_true(attributes.IsSequence() && attributes.size() > 0, "raw DWARF JSON 根 DIE 应包含 attributes");
    bool has_readable_attribute = false;
    for (const auto& attribute : attributes) {
        const auto name = attribute["name"].as<std::string>();
        if (name == "DW_AT_name" || name == "DW_AT_producer") {
            has_readable_attribute = true;
            expect_true(attribute["form"].as<std::string>().find("DW_FORM_") != std::string::npos,
                        "raw DWARF JSON attribute 应包含 DW_FORM 名称");
            expect_true(attribute["value"].IsDefined(), "raw DWARF JSON attribute 应包含 value");
        }
    }
    expect_true(has_readable_attribute, "raw DWARF JSON 应导出可读 attribute name/form/value");
}

void verify_dump_text_contains_elf_info(const std::string& fixture_path)
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        fixture_path,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "elf_class: ELF64", "dump 文本应包含 ELF class");
    expect_contains(output, "byte_order: LittleEndian", "dump 文本应包含大小端");
    expect_contains(output, "file_type: DYN", "dump 文本应包含文件类型");
    expect_contains(output, "machine: x86_64", "dump 文本应包含 machine");
    expect_contains(output, "os_abi: SystemV", "dump 文本应包含 OS ABI");
}

void verify_dump_text_contains_elf_info_any_class(const std::string& fixture_path,
                                                  const std::string& expected_class,
                                                  const std::string& expected_byte_order)
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        fixture_path,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "elf_class: " + expected_class, "dump 文本应包含目标 ELF class");
    expect_contains(output, "byte_order: " + expected_byte_order, "dump 文本应包含目标大小端");
    expect_contains(output, "file_type: ", "dump 文本应包含文件类型");
    expect_contains(output, "machine: ", "dump 文本应包含 machine");
    expect_contains(output, "os_abi: ", "dump 文本应包含 OS ABI");
}

void verify_bitfield_layout_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_C_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto* bitfield_type = find_type_by_name(model, "BitFieldStruct");
    expect_true(bitfield_type != nullptr, "C fixture 应包含 BitFieldStruct");
    expect_true(bitfield_type->members.size() == 3, "BitFieldStruct 应保留 3 个 bitfield 成员");

    const auto& enabled = bitfield_type->members[0];
    expect_true(enabled.name == "enabled", "第一个 bitfield 成员应为 enabled");
    expect_true(enabled.address.kind == elf_static_view::AddressKind::BitField, "enabled 应标记为 BitField");
    expect_true(enabled.address.bit_size.has_value() && enabled.address.bit_size.value() == 1,
                "enabled bit_size 应为 1");

    const auto& mode = bitfield_type->members[1];
    expect_true(mode.name == "mode", "第二个 bitfield 成员应为 mode");
    expect_true(mode.address.kind == elf_static_view::AddressKind::BitField, "mode 应标记为 BitField");
    expect_true(mode.address.bit_size.has_value() && mode.address.bit_size.value() == 3, "mode bit_size 应为 3");

    const auto& reserved = bitfield_type->members[2];
    expect_true(reserved.name == "reserved", "第三个 bitfield 成员应为 reserved");
    expect_true(reserved.address.kind == elf_static_view::AddressKind::BitField, "reserved 应标记为 BitField");
    expect_true(reserved.address.bit_size.has_value() && reserved.address.bit_size.value() == 4,
                "reserved bit_size 应为 4");

    const auto globals = find_variables_by_name(model, "global_value");
    expect_true(globals.size() == 1, "C fixture 应保留 global_value");
    expect_true(globals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
                "global_value 应解析成静态地址已知");
    expect_true(globals.front()->address.absolute_address.has_value(), "global_value 应解析出绝对地址");
    expect_true(globals.front()->address.location_description == "DW_OP_addrx" ||
                    globals.front()->address.location_description == "DW_OP_addr",
                "global_value 应保留静态地址位置描述");
}

void verify_member_pointer_type_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_CPP_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto* member_pointer_type = find_type_by_name(model, "MemberPointer");
    expect_true(member_pointer_type != nullptr, "C++ fixture 应包含 MemberPointer");
    expect_true(member_pointer_type->kind == elf_static_view::TypeKind::Typedef,
                "MemberPointer typedef 应保留 Typedef kind");
    expect_true(member_pointer_type->aliased_of.has_value(), "MemberPointer 应保留别名目标");

    const auto aliased_id = member_pointer_type->aliased_of->id;
    const auto member_pointer_iter =
        std::find_if(model.types.begin(), model.types.end(), [&](const elf_static_view::TypeNode& type) {
            return type.id == aliased_id;
        });
    expect_true(member_pointer_iter != model.types.end(), "MemberPointer 别名目标类型应存在");
    expect_true(member_pointer_iter->kind == elf_static_view::TypeKind::MemberPointer,
                "MemberPointer 别名目标应识别为 MemberPointer");

    const auto globals = find_variables_by_name(model, "global_item_member_ptr");
    expect_true(globals.size() == 1, "应解析出 global_item_member_ptr");
}

void verify_gnu_addr_index_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_GNU_ADDR_INDEX_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto gnu_addr_index_iter =
        std::find_if(model.symbols.begin(), model.symbols.end(), [](const elf_static_view::VariableRecord& variable) {
            return variable.address.location_description == "DW_OP_GNU_addr_index";
        });
    expect_true(gnu_addr_index_iter != model.symbols.end(),
                "GNU addr index fixture 应至少包含一个 DW_OP_GNU_addr_index 变量");
    expect_true(gnu_addr_index_iter->availability == elf_static_view::Availability::StaticAddressKnown,
                "GNU addr index 变量应解析成静态地址已知");
    expect_true(gnu_addr_index_iter->address.absolute_address.has_value(), "GNU addr index 变量应解析出绝对地址");
}

void verify_ref_sig8_debug_types_fixture()
{
    verify_abbrev_attribute_form_via_dwarfdump(
        ELF_STATIC_VIEW_REF_SIG8_DEBUG_TYPES_FIXTURE_PATH, "DW_AT_type", "DW_FORM_ref_sig8", "ref_sig8 fixture");
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_REF_SIG8_DEBUG_TYPES_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto globals = find_variables_by_name(model, "global_value");
    expect_true(globals.size() == 1, "ref_sig8 fixture 应保留 global_value");
    expect_true(globals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
                "ref_sig8 fixture 的 global_value 应解析成静态地址已知");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(
        output, "global_value [StaticAddressKnown] RefTarget", "ref_sig8 fixture 应把 global_value 解析回 RefTarget");
    expect_contains(output, "consume::local [RuntimeOnly] RefTarget", "ref_sig8 fixture 应把 local 也解析回 RefTarget");
    expect_contains(output, "global_value.flags [StaticLayoutKnown]", "ref_sig8 fixture 应展开成员数组 flags");
    expect_contains(output, "global_value.flags[0] [StaticLayoutKnown]", "ref_sig8 fixture 应展开 flags 首个元素");
    expect_contains(output,
                    "global_value.flags[0].all [StaticLayoutKnown] unsigned int",
                    "ref_sig8 fixture 应展开 union 内 all 成员的本地 base_type");
    expect_contains(output,
                    "global_value.flags[0].bits [StaticLayoutKnown] FlagBits",
                    "ref_sig8 fixture 应展开 union 内 bits 成员的签名类型");
    expect_contains(output,
                    "global_value.flags[0].bits.enabled [StaticLayoutKnown]",
                    "ref_sig8 fixture 应展开 bits 内 bitfield 成员");
}

void verify_ref_sig8_indirect_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_REF_SIG8_DEBUG_TYPES_INDIRECT_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(
        output, "global_value [StaticAddressKnown] RefTarget", "indirect debug_types fixture 应保留 RefTarget 名称");
    expect_contains(
        output, "consume::local [RuntimeOnly] RefTarget", "indirect debug_types fixture 应保留 local 的 RefTarget");
    expect_contains(
        output, "global_value.left [StaticLayoutKnown] int", "indirect debug_types fixture 应保留 left 的 int 类型");
    expect_contains(
        output, "global_value.right [StaticLayoutKnown] int", "indirect debug_types fixture 应保留 right 的 int 类型");
    expect_contains(output, "global_value.flags [StaticLayoutKnown]", "indirect debug_types fixture 应展开成员数组 flags");
    expect_contains(
        output, "global_value.flags[0] [StaticLayoutKnown]", "indirect debug_types fixture 应展开 flags 首个元素");
    expect_contains(output,
                    "global_value.flags[0].all [StaticLayoutKnown] unsigned int",
                    "indirect debug_types fixture 应展开 union 内 all 成员的本地 base_type");
    expect_contains(output,
                    "global_value.flags[0].bits [StaticLayoutKnown] FlagBits",
                    "indirect debug_types fixture 应展开 union 内 bits 成员的签名类型");
    expect_contains(output,
                    "global_value.flags[0].bits.enabled [StaticLayoutKnown]",
                    "indirect debug_types fixture 应展开 bits 内 bitfield 成员");
}

void verify_gcc_ref_sup4_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_GCC_DWARF5_REF_SUP4_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto output = elf_static_view::render_dump_text(model);
    // ref_sup4 只替换类型引用形式，变量位置表达式保持原样，因此 sup_value 仍应保留静态地址。
    expect_contains(
        output, "sup_value [StaticAddressKnown] SupTarget", "ref_sup4 fixture 应保留 sup_value 的 SupTarget 类型");
    expect_contains(output, "sup_value.value [StaticLayoutKnown] int", "ref_sup4 fixture 应继续解析成员 int 类型");
}

void verify_gcc_ref_addr_fixture()
{
    verify_abbrev_attribute_form_via_dwarfdump(
        ELF_STATIC_VIEW_GCC_DWARF5_REF_ADDR_FIXTURE_PATH, "DW_AT_type", "DW_FORM_ref_addr", "ref_addr fixture");
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_GCC_DWARF5_REF_ADDR_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(
        output, "sup_value [StaticAddressKnown] SupTarget", "ref_addr fixture 应保留 sup_value 的 SupTarget 类型");
    expect_contains(output, "sup_value.value [StaticLayoutKnown] int", "ref_addr fixture 应继续解析成员 int 类型");
}

void verify_gcc_small_ref_fixture(const std::string& fixture_path, const std::string& form_name)
{
    verify_abbrev_attribute_form_via_dwarfdump(
        fixture_path, "DW_AT_type", "DW_FORM_" + form_name, form_name + " fixture");
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        fixture_path,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(
        output, "sup_value [StaticAddressKnown] SupTarget", form_name + " fixture 应保留 sup_value 的 SupTarget 类型");
    expect_contains(output, "sup_value.value [StaticLayoutKnown] int", form_name + " fixture 应继续解析成员 int 类型");
}

void verify_gcc_ref_sup8_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_GCC_DWARF64_REF_SUP8_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(
        output, "sup_value [StaticAddressKnown] SupTarget", "ref_sup8 fixture 应保留 sup_value 的 SupTarget 类型");
    expect_contains(output, "sup_value.value [StaticLayoutKnown] int", "ref_sup8 fixture 应继续解析成员 int 类型");
}

void verify_gcc_ref8_fixture()
{
    verify_abbrev_attribute_form_via_dwarfdump(
        ELF_STATIC_VIEW_GCC_DWARF64_REF8_FIXTURE_PATH, "DW_AT_type", "DW_FORM_ref8", "ref8 fixture");

    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_GCC_DWARF64_REF8_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(
        output, "sup_value [StaticAddressKnown] SupTarget", "ref8 fixture 应保留 sup_value 的 SupTarget 类型");
    expect_contains(output, "sup_value.value [StaticLayoutKnown] int", "ref8 fixture 应继续解析成员 int 类型");
}

void verify_gcc_gnu_alt_fixture()
{
    elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_GCC_GNU_ALT_FIXTURE_PATH);
    Dwarf_Debug debug = debug_handle.get();

    Dwarf_Unsigned cu_header_length = 0;
    Dwarf_Half version_stamp = 0;
    Dwarf_Off abbrev_offset = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Half length_size = 0;
    Dwarf_Half extension_size = 0;
    Dwarf_Sig8 type_signature{};
    Dwarf_Unsigned type_offset = 0;
    Dwarf_Unsigned next_cu_header_offset = 0;
    Dwarf_Half header_cu_type = 0;
    Dwarf_Error error = nullptr;

    expect_true(dwarf_next_cu_header_d(debug,
                                       true,
                                       &cu_header_length,
                                       &version_stamp,
                                       &abbrev_offset,
                                       &address_size,
                                       &length_size,
                                       &extension_size,
                                       &type_signature,
                                       &type_offset,
                                       &next_cu_header_offset,
                                       &header_cu_type,
                                       &error) == DW_DLV_OK,
                "第一个 gcc gnu alt fixture CU 应可读取");
    expect_true(dwarf_next_cu_header_d(debug,
                                       true,
                                       &cu_header_length,
                                       &version_stamp,
                                       &abbrev_offset,
                                       &address_size,
                                       &length_size,
                                       &extension_size,
                                       &type_signature,
                                       &type_offset,
                                       &next_cu_header_offset,
                                       &header_cu_type,
                                       &error) == DW_DLV_OK,
                "第二个 gcc gnu alt fixture CU 应可读取");

    Dwarf_Die cu_die = nullptr;
    expect_true(dwarf_siblingof_b(debug, nullptr, true, &cu_die, &error) == DW_DLV_OK,
                "应能拿到当前 gcc gnu alt fixture 的第二个 CU DIE");

    auto name_attr = elf_static_view::elf::attribute_of(debug, cu_die, DW_AT_name);
    expect_true(name_attr.has_value(), "第二个 GNU alt CU 应存在 DW_AT_name");
    const auto name = elf_static_view::elf::string_attr(name_attr->get());
    expect_true(name.has_value(), "DW_FORM_GNU_strp_alt 的 name 应可被读取");
    expect_true(name->find("cygwin.S") != std::string::npos, "DW_FORM_GNU_strp_alt 的 name 应回到 alternate string");

    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_GCC_GNU_ALT_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(
        output, "sup_value [StaticAddressKnown] SupTarget", "GNU_ref_alt fixture 应保留 sup_value 的 SupTarget 类型");
    expect_contains(output, "sup_value.value [StaticLayoutKnown] int", "GNU_ref_alt fixture 应继续解析成员 int 类型");
}

void verify_gcc_line_strp_fixture()
{
    elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_GCC_DWARF5_STRP_FIXTURE_PATH);
    Dwarf_Debug debug = debug_handle.get();

    Dwarf_Unsigned cu_header_length = 0;
    Dwarf_Half version_stamp = 0;
    Dwarf_Off abbrev_offset = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Half length_size = 0;
    Dwarf_Half extension_size = 0;
    Dwarf_Sig8 type_signature{};
    Dwarf_Unsigned type_offset = 0;
    Dwarf_Unsigned next_cu_header_offset = 0;
    Dwarf_Half header_cu_type = 0;
    Dwarf_Error error = nullptr;

    expect_true(dwarf_next_cu_header_d(debug,
                                       true,
                                       &cu_header_length,
                                       &version_stamp,
                                       &abbrev_offset,
                                       &address_size,
                                       &length_size,
                                       &extension_size,
                                       &type_signature,
                                       &type_offset,
                                       &next_cu_header_offset,
                                       &header_cu_type,
                                       &error) == DW_DLV_OK,
                "第一个 gcc dwarf5 strp fixture CU 应可读取");

    Dwarf_Die cu_die = nullptr;
    expect_true(dwarf_siblingof_b(debug, nullptr, true, &cu_die, &error) == DW_DLV_OK,
                "应能拿到当前 gcc dwarf5 strp fixture 的第一个 CU DIE");

    auto name_attr = elf_static_view::elf::attribute_of(debug, cu_die, DW_AT_name);
    expect_true(name_attr.has_value(), "第一个 CU 应存在 DW_AT_name");
    const auto name = elf_static_view::elf::string_attr(name_attr->get());
    expect_true(name.has_value(), "DW_FORM_line_strp 的 name 应可被读取");
    expect_true(name->find("debug_sup_minimal.cc") != std::string::npos, "DW_FORM_line_strp 的 name 应回到源码文件名");

    auto comp_dir_attr = elf_static_view::elf::attribute_of(debug, cu_die, DW_AT_comp_dir);
    expect_true(comp_dir_attr.has_value(), "第一个 CU 应存在 DW_AT_comp_dir");
    const auto comp_dir = elf_static_view::elf::string_attr(comp_dir_attr->get());
    expect_true(comp_dir.has_value(), "DW_FORM_line_strp 的 comp_dir 应可被读取");
    const auto normalized_comp_dir = normalize_path_separators(*comp_dir);
    const auto normalized_binary_dir = normalize_path_separators(ELF_STATIC_VIEW_TEST_BINARY_DIR);
    expect_true(normalized_comp_dir.find(normalized_binary_dir) != std::string::npos,
                "DW_FORM_line_strp 的 comp_dir 应回到构建目录");
}

void verify_dwarf5_strx1_fixture()
{
    elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_DWARF5_STRX1_FIXTURE_PATH);
    Dwarf_Debug debug = debug_handle.get();

    Dwarf_Unsigned cu_header_length = 0;
    Dwarf_Half version_stamp = 0;
    Dwarf_Off abbrev_offset = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Half length_size = 0;
    Dwarf_Half extension_size = 0;
    Dwarf_Sig8 type_signature{};
    Dwarf_Unsigned type_offset = 0;
    Dwarf_Unsigned next_cu_header_offset = 0;
    Dwarf_Half header_cu_type = 0;
    Dwarf_Error error = nullptr;

    expect_true(dwarf_next_cu_header_d(debug,
                                       true,
                                       &cu_header_length,
                                       &version_stamp,
                                       &abbrev_offset,
                                       &address_size,
                                       &length_size,
                                       &extension_size,
                                       &type_signature,
                                       &type_offset,
                                       &next_cu_header_offset,
                                       &header_cu_type,
                                       &error) == DW_DLV_OK,
                "dwarf5 strx1 fixture 的第一个 unit 应可读取");

    Dwarf_Die root_die = nullptr;
    expect_true(dwarf_siblingof_b(debug, nullptr, true, &root_die, &error) == DW_DLV_OK,
                "应能拿到 dwarf5 strx1 fixture 的根 DIE");

    auto named_type = elf_static_view::elf::child_of(debug, root_die);
    expect_true(named_type.has_value(), "dwarf5 strx1 fixture 应存在 type unit 的首个子 DIE");

    auto name_attr = elf_static_view::elf::attribute_of(debug, named_type->get(), DW_AT_name);
    expect_true(name_attr.has_value(), "dwarf5 strx1 fixture 的 structure_type 应存在 DW_AT_name");
    const auto name = elf_static_view::elf::string_attr(name_attr->get());
    expect_true(name.has_value(), "DW_FORM_strx1 的 name 应可被读取");
    expect_true(name->find("FancyName") != std::string::npos, "DW_FORM_strx1 的 name 应回到 FancyName");
}

void verify_dwarf5_strx_form_fixture(const std::string& fixture_path,
                                     const std::string& expected_form_name,
                                     const std::string& label)
{
    verify_abbrev_attribute_form_via_dwarfdump(fixture_path, "DW_AT_name", expected_form_name, label);

    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        fixture_path,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "global_name [StaticAddressKnown] FancyName", label + " 应继续解析出 FancyName");
    expect_contains(output, "global_name.value [StaticLayoutKnown] int", label + " 应继续解析出成员 int");
    expect_contains(output, "use_name::local [RuntimeOnly] FancyName", label + " 应继续解析出 local 的 FancyName");
}

void verify_dwarf5_addrx_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DWARF5_ADDRX_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto globals = find_variables_by_name(model, "global_name");
    expect_true(globals.size() == 1, "dwarf5 addrx fixture 应保留 global_name");
    expect_true(globals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
                "dwarf5 addrx fixture 的 global_name 应解析成静态地址已知");
    expect_true(globals.front()->address.absolute_address.has_value(),
                "dwarf5 addrx fixture 的 global_name 应解析出绝对地址");
    expect_true(globals.front()->address.location_description == "DW_OP_addrx",
                "dwarf5 addrx fixture 应保留 DW_OP_addrx 位置描述");
}

void verify_dwarf5_addrx_form_fixture(const std::string& fixture_path,
                                      const Dwarf_Half expected_form,
                                      const std::string& label)
{
    const auto output_path =
        run_dwarfdump_to_temp("--print-abbrev", fixture_path, "addrx-form-" + std::to_string(expected_form), label);
    const auto abbrev_text = read_all(output_path);
    std::filesystem::remove(output_path);

    const auto expected_form_name = [&]() -> std::string {
        switch (expected_form) {
            case DW_FORM_addrx1:
                return "DW_FORM_addrx1";
            case DW_FORM_addrx2:
                return "DW_FORM_addrx2";
            case DW_FORM_addrx3:
                return "DW_FORM_addrx3";
            case DW_FORM_addrx4:
                return "DW_FORM_addrx4";
            default:
                return "DW_FORM_addrx";
        }
    }();
    expect_contains(abbrev_text, "DW_AT_low_pc", label + " 的 abbrev 应包含 DW_AT_low_pc");
    expect_contains(abbrev_text, expected_form_name, label + " 的 abbrev 应包含目标 DW_FORM_addrx*");
    expect_true(expected_form == DW_FORM_addrx1 || expected_form == DW_FORM_addrx2 || expected_form == DW_FORM_addrx3 ||
                    expected_form == DW_FORM_addrx4,
                label + " 仅应用于 DW_FORM_addrx1/2/3/4");

    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        fixture_path,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "global_name [StaticAddressKnown] FancyName", label + " 应继续解析出 FancyName");

    const auto globals = find_variables_by_name(model, "global_name");
    expect_true(globals.size() == 1, label + " 应保留 global_name");
    expect_true(globals.front()->address.absolute_address.has_value(), label + " 的 global_name 应仍有绝对地址");
}

void verify_text_presence_via_dwarfdump(const std::string& fixture_path,
                                        const std::string& dwarfdump_kind,
                                        const std::string& expected_text,
                                        const std::string& label)
{
    const auto output_path = run_dwarfdump_to_temp(dwarfdump_kind, fixture_path, "dwarfdump-presence", label);
    const auto content = read_all(output_path);
    std::filesystem::remove(output_path);
    expect_contains(content, expected_text, label + " 应显式包含目标文本");
}

void verify_abbrev_attribute_form_via_dwarfdump(const std::string& fixture_path,
                                                const std::string& attribute_name,
                                                const std::string& form_name,
                                                const std::string& label)
{
    const auto output_path = run_dwarfdump_to_temp("--print-abbrev", fixture_path, "abbrev-attr-form", label);
    const auto content = read_all(output_path);
    std::filesystem::remove(output_path);
    expect_contains(content, attribute_name, label + " 的 abbrev 应包含属性 " + attribute_name);
    expect_contains(content, form_name, label + " 的 abbrev 应包含表单 " + form_name);
}

void verify_fixture_contains_compile_unit_ranges(const std::string& fixture_path,
                                                 const std::size_t min_range_count,
                                                 const std::string& label)
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        fixture_path,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto cu_with_ranges = std::find_if(
        model.compile_units.begin(), model.compile_units.end(), [](const elf_static_view::CompileUnitRecord& cu) {
            return !cu.address.location_ranges.empty();
        });
    expect_true(cu_with_ranges != model.compile_units.end(), label + " 应至少有一个 compile unit 保留 ranges");
    expect_true(cu_with_ranges->address.location_ranges.size() >= min_range_count,
                label + " 的 compile unit ranges 数量应达到预期");
}

void verify_rnglistx_fixture()
{
    verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTX_FIXTURE_PATH,
                                       "--print-raw-rnglists",
                                       "offset entry count    :   1",
                                       "rnglistx fixture");
    verify_abbrev_attribute_form_via_dwarfdump(
        ELF_STATIC_VIEW_RNGLISTX_FIXTURE_PATH, "DW_AT_ranges", "DW_FORM_rnglistx", "rnglistx fixture");

    elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_RNGLISTX_FIXTURE_PATH);
    Dwarf_Debug debug = debug_handle.get();
    Dwarf_Unsigned cu_header_length = 0;
    Dwarf_Half version_stamp = 0;
    Dwarf_Off abbrev_offset = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Half length_size = 0;
    Dwarf_Half extension_size = 0;
    Dwarf_Sig8 type_signature{};
    Dwarf_Unsigned type_offset = 0;
    Dwarf_Unsigned next_cu_header_offset = 0;
    Dwarf_Half header_cu_type = 0;
    Dwarf_Error error = nullptr;
    expect_true(dwarf_next_cu_header_d(debug,
                                       true,
                                       &cu_header_length,
                                       &version_stamp,
                                       &abbrev_offset,
                                       &address_size,
                                       &length_size,
                                       &extension_size,
                                       &type_signature,
                                       &type_offset,
                                       &next_cu_header_offset,
                                       &header_cu_type,
                                       &error) == DW_DLV_OK,
                "rnglistx fixture 的第一个 unit 应可读取");
    auto cu_die = elf_static_view::elf::die_from_offset(debug, 0x0c, true);
    expect_true(cu_die.has_value(), "rnglistx fixture 应能按已知 offset 拿到 compile unit 根 DIE");

    auto ranges_attr = elf_static_view::elf::attribute_of(debug, cu_die->get(), DW_AT_ranges);
    expect_true(ranges_attr.has_value(), "rnglistx fixture 的 compile unit 应存在 DW_AT_ranges");

    {
        Dwarf_Half form = 0;
        Dwarf_Error attr_error = nullptr;
        const int form_result = dwarf_whatform(ranges_attr->get(), &form, &attr_error);
        expect_true(form_result == DW_DLV_OK, "rnglistx fixture 的 DW_AT_ranges 应可读取 form");
        expect_true(form == DW_FORM_rnglistx, "rnglistx fixture 的 DW_AT_ranges 应保持 DW_FORM_rnglistx");
    }

    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_RNGLISTX_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    expect_true(!model.compile_units.empty(), "rnglistx fixture 应至少保留一个 compile unit");
    const auto cu_with_ranges = std::find_if(
        model.compile_units.begin(), model.compile_units.end(), [](const elf_static_view::CompileUnitRecord& cu) {
            return !cu.address.location_ranges.empty();
        });
    expect_true(cu_with_ranges != model.compile_units.end(), "rnglistx fixture 的 compile unit 应至少有一个保留 range");
    expect_true(cu_with_ranges->address.location_ranges.size() >= 1,
                "rnglistx fixture 的 compile unit 应把 ranges 写入模型");
}

void verify_rnglists_start_end_fixture()
{
    verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_START_END_FIXTURE_PATH,
                                       "--print-raw-rnglists",
                                       "DW_RLE_start_end",
                                       "rnglists start_end fixture");

    verify_fixture_contains_compile_unit_ranges(
        ELF_STATIC_VIEW_RNGLISTS_START_END_FIXTURE_PATH, 1, "rnglists start_end fixture");
}

void verify_rnglists_offset_pair_fixture()
{
    verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_OFFSET_PAIR_FIXTURE_PATH,
                                       "--print-raw-rnglists",
                                       "DW_RLE_offset_pair",
                                       "rnglists offset_pair fixture");
    verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_OFFSET_PAIR_FIXTURE_PATH,
                                       "--print-raw-rnglists",
                                       "DW_RLE_base_address",
                                       "rnglists offset_pair fixture");

    verify_fixture_contains_compile_unit_ranges(
        ELF_STATIC_VIEW_RNGLISTS_OFFSET_PAIR_FIXTURE_PATH, 1, "rnglists offset_pair fixture");
}

void verify_rnglists_startx_length_fixture()
{
    verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_STARTX_LENGTH_FIXTURE_PATH,
                                       "--print-raw-rnglists",
                                       "DW_RLE_startx_length",
                                       "rnglists startx_length fixture");
    verify_fixture_contains_compile_unit_ranges(
        ELF_STATIC_VIEW_RNGLISTS_STARTX_LENGTH_FIXTURE_PATH, 1, "rnglists startx_length fixture");
}

void verify_rnglists_base_addressx_fixture()
{
    verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_BASE_ADDRESSX_FIXTURE_PATH,
                                       "--print-raw-rnglists",
                                       "DW_RLE_base_addressx",
                                       "rnglists base_addressx fixture");
    verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_BASE_ADDRESSX_FIXTURE_PATH,
                                       "--print-raw-rnglists",
                                       "DW_RLE_offset_pair",
                                       "rnglists base_addressx fixture");
    verify_fixture_contains_compile_unit_ranges(
        ELF_STATIC_VIEW_RNGLISTS_BASE_ADDRESSX_FIXTURE_PATH, 1, "rnglists base_addressx fixture");
}

void verify_rnglists_startx_endx_fixture()
{
    verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_RNGLISTS_STARTX_ENDX_FIXTURE_PATH,
                                       "--print-raw-rnglists",
                                       "DW_RLE_startx_endx",
                                       "rnglists startx_endx fixture");
    verify_fixture_contains_compile_unit_ranges(
        ELF_STATIC_VIEW_RNGLISTS_STARTX_ENDX_FIXTURE_PATH, 1, "rnglists startx_endx fixture");
}

void verify_implicit_const_fixture()
{
    const auto output_path = run_dwarfdump_to_temp(
        "--print-abbrev", ELF_STATIC_VIEW_GCC_GNU_ALT_FIXTURE_PATH, "implicit-const", "implicit_const fixture");
    const auto content = read_all(output_path);
    std::filesystem::remove(output_path);
    expect_contains(content, "DW_FORM_implicit_const", "fixture 应显式包含 DW_FORM_implicit_const");
    expect_contains(content, "DW_AT_decl_file", "implicit_const fixture 应保留 decl_file");
    expect_contains(content, "DW_AT_decl_column", "implicit_const fixture 应保留 decl_column");
}

void verify_dwarf5_strx_type_unit_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DWARF5_STRX_TYPE_UNIT_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output,
                    "global_name [StaticAddressKnown] FancyName",
                    "dwarf5 strx type unit fixture 应把 global_name 解析成 FancyName");
    expect_contains(
        output, "use_name::local [RuntimeOnly] FancyName", "dwarf5 strx type unit fixture 应把 local 解析成 FancyName");
    expect_contains(
        output, "global_name.value [StaticLayoutKnown] int", "dwarf5 strx type unit fixture 应保留成员 int 类型");
}

void verify_dwarf5_strx_type_unit_abbrev_offset_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DWARF5_STRX_TYPE_UNIT_ABBREV_OFFSET_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output,
                    "global_name [StaticAddressKnown] FancyName",
                    "abbrev_offset!=0 的 dwarf5 type unit 应把 global_name 解析成 FancyName");
    expect_contains(output,
                    "use_name::local [RuntimeOnly] FancyName",
                    "abbrev_offset!=0 的 dwarf5 type unit 应把 local 解析成 FancyName");
    expect_contains(output,
                    "global_name.value [StaticLayoutKnown] int",
                    "abbrev_offset!=0 的 dwarf5 type unit 应保留成员 int 类型");
}

void verify_split_dwarf_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_SPLIT_DWARF_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto globals = find_variables_by_name(model, "global_value");
    const auto locals = find_variables_by_name(model, "local");
    expect_true(globals.size() == 1, "split dwarf fixture 应解析出 global_value");
    expect_true(locals.size() == 1, "split dwarf fixture 应解析出 local");
    expect_true(globals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
                "split dwarf 的 global_value 应解析出静态地址");
    expect_true(locals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
                "split dwarf 的 local 应解析出静态地址");
    expect_true(globals.front()->type.id != "type@unknown", "split dwarf 的 global_value 不应退化成未知类型");
    expect_true(locals.front()->type.id != "type@unknown", "split dwarf 的 local 不应退化成未知类型");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "global_value [StaticAddressKnown] S", "split dwarf dump 应包含 global_value");
    expect_contains(output, "main::local [StaticAddressKnown] S", "split dwarf dump 应包含 local");
}

void verify_split_dwp_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto dwp_path = std::filesystem::path(ELF_STATIC_VIEW_SPLIT_DWARF_FIXTURE_PATH).replace_extension("dwp");
    const auto model = loader.dump(
        dwp_path.string(),
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto globals = find_variables_by_name(model, "global_value");
    const auto locals = find_variables_by_name(model, "local");
    expect_true(globals.size() == 1, "dwp fixture 应解析出 global_value");
    expect_true(locals.size() == 1, "dwp fixture 应解析出 local");
    expect_true(globals.front()->availability == elf_static_view::Availability::Unavailable,
                "纯 dwp fixture 当前没有 tied executable，global_value 应保留 Unavailable");
    expect_true(locals.front()->availability == elf_static_view::Availability::Unavailable,
                "纯 dwp fixture 当前没有 tied executable，local 应保留 Unavailable");
    expect_true(globals.front()->type.id != "type@unknown", "dwp 的 global_value 不应退化成未知类型");
    expect_true(locals.front()->type.id != "type@unknown", "dwp 的 local 不应退化成未知类型");
}

void verify_debug_sup_fixture()
{
    elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_DEBUG_SUP_FIXTURE_PATH);

    Dwarf_Half version = 0;
    Dwarf_Small is_supplementary = 0;
    char* filename = nullptr;
    Dwarf_Unsigned checksum_len = 0;
    Dwarf_Small* checksum = nullptr;
    Dwarf_Error error = nullptr;
    const int result = dwarf_get_debug_sup(
        debug_handle.get(), &version, &is_supplementary, &filename, &checksum_len, &checksum, &error);
    if (result != DW_DLV_OK) {
        std::string message = "debug_sup fixture 应可读出 .debug_sup";
        if (error != nullptr) {
            message += "，实际错误: ";
            message += dwarf_errmsg(error);
            dwarf_dealloc_error(nullptr, error);
        }
        throw std::runtime_error(message);
    }

    expect_true(version == 2, ".debug_sup version 应为 2");
    expect_true(is_supplementary == 0, ".debug_sup 样本应标记为主对象引用 supplementary");
    expect_true(filename != nullptr, ".debug_sup filename 不应为空");
    expect_true(std::string(filename) == "fake-file/fake-file-name", ".debug_sup filename 应与 dwarfgen 生成内容一致");
    expect_true(checksum_len == sizeof("fake checksum_content"), ".debug_sup checksum 长度应与 dwarfgen 固定内容一致");
    expect_true(checksum != nullptr, ".debug_sup checksum 不应为空");
    expect_true(std::string(reinterpret_cast<const char*>(checksum), static_cast<std::size_t>(checksum_len - 1)) ==
                    "fake checksum_content",
                ".debug_sup checksum 内容应与 dwarfgen 固定内容一致");

    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DEBUG_SUP_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    expect_true(model.elf_info.object_class == "ELF32", "debug_sup fixture 应保留 dwarfgen 产物的 ELF32 元信息");
    expect_true(model.elf_info.file_type == "REL", "debug_sup fixture 当前应是 dwarfgen 生成的 REL 对象");
    expect_true(model.symbols.empty(), "debug_sup 最小样本当前只验证 section 可读与加载不崩溃，不要求保留原始静态变量");
}

void verify_debug_sup_sidecar_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DEBUG_SUP_SIDECAR_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output,
                    "demo::global_object [StaticAddressKnown] Derived",
                    ".debug_sup sidecar 自动绑定后应保留主 ELF 的静态变量");
    expect_contains(
        output, "demo::Derived::shared [StaticAddressKnown] int", ".debug_sup sidecar 自动绑定后应保留类静态成员");
}

void verify_gcc_strp_sup_fixture()
{
    elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_GCC_DWARF5_STRP_SUP_FIXTURE_PATH);
    Dwarf_Debug debug = debug_handle.get();

    Dwarf_Unsigned cu_header_length = 0;
    Dwarf_Half version_stamp = 0;
    Dwarf_Off abbrev_offset = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Half length_size = 0;
    Dwarf_Half extension_size = 0;
    Dwarf_Sig8 type_signature{};
    Dwarf_Unsigned type_offset = 0;
    Dwarf_Unsigned next_cu_header_offset = 0;
    Dwarf_Half header_cu_type = 0;
    Dwarf_Error error = nullptr;

    expect_true(dwarf_next_cu_header_d(debug,
                                       true,
                                       &cu_header_length,
                                       &version_stamp,
                                       &abbrev_offset,
                                       &address_size,
                                       &length_size,
                                       &extension_size,
                                       &type_signature,
                                       &type_offset,
                                       &next_cu_header_offset,
                                       &header_cu_type,
                                       &error) == DW_DLV_OK,
                "第一个 gcc dwarf5 fixture CU 应可读取");
    expect_true(dwarf_next_cu_header_d(debug,
                                       true,
                                       &cu_header_length,
                                       &version_stamp,
                                       &abbrev_offset,
                                       &address_size,
                                       &length_size,
                                       &extension_size,
                                       &type_signature,
                                       &type_offset,
                                       &next_cu_header_offset,
                                       &header_cu_type,
                                       &error) == DW_DLV_OK,
                "第二个 gcc dwarf5 fixture CU 应可读取");

    Dwarf_Die cu_die = nullptr;
    expect_true(dwarf_siblingof_b(debug, nullptr, true, &cu_die, &error) == DW_DLV_OK,
                "应能拿到当前 gcc dwarf5 fixture 的第二个 CU DIE");

    auto name_attr = elf_static_view::elf::attribute_of(debug, cu_die, DW_AT_name);
    expect_true(name_attr.has_value(), "第二个 CU 应存在 DW_AT_name");
    const auto name = elf_static_view::elf::string_attr(name_attr->get());
    expect_true(name.has_value(), "DW_FORM_strp_sup 的 name 应可被读取");
    expect_true(name->find("cygwin.S") != std::string::npos, "DW_FORM_strp_sup 的 name 应回到 supplementary string");

    auto producer_attr = elf_static_view::elf::attribute_of(debug, cu_die, DW_AT_producer);
    expect_true(producer_attr.has_value(), "第二个 CU 应存在 DW_AT_producer");
    const auto producer = elf_static_view::elf::string_attr(producer_attr->get());
    expect_true(producer.has_value(), "DW_FORM_strp_sup 的 producer 应可被读取");
    expect_true(producer->find("GNU AS") != std::string::npos,
                "DW_FORM_strp_sup 的 producer 应回到 supplementary string");
}

void verify_current_supplementary_gap_notes()
{
    // 这不是功能测试，而是把当前“已接线”和“已正式覆盖”的边界固定住，避免误判。
    // 备注：
    // - DW_FORM_indirect 已通过 ref_sig8_debug_types_indirect fixture 间接覆盖；
    // - DW_FORM_addrx / addrx1 / addrx2 / addrx3 / addrx4 已有显式 fixture；
    // - DW_FORM_strx1 / strx2 / strx3 / strx4 已有显式 fixture；
    // - 当前剩余更大的问题不再是 supplementary/alternate form，而是“DWARF 2.0-最新”
    //   口径下仍有一些 code-only form/loc op 只被实现、还没逐项做 form 级显式回归。
    expect_true(true,
                "当前已正式覆盖 .debug_sup section 读取、sidecar 自动绑定、"
                "DW_FORM_strp_sup、DW_FORM_ref_sup4、DW_FORM_ref_sup8、"
                "DW_FORM_GNU_strp_alt、DW_FORM_GNU_ref_alt。");
}

void verify_const_value_text_json_round_trip()
{
    elf_static_view::ProjectModel model;
    model.file = "demo.elf";
    model.elf_info = {.object_class = "ELF64",
                      .byte_order = "LittleEndian",
                      .file_type = "DYN",
                      .machine = "x86_64",
                      .os_abi = "SystemV"};

    elf_static_view::VariableRecord symbol;
    symbol.id = "var@text";
    symbol.name = "const_blob";
    symbol.compile_unit_name = "demo.cpp";
    symbol.variable_kind = elf_static_view::VariableKind::FileStatic;
    symbol.availability = elf_static_view::Availability::StaticAddressKnown;
    symbol.address.kind = elf_static_view::AddressKind::Unknown;
    symbol.address.location_description = "DW_AT_const_value";
    symbol.type.id = "type@blob";
    symbol.const_value_text = "0x112233";
    symbol.has_static_storage = true;
    model.symbols.push_back(symbol);
    model.expanded.push_back(elf_static_view::ExpandedNode{
        .path = "const_blob",
        .display_name = "const_blob",
        .type_name = "Blob",
        .type_kind = elf_static_view::TypeKind::Struct,
        .availability = elf_static_view::Availability::StaticAddressKnown,
        .absolute_address = std::nullopt,
        .relative_offset = std::nullopt,
        .byte_size = std::nullopt,
        .array_count = std::nullopt,
        .array_stride = std::nullopt,
        .children = {},
    });

    const auto json = elf_static_view::render_dump_json(model);
    expect_contains(json, "\"const_value_text\": \"0x112233\"", "JSON 应输出 const_value_text");
    const auto parsed = elf_static_view::parse_dump_json(json);
    expect_true(parsed.symbols.size() == 1, "const_value_text JSON 往返应保留变量");
    expect_true(parsed.symbols.front().const_value_text.has_value(), "const_value_text JSON 往返应保留文本常量");
    expect_true(parsed.symbols.front().const_value_text.value() == "0x112233", "const_value_text JSON 往返值不正确");
    const auto text = elf_static_view::render_dump_text(parsed);
    expect_contains(text, "= 0x112233", "文本 dump 应展示 const_value_text");
}

void verify_value_expression_text_rendering()
{
    elf_static_view::ProjectModel model;
    model.file = "demo.elf";
    model.elf_info = {.object_class = "ELF64",
                      .byte_order = "LittleEndian",
                      .file_type = "DYN",
                      .machine = "x86_64",
                      .os_abi = "SystemV"};

    elf_static_view::VariableRecord symbol;
    symbol.id = "var@value-expr";
    symbol.name = "runtime_value";
    symbol.compile_unit_name = "demo.cpp";
    symbol.variable_kind = elf_static_view::VariableKind::Local;
    symbol.availability = elf_static_view::Availability::RuntimeOnly;
    symbol.address.kind = elf_static_view::AddressKind::Unknown;
    symbol.address.location_description = "value expression";
    symbol.type.id = "type@int";
    model.symbols.push_back(symbol);
    model.expanded.push_back(elf_static_view::ExpandedNode{
        .path = "runtime_value",
        .display_name = "runtime_value",
        .type_name = "int",
        .type_kind = elf_static_view::TypeKind::Base,
        .availability = elf_static_view::Availability::RuntimeOnly,
        .absolute_address = std::nullopt,
        .relative_offset = std::nullopt,
        .byte_size = std::nullopt,
        .array_count = std::nullopt,
        .array_stride = std::nullopt,
        .children = {},
    });

    const auto json = elf_static_view::render_dump_json(model);
    expect_contains(json, "\"location_description\": \"value expression\"", "JSON 应输出 value expression 描述");
    const auto parsed = elf_static_view::parse_dump_json(json);
    expect_true(parsed.symbols.size() == 1, "value expression JSON 往返应保留变量");
    expect_true(parsed.symbols.front().address.location_description == "value expression",
                "value expression JSON 往返后描述应保持一致");
    const auto text = elf_static_view::render_dump_text(parsed);
    expect_contains(text, "runtime_value", "文本 dump 应保留 value expression 变量");
}

void verify_named_location_op_rendering()
{
    elf_static_view::ProjectModel model;
    model.file = "demo.elf";
    model.elf_info = {.object_class = "ELF64",
                      .byte_order = "LittleEndian",
                      .file_type = "DYN",
                      .machine = "x86_64",
                      .os_abi = "SystemV"};

    elf_static_view::VariableRecord symbol;
    symbol.id = "var@named-op";
    symbol.name = "frame_local";
    symbol.compile_unit_name = "demo.cpp";
    symbol.variable_kind = elf_static_view::VariableKind::Local;
    symbol.availability = elf_static_view::Availability::RuntimeOnly;
    symbol.address.kind = elf_static_view::AddressKind::Unknown;
    symbol.address.location_description = "DW_OP_breg20";
    symbol.type.id = "type@int";
    model.symbols.push_back(symbol);
    model.expanded.push_back(elf_static_view::ExpandedNode{
        .path = "frame_local",
        .display_name = "frame_local",
        .type_name = "int",
        .type_kind = elf_static_view::TypeKind::Base,
        .availability = elf_static_view::Availability::RuntimeOnly,
        .absolute_address = std::nullopt,
        .relative_offset = std::nullopt,
        .byte_size = std::nullopt,
        .array_count = std::nullopt,
        .array_stride = std::nullopt,
        .children = {},
    });

    const auto json = elf_static_view::render_dump_json(model);
    expect_contains(json, "\"location_description\": \"DW_OP_breg20\"", "JSON 应输出命名后的 location op");
    const auto text = elf_static_view::render_dump_text(model);
    expect_contains(text, "frame_local", "文本 dump 应保留命名 location op 变量");
}

void verify_register_location_text_rendering()
{
    elf_static_view::ProjectModel model;
    model.file = "demo.elf";
    model.elf_info = {.object_class = "ELF64",
                      .byte_order = "LittleEndian",
                      .file_type = "DYN",
                      .machine = "x86_64",
                      .os_abi = "SystemV"};

    elf_static_view::VariableRecord symbol;
    symbol.id = "var@reg-op";
    symbol.name = "arg0";
    symbol.compile_unit_name = "demo.cpp";
    symbol.variable_kind = elf_static_view::VariableKind::Parameter;
    symbol.availability = elf_static_view::Availability::RuntimeOnly;
    symbol.address.kind = elf_static_view::AddressKind::Unknown;
    symbol.address.location_description = "register-value DW_OP_reg0";
    symbol.type.id = "type@int";
    model.symbols.push_back(symbol);
    model.expanded.push_back(elf_static_view::ExpandedNode{
        .path = "arg0",
        .display_name = "arg0",
        .type_name = "int",
        .type_kind = elf_static_view::TypeKind::Base,
        .availability = elf_static_view::Availability::RuntimeOnly,
        .absolute_address = std::nullopt,
        .relative_offset = std::nullopt,
        .byte_size = std::nullopt,
        .array_count = std::nullopt,
        .array_stride = std::nullopt,
        .children = {},
    });

    const auto json = elf_static_view::render_dump_json(model);
    expect_contains(
        json, "\"location_description\": \"register-value DW_OP_reg0\"", "JSON 应输出寄存器语义 location op");
}

void verify_register_address_text_rendering()
{
    elf_static_view::ProjectModel model;
    model.file = "demo.elf";
    model.elf_info = {.object_class = "ELF64",
                      .byte_order = "LittleEndian",
                      .file_type = "DYN",
                      .machine = "x86_64",
                      .os_abi = "SystemV"};

    elf_static_view::VariableRecord symbol;
    symbol.id = "var@breg-op";
    symbol.name = "stack_slot";
    symbol.compile_unit_name = "demo.cpp";
    symbol.variable_kind = elf_static_view::VariableKind::Local;
    symbol.availability = elf_static_view::Availability::RuntimeOnly;
    symbol.address.kind = elf_static_view::AddressKind::Unknown;
    symbol.address.location_description = "register-address DW_OP_breg20 -2";
    symbol.type.id = "type@int";
    model.symbols.push_back(symbol);
    model.expanded.push_back(elf_static_view::ExpandedNode{
        .path = "stack_slot",
        .display_name = "stack_slot",
        .type_name = "int",
        .type_kind = elf_static_view::TypeKind::Base,
        .availability = elf_static_view::Availability::RuntimeOnly,
        .absolute_address = std::nullopt,
        .relative_offset = std::nullopt,
        .byte_size = std::nullopt,
        .array_count = std::nullopt,
        .array_stride = std::nullopt,
        .children = {},
    });

    const auto json = elf_static_view::render_dump_json(model);
    expect_contains(json,
                    "\"location_description\": \"register-address DW_OP_breg20 -2\"",
                    "JSON 应输出寄存器寻址语义 location op");
}

void verify_unspecified_type_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_UNSPECIFIED_TYPE_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto* null_type_alias = find_type_by_name(model, "NullType");
    expect_true(null_type_alias != nullptr, "unspecified fixture 应包含 NullType");
    expect_true(null_type_alias->kind == elf_static_view::TypeKind::Typedef, "NullType 应保留 Typedef kind");
    expect_true(null_type_alias->aliased_of.has_value(), "NullType 应指向 decltype(nullptr) 底层类型");

    const auto aliased_id = null_type_alias->aliased_of->id;
    const auto unspecified_iter =
        std::find_if(model.types.begin(), model.types.end(), [&](const elf_static_view::TypeNode& type) {
            return type.id == aliased_id;
        });
    expect_true(unspecified_iter != model.types.end(), "decltype(nullptr) 类型节点应存在");
    expect_true(unspecified_iter->kind == elf_static_view::TypeKind::Unspecified,
                "decltype(nullptr) 应识别为 Unspecified");

    const auto globals = find_variables_by_name(model, "global_null");
    expect_true(globals.size() == 1, "应解析出 global_null");
}

void verify_atomic_type_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_ATOMIC_TYPE_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto atomic_iter =
        std::find_if(model.types.begin(), model.types.end(), [&](const elf_static_view::TypeNode& type) {
            return type.kind == elf_static_view::TypeKind::Atomic;
        });
    expect_true(atomic_iter != model.types.end(), "atomic fixture 应包含 atomic 类型");
    expect_true(atomic_iter->qualified_of.has_value(), "atomic 类型应指向底层类型");

    const auto globals = find_variables_by_name(model, "global_atomic");
    expect_true(globals.size() == 1, "应解析出 global_atomic");
    expect_true(globals.front()->availability == elf_static_view::Availability::StaticAddressKnown,
                "global_atomic 应解析出静态地址");
}

void verify_abstract_origin_const_value_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_ABSTRACT_ORIGIN_VALUES_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto folded_variables = find_variables_by_name(model, "folded");
    expect_true(!folded_variables.empty(), "abstract-origin fixture 应包含 folded");

    const auto folded_with_const =
        std::find_if(folded_variables.begin(), folded_variables.end(), [](const auto* symbol) {
            return symbol->const_value.has_value() && symbol->const_value.value() == 8;
        });
    expect_true(folded_with_const != folded_variables.end(),
                "至少一个 folded 实例应从 abstract/inlined 视图保留 const_value");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output,
                    "main::call_helper::helper::folded [StaticAddressKnown] int = 8",
                    "应保留具体实例 folded 的 const_value");
    expect_contains(output, "const_inline [StaticAddressKnown] int = 5", "应保留 const_inline 的 const_value");
    expect_true(output.find("helper::seed [RuntimeOnly] int\n") == std::string::npos,
                "abstract-origin fixture 不应保留无值 seed 占位节点");
    expect_true(output.find("call_helper::value [RuntimeOnly] int\n") == std::string::npos,
                "abstract-origin fixture 不应保留无值 value 占位节点");
}

void verify_loader_error_contains_file_path()
{
    elf_static_view::ProjectLoader loader;
    const std::string missing_path = "Z:/definitely-missing/fixture.elf";
    try {
        (void) loader.dump(
            missing_path,
            {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
        throw std::runtime_error("断言失败: 缺失文件应抛出异常");
    } catch (const std::exception& error) {
        const std::string message = error.what();
        expect_contains(message, "文件分析失败: " + missing_path, "失败日志应包含输入文件路径");
    }
}

void verify_inline_scope_static_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(ELF_STATIC_VIEW_INLINE_SCOPE_FIXTURE_PATH,
                                   {.include_runtime_only = true,
                                    .only_static_known = false,
                                    .symbol_name = std::nullopt,
                                    .expand_depth = 8,
                                    .load_policy = {.exclude_formal_parameters = false}});

    const auto file_statics = find_file_static_variables(model, "file_static");
    expect_true(file_statics.size() == 1, "inline scope fixture 应保留 file_static");

    const auto inline_statics =
        find_variables_by_kind(model, "inline_static", elf_static_view::VariableKind::FunctionStatic);
    expect_true(inline_statics.size() == 1, "inline scope fixture 应识别 helper 内的函数静态变量");
    expect_true(inline_statics.front()->address.absolute_address.has_value(), "inline_static 应解析出绝对地址");
    expect_true(inline_statics.front()->scope_path.size() == 1, "inline_static 应保留单层函数作用域");

    const auto block_statics =
        find_variables_by_kind(model, "block_static", elf_static_view::VariableKind::FunctionStatic);
    expect_true(block_statics.size() == 1, "inline scope fixture 应识别 lexical_block 内的静态变量");
    expect_true(block_statics.front()->address.absolute_address.has_value(), "block_static 应解析出绝对地址");
    expect_true(block_statics.front()->scope_path == inline_statics.front()->scope_path,
                "block_static 与 inline_static 应保持同一抽象函数作用域");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "inline_static", "inline scope fixture dump 应包含 inline_static");
    expect_contains(output, "block_static", "inline scope fixture dump 应包含 block_static");

    const auto value_variables = find_variables_by_name(model, "value");
    expect_true(!value_variables.empty(), "inline scope fixture 应包含 value 参数");
    const auto value_with_fbreg = std::find_if(value_variables.begin(), value_variables.end(), [](const auto* symbol) {
        return symbol->address.location_description.starts_with("frame-base+");
    });
    expect_true(value_with_fbreg != value_variables.end(),
                "inline scope fixture 应把 DW_OP_fbreg 解释成 frame-base 偏移");
}

void verify_const_value_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_CONST_VALUE_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto file_statics = find_file_static_variables(model, "const_static");
    expect_true(file_statics.size() == 1, "const_value fixture 应解析出 const_static");
    expect_true(file_statics.front()->const_value.has_value(), "const_static 应保留 DW_AT_const_value");
    expect_true(file_statics.front()->const_value.value() == 123, "const_static 常量值应为 123");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "const_static", "const_value fixture dump 应包含 const_static");
    expect_contains(output, "= 123", "const_value fixture dump 应展示常量值");

    const auto pair_variables = find_variables_by_name(model, "pair");
    expect_true(pair_variables.size() == 1, "const_value fixture 应保留局部变量 pair");
    expect_true(pair_variables.front()->availability == elf_static_view::Availability::RuntimeOnly,
                "piecewise loclist 的 pair 应标记为 RuntimeOnly");
    expect_true(pair_variables.front()->address.location_entry_count.has_value(), "pair 应记录 loclist entry 数量");
    expect_true(pair_variables.front()->address.location_entry_count.value() >= 1,
                "pair 的 loclist entry 数量应至少为 1");
    expect_true(pair_variables.front()->address.location_ranges.size() >= 1, "pair 应保留至少一段有效 loclist 范围");
    expect_true(pair_variables.front()->address.location_ranges.front().cooked_low_pc.has_value(),
                "pair 的 loclist 范围应保留 cooked low pc");
    expect_true(pair_variables.front()->address.location_ranges.front().cooked_high_pc.has_value(),
                "pair 的 loclist 范围应保留 cooked high pc");
    expect_contains(
        pair_variables.front()->address.location_description, "DW_OP_piece", "pair 应保留 DW_OP_piece 描述");
    expect_contains(pair_variables.front()->address.location_description,
                    "DW_OP_stack_value",
                    "pair 应保留 DW_OP_stack_value 描述");
    expect_contains(output, "ranges=[0x", "const_value fixture dump 应展示 loclist 范围");
    expect_contains(output, "pair [RuntimeOnly]", "const_value fixture dump 应包含 pair");
}

void verify_piece_stack_value_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(ELF_STATIC_VIEW_PIECE_STACK_VALUE_FIXTURE_PATH,
                                   {.include_runtime_only = true,
                                    .only_static_known = false,
                                    .symbol_name = std::nullopt,
                                    .expand_depth = 8,
                                    .load_policy = {.exclude_formal_parameters = false}});

    const auto seed_variables = find_variables_by_name(model, "seed");
    expect_true(seed_variables.size() == 1, "piece stack_value fixture 应保留参数 seed");
    expect_true(seed_variables.front()->availability == elf_static_view::Availability::RuntimeOnly,
                "seed 应标记为 RuntimeOnly");
    expect_true(seed_variables.front()->address.location_entry_count.has_value(), "seed 应记录 loclist entry 数量");
    expect_true(seed_variables.front()->address.location_entry_count.value() >= 2,
                "seed 的 loclist entry 数量应至少为 2");
    expect_true(seed_variables.front()->address.location_ranges.size() >= 2, "seed 应保留至少两段有效 loclist 范围");
    expect_contains(seed_variables.front()->address.location_description,
                    "DW_OP_entry_value",
                    "seed 应保留 DW_OP_entry_value 描述");
    expect_contains(seed_variables.front()->address.location_description,
                    "DW_OP_stack_value",
                    "seed 应保留 DW_OP_stack_value 描述");

    const auto bits_variables = find_variables_by_name(model, "bits");
    expect_true(bits_variables.size() == 1, "piece stack_value fixture 应保留局部变量 bits");
    expect_true(bits_variables.front()->availability == elf_static_view::Availability::RuntimeOnly,
                "bits 应标记为 RuntimeOnly");
    expect_true(bits_variables.front()->address.location_entry_count.has_value(), "bits 应记录 loclist entry 数量");
    expect_true(bits_variables.front()->address.location_entry_count.value() >= 2,
                "bits 的 loclist entry 数量应至少为 2");
    expect_true(bits_variables.front()->address.location_ranges.size() >= 2, "bits 应保留至少两段有效 loclist 范围");
    expect_true(bits_variables.front()->address.location_ranges.front().cooked_low_pc.has_value(),
                "bits 的 loclist 范围应保留 cooked low pc");
    expect_true(bits_variables.front()->address.location_ranges.front().cooked_high_pc.has_value(),
                "bits 的 loclist 范围应保留 cooked high pc");
    expect_contains(
        bits_variables.front()->address.location_description, "DW_OP_piece", "bits 应保留 DW_OP_piece 描述");
    expect_contains(bits_variables.front()->address.location_description,
                    "DW_OP_stack_value",
                    "bits 应保留 DW_OP_stack_value 描述");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "use_bits::seed [RuntimeOnly]", "piece stack_value fixture dump 应包含 seed");
    expect_contains(output, "use_bits::bits [RuntimeOnly]", "piece stack_value fixture dump 应包含 bits");
    expect_contains(output, "ranges=[0x", "piece stack_value fixture dump 应展示 loclist 范围");
}

void verify_bit_piece_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_BIT_PIECE_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto bits_variables = find_variables_by_name(model, "bits");
    expect_true(bits_variables.size() == 1, "bit piece fixture 应保留局部变量 bits");
    expect_true(bits_variables.front()->availability == elf_static_view::Availability::RuntimeOnly,
                "bits 应标记为 RuntimeOnly");
    expect_true(bits_variables.front()->address.location_entry_count.has_value(), "bits 应记录 loclist entry 数量");
    expect_true(bits_variables.front()->address.location_entry_count.value() >= 4,
                "bits 的 loclist entry 数量应至少为 4");
    expect_true(bits_variables.front()->address.location_ranges.size() >= 4, "bits 应保留至少四段 loclist 范围");
    expect_contains(
        bits_variables.front()->address.location_description, "DW_OP_bit_piece", "bits 应保留 DW_OP_bit_piece 描述");
    expect_contains(bits_variables.front()->address.location_description,
                    "DW_OP_stack_value",
                    "bits 应保留 DW_OP_stack_value 描述");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "bits [RuntimeOnly]", "bit piece fixture dump 应包含 bits");
    expect_contains(output, "ranges=[0x", "bit piece fixture dump 应展示 loclist 范围");
}

void verify_dwarf5_loclists_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DWARF5_LOCLISTS_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto locals = find_variables_by_name(model, "local");
    expect_true(locals.size() == 1, "dwarf5 loclists fixture 应解析出 local");
    expect_true(locals.front()->availability == elf_static_view::Availability::RuntimeOnly,
                "dwarf5 loclists 里的 local 应标记为 RuntimeOnly");
    expect_true(locals.front()->address.location_entry_count.has_value(),
                "dwarf5 loclists 里的 local 应保留 loclist entry 数量");
    expect_true(locals.front()->address.location_entry_count.value() >= 1,
                "dwarf5 loclists 里的 local 至少应有 1 条 loclist entry");
    expect_true(!locals.front()->address.location_ranges.empty(), "dwarf5 loclists 里的 local 应保留至少一段范围");
    expect_true(locals.front()->address.location_ranges.front().cooked_low_pc.has_value(),
                "dwarf5 loclists 的范围应保留 cooked low pc");
    expect_true(locals.front()->address.location_ranges.front().cooked_high_pc.has_value(),
                "dwarf5 loclists 的范围应保留 cooked high pc");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "local", "dwarf5 loclists fixture dump 应包含 local");
    expect_contains(output, "ranges=[0x", "dwarf5 loclists fixture dump 应展示范围");
}

void verify_dwarf5_loclists_base_default_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_DEFAULT_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto locals = find_variables_by_name(model, "local");
    expect_true(locals.size() == 1, "base/default loclists fixture 应保留 local");
    expect_true(locals.front()->address.location_entry_count.has_value(),
                "base/default loclists 应记录 loclist entry 数量");
    expect_true(locals.front()->address.location_entry_count.value() >= 4,
                "base/default loclists 应保留插入后的额外 entry");
    expect_true(locals.front()->address.location_ranges.size() >= 2, "base/default loclists 仍应保留有效范围");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "use_loclist::local [RuntimeOnly]", "base/default loclists dump 应包含 local");
    expect_contains(output, "ranges=[0x", "base/default loclists dump 应展示范围");
}

void verify_dwarf5_loclists_base_addressx_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_ADDRESSX_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto locals = find_variables_by_name(model, "local");
    expect_true(locals.size() == 1, "base_addressx loclists fixture 应保留 local");
    expect_true(locals.front()->address.location_entry_count.has_value(),
                "base_addressx loclists 应记录 loclist entry 数量");
    expect_true(locals.front()->address.location_entry_count.value() >= 4,
                "base_addressx loclists 应保留插入后的额外 entry");
    expect_true(locals.front()->address.location_ranges.size() >= 2, "base_addressx loclists 仍应保留有效范围");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "use_loclist::local [RuntimeOnly]", "base_addressx loclists dump 应包含 local");
    expect_contains(output, "ranges=[0x", "base_addressx loclists dump 应展示范围");
    verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_ADDRESSX_FIXTURE_PATH,
                                       "--print-raw-loclists",
                                       "DW_LLE_base_addressx",
                                       "base_addressx loclists fixture");
    verify_text_presence_via_dwarfdump(ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_ADDRESSX_FIXTURE_PATH,
                                       "--print-raw-loclists",
                                       "DW_LLE_offset_pair",
                                       "base_addressx loclists fixture");

    elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_ADDRESSX_FIXTURE_PATH);
    Dwarf_Debug debug = debug_handle.get();
    auto local_die = elf_static_view::elf::die_from_offset(debug, 0x4f, true);
    expect_true(local_die.has_value(), "base_addressx loclists fixture 应能定位 local DIE");
    auto location_attr = elf_static_view::elf::attribute_of(debug, local_die->get(), DW_AT_location);
    expect_true(location_attr.has_value(), "base_addressx loclists fixture 的 local 应存在 DW_AT_location");
    const auto location = elf_static_view::elf::read_location_description(debug, location_attr->get(), 8, 4, 5);
    expect_true(location.has_value(), "DW_FORM_loclistx 的 location 应可被 read_location_description 读取");
    expect_true(location->entry_count >= 4, "DW_FORM_loclistx 应至少保留四条 loclist entry");
    const auto concrete_entries =
        std::count_if(location->entries.begin(),
                      location->entries.end(),
                      [](const elf_static_view::elf::LocationDescription::Entry& entry) {
                          return (entry.cooked_low_pc.has_value() && entry.cooked_high_pc.has_value()) ||
                                 (entry.raw_low_pc.has_value() && entry.raw_high_pc.has_value());
                      });
    expect_true(concrete_entries >= 2, "DW_FORM_loclistx 应至少返回两条可用 range");
}

void verify_location_expression_block_decoding()
{
    elf_static_view::elf::DebugHandle debug_handle(ELF_STATIC_VIEW_C_FIXTURE_PATH);
    Dwarf_Debug debug = debug_handle.get();

    std::vector<std::uint8_t> absolute_expr{
        DW_OP_addr,
        0x7e,
        0x19,
        0x01,
        0x00,
    };
    const auto absolute_location =
        elf_static_view::elf::read_location_expression(debug, absolute_expr.data(), absolute_expr.size(), 4, 4, 2);
    expect_true(absolute_location.has_value(), "DW_OP_addr 表达式块应可被解析");
    expect_true(absolute_location->kind == DW_LKIND_expression, "DW_OP_addr 表达式块应标记为 expression");
    expect_true(absolute_location->operations.size() == 1, "DW_OP_addr 表达式块应只包含一个操作");
    expect_true(absolute_location->operations.front().atom == DW_OP_addr, "表达式块操作码应为 DW_OP_addr");
    expect_true(absolute_location->operations.front().operand1 == 0x1197e,
                "DW_FORM_block1 风格表达式应按 4 字节地址解析出 0x1197e");

    std::vector<std::uint8_t> breg_expr{
        static_cast<std::uint8_t>(DW_OP_breg20),
        0x00,
    };
    const auto breg_location =
        elf_static_view::elf::read_location_expression(debug, breg_expr.data(), breg_expr.size(), 4, 4, 2);
    expect_true(breg_location.has_value(), "DW_OP_breg20 表达式块应可被解析且不崩溃");
    expect_true(breg_location->operations.size() == 1, "DW_OP_breg20 表达式块应只包含一个操作");
    expect_true(breg_location->operations.front().atom == DW_OP_breg20, "表达式块操作码应为 DW_OP_breg20");
}

void verify_dwarf5_loclists_startx_length_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DWARF5_LOCLISTS_STARTX_LENGTH_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto locals = find_variables_by_name(model, "local");
    expect_true(locals.size() == 1, "startx_length loclists fixture 应保留 local");
    expect_true(locals.front()->address.location_entry_count.has_value(),
                "startx_length loclists 应记录 loclist entry 数量");
    expect_true(locals.front()->address.location_entry_count.value() >= 4,
                "startx_length loclists 应保留四条以上 entry");
    expect_true(locals.front()->address.location_ranges.size() >= 2, "startx_length loclists 应保留至少两段有效范围");
}

void verify_dwarf5_loclists_startx_endx_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DWARF5_LOCLISTS_STARTX_ENDX_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto locals = find_variables_by_name(model, "local");
    expect_true(locals.size() == 1, "startx_endx loclists fixture 应保留 local");
    expect_true(locals.front()->address.location_entry_count.has_value(),
                "startx_endx loclists 应记录 loclist entry 数量");
    expect_true(locals.front()->address.location_entry_count.value() >= 4, "startx_endx loclists 应保留四条以上 entry");
    expect_true(locals.front()->address.location_ranges.size() >= 2, "startx_endx loclists 应保留至少两段有效范围");
}

void verify_dwarf5_loclists_start_length_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DWARF5_LOCLISTS_START_LENGTH_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto locals = find_variables_by_name(model, "local");
    expect_true(locals.size() == 1, "start_length loclists fixture 应保留 local");
    expect_true(locals.front()->address.location_entry_count.has_value(),
                "start_length loclists 应记录 loclist entry 数量");
    expect_true(locals.front()->address.location_entry_count.value() >= 4,
                "start_length loclists 应保留四条以上 entry");
    expect_true(locals.front()->address.location_ranges.size() >= 2, "start_length loclists 应保留至少两段有效范围");
}

void verify_dwarf5_loclists_start_end_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_DWARF5_LOCLISTS_START_END_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});
    const auto locals = find_variables_by_name(model, "local");
    expect_true(locals.size() == 1, "start_end loclists fixture 应保留 local");
    expect_true(locals.front()->address.location_entry_count.has_value(),
                "start_end loclists 应记录 loclist entry 数量");
    expect_true(locals.front()->address.location_entry_count.value() >= 4, "start_end loclists 应保留四条以上 entry");
    expect_true(locals.front()->address.location_ranges.size() >= 2, "start_end loclists 应保留至少两段有效范围");
}

void verify_specification_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_SPECIFICATION_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto values = find_variables_by_name(model, "value");
    expect_true(values.size() == 1, "specification fixture 应只保留真实定义节点");

    expect_true(values.front()->availability == elf_static_view::Availability::StaticAddressKnown,
                "specification fixture 应保留静态地址已知的定义节点");
    expect_true(values.front()->address.absolute_address.has_value(),
                "specification fixture 应解析出定义节点的绝对地址");
    expect_true(values.front()->address.location_description == "DW_OP_addrx",
                "specification fixture 应保留 DW_OP_addrx 位置描述");
    expect_true(values.front()->type.id != "type@unknown", "definition 节点不应退化成未知类型");
    expect_true(values.front()->variable_kind == elf_static_view::VariableKind::FileStatic ||
                    values.front()->variable_kind == elf_static_view::VariableKind::StaticMember,
                "specification 定义节点应保留为真实静态变量，而不是 declaration 占位节点");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "value [StaticAddressKnown]", "specification fixture dump 应包含真实定义节点");
}

void verify_thread_local_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_THREAD_LOCAL_TLS_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    const auto tls_values = find_variables_by_name(model, "tls_value");
    expect_true(tls_values.size() == 1, "TLS fixture 应解析出 tls_value");
    expect_true(tls_values.front()->is_thread_local, "tls_value 应标记为 thread-local");
    expect_true(tls_values.front()->variable_kind == elf_static_view::VariableKind::ThreadLocal,
                "tls_value 应归类为 ThreadLocal");
    expect_true(tls_values.front()->availability == elf_static_view::Availability::RuntimeOnly,
                "tls_value 应标记为 RuntimeOnly");
    expect_true(tls_values.front()->address.location_description == "thread_local" ||
                    tls_values.front()->address.location_description == "ELF symtab TLS",
                "tls_value 应保留 TLS 位置描述");

    const auto plain_values = find_variables_by_name(model, "plain_value");
    expect_true(plain_values.size() == 1, "TLS fixture 应解析出 plain_value");
    expect_true(!plain_values.front()->is_thread_local, "plain_value 不应被误判成 TLS");
    expect_true(plain_values.front()->availability == elf_static_view::Availability::StaticAddressKnown,
                "plain_value 应保留静态地址");

    const auto output = elf_static_view::render_dump_text(model);
    expect_contains(output, "tls_value [RuntimeOnly]", "TLS fixture dump 应包含 tls_value");
}

void verify_multi_cu_file_static_fixture()
{
    elf_static_view::ProjectLoader loader;
    const auto model = loader.dump(
        ELF_STATIC_VIEW_MULTI_CU_FIXTURE_PATH,
        {.include_runtime_only = true, .only_static_known = false, .symbol_name = std::nullopt, .expand_depth = 8});

    // 这里故意在两个源文件里放同名 file-static 变量，验证解析器不会把不同 CU 的局部符号串成一个。
    expect_true(model.compile_units.size() >= 2, "多 CU fixture 至少应包含两个编译单元");
    const auto shared_file_statics = find_file_static_variables(model, "shared_value");
    expect_true(shared_file_statics.size() == 2, "应解析出两个同名 file-static 变量");
    expect_true(shared_file_statics[0]->compile_unit_name != shared_file_statics[1]->compile_unit_name,
                "两个同名变量应来自不同编译单元");
    expect_true(shared_file_statics[0]->address.absolute_address.has_value(), "第一个变量应有绝对地址");
    expect_true(shared_file_statics[1]->address.absolute_address.has_value(), "第二个变量应有绝对地址");
    expect_true(shared_file_statics[0]->address.absolute_address.value() !=
                    shared_file_statics[1]->address.absolute_address.value(),
                "两个变量的绝对地址应不同");

    const auto private_file_statics = find_file_static_variables(model, "unit_private_value");
    expect_true(private_file_statics.size() == 2, "每个 CU 都应解析出自己的 unit_private_value");
    expect_true(private_file_statics[0]->compile_unit_name != private_file_statics[1]->compile_unit_name,
                "同名 unit_private_value 不应丢失编译单元边界");
    expect_true(private_file_statics[0]->address.absolute_address.has_value() &&
                    private_file_statics[1]->address.absolute_address.has_value(),
                "同名 unit_private_value 应能解析出绝对地址");
    expect_true(private_file_statics[0]->address.absolute_address.value() !=
                    private_file_statics[1]->address.absolute_address.value(),
                "不同 CU 的 unit_private_value 地址不应相同");

    const auto function_statics =
        find_variables_by_kind(model, "shared_counter", elf_static_view::VariableKind::FunctionStatic);
    expect_true(function_statics.size() == 2, "每个 CU 的函数静态变量都应被解析出来");
    expect_true(function_statics[0]->compile_unit_name != function_statics[1]->compile_unit_name,
                "同名函数静态变量应保留编译单元边界");
    expect_true(function_statics[0]->scope_path != function_statics[1]->scope_path,
                "同名函数静态变量应保留各自函数作用域");
    expect_true(function_statics[0]->address.absolute_address.has_value() &&
                    function_statics[1]->address.absolute_address.has_value(),
                "函数静态变量应解析出绝对地址");

    const auto globals = find_variables_by_kind(model, "shared_global", elf_static_view::VariableKind::Global);
    expect_true(!globals.empty(), "多 CU fixture 应包含一个可见的全局变量定义");
    expect_true(globals.front()->address.absolute_address.has_value(), "全局变量定义应解析出绝对地址");
}

elf_static_view::ExpandedNode make_node(const std::string& path,
                                        const std::string& display_name,
                                        const std::string& type_name,
                                        const elf_static_view::Availability availability,
                                        const std::optional<std::uint64_t> absolute_address,
                                        const std::optional<std::int64_t> relative_offset)
{
    return elf_static_view::ExpandedNode{.path = path,
                                         .display_name = display_name,
                                         .type_name = type_name,
                                         .type_kind = elf_static_view::TypeKind::Base,
                                         .availability = availability,
                                         .absolute_address = absolute_address,
                                         .relative_offset = relative_offset,
                                         .byte_size = std::nullopt,
                                         .array_count = std::nullopt,
                                         .array_stride = std::nullopt,
                                         .children = {}};
}

void verify_filter_rules()
{
    elf_static_view::ui::AppState state;
    state.filters.form.variable_name_query = "shared";
    state.filters.form.path_rules_text = "demo::**\n!demo::counter";
    state.filters.form.include_runtime_only = false;
    state.filters.form.only_static_known = false;
    elf_static_view::ui::compile_filter_rules(state.filters);
    expect_true(!state.filters.compile_error.has_value(), "路径规则编译不应报错");

    const auto shared_node = make_node(
        "demo::shared", "shared", "int", elf_static_view::Availability::StaticAddressKnown, 0x1000, std::nullopt);
    const auto counter_node = make_node(
        "demo::counter", "counter", "int", elf_static_view::Availability::StaticAddressKnown, 0x1020, std::nullopt);
    const auto holder_node = make_node(
        "other::Holder", "Holder", "int", elf_static_view::Availability::StaticAddressKnown, 0x1040, std::nullopt);

    expect_true(elf_static_view::ui::matches_filters(state, shared_node), "shared 节点应命中过滤规则");
    expect_true(!elf_static_view::ui::matches_filters(state, counter_node), "counter 节点应被排除规则过滤");
    expect_true(!elf_static_view::ui::matches_filters(state, holder_node), "Holder 节点应被路径规则过滤");

    elf_static_view::ui::AppState cache_state;
    cache_state.project_model = elf_static_view::ProjectModel{};
    auto parent_node = make_node(
        "demo::holder", "holder", "Holder", elf_static_view::Availability::StaticAddressKnown, 0x2000, std::nullopt);
    parent_node.children.push_back(make_node("demo::holder.shared_value",
                                             "shared_value",
                                             "int",
                                             elf_static_view::Availability::StaticAddressKnown,
                                             0x2004,
                                             4));
    parent_node.children.push_back(make_node("demo::holder.runtime_value",
                                             "runtime_value",
                                             "int",
                                             elf_static_view::Availability::RuntimeOnly,
                                             std::nullopt,
                                             std::nullopt));
    cache_state.project_model->expanded.push_back(parent_node);

    elf_static_view::ui::compile_filter_rules(cache_state.filters);
    elf_static_view::ui::rebuild_filter_cache(cache_state);
    const auto empty_filter_rebuilds = cache_state.filters.cache.rebuild_count;
    expect_true(elf_static_view::ui::is_filter_cache_visible(cache_state, parent_node), "空筛选应保留根节点");
    expect_true(elf_static_view::ui::is_filter_cache_visible(cache_state, parent_node.children[0]),
                "空筛选应保留静态子节点");
    expect_true(!elf_static_view::ui::is_filter_cache_visible(cache_state, parent_node.children[1]),
                "默认筛选应隐藏仅运行时子节点");
    elf_static_view::ui::rebuild_filter_cache(cache_state);
    expect_true(cache_state.filters.cache.rebuild_count == empty_filter_rebuilds, "重复渲染查询不应重建筛选缓存");

    cache_state.filters.form.variable_name_query = "shared";
    elf_static_view::ui::compile_filter_rules(cache_state.filters);
    elf_static_view::ui::rebuild_filter_cache(cache_state);
    const auto name_filter_rebuilds = cache_state.filters.cache.rebuild_count;
    expect_true(name_filter_rebuilds == empty_filter_rebuilds + 1, "名称筛选变化后应失效并重建缓存");
    expect_true(elf_static_view::ui::is_filter_cache_visible(cache_state, parent_node),
                "名称命中子节点时应保留祖先节点");
    expect_true(elf_static_view::ui::is_filter_cache_visible(cache_state, parent_node.children[0]),
                "名称搜索应命中 display_name");
    expect_true(!elf_static_view::ui::is_filter_cache_visible(cache_state, parent_node.children[1]),
                "名称搜索不应保留未命中节点");

    cache_state.filters.form.variable_name_query = "holder.shared";
    elf_static_view::ui::compile_filter_rules(cache_state.filters);
    elf_static_view::ui::rebuild_filter_cache(cache_state);
    expect_true(elf_static_view::ui::is_filter_cache_visible(cache_state, parent_node.children[0]),
                "名称搜索应命中完整路径");

    cache_state.filters.form.variable_name_query.clear();
    cache_state.filters.form.path_rules_text = "demo::holder.*\n!demo::holder.runtime*";
    cache_state.filters.form.include_runtime_only = true;
    elf_static_view::ui::compile_filter_rules(cache_state.filters);
    elf_static_view::ui::rebuild_filter_cache(cache_state);
    expect_true(elf_static_view::ui::is_filter_cache_visible(cache_state, parent_node.children[0]),
                "include 路径规则应保留匹配节点");
    expect_true(!elf_static_view::ui::is_filter_cache_visible(cache_state, parent_node.children[1]),
                "exclude 路径规则应优先排除匹配节点");
}

void verify_filter_debounce_and_async_state()
{
    elf_static_view::ui::AppState state;
    state.filter_debounce_ms = 300;
    const auto start = std::chrono::steady_clock::now();

    state.filters.form.variable_name_query = "sha";
    elf_static_view::ui::mark_filter_text_changed(state, start);
    expect_true(!elf_static_view::ui::filter_debounce_elapsed(state, start + std::chrono::milliseconds(299)),
                "输入变化后 debounce 未到期不应启动筛选");
    expect_true(elf_static_view::ui::filter_debounce_elapsed(state, start + std::chrono::milliseconds(300)),
                "输入变化后 debounce 到期应允许启动筛选");

    elf_static_view::ui::mark_filter_text_changed(state, start + std::chrono::milliseconds(100));
    expect_true(!elf_static_view::ui::filter_debounce_elapsed(state, start + std::chrono::milliseconds(399)),
                "连续输入应刷新 debounce 起点");
    expect_true(elf_static_view::ui::filter_debounce_elapsed(state, start + std::chrono::milliseconds(400)),
                "连续输入只应在最后一次输入到期后启动筛选");

    state.filters.form.include_runtime_only = true;
    elf_static_view::ui::mark_filter_options_changed(state, start + std::chrono::milliseconds(500));
    expect_true(elf_static_view::ui::filter_debounce_elapsed(state, start + std::chrono::milliseconds(500)),
                "checkbox 变化应立即触发异步筛选");

    elf_static_view::ProjectModel model;
    model.expanded.push_back(
        make_node("demo::old", "old", "int", elf_static_view::Availability::StaticAddressKnown, 1, 0));
    model.expanded.push_back(
        make_node("demo::latest", "latest", "int", elf_static_view::Availability::StaticAddressKnown, 2, 0));
    state.project_model = model;
    state.filters.active_task_id = 2;
    state.filters.building = true;
    state.filters.has_pending_form = false;
    auto old_result = elf_static_view::ui::build_filter_cache(&state.project_model.value(),
                                                              elf_static_view::ui::FilterRuleSet{
                                                                  .variable_name_query = "old",
                                                                  .path_rules_text = {},
                                                                  .include_runtime_only = false,
                                                                  .only_static_known = false,
                                                              },
                                                              0,
                                                              true);
    expect_true(!elf_static_view::ui::receive_filter_build_result(state, 1, std::move(old_result)),
                "过期筛选任务结果不应覆盖最新缓存");
    expect_true(state.filters.building, "丢弃过期结果后仍应保持最新任务筛选中状态");

    auto latest_result = elf_static_view::ui::build_filter_cache(&state.project_model.value(),
                                                                 elf_static_view::ui::FilterRuleSet{
                                                                     .variable_name_query = "latest",
                                                                     .path_rules_text = {},
                                                                     .include_runtime_only = false,
                                                                     .only_static_known = false,
                                                                 },
                                                                 0,
                                                                 true);
    expect_true(elf_static_view::ui::receive_filter_build_result(state, 2, std::move(latest_result)),
                "最新筛选任务结果应被接收");
    expect_true(!state.filters.building, "最新结果接收后应结束筛选中状态");
    expect_true(state.filters.cache.visible_paths.find("demo::latest") != state.filters.cache.visible_paths.end(),
                "最新结果应写入可见路径缓存");
    expect_true(state.filters.cache.visible_paths.find("demo::old") == state.filters.cache.visible_paths.end(),
                "最新结果不应保留过期筛选路径");

    state.filters.cache.valid = false;
    expect_true(elf_static_view::ui::should_show_filter_progress(state), "缓存无效时变量树 UI 应显示筛选中状态");
}

void verify_address_bias()
{
    const auto node = make_node(
        "demo::global_value", "global_value", "int", elf_static_view::Availability::StaticAddressKnown, 0x1000, 24);

    expect_true(elf_static_view::apply_bias_to_absolute(node, 0x20).value() == 0x1020, "绝对地址偏移计算错误");
    expect_true(elf_static_view::apply_bias_to_relative(node, -4).value() == 20, "相对偏移计算错误");
    expect_true(elf_static_view::format_address_summary(node, 0x20) == "0x1020", "地址摘要应展示偏移后的绝对地址");

    const auto high_address_node = make_node("demo::high_address",
                                             "high_address",
                                             "int",
                                             elf_static_view::Availability::StaticAddressKnown,
                                             std::numeric_limits<std::uint64_t>::max() - 0x0fU,
                                             std::nullopt);
    expect_true(elf_static_view::apply_bias_to_absolute(high_address_node, 0x0f).value() ==
                    std::numeric_limits<std::uint64_t>::max(),
                "高地址加上最后一个偏移后应到达上限");
    expect_true(!elf_static_view::apply_bias_to_absolute(high_address_node, 0x10).has_value(),
                "绝对地址正向溢出应返回空");
    expect_true(elf_static_view::apply_bias_to_absolute(high_address_node, -0x10).value() ==
                    std::numeric_limits<std::uint64_t>::max() - 0x1fU,
                "绝对地址负向偏移应能正常计算");
}

void expect_parse_failure(const std::string& value, const std::string& message)
{
    bool failed = false;
    try {
        static_cast<void>(elf_static_view::parse_address_bias(value));
    } catch (const std::exception&) {
        failed = true;
    }
    expect_true(failed, message);
}

void verify_address_bias_parsing()
{
    expect_true(elf_static_view::parse_address_bias("0") == 0, "0 应按十进制解析");
    expect_true(elf_static_view::parse_address_bias("123") == 123, "十进制偏移解析错误");
    expect_true(elf_static_view::parse_address_bias("-16") == -16, "负十进制偏移解析错误");
    expect_true(elf_static_view::parse_address_bias("0x10") == 16, "十六进制偏移解析错误");
    expect_true(elf_static_view::parse_address_bias("0X20") == 32, "大写前缀十六进制解析错误");
    expect_true(elf_static_view::parse_address_bias("-0x10") == -16, "负十六进制偏移解析错误");
    expect_true(elf_static_view::parse_address_bias(" 42 ") == 42, "偏移解析应忽略首尾空白");
    expect_true(elf_static_view::parse_address_bias("-0x8000000000000000") == std::numeric_limits<std::int64_t>::min(),
                "INT64_MIN 十六进制值应能被解析");

    expect_parse_failure("", "空字符串应被拒绝");
    expect_parse_failure("0x", "只有十六进制前缀应被拒绝");
    expect_parse_failure("-0x", "只有负号和十六进制前缀应被拒绝");
    expect_parse_failure("abc", "非法字符输入应被拒绝");
    expect_parse_failure("12abc", "混合非法字符应被拒绝");
    expect_parse_failure("9223372036854775808", "超过 int64 正向范围应被拒绝");
    expect_parse_failure("-0x8000000000000001", "超过 int64 负向范围应被拒绝");
}

void verify_copy_address_formatting()
{
    elf_static_view::ui::AppState state;
    expect_true(elf_static_view::ui::format_address_for_copy(0x1234U, state) == "0x1234",
                "默认复制格式应为带 0x 前缀的十六进制");

    state.copy_hex_without_prefix = true;
    expect_true(elf_static_view::ui::format_address_for_copy(0x1234U, state) == "1234",
                "启用前缀移除后，十六进制复制结果不应带 0x");

    state.copy_address_base = elf_static_view::ui::CopyAddressBase::Dec;
    expect_true(elf_static_view::ui::format_address_for_copy(0x1234U, state) == "4660", "十进制复制结果不正确");

    state.copy_address_base = elf_static_view::ui::CopyAddressBase::Oct;
    expect_true(elf_static_view::ui::format_address_for_copy(0x1234U, state) == "11064", "八进制复制结果不正确");

    state.copy_address_base = elf_static_view::ui::CopyAddressBase::Bin;
    expect_true(elf_static_view::ui::format_address_for_copy(0x1234U, state) == "1001000110100",
                "二进制复制结果不正确");

    const auto negative_bias_node = make_node("demo::negative_bias",
                                              "negative_bias",
                                              "int",
                                              elf_static_view::Availability::StaticAddressKnown,
                                              0x108055a3U,
                                              std::nullopt);
    state.copy_address_base = elf_static_view::ui::CopyAddressBase::Hex;
    state.copy_hex_without_prefix = false;
    state.address_bias = -0x10800000;
    expect_true(elf_static_view::ui::format_adjusted_address_for_copy(negative_bias_node, state) ==
                    std::optional<std::string>("0x55a3"),
                "负偏移复制结果应保留符号并与展示一致");

    const auto positive_bias_node = make_node("demo::positive_bias",
                                              "positive_bias",
                                              "int",
                                              elf_static_view::Availability::StaticAddressKnown,
                                              0x55a3U,
                                              std::nullopt);
    state.address_bias = 0x20;
    expect_true(elf_static_view::ui::format_adjusted_address_for_copy(positive_bias_node, state) ==
                    std::optional<std::string>("0x55c3"),
                "正偏移复制结果应与展示一致");

    const auto overflow_node = make_node(
        "demo::overflow", "overflow", "int", elf_static_view::Availability::StaticAddressKnown, 0x10U, std::nullopt);
    state.address_bias = -0x20;
    expect_true(!elf_static_view::ui::format_adjusted_address_for_copy(overflow_node, state).has_value(),
                "偏移后地址下溢时复制 helper 应返回空");
}

void verify_window_title_formatting()
{
    elf_static_view::ui::AppState state;
    expect_true(elf_static_view::ui::build_window_title(state) ==
                    "ElfStaticView " + elf_static_view::ui::current_version_string(),
                "未加载文件时应显示默认标题");

    state.current_file_path = "workspace/demo/build/bin/app.elf";
    expect_true(elf_static_view::ui::build_window_title(state) == "ElfStaticView - .../build/bin/app.elf",
                "ELF 文件标题应显示缩写路径和文件名");

    state.current_snapshot_path = "snapshots/run/app.snapshot.json";
    expect_true(elf_static_view::ui::build_window_title(state) == "ElfStaticView - snapshots/run/app.snapshot.json",
                "快照标题应优先显示当前打开的快照路径");
}

void verify_utf8_path_helpers()
{
#if defined(_WIN32)
    constexpr auto sample_literal = u8"中文/静态变量.json";
    const std::string sample(reinterpret_cast<const char*>(sample_literal), sizeof(sample_literal) - 1);
    const std::wstring wide = elf_static_view::platform::utf8_to_wide(sample);
    expect_true(elf_static_view::platform::wide_to_utf8(wide) == sample, "UTF-8 与宽字符转换应可往返");

    const auto path = elf_static_view::platform::utf8_path(sample);
    expect_true(elf_static_view::platform::path_to_utf8(path) == sample, "UTF-8 路径转换应保留原始内容");
#endif
}

enum class FixtureClass {
    Elf32,
    Elf64,
};

enum class FixtureEndian {
    Little,
    Big,
};

void append_integer(std::vector<std::uint8_t>& bytes,
                    const std::uint64_t value,
                    const std::size_t size,
                    const FixtureEndian endian)
{
    if (endian == FixtureEndian::Little) {
        for (std::size_t index = 0; index < size; ++index) {
            bytes.push_back(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
        }
        return;
    }

    for (std::size_t index = 0; index < size; ++index) {
        const auto shift = static_cast<unsigned int>((size - index - 1U) * 8U);
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u16(std::vector<std::uint8_t>& bytes, const std::uint16_t value, const FixtureEndian endian)
{
    append_integer(bytes, value, sizeof(value), endian);
}

void append_u32(std::vector<std::uint8_t>& bytes, const std::uint32_t value, const FixtureEndian endian)
{
    append_integer(bytes, value, sizeof(value), endian);
}

void append_u64(std::vector<std::uint8_t>& bytes, const std::uint64_t value, const FixtureEndian endian)
{
    append_integer(bytes, value, sizeof(value), endian);
}

void overwrite_u32_le(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value)
{
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

void write_padding(std::vector<std::uint8_t>& bytes, const std::size_t target_size)
{
    if (bytes.size() < target_size) {
        bytes.resize(target_size, 0U);
    }
}

void append_elf_header(std::vector<std::uint8_t>& bytes,
                       const FixtureClass elf_class,
                       const FixtureEndian endian,
                       const std::uint64_t section_header_offset,
                       const std::uint16_t section_header_size,
                       const std::uint16_t machine = 0U)
{
    bytes.push_back(0x7fU);
    bytes.push_back(static_cast<std::uint8_t>('E'));
    bytes.push_back(static_cast<std::uint8_t>('L'));
    bytes.push_back(static_cast<std::uint8_t>('F'));
    bytes.push_back(elf_class == FixtureClass::Elf32 ? 1U : 2U);
    bytes.push_back(endian == FixtureEndian::Little ? 1U : 2U);
    bytes.push_back(1U);
    bytes.push_back(0U);
    bytes.resize(16, 0U);

    append_u16(bytes, 1U, endian);
    append_u16(bytes, machine != 0U ? machine : (elf_class == FixtureClass::Elf32 ? 3U : 62U), endian);
    append_u32(bytes, 1U, endian);
    if (elf_class == FixtureClass::Elf32) {
        append_u32(bytes, 0U, endian);
        append_u32(bytes, 0U, endian);
        append_u32(bytes, static_cast<std::uint32_t>(section_header_offset), endian);
        append_u32(bytes, 0U, endian);
        append_u16(bytes, 52U, endian);
    } else {
        append_u64(bytes, 0U, endian);
        append_u64(bytes, 0U, endian);
        append_u64(bytes, section_header_offset, endian);
        append_u32(bytes, 0U, endian);
        append_u16(bytes, 64U, endian);
    }
    append_u16(bytes, 0U, endian);
    append_u16(bytes, 0U, endian);
    append_u16(bytes, section_header_size, endian);
    append_u16(bytes, 3U, endian);
    append_u16(bytes, 0U, endian);
}

void append_section_header(std::vector<std::uint8_t>& bytes,
                           const FixtureClass elf_class,
                           const FixtureEndian endian,
                           const std::uint32_t type,
                           const std::uint64_t offset,
                           const std::uint64_t size,
                           const std::uint32_t link,
                           const std::uint64_t entry_size)
{
    append_u32(bytes, 0U, endian);
    append_u32(bytes, type, endian);
    if (elf_class == FixtureClass::Elf32) {
        append_u32(bytes, 0U, endian);
        append_u32(bytes, 0U, endian);
        append_u32(bytes, static_cast<std::uint32_t>(offset), endian);
        append_u32(bytes, static_cast<std::uint32_t>(size), endian);
        append_u32(bytes, link, endian);
        append_u32(bytes, 0U, endian);
        append_u32(bytes, 0U, endian);
        append_u32(bytes, static_cast<std::uint32_t>(entry_size), endian);
        return;
    }

    append_u64(bytes, 0U, endian);
    append_u64(bytes, 0U, endian);
    append_u64(bytes, offset, endian);
    append_u64(bytes, size, endian);
    append_u32(bytes, link, endian);
    append_u32(bytes, 0U, endian);
    append_u64(bytes, 0U, endian);
    append_u64(bytes, entry_size, endian);
}

void append_symbol(std::vector<std::uint8_t>& bytes,
                   const FixtureClass elf_class,
                   const FixtureEndian endian,
                   const std::uint32_t name_offset,
                   const std::uint64_t value,
                   const std::uint64_t size,
                   const std::uint8_t info,
                   const std::uint16_t section_index)
{
    append_u32(bytes, name_offset, endian);
    if (elf_class == FixtureClass::Elf32) {
        append_u32(bytes, static_cast<std::uint32_t>(value), endian);
        append_u32(bytes, static_cast<std::uint32_t>(size), endian);
        bytes.push_back(info);
        bytes.push_back(0U);
        append_u16(bytes, section_index, endian);
        return;
    }

    bytes.push_back(info);
    bytes.push_back(0U);
    append_u16(bytes, section_index, endian);
    append_u64(bytes, value, endian);
    append_u64(bytes, size, endian);
}

std::vector<std::uint8_t> build_symbol_table_fixture(const FixtureClass elf_class,
                                                     const FixtureEndian endian,
                                                     const std::uint64_t symbol_value)
{
    const std::size_t header_size = elf_class == FixtureClass::Elf32 ? 52U : 64U;
    const std::size_t symbol_entry_size = elf_class == FixtureClass::Elf32 ? 16U : 24U;
    const std::size_t section_header_size = elf_class == FixtureClass::Elf32 ? 40U : 64U;
    const std::vector<std::uint8_t> string_table = {0U, 'd', 'e', 'm', 'o', '_', 'v', 'a', 'l', 'u', 'e', 0U};
    const std::size_t symbol_table_offset = header_size;
    const std::size_t string_table_offset = symbol_table_offset + symbol_entry_size;
    const std::size_t section_header_offset = string_table_offset + string_table.size();

    std::vector<std::uint8_t> bytes;
    append_elf_header(bytes,
                      elf_class,
                      endian,
                      static_cast<std::uint64_t>(section_header_offset),
                      static_cast<std::uint16_t>(section_header_size));
    write_padding(bytes, symbol_table_offset);
    append_symbol(bytes, elf_class, endian, 1U, symbol_value, elf_class == FixtureClass::Elf32 ? 4U : 8U, 1U, 1U);
    write_padding(bytes, string_table_offset);
    bytes.insert(bytes.end(), string_table.begin(), string_table.end());
    write_padding(bytes, section_header_offset);
    append_section_header(bytes, elf_class, endian, 0U, 0U, 0U, 0U, 0U);
    append_section_header(bytes,
                          elf_class,
                          endian,
                          2U,
                          static_cast<std::uint64_t>(symbol_table_offset),
                          static_cast<std::uint64_t>(symbol_entry_size),
                          2U,
                          static_cast<std::uint64_t>(symbol_entry_size));
    append_section_header(bytes,
                          elf_class,
                          endian,
                          3U,
                          static_cast<std::uint64_t>(string_table_offset),
                          static_cast<std::uint64_t>(string_table.size()),
                          0U,
                          0U);
    return bytes;
}

std::vector<std::uint8_t> build_metadata_fixture(const FixtureClass elf_class,
                                                 const FixtureEndian endian,
                                                 const std::uint16_t machine)
{
    const std::size_t header_size = elf_class == FixtureClass::Elf32 ? 52U : 64U;
    const std::size_t section_header_size = elf_class == FixtureClass::Elf32 ? 40U : 64U;

    std::vector<std::uint8_t> bytes;
    append_elf_header(bytes,
                      elf_class,
                      endian,
                      static_cast<std::uint64_t>(header_size),
                      static_cast<std::uint16_t>(section_header_size),
                      machine);
    write_padding(bytes, header_size);
    append_section_header(bytes, elf_class, endian, 0U, 0U, 0U, 0U, 0U);
    append_section_header(bytes, elf_class, endian, 0U, 0U, 0U, 0U, 0U);
    append_section_header(bytes, elf_class, endian, 0U, 0U, 0U, 0U, 0U);
    return bytes;
}

void append_ti_coff_section_header(std::vector<std::uint8_t>& bytes,
                                   const std::uint32_t name_string_offset,
                                   const std::uint32_t size,
                                   const std::uint32_t file_offset)
{
    append_u32(bytes, 0U, FixtureEndian::Little);
    append_u32(bytes, name_string_offset, FixtureEndian::Little);
    append_u32(bytes, 0U, FixtureEndian::Little);
    append_u32(bytes, 0U, FixtureEndian::Little);
    append_u32(bytes, size, FixtureEndian::Little);
    append_u32(bytes, file_offset, FixtureEndian::Little);
    append_u32(bytes, 0U, FixtureEndian::Little);
    append_u32(bytes, 0U, FixtureEndian::Little);
    append_u16(bytes, 0U, FixtureEndian::Little);
    append_u16(bytes, 0U, FixtureEndian::Little);
    append_u16(bytes, 0U, FixtureEndian::Little);
    write_padding(bytes, bytes.size() + 10U);
}

std::vector<std::uint8_t> build_ti_coff_debug_fixture()
{
    constexpr std::size_t header_size = 22U;
    constexpr std::size_t section_header_size = 48U;
    constexpr std::size_t section_count = 2U;
    const std::vector<std::uint8_t> debug_info = {0x11U, 0x22U, 0x33U};
    const std::vector<std::uint8_t> debug_abbrev = {0x44U, 0x55U};
    const std::size_t debug_info_offset = header_size + section_header_size * section_count;
    const std::size_t debug_abbrev_offset = debug_info_offset + debug_info.size();
    const std::size_t string_table_offset = debug_abbrev_offset + debug_abbrev.size();
    const std::string debug_info_name = ".debug_info";
    const std::string debug_abbrev_name = ".debug_abbrev";
    const std::uint32_t debug_info_name_offset = 4U;
    const std::uint32_t debug_abbrev_name_offset =
        debug_info_name_offset + static_cast<std::uint32_t>(debug_info_name.size() + 1U);
    const std::uint32_t string_table_size =
        debug_abbrev_name_offset + static_cast<std::uint32_t>(debug_abbrev_name.size() + 1U);

    std::vector<std::uint8_t> bytes;
    append_u16(bytes, 0x00c2U, FixtureEndian::Little);
    append_u16(bytes, static_cast<std::uint16_t>(section_count), FixtureEndian::Little);
    append_u32(bytes, 0U, FixtureEndian::Little);
    append_u32(bytes, static_cast<std::uint32_t>(string_table_offset), FixtureEndian::Little);
    append_u32(bytes, 0U, FixtureEndian::Little);
    append_u16(bytes, 0U, FixtureEndian::Little);
    append_u16(bytes, 0U, FixtureEndian::Little);
    write_padding(bytes, header_size);

    append_ti_coff_section_header(bytes,
                                  debug_info_name_offset,
                                  static_cast<std::uint32_t>(debug_info.size()),
                                  static_cast<std::uint32_t>(debug_info_offset));
    append_ti_coff_section_header(bytes,
                                  debug_abbrev_name_offset,
                                  static_cast<std::uint32_t>(debug_abbrev.size()),
                                  static_cast<std::uint32_t>(debug_abbrev_offset));
    bytes.insert(bytes.end(), debug_info.begin(), debug_info.end());
    bytes.insert(bytes.end(), debug_abbrev.begin(), debug_abbrev.end());
    append_u32(bytes, string_table_size, FixtureEndian::Little);
    bytes.insert(bytes.end(), debug_info_name.begin(), debug_info_name.end());
    bytes.push_back(0U);
    bytes.insert(bytes.end(), debug_abbrev_name.begin(), debug_abbrev_name.end());
    bytes.push_back(0U);
    overwrite_u32_le(bytes, string_table_offset, string_table_size);
    return bytes;
}

void verify_ti_coff_object_section_indices()
{
    const auto path = std::filesystem::temp_directory_path() / "elf_static_view_ti_coff_debug_sections.out";
    const auto bytes = build_ti_coff_debug_fixture();
    {
        std::ofstream output(path, std::ios::binary);
        if (!output.is_open()) {
            throw std::runtime_error("无法写入 TI-COFF fixture: " + path.string());
        }
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            throw std::runtime_error("写入 TI-COFF fixture 失败: " + path.string());
        }
    }

    std::error_code remove_error;
    try {
        elf_static_view::elf::TiCoffObject object(path.string());
        expect_true(object.section_count() == 3U, "TI-COFF section_count 应包含 libdwarf 保留 0 号空段");
        expect_true(object.section_at(0U) == nullptr, "TI-COFF 0 号 section 应保持空段语义");
        const auto* debug_info_section = object.section_at(1U);
        expect_true(debug_info_section != nullptr, "TI-COFF 1 号 section 应指向第一个真实 debug section");
        expect_true(debug_info_section->name == ".debug_info", "TI-COFF 1 号 section 应为 .debug_info");
        const auto* debug_abbrev_section = object.section_at(2U);
        expect_true(debug_abbrev_section != nullptr, "TI-COFF 2 号 section 应指向第二个真实 debug section");
        expect_true(debug_abbrev_section->name == ".debug_abbrev", "TI-COFF 2 号 section 应为 .debug_abbrev");
        expect_true(object.section_at(3U) == nullptr, "TI-COFF 超出 section_count 的索引应返回空");
        expect_true(object.read_section_data(1U) == std::vector<std::uint8_t>({0x11U, 0x22U, 0x33U}),
                    "TI-COFF 读取 1 号 section 应返回第一个真实 debug section 数据");
        expect_true(object.missing_required_debug_sections().empty(),
                    "TI-COFF 必需 debug section 检查不应受保留空段影响");

        bool rejected_reserved_section = false;
        try {
            (void) object.read_section_data(0U);
        } catch (const std::exception&) {
            rejected_reserved_section = true;
        }
        expect_true(rejected_reserved_section, "TI-COFF 读取 0 号保留空段应返回受控错误");
    } catch (...) {
        std::filesystem::remove(path, remove_error);
        throw;
    }
    std::filesystem::remove(path, remove_error);
}

void verify_elf_symbol_table_endian_matrix()
{
    struct Case {
        FixtureClass elf_class;
        FixtureEndian endian;
        std::uint64_t value;
    };

    const std::vector<Case> cases = {
        {FixtureClass::Elf32, FixtureEndian::Little, 0x10203040ULL},
        {FixtureClass::Elf32, FixtureEndian::Big, 0x55667788ULL},
        {FixtureClass::Elf64, FixtureEndian::Little, 0x0102030405060708ULL},
        {FixtureClass::Elf64, FixtureEndian::Big, 0x8877665544332211ULL},
    };

    for (const auto& test_case : cases) {
        const auto bytes = build_symbol_table_fixture(test_case.elf_class, test_case.endian, test_case.value);
        const auto path = std::filesystem::temp_directory_path() /
                          ("elf_static_view_" + std::to_string(static_cast<int>(test_case.elf_class)) + "_" +
                           std::to_string(static_cast<int>(test_case.endian)) + ".bin");

        {
            std::ofstream output(path, std::ios::binary);
            if (!output.is_open()) {
                throw std::runtime_error("无法写入 ELF matrix fixture: " + path.string());
            }
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!output) {
                throw std::runtime_error("写入 ELF matrix fixture 失败: " + path.string());
            }
        }

        std::error_code remove_error;
        try {
            const auto table = elf_static_view::elf::ElfSymbolTable::load(path.string());
            const auto symbol = table.find("demo_value");
            expect_true(symbol.has_value(), "ELF matrix fixture 应能读到 demo_value");
            expect_true(symbol->value == test_case.value, "ELF matrix fixture 符号值解码错误");
        } catch (...) {
            std::filesystem::remove(path, remove_error);
            throw;
        }
        std::filesystem::remove(path, remove_error);
    }
}

void verify_elf_symbol_table_metadata()
{
    const auto bytes = build_symbol_table_fixture(FixtureClass::Elf64, FixtureEndian::Big, 0x1020ULL);
    const auto path = std::filesystem::temp_directory_path() / "elf_static_view_metadata.bin";
    {
        std::ofstream output(path, std::ios::binary);
        if (!output.is_open()) {
            throw std::runtime_error("无法写入 ELF metadata fixture: " + path.string());
        }
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    std::error_code remove_error;
    try {
        const auto metadata = elf_static_view::elf::ElfSymbolTable::inspect_file(path.string());
        expect_true(metadata.object_class == "ELF64", "应识别 ELF64");
        expect_true(metadata.byte_order == "BigEndian", "应识别大端");
        expect_true(metadata.file_type == "REL", "应识别 REL 类型");
    } catch (...) {
        std::filesystem::remove(path, remove_error);
        throw;
    }
    std::filesystem::remove(path, remove_error);
}

void verify_c2000_elf_metadata()
{
    const auto bytes = build_metadata_fixture(FixtureClass::Elf32, FixtureEndian::Little, 141U);
    const auto path = std::filesystem::temp_directory_path() / "elf_static_view_c2000_metadata.bin";
    {
        std::ofstream output(path, std::ios::binary);
        if (!output.is_open()) {
            throw std::runtime_error("无法写入 C2000 ELF metadata fixture: " + path.string());
        }
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    std::error_code remove_error;
    try {
        const auto metadata = elf_static_view::elf::ElfSymbolTable::inspect_file(path.string());
        expect_true(metadata.object_class == "ELF32", "C2000 fixture 应识别 ELF32");
        expect_true(metadata.byte_order == "LittleEndian", "C2000 fixture 应按 EI_DATA 保持小端");
        expect_true(metadata.machine == "TI TMS320C2000", "C2000 fixture 应识别 e_machine 141");
    } catch (...) {
        std::filesystem::remove(path, remove_error);
        throw;
    }
    std::filesystem::remove(path, remove_error);
}

void verify_snapshot_export_redaction()
{
    elf_static_view::ProjectSnapshot snapshot;
    snapshot.source_file = "D:/secret/demo.elf";
    snapshot.exported_at = "2026-05-24T10:00:00Z";
    snapshot.model.file = "D:/secret/demo.elf";
    snapshot.model.elf_info = {.object_class = "ELF64",
                               .byte_order = "LittleEndian",
                               .file_type = "EXEC",
                               .machine = "x86_64",
                               .os_abi = "SystemV"};
    elf_static_view::CompileUnitRecord compile_unit;
    compile_unit.id = "cu@0";
    compile_unit.name = "D:/src/demo.cpp";
    compile_unit.producer = "clang -g -O2";
    compile_unit.language = "C++";
    snapshot.model.compile_units.push_back(std::move(compile_unit));
    elf_static_view::VariableRecord symbol;
    symbol.id = "sym@0";
    symbol.name = "demo";
    symbol.compile_unit_name = "D:/src/demo.cpp";
    symbol.variable_kind = elf_static_view::VariableKind::Global;
    symbol.availability = elf_static_view::Availability::StaticAddressKnown;
    symbol.address.kind = elf_static_view::AddressKind::Absolute;
    symbol.address.absolute_address = 0x401000;
    symbol.type.id = "type@0";
    symbol.scope_path = {"demo"};
    symbol.has_static_storage = true;
    snapshot.model.symbols.push_back(symbol);

    const auto redacted = elf_static_view::build_export_snapshot(snapshot, {.include_sensitive_info = false});
    expect_true(redacted.source_file == "<redacted>", "脱敏快照应隐藏 source_file");
    expect_true(redacted.model.file == "<redacted>", "脱敏快照应隐藏 model.file");
    expect_true(redacted.model.compile_units.front().producer.empty(), "脱敏快照应清空 producer");
    expect_true(redacted.model.compile_units.front().name.empty(), "脱敏快照应清空 CU 名称");
    expect_true(redacted.model.symbols.front().compile_unit_name.empty(), "脱敏快照应清空变量 compile_unit_name");
    expect_true(redacted.model.symbols.front().address.absolute_address == 0x401000, "脱敏快照应保留地址信息");
}

void verify_lightweight_export_round_trip()
{
    elf_static_view::ProjectModel model;
    elf_static_view::TypeNode type;
    type.id = "type@int";
    type.name = "int";
    model.types.push_back(type);
    elf_static_view::VariableRecord symbol;
    symbol.id = "D:/secret/source.cpp::demo";
    symbol.name = "demo变量";
    symbol.type.id = "type@int";
    symbol.address.absolute_address = 0x401000;
    model.symbols.push_back(symbol);

    const elf_static_view::ExportOptions json_options{
        elf_static_view::ExportFormat::JsonCompact,
        elf_static_view::ExportSource::FullModel,
        elf_static_view::ExportPayloadKind::VariableSummary,
        false,
    };
    const auto lightweight = elf_static_view::build_lightweight_export(model, json_options);
    expect_true(lightweight.variables.size() == 1, "精简导出应包含变量记录");
    expect_true(lightweight.variables.front().path == "demo变量", "脱敏精简导出应保留变量逻辑路径");

    const elf_static_view::ExportDocument json_document{
        elf_static_view::ExportPayloadKind::VariableSummary,
        lightweight,
    };
    const auto compact_json = elf_static_view::render_export_document(json_document, json_options);
    expect_true(compact_json.find('\n') == std::string::npos, "紧凑 JSON 不应包含换行");
    const auto parsed_json = elf_static_view::parse_export_bytes(compact_json, json_options.format);
    const auto parsed_lightweight = std::get<elf_static_view::LightweightExport>(parsed_json.payload);
    expect_true(parsed_lightweight.variables.front().name == "demo变量", "精简 JSON 导入应保留变量名");
    expect_true(parsed_lightweight.variables.front().address == 0x401000, "精简 JSON 导入应保留地址");

    const elf_static_view::ExportOptions binary_options{
        elf_static_view::ExportFormat::BinaryPrivate,
        elf_static_view::ExportSource::CurrentFilteredView,
        elf_static_view::ExportPayloadKind::VariableSummary,
        true,
    };
    const elf_static_view::ExportDocument binary_document{
        elf_static_view::ExportPayloadKind::VariableSummary,
        elf_static_view::LightweightExport{
            1,
            elf_static_view::ExportSource::CurrentFilteredView,
            true,
            lightweight.variables,
        },
    };
    const auto binary = elf_static_view::render_export_document(binary_document, binary_options);
    expect_true(binary.size() < compact_json.size() + 64, "二进制精简导出应保持较小体积");
    const auto parsed_binary = elf_static_view::parse_export_bytes(binary, binary_options.format);
    const auto parsed_binary_lightweight = std::get<elf_static_view::LightweightExport>(parsed_binary.payload);
    expect_true(parsed_binary_lightweight.source == elf_static_view::ExportSource::CurrentFilteredView,
                "二进制导入应保留导出来源");
    expect_true(parsed_binary_lightweight.variables.front().type_name == "int", "二进制导入应保留类型名");
}

void verify_lightweight_export_expands_nodes_and_limits_arrays()
{
    elf_static_view::ProjectModel model;
    elf_static_view::TypeNode element_type;
    element_type.id = "type@int";
    element_type.name = "int";
    element_type.byte_size = 4;
    model.types.push_back(element_type);

    elf_static_view::TypeNode array_type;
    array_type.id = "type@array";
    array_type.kind = elf_static_view::TypeKind::Array;
    array_type.element_type = elf_static_view::TypeRef{"type@int"};
    array_type.array_dimensions = {3};
    array_type.byte_size = 12;
    model.types.push_back(array_type);

    elf_static_view::VariableRecord symbol;
    symbol.id = "sym@array";
    symbol.name = "numbers";
    symbol.type.id = "type@array";
    symbol.availability = elf_static_view::Availability::StaticAddressKnown;
    symbol.address.absolute_address = 0x5000;
    model.symbols.push_back(symbol);

    elf_static_view::ExportOptions limited_options;
    limited_options.payload_kind = elf_static_view::ExportPayloadKind::VariableSummary;
    limited_options.lightweight_max_array_elements = 2;
    const auto limited = elf_static_view::build_lightweight_export(model, limited_options);
    expect_true(limited.variables.size() == 3, "精简导出应包含父数组和前 N 个数组元素");
    expect_true(limited.variables[0].path == "numbers", "精简导出应保留父数组路径");
    expect_true(limited.variables[1].path == "numbers[0]", "精简导出应展开数组元素 0");
    expect_true(limited.variables[2].path == "numbers[1]", "精简导出应按上限截断数组元素");

    limited_options.lightweight_max_array_elements = 0;
    const auto unlimited = elf_static_view::build_lightweight_export(model, limited_options);
    expect_true(unlimited.variables.size() == 4, "数组上限为 0 时应不限制展开元素");
    expect_true(unlimited.variables[3].path == "numbers[2]", "不限制时应导出最后一个数组元素");
}

void verify_lightweight_export_keeps_sanitized_member_paths()
{
    elf_static_view::ProjectModel model;

    elf_static_view::ExpandedNode object_node;
    object_node.path = "root.object";
    object_node.display_name = "object";
    object_node.type_name = "Object";
    object_node.type_kind = elf_static_view::TypeKind::Struct;
    object_node.absolute_address = 0x6000;

    elf_static_view::ExpandedNode member_node;
    member_node.path = "root.object.member";
    member_node.display_name = "member";
    member_node.type_name = "int";
    member_node.type_kind = elf_static_view::TypeKind::Base;
    member_node.absolute_address = 0x6004;
    object_node.children.push_back(member_node);

    elf_static_view::ExpandedNode array_node;
    array_node.path = "root.array";
    array_node.display_name = "array";
    array_node.type_name = "int[2]";
    array_node.type_kind = elf_static_view::TypeKind::Array;
    array_node.absolute_address = 0x7000;

    for (std::uint64_t index = 0; index < 2; ++index) {
        elf_static_view::ExpandedNode element_node;
        element_node.path = "root.array[" + std::to_string(index) + "]";
        element_node.display_name = "array[" + std::to_string(index) + "]";
        element_node.type_name = "int";
        element_node.type_kind = elf_static_view::TypeKind::Base;
        element_node.absolute_address = 0x7000 + index * 4;
        array_node.children.push_back(std::move(element_node));
    }

    elf_static_view::ExpandedNode struct_array_element_node;
    struct_array_element_node.path = "e_var_struct[0]";
    struct_array_element_node.display_name = "e_var_struct[0]";
    struct_array_element_node.type_name = "ExampleStruct";
    struct_array_element_node.type_kind = elf_static_view::TypeKind::Struct;
    struct_array_element_node.absolute_address = 0x8000;
    elf_static_view::ExpandedNode struct_array_member_node;
    struct_array_member_node.path = "e_var_struct[0].name";
    struct_array_member_node.display_name = "name";
    struct_array_member_node.type_name = "char[16]";
    struct_array_member_node.type_kind = elf_static_view::TypeKind::Array;
    struct_array_member_node.absolute_address = 0x8004;
    struct_array_element_node.children.push_back(std::move(struct_array_member_node));

    model.expanded = {std::move(object_node), std::move(array_node), std::move(struct_array_element_node)};

    elf_static_view::ExportOptions options;
    options.payload_kind = elf_static_view::ExportPayloadKind::VariableSummary;
    options.include_sensitive_info = false;
    const auto lightweight = elf_static_view::build_lightweight_export(model, options);

    expect_true(lightweight.variables.size() == 7, "脱敏精简导出应递归收集成员和数组元素");
    expect_true(lightweight.variables[1].path == "root.object.member", "脱敏精简导出成员路径不应降级为显示名");
    expect_true(lightweight.variables[1].name == "root.object.member", "脱敏精简导出成员名称应使用完整逻辑路径");
    expect_true(lightweight.variables[3].path == "root.array[0]", "脱敏精简导出数组元素 0 应保留完整逻辑路径");
    expect_true(lightweight.variables[3].name == "root.array[0]", "脱敏精简导出数组元素名称应使用完整逻辑路径");
    expect_true(lightweight.variables[4].path == "root.array[1]", "脱敏精简导出数组元素 1 应保留完整逻辑路径");
    expect_true(lightweight.variables[6].path == "e_var_struct[0].name",
                "脱敏精简导出结构体数组成员路径应包含父数组元素");
    expect_true(lightweight.variables[6].name == "e_var_struct[0].name",
                "脱敏精简导出结构体数组成员名称应包含父数组元素");

    const elf_static_view::ExportDocument json_document{
        elf_static_view::ExportPayloadKind::VariableSummary,
        lightweight,
    };
    options.format = elf_static_view::ExportFormat::JsonCompact;
    const auto compact_json = elf_static_view::render_export_document(json_document, options);
    expect_true(compact_json.find("\"root.object.member\"") != std::string::npos,
                "紧凑 JSON 精简导出应写出完整成员逻辑路径");
    const auto parsed_compact_json = elf_static_view::parse_export_bytes(compact_json, options.format);
    const auto parsed_compact_lightweight = std::get<elf_static_view::LightweightExport>(parsed_compact_json.payload);
    expect_true(parsed_compact_lightweight.variables[6].name == "e_var_struct[0].name",
                "紧凑 JSON 精简导出成员名称应写出完整逻辑路径");
    options.format = elf_static_view::ExportFormat::JsonPretty;
    const auto pretty_json = elf_static_view::render_export_document(json_document, options);
    expect_true(pretty_json.find("\"root.array[0]\"") != std::string::npos,
                "格式化 JSON 精简导出应写出完整数组元素逻辑路径");
    const auto parsed_pretty_json = elf_static_view::parse_export_bytes(pretty_json, options.format);
    const auto parsed_pretty_lightweight = std::get<elf_static_view::LightweightExport>(parsed_pretty_json.payload);
    expect_true(parsed_pretty_lightweight.variables[6].name == "e_var_struct[0].name",
                "格式化 JSON 精简导出成员名称应写出完整逻辑路径");
    options.format = elf_static_view::ExportFormat::BinaryPrivate;
    const auto binary = elf_static_view::render_export_document(json_document, options);
    const auto parsed_binary = elf_static_view::parse_export_bytes(binary, options.format);
    const auto parsed_lightweight = std::get<elf_static_view::LightweightExport>(parsed_binary.payload);
    expect_true(parsed_lightweight.variables[1].path == "root.object.member",
                "私有二进制精简导出应写出完整成员逻辑路径");
    expect_true(parsed_lightweight.variables[3].path == "root.array[0]",
                "私有二进制精简导出应写出完整数组元素逻辑路径");
    expect_true(parsed_lightweight.variables[6].name == "e_var_struct[0].name",
                "私有二进制精简导出成员名称应写出完整逻辑路径");
}

void verify_import_project_data_auto_and_lightweight_model()
{
    elf_static_view::LightweightExport lightweight;
    lightweight.variables.push_back(elf_static_view::LightweightVariableRecord{
        .path = "root.member",
        .name = "member",
        .type_name = "int",
        .address = 0x401000,
    });
    lightweight.variables.push_back(elf_static_view::LightweightVariableRecord{
        .path = "root.no_address",
        .name = "no_address",
        .type_name = "char",
        .address = std::nullopt,
    });

    elf_static_view::ExportOptions json_options;
    json_options.format = elf_static_view::ExportFormat::JsonCompact;
    json_options.payload_kind = elf_static_view::ExportPayloadKind::VariableSummary;
    const elf_static_view::ExportDocument json_document{
        elf_static_view::ExportPayloadKind::VariableSummary,
        lightweight,
    };
    const auto compact_json = elf_static_view::render_export_document(json_document, json_options);
    const auto imported_json = elf_static_view::import_project_data_bytes(compact_json, "summary.json");
    expect_true(imported_json.payload_kind == elf_static_view::ExportPayloadKind::VariableSummary,
                "自动导入应识别紧凑 JSON 精简变量");
    expect_true(imported_json.snapshot.source_kind == "elf-static-view-lightweight", "精简导入应标记轻量来源");
    expect_true(imported_json.snapshot.model.expanded.size() == 2, "精简导入应构造展开节点");
    expect_true(
        imported_json.snapshot.model.expanded.front().availability == elf_static_view::Availability::StaticAddressKnown,
        "带地址精简节点应可参与静态地址查询");
    expect_true(imported_json.snapshot.model.expanded.back().availability == elf_static_view::Availability::Unavailable,
                "无地址精简节点应保留但不参与静态地址查询");

    const auto query_results = elf_static_view::query_static_addresses(imported_json.snapshot.model);
    expect_true(query_results.size() == 1, "静态地址查询应跳过无地址精简节点");
    expect_true(query_results.front().key == "root.member", "静态地址查询应返回精简节点路径");

    elf_static_view::LightweightExport duplicate_paths;
    duplicate_paths.variables.push_back(elf_static_view::LightweightVariableRecord{
        .path = "root.member",
        .name = "member",
        .type_name = "int",
        .address = 0x401000,
    });
    duplicate_paths.variables.push_back(elf_static_view::LightweightVariableRecord{
        .path = "root.member",
        .name = "member",
        .type_name = "int",
        .address = 0x401004,
    });
    duplicate_paths.variables.push_back(elf_static_view::LightweightVariableRecord{
        .path = "root.member",
        .name = "member",
        .type_name = "int",
        .address = 0x401008,
    });
    const auto duplicate_model =
        elf_static_view::build_lightweight_project_model(duplicate_paths, "duplicate-summary.esv");
    expect_true(duplicate_model.expanded.size() == 3, "重复 path 精简导入不应丢弃节点");
    expect_true(duplicate_model.expanded[0].path == "root.member", "首个重复 path 应保持原样");
    expect_true(duplicate_model.expanded[1].path == "root.member#2", "第二个重复 path 应追加兼容后缀");
    expect_true(duplicate_model.expanded[2].path == "root.member#3", "第三个重复 path 应追加兼容后缀");
    expect_true(duplicate_model.expanded[1].display_name == "member", "重复 path 去重不应改变用户看到的显示名");
    const auto duplicate_results = elf_static_view::query_static_addresses(duplicate_model);
    expect_true(duplicate_results.size() == 3, "静态地址查询应返回重复节点各自的唯一 key");
    expect_true(static_address_result_exists(duplicate_results, "root.member#2"),
                "静态地址查询应能返回第二个重复节点的兼容 key");
    elf_static_view::ExportOptions duplicate_export_options;
    duplicate_export_options.format = elf_static_view::ExportFormat::JsonCompact;
    duplicate_export_options.payload_kind = elf_static_view::ExportPayloadKind::VariableSummary;
    const auto duplicate_reexport =
        elf_static_view::build_lightweight_export(duplicate_model.expanded, duplicate_export_options);
    expect_true(duplicate_reexport.variables.size() == 3, "重复 path 再导出不应丢失节点");
    expect_true(duplicate_reexport.variables[0].path == "root.member", "重复 path 再导出首个节点应使用逻辑路径");
    expect_true(duplicate_reexport.variables[0].name == "root.member", "重复 path 再导出首个节点名称应使用逻辑路径");
    expect_true(duplicate_reexport.variables[1].path == "root.member",
                "重复 path 再导出第二个节点不应泄漏 #2 兼容后缀");
    expect_true(duplicate_reexport.variables[1].name == "root.member",
                "重复 path 再导出第二个节点名称不应泄漏 #2 兼容后缀");
    expect_true(duplicate_reexport.variables[2].path == "root.member",
                "重复 path 再导出第三个节点不应泄漏 #3 兼容后缀");
    const elf_static_view::ExportDocument duplicate_document{
        elf_static_view::ExportPayloadKind::VariableSummary,
        duplicate_reexport,
    };
    const auto duplicate_json = elf_static_view::render_export_document(duplicate_document, duplicate_export_options);
    expect_true(duplicate_json.find("#2") == std::string::npos && duplicate_json.find("#3") == std::string::npos,
                "重复 path 再导出的紧凑 JSON 不应包含内部兼容后缀");
    duplicate_export_options.format = elf_static_view::ExportFormat::JsonPretty;
    const auto duplicate_pretty_json =
        elf_static_view::render_export_document(duplicate_document, duplicate_export_options);
    expect_true(
        duplicate_pretty_json.find("#2") == std::string::npos && duplicate_pretty_json.find("#3") == std::string::npos,
        "重复 path 再导出的格式化 JSON 不应包含内部兼容后缀");
    duplicate_export_options.format = elf_static_view::ExportFormat::BinaryPrivate;
    const auto duplicate_binary = elf_static_view::render_export_document(duplicate_document, duplicate_export_options);
    const auto parsed_duplicate_binary =
        elf_static_view::parse_export_bytes(duplicate_binary, duplicate_export_options.format);
    const auto parsed_duplicate_lightweight =
        std::get<elf_static_view::LightweightExport>(parsed_duplicate_binary.payload);
    expect_true(parsed_duplicate_lightweight.variables[1].path == "root.member",
                "重复 path 再导出的私有二进制不应包含 #2 兼容后缀");
    expect_true(parsed_duplicate_lightweight.variables[1].name == "root.member",
                "重复 path 再导出的私有二进制名称不应包含 #2 兼容后缀");

    elf_static_view::ExportOptions binary_options;
    binary_options.format = elf_static_view::ExportFormat::BinaryPrivate;
    binary_options.payload_kind = elf_static_view::ExportPayloadKind::VariableSummary;
    const auto binary = elf_static_view::render_export_document(json_document, binary_options);
    const auto imported_binary = elf_static_view::import_project_data_bytes(binary, "summary.esv");
    expect_true(imported_binary.payload_kind == elf_static_view::ExportPayloadKind::VariableSummary,
                "自动导入应识别私有二进制精简变量");

    elf_static_view::ProjectSnapshot snapshot;
    snapshot.source_file = "old.elf";
    snapshot.model.file = "old.elf";
    elf_static_view::ExpandedNode snapshot_member;
    snapshot_member.path = "g_struct[0].name";
    snapshot_member.display_name = "name";
    snapshot_member.type_name = "char[16]";
    snapshot_member.type_kind = elf_static_view::TypeKind::Array;
    snapshot_member.absolute_address = 0x402000;
    elf_static_view::ExpandedNode snapshot_duplicate_member = snapshot_member;
    snapshot_duplicate_member.path = "g_struct[0].name#2";
    snapshot_duplicate_member.export_path = "g_struct[0].name";
    snapshot_duplicate_member.absolute_address = 0x402010;
    snapshot.model.expanded = {snapshot_member, snapshot_duplicate_member};
    const elf_static_view::ExportDocument snapshot_document{
        elf_static_view::ExportPayloadKind::FullSnapshot,
        snapshot,
    };
    elf_static_view::ExportOptions snapshot_options;
    snapshot_options.format = elf_static_view::ExportFormat::BinaryPrivate;
    snapshot_options.payload_kind = elf_static_view::ExportPayloadKind::FullSnapshot;
    const auto binary_snapshot = elf_static_view::render_export_document(snapshot_document, snapshot_options);
    expect_true(binary_snapshot.find("#2") == std::string::npos, "私有二进制完整快照不应写出内部兼容后缀");
    const auto imported_snapshot = elf_static_view::import_project_data_bytes(binary_snapshot, "snapshot.esv");
    expect_true(imported_snapshot.payload_kind == elf_static_view::ExportPayloadKind::FullSnapshot,
                "自动导入应识别私有二进制完整快照");
    expect_true(imported_snapshot.snapshot.source_file == "old.elf", "完整快照导入应保留快照语义");
    expect_true(imported_snapshot.snapshot.model.expanded.size() == 2, "完整快照导入应保留重复逻辑路径对应的节点");
    expect_true(imported_snapshot.snapshot.model.expanded[0].path == "g_struct[0].name",
                "完整快照导入首个节点应使用逻辑路径作为内部 key");
    expect_true(imported_snapshot.snapshot.model.expanded[1].path == "g_struct[0].name#2",
                "完整快照导入重复节点应恢复内部唯一 key");
    expect_true(imported_snapshot.snapshot.model.expanded[1].export_path == "g_struct[0].name",
                "完整快照导入重复节点应保留对外逻辑路径");
}

void verify_ui_config_round_trip()
{
    const auto temp_root =
        std::filesystem::temp_directory_path() /
        ("elf-static-view-config-round-trip-" +
         std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::remove_all(temp_root);
    std::filesystem::create_directories(temp_root);
    const auto executable_path = temp_root / "elf-static-view.exe";

    elf_static_view::ui::AppState first_state;
    elf_static_view::ui::load_app_config(first_state, executable_path);
    first_state.persist_address_bias_to_config = true;
    first_state.address_bias_input = "0x123";
    first_state.address_bias = elf_static_view::parse_address_bias(first_state.address_bias_input);
    first_state.copy_address_base = elf_static_view::ui::CopyAddressBase::Bin;
    first_state.copy_hex_without_prefix = true;
    first_state.ui_refresh_rate = 15;
    first_state.filter_debounce_ms = 450;
    first_state.version_check = elf_static_view::ui::VersionCheckState{
        .repository_url = "https://example.com/project",
        .check_uri = "https://example.com/releases.yaml",
        .latest_version = {},
        .release_url = {},
        .release_name = {},
        .release_notes = {},
        .message = {},
        .has_new_version = false,
        .check_uri_uses_default = false,
    };
    elf_static_view::ui::save_app_config(first_state);

    elf_static_view::ui::AppState restored_state;
    elf_static_view::ui::load_app_config(restored_state, executable_path);
    expect_true(restored_state.persist_address_bias_to_config, "地址偏移写回应当从配置文件恢复");
    expect_true(restored_state.address_bias == elf_static_view::parse_address_bias("0x123"),
                "地址偏移数值应当从配置文件恢复");
    expect_true(restored_state.address_bias_input == "0x123", "地址偏移输入框文本应当从配置文件恢复");
    expect_true(restored_state.copy_address_base == elf_static_view::ui::CopyAddressBase::Bin,
                "复制进制应当从配置文件恢复");
    expect_true(restored_state.copy_hex_without_prefix, "十六进制复制前缀设置应当从配置文件恢复");
    expect_true(restored_state.ui_refresh_rate == 15, "界面刷新率应当从配置文件恢复");
    expect_true(restored_state.filter_debounce_ms == 450, "筛选输入延迟应当从配置文件恢复");
    expect_true(restored_state.version_check.has_value(), "版本检查 URI 应当从配置文件恢复");
    expect_true(restored_state.version_check->repository_url == "https://example.com/project", "仓库地址恢复值不正确");
    expect_true(restored_state.version_check->check_uri == "https://example.com/releases.yaml",
                "版本检查 URI 恢复值不正确");

    restored_state.persist_address_bias_to_config = false;
    restored_state.address_bias_input = "0x456";
    restored_state.address_bias = elf_static_view::parse_address_bias(restored_state.address_bias_input);
    elf_static_view::ui::save_app_config(restored_state);

    elf_static_view::ui::AppState disabled_state;
    elf_static_view::ui::load_app_config(disabled_state, executable_path);
    expect_true(!disabled_state.persist_address_bias_to_config, "关闭地址偏移写回后，下次启动不应再启用");
    expect_true(disabled_state.address_bias == 0, "关闭地址偏移写回后，不应恢复旧地址偏移");
    expect_true(disabled_state.address_bias_input == "0", "关闭地址偏移写回后，地址偏移输入应保持默认值");
    expect_true(disabled_state.copy_address_base == elf_static_view::ui::CopyAddressBase::Bin,
                "关闭地址偏移写回不应影响复制进制恢复");
    expect_true(disabled_state.copy_hex_without_prefix, "关闭地址偏移写回不应影响十六进制复制前缀设置");
    expect_true(disabled_state.ui_refresh_rate == 15, "关闭地址偏移写回不应影响界面刷新率恢复");
    expect_true(disabled_state.filter_debounce_ms == 450, "关闭地址偏移写回不应影响筛选输入延迟恢复");

    {
        std::ofstream output(first_state.config_path, std::ios::binary);
        output << "ui:\n  filter_debounce_ms: 3001\n";
    }
    elf_static_view::ui::AppState invalid_debounce_state;
    elf_static_view::ui::load_app_config(invalid_debounce_state, executable_path);
    expect_true(invalid_debounce_state.filter_debounce_ms == 300, "非法筛选输入延迟应回退默认值");

    std::filesystem::remove_all(temp_root);
}

void verify_version_check_resolution()
{
    const auto& defaults = elf_static_view::ui::default_release_metadata();

    elf_static_view::ui::AppState default_state;
    const auto resolved_default = elf_static_view::ui::resolve_version_check_state(default_state);
    expect_true(resolved_default.repository_url == defaults.repository_url, "默认仓库地址应当回退到 GitHub 仓库");
    expect_true(resolved_default.check_uri == defaults.releases_api_url,
                "默认版本检查地址应当回退到 GitHub Releases API");
    expect_true(resolved_default.check_uri_uses_default, "默认版本检查来源应当标记为默认 GitHub");

    elf_static_view::ui::AppState custom_state;
    custom_state.version_check = elf_static_view::ui::VersionCheckState{
        .repository_url = "https://example.com/custom-repo",
        .check_uri = {},
        .latest_version = {},
        .release_url = {},
        .release_name = {},
        .release_notes = {},
        .message = {},
        .has_new_version = false,
        .check_uri_uses_default = true,
    };
    const auto resolved_custom = elf_static_view::ui::resolve_version_check_state(custom_state);
    expect_true(resolved_custom.repository_url == "https://example.com/custom-repo",
                "配置仓库地址后，应当优先显示配置值");
    expect_true(resolved_custom.check_uri == defaults.releases_api_url,
                "仅配置仓库地址时，版本检查地址仍应回退到默认 GitHub API");
}

void verify_version_response_parsing()
{
    const auto custom_result = elf_static_view::ui::parse_version_response_text(
        "latest_version: 1.2.3\nrelease_url: https://example.com/releases/1.2.3\nname: Example Release\n"
        "body: Example Notes\n",
        "https://example.com/releases.yaml",
        "https://example.com/project");
    expect_true(custom_result.latest_version == "1.2.3", "自定义版本响应应当解析 latest_version");
    expect_true(custom_result.release_url == "https://example.com/releases/1.2.3",
                "自定义版本响应应当解析 release_url");
    expect_true(custom_result.release_name == "Example Release", "自定义版本响应应当解析 name");
    expect_true(custom_result.release_notes == "Example Notes", "自定义版本响应应当解析 body");

    const auto github_result = elf_static_view::ui::parse_version_response_text(
        R"({"tag_name":"v2.0.0","html_url":"https://github.com/HamsterAPig/ElfStaticView/releases/tag/v2.0.0","name":"v2.0.0 Windows","body":"GitHub Release Notes"})",
        "https://api.github.com/repos/HamsterAPig/ElfStaticView/releases/latest",
        "https://github.com/HamsterAPig/ElfStaticView");
    expect_true(github_result.latest_version == "v2.0.0", "GitHub Releases JSON 应当解析 tag_name");
    expect_true(github_result.release_url == "https://github.com/HamsterAPig/ElfStaticView/releases/tag/v2.0.0",
                "GitHub Releases JSON 应当解析 html_url");
    expect_true(github_result.release_name == "v2.0.0 Windows", "GitHub Releases JSON 应当解析 name");
    expect_true(github_result.release_notes == "GitHub Release Notes", "GitHub Releases JSON 应当解析 body");
}

void verify_version_compare_rules()
{
    expect_true(elf_static_view::ui::compare_version_strings("v0.1.0", "0.1.0") == 0, "版本比较应当兼容可选的 v 前缀");
    expect_true(elf_static_view::ui::compare_version_strings("0.1.0+abcd1234", "0.1.0") == 0,
                "开发态版本不应被误判成比同基线 tag 更新");
    expect_true(elf_static_view::ui::compare_version_strings("0.1.0+abcd1234", "v0.1.1") < 0,
                "开发态版本仍应识别到更高的发布版本");
}

void verify_default_load_policy()
{
    const auto policy = elf_static_view::ui::default_load_policy();
    expect_true(policy.static_storage_only, "默认策略应启用静态存储优先");
    expect_true(policy.exclude_formal_parameters, "默认策略应跳过形参");
    expect_true(policy.exclude_runtime_only_variables, "默认策略应跳过 runtime-only 变量");
    expect_true(policy.expand_depth == 6, "默认展开深度应为 6");
    expect_true(policy.compile_unit_path_rules_text.find("**/Drivers/**") != std::string::npos,
                "默认策略应包含通用库目录过滤规则");
}

void verify_dump_accepts_load_policy(const std::string& fixture_path)
{
    elf_static_view::ProjectLoader loader;
    elf_static_view::LoadPolicy policy = elf_static_view::ui::default_load_policy();
    const auto model = loader.dump(fixture_path,
                                   {.include_runtime_only = false,
                                    .only_static_known = true,
                                    .symbol_name = std::nullopt,
                                    .expand_depth = policy.expand_depth,
                                    .load_policy = policy});
    expect_true(!model.file.empty(), "带 load_policy 的 dump 应返回有效模型");
    bool found_lazy_child = false;
    for (const auto& node : model.expanded) {
        if (node.children_lazy) {
            found_lazy_child = true;
            break;
        }
    }
    expect_true(found_lazy_child, "启用 lazy_expand_children 后应存在延迟展开节点");
}

void verify_class_array_nested_expand_fixture()
{
    elf_static_view::ProjectLoader loader;

    {
        elf_static_view::LoadPolicy policy = elf_static_view::ui::default_load_policy();
        policy.expand_depth = 1;
        policy.lazy_expand_children = true;
        const auto model = loader.dump(ELF_STATIC_VIEW_CLASS_ARRAY_NESTED_EXPAND_FIXTURE_PATH,
                                       {.include_runtime_only = true,
                                        .only_static_known = false,
                                        .symbol_name = std::nullopt,
                                        .expand_depth = policy.expand_depth,
                                        .load_policy = policy});
        const auto* object = find_expanded_path(model.expanded, "demo::global_object");
        expect_true(object != nullptr, "深度 1 应保留全局对象节点");
        expect_true(find_expanded_path(object->children, "demo::global_object.items") != nullptr,
                    "深度 1 应展开到全局对象的一层成员");
        expect_true(find_expanded_path(object->children, "demo::global_object.items[0]") == nullptr,
                    "深度 1 初始树不应直接展开数组元素");
        const auto* items = find_expanded_path(model.expanded, "demo::global_object.items");
        expect_true(items != nullptr && items->children_lazy, "深度截断后的数组成员应允许 UI 懒加载");
    }

    {
        elf_static_view::LoadPolicy policy = elf_static_view::ui::default_load_policy();
        policy.expand_depth = 3;
        policy.lazy_expand_children = true;
        const auto model = loader.dump(ELF_STATIC_VIEW_CLASS_ARRAY_NESTED_EXPAND_FIXTURE_PATH,
                                       {.include_runtime_only = true,
                                        .only_static_known = false,
                                        .symbol_name = std::nullopt,
                                        .expand_depth = policy.expand_depth,
                                        .load_policy = policy});
        expect_true(expanded_path_exists(model, "demo::global_object.items[0].nested"),
                    "深度 3 应展开到数组元素内的 nested 成员");
        expect_true(!expanded_path_exists(model, "demo::global_object.items[0].nested.leaf"),
                    "深度 3 初始树不应越过配置深度展开 leaf");
        const auto* nested = find_expanded_path(model.expanded, "demo::global_object.items[1].nested");
        expect_true(nested != nullptr && nested->children_lazy, "嵌套成员应保留继续懒加载标记");
    }

    {
        elf_static_view::LoadPolicy policy = elf_static_view::ui::default_load_policy();
        policy.expand_depth = 0;
        policy.lazy_expand_children = false;
        const auto model = loader.dump(ELF_STATIC_VIEW_CLASS_ARRAY_NESTED_EXPAND_FIXTURE_PATH,
                                       {.include_runtime_only = true,
                                        .only_static_known = false,
                                        .symbol_name = std::nullopt,
                                        .expand_depth = policy.expand_depth,
                                        .load_policy = policy});
        expect_true(expanded_path_exists(model, "demo::global_object.items[0].nested.leaf"),
                    "深度 0 应不限制展开并包含第一个数组元素 leaf");
        expect_true(expanded_path_exists(model, "demo::global_object.items[1].nested.leaf"),
                    "深度 0 应不限制展开并包含第二个数组元素 leaf");
        expect_true(!contains_lazy_node(model.expanded), "关闭 lazy_expand_children 后展开树不应包含 lazy 标记");
    }
}

void verify_data_member_location_plus_uconst_fixture()
{
    elf_static_view::ProjectLoader loader;
    elf_static_view::LoadPolicy policy = elf_static_view::ui::default_load_policy();
    policy.expand_depth = 0;
    policy.lazy_expand_children = false;
    const auto model = loader.dump(ELF_STATIC_VIEW_CLASS_ARRAY_MEMBER_PLUS_UCONST_FIXTURE_PATH,
                                   {.include_runtime_only = true,
                                    .only_static_known = false,
                                    .symbol_name = std::nullopt,
                                    .expand_depth = policy.expand_depth,
                                    .load_policy = policy});

    const auto* nested_leaf = find_type_by_name(model, "NestedLeaf");
    expect_true(nested_leaf != nullptr, "plus_uconst fixture 应保留 NestedLeaf 类型");
    const auto guard_member = std::find_if(
        nested_leaf->members.begin(), nested_leaf->members.end(), [](const elf_static_view::TypeMember& member) {
            return member.name == "guard";
        });
    expect_true(guard_member != nested_leaf->members.end(), "plus_uconst fixture 应包含 guard 成员");
    expect_true(guard_member->address.relative_offset.has_value() && guard_member->address.relative_offset.value() == 4,
                "DW_OP_plus_uconst 成员位置表达式应解析为 relative_offset=4");

    const auto* nested = find_expanded_path(model.expanded, "demo::global_object.items[0].nested");
    const auto* guard = find_expanded_path(model.expanded, "demo::global_object.items[0].nested.guard");
    expect_true(nested != nullptr && nested->absolute_address.has_value(),
                "plus_uconst fixture 应能展开 nested 成员地址");
    expect_true(guard != nullptr && guard->absolute_address.has_value(), "plus_uconst fixture 应能展开 guard 成员地址");
    expect_true(guard->absolute_address.value() == nested->absolute_address.value() + 4U,
                "DW_OP_plus_uconst 成员偏移应参与展开地址计算");
}

void verify_first_member_without_location_uses_base_address()
{
    elf_static_view::ProjectLoader loader;
    elf_static_view::LoadPolicy policy = elf_static_view::ui::default_load_policy();
    policy.expand_depth = 2;
    policy.lazy_expand_children = false;
    const auto model = loader.dump(ELF_STATIC_VIEW_CLASS_ARRAY_FIRST_MEMBER_MISSING_FIXTURE_PATH,
                                   {.include_runtime_only = true,
                                    .only_static_known = false,
                                    .symbol_name = std::nullopt,
                                    .expand_depth = policy.expand_depth,
                                    .load_policy = policy});

    const auto* object = find_expanded_path(model.expanded, "demo::global_object");
    const auto* items = find_expanded_path(model.expanded, "demo::global_object.items");
    expect_true(object != nullptr && object->absolute_address.has_value(), "首成员缺省位置 fixture 应保留对象基址");
    expect_true(items != nullptr && items->absolute_address.has_value(),
                "缺少 DW_AT_data_member_location 的首成员应使用对象基址");
    expect_true(items->absolute_address.value() == object->absolute_address.value(), "首成员地址应等于类对象基址");
    expect_true(items->relative_offset.has_value() && items->relative_offset.value() == 0,
                "首成员缺省位置应记录 relative_offset=0");
}

void verify_background_task_state_transitions()
{
    elf_static_view::ui::AppState state;
    elf_static_view::ui::begin_background_load(state, 1, "first.elf");
    elf_static_view::ui::begin_background_load(state, 2, "second.elf");
    expect_true(state.background_load.task_id == 2, "后发加载任务应覆盖当前 task_id");

    elf_static_view::ProjectModel old_model;
    old_model.file = "first.elf";
    elf_static_view::ui::finish_background_load(state, 1, std::move(old_model));
    expect_true(state.background_load.status == elf_static_view::ui::BackgroundLoadStatus::Loading,
                "过期加载任务完成后不应覆盖当前加载状态");

    elf_static_view::ui::fail_background_load(state, 2, "second failed");
    expect_true(state.background_load.status == elf_static_view::ui::BackgroundLoadStatus::Failed,
                "当前加载任务失败后应更新失败状态");
    expect_true(state.background_load.error_message == "second failed", "当前加载任务失败信息应写入 state");

    elf_static_view::ui::begin_ui_task(state.export_raw_dwarf_task, 3, "raw.json");
    expect_true(state.export_raw_dwarf_task.status == elf_static_view::ui::UiTaskStatus::Running,
                "原始 DWARF 导出任务开始后应进入 Running");
    expect_true(elf_static_view::ui::finish_ui_task(state.export_raw_dwarf_task, 3, "done"),
                "原始 DWARF 导出任务完成应接受当前 task_id");
    expect_true(state.export_raw_dwarf_task.status == elf_static_view::ui::UiTaskStatus::Succeeded,
                "原始 DWARF 导出任务完成后应进入 Succeeded");
}

void verify_opened_file_monitor_state_machine()
{
    using elf_static_view::ui::AppState;
    using elf_static_view::ui::LoadedContentKind;

    const auto base_time = std::chrono::steady_clock::now();

    AppState due_state;
    elf_static_view::ui::watch_opened_file(due_state, LoadedContentKind::ElfProject, "workspace/demo.elf");
    expect_true(elf_static_view::ui::opened_file_monitor_check_due(due_state, base_time),
                "已监听文件首次检查应立即允许");
    expect_true(
        !elf_static_view::ui::opened_file_monitor_check_due(due_state, base_time + std::chrono::milliseconds(999)),
        "已监听文件不应每帧访问文件系统");
    expect_true(
        elf_static_view::ui::opened_file_monitor_check_due(due_state, base_time + std::chrono::milliseconds(1000)),
        "已监听文件超过轮询间隔后应允许再次检查");

    AppState project_state;
    elf_static_view::ProjectModel project_model;
    project_model.file = "workspace/demo.elf";
    elf_static_view::ui::set_loaded_project(
        project_state, std::move(project_model), LoadedContentKind::ElfProject, "workspace/demo.elf");

    const auto no_delete_reload =
        elf_static_view::ui::observe_opened_file_presence(project_state, true, base_time, false);
    expect_true(!no_delete_reload.has_value(), "普通原地存在检查不应触发重新打开");

    const auto delete_reload = elf_static_view::ui::observe_opened_file_presence(
        project_state, false, base_time + std::chrono::milliseconds(1000), false);
    expect_true(!delete_reload.has_value(), "仅删除已打开文件不应触发重新打开");
    expect_true(project_state.project_model.has_value(), "删除期间应保留旧 project_model");
    expect_true(project_state.current_file_path == "workspace/demo.elf", "删除期间应保留当前 ELF 路径");
    expect_true(project_state.opened_file_monitor.missing, "删除后监听状态应记录暂时缺失");
    expect_true(!project_state.opened_file_monitor.reload_pending, "重复缺失前不应排队重新打开");

    const auto repeated_delete_reload = elf_static_view::ui::observe_opened_file_presence(
        project_state, false, base_time + std::chrono::milliseconds(1500), false);
    expect_true(!repeated_delete_reload.has_value(), "重复缺失不应重复触发重新打开");
    expect_true(!project_state.opened_file_monitor.reload_pending, "重复缺失不应留下重载排队状态");

    const auto early_recreate_reload = elf_static_view::ui::observe_opened_file_presence(
        project_state, true, base_time + std::chrono::milliseconds(1800), false);
    expect_true(!early_recreate_reload.has_value(), "重建后稳定等待未满足时不应立刻重新打开");
    expect_true(project_state.opened_file_monitor.reload_pending, "重建后应记录一次待重载信号");

    const auto ready_recreate_reload = elf_static_view::ui::observe_opened_file_presence(
        project_state, true, base_time + std::chrono::milliseconds(2200), false);
    expect_true(ready_recreate_reload.has_value() && ready_recreate_reload.value() == "workspace/demo.elf",
                "删除后重建且稳定后应触发一次重新打开");
    const auto duplicate_recreate_reload = elf_static_view::ui::observe_opened_file_presence(
        project_state, true, base_time + std::chrono::milliseconds(2600), false);
    expect_true(!duplicate_recreate_reload.has_value(), "一次重建完成后不应重复重新打开");
}

void verify_opened_file_monitor_busy_and_snapshot_paths()
{
    using elf_static_view::ui::AppState;
    using elf_static_view::ui::LoadedContentKind;

    const auto base_time = std::chrono::steady_clock::now();

    AppState busy_state;
    elf_static_view::ui::watch_opened_file(busy_state, LoadedContentKind::ElfProject, "workspace/busy.elf");
    static_cast<void>(elf_static_view::ui::observe_opened_file_presence(
        busy_state, false, base_time + std::chrono::milliseconds(1000), false));
    static_cast<void>(elf_static_view::ui::observe_opened_file_presence(
        busy_state, true, base_time + std::chrono::milliseconds(1300), true));

    const auto busy_reload = elf_static_view::ui::observe_opened_file_presence(
        busy_state, true, base_time + std::chrono::milliseconds(1700), true);
    expect_true(!busy_reload.has_value(), "加载中遇到重建不应立即重新打开");
    expect_true(busy_state.opened_file_monitor.reload_pending, "加载中重建只应保留一个待重载信号");

    const auto still_busy_reload = elf_static_view::ui::observe_opened_file_presence(
        busy_state, true, base_time + std::chrono::milliseconds(2100), true);
    expect_true(!still_busy_reload.has_value(), "加载持续期间不应堆积重复重载任务");
    expect_true(busy_state.opened_file_monitor.reload_pending, "加载持续期间应继续保留原始待重载信号");

    const auto resumed_reload = elf_static_view::ui::observe_opened_file_presence(
        busy_state, true, base_time + std::chrono::milliseconds(2500), false);
    expect_true(resumed_reload.has_value() && resumed_reload.value() == "workspace/busy.elf",
                "加载结束后应消费之前的重建信号并重新打开一次");

    AppState snapshot_state;
    elf_static_view::ProjectSnapshot snapshot;
    snapshot.source_file = "workspace/source-from-snapshot.elf";
    snapshot.model.file = snapshot.source_file;
    elf_static_view::ui::set_loaded_snapshot(snapshot_state, std::move(snapshot), "workspace/imported.snapshot.json");
    expect_true(snapshot_state.current_file_path == "workspace/source-from-snapshot.elf",
                "导入快照后 current_file_path 应保留快照内源文件");
    expect_true(snapshot_state.opened_file_monitor.path == "workspace/imported.snapshot.json",
                "导入快照后监听路径应指向 JSON 快照本身");

    const auto snapshot_delete_reload = elf_static_view::ui::observe_opened_file_presence(
        snapshot_state, false, base_time + std::chrono::milliseconds(1000), false);
    expect_true(!snapshot_delete_reload.has_value(), "仅删除 JSON 快照不应触发重新打开");
    expect_true(snapshot_state.snapshot.has_value(), "删除 JSON 快照期间应保留旧 snapshot");
    expect_true(snapshot_state.project_model.has_value(), "删除 JSON 快照期间应保留旧 project_model");
}

} // namespace

int main()
{
    try {
        verify_default_load_policy();
        verify_fixture(ELF_STATIC_VIEW_C_FIXTURE_PATH, ELF_STATIC_VIEW_C_EXPECTED_JSON);
        verify_fixture(ELF_STATIC_VIEW_CPP_FIXTURE_PATH, ELF_STATIC_VIEW_CPP_EXPECTED_JSON);
        verify_raw_dwarf_json_export(ELF_STATIC_VIEW_C_FIXTURE_PATH);
        verify_dump_accepts_load_policy(ELF_STATIC_VIEW_C_FIXTURE_PATH);
        verify_class_array_nested_expand_fixture();
        verify_data_member_location_plus_uconst_fixture();
        verify_first_member_without_location_uses_base_address();
        verify_dump_text_contains_elf_info_any_class(ELF_STATIC_VIEW_DEBUG_SUP_FIXTURE_PATH, "ELF32", "LittleEndian");
        verify_bitfield_layout_fixture();
        verify_gnu_addr_index_fixture();
        verify_ref_sig8_debug_types_fixture();
        verify_ref_sig8_indirect_fixture();
        verify_gcc_ref_addr_fixture();
        verify_gcc_small_ref_fixture(ELF_STATIC_VIEW_GCC_DWARF5_REF1_FIXTURE_PATH, "ref1");
        verify_gcc_small_ref_fixture(ELF_STATIC_VIEW_GCC_DWARF5_REF2_FIXTURE_PATH, "ref2");
        verify_gcc_small_ref_fixture(ELF_STATIC_VIEW_GCC_DWARF5_REF_UDATA_FIXTURE_PATH, "ref_udata");
        verify_gcc_ref_sup4_fixture();
        verify_gcc_ref8_fixture();
        verify_gcc_ref_sup8_fixture();
        verify_gcc_gnu_alt_fixture();
        verify_gcc_line_strp_fixture();
        verify_dwarf5_strx1_fixture();
        verify_dwarf5_strx_form_fixture(
            ELF_STATIC_VIEW_DWARF5_STRX2_FIXTURE_PATH, "DW_FORM_strx2", "dwarf5 strx2 fixture");
        verify_dwarf5_strx_form_fixture(
            ELF_STATIC_VIEW_DWARF5_STRX3_FIXTURE_PATH, "DW_FORM_strx3", "dwarf5 strx3 fixture");
        verify_dwarf5_strx_form_fixture(
            ELF_STATIC_VIEW_DWARF5_STRX4_FIXTURE_PATH, "DW_FORM_strx4", "dwarf5 strx4 fixture");
        verify_dwarf5_addrx_fixture();
        verify_dwarf5_addrx_form_fixture(
            ELF_STATIC_VIEW_DWARF5_ADDRX1_FIXTURE_PATH, DW_FORM_addrx1, "dwarf5 addrx1 fixture");
        verify_dwarf5_addrx_form_fixture(
            ELF_STATIC_VIEW_DWARF5_ADDRX2_FIXTURE_PATH, DW_FORM_addrx2, "dwarf5 addrx2 fixture");
        verify_dwarf5_addrx_form_fixture(
            ELF_STATIC_VIEW_DWARF5_ADDRX3_FIXTURE_PATH, DW_FORM_addrx3, "dwarf5 addrx3 fixture");
        verify_dwarf5_addrx_form_fixture(
            ELF_STATIC_VIEW_DWARF5_ADDRX4_FIXTURE_PATH, DW_FORM_addrx4, "dwarf5 addrx4 fixture");
        verify_rnglistx_fixture();
        verify_rnglists_start_end_fixture();
        verify_rnglists_offset_pair_fixture();
        verify_rnglists_base_addressx_fixture();
        verify_rnglists_startx_endx_fixture();
        verify_rnglists_startx_length_fixture();
        verify_implicit_const_fixture();
        verify_dwarf5_strx_type_unit_fixture();
        verify_dwarf5_strx_type_unit_abbrev_offset_fixture();
        verify_split_dwarf_fixture();
        verify_split_dwp_fixture();
        verify_debug_sup_fixture();
        verify_debug_sup_sidecar_fixture();
        verify_gcc_strp_sup_fixture();
        verify_current_supplementary_gap_notes();
        verify_member_pointer_type_fixture();
        verify_const_value_text_json_round_trip();
        verify_value_expression_text_rendering();
        verify_named_location_op_rendering();
        verify_register_location_text_rendering();
        verify_register_address_text_rendering();
        verify_unspecified_type_fixture();
        verify_atomic_type_fixture();
        verify_abstract_origin_const_value_fixture();
        verify_multi_cu_file_static_fixture();
        verify_inline_scope_static_fixture();
        verify_const_value_fixture();
        verify_piece_stack_value_fixture();
        verify_bit_piece_fixture();
        verify_dwarf5_loclists_fixture();
        verify_dwarf5_loclists_base_default_fixture();
        verify_dwarf5_loclists_base_addressx_fixture();
        verify_location_expression_block_decoding();
        verify_dwarf5_loclists_start_end_fixture();
        verify_dwarf5_loclists_startx_endx_fixture();
        verify_dwarf5_loclists_startx_length_fixture();
        verify_dwarf5_loclists_start_length_fixture();
        verify_specification_fixture();
        verify_thread_local_fixture();
        verify_json_round_trip(ELF_STATIC_VIEW_CPP_FIXTURE_PATH);
        verify_dump_text_contains_elf_info(ELF_STATIC_VIEW_CPP_FIXTURE_PATH);
        verify_loader_error_contains_file_path();
        verify_filter_rules();
        verify_filter_debounce_and_async_state();
        verify_address_bias();
        verify_address_bias_parsing();
        verify_copy_address_formatting();
        verify_window_title_formatting();
        verify_utf8_path_helpers();
        verify_ti_coff_object_section_indices();
        verify_elf_symbol_table_endian_matrix();
        verify_elf_symbol_table_metadata();
        verify_c2000_elf_metadata();
        verify_snapshot_export_redaction();
        verify_lightweight_export_round_trip();
        verify_lightweight_export_expands_nodes_and_limits_arrays();
        verify_lightweight_export_keeps_sanitized_member_paths();
        verify_import_project_data_auto_and_lightweight_model();
        verify_ui_config_round_trip();
        verify_version_check_resolution();
        verify_version_response_parsing();
        verify_version_compare_rules();
        verify_background_task_state_transitions();
        verify_opened_file_monitor_state_machine();
        verify_opened_file_monitor_busy_and_snapshot_paths();
        std::cout << "all tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
