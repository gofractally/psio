// Trusted reads and views on bytes that are not a frame of the type
// (gofractally/psio#15; docs/annotations.md 7.2 "On bytes that are not a
// frame").
//
// A trusted read holds nothing against what a type DECLARES. It is still
// handed bytes that may not be a frame of the type at all: a truncated row,
// a row an incompatible version of the type wrote. The contract pinned
// here, for pssz, ssz and the canonical ABI:
//
//   * no read outside the bytes, whatever they are (this file is also built
//     with AddressSanitizer + UndefinedBehaviorSanitizer);
//   * bounded work: nothing is sized by a number the bytes do not back;
//   * a view — safe or trusted — FAILS (psio::codec_abort: a
//     psio::codec_exception here) when a member cannot be placed. It does
//     not hand out an empty or zero member in its place;
//   * the pssz trusted decode fails the same way (the ssz and canonical-ABI
//     trusted decodes are for validated bytes only and are not walked);
//   * the two kinds of view read the same thing from the same bytes;
//   * bytes that do place every member read as some in-range value: that
//     value is unspecified.

#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>
#include <psio/wit.hpp>
#include <psio/wit_view.hpp>

#include <catch.hpp>

#include <array>
#include <map>
#include <set>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{
   // ── The reporter's shapes ────────────────────────────────────────────────
   struct TsNote
   {
      std::uint32_t id = 0;
      std::string   text;
      PSIO_REFLECT(TsNote, id, attr(text, utf8 | max<8>))
   };
   struct TsWide
   {
      std::uint32_t              id = 0;
      std::string                a;
      std::vector<std::uint64_t> b;
      std::optional<std::string> c;
      PSIO_REFLECT(TsWide, id, a, b, c)
   };
   struct TsOld
   {
      std::uint32_t id = 0;
      std::string   a;
      PSIO_REFLECT(TsOld, id, a)
   };
   struct TsFixed
   {
      std::uint64_t x = 0;
      std::uint64_t y = 0;
      std::uint32_t z = 0;
      PSIO_REFLECT(TsFixed, x, y, z)
   };

   // ── A shape with one of everything a span view reads ─────────────────────
   struct TsItem
   {
      std::string   product;
      std::uint32_t qty = 0;
      friend bool   operator==(const TsItem&, const TsItem&) = default;
      PSIO_REFLECT(TsItem, product, qty)
   };
   struct TsPoint
   {
      std::uint32_t x = 0;
      std::uint16_t y = 0;
      bool          on = false;
      friend bool   operator==(const TsPoint&, const TsPoint&) = default;
      PSIO_REFLECT(TsPoint, x, y, on)
   };
   // What pssz, ssz and the canonical ABI all read.
   struct TsRow
   {
      std::uint64_t              id = 0;
      std::uint32_t              n  = 0;
      bool                       flag = false;
      std::string                a, b, c;
      TsPoint                    at;
      TsItem                     first;
      std::vector<TsItem>        items;
      std::vector<std::uint32_t> numbers;
      std::vector<std::string>   names;
      std::vector<TsPoint>       points;
      friend bool                operator==(const TsRow&, const TsRow&) = default;
      PSIO_REFLECT(TsRow, id, n, flag, a, b, c, at, first, items, numbers, names, points)
   };
   // ... and what pssz reads besides.
   struct TsRich
   {
      std::uint32_t                            id = 0;
      std::optional<std::string>               note;
      std::optional<std::uint32_t>             count;
      std::optional<TsItem>                    item;
      std::variant<std::uint32_t, std::string> either;
      std::array<std::uint16_t, 3>             triple{};
      std::vector<std::vector<std::uint32_t>>  grid;
      std::vector<std::optional<std::string>>  maybe;
      TsRow                                    row;
      std::optional<std::string>               tail;
      friend bool operator==(const TsRich&, const TsRich&) = default;
      PSIO_REFLECT(TsRich, id, note, count, item, either, triple, grid, maybe, row, tail)
   };
   // The rest of what a pssz view reads: tuples, pairs, maps and sets (lists
   // of entries, with lookup), arrays of variable-size elements, an enum, a
   // nested value.
   enum class TsColor : std::uint8_t
   {
      red,
      green
   };
   struct TsShapes
   {
      std::uint32_t                                     id = 0;
      std::tuple<std::uint32_t, std::string, TsPoint>   triple;
      std::pair<std::string, std::uint16_t>             duo;
      std::map<std::string, std::uint32_t>              limits;
      std::map<std::uint32_t, TsItem>                   byNumber;
      std::set<std::int32_t>                            tags;
      std::array<TsPoint, 3>                            corners{};
      std::array<std::string, 2>                        names;
      std::vector<unsigned char>                        octets;
      TsColor                                           color = TsColor::red;
      psio::nested<TsItem>                              packed;
      std::variant<std::uint32_t, std::string, TsPoint> choice;
      std::optional<std::vector<std::uint8_t>>          maybeList;
      std::optional<std::uint32_t>                      trailing;
      friend bool operator==(const TsShapes&, const TsShapes&) = default;
      PSIO_REFLECT(TsShapes, id, triple, duo, limits, byNumber, tags, corners, names, octets, color,
                   packed, choice, maybeList, trailing)
   };
   TsShapes shapes()
   {
      TsShapes s;
      s.id        = 4;
      s.triple    = {1, "two", TsPoint{3, 4, true}};
      s.duo       = {"left", 9};
      s.limits    = {{"alpha", 1}, {"beta", 2}, {"gamma", 3}};
      s.byNumber  = {{5, {"five", 5}}, {6, {"six", 6}}};
      s.tags      = {-3, 3, 30};
      s.corners   = {TsPoint{1, 2, false}, TsPoint{3, 4, true}, TsPoint{5, 6, false}};
      s.names     = {"first", "second"};
      s.octets    = {1, 2, 3, 4, 5};
      s.color     = TsColor::green;
      s.packed    = psio::nested<TsItem>{TsItem{"in", 2}};
      s.choice    = TsPoint{7, 8, true};
      s.maybeList = std::vector<std::uint8_t>{9, 8};
      s.trailing  = 11;
      return s;
   }

   // Narrow offsets: a cap selects 1-byte offsets, a bound 2-byte ones.
   struct TsNarrow
   {
      std::uint32_t            id = 0;
      std::string              name;
      std::vector<std::string> tags;
      std::vector<TsItem>      items;
      friend bool              operator==(const TsNarrow&, const TsNarrow&) = default;
      PSIO_REFLECT(TsNarrow, id, attr(name, max<40>), attr(tags, max<8> | element(max<20>)),
                   attr(items, max<300>), definitionWillNotChange())
   };

   // Maps and sets, for the trusted decode (their views are lists of
   // entries: the list view above).
   struct TsAssoc
   {
      std::map<std::string, std::uint32_t>              byName;
      std::set<std::string>                             names;
      std::map<std::uint32_t, std::vector<std::string>> lists;
      std::set<std::uint32_t>                           ids;
      friend bool operator==(const TsAssoc&, const TsAssoc&) = default;
      PSIO_REFLECT(TsAssoc, byName, names, lists, ids)
   };
   TsAssoc assoc()
   {
      return TsAssoc{{{"a", 1}, {"bb", 2}}, {"x", "yy", "zzz"}, {{1, {"p", "q"}}, {2, {}}}, {5, 6, 7}};
   }

   TsRow row()
   {
      return TsRow{7,
                   9,
                   true,
                   "alpha",
                   "be",
                   "gamma-gamma",
                   {3, 4, true},
                   {"first", 11},
                   {{"x", 1}, {"yy", 2}, {"zzz", 3}},
                   {10, 20, 30, 40},
                   {"one", "", "three"},
                   {{1, 2, false}, {3, 4, true}}};
   }
   TsRich rich()
   {
      return TsRich{5,      "note", 77,    TsItem{"it", 2}, std::string{"alt"}, {1, 2, 3},
                    {{1, 2}, {}, {3}}, {std::string{"m"}, std::nullopt, std::string{}}, row(), "tail"};
   }
   TsNarrow narrow() { return TsNarrow{3, "narrow", {"a", "bb", ""}, {{"p", 1}, {"qq", 2}}}; }

   // ── Walking everything a view reaches ────────────────────────────────────
   template <typename X>
   inline constexpr bool is_optional_v = false;
   template <typename X>
   inline constexpr bool is_optional_v<std::optional<X>> = true;
   template <typename X>
   inline constexpr bool is_variant_v = false;
   template <typename... Xs>
   inline constexpr bool is_variant_v<std::variant<Xs...>> = true;
   template <typename X>
   inline constexpr bool is_list_v = false;
   template <typename X>
   inline constexpr bool is_list_v<std::vector<X>> = true;
   template <typename X, std::size_t N>
   inline constexpr bool is_list_v<std::array<X, N>> = true;
   template <typename X>
   inline constexpr bool is_tuple_v = false;
   template <typename... Xs>
   inline constexpr bool is_tuple_v<std::tuple<Xs...>> = true;
   template <typename A, typename B>
   inline constexpr bool is_tuple_v<std::pair<A, B>> = true;
   template <typename X>
   inline constexpr bool is_map_v = false;
   template <typename K, typename V>
   inline constexpr bool is_map_v<std::map<K, V>> = true;
   template <typename X>
   inline constexpr bool is_set_v = false;
   template <typename K>
   inline constexpr bool is_set_v<std::set<K>> = true;
   template <typename X>
   inline constexpr bool is_nested_v = false;
   template <typename X>
   inline constexpr bool is_nested_v<psio::nested<X>> = true;

   struct walker
   {
      const char*   begin = nullptr;
      const char*   end   = nullptr;
      std::uint64_t hash  = 1469598103934665603ull;
      bool          inside  = true;  // every span handed out lies in the bytes
      bool          bounded = true;  // no list announced more than the bytes hold

      void mix(std::uint64_t v) { hash = (hash ^ v) * 1099511628211ull; }
      void bytes(std::span<const char> s)
      {
         if (s.empty() || s.size() == 1)  // (1: the canonical None of a pruned optional)
            return;
         if (s.data() < begin || s.data() + s.size() > end)
         {
            inside = false;
            return;
         }
         for (const char c : s)
            mix(static_cast<unsigned char>(c));
      }

      template <typename F, typename V>
      void value(const V& v)
      {
         if constexpr (std::is_arithmetic_v<F>)
            mix(static_cast<std::uint64_t>(static_cast<F>(v)));
         else if constexpr (std::is_enum_v<F>)
            mix(static_cast<std::uint64_t>(static_cast<F>(v)));
         else if constexpr (is_tuple_v<F>)
         {
            bytes(v._psio_data());
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (value<std::tuple_element_t<Is, F>>(v.template get<Is>()), ...);
            }(std::make_index_sequence<std::tuple_size_v<F>>{});
         }
         else if constexpr (is_map_v<F> || is_set_v<F>)
         {
            // A list of entries in key order, with lookup. On bytes nobody
            // validated the order is whatever it is: the lookup must still
            // end, inside the bytes.
            const std::size_t n   = v.size();
            const std::size_t all = static_cast<std::size_t>(end - begin);
            mix(n);
            if (n > all + 16)
            {
               bounded = false;
               return;
            }
            for (std::size_t i = 0; i < n; ++i)
            {
               if constexpr (is_map_v<F>)
               {
                  value<typename F::key_type>(v[i].template get<0>());
                  value<typename F::mapped_type>(v[i].template get<1>());
               }
               else
                  value<typename F::key_type>(v[i]);
            }
            mix(v.contains(typename F::key_type{}));
         }
         else if constexpr (is_nested_v<F>)
         {
            bytes(v._psio_data());
            value<typename F::value_type>(v.get());
         }
         else if constexpr (std::is_same_v<F, std::string>)
         {
            mix(v.size());
            bytes(v._psio_data());
         }
         else if constexpr (is_optional_v<F>)
         {
            const bool has = v.has_value();
            mix(has);
            if (has)
               value<typename F::value_type>(*v);
         }
         else if constexpr (is_variant_v<F>)
         {
            mix(v.index());
            v.visit([&](const auto& alt) { value<typename std::remove_cvref_t<decltype(alt)>::element_type>(alt); });
         }
         else if constexpr (is_list_v<F>)
         {
            using E               = typename F::value_type;
            const std::size_t n   = v.size();
            const std::size_t all = static_cast<std::size_t>(end - begin);
            mix(n);
            // An element is at least one byte, or one offset.
            if (n > all + 16)
            {
               bounded = false;
               return;
            }
            for (std::size_t i = 0; i < n; ++i)
               value<E>(v[i]);
            // One past the end is a view of nothing: it reads as zero / empty.
            const auto past = v.at(n);
            if constexpr (std::is_arithmetic_v<E>)
               mix(static_cast<std::uint64_t>(static_cast<E>(past)));
            else if constexpr (requires { std::tuple_size<F>::value; })
               ;  // (an array always has N elements: at(N) is a view of nothing)
            else
               mix(past._psio_data().size());
         }
         else
         {
            bytes(v._psio_data());
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (value<std::remove_cvref_t<typename psio::reflect<F>::template member_type<Is>>>(
                   v.template get<Is>()),
                ...);
            }(std::make_index_sequence<psio::reflect<F>::member_count>{});
         }
      }
   };

   struct outcome
   {
      bool          failed = false;  // psio::codec_exception
      std::string   why;
      std::uint64_t hash = 0;
      friend bool   operator==(const outcome& a, const outcome& b)
      {
         return a.failed == b.failed && (a.failed || a.hash == b.hash);
      }
   };

   template <typename T, typename Make>
   outcome walk(std::span<const char> buffer, Make&& make)
   {
      outcome o;
      walker  w;
      w.begin = buffer.data();
      w.end   = buffer.data() + buffer.size();
      try
      {
         w.template value<T>(make());
      }
      catch (const psio::codec_exception& e)
      {
         o.failed = true;
         o.why    = e.what();
      }
      REQUIRE(w.inside);
      REQUIRE(w.bounded);
      o.hash = w.hash;
      return o;
   }

   // (TsAssoc is decoded only.)
   template <typename T>
   inline constexpr bool walkable = !std::is_same_v<T, TsAssoc>;

   struct tally
   {
      std::size_t tried = 0, valid = 0, failed = 0, value = 0;
   };

   // The bytes in an allocation of exactly their size: a read past either
   // end is a heap overflow the sanitizer build reports.
   struct exact_bytes
   {
      std::unique_ptr<char[]> p;
      std::size_t             n;
      explicit exact_bytes(std::span<const char> s) : p(new char[s.size() ? s.size() : 1]), n(s.size())
      {
         if (n)
            std::memcpy(p.get(), s.data(), n);
      }
      std::span<const char> span() const { return {p.get(), n}; }
   };

   // `decode_too`: the format's trusted decode is specified on bytes that
   // are not a frame (pssz). The ssz and canonical-ABI decoders are not:
   // their trusted decode is for validated bytes only (docs/annotations.md
   // 7.2), so only their views are walked.
   template <typename T, typename Fmt>
   void probe(Fmt fmt, std::span<const char> bytes, tally& t, bool decode_too)
   {
      const exact_bytes           copy{bytes};
      const std::span<const char> b = copy.span();
      ++t.tried;
      const bool valid = psio::validate<T>(fmt, b).ok();

      outcome safe;
      if constexpr (walkable<T>)
      {
         safe = walk<T>(b, [&] { return psio::view<T, Fmt, psio::storage::const_borrow>{b}; });
         const outcome trusted = walk<T>(
            b, [&] { return psio::view<T, Fmt, psio::storage::trusted_borrow>{psio::trusted{b}}; });
         // The two kinds place members the same way.
         REQUIRE(safe == trusted);
      }

      // The trusted decode: a value or the decoder's failure, never a read
      // outside the bytes, never a result sized by a number the bytes do
      // not back.
      bool decoded = false;
      if (!decode_too)
         decoded = valid;
      else
      try
      {
         const T value = psio::decode<T>(fmt, b);
         decoded       = true;
         if (valid)
            REQUIRE(psio::encode(fmt, value).size() <= b.size() + 8 * sizeof(T));
      }
      catch (const psio::codec_exception&)
      {
      }
      catch (const std::exception& e)
      {
         // Anything else is the decoder acting on a number it did not
         // compare with the bytes (a length, a count).
         std::string hex;
         for (const char c : b)
         {
            static const char digits[] = "0123456789abcdef";
            hex += digits[static_cast<unsigned char>(c) >> 4];
            hex += digits[static_cast<unsigned char>(c) & 15];
         }
         FAIL("the trusted decode failed with " << e.what() << " on " << hex);
      }

      if (valid)
      {
         ++t.valid;
         // Bytes that validate are a frame: nothing fails.
         INFO(safe.why);
         REQUIRE_FALSE(safe.failed);
         REQUIRE(decoded);
      }
      ((walkable<T> ? safe.failed : !decoded) ? t.failed : t.value) += 1;
   }

   std::size_t rounds()
   {
      // PSIO_TRUSTED_STRUCTURE_ROUNDS: random mutations per (format, type).
      if (const char* text = std::getenv("PSIO_TRUSTED_STRUCTURE_ROUNDS"))
         return static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
      return 4000;
   }

   template <typename T, typename Fmt, typename Bytes>
   tally mutations(Fmt fmt, const Bytes& good, std::uint64_t seed, bool decode_too = true)
   {
      const std::vector<char> g(good.begin(), good.end());
      tally                   t;
      probe<T>(fmt, g, t, decode_too);
      REQUIRE(t.valid == 1);
      // Every truncation.
      for (std::size_t n = 0; n < g.size(); ++n)
         probe<T>(fmt, std::span<const char>{g.data(), n}, t, decode_too);
      // Every byte: zero, all ones, one more, one less.
      for (std::size_t i = 0; i < g.size(); ++i)
         for (int mode = 0; mode < 4; ++mode)
         {
            std::vector<char> bad = g;
            bad[i] = mode == 0   ? char(0)
                     : mode == 1 ? char(0xff)
                     : mode == 2 ? char(bad[i] + 1)
                                 : char(bad[i] - 1);
            probe<T>(fmt, bad, t, decode_too);
         }
      // Two and four bytes of ones: an offset far outside.
      for (std::size_t width : {std::size_t{2}, std::size_t{4}})
         for (std::size_t i = 0; i + width <= g.size(); ++i)
         {
            std::vector<char> bad = g;
            std::memset(bad.data() + i, 0xff, width);
            probe<T>(fmt, bad, t, decode_too);
         }
      // Random edits: a few bytes replaced, a small number written where an
      // offset may be, a piece cut out or repeated, the tail dropped.
      std::mt19937_64 rng{seed};
      const auto      below = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
      for (std::size_t round = 0; round < rounds(); ++round)
      {
         std::vector<char> bad = g;
         for (std::size_t edits = 1 + below(4); edits > 0 && !bad.empty(); --edits)
            switch (below(6))
            {
               case 0:
                  bad[below(bad.size())] = static_cast<char>(rng());
                  break;
               case 1:
                  bad[below(bad.size())] = static_cast<char>(below(bad.size() + 2));
                  break;
               case 2:
               {
                  // A plausible offset, little-endian, 1 / 2 / 4 bytes.
                  const std::size_t   w = std::size_t{1} << below(3);
                  const std::uint32_t v = static_cast<std::uint32_t>(below(bad.size() * 2 + 2));
                  const std::size_t   i = below(bad.size());
                  for (std::size_t k = 0; k < w && i + k < bad.size(); ++k)
                     bad[i + k] = static_cast<char>(v >> (8 * k));
                  break;
               }
               case 3:
               {
                  const std::size_t i = below(bad.size());
                  bad.erase(bad.begin() + static_cast<std::ptrdiff_t>(i),
                            bad.begin() + static_cast<std::ptrdiff_t>(
                                             std::min(bad.size(), i + 1 + below(8))));
                  break;
               }
               case 4:
               {
                  const std::size_t i = below(bad.size());
                  const std::size_t n = std::min(bad.size() - i, 1 + below(8));
                  const std::vector<char> piece(bad.begin() + static_cast<std::ptrdiff_t>(i),
                                                bad.begin() + static_cast<std::ptrdiff_t>(i + n));
                  bad.insert(bad.begin() + static_cast<std::ptrdiff_t>(below(bad.size() + 1)),
                             piece.begin(), piece.end());
                  break;
               }
               default:
                  bad.resize(below(bad.size() + 1));
                  break;
            }
         probe<T>(fmt, bad, t, decode_too);
      }
      return t;
   }

   template <typename View>
   std::string failure(View&& use)
   {
      try
      {
         use();
      }
      catch (const psio::codec_exception& e)
      {
         return e.what();
      }
      return "NO FAILURE";
   }
   const std::string not_a_frame = "view: the bytes are not a frame of the type: ";
}  // namespace

