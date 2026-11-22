// psio::key — tuples, composite-key helpers, and the per-type contract
// (docs/key-spec.md): every encoding is order-preserving and prefix-free, so
// tuples concatenate, tuple<A, B> is a byte-prefix of tuple<A, B, C>, and
// reading is checked.

#include <psio/key.hpp>

#include <catch.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace
{
   using Bytes = std::vector<char>;

   // memcmp order — the order a key-value store compares keys in.
   bool bytes_less(const Bytes& a, const Bytes& b)
   {
      const std::size_t n = std::min(a.size(), b.size());
      const int         c = n ? std::memcmp(a.data(), b.data(), n) : 0;
      return c != 0 ? c < 0 : a.size() < b.size();
   }

   bool starts_with(const Bytes& k, const Bytes& prefix)
   {
      return k.size() >= prefix.size() &&
             (prefix.empty() || std::memcmp(k.data(), prefix.data(), prefix.size()) == 0);
   }

   Bytes hex(std::string_view text)
   {
      Bytes out;
      auto  nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
      for (std::size_t i = 0; i + 1 < text.size(); i += 2)
         out.push_back(static_cast<char>(nib(text[i]) << 4 | nib(text[i + 1])));
      return out;
   }

   template <typename T>
   Bytes enc(const T& v)
   {
      return psio::encode(psio::key{}, v);
   }

   template <typename T>
   bool valid(const Bytes& b)
   {
      return psio::validate<T>(psio::key{}, std::span<const char>{b}).ok();
   }

   template <typename T>
   T dec(const Bytes& b)
   {
      return psio::decode<T>(psio::key{}, std::span<const char>{b});
   }

   enum class Color : std::uint8_t { red = 1, green = 2, blue = 200 };
   enum class Level : std::int16_t { low = -5, mid = 0, high = 300 };

   struct Account
   {
      std::uint32_t                 shard = 0;
      std::string                   owner;
      std::optional<std::int64_t>   seq;
      friend bool operator==(const Account&, const Account&)  = default;
      friend auto operator<=>(const Account&, const Account&) = default;
   };
   PSIO_REFLECT(Account, shard, owner, seq)

   // A custom key type through the sortable adapter slot: three raw bytes.
   struct Tag3
   {
      std::array<std::uint8_t, 3> b{};
      friend bool operator==(const Tag3&, const Tag3&)  = default;
      friend auto operator<=>(const Tag3&, const Tag3&) = default;
   };
   struct tag3_codec
   {
      static std::size_t packsize(const Tag3&) noexcept { return 3; }
      static void        encode(const Tag3& v, std::vector<char>& s)
      {
         s.insert(s.end(), v.b.begin(), v.b.end());
      }
      static Tag3 decode(std::span<const char> s) noexcept
      {
         Tag3 out;
         std::memcpy(out.b.data(), s.data(), 3);
         return out;
      }
      static psio::codec_status validate(std::span<const char> s) noexcept
      {
         return s.size() < 3 ? psio::codec_fail("Tag3: short", 0, "tag3") : psio::codec_ok();
      }
      static psio::codec_status validate_strict(std::span<const char> s) noexcept
      {
         return validate(s);
      }
   };
}  // namespace
PSIO_ADAPTER(Tag3, psio::sortable_binary_category, tag3_codec)

namespace
{
   // ── Random values that stress the framing ──────────────────────────────
   struct Gen
   {
      std::mt19937_64 rng{0x9e3779b97f4a7c15ull};

      std::uint64_t u(std::uint64_t n) { return rng() % n; }

