// psio::nested<T> together with maps and sets: a nested value as the mapped
// value of a map, as the element of a set and as the key of a map; a map
// inside nested bytes. Typed (pssz, key, json / ijson) and schema-driven
// (validation, the dynamic value, text, the key encoder).
//
// Shared, like nested_cases.hpp, by a Catch test and by the executable built
// with exceptions and RTTI disabled: no case here may throw or abort.
//
// run() returns the number of checks that did not hold (each is printed).

#pragma once

#include "nested_cases.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

namespace nested_assoc_cases
{
   using namespace nested_cases;

   // psio::nested has == and no <: an ordered container of nested values
   // names its comparator. Any strict weak order does; the codec orders the
   // entries by key bytes whatever the container's own order is.
   struct BytesLess
   {
      template <typename N>
      bool operator()(const N& a, const N& b) const noexcept
      {
         const auto x = a.bytes();
         const auto y = b.bytes();
         return std::lexicographical_compare(
            x.begin(), x.end(), y.begin(), y.end(),
            [](char p, char q) { return static_cast<unsigned char>(p) < static_cast<unsigned char>(q); });
      }
   };
   // The reverse: a container that does NOT iterate in canonical order.
   struct BytesGreater
   {
      template <typename N>
      bool operator()(const N& a, const N& b) const noexcept { return BytesLess{}(b, a); }
   };

   using NTransfer = psio::nested<Transfer>;
   using NLater    = psio::nested<Transfer, psio::nested_validation::deferred>;
   using NU32      = psio::nested<std::uint32_t>;
   using Octets    = std::vector<std::uint8_t>;

   using ByName      = std::map<std::string, NTransfer>;
   using ByNameLater = std::map<std::string, NLater>;
   using ByNameRaw   = std::vector<std::tuple<std::string, Octets>>;
   using Seen        = std::set<NTransfer, BytesLess>;
   using SeenBack    = std::set<NTransfer, BytesGreater>;
   using SeenRaw     = std::vector<Octets>;
   using ByPacked    = std::map<NU32, std::uint8_t, BytesLess>;
   using ByPackedRaw = std::vector<std::tuple<Octets, std::uint8_t>>;
   using Index       = std::map<std::string, std::set<std::uint16_t>>;
   using IndexRaw    = std::vector<std::tuple<std::string, std::vector<std::uint16_t>>>;

   struct Ledger
   {
      ByName              by_name;
      Seen                seen;
      ByPacked            by_packed;
      psio::nested<Index> index;
      friend bool operator==(const Ledger&, const Ledger&) = default;
      PSIO_REFLECT(Ledger, by_name, seen, by_packed, index)
   };
   struct RawLedger
   {
      ByNameRaw   by_name;
      SeenRaw     seen;
      ByPackedRaw by_packed;
      Octets      index;
      PSIO_REFLECT(RawLedger, by_name, seen, by_packed, index)
   };

   // A map / set is the list it is, a nested value the byte list it is.
   static_assert(psio::pssz_evolves_from<psio::nested<Index>, psio::nested<Index>>);
   static_assert(!psio::inferred_bound<ByName>().has_value());
   static_assert(psio::detail::key_impl::orders_entries_v<NTransfer>);
   static_assert(psio::detail::key_impl::orders_entries_v<std::tuple<std::uint8_t, NLater>>);
   static_assert(!psio::detail::iterates_in_key_order_v<Seen>);

   inline Octets inner_of(const Transfer& value) { return octets(pssz_of(value)); }
   template <typename T>
   Bytes key_of(const T& value)
   {
      const auto key = psio::to_key(value);
      return Bytes(key.begin(), key.end());
   }
   // Sorted the way the wire is: by the key of the byte list.
   inline SeenRaw sorted(SeenRaw raw)
   {
      std::sort(raw.begin(), raw.end(),
                [](const Octets& a, const Octets& b) { return key_of(a) < key_of(b); });
      return raw;
   }

