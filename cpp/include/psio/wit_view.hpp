#pragma once
//
// psio/wit_view.hpp — view_layout::traits<psio::wit> specialization.
//
// Wit's canonical-ABI memory layout puts variable-length children
// (strings, vectors) on the heap, addressed by `(ptr:u32, len:u32)`
// pairs stored inline at the parent's field offset.  To follow these
// pointers the view system passes the absolute encoded buffer (`root`)
// alongside the local field span — `record_field_span(s, root)`.
//
// Inline (no pointer-follow):
//   - arithmetic / bool / enum / float
//   - Reflected struct (canonical-fixed footprint, even when its own
//     fields hold ptrs to heap data)
//   - std::optional<T> (canonical-ABI stores discriminator + inline payload)
//
// Heap-pointer-followed:
//   - std::string  → root.subspan(ptr, len)
//   - std::vector  → root.subspan(ptr, len * canonical_size<E>)
//
// vector_count / vector_element_span operate on the already-resolved
// heap span, so they don't need root.

#include <psio/view.hpp>
#include <psio/wit.hpp>
#include <psio/wit_abi.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace psio::view_layout {

   namespace detail_wit_view {

      template <typename T>
      struct is_std_vector : std::false_type {};
      template <typename T>
      struct is_std_vector<std::vector<T>> : std::true_type {};
      template <typename T>
      inline constexpr bool is_std_vector_v = is_std_vector<T>::value;

      template <typename T>
      struct is_std_string : std::false_type {};
      template <>
      struct is_std_string<std::string> : std::true_type {};
      template <typename T>
      inline constexpr bool is_std_string_v = is_std_string<T>::value;

   }  // namespace detail_wit_view

   template <>
   struct traits<::psio::wit>
   {
      // ── vector ─────────────────────────────────────────────────────────
      //
      // After record_field_span has followed the (ptr, len) pair to the
      // heap region, the resulting span holds `len` packed
      // canonical_size<T> elements.  count = size/elem_size; element span
      // is a fixed-size sub-slice.
      template <typename T>
      static std::size_t vector_count(std::span<const char> s) noexcept
      {
         constexpr std::size_t esz =
            ::psio::detail::wit_impl::canonical_size<T>();
         if constexpr (esz == 0)
            return 0;
         else
            return s.size() / esz;
      }

      template <typename T>
      static std::span<const char>
      vector_element_span(std::span<const char> s, std::size_t i) noexcept
      {
         constexpr std::size_t esz =
            ::psio::detail::wit_impl::canonical_size<T>();
         if (i * esz + esz > s.size())
            return {};
         return s.subspan(i * esz, esz);
      }

      // ── optional ───────────────────────────────────────────────────────
      //
      // Canonical-ABI option<T>: [u8 discriminator][padding to align<T>]
      // [canonical_size<T> payload].  No heap-pointer-follow.
      template <typename T>
      static bool optional_has_value(std::span<const char> s) noexcept
      {
         if (s.empty())
            return false;
         return static_cast<unsigned char>(s[0]) != 0;
      }

      template <typename T>
      static std::span<const char>
      optional_payload_span(std::span<const char> s) noexcept
      {
         constexpr std::size_t align =
            ::psio::detail::wit_impl::canonical_align<T>();
         constexpr std::size_t payload_off = align == 0 ? 1 : align;
         if (s.size() < payload_off)
            return {};
         return s.subspan(payload_off);
      }

      // ── Reflected struct field access ──────────────────────────────────
      //
      // For each field N of Reflected struct T:
      //   - std::string  → follow (ptr, len) into root → root.subspan(ptr, len)
      //   - std::vector  → follow (ptr, len) into root → root.subspan(ptr, len*esz)
      //   - everything else (arithmetic, bool, Reflected, optional) → inline
      //     subspan(offset, canonical_size<FieldT>).
      template <typename T, std::size_t N>
         requires ::psio::Reflected<T>
                  && (N < ::psio::reflect<T>::member_count)
      static std::span<const char>
      record_field_span(std::span<const char> s,
                        std::span<const char> root = {}) noexcept
      {
         using R      = ::psio::reflect<T>;
         using FieldT = std::remove_cvref_t<typename R::template member_type<N>>;
         constexpr std::size_t off = ::psio::wit_abi_field_offset_v<T, N>;

         if constexpr (detail_wit_view::is_std_string_v<FieldT>)
         {
            // (ptr, len) inline; data on heap.
            if (s.size() < off + 8)
               return {};
            std::uint32_t ptr, len;
            std::memcpy(&ptr, s.data() + off, 4);
            std::memcpy(&len, s.data() + off + 4, 4);
            if (root.empty() || std::size_t{ptr} + len > root.size())
               return {};
            return root.subspan(ptr, len);
         }
         else if constexpr (detail_wit_view::is_std_vector_v<FieldT>)
         {
            using E = typename FieldT::value_type;
            if (s.size() < off + 8)
               return {};
            std::uint32_t ptr, len;
            std::memcpy(&ptr, s.data() + off, 4);
            std::memcpy(&len, s.data() + off + 4, 4);
            constexpr std::size_t esz =
               ::psio::detail::wit_impl::canonical_size<E>();
            const std::size_t total = std::size_t{len} * esz;
            if (root.empty() || std::size_t{ptr} + total > root.size())
               return {};
            return root.subspan(ptr, total);
         }
         else
         {
            constexpr std::size_t sz =
               ::psio::detail::wit_impl::canonical_size<FieldT>();
            if (s.size() < off + sz)
               return {};
            return s.subspan(off, sz);
         }
      }
   };

}  // namespace psio::view_layout