      template <typename I>
         requires std::is_integral_v<I>
      void operator()(I& v)
      {
         // Edges as often as arbitrary values.
         static constexpr I edges[] = {std::numeric_limits<I>::min(),
                                       static_cast<I>(std::numeric_limits<I>::min() + 1),
                                       static_cast<I>(-1),
                                       0,
                                       1,
                                       static_cast<I>(std::numeric_limits<I>::max() - 1),
                                       std::numeric_limits<I>::max()};
         v = u(2) ? edges[u(std::size(edges))] : static_cast<I>(rng());
      }
      void operator()(bool& v) { v = u(2); }
      void operator()(double& v)
      {
         static const double edges[] = {0.0,
                                        -0.0,
                                        1.0,
                                        -1.0,
                                        std::numeric_limits<double>::infinity(),
                                        -std::numeric_limits<double>::infinity(),
                                        std::numeric_limits<double>::denorm_min(),
                                        -std::numeric_limits<double>::denorm_min(),
                                        std::numeric_limits<double>::max(),
                                        std::numeric_limits<double>::lowest()};
         v = u(2) ? edges[u(std::size(edges))]
                  : std::ldexp(static_cast<double>(static_cast<std::int64_t>(rng())),
                               static_cast<int>(u(200)) - 100);
      }
      void operator()(float& v)
      {
         double d;
         (*this)(d);
         v = static_cast<float>(d);
      }
      void operator()(std::string& v)
      {
         // Short strings over a tiny alphabet, so that shared prefixes,
         // embedded 00 / 01 / FF bytes and the empty string are all common.
         static constexpr char alphabet[] = {'\0', '\1', '\2', 'a', 'b', '\x7f', '\x80', '\xff'};
         v.clear();
         for (std::size_t n = u(5); n > 0; --n)
            v.push_back(alphabet[u(std::size(alphabet))]);
      }
      void operator()(Color& v) { v = static_cast<Color>(u(256)); }
      void operator()(Level& v) { v = static_cast<Level>(static_cast<std::int16_t>(rng())); }
      void operator()(Tag3& v)
      {
         for (auto& b : v.b)
            b = static_cast<std::uint8_t>(u(3) ? u(256) : (u(2) ? 0 : 255));
      }
      template <typename T>
      void operator()(std::optional<T>& v)
      {
         if (u(3) == 0)
            v.reset();
         else
            (*this)(v.emplace());
      }
      template <typename T>
      void operator()(std::vector<T>& v)
      {
         v.clear();
         for (std::size_t n = u(4); n > 0; --n)
         {
            T e{};
            (*this)(e);
            v.push_back(std::move(e));
         }
      }
      template <typename T, std::size_t N>
      void operator()(std::array<T, N>& v)
      {
         for (auto& e : v)
            (*this)(e);
      }
      template <typename Rep, typename Period>
      void operator()(std::chrono::duration<Rep, Period>& v)
      {
         Rep r;
         (*this)(r);
         v = std::chrono::duration<Rep, Period>{r};
      }
      template <typename D>
      void operator()(std::chrono::sys_time<D>& v)
      {
         D d;
         (*this)(d);
         v = std::chrono::sys_time<D>{d};
      }
      template <typename... Ts>
      void operator()(std::variant<Ts...>& v)
      {
         const std::size_t i = u(sizeof...(Ts));
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (void)((i == Is ? ((*this)(v.template emplace<Is>()), true) : false) || ...);
         }(std::index_sequence_for<Ts...>{});
      }
      template <typename... Ts>
      void operator()(std::tuple<Ts...>& v)
      {
         std::apply([&](auto&... e) { ((*this)(e), ...); }, v);
      }
      void operator()(Account& v)
      {
         (*this)(v.shard);
         (*this)(v.owner);
         (*this)(v.seq);
      }
   };

   // The whole contract for one key type, over `count` random values:
   //   - order:      a < b  <=>  key(a) <memcmp key(b);  a == b <=> same bytes
   //   - prefix-free: no key is a proper prefix of another
   //   - round trip, validate, size_of
   template <typename T>
   void check_order(std::size_t count = 600)
   {
      Gen                               gen;
      std::vector<std::pair<T, Bytes>> items;
      for (std::size_t i = 0; i < count; ++i)
      {
         T v{};
         gen(v);
         Bytes k = enc(v);
         REQUIRE(psio::size_of(psio::key{}, v) == k.size());
         REQUIRE(valid<T>(k));
         REQUIRE(enc(dec<T>(k)) == k);
         // Every key truncated by one byte is rejected (prefix-free).
         if (!k.empty())
            REQUIRE_FALSE(valid<T>(Bytes(k.begin(), k.end() - 1)));
         items.emplace_back(std::move(v), std::move(k));
      }
      for (std::size_t i = 0; i < items.size(); ++i)
         for (std::size_t j = 0; j < items.size(); j += 1 + (i + j) % 7)
         {
            const auto& [a, ka] = items[i];
            const auto& [b, kb] = items[j];
            REQUIRE((a < b) == bytes_less(ka, kb));
            REQUIRE((a == b) == (ka == kb));
            if (ka != kb)
               REQUIRE_FALSE(starts_with(kb, ka));
         }
      // Sorting by bytes is sorting by value.
      auto by_value = items, by_bytes = items;
      std::stable_sort(by_value.begin(), by_value.end(),
                       [](const auto& x, const auto& y) { return x.first < y.first; });
      std::stable_sort(by_bytes.begin(), by_bytes.end(),
                       [](const auto& x, const auto& y) { return bytes_less(x.second, y.second); });
      for (std::size_t i = 0; i < items.size(); ++i)
         REQUIRE(by_value[i].second == by_bytes[i].second);
   }

   // key(tuple<first K>) is a byte-prefix of key(full tuple), and equals
   // key_prefix<Full>(first K...).
   template <typename... Ts>
   void check_prefixes(const std::tuple<Ts...>& full)
   {
      using Full            = std::tuple<Ts...>;
      const Bytes full_key = enc(full);
      [&]<std::size_t... Ks>(std::index_sequence<Ks...>)
      {
         auto one = [&]<std::size_t K>(std::integral_constant<std::size_t, K>) {
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               const std::tuple<std::tuple_element_t<Is, Full>...> head{std::get<Is>(full)...};
               const Bytes                                         head_key = enc(head);
               REQUIRE(starts_with(full_key, head_key));
               REQUIRE(psio::key_prefix<Full>(std::get<Is>(full)...) == head_key);
               // The scan range [prefix, bound) holds the full key.
               REQUIRE_FALSE(bytes_less(full_key, head_key));
               if (auto bound = psio::key_prefix_upper_bound(head_key))
                  REQUIRE(bytes_less(full_key, *bound));
            }(std::make_index_sequence<K>{});
         };
         (one(std::integral_constant<std::size_t, Ks>{}), ...);
      }(std::make_index_sequence<sizeof...(Ts) + 1>{});
   }
}  // namespace

