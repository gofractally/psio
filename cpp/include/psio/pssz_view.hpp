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
//
// Each function has two forms (view.hpp "Safe and trusted views"). The
// PLACING form (trailing `view_layout::unchecked_t`, or the per-record
// state `trusted_record_state`) is what a view of either kind calls
// (`psio::pssz_view<T>`, `psio::pssz_trusted_view<T>`): it compares each
// offset with the bytes where it reads it (PSIO_VIEW_TRUST) and fails —
// psio::codec_abort — on a member it cannot place. The other form returns
// an empty span instead; the mutable view and the code that walks bytes
// without a view use it.

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
   //
   // It also carries what `psio::buffer<T, pssz_at<A>>` needs to hold such a
   // value packed (encode, validate, decode with the widths, bounds and
   // order specs of A), so the buffer of a view's format can be unpacked.
   // For a method's result use `psio::method_result_buffer<T, I>` /
   // `psio::shared_method_result<T, I>` (below), which also know what
   // returns(...) states when it narrows nothing.
   template <typename A>
   struct pssz_at
   {
      template <typename T>
      [[nodiscard]] static codec_status encode_checked(const T& value, std::vector<char>& out)
      {
         return detail::pssz_impl::encode_at<T, A>(value, out, A::value,
                                                   "pssz: member length_bound violated");
      }
      template <typename T>
      [[nodiscard]] static std::vector<char> encode(const T& value)
      {
         // The fast write, as every format's `encode`.
         std::vector<char> out;
         if (auto status = detail::pssz_impl::encode_at<T, A>(
                value, out, A::value, "pssz: member length_bound violated",
                detail::pssz_impl::encode_preflight::fast());
             !status.ok())
            ::psio::codec_abort(status.error());
         return out;
      }
      template <typename T>
      [[nodiscard]] static codec_status validate(std::span<const char> bytes) noexcept
      {
         return detail::pssz_impl::validate_at<T, A>(bytes, A::value,
                                                     "pssz: member length_bound violated");
      }
      template <typename T>
      [[nodiscard]] static T decode(std::span<const char> bytes)
      {
         return detail::pssz_impl::decode_at<T, A>(bytes);
      }
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
      constexpr std::size_t vector_count(std::span<const char> s)
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
            // The offset table lies inside the list: a first offset nobody
            // validated must not announce more elements than that (every
            // index below the count then has its offsets in the bytes).
            if (static_cast<std::size_t>(first) > s.size())
               return 0;
            return static_cast<std::size_t>(first) / W;
         }
      }

      template <std::size_t W, typename T>
      std::span<const char>
      vector_element_span(std::span<const char> s, std::size_t i)
      {
         if constexpr (::psio::detail::pssz_impl::is_fixed_v<T>)
         {
            constexpr std::size_t esz =
               ::psio::detail::pssz_impl::fixed_size_of<T>();
            // An element the bytes do not hold (an index past the end, an
            // array whose span is short) is an empty view, never a span
            // outside the bytes.
            if constexpr (esz != 0)
               if (i >= s.size() / esz)
                  return {};
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
            // Offsets nobody validated must not select bytes outside the list.
            if (static_cast<std::uint32_t>(off_i) > off_next || off_next > s.size())
               return {};
            return s.subspan(static_cast<std::uint32_t>(off_i),
                              off_next - static_cast<std::uint32_t>(off_i));
         }
      }

      // The unchecked forms (trusted view).
      template <std::size_t W, typename T>
      [[gnu::always_inline]] inline std::size_t vector_count(std::span<const char> s,
                                                             unchecked_t)
      {
         if constexpr (::psio::detail::pssz_impl::is_fixed_v<T>)
         {
            constexpr std::size_t esz = ::psio::detail::pssz_impl::fixed_size_of<T>();
            if constexpr (esz == 0)
               return 0;
            else
            {
               PSIO_VIEW_TRUST(s.size() % esz == 0,
                               "the span of a list is not a multiple of its element size");
               return s.size() / esz;
            }
         }
         else
         {
            // An empty list is zero bytes (and so is a view of nothing).
            if (s.empty())
               return 0;
            PSIO_VIEW_TRUST(s.size() >= W, "a list is shorter than its first offset");
            const std::uint32_t first = ::psio::detail::pssz_impl::read_offset<W>(s, 0);
            PSIO_VIEW_TRUST(first <= s.size(), "the offset table of a list is cut short");
            return first / W;
         }
      }

      template <std::size_t W, typename T>
      [[gnu::always_inline]] inline std::span<const char> vector_element_span(
         std::span<const char> s, std::size_t i, unchecked_t)
      {
         if constexpr (::psio::detail::pssz_impl::is_fixed_v<T>)
         {
            constexpr std::size_t esz = ::psio::detail::pssz_impl::fixed_size_of<T>();
            if constexpr (esz == 0)
               return {s.data(), 0};
            else
            {
               // (Not `i * esz + esz <= size`: that wraps for a large index.)
               PSIO_VIEW_TRUST(i < s.size() / esz, "a list element lies past the list");
               return {s.data() + i * esz, esz};
            }
         }
         else
         {
            using ::psio::detail::pssz_impl::read_offset;
            PSIO_VIEW_TRUST(s.size() >= W, "a list is shorter than its first offset");
            // The offset table ends where the first element begins.
            const std::size_t table = read_offset<W>(s, 0);
            PSIO_VIEW_TRUST(table <= s.size() && i < table / W,
                            "a list element lies past the offset table");
            const std::size_t start = read_offset<W>(s, i * W);
            const std::size_t stop =
               (i + 1) * W < table ? std::size_t{read_offset<W>(s, (i + 1) * W)} : s.size();
            PSIO_VIEW_TRUST(start <= stop && stop <= s.size(),
                            "the offsets of a list element are out of order or out of range");
            return {s.data() + start, stop - start};
         }
      }

      // ── optional ───────────────────────────────────────────────────────
      //
      // pssz splits on the payload's minimum encoded size, using the same
      // predicate as the codec (pssz_impl::optional_uses_selector_v):
      //   min(V) > 0  — no selector. Empty span = None; otherwise Some.
      //   min(V) == 0 — [u8 selector][payload if selector != 0].
      template <typename T>
      bool optional_has_value(std::span<const char> s)
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
      optional_payload_span(std::span<const char> s)
      {
         using V = typename T::value_type;
         if constexpr (!::psio::detail::pssz_impl::optional_uses_selector_v<V>)
            return s;
         else
            return s.empty() ? std::span<const char>{} : s.subspan(1);
      }

      // The unchecked forms (trusted view). A view of nothing (default-
      // constructed, `at(i)` past the end) has a null span.
      template <typename T>
      [[gnu::always_inline]] inline bool optional_has_value(std::span<const char> s,
                                                            unchecked_t)
      {
         using V = typename T::value_type;
         if constexpr (!::psio::detail::pssz_impl::optional_uses_selector_v<V>)
            return !s.empty();
         else
         {
            if (s.data() == nullptr)
               return false;
            PSIO_VIEW_TRUST(!s.empty(), "an optional has no selector byte");
            return static_cast<unsigned char>(s[0]) != 0;
         }
      }

      template <typename T>
      [[gnu::always_inline]] inline std::span<const char> optional_payload_span(
         std::span<const char> s, unchecked_t)
      {
         using V = typename T::value_type;
         if constexpr (!::psio::detail::pssz_impl::optional_uses_selector_v<V>)
            // (No bytes is None: its payload is a view of nothing.)
            return s.empty() ? std::span<const char>{} : s;
         else
         {
            if (s.data() == nullptr)
               return {};
            PSIO_VIEW_TRUST(!s.empty(), "an optional has no selector byte");
            return {s.data() + 1, s.size() - 1};
         }
      }

      // ── record ─────────────────────────────────────────────────────────
      //
      // Field N of record T at offset width W. Reads the writer's header so
      // a known trailing optional the writer pruned yields the canonical
      // None payload, and unknown appended slots bound the last known
      // variable field. Getters keep their O(1) prevalidated-input contract.
      //
      // The part every member of one record shares — reading the header and
      // checking that the fixed region it announces is there — is
      // `record_state`, computed once when a record view is constructed
      // (view.hpp "Per-record state"); `record_field_span_in` is the rest.
      inline constexpr std::size_t no_record = ~std::size_t{0};

      template <std::size_t W, typename T>
      struct record_state
      {
         // Size of the fixed region the writer wrote (the header's value;
         // the known size for a DWNC record). no_record: the bytes cannot
         // hold the record — every member then reads as absent, exactly as
         // when the header was examined on each access.
         std::size_t fixed = no_record;

         constexpr record_state() = default;
         explicit record_state(std::span<const char> bytes)
         {
            using namespace ::psio::detail::pssz_impl;
            constexpr auto fields = record_layout<W, T>(
               std::make_index_sequence<record_traits<T>::member_count>{});
            constexpr auto known_size = [&] {
               std::size_t n = 0;
               for (auto field : fields) n += field.fixed_size;
               return n;
            }();
            std::size_t fixed_size = known_size;
            std::size_t body       = bytes.size();
            if constexpr (!::psio::is_dwnc_v<T>)
            {
               if (bytes.size() < W) return;
               fixed_size = read_offset<W>(bytes, 0);
               body -= W;
            }
            if (fixed_size > body) return;
            fixed = fixed_size;
         }
      };

      template <std::size_t W, typename T, std::size_t N>
      std::span<const char> record_field_span_in(std::span<const char> bytes,
                                                 std::size_t           fixed_size);

      // Member N given the record's state (the record's bytes include its
      // header).
      template <std::size_t W, typename T, std::size_t N>
      [[gnu::always_inline]] inline std::span<const char> record_field_span(
         std::span<const char> bytes, const record_state<W, T>& state)
      {
         if (state.fixed == no_record) return {};
         if constexpr (!::psio::is_dwnc_v<T>)
            bytes = std::span<const char>{bytes.data() + W, bytes.size() - W};
         return record_field_span_in<W, T, N>(bytes, state.fixed);
      }

      template <std::size_t W, typename T, std::size_t N>
      std::span<const char> record_field_span(std::span<const char> bytes)
      {
         return record_field_span<W, T, N>(bytes, record_state<W, T>{bytes});
      }

      // `bytes`: the record after its header; `fixed_size` <= bytes.size().
      template <std::size_t W, typename T, std::size_t N>
      [[gnu::always_inline]] inline std::span<const char> record_field_span_in(
         std::span<const char> bytes, const std::size_t fixed_size)
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

      // The unchecked forms (trusted view).
      //
      // The state is the size of the fixed region the writer wrote, read
      // from the header without comparing it with the record's size; 0 for
      // a view of nothing (one null test, when the view is constructed), so
      // that every member of such a view reads as absent through the
      // compare below.
      template <std::size_t W, typename T>
      struct trusted_record_state
      {
         std::size_t fixed = 0;

         constexpr trusted_record_state() = default;
         [[gnu::always_inline]] explicit trusted_record_state(std::span<const char> bytes)
         {
            using namespace ::psio::detail::pssz_impl;
            if (bytes.data() == nullptr)
               return;
            if constexpr (::psio::is_dwnc_v<T>)
            {
               constexpr auto fields = record_layout<W, T>(
                  std::make_index_sequence<record_traits<T>::member_count>{});
               constexpr auto known_size = [&] {
                  std::size_t n = 0;
                  for (auto field : fields) n += field.fixed_size;
                  return n;
               }();
               PSIO_VIEW_TRUST(bytes.size() >= known_size, "a record is cut short");
               fixed = known_size;
            }
            else
            {
               constexpr auto fields = record_layout<W, T>(
                  std::make_index_sequence<record_traits<T>::member_count>{});
               // The fixed region of the members every writer writes.
               constexpr auto required_size = [&] {
                  std::size_t n = 0;
                  for (std::size_t i = 0; i < required_field_count<T>(); ++i)
                     n += fields[i].fixed_size;
                  return n;
               }();
               PSIO_VIEW_TRUST(bytes.size() >= W, "a record has no header");
               fixed = read_offset<W>(bytes, 0);
               PSIO_VIEW_TRUST(fixed <= bytes.size() - W,
                               "a record's header announces more than the record holds");
               // Checked here, once: a required member is then read with
               // no compare at all.
               PSIO_VIEW_TRUST(fixed >= required_size,
                               "a record's header omits a required member");
            }
         }
      };

      // Member N of a record on valid bytes. What stays is the wire's
      // meaning, where valid bytes can differ: a trailing optional member
      // (or the offset slot after a member) that the writer's header does
      // not cover was not written, and offset slots this reader does not
      // know end its last variable member. Validation refuses a header that
      // omits a member every writer writes (all up to the last one that is
      // not a plain std::optional — required_field_count), so such a member
      // is read without consulting the header at all: one null test tells a
      // view of nothing.
      template <std::size_t W, typename T, std::size_t N>
      [[gnu::always_inline]] inline std::span<const char> record_field_span(
         std::span<const char> bytes, const trusted_record_state<W, T>& state)
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
         // The first later member with an offset slot, if any.
         constexpr std::size_t next_variable = [&] {
            for (std::size_t i = N + 1; i < fields.size(); ++i)
               if (fields[i].variable) return i;
            return fields.size();
         }();
         // Members every valid encoding of T has.
         constexpr std::size_t written = required_field_count<T>();
         constexpr std::size_t header  = ::psio::is_dwnc_v<T> ? 0 : W;
         const std::size_t     fixed_size = state.fixed;
         if constexpr (N < written)
         {
            // (The state's constructor compared the header with the fixed
            // region of the required members.)
            if (bytes.data() == nullptr)
               return {};
         }
         else if (fixed_off + fields[N].fixed_size > fixed_size)
         {
            if constexpr (omittable_field_v<T, N>)
            {
               using F = typename record_traits<T>::template member_type<N>;
               if constexpr (optional_uses_selector_v<typename F::value_type>)
               {
                  static constexpr char none = 0;
                  return {&none, 1};
               }
            }
            return {};
         }
         if constexpr (!fields[N].variable)
         {
            if constexpr (fields[N].fixed_size == 0)
               return {};
            else
               return {bytes.data() + header + fixed_off, fields[N].fixed_size};
         }
         else
         {
            const std::size_t start = read_offset<W>(bytes, header + fixed_off);
            std::size_t       stop  = bytes.size() - header;
            if constexpr (next_variable < written)
            {
               // The next offset slot is always written.
               PSIO_VIEW_TRUST(next_slot + W <= fixed_size, "a record's header cuts an offset slot");
               stop = read_offset<W>(bytes, header + next_slot);
            }
            else if constexpr (next_variable == fields.size() && ::psio::is_dwnc_v<T>)
            {
               // The last variable member of a record that cannot grow ends
               // where the record ends.
            }
            else if (next_slot < fixed_size)
            {
               PSIO_VIEW_TRUST(W <= fixed_size - next_slot, "a record's header cuts an offset slot");
               stop = read_offset<W>(bytes, header + next_slot);
            }
            PSIO_VIEW_TRUST(start >= fixed_size && start <= stop && stop <= bytes.size() - header,
                            "the offsets of a record member are out of order or out of range");
            return {bytes.data() + header + start, stop - start};
         }
      }

      template <std::size_t W, typename T, std::size_t N>
      [[gnu::always_inline]] inline std::span<const char> record_field_span(
         std::span<const char> bytes, unchecked_t)
      {
         return record_field_span<W, T, N>(bytes, trusted_record_state<W, T>{bytes});
      }

      // ── variant ────────────────────────────────────────────────────────
      inline std::size_t variant_index(std::span<const char> s)
      {
         if (s.empty())
            return 0;
         return static_cast<std::size_t>(
            static_cast<unsigned char>(s[0]));
      }

      inline std::span<const char>
      variant_payload_span(std::span<const char> s)
      {
         return s.empty() ? std::span<const char>{} : s.subspan(1);
      }

      // The unchecked forms (trusted view).
      [[gnu::always_inline]] inline std::size_t variant_index(std::span<const char> s,
                                                              unchecked_t)
      {
         if (s.data() == nullptr)
            return 0;
         PSIO_VIEW_TRUST(!s.empty(), "a variant has no selector byte");
         return static_cast<std::size_t>(static_cast<unsigned char>(s[0]));
      }

      [[gnu::always_inline]] inline std::span<const char> variant_payload_span(
         std::span<const char> s, unchecked_t)
      {
         if (s.data() == nullptr)
            return {};
         PSIO_VIEW_TRUST(!s.empty(), "a variant has no selector byte");
         return {s.data() + 1, s.size() - 1};
      }

   }  // namespace detail_pssz

