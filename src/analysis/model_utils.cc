#include "analysis/model_utils.hpp"

#include <sstream>

namespace elf_static_view::analysis {

std::string escape_json(const std::string& value)
{
    std::ostringstream stream;
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                stream << "\\\\";
                break;
            case '"':
                stream << "\\\"";
                break;
            case '\n':
                stream << "\\n";
                break;
            case '\r':
                stream << "\\r";
                break;
            case '\t':
                stream << "\\t";
                break;
            default:
                stream << ch;
                break;
        }
    }
    return stream.str();
}

std::string join_scope(const std::vector<std::string>& scope_path, const std::string& name)
{
    std::ostringstream stream;
    for (const auto& item : scope_path) {
        if (!item.empty()) {
            if (stream.tellp() > 0) {
                stream << "::";
            }
            stream << item;
        }
    }
    if (!name.empty()) {
        if (stream.tellp() > 0) {
            stream << "::";
        }
        stream << name;
    }
    return stream.str();
}

bool should_emit_symbol(const VariableRecord& record,
                        bool include_runtime_only,
                        bool only_static_known,
                        const std::optional<std::string>& symbol_name)
{
    if (only_static_known && record.availability != Availability::StaticAddressKnown) {
        return false;
    }
    if (!include_runtime_only &&
        (record.availability == Availability::RuntimeOnly || record.availability == Availability::Unavailable ||
         record.availability == Availability::OptimizedOut)) {
        return false;
    }
    if (symbol_name.has_value()) {
        const auto full_name = join_scope(record.scope_path, record.name);
        return record.name == symbol_name.value() || full_name == symbol_name.value();
    }
    return true;
}

} // namespace elf_static_view::analysis
