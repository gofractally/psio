#pragma once
//
// psio/capnp.hpp — Cap'n Proto, the native wire format.
//
// `psio::capnp` reads and writes the encoding the official implementation
// uses (https://capnproto.org/encoding.html): a framed message (segment
// table, then segments), structs with a data section and a pointer section,
// lists, Text / Data, unions laid out exactly as `capnp compile` lays them
// out, default values XORed into the data section, far pointers. A message
// written here is read by libcapnp against the schema `psio::emit_capnp`
// exports for the same type, and the reverse (tests/capnp_interop_tests.cpp).
//
//   auto bytes = psio::to_capnp(order);                 // std::vector<char>
//   auto back  = psio::from_capnp<Order>(bytes);        // validates, then decodes
//   auto st    = psio::validate_capnp<Order>(bytes);    // codec_status
//   psio::capnp_view<Order> v{bytes};                   // psio/capnp_view.hpp
//
// How a C++ type maps (docs/capnp.md has the table and the reasons):
//
//   bool, integers, float, double        Bool, IntN / UIntN, Float32 / Float64
//   enum                                 its underlying integer
//   std::string                          Text
//   std::vector<T>, std::array<T, N>     List(T); a list of bytes is Data
//   reflected record                     struct
//   std::optional<P>, P a pointer type   the pointer; null is "no value"
//   std::optional<S>, S a scalar         union { none @n :Void; some @n+1 :S; }
//   std::variant<A, B, ...>              union, one member per alternative;
//                                        std::monostate is Void
//
// Ordinals. The members of a record take ordinals in ascending order of
// their field numbers (declaration position unless `attr(member, field<N>)`
// says otherwise — psio/detail/field_number.hpp); a variant member takes
// one ordinal per alternative, an optional scalar two. The layout is the
// one the official compiler computes for those ordinals, so adding a member
// with a higher field number is the same compatible change it is in a
// .capnp file.
//
// Defaults. A scalar member's default is its member initialiser: the wire
// holds `value XOR default`, an absent or truncated struct reads as the
// defaults. A pointer member's default is empty, a union's is its first
// alternative, an optional's is "no value".
//
// Writing produces one segment. Reading follows far pointers, so a
// multi-segment message from another implementation reads the same.
//
// Validation is typed: `validate_capnp<T>` checks every pointer T can reach
// — kind, element size, bounds within its segment, NUL-terminated Text,
// union discriminants — under the official reader's limits (64 levels of
// nesting, a traversal budget of 8 Mi words). Views and `decode` are
// trusted operations over validated bytes, as everywhere in PSIO; on bytes
// that were never validated they are the caller's risk.
//
// Not supported: packed encoding, capabilities / interfaces, AnyPointer,
// groups other than the unions above, generics, a List(struct) written as a
// list of primitives (the pre-0.5 upgrade path), std::map / std::set /
// std::tuple members (Cap'n Proto has no counterpart).