TEST_CASE("key: a tuple is its elements concatenated", "[key][tuple]")
{
   REQUIRE(enc(std::tuple<>{}).empty());
   REQUIRE(psio::size_of(psio::key{}, std::tuple<>{}) == 0);
   REQUIRE(valid<std::tuple<>>({}));
   REQUIRE_FALSE(valid<std::tuple<>>(hex("00")));
   REQUIRE(dec<std::tuple<>>({}) == std::tuple<>{});

   using T = std::tuple<std::uint16_t, std::string, std::int32_t, std::optional<bool>>;
   const T t{0x0102, std::string("a\0b", 3), -2, true};
   //            u16   'a' 00->0001 'b' 0000    i32 sign-flipped   some true
   REQUIRE(enc(t) == hex("0102" "61000162" "0000" "7ffffffe" "0101"));
   REQUIRE(dec<T>(enc(t)) == t);

   // Exactly the bytes of the elements one after another, and of a record
   // with the same fields.
   Bytes cat;
   std::apply([&](const auto&... e) { (psio::encode(psio::key{}, e, cat), ...); }, t);
   REQUIRE(enc(t) == cat);
   const Account a{7, "bob", -1};
   REQUIRE(enc(a) == enc(std::tuple{a.shard, a.owner, a.seq}));

   // Nested tuples flatten; tuples of references encode like values.
   REQUIRE(enc(std::tuple{std::tuple{std::uint8_t{1}, std::tuple<>{}}, std::string{"x"}}) ==
           hex("01" "780000"));
   const std::string  owner = "bob";
   const std::uint32_t shard = 7;
   REQUIRE(enc(std::tie(shard, owner)) == enc(std::tuple{shard, owner}));

   // A tuple of fixed-width elements is a fixed-size key: to_key is an array.
   const auto fixed = psio::to_key(std::tuple{std::uint32_t{1}, Color::green, std::int16_t{-1}});
   static_assert(std::is_same_v<decltype(fixed), const std::array<char, 7>>);
   REQUIRE(Bytes(fixed.begin(), fixed.end()) == hex("00000001" "02" "7fff"));
   const auto var = psio::to_key(std::tuple{std::uint32_t{1}, std::string{"a"}});
   static_assert(std::is_same_v<decltype(var), const Bytes>);
   REQUIRE(var == hex("00000001" "610000"));
}

