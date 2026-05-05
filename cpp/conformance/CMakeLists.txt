# pjson v1 spec conformance driver.
#
# Mirror of rust/psio/src/bin/pjson_conformance_driver.rs. Reads a
# fixture JSON on stdin and runs the wire format encode/decode/JSON
# pass. The two drivers consume the same fixture files and must
# produce byte-identical results.
#
# Self-contained: no dependency on the pre-audit pjson library code in
# include/psio/pjson*.hpp. Once Phase 5+ stabilizes the spec, the
# library kernel will be migrated to use this driver's logic.

add_executable(pjson_conformance_driver
   pjson_conformance_driver.cpp)

target_compile_features(pjson_conformance_driver PRIVATE cxx_std_23)
target_compile_options(pjson_conformance_driver PRIVATE
   -Wall -Wextra -Werror=unused-result)

# Vendored externals.
#   softfloat — binary128 → binary64 widening (single-header).
#   xxhash    — XXH3_64 for the §5.3 object-key prefilter hash.
target_include_directories(pjson_conformance_driver PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}/../external/softfloat
   ${CMAKE_CURRENT_SOURCE_DIR}/../external/xxhash)
