#pragma once
// Generated from the actual psibase SDK at 4d7097848da7 using legacy-oracle.cpp.
// Signed rows use psibase::SignedTransaction, shared_view_ptr<Transaction>,
// AccountNumber, MethodNumber and int64 TimePointSec from the real SDK headers.
#include <psio/annotate.hpp>
#include <psio/reflect.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <string_view>
namespace frac_legacy_fixtures {
struct row { std::string_view name; std::string_view bytes; };
inline constexpr std::array data = {
    row{"zero", std::string_view{"\x04\x00\x04\x00\x00\x00\x05\x00\x00\x00\x68\x65\x6c\x6c\x6f", 15}},
    row{"one-none", std::string_view{"\x04\x00\x04\x00\x00\x00\x05\x00\x00\x00\x68\x65\x6c\x6c\x6f", 15}},
    row{"one-empty", std::string_view{"\x08\x00\x08\x00\x00\x00\x00\x00\x00\x00\x05\x00\x00\x00\x68\x65\x6c\x6c\x6f", 19}},
    row{"one-some", std::string_view{"\x08\x00\x08\x00\x00\x00\x0d\x00\x00\x00\x05\x00\x00\x00\x68\x65\x6c\x6c\x6f\x04\x00\x00\x00\x6d\x65\x6d\x6f", 27}},
    row{"two-none", std::string_view{"\x04\x00\x04\x00\x00\x00\x05\x00\x00\x00\x68\x65\x6c\x6c\x6f", 15}},
    row{"two-first-some", std::string_view{"\x08\x00\x08\x00\x00\x00\x0d\x00\x00\x00\x05\x00\x00\x00\x68\x65\x6c\x6c\x6f\x04\x00\x00\x00\x6d\x65\x6d\x6f", 27}},
    row{"two-last-some", std::string_view{"\x0c\x00\x0c\x00\x00\x00\x01\x00\x00\x00\x0d\x00\x00\x00\x05\x00\x00\x00\x68\x65\x6c\x6c\x6f\x09\x00\x00\x00", 27}},
    row{"two-both-some", std::string_view{"\x0c\x00\x0c\x00\x00\x00\x11\x00\x00\x00\x15\x00\x00\x00\x05\x00\x00\x00\x68\x65\x6c\x6c\x6f\x04\x00\x00\x00\x6d\x65\x6d\x6f\x09\x00\x00\x00", 35}},
    row{"interior-none", std::string_view{"\x0c\x00\x0c\x00\x00\x00\x01\x00\x00\x00\x09\x00\x00\x00\x05\x00\x00\x00\x68\x65\x6c\x6c\x6f", 23}},
    row{"all-none", std::string_view{"\x00\x00", 2}},
    row{"all-last-some", std::string_view{"\x08\x00\x01\x00\x00\x00\x04\x00\x00\x00\x09\x00\x00\x00", 14}},
    row{"frozen-none", std::string_view{"\x01\x00\x00\x00", 4}},
    row{"transaction", std::string_view{"\x17\x00\x7b\x00\x00\x00\x00\x00\x00\x00\x04\x03\x02\x01\x01\x00\x05\x08\x00\x00\x00\x34\x00\x00\x00\x04\x00\x00\x00\x04\x00\x00\x00\x1c\x00\x01\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x06\x00\x00\x00\x04\x00\x07\x00\x00\x00\x04\x00\x00\x00\x04\x00\x00\x00\x0c\x00\x04\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x02\x00\x00\x00\x61\x62", 101}},
    row{"signed-none", std::string_view{"\x08\x00\x08\x00\x00\x00\x6d\x00\x00\x00\x65\x00\x00\x00\x17\x00\x7b\x00\x00\x00\x00\x00\x00\x00\x04\x03\x02\x01\x01\x00\x05\x08\x00\x00\x00\x34\x00\x00\x00\x04\x00\x00\x00\x04\x00\x00\x00\x1c\x00\x01\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x06\x00\x00\x00\x04\x00\x07\x00\x00\x00\x04\x00\x00\x00\x04\x00\x00\x00\x0c\x00\x04\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x02\x00\x00\x00\x61\x62\x04\x00\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00\x70", 128}},
    row{"signed-empty", std::string_view{"\x0c\x00\x0c\x00\x00\x00\x71\x00\x00\x00\x00\x00\x00\x00\x65\x00\x00\x00\x17\x00\x7b\x00\x00\x00\x00\x00\x00\x00\x04\x03\x02\x01\x01\x00\x05\x08\x00\x00\x00\x34\x00\x00\x00\x04\x00\x00\x00\x04\x00\x00\x00\x1c\x00\x01\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x06\x00\x00\x00\x04\x00\x07\x00\x00\x00\x04\x00\x00\x00\x04\x00\x00\x00\x0c\x00\x04\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x02\x00\x00\x00\x61\x62\x04\x00\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00\x70", 132}},
    row{"signed-some", std::string_view{"\x0c\x00\x0c\x00\x00\x00\x71\x00\x00\x00\x7a\x00\x00\x00\x65\x00\x00\x00\x17\x00\x7b\x00\x00\x00\x00\x00\x00\x00\x04\x03\x02\x01\x01\x00\x05\x08\x00\x00\x00\x34\x00\x00\x00\x04\x00\x00\x00\x04\x00\x00\x00\x1c\x00\x01\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x06\x00\x00\x00\x04\x00\x07\x00\x00\x00\x04\x00\x00\x00\x04\x00\x00\x00\x0c\x00\x04\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x02\x00\x00\x00\x61\x62\x04\x00\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00\x70\x04\x00\x00\x00\x04\x00\x00\x00\x03\x00\x00\x00\x73\x75\x62", 147}},
    row{"fixed-nested", std::string_view{"\x08\x00\x08\x00\x00\x00\x0a\x00\x00\x00\x04\x00\x07\x00\x00\x00\x07\x00\x00\x00\x73\x69\x62\x6c\x69\x6e\x67", 27}},
    row{"fixed-vector", std::string_view{"\x08\x00\x00\x00\x08\x00\x00\x00\x0a\x00\x00\x00\x04\x00\x07\x00\x00\x00\x04\x00\x09\x00\x00\x00", 24}},
    row{"fixed-frozen-parent", std::string_view{"\x08\x00\x00\x00\x09\x00\x00\x00\x04\x00\x07\x00\x00\x00", 14}},
    row{"fixed-extended-nested", std::string_view{"\x08\x00\x08\x00\x00\x00\x17\x00\x00\x00\x08\x00\x07\x00\x00\x00\x04\x00\x00\x00\x05\x00\x00\x00\x65\x78\x74\x72\x61\x07\x00\x00\x00\x73\x69\x62\x6c\x69\x6e\x67", 40}},
};
}

