#pragma once
//
// psio/to_hex.hpp — hex string conversion utilities.  Direct port of
// psio/to_hex.hpp.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace psio {

   // (`psio::hex` is the hex presentation tag of annotate.hpp; the
   // conversion functions are all named to_hex / from_hex.)
   template <typename SrcIt, typename DestIt>
   void to_hex(SrcIt begin, SrcIt end, DestIt dest,
               std::optional<char> delimiter = std::nullopt)
   {
      auto nibble = [&dest](std::uint8_t i) {
         if (i <= 9)
            *dest++ = '0' + i;
         else
            *dest++ = 'A' + i - 10;
      };

      bool first = true;
      while (begin != end)
      {
         if (!first && delimiter)
            *dest++ = *delimiter;
         first = false;

         nibble(((std::uint8_t)*begin) >> 4);
         nibble(((std::uint8_t)*begin) & 0xf);
         ++begin;
      }
   }

   template <typename SrcIt>
   std::string to_hex(SrcIt begin, SrcIt end,
                      std::optional<char> delimiter = std::nullopt)
   {
      std::string s;
      std::size_t size = (end - begin) * 2;
      if (delimiter && begin != end)
         size += (end - begin - 1);
      s.reserve(size);
      to_hex(begin, end, std::back_inserter(s), delimiter);
      return s;
   }

   inline std::string to_hex(const std::span<const char>& bytes,
                             std::optional<char> delimiter = std::nullopt)
   {
      return to_hex(bytes.begin(), bytes.end(), delimiter);
   }

   inline bool from_hex(std::string_view h, std::vector<char>& bytes)
   {
      bytes.resize(h.size() / 2);
      auto dest = bytes.begin();

      auto begin     = h.begin();
      auto end       = h.end();
      auto get_digit = [&](std::uint8_t& nibble) {
         if (*begin >= '0' && *begin <= '9')
            nibble = *begin++ - '0';
         else if (*begin >= 'a' && *begin <= 'f')
            nibble = *begin++ - 'a' + 10;
         else if (*begin >= 'A' && *begin <= 'F')
            nibble = *begin++ - 'A' + 10;
         else
            return false;
         return true;
      };
      if (h.size() % 2 != 0)
         return false;  // (and never read a second digit past the end)
      while (begin != end)
      {
         std::uint8_t h, l;
         if (!get_digit(h) || !get_digit(l))
            return false;
         *dest++ = (h << 4) | l;
      }
      return true;
   }

   // Into a destination of fixed size — a std::array<unsigned char, 32>, a
   // std::span<char>, a char[N] — which the text must fill exactly:
   // 2 * size hex digits, no more and no fewer. Nothing is allocated; on
   // failure the destination holds the bytes read so far.
   template <typename Dest>
      requires (!std::is_same_v<Dest, std::vector<char>> &&
                requires(Dest& d) {
                   { std::data(d) };
                   { std::size(d) } -> std::convertible_to<std::size_t>;
                } &&
                sizeof(*std::data(std::declval<Dest&>())) == 1 &&
                !std::is_const_v<std::remove_reference_t<decltype(*std::data(std::declval<Dest&>()))>>)
   bool from_hex(std::string_view text, Dest&& dest)
   {
      const std::size_t size = std::size(dest);
      if (text.size() != 2 * size)
         return false;
      auto digit = [](char c, std::uint8_t& nibble) {
         if (c >= '0' && c <= '9')
            nibble = static_cast<std::uint8_t>(c - '0');
         else if (c >= 'a' && c <= 'f')
            nibble = static_cast<std::uint8_t>(c - 'a' + 10);
         else if (c >= 'A' && c <= 'F')
            nibble = static_cast<std::uint8_t>(c - 'A' + 10);
         else
            return false;
         return true;
      };
      auto* out = std::data(dest);
      for (std::size_t i = 0; i < size; ++i)
      {
         std::uint8_t high = 0, low = 0;
         if (!digit(text[2 * i], high) || !digit(text[2 * i + 1], low))
            return false;
         out[i] = static_cast<std::remove_reference_t<decltype(*out)>>((high << 4) | low);
      }
      return true;
   }

}  // namespace psio
