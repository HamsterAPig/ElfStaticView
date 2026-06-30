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

function(elf_static_view_enable_warnings target_name)
  target_compile_options(
    ${target_name}
    PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
  )

  if(WIN32)
    target_compile_definitions(
      ${target_name}
      PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
  endif()
endfunction()
