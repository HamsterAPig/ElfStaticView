find_program(CLANG_FORMAT_EXE clang-format)
find_program(GIT_EXECUTABLE git)

if(NOT CLANG_FORMAT_EXE)
    message(STATUS "clang-format not found")
    return()
endif()

if(NOT GIT_EXECUTABLE)
    message(STATUS "git not found")
    return()
endif()

add_custom_target(format
        COMMAND
        ${CMAKE_COMMAND}
        -DPROJECT_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DCLANG_FORMAT_EXE=${CLANG_FORMAT_EXE}
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -P ${CMAKE_CURRENT_LIST_DIR}/RunClangFormat.cmake
        COMMENT "Formatting source files"
)