# psio test runner. Uses the Catch2 v2 single-header bundled with
# psizam (the single source of Catch2 in the tree, also used by psio
# v1). Phase 0 scaffold creates one placeholder test; subsequent phases
# add psio_<phase>_tests executables via psio_add_test().

set(PSIO_CATCH2_INCLUDE
   ${CMAKE_CURRENT_SOURCE_DIR}/../external/catch2)

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
# psio::fixed_string: a structural compile-time string (template argument,
# constexpr concatenation), and reflect<T>::fixed_name.
psio_add_test(psio_fixed_string_tests fixed_string_tests.cpp)

# Compile-time name / number / hashed-key lookup tables (name_lookup.hpp).
psio_add_test(psio_name_lookup_tests name_lookup_tests.cpp)
# Two members colliding under a caller-supplied hash must not compile.
get_target_property(PSIO_BOOST_INCLUDES Boost::headers INTERFACE_INCLUDE_DIRECTORIES)
add_test(NAME psio_name_lookup_collision_compile_fail
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only
                 -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                 ${CMAKE_CURRENT_SOURCE_DIR}/name_lookup_collision_fail.cpp)
set_tests_properties(psio_name_lookup_collision_compile_fail PROPERTIES
   PASS_REGULAR_EXPRESSION "two names of one type collide under the supplied hash")

# Schema fidelity: IR-derived widths, raw vs UTF-8 strings, explicit-width
# caps and custom text handlers agree with the typed codecs.
psio_add_test(psio_schema_fidelity_tests schema_fidelity_tests.cpp)

# Method reflection, the one proxy, interfaces from a reflected type.
psio_add_test(psio_reflect_method_tests reflect_method_tests.cpp)
# A field and a method with one name, or a member named like a proxy
# internal, must not compile.
add_test(NAME psio_reflect_duplicate_name_compile_fail
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only
                 -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                 ${CMAKE_CURRENT_SOURCE_DIR}/reflect_duplicate_name_fail.cpp)
set_tests_properties(psio_reflect_duplicate_name_compile_fail PROPERTIES
   PASS_REGULAR_EXPRESSION "a field and a method may not share a name")
add_test(NAME psio_reflect_reserved_name_compile_fail
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only
                 -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                 ${CMAKE_CURRENT_SOURCE_DIR}/reflect_reserved_name_fail.cpp)
set_tests_properties(psio_reflect_reserved_name_compile_fail PROPERTIES
   PASS_REGULAR_EXPRESSION "lists a member named like a proxy internal")
