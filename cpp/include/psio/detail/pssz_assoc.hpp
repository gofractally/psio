#pragma once
//
// psio/detail/pssz_assoc.hpp — maps and sets in PSSZ (included by pssz.hpp;
// docs/pssz-spec.md "Pairs, maps and sets").
//
// A map is the list of its entries — each the extensible 2-tuple (K, V) —
// and a set the list of its elements, in strictly ascending order of the
// `key` encoding of the key (detail/assoc_order.hpp):
//
//    encode(map<K, V>) == encode(vector<tuple<K, V>>)   of the sorted entries
//    encode(set<K>)    == encode(vector<K>)             of the sorted elements
//
// The encoder writes that order whatever the container iterates in and
// refuses two entries with equal key bytes. Every checking reader requires
// the order (which also rules out duplicates). The trusted decode inserts
// the entries into the container.

#include <psio/detail/assoc_order.hpp>
#include <psio/key.hpp>

#include <optional>
#include <span>
#include <vector>

namespace psio::detail::pssz_impl
{
   template <typename T>
   inline constexpr const char* assoc_duplicate_message =
      ::psio::detail::is_map_v<T> ? "pssz: map has duplicate keys"
                                  : "pssz: set has duplicate elements";
   template <typename T>
   inline constexpr const char* assoc_order_message =
      ::psio::detail::is_map_v<T>
         ? "pssz: map entries are not in strictly ascending key order"
         : "pssz: set elements are not in strictly ascending order";

   // Encoder preflight: every entry is encodable one node below the
   // container (as a list's elements are), and no two keys are equal.
   template <typename T>
   codec_status assoc_check_encode(const T& value, std::size_t depth, encode_preflight bounds)
   {
      PSIO_REQUIRE_ENTRY_ORDER("psio::pssz", T);
      using E = ::psio::detail::assoc_entry_t<T>;
      if constexpr (is_zero_fixed_v<E>)
         if (!value.empty())
            return codec_fail("pssz: nonempty zero-size vector is unsupported", 0, "pssz");
      for (const auto& item : value)
      {
         auto status = check_encode_depth_impl(item, depth + 1, bounds);
         if (!status.ok())
            return status;
      }
      if constexpr (!::psio::detail::iterates_in_key_order_v<T>)
      {
         const ::psio::detail::canonical_order<T> order(value);
         if (!order.unique())
            return codec_fail(assoc_duplicate_message<T>, 0, "pssz");
      }
      return codec_ok();
   }

   // The size of the list of the entries in canonical order (the offsets
   // checked against W are those the encoder writes).
   template <std::size_t W, bool Auto, typename T, typename A, typename P>
   std::size_t assoc_size(const T& v, P check)
   {
      using E = ::psio::detail::assoc_entry_t<T>;
      if constexpr (is_fixed_v<E>)
      {
         if constexpr (is_zero_fixed_v<E>)
            if (!v.empty())
               size_fail(check, "pssz: nonempty zero-size vector is unsupported");
         if constexpr (Auto)
            if (!v.empty())
               enforce_fixed_caps<E>(check);
         return checked_multiply(v.size(), fixed_size_of<E>(), check);
      }
      else
      {
         const ::psio::detail::canonical_order<T> order(v);
         size_sum    sum{checked_multiply(v.size(), W, check), false};  // the offset table
         std::size_t last = sum.total;
         order.for_each([&](const auto& item) {
            last = sum.total;
            sum.add(size_of_v<W, Auto, std::remove_cvref_t<decltype(item)>, ann::entry_ann<T, A>>(
               item, check));
         });
         enforce_container<W>(sum, last, check);
         return sum.total;
      }
   }

