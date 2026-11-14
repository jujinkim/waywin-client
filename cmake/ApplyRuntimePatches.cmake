# ExternalProject owns this checkout. Re-running with the same patches is a no-op;
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
foreach(name probe product)
  execute_process(COMMAND "${GIT}" apply --check "${ROOT}/patches/freerdp-3.33.1-${name}.patch"
    WORKING_DIRECTORY "${SOURCE}" COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${GIT}" apply --whitespace=nowarn "${ROOT}/patches/freerdp-3.33.1-${name}.patch"
    WORKING_DIRECTORY "${SOURCE}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
file(WRITE "${marker}" "${expected}")