// ── The cases of the report ─────────────────────────────────────────────────

TEST_CASE("a pssz view fails when a member cannot be placed", "[view][trusted_structure]")
{
   const auto note  = psio::to_pssz(TsNote{7, "abcdefgh"});
   const auto wide  = psio::to_pssz(TsWide{7, "hello", {1, 2, 3}, "opt"});
   const auto fixed = psio::to_pssz(TsFixed{1, 2, 3});

   // Each: the bytes, and what the view says. Both kinds of view say it.
   const auto both = [&]<typename T>(std::vector<char> bytes, auto&& use, const std::string& why) {
      const exact_bytes copy{bytes};
      CHECK_FALSE(psio::validate_pssz<T>(copy.span()).ok());
      CHECK(failure([&] { use(psio::pssz_view<T>{copy.span()}); }) == not_a_frame + why);
      CHECK(failure([&] { use(psio::pssz_trusted_view<T>{psio::trusted{copy.span()}}); }) ==
            not_a_frame + why);
      // The trusted decode fails too, with the decoder's message.
      CHECK(failure([&] { (void)psio::from_pssz<T>(psio::trusted{copy.span()}); }) != "NO FAILURE");
   };
   const auto cut = [](const auto& bytes, std::size_t n) {
      return std::vector<char>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
   };
   const auto with = [](const auto& bytes, std::initializer_list<std::pair<std::size_t, int>> edits) {
      std::vector<char> out(bytes.begin(), bytes.end());
      for (auto [at, value] : edits)
         out[at] = static_cast<char>(value);
      return out;
   };
   const auto sizes = [](const auto& v) { (void)v.a().size(); (void)v.b().size(); };

   // Cut to 10 bytes: inside the offsets.
   both.operator()<TsWide>(cut(wide, 10), sizes, "a record's header announces more than the record holds");
   // The first offset 0xffff.
   both.operator()<TsWide>(with(wide, {{8, 0xff}, {9, 0xff}}), sizes,
                           "the offsets of a record member are out of order or out of range");
   // No bytes at all.
   both.operator()<TsNote>({}, [](const auto& v) { (void)v.id(); }, "a record has no header");
   both.operator()<TsFixed>({}, [](const auto& v) { (void)v.y(); }, "a record has no header");
   // The header announces more than there is.
   both.operator()<TsWide>(with(wide, {{0, 0xff}}), sizes, "a record's header announces more than the record holds");
   both.operator()<TsNote>(with(note, {{0, 200}}), [](const auto& v) { (void)v.text().size(); },
                           "a record's header announces more than the record holds");
   // Offsets out of order.
   both.operator()<TsWide>(with(wide, {{12, 5}}), sizes,
                           "the offsets of a record member are out of order or out of range");
   // An offset past the end.
   both.operator()<TsNote>(with(note, {{5, 200}}), [](const auto& v) { (void)v.text().size(); },
                           "the offsets of a record member are out of order or out of range");
   // The fixed part cut.
   both.operator()<TsNote>(cut(note, 3), [](const auto& v) { (void)v.id(); },
                           "a record's header announces more than the record holds");
   both.operator()<TsFixed>(cut(fixed, 10), [](const auto& v) { (void)v.y(); },
                            "a record's header announces more than the record holds");
   // The payload cut: the list's span is no longer a whole number of elements.
   both.operator()<TsWide>(cut(wide, wide.size() - 9),
                           [](const auto& v) { (void)v.b().size(); (void)v.c().has_value(); },
                           "the offsets of a record member are out of order or out of range");
   // A list span that is not a multiple of its element size.
   both.operator()<TsWide>(with(wide, {{16, 0x2a}}), sizes,
                           "the span of a list is not a multiple of its element size");
   // A row an older version of the type wrote: a required member is missing.
   both.operator()<TsWide>(psio::to_pssz(TsOld{7, "hello"}), sizes, "a record's header omits a required member");
}

