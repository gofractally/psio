#pragma once
//
// psio/get_type_name.hpp — compile-time type-name string source
// (port of psio/get_type_name.hpp).
//
// `get_type_name<T>()` returns a `const char*` (NUL-terminated) naming
// the type T as it appears in psio schemas / WIT output.  Built-in
// overloads cover:
//
//   primitives:    bool, int8..int64, uint8..uint64, float, double,
//                  char, int128, uint128, std::string
//   sequences:     std::vector<T>      → "T[]"
//                  std::array<T, N>    → "T[N]"
//                  T[N]                → same
//   options:       std::optional<T>    → "T?"
//   sums:          std::variant<...>   → "variant_T1_T2_..."
//                  std::tuple<...>     → "tuple_T1_T2_..."
//   chrono:        std::chrono::duration<...>             → "duration"
//                  time_point<system_clock, duration<R>>  → "TimePointSec"
//                  time_point<system_clock, duration<R, micro>>
//                                                          → "TimePointUSec"
//   reflected:     PSIO_REFLECT'd T   → reflect<T>::name (string_view)
//
// `PSIO_REFLECT_TYPENAME(T)` adds an unqualified-name overload for
// types reflection doesn't auto-handle (foreign types).  Method-name
// hashing for non-reflected types delegates to `compress_name.hpp`.

#include <psio/compress_name.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include <boost/preprocessor/stringize.hpp>

namespace psio {

#define PSIO_REFLECT_TYPENAME(T)                \
   constexpr const char* get_type_name(const T*) \
   {                                             \
      return BOOST_PP_STRINGIZE(T);              \
   }

   // ── is_reflected<T> ─────────────────────────────────────────────────
   //
   // psio v1 used a separate `psio1::is_reflected<T>` trait that
   // `reflect.hpp` specialises for each PSIO1_REFLECT'd type.  v3 attaches
   // the same information to `reflect<T>::is_reflected` directly — a
   // static constexpr bool member.  This trait bridges the two so the
   // get_type_name overload set retains its v1 shape (one
   // `requires(is_reflected_v<T>)` overload routes all reflected types
   // to a single implementation).
   namespace detail {
      template <typename T, typename = void>
      struct has_reflected_member : std::false_type {};
      template <typename T>
      struct has_reflected_member<T,
         std::void_t<decltype(::psio::reflect<T>::is_reflected)>>
            : std::bool_constant<::psio::reflect<T>::is_reflected> {};
   }

   template <typename T>
   inline constexpr bool is_reflected_v = detail::has_reflected_member<T>::value;

   // ── Forward declarations ────────────────────────────────────────────
   template <typename T>
      requires(is_reflected_v<T>)
   constexpr const char* get_type_name(const T*);

   template <typename T>
   constexpr const char* get_type_name(const std::optional<T>*);
   template <typename T>
   constexpr const char* get_type_name(const std::vector<T>*);
   template <typename T, std::size_t S>
   constexpr const char* get_type_name(const std::array<T, S>*);
   template <typename T, std::size_t S>
   constexpr const char* get_type_name(const T (*)[S]);
   template <typename... T>
   constexpr const char* get_type_name(const std::tuple<T...>*);
   template <typename... T>
   constexpr const char* get_type_name(const std::variant<T...>*);
   template <typename Rep, typename Period>
   constexpr const char* get_type_name(
      const std::chrono::duration<Rep, Period>*);

   // ── Primitive overloads ─────────────────────────────────────────────
   // clang-format off
   constexpr const char* get_type_name(const bool*) { return "bool"; }
   constexpr const char* get_type_name(const std::int8_t*) { return "int8"; }
   constexpr const char* get_type_name(const std::uint8_t*) { return "uint8"; }
   constexpr const char* get_type_name(const std::int16_t*) { return "int16"; }
   constexpr const char* get_type_name(const std::uint16_t*) { return "uint16"; }
   constexpr const char* get_type_name(const std::int32_t*) { return "int32"; }
   constexpr const char* get_type_name(const std::uint32_t*) { return "uint32"; }
   constexpr const char* get_type_name(const std::int64_t*) { return "int64"; }
   constexpr const char* get_type_name(const std::uint64_t*) { return "uint64"; }
   constexpr const char* get_type_name(const float*) { return "float32"; }
   constexpr const char* get_type_name(const double*) { return "double"; }
   constexpr const char* get_type_name(const char*) { return "char"; }
   constexpr const char* get_type_name(const std::string*) { return "string"; }
   constexpr const char* get_type_name(const std::string_view*) { return "string"; }
   constexpr const char* get_type_name(const __int128*) { return "int128"; }
   constexpr const char* get_type_name(const unsigned __int128*) { return "uint128"; }
   // clang-format on

   // ── Reflected types — pull name from psio::reflect<T>::name ────────
   //
   // The static constexpr string_view from PSIO_REFLECT is
   // null-terminated (the underlying STRINGIZE produces a string
   // literal), so .data() is valid as a const char*.
   template <typename T>
      requires(is_reflected_v<T>)
   constexpr const char* get_type_name(const T*)
   {
      return ::psio::reflect<T>::name.data();
   }

   // ── String-array helpers (same as v1) ───────────────────────────────
   template <std::size_t N, std::size_t M>
   constexpr std::array<char, N + M>
   array_cat(std::array<char, N> lhs, std::array<char, M> rhs)
   {
      std::array<char, N + M> result{};
      for (std::size_t i = 0; i < N; ++i)
         result[i] = lhs[i];
      for (std::size_t i = 0; i < M; ++i)
         result[i + N] = rhs[i];
      return result;
   }

