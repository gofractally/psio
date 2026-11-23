#pragma once
// Differential cases for psio::validate_strict<T>(psio::pssz{}, bytes): the
// library's in-place walk against the reference implementation
// (pssz_strict_oracle.hpp: decode the value, encode it again, compare) on
// every input.
//
// The inputs: for each of a broad set of reflected shapes, randomly
// generated values, their encodings, and mutations of those encodings — bit
// flips, bytes set to boundary values, bytes nudged up and down (offsets,
// headers, selectors), truncations, appended bytes, inserted and removed
// bytes, swapped and duplicated ranges, and splices of two encodings of the
// same type.
//
// run_all(report) calls report(type name, strict ok, reference ok, plain ok)
// for every input; pssz_strict_inplace_tests.cpp requires the first two to
// agree.

#include "pssz_strict_cases.hpp"
#include "pssz_strict_oracle.hpp"

#include <psio/nested.hpp>
#include <psio/pssz.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace inplace_cases
{
   using strict_cases::Name;
   using u8  = std::uint8_t;
   using u16 = std::uint16_t;
   using u32 = std::uint32_t;
   using u64 = std::uint64_t;

   enum class Color : std::uint8_t { red, green, blue };

   // ── Shapes ────────────────────────────────────────────────────────────

   struct Fixed
   {
      u32 a = 0;
      u16 b = 0;
      bool c = false;
      friend bool operator==(const Fixed&, const Fixed&) = default;
      PSIO_REFLECT(Fixed, definitionWillNotChange(), a, b, c)
   };
   // All numbers, extensible: the one-compare fast path of the validator.
   struct Nums
   {
      u32 a = 0;
      u64 b = 0;
      PSIO_REFLECT(Nums, a, b)
   };
   struct Leafs
   {
      u8           a = 0;
      std::int64_t b = 0;
      double       c = 0;
      bool         d = false;
      Color        e = Color::red;
      PSIO_REFLECT(Leafs, a, b, c, d, e)
   };
   // Trailing optionals, with and without a selector.
   struct Opt
   {
      u32                            id = 0;
      std::optional<u32>             a;
      std::optional<std::string>     b;
      std::optional<Fixed>           c;
      std::optional<std::vector<u8>> d;
      PSIO_REFLECT(Opt, id, a, b, c, d)
   };
   // Optionals between required members.
   struct Mid
   {
      std::optional<std::string>       a;
      u16                              x = 0;
      std::optional<u16>               b;
      std::string                      s;
      std::optional<std::string>       c;
      std::optional<std::optional<u8>> d;
      PSIO_REFLECT(Mid, a, x, b, s, c, d)
   };
   // One-byte offsets.
   struct Small
   {
      std::string                s;
      std::vector<u8>            v;
      std::optional<std::string> o;
      PSIO_REFLECT(Small, s, v, o, maxDynamicData(120))
   };
   // Two-byte offsets, holding one-byte records.
   struct Medium
   {
      std::string        s;
      std::vector<Small> v;
      std::optional<u32> o;
      PSIO_REFLECT(Medium, s, v, o, maxDynamicData(4000))
   };
   struct Lists
   {
      std::vector<u32>                a;
      std::vector<Fixed>              b;
      std::vector<std::string>        c;
      std::vector<Small>              d;
      std::vector<Opt>                e;
      std::vector<std::vector<u16>>   f;
      std::vector<Leafs>              g;
      std::vector<std::optional<u32>> h;
      std::vector<Nums>               i;
      PSIO_REFLECT(Lists, a, b, c, d, e, f, g, h, i)
   };
   // Lists of variable elements in each width class (by inferred bound),
   // bounded and utf8 strings, wrapper types.
   struct Bounds
   {
      std::vector<std::string> w1;
      std::vector<std::string> w2;
      std::vector<std::string> w4;
      std::string              name;
      std::string              code;
      psio::bounded<u16, 4>    few;
      psio::utf8_string<8>     label;
      std::optional<std::string> maybe;
      PSIO_REFLECT(Bounds,
                   attr(w1, max<3> | element(max<8>)),
                   attr(w2, max<300> | element(utf8 | max<100>)),
                   w4,
                   attr(name, utf8 | max<12>),
                   attr(code, exact<2>),
                   few, label,
                   attr(maybe, utf8 | max<6>))
   };
   struct Var
   {
      std::variant<u32, std::string, Fixed, Small, std::vector<u16>> v;
      std::vector<std::variant<u8, std::string>>                     vs;
      std::optional<std::variant<bool, Opt>>                         ov;
      PSIO_REFLECT(Var, v, vs, ov)
   };
   // A comparator that merges keys the encoding distinguishes.
   struct CaseLess
   {
      bool operator()(const std::string& a, const std::string& b) const
      {
         auto low = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
         for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
            if (low(a[i]) != low(b[i]))
               return low(a[i]) < low(b[i]);
         return a.size() < b.size();
      }
   };
   struct Maps
   {
      std::map<std::string, u32>           a;
      std::map<u32, std::string>           b;
      std::set<u64>                        c;
      std::unordered_map<std::string, u16> d;
      std::map<double, u8>                 e;
      std::set<std::string>                f;
      std::map<u8, Small>                  g;
      std::unordered_set<u32>              h;
      std::map<std::string, u8, CaseLess>  ci;
      std::set<std::string, CaseLess>      cs;
      PSIO_REFLECT(Maps, a, b, c, d, e, f, g, h, ci, cs)
   };
   struct Misc
   {
      std::array<u16, 3>                           a;
      std::array<std::string, 2>                   b;
      psio::bitvector<10>                          bv;
      psio::bitlist<20>                            bl;
      psio::uint128                                big = 0;
      std::tuple<u32, std::string, std::optional<u8>> t;
      std::pair<u8, std::string>                   p;
      std::array<bool, 3>                          flags{};
      PSIO_REFLECT(Misc, a, b, bv, bl, big, t, p, flags)
   };
   struct Nest
   {
      psio::nested<Opt>                  n;
      std::optional<psio::nested<Fixed>> f;
      std::vector<psio::nested<Small>>   v;
      PSIO_REFLECT(Nest, n, f, v)
   };

   // A third-party semantic spec: validate_strict runs its (span) check on
   // the members it annotates, wherever the spec walk reaches them.
   struct no_x_spec
   {
      using spec_category = psio::runtime_spec_tag;
      using applies_to    = psio::shape_set<psio::ByteStringShape>;
      constexpr bool operator==(const no_x_spec&) const = default;
      static psio::codec_status validate(std::span<const char> bytes) noexcept
      {
         for (char c : bytes)
            if (c == 'x')
               return psio::codec_fail("no_x: an x", 0, "no_x");
         return psio::codec_ok();
      }
   };
   struct Spec
   {
      std::string                word;
      std::vector<std::string>   words;
      std::string                text;
      std::optional<std::string> maybe;
      friend bool operator==(const Spec&, const Spec&) = default;
      PSIO_REFLECT(Spec, word, words, attr(text, utf8), maybe)
   };
   // Transparent types: a bounded utf8 member, and a plain one (a
   // transparent member takes no other spec).
   struct Short
   {
      std::string text;
      friend auto operator<=>(const Short&, const Short&) = default;
      PSIO_REFLECT(Short, transparent(), attr(text, utf8 | max<4>))
   };
   struct Plain
   {
      std::string text;
      friend bool operator==(const Plain&, const Plain&) = default;
      PSIO_REFLECT(Plain, transparent(), text)
   };
   struct Wraps
   {
      Short                short_text;
      Plain                plain;
      std::vector<Plain>   plains;
      std::optional<Short> maybe;
      std::map<Short, Plain> map;
      PSIO_REFLECT(Wraps, short_text, plain, plains, maybe, map)
   };

   // Where the spec walk goes (records, lists, optionals, variants, maps
   // and sets) and where it does not (arrays, tuples, lists of maps).
   struct SpecTree
   {
      Spec                              one;
      std::vector<Spec>                 list;
      std::optional<Spec>               maybe;
      std::variant<u8, Spec>            choice;
      std::map<u8, Spec>                map;
      std::array<Spec, 1>               array;
      std::tuple<Spec, u8>              tuple;
      std::vector<std::map<u8, Spec>>   maps;
      std::vector<std::optional<Spec>>  optionals;
      psio::nested<Spec>                nested;
      PSIO_REFLECT(SpecTree, one, list, maybe, choice, map, array, tuple, maps, optionals,
                   nested)
   };

   // A member written through the adapter its as<Tag> names. The adapter
   // reads upper and lower case and writes lower case.
   struct letters_tag;
   struct Letters
   {
      std::string text;
      friend bool operator==(const Letters&, const Letters&) = default;
      PSIO_REFLECT(Letters, text)
   };
   struct LettersAdapter
   {
      static std::size_t packsize(const Letters& l) { return l.text.size(); }
      template <typename Sink>
      static void encode(const Letters& l, Sink& s)
      {
         s.insert(s.end(), l.text.begin(), l.text.end());
      }
      static Letters decode(std::span<const char> b)
      {
         Letters l;
         for (char c : b)
            l.text.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
         return l;
      }
      static psio::codec_status validate(std::span<const char> b) noexcept
      {
         for (char c : b)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
               return psio::codec_fail("Letters: not a letter", 0, "Letters");
         return psio::codec_ok();
      }
      static psio::codec_status validate_strict(std::span<const char> b) noexcept
      {
         return validate(b);
      }
   };
   struct AsMember
   {
      std::string id;
      Letters     letters;
      u16         n = 0;
      PSIO_REFLECT(AsMember, id, letters, n)
   };

   // psibase::Transaction in miniature: a fixed header, a list of actions
   // carrying raw byte payloads, a list of claims.
   struct Tapos
   {
      u32 expiration     = 0;
      u32 refBlockSuffix = 0;
      u16 flags          = 0;
      u8  refBlockIndex  = 0;
      PSIO_REFLECT(Tapos, definitionWillNotChange(), expiration, refBlockSuffix, flags,
                   refBlockIndex)
   };
   struct Account
   {
      u64 value = 0;
      PSIO_REFLECT(Account, definitionWillNotChange(), value)
   };
   struct Action
   {
      Account           sender;
      Account           service;
      Account           method;
      std::vector<char> rawData;
      PSIO_REFLECT(Action, sender, service, method, rawData)
   };
   struct Claim
   {
      Account           service;
      std::vector<char> rawData;
      PSIO_REFLECT(Claim, service, rawData)
   };
   struct Transaction
   {
      Tapos               tapos;
      std::vector<Action> actions;
      std::vector<Claim>  claims;
      PSIO_REFLECT(Transaction, tapos, actions, claims)
   };
   struct SignedTransaction
   {
      psio::nested<Transaction>                     transaction;
      std::vector<std::vector<char>>                proofs;
      std::optional<std::vector<std::vector<char>>> subjectiveData;
      PSIO_REFLECT(SignedTransaction, transaction, proofs, subjectiveData)
   };
}  // namespace inplace_cases

