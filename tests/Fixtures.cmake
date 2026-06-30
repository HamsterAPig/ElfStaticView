find_program(
  ELF_STATIC_VIEW_CLANG
  NAMES clang.exe clang
  REQUIRED
)
find_program(
  ELF_STATIC_VIEW_CLANGXX
  NAMES clang++.exe clang++
  REQUIRED
)
find_program(
  ELF_STATIC_VIEW_GCC
  NAMES gcc.exe gcc
  REQUIRED
)
find_program(
  ELF_STATIC_VIEW_GXX
  NAMES g++.exe g++
  REQUIRED
)
find_program(
  ELF_STATIC_VIEW_OBJCOPY
  NAMES objcopy.exe objcopy
  REQUIRED
)
find_program(
  ELF_STATIC_VIEW_LLVM_OBJCOPY
  NAMES llvm-objcopy.exe llvm-objcopy
  REQUIRED
)
find_program(
  ELF_STATIC_VIEW_LLVM_DWP
  NAMES llvm-dwp.exe llvm-dwp
  REQUIRED
)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(FIXTURE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/fixtures")
file(MAKE_DIRECTORY "${FIXTURE_OUTPUT_DIR}")

function(elf_static_view_normalize_fixture_path output_var path)
  if("${path}" MATCHES "^\\$<")
    set("${output_var}" "" PARENT_SCOPE)
    return()
  endif()

  get_filename_component(normalized_path "${path}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  file(TO_CMAKE_PATH "${normalized_path}" normalized_path)
  set("${output_var}" "${normalized_path}" PARENT_SCOPE)
endfunction()

function(elf_static_view_fixture_output_property output_var output_file)
  elf_static_view_normalize_fixture_path(normalized_output_file "${output_file}")
  if(NOT normalized_output_file)
    set("${output_var}" "" PARENT_SCOPE)
    return()
  endif()

  string(MD5 output_hash "${normalized_output_file}")
  set("${output_var}" "ELF_STATIC_VIEW_FIXTURE_OUTPUT_${output_hash}" PARENT_SCOPE)
endfunction()

function(elf_static_view_register_fixture_output target output_file)
  elf_static_view_fixture_output_property(fixture_property "${output_file}")
  if(fixture_property)
    # 记录输出文件所属的 fixture target，用于派生 fixture 自动补 target 级时序依赖。
    set_property(GLOBAL PROPERTY "${fixture_property}" "${target}_fixture")
  endif()
endfunction()

function(elf_static_view_add_fixture_input_dependencies target)
  foreach(input_file IN LISTS ARGN)
    elf_static_view_fixture_output_property(fixture_property "${input_file}")
    if(fixture_property)
      get_property(parent_fixture_target GLOBAL PROPERTY "${fixture_property}")
      if(parent_fixture_target AND NOT parent_fixture_target STREQUAL "${target}_fixture")
        add_dependencies("${target}_fixture" "${parent_fixture_target}")
      endif()
    endif()
  endforeach()
endfunction()

function(add_linux_elf_fixture target source_file compiler language_standard)
  get_filename_component(source_name "${source_file}" NAME_WE)
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND
      "${compiler}"
      "--target=x86_64-unknown-linux-gnu"
      "-g"
      ${ARGN}
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
  elf_static_view_register_fixture_output("${target}" "${output_file}")
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()

function(add_linux_split_dwarf_fixture target source_file compiler language_standard)
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  get_filename_component(target_stem "${target}" NAME_WE)
  set(sidecar_output_file "${output_file}-${target_stem}.dwo")
  add_custom_command(
    OUTPUT "${output_file}" "${sidecar_output_file}"
    COMMAND
      "${compiler}"
      "--target=x86_64-unknown-linux-gnu"
      "-g"
      "-gdwarf-5"
      "-gsplit-dwarf"
      "-fdebug-default-version=5"
      ${ARGN}
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
    COMMENT "Building split DWARF fixture ${target}"
  )
  add_custom_target("${target}_fixture" DEPENDS "${output_file}" "${sidecar_output_file}")
  elf_static_view_register_fixture_output("${target}" "${output_file}")
  elf_static_view_register_fixture_output("${target}" "${sidecar_output_file}")
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
  set("${target}_SIDECAR_OUTPUT" "${sidecar_output_file}" PARENT_SCOPE)
endfunction()

function(add_dwp_fixture target input_file)
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND
      "${ELF_STATIC_VIEW_LLVM_DWP}"
      "${input_file}"
      "-o"
      "${output_file}"
    DEPENDS "${input_file}"
    VERBATIM
    COMMENT "Building DWP fixture ${target}"
  )
  add_custom_target("${target}_fixture" DEPENDS "${output_file}")
  elf_static_view_add_fixture_input_dependencies("${target}" "${input_file}")
  elf_static_view_register_fixture_output("${target}" "${output_file}")
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
  elf_static_view_register_fixture_output("${target}" "${output_file}")
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()

function(add_objcopy_elf_fixture target source_file compiler language_standard output_format)
  get_filename_component(source_name "${source_file}" NAME_WE)
  set(intermediate_file "${FIXTURE_OUTPUT_DIR}/${source_name}.exe")
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND
      "${compiler}"
      "-g"
      "-O2"
      "${language_standard}"
      "-o"
      "${intermediate_file}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
    COMMAND
      "${ELF_STATIC_VIEW_OBJCOPY}"
      "-O"
      "${output_format}"
      "${intermediate_file}"
      "${output_file}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
    VERBATIM
    COMMENT "Building objcopy ELF fixture ${target}"
  )
  add_custom_target("${target}_fixture" DEPENDS "${output_file}")
  elf_static_view_register_fixture_output("${target}" "${output_file}")
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()

