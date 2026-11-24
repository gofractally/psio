// psio::flatbuf on bytes that are not a valid buffer.
//
// What is pinned, for every truncation, every single-byte corruption and a
// few hundred thousand random corruptions of encoded values, and for
// buffers of random bytes:
//
//   - `validate_flatbuf` returns (it does not crash, loop or read outside
//     the bytes — the last is what the sanitizer build of this file checks);
//   - the SAFE view can be walked in full whatever the bytes are, and every
//     string / vector it hands out lies inside the buffer;
//   - when the bytes validate, the TRUSTED view and the trusted decode read
//     the same values as the safe view, and the value re-encodes to a
//     buffer that validates and decodes to itself.
//
// The same file is built a second time with AddressSanitizer and
// UndefinedBehaviorSanitizer where the toolchain has them
// (psio_flatbuf_fuzz_tests_sanitized).

#include <psio/flatbuf.hpp>
#include <psio/flatbuf_view.hpp>

#include <catch.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "flatbuf_shapes.hpp"

namespace
{
   using namespace psio::detail::flatbuf_impl;

   // Walks everything a view reaches, folding what it reads into a hash.
   struct walker
   {
      const char*   begin  = nullptr;
      const char*   end    = nullptr;
      std::uint64_t hash   = 1469598103934665603ull;
      // Values visited. A corrupted buffer may reach one sub-object along
      // very many paths; the safe view is lazy, so the walk has to stop itself.
      std::size_t   budget = 4000;
      bool          inside = true;

      void mix(std::uint64_t v) { hash = (hash ^ v) * 1099511628211ull; }
      void bytes(std::span<const char> s)
      {
         if (s.empty())
            return;
         if (s.data() < begin || s.data() + s.size() > end)
         {
            inside = false;
            return;
         }
         for (const char c : s)
            mix(static_cast<unsigned char>(c));
      }
      bool spend()
      {
         if (budget == 0)
            return false;
         --budget;
         return true;
      }

