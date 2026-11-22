#pragma once
//
// psio/detail/json_assoc.hpp — maps and sets in JSON / I-JSON text (included
// by json.hpp; docs/json.md "Maps and sets").
//
//   set                 an array of its elements
//   map, text key       an object {"k": v, ...} when the key type's text is a
//                       JSON string by type: std::string, utf8_string<N>, or
//                       a type with a text adapter
//   map, any other key  an array of [k, v] pairs
//
// Writers emit canonical order (ascending key bytes, detail/assoc_order.hpp)
// and refuse two entries with equal key bytes. Readers accept any order and
// reject a repeated key / element: a map's keys are data, unlike a record's
// member names (where the last one wins).

#include <psio/detail/assoc_order.hpp>
#include <psio/key.hpp>

#include <span>
#include <string>
#include <string_view>

namespace psio::detail::json_impl
{
   // The text of K is a JSON string whatever the value: a string kind, a
   // type with a text adapter, or a transparent wrapper of one (its text is
   // its member's).
   template <typename Fmt, typename K>
   consteval bool text_key() noexcept
   {
      if constexpr (dispatch_adapter_v<Fmt, K>)
         return true;
      else if constexpr (::psio::is_transparent_v<K>)
         return text_key<Fmt, ::psio::transparent_inner_t<K>>();
      else
         return std::is_same_v<K, std::string> || is_utf8_string<K>::value;
   }
   template <typename Fmt, typename K>
   inline constexpr bool text_key_v = text_key<Fmt, K>();

   template <typename Fmt, int_mode IM, typename T>
   void encode_assoc(const T& v, writer& w)
   {
      PSIO_REQUIRE_ENTRY_ORDER("psio::json", T);
      using K = ::psio::detail::assoc_key_t<T>;
      const ::psio::detail::canonical_order<T> order(v);
      if (!order.unique())
         w.fail(::psio::detail::is_map_v<T> ? "json: map has duplicate keys"
                                            : "json: set has duplicate elements");
      bool first = true;
      auto comma = [&] {
         if (!first)
            w.put(',');
         first = false;
      };
      if constexpr (::psio::detail::is_set_v<T>)
      {
         w.put('[');
         order.for_each([&](const K& element) {
            comma();
            encode_value<Fmt, IM>(element, w);
         });
         w.put(']');
      }
      else if constexpr (text_key_v<Fmt, K>)
      {
         // The key's own text is already one JSON string.
         w.put('{');
         order.for_each([&](const auto& entry) {
            comma();
            encode_value<Fmt, int_mode::natural>(entry.first, w);
            w.put(':');
            encode_value<Fmt, int_mode::natural>(entry.second, w);
         });
         w.put('}');
      }
      else
      {
         w.put('[');
         order.for_each([&](const auto& entry) {
            comma();
            w.put('[');
            encode_value<Fmt, int_mode::natural>(entry.first, w);
            w.put(',');
            encode_value<Fmt, int_mode::natural>(entry.second, w);
            w.put(']');
         });
         w.put(']');
      }
   }

   // An object member name as a key of text-keyed type K: what read_value
   // makes of the same text as a JSON string.
   template <typename Fmt, typename K, typename C>
   bool read_key_shape(typed_reader<C>& r, std::string_view text, K& out);

   // The key, then — as for every value read — its type's invariant.
   template <typename Fmt, typename K, typename C>
   bool read_key_text(typed_reader<C>& r, std::string_view text, K& out)
   {
      if (!read_key_shape<Fmt>(r, text, out))
         return false;
      if constexpr (::psio::has_invariant_v<K>)
         if (auto st = ::psio::check_invariant(out); !st.ok())
         {
            const std::string_view what = st.error().what;
            if (r.invariant.ok())
               r.invariant = std::move(st);
            return r.c.fail(what);
         }
      return true;
   }

   template <typename Fmt, typename K, typename C>
   bool read_key_shape(typed_reader<C>& r, std::string_view text, K& out)
   {
      if constexpr (dispatch_adapter_v<Fmt, K>)
      {
         using Proj = ::psio::adapter<std::remove_cvref_t<K>, ::psio::text_category>;
         const std::span<const char> payload{text};
         if (auto st = Proj::validate(payload); !st.ok())
            return r.c.fail(st.error().what);
         out = Proj::decode(payload);
         return true;
      }
      else if constexpr (::psio::is_transparent_v<K>)
         return read_key_text<Fmt>(r, text, ::psio::transparent_inner(out));
      else if constexpr (std::is_same_v<K, std::string>)
      {
         out.assign(text);
         return true;
      }
      else
      {
         if (text.size() > K::max_size_value)
            return r.c.fail("json: string exceeds its bound");
         out = K{std::string(text)};
         return true;
      }
   }

   template <typename Fmt, int_mode IM, typename T, typename C>
   bool read_assoc(typed_reader<C>& r, T* out)
   {
      PSIO_REQUIRE_ENTRY_ORDER("psio::json", T);
      using K = ::psio::detail::assoc_key_t<T>;
      constexpr bool is_map = ::psio::detail::is_map_v<T>;
      if (out)
         out->clear();
      // Every key is materialised, also when only validating: duplicates
      // are those with equal key bytes.
      ::psio::detail::seen_keys seen;
      if constexpr (!is_map)
      {
         if (!r.c.begin_array())
            return false;
         for (;;)
         {
            bool has = false;
            if (!r.c.next_element(has))
               return false;
            if (!has)
               break;
            K element{};
            if (!read_value<Fmt, IM, K>(r, &element))
               return false;
            seen.add(element);
            if (out)
               out->insert(std::move(element));
         }
      }
      else
      {
         using V = typename T::mapped_type;
         // The mapped value, read in place once its key is in the container.
         auto mapped = [&](K& key) {
            if (!out)
               return read_value<Fmt, int_mode::natural, V>(r, nullptr);
            auto [at, inserted] = out->try_emplace(std::move(key));
            if (inserted)
               return read_value<Fmt, int_mode::natural, V>(r, &at->second);
            // A repeated (or merged) key: reported once the container is
            // read; its value is still checked.
            V scratch{};
            return read_value<Fmt, int_mode::natural, V>(r, &scratch);
         };
         if constexpr (text_key_v<Fmt, K>)
         {
            if (!r.c.begin_object())
               return false;
            for (;;)
            {
               bool             has = false;
               std::string_view name;
               if (!r.c.next_field(has, name))
                  return false;
               if (!has)
                  break;
               K key{};
               if (!read_key_text<Fmt>(r, name, key))
                  return false;
               seen.add(key);
               if (!mapped(key))
                  return false;
            }
         }
         else
         {
            if (!r.c.begin_array())
               return false;
            for (;;)
            {
               bool has = false;
               if (!r.c.next_element(has))
                  return false;
               if (!has)
                  break;
               K key{};
               if (!read_fixed_array(r, 2, [&](std::size_t i) {
                      if (i == 0)
                      {
                         if (!read_value<Fmt, int_mode::natural, K>(r, &key))
                            return false;
                         seen.add(key);
                         return true;
                      }
                      return mapped(key);
                   }))
                  return false;
            }
         }
      }
      const std::size_t count = seen.size();
      if (!seen.unique())
         return r.c.fail(is_map ? "json: duplicate map key" : "json: duplicate set element");
      if (out && out->size() != count)
         return r.c.fail(is_map ? "json: map container merged distinct keys"
                                : "json: set container merged distinct elements");
      return true;
   }

}  // namespace psio::detail::json_impl
