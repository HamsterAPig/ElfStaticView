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

set(ELF_STATIC_VIEW_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")
set(ELF_STATIC_VIEW_VERSION_HEADER "${ELF_STATIC_VIEW_GENERATED_INCLUDE_DIR}/elf_static_view/version.hpp")
file(MAKE_DIRECTORY "${ELF_STATIC_VIEW_GENERATED_INCLUDE_DIR}/elf_static_view")

add_custom_target(
  elf_static_view_version_header ALL
  COMMAND
    "${CMAKE_COMMAND}"
    -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
    -DOUTPUT_FILE=${ELF_STATIC_VIEW_VERSION_HEADER}
    -DTEMP_FILE=${ELF_STATIC_VIEW_VERSION_HEADER}.tmp
    -P "${PROJECT_SOURCE_DIR}/cmake/resolve_version.cmake"
  BYPRODUCTS
    "${ELF_STATIC_VIEW_VERSION_HEADER}"
    "${ELF_STATIC_VIEW_VERSION_HEADER}.tmp"
  VERBATIM
  COMMENT "Resolving git-backed ElfStaticView version"
)

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

add_subdirectory(3rdparty/libdwarf-code EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/yaml-cpp EXCLUDE_FROM_ALL)

add_library(elf_static_view_warnings INTERFACE)
target_compile_options(
  elf_static_view_warnings
  INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

if(WIN32)
  target_compile_definitions(
    elf_static_view_warnings
    INTERFACE
      WIN32_LEAN_AND_MEAN
      NOMINMAX
  )
endif()

add_library(elf_static_view_core
  src/analysis/address_bias.cpp
  src/analysis/export_document.cpp
  src/analysis/expander.cpp
  src/analysis/model_json.cpp
  src/analysis/model_utils.cpp
  src/analysis/project.cpp
  src/analysis/project_summary.cpp
  src/analysis/static_address_query.cpp
  src/elf/elf_symbol_table.cpp
  src/elf/dwarf_reader.cpp
  src/elf/raw_dwarf_reader.cpp
  src/elf/dwarf_wrappers.cpp
  src/elf/ti_coff_object.cpp
  src/logging/logger.cpp
)

target_include_directories(
  elf_static_view_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    src
    3rdparty/yaml-cpp/include
)

target_link_libraries(
  elf_static_view_core
  PUBLIC
    libdwarf::dwarf-static
  PRIVATE
    yaml-cpp::yaml-cpp
)

target_compile_options(
  elf_static_view_core
  PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

if(WIN32)
  target_compile_definitions(
    elf_static_view_core
    PRIVATE
      WIN32_LEAN_AND_MEAN
      NOMINMAX
  )
endif()

target_compile_features(elf_static_view_core PUBLIC cxx_std_20)
add_library(elf_static_view::core ALIAS elf_static_view_core)

if(ELF_STATIC_VIEW_BUILD_UI)
  FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
  )
  FetchContent_MakeAvailable(glfw)

  find_package(OpenGL REQUIRED)

  add_library(elf_static_view_ui_support STATIC
    src/ui/app_state.cpp
    src/ui/filter_matcher.cpp
    src/ui/ui_task_runner.cpp
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
    add_dependencies(${target_name} elf_static_view_version_header)

    target_include_directories(
      ${target_name}
      PRIVATE
        src
        "${ELF_STATIC_VIEW_GENERATED_INCLUDE_DIR}"
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
endif()

if(ELF_STATIC_VIEW_BUILD_TESTS AND ELF_STATIC_VIEW_BUILD_UI)
  add_subdirectory(tests)
endif()

set(ELF_STATIC_VIEW_INSTALL_CONFIGDIR "${CMAKE_INSTALL_LIBDIR}/cmake/ElfStaticView")

configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/ElfStaticViewConfig.cmake.in"
  "${CMAKE_BINARY_DIR}/ElfStaticViewConfig.cmake"
  INSTALL_DESTINATION "${ELF_STATIC_VIEW_INSTALL_CONFIGDIR}"
)

write_basic_package_version_file(
  "${CMAKE_BINARY_DIR}/ElfStaticViewConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)

install(
  TARGETS elf_static_view_core
  EXPORT ElfStaticViewTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  TARGETS yaml-cpp
  EXPORT ElfStaticViewTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  FILES
    "${CMAKE_BINARY_DIR}/3rdparty/libdwarf-code/src/lib/libdwarf/${CMAKE_STATIC_LIBRARY_PREFIX}dwarf${CMAKE_STATIC_LIBRARY_SUFFIX}"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

install(
  DIRECTORY include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  FILES
    "${ELF_STATIC_VIEW_VERSION_HEADER}"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/elf_static_view"
)

install(
  EXPORT ElfStaticViewTargets
  FILE ElfStaticViewTargets.cmake
  NAMESPACE elf_static_view::
  DESTINATION "${ELF_STATIC_VIEW_INSTALL_CONFIGDIR}"
)

install(
  FILES
    "${CMAKE_BINARY_DIR}/ElfStaticViewConfig.cmake"
    "${CMAKE_BINARY_DIR}/ElfStaticViewConfigVersion.cmake"
  DESTINATION "${ELF_STATIC_VIEW_INSTALL_CONFIGDIR}"
)
