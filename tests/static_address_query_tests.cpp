#include "elf_static_view/project.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect_true(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

[[nodiscard]] const elf_static_view::StaticAddressResult* find_result(
    const std::vector<elf_static_view::StaticAddressResult>& results,
    const std::string& key) {
  const auto iter = std::find_if(results.begin(), results.end(), [&](const auto& result) {
    return result.key == key;
  });
  if (iter == results.end()) {
    return nullptr;
  }
  return &(*iter);
}

[[nodiscard]] elf_static_view::TypeNode make_int_type() {
  elf_static_view::TypeNode type;
  type.id = "type:int";
  type.kind = elf_static_view::TypeKind::Base;
  type.name = "int";
  type.byte_size = 4;
  return type;
}

[[nodiscard]] elf_static_view::ProjectModel make_model() {
  elf_static_view::ProjectModel model;
  model.file = "memory-model";
  model.types.push_back(make_int_type());

  elf_static_view::ExpandedNode global_object;
  global_object.path = "demo.global_object";
  global_object.display_name = "global_object";
  global_object.type_name = "Sample";
  global_object.type_id = "type:struct";
  global_object.type_kind = elf_static_view::TypeKind::Struct;
  global_object.availability = elf_static_view::Availability::StaticAddressKnown;
  global_object.absolute_address = 0x1000;
  global_object.children_lazy = true;

  elf_static_view::ExpandedNode counter;
  counter.path = "demo::Derived.counter";
  counter.display_name = "counter";
  counter.type_name = "int";
  counter.type_id = "type:int";
  counter.type_kind = elf_static_view::TypeKind::Base;
  counter.availability = elf_static_view::Availability::StaticAddressKnown;
  counter.absolute_address = 0x2000;

  elf_static_view::ExpandedNode shared;
  shared.path = "demo::Derived.shared";
  shared.display_name = "shared";
  shared.type_name = "int";
  shared.type_id = "type:int";
  shared.type_kind = elf_static_view::TypeKind::Base;
  shared.availability = elf_static_view::Availability::StaticAddressKnown;
  shared.absolute_address = 0x2004;

  elf_static_view::ExpandedNode array;
  array.path = "root.array";
  array.display_name = "array";
  array.type_name = "int[3]";
  array.type_id = "type:int_array";
  array.type_kind = elf_static_view::TypeKind::Array;
  array.availability = elf_static_view::Availability::StaticAddressKnown;
  array.absolute_address = 0x3000;
  array.array_count = 3;
  array.array_stride = 4;

  for (std::uint64_t index = 0; index < 3; ++index) {
    elf_static_view::ExpandedNode element;
    element.path = "root.array[" + std::to_string(index) + "]";
    element.display_name = "array[" + std::to_string(index) + "]";
    element.type_name = "int";
    element.type_id = "type:int";
    element.type_kind = elf_static_view::TypeKind::Base;
    element.availability = elf_static_view::Availability::StaticAddressKnown;
    element.absolute_address = 0x3000 + index * 4;
    array.children.push_back(std::move(element));
  }

  elf_static_view::ExpandedNode tls_value;
  tls_value.path = "demo.tls_value";
  tls_value.display_name = "tls_value";
  tls_value.type_name = "int";
  tls_value.type_id = "type:int";
  tls_value.type_kind = elf_static_view::TypeKind::Base;
  tls_value.availability = elf_static_view::Availability::RuntimeOnly;
  tls_value.absolute_address = 0x4000;

  model.expanded = {global_object, counter, shared, array, tls_value};
  return model;
}

void verify_name_query_and_value_type() {
  const auto model = make_model();

  elf_static_view::StaticAddressQueryOptions options;
  options.name_query_text = "global_object";
  const auto results = elf_static_view::query_static_addresses(model, options);

  const auto* match = find_result(results, "demo.global_object");
  expect_true(match != nullptr, "应返回 demo.global_object");
  expect_true(match->value_type == "Sample", "应返回变量类型名称");
}

void verify_comma_name_query_and_path_rules() {
  const auto model = make_model();

  elf_static_view::StaticAddressQueryOptions options;
  options.name_query_text = "global_object,counter";
  options.path_rules_text = "demo::Derived.**\n!**.shared";
  const auto results = elf_static_view::query_static_addresses(model, options);

  expect_true(find_result(results, "demo::Derived.counter") != nullptr,
              "路径规则应保留 demo::Derived.counter");
  expect_true(find_result(results, "demo.global_object") == nullptr,
              "路径规则应过滤掉不在 demo::Derived 下的命中");
}

void verify_array_expansion() {
  const auto model = make_model();

  elf_static_view::StaticAddressQueryOptions options;
  options.name_query_text = "array";
  options.max_array_elements = 8;
  const auto results = elf_static_view::query_static_addresses(model, options);

  expect_true(find_result(results, "root.array[0]") != nullptr, "数组查询应返回首元素");
  expect_true(find_result(results, "root.array[1]") != nullptr, "数组查询应返回后续元素");
}

void verify_runtime_only_filtered_by_default() {
  const auto model = make_model();

  elf_static_view::StaticAddressQueryOptions options;
  options.name_query_text = "tls_value";
  const auto results = elf_static_view::query_static_addresses(model, options);

  expect_true(results.empty(), "默认不应返回 runtime only 节点");
}

void verify_session_cache_reuse() {
  const auto model = make_model();

  elf_static_view::StaticAddressQuerySession session(model);
  elf_static_view::StaticAddressQueryOptions options;
  options.name_query_text = "global_object";

  const auto first = session.query(options);
  const auto second = session.query(options);

  expect_true(first.size() == second.size(), "相同查询应返回相同数量");
  expect_true(!first.empty(), "缓存测试应命中结果");
  expect_true(first.front().key == second.front().key, "缓存结果 key 应稳定");
  expect_true(first.front().value == second.front().value, "缓存结果 value 应稳定");
  expect_true(first.front().value_type == second.front().value_type, "缓存结果类型应稳定");
}

}  // namespace

int main() {
  try {
    verify_name_query_and_value_type();
    verify_comma_name_query_and_path_rules();
    verify_array_expansion();
    verify_runtime_only_filtered_by_default();
    verify_session_cache_reuse();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "static address query tests failed: %s\n", error.what());
    return 1;
  }
}