PSIO_ADAPTER(inplace_cases::Letters, inplace_cases::letters_tag, inplace_cases::LettersAdapter)
template <>
inline constexpr auto psio::annotate<&inplace_cases::AsMember::letters> = std::tuple{
   psio::as<inplace_cases::letters_tag>,
};
template <>
inline constexpr auto psio::annotate<&inplace_cases::Spec::word> = std::tuple{
   inplace_cases::no_x_spec{},
};
template <>
inline constexpr auto psio::annotate<&inplace_cases::Spec::words> = std::tuple{
   psio::element_spec<inplace_cases::no_x_spec>{},
};
template <>
inline constexpr auto psio::annotate<&inplace_cases::Spec::maybe> = std::tuple{
   inplace_cases::no_x_spec{},
};

namespace inplace_cases
{
   // ── Random values ─────────────────────────────────────────────────────

   using strict_oracle::pick;
   using strict_oracle::Rng;

   template <typename T>
   struct is_vector : std::false_type {};
   template <typename E, typename A>
   struct is_vector<std::vector<E, A>> : std::true_type {};
   template <typename T>
   struct is_optional : std::false_type {};
   template <typename E>
   struct is_optional<std::optional<E>> : std::true_type {};
   template <typename T>
   struct is_variant : std::false_type {};
   template <typename... E>
   struct is_variant<std::variant<E...>> : std::true_type {};
   template <typename T>
   struct is_array : std::false_type {};
   template <typename E, std::size_t N>
   struct is_array<std::array<E, N>> : std::true_type {};
   template <typename T>
   struct is_tuple : std::false_type {};
   template <typename... E>
   struct is_tuple<std::tuple<E...>> : std::true_type {};
   template <typename A, typename B>
   struct is_tuple<std::pair<A, B>> : std::true_type {};
   template <typename T>
   struct is_nested : std::false_type {};
   template <typename E>
   struct is_nested<psio::nested<E>> : std::true_type { using inner = E; };
   template <typename T>
   struct is_bounded : std::false_type {};
   template <typename E, std::size_t N>
   struct is_bounded<psio::bounded<E, N>> : std::true_type { using element = E; };
   template <typename T>
   struct is_utf8 : std::false_type {};
   template <std::size_t N>
   struct is_utf8<psio::utf8_string<N>> : std::true_type {};
   template <typename T>
   struct is_bitvector : std::false_type {};
   template <std::size_t N>
   struct is_bitvector<psio::bitvector<N>> : std::true_type
   {
      static constexpr std::size_t size = N;
   };
   template <typename T>
   struct is_bitlist : std::false_type {};
   template <std::size_t N>
   struct is_bitlist<psio::bitlist<N>> : std::true_type
   {
      static constexpr std::size_t size = N;
   };

