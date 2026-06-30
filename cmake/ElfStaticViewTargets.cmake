add_library(elf_static_view_core
  src/analysis/address_bias.cc
  src/analysis/export_document.cc
  src/analysis/expander.cc
  src/analysis/model_json.cc
  src/analysis/model_utils.cc
  src/analysis/project.cc
  src/analysis/project_summary.cc
  src/analysis/static_address_query.cc
  src/elf/elf_symbol_table.cc
  src/elf/dwarf_reader.cc
  src/elf/raw_dwarf_reader.cc
  src/elf/dwarf_wrappers.cc
  src/elf/ti_coff_object.cc
  src/logging/logger.cc
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

elf_static_view_enable_warnings(elf_static_view_core)
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
    src/ui/app_state.cc
    src/ui/filter_matcher.cc
    src/ui/ui_task_runner.cc
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
    src/entry.cc
    src/ui/application.cc
    src/ui/file_dialogs.cc
    src/ui/main_window.cc
    src/ui/version_check.cc
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
      src/win32_main.cc
      ${ELF_STATIC_VIEW_APP_SOURCES}
    )
    add_executable(
      elf-static-view-cli
      src/main.cc
      ${ELF_STATIC_VIEW_APP_SOURCES}
    )
    configure_elf_static_view_target(elf-static-view)
    configure_elf_static_view_target(elf-static-view-cli)
  else()
    add_executable(
      elf-static-view
      src/main.cc
      ${ELF_STATIC_VIEW_APP_SOURCES}
    )
    configure_elf_static_view_target(elf-static-view)
  endif()
endif()

