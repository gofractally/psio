#pragma once
// Random values of the shapes psio::protobuf maps, for the protobuf tests
// (deterministic: a fixed-seed xorshift generator, the same on every
// platform, so the conformance vectors can be regenerated).

#include <psio/protobuf.hpp>

#include <cstdint>
#include <cstring>
#include <string>

struct pb_rng
{
   std::uint64_t state;
   explicit pb_rng(std::uint64_t seed) : state{seed * 0x9e3779b97f4a7c15ull + 1} {}
   std::uint64_t next()
   {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      return state;
   }
   std::uint64_t below(std::uint64_t n) { return n ? next() % n : 0; }
   bool          chance(unsigned percent) { return below(100) < percent; }
};

namespace pb_random_detail
{
   namespace pb = psio::detail::pb_impl;

   // Integers that exercise every varint length and both ends of a type.
   template <typename I>
   I integer(pb_rng& r)
   {
      using U = std::make_unsigned_t<I>;
      switch (r.below(8))
      {
         case 0: return 0;
         case 1: return std::numeric_limits<I>::max();
         case 2: return std::numeric_limits<I>::min();
         case 3: return static_cast<I>(r.below(128));
         case 4: return static_cast<I>(static_cast<U>(0) - static_cast<U>(r.below(128)));
         default:
         {
            const unsigned bits = 1 + static_cast<unsigned>(r.below(sizeof(I) * 8));
            const auto     v    = static_cast<U>(r.next() >> (64 - bits));
            return static_cast<I>(r.chance(50) && std::is_signed_v<I> ? static_cast<U>(0) - v : v);
         }
      }
   }

   inline std::string text(pb_rng& r)
   {
      static const char* const pieces[] = {"a",  "psio", " ", "é",  "ß",  "€", "日本", "𝄞",
                                           "\n", "\"",   "0", "xyz", "\t", "Z", "ñ",  "😀"};
      std::string out;
      const auto  n = r.below(4) == 0 ? r.below(40) : r.below(6);
      for (std::uint64_t i = 0; i < n; ++i)
         out += pieces[r.below(16)];
      return out;
   }

   template <typename X>
   void fill(pb_rng& r, X& v, unsigned depth);

   template <typename X>
   X make(pb_rng& r, unsigned depth)
   {
      X v{};
      fill(r, v, depth);
      return v;
   }

   template <typename X>
   void fill(pb_rng& r, X& v, unsigned depth)
   {
      const std::uint64_t count = depth > 3 ? 0 : r.below(4) == 0 ? r.below(9) : r.below(3);
      if constexpr (std::is_same_v<X, bool>)
         v = r.chance(50);
      else if constexpr (std::is_same_v<X, float>)
      {
         static const float special[] = {0.0f, -0.0f, 1.5f, -2.25f, 3.4028235e38f, 1e-40f};
         v = r.chance(30) ? special[r.below(6)] : static_cast<float>(static_cast<std::int64_t>(r.next() >> 40)) / 64;
      }
      else if constexpr (std::is_same_v<X, double>)
      {
         static const double special[] = {0.0, -0.0, 1.5, -2.25, 1.7976931348623157e308, 5e-324};
         v = r.chance(30) ? special[r.below(6)] : static_cast<double>(static_cast<std::int64_t>(r.next())) / 1024;
      }
      else if constexpr (std::is_enum_v<X>)
      {
         // Declared values and others its type holds (an open enum).
         using U = std::underlying_type_t<X>;
         v       = static_cast<X>(r.chance(70) ? static_cast<U>(r.below(3)) : integer<U>(r));
      }
      else if constexpr (std::is_same_v<X, char>)
         v = static_cast<char>(integer<unsigned char>(r));
      else if constexpr (std::is_integral_v<X>)
         v = integer<X>(r);
      else if constexpr (std::is_same_v<X, std::string>)
         v = text(r);
      else if constexpr (pb::is_byte_array_v<X>)
         for (auto& b : v)
            b = static_cast<typename X::value_type>(r.chance(40) ? 0 : r.below(256));
      else if constexpr (pb::is_byte_vector_v<X>)
      {
         v.resize(r.below(4) == 0 ? r.below(70) : r.below(5));
         for (auto& b : v)
            b = static_cast<typename X::value_type>(r.below(256));
      }
      else if constexpr (pb::is_optional_v<X>)
      {
         if (r.chance(60) && depth <= 4)
            v = make<typename X::value_type>(r, depth + 1);
         else
            v.reset();
      }
      else if constexpr (pb::is_vector_v<X>)
      {
         v.clear();
         for (std::uint64_t i = 0; i < count; ++i)
            v.push_back(make<typename X::value_type>(r, depth + 1));
      }
      else if constexpr (pb::is_set_v<X>)
      {
         v.clear();
         for (std::uint64_t i = 0; i < count; ++i)
            v.insert(make<typename X::value_type>(r, depth + 1));
      }
      else if constexpr (pb::is_map_v<X>)
      {
         v.clear();
         for (std::uint64_t i = 0; i < count; ++i)
            v.insert_or_assign(make<typename X::key_type>(r, depth + 1),
                               make<typename X::mapped_type>(r, depth + 1));
      }
      else if constexpr (pb::is_variant_v<X>)
      {
         const std::size_t which = r.below(std::variant_size_v<X>);
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((which == Is
                 ? (void)v.template emplace<Is>(make<std::variant_alternative_t<Is, X>>(r, depth + 1))
                 : (void)0),
             ...);
         }(std::make_index_sequence<std::variant_size_v<X>>{});
      }
      else if constexpr (std::is_same_v<X, std::monostate>)
      {
      }
      else
      {
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (fill(r, v.*(psio::reflect<X>::template member_pointer<Is>), depth + 1), ...);
         }(std::make_index_sequence<psio::reflect<X>::member_count>{});
      }
   }
}  // namespace pb_random_detail

template <typename T>
T pb_random(pb_rng& r)
{
   return pb_random_detail::make<T>(r, 0);
}