TEST_CASE("a view of nothing still reads as zero / empty", "[view][trusted_structure]")
{
   // A default-constructed view and at(i) past the end are not bytes that
   // failed to be a frame: there are no bytes.
   const psio::pssz_view<TsWide>         none;
   const psio::pssz_trusted_view<TsWide> trusted_none;
   CHECK(none.id() == 0);
   CHECK(none.a().size() == 0);
   CHECK(none.b().size() == 0);
   CHECK_FALSE(none.c().has_value());
   CHECK(trusted_none.id() == 0);
   CHECK(trusted_none.b().size() == 0);
   const auto bytes = psio::to_pssz(row());
   const psio::pssz_view<TsRow> v{std::span<const char>{bytes}};
   CHECK(v.items().at(99).qty() == 0);
   CHECK(v.items().at(99).product().size() == 0);
   CHECK(v.numbers().at(99) == 0);
   // No bytes at all are not a view of nothing either, whether the empty
   // span carries a pointer or not (an empty std::vector's may be null).
   CHECK(failure([] { (void)psio::pssz_view<TsWide>{std::span<const char>{}}.id(); }) ==
         not_a_frame + "a record has no header");
   CHECK(failure([] {
            const std::vector<char> empty;
            (void)psio::pssz_trusted_view<TsWide>{psio::trusted{empty}}.id();
         }) == not_a_frame + "a record has no header");
   // operator[] past the end is not a view of nothing: it fails.
   CHECK(failure([&] { (void)v.items()[99]; }) != "NO FAILURE");
   // A trailing optional the writer pruned is absent, not unplaceable.
   const auto pruned = psio::to_pssz(TsWide{7, "hello", {1}, std::nullopt});
   CHECK_FALSE(psio::pssz_view<TsWide>{std::span<const char>{pruned}}.c().has_value());
   // Any byte is a bool: nonzero is true.
   auto odd = psio::to_pssz(TsPoint{1, 2, true});
   odd.back() = static_cast<char>(0xff);
   CHECK(psio::pssz_view<TsPoint>{std::span<const char>{odd}}.on());
}

