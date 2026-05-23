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

add_linux_elf_fixture(static_vars_c.elf fixtures/c/static_vars.c "${ELF_STATIC_VIEW_CLANG}" "-std=c17")
add_linux_elf_fixture(static_vars_cpp.elf fixtures/cpp/static_vars.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20")

add_executable(elf_static_view_tests
  test_main.cpp
)

target_include_directories(
  elf_static_view_tests
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(
  elf_static_view_tests
  PRIVATE
    elf_static_view_core
    elf_static_view_ui_support
    elf_static_view_warnings
)

add_dependencies(elf_static_view_tests static_vars_c.elf_fixture static_vars_cpp.elf_fixture)

target_compile_definitions(
  elf_static_view_tests
  PRIVATE
    ELF_STATIC_VIEW_C_FIXTURE_PATH="${static_vars_c.elf_OUTPUT}"
    ELF_STATIC_VIEW_CPP_FIXTURE_PATH="${static_vars_cpp.elf_OUTPUT}"
    ELF_STATIC_VIEW_C_EXPECTED_JSON="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/c/static_vars.expected.json"
    ELF_STATIC_VIEW_CPP_EXPECTED_JSON="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cpp/static_vars.expected.json"
)

add_test(NAME elf_static_view_tests COMMAND elf_static_view_tests)
