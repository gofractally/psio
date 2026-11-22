#pragma once
//
// psio/fixed_string.hpp — a compile-time string that can be a template
// argument.
//
// std::string_view is not a structural type, so it cannot be a non-type
// template argument, and it cannot be concatenated in a constant
// expression. psio::fixed_string<N> holds exactly N characters (plus a
// terminating NUL) by value, is structural, and concatenates and compares
// in constant expressions:
//
//     template <psio::fixed_string Name>
//     struct Edge { static constexpr std::string_view name = Name; };
//
//     Edge<"UserEdge">                                    // from a literal
//     Edge<psio::reflect<User>::fixed_name + "Edge">      // "UserEdge"
//     Edge<psio::fixed_string{"User"} + "Edge">           // the same type
//
// Two fixed_strings with the same characters are the same template
// argument; the length is part of the type.
//
//   fixed_string{"text"}            deduces fixed_string<4>
//   fixed_string<N>{string_view}    explicit; the view must hold N characters
//   a + b, a + "lit", "lit" + a     fixed_string<N + M>
//   a == b, a <=> b                 by characters, across lengths, and
//                                   against anything std::string_view takes
//   .size() .data() .c_str() .view()  and an implicit std::string_view
//
// reflect.hpp provides psio::reflect<T>::fixed_name (and
// psio::reflect_enum<E>::fixed_name): the reflected name in this form.

#include <compare>
#include <cstddef>
#include <string_view>

namespace psio
{
   template <std::size_t N>
   struct fixed_string
   {
      // Public so that the type is structural. N characters and a NUL.
      char chars[N + 1]{};

      constexpr fixed_string() noexcept = default;
      // From a literal of exactly N characters (CTAD: fixed_string{"abc"}).
      constexpr fixed_string(const char (&text)[N + 1]) noexcept
      {
         for (std::size_t i = 0; i < N; ++i)
            chars[i] = text[i];
      }
      // From a view of exactly N characters. A view of another length is not
      // a constant expression (and is truncated / NUL-padded at run time).
      constexpr explicit fixed_string(std::string_view text) noexcept
      {
         if (text.size() != N)
            length_mismatch();
         for (std::size_t i = 0; i < N && i < text.size(); ++i)
            chars[i] = text[i];
      }

      static constexpr std::size_t size() noexcept { return N; }
      static constexpr bool        empty() noexcept { return N == 0; }
      constexpr const char*        data() const noexcept { return chars; }
      constexpr const char*        c_str() const noexcept { return chars; }
      constexpr std::string_view   view() const noexcept { return {chars, N}; }
      constexpr operator std::string_view() const noexcept { return view(); }
      constexpr char        operator[](std::size_t i) const noexcept { return chars[i]; }
      constexpr const char* begin() const noexcept { return chars; }
      constexpr const char* end() const noexcept { return chars + N; }

    private:
      // Not constexpr: reaching it in a constant expression is the error.
      static void length_mismatch() noexcept {}
   };

   template <std::size_t N>
   fixed_string(const char (&)[N]) -> fixed_string<N - 1>;

   // ── Concatenation ─────────────────────────────────────────────────────
   template <std::size_t N, std::size_t M>
   constexpr fixed_string<N + M> operator+(const fixed_string<N>& a,
                                           const fixed_string<M>& b) noexcept
   {
      fixed_string<N + M> out;
      for (std::size_t i = 0; i < N; ++i)
         out.chars[i] = a.chars[i];
      for (std::size_t i = 0; i < M; ++i)
         out.chars[N + i] = b.chars[i];
      return out;
   }
   template <std::size_t N, std::size_t M>
   constexpr fixed_string<N + M - 1> operator+(const fixed_string<N>& a,
                                               const char (&b)[M]) noexcept
   {
      return a + fixed_string<M - 1>{b};
   }
   template <std::size_t N, std::size_t M>
   constexpr fixed_string<N + M - 1> operator+(const char (&a)[N],
                                               const fixed_string<M>& b) noexcept
   {
      return fixed_string<N - 1>{a} + b;
   }

   // ── Comparison: by characters, whatever the lengths ───────────────────
   template <std::size_t N, std::size_t M>
   constexpr bool operator==(const fixed_string<N>& a, const fixed_string<M>& b) noexcept
   {
      return a.view() == b.view();
   }
   template <std::size_t N, std::size_t M>
   constexpr std::strong_ordering operator<=>(const fixed_string<N>& a,
                                              const fixed_string<M>& b) noexcept
   {
      return a.view() <=> b.view();
   }
   template <std::size_t N>
   constexpr bool operator==(const fixed_string<N>& a, std::string_view b) noexcept
   {
      return a.view() == b;
   }
   template <std::size_t N>
   constexpr std::strong_ordering operator<=>(const fixed_string<N>& a,
                                              std::string_view        b) noexcept
   {
      return a.view() <=> b;
   }
}  // namespace psio