TEST_CASE("key: byte order is value order, for every element kind", "[key][order]")
{
   check_order<std::uint8_t>();
   check_order<std::int8_t>();
   check_order<std::uint64_t>();
   check_order<std::int64_t>();
   check_order<bool>(40);
   check_order<double>();
   check_order<float>();
   check_order<Color>();
   check_order<Level>();
   check_order<std::string>();
   check_order<std::optional<std::string>>();
   check_order<std::optional<std::optional<std::uint8_t>>>();
   check_order<std::vector<std::uint8_t>>();
   check_order<std::vector<std::int8_t>>();
   check_order<std::vector<std::int16_t>>();
   check_order<std::vector<std::string>>();
   check_order<std::vector<std::vector<std::uint8_t>>>();
   check_order<std::vector<std::optional<std::uint8_t>>>();
   check_order<std::vector<bool>>();
   check_order<std::array<std::int16_t, 3>>();
   check_order<std::array<std::string, 2>>();
   check_order<std::variant<std::uint8_t, std::string, std::int32_t>>();
   check_order<Tag3>();
   check_order<Account>();
   check_order<std::chrono::sys_seconds>();
   check_order<std::chrono::sys_time<std::chrono::microseconds>>();
   check_order<std::chrono::milliseconds>();
   check_order<std::chrono::duration<std::uint32_t>>();
}

TEST_CASE("key: tuples order lexicographically by element", "[key][tuple][order]")
{
   // Variable-length elements followed by more elements are the hard case:
   // the framing of the string / vector / optional must not let a later
   // element decide before an earlier one.
   check_order<std::tuple<std::string, std::uint8_t>>(900);
   check_order<std::tuple<std::string, std::string>>(900);
   check_order<std::tuple<std::vector<std::uint8_t>, std::int8_t>>(900);
   check_order<std::tuple<std::vector<std::int16_t>, std::int8_t>>(900);
   check_order<std::tuple<std::vector<std::string>, std::string>>(900);
   check_order<std::tuple<std::optional<std::string>, std::string>>(900);
   check_order<std::tuple<std::optional<std::uint8_t>, std::uint8_t>>(900);
   check_order<std::tuple<std::uint32_t, std::string, std::optional<std::int64_t>>>(900);
   check_order<std::tuple<Color, Level, std::string, double>>(900);
   check_order<std::tuple<std::int64_t, std::uint64_t, bool>>(900);
   check_order<std::tuple<std::string, Tag3, std::vector<std::uint8_t>, std::int32_t>>(900);
   check_order<std::tuple<Account, std::string>>(900);
   check_order<std::tuple<std::variant<std::string, std::uint8_t>, std::uint8_t>>(900);
   check_order<std::tuple<std::chrono::sys_seconds, std::string, std::chrono::nanoseconds>>(900);
   check_order<std::tuple<std::tuple<std::string, std::uint8_t>, std::tuple<>, std::string>>(900);
}

TEST_CASE("key: the key of a leading sub-tuple is a prefix of the full key", "[key][tuple][prefix]")
{
   Gen gen;
   for (int i = 0; i < 300; ++i)
   {
      std::tuple<std::uint32_t, std::string, std::optional<std::int64_t>, std::vector<std::string>,
                 double>
         a;
      gen(a);
      check_prefixes(a);
      std::tuple<std::string, std::vector<std::uint8_t>, Color, std::optional<std::string>, Tag3,
                 std::chrono::sys_seconds>
         b;
      gen(b);
      check_prefixes(b);
   }
}

