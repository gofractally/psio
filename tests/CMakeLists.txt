# psio test runner. Uses the Catch2 v2 single-header bundled with
# psizam (the single source of Catch2 in the tree, also used by psio
# v1). Phase 0 scaffold creates one placeholder test; subsequent phases
# add psio_<phase>_tests executables via psio_add_test().

set(PSIO_CATCH2_INCLUDE
   ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include/catch2)

# Helper — every phase's test executable calls this so they share the
# same main(), Catch2 include path, C++ standard, and psio link.
function(psio_add_test TARGET)
   add_executable(${TARGET} ${ARGN} main.cpp)
   target_link_libraries(${TARGET} PRIVATE psio)
   target_include_directories(${TARGET} PRIVATE ${PSIO_CATCH2_INCLUDE})
   target_compile_features(${TARGET} PRIVATE cxx_std_23)
   add_test(NAME ${TARGET} COMMAND ${TARGET})
endfunction()

# Phase 0: placeholder test proves the runner wiring works.
psio_add_test(psio_scaffold_test scaffold_test.cpp)

# Phase 1: reflection core — psio::reflect<T>.
psio_add_test(psio_reflect_tests reflect_tests.cpp)

# Phase 2: shape concepts + annotation system + rich wrappers.
psio_add_test(psio_shape_tests shape_tests.cpp)
psio_add_test(psio_annotate_tests annotate_tests.cpp)
psio_add_test(psio_wrapper_tests wrapper_tests.cpp)

# Phase 3: buffer / view / mutable_view + access-surface rule.
psio_add_test(psio_view_access_tests view_access_tests.cpp)

# Phase 4: error model — codec_error, codec_status, or_throw.
psio_add_test(psio_error_tests error_tests.cpp)

# Phase 5: CPOs + format_tag_base scoped sugar.
psio_add_test(psio_cpo_dispatch_tests cpo_dispatch_tests.cpp)

# Phase 6: SSZ codec (first format).
psio_add_test(psio_ssz_tests ssz_tests.cpp)

# Phase 7: pSSZ codec (parametric-width family).
psio_add_test(psio_pssz_tests pssz_tests.cpp)

# Cross-format parity: maxFields / maxDynamicData enforcement.
psio_add_test(psio_caps_parity_tests caps_parity_tests.cpp)

# Phase 9: JSON codec.
psio_add_test(psio_json_tests json_tests.cpp)

# Phase 8: fracpack codec (frac32/frac16).
psio_add_test(psio_frac_tests frac_tests.cpp)

# Phase 10: bin codec (simplest length-prefixed binary).
psio_add_test(psio_bin_tests bin_tests.cpp)

# pjson: schemaless self-describing format. Spec in
# .issues/psio3-schemaless-jsonb.md.
psio_add_test(psio_pjson_tests pjson_tests.cpp)

# §4.4 format-neutral conformance harness — one fixture, all formats.
psio_add_test(psio_conformance_tests conformance_tests.cpp)

# §8 criterion #6: static↔dynamic byte-identity harness.
psio_add_test(psio_static_dynamic_parity_tests
   static_dynamic_parity_tests.cpp)

# §9 step-2 follow-up: minimal fuzz smoke test for validate totality.
psio_add_test(psio_validate_fuzz_tests validate_fuzz_tests.cpp)

# Regression pins: padding-layout struct encoding + field-name collision
# cases that v1 hit in consumer code.
psio_add_test(psio_padding_and_names_tests
   padding_and_names_tests.cpp)

# Phase 14a: psio::schema + reflect-to-schema bridge.
psio_add_test(psio_schema_tests schema_tests.cpp)

# Phase 14b: dynamic_value + dynamic JSON codec.
psio_add_test(psio_dynamic_tests dynamic_tests.cpp)

# Phase 14c: transcode (compile-time T + dynamic-schema forms).
psio_add_test(psio_transcode_tests transcode_tests.cpp)

# Adapter machinery (§5.3.7).
psio_add_test(psio_adapter_tests adapter_tests.cpp)

# Phase 13: runtime reflection patterns (make_proxy examples).
psio_add_test(psio_runtime_reflect_tests runtime_reflect_tests.cpp)

# Phase 17: compat aliases for consumer migration.
psio_add_test(psio_compat_tests compat_tests.cpp)

# Varint codec library — leb128, prefix2 (BE + LE), prefix3.
psio_add_test(psio_varint_tests varint_tests.cpp)

# Name encoding (psio::name_id) — v1↔v3 byte-parity.
add_executable(psio_name_tests name_tests.cpp main.cpp)
target_link_libraries(psio_name_tests PRIVATE psio psio1)
target_include_directories(psio_name_tests PRIVATE ${PSIO_CATCH2_INCLUDE})
target_compile_features(psio_name_tests PRIVATE cxx_std_23)
add_test(NAME psio_name_tests COMMAND psio_name_tests)

