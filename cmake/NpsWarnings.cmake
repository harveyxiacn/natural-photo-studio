function(nps_enable_warnings target)
  if(MSVC)
    target_compile_options(
      "${target}"
      PRIVATE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
    )
    if(NPS_WARNINGS_AS_ERRORS)
      target_compile_options("${target}" PRIVATE /WX)
    endif()
  else()
    target_compile_options(
      "${target}"
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
    if(NPS_WARNINGS_AS_ERRORS)
      target_compile_options("${target}" PRIVATE -Werror)
    endif()
  endif()
endfunction()
