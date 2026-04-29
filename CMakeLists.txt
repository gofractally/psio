# psio — v2-architecture successor to psio.
#
# Implements the design captured in .issues/psio-v2-design.md:
#   - tag_invoke CPOs as the customization mechanism
#   - format_tag_base<Derived> CRTP for format-scoped sugar
#   - Shape concepts + reflection annotations (psio::annotate<X>)
#   - Runtime reflection (psio::reflect<T>) with perfect-hash name lookup
#   - Dynamic / schema-driven access (psio::schema, transcode, dynamic_view)
#   - Rich wrapper types coexisting with annotations
#   - [[nodiscard]] codec_status error model, exception-optional
#
# psio v1 (../psio/) remains as the reference implementation during
# development. psio never #includes psio headers — they are reference-only.

add_library(psio
   # Compiled units land here as phases 14+ introduce them.
   # Phase 0 scaffold: single placeholder TU so CMake accepts the
   # library target; psio is header-only until phase 14.
   src/stub.cpp
)

target_include_directories(psio PUBLIC include)
target_compile_features(psio PUBLIC cxx_std_23)

# xxhash — vendored single-header under external/psitri; pjson uses it
# for the 8-bit key prefilter. XXH_INLINE_ALL is defined at include
# sites so no separate compilation unit is needed.
target_include_directories(psio PUBLIC
   ${CMAKE_SOURCE_DIR}/external/psitri/libraries/hash/include)

# ucc — psitri's small-utility library. pjson_view leverages
# ucc::find_byte (SWAR 8-byte-at-a-time scalar scan, falls back to
# NEON/AVX2 on the supported targets) for the hash-prefilter walk
# during object key lookup. Same pattern psitri's leaf nodes use.
target_include_directories(psio PUBLIC
   ${CMAKE_SOURCE_DIR}/external/psitri/libraries/ucc/include)

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
