#pragma once
//
// psio/detail/key_assoc.hpp — maps and sets in the `key` format, and which
// types can order the entries of a map / set (included by key.hpp;
// docs/key-spec.md).
//
// The key of a map / set is the key of the list of its entries / elements
// in canonical order (detail/assoc_order.hpp): `01` + key(k) + key(v) per
// entry, then `00`; the octet framing for a set of octets. Readers require
// strictly ascending keys.

#include <psio/detail/assoc_order.hpp>

#include <limits>
#include <vector>

namespace psio::detail::key_impl
{
   // ── Which types have a key encoding ──────────────────────────────
   //
   //   none      no key encoding (bitlist, a multi container, a type
   //             encode_scalar rejects), here or in a part of the type
   //   adapted   encoded, with a sortable_binary_category adapter
   //             somewhere in the type: its bytes are the adapter's
   //   plain     encoded by the rules of docs/key-spec.md alone
   //
   // The key type of a map and the element type of a set must be
   // `plain`: their entries are ordered by key bytes, and that order
   // has to be the same for every reader, including one that knows the
   // type only through its schema.
   enum class key_form : std::uint8_t
   {
      none,
      adapted,
      plain,
   };
   constexpr key_form weakest(key_form a, key_form b) noexcept { return a < b ? a : b; }

   template <typename T>
   consteval key_form key_form_of() noexcept
   {
      using U = std::remove_cv_t<T>;
      if constexpr (unsupported_v<U>)
         return key_form::none;
      else if constexpr (::psio::format_should_dispatch_adapter_v<::psio::key, U>)
         return key_form::adapted;
      else if constexpr (::psio::is_transparent_v<U>)
         // A transparent type has its member's key.
         return key_form_of<::psio::transparent_inner_t<U>>();
      else if constexpr (::psio::detail::is_nested_v<U>)
         // The key of a byte list, whatever the inner type is.
         return key_form::plain;
      else if constexpr (::psio::detail::is_map_v<U>)
         return key_form_of<::psio::detail::assoc_key_t<U>>() != key_form::plain
                   ? key_form::none
                   : weakest(key_form::plain, key_form_of<typename U::mapped_type>());
      else if constexpr (::psio::detail::is_set_v<U>)
         return key_form_of<::psio::detail::assoc_key_t<U>>() != key_form::plain
                   ? key_form::none
                   : key_form::plain;
      else if constexpr (std::is_same_v<U, std::string> || is_utf8_string<U>::value ||
                         is_byte_array<U>::value || is_bitvector<U>::value)
         return key_form::plain;
      else if constexpr (is_std_optional<U>::value || is_std_vector<U>::value ||
                         is_bounded<U>::value || is_std_array<U>::value)
         return key_form_of<typename U::value_type>();
      else if constexpr (is_std_variant<U>::value)
         return []<std::size_t... Is>(std::index_sequence<Is...>) {
            key_form form = std::variant_size_v<U> <= 256 ? key_form::plain : key_form::none;
            ((form = weakest(form, key_form_of<std::variant_alternative_t<Is, U>>())), ...);
            return form;
         }(std::make_index_sequence<std::variant_size_v<U>>{});
      else if constexpr (is_std_tuple<U>::value)
         return []<std::size_t... Is>(std::index_sequence<Is...>) {
            key_form form = key_form::plain;
            ((form = weakest(form, key_form_of<std::tuple_element_t<Is, U>>())), ...);
            return form;
         }(std::make_index_sequence<std::tuple_size_v<U>>{});
      else if constexpr (Record<U>)
         return []<std::size_t... Is>(std::index_sequence<Is...>) {
            key_form form = key_form::plain;
            ((form = weakest(form, key_form_of<typename ::psio::reflect<
                                      U>::template member_type<Is>>())),
             ...);
            return form;
         }(std::make_index_sequence<::psio::reflect<U>::member_count>{});
      else
         return std::is_same_v<U, bool> || is_raw_byte_v<U> ||
                      std::is_same_v<U, ::psio::uint256> ||
                      std::is_same_v<U, ::psio::uint128> ||
                      std::is_same_v<U, ::psio::int128> || std::is_same_v<U, float> ||
                      std::is_same_v<U, double> || std::is_enum_v<U> ||
                      ::psio::TimeValue<U> || std::is_integral_v<U>
                   ? key_form::plain
                   : key_form::none;
   }

   // Whether K can be the key type of a map / the element type of a set.
   template <typename K>
   inline constexpr bool orders_entries_v = key_form_of<K>() == key_form::plain;

