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

target_include_directories(psio PUBLIC include)
target_compile_features(psio PUBLIC cxx_std_23)

# xxhash — vendored single-header at external/xxhash/xxhash.h.  pjson
# uses it for the 8-bit key prefilter; psch uses it for the schema-
# digest hash.  XXH_INLINE_ALL is defined at include sites so no
# separate compilation unit is needed.
target_include_directories(psio PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/external/xxhash)

# simdjson — optional. Powers psio::pjson::from_json (JSON text →
# pjson-binary). When available, callers get the SIMD stage-1 scan;
# the fused encoder walks simdjson's DOM tape twice (size + write) and
# emits pjson bytes directly with no intermediate value tree.
find_package(simdjson QUIET)
if(simdjson_FOUND)
   target_link_libraries(psio PUBLIC simdjson::simdjson)
   target_compile_definitions(psio PUBLIC PSIO_HAVE_SIMDJSON=1)
   message(STATUS "psio: simdjson found, json_to_pjson enabled")
else()
   target_compile_definitions(psio PUBLIC PSIO_HAVE_SIMDJSON=0)
   message(STATUS "psio: simdjson not found; json_to_pjson disabled")
endif()

# Boost::headers is needed by the reflection macro machinery (preprocessor
# seq/tuple helpers) once Phase 1 lands. Already on CI for psio/psio2.
find_package(Boost REQUIRED)
target_link_libraries(psio PUBLIC Boost::headers)

# Exception-optional build. The generic API (validate returning
# codec_status) works under -fno-exceptions; only the opt-in .or_throw()
# helper disappears.
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
   add_subdirectory(tests)
endif()

if(PSIO_ENABLE_BENCHMARKS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(benchmarks)
endif()

if(PSIO_ENABLE_TOOLS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(tools)
endif()
