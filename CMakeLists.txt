cmake_minimum_required(VERSION 3.24)

project(
  ElfStaticView
  VERSION 0.1.0
  DESCRIPTION "Offline ELF/DWARF static variable explorer"
  LANGUAGES C CXX
)

include(CTest)
include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ELF_STATIC_VIEW_BUILD_TESTS "Build tests and fixture generation." ON)
option(ELF_STATIC_VIEW_BUILD_UI "Build GLFW/OpenGL3 Dear ImGui desktop UI." ON)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
)
FetchContent_MakeAvailable(glfw)

find_package(OpenGL REQUIRED)

add_subdirectory(3rdparty/libdwarf-code EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/yaml-cpp EXCLUDE_FROM_ALL)

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
    yaml-cpp::yaml-cpp
    elf_static_view_warnings
)

target_compile_features(elf_static_view_core PUBLIC cxx_std_20)

add_library(elf_static_view_ui_support STATIC
  src/ui/app_state.cpp
  src/ui/filter_matcher.cpp
)

target_include_directories(
  elf_static_view_ui_support
  PUBLIC
    src
  PRIVATE
    include
)

target_link_libraries(
  elf_static_view_ui_support
  PUBLIC
    elf_static_view_core
  PRIVATE
    elf_static_view_warnings
)

target_compile_features(elf_static_view_ui_support PUBLIC cxx_std_20)

add_library(elf_static_view_imgui STATIC
  3rdparty/imgui/imgui.cpp
  3rdparty/imgui/imgui_draw.cpp
  3rdparty/imgui/imgui_tables.cpp
  3rdparty/imgui/imgui_widgets.cpp
  3rdparty/imgui/misc/cpp/imgui_stdlib.cpp
  3rdparty/imgui/backends/imgui_impl_glfw.cpp
  3rdparty/imgui/backends/imgui_impl_opengl3.cpp
)

target_include_directories(
  elf_static_view_imgui
  PUBLIC
    3rdparty/imgui
    3rdparty/imgui/backends
    3rdparty/imgui/misc/cpp
)

target_link_libraries(
  elf_static_view_imgui
  PUBLIC
    glfw
    OpenGL::GL
  PRIVATE
    elf_static_view_warnings
)

target_compile_features(elf_static_view_imgui PUBLIC cxx_std_20)

add_executable(
  elf-static-view
  src/main.cpp
  src/ui/application.cpp
  src/ui/file_dialogs.cpp
  src/ui/main_window.cpp
)

target_include_directories(
  elf-static-view
  PRIVATE
    src
)

target_link_libraries(
  elf-static-view
  PRIVATE
    elf_static_view_core
    elf_static_view_ui_support
    elf_static_view_imgui
    elf_static_view_warnings
)

if(WIN32)
  target_link_libraries(elf-static-view PRIVATE comdlg32)
endif()

if(ELF_STATIC_VIEW_BUILD_TESTS)
  add_subdirectory(tests)
endif()
