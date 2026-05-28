#pragma once

#include "elf_static_view/project_types.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace elf_static_view {

class ProjectLoader {
public:
  ProjectLoader() = default;

  [[nodiscard]] ProjectModel scan(const std::string& file_path,
                                  const ScanOptions& options = {}) const;
  [[nodiscard]] ProjectModel dump(const std::string& file_path,
                                  const DumpOptions& options = {}) const;
  [[nodiscard]] std::string dump_raw_dwarf_json(const std::string& file_path) const;
};

class StaticAddressQuerySession {
public:
  explicit StaticAddressQuerySession(const ProjectModel& model);
  ~StaticAddressQuerySession();
  StaticAddressQuerySession(StaticAddressQuerySession&&) noexcept;
  StaticAddressQuerySession& operator=(StaticAddressQuerySession&&) noexcept;
  StaticAddressQuerySession(const StaticAddressQuerySession&) = delete;
  StaticAddressQuerySession& operator=(const StaticAddressQuerySession&) = delete;

  [[nodiscard]] std::vector<StaticAddressResult> query(
      const StaticAddressQueryOptions& options = {});

private:
  struct Impl;

  const ProjectModel* model_ = nullptr;
  std::vector<ExpandedNode> expanded_nodes_;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] ProjectSummary summarize(const ProjectModel& model);
[[nodiscard]] std::string render_scan_text(const ProjectModel& model);
[[nodiscard]] std::string render_dump_text(const ProjectModel& model);
[[nodiscard]] std::string render_dump_text(const ProjectModel& model, std::int64_t address_bias);
[[nodiscard]] std::string render_dump_json(const ProjectModel& model);
[[nodiscard]] ProjectModel parse_dump_json(const std::string& json_text);
[[nodiscard]] std::string render_snapshot_json(
    const ProjectSnapshot& snapshot,
    const SnapshotExportOptions& options = {});
[[nodiscard]] ProjectSnapshot parse_snapshot_json(const std::string& json_text);
[[nodiscard]] std::string render_raw_dwarf_json(const RawDwarfDocument& document);
[[nodiscard]] std::string render_expanded_node_json(const ExpandedNode& node);
[[nodiscard]] ProjectSnapshot build_export_snapshot(
    const ProjectSnapshot& snapshot,
    const SnapshotExportOptions& options = {});
[[nodiscard]] std::vector<StaticAddressResult> query_static_addresses(
    const ProjectModel& model,
    const StaticAddressQueryOptions& options = {});
[[nodiscard]] std::vector<StaticAddressResult> query_static_addresses_from_file(
    const std::string& elf_path,
    const StaticAddressQueryOptions& options = {},
    const LoadPolicy& load_policy = {});

}  // namespace elf_static_view
