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

namespace psio {
   // The automatic profile at a position whose annotations (carrier A,
   // detail/annotation_tree.hpp) narrow an offset width below it: the format
   // of the view of such a member and of what it holds. Views name it; it is
   // not a codec tag. A position whose annotations change no width is viewed
   // as plain `psio::pssz`.
   template <typename A>
   struct pssz_at
   {
   };
}  // namespace psio

namespace psio::view_layout {

   namespace detail_pssz {

      // The view format of a value of type T at a position carrying A.
      template <typename T, typename A>
      using format_at = std::conditional_t<
         ::psio::detail::pssz_impl::widths_unchanged<T, A>(), ::psio::pssz, ::psio::pssz_at<A>>;

      // A map / set is viewed as the list of its entries. This is that
      // list's carrier under the map's carrier A: the same length bound, and
      // as element(...) what keys(...) / values(...) (a map) or element(...)
      // (a set) state for one entry.
      template <typename M, typename A>
      struct assoc_list_raw
      {
         static constexpr auto value = [] {
            namespace ann = ::psio::detail::ann;
            const auto entry = ann::entry_specs<M>(A::value);
            const auto bound = [&] {
               if constexpr (ann::has_length_bound_v<A>)
                  return std::tuple{std::get<::psio::length_bound>(A::value)};
               else
                  return std::tuple<>{};
            }();
            if constexpr (std::tuple_size_v<std::remove_cvref_t<decltype(entry)>> == 0)
               return bound;
            else
               return std::tuple_cat(
                  bound, std::tuple{::psio::element_spec<std::remove_cvref_t<decltype(entry)>>{
                            entry}});
         }();
         using value_t = std::remove_cvref_t<decltype(value)>;
         static constexpr std::size_t cap = A::cap;
      };
      template <typename M, typename Fmt>
      struct assoc_list_format
      {
         using type = Fmt;
      };
      template <typename M, typename A>
      struct assoc_list_format<M, ::psio::pssz_at<A>>
      {
         using type = ::psio::pssz_at<
            typename ::psio::detail::ann::normalized<assoc_list_raw<M, A>>::type>;
      };
      // The format of the list view a map / set view of format Fmt is.
      template <typename M, typename Fmt>
      using assoc_list_format_t = typename assoc_list_format<M, Fmt>::type;

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
      // The value of a view (view.hpp "View to value"): trusted decode.
      template <typename T>
      static T unpack(std::span<const char> s)
      {
         return ::psio::detail::pssz_impl::decode_value<W, false, T>(s, 0, s.size());
      }

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
      // The value of a view (view.hpp "View to value"): trusted decode.
      template <typename T>
      static T unpack(std::span<const char> s)
      {
         return ::psio::detail::pssz_impl::decode_value<::psio::auto_pssz_width_v<T>, true, T>(
            s, 0, s.size());
      }

      template <typename T, std::size_t N>
         requires (::psio::detail::pssz_impl::Record<T> &&
                   N < ::psio::detail::pssz_impl::record_traits<T>::member_count)
      static std::span<const char> record_field_span(
         std::span<const char> s, std::span<const char> = {}) noexcept
      {
         return detail_pssz::record_field_span<::psio::auto_pssz_width_v<T>, T, N>(s);
      }

      // A child view re-selects its width from the member's type alone
      // unless the member's annotations narrow an offset width in it (a
      // bounded list of variable elements, an array of bounded variable
      // elements, a tuple / pair / map entry with bounded elements): the
      // view of such a member carries the annotations (psio::pssz_at).
      template <typename T, std::size_t N>
      using member_format = detail_pssz::format_at<
         std::remove_cvref_t<
            typename ::psio::detail::pssz_impl::record_traits<T>::template member_type<N>>,
         ::psio::detail::pssz_impl::field_ann<T, ::psio::detail::pssz_impl::no_ann, N>>;

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

   // ── pssz_at<A> — the automatic profile under annotations A ────────────
   //
   // Every width is the one the codec selects for the viewed type at a
   // position carrying A (pssz_impl::width_for<T, A>), and the carrier is
   // threaded down exactly as the codec threads it: a list / array element
   // gets element(...), a tuple element at<I>(...), an optional's payload the
   // optional's own. Offsets have one compile-time width per view type, so
   // element access stays O(1).
   template <typename A>
   struct traits<::psio::pssz_at<A>>
   {
      template <typename T>
      static constexpr std::size_t width = ::psio::detail::pssz_impl::width_for<T, A>();

