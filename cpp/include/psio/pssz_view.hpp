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
//   optional<V>, min(V) > 0  — empty span (None) or the bare encoding
//                         of V (Some). NO selector; length disambiguates.
//   optional<V>, min(V) == 0 — [u8 selector][payload if selector == 1].
//   record              — [uW fixed-region size unless DWNC][fixed region]
//                         [payloads]. The header is the writer's region
//                         size: trailing optional fields may be absent
//                         (pruned None) and unknown appended optional slots
//                         may follow the reader's known fields.
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
      // pssz splits on the payload's minimum encoded size, using the same
      // predicate as the codec (pssz_impl::optional_uses_selector_v):
      //   min(V) > 0  — no selector. Empty span = None; otherwise Some.
      //   min(V) == 0 — [u8 selector][payload if selector != 0].
      template <typename T>
      bool optional_has_value(std::span<const char> s) noexcept
      {
         using V = typename T::value_type;
         if constexpr (!::psio::detail::pssz_impl::optional_uses_selector_v<V>)
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
         if constexpr (!::psio::detail::pssz_impl::optional_uses_selector_v<V>)
            return s;
         else
            return s.empty() ? std::span<const char>{} : s.subspan(1);
      }

      // ── record ─────────────────────────────────────────────────────────
      //
      // Field N of record T at offset width W. Reads the writer's header so
      // a known trailing optional the writer pruned yields the canonical
      // None payload, and unknown appended slots bound the last known
      // variable field. Getters keep their O(1) prevalidated-input contract.
      template <std::size_t W, typename T, std::size_t N>
      std::span<const char> record_field_span(std::span<const char> bytes) noexcept
      {
         using namespace ::psio::detail::pssz_impl;
         constexpr auto fields = record_layout<W, T>(
            std::make_index_sequence<record_traits<T>::member_count>{});
         constexpr auto fixed_off = [&] {
            std::size_t n = 0;
            for (std::size_t i = 0; i < N; ++i) n += fields[i].fixed_size;
            return n;
         }();
         constexpr auto known_size = [&] {
            std::size_t n = 0;
            for (auto field : fields) n += field.fixed_size;
            return n;
         }();
         constexpr auto next_slot = [&] {
            std::size_t pos = fixed_off + fields[N].fixed_size;
            for (std::size_t i = N + 1; i < fields.size(); ++i)
            {
               if (fields[i].variable) return pos;
               pos += fields[i].fixed_size;
            }
            return known_size;
         }();
         std::size_t fixed_size = known_size;
         if constexpr (!::psio::is_dwnc_v<T>)
         {
            if (bytes.size() < W) return {};
            fixed_size = read_offset<W>(bytes, 0);
            bytes = bytes.subspan(W);
         }
         if (fixed_size > bytes.size()) return {};
         if (fixed_off > fixed_size || fields[N].fixed_size > fixed_size - fixed_off)
         {
            if constexpr (omittable_field_v<T, N>)
            {
               using F = typename record_traits<T>::template member_type<N>;
               // A selector-carrying optional has a one-byte None; a
               // length-disambiguated optional's None is the empty span.
               if constexpr (optional_uses_selector_v<typename F::value_type>)
               {
                  static constexpr char none = 0;
                  return {&none, 1};
               }
            }
            return {};
         }
         if constexpr (!fields[N].variable)
            return bytes.subspan(fixed_off, fields[N].fixed_size);
         else
         {
            const auto start = read_offset<W>(bytes, fixed_off);
            std::size_t stop = bytes.size();
            // A known next slot can have been pruned; an unknown optional
            // suffix begins exactly at the reader's known fixed size.
            if (next_slot < fixed_size)
            {
               if (W > fixed_size - next_slot) return {};
               stop = read_offset<W>(bytes, next_slot);
            }
            if (start < fixed_size || start > stop || stop > bytes.size()) return {};
            return bytes.subspan(start, stop - start);
         }
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
      template <typename T, std::size_t N>
         requires (::psio::detail::pssz_impl::Record<T> &&
                   N < ::psio::detail::pssz_impl::record_traits<T>::member_count)
      static std::span<const char> record_field_span(
         std::span<const char> s, std::span<const char> = {}) noexcept
      { return detail_pssz::record_field_span<W, T, N>(s); }

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

      template <typename T, std::size_t N>
      static std::span<const char> array_element_span(std::span<const char> s, std::size_t i) noexcept
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
   //
   // Automatic widths belong to each logical type (docs/pssz-type-width.md),
   // so child views stay in the automatic profile and re-select their own
   // width. This trait therefore deliberately defines no `nested_format`:
   // a parent's width is never inherited by its children.
   template <>
   struct traits<::psio::pssz>
   {

      template <typename T, std::size_t N>
         requires (::psio::detail::pssz_impl::Record<T> &&
                   N < ::psio::detail::pssz_impl::record_traits<T>::member_count)
      static std::span<const char> record_field_span(
         std::span<const char> s, std::span<const char> = {}) noexcept
      { return detail_pssz::record_field_span<::psio::auto_pssz_width_v<T>, T, N>(s); }

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

      // A fixed-count array owns its own logical width, which can differ
      // from a vector of the same element type.
      template <typename T, std::size_t N>
      static std::span<const char> array_element_span(std::span<const char> s, std::size_t i) noexcept
      {
         constexpr auto W = ::psio::auto_pssz_width_v<std::array<T, N>>;
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

}  // namespace psio::view_layout

namespace psio {
   namespace detail {
      template <typename F> inline constexpr bool is_pssz_format_v = false;
      template <> inline constexpr bool is_pssz_format_v<pssz> = true;
      template <std::size_t W> inline constexpr bool is_pssz_format_v<pssz_<W>> = true;
   }

   template <typename... Ts, typename Fmt, storage Store>
      requires (detail::is_pssz_format_v<Fmt> &&
                !::psio::format_should_dispatch_adapter_v<Fmt, std::tuple<Ts...>>)
   class view<std::tuple<Ts...>, Fmt, Store>
   {
      std::span<const char> data_;
    public:
      using element_type = std::tuple<Ts...>;
      using format_type = Fmt;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}
      view(std::span<const char> s, std::span<const char>) noexcept : data_(s) {}
      std::span<const char> _psio_data() const noexcept { return data_; }
      template <std::size_t N> auto get() const noexcept
      {
         using F = std::tuple_element_t<N, element_type>;
         auto span = view_layout::traits<Fmt>::template record_field_span<element_type, N>(data_);
         if constexpr (std::is_arithmetic_v<F>)
         {
            F out{};
            if (span.size() == sizeof(F)) std::memcpy(&out, span.data(), sizeof(F));
            return out;
         }
         else return view<F, view_layout::nested_format_t<Fmt, element_type>, Store>{span};
      }
   };

   // A std::pair is the 2-tuple of its members: the tuple's view, with the
   // members also reachable as first() / second().
   template <typename A, typename B, typename Fmt, storage Store>
      requires (detail::is_pssz_format_v<Fmt> &&
                !::psio::format_should_dispatch_adapter_v<Fmt, std::pair<A, B>>)
   class view<std::pair<A, B>, Fmt, Store>
       : public view<typename detail::is_std_pair<std::pair<A, B>>::tuple_type, Fmt, Store>
   {
      using base = view<typename detail::is_std_pair<std::pair<A, B>>::tuple_type, Fmt, Store>;

    public:
      using element_type = std::pair<A, B>;
      using base::base;
      auto first() const noexcept { return this->template get<0>(); }
      auto second() const noexcept { return this->template get<1>(); }
   };

   // A map / set is the list of its entries / elements in canonical order:
   // the view of that list (size(), operator[], iteration), whose elements
   // are views of the entries (tuple<K, V>) or of the elements. The frame is
   // expected to have been validated, which checks the order.
   template <typename M, typename Fmt, storage Store>
      requires (detail::is_assoc_v<M> && detail::is_pssz_format_v<Fmt> &&
                !::psio::format_should_dispatch_adapter_v<Fmt, M>)
   class view<M, Fmt, Store> : public view<detail::assoc_list_t<M>, Fmt, Store>
   {
      using base = view<detail::assoc_list_t<M>, Fmt, Store>;

    public:
      using element_type = M;
      using base::base;
   };

   // utf8_string<N> is a String on the wire: the view borrows its bytes. As
   // for every view, the frame is expected to have been validated (which
   // checks UTF-8 and the bound for this type).
   template <std::size_t N, typename Fmt, storage Store>
      requires detail::is_pssz_format_v<Fmt>
   class view<utf8_string<N>, Fmt, Store>
   {
      std::span<const char> data_;
    public:
      using element_type = utf8_string<N>;
      using format_type = Fmt;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}
      view(std::span<const char> s, std::span<const char>) noexcept : data_(s) {}
      [[nodiscard]] std::span<const char> _psio_data() const noexcept { return data_; }
      [[nodiscard]] std::string_view view_() const noexcept
      {
         return std::string_view{data_.data(), data_.size()};
      }
   };

   // A plain enum is its underlying integer on the wire; the view reads it
   // by value. Any underlying value is returned as-is (no enumerator check).
   template <typename T, typename Fmt, storage Store>
      requires (std::is_enum_v<T> && detail::is_pssz_format_v<Fmt> &&
                !::psio::format_should_dispatch_adapter_v<Fmt, T>)
   class view<T, Fmt, Store>
   {
      std::span<const char> data_;
    public:
      using element_type = T;
      using format_type = Fmt;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}
      view(std::span<const char> s, std::span<const char>) noexcept : data_(s) {}
      [[nodiscard]] std::span<const char> _psio_data() const noexcept { return data_; }
      [[nodiscard]] T get() const noexcept
      {
         std::underlying_type_t<T> raw{};
         if (data_.size() >= sizeof(raw)) std::memcpy(&raw, data_.data(), sizeof(raw));
         return static_cast<T>(raw);
      }
   };

   // A duration / time point is its tick count on the wire (time.hpp); the
   // view reads it by value.
   template <::psio::TimeValue T, typename Fmt, storage Store>
      requires detail::is_pssz_format_v<Fmt>
   class view<T, Fmt, Store>
   {
      std::span<const char> data_;
    public:
      using element_type = T;
      using format_type = Fmt;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}
      view(std::span<const char> s, std::span<const char>) noexcept : data_(s) {}
      [[nodiscard]] std::span<const char> _psio_data() const noexcept { return data_; }
      [[nodiscard]] T get() const noexcept
      {
         ::psio::time_rep_t<T> raw{};
         if (data_.size() >= sizeof(raw)) std::memcpy(&raw, data_.data(), sizeof(raw));
         return ::psio::time_from_ticks<T>(raw);
      }
   };

   // uint256 is its 32 little-endian bytes on the wire; the view reads it by
   // value. (uint128 / int128 are arithmetic and use the primitive path.)
   template <typename Fmt, storage Store>
      requires detail::is_pssz_format_v<Fmt>
   class view<uint256, Fmt, Store>
   {
      std::span<const char> data_;
    public:
      using element_type = uint256;
      using format_type = Fmt;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}
      view(std::span<const char> s, std::span<const char>) noexcept : data_(s) {}
      [[nodiscard]] std::span<const char> _psio_data() const noexcept { return data_; }
      [[nodiscard]] uint256 get() const noexcept
      {
         uint256 out{};
         if (data_.size() >= sizeof(out.limb)) std::memcpy(out.limb, data_.data(), sizeof(out.limb));
         return out;
      }
   };

   template <typename T, typename Fmt, storage Store>
      requires detail::is_pssz_format_v<Fmt>
   class view<schema_types::Box<T>, Fmt, Store> : public view<T, Fmt, Store>
   {
    public:
      using view<T, Fmt, Store>::view;
   };

   // Construction checks the complete frame; getters retain their O(1)
   // prevalidated-input contract and borrow the original payload bytes.
   template <typename T, typename Fmt>
      requires detail::is_pssz_format_v<Fmt>
   [[nodiscard]] codec_status validated_pssz_view(
      Fmt fmt, std::span<const char> bytes,
      std::optional<view<T, Fmt, storage::const_borrow>>& result, bool& has_unknown)
   {
      auto status = validate_pssz_compatible<T>(fmt, bytes, has_unknown);
      if (status.ok()) result.emplace(bytes);
      return status;
   }
}