   template <std::size_t N>
   constexpr std::array<char, N> to_array(std::string_view s)
   {
      std::array<char, N> result{};
      for (std::size_t i = 0; i < N; ++i)
         result[i] = s[i];
      return result;
   }

   template <typename T, std::size_t N>
   constexpr auto append_type_name(const char (&suffix)[N])
   {
      constexpr std::string_view name = get_type_name((T*)nullptr);
      return array_cat(to_array<name.size()>(name),
                       to_array<N>({suffix, N}));
   }

   template <typename T>
   constexpr auto vector_type_name = append_type_name<T>("[]");

   template <typename T>
   constexpr auto optional_type_name = append_type_name<T>("?");

   template <typename T>
   constexpr const char* get_type_name(const std::vector<T>*)
   {
      return vector_type_name<T>.data();
   }

   constexpr std::size_t const_log10(std::size_t n)
   {
      std::size_t result = 0;
      do
      {
         n /= 10;
         ++result;
      } while (n > 0);
      return result;
   }

   template <typename T, std::size_t S>
   constexpr auto get_array_type_name()
   {
      constexpr std::string_view name   = get_type_name((T*)nullptr);
      std::array<char, 2 + const_log10(S) + 1> bounds = {};
      bounds[0]                 = '[';
      bounds[bounds.size() - 2] = ']';
      for (std::size_t N = S, i = bounds.size() - 3; i > 0; --i, N /= 10)
         bounds[i] = '0' + N % 10;
      return array_cat(to_array<name.size()>(name), bounds);
   }

   template <typename T, std::size_t S>
   constexpr auto array_type_name = get_array_type_name<T, S>();

   template <typename T, std::size_t S>
   constexpr const char* get_type_name(const std::array<T, S>*)
   {
      return array_type_name<T, S>.data();
   }
   template <typename T, std::size_t S>
   constexpr const char* get_type_name(const T (*)[S])
   {
      return array_type_name<T, S>.data();
   }

   template <typename T>
   constexpr const char* get_type_name(const std::optional<T>*)
   {
      return optional_type_name<T>.data();
   }

   struct variant_type_appender
   {
      char*                           buf;
      constexpr variant_type_appender operator+(std::string_view s)
      {
         *buf++ = '_';
         for (auto ch : s)
            *buf++ = ch;
         return *this;
      }
   };

   template <typename... T>
   constexpr auto get_variant_type_name()
   {
      constexpr std::size_t size = sizeof("variant") +
         ((std::string_view(get_type_name((T*)nullptr)).size() + 1) + ...);
      std::array<char, size> buffer{'v', 'a', 'r', 'i', 'a', 'n', 't'};
      (variant_type_appender{buffer.data() + 7} + ... +
       std::string_view(get_type_name((T*)nullptr)));
      buffer[buffer.size() - 1] = '\0';
      return buffer;
   }

   template <typename... T>
   constexpr auto get_tuple_type_name()
   {
      constexpr std::size_t size = sizeof("tuple") +
         ((std::string_view(get_type_name((T*)nullptr)).size() + 1) + ... + 0);
      std::array<char, size> buffer{'t', 'u', 'p', 'l', 'e'};
      (variant_type_appender{buffer.data() + 5} + ... +
       std::string_view(get_type_name((T*)nullptr)));
      buffer[buffer.size() - 1] = '\0';
      return buffer;
   }

   template <typename... T>
   constexpr auto variant_type_name = get_variant_type_name<T...>();

   template <typename... T>
   constexpr const char* get_type_name(const std::variant<T...>*)
   {
      return variant_type_name<T...>.data();
   }

   template <typename... T>
   constexpr auto tuple_type_name = get_tuple_type_name<T...>();

   template <typename... T>
   constexpr const char* get_type_name(const std::tuple<T...>*)
   {
      return tuple_type_name<T...>.data();
   }

   template <typename Rep, typename Period>
   constexpr const char* get_type_name(
      const std::chrono::duration<Rep, Period>*)
   {
      return "duration";
   }

   template <typename Rep>
   constexpr const char* get_type_name(
      const std::chrono::time_point<std::chrono::system_clock,
                                     std::chrono::duration<Rep>>*)
   {
      return "TimePointSec";
   }

   template <typename Rep>
   constexpr const char* get_type_name(
      const std::chrono::time_point<std::chrono::system_clock,
                                     std::chrono::duration<Rep, std::micro>>*)
   {
      return "TimePointUSec";
   }

   template <typename T>
   constexpr const char* get_type_name()
   {
      return get_type_name((const T*)nullptr);
   }

   // ── Method-name → u64 ──────────────────────────────────────────────
   //
   // `hash_name` returns the 64-bit identifier for a method-name string,
   // delegating to `compress_name`.  Names that compress within the
   // arithmetic-coding budget produce a "compressed" u64; longer names
   // overflow into a fallback hash (bit 7 set).  `is_compressed_name`
   // distinguishes the two.
   //
   // Naming nit (carried from v1): the `hash_name` name is misleading
   // — most inputs return a *compressed* u64, not a hash.  Renaming
   // would be a follow-up coordinated with v1 callers.
   inline constexpr std::uint64_t hash_name(std::string_view str)
   {
      return detail::method_to_number(str);
   }

   inline constexpr bool is_compressed_name(std::uint64_t c)
   {
      return not detail::is_hash_name(c);
   }

}  // namespace psio