   inline std::string random_string(Rng& rng)
   {
      static constexpr std::string_view letters = "abAByzx";
      std::string       out;
      const std::size_t n = pick(rng, 6);
      for (std::size_t i = 0; i < n; ++i)
      {
         const auto roll = pick(rng, 40);
         if (roll == 0)
            out += "\xc3\xa9";  // two-byte UTF-8
         else if (roll == 1)
            out += '\xff';  // not UTF-8
         else
            out += letters[pick(rng, roll < 30 ? 6 : 7)];
      }
      return out;
   }

   template <typename T>
   void gen(Rng& rng, T& out, int depth = 0);

   template <typename T>
   T make(Rng& rng, int depth)
   {
      T out{};
      gen(rng, out, depth);
      return out;
   }

   template <typename T>
   void gen(Rng& rng, T& out, int depth)
   {
      const std::size_t max_items = depth > 3 ? 1 : 3;
      if constexpr (std::is_same_v<T, bool>)
         out = rng() & 1;
      else if constexpr (std::is_same_v<T, psio::uint128>)
         out = (psio::uint128(rng()) << 64) | rng();
      else if constexpr (std::is_floating_point_v<T>)
      {
         static constexpr double values[] = {0.0, -0.0, 1.5, -2.25, 1e10, 3.0, 7.0};
         out = static_cast<T>(values[pick(rng, 7)]);
      }
      else if constexpr (std::is_integral_v<T>)
         out = static_cast<T>(pick(rng, 4) == 0 ? rng() : pick(rng, 6));
      else if constexpr (std::is_enum_v<T>)
         out = static_cast<T>(pick(rng, 4));
      else if constexpr (std::is_same_v<T, std::string>)
         out = random_string(rng);
      else if constexpr (std::is_same_v<T, Name>)
         out = Name{rng() % 100};
      else if constexpr (std::is_same_v<T, strict_cases::Code>)
         out = strict_cases::Code{static_cast<u32>(rng())};
      else if constexpr (std::is_same_v<T, strict_cases::Flag>)
         out = strict_cases::Flag{bool(rng() & 1)};
      else if constexpr (std::is_same_v<T, strict_cases::Memo>)
         out = strict_cases::Memo{random_string(rng)};
      else if constexpr (std::is_same_v<T, strict_cases::Counter>)
         out = strict_cases::Counter{static_cast<u32>(pick(rng, 9))};
      else if constexpr (std::is_same_v<T, strict_cases::Range>)
      {
         const u32 a = static_cast<u32>(pick(rng, 9));
         out         = strict_cases::Range{a, a + static_cast<u32>(pick(rng, 4))};
      }
      else if constexpr (std::is_same_v<T, Letters>)
      {
         const std::size_t n = pick(rng, 5);
         for (std::size_t i = 0; i < n; ++i)
            out.text.push_back(char('a' + pick(rng, 26)));
      }
      else if constexpr (is_utf8<T>::value)
      {
         std::string text = random_string(rng);
         out              = T{text.substr(0, 8)};
      }
      else if constexpr (is_bitvector<T>::value)
         for (std::size_t i = 0; i < is_bitvector<T>::size; ++i)
            out.set(i, rng() & 1);
      else if constexpr (is_bitlist<T>::value)
      {
         const std::size_t n = pick(rng, is_bitlist<T>::size + 1);
         for (std::size_t i = 0; i < n; ++i)
            out.push_back(rng() & 1);
      }
      else if constexpr (is_nested<T>::value)
         out = T{make<typename is_nested<T>::inner>(rng, depth + 1)};
      else if constexpr (is_bounded<T>::value)
      {
         std::vector<typename is_bounded<T>::element> items(pick(rng, max_items + 1));
         for (auto& item : items)
            gen(rng, item, depth + 1);
         out = T{std::move(items)};
      }
      else if constexpr (is_vector<T>::value)
      {
         const std::size_t n = pick(rng, max_items + 1);
         out.clear();
         for (std::size_t i = 0; i < n; ++i)
            out.push_back(make<typename T::value_type>(rng, depth + 1));
      }
      else if constexpr (is_optional<T>::value)
      {
         if (pick(rng, 5) < 3)
            out = make<typename T::value_type>(rng, depth + 1);
         else
            out.reset();
      }
      else if constexpr (is_variant<T>::value)
      {
         const std::size_t index = pick(rng, std::variant_size_v<T>);
         [&]<std::size_t... I>(std::index_sequence<I...>) {
            (void)((index == I
                       ? (out.template emplace<I>(
                             make<std::variant_alternative_t<I, T>>(rng, depth + 1)),
                          true)
                       : false) ||
                   ...);
         }(std::make_index_sequence<std::variant_size_v<T>>{});
      }
      else if constexpr (is_array<T>::value)
         for (auto& item : out)
            gen(rng, item, depth + 1);
      else if constexpr (is_tuple<T>::value)
         std::apply([&](auto&... item) { (gen(rng, item, depth + 1), ...); }, out);
      else if constexpr (psio::detail::is_map_v<T>)
      {
         const std::size_t n = pick(rng, max_items + 1);
         for (std::size_t i = 0; i < n; ++i)
            out.emplace(make<typename T::key_type>(rng, depth + 1),
                        make<typename T::mapped_type>(rng, depth + 1));
      }
      else if constexpr (psio::detail::is_set_v<T>)
      {
         const std::size_t n = pick(rng, max_items + 1);
         for (std::size_t i = 0; i < n; ++i)
            out.emplace(make<typename T::key_type>(rng, depth + 1));
      }
      else if constexpr (psio::Reflected<T>)
      {
         [&]<std::size_t... I>(std::index_sequence<I...>) {
            (gen(rng, out.*(psio::reflect<T>::template member_pointer<I>), depth + 1), ...);
         }(std::make_index_sequence<psio::reflect<T>::member_count>{});
      }
      else
         static_assert(sizeof(T) == 0, "inplace_cases::gen: unsupported type");
   }

