# psio benchmarks — cross-library perf + wire-size harness.
#
# Shape library (shapes.hpp) defines reflected struct types at each
# size tier (Point, NameRecord, FlatRecord, Record, Validator, Order,
# ValidatorList, plus *Bounded variants).  harness.hpp is the timing
# + report utility layer.
#
# Competitor adapters (libcapnp, libflatbuffers, msgpack-c, protobuf,
# sszpp) are optional find_package-gated additions wired into the
# new psio_vs_externals bench (in progress).

# bench_perf_report (legacy): v1 ↔ v3 markdown+CSV report.  shapes.hpp
# was rewritten to drop the V1Foo/V3Foo pairs, which made this
# obsolete.  Kept on disk as reference for the new bench's loop
# structure, but no longer built by default — re-enable manually if
# you need the old comparison columns.
#
# add_executable(psio_bench_perf_report bench_perf_report.cpp)
# target_link_libraries(psio_bench_perf_report PRIVATE psio psio)
# target_include_directories(psio_bench_perf_report PRIVATE
#    ${CMAKE_CURRENT_SOURCE_DIR})
# target_compile_features(psio_bench_perf_report PRIVATE cxx_std_23)

# Unified head-to-head: psio's wire formats × canonical external libs
# × shapes × ops.  Replaces the v1↔v3 bench_perf_report matrix.  The
# psio side is built unconditionally; competitor adapters land via
# find_package gates (PSIO_HAVE_MSGPACK / PSIO_HAVE_PROTOBUF /
# PSIO_HAVE_CAPNP / PSIO_HAVE_FLATBUF / PSIO_HAVE_SSZPP) in follow-up
# commits.
add_executable(psio_bench_vs_externals bench_psio_vs_externals.cpp)
target_link_libraries(psio_bench_vs_externals PRIVATE psio)
target_include_directories(psio_bench_vs_externals PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(psio_bench_vs_externals PRIVATE cxx_std_23)
target_compile_options(psio_bench_vs_externals PRIVATE -O3 -DNDEBUG)

# Optional canonical-external library adapters.  Each is gated by
# find_package; missing libraries quietly drop their column from the
# output rather than failing the build.

find_package(msgpack-cxx QUIET CONFIG)
if(msgpack-cxx_FOUND)
   target_link_libraries(psio_bench_vs_externals PRIVATE msgpack-cxx)
   target_compile_definitions(psio_bench_vs_externals PRIVATE
      PSIO_HAVE_MSGPACK=1)
   message(STATUS
      "psio_bench_vs_externals: msgpack-cxx found — enabled")
else()
   message(STATUS
      "psio_bench_vs_externals: msgpack-cxx not found — skipping")
endif()

find_package(CapnProto QUIET CONFIG)
if(CapnProto_FOUND)
   #  Manual capnp invocation — capnp_generate_cpp from the cmake
   #  module hardcodes paths in ways the bench tree doesn't fit.
   set(CP_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/adapters/bench_schemas.capnp)
   set(CP_GEN_H
      ${CMAKE_CURRENT_BINARY_DIR}/adapters/bench_schemas.capnp.h)
   set(CP_GEN_CC
      ${CMAKE_CURRENT_BINARY_DIR}/adapters/bench_schemas.capnp.c++)
   add_custom_command(
      OUTPUT ${CP_GEN_H} ${CP_GEN_CC}
      COMMAND ${CMAKE_COMMAND} -E make_directory
               ${CMAKE_CURRENT_BINARY_DIR}/adapters
      COMMAND ${CMAKE_COMMAND} -E copy
               ${CP_SCHEMA} ${CMAKE_CURRENT_BINARY_DIR}/adapters/
      COMMAND capnp compile -oc++:${CMAKE_CURRENT_BINARY_DIR}/adapters
               --src-prefix=${CMAKE_CURRENT_BINARY_DIR}/adapters
               ${CMAKE_CURRENT_BINARY_DIR}/adapters/bench_schemas.capnp
      DEPENDS ${CP_SCHEMA}
      COMMENT "capnp compile bench_schemas.capnp")
   target_sources(psio_bench_vs_externals PRIVATE ${CP_GEN_CC})
   target_include_directories(psio_bench_vs_externals PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}/adapters)
   target_link_libraries(psio_bench_vs_externals PRIVATE
      CapnProto::capnp)
   target_compile_definitions(psio_bench_vs_externals PRIVATE
      PSIO_HAVE_CAPNP=1)
   message(STATUS
      "psio_bench_vs_externals: capnp found — enabled")
else()
   message(STATUS
      "psio_bench_vs_externals: capnp not found — skipping")
endif()

