cmake_minimum_required(VERSION 3.24)

project(ElfStaticViewSubdirTest LANGUAGES CXX)

set(ELF_STATIC_VIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ELF_STATIC_VIEW_BUILD_UI OFF CACHE BOOL "" FORCE)

add_subdirectory(../.. elfstaticview-build)

add_executable(elf_static_view_package_subdir_smoke main.cc)
target_link_libraries(elf_static_view_package_subdir_smoke PRIVATE elf_static_view::core)
target_compile_features(elf_static_view_package_subdir_smoke PRIVATE cxx_std_20)