# Small utility headers ported from psio v1.
psio_add_test(psio_util_tests util_tests.cpp)

# WIT resource vocabulary (own<T> / borrow<T> / wit_resource_drop).
psio_add_test(psio_wit_resource_tests wit_resource_tests.cpp)

# Method-name compressor + type-name source (compress_name + get_type_name).
add_executable(psio_type_name_tests type_name_tests.cpp main.cpp)
target_link_libraries(psio_type_name_tests PRIVATE psio psio)
target_include_directories(psio_type_name_tests PRIVATE ${PSIO_CATCH2_INCLUDE})
target_compile_features(psio_type_name_tests PRIVATE cxx_std_23)
add_test(NAME psio_type_name_tests COMMAND psio_type_name_tests)

# WIT IR data structures (wit_types.hpp).
psio_add_test(psio_wit_types_tests wit_types_tests.cpp)

# Structural metadata macros (structural.hpp): PSIO_PACKAGE + PSIO_INTERFACE.
psio_add_test(psio_structural_tests structural_tests.cpp)

# Runtime WIT generator (wit_gen.hpp).
psio_add_test(psio_wit_gen_tests wit_gen_tests.cpp)

# Consteval WIT generator (wit_constexpr.hpp).
psio_add_test(psio_wit_constexpr_tests wit_constexpr_tests.cpp)

# Recursive-descent WIT parser (wit_parser.hpp).
psio_add_test(psio_wit_parser_tests wit_parser_tests.cpp)

# Schema IR (schema_ir.hpp) — multi-format pivot data types.
psio_add_test(psio_schema_ir_tests schema_ir_tests.cpp)

# SchemaBuilder (schema_builder.hpp) — C++ reflection → Schema IR.
psio_add_test(psio_schema_builder_tests schema_builder_tests.cpp)

# emit_wit (emit_wit.hpp) — Schema IR → WIT text emitter.
psio_add_test(psio_emit_wit_tests emit_wit_tests.cpp)

# pSSZ round-trip on Schema IR — the canonical schema-schema wire form.
psio_add_test(psio_schema_pssz_tests schema_pssz_tests.cpp)

# emit_pssz (emit_pssz.hpp) — Schema IR → pSSZ wire bytes.
psio_add_test(psio_emit_pssz_tests emit_pssz_tests.cpp)

# emit_capnp (emit_capnp.hpp) — Schema IR → Cap'n Proto schema text.
psio_add_test(psio_emit_capnp_tests emit_capnp_tests.cpp)

# capnp_parser (capnp_parser.hpp) — .capnp text → Schema IR.
psio_add_test(psio_capnp_parser_tests capnp_parser_tests.cpp)

# emit_fbs (emit_fbs.hpp) — Schema IR → FlatBuffers schema text.
psio_add_test(psio_emit_fbs_tests emit_fbs_tests.cpp)

# fbs_parser (fbs_parser.hpp) — .fbs text → Schema IR.
psio_add_test(psio_fbs_parser_tests fbs_parser_tests.cpp)

# MsgPack codec (msgpack.hpp) — schema-driven encode/decode/validate.
psio_add_test(psio_msgpack_tests msgpack_tests.cpp)

# Protobuf codec (protobuf.hpp) — schema-driven encode/decode/validate.
psio_add_test(psio_protobuf_tests protobuf_tests.cpp)

# emit_protobuf (emit_protobuf.hpp) — Schema IR → .proto3 schema text.
psio_add_test(psio_emit_protobuf_tests emit_protobuf_tests.cpp)

# protobuf_parser (protobuf_parser.hpp) — .proto3 text → Schema IR.
psio_add_test(psio_protobuf_parser_tests protobuf_parser_tests.cpp)

# Quick perf comparison of json / msgpack / pjson on a representative
# struct.  Built as a regular executable (not a ctest).  Run manually:
#   ./bin/psio_format_perf
add_executable(psio_format_perf format_perf.cpp)
target_link_libraries(psio_format_perf PRIVATE psio)
target_compile_features(psio_format_perf PRIVATE cxx_std_23)
target_compile_options(psio_format_perf PRIVATE -O3 -DNDEBUG)