#include <psio/adapter.hpp>
#include <psio/annotate.hpp>
#include <psio/cpo.hpp>
#include <psio/detail/capnp_holes.hpp>
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
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio {

   struct capnp;

   namespace detail::capnp_impl {

      static_assert(std::endian::native == std::endian::little,
                    "psio::capnp reads and writes little-endian words in place");

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

      template <typename T>
      concept Record = ::psio::Reflected<T> && std::is_class_v<T>;

      // A value of the data section: 1 bit (bool) or 1, 2, 4, 8 bytes.
      template <typename T>
      concept Scalar = (std::is_arithmetic_v<T> || std::is_enum_v<T>) && sizeof(T) <= 8;

      template <typename T>
      struct is_pointer_type : std::bool_constant<Record<T>>
      {
      };
      template <>
      struct is_pointer_type<std::string> : std::true_type
      {
      };
      template <typename E, typename A>
      struct is_pointer_type<std::vector<E, A>> : std::true_type
      {
      };
      template <typename E, std::size_t N>
      struct is_pointer_type<std::array<E, N>> : std::true_type
      {
      };
      // An optional pointer is that pointer; null is "no value". (An
      // optional of an optional has no second null to use.)
      template <typename E>
      struct is_pointer_type<std::optional<E>>
         : std::bool_constant<is_pointer_type<E>::value && !is_optional_v<E>>
      {
      };

      // A value held behind one pointer.
      template <typename T>
      concept Pointer = is_pointer_type<T>::value;

      // A record member laid out as a union: a variant, or an optional scalar.
      template <typename T>
      inline constexpr bool is_union_v = false;
      template <typename... Ts>
      inline constexpr bool is_union_v<std::variant<Ts...>> = true;
      template <Scalar S>
      inline constexpr bool is_union_v<std::optional<S>> = true;

      // log2 of a scalar's size in bits: Bool 0, then 3 (byte) … 6 (word).
      template <Scalar S>
      consteval std::uint8_t lg_bits()
      {
         if constexpr (std::is_same_v<S, bool>)
            return 0;
         else
            return sizeof(S) == 1 ? 3 : sizeof(S) == 2 ? 4 : sizeof(S) == 4 ? 5 : 6;
      }

      // The element-size code of a list of scalars.
      template <Scalar S>
      consteval std::uint8_t list_code()
      {
         return std::is_same_v<S, bool> ? 1 : static_cast<std::uint8_t>(lg_bits<S>() - 1);
      }

      // ── Struct layout: the official compiler's allocator ──────────────
      //
      // This is `StructLayout` of capnp's node-translator: fields are
      // placed in ordinal order; a data field takes the smallest hole that
      // fits or a new word; the members of a union share space, each
      // reusing (and if possible growing) the locations earlier members
      // claimed; the 16-bit discriminant is allocated when the union's
      // second member is. tests/capnp_layout_tests.cpp checks the result
      // against `capnp compile` on unions of every size mix.

      enum class slot_kind : std::uint8_t
      {
         none,     // Void
         data,
         pointer
      };

      struct slot
      {
         slot_kind     kind   = slot_kind::none;
         std::uint8_t  lg     = 0;  // data: log2 bits
         std::uint32_t offset = 0;  // data: in units of the field's size; pointer: index

         // data: the byte that holds the field (the first of them)
         constexpr std::uint32_t byte() const noexcept
         {
            return lg == 0 ? offset >> 3 : offset << (lg - 3);
         }
         constexpr std::uint32_t bit() const noexcept { return offset & 7; }
      };

      using hole_set = ::psio::detail::capnp_hole_set;

      struct layout_top
      {
         std::uint32_t data_words = 0;
         std::uint32_t ptr_count  = 0;
         hole_set      holes;

         constexpr std::uint32_t add_data(std::uint32_t lg)
         {
            if (auto hole = holes.try_allocate(lg))
               return *hole;
            const std::uint32_t offset = data_words++ << (6 - lg);
            holes.add_holes_at_end(lg, offset + 1);
            return offset;
         }
         constexpr std::uint32_t add_pointer() { return ptr_count++; }
         constexpr bool try_expand_data(std::uint32_t old_lg, std::uint32_t old_offset,
                                        std::uint32_t factor)
         {
            return holes.try_expand(old_lg, old_offset, factor);
         }
      };

      struct layout_union
      {
         struct data_location
         {
            std::uint32_t lg;
            std::uint32_t offset;

            constexpr bool try_expand_to(layout_top& top, std::uint32_t new_lg)
            {
               if (new_lg <= lg)
                  return true;
               if (top.try_expand_data(lg, offset, new_lg - lg))
               {
                  offset >>= (new_lg - lg);
                  lg = new_lg;
                  return true;
               }
               return false;
            }
         };

         layout_top&                  top;
         std::uint32_t                groups = 0;
         std::optional<std::uint32_t> discriminant;  // offset in 16-bit units
         std::vector<data_location>   data;
         std::vector<std::uint32_t>   pointers;

         constexpr explicit layout_union(layout_top& t) : top(t) {}

         constexpr bool add_discriminant()
         {
            if (discriminant)
               return false;
            discriminant = top.add_data(4);
            return true;
         }
         constexpr void new_group_adding_first_member()
         {
            if (++groups == 2)
               add_discriminant();
         }
      };

      // One member of a union (the "group" the compiler makes for it).
      struct layout_group
      {
         struct usage
         {
            bool          used    = false;
            std::uint32_t lg_used = 0;
            hole_set      holes;

            constexpr std::optional<std::uint32_t> smallest_hole_at_least(
               const layout_union::data_location& loc, std::uint32_t lg) const
            {
               if (!used)
                  return lg <= loc.lg ? std::optional<std::uint32_t>{loc.lg} : std::nullopt;
               if (lg >= lg_used)
                  return lg < loc.lg ? std::optional<std::uint32_t>{lg} : std::nullopt;
               if (auto r = holes.smallest_at_least(lg))
                  return r;
               return lg_used < loc.lg ? std::optional<std::uint32_t>{lg_used} : std::nullopt;
            }
            constexpr std::uint32_t allocate_from_hole(const layout_union::data_location& loc,
                                                       std::uint32_t                      lg)
            {
               std::uint32_t result = 0;
               if (!used)
               {
                  used    = true;
                  lg_used = lg;
               }
               else if (lg >= lg_used)
               {
                  holes.add_holes_at_end(lg_used, 1, lg);
                  lg_used = lg + 1;
                  result  = 1;
               }
               else if (auto hole = holes.try_allocate(lg))
                  result = *hole;
               else
               {
                  result = std::uint32_t{1} << (lg_used - lg);
                  holes.add_holes_at_end(lg, result + 1, lg_used);
                  lg_used += 1;
               }
               return (loc.offset << (loc.lg - lg)) + result;
            }
            constexpr std::optional<std::uint32_t> try_allocate_by_expanding(
               layout_top& top, layout_union::data_location& loc, std::uint32_t lg)
            {
               if (!used)
               {
                  if (!loc.try_expand_to(top, lg))
                     return std::nullopt;
                  used    = true;
                  lg_used = lg;
                  return loc.offset << (loc.lg - lg);
               }
               const std::uint32_t desired = std::max(lg_used, lg) + 1;
               if (desired > loc.lg && !loc.try_expand_to(top, desired))
                  return std::nullopt;
               holes.add_holes_at_end(lg_used, 1, desired);
               lg_used = desired;
               const auto hole = holes.try_allocate(lg);
               return (loc.offset << (loc.lg - lg)) + *hole;
            }
         };

         layout_union&      parent;
         std::vector<usage> data_usage;
         std::uint32_t      pointer_usage = 0;
         bool               has_members   = false;

         constexpr explicit layout_group(layout_union& u) : parent(u) {}

         constexpr void add_member()
         {
            if (!has_members)
            {
               has_members = true;
               parent.new_group_adding_first_member();
            }
         }
         constexpr void add_void() { add_member(); }
         constexpr std::uint32_t add_data(std::uint32_t lg)
         {
            add_member();
            std::uint32_t              best_size = ~std::uint32_t{0};
            std::optional<std::size_t> best;
            for (std::size_t i = 0; i < parent.data.size(); ++i)
            {
               if (data_usage.size() == i)
                  data_usage.emplace_back();
               if (auto hole = data_usage[i].smallest_hole_at_least(parent.data[i], lg))
                  if (*hole < best_size)
                  {
                     best_size = *hole;
                     best      = i;
                  }
            }
            if (best)
               return data_usage[*best].allocate_from_hole(parent.data[*best], lg);
            for (std::size_t i = 0; i < parent.data.size(); ++i)
               if (auto r =
                      data_usage[i].try_allocate_by_expanding(parent.top, parent.data[i], lg))
                  return *r;
            const std::uint32_t offset = parent.top.add_data(lg);
            parent.data.push_back({lg, offset});
            usage u;
            u.used    = true;
            u.lg_used = lg;
            data_usage.push_back(u);
            return offset;
         }
         constexpr std::uint32_t add_pointer()
         {
            add_member();
            if (pointer_usage < parent.pointers.size())
               return parent.pointers[pointer_usage++];
            ++pointer_usage;
            parent.pointers.push_back(parent.top.add_pointer());
            return parent.pointers.back();
         }
      };

      // What one ordinal needs.
      template <typename A>
      consteval slot slot_shape()
      {
         if constexpr (std::is_same_v<A, std::monostate>)
            return {slot_kind::none, 0, 0};
         else if constexpr (Scalar<A>)
            return {slot_kind::data, lg_bits<A>(), 0};
         else
         {
            static_assert(Pointer<A>,
                          "psio::capnp: this type has no Cap'n Proto counterpart (supported: "
                          "bool, integers up to 64 bits, float, double, enums, std::string, "
                          "std::vector, std::array, reflected records, std::optional and "
                          "std::variant of those as record members)");
            return {slot_kind::pointer, 0, 0};
         }
      }

      // A plain member's shape; nothing for a union member (its slot is the
      // discriminant, placed by the allocator).
      template <typename F>
      consteval slot plain_shape()
      {
         if constexpr (is_union_v<F>)
            return {};
         else
            return slot_shape<F>();
      }

      // The alternatives of a union member, as slot shapes.
      template <typename F>
      struct union_alts
      {
         static constexpr std::size_t count = 0;
         static consteval void        fill(slot*) {}
      };
      template <typename... Ts>
      struct union_alts<std::variant<Ts...>>
      {
         static_assert(sizeof...(Ts) >= 2,
                       "psio::capnp: a Cap'n Proto union has at least two members; a "
                       "std::variant with one alternative has no counterpart");
         static constexpr std::size_t count = sizeof...(Ts);
         static consteval void        fill(slot* out)
         {
            std::size_t i = 0;
            ((out[i++] = slot_shape<Ts>()), ...);
         }
      };
      template <Scalar S>
      struct union_alts<std::optional<S>>
      {
         static constexpr std::size_t count = 2;  // none :Void, some :S
         static consteval void        fill(slot* out)
         {
            out[0] = {slot_kind::none, 0, 0};
            out[1] = slot_shape<S>();
         }
      };

      template <Record T>
      struct layout
      {
         using R = ::psio::reflect<T>;
         static constexpr std::size_t members = R::member_count;

         template <std::size_t I>
         using member = std::remove_cvref_t<typename R::template member_type<I>>;

         static constexpr std::size_t alt_total = []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (std::size_t{0} + ... + union_alts<member<Is>>::count);
         }(std::make_index_sequence<members>{});

         struct result
         {
            // A plain member: its slot. A union member: its discriminant.
            std::array<slot, members> field{};
            // A union member: the index of its first alternative in `alt`.
            std::array<std::uint16_t, members>               alt_first{};
            std::array<slot, alt_total == 0 ? 1 : alt_total> alt{};
            std::uint16_t                                    data_words = 0;
            std::uint16_t                                    ptr_count  = 0;
            bool                                             too_large  = false;
         };

         static consteval result compute()
         {
            result r{};
            // Shapes.
            std::array<bool, members>        is_union{};
            std::array<std::size_t, members> alt_count{};
            {
               std::size_t next = 0;
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  ((is_union[Is]     = is_union_v<member<Is>>,
                    alt_count[Is]    = union_alts<member<Is>>::count,
                    r.alt_first[Is]  = static_cast<std::uint16_t>(next),
                    union_alts<member<Is>>::fill(r.alt.data() + next),
                    next            += alt_count[Is],
                    r.field[Is]      = plain_shape<member<Is>>()),
                   ...);
               }(std::make_index_sequence<members>{});
            }
            // Ordinal order: ascending field number, declaration order among equals.
            const auto& order = ::psio::detail::members_by_field_number<T>;

            layout_top top;
            for (std::size_t k = 0; k < members; ++k)
            {
               const std::size_t i = order[k];
               if (!is_union[i])
               {
                  auto& s = r.field[i];
                  if (s.kind == slot_kind::data)
                     s.offset = top.add_data(s.lg);
                  else if (s.kind == slot_kind::pointer)
                     s.offset = top.add_pointer();
                  continue;
               }
               layout_union u{top};
               for (std::size_t a = 0; a < alt_count[i]; ++a)
               {
                  auto&        s = r.alt[r.alt_first[i] + a];
                  layout_group g{u};
                  if (s.kind == slot_kind::none)
                     g.add_void();
                  else if (s.kind == slot_kind::data)
                     s.offset = g.add_data(s.lg);
                  else
                     s.offset = g.add_pointer();
               }
               u.add_discriminant();
               r.field[i] = {slot_kind::data, 4, *u.discriminant};
            }
            r.too_large = top.data_words > 0xffff || top.ptr_count > 0xffff;
            r.data_words = static_cast<std::uint16_t>(top.data_words);
            r.ptr_count  = static_cast<std::uint16_t>(top.ptr_count);
            return r;
         }

         static constexpr result        value      = compute();
         static_assert(!value.too_large,
                       "psio::capnp: a struct section holds at most 65535 words");
         static constexpr std::uint16_t data_words = value.data_words;
         static constexpr std::uint16_t ptr_count  = value.ptr_count;
         static constexpr std::uint32_t words =
            std::uint32_t{value.data_words} + value.ptr_count;

         static constexpr slot field(std::size_t i) noexcept { return value.field[i]; }
         static constexpr slot alt(std::size_t i, std::size_t a) noexcept
         {
            return value.alt[value.alt_first[i] + a];
         }
      };

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
      // a constant expression (the usual case — the XOR then folds away
      // when it is zero), else computed once.
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

      template <Scalar S>
      using bits_t = std::conditional_t<
         sizeof(S) == 1, std::uint8_t,
         std::conditional_t<sizeof(S) == 2, std::uint16_t,
                            std::conditional_t<sizeof(S) == 4, std::uint32_t, std::uint64_t>>>;

      template <Scalar S>
      [[gnu::always_inline]] constexpr bits_t<S> to_bits(S v) noexcept
      {
         if constexpr (std::is_floating_point_v<S>)
            return std::bit_cast<bits_t<S>>(v);
         else
            return static_cast<bits_t<S>>(v);
      }
      template <Scalar S>
      [[gnu::always_inline]] constexpr S from_bits(bits_t<S> b) noexcept
      {
         if constexpr (std::is_floating_point_v<S>)
            return std::bit_cast<S>(b);
         else
            return static_cast<S>(b);
      }

      // ── Wire primitives ───────────────────────────────────────────────

      [[gnu::always_inline]] inline std::uint64_t load64(const std::uint8_t* p) noexcept
      {
         std::uint64_t v;
         std::memcpy(&v, p, 8);
         return v;
      }
      [[gnu::always_inline]] inline std::uint32_t load32(const std::uint8_t* p) noexcept
      {
         std::uint32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }

      // Bits 2..31 of a struct / list pointer: the signed offset in words
      // from the end of the pointer to the object.
      [[gnu::always_inline]] constexpr std::int32_t pointer_offset(std::uint64_t w) noexcept
      {
         return static_cast<std::int32_t>(static_cast<std::uint32_t>(w)) >> 2;
      }

      inline constexpr std::uint32_t max_segments = 512;  // the official reader's limit

      // Bytes of the segment table of a message with `n` segments.
      constexpr std::size_t table_bytes(std::size_t n) noexcept
      {
         return (4 + 4 * n + 7) & ~std::size_t{7};
      }

      // Segment `id` of a validated message: its first byte and its size.
      inline std::span<const std::uint8_t> segment_of(const std::uint8_t* msg,
                                                      std::uint32_t       id) noexcept
      {
         const std::size_t   n = std::size_t{load32(msg)} + 1;
         const std::uint8_t* p = msg + table_bytes(n);
         for (std::uint32_t i = 0; i < id; ++i)
            p += std::size_t{load32(msg + 4 + 4 * i)} * 8;
         return {p, std::size_t{load32(msg + 4 + 4 * id)} * 8};
      }

      // A pointer followed to its object: the word that describes the
      // object (the pointer itself, or the landing pad's) and the object.
      struct resolved
      {
         std::uint64_t       word;
         const std::uint8_t* target;
      };

      [[gnu::noinline]] inline resolved resolve_far(const std::uint8_t* msg,
                                                    std::uint64_t       w) noexcept
      {
         const auto          seg = segment_of(msg, static_cast<std::uint32_t>(w >> 32));
         const std::uint8_t* pad = seg.data() + std::size_t{(static_cast<std::uint32_t>(w) >> 3)} * 8;
         if ((w & 4) == 0)
         {
            const std::uint64_t w2 = load64(pad);
            return {w2, pad + 8 + std::int64_t{pointer_offset(w2)} * 8};
         }
         // Double-far: the pad is a far pointer to the object and a tag word.
         const std::uint64_t far = load64(pad);
         const auto          seg2 = segment_of(msg, static_cast<std::uint32_t>(far >> 32));
         return {load64(pad + 8),
                 seg2.data() + std::size_t{(static_cast<std::uint32_t>(far) >> 3)} * 8};
      }

      // The pointer at `slot` (non-null), followed. Trusted.
      [[gnu::always_inline]] inline resolved resolve(const std::uint8_t* msg,
                                                     const std::uint8_t* slot,
                                                     std::uint64_t       w) noexcept
      {
         if ((w & 3) == 2) [[unlikely]]
            return resolve_far(msg, w);
         return {w, slot + 8 + std::int64_t{pointer_offset(w)} * 8};
      }

      // A struct: its data section, the sizes of both sections, and the
      // message (for far pointers below it). A null struct is all zero and
      // reads as every member's default.
      struct struct_ref
      {
         const std::uint8_t* data       = nullptr;
         const std::uint8_t* msg        = nullptr;
         std::uint32_t       data_bytes = 0;
         std::uint16_t       ptr_count  = 0;

         [[gnu::always_inline]] const std::uint8_t* pointer(std::uint32_t i) const noexcept
         {
            return data + data_bytes + std::size_t{i} * 8;
         }
      };

      // A list: its elements, their count and stride, and for a list of
      // structs the element's section sizes.
      struct list_ref
      {
         const std::uint8_t* data            = nullptr;
         const std::uint8_t* msg             = nullptr;
         std::uint32_t       count           = 0;
         std::uint32_t       step            = 0;  // bytes per element; 0 for bits
         std::uint32_t       elem_data_bytes = 0;
         std::uint16_t       elem_ptr_count  = 0;
      };

      [[gnu::always_inline]] inline struct_ref struct_at(const std::uint8_t* msg,
                                                         const std::uint8_t* slot) noexcept
      {
         const std::uint64_t w = load64(slot);
         if (w == 0)
            return {};
         const auto r = resolve(msg, slot, w);
         return {r.target, msg, static_cast<std::uint32_t>((r.word >> 32) & 0xffff) * 8,
                 static_cast<std::uint16_t>(r.word >> 48)};
      }

      inline constexpr std::uint32_t list_step[8] = {0, 0, 1, 2, 4, 8, 8, 0};

      [[gnu::always_inline]] inline list_ref list_at(const std::uint8_t* msg,
                                                     const std::uint8_t* slot) noexcept
      {
         const std::uint64_t w = load64(slot);
         if (w == 0)
            return {};
         const auto          r    = resolve(msg, slot, w);
         const std::uint32_t code = static_cast<std::uint32_t>(r.word >> 32) & 7;
         if (code == 7)
         {
            const std::uint64_t tag = load64(r.target);
            const std::uint32_t dw  = static_cast<std::uint32_t>(tag >> 32) & 0xffff;
            const std::uint16_t pc  = static_cast<std::uint16_t>(tag >> 48);
            return {r.target + 8, msg, static_cast<std::uint32_t>(tag) >> 2, (dw + pc) * 8,
                    dw * 8,       pc};
         }
         return {r.target, msg, static_cast<std::uint32_t>(r.word >> 35), list_step[code], 0, 0};
      }

      // Text without its NUL. A null pointer is the empty string.
      [[gnu::always_inline]] inline std::string_view text_at(const std::uint8_t* msg,
                                                             const std::uint8_t* slot) noexcept
      {
         const std::uint64_t w = load64(slot);
         if (w == 0)
            return {};
         const auto r = resolve(msg, slot, w);
         return {reinterpret_cast<const char*>(r.target),
                 static_cast<std::uint32_t>(r.word >> 35) - 1};
      }

      // The root struct of a framed message.
      inline struct_ref root_of(std::span<const char> bytes) noexcept
      {
         const auto* msg = reinterpret_cast<const std::uint8_t*>(bytes.data());
         return struct_at(msg, msg + table_bytes(std::size_t{load32(msg)} + 1));
      }

      // Scalar at `loc` of struct `s`: zero bits beyond the data section
      // the writer wrote (an older schema), XOR the default.
      template <Scalar S>
      [[gnu::always_inline]] inline S read_scalar(const struct_ref& s, slot loc, S def) noexcept
      {
         if constexpr (std::is_same_v<S, bool>)
         {
            const std::uint32_t byte = loc.byte();
            const bool          wire =
               byte < s.data_bytes ? ((s.data[byte] >> loc.bit()) & 1) != 0 : false;
            return wire != def;
         }
         else
         {
            const std::uint32_t byte = loc.byte();
            bits_t<S>           wire = 0;
            if (byte + sizeof(S) <= s.data_bytes) [[likely]]
               std::memcpy(&wire, s.data + byte, sizeof(S));
            return from_bits<S>(static_cast<bits_t<S>>(wire ^ to_bits(def)));
         }
      }

      // The pointer slot `loc` of `s`, or nullptr when the writer's struct
      // has no such slot.
      [[gnu::always_inline]] inline const std::uint8_t* pointer_slot(const struct_ref& s,
                                                                     slot loc) noexcept
      {
         return loc.offset < s.ptr_count ? s.pointer(loc.offset) : nullptr;
      }

      // ── Decode (trusted) ──────────────────────────────────────────────

      template <Record T>
      void decode_struct(const struct_ref& s, T& out);

      template <typename E>
      void decode_list(const list_ref& l, E* out, std::size_t n);

      template <Pointer P>
      void decode_pointer(const std::uint8_t* msg, const std::uint8_t* slot, P& out)
      {
         if constexpr (std::is_same_v<P, std::string>)
            out = std::string(text_at(msg, slot));
         else if constexpr (is_vector_v<P>)
         {
            const auto l = list_at(msg, slot);
            out.resize(l.count);
            if constexpr (std::is_same_v<typename P::value_type, bool>)
            {
               for (std::uint32_t i = 0; i < l.count; ++i)
                  out[i] = ((l.data[i >> 3] >> (i & 7)) & 1) != 0;
            }
            else if (l.count != 0)
               decode_list<typename P::value_type>(l, out.data(), l.count);
         }
         else if constexpr (is_array_v<P>)
         {
            const auto l = list_at(msg, slot);
            out          = P{};
            decode_list<typename P::value_type>(l, out.data(),
                                                std::min<std::size_t>(l.count, out.size()));
         }
         else if constexpr (is_optional_v<P>)
         {
            if (load64(slot) == 0)
               out.reset();
            else
               decode_pointer(msg, slot, out.emplace());
         }
         else
         {
            out = P{};
            decode_struct(struct_at(msg, slot), out);
         }
      }

      template <typename E>
      void decode_list(const list_ref& l, E* out, std::size_t n)
      {
         if constexpr (std::is_same_v<E, bool>)
         {
            for (std::size_t i = 0; i < n; ++i)
               out[i] = ((l.data[i >> 3] >> (i & 7)) & 1) != 0;
         }
         else if constexpr (Scalar<E>)
         {
            if (n != 0)
               std::memcpy(static_cast<void*>(out), l.data, n * sizeof(E));
         }
         else if constexpr (Record<E>)
         {
            for (std::size_t i = 0; i < n; ++i)
               decode_struct(struct_ref{l.data + i * std::size_t{l.step}, l.msg,
                                        l.elem_data_bytes, l.elem_ptr_count},
                             out[i]);
         }
         else
         {
            static_assert(Pointer<E>, "psio::capnp: unsupported list element type");
            for (std::size_t i = 0; i < n; ++i)
               decode_pointer(l.msg, l.data + i * 8, out[i]);
         }
      }

      // Alternative `Alt` of a union member whose slot is `loc`.
      template <typename Alt>
      void decode_alt(const struct_ref& s, slot loc, Alt& out)
      {
         if constexpr (std::is_same_v<Alt, std::monostate>)
            ;
         else if constexpr (Scalar<Alt>)
            out = read_scalar<Alt>(s, loc, Alt{});
         else if (const auto* p = pointer_slot(s, loc))
            decode_pointer(s.msg, p, out);
      }

      template <Record T, std::size_t I>
      void decode_member(const struct_ref& s, T& out)
      {
         using L = layout<T>;
         using F = typename L::template member<I>;
         F& f    = out.*(::psio::reflect<T>::template member_pointer<I>);
         constexpr slot loc = L::field(I);
         if constexpr (is_variant_v<F>)
         {
            const std::uint16_t which = read_scalar<std::uint16_t>(s, loc, 0);
            [&]<std::size_t... As>(std::index_sequence<As...>) {
               (void)((which == As
                          ? (decode_alt(s, L::alt(I, As), f.template emplace<As>()), true)
                          : false) ||
                      ...);
            }(std::make_index_sequence<std::variant_size_v<F>>{});
         }
         else if constexpr (is_union_v<F>)  // optional scalar
         {
            if (read_scalar<std::uint16_t>(s, loc, 0) == 0)
               f.reset();
            else
               f = read_scalar<typename F::value_type>(s, L::alt(I, 1), typename F::value_type{});
         }
         else if constexpr (Scalar<F>)
            f = read_scalar<F>(s, loc, member_default<T, I>());
         else if (const auto* p = pointer_slot(s, loc))
            decode_pointer(s.msg, p, f);
      }

      template <Record T>
      void decode_struct(const struct_ref& s, T& out)
      {
         if (s.data == nullptr)
            return;  // a null struct is the default-constructed T
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (decode_member<T, Is>(s, out), ...);
         }(std::make_index_sequence<layout<T>::members>{});
      }

      // ── Encode ────────────────────────────────────────────────────────
      //
      // One segment, objects in the order libcapnp's builders allocate them
      // when a message is filled in member order: a struct, then for each
      // pointer member in turn the object it points to and everything below
      // that object.

      class builder
      {
         std::vector<char>& out_;

       public:
         static constexpr bool counts_only = false;

         explicit builder(std::vector<char>& out, std::size_t reserve_bytes) : out_(out)
         {
            out_.clear();
            out_.reserve(reserve_bytes);
            out_.resize(8);
         }
         // Allocates n zeroed words; their index.
         std::uint32_t alloc(std::uint32_t n)
         {
            const std::size_t at = (out_.size() - 8) / 8;
            out_.resize(out_.size() + std::size_t{n} * 8);
            return static_cast<std::uint32_t>(at);
         }
         std::uint8_t* bytes(std::uint32_t word) noexcept
         {
            return reinterpret_cast<std::uint8_t*>(out_.data()) + 8 + std::size_t{word} * 8;
         }
         void put(std::uint32_t word, std::uint64_t v) noexcept
         {
            std::memcpy(bytes(word), &v, 8);
         }
         void finish() noexcept
         {
            // One segment: [segment count - 1 = 0][its size in words].
            const std::uint32_t words = static_cast<std::uint32_t>((out_.size() - 8) / 8);
            std::memcpy(out_.data() + 4, &words, 4);
         }
         std::size_t size_bytes() const noexcept { return out_.size(); }
      };

      // The same walk, counting words.
      class counter
      {
         std::uint64_t words_ = 0;

       public:
         static constexpr bool counts_only = true;
         std::uint32_t         alloc(std::uint32_t n)
         {
            const auto at = words_;
            words_ += n;
            return static_cast<std::uint32_t>(at);
         }
         std::uint8_t* bytes(std::uint32_t) noexcept { return nullptr; }
         void          put(std::uint32_t, std::uint64_t) noexcept {}
         std::uint64_t words() const noexcept { return words_; }
      };

      inline constexpr std::uint32_t max_list_count = (std::uint32_t{1} << 29) - 1;

      [[noreturn]] inline void encode_too_large()
      {
         ::psio::codec_abort(codec_error{
            "capnp: value does not fit one segment (a list holds at most 2^29-1 elements or "
            "words; a segment at most 2^29 words)",
            0, "capnp"});
      }

      constexpr std::uint64_t struct_pointer(std::uint32_t at, std::uint32_t target, std::uint16_t dw,
                                             std::uint16_t pc) noexcept
      {
         const std::int32_t off = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(at) - 1;
         return (std::uint64_t{pc} << 48) | (std::uint64_t{dw} << 32) |
                (static_cast<std::uint32_t>(off) << 2);
      }
      constexpr std::uint64_t list_pointer(std::uint32_t at, std::uint32_t target, std::uint8_t code,
                                           std::uint32_t count) noexcept
      {
         const std::int32_t off = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(at) - 1;
         return (std::uint64_t{count} << 35) | (std::uint64_t{code} << 32) |
                (static_cast<std::uint32_t>(off) << 2) | 1u;
      }

      template <Record T, typename B>
      void encode_struct(B& b, std::uint32_t data_at, const T& v);

      // Writes the object `v` and the pointer to it at word `at`. An empty
      // string or list is a null pointer unless `keep_empty` (the payload
      // of an optional, where null means "no value").
      template <Pointer P, typename B>
      void encode_pointer(B& b, std::uint32_t at, const P& v, bool keep_empty = false);

      template <typename E, typename B>
      void encode_list(B& b, std::uint32_t at, const E* data, std::size_t n, bool keep_empty)
      {
         if (n == 0 && !keep_empty)
            return;
         if (n > max_list_count)
            encode_too_large();
         const std::uint32_t count = static_cast<std::uint32_t>(n);
         if constexpr (std::is_same_v<E, bool>)
         {
            const std::uint32_t target = b.alloc((count + 63) / 64);
            b.put(at, list_pointer(at, target, 1, count));
            if constexpr (!B::counts_only)
            {
               std::uint8_t* p = b.bytes(target);
               for (std::uint32_t i = 0; i < count; ++i)
                  if (data[i])
                     p[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
            }
         }
         else if constexpr (Scalar<E>)
         {
            const std::uint64_t bytes = std::uint64_t{count} * sizeof(E);
            const std::uint32_t target = b.alloc(static_cast<std::uint32_t>((bytes + 7) / 8));
            b.put(at, list_pointer(at, target, list_code<E>(), count));
            if constexpr (!B::counts_only)
               if (count != 0)
                  std::memcpy(b.bytes(target), static_cast<const void*>(data), bytes);
         }
         else if constexpr (Record<E>)
         {
            using L = layout<E>;
            const std::uint64_t words = std::uint64_t{count} * L::words;
            if (words > max_list_count)
               encode_too_large();
            const std::uint32_t tag = b.alloc(1 + static_cast<std::uint32_t>(words));
            b.put(at, list_pointer(at, tag, 7, static_cast<std::uint32_t>(words)));
            b.put(tag, (std::uint64_t{L::ptr_count} << 48) | (std::uint64_t{L::data_words} << 32) |
                          (std::uint64_t{count} << 2));
            for (std::uint32_t i = 0; i < count; ++i)
               encode_struct(b, tag + 1 + i * L::words, data[i]);
         }
         else
         {
            static_assert(Pointer<E>, "psio::capnp: unsupported list element type");
            const std::uint32_t target = b.alloc(count);
            b.put(at, list_pointer(at, target, 6, count));
            for (std::uint32_t i = 0; i < count; ++i)
               encode_pointer(b, target + i, data[i]);
         }
      }

      template <Pointer P, typename B>
      void encode_pointer(B& b, std::uint32_t at, const P& v, bool keep_empty)
      {
         if constexpr (std::is_same_v<P, std::string>)
         {
            if (v.empty() && !keep_empty)
               return;
            if (v.size() >= max_list_count)
               encode_too_large();
            const std::uint32_t count  = static_cast<std::uint32_t>(v.size()) + 1;  // + NUL
            const std::uint32_t target = b.alloc((count + 7) / 8);
            b.put(at, list_pointer(at, target, 2, count));
            if constexpr (!B::counts_only)
               std::memcpy(b.bytes(target), v.data(), v.size());
         }
         else if constexpr (is_vector_v<P>)
         {
            if constexpr (std::is_same_v<typename P::value_type, bool>)
            {
               // std::vector<bool> has no contiguous bools.
               if (v.empty() && !keep_empty)
                  return;
               if (v.size() > max_list_count)
                  encode_too_large();
               const std::uint32_t count  = static_cast<std::uint32_t>(v.size());
               const std::uint32_t target = b.alloc((count + 63) / 64);
               b.put(at, list_pointer(at, target, 1, count));
               if constexpr (!B::counts_only)
               {
                  std::uint8_t* p = b.bytes(target);
                  for (std::uint32_t i = 0; i < count; ++i)
                     if (v[i])
                        p[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
               }
            }
            else
               encode_list<typename P::value_type>(b, at, v.data(), v.size(), keep_empty);
         }
         else if constexpr (is_array_v<P>)
            encode_list<typename P::value_type>(b, at, v.data(), v.size(), keep_empty);
         else if constexpr (is_optional_v<P>)
         {
            if (v.has_value())
               encode_pointer(b, at, *v, true);
         }
         else
         {
            using L = layout<P>;
            if constexpr (L::words == 0)
               b.put(at, 0xfffffffcull);  // a zero-sized struct: offset -1
            else
            {
               const std::uint32_t data_at = b.alloc(L::words);
               b.put(at, struct_pointer(at, data_at, L::data_words, L::ptr_count));
               encode_struct(b, data_at, v);
            }
         }
      }

      template <Scalar S, typename B>
      [[gnu::always_inline]] inline void write_scalar(B& b, std::uint32_t data_at, slot loc, S v,
                                                      S def) noexcept
      {
         if constexpr (!B::counts_only)
         {
            if constexpr (std::is_same_v<S, bool>)
            {
               if (v != def)
                  b.bytes(data_at)[loc.byte()] |= static_cast<std::uint8_t>(1u << loc.bit());
            }
            else
            {
               const bits_t<S> wire = static_cast<bits_t<S>>(to_bits(v) ^ to_bits(def));
               std::memcpy(b.bytes(data_at) + loc.byte(), &wire, sizeof(S));
            }
         }
      }

      template <typename Alt, typename B>
      void encode_alt(B& b, std::uint32_t data_at, std::uint32_t ptrs_at, slot loc, const Alt& v)
      {
         if constexpr (std::is_same_v<Alt, std::monostate>)
            ;
         else if constexpr (Scalar<Alt>)
            write_scalar<Alt>(b, data_at, loc, v, Alt{});
         else
            encode_pointer(b, ptrs_at + loc.offset, v);
      }

      template <Record T, std::size_t I, typename B>
      void encode_member(B& b, std::uint32_t data_at, const T& v)
      {
         using L = layout<T>;
         using F = typename L::template member<I>;
         const F& f = v.*(::psio::reflect<T>::template member_pointer<I>);
         constexpr slot      loc     = L::field(I);
         const std::uint32_t ptrs_at = data_at + L::data_words;
         if constexpr (is_variant_v<F>)
         {
            if (f.valueless_by_exception())
               return;
            write_scalar<std::uint16_t>(b, data_at, loc, static_cast<std::uint16_t>(f.index()), 0);
            [&]<std::size_t... As>(std::index_sequence<As...>) {
               (void)((f.index() == As
                          ? (encode_alt(b, data_at, ptrs_at, L::alt(I, As), *std::get_if<As>(&f)),
                             true)
                          : false) ||
                      ...);
            }(std::make_index_sequence<std::variant_size_v<F>>{});
         }
         else if constexpr (is_union_v<F>)  // optional scalar
         {
            if (f.has_value())
            {
               write_scalar<std::uint16_t>(b, data_at, loc, 1, 0);
               write_scalar<typename F::value_type>(b, data_at, L::alt(I, 1), *f,
                                                    typename F::value_type{});
            }
         }
         else if constexpr (Scalar<F>)
            write_scalar<F>(b, data_at, loc, f, member_default<T, I>());
         else
            encode_pointer(b, ptrs_at + loc.offset, f);
      }

      template <Record T, typename B>
      void encode_struct(B& b, std::uint32_t data_at, const T& v)
      {
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (encode_member<T, Is>(b, data_at, v), ...);
         }(std::make_index_sequence<layout<T>::members>{});
      }

      // Root pointer, root struct, everything below it.
      template <Record T, typename B>
      void encode_message(B& b, const T& v)
      {
         using L = layout<T>;
         const std::uint32_t root = b.alloc(1);
         if constexpr (L::words == 0)
            b.put(root, 0xfffffffcull);
         else
         {
            const std::uint32_t data_at = b.alloc(L::words);
            b.put(root, struct_pointer(root, data_at, L::data_words, L::ptr_count));
            encode_struct(b, data_at, v);
         }
      }

      template <Record T>
      std::uint64_t message_words(const T& v)
      {
         counter c;
         encode_message(c, v);
         return c.words();
      }

      template <Record T>
      void encode_into(const T& v, std::vector<char>& out)
      {
         const std::uint64_t words = message_words(v);
         if (words > (std::uint64_t{1} << 29))
            encode_too_large();
         builder b{out, 8 + static_cast<std::size_t>(words) * 8};
         encode_message(b, v);
         b.finish();
      }

      // ── Validation (typed) ────────────────────────────────────────────

      struct limits
      {
         std::uint64_t traversal_words = 8ull * 1024 * 1024;  // the official default (64 MiB)
         std::uint32_t nesting         = 64;                  // the official default
      };

      struct checker
      {
         const std::uint8_t* msg;
         std::size_t         len;
         std::uint32_t       segments;
         std::uint64_t       budget;
         std::uint32_t       max_depth;
         const char*         error = nullptr;
         std::size_t         at    = 0;

         bool fail(const char* what, const std::uint8_t* where) noexcept
         {
            if (!error)
            {
               error = what;
               at    = static_cast<std::size_t>(where - msg);
            }
            return false;
         }
         bool spend(std::uint64_t words, const std::uint8_t* where) noexcept
         {
            if (words > budget)
               return fail("capnp: message exceeds the traversal limit", where);
            budget -= words;
            return true;
         }
      };

      // A pointer followed with every step checked: the describing word,
      // the object's first byte, and the segment the object must lie in.
      struct checked_target
      {
         std::uint64_t                 word   = 0;
         const std::uint8_t*           target = nullptr;
         std::span<const std::uint8_t> seg;
      };

      inline bool follow(checker& c, std::span<const std::uint8_t> seg, const std::uint8_t* slot,
                         std::uint64_t w, checked_target& out) noexcept
      {
         const std::uint32_t kind = static_cast<std::uint32_t>(w) & 3;
         if (kind == 3)
            return c.fail("capnp: capability pointers are not supported", slot);
         if (kind != 2)
         {
            // target = slot + 8 + offset words, within [seg.begin, seg.end].
            const std::int64_t at = (slot - seg.data()) / 8 + 1 + std::int64_t{pointer_offset(w)};
            if (at < 0 || static_cast<std::uint64_t>(at) > seg.size() / 8)
               return c.fail("capnp: pointer target outside its segment", slot);
            out = {w, seg.data() + at * 8, seg};
            return true;
         }
         const std::uint32_t id = static_cast<std::uint32_t>(w >> 32);
         if (id >= c.segments)
            return c.fail("capnp: far pointer to a segment the message does not have", slot);
         const auto          far_seg   = segment_of(c.msg, id);
         const std::uint64_t pad_word  = static_cast<std::uint32_t>(w) >> 3;
         const bool          two_words = (w & 4) != 0;
         if (pad_word + (two_words ? 2 : 1) > far_seg.size() / 8)
            return c.fail("capnp: far pointer landing pad outside its segment", slot);
         const std::uint8_t* pad = far_seg.data() + pad_word * 8;
         const std::uint64_t w2  = load64(pad);
         if (!two_words)
         {
            if ((w2 & 3) >= 2)
               return c.fail("capnp: far pointer landing pad is not a struct or list pointer", pad);
            if (w2 == 0)
            {
               out = {0, nullptr, far_seg};
               return true;
            }
            const std::int64_t at = static_cast<std::int64_t>(pad_word) + 1 + pointer_offset(w2);
            if (at < 0 || static_cast<std::uint64_t>(at) > far_seg.size() / 8)
               return c.fail("capnp: pointer target outside its segment", pad);
            out = {w2, far_seg.data() + at * 8, far_seg};
            return true;
         }
         // Double-far: [far pointer to the object][tag with offset 0].
         if ((w2 & 7) != 2)
            return c.fail("capnp: double-far landing pad does not start with a far pointer", pad);
         const std::uint32_t id2 = static_cast<std::uint32_t>(w2 >> 32);
         if (id2 >= c.segments)
            return c.fail("capnp: far pointer to a segment the message does not have", pad);
         const auto          seg2 = segment_of(c.msg, id2);
         const std::uint64_t at   = static_cast<std::uint32_t>(w2) >> 3;
         if (at > seg2.size() / 8)
            return c.fail("capnp: pointer target outside its segment", pad);
         const std::uint64_t tag = load64(pad + 8);
         if ((tag & 3) >= 2 || pointer_offset(tag) != 0)
            return c.fail("capnp: double-far landing pad tag is not a struct or list tag", pad + 8);
         out = {tag, seg2.data() + at * 8, seg2};
         return true;
      }

      inline bool fits(const checked_target& t, std::uint64_t bytes) noexcept
      {
         return bytes <= static_cast<std::uint64_t>(t.seg.data() + t.seg.size() - t.target);
      }

      template <Record T>
      bool check_struct(checker& c, std::span<const std::uint8_t> seg, const struct_ref& s,
                        std::uint32_t depth) noexcept;

      template <typename E>
      bool check_list(checker& c, const checked_target& t, const std::uint8_t* slot,
                      std::optional<std::size_t> exact_count, std::uint32_t depth) noexcept;

      template <Pointer P>
      bool check_pointer(checker& c, std::span<const std::uint8_t> seg, const std::uint8_t* slot,
                         std::uint32_t depth) noexcept
      {
         const std::uint64_t w = load64(slot);
         if (w == 0)
            return true;
         if constexpr (is_optional_v<P>)
            return check_pointer<typename P::value_type>(c, seg, slot, depth);
         else
         {
            if (depth >= c.max_depth)
               return c.fail("capnp: message is nested too deeply", slot);
            checked_target t;
            if (!follow(c, seg, slot, w, t))
               return false;
            if (t.target == nullptr)
               return true;  // a far pointer to a null pointer
            const std::uint32_t kind = static_cast<std::uint32_t>(t.word) & 3;
            if constexpr (std::is_same_v<P, std::string>)
            {
               const std::uint32_t n = static_cast<std::uint32_t>(t.word >> 35);
               if (kind != 1 || ((t.word >> 32) & 7) != 2)
                  return c.fail("capnp: expected Text (a list of bytes)", slot);
               if (n == 0 || !fits(t, n))
                  return c.fail("capnp: Text outside its segment or empty", slot);
               if (t.target[n - 1] != 0)
                  return c.fail("capnp: Text is not NUL-terminated", slot);
               return c.spend((std::uint64_t{n} + 7) / 8, slot);
            }
            else if constexpr (is_vector_v<P>)
            {
               if (kind != 1)
                  return c.fail("capnp: expected a list pointer", slot);
               return check_list<typename P::value_type>(c, t, slot, std::nullopt, depth);
            }
            else if constexpr (is_array_v<P>)
            {
               if (kind != 1)
                  return c.fail("capnp: expected a list pointer", slot);
               return check_list<typename P::value_type>(c, t, slot, std::tuple_size_v<P>, depth);
            }
            else
            {
               if (kind != 0)
                  return c.fail("capnp: expected a struct pointer", slot);
               const std::uint32_t dw = static_cast<std::uint32_t>(t.word >> 32) & 0xffff;
               const std::uint32_t pc = static_cast<std::uint32_t>(t.word >> 48);
               if (!fits(t, (std::uint64_t{dw} + pc) * 8))
                  return c.fail("capnp: struct outside its segment", slot);
               if (!c.spend(std::max<std::uint64_t>(std::uint64_t{dw} + pc, 1), slot))
                  return false;
               return check_struct<P>(
                  c, t.seg, struct_ref{t.target, c.msg, dw * 8, static_cast<std::uint16_t>(pc)},
                  depth + 1);
            }
         }
      }

      template <typename E>
      bool check_list(checker& c, const checked_target& t, const std::uint8_t* slot,
                      std::optional<std::size_t> exact_count, std::uint32_t depth) noexcept
      {
         const std::uint32_t code = static_cast<std::uint32_t>(t.word >> 32) & 7;
         const std::uint32_t n    = static_cast<std::uint32_t>(t.word >> 35);
         if constexpr (Record<E>)
         {
            if (code != 7)
               return c.fail("capnp: expected a list of structs (inline composite)", slot);
            if (!fits(t, (std::uint64_t{n} + 1) * 8))
               return c.fail("capnp: list outside its segment", slot);
            const std::uint64_t tag = load64(t.target);
            if ((tag & 3) != 0)
               return c.fail("capnp: composite list tag is not a struct tag", t.target);
            const std::uint32_t count = static_cast<std::uint32_t>(tag) >> 2;
            const std::uint32_t dw    = static_cast<std::uint32_t>(tag >> 32) & 0xffff;
            const std::uint32_t pc    = static_cast<std::uint32_t>(tag >> 48);
            if (std::uint64_t{count} * (dw + pc) > n)
               return c.fail("capnp: composite list elements overrun the list", t.target);
            if (exact_count && *exact_count != count)
               return c.fail("capnp: list length differs from the fixed length of the array", slot);
            // A list of zero-sized structs still costs a word per element.
            if (!c.spend(std::uint64_t{n} + 1 + (dw + pc == 0 ? count : 0), slot))
               return false;
            for (std::uint32_t i = 0; i < count; ++i)
               if (!check_struct<E>(c, t.seg,
                                    struct_ref{t.target + 8 + std::size_t{i} * (dw + pc) * 8, c.msg,
                                               dw * 8, static_cast<std::uint16_t>(pc)},
                                    depth + 1))
                  return false;
            return true;
         }
         else
         {
            if (exact_count && *exact_count != n)
               return c.fail("capnp: list length differs from the fixed length of the array", slot);
            if constexpr (Scalar<E>)
            {
               if (code != list_code<E>())
                  return c.fail("capnp: list element size differs from the element type", slot);
               const std::uint64_t bytes =
                  std::is_same_v<E, bool> ? (std::uint64_t{n} + 7) / 8 : std::uint64_t{n} * sizeof(E);
               if (!fits(t, bytes))
                  return c.fail("capnp: list outside its segment", slot);
               return c.spend((bytes + 7) / 8, slot);
            }
            else
            {
               static_assert(Pointer<E>, "psio::capnp: unsupported list element type");
               if (code != 6)
                  return c.fail("capnp: expected a list of pointers", slot);
               if (!fits(t, std::uint64_t{n} * 8))
                  return c.fail("capnp: list outside its segment", slot);
               if (!c.spend(n, slot))
                  return false;
               for (std::uint32_t i = 0; i < n; ++i)
                  if (!check_pointer<E>(c, t.seg, t.target + std::size_t{i} * 8, depth + 1))
                     return false;
               return true;
            }
         }
      }

      template <typename Alt>
      bool check_alt(checker& c, std::span<const std::uint8_t> seg, const struct_ref& s, slot loc,
                     std::uint32_t depth) noexcept
      {
         if constexpr (Pointer<Alt>)
         {
            if (const auto* p = pointer_slot(s, loc))
               return check_pointer<Alt>(c, seg, p, depth);
         }
         return true;
      }

      template <Record T, std::size_t I>
      bool check_member(checker& c, std::span<const std::uint8_t> seg, const struct_ref& s,
                        std::uint32_t depth) noexcept
      {
         using L = layout<T>;
         using F = typename L::template member<I>;
         constexpr slot loc = L::field(I);
         if constexpr (is_variant_v<F>)
         {
            const std::uint16_t which = read_scalar<std::uint16_t>(s, loc, 0);
            if (which >= std::variant_size_v<F>)
               return c.fail("capnp: union discriminant names a member this type does not have",
                             s.data);
            bool ok = true;
            [&]<std::size_t... As>(std::index_sequence<As...>) {
               (void)((which == As ? (ok = check_alt<std::variant_alternative_t<As, F>>(
                                         c, seg, s, L::alt(I, As), depth),
                                      true)
                                   : false) ||
                      ...);
            }(std::make_index_sequence<std::variant_size_v<F>>{});
            return ok;
         }
         else if constexpr (is_union_v<F>)
         {
            if (read_scalar<std::uint16_t>(s, loc, 0) > 1)
               return c.fail("capnp: union discriminant names a member this type does not have",
                             s.data);
            return true;
         }
         else if constexpr (Scalar<F>)
            return true;
         else
         {
            if (const auto* p = pointer_slot(s, loc))
               return check_pointer<F>(c, seg, p, depth);
            return true;
         }
      }

      template <Record T>
      bool check_struct(checker& c, std::span<const std::uint8_t> seg, const struct_ref& s,
                        std::uint32_t depth) noexcept
      {
         return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return (check_member<T, Is>(c, seg, s, depth) && ...);
         }(std::make_index_sequence<layout<T>::members>{});
      }

      // The size in bytes of the framed message at the start of `bytes`
      // (segment table and segments), or 0 if the table is malformed or
      // the bytes are shorter than it says.
      inline std::size_t framed_size(std::span<const char> bytes) noexcept
      {
         const auto* msg = reinterpret_cast<const std::uint8_t*>(bytes.data());
         if (bytes.size() < 8)
            return 0;
         const std::uint64_t n = std::uint64_t{load32(msg)} + 1;
         if (n > max_segments || table_bytes(n) > bytes.size())
            return 0;
         std::uint64_t total = table_bytes(n);
         for (std::uint64_t i = 0; i < n; ++i)
            total += std::uint64_t{load32(msg + 4 + 4 * i)} * 8;
         return total <= bytes.size() ? static_cast<std::size_t>(total) : 0;
      }

      template <Record T>
      codec_status validate_message(std::span<const char> bytes, limits lim = {}) noexcept
      {
         const std::size_t size = framed_size(bytes);
         if (size == 0)
            return codec_fail("capnp: malformed or truncated segment table", 0, "capnp");
         if (size != bytes.size())
            return codec_fail("capnp: bytes after the end of the message",
                              static_cast<std::uint32_t>(size), "capnp");
         const auto* msg = reinterpret_cast<const std::uint8_t*>(bytes.data());
         checker     c{msg, bytes.size(), load32(msg) + 1, lim.traversal_words, lim.nesting};
         const auto  seg0 = segment_of(msg, 0);
         if (seg0.size() < 8)
            return codec_fail("capnp: message has no root pointer", 0, "capnp");
         if (!check_pointer<T>(c, seg0, seg0.data(), 0))
            return codec_fail(c.error, static_cast<std::uint32_t>(c.at), "capnp");
         // The message is one T as Cap'n Proto defines it. What T declares
         // about its values — member bounds, utf8, order, invariants
         // (value_contract.hpp) — is checked on the value the message
         // decodes to; a type that declares nothing is done here.
         if constexpr (::psio::has_value_contract_v<T>)
         {
            T out{};
            decode_struct(root_of(bytes), out);
            return ::psio::check_value_contract(out, "capnp: member length_bound violated",
                                                "capnp");
         }
         else
            return codec_ok();
      }

   }  // namespace detail::capnp_impl

   // ── Format tag ──────────────────────────────────────────────────────────

   struct capnp : format_tag_base<capnp>
   {
      static constexpr const char*       name         = "capnp";
      static constexpr trusted_read_kind trusted_read = trusted_read_kind::validated_bytes_only;
      static constexpr sink_write_kind   sink_write   = sink_write_kind::replaces;
      using preferred_presentation_category = ::psio::binary_category;

      // The limits `validate` applies; the official reader's defaults.
      using limits = detail::capnp_impl::limits;

      // The struct layout of T: `layout<T>::data_words`, `::ptr_count`, and
      // the slot of each member — what `capnp compile` computes for the
      // schema of T.
      template <typename T>
      using layout = detail::capnp_impl::layout<T>;

      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode), capnp, const T& v)
      {
         std::vector<char> out;
         detail::capnp_impl::encode_into(v, out);
         return out;
      }

      // Sink form: replaces the contents of `sink`, reusing its capacity.
      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend void tag_invoke(decltype(::psio::encode), capnp, const T& v, std::vector<char>& sink)
      {
         detail::capnp_impl::encode_into(v, sink);
      }

      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend T tag_invoke(decltype(::psio::decode<T>), capnp, T*, std::span<const char> bytes)
      {
         T out{};
         detail::capnp_impl::decode_struct(detail::capnp_impl::root_of(bytes), out);
         return out;
      }

      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), capnp, const T& v)
      {
         return 8 + static_cast<std::size_t>(detail::capnp_impl::message_words(v)) * 8;
      }

      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>), capnp, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(), "capnp"); !st.ok())
            return st;
         return detail::capnp_impl::validate_message<T>(bytes);
      }

      // `validate` (the structure, then what T declares about its values:
      // bounds, utf8, order, invariants), then the specs of user code that
      // have a `validate(span)` of their own, on the decoded value.
      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>), capnp, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = detail::capnp_impl::validate_message<T>(bytes); !st.ok())
            return st;
         T out{};
         detail::capnp_impl::decode_struct(detail::capnp_impl::root_of(bytes), out);
         return ::psio::validate_specs_on_value(out);
      }

      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>), capnp, T*,
                                           std::span<const char> bytes) noexcept
      {
         auto v = std::make_unique<T>();
         detail::capnp_impl::decode_struct(detail::capnp_impl::root_of(bytes), *v);
         return v;
      }
   };

   // `validate_capnp<T>(bytes, limits)`: the typed check under limits other
   // than the official defaults.
   template <typename T>
      requires detail::capnp_impl::Record<T>
   [[nodiscard]] codec_status validate_capnp(std::span<const char> bytes, capnp::limits lim) noexcept
   {
      return detail::capnp_impl::validate_message<T>(bytes, lim);
   }

   // The size of the framed message at the start of `bytes` (0: malformed
   // or truncated): where the next message of a stream begins.
   [[nodiscard]] inline std::size_t capnp_message_size(std::span<const char> bytes) noexcept
   {
      return detail::capnp_impl::framed_size(bytes);
   }

   PSIO_FORMAT_HELPERS(capnp)

}  // namespace psio