   using strict_oracle::Bytes;
   using strict_oracle::for_each_mutation;

   // ── The comparison ────────────────────────────────────────────────────

   template <typename T, typename Fmt, typename Report>
   void fuzz(const char* name, Fmt fmt, std::size_t values, std::uint64_t seed, Report&& report)
   {
      Rng   rng(seed);
      Bytes previous;
      for (std::size_t i = 0; i < values; ++i)
      {
         Bytes bytes;
#if defined(__cpp_exceptions)
         try
         {
            bytes = psio::encode(fmt, make<T>(rng, 0));
         }
         catch (...)
         {
            continue;  // a value outside the type's bounds has no encoding
         }
#else
         bytes = psio::encode(fmt, make<T>(rng, 0));
#endif
         for_each_mutation(rng, bytes, previous, [&](const Bytes& input) {
            const std::span<const char> span{input};
            report(name, psio::validate_strict<T>(fmt, span).ok(),
                   strict_oracle::validate<T>(fmt, span).ok(),
                   psio::validate<T>(fmt, span).ok(), input);
         });
         previous = std::move(bytes);
      }
   }

#define INPLACE_FUZZ(T, N) fuzz<T>(#T, psio::pssz{}, N, seed++, report)
#define INPLACE_FUZZ_W(T, W, N) fuzz<T>(#T " pssz_<" #W ">", psio::pssz_<W>{}, N, seed++, report)

