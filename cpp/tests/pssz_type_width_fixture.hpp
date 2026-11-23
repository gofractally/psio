#pragma once
#include <psio/pssz.hpp>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Concrete relay/action-data records; tests use the real typed codec and views.
struct WidthCheckpoint
{
   std::uint32_t height;
   std::string label;
};
PSIO_REFLECT(WidthCheckpoint, height, label, maxDynamicData(64))
PSIO_FIELD_ATTRS(WidthCheckpoint, label, psio::length_bound{.max=16})

struct WidthRelayBatch
{
   WidthCheckpoint checkpoint;
   std::vector<WidthCheckpoint> entries;
   std::optional<WidthCheckpoint> latest;
   std::array<WidthCheckpoint, 2> pair;
   std::variant<WidthCheckpoint, std::uint32_t> proof;
   std::string context;
};
PSIO_REFLECT(WidthRelayBatch, checkpoint, entries, latest, pair, proof, context)

inline WidthRelayBatch width_sample()
{
   return {{7,"main"}, {{8,"one"},{9,"two"}}, WidthCheckpoint{10,"last"},
           {{{11,"left"},{12,"right"}}}, WidthCheckpoint{13,"proof"}, "relay"};
}

struct WidthLargeBudget { std::uint8_t value; };
PSIO_REFLECT(WidthLargeBudget, value, maxDynamicData(1000000))
struct WidthNarrowEnvelope { WidthLargeBudget child; std::uint32_t sequence; };
PSIO_REFLECT(WidthNarrowEnvelope, child, sequence, maxDynamicData(64))
struct WidthReserveParent { WidthLargeBudget child; };
PSIO_REFLECT(WidthReserveParent, child)

struct WidthGrowV1 { std::uint32_t id; };
PSIO_REFLECT(WidthGrowV1, id, maxDynamicData(512))
struct WidthGrowV2 { std::uint32_t id; std::optional<std::uint32_t> time; };
PSIO_REFLECT(WidthGrowV2, id, time, maxDynamicData(512))
struct WidthGrowParentV1 { WidthGrowV1 child; };
PSIO_REFLECT(WidthGrowParentV1, child)
struct WidthGrowParentV2 { WidthGrowV2 child; };
PSIO_REFLECT(WidthGrowParentV2, child)

struct WidthThreshold251 { std::array<std::uint8_t,251> value; };
PSIO_REFLECT(WidthThreshold251, value)
struct WidthThreshold252 { std::array<std::uint8_t,252> value; };
PSIO_REFLECT(WidthThreshold252, value)
// The header counts at the record's own width: 1 + 254 = 255 is the last
// one-byte record, 2 + 255 = 257 the first two-byte one.
struct WidthThreshold254 { std::array<std::uint8_t,254> value; };
PSIO_REFLECT(WidthThreshold254, value)
struct WidthThreshold255 { std::array<std::uint8_t,255> value; };
PSIO_REFLECT(WidthThreshold255, value)
struct WidthBoundedBatch { std::vector<WidthCheckpoint> entries; };
PSIO_REFLECT(WidthBoundedBatch, entries)
PSIO_FIELD_ATTRS(WidthBoundedBatch, entries, psio::length_bound{.max=4})

struct WidthStringPair { std::string first; std::string second; };
PSIO_REFLECT(WidthStringPair, first, second)
struct WidthCappedPair { std::string first; std::string second; };
PSIO_REFLECT(WidthCappedPair, first, second, maxDynamicData(255))

struct WidthBadFixed { std::uint64_t value; };
PSIO_REFLECT(WidthBadFixed, value, definitionWillNotChange(), maxDynamicData(4))
struct WidthBadFixedParent { WidthBadFixed fixed; std::string tail; };
PSIO_REFLECT(WidthBadFixedParent, fixed, tail)

struct WidthHugeBudget { std::uint8_t value; };
PSIO_REFLECT(WidthHugeBudget, value, maxDynamicData(std::numeric_limits<std::size_t>::max()))
struct WidthOverflowParent { WidthHugeBudget child; };
PSIO_REFLECT(WidthOverflowParent, child)