   inline Ledger sample_ledger()
   {
      Ledger ledger;
      ledger.by_name   = {{"rent", NTransfer::from(sample_transfer())},
                          {"gift", NTransfer::from(Transfer{"carol", 1, std::nullopt})}};
      ledger.seen      = {NTransfer::from(Transfer{"z", 0, std::nullopt}),
                          NTransfer::from(Transfer{"a", 256, std::nullopt}),
                          NTransfer::from(sample_transfer())};
      ledger.by_packed = {{NU32::from(1), 7}, {NU32::from(256), 9}, {NU32::from(0), 0}};
      ledger.index     = psio::nested<Index>::from(Index{{"b", {3, 1}}, {"a", {}}});
      return ledger;
   }

   // ── pssz: the bytes are those of the lists of byte lists ────────────────
   inline void pssz_bytes()
   {
      const Ledger ledger = sample_ledger();
      const Bytes  bytes  = pssz_of(ledger);

      RawLedger raw;
      raw.by_name = {{"gift", inner_of(Transfer{"carol", 1, std::nullopt})},
                     {"rent", inner_of(sample_transfer())}};
      raw.seen    = sorted({inner_of(Transfer{"z", 0, std::nullopt}),
                            inner_of(Transfer{"a", 256, std::nullopt}),
                            inner_of(sample_transfer())});
      // Inner bytes order: 0 (00 00 00 00), 256 (00 01 00 00), 1 (01 00 00 00).
      raw.by_packed = {{octets(pssz_of(std::uint32_t{0})), 0},
                       {octets(pssz_of(std::uint32_t{256})), 9},
                       {octets(pssz_of(std::uint32_t{1})), 7}};
      raw.index     = octets(pssz_of(IndexRaw{{"a", {}}, {"b", {1, 3}}}));
      NESTED_CHECK(bytes == pssz_of(raw));
      NESTED_CHECK(psio::size_of(psio::pssz{}, ledger) == bytes.size());

      // Whatever the container's own order.
      SeenBack back;
      for (const auto& item : ledger.seen)
         back.insert(item);
      NESTED_CHECK(pssz_of(back) == pssz_of(ledger.seen));
      NESTED_CHECK(pssz_of(ledger.seen) == pssz_of(raw.seen));

      // Every reader.
      NESTED_CHECK(valid<Ledger>(bytes));
      NESTED_CHECK(strict<Ledger>(bytes));
      bool unknown = true;
      NESTED_CHECK(psio::validate_pssz_compatible<Ledger>(psio::pssz{},
                                                          std::span<const char>{bytes}, unknown)
                      .ok() &&
                   !unknown);
      Ledger back_ledger;
      NESTED_CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{bytes},
                                             back_ledger, unknown)
                      .ok());
      NESTED_CHECK(back_ledger == ledger);
      NESTED_CHECK(psio::decode<Ledger>(psio::pssz{}, std::span<const char>{bytes}) == ledger);
      NESTED_CHECK(psio::decode<SeenBack>(psio::pssz{},
                                          std::span<const char>{pssz_of(ledger.seen)}) == back);

      // An explicit-width frame: the nested bytes stay in the automatic
      // profile.
      Bytes wide;
      NESTED_CHECK(psio::encode_pssz_checked(psio::pssz_<4>{}, ledger, wide).ok());
      NESTED_CHECK(psio::validate<Ledger>(psio::pssz_<4>{}, std::span<const char>{wide}).ok());
      NESTED_CHECK(psio::decode<Ledger>(psio::pssz_<4>{}, std::span<const char>{wide}) == ledger);

      // Views: the entries of the map, the inner value of each.
      const psio::view<Ledger, psio::pssz> view{std::span<const char>{bytes}};
      NESTED_CHECK(view.get<0>().size() == 2);
      NESTED_CHECK(view.get<0>()[1].template get<1>().get().template get<1>() == 5u);
      NESTED_CHECK(view.get<1>().size() == 3);
      NESTED_CHECK(view.get<3>().get().size() == 2);
      NESTED_CHECK(view.get<3>().get()[1].template get<1>().size() == 2);
      NESTED_CHECK(ledger.index.view()[1].template get<1>()[0].get() == 1);
      NESTED_CHECK(ledger.index.decode() == Index{{"a", {}}, {"b", {1, 3}}});
   }

   // ── pssz: validation descends into map values, set elements and keys ────
   inline void pssz_validation()
   {
      const Octets good  = inner_of(sample_transfer());
      const Octets newer = octets(pssz_of(TransferV2{"a", 1, {}, 2u}));
      Octets       cut   = good;
      cut.resize(3);  // not a record: its header is cut short

      // A map value that is not the inner type.
      const Bytes bad_value = pssz_of(ByNameRaw{{"a", good}, {"b", cut}});
      NESTED_CHECK(valid<ByNameRaw>(bad_value));
      NESTED_CHECK(!valid<ByName>(bad_value));
      NESTED_CHECK(strict_rejects<ByName>(bad_value));
      ByName out;
      bool   unknown = false;
      NESTED_CHECK(!psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{bad_value}, out,
                                              unknown)
                       .ok());
      NESTED_CHECK(out.empty());
      // Deferred: the reader does not look inside.
      NESTED_CHECK(valid<ByNameLater>(bad_value));
      NESTED_CHECK(strict<ByNameLater>(bad_value));

      // Unknown fields inside a map value: compatible, reported, not exact.
      const Bytes newer_value = pssz_of(ByNameRaw{{"a", newer}});
      unknown                 = false;
      NESTED_CHECK(psio::validate_pssz_compatible<ByName>(
                      psio::pssz{}, std::span<const char>{newer_value}, unknown)
                      .ok() &&
                   unknown);
      NESTED_CHECK(strict_rejects<ByName>(newer_value));

      // A set element that is not the inner type; elements out of order;
      // a repeated element.
      NESTED_CHECK(!valid<Seen>(pssz_of(SeenRaw{cut})));
      const SeenRaw two = sorted({good, inner_of(Transfer{"z", 0, std::nullopt})});
      NESTED_CHECK(valid<Seen>(pssz_of(two)));
      NESTED_CHECK(fails_with(
         psio::validate<Seen>(psio::pssz{}, std::span<const char>{pssz_of(SeenRaw{two[1], two[0]})}),
         "pssz: set elements are not in strictly ascending order"));
      NESTED_CHECK(fails_with(
         psio::validate<Seen>(psio::pssz{}, std::span<const char>{pssz_of(SeenRaw{two[0], two[0]})}),
         "pssz: set elements are not in strictly ascending order"));

      // The order is that of the bytes read: two elements that differ only
      // in a field this reader does not know are two elements.
      const SeenRaw twins = sorted({octets(pssz_of(TransferV2{"a", 1, {}, 2u})),
                                    octets(pssz_of(TransferV2{"a", 1, {}, 3u}))});
      unknown             = false;
      NESTED_CHECK(psio::validate_pssz_compatible<Seen>(
                      psio::pssz{}, std::span<const char>{pssz_of(twins)}, unknown)
                      .ok() &&
                   unknown);

      // A nested key: by inner bytes, so 256 sorts before 1.
      const Octets one = octets(pssz_of(std::uint32_t{1}));
      const Octets big = octets(pssz_of(std::uint32_t{256}));
      NESTED_CHECK(valid<ByPacked>(pssz_of(ByPackedRaw{{big, 9}, {one, 7}})));
      NESTED_CHECK(fails_with(psio::validate<ByPacked>(
                                 psio::pssz{},
                                 std::span<const char>{pssz_of(ByPackedRaw{{one, 7}, {big, 9}})}),
                              "pssz: map entries are not in strictly ascending key order"));
      NESTED_CHECK(!valid<ByPacked>(pssz_of(ByPackedRaw{{Octets{1, 2, 3}, 7}})));

      // A map inside nested bytes: its order is checked with the enclosing
      // value, and by the nested value's own checks.
      const Bytes unsorted = pssz_of(IndexRaw{{"b", {}}, {"a", {}}});
      const Bytes inner_set = pssz_of(IndexRaw{{"a", {3, 1}}});
      using Holder    = std::tuple<std::uint8_t, psio::nested<Index>>;
      using RawHolder = std::tuple<std::uint8_t, Octets>;
      NESTED_CHECK(valid<Holder>(pssz_of(RawHolder{1, octets(pssz_of(IndexRaw{{"a", {1, 3}}}))})));
      NESTED_CHECK(fails_with(
         psio::validate<Holder>(psio::pssz{},
                                std::span<const char>{pssz_of(RawHolder{1, octets(unsorted)})}),
         "pssz: map entries are not in strictly ascending key order"));
      NESTED_CHECK(fails_with(
         psio::validate<Holder>(psio::pssz{},
                                std::span<const char>{pssz_of(RawHolder{1, octets(inner_set)})}),
         "pssz: set elements are not in strictly ascending order"));
      psio::nested<Index> adopted;
      NESTED_CHECK(
         !psio::nested<Index>::checked(std::span<const char>{unsorted}, adopted).ok());
      NESTED_CHECK(adopted == psio::nested<Index>{});
      NESTED_CHECK(!psio::nested<Index>::unchecked(unsorted).validate().ok());
   }

   // ── key ─────────────────────────────────────────────────────────────────
   inline void key_format()
   {
      const Ledger ledger = sample_ledger();
      const Octets good   = inner_of(sample_transfer());
      Octets       cut    = good;
      cut.resize(3);  // not a record: its header is cut short

      // The key of the list of (key, byte list) entries / of byte lists.
      NESTED_CHECK(key_of(ledger.by_name) ==
                   key_of(ByNameRaw{{"gift", inner_of(Transfer{"carol", 1, std::nullopt})},
                                    {"rent", good}}));
      const SeenRaw raw_seen = sorted({inner_of(Transfer{"z", 0, std::nullopt}),
                                       inner_of(Transfer{"a", 256, std::nullopt}), good});
      NESTED_CHECK(key_of(ledger.seen) == key_of(raw_seen));
      NESTED_CHECK(key_of(ledger.index) ==
                   key_of(octets(pssz_of(IndexRaw{{"a", {}}, {"b", {1, 3}}}))));

      const Bytes key = key_of(ledger);
      NESTED_CHECK(psio::validate<Ledger>(psio::key{}, std::span<const char>{key}).ok());
      NESTED_CHECK(psio::validate_strict<Ledger>(psio::key{}, std::span<const char>{key}).ok());
      NESTED_CHECK(psio::decode<Ledger>(psio::key{}, std::span<const char>{key}) == ledger);

      // Readers descend, and require the order.
      const Bytes bad = key_of(ByNameRaw{{"a", cut}});
      NESTED_CHECK(psio::validate<ByNameRaw>(psio::key{}, std::span<const char>{bad}).ok());
      NESTED_CHECK(fails_with(psio::validate<ByName>(psio::key{}, std::span<const char>{bad}),
                              "key: nested bytes are not a valid encoding of their type"));
      NESTED_CHECK(psio::validate<ByNameLater>(psio::key{}, std::span<const char>{bad}).ok());
      const Bytes swapped = key_of(SeenRaw{raw_seen[1], raw_seen[0]});
      NESTED_CHECK(fails_with(psio::validate<Seen>(psio::key{}, std::span<const char>{swapped}),
                              "key: set elements are not in strictly ascending order"));
      const Bytes inner_unsorted =
         key_of(octets(pssz_of(IndexRaw{{"b", {}}, {"a", {}}})));
      NESTED_CHECK(!psio::validate<psio::nested<Index>>(psio::key{},
                                                        std::span<const char>{inner_unsorted})
                       .ok());
   }

   // ── json / ijson: the inner value's own text, inside the map's form ─────
   template <typename Fmt>
   void text_format()
   {
      const Ledger ledger = sample_ledger();
      std::string  text;
      NESTED_CHECK(psio::encode_json_checked(Fmt{}, ledger, text).ok());
      NESTED_CHECK(psio::validate<Ledger>(Fmt{}, std::span<const char>{text}).ok());
      Ledger back;
      NESTED_CHECK(psio::decode_json_checked(Fmt{}, std::span<const char>{text}, back).ok());
      NESTED_CHECK(back == ledger);

      // A string-keyed map of nested values is the object of the inner
      // values; a set of nested values the array of them, in wire order.
      std::string by_name, plain;
      NESTED_CHECK(psio::encode_json_checked(Fmt{}, ledger.by_name, by_name).ok());
      NESTED_CHECK(psio::encode_json_checked(
                      Fmt{},
                      std::map<std::string, Transfer>{{"rent", sample_transfer()},
                                                      {"gift", Transfer{"carol", 1, std::nullopt}}},
                      plain)
                      .ok());
      NESTED_CHECK(by_name == plain);
      std::string       seen, seen_plain;
      std::vector<Transfer> in_order;
      for (const auto& item :
           psio::decode<std::vector<NTransfer>>(psio::pssz{},
                                                std::span<const char>{pssz_of(ledger.seen)}))
         in_order.push_back(item.decode());
      NESTED_CHECK(psio::encode_json_checked(Fmt{}, ledger.seen, seen).ok());
      NESTED_CHECK(psio::encode_json_checked(Fmt{}, in_order, seen_plain).ok());
      NESTED_CHECK(seen == seen_plain);

      // A nested key is not a string by type: pairs, ordered by inner bytes.
      std::string packed;
      NESTED_CHECK(psio::encode_json_checked(Fmt{}, ledger.by_packed, packed).ok());
      NESTED_CHECK(packed == "[[0,0],[256,9],[1,7]]");
      ByPacked read;
      NESTED_CHECK(psio::decode_json_checked(Fmt{}, std::string_view{"[[1,7],[0,0],[256,9]]"},
                                             read)
                      .ok());
      NESTED_CHECK(read == ledger.by_packed);
      NESTED_CHECK(fails_with(
         psio::decode_json_checked(Fmt{}, std::string_view{"[[1,7],[1,8]]"}, read),
         "json: duplicate map key"));

      // A map inside nested bytes: its own text; any order in, canonical out.
      std::string index;
      NESTED_CHECK(psio::encode_json_checked(Fmt{}, ledger.index, index).ok());
      NESTED_CHECK(index == R"({"a":[],"b":[1,3]})");
      psio::nested<Index> parsed;
      NESTED_CHECK(psio::decode_json_checked(Fmt{}, std::string_view{R"({"b":[3,1],"a":[]})"},
                                             parsed)
                      .ok());
      NESTED_CHECK(parsed == ledger.index);
      NESTED_CHECK(fails_with(
         psio::decode_json_checked(Fmt{}, std::string_view{R"({"b":[3,3]})"}, parsed),
         "json: duplicate set element"));

      // A repeated set element: equal inner values are equal bytes.
      Seen twice;
      NESTED_CHECK(seen_plain.size() > 2);
      const std::string element =
         [&] {
            std::string one;
            NESTED_CHECK(psio::encode_json_checked(Fmt{}, sample_transfer(), one).ok());
            return one;
         }();
      NESTED_CHECK(fails_with(
         psio::decode_json_checked(Fmt{}, std::string_view{"[" + element + "," + element + "]"},
                                   twice),
         "json: duplicate set element"));
   }

   // ── Schema-driven ───────────────────────────────────────────────────────
   template <typename T>
   void dynamic_key(const T& value)
   {
      namespace rt = psio::pssz_ir;
      const auto  schema = psio::SchemaBuilder{}.insert<T>("T").build();
      const Bytes frame  = pssz_of(value);
      const Bytes typed  = key_of(value);
      for (auto mode : {rt::nested_bytes::typed, rt::nested_bytes::opaque})
      {
         rt::Value dynamic;
         bool      unknown = true;
         NESTED_CHECK(rt::decode_auto(schema, "T", frame, dynamic, unknown, nullptr, mode).ok() && !unknown);
         Bytes key{'k'};
         NESTED_CHECK(rt::to_key(schema, "T", dynamic, key, nullptr, mode).ok());
         NESTED_CHECK(key.size() == typed.size() + 1 && key[0] == 'k' &&
                      std::equal(typed.begin(), typed.end(), key.begin() + 1));
      }
   }

   inline void schema_driven()
   {
      namespace ir = psio::schema_types;
      namespace rt = psio::pssz_ir;
      const auto schema = psio::SchemaBuilder{}
                             .insert<Ledger>("Ledger")
                             .insert<ByName>("ByName")
                             .insert<Seen>("Seen")
                             .insert<ByPacked>("ByPacked")
                             .insert<psio::nested<Index>>("Index")
                             .build();
      // Custom{List{Nested{Transfer}}, "set"}; Nested{Custom{..., "map"}}.
      auto named = [&](std::string_view name) -> const ir::AnyType* {
         for (const auto& type : schema.types)
            if (type.name == name)
               return type.type->resolve(schema);
         return nullptr;
      };
      {
         const auto* seen   = named("Seen");
         const auto* custom = seen ? std::get_if<ir::Custom>(&seen->value) : nullptr;
         NESTED_CHECK(custom && custom->id == "set");
         const auto* list =
            custom ? std::get_if<ir::List>(&custom->type->resolve(schema)->value) : nullptr;
         NESTED_CHECK(list &&
                      std::holds_alternative<ir::Nested>(list->type->resolve(schema)->value));
         const auto* index  = named("Index");
         const auto* nested = index ? std::get_if<ir::Nested>(&index->value) : nullptr;
         const auto* inner =
            nested ? std::get_if<ir::Custom>(&nested->type->resolve(schema)->value) : nullptr;
         NESTED_CHECK(inner && inner->id == "map");
      }

      const Ledger ledger = sample_ledger();
      const Bytes  bytes  = pssz_of(ledger);
      NESTED_CHECK(rt::validate_pssz(schema, "Ledger", bytes).ok());
      rt::Value value;
      bool      unknown = true;
      NESTED_CHECK(rt::decode_auto(schema, "Ledger", bytes, value, unknown).ok() && !unknown);
      Bytes again;
      NESTED_CHECK(rt::encode_auto(schema, "Ledger", value, again).ok());
      NESTED_CHECK(again == bytes);
      // An explicit-width frame.
      Bytes wide, wide_again;
      NESTED_CHECK(psio::encode_pssz_checked(psio::pssz_<4>{}, ledger, wide).ok());
      rt::Value wide_value;
      NESTED_CHECK(rt::decode_exact<4>(schema, "Ledger", wide, wide_value).ok());
      NESTED_CHECK(wide_value == value);
      NESTED_CHECK(rt::encode<4>(schema, "Ledger", wide_value, wide_again).ok());
      NESTED_CHECK(wide_again == wide);

      // Text: the typed text, both ways.
      for (int pass = 0; pass < 2; ++pass)
      {
         std::string typed, dynamic;
         Bytes       back;
         if (pass == 0)
         {
            NESTED_CHECK(psio::encode_json_checked(psio::json{}, ledger, typed).ok());
            NESTED_CHECK(rt::pssz_to_json(schema, "Ledger", psio::json{}, bytes, dynamic).ok());
            NESTED_CHECK(rt::json_to_pssz(schema, "Ledger", psio::json{}, typed, back).ok());
         }
         else
         {
            NESTED_CHECK(psio::encode_json_checked(psio::ijson{}, ledger, typed).ok());
            NESTED_CHECK(rt::pssz_to_json(schema, "Ledger", psio::ijson{}, bytes, dynamic).ok());
            NESTED_CHECK(rt::json_to_pssz(schema, "Ledger", psio::ijson{}, typed, back).ok());
         }
         NESTED_CHECK(dynamic == typed);
         NESTED_CHECK(back == bytes);
      }
      // Text in another order is sorted by the inner bytes; a repeated key
      // (equal inner values) is refused.
      Bytes packed;
      NESTED_CHECK(rt::json_to_pssz(schema, "ByPacked", psio::json{},
                                    std::string_view{"[[1,7],[0,0],[256,9]]"}, packed)
                      .ok());
      NESTED_CHECK(packed == pssz_of(ledger.by_packed));
      NESTED_CHECK(fails_with(rt::json_to_pssz(schema, "ByPacked", psio::json{},
                                               std::string_view{"[[1,7],[1,8]]"}, packed),
                              "json: duplicate map key"));

      // Validation descends into the entries, unless told the bytes are
      // opaque; the order of nested keys / elements is checked either way.
      const Octets good = inner_of(sample_transfer());
      Octets       cut  = good;
      cut.resize(3);  // not a record: its header is cut short
      const Bytes bad_value = pssz_of(ByNameRaw{{"a", good}, {"b", cut}});
      NESTED_CHECK(!rt::validate_pssz(schema, "ByName", bad_value).ok());
      NESTED_CHECK(
         rt::validate_pssz(schema, "ByName", bad_value, nullptr, nullptr, rt::nested_bytes::opaque).ok());
      const SeenRaw two = sorted({good, inner_of(Transfer{"z", 0, std::nullopt})});
      const Bytes   swapped = pssz_of(SeenRaw{two[1], two[0]});
      const Bytes   doubled = pssz_of(SeenRaw{two[0], two[0]});
      for (auto mode : {rt::nested_bytes::typed, rt::nested_bytes::opaque})
      {
         NESTED_CHECK(rt::validate_pssz(schema, "Seen", pssz_of(two), nullptr, nullptr, mode).ok());
         NESTED_CHECK(fails_with(rt::validate_pssz(schema, "Seen", swapped, nullptr, nullptr, mode),
                                 "pssz IR: set elements are not in strictly ascending order"));
         NESTED_CHECK(fails_with(rt::validate_pssz(schema, "Seen", doubled, nullptr, nullptr, mode),
                                 "pssz IR: set elements are not in strictly ascending order"));
      }
      const Octets one = octets(pssz_of(std::uint32_t{1}));
      const Octets big = octets(pssz_of(std::uint32_t{256}));
      NESTED_CHECK(
         rt::validate_pssz(schema, "ByPacked", pssz_of(ByPackedRaw{{big, 9}, {one, 7}})).ok());
      NESTED_CHECK(fails_with(
         rt::validate_pssz(schema, "ByPacked", pssz_of(ByPackedRaw{{one, 7}, {big, 9}})),
         "pssz IR: map entries are not in strictly ascending key order"));
      NESTED_CHECK(
         !rt::validate_pssz(schema, "ByPacked", pssz_of(ByPackedRaw{{Octets{1, 2, 3}, 7}})).ok());
      NESTED_CHECK(fails_with(
         rt::validate_pssz(schema, "Index", pssz_of(octets(pssz_of(IndexRaw{{"b", {}}, {"a", {}}})))),
         "pssz IR: map entries are not in strictly ascending key order"));

      // The order is that of the bytes read (as for the typed reader): two
      // elements that differ only in an unknown inner field are in order,
      // and reported as carrying unknown fields.
      const SeenRaw twins = sorted({octets(pssz_of(TransferV2{"a", 1, {}, 2u})),
                                    octets(pssz_of(TransferV2{"a", 1, {}, 3u}))});
      unknown             = false;
      NESTED_CHECK(rt::validate_pssz(schema, "Seen", pssz_of(twins), &unknown).ok() && unknown);
      NESTED_CHECK(fails_with(
         rt::validate_pssz(schema, "Seen", pssz_of(SeenRaw{twins[1], twins[0]}), &unknown),
         "pssz IR: set elements are not in strictly ascending order"));

      // The key encoder: a Nested node anywhere a key can hold one.
      dynamic_key(NTransfer::from(sample_transfer()));
      dynamic_key(psio::nested<std::string>::from(std::string{"a\0b", 3}));
      dynamic_key(psio::nested<std::string>{});
      dynamic_key(std::tuple<std::uint32_t, NTransfer>{7, NTransfer::from(sample_transfer())});
      dynamic_key(std::optional<NU32>{NU32::from(0)});
      dynamic_key(std::vector<NU32>{NU32::from(1), NU32::from(0)});
      dynamic_key(ledger.by_name);
      dynamic_key(ledger.seen);
      dynamic_key(ledger.by_packed);
      dynamic_key(ledger.index);
      dynamic_key(psio::nested<NTransfer>::from(NTransfer::from(sample_transfer())));
      dynamic_key(ledger);

      // A dynamic value that is not the inner type has no key.
      Bytes     key;
      rt::Value wrong = rt::Value{std::string{"not a record"}};
      NESTED_CHECK(!rt::to_key(schema, "Seen", rt::Value{rt::Sequence{wrong}}, key).ok());
      NESTED_CHECK(key.empty());
   }

   inline int run()
   {
      failures() = 0;
      pssz_bytes();
      pssz_validation();
      key_format();
      text_format<psio::json>();
      text_format<psio::ijson>();
      schema_driven();
      return failures();
   }
}  // namespace nested_assoc_cases