# ── Optional: head-to-head vs msgpack-cxx ─────────────────────────────
#
# When msgpack-cxx is available (e.g. `brew install msgpack-cxx` on
# macOS, `apt install libmsgpack-cxx-dev` on Debian/Ubuntu, or any
# CMake-package-config-locatable install), build a separate perf
# binary that links it.  Compares:
#   - typed encode/decode (psio::msgpack vs msgpack::pack /
#     msgpack::unpack with MSGPACK_DEFINE'd struct)
#   - schemaless dynamic walk (msgpack::object tagged tree vs psio's
#     pjson_view, since psio's native schemaless self-describing
#     format is pjson — different wire, same use case).
#
# Run manually:  ./bin/psio_format_perf_external
find_package(msgpack-cxx QUIET CONFIG)
if(msgpack-cxx_FOUND)
   add_executable(psio_format_perf_external format_perf_external.cpp)
   target_link_libraries(psio_format_perf_external
      PRIVATE psio msgpack-cxx)
   target_compile_features(psio_format_perf_external PRIVATE cxx_std_23)
   target_compile_options(psio_format_perf_external
      PRIVATE -O3 -DNDEBUG)
   message(STATUS
      "psio: msgpack-cxx found — psio_format_perf_external enabled")
else()
   message(STATUS
      "psio: msgpack-cxx not found — skipping psio_format_perf_external")
endif()

# Production-shaped WASI 2.3 inputs through wit_gen + wit_constexpr +
# wit_encode (resources, own/borrow, multi-interface).
psio_add_test(psio_wit_wasi_shapes_tests wit_wasi_shapes_tests.cpp)

# WIT Component Model binary encoder (wit_encode.hpp).
# Links psio1 too — the byte-parity test compares v3 output against v1.
add_executable(psio_wit_encode_tests wit_encode_tests.cpp main.cpp)
target_link_libraries(psio_wit_encode_tests PRIVATE psio psio1)
target_include_directories(psio_wit_encode_tests PRIVATE ${PSIO_CATCH2_INCLUDE})
target_compile_features(psio_wit_encode_tests PRIVATE cxx_std_23)
add_test(NAME psio_wit_encode_tests COMMAND psio_wit_encode_tests)

# Phase 6b partial: v1 ↔ psio SSZ byte-parity tests. Links BOTH psio
# (v1) and psio into the same executable so we can encode the same
# value through both codecs and assert identical bytes.
add_executable(psio_v1_parity_ssz_tests v1_parity_ssz_tests.cpp main.cpp)
target_link_libraries(psio_v1_parity_ssz_tests PRIVATE psio psio)
target_include_directories(psio_v1_parity_ssz_tests PRIVATE
   ${PSIO_CATCH2_INCLUDE})
target_compile_features(psio_v1_parity_ssz_tests PRIVATE cxx_std_23)
add_test(NAME psio_v1_parity_ssz_tests COMMAND psio_v1_parity_ssz_tests)

add_executable(psio_v1_parity_pssz_tests v1_parity_pssz_tests.cpp main.cpp)
target_link_libraries(psio_v1_parity_pssz_tests PRIVATE psio psio)
target_include_directories(psio_v1_parity_pssz_tests PRIVATE
   ${PSIO_CATCH2_INCLUDE})
target_compile_features(psio_v1_parity_pssz_tests PRIVATE cxx_std_23)
add_test(NAME psio_v1_parity_pssz_tests COMMAND psio_v1_parity_pssz_tests)

add_executable(psio_v1_parity_frac_tests v1_parity_frac_tests.cpp main.cpp)
target_link_libraries(psio_v1_parity_frac_tests PRIVATE psio psio)
target_include_directories(psio_v1_parity_frac_tests PRIVATE
   ${PSIO_CATCH2_INCLUDE})
target_compile_features(psio_v1_parity_frac_tests PRIVATE cxx_std_23)
add_test(NAME psio_v1_parity_frac_tests COMMAND psio_v1_parity_frac_tests)

add_executable(psio_v1_parity_borsh_tests v1_parity_borsh_tests.cpp main.cpp)
target_link_libraries(psio_v1_parity_borsh_tests PRIVATE psio psio)
target_include_directories(psio_v1_parity_borsh_tests PRIVATE
   ${PSIO_CATCH2_INCLUDE})
target_compile_features(psio_v1_parity_borsh_tests PRIVATE cxx_std_23)
add_test(NAME psio_v1_parity_borsh_tests COMMAND psio_v1_parity_borsh_tests)

add_executable(psio_v1_parity_bincode_tests v1_parity_bincode_tests.cpp main.cpp)
target_link_libraries(psio_v1_parity_bincode_tests PRIVATE psio psio)
target_include_directories(psio_v1_parity_bincode_tests PRIVATE
   ${PSIO_CATCH2_INCLUDE})
target_compile_features(psio_v1_parity_bincode_tests PRIVATE cxx_std_23)
add_test(NAME psio_v1_parity_bincode_tests COMMAND psio_v1_parity_bincode_tests)

add_executable(psio_v1_parity_key_tests v1_parity_key_tests.cpp main.cpp)
target_link_libraries(psio_v1_parity_key_tests PRIVATE psio psio)
target_include_directories(psio_v1_parity_key_tests PRIVATE
   ${PSIO_CATCH2_INCLUDE})