TEST_CASE("key_prefix: elements are encoded as the key type's elements", "[key][prefix]")
{
   using K = std::tuple<std::uint32_t, std::string, std::optional<std::int64_t>>;

   REQUIRE(psio::key_prefix<K>().empty());
   // An int argument is the key's uint32, not four sign-flipped bytes.
   REQUIRE(psio::key_prefix<K>(7) == hex("00000007"));
   REQUIRE(psio::key_prefix<K>(7) != enc(7));
   // A literal / string_view is the key's std::string.
   REQUIRE(psio::key_prefix<K>(7, "bob") == hex("00000007" "626f620000"));
   REQUIRE(psio::key_prefix<K>(7, std::string_view{"bob"}) == psio::key_prefix<K>(7, "bob"));
   // A bare value and nullopt are the key's optional.
   REQUIRE(psio::key_prefix<K>(7, "bob", 5) == enc(K{7, "bob", 5}));
   REQUIRE(psio::key_prefix<K>(7, "bob", std::nullopt) == enc(K{7, "bob", std::nullopt}));

   // A reflected record is a composite key too: its fields are the elements.
   REQUIRE(psio::key_prefix<Account>(7, "bob") == psio::key_prefix<K>(7, "bob"));
   REQUIRE(psio::key_prefix<Account>(7, "bob", 5) == enc(Account{7, "bob", 5}));

   // The compile-time contract: arity and per-position convertibility.
   static_assert(psio::detail::key_impl::Composite<K>);
   static_assert(psio::detail::key_impl::Composite<Account>);
   static_assert(!psio::detail::key_impl::Composite<std::uint32_t>);
}

TEST_CASE("key_prefix_upper_bound: the exclusive end of a prefix scan", "[key][prefix]")
{
   auto bound = [](std::string_view h) { return psio::key_prefix_upper_bound(hex(h)); };

   REQUIRE(bound("") == std::nullopt);          // every key
   REQUIRE(bound("ff") == std::nullopt);        // no byte string is above every ff…
   REQUIRE(bound("ffffff") == std::nullopt);
   REQUIRE(bound("00") == hex("01"));
   REQUIRE(bound("1234") == hex("1235"));
   REQUIRE(bound("12ff") == hex("13"));         // trailing ff bytes are dropped
   REQUIRE(bound("12ffff") == hex("13"));
   REQUIRE(bound("fffe") == hex("ffff"));
   REQUIRE(bound("00ff") == hex("01"));
   REQUIRE(bound("7f") == hex("80"));           // no signed-char surprise
   REQUIRE(bound("fe") == hex("ff"));

   // Definition: starts_with(k, p)  <=>  p <= k < bound(p), for all k.
   std::mt19937_64 rng{42};
   auto            random_bytes = [&](std::size_t max) {
      static constexpr unsigned char alphabet[] = {0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff, 0xff};
      Bytes out;
      for (std::size_t n = rng() % (max + 1); n > 0; --n)
         out.push_back(static_cast<char>(alphabet[rng() % std::size(alphabet)]));
      return out;
   };
   for (int i = 0; i < 20000; ++i)
   {
      const Bytes p = random_bytes(3), k = random_bytes(5);
      const auto  b = psio::key_prefix_upper_bound(p);
      const bool  in_range = !bytes_less(k, p) && (!b || bytes_less(k, *b));
      REQUIRE(starts_with(k, p) == in_range);
      if (b)
      {
         // The smallest such bound: it does not itself start with p.
         REQUIRE(bytes_less(p, *b));
         REQUIRE_FALSE(starts_with(*b, p));
      }
   }
}