// ── Every mutation ──────────────────────────────────────────────────────────

TEST_CASE("pssz: views and the trusted decode on bytes that are not a frame",
          "[view][trusted_structure][fuzz]")
{
   const tally a = mutations<TsRow>(psio::pssz{}, psio::to_pssz(row()), 1);
   const tally b = mutations<TsRich>(psio::pssz{}, psio::to_pssz(rich()), 2);
   const tally c = mutations<TsNarrow>(psio::pssz{}, psio::to_pssz(narrow()), 3);
   const tally d = mutations<TsRow>(psio::pssz_<2>{}, psio::encode(psio::pssz_<2>{}, row()), 4);
   const tally e = mutations<TsAssoc>(psio::pssz{}, psio::to_pssz(assoc()), 7);
   const tally f = mutations<TsShapes>(psio::pssz{}, psio::to_pssz(shapes()), 8);
   for (const tally& t : {a, b, c, d, e, f})
   {
      CHECK(t.failed > t.tried / 10);  // most mutations are not a frame ...
      CHECK(t.value > 0);              // ... and some still place every member
      CHECK(t.valid > 0);
   }
}

TEST_CASE("ssz: views on bytes that are not a frame",
          "[view][trusted_structure][fuzz]")
{
   const tally t = mutations<TsRow>(psio::ssz{}, psio::to_ssz(row()), 5, false);
   CHECK(t.failed > t.tried / 10);
   CHECK(t.value > 0);
}

TEST_CASE("canonical ABI: views on bytes that are not a frame",
          "[view][trusted_structure][fuzz]")
{
   const tally t = mutations<TsRow>(psio::wit{}, psio::to_wit(row()), 6, false);
   CHECK(t.failed > t.tried / 10);
   CHECK(t.value > 0);
}
