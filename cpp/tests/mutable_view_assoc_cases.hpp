// psio::mutable_view over pairs, maps and nested values (docs/pssz-spec.md
// §17): the mapped value of a map entry and the inner value of a
// psio::nested<T> can be overwritten in place; keys cannot.
//
// Shared by a Catch test and by the exception-disabled executable. Every
// write is checked the same way: the frame is byte-identical to a fresh
// encoding of the expected value.
//
// run<F>() returns 0, or the number of the first check that did not hold.

#pragma once

#include <psio/mutable_view.hpp>
#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mv_assoc_cases
{
   struct Balance
   {
      std::uint32_t amount = 0;
      bool          frozen = false;
      friend bool   operator==(const Balance&, const Balance&) = default;
   };
   PSIO_REFLECT(Balance, amount, frozen, definitionWillNotChange())

   struct Args
   {
      std::string                  to;
      std::uint64_t                amount = 0;
      std::optional<std::uint16_t> memo;
      friend bool operator==(const Args&, const Args&) = default;
   };
   PSIO_REFLECT(Args, to, amount, memo)

   struct Maps
   {
      std::map<std::string, std::uint64_t>                        totals;
      std::map<std::uint32_t, Balance>                            balances;
      std::unordered_map<std::string, std::vector<std::uint16_t>> lists;
      std::map<std::string, std::optional<std::uint32_t>>         limits;
      std::pair<std::uint32_t, std::string>                       pair;
      std::map<std::uint8_t, std::map<std::uint8_t, std::uint16_t>> grid;
      friend bool operator==(const Maps&, const Maps&) = default;
   };
   PSIO_REFLECT(Maps, totals, balances, lists, limits, pair, grid)

   struct Packed
   {
      psio::nested<Args>                              args;
      std::map<std::string, psio::nested<Balance>>    by_name;
      psio::nested<std::map<std::string, std::uint64_t>> totals;
      std::optional<psio::nested<Balance>>            maybe;
      std::optional<psio::nested<Balance>>            absent;
      psio::nested<psio::nested<Balance>>             twice;
      friend bool operator==(const Packed&, const Packed&) = default;
   };
   PSIO_REFLECT(Packed, args, by_name, totals, maybe, absent, twice)

   template <typename F, typename T>
   bool is_encoding_of(const std::vector<char>& bytes, const T& expected)
   {
      std::vector<char> fresh;
      return psio::encode_pssz_checked(F{}, expected, fresh).ok() && fresh == bytes &&
             psio::validate<T>(F{}, std::span<const char>{bytes}).ok();
   }

   template <typename F>
   int run_maps()
   {
      Maps maps;
      maps.totals   = {{"b", 2}, {"a", 1}};
      maps.balances = {{7, {70, false}}, {3, {30, true}}};
      maps.lists    = {{"y", {1, 2}}, {"x", {}}};
      maps.limits   = {{"none", std::nullopt}, {"some", 5}};
      maps.pair     = {4, "p"};
      maps.grid     = {{1, {{1, 11}, {2, 12}}}, {0, {}}};
      std::vector<char> bytes;
      if (!psio::encode_pssz_checked(F{}, maps, bytes).ok()) return 1;
      const psio::mutable_view<Maps, F> view{bytes};

      // Entries are in key order, whatever the container's own order; a key
      // is readable.
      const auto totals = view.template get<0>();
      if (totals.size() != 2 || totals.empty()) return 2;
      if (totals[0].key().view_() != "a" || totals[1].key().view_() != "b") return 3;
      if (!totals[1].value().set(20)) return 4;
      maps.totals["b"] = 20;

      // A fixed-size mapped record: as a whole, or member by member.
      const auto balances = view.template get<1>();
      if (balances[0].key() != 3u || balances[1].key() != 7u) return 5;
      if (!balances[0].value().set(Balance{31, false})) return 6;
      if (!balances[1].value().template get<1>().set(true)) return 7;
      maps.balances[3] = {31, false};
      maps.balances[7].frozen = true;

      // A variable-size mapped value is navigated like any other.
      std::size_t visited = 0;
      for (auto entry : view.template get<2>())
      {
         ++visited;
         if (entry.key().view_() == "y" && !entry.value()[1].set(9)) return 8;
         if (entry.key().view_() == "x" && entry.value().at(0).set(9)) return 9;
      }
      if (visited != 2) return 10;
      maps.lists["y"][1] = 9;

      // An optional mapped value: presence does not change.
      const auto limits = view.template get<3>();
      if (limits[0].value().has_value() || (*limits[0].value()).set(1)) return 11;
      if (!limits[1].value().has_value() || !(*limits[1].value()).set(6)) return 12;
      maps.limits["some"] = 6;

      // A pair is the 2-tuple of its members.
      if (!view.template get<4>().first().set(40)) return 13;
      if (psio::size(view.template get<4>().second()) != 1) return 14;
      maps.pair.first = 40;

      // A map in a map; positions out of range.
      const auto grid = view.template get<5>();
      if (grid[0].key() != 0 || !grid[0].value().empty()) return 15;
      if (!grid[1].value()[1].value().set(120)) return 16;
      if (grid.at(2).value().at(0).value().set(1)) return 17;
      if (grid[1].value().at(2).value().set(1)) return 18;
      maps.grid[1][2] = 120;

      return is_encoding_of<F>(bytes, maps) ? 0 : 19;
   }

   template <typename F>
   int run_packed()
   {
      Packed packed;
      packed.args    = psio::nested<Args>::from(Args{"bob", 5, 7});
      packed.by_name = {{"b", psio::nested<Balance>::from(Balance{2, false})},
                        {"a", psio::nested<Balance>::from(Balance{1, true})}};
      packed.totals  = psio::nested<std::map<std::string, std::uint64_t>>::from({{"k", 1}});
      packed.maybe   = psio::nested<Balance>::from(Balance{3, false});
      packed.twice   = psio::nested<psio::nested<Balance>>::from(
         psio::nested<Balance>::from(Balance{4, false}));
      std::vector<char> bytes;
      if (!psio::encode_pssz_checked(F{}, packed, bytes).ok()) return 1;
      const psio::mutable_view<Packed, F> view{bytes};

      // The inner value of a nested member: always the automatic profile.
      const auto args = view.template get<0>().get();
      if (!args.template get<1>().set(50)) return 2;
      if (!(*args.template get<2>()).set(70)) return 3;
      if (psio::as_view(args).template get<0>().view_() != "bob") return 4;
      packed.args = psio::nested<Args>::from(Args{"bob", 50, 70});

      // A nested mapped value.
      const auto by_name = view.template get<1>();
      if (by_name[0].key().view_() != "a") return 5;
      if (!(*by_name[0].value()).set(Balance{10, false})) return 6;
      if (!by_name[1].value().get().template get<0>().set(20)) return 7;
      packed.by_name["a"] = psio::nested<Balance>::from(Balance{10, false});
      packed.by_name["b"] = psio::nested<Balance>::from(Balance{20, false});

      // A map inside nested bytes.
      if (!view.template get<2>().get()[0].value().set(100)) return 8;
      packed.totals = psio::nested<std::map<std::string, std::uint64_t>>::from({{"k", 100}});

      // Through an optional, present and absent.
      if (!(*view.template get<3>()).get().set(Balance{30, true})) return 9;
      if (view.template get<4>().has_value() || (*view.template get<4>()).get().set(Balance{}))
         return 10;
      packed.maybe = psio::nested<Balance>::from(Balance{30, true});

      // Nested in nested.
      if (!view.template get<5>().get().get().template get<1>().set(true)) return 11;
      packed.twice = psio::nested<psio::nested<Balance>>::from(
         psio::nested<Balance>::from(Balance{4, true}));

      if (!is_encoding_of<F>(bytes, packed)) return 12;
      // The nested values are still what they say they are.
      const auto back = psio::decode<Packed>(F{}, std::span<const char>{bytes});
      if (!(back == packed) || !back.args.validate_strict().ok() ||
          !back.totals.validate().ok() || !back.twice.decode().validate().ok())
         return 13;
      return 0;
   }

   template <typename F>
   int run()
   {
      if (int status = run_maps<F>()) return status;
      if (int status = run_packed<F>()) return 100 + status;
      return 0;
   }
}  // namespace mv_assoc_cases
