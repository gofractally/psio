#pragma once
//
// psio/pssz_view.hpp — pSSZ's `view_layout::traits` specializations.
//
// Two tags to cover:
//   - `psio::pssz_<W>`   — explicit offset width (1, 2, or 4 bytes)
//   - `psio::pssz`       — auto-W: W = auto_pssz_width_v<T>
//
// Wire layout (differs from SSZ only in a few places):
//
//   vector<fixed T>     — n × fixed_size<T> raw bytes, no prefix.
//   vector<variable T>  — [uW offset_0][uW offset_1]...[payload],
//                         n = offset_0 / W (offsets are W bytes, not
//                         u32 like SSZ).
//   optional<fixed V>   — span is either empty (None) or
//                         fixed_size<V> bytes (Some). NO selector.
//                         Length disambiguates.
//   optional<variable V>— [u8 selector][payload if selector == 1].
//                         Same as SSZ.
//   variant<Ts...>      — [u8 selector][payload]. Same as SSZ.
//
// All three composite view templates from psio/view.hpp are written
// once and reused here; this header only plugs in the wire policy.

#include <psio/pssz.hpp>
#include <psio/view.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

namespace psio::view_layout {

   namespace detail_pssz {

      // Shared helpers parameterized on the explicit offset width W.
      // The pssz / pssz_<W> traits both forward here; the only
      // difference is how W is picked (compile-time constant for
      // pssz_<W>, auto_pssz_width_v<T> for pssz).

      // ── vector ─────────────────────────────────────────────────────────
      template <std::size_t W, typename T>
      constexpr std::size_t vector_count(std::span<const char> s) noexcept
      {
         if constexpr (::psio::detail::pssz_impl::is_fixed_v<T>)
         {
            constexpr std::size_t esz =
               ::psio::detail::pssz_impl::fixed_size_of<T>();
            if constexpr (esz == 0)
               return 0;
            else
               return s.size() / esz;
         }
         else
         {
            if (s.size() < W)
               return 0;
            using O = typename ::psio::detail::pssz_impl::width_info<W>::offset_t;
            O first{};
            std::memcpy(&first, s.data(), W);
            return static_cast<std::size_t>(first) / W;
         }
      }

      template <std::size_t W, typename T>
      std::span<const char>
      vector_element_span(std::span<const char> s, std::size_t i) noexcept
      {
         if constexpr (::psio::detail::pssz_impl::is_fixed_v<T>)
         {
            constexpr std::size_t esz =
               ::psio::detail::pssz_impl::fixed_size_of<T>();
            return s.subspan(i * esz, esz);
         }
         else
         {
            const std::size_t n = vector_count<W, T>(s);
            if (i >= n)
               return {};
            using O = typename ::psio::detail::pssz_impl::width_info<W>::offset_t;
            O off_i{};
            std::memcpy(&off_i, s.data() + i * W, W);
            std::uint32_t off_next;
            if (i + 1 < n)
            {
               O tmp{};
               std::memcpy(&tmp, s.data() + (i + 1) * W, W);
               off_next = static_cast<std::uint32_t>(tmp);
            }
            else
            {
               off_next = static_cast<std::uint32_t>(s.size());
            }
            return s.subspan(static_cast<std::uint32_t>(off_i),
                              off_next - static_cast<std::uint32_t>(off_i));
         }
      }

      // ── optional ───────────────────────────────────────────────────────
      //
      // pssz splits on the payload's fixed/variable status:
      //   fixed V    — no selector. Empty span = None; fixed_size<V>
      //                 bytes = Some.
      //   variable V — [u8 selector][payload if selector != 0].
      template <typename T>
      bool optional_has_value(std::span<const char> s) noexcept
      {
         using V = typename T::value_type;
         if constexpr (::psio::detail::pssz_impl::is_fixed_v<V>)
            return !s.empty();
         else
         {
            if (s.empty())
               return false;
            return static_cast<unsigned char>(s[0]) != 0;
         }
      }

