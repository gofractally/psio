#pragma once
//
// psio/detail/assoc_order.hpp — the canonical order of a map's entries and
// a set's elements.
//
// One rule, every format and language: ascending bytewise order of the
// `key` encoding of the key (docs/key-spec.md). Two entries whose keys have
// equal key bytes are duplicates. `canonical_order<C>` yields a container's
// elements in that order whatever the container's own iteration order is
// (a hash container, a custom comparator), and says whether the keys are
// unique.
//
// For a std::map / std::set with the default comparator whose key type's
// `<` is known to coincide with key order (`key_order_is_less<K>()`), the
// container already iterates in canonical order and holds no duplicates:
// it is walked in place. That is an optimisation only; results are
// identical.

#include <psio/detail/assoc.hpp>
#include <psio/detail/variant_util.hpp>
#include <psio/adapter.hpp>
#include <psio/stream.hpp>
#include <psio/time.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio
{
   struct key;

   namespace detail::key_impl
   {
      // key.hpp
      template <typename T, typename S>
      void encode_value(const T& v, S& s);
   }  // namespace detail::key_impl

   namespace detail
   {
      // Appends the key encoding of `value`.
      template <typename K>
      void append_key_bytes(const K& value, std::vector<char>& out)
      {
         ::psio::vector_stream s{out};
         key_impl::encode_value(value, s);
      }

      // Bytewise (unsigned) lexicographic comparison.
      inline int compare_key_bytes(const char* a, std::size_t an, const char* b,
                                   std::size_t bn) noexcept
      {
         const std::size_t n = an < bn ? an : bn;
         if (n)
            if (const int c = std::memcmp(a, b, n))
               return c;
         return an < bn ? -1 : an > bn ? 1 : 0;
      }

      template <typename T>
      struct order_sequence : std::false_type
      {
      };
      template <typename E>
      struct order_sequence<std::optional<E>> : std::true_type
      {
         using element = E;
      };
      template <typename E, typename A>
      struct order_sequence<std::vector<E, A>> : std::true_type
      {
         using element = E;
      };
      template <typename E, std::size_t N>
      struct order_sequence<std::array<E, N>> : std::true_type
      {
         using element = E;
      };
      template <typename T>
      struct order_tuple : std::false_type
      {
      };
      template <typename... Ts>
      struct order_tuple<std::tuple<Ts...>> : std::true_type
      {
      };
      template <typename A, typename B>
      struct order_tuple<std::pair<A, B>> : std::true_type
      {
      };

      // Whether `a < b` on K is the order of K's key bytes, and `a == b`
      // their equality: integers, enums, std::string, time values, and
      // array / vector / optional / tuple / pair / variant of such. Not
      // floats (-0 and +0 are one key, NaNs are ordered), plain `char`
      // (its `<` is signed on some targets, its key byte is not), 128-bit
      // and wider integers, records (their `<` is user code) or anything
      // with a key adapter.
      template <typename K>
      consteval bool key_order_is_less() noexcept
      {
         using U = std::remove_cv_t<K>;
         if constexpr (::psio::format_should_dispatch_adapter_v<::psio::key, U>)
            return false;
         else if constexpr (std::is_same_v<U, char>)
            return false;
         else if constexpr (std::is_enum_v<U>)
            return !std::is_same_v<std::underlying_type_t<U>, char>;
         else if constexpr (std::is_integral_v<U>)
            return sizeof(U) <= 8;
         else if constexpr (::psio::TimeValue<U>)
            return key_order_is_less<::psio::time_rep_t<U>>();
         else if constexpr (std::is_same_v<U, std::string>)
            return true;
         else if constexpr (order_sequence<U>::value)
            return key_order_is_less<typename order_sequence<U>::element>();
         else if constexpr (is_std_variant<U>::value)
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               return (key_order_is_less<std::variant_alternative_t<Is, U>>() && ...);
            }(std::make_index_sequence<std::variant_size_v<U>>{});
         else if constexpr (order_tuple<U>::value)
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               return (key_order_is_less<std::tuple_element_t<Is, U>>() && ...);
            }(std::make_index_sequence<std::tuple_size_v<U>>{});
         else
            return false;
      }

      // A container that iterates in canonical order by construction.
      template <typename C>
      inline constexpr bool iterates_in_key_order_v =
         assoc_traits<C>::default_less && key_order_is_less<assoc_key_t<C>>();

      // The elements of map / set `C` in canonical order.
      template <typename C>
      class canonical_order
      {
         using item_type = typename C::value_type;
         struct slot
         {
            const item_type* item;
            std::size_t      begin;  // key bytes: [begin, end) of keys_
            std::size_t      end;
         };
         const C&          container_;
         std::vector<char> keys_;
         std::vector<slot> slots_;
         bool              unique_ = true;

         int compare(const slot& a, const slot& b) const noexcept
         {
            return compare_key_bytes(keys_.data() + a.begin, a.end - a.begin,
                                     keys_.data() + b.begin, b.end - b.begin);
         }

        public:
         explicit canonical_order(const C& container) : container_(container)
         {
            if constexpr (!iterates_in_key_order_v<C>)
            {
               slots_.reserve(container.size());
               for (const item_type& item : container)
               {
                  const std::size_t begin = keys_.size();
                  append_key_bytes(assoc_key<C>(item), keys_);
                  slots_.push_back(slot{&item, begin, keys_.size()});
               }
               std::sort(slots_.begin(), slots_.end(),
                         [&](const slot& a, const slot& b) { return compare(a, b) < 0; });
               for (std::size_t i = 1; i < slots_.size() && unique_; ++i)
                  unique_ = compare(slots_[i - 1], slots_[i]) != 0;
            }
         }
         canonical_order(const canonical_order&)            = delete;
         canonical_order& operator=(const canonical_order&) = delete;

         // No two elements have equal key bytes.
         bool        unique() const noexcept { return unique_; }
         std::size_t size() const noexcept { return container_.size(); }

         // f(const value_type&) for every element, ascending.
         template <typename F>
         void for_each(F&& f) const
         {
            if constexpr (iterates_in_key_order_v<C>)
               for (const item_type& item : container_)
                  f(item);
            else
               for (const slot& s : slots_)
                  f(*s.item);
         }
      };

      // Key bytes seen by a reader that accepts any order (text): `add`
      // every key, then `unique()`.
      class seen_keys
      {
         struct slot
         {
            std::size_t begin;
            std::size_t end;
         };
         std::vector<char> keys_;
         std::vector<slot> slots_;

        public:
         template <typename K>
         void add(const K& key)
         {
            const std::size_t begin = keys_.size();
            append_key_bytes(key, keys_);
            slots_.push_back(slot{begin, keys_.size()});
         }
         std::size_t size() const noexcept { return slots_.size(); }
         bool        unique()
         {
            auto compare = [&](const slot& a, const slot& b) {
               return compare_key_bytes(keys_.data() + a.begin, a.end - a.begin,
                                        keys_.data() + b.begin, b.end - b.begin);
            };
            std::sort(slots_.begin(), slots_.end(),
                      [&](const slot& a, const slot& b) { return compare(a, b) < 0; });
            for (std::size_t i = 1; i < slots_.size(); ++i)
               if (compare(slots_[i - 1], slots_[i]) == 0)
                  return false;
            return true;
         }
      };

   }  // namespace detail
}  // namespace psio
