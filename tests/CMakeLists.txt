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
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()

function(add_patched_elf_fixture target input_file script_file)
  set(output_file "${FIXTURE_OUTPUT_DIR}/${target}")
  add_custom_command(
    OUTPUT "${output_file}"
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scripts/patch_fixture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/${script_file}"
      "-InputPath"
      "${input_file}"
      "-OutputPath"
      "${output_file}"
      ${ARGN}
    DEPENDS
      "${input_file}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${script_file}"
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scripts/patch_fixture.py"
    VERBATIM
    COMMENT "Building patched ELF fixture ${target}"
  )
  add_custom_target("${target}_fixture" DEPENDS "${output_file}")
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
  set("${target}_OUTPUT" "${output_file}" PARENT_SCOPE)
endfunction()

add_gcc_dwarf5_objcopy_fixture(static_vars_c.elf fixtures/c/static_vars.c "-x c")
add_gcc_dwarf5_objcopy_fixture(atomic_type.elf fixtures/c/atomic_type.c "-x c")
add_linux_elf_fixture(static_vars_cpp.elf fixtures/cpp/static_vars.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-O0")
add_patched_elf_fixture(gnu_addr_index.elf "${static_vars_cpp.elf_OUTPUT}" fixtures/scripts/patch_addrx_to_gnu_addr_index.ps1)
add_gcc_dwarf5_objcopy_fixture(gcc_dwarf5_strp.elf fixtures/cpp/debug_sup_minimal.cpp "-std=c++20")
add_gcc_dwarf64_objcopy_fixture(gcc_dwarf64_ref8.elf fixtures/cpp/debug_sup_minimal.cpp "-std=c++20")
add_linux_elf_fixture(inline_scope_static.elf fixtures/cpp/inline_scope_static.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-O0")
add_linux_elf_fixture(const_value_piece.elf fixtures/cpp/const_value_piece.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-O2")
add_linux_elf_fixture(piece_stack_value.elf fixtures/cpp/piece_stack_value.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-O2")
add_objcopy_elf_fixture(bit_piece_gcc.elf fixtures/cpp/bit_piece_gcc.cpp "${ELF_STATIC_VIEW_GXX}" "-std=c++20" "elf64-x86-64")
add_patched_elf_fixture(rnglistx_bit_piece.elf "${bit_piece_gcc.elf_OUTPUT}" fixtures/scripts/patch_ranges_to_rnglistx.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(rnglists_start_end_bit_piece.elf "${rnglistx_bit_piece.elf_OUTPUT}" fixtures/scripts/patch_rnglists_start_end.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(rnglists_offset_pair_bit_piece.elf "${rnglistx_bit_piece.elf_OUTPUT}" fixtures/scripts/patch_rnglists_offset_pair.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(rnglists_base_addressx_bit_piece.elf "${rnglistx_bit_piece.elf_OUTPUT}" fixtures/scripts/patch_rnglists_base_addressx.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(rnglists_startx_endx_bit_piece.elf "${rnglistx_bit_piece.elf_OUTPUT}" fixtures/scripts/patch_rnglists_startx_endx.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(rnglists_startx_length_bit_piece.elf "${rnglistx_bit_piece.elf_OUTPUT}" fixtures/scripts/patch_rnglists_startx_length.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_linux_elf_fixture(ref_sig8_debug_types_base.elf fixtures/cpp/ref_sig8_debug_types.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-gdwarf-4" "-fdebug-types-section" "-O0")
add_patched_elf_fixture(ref_sig8_debug_types.elf "${ref_sig8_debug_types_base.elf_OUTPUT}" fixtures/scripts/patch_ref_sig8_debug_types.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(ref_sig8_debug_types_indirect.elf "${ref_sig8_debug_types.elf_OUTPUT}" fixtures/scripts/patch_debug_types_name_indirect.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_linux_elf_fixture(dwarf5_strx_type_unit.elf fixtures/cpp/dwarf5_strx_type_unit.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-gdwarf-5" "-fdebug-types-section" "-fdebug-default-version=5" "-O0")
add_patched_elf_fixture(dwarf5_strx_type_unit_abbrev_offset.elf "${dwarf5_strx_type_unit.elf_OUTPUT}" fixtures/scripts/patch_type_unit_abbrev_offset.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(dwarf5_strx2_type_unit.elf "${dwarf5_strx_type_unit.elf_OUTPUT}" fixtures/scripts/patch_dwarf5_strx_form.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}" -Mode strx2)
add_patched_elf_fixture(dwarf5_strx3_type_unit.elf "${dwarf5_strx_type_unit.elf_OUTPUT}" fixtures/scripts/patch_dwarf5_strx_form.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}" -Mode strx3)
add_patched_elf_fixture(dwarf5_strx4_type_unit.elf "${dwarf5_strx_type_unit.elf_OUTPUT}" fixtures/scripts/patch_dwarf5_strx_form.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}" -Mode strx4)
add_patched_elf_fixture(dwarf5_addrx1_type_unit.elf "${dwarf5_strx_type_unit.elf_OUTPUT}" fixtures/scripts/patch_dwarf5_addrx_form.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}" -Mode addrx1)
add_patched_elf_fixture(dwarf5_addrx2_type_unit.elf "${dwarf5_strx_type_unit.elf_OUTPUT}" fixtures/scripts/patch_dwarf5_addrx_form.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}" -Mode addrx2)
add_patched_elf_fixture(dwarf5_addrx3_type_unit.elf "${dwarf5_strx_type_unit.elf_OUTPUT}" fixtures/scripts/patch_dwarf5_addrx_form.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}" -Mode addrx3)
add_patched_elf_fixture(dwarf5_addrx4_type_unit.elf "${dwarf5_strx_type_unit.elf_OUTPUT}" fixtures/scripts/patch_dwarf5_addrx_form.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}" -Mode addrx4)
add_linux_split_dwarf_fixture(split_dwarf_static.elf fixtures/cpp/split_dwarf_static.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-O0")
add_dwp_fixture(split_dwarf_static.dwp "${split_dwarf_static.elf_SIDECAR_OUTPUT}")
add_dwarfgen_debug_sup_fixture(debug_sup_from_obj.elf "${static_vars_cpp.elf_OUTPUT}")
add_patched_elf_fixture(debug_sup_sidecar.elf "${static_vars_cpp.elf_OUTPUT}" fixtures/scripts/build_debug_sup_sidecar.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_custom_command(
  OUTPUT "${FIXTURE_OUTPUT_DIR}/debug_sup_gcc_strp_sup.bin"
  COMMAND
    "$<TARGET_FILE:debug_sup_form_builder>"
    "--output"
    "${FIXTURE_OUTPUT_DIR}/debug_sup_gcc_strp_sup.bin"
    "--sup-filename"
    "gcc_dwarf5_strp.elf"
  DEPENDS debug_sup_form_builder
  VERBATIM
  COMMENT "Building raw .debug_sup bytes for gcc strp_sup fixture"
)
add_custom_target(debug_sup_gcc_strp_sup.bin_fixture DEPENDS "${FIXTURE_OUTPUT_DIR}/debug_sup_gcc_strp_sup.bin")
add_patched_elf_fixture(
  gcc_dwarf5_strp_sup.elf
  "${gcc_dwarf5_strp.elf_OUTPUT}"
  fixtures/scripts/patch_gcc_strp_to_strp_sup.ps1
  -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}"
  -DebugSupBytesPath "${FIXTURE_OUTPUT_DIR}/debug_sup_gcc_strp_sup.bin"
)
add_patched_elf_fixture(
  gcc_dwarf5_ref_sup4.elf
  "${gcc_dwarf5_strp_sup.elf_OUTPUT}"
  fixtures/scripts/patch_gcc_ref4_to_ref_sup4.ps1
  -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}"
)
add_patched_elf_fixture(
  gcc_dwarf5_ref_addr.elf
  "${gcc_dwarf5_strp.elf_OUTPUT}"
  fixtures/scripts/patch_gcc_ref4_to_ref_addr.ps1
  -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}"
)
add_patched_elf_fixture(
  gcc_dwarf5_ref1.elf
  "${gcc_dwarf5_strp.elf_OUTPUT}"
  fixtures/scripts/patch_gcc_ref4_to_small_ref.ps1
  -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}"
  -Mode ref1
)
add_patched_elf_fixture(
  gcc_dwarf5_ref2.elf
  "${gcc_dwarf5_strp.elf_OUTPUT}"
  fixtures/scripts/patch_gcc_ref4_to_small_ref.ps1
  -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}"
  -Mode ref2
)
add_patched_elf_fixture(
  gcc_dwarf5_ref_udata.elf
  "${gcc_dwarf5_strp.elf_OUTPUT}"
  fixtures/scripts/patch_gcc_ref4_to_small_ref.ps1
  -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}"
  -Mode ref_udata
)
add_patched_elf_fixture(
  gcc_dwarf64_ref_sup8.elf
  "${gcc_dwarf64_ref8.elf_OUTPUT}"
  fixtures/scripts/patch_gcc_ref8_to_ref_sup8.ps1
  -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}"
)
add_patched_elf_fixture(
  gcc_gnu_alt.elf
  "${gcc_dwarf5_strp.elf_OUTPUT}"
  fixtures/scripts/patch_gcc_gnu_altlink.ps1
  -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}"
  -AltFileName "gcc_dwarf5_strp.elf"
)
add_linux_elf_fixture(unspecified_type.elf fixtures/cpp/unspecified_type.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-O0")
add_linux_elf_fixture(class_array_nested_expand.elf fixtures/cpp/class_array_nested_expand.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-O0")
add_linux_elf_fixture(abstract_origin_values.elf fixtures/cpp/abstract_origin_values.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-O2")
add_linux_elf_fixture(dwarf5_loclists.elf fixtures/cpp/dwarf5_loclists.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-gdwarf-5" "-O2")
add_patched_elf_fixture(dwarf5_loclists_base_default.elf "${dwarf5_loclists.elf_OUTPUT}" fixtures/scripts/patch_loclists_base_default.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(dwarf5_loclists_base_addressx.elf "${dwarf5_loclists.elf_OUTPUT}" fixtures/scripts/patch_loclists_base_addressx.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(dwarf5_loclists_start_end.elf "${dwarf5_loclists_base_addressx.elf_OUTPUT}" fixtures/scripts/patch_loclists_start_end.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(dwarf5_loclists_startx_endx.elf "${dwarf5_loclists_base_addressx.elf_OUTPUT}" fixtures/scripts/patch_loclists_startx_endx.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(dwarf5_loclists_startx_length.elf "${dwarf5_loclists_base_addressx.elf_OUTPUT}" fixtures/scripts/patch_loclists_startx_length.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_patched_elf_fixture(dwarf5_loclists_start_length.elf "${dwarf5_loclists_base_addressx.elf_OUTPUT}" fixtures/scripts/patch_loclists_start_length.ps1 -ObjcopyPath "${ELF_STATIC_VIEW_LLVM_OBJCOPY}")
add_linux_elf_fixture(specification_static.elf fixtures/cpp/specification_static.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-gdwarf-5" "-O0")
add_linux_elf_fixture(thread_local_tls.elf fixtures/cpp/thread_local_tls.cpp "${ELF_STATIC_VIEW_CLANGXX}" "-std=c++20" "-gdwarf-5" "-O0")
add_gcc_dwarf5_objcopy_fixture_from_sources(
  multi_cu_static_vars.elf
  fixtures/c/multi_cu/main.c
  fixtures/c/multi_cu/unit_a.c
  fixtures/c/multi_cu/unit_b.c
)

add_executable(elf_static_view_tests
  test_main.cpp
  ../src/ui/version_check.cpp
)

add_executable(
  elf_static_view_perf_tests
  perf_policy_tests.cpp
  ../src/ui/version_check.cpp
)

add_executable(
  debug_sup_form_builder
  fixtures/scripts/debug_sup_form_builder.cpp
)

target_include_directories(
  debug_sup_form_builder
  PRIVATE
    "${CMAKE_SOURCE_DIR}/3rdparty/libdwarf-code/src/lib/libdwarf"
    "${CMAKE_SOURCE_DIR}/3rdparty/libdwarf-code/src/lib/libdwarfp"
)

target_link_libraries(
  debug_sup_form_builder
  PRIVATE
    dwarfp
    dwarf
    elf_static_view_warnings
)

target_include_directories(
  elf_static_view_tests
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
)
target_include_directories(
  elf_static_view_perf_tests
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
target_link_libraries(
  elf_static_view_perf_tests
  PRIVATE
    elf_static_view_core
    elf_static_view_ui_support
    yaml-cpp::yaml-cpp
    elf_static_view_warnings
)

if(WIN32)
  target_link_libraries(elf_static_view_tests PRIVATE winhttp)
  target_link_libraries(elf_static_view_perf_tests PRIVATE winhttp)
endif()

add_dependencies(elf_static_view_perf_tests elf_static_view_version_header)

add_dependencies(elf_static_view_tests static_vars_c.elf_fixture static_vars_cpp.elf_fixture)
add_dependencies(elf_static_view_tests gnu_addr_index.elf_fixture)
add_dependencies(elf_static_view_tests atomic_type.elf_fixture)
add_dependencies(elf_static_view_tests multi_cu_static_vars.elf_fixture)
add_dependencies(elf_static_view_tests inline_scope_static.elf_fixture)
add_dependencies(elf_static_view_tests const_value_piece.elf_fixture)
add_dependencies(elf_static_view_tests piece_stack_value.elf_fixture)
add_dependencies(elf_static_view_tests bit_piece_gcc.elf_fixture)
add_dependencies(elf_static_view_tests rnglistx_bit_piece.elf_fixture)
add_dependencies(elf_static_view_tests rnglists_start_end_bit_piece.elf_fixture)
add_dependencies(elf_static_view_tests rnglists_offset_pair_bit_piece.elf_fixture)
add_dependencies(elf_static_view_tests rnglists_base_addressx_bit_piece.elf_fixture)
add_dependencies(elf_static_view_tests rnglists_startx_endx_bit_piece.elf_fixture)
add_dependencies(elf_static_view_tests rnglists_startx_length_bit_piece.elf_fixture)
add_dependencies(elf_static_view_tests ref_sig8_debug_types.elf_fixture)
add_dependencies(elf_static_view_tests ref_sig8_debug_types_indirect.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_strx_type_unit.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_strx_type_unit_abbrev_offset.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_strx2_type_unit.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_strx3_type_unit.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_strx4_type_unit.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_addrx1_type_unit.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_addrx2_type_unit.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_addrx3_type_unit.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_addrx4_type_unit.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_loclists_startx_length.elf_fixture)
add_dependencies(elf_static_view_tests split_dwarf_static.elf_fixture)
add_dependencies(elf_static_view_tests split_dwarf_static.dwp_fixture)
add_dependencies(elf_static_view_tests debug_sup_from_obj.elf_fixture)
add_dependencies(elf_static_view_tests debug_sup_sidecar.elf_fixture)
add_dependencies(elf_static_view_tests gcc_dwarf5_strp.elf_fixture)
add_dependencies(elf_static_view_tests gcc_dwarf64_ref8.elf_fixture)
add_dependencies(elf_static_view_tests debug_sup_gcc_strp_sup.bin_fixture)
add_dependencies(gcc_dwarf5_strp_sup.elf_fixture debug_sup_gcc_strp_sup.bin_fixture)
add_dependencies(elf_static_view_tests gcc_dwarf5_strp_sup.elf_fixture)
add_dependencies(elf_static_view_tests gcc_dwarf5_ref_addr.elf_fixture)
add_dependencies(elf_static_view_tests gcc_dwarf5_ref1.elf_fixture)
add_dependencies(elf_static_view_tests gcc_dwarf5_ref2.elf_fixture)
add_dependencies(elf_static_view_tests gcc_dwarf5_ref_udata.elf_fixture)
add_dependencies(gcc_dwarf5_ref_sup4.elf_fixture gcc_dwarf5_strp_sup.elf_fixture)
add_dependencies(elf_static_view_tests gcc_dwarf5_ref_sup4.elf_fixture)
add_dependencies(elf_static_view_tests gcc_dwarf64_ref_sup8.elf_fixture)
add_dependencies(elf_static_view_tests gcc_gnu_alt.elf_fixture)
add_dependencies(dwarf5_loclists_base_default.elf_fixture dwarf5_loclists.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_loclists_base_default.elf_fixture)
add_dependencies(dwarf5_loclists_base_addressx.elf_fixture dwarf5_loclists.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_loclists_base_addressx.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_loclists_start_end.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_loclists_startx_endx.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_loclists_startx_length.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_loclists_start_length.elf_fixture)
add_dependencies(elf_static_view_tests unspecified_type.elf_fixture)
add_dependencies(elf_static_view_tests class_array_nested_expand.elf_fixture)
add_dependencies(elf_static_view_tests abstract_origin_values.elf_fixture)
add_dependencies(elf_static_view_tests dwarf5_loclists.elf_fixture)
add_dependencies(elf_static_view_tests specification_static.elf_fixture)
add_dependencies(elf_static_view_tests thread_local_tls.elf_fixture)

add_dependencies(elf_static_view_tests elf_static_view_version_header)
add_dependencies(elf_static_view_tests dwarfdump)

target_compile_definitions(
  elf_static_view_tests
  PRIVATE
    ELF_STATIC_VIEW_DWARFDUMP_PATH="$<TARGET_FILE:dwarfdump>"
    ELF_STATIC_VIEW_C_FIXTURE_PATH="${static_vars_c.elf_OUTPUT}"
    ELF_STATIC_VIEW_GNU_ADDR_INDEX_FIXTURE_PATH="${gnu_addr_index.elf_OUTPUT}"
    ELF_STATIC_VIEW_ATOMIC_TYPE_FIXTURE_PATH="${atomic_type.elf_OUTPUT}"
    ELF_STATIC_VIEW_CPP_FIXTURE_PATH="${static_vars_cpp.elf_OUTPUT}"
    ELF_STATIC_VIEW_INLINE_SCOPE_FIXTURE_PATH="${inline_scope_static.elf_OUTPUT}"
    ELF_STATIC_VIEW_CONST_VALUE_FIXTURE_PATH="${const_value_piece.elf_OUTPUT}"
    ELF_STATIC_VIEW_PIECE_STACK_VALUE_FIXTURE_PATH="${piece_stack_value.elf_OUTPUT}"
    ELF_STATIC_VIEW_BIT_PIECE_FIXTURE_PATH="${bit_piece_gcc.elf_OUTPUT}"
    ELF_STATIC_VIEW_RNGLISTX_FIXTURE_PATH="${rnglistx_bit_piece.elf_OUTPUT}"
    ELF_STATIC_VIEW_RNGLISTS_START_END_FIXTURE_PATH="${rnglists_start_end_bit_piece.elf_OUTPUT}"
    ELF_STATIC_VIEW_RNGLISTS_OFFSET_PAIR_FIXTURE_PATH="${rnglists_offset_pair_bit_piece.elf_OUTPUT}"
    ELF_STATIC_VIEW_RNGLISTS_BASE_ADDRESSX_FIXTURE_PATH="${rnglists_base_addressx_bit_piece.elf_OUTPUT}"
    ELF_STATIC_VIEW_RNGLISTS_STARTX_ENDX_FIXTURE_PATH="${rnglists_startx_endx_bit_piece.elf_OUTPUT}"
    ELF_STATIC_VIEW_RNGLISTS_STARTX_LENGTH_FIXTURE_PATH="${rnglists_startx_length_bit_piece.elf_OUTPUT}"
    ELF_STATIC_VIEW_REF_SIG8_DEBUG_TYPES_FIXTURE_PATH="${ref_sig8_debug_types.elf_OUTPUT}"
    ELF_STATIC_VIEW_REF_SIG8_DEBUG_TYPES_INDIRECT_FIXTURE_PATH="${ref_sig8_debug_types_indirect.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_STRX1_FIXTURE_PATH="${dwarf5_strx_type_unit.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_STRX2_FIXTURE_PATH="${dwarf5_strx2_type_unit.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_STRX3_FIXTURE_PATH="${dwarf5_strx3_type_unit.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_STRX4_FIXTURE_PATH="${dwarf5_strx4_type_unit.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_ADDRX_FIXTURE_PATH="${dwarf5_strx_type_unit.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_ADDRX1_FIXTURE_PATH="${dwarf5_addrx1_type_unit.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_ADDRX2_FIXTURE_PATH="${dwarf5_addrx2_type_unit.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_ADDRX3_FIXTURE_PATH="${dwarf5_addrx3_type_unit.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_ADDRX4_FIXTURE_PATH="${dwarf5_addrx4_type_unit.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_STRX_TYPE_UNIT_FIXTURE_PATH="${dwarf5_strx_type_unit.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_STRX_TYPE_UNIT_ABBREV_OFFSET_FIXTURE_PATH="${dwarf5_strx_type_unit_abbrev_offset.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_LOCLISTS_START_END_FIXTURE_PATH="${dwarf5_loclists_start_end.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_LOCLISTS_STARTX_ENDX_FIXTURE_PATH="${dwarf5_loclists_startx_endx.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_LOCLISTS_STARTX_LENGTH_FIXTURE_PATH="${dwarf5_loclists_startx_length.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_LOCLISTS_START_LENGTH_FIXTURE_PATH="${dwarf5_loclists_start_length.elf_OUTPUT}"
    ELF_STATIC_VIEW_SPLIT_DWARF_FIXTURE_PATH="${split_dwarf_static.elf_OUTPUT}"
    ELF_STATIC_VIEW_DEBUG_SUP_FIXTURE_PATH="${debug_sup_from_obj.elf_OUTPUT}"
    ELF_STATIC_VIEW_DEBUG_SUP_SIDECAR_FIXTURE_PATH="${debug_sup_sidecar.elf_OUTPUT}"
    ELF_STATIC_VIEW_GCC_DWARF5_STRP_FIXTURE_PATH="${gcc_dwarf5_strp.elf_OUTPUT}"
    ELF_STATIC_VIEW_GCC_DWARF5_STRP_SUP_FIXTURE_PATH="${gcc_dwarf5_strp_sup.elf_OUTPUT}"
    ELF_STATIC_VIEW_GCC_DWARF5_REF_ADDR_FIXTURE_PATH="${gcc_dwarf5_ref_addr.elf_OUTPUT}"
    ELF_STATIC_VIEW_GCC_DWARF5_REF1_FIXTURE_PATH="${gcc_dwarf5_ref1.elf_OUTPUT}"
    ELF_STATIC_VIEW_GCC_DWARF5_REF2_FIXTURE_PATH="${gcc_dwarf5_ref2.elf_OUTPUT}"
    ELF_STATIC_VIEW_GCC_DWARF5_REF_UDATA_FIXTURE_PATH="${gcc_dwarf5_ref_udata.elf_OUTPUT}"
    ELF_STATIC_VIEW_GCC_DWARF5_REF_SUP4_FIXTURE_PATH="${gcc_dwarf5_ref_sup4.elf_OUTPUT}"
    ELF_STATIC_VIEW_GCC_DWARF64_REF8_FIXTURE_PATH="${gcc_dwarf64_ref8.elf_OUTPUT}"
    ELF_STATIC_VIEW_GCC_DWARF64_REF_SUP8_FIXTURE_PATH="${gcc_dwarf64_ref_sup8.elf_OUTPUT}"
    ELF_STATIC_VIEW_GCC_GNU_ALT_FIXTURE_PATH="${gcc_gnu_alt.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_DEFAULT_FIXTURE_PATH="${dwarf5_loclists_base_default.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_LOCLISTS_BASE_ADDRESSX_FIXTURE_PATH="${dwarf5_loclists_base_addressx.elf_OUTPUT}"
    ELF_STATIC_VIEW_UNSPECIFIED_TYPE_FIXTURE_PATH="${unspecified_type.elf_OUTPUT}"
    ELF_STATIC_VIEW_CLASS_ARRAY_NESTED_EXPAND_FIXTURE_PATH="${class_array_nested_expand.elf_OUTPUT}"
    ELF_STATIC_VIEW_ABSTRACT_ORIGIN_VALUES_FIXTURE_PATH="${abstract_origin_values.elf_OUTPUT}"
    ELF_STATIC_VIEW_DWARF5_LOCLISTS_FIXTURE_PATH="${dwarf5_loclists.elf_OUTPUT}"
    ELF_STATIC_VIEW_SPECIFICATION_FIXTURE_PATH="${specification_static.elf_OUTPUT}"
    ELF_STATIC_VIEW_THREAD_LOCAL_TLS_FIXTURE_PATH="${thread_local_tls.elf_OUTPUT}"
    ELF_STATIC_VIEW_MULTI_CU_FIXTURE_PATH="${multi_cu_static_vars.elf_OUTPUT}"
    ELF_STATIC_VIEW_C_EXPECTED_JSON="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/c/static_vars.expected.json"
    ELF_STATIC_VIEW_CPP_EXPECTED_JSON="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cpp/static_vars.expected.json"
)

add_test(NAME elf_static_view_tests COMMAND elf_static_view_tests)
add_test(NAME elf_static_view_perf_tests COMMAND elf_static_view_perf_tests)