      // The value of a view (view.hpp "View to value"): trusted decode with
      // the widths this position's annotations select — the bytes of a
      // member under a cap are not a standalone encoding of its type.
      template <typename T>
      static T unpack(std::span<const char> s)
      {
         return ::psio::detail::pssz_impl::decode_value<width<T>, true, T, A>(s, 0, s.size());
      }

      template <typename T, std::size_t N>
         requires (::psio::detail::pssz_impl::Record<T> &&
                   N < ::psio::detail::pssz_impl::record_traits<T>::member_count)
      static std::span<const char> record_field_span(
         std::span<const char> s, std::span<const char> = {}) noexcept
      {
         return detail_pssz::record_field_span<width<T>, T, N>(s);
      }

      template <typename T, std::size_t N>
      using member_format = detail_pssz::format_at<
         std::remove_cvref_t<
            typename ::psio::detail::pssz_impl::record_traits<T>::template member_type<N>>,
         ::psio::detail::pssz_impl::field_ann<T, A, N>>;

      template <typename Parent, typename = void>
      struct nested_format_of
      {
         // A reflected record's members are reached through member_format.
         using type = ::psio::pssz;
      };
      template <typename E, typename Al>
      struct nested_format_of<std::vector<E, Al>>
      {
         using type = detail_pssz::format_at<E, ::psio::detail::ann::element_ann<A>>;
      };
      template <typename E, std::size_t N>
      struct nested_format_of<std::array<E, N>>
      {
         using type = detail_pssz::format_at<E, ::psio::detail::ann::element_ann<A>>;
      };
      template <typename V>
      struct nested_format_of<std::optional<V>>
      {
         using type = detail_pssz::format_at<V, A>;
      };
      template <typename... Ts>
      struct nested_format_of<std::variant<Ts...>>
      {
         // One view format serves every alternative of a variant view.
         static_assert(
            []<std::size_t... Is>(std::index_sequence<Is...>) {
               return (::psio::detail::pssz_impl::widths_unchanged<
                          Ts, ::psio::detail::ann::at_ann<A, Is>>() &&
                       ...);
            }(std::index_sequence_for<Ts...>{}),
            "psio: a pssz view cannot address this variant: an at<I>(...) annotation narrows "
            "the offset width of a value nested in one of its alternatives. State the bound "
            "in the alternative's type (psio::bounded, psio::utf8_string) to view it");
         using type = ::psio::pssz;
      };
      template <typename Parent>
      using nested_format = typename nested_format_of<Parent>::type;

