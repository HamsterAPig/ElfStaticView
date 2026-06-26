#include "elf_static_view/project.hpp"

#include "analysis/address_bias.hpp"
#include "analysis/expander.hpp"
#include "analysis/model_utils.hpp"
#include "elf/dwarf_reader.hpp"
#include "elf/raw_dwarf_reader.hpp"

#include <chrono>
#include <exception>
#include <sstream>
#include <unordered_map>

namespace elf_static_view {

namespace {

    [[nodiscard]] std::string build_load_error_message(const std::string& file_path, const std::exception& error)
    {
        std::ostringstream stream;
        stream << "文件分析失败: " << file_path << " | " << error.what();
        return stream.str();
    }

    [[nodiscard]] LoadPolicy normalize_load_policy(const ScanOptions& options)
    {
        LoadPolicy policy = options.load_policy;
        policy.exclude_runtime_only_variables = !options.include_runtime_only;
        return policy;
    }

    [[nodiscard]] LoadPolicy normalize_load_policy(const DumpOptions& options)
    {
        LoadPolicy policy = options.load_policy;
        policy.exclude_runtime_only_variables = !options.include_runtime_only;
        policy.static_storage_only = options.only_static_known;
        policy.expand_depth = options.expand_depth;
        return policy;
    }

    void append_elf_info_lines(std::ostringstream& stream, const ProjectModel& model)
    {
        stream << "elf_class: " << model.elf_info.object_class << '\n';
        stream << "byte_order: " << model.elf_info.byte_order << '\n';
        stream << "file_type: " << model.elf_info.file_type << '\n';
        stream << "machine: " << model.elf_info.machine << '\n';
        stream << "os_abi: " << model.elf_info.os_abi << '\n';
    }