   // report(name, strict ok, reference ok, plain ok, input bytes).
   // `scale`: multiplies the number of values generated per type.
   template <typename Report>
   void run_all(Report&& report, std::size_t scale = 1)
   {
      std::uint64_t     seed = 0x5eed'0001;
      const std::size_t n    = 60 * scale;
      namespace sc = strict_cases;
      using VariantRoot = std::variant<u8, std::string, Opt>;
      using MapRoot     = std::map<std::string, u32>;
      using CaseMapRoot = std::map<std::string, Spec, CaseLess>;
      using HashMapRoot = std::unordered_map<u16, std::string>;
      using TupleRoot   = std::tuple<u32, std::string, std::optional<Small>>;
      using ArrayRoot   = std::array<std::optional<u16>, 3>;
      INPLACE_FUZZ(Fixed, n);
      INPLACE_FUZZ(Nums, n);
      INPLACE_FUZZ(Leafs, n);
      INPLACE_FUZZ(Opt, 3 * n);
      INPLACE_FUZZ(Mid, 3 * n);
      INPLACE_FUZZ(Small, 2 * n);
      INPLACE_FUZZ(Medium, 2 * n);
      INPLACE_FUZZ(Lists, 3 * n);
      INPLACE_FUZZ(Bounds, 3 * n);
      INPLACE_FUZZ(Var, 3 * n);
      INPLACE_FUZZ(Maps, 4 * n);
      INPLACE_FUZZ(Misc, 2 * n);
      INPLACE_FUZZ(Nest, 2 * n);
      INPLACE_FUZZ(Spec, 2 * n);
      INPLACE_FUZZ(SpecTree, 4 * n);
      INPLACE_FUZZ(AsMember, 2 * n);
      INPLACE_FUZZ(Short, n);
      INPLACE_FUZZ(Plain, n);
      INPLACE_FUZZ(Wraps, 3 * n);
      INPLACE_FUZZ(std::vector<Plain>, n);
      INPLACE_FUZZ(Transaction, 2 * n);
      INPLACE_FUZZ(SignedTransaction, 2 * n);
      // The existing strict cases: adapters, invariants, a transparent
      // type, a constructed record, order annotations, nested values.
      INPLACE_FUZZ(sc::Name, n);
      INPLACE_FUZZ(sc::Code, n);
      INPLACE_FUZZ(sc::Flag, n);
      INPLACE_FUZZ(sc::Memo, n);
      INPLACE_FUZZ(sc::Range, n);
      INPLACE_FUZZ(sc::Counter, n);
      INPLACE_FUZZ(sc::Action, n);
      INPLACE_FUZZ(sc::ActionV2, n);
      INPLACE_FUZZ(sc::Holder, n);
      INPLACE_FUZZ(sc::Tags, n);
      INPLACE_FUZZ(sc::Ids, n);
      INPLACE_FUZZ(sc::Envelope, n);
      INPLACE_FUZZ(sc::Everything, 3 * n);
      // Roots that are not records (the spec walk starts at a record only).
      INPLACE_FUZZ(std::string, n);
      INPLACE_FUZZ(std::vector<Opt>, n);
      INPLACE_FUZZ(std::vector<Spec>, n);
      INPLACE_FUZZ(std::optional<std::string>, n);
      INPLACE_FUZZ(std::optional<Spec>, n);
      INPLACE_FUZZ(VariantRoot, n);
      INPLACE_FUZZ(MapRoot, n);
      INPLACE_FUZZ(CaseMapRoot, n);
      INPLACE_FUZZ(HashMapRoot, n);
      INPLACE_FUZZ(std::set<double>, n);
      INPLACE_FUZZ(TupleRoot, n);
      INPLACE_FUZZ(std::vector<std::vector<std::string>>, n);
      INPLACE_FUZZ(ArrayRoot, n);
      INPLACE_FUZZ(psio::nested<Maps>, n);
      // The explicit-width profiles.
      INPLACE_FUZZ_W(Opt, 1, n);
      INPLACE_FUZZ_W(Opt, 2, n);
      INPLACE_FUZZ_W(Opt, 4, n);
      INPLACE_FUZZ_W(Lists, 2, n);
      INPLACE_FUZZ_W(Lists, 4, n);
      INPLACE_FUZZ_W(Maps, 2, n);
      INPLACE_FUZZ_W(Var, 4, n);
      INPLACE_FUZZ_W(SpecTree, 4, n);
      INPLACE_FUZZ_W(sc::Everything, 4, n);
      INPLACE_FUZZ_W(AsMember, 2, n);
   }
#undef INPLACE_FUZZ
#undef INPLACE_FUZZ_W
}  // namespace inplace_cases
