#pragma once
//
// psio/flatbuf.hpp — FlatBuffers, the native wire format.
//
// `psio::flatbuf` reads and writes the encoding the official implementation
// uses (https://flatbuffers.dev/internals/): a root offset, tables found
// through vtables, inline structs, strings, vectors, unions, optional
// scalars, a file identifier, a size prefix. No dependency on the
// FlatBuffers library. A buffer written here is read by code `flatc`
// generates for the matching schema, and the reverse
// (tests/flatbuf_interop_tests.cpp); for the shapes tested there the bytes
// are the ones the official object API (`T::Pack`) writes.
//
//   auto bytes = psio::to_flatbuf(order);                 // std::vector<char>
//   auto st    = psio::validate_flatbuf<Order>(bytes);    // the verifier
//   auto back  = psio::from_flatbuf<Order>(bytes);        // validates, then decodes
//   psio::flatbuf_view<Order> v{bytes};                   // psio/flatbuf_view.hpp
//
// How a C++ type maps (docs/flatbuffers.md has the table and the reasons):
//
//   bool, integers, float, double      bool, byte … ulong, float, double
//   enum                               an enum of its underlying type
//   std::string                        string
//   std::vector<T>                     [T]: scalars, structs, strings, tables
//   std::array<T, N>                   in a struct: a fixed array [T:N];
//                                      in a table: a vector of exactly N
//   reflected record                   table
//   reflected record that is           struct (fixed size, inline, no vtable)
//     definitionWillNotChange() and
//     holds only scalars / arrays /
//     such records
//   std::optional<scalar>              an optional scalar (`= null`)
//   std::optional<anything else>       the field; absent is "no value"
//   std::variant<A, B, ...>            union of tables A, B, …; a leading
//                                      std::monostate is NONE
//   std::set<K>                        [K], sorted (K: scalar or string)
//   std::map<K, V>                     [Entry], sorted by key, where
//                                      table Entry { key:K (key); value:V; }
//
// Field ids. The members of a table take vtable slots in ascending order of
// their field numbers (declaration position unless `attr(member, field<N>)`
// says otherwise — psio/detail/field_number.hpp). A union takes two
// consecutive slots (type, then value), as in a `.fbs` file. A member
// annotated `deprecated` keeps its slot and is neither written nor read.
// Struct members are laid out in declaration order with natural alignment.
//
// Defaults. A scalar member's default is its member initialiser: a value
// equal to it is not written and an absent field reads as it. (Floats are
// compared by bits, so -0.0 is written where the default is 0.0.) A string,
// vector, set or map that is empty is not written; a nested table always
// is. An engaged std::optional is always written.
//
// Validation is typed and safe on arbitrary bytes: `validate_flatbuf<T>`
// follows everything a T can reach and checks bounds, alignment, vtables,
// field offsets against the table's size, string terminators, vector
// lengths, union types, the order of set / map keys — under a depth limit
// and a table budget (the official verifier's defaults) and a payload
// budget (`flatbuf::limits`). Offsets are unsigned and point forward, so a
// buffer cannot contain a cycle; the budgets bound the work a buffer that
// shares sub-objects can cause, and the size of what it decodes to: tables,
// strings and vectors are counted each time they are reached.
//
// Not supported: vectors of unions, unions of structs or strings, nested
// vectors without a wrapping table (FlatBuffers has none either), shared
// strings on write, `nested_flatbuffer`, `force_align`, 64-bit offsets,
// flexbuffers.