   template <std::size_t W, bool Auto, typename T, typename Sink, typename A>
   void assoc_encode(const T& v, Sink& s)
   {
      using E = ::psio::detail::assoc_entry_t<T>;
      const ::psio::detail::canonical_order<T> order(v);
      if (!order.unique())
         size_failure(assoc_duplicate_message<T>);
      if constexpr (is_fixed_v<E>)
      {
         if constexpr (is_zero_fixed_v<E>)
            if (!v.empty())
               size_failure("pssz: nonempty zero-size vector is unsupported");
         order.for_each([&](const auto& item) { encode_value<W, Auto>(item, s); });
      }
      else
      {
         const std::size_t table = s.written();
         skip_bytes(s, checked_multiply(v.size(), W));
         std::size_t slot = table;
         order.for_each([&](const auto& item) {
            write_offset<W>(s, slot, s.written() - table);
            slot += W;
            encode_value<W, Auto, std::remove_cvref_t<decltype(item)>, Sink,
                         ann::entry_ann<T, A>>(item, s);
         });
      }
   }

   // Trusted decode: the list, inserted into the container. A container
   // whose comparator / equality merges keys the encoding distinguishes
   // would silently lose entries: that is a failure.
   template <std::size_t W, bool Auto, typename T, typename A>
   T assoc_decode(std::span<const char> src, std::size_t pos, std::size_t end)
   {
      PSIO_REQUIRE_ENTRY_ORDER("psio::pssz", T);
      using K = ::psio::detail::assoc_key_t<T>;
      T out;
      std::size_t count = 0;
      if constexpr (::psio::detail::is_map_v<T>)
      {
         auto items =
            decode_vector<W, Auto, std::pair<K, typename T::mapped_type>, ann::entry_ann<T, A>>(
               src, pos, end);
         count = items.size();
         for (auto& item : items)
            out.emplace_hint(out.end(), std::move(item));
      }
      else
      {
         auto items = decode_vector<W, Auto, K, ann::entry_ann<T, A>>(src, pos, end);
         count      = items.size();
         for (auto&& item : items)
            out.emplace_hint(out.end(), std::move(item));
      }
      if (out.size() != count)
         size_failure(::psio::detail::is_map_v<T>
                         ? "pssz: map container merged distinct keys"
                         : "pssz: set container merged distinct elements");
      return out;
   }

   // The keys of a structurally valid list of entries, in wire order:
   // f(const K&) -> bool (false stops the walk).
   template <std::size_t W, bool Auto, typename T, typename F, typename A = no_ann>
   codec_status assoc_for_each_key(std::span<const char> src, std::size_t pos, std::size_t end,
                                   F&& f)
   {
      using EA = ann::entry_ann<T, A>;
      using E = ::psio::detail::assoc_entry_t<T>;
      using K = ::psio::detail::assoc_key_t<T>;
      if (pos == end || is_zero_fixed_v<E>)
         return codec_ok();
      // One element's key, from the element's span.
      auto element = [&](std::size_t begin, std::size_t stop, bool& more) -> codec_status {
         if constexpr (::psio::detail::is_map_v<T>)
         {
            constexpr std::size_t EW = [] {
               if constexpr (Auto) return width_for<E, EA>();
               else return W;
            }();
            constexpr auto layout = record_layout<EW, E>(std::make_index_sequence<2>{});
            std::array<::psio::detail::pssz_field_span, 2> spans{};
            bool unknown = false;
            if (auto st = ::psio::detail::pssz_record_spans_inline<EW>(src, begin, stop, false, layout,
                                                               spans, unknown);
                !st.ok())
               return st;
            // An absent key slot is a pruned trailing optional: None.
            K key{};
            if (spans[0].present)
               key = decode_value<EW, Auto, K, ann::at_ann<EA, 0>>(src, spans[0].begin,
                                                                   spans[0].end);
            more = f(static_cast<const K&>(key));
         }
         else
         {
            const K key = decode_value<W, Auto, K, EA>(src, begin, stop);
            more        = f(key);
         }
         return codec_ok();
      };
      bool more = true;
      if constexpr (is_fixed_v<E>)
      {
         constexpr std::size_t esz = fixed_size_of<E>();
         for (std::size_t at = pos; more && at < end; at += esz)
            if (auto st = element(at, at + esz, more); !st.ok())
               return st;
      }
      else
      {
         const std::size_t n = read_offset<W>(src, pos) / W;
         for (std::size_t i = 0; more && i < n; ++i)
         {
            const std::size_t begin = pos + read_offset<W>(src, pos + i * W);
            const std::size_t stop =
               i + 1 < n ? pos + read_offset<W>(src, pos + (i + 1) * W) : end;
            if (auto st = element(begin, stop, more); !st.ok())
               return st;
         }
      }
      return codec_ok();
   }

