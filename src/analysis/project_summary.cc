#include "elf_static_view/project.hpp"

namespace elf_static_view {

ProjectSummary summarize(const ProjectModel& model)
{
    ProjectSummary summary;
    summary.compile_unit_count = model.compile_units.size();
    summary.type_count = model.types.size();
    summary.symbol_count = model.symbols.size();
    for (const auto& symbol : model.symbols) {
        switch (symbol.availability) {
            case Availability::StaticAddressKnown:
                ++summary.static_address_known_count;
                break;
            case Availability::RuntimeOnly:
                ++summary.runtime_only_count;
                break;
            case Availability::Unavailable:
            case Availability::OptimizedOut:
                ++summary.unavailable_count;
                break;
            case Availability::StaticLayoutKnown:
                break;
        }
    }
    return summary;
}

} // namespace elf_static_view