find_package(flatbuffers QUIET CONFIG)
if(flatbuffers_FOUND)
   #  flatc generates one bench_schemas_generated.h next to the
   #  consuming source.  We invoke flatc manually because the cmake
   #  module's `flatbuffers_generate_headers()` macro is finicky.
   set(FB_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/adapters/bench_schemas.fbs)
   set(FB_GEN
      ${CMAKE_CURRENT_BINARY_DIR}/adapters/bench_schemas_generated.h)
   add_custom_command(
      OUTPUT ${FB_GEN}
      COMMAND ${CMAKE_COMMAND} -E make_directory
               ${CMAKE_CURRENT_BINARY_DIR}/adapters
      COMMAND flatc --cpp -o ${CMAKE_CURRENT_BINARY_DIR}/adapters
               --gen-name-strings ${FB_SCHEMA}
      DEPENDS ${FB_SCHEMA}
      COMMENT "flatc bench_schemas.fbs")
   add_custom_target(psio_bench_fb_gen DEPENDS ${FB_GEN})
   add_dependencies(psio_bench_vs_externals psio_bench_fb_gen)
   target_include_directories(psio_bench_vs_externals PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}/adapters)
   target_link_libraries(psio_bench_vs_externals PRIVATE
      flatbuffers::flatbuffers)
   target_compile_definitions(psio_bench_vs_externals PRIVATE
      PSIO_HAVE_FLATBUF=1)
   message(STATUS
      "psio_bench_vs_externals: flatbuffers found — enabled")
else()
   message(STATUS
      "psio_bench_vs_externals: flatbuffers not found — skipping")
endif()

find_package(Protobuf QUIET CONFIG)
if(Protobuf_FOUND)
   #  Modern protobuf provides `protobuf_generate` (per-target) which
   #  attaches generated .pb.cc/.pb.h to the consuming target and
   #  drops them in CMAKE_CURRENT_BINARY_DIR/adapters/.
   protobuf_generate(
      TARGET psio_bench_vs_externals
      LANGUAGE cpp
      IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/adapters
      PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/adapters
      PROTOS ${CMAKE_CURRENT_SOURCE_DIR}/adapters/bench_schemas.proto)
   target_include_directories(psio_bench_vs_externals PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}/adapters)
   target_link_libraries(psio_bench_vs_externals PRIVATE
      protobuf::libprotobuf)
   target_compile_definitions(psio_bench_vs_externals PRIVATE
      PSIO_HAVE_PROTOBUF=1)
   message(STATUS
      "psio_bench_vs_externals: libprotobuf found — enabled")
else()
   message(STATUS
      "psio_bench_vs_externals: libprotobuf not found — skipping")
endif()

# Nested-encode baseline — per-format encode timing on balanced
# binary trees of progressively greater depth.  Surfaces the
# fractal O(D × N) recursive-size-walk cost that the protobuf
# stack-cache eliminated; identifies which other formats need
# the same treatment.
add_executable(psio_bench_nested_encode bench_nested_encode.cpp)
target_link_libraries(psio_bench_nested_encode PRIVATE psio)
target_include_directories(psio_bench_nested_encode PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(psio_bench_nested_encode PRIVATE cxx_std_23)
target_compile_options(psio_bench_nested_encode PRIVATE -O3 -DNDEBUG)

# Varint codec library — leb128, prefix2 (BE + LE), prefix3.
add_executable(psio_bench_varint bench_varint.cpp)
target_link_libraries(psio_bench_varint PRIVATE psio)
target_include_directories(psio_bench_varint PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(psio_bench_varint PRIVATE cxx_std_23)
target_compile_options(psio_bench_varint PRIVATE -O3 -DNDEBUG)

# pjson walk: JSON → pjson, plus every reader path on the resulting
# bytes. simdjson DOM / on-demand are included as competitive
# baselines when simdjson is found by the parent.
add_executable(psio_bench_pjson_walk bench_pjson_walk.cpp)
target_link_libraries(psio_bench_pjson_walk PRIVATE psio)
target_include_directories(psio_bench_pjson_walk PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(psio_bench_pjson_walk PRIVATE cxx_std_23)
target_compile_options(psio_bench_pjson_walk PRIVATE -O3 -DNDEBUG)

# pjson per-type encode/decode/view-access timing.
add_executable(psio_bench_pjson_types bench_pjson_types.cpp)
target_link_libraries(psio_bench_pjson_types PRIVATE psio)
target_include_directories(psio_bench_pjson_types PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(psio_bench_pjson_types PRIVATE cxx_std_23)
target_compile_options(psio_bench_pjson_types PRIVATE -O3 -DNDEBUG)

# psch schema-format perf: encode + view + lookup latency, plus
# size-versus-typical-alternatives.
add_executable(psio_bench_psch bench_psch.cpp)
target_link_libraries(psio_bench_psch PRIVATE psio)
target_include_directories(psio_bench_psch PRIVATE
   ${CMAKE_SOURCE_DIR}/external/psitri/libraries/hash/include)
target_compile_features(psio_bench_psch PRIVATE cxx_std_23)
target_compile_options(psio_bench_psch PRIVATE -O3 -DNDEBUG)