   // Strictly ascending keys. With a key type whose `<` is the key order the
   // decoded keys are compared directly; otherwise their key bytes are.
   template <std::size_t W, bool Auto, typename T, typename A = no_ann>
   codec_status assoc_check_order(std::span<const char> src, std::size_t pos, std::size_t end)
   {
      using K = ::psio::detail::assoc_key_t<T>;
      bool ascending = true;
      codec_status status;
      if constexpr (::psio::detail::key_order_is_less<K>())
      {
         std::optional<K> previous;
         const auto visit = [&](const K& key) {
            if (previous && !(*previous < key))
               return ascending = false;
            previous = key;
            return true;
         };
         status = assoc_for_each_key<W, Auto, T, decltype(visit)&, A>(src, pos, end, visit);
      }
      else
      {
         std::vector<char> previous, current;
         bool              first = true;
         const auto visit = [&](const K& key) {
            current.clear();
            ::psio::detail::append_key_bytes(key, current);
            if (!first && ::psio::detail::compare_key_bytes(previous.data(), previous.size(),
                                                            current.data(), current.size()) >= 0)
               return ascending = false;
            first = false;
            previous.swap(current);
            return true;
         };
         status = assoc_for_each_key<W, Auto, T, decltype(visit)&, A>(src, pos, end, visit);
      }
      if (!status.ok())
         return status;
      if (!ascending)
         return codec_fail(assoc_order_message<T>, static_cast<std::uint32_t>(pos), "pssz");
      return codec_ok();
   }

   // The list of entries, then their order. The keys are materialised from
   // spans the structural walk has just accepted.
   template <std::size_t W, bool Auto, typename T, typename A>
   codec_status assoc_validate(std::span<const char> src, std::size_t pos, std::size_t end,
                               std::size_t depth, validate_flags* flags) noexcept
   {
      PSIO_REQUIRE_ENTRY_ORDER("psio::pssz", T);
      using E = ::psio::detail::assoc_entry_t<T>;
      if (auto st = validate_vector_payload<W, Auto, E, ann::entry_ann<T, A>>(src, pos, end,
                                                                              depth, flags);
          !st.ok())
         return st;
#if defined(__cpp_exceptions)
      try
      {
         return assoc_check_order<W, Auto, T, A>(src, pos, end);
      }
      catch (...)
      {
         return codec_fail("pssz: map / set key could not be read", static_cast<std::uint32_t>(pos),
                           "pssz");
      }
#else
      return assoc_check_order<W, Auto, T, A>(src, pos, end);
#endif
   }

   // Maps evolve through their value type only: a different key type has a
   // different order. Sets and maps of the same types are one value kind
   // whatever container holds them; a map is not the plain list of its
   // entries (which need not be sorted).
   template <typename Old, typename New>
   consteval bool assoc_evolves() noexcept
   {
      if constexpr (::psio::detail::is_map_v<Old> && ::psio::detail::is_map_v<New>)
         return std::is_same_v<::psio::detail::assoc_key_t<Old>,
                               ::psio::detail::assoc_key_t<New>> &&
                layout_evolves<typename Old::mapped_type, typename New::mapped_type>();
      else if constexpr (::psio::detail::is_set_v<Old> && ::psio::detail::is_set_v<New>)
         return std::is_same_v<::psio::detail::assoc_key_t<Old>,
                               ::psio::detail::assoc_key_t<New>>;
      else
         return false;
   }

}  // namespace psio::detail::pssz_impl
