include(ExternalProject)
find_package(Git REQUIRED)
set(WAYWIN_RUNTIME_REPOSITORY "https://github.com/FreeRDP/FreeRDP.git" CACHE STRING "Mirror of the pinned FreeRDP repository")
set(WAYWIN_RUNTIME_ARCHIVE "" CACHE FILEPATH "Bundled, pinned FreeRDP source archive; no Git download when set")
if(WAYWIN_RUNTIME_ARCHIVE)
  if(NOT EXISTS "${WAYWIN_RUNTIME_ARCHIVE}" OR IS_DIRECTORY "${WAYWIN_RUNTIME_ARCHIVE}")
    message(FATAL_ERROR "Bundled FreeRDP source archive is missing. Obtain a complete WayWin bundle.")
  endif()
  file(READ "${CMAKE_SOURCE_DIR}/dependencies.lock.json" dependency_lock)
  string(JSON archive_hash GET "${dependency_lock}" freerdp source_archive_sha256)
  file(SHA256 "${WAYWIN_RUNTIME_ARCHIVE}" actual_hash)
  if(NOT actual_hash STREQUAL archive_hash)
    message(FATAL_ERROR "Bundled FreeRDP source archive failed SHA-256 verification. Obtain a complete WayWin bundle.")
  endif()
  set(runtime_download URL "${WAYWIN_RUNTIME_ARCHIVE}" URL_HASH "SHA256=${archive_hash}")
  # CMake 3.22 predates this policy; newer versions should timestamp extracted files now.
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
else()
  set(runtime_download
    GIT_REPOSITORY "${WAYWIN_RUNTIME_REPOSITORY}"
    GIT_TAG 65bcca1913a28cbdd7eb913599f692bd2b707991
    GIT_CONFIG core.autocrlf=false)
endif()
set(WAYWIN_RUNTIME_STAGE "${CMAKE_BINARY_DIR}/runtime")
ExternalProject_Add(waywin-runtime
  ${runtime_download}
  UPDATE_COMMAND ""
  PATCH_COMMAND "${CMAKE_COMMAND}" -DSOURCE=<SOURCE_DIR> -DROOT=${CMAKE_SOURCE_DIR}
    -DGIT=${GIT_EXECUTABLE} -P "${CMAKE_SOURCE_DIR}/cmake/ApplyRuntimePatches.cmake"
  CMAKE_GENERATOR Ninja
  CMAKE_ARGS -C${CMAKE_SOURCE_DIR}/cmake/FreeRDPProduct.cmake
    -DCMAKE_INSTALL_PREFIX=${WAYWIN_RUNTIME_STAGE}
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DWITH_CLIENT_SDL_APP_BUNDLE=OFF
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel 4
  INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>
  LOG_DOWNLOAD ON LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON
  LOG_OUTPUT_ON_FAILURE ON)
ExternalProject_Add_Step(waywin-runtime refresh-manifest
  COMMAND "${CMAKE_COMMAND}" -DSTAGE=${WAYWIN_RUNTIME_STAGE} -DROOT=${CMAKE_SOURCE_DIR}
    -DBUILD=<BINARY_DIR> -P "${CMAKE_SOURCE_DIR}/cmake/RuntimeManifest.cmake"
  DEPENDEES install ALWAYS ON)
install(DIRECTORY "${WAYWIN_RUNTIME_STAGE}/" DESTINATION lib/waywin/runtime
  USE_SOURCE_PERMISSIONS PATTERN "include" EXCLUDE PATTERN "share" EXCLUDE)

ExternalProject_Add_Step(waywin-runtime validate-patches
  COMMAND "${CMAKE_COMMAND}" -DSOURCE=<SOURCE_DIR> -DROOT=${CMAKE_SOURCE_DIR}
    -DGIT=${GIT_EXECUTABLE} -P "${CMAKE_SOURCE_DIR}/cmake/ApplyRuntimePatches.cmake"
  DEPENDEES patch DEPENDERS configure
  DEPENDS "${CMAKE_SOURCE_DIR}/patches/freerdp-3.33.1-probe.patch"
    "${CMAKE_SOURCE_DIR}/patches/freerdp-3.33.1-product.patch")
