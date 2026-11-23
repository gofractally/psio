#pragma once
//
// psio/ssz_view.hpp — SSZ's `view_layout::traits` specialization.
//
// The unified view templates in `psio/view.hpp` are format-agnostic.
// This header wires SSZ's wire layout into them — no view templates
// are duplicated. Users get `view<std::vector<T>, ssz>`,
// `view<std::optional<T>, ssz>`, `view<std::variant<Ts...>, ssz>`
// simply by including this header alongside `psio/ssz.hpp`.
//
// Adding another format follows the same recipe: write a traits
// specialization in `<format>_view.hpp`, include it, and the same
// `view<T, Fmt>` template machinery lights up.
//
// Each function has a checked form (the safe view, `psio::ssz_view<T>`) and
// an unchecked one with a trailing `unchecked_t` (the trusted view,
// `psio::ssz_trusted_view<T>`, over bytes `psio::validate_ssz<T>` accepted):
// view.hpp "Safe and trusted views".

#include <psio/ssz.hpp>
#include <psio/view.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace psio::view_layout {

   // SSZ wire layout:
   //   vector<fixed T>:    span = n × fixed_size<T> bytes.
   //   vector<variable T>: span = [u32 offset_0][offset_1][...][payload]
   //                       where n = offset_0 / 4 and offset_i is the
   //                       byte offset of element i relative to the
   //                       start of the span.
   //   optional<T>:        span = [u8 selector][payload if selector != 0]
   //   variant<Ts...>:     span = [u8 selector][payload]
   template <>
   struct traits<::psio::ssz>
   {
      static constexpr bool trusted_views = true;

      // ── vector ─────────────────────────────────────────────────────────
      template <typename T>
      static std::size_t vector_count(std::span<const char> s) noexcept
      {
         if constexpr (::psio::detail::ssz_impl::is_fixed_v<T>)
         {
            constexpr std::size_t esz =
               ::psio::detail::ssz_impl::fixed_size_of<T>();
            if constexpr (esz == 0)
               return 0;
            else
               return s.size() / esz;
         }
         else
         {
            if (s.size() < 4)
               return 0;
            std::uint32_t first = 0;
            std::memcpy(&first, s.data(), 4);
            // The offset table lies inside the list: a first offset nobody
            // validated must not announce more elements than that (every
            // index below the count then has its offsets in the bytes).
            if (first > s.size())
               return 0;
            return first / 4u;
         }
      }

      template <typename T>
      static std::span<const char>
      vector_element_span(std::span<const char> s, std::size_t i) noexcept
      {
         if constexpr (::psio::detail::ssz_impl::is_fixed_v<T>)
         {
            constexpr std::size_t esz =
               ::psio::detail::ssz_impl::fixed_size_of<T>();
            // An index past the end is an empty view, never a span outside
            // the bytes.
            if constexpr (esz != 0)
               if (i >= s.size() / esz)
                  return {};
            return s.subspan(i * esz, esz);
         }
         else
         {
            const std::size_t n = vector_count<T>(s);
            if (i >= n)
               return {};
            std::uint32_t off_i = 0;
            std::memcpy(&off_i, s.data() + i * 4u, 4);
            std::uint32_t off_next;
            if (i + 1 < n)
               std::memcpy(&off_next, s.data() + (i + 1) * 4u, 4);
            else
               off_next = static_cast<std::uint32_t>(s.size());
            // Offsets nobody validated must not select bytes outside the list.
            if (off_i > off_next || off_next > s.size())
               return {};
            return s.subspan(off_i, off_next - off_i);
         }
      }

      // The unchecked forms (trusted view).
      template <typename T>
      static std::size_t vector_count(std::span<const char> s, unchecked_t) noexcept
      {
         if constexpr (::psio::detail::ssz_impl::is_fixed_v<T>)
            return vector_count<T>(s);
         else
         {
            // An empty list is zero bytes (and so is a view of nothing).
            if (s.empty())
               return 0;
            PSIO_VIEW_TRUST(s.size() >= 4, "a list is shorter than its first offset");
            std::uint32_t first = 0;
            std::memcpy(&first, s.data(), 4);
            PSIO_VIEW_TRUST(first <= s.size(), "the offset table of a list is cut short");
            return first / 4u;
         }
      }

      template <typename T>
      [[gnu::always_inline]] static std::span<const char> vector_element_span(
         std::span<const char> s, std::size_t i, unchecked_t) noexcept
      {
         if constexpr (::psio::detail::ssz_impl::is_fixed_v<T>)
         {
            constexpr std::size_t esz = ::psio::detail::ssz_impl::fixed_size_of<T>();
            PSIO_VIEW_TRUST(i * esz + esz <= s.size(), "a list element lies past the list");
            return {s.data() + i * esz, esz};
         }
         else
         {
            PSIO_VIEW_TRUST(s.size() >= 4, "a list is shorter than its first offset");
            std::uint32_t table = 0;
            std::memcpy(&table, s.data(), 4);
            PSIO_VIEW_TRUST(table <= s.size() && (i + 1) * 4 <= table,
                            "a list element lies past the offset table");
            std::uint32_t start = 0;
            std::memcpy(&start, s.data() + i * 4u, 4);
            std::size_t stop = s.size();
            if ((i + 1) * 4u < table)
            {
               std::uint32_t next = 0;
               std::memcpy(&next, s.data() + (i + 1) * 4u, 4);
               stop = next;
            }
            PSIO_VIEW_TRUST(start <= stop && stop <= s.size(),
                            "the offsets of a list element are out of order or out of range");
            return {s.data() + start, stop - start};
         }
      }

      // ── optional ───────────────────────────────────────────────────────
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
         if (s.empty())
            return {};
         return s.subspan(1);
      }

      // ── variant ────────────────────────────────────────────────────────
      template <typename... Ts>
      static std::size_t variant_index(std::span<const char> s) noexcept
      {
         if (s.empty())
            return 0;
         return static_cast<std::size_t>(
            static_cast<unsigned char>(s[0]));
      }

      template <typename... Ts>
      static std::span<const char>
      variant_payload_span(std::span<const char> s) noexcept
      {
         if (s.empty())
            return {};
         return s.subspan(1);
      }

      // The unchecked forms (trusted view). A view of nothing (default-
      // constructed, `at(i)` past the end) has a null span.
      template <typename T>
      static bool optional_has_value(std::span<const char> s, unchecked_t) noexcept
      {
         if (s.data() == nullptr)
            return false;
         PSIO_VIEW_TRUST(!s.empty(), "an optional has no selector byte");
         return static_cast<unsigned char>(s[0]) != 0;
      }
      template <typename T>
      static std::span<const char> optional_payload_span(std::span<const char> s,
                                                         unchecked_t) noexcept
      {
         if (s.data() == nullptr)
            return {};
         PSIO_VIEW_TRUST(!s.empty(), "an optional has no selector byte");
         return {s.data() + 1, s.size() - 1};
      }
      template <typename... Ts>
      static std::size_t variant_index(std::span<const char> s, unchecked_t) noexcept
      {
         if (s.data() == nullptr)
            return 0;
         PSIO_VIEW_TRUST(!s.empty(), "a variant has no selector byte");
         return static_cast<std::size_t>(static_cast<unsigned char>(s[0]));
      }
      template <typename... Ts>
      static std::span<const char> variant_payload_span(std::span<const char> s,
                                                        unchecked_t) noexcept
      {
         if (s.data() == nullptr)
            return {};
         PSIO_VIEW_TRUST(!s.empty(), "a variant has no selector byte");
         return {s.data() + 1, s.size() - 1};
      }

      // ── Reflected struct field access ──────────────────────────────────
      //
      // SSZ record wire layout:
      //   - Fixed region: each field N is at byte offset
      //     sum(slot_size_of(field_i) for i < N), where slot_size_of is
      //     fixed_size_of for fixed fields and 4 (u32 offset) for
      //     variable fields.
      //   - Heap region: variable-field payloads, in declaration order,
      //     each pointed to by its 4-byte offset slot in the fixed region.
      //
      // record_field_span returns the byte range for field N's *value*:
      //   - fixed field   → s.subspan(fixed_off, fixed_size_of<FieldT>())
      //   - variable field → s.subspan(load(s.data()+fixed_off),
      //                                 load(next_var_off) - load(this_off))
      //                      For the last variable field, the end is s.size().
     private:
      template <typename T, std::size_t N>
      static consteval std::size_t ssz_slot_size_of_field()
      {
         using R   = ::psio::reflect<T>;
         using F_N = std::remove_cvref_t<typename R::template member_type<N>>;
         if constexpr (::psio::detail::ssz_impl::is_fixed_v<F_N>)
            return ::psio::detail::ssz_impl::fixed_size_of<F_N>();
         else
            return std::size_t{4};  // ssz variable-field offset slot is u32
      }

      template <typename T, std::size_t N>
      static consteval std::size_t ssz_fixed_region_offset_of_field()
      {
         std::size_t off = 0;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((off += ssz_slot_size_of_field<T, Is>()), ...);
         }(std::make_index_sequence<N>{});
         return off;
      }

     private:
      // Find the fixed-region offset slot of the FIRST variable field
      // whose index is > N.  Returns 0 if no later variable field exists
      // (caller treats 0 as "use end-of-buffer for end_off").
      template <typename T, std::size_t N, std::size_t I = N + 1>
      static consteval std::size_t ssz_next_var_slot_after()
      {
         using R = ::psio::reflect<T>;
         if constexpr (I >= R::member_count)
            return 0;
         else
         {
            using F_I = std::remove_cvref_t<
               typename R::template member_type<I>>;
            if constexpr (!::psio::detail::ssz_impl::is_fixed_v<F_I>)
               return ssz_fixed_region_offset_of_field<T, I>();
            else
               return ssz_next_var_slot_after<T, N, I + 1>();
         }
      }

     public:
      template <typename T, std::size_t N>
         requires ::psio::detail::ssz_impl::Record<T>
                  && (N < ::psio::reflect<T>::member_count)
      static std::span<const char>
      record_field_span(std::span<const char> s,
                        std::span<const char> /*root*/ = {}) noexcept
      {
         using R      = ::psio::reflect<T>;
         using FieldT = std::remove_cvref_t<typename R::template member_type<N>>;
         constexpr std::size_t fixed_off =
            ssz_fixed_region_offset_of_field<T, N>();

         if constexpr (::psio::detail::ssz_impl::is_fixed_v<FieldT>)
         {
            constexpr std::size_t sz =
               ::psio::detail::ssz_impl::fixed_size_of<FieldT>();
            if (s.size() < fixed_off + sz)
               return {};
            return s.subspan(fixed_off, sz);
         }
         else
         {
            // Variable field: read u32 offset at fixed_off; the payload
            // runs until the next variable field's offset (or end-of-
            // buffer if this is the last variable field).
            if (s.size() < fixed_off + 4)
               return {};
            std::uint32_t this_off = 0;
            std::memcpy(&this_off, s.data() + fixed_off, 4);

            constexpr std::size_t next_var_slot =
               ssz_next_var_slot_after<T, N>();

            std::uint32_t end_off;
            if constexpr (next_var_slot == 0)
               end_off = static_cast<std::uint32_t>(s.size());
            else
            {
               if (s.size() < next_var_slot + 4)
                  return {};
               std::memcpy(&end_off, s.data() + next_var_slot, 4);
            }
            if (this_off > end_off || end_off > s.size())
               return {};
            return s.subspan(this_off, end_off - this_off);
         }
      }

      // The unchecked form (trusted view): one null test for a view of
      // nothing, no compare of an offset with the record's size.
      template <typename T, std::size_t N>
         requires ::psio::detail::ssz_impl::Record<T>
                  && (N < ::psio::reflect<T>::member_count)
      [[gnu::always_inline]] static std::span<const char> record_field_span(
         std::span<const char> s, std::span<const char> /*root*/, unchecked_t) noexcept
      {
         using R      = ::psio::reflect<T>;
         using FieldT = std::remove_cvref_t<typename R::template member_type<N>>;
         constexpr std::size_t fixed_off = ssz_fixed_region_offset_of_field<T, N>();
         if (s.data() == nullptr)
            return {};
         if constexpr (::psio::detail::ssz_impl::is_fixed_v<FieldT>)
         {
            constexpr std::size_t sz = ::psio::detail::ssz_impl::fixed_size_of<FieldT>();
            PSIO_VIEW_TRUST(s.size() >= fixed_off + sz, "a record is cut short");
            return {s.data() + fixed_off, sz};
         }
         else
         {
            PSIO_VIEW_TRUST(s.size() >= fixed_off + 4, "a record is cut short");
            std::uint32_t this_off = 0;
            std::memcpy(&this_off, s.data() + fixed_off, 4);
            constexpr std::size_t next_var_slot = ssz_next_var_slot_after<T, N>();
            std::size_t           end_off       = s.size();
            if constexpr (next_var_slot != 0)
            {
               PSIO_VIEW_TRUST(s.size() >= next_var_slot + 4, "a record is cut short");
               std::uint32_t next = 0;
               std::memcpy(&next, s.data() + next_var_slot, 4);
               end_off = next;
            }
            PSIO_VIEW_TRUST(this_off <= end_off && end_off <= s.size(),
                            "the offsets of a record member are out of order or out of range");
            return {s.data() + this_off, end_off - this_off};
         }
      }
   };

}  // namespace psio::view_layout