function(add_patched_elf_fixture target input_file patch_command)
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scripts/patch_fixture.py"
      "${patch_command}"
      "-InputPath"
      "${input_file}"
      "-OutputPath"
      "${output_file}"
      ${ARGN}
    DEPENDS
      "${input_file}"
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scripts/patch_fixture.py"
    VERBATIM
    COMMENT "Building patched ELF fixture ${target}"
  )
  add_custom_target("${target}_fixture" DEPENDS "${output_file}")
  elf_static_view_add_fixture_input_dependencies("${target}" "${input_file}")
  elf_static_view_register_fixture_output("${target}" "${output_file}")
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()

function(add_dwarfgen_debug_sup_fixture target input_file)
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND
      "$<TARGET_FILE:dwarfgen>"
      "-t"
      "obj"
      "--add-debug-sup"
      "-o"
      "${output_file}"
      "${input_file}"
    DEPENDS dwarfgen "${input_file}"
    VERBATIM
    COMMENT "Building dwarfgen .debug_sup fixture ${target}"
  )
  add_custom_target("${target}_fixture" DEPENDS "${output_file}")
  elf_static_view_add_fixture_input_dependencies("${target}" "${input_file}")
  elf_static_view_register_fixture_output("${target}" "${output_file}")
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()

function(add_gcc_dwarf5_objcopy_fixture target source_file language_standard)
  get_filename_component(source_name "${source_file}" NAME_WE)
  set(intermediate_file "${FIXTURE_OUTPUT_DIR}/${source_name}.gcc.exe")
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  set(compiler "${ELF_STATIC_VIEW_GXX}")
  set(language_args "${language_standard}")
  if("${language_standard}" STREQUAL "-x c")
    set(compiler "${ELF_STATIC_VIEW_GCC}")
    set(language_args "-x" "c")
  endif()
  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND
      "${compiler}"
      "-g"
      "-gdwarf-5"
      "-O0"
      ${language_args}
      "-o"
      "${intermediate_file}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
    COMMAND
      "${ELF_STATIC_VIEW_OBJCOPY}"
      "-O"
      "elf64-x86-64"
      "${intermediate_file}"
      "${output_file}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
    VERBATIM
    COMMENT "Building GCC DWARF5 objcopy fixture ${target}"
  )
  add_custom_target("${target}_fixture" DEPENDS "${output_file}")
  elf_static_view_register_fixture_output("${target}" "${output_file}")
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()

function(add_gcc_dwarf64_objcopy_fixture target source_file language_standard)
  get_filename_component(source_name "${source_file}" NAME_WE)
  set(intermediate_file "${FIXTURE_OUTPUT_DIR}/${source_name}.gcc64.exe")
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND
      "${ELF_STATIC_VIEW_GXX}"
      "-g"
      "-gdwarf-5"
      "-gdwarf64"
      "-O0"
      "${language_standard}"
      "-o"
      "${intermediate_file}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
    COMMAND
      "${ELF_STATIC_VIEW_OBJCOPY}"
      "-O"
      "elf64-x86-64"
      "${intermediate_file}"
      "${output_file}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
    VERBATIM
    COMMENT "Building GCC DWARF64 objcopy fixture ${target}"
  )
  add_custom_target("${target}_fixture" DEPENDS "${output_file}")
  elf_static_view_register_fixture_output("${target}" "${output_file}")
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()

function(add_gcc_dwarf5_objcopy_fixture_from_sources target)
  get_filename_component(target_name "${target}" NAME_WE)
  set(intermediate_file "${FIXTURE_OUTPUT_DIR}/${target_name}.gcc.exe")
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  set(source_paths)
  foreach(source_file IN LISTS ARGN)
    list(APPEND source_paths "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
  endforeach()
  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND
      "${ELF_STATIC_VIEW_GCC}"
      "-g"
      "-gdwarf-5"
      "-O0"
      "-std=c17"
      ${source_paths}
      "-o"
      "${intermediate_file}"
    COMMAND
      "${ELF_STATIC_VIEW_OBJCOPY}"
      "-O"
      "elf64-x86-64"
      "${intermediate_file}"
      "${output_file}"
    DEPENDS ${source_paths}
    VERBATIM
    COMMENT "Building GCC DWARF5 objcopy fixture ${target}"
  )
  add_custom_target("${target}_fixture" DEPENDS "${output_file}")
  elf_static_view_register_fixture_output("${target}" "${output_file}")
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()
