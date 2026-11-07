execute_process(COMMAND git describe --always --abbrev=7
  WORKING_DIRECTORY ${SRC} OUTPUT_VARIABLE v
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git status --porcelain -- .
  WORKING_DIRECTORY ${SRC} OUTPUT_VARIABLE dirty
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT v)
  set(v "unknown")
endif()
if(dirty)
  set(v "${v}-dirty")
endif()
string(SUBSTRING "${v}" 0 16 v)
set(content "#define FW_VERSION \"${v}\"\n")
set(old "")
if(EXISTS ${OUT})
  file(READ ${OUT} old)
endif()
if(NOT "${content}" STREQUAL "${old}")
  file(WRITE ${OUT} "${content}")
endif()