// The unchecked forms of a PSSZ traits class (view.hpp "Safe and trusted
// views"). PSIO_W(type...) is the offset width the class selects for a type.
#define PSIO_PSSZ_VIEW_UNCHECKED_FORMS_                                                    \
   static constexpr bool trusted_views = true;                                             \
   template <typename T>                                                                   \
      requires ::psio::detail::pssz_impl::Record<T>                                        \
   using trusted_record_state = detail_pssz::trusted_record_state<PSIO_W(T), T>;           \
   template <typename T, std::size_t N>                                                    \
   static std::span<const char> record_field_span(                                         \
      std::span<const char> s, std::span<const char>,                                      \
      const trusted_record_state<T>& state)                                       \
   {                                                                                       \
      return detail_pssz::record_field_span<PSIO_W(T), T, N>(s, state);                    \
   }                                                                                       \
   template <typename T, std::size_t N>                                                    \
      requires (::psio::detail::pssz_impl::Record<T> &&                                    \
                N < ::psio::detail::pssz_impl::record_traits<T>::member_count)             \
   static std::span<const char> record_field_span(                                         \
      std::span<const char> s, std::span<const char>, unchecked_t)                \
   {                                                                                       \
      return detail_pssz::record_field_span<PSIO_W(T), T, N>(s, unchecked);                \
   }                                                                                       \
   template <typename T>                                                                   \
   static std::size_t vector_count(std::span<const char> s, unchecked_t)          \
   {                                                                                       \
      return detail_pssz::vector_count<PSIO_W(std::vector<T>), T>(s, unchecked);           \
   }                                                                                       \
   template <typename T>                                                                   \
   static std::span<const char> vector_element_span(std::span<const char> s, std::size_t i, \
                                                    unchecked_t)                  \
   {                                                                                       \
      return detail_pssz::vector_element_span<PSIO_W(std::vector<T>), T>(s, i, unchecked); \
   }                                                                                       \
   template <typename T, std::size_t N>                                                    \
   static std::span<const char> array_element_span(std::span<const char> s, std::size_t i, \
                                                   unchecked_t)                   \
   {                                                                                       \
      /* (An array always has N elements: a view of nothing has N empty ones.) */          \
      if (s.data() == nullptr)                                                             \
         return {};                                                                        \
      return detail_pssz::vector_element_span<PSIO_W(std::array<T, N>), T>(s, i, unchecked); \
   }                                                                                       \
   template <typename T>                                                                   \
   static bool optional_has_value(std::span<const char> s, unchecked_t)           \
   {                                                                                       \
      return detail_pssz::optional_has_value<std::optional<T>>(s, unchecked);              \
   }                                                                                       \
   template <typename T>                                                                   \
   static std::span<const char> optional_payload_span(std::span<const char> s,             \
                                                      unchecked_t)                \
   {                                                                                       \
      return detail_pssz::optional_payload_span<std::optional<T>>(s, unchecked);           \
   }                                                                                       \
   template <typename... Ts>                                                               \
   static std::size_t variant_index(std::span<const char> s, unchecked_t)         \
   {                                                                                       \
      return detail_pssz::variant_index(s, unchecked);                                     \
   }                                                                                       \
   template <typename... Ts>                                                               \
   static std::span<const char> variant_payload_span(std::span<const char> s,              \
                                                     unchecked_t)                 \
   {                                                                                       \
      return detail_pssz::variant_payload_span(s, unchecked);                              \
   }

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
         std::span<const char> s, std::span<const char> = {})
      { return detail_pssz::record_field_span<W, T, N>(s); }
      template <typename T>
         requires ::psio::detail::pssz_impl::Record<T>
      using record_state = detail_pssz::record_state<W, T>;
      template <typename T, std::size_t N>
      static std::span<const char> record_field_span(
         std::span<const char> s, std::span<const char>, const record_state<T>& state)
      { return detail_pssz::record_field_span<W, T, N>(s, state); }

      template <typename T>
      static std::size_t vector_count(std::span<const char> s)
      {
         return detail_pssz::vector_count<W, T>(s);
      }

      template <typename T>
      static std::span<const char>
      vector_element_span(std::span<const char> s, std::size_t i)
      {
         return detail_pssz::vector_element_span<W, T>(s, i);
      }

      template <typename T, std::size_t N>
      static std::span<const char> array_element_span(std::span<const char> s, std::size_t i)
      {
         return detail_pssz::vector_element_span<W, T>(s, i);
      }

      template <typename T>
      static bool optional_has_value(std::span<const char> s)
      {
         return detail_pssz::optional_has_value<std::optional<T>>(s);
      }

      template <typename T>
      static std::span<const char>
      optional_payload_span(std::span<const char> s)
      {
         return detail_pssz::optional_payload_span<std::optional<T>>(s);
      }

      template <typename... Ts>
      static std::size_t variant_index(std::span<const char> s)
      {
         return detail_pssz::variant_index(s);
      }

      template <typename... Ts>
      static std::span<const char>
      variant_payload_span(std::span<const char> s)
      {
         return detail_pssz::variant_payload_span(s);
      }

