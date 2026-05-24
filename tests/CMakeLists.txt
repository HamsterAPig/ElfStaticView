find_program(
  ELF_STATIC_VIEW_CLANG
  NAMES clang.exe clang
  HINTS "D:/ProgramData/ClionComplier/ClangLLVM/bin"
  REQUIRED
)
find_program(
  ELF_STATIC_VIEW_CLANGXX
  NAMES clang++.exe clang++
  HINTS "D:/ProgramData/ClionComplier/ClangLLVM/bin"
  REQUIRED
)

set(FIXTURE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/fixtures")
file(MAKE_DIRECTORY "${FIXTURE_OUTPUT_DIR}")

function(add_linux_elf_fixture target source_file compiler language_standard)
  get_filename_component(source_name "${source_file}" NAME_WE)
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND
      "${compiler}"
      "--target=x86_64-unknown-linux-gnu"
      "-g"
      "-O0"
      "${language_standard}"
      "-fuse-ld=lld"
      "-nostdlib"
      "-Wl,-e,main"
      "-Wl,--build-id=none"
      "-o"
      "${output_file}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
    VERBATIM
    COMMENT "Building ELF fixture ${target}"
  )
  add_custom_target("${target}_fixture" DEPENDS "${output_file}")
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()

function(add_linux_elf_fixture_from_sources target compiler language_standard)
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  set(source_paths)
  foreach(source_file IN LISTS ARGN)
    list(APPEND source_paths "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
  endforeach()
  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND
      "${compiler}"
      "--target=x86_64-unknown-linux-gnu"
      "-g"
      "-O0"
      "${language_standard}"
      "-fuse-ld=lld"
      "-nostdlib"
      "-Wl,-e,main"
      "-Wl,--build-id=none"
      "-o"
      "${output_file}"
      ${source_paths}
    DEPENDS ${source_paths}
    VERBATIM
    COMMENT "Building ELF fixture ${target}"
  )
  add_custom_target("${target}_fixture" DEPENDS "${output_file}")
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()

add_linux_elf_fixture(static_vars_c.elf fixtures/c/static_vars.c "${ELF_STATIC_VIEW_CLANG}" "-std=c17")
add_linux_elf_fixture(static_vars_cpp.elf fixtures/cpp/static_vars.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20")
add_linux_elf_fixture_from_sources(
  multi_cu_static_vars.elf
  "${ELF_STATIC_VIEW_CLANG}"
  "-std=c17"
  fixtures/c/multi_cu/main.c
  fixtures/c/multi_cu/unit_a.c
  fixtures/c/multi_cu/unit_b.c
)

add_executable(elf_static_view_tests
  test_main.cpp
  ../src/ui/version_check.cpp
)

target_include_directories(
  elf_static_view_tests
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
)
target_link_libraries(
  elf_static_view_tests
  PRIVATE
    elf_static_view_core
    elf_static_view_ui_support
    yaml-cpp::yaml-cpp
    elf_static_view_warnings
)

if(WIN32)
  target_link_libraries(elf_static_view_tests PRIVATE winhttp)
endif()

add_dependencies(elf_static_view_tests static_vars_c.elf_fixture static_vars_cpp.elf_fixture)
add_dependencies(elf_static_view_tests multi_cu_static_vars.elf_fixture)

add_dependencies(elf_static_view_tests elf_static_view_version_header)

target_compile_definitions(
  elf_static_view_tests
  PRIVATE
    ELF_STATIC_VIEW_C_FIXTURE_PATH="${static_vars_c.elf_OUTPUT}"
    ELF_STATIC_VIEW_CPP_FIXTURE_PATH="${static_vars_cpp.elf_OUTPUT}"
    ELF_STATIC_VIEW_MULTI_CU_FIXTURE_PATH="${multi_cu_static_vars.elf_OUTPUT}"
    ELF_STATIC_VIEW_C_EXPECTED_JSON="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/c/static_vars.expected.json"
    ELF_STATIC_VIEW_CPP_EXPECTED_JSON="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cpp/static_vars.expected.json"
)

add_test(NAME elf_static_view_tests COMMAND elf_static_view_tests)
