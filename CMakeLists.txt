cmake_minimum_required(VERSION 3.24)

project(
  ElfStaticView
  VERSION 0.1.0
  DESCRIPTION "Offline ELF/DWARF static variable explorer"
  LANGUAGES C CXX
)

include(CTest)
include(FetchContent)

find_package(Git QUIET)

set(ELF_STATIC_VIEW_VERSION_STRING "${PROJECT_VERSION}")
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" describe --tags --exact-match HEAD
    OUTPUT_VARIABLE ELF_STATIC_VIEW_GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE ELF_STATIC_VIEW_GIT_TAG_RESULT
    ERROR_QUIET
  )
  if(ELF_STATIC_VIEW_GIT_TAG_RESULT EQUAL 0 AND NOT ELF_STATIC_VIEW_GIT_TAG STREQUAL "")
    set(ELF_STATIC_VIEW_VERSION_STRING "${ELF_STATIC_VIEW_GIT_TAG}")
  else()
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" describe --tags --abbrev=0 HEAD
      OUTPUT_VARIABLE ELF_STATIC_VIEW_LAST_GIT_TAG
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE ELF_STATIC_VIEW_LAST_GIT_TAG_RESULT
      ERROR_QUIET
    )
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --short=8 HEAD
      OUTPUT_VARIABLE ELF_STATIC_VIEW_GIT_SHA
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE ELF_STATIC_VIEW_GIT_SHA_RESULT
      ERROR_QUIET
    )
    if(ELF_STATIC_VIEW_GIT_SHA_RESULT EQUAL 0 AND NOT ELF_STATIC_VIEW_GIT_SHA STREQUAL "")
      if(ELF_STATIC_VIEW_LAST_GIT_TAG_RESULT EQUAL 0 AND NOT ELF_STATIC_VIEW_LAST_GIT_TAG STREQUAL "")
        set(ELF_STATIC_VIEW_VERSION_STRING "${ELF_STATIC_VIEW_LAST_GIT_TAG}+${ELF_STATIC_VIEW_GIT_SHA}")
      else()
        set(ELF_STATIC_VIEW_VERSION_STRING "0.0.0+${ELF_STATIC_VIEW_GIT_SHA}")
      endif()
    endif()
  endif()
endif()

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
  src/analysis/address_bias.cpp
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

set(ELF_STATIC_VIEW_APP_SOURCES
  src/entry.cpp
  src/ui/application.cpp
  src/ui/file_dialogs.cpp
  src/ui/main_window.cpp
  src/ui/version_check.cpp
)

function(configure_elf_static_view_target target_name)
  target_include_directories(
    ${target_name}
    PRIVATE
      src
  )

  target_link_libraries(
    ${target_name}
    PRIVATE
      elf_static_view_core
      elf_static_view_ui_support
      elf_static_view_imgui
      yaml-cpp::yaml-cpp
      elf_static_view_warnings
  )

  target_compile_definitions(
    ${target_name}
    PRIVATE
      ELF_STATIC_VIEW_VERSION="${ELF_STATIC_VIEW_VERSION_STRING}"
  )

  if(WIN32)
    target_link_libraries(${target_name} PRIVATE comdlg32 winhttp)
  endif()
endfunction()

if(WIN32)
  add_executable(
    elf-static-view
    WIN32
    src/win32_main.cpp
    ${ELF_STATIC_VIEW_APP_SOURCES}
  )
  add_executable(
    elf-static-view-cli
    src/main.cpp
    ${ELF_STATIC_VIEW_APP_SOURCES}
  )
  configure_elf_static_view_target(elf-static-view)
  configure_elf_static_view_target(elf-static-view-cli)
else()
  add_executable(
    elf-static-view
    src/main.cpp
    ${ELF_STATIC_VIEW_APP_SOURCES}
  )
  configure_elf_static_view_target(elf-static-view)
endif()

if(ELF_STATIC_VIEW_BUILD_TESTS)
  add_subdirectory(tests)
endif()