struct CompatBase { std::string text; bool operator==(const CompatBase&) const = default; };
PSIO_REFLECT(CompatBase, text)
struct CompatOne { std::string text; std::optional<std::string> memo; bool operator==(const CompatOne&) const = default; };
PSIO_REFLECT(CompatOne, text, memo)
struct CompatMany { std::string text; std::optional<std::string> memo; std::optional<std::uint32_t> count; bool operator==(const CompatMany&) const = default; };
PSIO_REFLECT(CompatMany, text, memo, count)
struct CompatInterior { std::string text; std::optional<std::string> memo; std::uint32_t tail; bool operator==(const CompatInterior&) const = default; };
PSIO_REFLECT(CompatInterior, text, memo, tail)
struct CompatAllOptional { std::optional<std::string> memo; std::optional<std::uint32_t> count; bool operator==(const CompatAllOptional&) const = default; };
PSIO_REFLECT(CompatAllOptional, memo, count)
struct CompatFrozen { std::optional<std::string> memo; bool operator==(const CompatFrozen&) const = default; };
PSIO_REFLECT(CompatFrozen, definitionWillNotChange(), memo)

// Materialized wire projections. Golden rows come from the genuine old SDK
// types, including the nested transaction's u32 byte-length prefix. A caller
// must validate that inner byte sequence as Transaction as well as its envelope.
struct CompatAction { std::uint64_t sender, service, method; std::vector<char> rawData; bool operator==(const CompatAction&) const = default; };
PSIO_REFLECT(CompatAction, sender, service, method, rawData)
struct CompatClaim { std::uint64_t service; std::vector<char> rawData; bool operator==(const CompatClaim&) const = default; };
PSIO_REFLECT(CompatClaim, service, rawData)
struct CompatTapos { std::int64_t expiration; std::uint32_t refBlockSuffix; std::uint16_t flags; std::uint8_t refBlockIndex; bool operator==(const CompatTapos&) const = default; };
PSIO_REFLECT(CompatTapos, definitionWillNotChange(), expiration, refBlockSuffix, flags, refBlockIndex)
struct CompatTransaction { CompatTapos tapos; std::vector<CompatAction> actions; std::vector<CompatClaim> claims; bool operator==(const CompatTransaction&) const = default; };
PSIO_REFLECT(CompatTransaction, tapos, actions, claims)
struct CompatSignedTransaction { std::vector<char> transaction; std::vector<std::vector<char>> proofs; std::optional<std::vector<std::vector<char>>> subjectiveData; bool operator==(const CompatSignedTransaction&) const = default; };
PSIO_REFLECT(CompatSignedTransaction, transaction, proofs, subjectiveData)
struct CompatOuterWriter { CompatMany child; std::string sibling; };
PSIO_REFLECT(CompatOuterWriter, child, sibling)
struct CompatOuterReader { CompatBase child; std::string sibling; };
PSIO_REFLECT(CompatOuterReader, child, sibling)


struct CompatFixed { std::uint32_t id; bool operator==(const CompatFixed&) const = default; };
PSIO_REFLECT(CompatFixed, id)
struct CompatFixedOuter { CompatFixed child; std::string sibling; bool operator==(const CompatFixedOuter&) const = default; };
PSIO_REFLECT(CompatFixedOuter, child, sibling)
struct CompatFixedFrozenOuter { CompatFixed child; std::uint32_t tail; bool operator==(const CompatFixedFrozenOuter&) const = default; };
PSIO_REFLECT(CompatFixedFrozenOuter, definitionWillNotChange(), child, tail)
struct CompatFixedExtended { std::uint32_t id; std::optional<std::string> extra; bool operator==(const CompatFixedExtended&) const = default; };
PSIO_REFLECT(CompatFixedExtended, id, extra)
struct CompatFixedOuterWriter { CompatFixedExtended child; std::string sibling; bool operator==(const CompatFixedOuterWriter&) const = default; };
PSIO_REFLECT(CompatFixedOuterWriter, child, sibling)
