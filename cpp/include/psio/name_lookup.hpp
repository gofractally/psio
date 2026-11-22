#pragma once
//
// psio/name_lookup.hpp — constant-time lookup tables generated at compile
// time from a fixed set of names, numbers or caller-hashed keys.
//
//   psio::make_name_table(names)          name        -> index
//   psio::make_hashed_table<Hash>(names)  Hash(name)  -> index (64-bit key)
//   psio::make_number_table<Holder>()     number      -> index
//   psio::runtime_name_index              name -> index, built at run time
//
// Every table is a constexpr value: there is no run-time initialization, and
// `find` works in constant expressions. They are generic over the name set,
// so the same facility serves record members (reflect.hpp), variant
// alternative names (json.hpp) and any other fixed list such as an
// interface's function names.
//
// ── Name lookup ───────────────────────────────────────────────────────────
//
// `name_table<N>::find(name)` returns the index of `name`, or N.
//
//   1. The name is reduced to a 64-bit key. Tier 0 reads the length, the
//      first eight and the last eight bytes (a fixed number of loads for any
//      length; it sees every byte of a name of up to 16 bytes). When two
//      names of the set share that key, tier 1 hashes the whole name instead.
//   2. A hash-and-displace perfect hash maps the key to a slot: the key picks
//      a bucket, the bucket's displacement picks the multiplier of a second
//      multiplicative hash, and that picks the slot. Two multiplies, two
//      table loads, no probing.
//   3. Exactly one confirming compare against the name stored in that slot
//      (slots no name hashes to point at member 0, so there is no separate
//      "empty" test).
//
// The displacement search runs at compile time and is bounded: when it would
// exceed its budget (or when tier 0 and tier 1 both collide, which takes an
// adversarial name set), the table falls back to tier 2, a binary search over
// the names sorted by (length, bytes) — still sublinear, never a compile
// error. `name_table::tier` says which scheme a given set got.
//
// ── Caller-hashed lookup ──────────────────────────────────────────────────
//
// `make_hashed_table<Hash>(names)` builds the same perfect hash over
// `Hash{}(name)` for a caller-supplied constexpr hash (for example a
// compressed 64-bit method name). `find(key)` confirms by comparing the
// stored key. `unique` is false when two names of the set have the same
// key; reflect<T>::index_of_hash turns that into a compile error.
//
// ── Number lookup ─────────────────────────────────────────────────────────
//
// `make_number_table<Holder>()` (Holder::numbers is a constexpr
// std::array<std::uint32_t, N>) is a direct table indexed by the number when
// the numbers are dense (pure arithmetic when they are exactly 1..N), and
// the perfect hash above when they are sparse.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psio
{
   namespace detail::lookup
   {
      using index_t = std::uint16_t;

      constexpr std::uint64_t load_le(const char* p, std::size_t n) noexcept
      {
         std::uint64_t v = 0;
         for (std::size_t i = 0; i < n; ++i)
            v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
         return v;
      }
      constexpr std::uint64_t read64(const char* p) noexcept
      {
         if (std::is_constant_evaluated() || std::endian::native != std::endian::little)
            return load_le(p, 8);
         std::uint64_t v;
         std::memcpy(&v, p, 8);
         return v;
      }
      constexpr std::uint64_t read32(const char* p) noexcept
      {
         if (std::is_constant_evaluated() || std::endian::native != std::endian::little)
            return load_le(p, 4);
         std::uint32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }

      // Up to eight bytes at p as one word, without reading past p + n.
      constexpr std::uint64_t read_tail(const char* p, std::size_t n) noexcept
      {
         if (n >= 8)
            return read64(p);
         if (n >= 4)
            return read32(p) | (read32(p + n - 4) << 32);
         if (n > 0)
            return static_cast<std::uint64_t>(static_cast<unsigned char>(p[0])) |
                   (static_cast<std::uint64_t>(static_cast<unsigned char>(p[n >> 1])) << 8) |
                   (static_cast<std::uint64_t>(static_cast<unsigned char>(p[n - 1])) << 16);
         return 0;
      }

      inline constexpr std::uint64_t k_len  = 0x9E3779B97F4A7C15ull;
      inline constexpr std::uint64_t k_pre  = 0xD6E8FEB86659FD93ull;
      inline constexpr std::uint64_t k_slot = 0xFF51AFD7ED558CCDull;  // odd

      // Tier 0: length, first eight and last eight bytes. Exact (no two
      // names share the pre-mix input) for names of up to 16 bytes.
      constexpr std::uint64_t short_key(std::string_view s) noexcept
      {
         const std::size_t n = s.size();
         const char*       p = s.data();
         if (n < 8)
            return read_tail(p, n) ^ (static_cast<std::uint64_t>(n) * k_len);
         const unsigned __int128 wide =
            static_cast<unsigned __int128>(read64(p) ^ k_len) *
            (read64(p + n - 8) ^ k_slot ^ static_cast<std::uint64_t>(n));
         return static_cast<std::uint64_t>(wide) ^ static_cast<std::uint64_t>(wide >> 64);
      }

      // Tier 1: every byte.
      constexpr std::uint64_t full_hash(std::string_view s) noexcept
      {
         std::size_t   n = s.size();
         const char*   p = s.data();
         std::uint64_t h = static_cast<std::uint64_t>(n) * k_len;
         while (n > 8)
         {
            h = (h ^ read64(p)) * k_slot;
            h ^= h >> 32;
            p += 8;
            n -= 8;
         }
         h = (h ^ read_tail(p, n)) * 0xC4CEB9FE1A85EC53ull;
         h ^= h >> 29;
         return h;
      }

      constexpr std::size_t table_slots(std::size_t n) noexcept
      {
         return std::bit_ceil(std::max<std::size_t>(2, n + n / 4 + 1));
      }
      constexpr std::size_t table_buckets(std::size_t n) noexcept
      {
         return std::bit_ceil((n + 4) / 4);
      }

      // Perfect hash over N distinct 64-bit keys (hash and displace).
      template <std::size_t N>
      struct key_table
      {
         static constexpr std::size_t slots   = table_slots(N);
         static constexpr std::size_t buckets = table_buckets(N);
         static constexpr unsigned    shift =
            64 - static_cast<unsigned>(std::countr_zero(slots));

         std::array<std::uint64_t, N>      keys{};
         std::array<std::uint16_t, buckets> disp{};
         std::array<index_t, slots>         slot{};  // unused slots hold 0
         std::array<index_t, N>             sorted{};  // by key, for the fallback
         bool                               unique  = true;   // keys distinct
         bool                               perfect = false;  // disp / slot valid

         // The premix: `bucket` takes the high bits of the product (they
         // depend on every key bit), `mixed` feeds the slot hash.
         static constexpr std::uint64_t premix(std::uint64_t key) noexcept
         {
            return key * k_pre;
         }
         static constexpr std::size_t bucket_of(std::uint64_t h) noexcept
         {
            // buckets == 1 would shift by 64.
            return static_cast<std::size_t>((h >> 1) >>
                                            (63 - static_cast<unsigned>(std::countr_zero(buckets))));
         }
         static constexpr std::size_t slot_of(std::uint64_t h, std::uint64_t d) noexcept
         {
            return static_cast<std::size_t>(((h ^ (h >> 32)) * (k_slot + 2 * d)) >> shift);
         }

         // The one index `key` can be at. Needs `perfect`; the caller
         // confirms (an absent key lands on an arbitrary index).
         constexpr std::size_t candidate(std::uint64_t key) const noexcept
         {
            const std::uint64_t h = premix(key);
            return slot[slot_of(h, disp[bucket_of(h)])];
         }

         // Index of `key`, or N.
         constexpr std::size_t find(std::uint64_t key) const noexcept
         {
            if constexpr (N == 0)
               return 0;
            else
            {
               if (perfect)
               {
                  const std::size_t i = candidate(key);
                  return keys[i] == key ? i : N;
               }
               std::size_t lo = 0, hi = N;
               while (lo < hi)
               {
                  const std::size_t mid = lo + (hi - lo) / 2;
                  if (keys[sorted[mid]] < key)
                     lo = mid + 1;
                  else
                     hi = mid;
               }
               return lo < N && keys[sorted[lo]] == key ? sorted[lo] : N;
            }
         }
      };

      template <std::size_t N>
      constexpr key_table<N> make_key_table(const std::array<std::uint64_t, N>& keys) noexcept
      {
         static_assert(N < 0xFFFF, "psio: too many names for one lookup table");
         using table = key_table<N>;
         table t{};
         t.keys = keys;
         if constexpr (N > 0)
         {
            for (std::size_t i = 0; i < N; ++i)
               t.sorted[i] = static_cast<index_t>(i);
            std::sort(t.sorted.begin(), t.sorted.end(), [&](index_t a, index_t b) {
               return keys[a] != keys[b] ? keys[a] < keys[b] : a < b;
            });
            for (std::size_t i = 1; i < N; ++i)
               if (keys[t.sorted[i - 1]] == keys[t.sorted[i]])
                  t.unique = false;
            if (!t.unique)
               return t;

            constexpr std::size_t B = table::buckets;
            constexpr std::size_t M = table::slots;
            std::array<std::uint64_t, N> hash{};
            std::array<index_t, B + 1>   start{};  // bucket -> first item
            for (std::size_t i = 0; i < N; ++i)
            {
               hash[i] = table::premix(keys[i]);
               ++start[table::bucket_of(hash[i]) + 1];
            }
            for (std::size_t b = 0; b < B; ++b)
               start[b + 1] = static_cast<index_t>(start[b + 1] + start[b]);
            std::array<index_t, N> item{};  // items grouped by bucket
            {
               std::array<index_t, B> fill{};
               for (std::size_t i = 0; i < N; ++i)
               {
                  const std::size_t b = table::bucket_of(hash[i]);
                  item[start[b] + fill[b]++] = static_cast<index_t>(i);
               }
            }
            // Buckets, largest first (counting sort by size).
            std::array<index_t, B> order{};
            {
               std::array<index_t, N + 2> at{};
               for (std::size_t b = 0; b < B; ++b)
                  ++at[N - (start[b + 1] - start[b]) + 1];
               for (std::size_t s = 0; s <= N; ++s)
                  at[s + 1] = static_cast<index_t>(at[s + 1] + at[s]);
               for (std::size_t b = 0; b < B; ++b)
                  order[at[N - (start[b + 1] - start[b])]++] = static_cast<index_t>(b);
            }
            std::array<bool, M>        used{};
            std::array<std::size_t, N> pos{};
            // Bounds the compile-time search: placements tried, in keys.
            std::size_t budget = 20000 + 64 * N;
            for (std::size_t o = 0; o < B; ++o)
            {
               const std::size_t b    = order[o];
               const std::size_t size = start[b + 1] - start[b];
               if (size == 0)
                  break;
               bool placed = false;
               for (std::uint32_t d = 0; d <= 0xFFFF && !placed; ++d)
               {
                  if (budget < size)
                     return t;
                  budget -= size;
                  bool ok = true;
                  for (std::size_t j = 0; j < size && ok; ++j)
                  {
                     pos[j] = table::slot_of(hash[item[start[b] + j]], d);
                     if (used[pos[j]])
                        ok = false;
                     for (std::size_t l = 0; l < j && ok; ++l)
                        if (pos[l] == pos[j])
                           ok = false;
                  }
                  if (!ok)
                     continue;
                  for (std::size_t j = 0; j < size; ++j)
                  {
                     used[pos[j]]   = true;
                     t.slot[pos[j]] = item[start[b] + j];
                  }
                  t.disp[b] = static_cast<std::uint16_t>(d);
                  placed    = true;
               }
               if (!placed)
                  return t;
            }
            t.perfect = true;
         }
         return t;
      }

      constexpr bool name_less(std::string_view a, std::string_view b) noexcept
      {
         return a.size() != b.size() ? a.size() < b.size() : a < b;
      }
   }  // namespace detail::lookup

   // ── name -> index ──────────────────────────────────────────────────────
   template <std::size_t N>
   struct name_table
   {
      std::array<std::string_view, N>        names{};
      detail::lookup::key_table<N>           keys{};
      std::array<detail::lookup::index_t, N> sorted{};  // tier 2 only
      // 0: length + head + tail key; 1: whole-name hash; 2: binary search.
      std::uint8_t tier = 0;

      static constexpr std::size_t npos = N;

      constexpr std::size_t find_sorted(std::string_view name) const noexcept
      {
         std::size_t lo = 0, hi = N;
         while (lo < hi)
         {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (detail::lookup::name_less(names[sorted[mid]], name))
               lo = mid + 1;
            else
               hi = mid;
         }
         return lo < N && names[sorted[lo]] == name ? sorted[lo] : N;
      }

      // Index of `name`, or N (== npos).
      constexpr std::size_t find(std::string_view name) const noexcept
      {
         if constexpr (N == 0)
            return 0;
         else
         {
            if (tier == 2)
               return find_sorted(name);
            const std::size_t i =
               keys.candidate(tier == 0 ? detail::lookup::short_key(name)
                                        : detail::lookup::full_hash(name));
            return names[i] == name ? i : N;
         }
      }
   };

   template <std::size_t N>
   constexpr name_table<N> make_name_table(const std::array<std::string_view, N>& names) noexcept
   {
      name_table<N> t{};
      t.names = names;
      if constexpr (N > 0)
      {
         std::array<std::uint64_t, N> keys{};
         for (std::size_t i = 0; i < N; ++i)
            keys[i] = detail::lookup::short_key(names[i]);
         t.keys = detail::lookup::make_key_table(keys);
         if (t.keys.perfect)
            return t;
         for (std::size_t i = 0; i < N; ++i)
            keys[i] = detail::lookup::full_hash(names[i]);
         t.keys = detail::lookup::make_key_table(keys);
         t.tier = 1;
         if (t.keys.perfect)
            return t;
         // A repeated name resolves to its first occurrence, as a scan would.
         t.tier = 2;
         for (std::size_t i = 0; i < N; ++i)
            t.sorted[i] = static_cast<detail::lookup::index_t>(i);
         std::sort(t.sorted.begin(), t.sorted.end(),
                   [&](detail::lookup::index_t a, detail::lookup::index_t b) {
                      return names[a] != names[b] ? detail::lookup::name_less(names[a], names[b])
                                                  : a < b;
                   });
      }
      return t;
   }

   // ── Hash(name) -> index ────────────────────────────────────────────────
   template <std::size_t N>
   struct hashed_table
   {
      detail::lookup::key_table<N> keys{};
      // False when two names have the same key under the hash.
      bool unique = true;

      static constexpr std::size_t npos = N;

      // Index of the name whose hash is `key`, or N.
      constexpr std::size_t find(std::uint64_t key) const noexcept { return keys.find(key); }
      // The key of name `i`.
      constexpr std::uint64_t key(std::size_t i) const noexcept { return keys.keys[i]; }
   };

   // Hash: default-constructible, `Hash{}(std::string_view)` is a constant
   // expression convertible to std::uint64_t.
   template <typename Hash, std::size_t N>
   constexpr hashed_table<N> make_hashed_table(
      const std::array<std::string_view, N>& names) noexcept
   {
      hashed_table<N>              t{};
      std::array<std::uint64_t, N> keys{};
      for (std::size_t i = 0; i < N; ++i)
         keys[i] = static_cast<std::uint64_t>(Hash{}(names[i]));
      t.keys   = detail::lookup::make_key_table(keys);
      t.unique = t.keys.unique;
      return t;
   }

   // ── number -> index ────────────────────────────────────────────────────
   namespace detail::lookup
   {
      template <std::size_t N>
      constexpr std::uint32_t max_number(const std::array<std::uint32_t, N>& numbers) noexcept
      {
         std::uint32_t m = 0;
         for (auto n : numbers)
            m = std::max(m, n);
         return m;
      }
      template <std::size_t N>
      constexpr bool is_dense(const std::array<std::uint32_t, N>& numbers) noexcept
      {
         return max_number(numbers) <= 4 * N + 64;
      }

      // Indexed by the number itself; `Size` is the largest number + 1.
      template <std::size_t N, std::size_t Size>
      struct direct_number_table
      {
         std::array<index_t, Size> index{};  // N: no such number
         bool identity = false;              // numbers are exactly 1..N in order
         bool unique   = true;

         static constexpr std::size_t npos = N;

         constexpr std::size_t find(std::uint64_t number) const noexcept
         {
            if (identity)
               return number - 1 < N ? static_cast<std::size_t>(number - 1) : N;
            return number < Size ? index[number] : N;
         }
      };

      template <std::size_t N>
      struct sparse_number_table
      {
         key_table<N> keys{};
         bool         unique = true;

         static constexpr std::size_t npos = N;

         constexpr std::size_t find(std::uint64_t number) const noexcept
         {
            return keys.find(number);
         }
      };
   }  // namespace detail::lookup

   // Holder::numbers: static constexpr std::array<std::uint32_t, N>. The
   // result's `find(number)` returns the index, or N; `unique` is false when
   // a number repeats (then the first index is returned).
   template <typename Holder>
   constexpr auto make_number_table() noexcept
   {
      constexpr auto&       numbers = Holder::numbers;
      constexpr std::size_t N       = numbers.size();
      if constexpr (detail::lookup::is_dense(numbers))
      {
         constexpr std::size_t Size = detail::lookup::max_number(numbers) + 1;
         detail::lookup::direct_number_table<N, Size> t{};
         for (auto& slot : t.index)
            slot = static_cast<detail::lookup::index_t>(N);
         t.identity = true;
         for (std::size_t i = N; i-- > 0;)
         {
            if (t.index[numbers[i]] != N)
               t.unique = false;
            t.index[numbers[i]] = static_cast<detail::lookup::index_t>(i);
            if (numbers[i] != i + 1)
               t.identity = false;
         }
         return t;
      }
      else
      {
         detail::lookup::sparse_number_table<N> t{};
         std::array<std::uint64_t, N>           keys{};
         for (std::size_t i = 0; i < N; ++i)
            keys[i] = numbers[i];
         t.keys   = detail::lookup::make_key_table(keys);
         t.unique = t.keys.unique;
         return t;
      }
   }

   // ── Tables keyed by the type that carries the names ────────────────────
   //
   // Instantiated on first use only, so a type that is never looked up by
   // name costs nothing at compile time.
   namespace detail
   {
      // Holder::_names_array_: static constexpr std::array<std::string_view, N>.
      template <typename Holder>
      inline constexpr auto name_table_for = ::psio::make_name_table(Holder::_names_array_);

      template <typename Holder, typename Hash>
      struct hashed_table_for
      {
         static constexpr auto table =
            ::psio::make_hashed_table<Hash>(Holder::_names_array_);
         static_assert(table.unique,
                       "psio: two names of one type collide under the supplied hash");
      };

      // Holder::numbers as make_number_table wants it.
      template <typename Holder>
      inline constexpr auto number_table_for = ::psio::make_number_table<Holder>();
   }  // namespace detail

   // ── name -> index for a name set known only at run time ────────────────
   //
   // Built once (for example per record of a compiled schema) and then
   // queried without allocation: open addressing over the tier-1 hash, one
   // compare per probe.
   class runtime_name_index
   {
      std::vector<std::string_view> names_;
      std::vector<std::uint32_t>    slots_;  // index + 1; 0 is empty
      std::uint64_t                 mask_ = 0;

     public:
      runtime_name_index() = default;

      // The views must outlive the index. A repeated name resolves to its
      // first occurrence.
      explicit runtime_name_index(std::vector<std::string_view> names) : names_(std::move(names))
      {
         const std::size_t size = std::bit_ceil(std::max<std::size_t>(4, names_.size() * 2));
         slots_.assign(size, 0);
         mask_ = size - 1;
         for (std::size_t i = 0; i < names_.size(); ++i)
         {
            std::uint64_t at    = detail::lookup::full_hash(names_[i]) & mask_;
            bool          known = false;
            while (slots_[at])
            {
               if (names_[slots_[at] - 1] == names_[i])
               {
                  known = true;
                  break;
               }
               at = (at + 1) & mask_;
            }
            if (!known)
               slots_[at] = static_cast<std::uint32_t>(i + 1);
         }
      }

      std::size_t size() const noexcept { return names_.size(); }

      // Index of `name`, or size().
      std::size_t find(std::string_view name) const noexcept
      {
         if (slots_.empty())
            return 0;
         std::uint64_t at = detail::lookup::full_hash(name) & mask_;
         while (slots_[at])
         {
            if (names_[slots_[at] - 1] == name)
               return slots_[at] - 1;
            at = (at + 1) & mask_;
         }
         return names_.size();
      }
   };

}  // namespace psio