#include <psio/adapter.hpp>
#include <psio/annotate.hpp>
#include <psio/cpo.hpp>
#include <psio/detail/field_number.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/reflect.hpp>
#include <psio/validate_strict_walker.hpp>
#include <psio/value_contract.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio {

   struct flatbuf;

   namespace detail::flatbuf_impl {

      static_assert(std::endian::native == std::endian::little,
                    "psio::flatbuf reads and writes little-endian values in place");

      // The largest buffer the format allows (offsets are 32 bits, and a
      // signed offset must reach every vtable).
      inline constexpr std::size_t max_buffer_size = 0x7fffffff;

      // ── Shapes ────────────────────────────────────────────────────────

      template <typename T>
      inline constexpr bool is_vector_v = false;
      template <typename E, typename A>
      inline constexpr bool is_vector_v<std::vector<E, A>> = true;

      template <typename T>
      inline constexpr bool is_array_v = false;
      template <typename E, std::size_t N>
      inline constexpr bool is_array_v<std::array<E, N>> = true;

      template <typename T>
      inline constexpr bool is_optional_v = false;
      template <typename E>
      inline constexpr bool is_optional_v<std::optional<E>> = true;

      template <typename T>
      inline constexpr bool is_variant_v = false;
      template <typename... Ts>
      inline constexpr bool is_variant_v<std::variant<Ts...>> = true;

      // Sorted containers. Only the default order is the order FlatBuffers
      // key lookup assumes, so only std::less is accepted.
      template <typename T>
      inline constexpr bool is_set_v = false;
      template <typename K, typename A>
      inline constexpr bool is_set_v<std::set<K, std::less<K>, A>> = true;

      template <typename T>
      inline constexpr bool is_map_v = false;
      template <typename K, typename V, typename A>
      inline constexpr bool is_map_v<std::map<K, V, std::less<K>, A>> = true;

      template <typename T>
      concept Record = ::psio::Reflected<T> && std::is_class_v<T>;

      // A value stored inline in 1, 2, 4 or 8 bytes.
      template <typename T>
      concept Scalar = (std::is_integral_v<T> || std::is_same_v<T, float> ||
                        std::is_same_v<T, double> || std::is_enum_v<T>) &&
                       sizeof(T) <= 8;

      template <typename T>
      struct array_traits;
      template <typename E, std::size_t N>
      struct array_traits<std::array<E, N>>
      {
         using element                      = E;
         static constexpr std::size_t count = N;
      };

      // A value of fixed size that may be a struct member: a scalar, a
      // fixed array of such, a struct.
      template <typename T>
      consteval bool fixed_check();

      template <typename T>
      inline constexpr bool is_fixed_v = fixed_check<T>();

      template <typename T>
      consteval bool struct_check()
      {
         if constexpr (!Record<T>)
            return false;
         else if constexpr (!::psio::is_dwnc_v<T> || ::psio::reflect<T>::member_count == 0)
            return false;
         else
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               return (fixed_check<std::remove_cvref_t<
                          typename ::psio::reflect<T>::template member_type<Is>>>() &&
                       ...);
            }(std::make_index_sequence<::psio::reflect<T>::member_count>{});
      }

      template <typename T>
      consteval bool fixed_check()
      {
         if constexpr (Scalar<T>)
            return true;
         else if constexpr (is_array_v<T>)
            return array_traits<T>::count > 0 &&
                   !is_array_v<typename array_traits<T>::element> &&
                   fixed_check<typename array_traits<T>::element>();
         else
            return struct_check<T>();
      }

      // A FlatBuffers struct: fixed size, stored inline, no vtable.
      template <typename T>
      inline constexpr bool is_struct_v = struct_check<T>();

      template <typename T>
      concept Struct = is_struct_v<T>;

      template <typename T>
      concept Table = Record<T> && !is_struct_v<T>;

      // ── Struct layout ─────────────────────────────────────────────────

      template <typename F>
      consteval std::size_t fixed_size();
      template <typename F>
      consteval std::size_t fixed_align();

      template <std::size_t N>
      struct struct_layout_data
      {
         std::array<std::uint32_t, N> offset{};
         std::uint32_t                size  = 0;
         std::uint32_t                align = 1;
      };

      // Members in declaration order, each at its natural alignment; the
      // size is rounded up to the largest alignment. (What `flatc` computes
      // for a struct without force_align.)
      template <Struct T>
      struct struct_layout
      {
         using R                            = ::psio::reflect<T>;
         static constexpr std::size_t count = R::member_count;
         template <std::size_t I>
         using member = std::remove_cvref_t<typename R::template member_type<I>>;

         static constexpr struct_layout_data<count> value = [] {
            struct_layout_data<count> r{};
            std::uint32_t             pos = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (([&] {
                   const auto a = static_cast<std::uint32_t>(fixed_align<member<Is>>());
                   pos          = (pos + a - 1) & ~(a - 1);
                   r.offset[Is] = pos;
                   pos += static_cast<std::uint32_t>(fixed_size<member<Is>>());
                   if (a > r.align)
                      r.align = a;
                }()),
                ...);
            }(std::make_index_sequence<count>{});
            r.size = (pos + r.align - 1) & ~(r.align - 1);
            return r;
         }();

         static constexpr std::size_t size  = value.size;
         static constexpr std::size_t align = value.align;
         static constexpr std::size_t offset(std::size_t i) noexcept { return value.offset[i]; }
      };

      template <typename F>
      consteval std::size_t fixed_size()
      {
         if constexpr (Scalar<F>)
            return sizeof(F);
         else if constexpr (is_array_v<F>)
            return array_traits<F>::count * fixed_size<typename array_traits<F>::element>();
         else
            return struct_layout<F>::size;
      }
      template <typename F>
      consteval std::size_t fixed_align()
      {
         if constexpr (Scalar<F>)
            return sizeof(F);
         else if constexpr (is_array_v<F>)
            return fixed_align<typename array_traits<F>::element>();
         else
            return struct_layout<F>::align;
      }

      // ── Members of a table ────────────────────────────────────────────

      // An element of a vector.
      template <typename E>
      concept Element = Scalar<E> || Struct<E> || std::is_same_v<E, std::string> || Table<E>;

      template <typename K>
      concept Key = Scalar<K> || std::is_same_v<K, std::string>;

      template <typename V>
      struct variant_traits;
      template <typename... Ts>
      struct variant_traits<std::variant<Ts...>>
      {
         static constexpr std::size_t count = sizeof...(Ts);
         // A leading std::monostate is the union's NONE.
         static constexpr bool has_none =
            std::is_same_v<std::variant_alternative_t<0, std::variant<Ts...>>, std::monostate>;
         static constexpr bool valid = [] {
            std::size_t i  = 0;
            bool        ok = true;
            ((ok = ok && ((i == 0 && std::is_same_v<Ts, std::monostate>) || Table<Ts>), ++i), ...);
            return ok;
         }();
      };

      // Stored inline in the table: a scalar or a struct.
      template <typename F>
      inline constexpr bool is_inline_v = Scalar<F> || is_struct_v<F>;
      template <typename F>
      inline constexpr bool is_inline_v<std::optional<F>> = Scalar<F> || is_struct_v<F>;

      template <typename F>
      consteval bool member_supported();

      template <typename F>
      consteval bool offset_supported()
      {
         if constexpr (std::is_same_v<F, std::string>)
            return true;
         else if constexpr (is_vector_v<F>)
            return Element<typename F::value_type>;
         else if constexpr (is_array_v<F>)
            return array_traits<F>::count > 0 && Element<typename array_traits<F>::element>;
         else if constexpr (is_set_v<F>)
            return Key<typename F::key_type>;
         else if constexpr (is_map_v<F>)
            return Key<typename F::key_type> && !is_variant_v<typename F::mapped_type> &&
                   member_supported<typename F::mapped_type>();
         else
            return Table<F>;
      }

      template <typename F>
      consteval bool member_supported()
      {
         if constexpr (Scalar<F> || is_struct_v<F>)
            return true;
         else if constexpr (is_optional_v<F>)
         {
            using X = typename F::value_type;
            if constexpr (Scalar<X> || is_struct_v<X>)
               return true;
            else if constexpr (is_optional_v<X> || is_variant_v<X>)
               return false;
            else
               return offset_supported<X>();
         }
         else if constexpr (is_variant_v<F>)
            return variant_traits<F>::valid;
         else
            return offset_supported<F>();
      }

      // The bytes a present member occupies in its table.
      template <typename F>
      consteval std::size_t inline_size()
      {
         if constexpr (is_optional_v<F>)
            return inline_size<typename F::value_type>();
         else if constexpr (Scalar<F>)
            return sizeof(F);
         else if constexpr (is_struct_v<F>)
            return struct_layout<F>::size;
         else
            return 4;
      }
      template <typename F>
      consteval std::size_t inline_align()
      {
         if constexpr (is_optional_v<F>)
            return inline_align<typename F::value_type>();
         else if constexpr (Scalar<F>)
            return sizeof(F);
         else if constexpr (is_struct_v<F>)
            return struct_layout<F>::align;
         else
            return 4;
      }
      // The size `flatc` sorts a field by when it orders the fields of a
      // table (a struct counts as 4 whatever its alignment).
      template <typename F>
      consteval std::uint8_t sort_size()
      {
         if constexpr (is_optional_v<F>)
            return sort_size<typename F::value_type>();
         else if constexpr (Scalar<F>)
            return static_cast<std::uint8_t>(sizeof(F));
         else
            return 4;
      }

      // ── Defaults ──────────────────────────────────────────────────────

      template <typename T, std::size_t I>
      constexpr auto member_default_constexpr()
      {
         T obj{};
         return obj.*(::psio::reflect<T>::template member_pointer<I>);
      }
      template <typename T, std::size_t I>
      concept constexpr_default =
         requires { typename std::bool_constant<(member_default_constexpr<T, I>(), true)>; };

      // The member initialiser of scalar member I: a constant where T{} is
      // a constant expression (the usual case), else computed once.
      template <typename T, std::size_t I>
      [[gnu::always_inline]] inline auto member_default() noexcept
      {
         using F = std::remove_cvref_t<typename ::psio::reflect<T>::template member_type<I>>;
         if constexpr (constexpr_default<T, I>)
         {
            constexpr F value = member_default_constexpr<T, I>();
            return value;
         }
         else
         {
            static const F value = [] {
               T obj{};
               return obj.*(::psio::reflect<T>::template member_pointer<I>);
            }();
            return value;
         }
      }

      template <typename T, std::size_t I>
      inline constexpr bool member_deprecated_v = [] {
         using R          = ::psio::reflect<T>;
         using FieldT     = typename R::template member_type<I>;
         constexpr auto P = R::template member_pointer<I>;
         return ::psio::find_spec<::psio::deprecated_spec>(
                   ::psio::effective_annotations_for_v<T, FieldT, P>)
            .has_value();
      }();

      // ── Table definitions ─────────────────────────────────────────────
      //
      // What the codec needs to know about a table: its members, their
      // vtable slots, how to reach them in the C++ object. `record_def<T>`
      // is a reflected record; `entry_def<K, V>` is the two-field table of
      // a std::map entry.

      template <std::size_t N>
      struct slot_data
      {
         std::array<std::uint16_t, N> slot{};     // by member index
         std::array<std::uint16_t, N> by_rank{};  // member index by ascending slot
         std::uint16_t                total = 0;
      };

      template <Table T>
      struct record_def
      {
         using R                            = ::psio::reflect<T>;
         static constexpr std::size_t count = R::member_count;
         template <std::size_t I>
         using member = std::remove_cvref_t<typename R::template member_type<I>>;

         static constexpr slot_data<count> slots_ = [] {
            slot_data<count>                r{};
            std::array<std::uint8_t, count> width{};
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((width[Is] = is_variant_v<member<Is>> ? 2 : 1), ...);
            }(std::make_index_sequence<count>{});
            const auto&   order = ::psio::detail::members_by_field_number<T>;
            std::uint32_t next  = 0;
            for (std::size_t rank = 0; rank < count; ++rank)
            {
               const std::size_t m = order[rank];
               r.slot[m]           = static_cast<std::uint16_t>(next);
               r.by_rank[rank]     = static_cast<std::uint16_t>(m);
               next += width[m];
            }
            r.total = static_cast<std::uint16_t>(next);
            return r;
         }();
         static constexpr std::size_t slots = slots_.total;
         static_assert(slots < 0x7ffe, "psio::flatbuf: a table has at most 32765 vtable slots");

         static constexpr std::uint16_t slot(std::size_t i) noexcept { return slots_.slot[i]; }
         static constexpr std::size_t   by_rank(std::size_t r) noexcept { return slots_.by_rank[r]; }

         template <std::size_t I>
         static constexpr bool deprecated = member_deprecated_v<T, I>;
         // An empty string / vector member is not written.
         template <std::size_t I>
         static constexpr bool keep_empty = false;
         template <std::size_t I>
         static constexpr bool required = false;

         template <std::size_t I>
         [[gnu::always_inline]] static const member<I>& get(const T& v) noexcept
         {
            return v.*(R::template member_pointer<I>);
         }
         template <std::size_t I>
         [[gnu::always_inline]] static member<I>& get(T& v) noexcept
         {
            return v.*(R::template member_pointer<I>);
         }
         template <std::size_t I>
         [[gnu::always_inline]] static member<I> default_of() noexcept
         {
            return member_default<T, I>();
         }
      };

      template <typename K, typename V>
      struct entry_def
      {
         static constexpr std::size_t count = 2;
         template <std::size_t I>
         using member                       = std::conditional_t<I == 0, K, V>;
         static constexpr std::size_t slots = 2;
         static constexpr std::uint16_t slot(std::size_t i) noexcept
         {
            return static_cast<std::uint16_t>(i);
         }
         static constexpr std::size_t by_rank(std::size_t r) noexcept { return r; }
         template <std::size_t I>
         static constexpr bool deprecated = false;
         // The key is `(key)` in the schema, which makes a string key a
         // required field: it is written even when empty.
         template <std::size_t I>
         static constexpr bool keep_empty = I == 0;
         template <std::size_t I>
         static constexpr bool required = I == 0 && std::is_same_v<K, std::string>;

         template <std::size_t I, typename P>
         [[gnu::always_inline]] static auto& get(P& entry) noexcept
         {
            if constexpr (I == 0)
               return entry.first;
            else
               return entry.second;
         }
         template <std::size_t I>
         [[gnu::always_inline]] static member<I> default_of() noexcept
         {
            return member<I>{};
         }
      };

      // The vtable position of slot s.
      constexpr std::uint16_t vt_of(std::size_t slot) noexcept
      {
         return static_cast<std::uint16_t>(4 + 2 * slot);
      }

      template <typename Def>
      consteval bool def_supported()
      {
         return []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (member_supported<typename Def::template member<Is>>() && ...);
         }(std::make_index_sequence<Def::count>{});
      }

      // The order in which the fields of a table are written, which is
      // `flatc`'s: by size, largest first, and within a size by descending
      // field id. One entry per vtable slot; a union is two (its type byte,
      // its value offset).
      struct add_entry
      {
         std::uint16_t slot   = 0;
         std::uint16_t member = 0;
         std::uint8_t  size   = 0;
         std::uint8_t  part   = 0;
      };

      template <typename Def>
      inline constexpr std::array<add_entry, Def::slots> add_order = [] {
         std::array<add_entry, Def::slots> e{};
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (([&] {
                using F               = typename Def::template member<Is>;
                const std::uint16_t s = Def::slot(Is);
                if constexpr (is_variant_v<F>)
                {
                   e[s]     = add_entry{s, static_cast<std::uint16_t>(Is), 1, 0};
                   e[s + 1] = add_entry{static_cast<std::uint16_t>(s + 1),
                                        static_cast<std::uint16_t>(Is), 4, 1};
                }
                else
                   e[s] = add_entry{s, static_cast<std::uint16_t>(Is), sort_size<F>(), 0};
             }()),
             ...);
         }(std::make_index_sequence<Def::count>{});
         for (std::size_t i = 1; i < e.size(); ++i)
            for (std::size_t j = i; j > 0; --j)
            {
               const bool before = e[j].size > e[j - 1].size ||
                                   (e[j].size == e[j - 1].size && e[j].slot > e[j - 1].slot);
               if (!before)
                  break;
               const auto t = e[j];
               e[j]         = e[j - 1];
               e[j - 1]     = t;
            }
         return e;
      }();

      // ── Wire primitives ───────────────────────────────────────────────

      template <typename T>
      [[gnu::always_inline]] inline T load(const std::uint8_t* p) noexcept
      {
         T v;
         std::memcpy(static_cast<void*>(&v), p, sizeof(T));
         return v;
      }
      template <typename T>
      [[gnu::always_inline]] inline void store(std::uint8_t* p, T v) noexcept
      {
         std::memcpy(p, static_cast<const void*>(&v), sizeof(T));
      }

      template <Scalar S>
      [[gnu::always_inline]] inline S load_scalar(const std::uint8_t* p) noexcept
      {
         if constexpr (std::is_same_v<S, bool>)
            return *p != 0;
         else
            return load<S>(p);
      }
      template <Scalar S>
      [[gnu::always_inline]] inline void store_scalar(std::uint8_t* p, S v) noexcept
      {
         if constexpr (std::is_same_v<S, bool>)
            *p = v ? 1 : 0;
         else
            store<S>(p, v);
      }

      // Whether a scalar equals its default, i.e. is not written. Floats by
      // bits: -0.0 and a NaN of another payload are values of their own.
      template <Scalar S>
      [[gnu::always_inline]] inline bool same_scalar(S a, S b) noexcept
      {
         if constexpr (std::is_same_v<S, float>)
            return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
         else if constexpr (std::is_same_v<S, double>)
            return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
         else
            return a == b;
      }

      // ── Structs ───────────────────────────────────────────────────────

      template <typename F>
      inline void write_fixed(std::uint8_t* dst, const F& v) noexcept
      {
         if constexpr (Scalar<F>)
            store_scalar<F>(dst, v);
         else if constexpr (is_array_v<F>)
         {
            using E                    = typename array_traits<F>::element;
            constexpr std::size_t step = fixed_size<E>();
            if constexpr (Scalar<E> && !std::is_same_v<E, bool>)
               std::memcpy(dst, static_cast<const void*>(v.data()), step * v.size());
            else
               for (std::size_t i = 0; i < v.size(); ++i)
                  write_fixed<E>(dst + i * step, v[i]);
         }
         else
         {
            using L = struct_layout<F>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (write_fixed<typename L::template member<Is>>(
                   dst + L::offset(Is), v.*(::psio::reflect<F>::template member_pointer<Is>)),
                ...);
            }(std::make_index_sequence<L::count>{});
         }
      }

      template <typename F>
      inline void read_fixed(const std::uint8_t* src, F& out) noexcept
      {
         if constexpr (Scalar<F>)
            out = load_scalar<F>(src);
         else if constexpr (is_array_v<F>)
         {
            using E                    = typename array_traits<F>::element;
            constexpr std::size_t step = fixed_size<E>();
            if constexpr (Scalar<E> && !std::is_same_v<E, bool>)
               std::memcpy(static_cast<void*>(out.data()), src, step * out.size());
            else
               for (std::size_t i = 0; i < out.size(); ++i)
                  read_fixed<E>(src + i * step, out[i]);
         }
         else
         {
            using L = struct_layout<F>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (read_fixed<typename L::template member<Is>>(
                   src + L::offset(Is), out.*(::psio::reflect<F>::template member_pointer<Is>)),
                ...);
            }(std::make_index_sequence<L::count>{});
         }
      }

      // ── Encode ────────────────────────────────────────────────────────

      struct write_options
      {
         // Four characters, or empty for none.
         std::string_view file_identifier{};
         // Prefix the buffer with its size (a stream of buffers).
         bool size_prefixed = false;
      };

      // Back to front: writes grow toward the front of the storage, and an
      // object is named by its distance from the end, which does not change
      // when the storage grows. The steps are the official builder's, so
      // that the bytes are.
      class builder
      {
         static constexpr std::size_t local_bytes = 1024;
         static constexpr std::size_t local_vts   = 16;

         alignas(16) std::uint8_t        local_[local_bytes];
         std::uint8_t*                   buf_;
         std::unique_ptr<std::uint8_t[]> heap_;
         std::size_t                     cap_;
         std::size_t                     head_;
         std::size_t                     min_align_ = 1;
         std::uint32_t                   tbl_start_ = 0;
         // The vtables written so far, for sharing.
         std::uint32_t              vt_local_[local_vts];
         std::size_t                vt_count_ = 0;
         std::vector<std::uint32_t> vt_more_;
         // Offsets of the elements of the vectors under construction: a
         // stack, inline until it outgrows `local_scratch`.
         static constexpr std::size_t     local_scratch = 128;
         std::uint32_t                    scratch_local_[local_scratch];
         std::unique_ptr<std::uint32_t[]> scratch_heap_;
         std::uint32_t*                   scratch_      = scratch_local_;
         std::size_t                      scratch_size_ = 0;
         std::size_t                      scratch_cap_  = local_scratch;

         void grow_scratch()
         {
            const std::size_t                cap = scratch_cap_ * 2;
            std::unique_ptr<std::uint32_t[]> nb(new std::uint32_t[cap]);
            std::memcpy(nb.get(), scratch_, scratch_size_ * sizeof(std::uint32_t));
            scratch_heap_ = std::move(nb);
            scratch_      = scratch_heap_.get();
            scratch_cap_  = cap;
         }

         void grow(std::size_t needed)
         {
            const std::size_t tail = sz();
            if (needed > max_buffer_size - tail)
               ::psio::codec_abort(
                  codec_error{"flatbuf: the buffer would exceed 2 GiB", 0, "flatbuf"});
            const std::size_t               new_cap = std::max(cap_ * 2, cap_ + needed);
            std::unique_ptr<std::uint8_t[]> nb(new std::uint8_t[new_cap]);
            std::memcpy(nb.get() + new_cap - tail, buf_ + head_, tail);
            heap_ = std::move(nb);
            buf_  = heap_.get();
            cap_  = new_cap;
            head_ = new_cap - tail;
         }

       public:
         builder() noexcept : buf_(local_), cap_(local_bytes), head_(local_bytes) {}
         builder(const builder&)            = delete;
         builder& operator=(const builder&) = delete;

         [[gnu::always_inline]] std::uint32_t sz() const noexcept
         {
            return static_cast<std::uint32_t>(cap_ - head_);
         }

         [[gnu::always_inline]] std::uint8_t* alloc(std::size_t n)
         {
            if (n > head_) [[unlikely]]
               grow(n);
            head_ -= n;
            return buf_ + head_;
         }
         [[gnu::always_inline]] void zero_pad(std::size_t n) { std::memset(alloc(n), 0, n); }
         [[gnu::always_inline]] void track(std::size_t a) noexcept
         {
            if (a > min_align_)
               min_align_ = a;
         }
         [[gnu::always_inline]] void align(std::size_t a)
         {
            const std::size_t p = (~std::size_t{sz()} + 1) & (a - 1);
            if (p)
               zero_pad(p);
            track(a);
         }
         // Pad so that, once `len` more bytes are written, the size is a
         // multiple of `a`.
         [[gnu::always_inline]] void pre_align(std::size_t len, std::size_t a)
         {
            if (!len)
               return;
            track(a);
            const std::size_t p = (~(std::size_t{sz()} + len) + 1) & (a - 1);
            if (p)
               zero_pad(p);
         }
         template <typename T>
         [[gnu::always_inline]] void push(T v)
         {
            store<T>(alloc(sizeof(T)), v);
         }

         std::uint32_t create_string(const char* s, std::size_t len)
         {
            pre_align(len + 1, 4);
            *alloc(1) = 0;
            if (len)
               std::memcpy(alloc(len), s, len);
            align(4);
            push(static_cast<std::uint32_t>(len));
            return sz();
         }

         // A vector whose `count` elements of `elem_size` bytes the caller
         // writes into the returned storage.
         std::uint8_t* start_vector(std::size_t count, std::size_t elem_size, std::size_t elem_align)
         {
            const std::size_t body = count * elem_size;
            pre_align(body, 4);
            pre_align(body, elem_align);
            return alloc(body);
         }
         std::uint32_t end_vector(std::size_t count)
         {
            align(4);
            push(static_cast<std::uint32_t>(count));
            return sz();
         }

         // A vector of offsets to the objects `scratch_[base ..)`.
         std::uint32_t create_offset_vector(std::size_t base)
         {
            const std::size_t count = scratch_size_ - base;
            pre_align(count * 4, 4);
            for (std::size_t i = count; i > 0;)
            {
               --i;
               align(4);
               const std::uint32_t at = sz() + 4;
               push(static_cast<std::uint32_t>(at - scratch_[base + i]));
            }
            scratch_size_ = base;
            return end_vector(count);
         }
         [[nodiscard]] std::size_t scratch_mark() const noexcept { return scratch_size_; }
         [[gnu::always_inline]] void scratch_push(std::uint32_t off)
         {
            if (scratch_size_ == scratch_cap_) [[unlikely]]
               grow_scratch();
            scratch_[scratch_size_++] = off;
         }

         [[gnu::always_inline]] void start_table() noexcept { tbl_start_ = sz(); }

         template <Scalar S>
         [[gnu::always_inline]] std::uint32_t add_scalar(S v)
         {
            align(sizeof(S));
            store_scalar<S>(alloc(sizeof(S)), v);
            return sz();
         }
         [[gnu::always_inline]] std::uint32_t add_offset(std::uint32_t target)
         {
            align(4);
            const std::uint32_t at = sz() + 4;
            push(static_cast<std::uint32_t>(at - target));
            return sz();
         }
         template <Struct S>
         [[gnu::always_inline]] std::uint32_t add_struct(const S& v)
         {
            constexpr std::size_t size = struct_layout<S>::size;
            align(struct_layout<S>::align);
            auto* dst = alloc(size);
            std::memset(dst, 0, size);
            write_fixed<S>(dst, v);
            return sz();
         }

         // `at[s]`: where the field of slot s was written (its distance
         // from the end), 0 when it was not.
         std::uint32_t end_table(const std::uint32_t* at, std::size_t slots)
         {
            align(4);
            push(std::int32_t{0});
            const std::uint32_t tbl = sz();
            while (slots > 0 && at[slots - 1] == 0)
               --slots;
            const std::size_t   vt_size = 4 + 2 * slots;
            const std::uint32_t object  = tbl - tbl_start_;
            if (object > 0xffff) [[unlikely]]
               ::psio::codec_abort(
                  codec_error{"flatbuf: a table's inline data exceeds 64 KiB", 0, "flatbuf"});
            std::uint8_t* vt = alloc(vt_size);
            store<std::uint16_t>(vt, static_cast<std::uint16_t>(vt_size));
            store<std::uint16_t>(vt + 2, static_cast<std::uint16_t>(object));
            for (std::size_t s = 0; s < slots; ++s)
               store<std::uint16_t>(vt + 4 + 2 * s,
                                    at[s] ? static_cast<std::uint16_t>(tbl - at[s])
                                          : std::uint16_t{0});
            std::uint32_t vt_off = sz();
            // Share an identical vtable written earlier.
            bool                shared = false;
            const std::uint8_t* end    = buf_ + cap_;
            const auto          same   = [&](std::uint32_t off) {
               const std::uint8_t* other = end - off;
               return load<std::uint16_t>(other) == vt_size && std::memcmp(other, vt, vt_size) == 0;
            };
            const std::size_t in_local = std::min(vt_count_, local_vts);
            for (std::size_t i = 0; i < in_local && !shared; ++i)
               if (same(vt_local_[i]))
               {
                  vt_off = vt_local_[i];
                  shared = true;
               }
            for (std::size_t i = 0; i < vt_more_.size() && !shared; ++i)
               if (same(vt_more_[i]))
               {
                  vt_off = vt_more_[i];
                  shared = true;
               }
            if (shared)
               head_ += vt_size;
            else
            {
               if (vt_count_ < local_vts)
                  vt_local_[vt_count_] = vt_off;
               else
                  vt_more_.push_back(vt_off);
               ++vt_count_;
            }
            store<std::int32_t>(buf_ + cap_ - tbl,
                                static_cast<std::int32_t>(vt_off) - static_cast<std::int32_t>(tbl));
            return tbl;
         }

         void finish(std::uint32_t root, const write_options& opts, std::vector<char>& sink)
         {
            if (!opts.file_identifier.empty() && opts.file_identifier.size() != 4)
               ::psio::codec_abort(codec_error{
                  "flatbuf: a file identifier is exactly four characters", 0, "flatbuf"});
            const std::size_t prefix = opts.size_prefixed ? 4 : 0;
            track(prefix);
            pre_align(prefix + 4 + opts.file_identifier.size(), min_align_);
            if (!opts.file_identifier.empty())
               std::memcpy(alloc(4), opts.file_identifier.data(), 4);
            add_offset(root);
            if (opts.size_prefixed)
               push(static_cast<std::uint32_t>(sz()));
            const std::size_t size = sz();
            sink.resize(size);
            std::memcpy(sink.data(), buf_ + head_, size);
         }
      };

      template <typename Def, typename Obj>
      std::uint32_t write_table(builder& b, const Obj& v);

      // The out-of-line part of a value: the offset of what was written
      // (its distance from the end), or 0 for nothing. `keep_empty`: an
      // engaged optional writes its empty string / vector.
      template <typename F>
      std::uint32_t write_child(builder& b, const F& v, bool keep_empty)
      {
         if constexpr (is_inline_v<F>)
            return 0;
         else if constexpr (is_optional_v<F>)
            return v.has_value() ? write_child<typename F::value_type>(b, *v, true) : 0;
         else if constexpr (std::is_same_v<F, std::string>)
            return v.empty() && !keep_empty ? 0 : b.create_string(v.data(), v.size());
         else if constexpr (is_vector_v<F> || is_array_v<F> || is_set_v<F>)
         {
            using E               = typename F::value_type;
            const std::size_t cnt = v.size();
            if (cnt == 0 && !keep_empty && !is_array_v<F>)
               return 0;
            if constexpr (Scalar<E>)
            {
               auto* dst = b.start_vector(cnt, sizeof(E), sizeof(E));
               if constexpr (std::is_same_v<E, bool> || is_set_v<F>)
               {
                  std::size_t i = 0;
                  for (const auto& e : v)
                     store_scalar<E>(dst + (i++) * sizeof(E), static_cast<E>(e));
               }
               else if (cnt)
                  std::memcpy(dst, static_cast<const void*>(v.data()), cnt * sizeof(E));
               return b.end_vector(cnt);
            }
            else if constexpr (Struct<E>)
            {
               constexpr std::size_t step = struct_layout<E>::size;
               auto*                 dst  = b.start_vector(cnt, step, struct_layout<E>::align);
               if (cnt)
                  std::memset(dst, 0, cnt * step);
               for (std::size_t i = 0; i < cnt; ++i)
                  write_fixed<E>(dst + i * step, v[i]);
               return b.end_vector(cnt);
            }
            else
            {
               const std::size_t base = b.scratch_mark();
               for (const auto& e : v)
                  b.scratch_push(write_child<E>(b, e, true));
               return b.create_offset_vector(base);
            }
         }
         else if constexpr (is_map_v<F>)
         {
            if (v.empty() && !keep_empty)
               return 0;
            using Entry            = entry_def<typename F::key_type, typename F::mapped_type>;
            const std::size_t base = b.scratch_mark();
            for (const auto& e : v)
               b.scratch_push(write_table<Entry>(b, e));
            return b.create_offset_vector(base);
         }
         else if constexpr (is_variant_v<F>)
         {
            return std::visit(
               [&](const auto& alt) -> std::uint32_t {
                  using Alt = std::remove_cvref_t<decltype(alt)>;
                  if constexpr (std::is_same_v<Alt, std::monostate>)
                     return 0;
                  else
                     return write_table<record_def<Alt>>(b, alt);
               },
               v);
         }
         else
            return write_table<record_def<F>>(b, v);
      }

      // The union type byte of a variant: 0 is NONE.
      template <typename V>
      [[gnu::always_inline]] inline std::uint8_t union_type(const V& v) noexcept
      {
         return static_cast<std::uint8_t>(v.index() + (variant_traits<V>::has_none ? 0 : 1));
      }

      template <typename Def, std::size_t I, std::size_t Part, typename Obj>
      [[gnu::always_inline]] inline void add_member(builder& b, const Obj& obj,
                                                    const std::uint32_t* child, std::uint32_t* at)
      {
         using F = typename Def::template member<I>;
         if constexpr (Def::template deprecated<I>)
            return;
         else
         {
            const auto&           v = Def::template get<I>(obj);
            constexpr std::size_t s = Def::slot(I);
            if constexpr (Scalar<F>)
            {
               if (!same_scalar<F>(v, Def::template default_of<I>()))
                  at[s] = b.add_scalar<F>(v);
            }
            else if constexpr (is_struct_v<F>)
               at[s] = b.add_struct(v);
            else if constexpr (is_optional_v<F> && is_inline_v<F>)
            {
               using X = typename F::value_type;
               if (v.has_value())
               {
                  if constexpr (Scalar<X>)
                     at[s] = b.add_scalar<X>(*v);
                  else
                     at[s] = b.add_struct(*v);
               }
            }
            else if constexpr (is_variant_v<F>)
            {
               if constexpr (Part == 0)
               {
                  if (const std::uint8_t type = union_type(v); type != 0)
                     at[s] = b.add_scalar<std::uint8_t>(type);
               }
               else if (child[I])
                  at[s + 1] = b.add_offset(child[I]);
            }
            else if (child[I])
               at[s] = b.add_offset(child[I]);
         }
      }

      template <typename Def, typename Obj>
      std::uint32_t write_table(builder& b, const Obj& v)
      {
         static_assert(def_supported<Def>(),
                       "psio::flatbuf: a member of this record has a type FlatBuffers cannot "
                       "hold (see the mapping in psio/flatbuf.hpp)");
         constexpr std::size_t N = Def::count;
         constexpr std::size_t S = Def::slots;
         std::uint32_t         child[N ? N : 1]{};
         // Children first, in field order.
         [&]<std::size_t... Rs>(std::index_sequence<Rs...>) {
            (([&] {
                constexpr std::size_t I = Def::by_rank(Rs);
                if constexpr (!Def::template deprecated<I>)
                   child[I] = write_child<typename Def::template member<I>>(
                      b, Def::template get<I>(v), Def::template keep_empty<I>);
             }()),
             ...);
         }(std::make_index_sequence<N>{});

         b.start_table();
         std::uint32_t at[S ? S : 1]{};
         [&]<std::size_t... Js>(std::index_sequence<Js...>) {
            (add_member<Def, add_order<Def>[Js].member, add_order<Def>[Js].part>(b, v, child, at),
             ...);
         }(std::make_index_sequence<S>{});
         return b.end_table(at, S);
      }

      template <Table T>
      void encode_into(const T& v, const write_options& opts, std::vector<char>& sink)
      {
         builder             b;
         const std::uint32_t root = write_table<record_def<T>>(b, v);
         b.finish(root, opts, sink);
      }

      // ── Navigation (trusted: the bytes have been validated) ───────────

      // A table with no fields: what a view of an absent table points at,
      // so that reading a field needs no test for "no table".
      alignas(4) inline constexpr std::uint8_t empty_table_bytes[8] = {4, 0, 4, 0, 4, 0, 0, 0};
      [[gnu::always_inline]] inline const std::uint8_t* empty_table() noexcept
      {
         return empty_table_bytes + 4;
      }
      // An empty string, for the same reason.
      alignas(4) inline constexpr std::uint8_t empty_string_bytes[8] = {};

      [[gnu::always_inline]] inline const std::uint8_t* follow(const std::uint8_t* cell) noexcept
      {
         return cell + load<std::uint32_t>(cell);
      }
      // The cell of the field at vtable position `vt`, or nullptr.
      [[gnu::always_inline]] inline const std::uint8_t* field_cell(const std::uint8_t* tbl,
                                                                   std::uint16_t       vt) noexcept
      {
         const std::uint8_t* vtable = tbl - load<std::int32_t>(tbl);
         if (vt >= load<std::uint16_t>(vtable))
            return nullptr;
         const std::uint16_t off = load<std::uint16_t>(vtable + vt);
         return off ? tbl + off : nullptr;
      }
      [[gnu::always_inline]] inline std::string_view string_at(const std::uint8_t* s) noexcept
      {
         return {reinterpret_cast<const char*>(s + 4), load<std::uint32_t>(s)};
      }
      // The root table of a buffer (not size-prefixed).
      [[gnu::always_inline]] inline const std::uint8_t* root_of(std::span<const char> bytes) noexcept
      {
         return follow(reinterpret_cast<const std::uint8_t*>(bytes.data()));
      }

      // ── Decode (trusted) ──────────────────────────────────────────────

      template <typename Def, typename Obj>
      void decode_table(const std::uint8_t* tbl, Obj& out);

      // The value whose inline cell — a table field or a vector element —
      // is at `cell`.
      template <typename F>
      void decode_cell(const std::uint8_t* cell, F& out)
      {
         if constexpr (Scalar<F>)
            out = load_scalar<F>(cell);
         else if constexpr (is_struct_v<F>)
            read_fixed<F>(cell, out);
         else if constexpr (is_optional_v<F>)
            decode_cell<typename F::value_type>(cell, out.emplace());
         else if constexpr (std::is_same_v<F, std::string>)
            out.assign(string_at(follow(cell)));
         else if constexpr (is_vector_v<F> || is_array_v<F>)
         {
            using E                           = typename F::value_type;
            const std::uint8_t* const vec     = follow(cell);
            const std::size_t         written = load<std::uint32_t>(vec);
            const std::uint8_t* const data    = vec + 4;
            std::size_t               cnt     = written;
            if constexpr (is_array_v<F>)
               cnt = std::min(written, out.size());
            else
               out.resize(cnt);
            if constexpr (Scalar<E> && !std::is_same_v<E, bool>)
            {
               if (cnt)
                  std::memcpy(static_cast<void*>(out.data()), data, cnt * sizeof(E));
            }
            else if constexpr (std::is_same_v<E, bool>)
            {
               for (std::size_t i = 0; i < cnt; ++i)
                  out[i] = data[i] != 0;
            }
            else
            {
               constexpr std::size_t step = inline_size<E>();
               for (std::size_t i = 0; i < cnt; ++i)
                  decode_cell<E>(data + i * step, out[i]);
            }
         }
         else if constexpr (is_set_v<F>)
         {
            using K                        = typename F::key_type;
            const std::uint8_t* const vec  = follow(cell);
            const std::size_t         cnt  = load<std::uint32_t>(vec);
            constexpr std::size_t     step = inline_size<K>();
            for (std::size_t i = 0; i < cnt; ++i)
            {
               K key{};
               decode_cell<K>(vec + 4 + i * step, key);
               out.insert(out.end(), std::move(key));
            }
         }
         else if constexpr (is_map_v<F>)
         {
            using K                       = typename F::key_type;
            using V                       = typename F::mapped_type;
            const std::uint8_t* const vec = follow(cell);
            const std::size_t         cnt = load<std::uint32_t>(vec);
            for (std::size_t i = 0; i < cnt; ++i)
            {
               std::pair<K, V> entry{};
               decode_table<entry_def<K, V>>(follow(vec + 4 + i * 4), entry);
               out.insert(out.end(), std::move(entry));
            }
         }
         else
            decode_table<record_def<F>>(follow(cell), out);
      }

      template <typename Def, std::size_t I, typename Obj>
      [[gnu::always_inline]] inline void decode_member(const std::uint8_t* tbl, Obj& out)
      {
         using F = typename Def::template member<I>;
         if constexpr (Def::template deprecated<I>)
            return;
         else if constexpr (is_variant_v<F>)
         {
            constexpr std::size_t     s    = Def::slot(I);
            const std::uint8_t* const tc   = field_cell(tbl, vt_of(s));
            const std::uint8_t* const vc   = field_cell(tbl, vt_of(s + 1));
            const std::size_t         type = tc ? *tc : 0;
            if (type == 0 || vc == nullptr)
               return;  // NONE: the default-constructed variant
            auto&             v     = Def::template get<I>(out);
            const std::size_t which = type - (variant_traits<F>::has_none ? 0 : 1);
            [&]<std::size_t... Js>(std::index_sequence<Js...>) {
               (void)((which == Js
                          ? ([&] {
                                using Alt = std::variant_alternative_t<Js, F>;
                                if constexpr (!std::is_same_v<Alt, std::monostate>)
                                   decode_table<record_def<Alt>>(follow(vc),
                                                                 v.template emplace<Js>());
                             }(),
                             true)
                          : false) ||
                      ...);
            }(std::make_index_sequence<variant_traits<F>::count>{});
         }
         else
         {
            if (const std::uint8_t* const cell = field_cell(tbl, vt_of(Def::slot(I))))
               decode_cell<F>(cell, Def::template get<I>(out));
         }
      }

      // `out` is freshly constructed: an absent member keeps its initialiser.
      template <typename Def, typename Obj>
      void decode_table(const std::uint8_t* tbl, Obj& out)
      {
         static_assert(def_supported<Def>(),
                       "psio::flatbuf: a member of this record has a type FlatBuffers cannot "
                       "hold (see the mapping in psio/flatbuf.hpp)");
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (decode_member<Def, Is>(tbl, out), ...);
         }(std::make_index_sequence<Def::count>{});
      }

      // ── Validation (typed; safe on any bytes) ─────────────────────────

      struct limits
      {
         // Tables nested inside tables.
         std::uint32_t max_depth = 64;
         // Tables visited in total (a table reachable twice counts twice).
         std::uint32_t max_tables = 1'000'000;
         // The bytes of the strings and of the vectors of scalars / structs
         // reached (each with its 4-byte length), one reachable twice
         // counted twice, as a multiple of the buffer's size (on top of
         // `payload_allowance`, one mebibyte, that every buffer has):
         // what a decode copies. The bytes read to compare string keys of
         // a set / map (the shorter key of each neighbouring pair) are
         // charged to the same budget, so the verifier's work is bounded
         // by the buffer too. A buffer in which nothing is shared needs
         // 3; the default leaves room for the strings an official builder
         // shares (`CreateSharedString`).
         std::uint32_t max_expansion = 64;
      };
      inline constexpr std::uint64_t payload_allowance = std::uint64_t{1} << 20;

      struct checker
      {
         const std::uint8_t* buf;
         std::uint64_t       size;
         limits              lim;
         std::uint32_t       depth  = 0;
         std::uint32_t       tables = 0;
         // What is left of the payload budget (limits::max_expansion).
         std::uint64_t       payload = 0;
         const char*         error  = nullptr;
         std::uint64_t       at     = 0;

         bool fail(const char* what, std::uint64_t pos) noexcept
         {
            if (!error)
            {
               error = what;
               at    = pos;
            }
            return false;
         }
         [[nodiscard]] bool has(std::uint64_t pos, std::uint64_t n) const noexcept
         {
            return pos <= size && n <= size - pos;
         }
         // Takes `n` bytes from the payload budget.
         [[nodiscard]] bool charge(std::uint64_t n, const char* what, std::uint64_t pos) noexcept
         {
            if (n > payload)
               return fail(what, pos);
            payload -= n;
            return true;
         }
      };

      // The position the offset at `cell` (4 readable bytes) points to, or
      // 0 after recording an error.
      inline std::uint64_t check_offset(checker& c, std::uint64_t cell) noexcept
      {
         if (cell & 3)
            return c.fail("flatbuf: misaligned offset", cell), 0;
         const std::uint32_t o = load<std::uint32_t>(c.buf + cell);
         if (o == 0)
            return c.fail("flatbuf: null offset", cell), 0;
         if (o > max_buffer_size || !c.has(cell + o, 4))
            return c.fail("flatbuf: offset out of bounds", cell), 0;
         return cell + o;
      }

      inline bool check_string(checker& c, std::uint64_t pos) noexcept
      {
         if (pos & 3)
            return c.fail("flatbuf: misaligned string", pos);
         const std::uint64_t len = load<std::uint32_t>(c.buf + pos);
         if (!c.has(pos + 4, len + 1))
            return c.fail("flatbuf: string out of bounds", pos);
         if (c.buf[pos + 4 + len] != 0)
            return c.fail("flatbuf: string is not NUL-terminated", pos);
         return c.charge(4 + len, "flatbuf: too many bytes referenced", pos);
      }

      // The element count of the vector at `pos` (4 readable bytes); false
      // after recording an error. `leaf`: the elements are scalars or
      // structs, the vector's own payload (the tables and strings that
      // offsets lead to are budgeted where they are reached).
      inline bool check_vector(checker& c, std::uint64_t pos, std::uint64_t elem_size,
                               std::uint64_t elem_align, bool leaf, std::uint64_t& count) noexcept
      {
         if (pos & 3)
            return c.fail("flatbuf: misaligned vector", pos);
         count = load<std::uint32_t>(c.buf + pos);
         if (!c.has(pos + 4, count * elem_size))
            return c.fail("flatbuf: vector out of bounds", pos);
         if (count && ((pos + 4) & (elem_align - 1)))
            return c.fail("flatbuf: misaligned vector elements", pos);
         return !leaf ||
                c.charge(4 + count * elem_size, "flatbuf: too many bytes referenced", pos);
      }

      template <typename Def>
      bool check_table(checker& c, std::uint64_t pos) noexcept;

      template <typename K>
      [[gnu::always_inline]] inline auto key_at(const std::uint8_t* cell) noexcept
      {
         if constexpr (std::is_same_v<K, std::string>)
            return string_at(follow(cell));
         else
            return load_scalar<K>(cell);
      }
      // The key of the map entry whose offset is at `cell`.
      template <typename K>
      [[gnu::always_inline]] inline auto entry_key_at(const std::uint8_t* cell) noexcept
      {
         const std::uint8_t* const tbl = follow(cell);
         const std::uint8_t* const kc  = field_cell(tbl, vt_of(0));
         if constexpr (std::is_same_v<K, std::string>)
            return kc ? string_at(follow(kc)) : std::string_view{};
         else
            return kc ? load_scalar<K>(kc) : K{};
      }

      // Whether key `a` orders before key `b`, neighbours in the vector at
      // `vec`. Comparing strings reads up to the length of the shorter one:
      // that is charged first, so keys that share their bytes (one long
      // string in many sets) cannot make the verifier read more than the
      // budget.
      template <typename K, typename Key>
      [[gnu::always_inline]] inline bool keys_ascend(checker& c, const Key& a, const Key& b,
                                                     std::uint64_t vec, const char* what) noexcept
      {
         if constexpr (std::is_same_v<K, std::string>)
            if (!c.charge(std::min(a.size(), b.size()), "flatbuf: too many key bytes compared", vec))
               return false;
         return a < b ? true : c.fail(what, vec);
      }

      // The value whose inline cell is at `pos` (a table field, or a vector
      // element): its inline bytes, then what they point to.
      template <typename F>
      bool check_cell(checker& c, std::uint64_t pos) noexcept
      {
         if constexpr (is_optional_v<F>)
            return check_cell<typename F::value_type>(c, pos);
         else if constexpr (Scalar<F> || is_struct_v<F>)
         {
            if (pos & (inline_align<F>() - 1))
               return c.fail("flatbuf: misaligned field", pos);
            if (!c.has(pos, inline_size<F>()))
               return c.fail("flatbuf: field out of bounds", pos);
            return true;
         }
         else
         {
            if (!c.has(pos, 4))
               return c.fail("flatbuf: field out of bounds", pos);
            const std::uint64_t target = check_offset(c, pos);
            if (!target)
               return false;
            if constexpr (std::is_same_v<F, std::string>)
               return check_string(c, target);
            else if constexpr (is_vector_v<F> || is_array_v<F> || is_set_v<F>)
            {
               using E = typename F::value_type;
               std::uint64_t count = 0;
               if (!check_vector(c, target, inline_size<E>(), inline_align<E>(),
                                 Scalar<E> || is_struct_v<E>, count))
                  return false;
               if constexpr (is_array_v<F>)
               {
                  if (count != array_traits<F>::count)
                     return c.fail("flatbuf: a fixed-length vector has the wrong length", target);
               }
               if constexpr (!(Scalar<E> || is_struct_v<E>))
               {
                  for (std::uint64_t i = 0; i < count; ++i)
                     if (!check_cell<E>(c, target + 4 + i * 4))
                        return false;
               }
               if constexpr (is_set_v<F>)
               {
                  constexpr std::size_t step = inline_size<E>();
                  for (std::uint64_t i = 1; i < count; ++i)
                     if (!keys_ascend<E>(c, key_at<E>(c.buf + target + 4 + (i - 1) * step),
                                         key_at<E>(c.buf + target + 4 + i * step), target,
                                         "flatbuf: set keys are not in ascending order"))
                        return false;
               }
               return true;
            }
            else if constexpr (is_map_v<F>)
            {
               using K = typename F::key_type;
               using V = typename F::mapped_type;
               std::uint64_t count = 0;
               if (!check_vector(c, target, 4, 4, false, count))
                  return false;
               for (std::uint64_t i = 0; i < count; ++i)
               {
                  const std::uint64_t entry = check_offset(c, target + 4 + i * 4);
                  if (!entry || !check_table<entry_def<K, V>>(c, entry))
                     return false;
               }
               for (std::uint64_t i = 1; i < count; ++i)
                  if (!keys_ascend<K>(c, entry_key_at<K>(c.buf + target + 4 + (i - 1) * 4),
                                      entry_key_at<K>(c.buf + target + 4 + i * 4), target,
                                      "flatbuf: map keys are not in ascending order"))
                     return false;
               return true;
            }
            else
               return check_table<record_def<F>>(c, target);
         }
      }

      // A table header that has passed its checks.
      struct checked_table
      {
         std::uint64_t pos    = 0;
         std::uint64_t vtable = 0;
         std::uint32_t vsize  = 0;
         std::uint32_t object = 0;
      };

      // The position of the field at vtable position `vt` occupying `size`
      // bytes: 0 when absent; `ok` false after recording an error.
      inline std::uint64_t checked_field(checker& c, const checked_table& t, std::uint32_t vt,
                                         std::uint32_t size, bool& ok) noexcept
      {
         if (vt + 2 > t.vsize)
            return 0;
         const std::uint32_t off = load<std::uint16_t>(c.buf + t.vtable + vt);
         if (off == 0)
            return 0;
         if (off < 4 || off + size > t.object)
         {
            ok = c.fail("flatbuf: field outside its table", t.vtable + vt);
            return 0;
         }
         return t.pos + off;
      }

      template <typename Def, std::size_t I>
      [[gnu::always_inline]] inline bool check_member(checker& c, const checked_table& t) noexcept
      {
         using F = typename Def::template member<I>;
         if constexpr (Def::template deprecated<I>)
            return true;
         else if constexpr (is_variant_v<F>)
         {
            constexpr std::size_t s  = Def::slot(I);
            bool                  ok = true;
            const std::uint64_t   tc = checked_field(c, t, vt_of(s), 1, ok);
            if (!ok)
               return false;
            const std::uint64_t vc = checked_field(c, t, vt_of(s + 1), 4, ok);
            if (!ok)
               return false;
            const std::size_t type = tc ? c.buf[tc] : 0;
            if (type == 0)
               return vc ? c.fail("flatbuf: union is NONE but has a value", vc) : true;
            const std::size_t which = type - (variant_traits<F>::has_none ? 0 : 1);
            if (which >= variant_traits<F>::count)
               return c.fail("flatbuf: unknown union type", tc);
            if (!vc)
               return c.fail("flatbuf: union has a type but no value", tc);
            const std::uint64_t target = check_offset(c, vc);
            if (!target)
               return false;
            bool result = true;
            [&]<std::size_t... Js>(std::index_sequence<Js...>) {
               (void)((which == Js
                          ? ([&] {
                                using Alt = std::variant_alternative_t<Js, F>;
                                if constexpr (!std::is_same_v<Alt, std::monostate>)
                                   result = check_table<record_def<Alt>>(c, target);
                             }(),
                             true)
                          : false) ||
                      ...);
            }(std::make_index_sequence<variant_traits<F>::count>{});
            return result;
         }
         else
         {
            bool                ok   = true;
            const std::uint64_t cell = checked_field(
               c, t, vt_of(Def::slot(I)), static_cast<std::uint32_t>(inline_size<F>()), ok);
            if (!ok)
               return false;
            if constexpr (Def::template required<I>)
            {
               if (!cell)
                  return c.fail("flatbuf: map entry has no key", t.pos);
            }
            return cell ? check_cell<F>(c, cell) : true;
         }
      }

      template <typename Def>
      bool check_table(checker& c, std::uint64_t pos) noexcept
      {
         static_assert(def_supported<Def>(),
                       "psio::flatbuf: a member of this record has a type FlatBuffers cannot "
                       "hold (see the mapping in psio/flatbuf.hpp)");
         if (c.depth >= c.lim.max_depth)
            return c.fail("flatbuf: tables nested too deeply", pos);
         if (c.tables >= c.lim.max_tables)
            return c.fail("flatbuf: too many tables", pos);
         ++c.tables;
         if (pos & 3)
            return c.fail("flatbuf: misaligned table", pos);
         if (!c.has(pos, 4))
            return c.fail("flatbuf: table out of bounds", pos);
         const std::int64_t vpos =
            static_cast<std::int64_t>(pos) - load<std::int32_t>(c.buf + pos);
         if (vpos < 0 || (vpos & 1) || !c.has(static_cast<std::uint64_t>(vpos), 4))
            return c.fail("flatbuf: vtable out of bounds", pos);
         checked_table t;
         t.pos    = pos;
         t.vtable = static_cast<std::uint64_t>(vpos);
         t.vsize  = load<std::uint16_t>(c.buf + t.vtable);
         t.object = load<std::uint16_t>(c.buf + t.vtable + 2);
         if (t.vsize < 4 || (t.vsize & 1) || !c.has(t.vtable, t.vsize))
            return c.fail("flatbuf: bad vtable size", t.vtable);
         if (t.object < 4 || !c.has(pos, t.object))
            return c.fail("flatbuf: bad table size", t.vtable);
         ++c.depth;
         const bool ok = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return (check_member<Def, Is>(c, t) && ...);
         }(std::make_index_sequence<Def::count>{});
         --c.depth;
         return ok;
      }

      struct read_options
      {
         // Require this file identifier (four characters; empty: any).
         std::string_view file_identifier{};
         // The buffer starts with its size.
         bool size_prefixed = false;
      };

      template <Table T>
      codec_status validate_buffer(std::span<const char> bytes, const read_options& opts = {},
                                   limits lim = {}) noexcept
      {
         checker c{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), lim};
         if (bytes.size() > max_buffer_size)
            return codec_fail("flatbuf: buffer larger than 2 GiB", 0, "flatbuf");
         c.payload = payload_allowance + c.size * lim.max_expansion;
         const std::uint64_t start = opts.size_prefixed ? 4 : 0;
         if (!c.has(0, start + 4))
            return codec_fail("flatbuf: buffer too small", 0, "flatbuf");
         if (opts.size_prefixed && load<std::uint32_t>(c.buf) != bytes.size() - 4)
            return codec_fail("flatbuf: size prefix does not match the buffer", 0, "flatbuf");
         if (!opts.file_identifier.empty())
         {
            if (opts.file_identifier.size() != 4 || !c.has(start, 8) ||
                std::memcmp(c.buf + start + 4, opts.file_identifier.data(), 4) != 0)
               return codec_fail("flatbuf: wrong file identifier",
                                 static_cast<std::uint32_t>(start + 4), "flatbuf");
         }
         const std::uint64_t root = check_offset(c, start);
         if (!root || !check_table<record_def<T>>(c, root))
            return codec_fail(c.error, static_cast<std::uint32_t>(c.at), "flatbuf");
         // The buffer is one T as FlatBuffers defines it. What T declares
         // about its values — member bounds, utf8, order, invariants
         // (value_contract.hpp) — is checked on the value the buffer decodes
         // to; a type that declares nothing is done here.
         if constexpr (::psio::has_value_contract_v<T>)
         {
            T out{};
            decode_table<record_def<T>>(root_of(bytes.subspan(start)), out);
            return ::psio::check_value_contract(out, "flatbuf: member length_bound violated",
                                                "flatbuf");
         }
         else
            return codec_ok();
      }

   }  // namespace detail::flatbuf_impl

   // ── Format tag ──────────────────────────────────────────────────────────

   struct flatbuf : format_tag_base<flatbuf>
   {
      static constexpr const char*       name         = "flatbuf";
      static constexpr trusted_read_kind trusted_read = trusted_read_kind::validated_bytes_only;
      static constexpr sink_write_kind   sink_write   = sink_write_kind::replaces;
      using preferred_presentation_category = ::psio::binary_category;

      // The limits `validate` applies; the official verifier's defaults.
      using limits = detail::flatbuf_impl::limits;
      // A file identifier and / or a size prefix: `to_flatbuf(v, options)`,
      // `validate_flatbuf<T>(bytes, options)`.
      using options = detail::flatbuf_impl::write_options;

      // The layout of T. A table: `layout<T>::slot(i)` is the field id of
      // member i and `vtable_offset(i)` its position in the vtable — what
      // `flatc` assigns for the schema of T. A struct: `size`, `align` and
      // `offset(i)`.
      template <typename T>
      struct layout
      {
         static constexpr bool is_struct = detail::flatbuf_impl::is_struct_v<T>;
         static constexpr std::size_t members = ::psio::reflect<T>::member_count;

         static constexpr std::size_t slot(std::size_t i) noexcept
            requires (!is_struct)
         {
            return detail::flatbuf_impl::record_def<T>::slot(i);
         }
         static constexpr std::size_t vtable_offset(std::size_t i) noexcept
            requires (!is_struct)
         {
            return detail::flatbuf_impl::vt_of(detail::flatbuf_impl::record_def<T>::slot(i));
         }
         static constexpr std::size_t slots() noexcept
            requires (!is_struct)
         {
            return detail::flatbuf_impl::record_def<T>::slots;
         }
         static constexpr std::size_t size() noexcept
            requires is_struct
         {
            return detail::flatbuf_impl::struct_layout<T>::size;
         }
         static constexpr std::size_t align() noexcept
            requires is_struct
         {
            return detail::flatbuf_impl::struct_layout<T>::align;
         }
         static constexpr std::size_t offset(std::size_t i) noexcept
            requires is_struct
         {
            return detail::flatbuf_impl::struct_layout<T>::offset(i);
         }
      };

      template <typename T>
         requires detail::flatbuf_impl::Table<T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode), flatbuf, const T& v)
      {
         std::vector<char> out;
         detail::flatbuf_impl::encode_into(v, {}, out);
         return out;
      }

      // Sink form: replaces the contents of `sink`, reusing its capacity.
      template <typename T>
         requires detail::flatbuf_impl::Table<T>
      friend void tag_invoke(decltype(::psio::encode), flatbuf, const T& v, std::vector<char>& sink)
      {
         detail::flatbuf_impl::encode_into(v, {}, sink);
      }

      template <typename T>
         requires detail::flatbuf_impl::Table<T>
      friend T tag_invoke(decltype(::psio::decode<T>), flatbuf, T*, std::span<const char> bytes)
      {
         T out{};
         detail::flatbuf_impl::decode_table<detail::flatbuf_impl::record_def<T>>(
            detail::flatbuf_impl::root_of(bytes), out);
         return out;
      }

      template <typename T>
         requires detail::flatbuf_impl::Table<T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), flatbuf, const T& v)
      {
         // Padding depends on position, so the size is found by encoding.
         std::vector<char> out;
         detail::flatbuf_impl::encode_into(v, {}, out);
         return out.size();
      }

      template <typename T>
         requires detail::flatbuf_impl::Table<T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>), flatbuf, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(), "flatbuf"); !st.ok())
            return st;
         return detail::flatbuf_impl::validate_buffer<T>(bytes);
      }

      // `validate` (the structure, then what T declares about its values:
      // bounds, utf8, order, invariants), then the specs of user code that
      // have a `validate(span)` of their own, on the decoded value.
      template <typename T>
         requires detail::flatbuf_impl::Table<T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>), flatbuf, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(), "flatbuf"); !st.ok())
            return st;
         if (auto st = detail::flatbuf_impl::validate_buffer<T>(bytes); !st.ok())
            return st;
         T out{};
         detail::flatbuf_impl::decode_table<detail::flatbuf_impl::record_def<T>>(
            detail::flatbuf_impl::root_of(bytes), out);
         return ::psio::validate_specs_on_value(out);
      }

      template <typename T>
         requires detail::flatbuf_impl::Table<T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>), flatbuf, T*,
                                           std::span<const char> bytes) noexcept
      {
         auto v = std::make_unique<T>();
         detail::flatbuf_impl::decode_table<detail::flatbuf_impl::record_def<T>>(
            detail::flatbuf_impl::root_of(bytes), *v);
         return v;
      }
   };

   PSIO_FORMAT_HELPERS(flatbuf)

   // `to_flatbuf(v, {.file_identifier = "ORDR", .size_prefixed = true})`.
   template <typename T>
      requires detail::flatbuf_impl::Table<T>
   [[nodiscard]] std::vector<char> to_flatbuf(const T& v, const flatbuf::options& opts)
   {
      std::vector<char> out;
      detail::flatbuf_impl::encode_into(v, opts, out);
      return out;
   }

   // The typed check under limits other than the defaults, and / or of a
   // buffer with a required file identifier or a size prefix.
   template <typename T>
      requires detail::flatbuf_impl::Table<T>
   [[nodiscard]] codec_status validate_flatbuf(std::span<const char> bytes,
                                               flatbuf::limits       lim) noexcept
   {
      return detail::flatbuf_impl::validate_buffer<T>(bytes, {}, lim);
   }
   template <typename T>
      requires detail::flatbuf_impl::Table<T>
   [[nodiscard]] codec_status validate_flatbuf(std::span<const char> bytes,
                                               const flatbuf::options& opts,
                                               flatbuf::limits         lim = {}) noexcept
   {
      return detail::flatbuf_impl::validate_buffer<T>(
         bytes, {opts.file_identifier, opts.size_prefixed}, lim);
   }

   // The file identifier of a buffer (not size-prefixed): the four bytes
   // after the root offset. Empty when the buffer is too short to have one.
   // A buffer written without an identifier has other data there.
   [[nodiscard]] inline std::string_view flatbuf_identifier(std::span<const char> bytes) noexcept
   {
      return bytes.size() >= 8 ? std::string_view{bytes.data() + 4, 4} : std::string_view{};
   }
   [[nodiscard]] inline bool flatbuf_has_identifier(std::span<const char> bytes,
                                                    std::string_view      id) noexcept
   {
      return id.size() == 4 && flatbuf_identifier(bytes) == id;
   }

   // The buffer inside a size-prefixed one — what views, `from_flatbuf` and
   // `flatbuf_identifier` take. Empty when the prefix exceeds the bytes; any
   // bytes after the prefixed buffer (the next one of a stream) are left out.
   [[nodiscard]] inline std::span<const char> flatbuf_size_prefixed(
      std::span<const char> bytes) noexcept
   {
      if (bytes.size() < 4)
         return {};
      const std::uint32_t n = detail::flatbuf_impl::load<std::uint32_t>(
         reinterpret_cast<const std::uint8_t*>(bytes.data()));
      if (n > bytes.size() - 4)
         return {};
      return bytes.subspan(4, n);
   }

}  // namespace psio