target_compile_features(psio_v1_parity_key_tests PRIVATE cxx_std_23)
add_test(NAME psio_v1_parity_key_tests COMMAND psio_v1_parity_key_tests)

add_executable(psio_v1_parity_avro_tests v1_parity_avro_tests.cpp main.cpp)
target_link_libraries(psio_v1_parity_avro_tests PRIVATE psio psio)
target_include_directories(psio_v1_parity_avro_tests PRIVATE
   ${PSIO_CATCH2_INCLUDE})
target_compile_features(psio_v1_parity_avro_tests PRIVATE cxx_std_23)
add_test(NAME psio_v1_parity_avro_tests COMMAND psio_v1_parity_avro_tests)

# Cross-format v1-oracle tests. Ported in pattern from psio2's
# v1_oracle_tests.cpp: parallel v1/v3 types with matching shapes, byte
# equality across every format. Exercises reflected records — coverage
# the per-format parity tests were missing.
add_executable(psio_v1_oracle_tests v1_oracle_tests.cpp main.cpp)
target_link_libraries(psio_v1_oracle_tests PRIVATE psio psio)
target_include_directories(psio_v1_oracle_tests PRIVATE
   ${PSIO_CATCH2_INCLUDE})
target_compile_features(psio_v1_oracle_tests PRIVATE cxx_std_23)
add_test(NAME psio_v1_oracle_tests COMMAND psio_v1_oracle_tests)

# Flatbuf-lib parity test — only if the Google flatbuffers C++ runtime is
# available via find_package. Matches the same optional path v1 uses for
# its flatbuf benchmarks.
find_package(flatbuffers QUIET)
if(flatbuffers_FOUND)
   add_executable(psio_v1_parity_flatbuf_lib_tests
      v1_parity_flatbuf_lib_tests.cpp main.cpp)
   target_link_libraries(psio_v1_parity_flatbuf_lib_tests
      PRIVATE psio psio flatbuffers::flatbuffers)
   target_include_directories(psio_v1_parity_flatbuf_lib_tests PRIVATE
      ${PSIO_CATCH2_INCLUDE})
   target_compile_features(psio_v1_parity_flatbuf_lib_tests PRIVATE
      cxx_std_23)
   add_test(NAME psio_v1_parity_flatbuf_lib_tests
      COMMAND psio_v1_parity_flatbuf_lib_tests)

   # Native (zero-dep) vs lib (Google runtime) parity harness.
   add_executable(psio_v1_parity_flatbuf_tests
      v1_parity_flatbuf_tests.cpp main.cpp)
   target_link_libraries(psio_v1_parity_flatbuf_tests
      PRIVATE psio flatbuffers::flatbuffers)
   target_include_directories(psio_v1_parity_flatbuf_tests PRIVATE
      ${PSIO_CATCH2_INCLUDE})
   target_compile_features(psio_v1_parity_flatbuf_tests PRIVATE
      cxx_std_23)
   add_test(NAME psio_v1_parity_flatbuf_tests
      COMMAND psio_v1_parity_flatbuf_tests)
endif()

# Official Ethereum consensus-spec-tests harness.
# Fixtures live in ${CMAKE_BINARY_DIR}/spec-tests/_decoded, produced by
# libraries/psio/cpp/tests/spec_tests/preprocess.py. The test reads
# the manifest path from PSIO_SPEC_MANIFEST and the raw-bytes root
# from PSIO_SPEC_DECODED_ROOT (both set below), so the test binary
# doesn't need to guess relative paths.
psio_add_test(psio_spec_ssz_tests spec_ssz_tests.cpp)
set_tests_properties(psio_spec_ssz_tests PROPERTIES
   ENVIRONMENT
      "PSIO_SPEC_MANIFEST=${CMAKE_BINARY_DIR}/spec-tests/_decoded/manifest.txt;PSIO_SPEC_DECODED_ROOT=${CMAKE_BINARY_DIR}/spec-tests/_decoded")

# Smoke perf: v1 vs v3 timing on representative SSZ workloads. Built
# as a regular executable (not a ctest) so ctest runs stay fast; run
# manually: ./bin/psio_smoke_perf
add_executable(psio_smoke_perf smoke_perf.cpp)
target_link_libraries(psio_smoke_perf PRIVATE psio psio)
target_compile_features(psio_smoke_perf PRIVATE cxx_std_23)
target_compile_options(psio_smoke_perf PRIVATE -O3 -DNDEBUG)

# psch — pSSZ schema binary (writer + view). Round-trip + lookup.
psio_add_test(psio_psch_tests psch_tests.cpp)
target_include_directories(psio_psch_tests PRIVATE
   ${CMAKE_SOURCE_DIR}/external/psitri/libraries/hash/include)