TEST_CASE("key: a prefix scan over a sorted store returns exactly the matching rows",
          "[key][prefix]")
{
   using K = std::tuple<std::uint32_t, std::string, std::optional<std::int64_t>>;
   struct Less
   {
      bool operator()(const Bytes& a, const Bytes& b) const { return bytes_less(a, b); }
   };
   std::map<Bytes, K, Less> store;
   Gen                      gen;
   for (int i = 0; i < 3000; ++i)
   {
      K k;
      gen(k);
      std::get<0>(k) %= 3;  // few shards, so prefixes select many rows
      store.emplace(enc(k), k);
   }
   auto scan = [&](const Bytes& prefix) {
      std::vector<K> out;
      const auto     bound = psio::key_prefix_upper_bound(prefix);
      const auto     last  = bound ? store.lower_bound(*bound) : store.end();
      for (auto it = store.lower_bound(prefix); it != last; ++it)
         out.push_back(dec<K>(it->first));
      return out;
   };
   auto expect = [&](auto&& pred) {
      std::vector<K> out;
      for (const auto& [bytes, k] : store)
         if (pred(k))
            out.push_back(k);
      return out;
   };
   REQUIRE(scan(psio::key_prefix<K>()).size() == store.size());
   for (std::uint32_t shard = 0; shard < 4; ++shard)
   {
      REQUIRE(scan(psio::key_prefix<K>(shard)) ==
              expect([&](const K& k) { return std::get<0>(k) == shard; }));
      for (std::string owner : {std::string{}, std::string{"a"}, std::string("\0", 1),
                                std::string{"\xff"}, std::string{"\xff\xff"}, std::string{"a\xff"}})
         REQUIRE(scan(psio::key_prefix<K>(shard, owner)) == expect([&](const K& k) {
                    return std::get<0>(k) == shard && std::get<1>(k) == owner;
                 }));
   }
   // Rows come back in tuple order.
   const auto all = scan({});
   REQUIRE(std::is_sorted(all.begin(), all.end()));
}

TEST_CASE("key: enums are their underlying integer", "[key][enum]")
{
   REQUIRE(enc(Color::blue) == hex("c8"));
   REQUIRE(enc(Level::low) == enc(std::int16_t{-5}));
   REQUIRE(enc(Level::low) == hex("7ffb"));
   REQUIRE(dec<Level>(hex("812c")) == Level::high);
   // Any value of the underlying type is a value of the enum.
   REQUIRE(valid<Color>(hex("ff")));
   REQUIRE(bytes_less(enc(Level::low), enc(Level::mid)));
   REQUIRE(bytes_less(enc(Level::mid), enc(Level::high)));
}

TEST_CASE("key: strings, bounded lists and byte arrays", "[key][shapes]")
{
   // utf8_string is a string; its bound and UTF-8 are checked on read.
   using U = psio::utf8_string<4>;
   REQUIRE(enc(U{"h\xc3\xa9"}) == enc(std::string{"h\xc3\xa9"}));
   REQUIRE(dec<U>(enc(U{"abcd"})) == U{"abcd"});
   REQUIRE_FALSE(valid<U>(enc(std::string{"abcde"})));
   REQUIRE_FALSE(valid<U>(enc(std::string{"\xff"})));
   REQUIRE(valid<psio::utf8_string<>>(enc(std::string{"abcde"})));

   // bounded<T, N> is a list with a checked bound.
   using B = psio::bounded<std::uint16_t, 2>;
   REQUIRE(enc(B{1, 2}) == enc(std::vector<std::uint16_t>{1, 2}));
   REQUIRE(enc(B{1, 2}) == hex("010001" "010002" "00"));
   REQUIRE(dec<B>(enc(B{1, 2})).storage() == std::vector<std::uint16_t>{1, 2});
   REQUIRE_FALSE(valid<B>(enc(std::vector<std::uint16_t>{1, 2, 3})));
   using BB = psio::bounded<std::uint8_t, 2>;
   REQUIRE(enc(BB{0, 9}) == hex("000109" "0000"));
   REQUIRE_FALSE(valid<BB>(enc(std::vector<std::uint8_t>{1, 2, 3})));

   // Octet lists share the string framing. char and std::byte are raw
   // bytes on every platform; int8_t is sign-flipped so that it sorts.
   REQUIRE(enc(std::vector<char>{'a', '\0', '\xff'}) == enc(std::string("a\0\xff", 3)));
   REQUIRE(enc(std::vector<std::byte>{std::byte{0x61}}) == hex("610000"));
   REQUIRE(enc(std::vector<std::uint8_t>{0x61, 0}) == hex("610001" "0000"));
   REQUIRE(enc(std::vector<std::int8_t>{-128, 0, 127}) == hex("0001" "80" "ff" "0000"));
   REQUIRE(dec<std::vector<std::int8_t>>(hex("000180ff0000")) ==
           std::vector<std::int8_t>{-128, 0, 127});
   REQUIRE(enc('a') == hex("61"));
   REQUIRE(enc('\xff') == hex("ff"));

   // Fixed-count sequences have no framing.
   REQUIRE(enc(std::array<std::uint16_t, 2>{1, 2}) == hex("00010002"));
   REQUIRE(enc(std::array<std::uint8_t, 3>{0, 0, 9}) == hex("000009"));
   REQUIRE(enc(std::array<std::uint8_t, 0>{}).empty());
   psio::byte_array<3> ba;
   ba[0] = 0;
   ba[1] = 0xff;
   ba[2] = 7;
   REQUIRE(enc(ba) == hex("00ff07"));
   REQUIRE(dec<psio::byte_array<3>>(hex("00ff07")).storage() == ba.storage());
   static_assert(psio::detail::key_impl::key_fixed_size<psio::byte_array<3>>() == 3);
   static_assert(psio::detail::key_impl::key_fixed_size<std::array<std::uint16_t, 2>>() == 4);

   REQUIRE(enc(std::vector<bool>{true, false}) == hex("0101" "0100" "00"));
   REQUIRE(dec<std::vector<bool>>(hex("0101010000")) == std::vector<bool>{true, false});
}