#define PSIO_W(...) W
      PSIO_PSSZ_VIEW_UNCHECKED_FORMS_
#undef PSIO_W
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
         std::span<const char> s, std::span<const char> = {})
      {
         return detail_pssz::record_field_span<::psio::auto_pssz_width_v<T>, T, N>(s);
      }
      template <typename T>
         requires ::psio::detail::pssz_impl::Record<T>
      using record_state = detail_pssz::record_state<::psio::auto_pssz_width_v<T>, T>;
      template <typename T, std::size_t N>
      static std::span<const char> record_field_span(
         std::span<const char> s, std::span<const char>, const record_state<T>& state)
      {
         return detail_pssz::record_field_span<::psio::auto_pssz_width_v<T>, T, N>(s, state);
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
      static std::size_t vector_count(std::span<const char> s)
      {
         constexpr std::size_t W =
            ::psio::auto_pssz_width_v<std::vector<T>>;
         return detail_pssz::vector_count<W, T>(s);
      }

      template <typename T>
      static std::span<const char>
      vector_element_span(std::span<const char> s, std::size_t i)
      {
         constexpr std::size_t W =
            ::psio::auto_pssz_width_v<std::vector<T>>;
         return detail_pssz::vector_element_span<W, T>(s, i);
      }

      // A fixed-count array owns its own logical width, which can differ
      // from a vector of the same element type.
      template <typename T, std::size_t N>
      static std::span<const char> array_element_span(std::span<const char> s, std::size_t i)
      {
         constexpr auto W = ::psio::auto_pssz_width_v<std::array<T, N>>;
         return detail_pssz::vector_element_span<W, T>(s, i);
      }

      template <typename T>
      static bool optional_has_value(std::span<const char> s)
      {
         return detail_pssz::optional_has_value<std::optional<T>>(s);
      }

      template <typename T>
      static std::span<const char>
      optional_payload_span(std::span<const char> s)
      {
         return detail_pssz::optional_payload_span<std::optional<T>>(s);
      }

      template <typename... Ts>
      static std::size_t variant_index(std::span<const char> s)
      {
         return detail_pssz::variant_index(s);
      }

      template <typename... Ts>
      static std::span<const char>
      variant_payload_span(std::span<const char> s)
      {
         return detail_pssz::variant_payload_span(s);
      }

#define PSIO_W(...) ::psio::auto_pssz_width_v<__VA_ARGS__>
      PSIO_PSSZ_VIEW_UNCHECKED_FORMS_
#undef PSIO_W
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
         std::span<const char> s, std::span<const char> = {})
      {
         return detail_pssz::record_field_span<width<T>, T, N>(s);
      }
      template <typename T>
         requires ::psio::detail::pssz_impl::Record<T>
      using record_state = detail_pssz::record_state<width<T>, T>;
      template <typename T, std::size_t N>
      static std::span<const char> record_field_span(
         std::span<const char> s, std::span<const char>, const record_state<T>& state)
      {
         return detail_pssz::record_field_span<width<T>, T, N>(s, state);
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
      static std::size_t vector_count(std::span<const char> s)
      {
         return detail_pssz::vector_count<width<std::vector<T>>, T>(s);
      }
      template <typename T>
      static std::span<const char>
      vector_element_span(std::span<const char> s, std::size_t i)
      {
         return detail_pssz::vector_element_span<width<std::vector<T>>, T>(s, i);
      }
      template <typename T, std::size_t N>
      static std::span<const char> array_element_span(std::span<const char> s, std::size_t i)
      {
         return detail_pssz::vector_element_span<width<std::array<T, N>>, T>(s, i);
      }
      template <typename T>
      static bool optional_has_value(std::span<const char> s)
      {
         return detail_pssz::optional_has_value<std::optional<T>>(s);
      }
      template <typename T>
      static std::span<const char>
      optional_payload_span(std::span<const char> s)
      {
         return detail_pssz::optional_payload_span<std::optional<T>>(s);
      }
      template <typename... Ts>
      static std::size_t variant_index(std::span<const char> s)
      {
         return detail_pssz::variant_index(s);
      }
      template <typename... Ts>
      static std::span<const char>
      variant_payload_span(std::span<const char> s)
      {
         return detail_pssz::variant_payload_span(s);
      }

#define PSIO_W(...) width<__VA_ARGS__>
      PSIO_PSSZ_VIEW_UNCHECKED_FORMS_
#undef PSIO_W
   };
