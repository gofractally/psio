# psio — multi-format C++23 serialization library.
#
# Tag-dispatched CPOs (encode / decode / size_of / validate /
# validate_or_throw) over a uniform reflection layer (psio::reflect<T>),
# Shape concepts + annotation channel (psio::annotate<X>), rich
# wrappers, schema IR + transcode, [[nodiscard]] codec_status error
# model with optional exceptions.
#
# Builds standalone (`cmake -S .` from a fresh clone) and as a
# subdirectory / FetchContent target inside a parent project.
# Standalone build defaults tests / benchmarks / tools OFF; turn on
# with -DPSIO_ENABLE_TESTS=ON etc.  Dependency surface is documented
# in docs/dependency-pattern.md.

cmake_minimum_required(VERSION 3.20)

# project() is only needed when psio is the top-level project (a
# fresh clone built standalone).  When consumed as a subdirectory or
# via FetchContent, the parent project's project() call has already
# enabled languages and we don't want to override its build-type /
# output-directory settings.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
   project(psio VERSION 0.1.0 LANGUAGES C CXX)
endif()

add_library(psio
   # Compiled units land here as phases 14+ introduce them.
   # Phase 0 scaffold: single placeholder TU so CMake accepts the
   # library target; psio is header-only until phase 14.
   src/stub.cpp
)

add_library(psio::psio ALIAS psio)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
target_include_directories(psio PUBLIC
   $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
   $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(psio PUBLIC cxx_std_23)

# xxhash — vendored single-header at external/xxhash/xxhash.h.  pjson
# uses it for the 8-bit key prefilter; psch uses it for the schema-
# digest hash.  XXH_INLINE_ALL is defined at include sites so no
# separate compilation unit is needed.
target_include_directories(psio PUBLIC
   $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/xxhash>
   $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/psio/external>)

# simdjson — the JSON syntax layer of the typed and schema-driven JSON /
# I-JSON readers (psio/json_simdjson.hpp) and of psio::pjson::from_json.
#
# PSIO fetches it itself, pinned to an exact release and archive hash; a
# system install is never looked up, so every build parses with the same
# code. Overrides, in order of precedence:
#   * a `simdjson::simdjson` target the parent project already defines;
#   * -DFETCHCONTENT_SOURCE_DIR_SIMDJSON=<dir> (a local source tree, no
#     download; standard FetchContent behaviour);
#   * a parent FetchContent_Declare(simdjson ...) made before this one.
# -DPSIO_ENABLE_SIMDJSON=OFF builds without it (PSIO_HAVE_SIMDJSON=0): the
# readers then run on the hand-written lexer alone. That is the default for
# WASI, where guest builds do not link it.
set(PSIO_SIMDJSON_VERSION 5.2.0)
set(PSIO_SIMDJSON_SHA256 499176afd4b32fbe6f65551ef573b880407280072e1c19e22df0b45da58cdf64)
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
   set(PSIO_ENABLE_SIMDJSON_DEFAULT OFF)
else()
   set(PSIO_ENABLE_SIMDJSON_DEFAULT ON)
endif()
option(PSIO_ENABLE_SIMDJSON
   "Use simdjson (fetched, pinned) as the JSON syntax layer"
   ${PSIO_ENABLE_SIMDJSON_DEFAULT})
if(PSIO_ENABLE_SIMDJSON)
   if(NOT TARGET simdjson::simdjson)
      include(FetchContent)
      if(POLICY CMP0135)
         cmake_policy(SET CMP0135 NEW)
      endif()
      FetchContent_Declare(simdjson
         URL https://github.com/simdjson/simdjson/archive/refs/tags/v${PSIO_SIMDJSON_VERSION}.tar.gz
         URL_HASH SHA256=${PSIO_SIMDJSON_SHA256})
      # psio's installed targets link simdjson::simdjson, so the fetched
      # simdjson is installed with psio (its own export set and package
      # config, which psioConfig.cmake finds with find_dependency).
      if(NOT DEFINED SIMDJSON_INSTALL)
         set(SIMDJSON_INSTALL ON CACHE BOOL "Install the simdjson psio was built with")
      endif()
      FetchContent_MakeAvailable(simdjson)
   endif()
   target_link_libraries(psio PUBLIC simdjson::simdjson)
   target_compile_definitions(psio PUBLIC PSIO_HAVE_SIMDJSON=1)
   message(STATUS "psio: simdjson ${PSIO_SIMDJSON_VERSION} (pinned) is the JSON syntax layer")
else()
   target_compile_definitions(psio PUBLIC PSIO_HAVE_SIMDJSON=0)
   message(STATUS "psio: simdjson disabled; hand-written JSON lexer only, json_to_pjson disabled")
endif()

# Boost::headers is needed by the reflection macro machinery (preprocessor
# seq/tuple helpers) once Phase 1 lands. Already on CI for psio/psio2.
find_package(Boost REQUIRED)
target_link_libraries(psio PUBLIC Boost::headers)

# Controls exception helper declarations. A complete -fno-exceptions profile
# is not yet supported: some headers still contain unguarded throws and
# codec_exception references. See docs/compatibility.md at the repository root.
option(PSIO_EXCEPTIONS_ENABLED "Enable exception-throwing helpers" ON)
if(PSIO_EXCEPTIONS_ENABLED)
   target_compile_definitions(psio PUBLIC PSIO_EXCEPTIONS_ENABLED=1)
else()
   target_compile_definitions(psio PUBLIC PSIO_EXCEPTIONS_ENABLED=0)
endif()

# Phase 4 will make this required for all v3 library code.
target_compile_options(psio INTERFACE -Werror=unused-result)

# Build-gated components ------------------------------------------------------

option(PSIO_ENABLE_TESTS "Build psio tests" OFF)
option(PSIO_ENABLE_BENCHMARKS "Build psio benchmarks" OFF)
option(PSIO_ENABLE_TOOLS "Build psio tools (fixture emitters, psio_tool)" OFF)

if(PSIO_ENABLE_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   enable_testing()
   add_subdirectory(tests)
endif()

if(PSIO_ENABLE_BENCHMARKS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(benchmarks)
endif()

if(PSIO_ENABLE_TOOLS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(tools)
endif()

# Public-library conformance fixtures and optional historical draft driver.
option(PSIO_ENABLE_REFERENCE_DRIVER "Build the historical pjson spec reference driver" OFF)
if(PSIO_ENABLE_REFERENCE_DRIVER OR PSIO_ENABLE_TESTS)
   add_subdirectory(conformance)
endif()

install(TARGETS psio EXPORT psioTargets
   ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
   LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
   RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/psio DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES external/xxhash/xxhash.h
   DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/psio/external)
set(PSIO_CONFIG_SIMDJSON ${PSIO_ENABLE_SIMDJSON})
configure_package_config_file(cmake/psioConfig.cmake.in
   ${CMAKE_CURRENT_BINARY_DIR}/psioConfig.cmake
   INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/psio)
write_basic_package_version_file(
   ${CMAKE_CURRENT_BINARY_DIR}/psioConfigVersion.cmake
   VERSION 0.1.0 COMPATIBILITY SameMajorVersion)
install(EXPORT psioTargets NAMESPACE psio::
   DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/psio)
install(FILES
   ${CMAKE_CURRENT_BINARY_DIR}/psioConfig.cmake
   ${CMAKE_CURRENT_BINARY_DIR}/psioConfigVersion.cmake
   DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/psio)