    void append_location_range_suffix(std::ostringstream& stream, const AddressInfo& address)
    {
        if (address.location_ranges.empty()) {
            return;
        }
        stream << " ranges=";
        for (std::size_t index = 0; index < address.location_ranges.size(); ++index) {
            const auto& range = address.location_ranges[index];
            if (index > 0) {
                stream << ',';
            }
            stream << '[';
            if (range.cooked_low_pc.has_value()) {
                stream << "0x" << std::hex << range.cooked_low_pc.value();
            } else if (range.raw_low_pc.has_value()) {
                stream << "raw:0x" << std::hex << range.raw_low_pc.value();
            } else {
                stream << '?';
            }
            stream << "..";
            if (range.cooked_high_pc.has_value()) {
                stream << "0x" << std::hex << range.cooked_high_pc.value();
            } else if (range.raw_high_pc.has_value()) {
                stream << "raw:0x" << std::hex << range.raw_high_pc.value();
            } else {
                stream << '?';
            }
            if (range.debug_addr_unavailable) {
                stream << " unavailable";
            }
            stream << ']';
            stream << std::dec;
        }
    }

} // namespace

std::string to_string(AddressKind value)
{
    switch (value) {
        case AddressKind::Absolute:
            return "Absolute";
        case AddressKind::SectionRelative:
            return "SectionRelative";
        case AddressKind::MemberOffset:
            return "MemberOffset";
        case AddressKind::ArrayElementOffset:
            return "ArrayElementOffset";
        case AddressKind::BitField:
            return "BitField";
        case AddressKind::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

std::string to_string(Availability value)
{
    switch (value) {
        case Availability::StaticAddressKnown:
            return "StaticAddressKnown";
        case Availability::StaticLayoutKnown:
            return "StaticLayoutKnown";
        case Availability::RuntimeOnly:
            return "RuntimeOnly";
        case Availability::Unavailable:
            return "Unavailable";
        case Availability::OptimizedOut:
            return "OptimizedOut";
    }
    return "Unavailable";
}

std::string to_string(TypeKind value)
{
    switch (value) {
        case TypeKind::Base:
            return "Base";
        case TypeKind::Pointer:
            return "Pointer";
        case TypeKind::Reference:
            return "Reference";
        case TypeKind::MemberPointer:
            return "MemberPointer";
        case TypeKind::Typedef:
            return "Typedef";
        case TypeKind::Qualified:
            return "Qualified";
        case TypeKind::Array:
            return "Array";
        case TypeKind::Struct:
            return "Struct";
        case TypeKind::Class:
            return "Class";
        case TypeKind::Union:
            return "Union";
        case TypeKind::Enum:
            return "Enum";
        case TypeKind::Subroutine:
            return "Subroutine";
        case TypeKind::Atomic:
            return "Atomic";
        case TypeKind::Unspecified:
            return "Unspecified";
        case TypeKind::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

std::string to_string(VariableKind value)
{
    switch (value) {
        case VariableKind::Global:
            return "Global";
        case VariableKind::Namespace:
            return "Namespace";
        case VariableKind::FileStatic:
            return "FileStatic";
        case VariableKind::FunctionStatic:
            return "FunctionStatic";
        case VariableKind::StaticMember:
            return "StaticMember";
        case VariableKind::Local:
            return "Local";
        case VariableKind::Parameter:
            return "Parameter";
        case VariableKind::ThreadLocal:
            return "ThreadLocal";
        case VariableKind::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

ProjectModel ProjectLoader::scan(const std::string& file_path, const ScanOptions& options) const
{
    elf::DwarfReader reader;
    const LoadPolicy load_policy = normalize_load_policy(options);
    ProjectModel model;
    try {
        model = reader.load(file_path, load_policy);
    } catch (const std::exception& error) {
        throw std::runtime_error(build_load_error_message(file_path, error));
    }
    const auto expand_started_at = std::chrono::steady_clock::now();
    analysis::Expander expander(model.types, load_policy.expand_depth, load_policy.lazy_expand_children);
    model.expanded = expander.build(model.symbols, options.include_runtime_only, false, std::nullopt);
    model.metrics.expand_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - expand_started_at)
            .count());
    return model;
}

ProjectModel ProjectLoader::dump(const std::string& file_path, const DumpOptions& options) const
{
    elf::DwarfReader reader;
    const LoadPolicy load_policy = normalize_load_policy(options);
    ProjectModel model;
    try {
        model = reader.load(file_path, load_policy);
    } catch (const std::exception& error) {
        throw std::runtime_error(build_load_error_message(file_path, error));
    }
    const auto expand_started_at = std::chrono::steady_clock::now();
    analysis::Expander expander(model.types, load_policy.expand_depth, load_policy.lazy_expand_children);
    model.expanded =
        expander.build(model.symbols, options.include_runtime_only, options.only_static_known, options.symbol_name);
    model.metrics.expand_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - expand_started_at)
            .count());
    return model;
}

std::string ProjectLoader::dump_raw_dwarf_json(const std::string& file_path) const
{
    elf::RawDwarfReader reader;
    return render_raw_dwarf_json(reader.load(file_path));
}

std::string render_scan_text(const ProjectModel& model)
{
    const auto summary = summarize(model);
    std::ostringstream stream;
    stream << "file: " << model.file << '\n';
    append_elf_info_lines(stream, model);
    stream << "compile_units: " << summary.compile_unit_count << '\n';
    stream << "types: " << summary.type_count << '\n';
    stream << "symbols: " << summary.symbol_count << '\n';
    stream << "static_address_known: " << summary.static_address_known_count << '\n';
    stream << "runtime_only: " << summary.runtime_only_count << '\n';
    stream << "unavailable: " << summary.unavailable_count << '\n';
    return stream.str();
}

namespace {

    using SymbolPathIndex = std::unordered_map<std::string, const VariableRecord*>;

    [[nodiscard]] SymbolPathIndex build_symbol_path_index(const ProjectModel& model)
    {
        SymbolPathIndex index;
        index.reserve(model.symbols.size());
        for (const auto& symbol : model.symbols) {
            index.emplace(analysis::join_scope(symbol.scope_path, symbol.name), &symbol);
        }
        return index;
    }

    void render_expanded_text(const ExpandedNode& node,
                              const int level,
                              const std::optional<std::int64_t> address_bias,
                              const SymbolPathIndex& symbol_index,
                              std::ostringstream& stream)
    {
        for (int i = 0; i < level; ++i) {
            stream << "  ";
        }
        stream << "- " << node.path << " [" << to_string(node.availability) << "] " << node.type_name;
        const auto symbol_iter = symbol_index.find(node.path);
        const VariableRecord* symbol = symbol_iter != symbol_index.end() ? symbol_iter->second : nullptr;
        if (symbol != nullptr && !symbol->address.location_ranges.empty()) {
            append_location_range_suffix(stream, symbol->address);
        }
        if (node.absolute_address.has_value()) {
            if (address_bias.has_value()) {
                stream << " @" << format_address_summary(node, address_bias.value());
            } else {
                stream << " @0x" << std::hex << node.absolute_address.value() << std::dec;
            }
        }
        if (symbol != nullptr && symbol->const_value.has_value()) {
            stream << " = " << symbol->const_value.value();
        } else if (symbol != nullptr && symbol->const_value_text.has_value()) {
            stream << " = " << symbol->const_value_text.value();
        }
        stream << '\n';
        for (const auto& child : node.children) {
            render_expanded_text(child, level + 1, address_bias, symbol_index, stream);
        }
    }

    std::string render_dump_text_with_bias(const ProjectModel& model, const std::optional<std::int64_t> address_bias)
    {
        std::ostringstream stream;
        stream << "file: " << model.file << '\n';
        append_elf_info_lines(stream, model);
        // 文本导出按 path 建索引，避免每个节点都线性扫描全部符号。
        const SymbolPathIndex symbol_index = build_symbol_path_index(model);
        for (const auto& node : model.expanded) {
            render_expanded_text(node, 0, address_bias, symbol_index, stream);
        }
        return stream.str();
    }

} // namespace

std::string render_dump_text(const ProjectModel& model)
{
    return render_dump_text_with_bias(model, std::nullopt);
}

std::string render_dump_text(const ProjectModel& model, const std::int64_t address_bias)
{
    return render_dump_text_with_bias(model, std::optional<std::int64_t>(address_bias));
}

} // namespace elf_static_view
