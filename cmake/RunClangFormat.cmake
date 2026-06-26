execute_process(
        COMMAND
        ${GIT_EXECUTABLE}
        ls-files
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_FILES
)

string(REPLACE "\n" ";" GIT_FILES "${GIT_FILES}")

foreach(FILE ${GIT_FILES})
    if(FILE MATCHES "\\.(h|hpp|c|cc|cpp)$")
        message(STATUS "Formatting ${FILE}")
        execute_process(
                COMMAND
                ${CLANG_FORMAT_EXE}
                -i
                --style=file
                "${PROJECT_SOURCE_DIR}/${FILE}"
        )
    endif()
endforeach()