      template <typename T>
      std::span<const char>
      optional_payload_span(std::span<const char> s) noexcept
      {
         using V = typename T::value_type;
         if constexpr (::psio::detail::pssz_impl::is_fixed_v<V>)
            return s;
         else
            return s.empty() ? std::span<const char>{} : s.subspan(1);
      }

      // ── variant ────────────────────────────────────────────────────────
      inline std::size_t variant_index(std::span<const char> s) noexcept
      {
         if (s.empty())
            return 0;
         return static_cast<std::size_t>(
            static_cast<unsigned char>(s[0]));
      }

      inline std::span<const char>
      variant_payload_span(std::span<const char> s) noexcept
      {
         return s.empty() ? std::span<const char>{} : s.subspan(1);
      }

   }  // namespace detail_pssz

   // ── pssz_<W> — explicit width ─────────────────────────────────────────
   template <std::size_t W>
   struct traits<::psio::pssz_<W>>
   {
      template <typename T>
      static std::size_t vector_count(std::span<const char> s) noexcept
      {
         return detail_pssz::vector_count<W, T>(s);
      }

      template <typename T>
      static std::span<const char>
      vector_element_span(std::span<const char> s, std::size_t i) noexcept
      {
         return detail_pssz::vector_element_span<W, T>(s, i);
      }

      template <typename T>
      static bool optional_has_value(std::span<const char> s) noexcept
      {
         return detail_pssz::optional_has_value<std::optional<T>>(s);
      }

      template <typename T>
      static std::span<const char>
      optional_payload_span(std::span<const char> s) noexcept
      {
         return detail_pssz::optional_payload_span<std::optional<T>>(s);
      }

      template <typename... Ts>
      static std::size_t variant_index(std::span<const char> s) noexcept
      {
         return detail_pssz::variant_index(s);
      }

      template <typename... Ts>
      static std::span<const char>
      variant_payload_span(std::span<const char> s) noexcept
      {
         return detail_pssz::variant_payload_span(s);
      }
   };

   // ── pssz — auto-W (W chosen from ViewedType) ──────────────────────────
   //
   // The view knows the outer shape it represents (std::vector<T> /
   // std::optional<T> / std::variant<Ts...>) and picks W to match what
   // the encoder picked for that shape via auto_pssz_width_v.
   template <>
   struct traits<::psio::pssz>
   {
      template <typename T>
      static std::size_t vector_count(std::span<const char> s) noexcept
      {
         constexpr std::size_t W =
            ::psio::auto_pssz_width_v<std::vector<T>>;
         return detail_pssz::vector_count<W, T>(s);
      }

      template <typename T>
      static std::span<const char>
      vector_element_span(std::span<const char> s, std::size_t i) noexcept
      {
         constexpr std::size_t W =
            ::psio::auto_pssz_width_v<std::vector<T>>;
         return detail_pssz::vector_element_span<W, T>(s, i);
      }

      template <typename T>
      static bool optional_has_value(std::span<const char> s) noexcept
      {
         return detail_pssz::optional_has_value<std::optional<T>>(s);
      }

      template <typename T>
      static std::span<const char>
      optional_payload_span(std::span<const char> s) noexcept
      {
         return detail_pssz::optional_payload_span<std::optional<T>>(s);
      }

      template <typename... Ts>
      static std::size_t variant_index(std::span<const char> s) noexcept
      {
         return detail_pssz::variant_index(s);
      }

      template <typename... Ts>
      static std::span<const char>
      variant_payload_span(std::span<const char> s) noexcept
      {
         return detail_pssz::variant_payload_span(s);
      }