TEST_CASE("key: empty, absent and zero are all distinct and ordered", "[key][order]")
{
   using O = std::optional<std::string>;
   const Bytes none = enc(O{}), empty = enc(O{""}), zero = enc(O{std::string(1, '\0')});
   REQUIRE(none == hex("00"));
   REQUIRE(empty == hex("010000"));
   REQUIRE(zero == hex("0100010000"));
   REQUIRE(bytes_less(none, empty));
   REQUIRE(bytes_less(empty, zero));

   using V = std::vector<std::optional<std::uint8_t>>;
   REQUIRE(enc(V{}) == hex("00"));
   REQUIRE(enc(V{std::nullopt}) == hex("0100" "00"));
   REQUIRE(enc(V{std::uint8_t{0}}) == hex("010100" "00"));
   REQUIRE(bytes_less(enc(V{}), enc(V{std::nullopt})));
   REQUIRE(bytes_less(enc(V{std::nullopt}), enc(V{std::uint8_t{0}})));
   REQUIRE(bytes_less(enc(V{std::nullopt}), enc(V{std::nullopt, std::nullopt})));
}

TEST_CASE("key: floats — the two places byte order is not operator<", "[key][float]")
{
   const double inf = std::numeric_limits<double>::infinity();
   const double nan = std::numeric_limits<double>::quiet_NaN();

   // -0 and +0 are equal values and one key; the key decodes to +0.
   REQUIRE(enc(-0.0) == enc(0.0));
   REQUIRE_FALSE(std::signbit(dec<double>(enc(-0.0))));
   REQUIRE_FALSE(valid<double>(hex("7fffffffffffffff")));  // the image of -0
   REQUIRE_FALSE(valid<float>(hex("7fffffff")));

   // NaN is unordered under operator<; its key has a place: a NaN with the
   // sign bit clear is above +inf, one with it set is below -inf. Payload
   // bits are kept, so distinct NaNs are distinct keys.
   REQUIRE(bytes_less(enc(inf), enc(nan)));
   REQUIRE(bytes_less(enc(-nan), enc(-inf)));
   REQUIRE(std::isnan(dec<double>(enc(nan))));
   REQUIRE(std::signbit(dec<double>(enc(-nan))));
   REQUIRE(valid<double>(enc(nan)));
}