   // What every codec asserts about the key type of a map / set C.
#define PSIO_REQUIRE_ENTRY_ORDER(Format, C)                                                   \
   static_assert(                                                                             \
      ::psio::detail::key_impl::orders_entries_v<::psio::detail::assoc_key_t<C>>,             \
      Format ": the key type of a map (the element type of a set) must have a key encoding " \
             "(docs/key-spec.md) and no sortable_binary_category adapter: entries are "      \
             "ordered by the key bytes of their keys")

   // A map / set is the list of its entries / elements in canonical
   // order: the octet framing for a set of octets, `01 entry ... 00`
   // otherwise. Two entries with equal key bytes have no encoding.
   template <typename C, typename S>
   void encode_assoc(const C& v, S& s)
   {
      PSIO_REQUIRE_ENTRY_ORDER("psio::key", C);
      using K = ::psio::detail::assoc_key_t<C>;
      const ::psio::detail::canonical_order<C> order(v);
      if (!order.unique())
         ::psio::codec_abort(::psio::codec_error{
            ::psio::detail::is_map_v<C> ? "key: map has duplicate keys"
                                        : "key: set has duplicate elements",
            0, "key"});
      if constexpr (::psio::detail::is_set_v<C> && is_octet_v<K>)
      {
         order.for_each([&](const K& elem) {
            const char b = octet_key_byte(elem);
            s.write(b);
            if (b == '\0')
               s.write('\1');
         });
         s.write('\0');
         s.write('\0');
      }
      else
      {
         order.for_each([&](const auto& item) {
            s.write('\x01');
            if constexpr (::psio::detail::is_map_v<C>)
            {
               encode_value<K>(item.first, s);
               encode_value(item.second, s);
            }
            else
               encode_value<K>(item, s);
         });
         s.write('\x00');
      }
   }

   // A map / set: the list framing of its entries / elements, whose keys
   // must be strictly ascending by key bytes. The key bytes are the
   // bytes just read, so the order is checked on the buffer itself.
   template <typename C>
   bool read_assoc(reader& r, C* out)
   {
      PSIO_REQUIRE_ENTRY_ORDER("psio::key", C);
      using K = ::psio::detail::assoc_key_t<C>;
      constexpr bool    is_map = ::psio::detail::is_map_v<C>;
      constexpr const char* unordered =
   is_map ? "key: map entries are not in strictly ascending key order"
          : "key: set elements are not in strictly ascending order";
      constexpr const char* merged =
   is_map ? "key: map container merged distinct keys"
          : "key: set container merged distinct elements";
      if (out)
   out->clear();
      std::size_t count = 0;
      if constexpr (::psio::detail::is_set_v<C> && is_octet_v<K>)
      {
   std::vector<K> elems;
   if (!read_list(r, &elems, std::numeric_limits<std::size_t>::max()))
      return false;
   for (std::size_t i = 1; i < elems.size(); ++i)
      if (static_cast<unsigned char>(octet_key_byte(elems[i - 1])) >=
          static_cast<unsigned char>(octet_key_byte(elems[i])))
         return r.fail(unordered);
   count = elems.size();
   if (out)
      out->insert(elems.begin(), elems.end());
      }
      else
      {
   const char* prev_begin = nullptr;
   const char* prev_end   = nullptr;
   for (;;)
   {
      if (!r.need(1))
         return false;
      const auto tag = static_cast<unsigned char>(*r.p++);
      if (tag == 0)
         break;
      if (tag != 1)
         return r.fail("key: invalid list element marker");
      const char* const key_begin = r.p;
      K                 k{};
      if (!read_value<K>(r, out ? &k : nullptr))
         return false;
      const char* const key_end = r.p;
      if (prev_begin &&
          ::psio::detail::compare_key_bytes(
             prev_begin, static_cast<std::size_t>(prev_end - prev_begin), key_begin,
             static_cast<std::size_t>(key_end - key_begin)) >= 0)
      {
         r.p = key_begin;
         return r.fail(unordered);
      }
      prev_begin = key_begin;
      prev_end   = key_end;
      ++count;
      if constexpr (is_map)
      {
         using V = typename C::mapped_type;
         if (!out)
         {
            if (!read_value<V>(r, nullptr))
               return false;
         }
         else
         {
            V value{};
            if (!read_value<V>(r, &value))
               return false;
            out->emplace_hint(out->end(), std::move(k), std::move(value));
         }
      }
      else if (out)
         out->emplace_hint(out->end(), std::move(k));
   }
      }
      if (out && out->size() != count)
   return r.fail(merged);
      return true;
   }

}  // namespace psio::detail::key_impl