      template <typename T>
      static std::size_t vector_count(std::span<const char> s) noexcept
      {
         return detail_pssz::vector_count<width<std::vector<T>>, T>(s);
      }
      template <typename T>
      static std::span<const char>
      vector_element_span(std::span<const char> s, std::size_t i) noexcept
      {
         return detail_pssz::vector_element_span<width<std::vector<T>>, T>(s, i);
      }
      template <typename T, std::size_t N>
      static std::span<const char> array_element_span(std::span<const char> s, std::size_t i) noexcept
      {
         return detail_pssz::vector_element_span<width<std::array<T, N>>, T>(s, i);
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
   namespace detail::pssz_impl {
      // A view given to the encoder as a value (pssz.hpp "Sources"): the
      // bytes of a view of a position carrying VA are what is written at a
      // position carrying A when the two are the same position kind, or
      // when neither changes a width in T.
      template <typename VA, std::size_t W, typename T, typename A>
      inline constexpr bool view_verbatim_v<::psio::pssz_at<VA>, W, true, T, A> =
         std::is_same_v<VA, A> || (widths_unchanged<T, VA>() && widths_unchanged<T, A>());
   }  // namespace detail::pssz_impl
   namespace detail {
      template <typename F> inline constexpr bool is_pssz_format_v = false;
      template <> inline constexpr bool is_pssz_format_v<pssz> = true;
      template <std::size_t W> inline constexpr bool is_pssz_format_v<pssz_<W>> = true;
      template <typename A> inline constexpr bool is_pssz_format_v<pssz_at<A>> = true;
   }

   namespace detail {
      // The view of a positional record: std::tuple<Ts...> or
      // psio::annotated_tuple<Meta, Ts...> (a method's argument tuple, whose
      // cap and parameter annotations select its widths).
      template <typename Tuple, typename Fmt, storage Store>
      class tuple_view
      {
         std::span<const char> data_;

       public:
         using element_type = Tuple;
         using format_type = Fmt;
         static constexpr storage storage_kind = Store;
         static_assert(sizeof(detail::read_view_store<Store>) > 0);
         tuple_view() = default;
         explicit tuple_view(std::span<const char> s) noexcept : data_(s) {}
         tuple_view(std::span<const char> s, std::span<const char>) noexcept : data_(s) {}
         std::span<const char> _psio_data() const noexcept { return data_; }
         // Element N: its value for an arithmetic element, its view
         // otherwise. Structured bindings work: `auto [a, b] = tuple_view;`.
         template <std::size_t N> auto get() const noexcept
         {
            using F = std::remove_cvref_t<std::tuple_element_t<N, Tuple>>;
            auto span = view_layout::traits<Fmt>::template record_field_span<Tuple, N>(data_);
            if constexpr (std::is_arithmetic_v<F>)
            {
               F out{};
               if (span.size() == sizeof(F)) std::memcpy(&out, span.data(), sizeof(F));
               return out;
            }
            else return view<F, view_layout::member_format_t<Fmt, Tuple, N>, Store>{span};
         }
         PSIO_VIEW_UNPACK_
      };
   }  // namespace detail

   template <typename... Ts, typename Fmt, storage Store>
      requires (detail::is_pssz_format_v<Fmt> &&
                !::psio::format_should_dispatch_adapter_v<Fmt, std::tuple<Ts...>>)
   class view<std::tuple<Ts...>, Fmt, Store>
       : public detail::tuple_view<std::tuple<Ts...>, Fmt, Store>
   {
    public:
      using detail::tuple_view<std::tuple<Ts...>, Fmt, Store>::tuple_view;
   };

   // The view of an annotated tuple: `psio::pssz_view<
   // psio::reflect<T>::method_arg_tuple<I>>` reads a method's packed
   // arguments with the widths its cap and parameter annotations select.
   // (Viewing those bytes as the plain std::tuple reads wrong offsets once
   // the method has a cap.)
   template <typename Meta, typename... Ts, typename Fmt, storage Store>
      requires detail::is_pssz_format_v<Fmt>
   class view<annotated_tuple<Meta, Ts...>, Fmt, Store>
       : public detail::tuple_view<annotated_tuple<Meta, Ts...>, Fmt, Store>
   {
    public:
      using detail::tuple_view<annotated_tuple<Meta, Ts...>, Fmt, Store>::tuple_view;
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
      using format_type  = Fmt;
      using base::base;
      auto first() const noexcept { return this->template get<0>(); }
      auto second() const noexcept { return this->template get<1>(); }
      [[nodiscard]] std::span<const char> _psio_data() const noexcept { return base::_psio_data(); }
      PSIO_VIEW_UNPACK_
   };

   // A map / set is the list of its entries / elements in canonical order:
   // the view of that list (size(), operator[], iteration), whose elements
   // are views of the entries (tuple<K, V>) or of the elements. The frame is
   // expected to have been validated, which checks the order.
   template <typename M, typename Fmt, storage Store>
      requires (detail::is_assoc_v<M> && detail::is_pssz_format_v<Fmt> &&
                !::psio::format_should_dispatch_adapter_v<Fmt, M>)
   class view<M, Fmt, Store>
       : public view<detail::assoc_list_t<M>, view_layout::detail_pssz::assoc_list_format_t<M, Fmt>,
                     Store>
   {
      using base = view<detail::assoc_list_t<M>,
                        view_layout::detail_pssz::assoc_list_format_t<M, Fmt>, Store>;

      using key_type = detail::assoc_key_t<M>;

      // The key of entry i, as a view (or a value for an arithmetic key).
      [[nodiscard]] auto key_at(std::size_t i) const noexcept
      {
         if constexpr (detail::is_map_v<M>)
            return (*this)[i].template get<0>();
         else
            return (*this)[i];
      }

    public:
      using element_type = M;
      using format_type  = Fmt;
      using base::base;
      [[nodiscard]] std::span<const char> _psio_data() const noexcept { return base::_psio_data(); }

      // ── Lookup ─────────────────────────────────────────────────────────
      //
      // The entries are in ascending order of the `key` encoding of their
      // keys (docs/key-spec.md), so lookup is a binary search over that
      // encoding: O(log n) probes, each of which unpacks one key.
      //
      //   lower_bound(key)   index of the first entry whose key is not
      //                      before `key`; size() if there is none
      //   find(key)          index of the entry with that key, or size()
      //   contains(key)
      //   at(key)            a map: std::optional of the mapped value's
      //                      view (the value itself when it is arithmetic)
      //   value_or(key, x)   a map: the mapped value, or x
      //
      // `key` is anything key_type is constructible from.
      template <typename K>
      [[nodiscard]] std::size_t lower_bound(const K& key) const
      {
         std::vector<char> want, have;
         ::psio::detail::append_key_bytes(key_type(key), want);
         std::size_t lo = 0, hi = this->size();
         while (lo < hi)
         {
            const std::size_t mid = lo + (hi - lo) / 2;
            have.clear();
            ::psio::detail::append_key_bytes(key_type(::psio::value_of(key_at(mid))), have);
            if (::psio::detail::compare_key_bytes(have.data(), have.size(), want.data(),
                                                  want.size()) < 0)
               lo = mid + 1;
            else
               hi = mid;
         }
         return lo;
      }
      template <typename K>
      [[nodiscard]] std::size_t find(const K& key) const
      {
         const std::size_t i = lower_bound(key);
         if (i < this->size() && key_type(::psio::value_of(key_at(i))) == key_type(key))
            return i;
         return this->size();
      }
      template <typename K>
      [[nodiscard]] bool contains(const K& key) const
      {
         return find(key) != this->size();
      }
      template <typename K>
         requires detail::is_map_v<M>
      [[nodiscard]] auto at(const K& key) const
      {
         using R = std::optional<decltype((*this)[0].template get<1>())>;
         const std::size_t i = find(key);
         return i == this->size() ? R{} : R{(*this)[i].template get<1>()};
      }
      template <typename K, typename U>
         requires detail::is_map_v<M>
      [[nodiscard]] auto value_or(const K& key, U&& fallback) const
      {
         using Mapped        = typename M::mapped_type;
         const std::size_t i = find(key);
         if (i == this->size())
            return static_cast<Mapped>(std::forward<U>(fallback));
         return Mapped(::psio::value_of((*this)[i].template get<1>()));
      }

      PSIO_VIEW_UNPACK_
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
      operator std::string_view() const noexcept { return view_(); }
      [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
      [[nodiscard]] bool        empty() const noexcept { return data_.empty(); }
      PSIO_VIEW_UNPACK_
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
      [[nodiscard]] T unpack() const noexcept { return get(); }
      operator T() const noexcept { return get(); }
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
      [[nodiscard]] T unpack() const noexcept { return get(); }
      operator T() const noexcept { return get(); }
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
      [[nodiscard]] uint256 unpack() const noexcept { return get(); }
      operator uint256() const noexcept { return get(); }
   };

   template <typename T, typename Fmt, storage Store>
      requires detail::is_pssz_format_v<Fmt>
   class view<schema_types::Box<T>, Fmt, Store> : public view<T, Fmt, Store>
   {
    public:
      using view<T, Fmt, Store>::view;
   };

   // A transparent type (transparent.hpp) is its member: so is its view.
   template <typename T, typename Fmt, storage Store>
      requires (detail::is_pssz_format_v<Fmt> && ::psio::is_transparent_v<T> &&
                !::psio::format_should_dispatch_adapter_v<Fmt, T>)
   class view<T, Fmt, Store> : public view<::psio::transparent_inner_t<T>, Fmt, Store>
   {
      using base = view<::psio::transparent_inner_t<T>, Fmt, Store>;

    public:
      // Navigation is the member's; the value is T.
      using element_type = T;
      using format_type  = Fmt;
      using base::base;
      view() = default;
      view(const base& inner) noexcept : base(inner) {}
      [[nodiscard]] std::span<const char> _psio_data() const noexcept { return base::_psio_data(); }
      PSIO_VIEW_UNPACK_
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

   // ── Calling a reflected method with packed arguments ──────────────────
   //
   // The arguments of method I of T travel as ONE value: the encoding of
   // `reflect<T>::method_arg_tuple<I>`, a tuple of the parameters' wire
   // types (a parameter declared `psio::pssz_view<const X>` is a packed X)
   // carrying the method's parameter annotations and cap.
   //
   //   validate_method_args<T, I>(bytes)        codec_status: are the bytes that tuple
   //   method_args_view<T, I>                   the read view of that tuple: get<N>(),
   //                                            structured bindings
   //   method_params_from_pssz<T, I>(trusted)   every argument as the method DECLARES it,
   //                                            in a reflect<T>::method_params<I>: a
   //                                            view parameter is a view into the
   //                                            bytes, a std::string_view / std::span<
   //                                            const char> parameter borrows them, a
   //                                            value parameter is unpacked
   //   invoke_method_pssz<T, I>(obj, bytes)     validate, then call; returns what the
   //                                            method returns
   //   invoke_method_pssz<T, I>(obj, trusted)   call without validating
   //   call_method_pssz<T, I>(obj, trusted,     call and append the encoded result
   //                          result_bytes)     (psio::encode_method_result, which
   //                                            honours returns(...)); a codec_status
   //
   // The split is explicit: the span overload checks the bytes and fails
   // through psio::codec_abort (codec_exception where exceptions are
   // enabled) before the method runs; `psio::trusted{bytes}` is the
   // caller's statement that validate_method_args has accepted them.
   // Views and borrowed strings handed to the method point into `bytes`,
   // which must outlive the call.
   //
   // A view parameter needs the argument's bytes to be a standalone
   // encoding of its type. That holds for every reflected record, and for
   // any type when the method has neither a cap nor annotations on that
   // parameter. A parameter whose position narrows offset widths (a
   // std::vector<std::string> under the method's maxDynamicData, say) can be
   // taken by value but not as psio::pssz_view: a compile error says so.
   template <typename T, std::size_t I>
   using method_args_view =
      view<typename reflect<T>::template method_arg_tuple<I>, pssz, storage::const_borrow>;

   template <typename T, std::size_t I>
   [[nodiscard]] codec_status validate_method_args(std::span<const char> bytes) noexcept
   {
      return ::psio::validate<typename reflect<T>::template method_arg_tuple<I>>(pssz{}, bytes);
   }

   namespace detail {
      template <typename P>
      inline constexpr bool is_byte_span_v = false;
      template <typename B, std::size_t N>
         requires (sizeof(B) == 1 && std::is_const_v<B> &&
                   (std::is_integral_v<B> || std::is_same_v<std::remove_const_t<B>, std::byte>))
      inline constexpr bool is_byte_span_v<std::span<B, N>> = true;

      // Argument N of the packed argument tuple `Args`, as parameter type P.
      template <typename Args, std::size_t N, typename P>
      [[nodiscard]] P method_param_from(std::span<const char> bytes)
      {
         using layout = view_layout::traits<pssz>;
         using Format = view_layout::member_format_t<pssz, Args, N>;
         using Wire   = std::tuple_element_t<N, typename Args::tuple_type>;
         const auto part = layout::template record_field_span<Args, N>(bytes);
         if constexpr (is_view_v<P>)
         {
            static_assert(std::is_same_v<std::remove_const_t<typename P::element_type>, Wire>);
            static_assert(std::is_same_v<typename P::format_type, pssz>,
                          "psio: a view-typed method parameter is a psio::pssz_view "
                          "(psio::view<X, psio::pssz>)");
            static_assert(std::is_same_v<Format, pssz>,
                          "psio: this parameter cannot be taken as a psio::pssz_view: the "
                          "method's cap or the parameter's annotations narrow the offset widths "
                          "of its encoding, so its bytes are not a standalone encoding of its "
                          "type. Take it by value, or state the bound in the type "
                          "(psio::bounded, a record with its own maxDynamicData)");
            return P{part};
         }
         else if constexpr (std::is_same_v<P, std::string_view>)
            return std::string_view{part.data(), part.size()};
         else if constexpr (is_byte_span_v<P>)
            return P{reinterpret_cast<typename P::pointer>(part.data()), part.size()};
         else
         {
            static_assert(std::is_same_v<P, Wire>,
                          "psio: a method parameter that only refers to its argument must be a "
                          "psio::pssz_view<X>, a std::string_view or a std::span of const bytes");
            return view_layout::traits<Format>::template unpack<Wire>(part);
         }
      }
   }  // namespace detail

   // Every argument as the method declares it. Trusted: see above.
   template <typename T, std::size_t I>
   [[nodiscard]] typename reflect<T>::template method_params<I> method_params_from_pssz(
      trusted bytes)
   {
      using Args   = typename reflect<T>::template method_arg_tuple<I>;
      using Params = typename reflect<T>::template method_params<I>;
      return [&]<std::size_t... Ns>(std::index_sequence<Ns...>) {
         return Params{
            detail::method_param_from<Args, Ns, std::tuple_element_t<Ns, Params>>(bytes.bytes)...};
      }(std::make_index_sequence<std::tuple_size_v<Params>>{});
   }

   template <typename T, std::size_t I, typename Obj>
   decltype(auto) invoke_method_pssz(Obj&& obj, trusted bytes)
   {
      return reflect<T>::template invoke_method<I>(std::forward<Obj>(obj),
                                                   method_params_from_pssz<T, I>(bytes));
   }
   template <typename T, std::size_t I, typename Obj>
   decltype(auto) invoke_method_pssz(Obj&& obj, std::span<const char> bytes)
   {
      if (auto status = validate_method_args<T, I>(bytes); !status.ok())
         ::psio::codec_abort(status.error());
      return invoke_method_pssz<T, I>(std::forward<Obj>(obj), trusted{bytes});
   }

   // Calls method I and appends its encoded result to `result` (nothing
   // for a void method). The status is that of encoding the result: a
   // value outside returns(...), over a cap, with no encoding.
   template <typename T, std::size_t I, typename Obj>
   [[nodiscard]] codec_status call_method_pssz(Obj&& obj, trusted bytes, std::vector<char>& result)
   {
      if constexpr (std::is_void_v<typename reflect<T>::template method_result<I>>)
      {
         invoke_method_pssz<T, I>(std::forward<Obj>(obj), bytes);
         return codec_ok();
      }
      else
         return ::psio::encode_method_result<T, I>(
            invoke_method_pssz<T, I>(std::forward<Obj>(obj), bytes), result);
   }

   // ── from_pssz / decode from a view ────────────────────────────────────
   //
   // `psio::from_pssz<T>(view)` is `view.unpack()`, for code that spells a
   // decode the same way whatever it holds. Trusted: a view is over
   // validated bytes.
   template <typename T, detail::read_view V>
      requires (std::same_as<T, typename V::element_type> &&
                detail::is_pssz_format_v<typename V::format_type>)
   [[nodiscard]] T from_pssz(const V& v)
   {
      return v.unpack();
   }
}

// Structured bindings over a tuple / pair / annotated-tuple view:
//    auto [socket, data, flags] = args_view;
// Each binding is what get<N>() returns (a value for an arithmetic element,
// a sub-view otherwise).
template <typename... Ts, typename Fmt, psio::storage Store>
struct std::tuple_size<psio::view<std::tuple<Ts...>, Fmt, Store>>
   : std::integral_constant<std::size_t, sizeof...(Ts)>
{
};
template <std::size_t I, typename... Ts, typename Fmt, psio::storage Store>
struct std::tuple_element<I, psio::view<std::tuple<Ts...>, Fmt, Store>>
{
   using type = decltype(std::declval<const psio::view<std::tuple<Ts...>, Fmt, Store>&>().template get<I>());
};
template <typename Meta, typename... Ts, typename Fmt, psio::storage Store>
struct std::tuple_size<psio::view<psio::annotated_tuple<Meta, Ts...>, Fmt, Store>>
   : std::integral_constant<std::size_t, sizeof...(Ts)>
{
};
template <std::size_t I, typename Meta, typename... Ts, typename Fmt, psio::storage Store>
struct std::tuple_element<I, psio::view<psio::annotated_tuple<Meta, Ts...>, Fmt, Store>>
{
   using type = decltype(
      std::declval<const psio::view<psio::annotated_tuple<Meta, Ts...>, Fmt, Store>&>().template get<I>());
};
template <typename A, typename B, typename Fmt, psio::storage Store>
struct std::tuple_size<psio::view<std::pair<A, B>, Fmt, Store>>
   : std::integral_constant<std::size_t, 2>
{
};
template <std::size_t I, typename A, typename B, typename Fmt, psio::storage Store>
struct std::tuple_element<I, psio::view<std::pair<A, B>, Fmt, Store>>
{
   using type =
      decltype(std::declval<const psio::view<std::pair<A, B>, Fmt, Store>&>().template get<I>());
};

// The views of psio::nested<T> and of pssz_buffer<T> / shared_pssz<T> members.
#include <psio/nested.hpp>
