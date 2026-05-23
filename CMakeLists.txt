cmake_minimum_required(VERSION 3.24)

project(
  ElfStaticView
  VERSION 0.1.0
  DESCRIPTION "Offline ELF/DWARF static variable explorer"
  LANGUAGES C CXX
)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ELF_STATIC_VIEW_BUILD_TESTS "Build tests and fixture generation." ON)
add_subdirectory(3rdparty/libdwarf-code EXCLUDE_FROM_ALL)

add_library(elf_static_view_warnings INTERFACE)
target_compile_options(
  elf_static_view_warnings
  INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

add_library(elf_static_view_core
  src/analysis/expander.cpp
  src/analysis/model_json.cpp
  src/analysis/model_utils.cpp
  src/analysis/project.cpp
  src/analysis/project_summary.cpp
  src/elf/elf_symbol_table.cpp
  src/elf/dwarf_reader.cpp
  src/elf/dwarf_wrappers.cpp
  src/logging/logger.cpp
)

target_include_directories(
  elf_static_view_core
  PUBLIC
    include
  PRIVATE
    src
)

target_link_libraries(
  elf_static_view_core
  PUBLIC
    libdwarf::dwarf-static
  PRIVATE
    elf_static_view_warnings
)

target_compile_features(elf_static_view_core PUBLIC cxx_std_20)

add_executable(elf-static-view src/main.cpp)
target_link_libraries(
  elf-static-view
  PRIVATE
    elf_static_view_core
    elf_static_view_warnings
)

if(ELF_STATIC_VIEW_BUILD_TESTS)
  add_subdirectory(tests)
endif()