#undef PSIO_PSSZ_VIEW_UNCHECKED_FORMS_

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

   // A PSSZ view converts implicitly to its value exactly when the value is
   // trivially copyable and of fixed encoded size (view.hpp, "From a view to
   // what it designates"): scalars, enums, time points, uint256,
   // std::array of such, definitionWillNotChange records of such — a
   // 32-byte checksum, an account number. (Not a record that itself only
   // refers to its members.)
   namespace view_layout {
      template <typename Fmt, typename T>
         requires ::psio::detail::is_pssz_format_v<Fmt>
      inline constexpr bool value_convertible_v<Fmt, T> =
         std::is_trivially_copyable_v<T> && !::psio::is_source_v<T> &&
         ::psio::detail::pssz_impl::plain_members_v<T> &&
         ::psio::detail::pssz_impl::is_fixed_v<T>;
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
         explicit tuple_view(view_source<Store> s) : data_(view_source_bytes(s)) {}
         tuple_view(view_source<Store> s, std::span<const char>)
            : data_(view_source_bytes(s))
         {
         }
         std::span<const char> _psio_data() const { return data_; }
         // Element N: its value for an arithmetic element, its view
         // otherwise. Structured bindings work: `auto [a, b] = tuple_view;`.
         template <std::size_t N> auto get() const
         {
            using F = std::remove_cvref_t<std::tuple_element_t<N, Tuple>>;
            auto span = view_layout::field_of<Fmt, Store, Tuple, N>(data_, {});
            if constexpr (std::is_arithmetic_v<F>)
            {
               F out{};
               if constexpr (trusted_store_v<Store>)
               {
                  if (view_has_scalar<Store, sizeof(F)>(span))
                     std::memcpy(&out, span.data(), sizeof(F));
               }
               else if (span.size() == sizeof(F))
                  std::memcpy(&out, span.data(), sizeof(F));
               return out;
            }
            else
               return view<F, view_layout::member_format_t<Fmt, Tuple, N>, Store>{
                  view_src<Store>(span)};
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
      using element_type = std::tuple<Ts...>;
      using format_type  = Fmt;
      static constexpr storage storage_kind = Store;
      PSIO_VIEW_TO_SAFE_(this->_psio_data())
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
      using element_type = annotated_tuple<Meta, Ts...>;
      using format_type  = Fmt;
      static constexpr storage storage_kind = Store;
      PSIO_VIEW_TO_SAFE_(this->_psio_data())
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
      auto first() const { return this->template get<0>(); }
      auto second() const { return this->template get<1>(); }
      [[nodiscard]] std::span<const char> _psio_data() const { return base::_psio_data(); }
      static constexpr storage storage_kind = Store;
      PSIO_VIEW_UNPACK_
      PSIO_VIEW_TO_SAFE_(_psio_data())
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

      // An integer key whose `<` is the order of its `key` encoding (char is
      // excluded: its key form is the unsigned byte whatever the platform's
      // char signedness).
      static constexpr bool key_is_ordered_integer =
         std::is_integral_v<key_type> && !std::is_same_v<key_type, char>;

      // The key of entry i, as a view (or a value for an arithmetic key).
      [[nodiscard]] auto key_at(std::size_t i) const
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
      [[nodiscard]] std::span<const char> _psio_data() const { return base::_psio_data(); }

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
         // Integers and strings order exactly as their `key` encodings do,
         // so the probe compares the key in place: no encoding, no
         // allocation.
         if constexpr (key_is_ordered_integer)
         {
            const key_type want(key);
            std::size_t    lo = 0, hi = this->size();
            while (lo < hi)
            {
               const std::size_t mid = lo + (hi - lo) / 2;
               if (static_cast<key_type>(key_at(mid)) < want)
                  lo = mid + 1;
               else
                  hi = mid;
            }
            return lo;
         }
         else if constexpr (std::is_same_v<key_type, std::string> &&
                            std::is_convertible_v<const K&, std::string_view>)
         {
            const std::string_view want(key);
            std::size_t            lo = 0, hi = this->size();
            while (lo < hi)
            {
               const std::size_t mid = lo + (hi - lo) / 2;
               if (key_at(mid).view_() < want)
                  lo = mid + 1;
               else
                  hi = mid;
            }
            return lo;
         }
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
         if (i == this->size())
            return i;
         if constexpr (key_is_ordered_integer)
            return static_cast<key_type>(key_at(i)) == key_type(key) ? i : this->size();
         else if constexpr (std::is_same_v<key_type, std::string> &&
                            std::is_convertible_v<const K&, std::string_view>)
            return key_at(i).view_() == std::string_view(key) ? i : this->size();
         else
            return key_type(::psio::value_of(key_at(i))) == key_type(key) ? i : this->size();
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

      static constexpr storage storage_kind = Store;
      PSIO_VIEW_UNPACK_
      PSIO_VIEW_TO_SAFE_(this->_psio_root())
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
      explicit view(detail::view_source<Store> s) : data_(detail::view_source_bytes(s)) {}
      view(detail::view_source<Store> s, std::span<const char>)
         : data_(detail::view_source_bytes(s))
      {
      }
      [[nodiscard]] std::span<const char> _psio_data() const { return data_; }
      [[nodiscard]] std::string_view view_() const
      {
         return std::string_view{data_.data(), data_.size()};
      }
      operator std::string_view() const { return view_(); }
      [[nodiscard]] std::size_t size() const { return data_.size(); }
      [[nodiscard]] bool        empty() const { return data_.empty(); }
      PSIO_VIEW_UNPACK_
      PSIO_VIEW_TO_SAFE_(_psio_data())
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
      explicit view(detail::view_source<Store> s) : data_(detail::view_source_bytes(s)) {}
      view(detail::view_source<Store> s, std::span<const char>)
         : data_(detail::view_source_bytes(s))
      {
      }
      [[nodiscard]] std::span<const char> _psio_data() const { return data_; }
      [[nodiscard]] T get() const
      {
         std::underlying_type_t<T> raw{};
         if (detail::view_has_scalar<Store, sizeof(raw)>(data_))
            std::memcpy(&raw, data_.data(), sizeof(raw));
         return static_cast<T>(raw);
      }
      [[nodiscard]] T unpack() const { return get(); }
      operator T() const { return get(); }
      PSIO_VIEW_TO_SAFE_(_psio_data())
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
      explicit view(detail::view_source<Store> s) : data_(detail::view_source_bytes(s)) {}
      view(detail::view_source<Store> s, std::span<const char>)
         : data_(detail::view_source_bytes(s))
      {
      }
      [[nodiscard]] std::span<const char> _psio_data() const { return data_; }
      [[nodiscard]] T get() const
      {
         ::psio::time_rep_t<T> raw{};
         if (detail::view_has_scalar<Store, sizeof(raw)>(data_))
            std::memcpy(&raw, data_.data(), sizeof(raw));
         return ::psio::time_from_ticks<T>(raw);
      }
      [[nodiscard]] T unpack() const { return get(); }
      operator T() const { return get(); }
      PSIO_VIEW_TO_SAFE_(_psio_data())
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
      explicit view(detail::view_source<Store> s) : data_(detail::view_source_bytes(s)) {}
      view(detail::view_source<Store> s, std::span<const char>)
         : data_(detail::view_source_bytes(s))
      {
      }
      [[nodiscard]] std::span<const char> _psio_data() const { return data_; }
      [[nodiscard]] uint256 get() const
      {
         uint256 out{};
         if (detail::view_has_scalar<Store, sizeof(out.limb)>(data_))
            std::memcpy(out.limb, data_.data(), sizeof(out.limb));
         return out;
      }
      [[nodiscard]] uint256 unpack() const { return get(); }
      operator uint256() const { return get(); }
      PSIO_VIEW_TO_SAFE_(_psio_data())
   };

   template <typename T, typename Fmt, storage Store>
      requires detail::is_pssz_format_v<Fmt>
   class view<schema_types::Box<T>, Fmt, Store> : public view<T, Fmt, Store>
   {
    public:
      using view<T, Fmt, Store>::view;
      using format_type                     = Fmt;
      static constexpr storage storage_kind = Store;
      PSIO_VIEW_TO_SAFE_AS_(schema_types::Box<T>, this->_psio_data())
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
      view(const base& inner) : base(inner) {}
      [[nodiscard]] std::span<const char> _psio_data() const { return base::_psio_data(); }
      static constexpr storage storage_kind = Store;
      PSIO_VIEW_UNPACK_
      PSIO_VIEW_TO_SAFE_(_psio_data())
   };

   // ── Validate once, then view ──────────────────────────────────────────
   //
   //   auto row = psio::validated_pssz_view<Row>(bytes);   // psio::checked_view<Row, psio::pssz>
   //   if (!row) return report(row.error());
   //   use(row->owner(), row->balance());                  // psio::pssz_trusted_view<Row>
   //
   // `validated_pssz_view<T>(bytes)` checks the complete frame as
   // `psio::validate_pssz<T>` does (unknown appended optional fields are
   // accepted) and hands back the TRUSTED view of it, in a
   // psio::checked_view (view.hpp): no later access compares an offset
   // again. `psio::validated_view<T>(fmt, bytes)` is the same for an
   // explicit-width profile.
   template <typename T>
   [[nodiscard]] checked_view<T, pssz> validated_pssz_view(std::span<const char> bytes) noexcept
   {
      return ::psio::validated_view<T>(pssz{}, bytes);
   }

   // The same with an out-parameter, reporting whether the frame carries
   // fields this T does not know. `result` is either kind of view: a
   // std::optional<psio::pssz_trusted_view<T>> receives the trusted view, a
   // std::optional<psio::pssz_view<T>> the safe one.
   template <typename T, typename Fmt, storage Store>
      requires detail::is_pssz_format_v<Fmt>
   [[nodiscard]] codec_status validated_pssz_view(
      Fmt fmt, std::span<const char> bytes, std::optional<view<T, Fmt, Store>>& result,
      bool& has_unknown)
   {
      auto status = validate_pssz_compatible<T>(fmt, bytes, has_unknown);
      if (status.ok()) result.emplace(detail::view_src<Store>(bytes));
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
   using method_args_trusted_view =
      view<typename reflect<T>::template method_arg_tuple<I>, pssz, storage::trusted_borrow>;

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
            // (A parameter declared psio::pssz_trusted_view<X> is handed the
            // trusted view: the argument tuple has been validated, or the
            // caller vouched for it.)
            return P{view_src<P::storage_kind>(part)};
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
   // for a void method). The result is the callee's own value: the FAST
   // write (psio::encode_method_result), whose status is that of a value
   // with no encoding (an offset width, a cap, depth).
   // `call_method_pssz_checked` is the VALIDATED write of the result
   // (psio::encode_method_result_checked): also a value outside
   // returns(...). The caller validates what it reads either way.
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
   template <typename T, std::size_t I, typename Obj>
   [[nodiscard]] codec_status call_method_pssz_checked(Obj&& obj, trusted bytes,
                                                       std::vector<char>& result)
   {
      if constexpr (std::is_void_v<typename reflect<T>::template method_result<I>>)
      {
         invoke_method_pssz<T, I>(std::forward<Obj>(obj), bytes);
         return codec_ok();
      }
      else
         return ::psio::encode_method_result_checked<T, I>(
            invoke_method_pssz<T, I>(std::forward<Obj>(obj), bytes), result);
   }

   // ── Viewing a method's result ─────────────────────────────────────────
   //
   // The read view of the result of method I of T, as returns(...) types
   // it. Where returns(...) narrows an offset width (a `max<4> |
   // element(utf8 | max<16>)` list of strings has a 1-byte offset table)
   // the view of the plain return type would read wrong offsets; this one
   // reads the result's own.
   //
   //   method_result_view<T, I>             the safe view
   //   method_result_trusted_view<T, I>     the trusted view
   //   validated_method_result_view<T, I>(bytes)
   //                                        validate_method_result, then the
   //                                        trusted view in a checked_view
   //
   // An empty optional result whose payload cannot be empty is zero bytes
   // (pssz.hpp "A result in one call"): its view is the empty optional
   // view (`!view.has_value()`).
   namespace detail
   {
      template <typename T, std::size_t I>
      using method_result_format = view_layout::detail_pssz::format_at<
         pssz_impl::method_result_t<T, I>, pssz_impl::method_result_ann<T, I>>;
   }
   template <typename T, std::size_t I>
   using method_result_view =
      view<method_result_t<T, I>, detail::method_result_format<T, I>, storage::const_borrow>;
   template <typename T, std::size_t I>
   using method_result_trusted_view =
      view<method_result_t<T, I>, detail::method_result_format<T, I>, storage::trusted_borrow>;

   // ── A result held packed ──────────────────────────────────────────────
   //
   // The counterpart of `psio::pssz_buffer<X>` / `psio::shared_pssz<X>` for
   // the result of method I of T (buffer.hpp, docs/helpers.md "Packed
   // values"): bytes that ARE a result of the method, owned or shared.
   //
   //   method_result_buffer<T, I>    owning
   //   shared_method_result<T, I>    reference-counted, immutable
   //
   //   B{value} / B::from(value)     psio::method_result_to_pssz
   //   B{bytes} / B::checked(...)    psio::validate_method_result
   //   b.view() / *b / b->           psio::method_result_trusted_view<T, I>
   //   psio::as_view(b)              psio::method_result_view<T, I>
   //   b.unpack()                    psio::method_result_from_pssz(trusted)
   //   b.decode_checked(out)         psio::decode_method_result
   //
   // `pssz_method_result<T, I>` is the buffer's format tag: the codec of
   // the result as returns(...) types it. It is always this tag — also for
   // a method whose returns(...) changes no byte — so that what the buffer
   // validates is the method's result and not merely its return type.
   template <typename T, std::size_t I>
   struct pssz_method_result
   {
      using result_type = method_result_t<T, I>;
      template <typename R>
      using view_format = detail::method_result_format<T, I>;

      template <std::same_as<result_type> R>
      [[nodiscard]] static codec_status encode_checked(const R& value, std::vector<char>& out)
      {
         return ::psio::encode_method_result_checked<T, I>(value, out);
      }
      template <std::same_as<result_type> R>
      [[nodiscard]] static std::vector<char> encode(const R& value)
      {
         return ::psio::method_result_to_pssz<T, I>(value);
      }
      template <std::same_as<result_type> R>
      [[nodiscard]] static codec_status validate(std::span<const char> bytes) noexcept
      {
         return ::psio::validate_method_result<T, I>(bytes);
      }
      template <std::same_as<result_type> R>
      [[nodiscard]] static R decode(std::span<const char> bytes)
      {
         return ::psio::method_result_from_pssz<T, I>(trusted{bytes});
      }
   };
   template <typename T, std::size_t I>
   using method_result_buffer =
      buffer<method_result_t<T, I>, pssz_method_result<T, I>, storage::owning>;
   template <typename T, std::size_t I>
   using shared_method_result =
      buffer<method_result_t<T, I>, pssz_method_result<T, I>, storage::shared>;

   // The same for the arguments: these ARE `psio::pssz_buffer` /
   // `psio::shared_pssz` of the argument tuple, whose type carries the
   // method's cap and parameter annotations.
   template <typename T, std::size_t I>
   using method_args_buffer =
      buffer<typename reflect<T>::template method_arg_tuple<I>, pssz, storage::owning>;
   template <typename T, std::size_t I>
   using shared_method_args =
      buffer<typename reflect<T>::template method_arg_tuple<I>, pssz, storage::shared>;

   template <typename T, std::size_t I>
   [[nodiscard]] checked_view<method_result_t<T, I>, detail::method_result_format<T, I>>
   validated_method_result_view(std::span<const char> bytes) noexcept
   {
      using View = checked_view<method_result_t<T, I>, detail::method_result_format<T, I>>;
      if (auto status = validate_method_result<T, I>(bytes); !status.ok())
         return View{std::move(status)};
      return View{trusted{bytes}};
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
