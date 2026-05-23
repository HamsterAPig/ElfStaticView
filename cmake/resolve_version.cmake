if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR 未提供")
endif()

if(NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "OUTPUT_FILE 未提供")
endif()

if(NOT DEFINED TEMP_FILE)
  set(TEMP_FILE "${OUTPUT_FILE}.tmp")
endif()

set(version_string "0.0.0")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" describe --tags --exact-match HEAD
    OUTPUT_VARIABLE current_tag
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE current_tag_result
    ERROR_QUIET
  )
  if(current_tag_result EQUAL 0 AND NOT current_tag STREQUAL "")
    set(version_string "${current_tag}")
  else()
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" describe --tags --abbrev=0 HEAD
      OUTPUT_VARIABLE last_tag
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE last_tag_result
      ERROR_QUIET
    )
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --short=8 HEAD
      OUTPUT_VARIABLE short_sha
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE short_sha_result
      ERROR_QUIET
    )
    if(short_sha_result EQUAL 0 AND NOT short_sha STREQUAL "")
      if(last_tag_result EQUAL 0 AND NOT last_tag STREQUAL "")
        set(version_string "${last_tag}+${short_sha}")
      else()
        set(version_string "0.0.0+${short_sha}")
      endif()
    endif()
  endif()
endif()

file(WRITE "${TEMP_FILE}" "#pragma once\n\n#define ELF_STATIC_VIEW_VERSION \"${version_string}\"\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TEMP_FILE}" "${OUTPUT_FILE}"
)