TEST_CASE("key: reading is checked", "[key][validate]")
{
   using K = std::tuple<std::uint16_t, std::string, std::optional<bool>, std::vector<std::uint16_t>>;
   const K     k{1, "ab", true, {5}};
   const Bytes good = enc(k);
   REQUIRE(good == hex("0001" "61620000" "0101" "010005" "00"));
   REQUIRE(valid<K>(good));

   auto rejects = [](std::string_view h) {
      const Bytes b = hex(h);
      if (valid<K>(b))
         return false;
      REQUIRE_THROWS_AS(dec<K>(b), psio::codec_exception);
      return true;
   };
   // Every strict prefix, and any trailing byte.
   for (std::size_t n = 0; n < good.size(); ++n)
      REQUIRE_FALSE(valid<K>(Bytes(good.begin(), good.begin() + n)));
   REQUIRE(rejects("0001" "61620000" "0101" "010005" "00" "00"));
   REQUIRE(rejects("0001" "61620002" "0101" "010005" "00"));  // 00 02: no such escape
   REQUIRE(rejects("0001" "616200"));                         // unterminated string
   REQUIRE(rejects("0001" "61620000" "02"));                  // optional marker
   REQUIRE(rejects("0001" "61620000" "0102" "00"));           // bool
   REQUIRE(rejects("0001" "61620000" "0101" "020005" "00"));  // list marker
   REQUIRE(rejects("0001" "61620000" "0101" "0100"));         // truncated element

   REQUIRE_FALSE(valid<bool>(hex("02")));
   REQUIRE_FALSE(valid<std::uint32_t>(hex("000000")));
   REQUIRE_FALSE(valid<std::uint32_t>(hex("0000000000")));
   REQUIRE_FALSE(valid<std::variant<std::uint8_t, bool>>(hex("0200")));
   REQUIRE(valid<std::variant<std::uint8_t, bool>>(hex("0101")));
   REQUIRE_FALSE(valid<psio::bitvector<4>>(hex("10")));  // padding bit set
   REQUIRE(valid<psio::bitvector<4>>(hex("0f")));
   REQUIRE_FALSE(valid<Tag3>(hex("0102")));
   REQUIRE_FALSE(valid<Tag3>(hex("01020304")));
   REQUIRE(valid<std::tuple<Tag3, std::uint8_t>>(hex("01020304")));

   // Total on arbitrary bytes: never reads out of bounds, never accepts a
   // non-canonical encoding (what it accepts re-encodes to itself).
   std::mt19937_64 rng{7};
   for (int i = 0; i < 20000; ++i)
   {
      Bytes b = good;
      for (int flips = 1 + static_cast<int>(rng() % 3); flips > 0; --flips)
         b[rng() % b.size()] = static_cast<char>(rng() % 4 == 0 ? rng() : rng() % 3);
      if (rng() % 4 == 0)
         b.resize(rng() % (b.size() + 1));
      if (valid<K>(b))
         REQUIRE(enc(dec<K>(b)) == b);
   }
}

TEST_CASE("key: time values order chronologically", "[key][time]")
{
   using namespace std::chrono;
   // The tick count, sign-flipped big-endian, at the width of Rep.
   REQUIRE(enc(sys_seconds{seconds{0}}) == hex("8000000000000000"));
   REQUIRE(enc(sys_seconds{seconds{-1}}) == hex("7fffffffffffffff"));
   REQUIRE(enc(milliseconds{1}) == hex("8000000000000001"));
   REQUIRE(enc(duration<std::uint32_t>{5}) == hex("00000005"));
   REQUIRE(enc(sys_time<duration<std::int32_t>>{duration<std::int32_t>{-2}}) == hex("7ffffffe"));
   REQUIRE(dec<sys_seconds>(hex("7fffffffffffffff")) == sys_seconds{seconds{-1}});
   static_assert(psio::detail::key_impl::key_fixed_size<sys_seconds>() == 8);
   static_assert(
      psio::detail::key_impl::key_fixed_size<std::tuple<sys_seconds, std::uint32_t>>() == 12);

   const sys_seconds before{seconds{-86400}}, epoch{}, after{seconds{1795342530}};
   REQUIRE(bytes_less(enc(before), enc(epoch)));
   REQUIRE(bytes_less(enc(epoch), enc(after)));
}
