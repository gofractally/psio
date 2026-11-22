#pragma once
//
// psio/detail/assoc.hpp — std::pair, maps and sets as PSIO values: the type
// traits every format shares (docs/pssz-spec.md "Pairs, maps and sets").
//
//   std::pair<A, B>             IS std::tuple<A, B>
//   map  M<K, V>                the list of its entries (K, V), each a
//                               2-tuple, in strictly ascending key order
//   set  S<K>                   the list of its elements, strictly ascending
//
// The order is the bytewise order of the `key` encoding of K
// (docs/key-spec.md); see detail/assoc_order.hpp.
//
// Containers: std::map, std::set, std::unordered_map, std::unordered_set,
// with any comparator / hash / allocator. The multi containers are not
// values (duplicates have no canonical form).

#include <cstddef>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace psio::detail
{
   // ── std::pair ──────────────────────────────────────────────────────────
   template <typename T>
   struct is_std_pair : std::false_type
   {
   };
   template <typename A, typename B>
   struct is_std_pair<std::pair<A, B>> : std::true_type
   {
      // The tuple a pair is (a map's value_type is pair<const K, V>).
      using tuple_type = std::tuple<std::remove_const_t<A>, B>;
   };
   template <typename T>
   inline constexpr bool is_std_pair_v = is_std_pair<T>::value;

   // ── Maps and sets ──────────────────────────────────────────────────────
   //
   //   key_type      K
   //   entry_type    what one list element is on the wire: std::tuple<K, V>
   //                 for a map, K for a set
   //   list_type     std::vector<entry_type>: the type whose encoding the
   //                 container's is, in every format
   //   default_less  a std::map / std::set ordered by std::less
   template <typename T>
   struct assoc_traits
   {
      static constexpr bool is_map = false;
      static constexpr bool is_set = false;
   };
   template <typename K, typename V, typename C, typename A>
   struct assoc_traits<std::map<K, V, C, A>>
   {
      static constexpr bool is_map = true;
      static constexpr bool is_set = false;
      static constexpr bool default_less =
         std::is_same_v<C, std::less<K>> || std::is_same_v<C, std::less<>>;
      using key_type    = K;
      using mapped_type = V;
      using entry_type  = std::tuple<K, V>;
      using list_type   = std::vector<entry_type>;
   };
   template <typename K, typename V, typename H, typename E, typename A>
   struct assoc_traits<std::unordered_map<K, V, H, E, A>>
   {
      static constexpr bool is_map       = true;
      static constexpr bool is_set       = false;
      static constexpr bool default_less = false;
      using key_type    = K;
      using mapped_type = V;
      using entry_type  = std::tuple<K, V>;
      using list_type   = std::vector<entry_type>;
   };
   template <typename K, typename C, typename A>
   struct assoc_traits<std::set<K, C, A>>
   {
      static constexpr bool is_map = false;
      static constexpr bool is_set = true;
      static constexpr bool default_less =
         std::is_same_v<C, std::less<K>> || std::is_same_v<C, std::less<>>;
      using key_type   = K;
      using entry_type = K;
      using list_type  = std::vector<entry_type>;
   };
   template <typename K, typename H, typename E, typename A>
   struct assoc_traits<std::unordered_set<K, H, E, A>>
   {
      static constexpr bool is_map       = false;
      static constexpr bool is_set       = true;
      static constexpr bool default_less = false;
      using key_type   = K;
      using entry_type = K;
      using list_type  = std::vector<entry_type>;
   };

   template <typename T>
   inline constexpr bool is_map_v = assoc_traits<T>::is_map;
   template <typename T>
   inline constexpr bool is_set_v = assoc_traits<T>::is_set;
   template <typename T>
   inline constexpr bool is_assoc_v = is_map_v<T> || is_set_v<T>;

   template <typename T>
   using assoc_key_t = typename assoc_traits<T>::key_type;
   template <typename T>
   using assoc_entry_t = typename assoc_traits<T>::entry_type;
   template <typename T>
   using assoc_list_t = typename assoc_traits<T>::list_type;

   // The key of one element of the container (a map entry or a set element).
   template <typename C>
   constexpr const assoc_key_t<C>& assoc_key(const typename C::value_type& item) noexcept
   {
      if constexpr (is_map_v<C>)
         return item.first;
      else
         return item;
   }

   // ── The multi containers: not values ───────────────────────────────────
   template <typename T>
   struct is_multi_assoc : std::false_type
   {
   };
   template <typename K, typename V, typename C, typename A>
   struct is_multi_assoc<std::multimap<K, V, C, A>> : std::true_type
   {
   };
   template <typename K, typename C, typename A>
   struct is_multi_assoc<std::multiset<K, C, A>> : std::true_type
   {
   };
   template <typename K, typename V, typename H, typename E, typename A>
   struct is_multi_assoc<std::unordered_multimap<K, V, H, E, A>> : std::true_type
   {
   };
   template <typename K, typename H, typename E, typename A>
   struct is_multi_assoc<std::unordered_multiset<K, H, E, A>> : std::true_type
   {
   };
   template <typename T>
   inline constexpr bool is_multi_assoc_v = is_multi_assoc<T>::value;

   // What a static_assert names when a multi container reaches a codec.
#define PSIO_REJECT_MULTI_ASSOC(Format, T)                                                   \
   static_assert(!::psio::detail::is_multi_assoc_v<T>,                                       \
                 Format ": std::multimap / std::multiset (and their unordered forms) are "  \
                        "not PSIO values: duplicate keys have no canonical form; use "      \
                        "std::map<K, std::vector<V>> or std::vector")

}  // namespace psio::detail