      template <typename F, typename V>
      void value(const V& v)
      {
         if (!spend())
            return;
         if constexpr (Scalar<F>)
         {
            if constexpr (std::is_floating_point_v<F>)
            {
               std::uint64_t bits = 0;
               std::memcpy(&bits, &v, sizeof(F));
               mix(bits);
            }
            else
               mix(static_cast<std::uint64_t>(v));
         }
         else if constexpr (std::is_same_v<F, std::string>)
         {
            mix(v.size());
            bytes(v._psio_data());
         }
         else if constexpr (is_optional_v<F>)
         {
            mix(v.has_value());
            if (v.has_value())
               value<typename F::value_type>(*v);
         }
         else if constexpr (is_variant_v<F>)
         {
            mix(v.index());
            v.visit([&](const auto& alt) {
               using A = std::remove_cvref_t<decltype(alt)>;
               if constexpr (!std::is_same_v<A, std::monostate>)
                  value<typename A::element_type>(alt);
            });
         }
         else if constexpr (is_map_v<F>)
         {
            mix(v.size());
            for (std::size_t i = 0; i < v.size() && budget; ++i)
            {
               value<typename F::key_type>(v.key(i));
               value<typename F::mapped_type>(v.value(i));
            }
            // Lookup must terminate and stay inside whatever the order is.
            if (v.size())
               mix(v.find(typename F::key_type{}));
         }
         else if constexpr (is_vector_v<F> || is_array_v<F> || is_set_v<F>)
         {
            using E = typename F::value_type;
            mix(v.size());
            if constexpr (Scalar<E> || is_struct_v<E>)
               bytes(v._psio_data());
            for (std::size_t i = 0; i < v.size() && budget; ++i)
               value<E>(v[i]);
            value<E>(v.at(v.size()));  // one past the end
            if constexpr (is_set_v<F>)
            {
               if (v.size())
                  mix(v.find(E{}));
            }
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

   template <typename T, typename View>
   walker walk(std::span<const char> buffer, const View& v)
   {
      walker w;
      w.begin = buffer.data();
      w.end   = buffer.data() + buffer.size();
      w.template value<T>(v);
      return w;
   }

   struct tally
   {
      std::size_t tried = 0, valid = 0;
   };

   // Exactly `bytes`, in an allocation of exactly that size (so that a read
   // past either end is a heap overflow the sanitizer build reports).
   template <typename T>
   void probe(std::span<const char> bytes, tally& t)
   {
      std::unique_ptr<char[]> exact(new char[bytes.size() ? bytes.size() : 1]);
      if (!bytes.empty())
         std::memcpy(exact.get(), bytes.data(), bytes.size());
      const std::span<const char> b{exact.get(), bytes.size()};

      ++t.tried;
      const auto status = psio::validate_flatbuf<T>(b);
      const auto safe   = walk<T>(b, psio::flatbuf_view<T>{b});
      REQUIRE(safe.inside);
      // The mutable view navigates as the safe view does.
      std::span<char> writable{exact.get(), bytes.size()};
      (void)psio::flatbuf_mutable_view<T>{writable}.as_view();

      if (!status.ok())
         return;
      ++t.valid;
      const auto trusted = walk<T>(b, psio::flatbuf_trusted_view<T>{psio::trusted{b}});
      REQUIRE(trusted.inside);
      if (safe.budget && trusted.budget)
         REQUIRE(trusted.hash == safe.hash);
      const T    value = psio::from_flatbuf<T>(psio::trusted{b});
      const auto again = psio::to_flatbuf(value);
      REQUIRE(psio::validate_flatbuf<T>(again).ok());
      // Compared as encodings: a corrupted float may be a NaN.
      REQUIRE(psio::to_flatbuf(psio::from_flatbuf<T>(psio::trusted{again})) == again);
      REQUIRE(psio::to_flatbuf(psio::flatbuf_view<T>{b}.unpack()) == again);
   }

   template <typename T>
   void systematic(const T& value, tally& t)
   {
      const auto bytes = psio::to_flatbuf(value);
      // Truncations.
      for (std::size_t n = 0; n <= bytes.size(); ++n)
         probe<T>(std::span<const char>{bytes.data(), n}, t);
      // Every byte, several replacements.
      auto b = bytes;
      for (std::size_t i = 0; i < b.size(); ++i)
      {
         const char was = b[i];
         for (const unsigned char v :
              {static_cast<unsigned char>(0), static_cast<unsigned char>(0xff),
               static_cast<unsigned char>(was + 1), static_cast<unsigned char>(was - 1),
               static_cast<unsigned char>(was ^ 0x80), static_cast<unsigned char>(was + 4),
               static_cast<unsigned char>(was - 4), static_cast<unsigned char>(0x7f)})
         {
            if (static_cast<char>(v) == was)
               continue;
            b[i] = static_cast<char>(v);
            probe<T>(b, t);
         }
         b[i] = was;
      }
   }

   template <typename T>
   void random_edits(const T& value, std::uint64_t seed, int iterations, tally& t)
   {
      const auto      bytes = psio::to_flatbuf(value);
      std::mt19937_64 rng(seed);
      for (int iter = 0; iter < iterations; ++iter)
      {
         auto      b     = bytes;
         const int edits = 1 + static_cast<int>(rng() % 4);
         for (int e = 0; e < edits; ++e)
         {
            const std::size_t at = rng() % b.size();
            switch (rng() % 6)
            {
               case 0: b[at] = static_cast<char>(rng()); break;
               case 1: b[at] = static_cast<char>(b[at] + 4); break;
               case 2: b[at] = static_cast<char>(b[at] ^ (1u << (rng() % 8))); break;
               case 3:
               {
                  // A 32-bit word: an offset or a length.
                  const std::size_t   w = at & ~std::size_t{3};
                  const std::uint32_t v =
                     rng() % 2 ? static_cast<std::uint32_t>(rng() % (b.size() + 8))
                               : static_cast<std::uint32_t>(rng());
                  if (w + 4 <= b.size())
                     std::memcpy(b.data() + w, &v, 4);
                  break;
               }
               case 4:
               {
                  // A 16-bit word: a vtable entry.
                  const std::size_t   w = at & ~std::size_t{1};
                  const std::uint16_t v = static_cast<std::uint16_t>(rng() % 96);
                  if (w + 2 <= b.size())
                     std::memcpy(b.data() + w, &v, 2);
                  break;
               }
               default: b.resize(at); break;
            }
            if (b.empty())
               break;
         }
         probe<T>(b, t);
      }
   }

   Tree sample_tree()
   {
      Tree t;
      t.value = 7;
      t.name  = "root";
      t.children.resize(4);
      t.children[0].name = "a";
      t.children[1].children.resize(3);
      t.children[1].children[2].value = 9;
      t.children[1].children[2].children.resize(1);
      t.children[3].value = -1;
      return t;
   }
}  // namespace

TEST_CASE("flatbuf fuzz: truncations and single-byte corruptions", "[flatbuf][fuzz]")
{
   tally t;
   systematic(sample_order(), t);
   systematic(Order{}, t);
   systematic(sample_tree(), t);
   systematic(Scalars{true, -1, 2, -3, 4, -5, 6, -7, 8, 1.5f, -2.25, Color::blue, 'x'}, t);
   systematic(sample_profile(), t);
   WARN("systematic: " << t.tried << " buffers, " << t.valid << " valid");
   CHECK(t.valid > 0);
   CHECK(t.valid < t.tried);
}

TEST_CASE("flatbuf fuzz: random corruptions", "[flatbuf][fuzz]")
{
   tally t;
   random_edits(sample_order(), 1, 150000, t);
   random_edits(sample_tree(), 2, 60000, t);
   random_edits(sample_profile(), 3, 30000, t);
   WARN("random edits: " << t.tried << " buffers, " << t.valid << " valid");
   CHECK(t.valid > 0);
   CHECK(t.valid < t.tried);
}

TEST_CASE("flatbuf fuzz: random bytes", "[flatbuf][fuzz]")
{
   tally           t;
   std::mt19937_64 rng(4);
   std::vector<char> b;
   for (int iter = 0; iter < 60000; ++iter)
   {
      b.resize(rng() % 96);
      for (auto& c : b)
         c = static_cast<char>(rng() % 3 ? rng() % 24 : rng());
      // Half the time, a plausible root offset.
      if (b.size() >= 8 && iter % 2)
      {
         const std::uint32_t root = static_cast<std::uint32_t>((rng() % (b.size() / 4)) * 4);
         std::memcpy(b.data(), &root, 4);
      }
      probe<Order>(b, t);
      probe<Tree>(b, t);
   }
   WARN("random bytes: " << t.tried << " buffers, " << t.valid << " valid");
   SUCCEED();
}

TEST_CASE("flatbuf fuzz: a buffer that shares sub-objects cannot make the verifier run away",
          "[flatbuf][fuzz]")
{
   // A Tree whose `children` vector holds N offsets to ONE child, whose own
   // `children` is that same vector's twin, and so on: 12 levels of fan-out
   // 40 would be 40^12 tables if every path were walked. Offsets only point
   // forward, so the sharing is built by hand from the leaves back.
   const std::uint32_t fan = 40, levels = 12;
   std::vector<char>   buf;
   const auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(buf.data() + at, &v, 4); };
   const auto put16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(buf.data() + at, &v, 2); };
   // Layout, front to back: root offset; one vtable; then per level a table
   // { soffset, children offset } and its vector of `fan` offsets to the next
   // level's table.
   const std::size_t vtable = 4;                      // 10 bytes: size, object, value, name, children
   const std::size_t first  = 16;
   const std::size_t stride = 8 + 4 + std::size_t{fan} * 4;
   buf.assign(first + stride * levels + 12, 0);
   put32(0, static_cast<std::uint32_t>(first));
   put16(vtable, 10);
   put16(vtable + 2, 8);
   put16(vtable + 8, 4);  // children at +4
   for (std::uint32_t l = 0; l <= levels; ++l)
   {
      const std::size_t tbl = first + stride * l;
      put32(tbl, static_cast<std::uint32_t>(tbl - vtable));  // soffset back to the vtable
      if (l == levels)
      {
         // The leaf: a table whose children field points at an empty vector.
         put32(tbl + 4, 4);
         break;
      }
      const std::size_t vec = tbl + 8;
      put32(tbl + 4, static_cast<std::uint32_t>(vec - (tbl + 4)));
      put32(vec, fan);
      const std::size_t next = first + stride * (l + 1);
      for (std::uint32_t i = 0; i < fan; ++i)
      {
         const std::size_t cell = vec + 4 + std::size_t{i} * 4;
         put32(cell, static_cast<std::uint32_t>(next - cell));
      }
   }
   // The leaf's vector: the 4 zero bytes after its table.
   const auto st = psio::validate_flatbuf<Tree>(buf);
   REQUIRE_FALSE(st.ok());
   CHECK(st.error().what == "flatbuf: too many tables");

