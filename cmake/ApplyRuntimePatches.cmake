# ExternalProject owns these sources. Re-running with the same patches is a no-op;
# changing patches requires a fresh build directory rather than overwriting edits.
file(SHA256 "${ROOT}/patches/freerdp-3.33.1-probe.patch" probe)
file(SHA256 "${ROOT}/patches/freerdp-3.33.1-product.patch" product)
set(expected "${probe}\n${product}\n")
set(marker "${SOURCE}/.waywin-patches")
if(EXISTS "${marker}")
  file(READ "${marker}" previous)
  if(NOT previous STREQUAL expected)
    message(FATAL_ERROR "Runtime patches changed. Configure a new build directory; existing runtime sources are preserved.")
  endif()
  return()
endif()
get_filename_component(source_parent "${SOURCE}" DIRECTORY)
set(git_apply "${CMAKE_COMMAND}" -E env
  --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_INDEX_FILE
  "GIT_CEILING_DIRECTORIES=${source_parent}"
  "${GIT}" -c core.autocrlf=false apply --no-index)
foreach(name probe product)
  # Archives have no .git directory, and may be extracted inside another repository.
  # Stop parent-repository discovery so Git-format patches cannot be silently skipped.
  execute_process(COMMAND ${git_apply} --check "${ROOT}/patches/freerdp-3.33.1-${name}.patch"
    WORKING_DIRECTORY "${SOURCE}" COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND ${git_apply} --whitespace=nowarn "${ROOT}/patches/freerdp-3.33.1-${name}.patch"
    WORKING_DIRECTORY "${SOURCE}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
file(WRITE "${marker}" "${expected}")
