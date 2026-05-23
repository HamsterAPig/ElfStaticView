#pragma once

#include "elf_static_view/project_types.hpp"

#include <string>

namespace elf_static_view {

class ProjectLoader {
public:
  ProjectLoader() = default;

  [[nodiscard]] ProjectModel scan(const std::string& file_path,
                                  const ScanOptions& options = {}) const;
  [[nodiscard]] ProjectModel dump(const std::string& file_path,
                                  const DumpOptions& options = {}) const;
};

[[nodiscard]] ProjectSummary summarize(const ProjectModel& model);
[[nodiscard]] std::string render_scan_text(const ProjectModel& model);
[[nodiscard]] std::string render_dump_text(const ProjectModel& model);
[[nodiscard]] std::string render_dump_json(const ProjectModel& model);
[[nodiscard]] ProjectModel parse_dump_json(const std::string& json_text);
[[nodiscard]] std::string render_snapshot_json(const ProjectSnapshot& snapshot);
[[nodiscard]] ProjectSnapshot parse_snapshot_json(const std::string& json_text);
[[nodiscard]] std::string render_expanded_node_json(const ExpandedNode& node);

}  // namespace elf_static_view