   // With the sharing cut down to something a budget allows, it is valid —
   // so the construction above is a well-formed buffer, not noise.
   std::vector<char> small = buf;
   for (std::uint32_t l = 0; l < levels; ++l)
   {
      const std::uint32_t one = 1;
      std::memcpy(small.data() + first + stride * l + 8, &one, 4);
   }
   REQUIRE(psio::validate_flatbuf<Tree>(small).ok());
   CHECK(psio::flatbuf_view<Tree>{small}.children()[0].children().size() == 1);
}

// ── The payload budget (gofractally/psio#10) ─────────────────────────────

namespace
{
   // table Bag { keys: [string]; } (a sorted set), table Bags { bags: [Bag]; }
   struct Bag
   {
      std::set<std::string> keys;
      friend bool           operator==(const Bag&, const Bag&) = default;
   };
   PSIO_REFLECT(Bag, keys)
   struct Bags
   {
      std::vector<Bag> bags;
      friend bool      operator==(const Bags&, const Bags&) = default;
   };
   PSIO_REFLECT(Bags, bags)
   // table Names { names: [string]; }
   struct Names
   {
      std::vector<std::string> names;
      friend bool              operator==(const Names&, const Names&) = default;
   };
   PSIO_REFLECT(Names, names)

   using Bytes = std::vector<char>;
   void put32(Bytes& b, std::size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
   void put16(Bytes& b, std::size_t at, std::uint16_t v) { std::memcpy(b.data() + at, &v, 2); }

   // A root table with one member, an offset at +4, and its vector of
   // `count` elements at 20: the first 24 bytes.
   void one_vector_root(Bytes& b, std::size_t count)
   {
      put32(b, 0, 12);  // the root table
      put16(b, 4, 6);   // its vtable: size, object size, member 0 at +4
      put16(b, 6, 8);
      put16(b, 8, 4);
      put32(b, 12, 8);  // soffset to the vtable
      put32(b, 16, 4);  // the vector, at 20
      put32(b, 20, static_cast<std::uint32_t>(count));
   }

   // `tables` bags, each with its own set of two offsets to the SAME two
   // keys of `len` bytes, which differ in their last byte: the verifier
   // compares tables × len bytes unless it counts them.
   Bytes shared_keys(std::size_t tables, std::size_t len)
   {
      const std::size_t t0 = 24 + 4 * tables, v0 = t0 + 8 * tables, s0 = v0 + 12 * tables;
      const std::size_t slen = (4 + len + 1 + 3) & ~std::size_t{3};
      Bytes             b(s0 + 2 * slen, 0);
      one_vector_root(b, tables);
      for (std::size_t i = 0; i < tables; ++i)
      {
         const std::size_t cell = 24 + 4 * i, tbl = t0 + 8 * i, keys = v0 + 12 * i;
         put32(b, cell, static_cast<std::uint32_t>(tbl - cell));
         put32(b, tbl, static_cast<std::uint32_t>(tbl - 4));  // the same vtable
         put32(b, tbl + 4, static_cast<std::uint32_t>(keys - (tbl + 4)));
         put32(b, keys, 2);
         put32(b, keys + 4, static_cast<std::uint32_t>(s0 - (keys + 4)));
         put32(b, keys + 8, static_cast<std::uint32_t>(s0 + slen - (keys + 8)));
      }
      for (std::size_t k = 0; k < 2; ++k)
      {
         put32(b, s0 + k * slen, static_cast<std::uint32_t>(len));
         std::memset(b.data() + s0 + k * slen + 4, 'a', len);
         b[s0 + k * slen + 4 + len - 1] = static_cast<char>('a' + k);
      }
      return b;
   }

   // `count` offsets to one string of `len` bytes.
   Bytes shared_string(std::size_t count, std::size_t len)
   {
      const std::size_t s0 = 24 + 4 * count;
      Bytes             b(s0 + ((4 + len + 1 + 3) & ~std::size_t{3}), 0);
      one_vector_root(b, count);
      for (std::size_t i = 0; i < count; ++i)
         put32(b, 24 + 4 * i, static_cast<std::uint32_t>(s0 - (24 + 4 * i)));
      put32(b, s0, static_cast<std::uint32_t>(len));
      std::memset(b.data() + s0 + 4, 'a', len);
      return b;
   }
}  // namespace

TEST_CASE("flatbuf budget: comparing keys that share their bytes is bounded by the buffer",
          "[flatbuf][fuzz][budget]")
{
   // tables × key length bytes were compared: 0.22 / 0.44 / 0.88 MB took
   // 4.6 / 24 / 71 ms. The bytes compared are charged to the payload budget.
   for (auto [tables, len] : {std::pair<std::size_t, std::size_t>{5000, 50000},
                              {10000, 100000},
                              {20000, 200000}})
   {
      const Bytes b  = shared_keys(tables, len);
      const auto  st = psio::validate_flatbuf<Bags>(b);
      REQUIRE_FALSE(st.ok());
      const bool budget = st.error().what == "flatbuf: too many bytes referenced" ||
                          st.error().what == "flatbuf: too many key bytes compared";
      CHECK(budget);
   }
   // The construction is a well-formed buffer: small, it is valid and is
   // what it looks like.
   const Bytes small = shared_keys(50, 100);
   REQUIRE(psio::validate_flatbuf<Bags>(small).ok());
   const Bags bags = psio::from_flatbuf<Bags>(small);
   REQUIRE(bags.bags.size() == 50);
   CHECK(bags.bags[49].keys ==
         std::set<std::string>{std::string(100, 'a'), std::string(99, 'a') + "b"});
   // Where the budget runs out: 5,000 sets of two 50 kB keys reach 500 MB of
   // strings and compare 250 MB; the buffer allows 1 MiB + 64 × 220,040.
   const Bytes b = shared_keys(5000, 50000);
   REQUIRE(b.size() == 220040);
   const auto st = psio::validate_flatbuf<Bags>(b);
   REQUIRE_FALSE(st.ok());
   CHECK(st.error().what == "flatbuf: too many key bytes compared");
   // The budget is an option: lifted, the buffer is valid.
   const psio::flatbuf::limits unlimited{.max_expansion = 0xffffffffu};
   CHECK(psio::validate_flatbuf<Bags>(shared_keys(200, 2000), unlimited).ok());
   CHECK_FALSE(psio::validate_flatbuf<Bags>(shared_keys(200, 2000),
                                            psio::flatbuf::limits{.max_expansion = 0})
                  .ok());
   // An honest buffer needs no more than three times its size: every key
   // reached once and compared with its two neighbours.
   Bags honest;
   for (int i = 0; i < 2000; ++i)
   {
      Bag bag;
      for (int k = 0; k < 8; ++k)
      {
         char key[32];
         std::snprintf(key, sizeof key, "key-%06d-%02d-", i, k);
         bag.keys.insert(key + std::string(40, 'x'));
      }
      honest.bags.push_back(std::move(bag));
   }
   const auto bytes = psio::to_flatbuf(honest);
   CHECK(psio::validate_flatbuf<Bags>(bytes, psio::flatbuf::limits{.max_expansion = 3}).ok());
   CHECK(psio::from_flatbuf<Bags>(bytes) == honest);
}

TEST_CASE("flatbuf budget: a shared string cannot decode to more than the budget",
          "[flatbuf][fuzz][budget]")
{
   // 50,032 bytes that verified and decoded to 100 MB: 10,000 offsets to one
   // string of 10 kB.
   const Bytes b = shared_string(10000, 10000);
   REQUIRE(b.size() == 50032);
   const auto st = psio::validate_flatbuf<Names>(b);
   REQUIRE_FALSE(st.ok());
   CHECK(st.error().what == "flatbuf: too many bytes referenced");
   CHECK_THROWS_AS(psio::from_flatbuf<Names>(b), psio::codec_exception);
   // The safe view does not copy: it reads the bytes whatever they are.
   CHECK(psio::flatbuf_view<Names>{b}.names().size() == 10000);
   // What the default allows: 1 MiB and 64 times the buffer. A string an
   // official builder shares (CreateSharedString) is reached once per
   // reference: 1,000 references to a 1 kB string are 1 MB from 5 kB.
   const Bytes shared = shared_string(1000, 1000);
   REQUIRE(psio::validate_flatbuf<Names>(shared).ok());
   const Names names = psio::from_flatbuf<Names>(shared);
   REQUIRE(names.names.size() == 1000);
   CHECK(names.names[999] == std::string(1000, 'a'));
   // The limit is an option, either way.
   CHECK(psio::validate_flatbuf<Names>(shared_string(2000, 1000),
                                       psio::flatbuf::limits{.max_expansion = 0})
            .error()
            .what == "flatbuf: too many bytes referenced");
   CHECK(psio::validate_flatbuf<Names>(b, psio::flatbuf::limits{.max_expansion = 4000}).ok());
   // A vector of scalars is payload as a string is: `Longs.values` shared
   // by nothing here, so the same bytes are reached once.
   Longs longs;
   longs.values.assign(1000, 7);
   CHECK(psio::validate_flatbuf<Longs>(psio::to_flatbuf(longs),
                                       psio::flatbuf::limits{.max_expansion = 1})
            .ok());
}