      // ── Reflected struct field access ──────────────────────────────────
      //
      // For DWNC records: layout matches ssz (no header); offsets and
      // slot sizes are prefix-sum from byte 0.
      //
      // For non-DWNC records: pssz prepends a `W`-byte fixed_size header
      // (forward-compat slot — newer decoder reads this to know how many
      // bytes of fixed region were actually written by the writer).  We
      // strip this header at the start so all subsequent offsets are
      // relative to container_start (matching encode's convention).
      //
      // Variable-field offset slots are `W` bytes (matches the format's
      // pssz_<W> width or auto-width).
     private:
      template <std::size_t W, typename T, std::size_t I>
      static consteval std::size_t pssz_slot_size_of_field()
      {
         using R   = ::psio::reflect<T>;
         using F_I = std::remove_cvref_t<typename R::template member_type<I>>;
         if constexpr (::psio::detail::pssz_impl::is_fixed_v<F_I>)
            return ::psio::detail::pssz_impl::fixed_size_of<F_I>();
         else
            return W;
      }

      template <std::size_t W, typename T, std::size_t N>
      static consteval std::size_t pssz_fixed_region_offset_of_field()
      {
         std::size_t off = 0;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((off += pssz_slot_size_of_field<W, T, Is>()), ...);
         }(std::make_index_sequence<N>{});
         return off;
      }

      template <std::size_t W, typename T, std::size_t N,
                std::size_t I = N + 1>
      static consteval std::size_t pssz_next_var_slot_after()
      {
         using R = ::psio::reflect<T>;
         if constexpr (I >= R::member_count)
            return 0;
         else
         {
            using F_I = std::remove_cvref_t<
               typename R::template member_type<I>>;
            if constexpr (!::psio::detail::pssz_impl::is_fixed_v<F_I>)
               return pssz_fixed_region_offset_of_field<W, T, I>();
            else
               return pssz_next_var_slot_after<W, T, N, I + 1>();
         }
      }

     public:
      template <typename T, std::size_t N>
         requires ::psio::Reflected<T>
                  && (N < ::psio::reflect<T>::member_count)
      static std::span<const char>
      record_field_span(std::span<const char> s,
                        std::span<const char> /*root*/ = {}) noexcept
      {
         constexpr std::size_t W = ::psio::auto_pssz_width_v<T>;
         using R      = ::psio::reflect<T>;
         using FieldT = std::remove_cvref_t<typename R::template member_type<N>>;

         // Non-DWNC: strip the W-byte fixed_size header so subsequent
         // offsets are relative to container_start.
         if constexpr (!::psio::is_dwnc_v<T>)
         {
            if (s.size() < W)
               return {};
            s = s.subspan(W);
         }

         constexpr std::size_t fixed_off =
            pssz_fixed_region_offset_of_field<W, T, N>();

         if constexpr (::psio::detail::pssz_impl::is_fixed_v<FieldT>)
         {
            constexpr std::size_t sz =
               ::psio::detail::pssz_impl::fixed_size_of<FieldT>();
            if (s.size() < fixed_off + sz)
               return {};
            return s.subspan(fixed_off, sz);
         }
         else
         {
            if (s.size() < fixed_off + W)
               return {};
            using O = typename
               ::psio::detail::pssz_impl::width_info<W>::offset_t;
            O this_off_o{};
            std::memcpy(&this_off_o, s.data() + fixed_off, W);
            std::uint32_t this_off =
               static_cast<std::uint32_t>(this_off_o);

            constexpr std::size_t next_var_slot =
               pssz_next_var_slot_after<W, T, N>();
            std::uint32_t end_off;
            if constexpr (next_var_slot == 0)
               end_off = static_cast<std::uint32_t>(s.size());
            else
            {
               if (s.size() < next_var_slot + W)
                  return {};
               O next_o{};
               std::memcpy(&next_o, s.data() + next_var_slot, W);
               end_off = static_cast<std::uint32_t>(next_o);
            }
            if (this_off > end_off || end_off > s.size())
               return {};
            return s.subspan(this_off, end_off - this_off);
         }
      }
   };

}  // namespace psio::view_layout