# Consumer declaration patterns (WASI-style) pinned byte for byte.
psio_add_test(psio_interface_consumer_pattern_tests interface_consumer_pattern_tests.cpp)
target_compile_definitions(psio_interface_consumer_pattern_tests PRIVATE
   PSIO_CONSUMER_PATTERN_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/interface_consumer_pattern.golden.txt")

# Phase 2: shape concepts + annotation system + rich wrappers.
psio_add_test(psio_shape_tests shape_tests.cpp)
psio_add_test(psio_annotate_tests annotate_tests.cpp)
add_library(psio_annotate_header_check OBJECT annotate_header_check.cpp)
target_link_libraries(psio_annotate_header_check PRIVATE psio)
target_compile_features(psio_annotate_header_check PRIVATE cxx_std_23)
target_compile_definitions(psio_annotate_header_check PRIVATE _LIBCPP_REMOVE_TRANSITIVE_INCLUDES)
# <psio/reflect.hpp> alone is enough for every keyword PSIO_REFLECT accepts.
add_library(psio_reflect_header_check OBJECT reflect_header_check.cpp)
target_link_libraries(psio_reflect_header_check PRIVATE psio)
target_compile_features(psio_reflect_header_check PRIVATE cxx_std_23)
target_compile_definitions(psio_reflect_header_check PRIVATE _LIBCPP_REMOVE_TRANSITIVE_INCLUDES)
psio_add_test(psio_wrapper_tests wrapper_tests.cpp)

# Phase 3: buffer / view / mutable_view + access-surface rule.
psio_add_test(psio_view_access_tests view_access_tests.cpp)

# psio::mutable_view (docs/pssz-spec.md §17): in-place overwrite of fixed-size
# values in a PSSZ frame; the frame stays valid and canonical.
psio_add_test(psio_mutable_view_tests mutable_view_tests.cpp)
# The same surface with exceptions and RTTI disabled.
add_executable(psio_mutable_view_noexceptions_test mutable_view_noexceptions.cpp)
target_include_directories(psio_mutable_view_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_mutable_view_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_mutable_view_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_mutable_view_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_mutable_view_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_mutable_view_noexceptions_test COMMAND psio_mutable_view_noexceptions_test)
# Mutations that cannot preserve the frame must not compile, each with a
# message naming the reason. The control (no case selected) must compile.
add_test(NAME psio_mutable_view_control_compiles
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only
                 -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                 ${CMAKE_CURRENT_SOURCE_DIR}/mutable_view_fail.cpp)
function(psio_mutable_view_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_mutable_view_${NAME}_compile_fail
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -D${DEFINE}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/mutable_view_fail.cpp)
   set_tests_properties(psio_mutable_view_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
set(PSIO_MV_FIXED_ONLY "only a fixed-size value can be overwritten in place")
psio_mutable_view_compile_fail(string MV_FAIL_STRING "${PSIO_MV_FIXED_ONLY}")
psio_mutable_view_compile_fail(vector MV_FAIL_VECTOR "${PSIO_MV_FIXED_ONLY}")
psio_mutable_view_compile_fail(bitlist MV_FAIL_BITLIST "${PSIO_MV_FIXED_ONLY}")
psio_mutable_view_compile_fail(extensible_record MV_FAIL_EXTENSIBLE_RECORD "${PSIO_MV_FIXED_ONLY}")
psio_mutable_view_compile_fail(optional MV_FAIL_OPTIONAL
   "an optional's presence cannot change in place")
psio_mutable_view_compile_fail(variant MV_FAIL_VARIANT
   "a variant's alternative cannot change in place")
psio_mutable_view_compile_fail(blocked_type MV_FAIL_BLOCKED_TYPE
   "in-place mutation is blocked for this type")
psio_mutable_view_compile_fail(blocked_root MV_FAIL_BLOCKED_ROOT
   "in-place mutation is blocked for this type")
psio_mutable_view_compile_fail(blocked_nested MV_FAIL_BLOCKED_NESTED
   "the value contains a type whose in-place mutation is blocked")
psio_mutable_view_compile_fail(as_override MV_FAIL_AS_OVERRIDE
   "a member with an as<Tag> override is not addressable in place")
psio_mutable_view_compile_fail(sorted_member MV_FAIL_SORTED_MEMBER
   "a member whose annotations constrain its content is not addressable in place")
psio_mutable_view_compile_fail(validated_member MV_FAIL_VALIDATED_MEMBER
   "a member whose annotations constrain its content is not addressable in place")
psio_mutable_view_compile_fail(element_member MV_FAIL_ELEMENT_MEMBER
   "a member whose annotations constrain its content is not addressable in place")
psio_mutable_view_compile_fail(at_member MV_FAIL_AT_MEMBER
   "a member whose annotations constrain its content is not addressable in place")
psio_mutable_view_compile_fail(format MV_FAIL_FORMAT
   "in-place mutation is defined for PSSZ formats only")

# The per-format helper names (psio/helpers.hpp): to_F / from_F / validate_F /
# F_size / F_view / F_buffer / shared_F, exercised for every format tag.
psio_add_test(psio_helpers_tests helpers_tests.cpp)
# The same names with exceptions and RTTI disabled. The checked decode of a
# rejected input calls std::abort there: the second test expects the process
# to die of SIGABRT (exit status 134 as the shell reports it).
add_executable(psio_helpers_noexceptions_test helpers_noexceptions.cpp)
target_include_directories(psio_helpers_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_helpers_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_helpers_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_helpers_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_helpers_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_helpers_noexceptions_test COMMAND psio_helpers_noexceptions_test)
add_test(NAME psio_helpers_noexceptions_checked_decode_aborts
         COMMAND sh -c "\"$1\" abort; test $? -eq 134" sh
                 $<TARGET_FILE:psio_helpers_noexceptions_test>)
# A helper its format does not have must not compile, and the diagnostic
# names the helper. The control (no case selected) must compile.
set(PSIO_HELPERS_FAIL_COMMAND
    ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -DPSIO_EXCEPTIONS_ENABLED=1
    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES})
add_test(NAME psio_helpers_control_compiles
         COMMAND ${PSIO_HELPERS_FAIL_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/helpers_fail.cpp)
function(psio_helpers_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_helpers_${NAME}_compile_fail
            COMMAND ${PSIO_HELPERS_FAIL_COMMAND} -D${DEFINE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/helpers_fail.cpp)
   set_tests_properties(psio_helpers_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
psio_helpers_compile_fail(json_view HELPERS_FAIL_JSON_VIEW
   "constraints not satisfied for alias template 'json_view'")
psio_helpers_compile_fail(ssz_mutable_view HELPERS_FAIL_SSZ_MUTABLE_VIEW
   "constraints not satisfied for alias template 'ssz_mutable_view'")
psio_helpers_compile_fail(wit_sink HELPERS_FAIL_WIT_SINK
   "no matching function for call to 'to_wit'")
psio_helpers_compile_fail(json_vector_sink HELPERS_FAIL_JSON_VECTOR_SINK
   "no matching function for call to 'to_json'")
psio_helpers_compile_fail(unchecked_bytes HELPERS_FAIL_UNCHECKED_BYTES
   "no viable conversion from 'std::span<const char>' to 'psio::trusted'")
# Const safety: a view never writes, and a mutable view is made from mutable
# bytes only — not from const bytes, a const or shared buffer, or a view.
psio_helpers_compile_fail(view_set HELPERS_FAIL_VIEW_SET
   "member reference base type")
psio_helpers_compile_fail(const_view_set HELPERS_FAIL_CONST_VIEW_SET
   "no member named 'set' in 'psio::view<const Rec, psio::pssz>'")
psio_helpers_compile_fail(shared_mutable HELPERS_FAIL_SHARED_MUTABLE
   "no matching function for call to 'as_mutable_view'")
psio_helpers_compile_fail(mutable_from_const_bytes HELPERS_FAIL_MUTABLE_FROM_CONST_BYTES
   "no matching constructor for initialization of 'psio::pssz_mutable_view<Rec>'")
psio_helpers_compile_fail(mutable_from_const_span HELPERS_FAIL_MUTABLE_FROM_CONST_SPAN
   "no matching constructor for initialization of 'psio::pssz_mutable_view<Rec>'")
psio_helpers_compile_fail(mutable_from_const_buffer HELPERS_FAIL_MUTABLE_FROM_CONST_BUFFER
   "no matching function for call to 'as_mutable_view'")
psio_helpers_compile_fail(mutable_from_view HELPERS_FAIL_MUTABLE_FROM_VIEW
   "no matching constructor for initialization of 'psio::pssz_mutable_view<Rec>'")
psio_helpers_compile_fail(mutable_from_view_bytes HELPERS_FAIL_MUTABLE_FROM_VIEW_BYTES
   "no matching constructor for initialization of 'psio::pssz_mutable_view<Rec>'")
psio_helpers_compile_fail(view_assigned_to_mutable HELPERS_FAIL_VIEW_ASSIGNED_TO_MUTABLE
   "no viable conversion from 'psio::pssz_view<Rec>'")
psio_helpers_compile_fail(mutable_view_of_const HELPERS_FAIL_MUTABLE_VIEW_OF_CONST
   "constraints not satisfied for alias template 'pssz_mutable_view' .with X = const Rec.")
psio_helpers_compile_fail(generic_mutable_view_of_const HELPERS_FAIL_GENERIC_MUTABLE_VIEW_OF_CONST
   "a mutable view of a const type is a contradiction")
psio_helpers_compile_fail(view_mut_borrow HELPERS_FAIL_VIEW_MUT_BORROW
   "psio::view is the read-only view; the view that writes is psio::mutable_view")
# A view borrows: the owning / shared storage kinds name buffers.
psio_helpers_compile_fail(view_owning HELPERS_FAIL_VIEW_OWNING
   "the type that owns encoded bytes is psio::buffer<T, Fmt> .F_buffer<T>.")
psio_helpers_compile_fail(view_shared HELPERS_FAIL_VIEW_SHARED
   "psio::buffer<T, Fmt, storage::shared> .shared_F<T>.")
psio_helpers_compile_fail(view_owning_scalar HELPERS_FAIL_VIEW_OWNING_SCALAR
   "the type that owns encoded bytes is psio::buffer<T, Fmt> .F_buffer<T>.")
psio_helpers_compile_fail(view_shared_const HELPERS_FAIL_VIEW_SHARED_CONST
   "psio::buffer<T, Fmt, storage::shared> .shared_F<T>.")

# native format + view_of concept: zero-cost shim adapting a live const T&
# to the view<T, Fmt, Store> accessor surface.
psio_add_test(psio_native_view_tests native_view_tests.cpp)

# Phase 4: error model — codec_error, codec_status, or_throw.
psio_add_test(psio_error_tests error_tests.cpp)
# An error message built at run time (codec_fail_owned): through every
# checked entry point, with and without exceptions.
psio_add_test(psio_owned_error_tests owned_error_tests.cpp)
add_executable(psio_owned_error_noexceptions_test owned_error_noexceptions.cpp)
target_include_directories(psio_owned_error_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_owned_error_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_owned_error_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_owned_error_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_owned_error_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_owned_error_noexceptions_test COMMAND psio_owned_error_noexceptions_test)
psio_add_test(psio_stream_tests stream_tests.cpp)
add_executable(psio_guest_codec_check guest_codec_check.cpp)
target_link_libraries(psio_guest_codec_check PRIVATE psio)
add_test(NAME psio_guest_codec_check COMMAND psio_guest_codec_check)

# Phase 5: CPOs + format_tag_base scoped sugar.
psio_add_test(psio_cpo_dispatch_tests cpo_dispatch_tests.cpp)

# Phase 6: SSZ codec (first format).
psio_add_test(psio_ssz_tests ssz_tests.cpp)
# ssz.hpp without exceptions, like the other format headers: an executable
# built with PSIO_EXCEPTIONS_ENABLED=0 and -fno-exceptions, and the same
# source compiled with the macro not defined at all (with and without
# compiler exception support).
add_executable(psio_ssz_noexceptions_test ssz_noexceptions.cpp)
target_include_directories(psio_ssz_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_ssz_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_ssz_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_ssz_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_ssz_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_ssz_noexceptions_test COMMAND psio_ssz_noexceptions_test)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   add_test(NAME psio_ssz_exceptions_macro_undefined_compiles
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/ssz_noexceptions.cpp)
   add_test(NAME psio_ssz_exceptions_macro_undefined_no_exceptions_compiles
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -fno-exceptions -fno-rtti
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/ssz_noexceptions.cpp)
endif()

# Phase 7: pSSZ codec (parametric-width family).
psio_add_test(psio_pssz_tests pssz_tests.cpp)
psio_add_test(psio_pssz_type_width_tests pssz_type_width_tests.cpp)
psio_add_test(psio_pssz_zero_tests pssz_zero_tests.cpp)
# Plain C++ enums: the underlying integer in every position and width.
psio_add_test(psio_pssz_enum_tests pssz_enum_tests.cpp)
# String pairing: std::string raw bytes, utf8_string<> / utf8_string<N> UTF-8.
psio_add_test(psio_pssz_string_tests pssz_string_tests.cpp)
# A member length_bound is enforced by validate and the checked readers.
psio_add_test(psio_pssz_length_bound_tests pssz_length_bound_tests.cpp)
# The inferred size bound of variants, tuples and pairs, and the widths
# that follow from it (typed and schema-driven).
psio_add_test(psio_pssz_inferred_bound_tests pssz_inferred_bound_tests.cpp)
# inferred_bound (what a width is chosen from) against max_encoded_size (the
# largest encoding, at the real widths and at most the cap): cap boundaries.
psio_add_test(psio_pssz_max_encoded_size_tests pssz_max_encoded_size_tests.cpp)
# Bounds by annotation at any depth (element / keys / values / at<I>) and by
# wrapper type (psio::bounded, psio::utf8_string): widths, bytes, enforcement.
psio_add_test(psio_annotation_tree_tests annotation_tree_tests.cpp)
# method(name, attr(p, specs)..., maxDynamicData(N), returns(specs)): metadata,
# the annotated argument tuple, evolution, the interface schema.
psio_add_test(psio_method_annotation_tests method_annotation_tests.cpp)
psio_add_test(psio_doc_annotations_examples doc_annotations_examples.cpp)  # the snippets of docs/annotations*.md, docs/tutorial-annotations.md
# An annotation on the wrong shape is a compile error that names the mistake.
add_test(NAME psio_annotation_tree_control_compiles
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only
                 -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                 ${CMAKE_CURRENT_SOURCE_DIR}/annotation_tree_fail.cpp)
function(psio_annotation_tree_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_annotation_tree_${NAME}_compile_fail
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -D${DEFINE}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/annotation_tree_fail.cpp)
   set_tests_properties(psio_annotation_tree_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
set(PSIO_AT_ELEMENT "element\\(\\.\\.\\.\\) annotates the elements of a list")
psio_annotation_tree_compile_fail(element_on_string AT_FAIL_ELEMENT_ON_STRING "${PSIO_AT_ELEMENT}")
psio_annotation_tree_compile_fail(element_on_map AT_FAIL_ELEMENT_ON_MAP "${PSIO_AT_ELEMENT}")
psio_annotation_tree_compile_fail(keys_on_vector AT_FAIL_KEYS_ON_VECTOR
   "keys\\(\\.\\.\\.\\) annotates the keys of a map")
psio_annotation_tree_compile_fail(values_on_vector AT_FAIL_VALUES_ON_VECTOR
   "values\\(\\.\\.\\.\\) annotates the mapped values of a map")
psio_annotation_tree_compile_fail(at_on_vector AT_FAIL_AT_ON_VECTOR
   "at<I>\\(\\.\\.\\.\\) annotates element I of a std::tuple / std::pair")
psio_annotation_tree_compile_fail(at_out_of_range AT_FAIL_AT_OUT_OF_RANGE
   "I is not an element of this tuple / pair")
psio_annotation_tree_compile_fail(at_variant_out_of_range AT_FAIL_AT_VARIANT_OUT_OF_RANGE
   "I is not an alternative of this variant")
psio_annotation_tree_compile_fail(max_on_int_element AT_FAIL_MAX_ON_INT_ELEMENT
   "bounds a std::string \\(bytes\\) or a std::vector, map or set \\(elements\\)")
psio_annotation_tree_compile_fail(utf8_on_list_element AT_FAIL_UTF8_ON_LIST_ELEMENT
   "this position is not a string")
psio_annotation_tree_compile_fail(nested_exact AT_FAIL_NESTED_EXACT
   "below the member itself a length bound is max<N>")
psio_annotation_tree_compile_fail(view_narrowed AT_FAIL_VIEW_NARROWED
   "a pssz view cannot address this variant")
psio_annotation_tree_compile_fail(deep AT_FAIL_DEEP
   "bounds a std::string \\(bytes\\) or a std::vector, map or set \\(elements\\)")

# A tuple has at most 255 elements (docs/pssz-spec.md 8.4): the codec, the
# width trait and the schema builder refuse a longer one at compile time, and
# say so. The control (255 elements) must compile.
set(PSIO_TUPLE_LIMIT_COMMAND
    ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -DPSIO_EXCEPTIONS_ENABLED=1
    -ftemplate-depth=2048 -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES})
add_test(NAME psio_pssz_tuple_limit_control_compiles
         COMMAND ${PSIO_TUPLE_LIMIT_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/pssz_tuple_limit_fail.cpp)
foreach(case ENCODE DECODE WIDTH SCHEMA)
   string(TOLOWER ${case} case_name)
   add_test(NAME psio_pssz_tuple_limit_${case_name}_compile_fail
            COMMAND ${PSIO_TUPLE_LIMIT_COMMAND} -DTUPLE_LIMIT_FAIL_${case}
                    ${CMAKE_CURRENT_SOURCE_DIR}/pssz_tuple_limit_fail.cpp)
   set_tests_properties(psio_pssz_tuple_limit_${case_name}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "a tuple has at most 255 elements")
endforeach()

# Shared cross-language conformance fixture (conformance/pssz-vectors.json).
# The emitter generates it from the implementation; the regen test fails when
# the checked-in file is not what the implementation produces, and the
# conformance test checks every vector against every reader entry point.
set(PSIO_PSSZ_VECTORS ${CMAKE_SOURCE_DIR}/conformance/pssz-vectors.json)
add_executable(psio_pssz_conformance_emit pssz_conformance_emit.cpp)
target_link_libraries(psio_pssz_conformance_emit PRIVATE psio)
target_compile_features(psio_pssz_conformance_emit PRIVATE cxx_std_23)
add_test(NAME psio_pssz_conformance_regen
         COMMAND psio_pssz_conformance_emit --check ${PSIO_PSSZ_VECTORS})
psio_add_test(psio_pssz_conformance_tests pssz_conformance_tests.cpp)
target_compile_definitions(psio_pssz_conformance_tests PRIVATE
   PSIO_PSSZ_VECTORS="${PSIO_PSSZ_VECTORS}")
add_executable(psio_pssz_type_width_consumer pssz_type_width_consumer.cpp)
target_link_libraries(psio_pssz_type_width_consumer PRIVATE psio::psio)
add_test(NAME psio_pssz_type_width_consumer COMMAND psio_pssz_type_width_consumer)
add_executable(psio_empty_action_consumer empty_action_consumer.cpp)
target_link_libraries(psio_empty_action_consumer PRIVATE psio::psio)
add_test(NAME psio_empty_action_consumer COMMAND psio_empty_action_consumer)
psio_add_test(psio_pssz_record_layout_tests pssz_record_layout_tests.cpp)
psio_add_test(psio_pssz_ir_tests pssz_ir_tests.cpp)
add_executable(psio_pssz_ir_noexceptions_test pssz_ir_noexceptions.cpp)
target_include_directories(psio_pssz_ir_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_pssz_ir_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_pssz_ir_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_pssz_ir_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_pssz_ir_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_pssz_ir_noexceptions_test COMMAND psio_pssz_ir_noexceptions_test)
# The JSON API beyond one typed value in a std::string: views, untagged
# variants, psio::json_value, cursor reads, pretty printing, sinks.
psio_add_test(psio_json_api_tests json_api_tests.cpp)
add_executable(psio_json_value_noexceptions_test json_value_noexceptions.cpp)
target_include_directories(psio_json_value_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_json_value_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_json_value_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_json_value_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_json_value_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_json_value_noexceptions_test COMMAND psio_json_value_noexceptions_test)
# Run-time schema comparison (pssz_ir_compare.hpp): equivalent / evolves_from
# over the Schema IR, the twin of the compile-time psio::pssz_evolves_from.
psio_add_test(psio_pssz_ir_compare_tests pssz_ir_compare_tests.cpp)
add_executable(psio_pssz_ir_compare_noexceptions_test pssz_ir_compare_noexceptions.cpp)
target_include_directories(psio_pssz_ir_compare_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_pssz_ir_compare_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_pssz_ir_compare_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_pssz_ir_compare_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_pssz_ir_compare_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_pssz_ir_compare_noexceptions_test COMMAND psio_pssz_ir_compare_noexceptions_test)
# Shared cross-language evolution fixture (conformance/evolution-vectors.json):
# pairs of schemas with the verdicts of evolves_from / equivalent. Emitted
# here (and checked against the compile-time psio::pssz_evolves_from), for
# the run-time checkers of the other languages.
set(PSIO_EVOLUTION_VECTORS ${CMAKE_SOURCE_DIR}/conformance/evolution-vectors.json)
add_executable(psio_evolution_conformance_emit evolution_conformance_emit.cpp)
target_link_libraries(psio_evolution_conformance_emit PRIVATE psio)
target_compile_features(psio_evolution_conformance_emit PRIVATE cxx_std_23)
add_test(NAME psio_evolution_conformance_regen
         COMMAND psio_evolution_conformance_emit --check ${PSIO_EVOLUTION_VECTORS})
psio_add_test(psio_evolution_conformance_tests evolution_conformance_tests.cpp)
target_compile_definitions(psio_evolution_conformance_tests PRIVATE
   PSIO_EVOLUTION_VECTORS="${PSIO_EVOLUTION_VECTORS}")
psio_add_test(psio_pssz_optional_contract_tests pssz_optional_contract_tests.cpp)
# Cross-version matrix: field evolution, mandatory pruning, golden bytes.
psio_add_test(psio_pssz_evolution_matrix_tests pssz_evolution_matrix_tests.cpp)

# PSSZ guest profile must compile independently of the native library's
# exception-enabled interface definitions.
add_executable(psio_pssz_noexceptions_test pssz_noexceptions.cpp)
target_include_directories(psio_pssz_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_pssz_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_pssz_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_pssz_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_pssz_noexceptions_test PRIVATE -fno-exceptions)
endif()
add_test(NAME psio_pssz_noexceptions_test COMMAND psio_pssz_noexceptions_test)

# Cross-format parity: maxFields / maxDynamicData enforcement.
psio_add_test(psio_caps_parity_tests caps_parity_tests.cpp)

# Phase 9: JSON codec.
psio_add_test(psio_json_tests json_tests.cpp)
# I-JSON (psio::ijson), tuples, integer-form annotations, typed validation.
psio_add_test(psio_ijson_tests ijson_tests.cpp)
# Member / alternative lookup in the typed readers: expected order, any other
# order through the perfect hash, repeated and unknown keys, 200 members.
psio_add_test(psio_json_lookup_tests json_lookup_tests.cpp)
# The two syntax layers under the typed readers (hand-written lexer and
# simdjson) accept the same texts and read the same values: pointed cases
# and a differential run over mutated documents.
psio_add_test(psio_json_parser_tests json_parser_tests.cpp)
# Shared cross-language JSON / I-JSON fixture (conformance/json-vectors.json):
# emitted from the implementation, checked for drift, and consumed here and
# by cargo test (rust/psio/tests/json_conformance.rs).
set(PSIO_JSON_VECTORS ${CMAKE_SOURCE_DIR}/conformance/json-vectors.json)
add_executable(psio_json_conformance_emit json_conformance_emit.cpp)
target_link_libraries(psio_json_conformance_emit PRIVATE psio)
target_compile_features(psio_json_conformance_emit PRIVATE cxx_std_23)
add_test(NAME psio_json_conformance_regen
         COMMAND psio_json_conformance_emit --check ${PSIO_JSON_VECTORS})
psio_add_test(psio_json_conformance_tests json_conformance_tests.cpp)
target_compile_definitions(psio_json_conformance_tests PRIVATE
   PSIO_JSON_VECTORS="${PSIO_JSON_VECTORS}")
# Schema-driven conformance: both shared fixtures re-derived from the Schema
# IR recorded in them, with no C++ type (pssz_ir.hpp, pssz_ir_json.hpp).
psio_add_test(psio_schema_conformance_tests schema_conformance_tests.cpp)
target_compile_definitions(psio_schema_conformance_tests PRIVATE
   PSIO_JSON_VECTORS="${PSIO_JSON_VECTORS}"
   PSIO_PSSZ_VECTORS="${PSIO_PSSZ_VECTORS}")
# The json / ijson status paths with exceptions (and RTTI) disabled.
add_executable(psio_ijson_noexceptions_test ijson_noexceptions.cpp)
target_include_directories(psio_ijson_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_ijson_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_ijson_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_ijson_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_ijson_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_ijson_noexceptions_test COMMAND psio_ijson_noexceptions_test)

# The experimental fracpack codec (psio::experimental::frac32 / frac16).
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

# Structural validation walkers (real bounds-checked decode-without-copy)
# across every binary format. Round-trip + truncation + bad-tag + depth-cap.
psio_add_test(psio_validate_structural_tests
   validate_structural_tests.cpp)

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

# Small utility headers ported from psio v1.
psio_add_test(psio_util_tests util_tests.cpp)

# WIT resource vocabulary (own<T> / borrow<T> / wit_resource_drop).
psio_add_test(psio_wit_resource_tests wit_resource_tests.cpp)

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
# A byte list is one schema node whichever C++ type spells the byte
# (std::vector<char> = std::vector<std::uint8_t> = Custom{List<u8>, "hex"}).
psio_add_test(psio_schema_byte_list_tests schema_byte_list_tests.cpp)
add_executable(psio_schema_builder_identity_test schema_builder_identity_main.cpp schema_builder_identity_other.cpp)
target_include_directories(psio_schema_builder_identity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_schema_builder_identity_test PRIVATE cxx_std_23)
target_link_libraries(psio_schema_builder_identity_test PRIVATE Boost::headers)
target_compile_definitions(psio_schema_builder_identity_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_schema_builder_identity_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_schema_builder_identity_test COMMAND psio_schema_builder_identity_test)

# emit_wit (emit_wit.hpp) — Schema IR → WIT text emitter.
psio_add_test(psio_emit_wit_tests emit_wit_tests.cpp)
# The WIT text of the interface fixture's schema, pinned (the reference for
# the other implementations' WIT emitters), with its known defects.
psio_add_test(psio_interface_wit_tests interface_wit_tests.cpp)

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

# psch — pSSZ schema binary (writer + view). Round-trip + lookup.
# Inherits the vendored xxhash include directory from the `psio`
# target's PUBLIC interface.
psio_add_test(psio_psch_tests psch_tests.cpp)

psio_add_test(psio_wit_owned_tests wit_owned_tests.cpp)

# psio::key: tuples, composite-key helpers (key_prefix / key_prefix_upper_bound),
# the order + prefix-free contract of every element kind, and checked reading.
psio_add_test(psio_key_tests key_tests.cpp)
# Types with no key form, and ill-typed prefixes, must fail with a clear message.
function(psio_key_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_key_${NAME}_compile_fail
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -D${DEFINE}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/key_unsupported_fail.cpp)
   set_tests_properties(psio_key_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
psio_key_compile_fail(multimap KEY_FAIL_MULTIMAP "are not PSIO values")
psio_key_compile_fail(bitlist KEY_FAIL_BITLIST "bitlist has no key encoding")
psio_key_compile_fail(steady_clock KEY_FAIL_STEADY_CLOCK "no matching function for call to 'to_rfc3339'")
psio_key_compile_fail(float_rep KEY_FAIL_FLOAT_REP "needs an integer Rep")
psio_key_compile_fail(prefix_arity KEY_FAIL_PREFIX_ARITY "more elements than the key type has")
psio_key_compile_fail(prefix_type KEY_FAIL_PREFIX_TYPE "does not convert to the key's")

# Time values (std::chrono durations / system_clock time points) in every
# format: tick count in binary and key, RFC 3339 text, schema time ids.
psio_add_test(psio_time_tests time_tests.cpp)
add_executable(psio_time_noexceptions_test time_noexceptions.cpp)
target_include_directories(psio_time_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_time_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_time_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_time_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_time_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_time_noexceptions_test COMMAND psio_time_noexceptions_test)

# Shared cross-language time / key fixture (conformance/time-key-vectors.json):
# emitted here, consumed by ctest, cargo test
# (rust/psio/tests/time_key_conformance.rs) and the JS tests
# (js/src/test/time-key-conformance.test.ts).
set(PSIO_TIME_KEY_VECTORS ${CMAKE_SOURCE_DIR}/conformance/time-key-vectors.json)
add_executable(psio_time_key_conformance_emit time_key_conformance_emit.cpp)
target_link_libraries(psio_time_key_conformance_emit PRIVATE psio)
target_compile_features(psio_time_key_conformance_emit PRIVATE cxx_std_23)
add_test(NAME psio_time_key_conformance_regen
         COMMAND psio_time_key_conformance_emit --check ${PSIO_TIME_KEY_VECTORS})
psio_add_test(psio_time_key_conformance_tests time_key_conformance_tests.cpp)
target_compile_definitions(psio_time_key_conformance_tests PRIVATE
   PSIO_TIME_KEY_VECTORS="${PSIO_TIME_KEY_VECTORS}")

# Shared cross-language Schema IR text fixture
# (conformance/schema-json-vectors.json): psio::Schema as PSSZ bytes, json
# and ijson. Emitted here, consumed by ctest, cargo test
# (rust/psio/tests/schema_json_conformance.rs) and the JS tests
# (js/src/test/schema-json-conformance.test.ts).
set(PSIO_SCHEMA_JSON_VECTORS ${CMAKE_SOURCE_DIR}/conformance/schema-json-vectors.json)
add_executable(psio_schema_json_conformance_emit schema_json_conformance_emit.cpp)
target_link_libraries(psio_schema_json_conformance_emit PRIVATE psio)
target_compile_features(psio_schema_json_conformance_emit PRIVATE cxx_std_23)
add_test(NAME psio_schema_json_conformance_regen
         COMMAND psio_schema_json_conformance_emit --check ${PSIO_SCHEMA_JSON_VECTORS})
psio_add_test(psio_schema_json_conformance_tests schema_json_conformance_tests.cpp)
target_compile_definitions(psio_schema_json_conformance_tests PRIVATE
   PSIO_SCHEMA_JSON_VECTORS="${PSIO_SCHEMA_JSON_VECTORS}")
# psio::Schema through psio::json / psio::ijson with exceptions and RTTI off.
add_executable(psio_schema_json_noexceptions_test schema_json_noexceptions.cpp)
target_include_directories(psio_schema_json_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_schema_json_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_schema_json_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_schema_json_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_schema_json_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_schema_json_noexceptions_test COMMAND psio_schema_json_noexceptions_test)

# Key / time and schema fidelity together: time ids as built-in custom
# presentations, enums and raw / UTF-8 strings in keys, member bounds in keys.
psio_add_test(psio_key_time_schema_tests key_time_schema_tests.cpp)

# Annotations declared inside PSIO_REFLECT, and with the separate statements,
# for types in nested namespaces (in-class and namespace-scope forms).
psio_add_test(psio_annotate_namespace_tests annotate_namespace_tests.cpp)
# One attribute declared twice, or declared after its first use, must not
# compile. The file compiles without any ANN_FAIL_* (control).
function(psio_annotate_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_annotate_${NAME}_compile_fail
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -D${DEFINE}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/annotate_conflict_fail.cpp)
   set_tests_properties(psio_annotate_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
add_test(NAME psio_annotate_conflict_control_compiles
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only
                 -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                 ${CMAKE_CURRENT_SOURCE_DIR}/annotate_conflict_fail.cpp)
psio_annotate_compile_fail(reflect_and_field_attrs ANN_FAIL_REFLECT_AND_FIELD_ATTRS
   "an annotation is declared twice")
psio_annotate_compile_fail(reflect_and_type_attrs ANN_FAIL_REFLECT_AND_TYPE_ATTRS
   "an annotation is declared twice")
psio_annotate_compile_fail(field_attrs_twice ANN_FAIL_FIELD_ATTRS_TWICE
   "redefinition of 'psio_annotation_lookup'")
psio_annotate_compile_fail(type_attrs_twice ANN_FAIL_TYPE_ATTRS_TWICE
   "redefinition of 'psio_annotation_lookup'")
psio_annotate_compile_fail(type_keyword_twice ANN_FAIL_TYPE_KEYWORD_TWICE
   "lists a type-level attribute twice")
psio_annotate_compile_fail(field_attrs_after_use ANN_FAIL_FIELD_ATTRS_AFTER_USE
   "comes after the first use of the field's annotations")
psio_annotate_compile_fail(type_attrs_after_use ANN_FAIL_TYPE_ATTRS_AFTER_USE
   "comes after the first use of the type's annotations")

# Every *_checked / status-returning entry point reports through its status:
# with exceptions enabled nothing is thrown, with them disabled nothing aborts.
psio_add_test(psio_checked_never_throws_tests checked_never_throws_tests.cpp)
add_executable(psio_checked_noexceptions_test checked_noexceptions.cpp)
target_include_directories(psio_checked_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_checked_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_checked_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_checked_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_checked_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_checked_noexceptions_test COMMAND psio_checked_noexceptions_test)

# psio::graphql (docs/graphql.md): parse, validate, plan, execute over values
# and PSSZ views; introspection and SDL; connections; limits; seeded fuzz.
psio_add_test(psio_graphql_tests graphql_tests.cpp)
# The same engine with exceptions and RTTI disabled (how a WebAssembly guest
# builds it): every failure is an error in the response, nothing aborts.
add_executable(psio_graphql_noexceptions_test graphql_noexceptions.cpp)
target_include_directories(psio_graphql_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_graphql_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_graphql_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_graphql_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_graphql_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_graphql_noexceptions_test COMMAND psio_graphql_noexceptions_test)
# Schemas that must not compile, each with a message naming the reason.
function(psio_graphql_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_graphql_${NAME}_compile_fail
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -D${DEFINE}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/graphql_schema_fail.cpp)
   set_tests_properties(psio_graphql_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
psio_graphql_compile_fail(type_name_conflict GQL_FAIL_TYPE_NAME "have the same GraphQL type name")
psio_graphql_compile_fail(input_name_conflict GQL_FAIL_INPUT_NAME "have the same GraphQL type name")
psio_graphql_compile_fail(member_name_conflict GQL_FAIL_MEMBER_NAME "have the same GraphQL name")
psio_graphql_compile_fail(unmapped_member GQL_FAIL_UNMAPPED "this type has no GraphQL mapping")
psio_graphql_compile_fail(union_of_scalars GQL_FAIL_UNION "must be a reflected record")
psio_graphql_compile_fail(unnamed_parameter GQL_FAIL_PARAM_NAMES "must name every parameter")
psio_graphql_compile_fail(unsupported_argument GQL_FAIL_ARGUMENT "an argument must be a scalar")
psio_graphql_compile_fail(nested_optional GQL_FAIL_OPTIONAL "an optional of an optional has no GraphQL type")
psio_graphql_compile_fail(id_on_object GQL_FAIL_ID "psio::graphql::id annotates a scalar member")
psio_graphql_compile_fail(empty_object GQL_FAIL_EMPTY "needs at least one visible field")

# Consumer warning hygiene: every public header, with the core codecs
# instantiated, compiles under -Wall -Wextra -Werror with PSIO's include
# directory as a plain -I (not SYSTEM), which is how a consumer that builds
# PSIO in-tree sees it. Third-party headers (Boost, simdjson) are SYSTEM here
# so only PSIO's own code is held to the bar.
file(GLOB PSIO_PUBLIC_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/../include
     CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../include/psio/*.hpp)
list(SORT PSIO_PUBLIC_HEADERS)
# json_simdjson.hpp is json.hpp's simdjson layer: json.hpp includes it when
# PSIO_HAVE_SIMDJSON is 1, and it is not a header to include without simdjson.
list(REMOVE_ITEM PSIO_PUBLIC_HEADERS psio/json_simdjson.hpp)
set(PSIO_ALL_HEADERS_TEXT "// Generated by cpp/tests/CMakeLists.txt: every public PSIO header.\n")
foreach(header ${PSIO_PUBLIC_HEADERS})
   string(APPEND PSIO_ALL_HEADERS_TEXT "#include <${header}>\n")
endforeach()
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/warnings/psio_all_headers.hpp
     CONTENT "${PSIO_ALL_HEADERS_TEXT}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
   add_executable(psio_warnings_check warnings_check.cpp)
   target_compile_features(psio_warnings_check PRIVATE cxx_std_23)
   target_include_directories(psio_warnings_check PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../include
      ${CMAKE_CURRENT_SOURCE_DIR}/../external/xxhash
      ${CMAKE_CURRENT_BINARY_DIR}/warnings)
   target_include_directories(psio_warnings_check SYSTEM PRIVATE ${PSIO_BOOST_INCLUDES})
   get_target_property(PSIO_OWN_DEFINITIONS psio INTERFACE_COMPILE_DEFINITIONS)
   target_compile_definitions(psio_warnings_check PRIVATE ${PSIO_OWN_DEFINITIONS})
   if(PSIO_ENABLE_SIMDJSON)
      get_target_property(PSIO_SIMDJSON_INCLUDES simdjson::simdjson INTERFACE_INCLUDE_DIRECTORIES)
      target_include_directories(psio_warnings_check SYSTEM PRIVATE ${PSIO_SIMDJSON_INCLUDES})
      target_link_libraries(psio_warnings_check PRIVATE simdjson::simdjson)
   endif()
   target_compile_options(psio_warnings_check PRIVATE -Wall -Wextra -Werror)
   add_test(NAME psio_warnings_check COMMAND psio_warnings_check)
endif()

# psio::nested<T> (nested.hpp): a member that is the PSSZ encoding of a T,
# through pssz, key, json / ijson, views, the Schema IR and the schema-driven
# codecs; validation descent, depth across the nesting boundary, hostile
# input.
psio_add_test(psio_nested_tests nested_tests.cpp)
# The same cases with exceptions and RTTI disabled.
add_executable(psio_nested_noexceptions_test nested_noexceptions.cpp)
target_include_directories(psio_nested_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_nested_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_nested_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_nested_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_nested_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_nested_noexceptions_test COMMAND psio_nested_noexceptions_test)
# Uses of nested<T> that must not compile, each with a message naming the
# reason.
function(psio_nested_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_nested_${NAME}_compile_fail
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -D${DEFINE}
                    -DPSIO_EXCEPTIONS_ENABLED=1
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/nested_fail.cpp)
   set_tests_properties(psio_nested_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
psio_nested_compile_fail(cv_qualified NESTED_FAIL_CV "T is a value type")
psio_nested_compile_fail(graphql_deferred NESTED_FAIL_GRAPHQL_DEFERRED
   "this type has no GraphQL mapping")
psio_nested_compile_fail(graphql_argument NESTED_FAIL_GRAPHQL_ARGUMENT
   "an argument must be a scalar")
psio_nested_compile_fail(other_format NESTED_FAIL_OTHER_FORMAT "psio::ssz: .*unsupported type")
# std::pair, maps and sets as values (docs/pssz-spec.md "Pairs, maps and
# sets"): every core format, typed and schema-driven, the canonical order and
# its checks, hostile input.
psio_add_test(psio_assoc_tests assoc_tests.cpp)
# The multi containers, and a map / set whose key type has no key encoding
# (or only an adapter's), must fail with a clear message.
function(psio_assoc_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_assoc_${NAME}_compile_fail
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -D${DEFINE}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/assoc_unsupported_fail.cpp)
   set_tests_properties(psio_assoc_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
add_test(NAME psio_assoc_unsupported_control
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only
                 -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                 ${CMAKE_CURRENT_SOURCE_DIR}/assoc_unsupported_fail.cpp)
psio_assoc_compile_fail(pssz_multimap ASSOC_FAIL_PSSZ_MULTIMAP "psio::pssz: std::multimap / std::multiset")
psio_assoc_compile_fail(pssz_multiset ASSOC_FAIL_PSSZ_MULTISET "psio::pssz: std::multimap / std::multiset")
psio_assoc_compile_fail(pssz_unordered_multimap ASSOC_FAIL_PSSZ_UNORDERED_MULTIMAP "psio::pssz: std::multimap / std::multiset")
psio_assoc_compile_fail(pssz_validate_multiset ASSOC_FAIL_PSSZ_VALIDATE_MULTISET "psio::pssz: std::multimap / std::multiset")
psio_assoc_compile_fail(json_multimap ASSOC_FAIL_JSON_MULTIMAP "psio::json: std::multimap / std::multiset")
psio_assoc_compile_fail(ijson_multiset ASSOC_FAIL_IJSON_MULTISET "psio::json: std::multimap / std::multiset")
psio_assoc_compile_fail(key_multiset ASSOC_FAIL_KEY_MULTISET "psio::key: std::multimap / std::multiset")
psio_assoc_compile_fail(pssz_bitlist_key ASSOC_FAIL_PSSZ_BITLIST_KEY "psio::pssz: the key type of a map")
psio_assoc_compile_fail(pssz_validate_bitlist_set ASSOC_FAIL_PSSZ_VALIDATE_BITLIST_SET "psio::pssz: the key type of a map")
psio_assoc_compile_fail(json_bitlist_key ASSOC_FAIL_JSON_BITLIST_KEY "psio::json: the key type of a map")
psio_assoc_compile_fail(key_nested_key ASSOC_FAIL_KEY_NESTED_KEY "psio::key: the key type of a map")
psio_assoc_compile_fail(pssz_adapter_key ASSOC_FAIL_PSSZ_ADAPTER_KEY "psio::pssz: the key type of a map")
psio_assoc_compile_fail(json_adapter_key ASSOC_FAIL_JSON_ADAPTER_KEY "psio::json: the key type of a map")
# psio::nested<T> with maps and sets: a nested value as a map value, a set
# element and a map key, a map inside nested bytes; typed and schema-driven
# (the exception-disabled run is part of psio_nested_noexceptions_test).
psio_add_test(psio_nested_assoc_tests nested_assoc_tests.cpp)
# psio::mutable_view over pairs, maps and nested values (docs/pssz-spec.md
# §17): mapped values and the inner value of a nested<T> are writable in
# place; sets, map keys and deferred nested values are not (the
# exception-disabled run is part of psio_mutable_view_noexceptions_test).
psio_add_test(psio_mutable_view_assoc_tests mutable_view_assoc_tests.cpp)
add_test(NAME psio_mutable_view_assoc_control_compiles
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only
                 -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                 ${CMAKE_CURRENT_SOURCE_DIR}/mutable_view_assoc_fail.cpp)
function(psio_mutable_view_assoc_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_mutable_view_assoc_${NAME}_compile_fail
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -D${DEFINE}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/mutable_view_assoc_fail.cpp)
   set_tests_properties(psio_mutable_view_assoc_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
set(PSIO_MV_NO_SET "a set has no mutable view")
psio_mutable_view_assoc_compile_fail(set MV_FAIL_SET "${PSIO_MV_NO_SET}")
psio_mutable_view_assoc_compile_fail(set_root MV_FAIL_SET_ROOT "${PSIO_MV_NO_SET}")
psio_mutable_view_assoc_compile_fail(set_value MV_FAIL_SET_VALUE "${PSIO_MV_NO_SET}")
psio_mutable_view_assoc_compile_fail(set_nested MV_FAIL_SET_NESTED "${PSIO_MV_NO_SET}")
psio_mutable_view_assoc_compile_fail(map_key MV_FAIL_MAP_KEY "no member named 'set'")
psio_mutable_view_assoc_compile_fail(map_entry MV_FAIL_MAP_ENTRY "no member named 'get'")
psio_mutable_view_assoc_compile_fail(map_whole MV_FAIL_MAP_WHOLE "${PSIO_MV_FIXED_ONLY}")
psio_mutable_view_assoc_compile_fail(map_value_variable MV_FAIL_MAP_VALUE_VARIABLE
   "${PSIO_MV_FIXED_ONLY}")
psio_mutable_view_assoc_compile_fail(map_value_blocked MV_FAIL_MAP_VALUE_BLOCKED
   "in-place mutation is blocked for this type")
psio_mutable_view_assoc_compile_fail(nested_whole MV_FAIL_NESTED_WHOLE "${PSIO_MV_FIXED_ONLY}")
psio_mutable_view_assoc_compile_fail(nested_deferred MV_FAIL_NESTED_DEFERRED
   "a nested value with deferred validation has no mutable view")
psio_mutable_view_assoc_compile_fail(nested_blocked MV_FAIL_NESTED_BLOCKED
   "in-place mutation is blocked for this type")


# The method surface of reflect<T>: identical for types with and without
# methods; the proxy's argument forwarding and the reflected type of a
# proxied call.
psio_add_test(psio_reflect_method_surface_tests reflect_method_surface_tests.cpp)
# Describing a type whose methods are declared and never defined links: the
# method metadata, the interface schema and a call proxy do not refer to the
# functions. Built without optimisation so nothing is optimised away.
add_executable(psio_reflect_method_metadata_only reflect_method_metadata_only.cpp)
target_link_libraries(psio_reflect_method_metadata_only PRIVATE psio)
target_compile_features(psio_reflect_method_metadata_only PRIVATE cxx_std_23)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_reflect_method_metadata_only PRIVATE -O0)
endif()
add_test(NAME psio_reflect_method_metadata_only COMMAND psio_reflect_method_metadata_only)
# Uses of the method surface that must not compile, each with a message
# naming the reason.
function(psio_reflect_method_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_reflect_method_${NAME}_compile_fail
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -D${DEFINE}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/reflect_method_surface_fail.cpp)
   set_tests_properties(psio_reflect_method_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
set(PSIO_METHOD_INDEX_MESSAGE "psio: method index out of range")
psio_reflect_method_compile_fail(empty_param_names METHOD_FAIL_EMPTY_PARAM_NAMES "${PSIO_METHOD_INDEX_MESSAGE}")
psio_reflect_method_compile_fail(empty_type METHOD_FAIL_EMPTY_TYPE "${PSIO_METHOD_INDEX_MESSAGE}")
psio_reflect_method_compile_fail(empty_result METHOD_FAIL_EMPTY_RESULT "${PSIO_METHOD_INDEX_MESSAGE}")
psio_reflect_method_compile_fail(empty_name METHOD_FAIL_EMPTY_NAME "${PSIO_METHOD_INDEX_MESSAGE}")
psio_reflect_method_compile_fail(empty_pointer METHOD_FAIL_EMPTY_POINTER "${PSIO_METHOD_INDEX_MESSAGE}")
psio_reflect_method_compile_fail(empty_is_const METHOD_FAIL_EMPTY_IS_CONST "${PSIO_METHOD_INDEX_MESSAGE}")
psio_reflect_method_compile_fail(empty_invoke METHOD_FAIL_EMPTY_INVOKE "${PSIO_METHOD_INDEX_MESSAGE}")
psio_reflect_method_compile_fail(past_end METHOD_FAIL_PAST_END "${PSIO_METHOD_INDEX_MESSAGE}")
psio_reflect_method_compile_fail(walk_pointer METHOD_FAIL_WALK_POINTER
   "for_each_method calls fn\\(std::integral_constant<std::size_t, I>, name\\)")
psio_reflect_method_compile_fail(proxy_inherited METHOD_FAIL_PROXY_INHERITED
   "the member pointer is not method I of the reflection of its class")
psio_reflect_method_compile_fail(proxy_static METHOD_FAIL_PROXY_STATIC
   "a static method's pointer names no class")
# ── Transparent types, custom types of any shape, invariants ──────────────
# docs/pssz-spec.md "Transparent types" / "Type invariants", docs/json.md
# "Custom types through a schema"; the fixture types are
# custom_conformance_types.hpp (conformance/custom-types.md).
psio_add_test(psio_custom_types_tests custom_types_tests.cpp)

# The same with exceptions disabled: every checked path reports a violated
# invariant through its status.
add_executable(psio_custom_noexceptions_test custom_noexceptions.cpp)
target_include_directories(psio_custom_noexceptions_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(psio_custom_noexceptions_test PRIVATE cxx_std_23)
target_link_libraries(psio_custom_noexceptions_test PRIVATE Boost::headers)
target_compile_definitions(psio_custom_noexceptions_test PRIVATE PSIO_EXCEPTIONS_ENABLED=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
   target_compile_options(psio_custom_noexceptions_test PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME psio_custom_noexceptions_test COMMAND psio_custom_noexceptions_test)

# Misuse that must not compile, each with a message naming the reason. The
# file compiles without any CT_FAIL_* (control).
function(psio_custom_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_custom_${NAME}_compile_fail
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -D${DEFINE}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/custom_types_fail.cpp)
   set_tests_properties(psio_custom_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
add_test(NAME psio_custom_types_control_compiles
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only
                 -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                 ${CMAKE_CURRENT_SOURCE_DIR}/custom_types_fail.cpp)
psio_custom_compile_fail(transparent_two_members CT_FAIL_TRANSPARENT_TWO_MEMBERS
   "a transparent\\(\\) type has exactly one data member")
psio_custom_compile_fail(transparent_no_member CT_FAIL_TRANSPARENT_NO_MEMBER
   "a transparent\\(\\) type has exactly one data member")
psio_custom_compile_fail(transparent_dwnc CT_FAIL_TRANSPARENT_DWNC
   "cannot be combined with definitionWillNotChange / maxFields / maxDynamicData")
psio_custom_compile_fail(transparent_max_fields CT_FAIL_TRANSPARENT_MAX_FIELDS
   "cannot be combined with definitionWillNotChange / maxFields / maxDynamicData")
psio_custom_compile_fail(transparent_max_dynamic_data CT_FAIL_TRANSPARENT_MAX_DYNAMIC_DATA
   "cannot be combined with definitionWillNotChange / maxFields / maxDynamicData")
psio_custom_compile_fail(transparent_type_attrs_dwnc CT_FAIL_TRANSPARENT_TYPE_ATTRS_DWNC
   "cannot be combined with definitionWillNotChange / maxFields / maxDynamicData")
psio_custom_compile_fail(transparent_member_attr CT_FAIL_TRANSPARENT_MEMBER_ATTR
   "the member of a transparent\\(\\) type takes only the annotations that make it a bounded type")
psio_custom_compile_fail(transparent_field_attrs CT_FAIL_TRANSPARENT_FIELD_ATTRS
   "the member of a transparent\\(\\) type takes only the annotations that make it a bounded type")
psio_custom_compile_fail(transparent_twice CT_FAIL_TRANSPARENT_TWICE
   "lists a type-level attribute twice")
psio_custom_compile_fail(transparent_and_type_attrs CT_FAIL_TRANSPARENT_AND_TYPE_ATTRS
   "an annotation is declared twice")
psio_custom_compile_fail(transparent_optional CT_FAIL_TRANSPARENT_OPTIONAL
   "the member of a transparent\\(\\) type is not a std::optional")
psio_custom_compile_fail(transparent_length_bound CT_FAIL_TRANSPARENT_LENGTH_BOUND
   "attached to a field whose shape is not in the spec's applies_to set")
psio_custom_compile_fail(transparent_graphql_record CT_FAIL_TRANSPARENT_GRAPHQL_RECORD
   "a transparent type over a record, a list or a variant has no GraphQL mapping")
psio_custom_compile_fail(invariant_twice CT_FAIL_INVARIANT_TWICE
   "declares invariant\\(...\\) twice: a type has one invariant")
psio_custom_compile_fail(invariant_not_noexcept CT_FAIL_INVARIANT_NOT_NOEXCEPT
   "needs fn callable as fn\\(const T&\\), declared noexcept and returning psio::codec_status")
psio_custom_compile_fail(invariant_return_type CT_FAIL_INVARIANT_RETURN_TYPE
   "needs fn callable as fn\\(const T&\\), declared noexcept and returning psio::codec_status")
psio_custom_compile_fail(invariant_argument CT_FAIL_INVARIANT_ARGUMENT
   "needs fn callable as fn\\(const T&\\), declared noexcept and returning psio::codec_status")
psio_custom_compile_fail(invariant_reflect_and_macro CT_FAIL_INVARIANT_REFLECT_AND_MACRO
   "the invariant of this type is declared twice")
psio_custom_compile_fail(invariant_macro_twice CT_FAIL_INVARIANT_MACRO_TWICE
   "redefinition of 'invariant_of<")
psio_custom_compile_fail(invariant_macro_not_noexcept CT_FAIL_INVARIANT_MACRO_NOT_NOEXCEPT
   "needs fn callable as fn\\(const Type&\\), declared noexcept and returning psio::codec_status")
psio_custom_compile_fail(invariant_arithmetic CT_FAIL_INVARIANT_ARITHMETIC
   "PSIO_INVARIANT takes a class or an enum type")
psio_custom_compile_fail(invariant_after_use CT_FAIL_INVARIANT_AFTER_USE
   "explicit specialization of 'psio::invariant_of<")
psio_custom_compile_fail(invariant_no_schema_id CT_FAIL_INVARIANT_NO_SCHEMA_ID
   "needs a schema id")
psio_custom_compile_fail(text_no_schema_id CT_FAIL_TEXT_NO_SCHEMA_ID
   "needs a schema id")
# Invariants (psio/invariant.hpp) block in-place mutation: a leaf inside any
# type with an invariant is not writable, wherever the type sits.
set(PSIO_MV_BLOCKED "in-place mutation is blocked for this type")
psio_mutable_view_assoc_compile_fail(invariant_leaf MV_FAIL_INVARIANT_LEAF "${PSIO_MV_BLOCKED}")
psio_mutable_view_assoc_compile_fail(invariant_whole_type MV_FAIL_INVARIANT_WHOLE_TYPE
   "${PSIO_MV_BLOCKED}")
psio_mutable_view_assoc_compile_fail(invariant_transparent MV_FAIL_INVARIANT_TRANSPARENT
   "${PSIO_MV_BLOCKED}")
psio_mutable_view_assoc_compile_fail(invariant_map_value MV_FAIL_INVARIANT_MAP_VALUE
   "${PSIO_MV_BLOCKED}")
psio_mutable_view_assoc_compile_fail(invariant_nested MV_FAIL_INVARIANT_NESTED
   "${PSIO_MV_BLOCKED}")
psio_mutable_view_assoc_compile_fail(invariant_list_element MV_FAIL_INVARIANT_LIST_ELEMENT
   "${PSIO_MV_BLOCKED}")
psio_mutable_view_assoc_compile_fail(invariant_contained MV_FAIL_INVARIANT_CONTAINED
   "the value contains a type whose in-place mutation is blocked")

# Transparent types, custom text forms and invariants with maps, sets, pairs
# and nested values: invariants on keys / values / elements and inside
# nested bytes, transparent wrappers of containers, text-keyed maps; typed
# and schema-driven (the exception-disabled run is part of
# psio_custom_noexceptions_test).
psio_add_test(psio_custom_assoc_tests custom_assoc_tests.cpp)

# The cross-language consistency round: one section per rule (merged length
# bounds, key writer checks, reserved custom ids, ...).
psio_add_test(psio_consistency_tests consistency_tests.cpp)
add_test(NAME psio_consistency_control_compiles
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only
                 -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                 ${CMAKE_CURRENT_SOURCE_DIR}/consistency_fail.cpp)
function(psio_consistency_compile_fail NAME DEFINE MESSAGE)
   add_test(NAME psio_consistency_${NAME}_compile_fail
            COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -D${DEFINE}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/../include -I${PSIO_BOOST_INCLUDES}
                    ${CMAKE_CURRENT_SOURCE_DIR}/consistency_fail.cpp)
   set_tests_properties(psio_consistency_${NAME}_compile_fail PROPERTIES
      PASS_REGULAR_EXPRESSION "${MESSAGE}")
endfunction()
set(PSIO_CONS_BOUNDS "contradictory length bounds on one member or position")
psio_consistency_compile_fail(max_twice CONS_FAIL_MAX_TWICE "${PSIO_CONS_BOUNDS}")
psio_consistency_compile_fail(min_above_max CONS_FAIL_MIN_ABOVE_MAX "${PSIO_CONS_BOUNDS}")

# Shared vectors of the two name coders (conformance/name-vectors.json):
# psio/name.hpp identifier names and psio/compress_name.hpp method names.
set(PSIO_NAME_VECTORS ${CMAKE_SOURCE_DIR}/conformance/name-vectors.json)
add_executable(psio_name_conformance_emit name_conformance_emit.cpp)
target_link_libraries(psio_name_conformance_emit PRIVATE psio)
target_compile_features(psio_name_conformance_emit PRIVATE cxx_std_23)
add_test(NAME psio_name_conformance_regen
         COMMAND psio_name_conformance_emit --check ${PSIO_NAME_VECTORS})
set(PSIO_CONS_RESERVED "this custom type's schema id is reserved for one of PSIO's own types")
psio_consistency_compile_fail(reserved_invariant_id CONS_FAIL_RESERVED_INVARIANT_ID
   "${PSIO_CONS_RESERVED}")
psio_consistency_compile_fail(reserved_reflected_name CONS_FAIL_RESERVED_REFLECTED_NAME
   "${PSIO_CONS_RESERVED}")
psio_consistency_compile_fail(fields_over_cap CONS_FAIL_FIELDS_OVER_CAP
   "the record has more fields than its maxFields\\(N\\) allows")
psio_consistency_compile_fail(fields_cap_over_255 CONS_FAIL_FIELDS_CAP_OVER_255
   "maxFields\\(N\\): N is at most 255")
psio_consistency_compile_fail(params_over_cap CONS_FAIL_PARAMS_OVER_CAP
   "the method has more parameters than its maxFields\\(N\\) allows")
set(PSIO_CONS_TRANSPARENT
   "the member of a transparent\\(\\) type takes only the annotations that make it a bounded type")
psio_consistency_compile_fail(transparent_exact CONS_FAIL_TRANSPARENT_EXACT "${PSIO_CONS_TRANSPARENT}")
psio_consistency_compile_fail(transparent_min CONS_FAIL_TRANSPARENT_MIN "${PSIO_CONS_TRANSPARENT}")
psio_consistency_compile_fail(transparent_text_form CONS_FAIL_TRANSPARENT_SORTED
   "${PSIO_CONS_TRANSPARENT}")
psio_consistency_compile_fail(transparent_max_on_int CONS_FAIL_TRANSPARENT_MAX_ON_INT
   "${PSIO_CONS_TRANSPARENT}")

# Shared cross-language interface fixture (conformance/interface-vectors.json):
# the Interface / Func IR of a method declaration, and the bytes and text of
# its argument lists and results.
set(PSIO_INTERFACE_VECTORS ${CMAKE_SOURCE_DIR}/conformance/interface-vectors.json)
add_executable(psio_interface_conformance_emit interface_conformance_emit.cpp)
target_link_libraries(psio_interface_conformance_emit PRIVATE psio)
target_compile_features(psio_interface_conformance_emit PRIVATE cxx_std_23)
add_test(NAME psio_interface_conformance_regen
         COMMAND psio_interface_conformance_emit --check ${PSIO_INTERFACE_VECTORS})
