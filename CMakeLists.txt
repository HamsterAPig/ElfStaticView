cmake_minimum_required(VERSION 3.24)

project(
  ElfStaticView
  VERSION 0.1.0
  DESCRIPTION "Offline ELF/DWARF static variable explorer"
  LANGUAGES C CXX
)

include(CTest)
include(FetchContent)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ELF_STATIC_VIEW_BUILD_TESTS "Build tests and fixture generation." ON)
option(ELF_STATIC_VIEW_BUILD_UI "Build GLFW/OpenGL3 Dear ImGui desktop UI." ON)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_DWARFGEN ON CACHE BOOL "Build dwarfgen for DWARF supplementary fixtures" FORCE)

include(cmake/Format.cmake)
include(cmake/VersionHeader.cmake)

add_subdirectory(3rdparty/libdwarf-code EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/yaml-cpp EXCLUDE_FROM_ALL)

include(cmake/CompilerOptions.cmake)
include(cmake/ElfStaticViewTargets.cmake)

if(ELF_STATIC_VIEW_BUILD_TESTS AND ELF_STATIC_VIEW_BUILD_UI)
  add_subdirectory(tests)
endif()

include(cmake/InstallRules.cmake)
