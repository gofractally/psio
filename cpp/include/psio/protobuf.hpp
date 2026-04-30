#pragma once
//
// psio/protobuf.hpp — Schema-driven Protocol Buffers codec.
//
// Protobuf is the canonical "schema-required, length-delimited
// tag-and-value" wire format.  Each field is encoded as
//
//     varint(tag = (field_number << 3) | wire_type)
//     <payload, depending on wire_type>
//
// where `wire_type ∈ {0=varint, 1=fixed64, 2=length-delimited,
// 5=fixed32}`.  Field numbers — NOT positions, NOT names — are the
// stable wire identity, so old senders / new receivers can interop
// across schema evolution as long as the field number stays put.
//
// Field-number assignment:
//
//   Pulled from psio's existing reflection annotation system — no
//   protobuf-specific knob.  By default, the N-th field in
//   declaration order gets field_number N+1 (this is what
//   `reflect<T>::field_number<I>` returns out of the box).  Override
//   per-field with the canonical `attr(member, field<N>)` form:
//
//       struct MyMsg {
//          std::int32_t old_field;
//          std::int32_t new_field;
//       };
//       PSIO_REFLECT(MyMsg, attr(old_field, field<3>),
//                           attr(new_field, field<7>))
//
//   The codec calls `find_spec<field_num_spec>` on the field's
//   effective annotations and falls back to declaration order when
//   none is set — same behaviour every other format consults
//   `field<N>` for, just consumed by protobuf's
//   field-number-on-the-wire mechanic.
//
// Per-field integer-encoding overrides:
//
//   `attr(member, pb_fixed)`  — encode integral fields as fixed-width
//                                (wire_type 1/5).  Maps to .proto's
//                                fixed32/sfixed32/fixed64/sfixed64
//                                depending on the C++ type's size and
//                                signedness.  Default for floats/doubles
//                                already, so this is a no-op there.
//   `attr(member, pb_sint)`   — encode signed integers with zigzag
//                                varint (wire_type 0).  Maps to
//                                .proto's sint32/sint64.  Roughly half
//                                the bytes of the default for small
//                                negatives.
//   Both apply per-element to vector<int>; tag wire_type is adjusted
//   for fixed, varint stays for zigzag.
//
// Type mapping (C++ → wire_type, default integer encoding):
//
//   bool / [u]int{8,16,32,64} / enum  → varint     (wire_type 0)
//   float                              → fixed32    (wire_type 5)
//   double                             → fixed64    (wire_type 1)
//   std::string / std::string_view     → length-delim (wire_type 2)
//   std::vector<uint8_t> / std::byte   → length-delim (bytes)
//   std::vector<scalar>                → length-delim, PACKED
//                                        (wire_type 2 — concatenated
//                                        varint or fixed payload, no
//                                        per-element tag)
//   std::vector<std::string>           → REPEATED, unpacked (one
//                                        wire_type-2 record per
//                                        element)
//   std::vector<message>               → REPEATED, unpacked
//   std::optional<T>                   → field is omitted when
//                                        empty; present otherwise
//   PSIO_REFLECT'd struct              → length-delim sub-message
//
// Out of scope for the first cut (every entry below has a clear
// landing spot — sint32/sint64 zigzag, fixed-width ints, map<K,V>,
// std::variant→oneof — all annotation-driven additions to the
// dispatch).  Unknown fields are dropped on decode (a strict mode
// is a separate hook).

#include <psio/adapter.hpp>
#include <psio/annotate.hpp>
#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>
#include <psio/varint/leb128.hpp>
#include <psio/wrappers.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psio
{

   //  Per-field integer-encoding annotations (protobuf-specific).
   //  Spec types live in the `psio` namespace so `attr(field, pb_fixed)`
   //  and `attr(field, pb_sint)` route through the canonical
   //  effective_annotations_for_v path used by every other format.
   //
   //  pb_fixed   — encode integral fields as fixed32 / fixed64 (or
   //               sfixed32 / sfixed64 for signed types) instead of
   //               varint.  Wire-type 1 or 5.
   //  pb_sint    — encode signed integral fields with zigzag varint.
   //               Wire-type 0.  Negatives encode in 1–10 bytes
   //               instead of always 10.
   struct pb_fixed_spec
   {
      using spec_category = static_spec_tag;
      constexpr bool operator==(const pb_fixed_spec&) const = default;
   };
   struct pb_sint_spec
   {
      using spec_category = static_spec_tag;
      constexpr bool operator==(const pb_sint_spec&) const = default;
   };
   inline constexpr pb_fixed_spec pb_fixed{};
   inline constexpr pb_sint_spec  pb_sint{};

   namespace detail::pb_impl
   {
      enum class pb_int_enc : std::uint8_t { default_, fixed, zigzag };

      template <typename T, std::size_t I>
      constexpr pb_int_enc resolve_int_encoding() noexcept
      {
         using R          = ::psio::reflect<T>;
         using FieldT     = typename R::template member_type<I>;
         constexpr auto P = R::template member_pointer<I>;
         using Anns       = std::remove_cvref_t<
            decltype(::psio::effective_annotations_for_v<T, FieldT, P>)>;
         if constexpr (::psio::has_spec_v<::psio::pb_fixed_spec, Anns>)
            return pb_int_enc::fixed;
         else if constexpr (::psio::has_spec_v<::psio::pb_sint_spec, Anns>)
            return pb_int_enc::zigzag;
         else
            return pb_int_enc::default_;
      }

      // ── Wire-type constants ─────────────────────────────────────
      namespace wt
      {
         constexpr std::uint8_t varint        = 0;
         constexpr std::uint8_t fixed64       = 1;
         constexpr std::uint8_t length_delim  = 2;
         constexpr std::uint8_t fixed32       = 5;
      }  // namespace wt

      // ── varint = LEB128 (matches the protobuf wire format) ─────
      //
      //  Decode goes through psio::varint::leb128::fast::decode_u64,
      //  which picks NEON on aarch64, SSE/SWAR on x86_64, and scalar
      //  elsewhere; ~2x the throughput of the byte-at-a-time loop on
      //  long varints, and zero overhead on the common 1-byte tags
      //  since the fast path checks `avail < 16` and falls back to
      //  scalar for short reads.
      //
      //  Encode is left scalar: the leb128 library's measurements
      //  (varint/leb128.hpp:248) show the encode side is dominated
      //  by the writer's store + advance, with vectorised packing
      //  pulled from below the level the wire-rate measurements care
      //  about.  Keeping the scalar form here avoids dragging
      //  alignment/padding concerns into the protobuf write path.

      inline std::size_t varint_size(std::uint64_t v) noexcept
      {
         return ::psio::varint::leb128::scalar::size_u64(v);
      }

      template <typename Sink>
      inline void emit_varint(std::uint64_t v, Sink& s)
      {
         std::uint8_t buf[10];
         std::size_t  n =
            ::psio::varint::leb128::scalar::encode_u64(buf, v);
         s.write(buf, n);
      }

      inline std::uint64_t read_varint(std::span<const char> src,
                                       std::size_t&          pos)
      {
         //  fast::decode_u64 returns {.value, .size, .ok}.  Up to 16
         //  bytes ahead are read by the SIMD path so the bounds
         //  check is per-call, not per-byte.
         auto avail = src.size() - pos;
         auto r     = ::psio::varint::leb128::fast::decode_u64(
            reinterpret_cast<const std::uint8_t*>(src.data() + pos),
            avail);
         if (!r.ok)
            throw std::runtime_error("protobuf: malformed varint");
         pos += r.len;
         return r.value;
      }

      // Tag = (field_number << 3) | wire_type, varint-encoded.
      template <typename Sink>
      inline void emit_tag(std::uint32_t field, std::uint8_t wt, Sink& s)
      {
         emit_varint((static_cast<std::uint64_t>(field) << 3) | wt, s);
      }

      inline std::size_t tag_size(std::uint32_t field, std::uint8_t /*wt*/) noexcept
      {
         return varint_size(static_cast<std::uint64_t>(field) << 3);
      }

      // ── Endian-fixed payload helpers ────────────────────────────
      template <typename T>
         requires std::is_trivially_copyable_v<T>
      inline T to_le(T v) noexcept
      {
         if constexpr (sizeof(T) == 1 ||
                       std::endian::native == std::endian::little)
            return v;
         else
         {
            unsigned char buf[sizeof(T)];
            std::memcpy(buf, &v, sizeof(T));
            for (std::size_t i = 0; i < sizeof(T) / 2; ++i)
               std::swap(buf[i], buf[sizeof(T) - 1 - i]);
            T out;
            std::memcpy(&out, buf, sizeof(T));
            return out;
         }
      }

      // ── std-container detectors ─────────────────────────────────
      template <typename T>
      struct pb_is_vector : std::false_type {};
      template <typename E, typename A>
      struct pb_is_vector<std::vector<E, A>> : std::true_type
      {
         using elem = E;
      };

      template <typename T>
      struct pb_is_optional : std::false_type {};
      template <typename T>
      struct pb_is_optional<std::optional<T>> : std::true_type
      {
         using elem = T;
      };

      template <typename T>
      constexpr bool is_byte_vector_v =
         std::is_same_v<T, std::vector<std::uint8_t>> ||
         std::is_same_v<T, std::vector<std::byte>>;

      template <typename T>
      constexpr bool is_packed_scalar_v =
         std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;

      // ── pb_int_enc helpers ──────────────────────────────────────
      //
      //  Wire-type, value-size, write, and read for integral payloads
      //  under each encoding choice.  Floating-point types ignore Enc:
      //  fixed-width is already their default and zigzag is undefined.
      //  Bool also ignores Enc.

      template <typename U, pb_int_enc E>
      constexpr std::uint8_t pb_int_wire_type() noexcept
      {
         if constexpr (E == pb_int_enc::fixed)
            return sizeof(U) <= 4 ? wt::fixed32 : wt::fixed64;
         else
            return wt::varint;
      }

      template <pb_int_enc E, typename U>
      inline std::size_t pb_int_value_size(U v) noexcept
      {
         if constexpr (E == pb_int_enc::fixed)
            return sizeof(U) <= 4 ? 4u : 8u;
         else if constexpr (E == pb_int_enc::zigzag)
         {
            //  zigzag: (n << 1) ^ (n >> bw-1).  Cast through int64 so
            //  signed shifts behave consistently across narrower types.
            auto s = static_cast<std::int64_t>(v);
            auto u = (static_cast<std::uint64_t>(s) << 1) ^
                     static_cast<std::uint64_t>(s >> 63);
            return varint_size(u);
         }
         else
         {
            if constexpr (std::is_signed_v<U>)
            {
               auto s = static_cast<std::int64_t>(v);
               return varint_size(static_cast<std::uint64_t>(s));
            }
            else
               return varint_size(static_cast<std::uint64_t>(v));
         }
      }

      template <pb_int_enc E, typename U, typename Sink>
      inline void pb_int_write_value(U v, Sink& s)
      {
         if constexpr (E == pb_int_enc::fixed)
         {
            if constexpr (sizeof(U) <= 4)
            {
               std::uint32_t bits = 0;
               if constexpr (std::is_signed_v<U>)
               {
                  std::int32_t sv = static_cast<std::int32_t>(v);
                  std::memcpy(&bits, &sv, sizeof(bits));
               }
               else
                  bits = static_cast<std::uint32_t>(v);
               std::uint32_t le = to_le(bits);
               s.write(&le, sizeof(le));
            }
            else
            {
               std::uint64_t bits = 0;
               if constexpr (std::is_signed_v<U>)
               {
                  std::int64_t sv = static_cast<std::int64_t>(v);
                  std::memcpy(&bits, &sv, sizeof(bits));
               }
               else
                  bits = static_cast<std::uint64_t>(v);
               std::uint64_t le = to_le(bits);
               s.write(&le, sizeof(le));
            }
         }
         else if constexpr (E == pb_int_enc::zigzag)
         {
            auto sv = static_cast<std::int64_t>(v);
            auto u  = (static_cast<std::uint64_t>(sv) << 1) ^
                     static_cast<std::uint64_t>(sv >> 63);
            emit_varint(u, s);
         }
         else
         {
            if constexpr (std::is_signed_v<U>)
            {
               auto sv = static_cast<std::int64_t>(v);
               emit_varint(static_cast<std::uint64_t>(sv), s);
            }
            else
               emit_varint(static_cast<std::uint64_t>(v), s);
         }
      }

      template <pb_int_enc E, typename U>
      inline U pb_int_read_value(std::span<const char> src,
                                 std::size_t&          pos,
                                 std::uint8_t          wire_type)
      {
         if constexpr (E == pb_int_enc::fixed)
         {
            if constexpr (sizeof(U) <= 4)
            {
               if (wire_type != wt::fixed32)
                  throw std::runtime_error(
                     "protobuf: fixed32 wire-type mismatch");
               if (pos + 4 > src.size())
                  throw std::runtime_error(
                     "protobuf: truncated fixed32 int");
               std::uint32_t bits;
               std::memcpy(&bits, src.data() + pos, 4);
               bits = to_le(bits);
               pos += 4;
               U out;
               if constexpr (std::is_signed_v<U>)
               {
                  std::int32_t sv;
                  std::memcpy(&sv, &bits, 4);
                  out = static_cast<U>(sv);
               }
               else
                  out = static_cast<U>(bits);
               return out;
            }
            else
            {
               if (wire_type != wt::fixed64)
                  throw std::runtime_error(
                     "protobuf: fixed64 wire-type mismatch");
               if (pos + 8 > src.size())
                  throw std::runtime_error(
                     "protobuf: truncated fixed64 int");
               std::uint64_t bits;
               std::memcpy(&bits, src.data() + pos, 8);
               bits = to_le(bits);
               pos += 8;
               U out;
               if constexpr (std::is_signed_v<U>)
               {
                  std::int64_t sv;
                  std::memcpy(&sv, &bits, 8);
                  out = static_cast<U>(sv);
               }
               else
                  out = static_cast<U>(bits);
               return out;
            }
         }
         else if constexpr (E == pb_int_enc::zigzag)
         {
            if (wire_type != wt::varint)
               throw std::runtime_error(
                  "protobuf: sint wire-type mismatch");
            std::uint64_t u = read_varint(src, pos);
            std::int64_t  s =
               static_cast<std::int64_t>((u >> 1) ^ (~(u & 1) + 1));
            return static_cast<U>(s);
         }
         else
         {
            if (wire_type != wt::varint)
               throw std::runtime_error(
                  "protobuf: int wire-type mismatch");
            std::uint64_t raw = read_varint(src, pos);
            return static_cast<U>(raw);
         }
      }

      // ── size_of pass ────────────────────────────────────────────

      //  Forward-decls for recursive message + repeated handling.
      template <typename T>
      std::size_t pb_message_size(const T& v) noexcept;

      template <typename T>
      std::size_t pb_value_size(const T& v) noexcept;

      template <typename T>
      std::size_t pb_value_size(const T& v) noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
            return 1;
         else if constexpr (std::is_signed_v<U> && std::is_integral_v<U>)
         {
            //  proto3 default: varint with sign-extended u64 for
            //  negatives.  Negative int32 always costs 10 bytes.
            std::int64_t s = static_cast<std::int64_t>(v);
            return varint_size(static_cast<std::uint64_t>(s));
         }
         else if constexpr (std::is_unsigned_v<U> && std::is_integral_v<U>)
            return varint_size(static_cast<std::uint64_t>(v));
         else if constexpr (std::is_same_v<U, float>)
            return 4;
         else if constexpr (std::is_same_v<U, double>)
            return 8;
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
            return varint_size(v.size()) + v.size();
         else if constexpr (Reflected<U>)
         {
            std::size_t n = pb_message_size(v);
            return varint_size(n) + n;
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::protobuf: unsupported value type");
            return 0;
         }
      }

      //  Pull the field number from the canonical reflection
      //  surface.  reflect<T>::field_number<I> is Idx+1 by default
      //  and is overwritten by `attr(name, field<N>)` annotations
      //  via the existing effective_annotations_for_v +
      //  find_spec<field_num_spec> path that every other format
      //  consults for the same purpose.
      template <typename T, std::size_t I>
      constexpr std::uint32_t resolve_field_number() noexcept
      {
         using R          = ::psio::reflect<T>;
         using FieldT     = typename R::template member_type<I>;
         constexpr auto P = R::template member_pointer<I>;
         //  effective_annotations_for_v carries the actual annotation
         //  tuple (member-level wins over wrapper-inherent and
         //  type-level — same precedence every other format consults).
         constexpr auto found = ::psio::find_spec<::psio::field_num_spec>(
            ::psio::effective_annotations_for_v<T, FieldT, P>);
         if constexpr (found.has_value())
            return found->value;
         else
            return R::template field_number<I>;
      }

      template <pb_int_enc Enc, typename T>
      std::size_t pb_field_size(const T& v, std::uint32_t field) noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (pb_is_optional<U>::value)
         {
            if (!v.has_value())
               return 0;
            return pb_field_size<Enc, typename pb_is_optional<U>::elem>(
               *v, field);
         }
         else if constexpr (is_byte_vector_v<U>)
         {
            //  bytes: tag + varint(len) + raw bytes.
            return tag_size(field, wt::length_delim) +
                   varint_size(v.size()) + v.size();
         }
         else if constexpr (pb_is_vector<U>::value)
         {
            using E = typename pb_is_vector<U>::elem;
            if (v.empty())
               return 0;
            if constexpr (is_packed_scalar_v<E>)
            {
               //  Packed: tag + varint(payload_len) + payload.  When
               //  E is integral, Enc decides the per-element width;
               //  for floats Enc is ignored (sizeof is fixed already).
               std::size_t payload = 0;
               if constexpr (std::is_integral_v<E> &&
                             !std::is_same_v<E, bool>)
               {
                  for (auto const& x : v)
                     payload += pb_int_value_size<Enc>(x);
               }
               else
               {
                  for (auto const& x : v)
                     payload += pb_value_size(x);
               }
               return tag_size(field, wt::length_delim) +
                      varint_size(payload) + payload;
            }
            else
            {
               //  Unpacked: per-element (tag + value-with-its-own-length).
               std::size_t total = 0;
               for (auto const& x : v)
                  total += tag_size(field, wt::length_delim) +
                           pb_value_size(x);
               return total;
            }
         }
         else if constexpr (std::is_same_v<U, bool>)
            return tag_size(field, wt::varint) + 1;
         else if constexpr (std::is_integral_v<U>)
            return tag_size(field, pb_int_wire_type<U, Enc>()) +
                   pb_int_value_size<Enc>(v);
         else if constexpr (std::is_same_v<U, float>)
            return tag_size(field, wt::fixed32) + 4;
         else if constexpr (std::is_same_v<U, double>)
            return tag_size(field, wt::fixed64) + 8;
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
            return tag_size(field, wt::length_delim) +
                   varint_size(v.size()) + v.size();
         else if constexpr (Reflected<U>)
            return tag_size(field, wt::length_delim) + pb_value_size(v);
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::protobuf: unsupported field type");
            return 0;
         }
      }

      template <typename T>
      std::size_t pb_message_size(const T& v) noexcept
      {
         using R          = ::psio::reflect<T>;
         constexpr auto N = R::member_count;
         std::size_t    total = 0;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            ((total += pb_field_size<resolve_int_encoding<T, Is>()>(
                 v.*(R::template member_pointer<Is>),
                 resolve_field_number<T, Is>())),
             ...);
         }(std::make_index_sequence<N>{});
         return total;
      }

      // ── encode pass ─────────────────────────────────────────────

      template <typename T, typename Sink>
      void pb_write_value(const T& v, Sink& s);

      template <typename T, typename Sink>
      void pb_write_message(const T& v, Sink& s);

      template <typename T, typename Sink>
      void pb_write_value(const T& v, Sink& s)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
         {
            std::uint8_t b = v ? 1 : 0;
            s.write(&b, 1);
         }
         else if constexpr (std::is_signed_v<U> && std::is_integral_v<U>)
         {
            std::int64_t i = static_cast<std::int64_t>(v);
            emit_varint(static_cast<std::uint64_t>(i), s);
         }
         else if constexpr (std::is_unsigned_v<U> && std::is_integral_v<U>)
            emit_varint(static_cast<std::uint64_t>(v), s);
         else if constexpr (std::is_same_v<U, float>)
         {
            std::uint32_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            std::uint32_t le = to_le(bits);
            s.write(&le, sizeof(le));
         }
         else if constexpr (std::is_same_v<U, double>)
         {
            std::uint64_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            std::uint64_t le = to_le(bits);
            s.write(&le, sizeof(le));
         }
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
         {
            emit_varint(v.size(), s);
            if (!v.empty())
               s.write(v.data(), v.size());
         }
         else if constexpr (Reflected<U>)
         {
            //  length-delimited sub-message: varint(size) then body.
            emit_varint(pb_message_size(v), s);
            pb_write_message(v, s);
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::protobuf: unsupported value type");
         }
      }

      template <pb_int_enc Enc, typename T, typename Sink>
      void pb_write_field(const T& v, std::uint32_t field, Sink& s)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (pb_is_optional<U>::value)
         {
            if (!v.has_value())
               return;
            pb_write_field<Enc, typename pb_is_optional<U>::elem>(
               *v, field, s);
         }
         else if constexpr (is_byte_vector_v<U>)
         {
            emit_tag(field, wt::length_delim, s);
            emit_varint(v.size(), s);
            if (!v.empty())
               s.write(v.data(), v.size());
         }
         else if constexpr (pb_is_vector<U>::value)
         {
            using E = typename pb_is_vector<U>::elem;
            if (v.empty())
               return;
            if constexpr (is_packed_scalar_v<E>)
            {
               //  Packed: one tag, varint(payload_len), values.  Enc
               //  applies per-element when E is integral.
               //
               //  Fast-path: when wire is fixed-width (float, double,
               //  fixed32/fixed64 integer encoding), payload size =
               //  v.size() * sizeof(E). No size walk needed — saves a
               //  full pass over the elements.
               std::size_t payload = 0;
               constexpr bool fixed_width =
                  std::is_floating_point_v<E> ||
                  (std::is_integral_v<E> &&
                   !std::is_same_v<E, bool> &&
                   pb_int_wire_type<E, Enc>() != wt::varint);
               if constexpr (fixed_width)
                  payload = v.size() * sizeof(E);
               else if constexpr (std::is_integral_v<E> &&
                                  !std::is_same_v<E, bool>)
               {
                  for (auto const& x : v)
                     payload += pb_int_value_size<Enc>(x);
               }
               else
               {
                  for (auto const& x : v)
                     payload += pb_value_size(x);
               }
               emit_tag(field, wt::length_delim, s);
               emit_varint(payload, s);
               if constexpr (std::is_integral_v<E> &&
                             !std::is_same_v<E, bool>)
               {
                  for (auto const& x : v)
                     pb_int_write_value<Enc>(x, s);
               }
               else
               {
                  for (auto const& x : v)
                     pb_write_value(x, s);
               }
            }
            else
            {
               //  Unpacked: one tag per element.
               for (auto const& x : v)
               {
                  emit_tag(field, wt::length_delim, s);
                  pb_write_value(x, s);
               }
            }
         }
         else if constexpr (std::is_same_v<U, bool>)
         {
            emit_tag(field, wt::varint, s);
            pb_write_value(v, s);
         }
         else if constexpr (std::is_integral_v<U>)
         {
            emit_tag(field, pb_int_wire_type<U, Enc>(), s);
            pb_int_write_value<Enc>(v, s);
         }
         else if constexpr (std::is_same_v<U, float>)
         {
            emit_tag(field, wt::fixed32, s);
            pb_write_value(v, s);
         }
         else if constexpr (std::is_same_v<U, double>)
         {
            emit_tag(field, wt::fixed64, s);
            pb_write_value(v, s);
         }
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
         {
            emit_tag(field, wt::length_delim, s);
            pb_write_value(v, s);
         }
         else if constexpr (Reflected<U>)
         {
            emit_tag(field, wt::length_delim, s);
            pb_write_value(v, s);
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::protobuf: unsupported field type");
         }
      }

      template <typename T, typename Sink>
      void pb_write_message(const T& v, Sink& s)
      {
         using R          = ::psio::reflect<T>;
         constexpr auto N = R::member_count;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            ((pb_write_field<resolve_int_encoding<T, Is>()>(
                 v.*(R::template member_pointer<Is>),
                 resolve_field_number<T, Is>(),
                 s)),
             ...);
         }(std::make_index_sequence<N>{});
      }

      // ── Stack-cached size precomputation ────────────────────────
      //
      //  The naive size+write pipeline above re-walks each subtree
      //  every time it computes a length prefix — fractal cost
      //  O(D × N) for depth D and N nodes.  The pipeline below visits
      //  every node a fixed number of times (2 or 3) regardless of
      //  nesting:
      //
      //    1. count pass   — only when T's slot count is shape-dependent
      //                      (vector<message>, recursive types).
      //                      Skipped when pb_dynamic_size_count_v<T> is
      //                      a compile-time constant.
      //    2. size pass    — populates sizes[0..K) and returns total bytes
      //    3. write pass   — emits bytes, consumes sizes[0..K) in lockstep
      //
      //  Slot allocation: small-buffer optimisation with K ≤ 64 on
      //  stack, heap fallback for huge schemas.  See
      //  .issues/psio-size-cache-design.md for rationale.

      //  Sentinel: the value pb_dynamic_size_count_v<T> takes when T's
      //  slot count cannot be determined at compile time (e.g.,
      //  vector<message>, recursive types).  The encoder runs a
      //  count pass at runtime in that case.
      inline constexpr std::size_t pb_size_count_dynamic =
         std::numeric_limits<std::size_t>::max();

      template <typename T>
      constexpr std::size_t pb_dynamic_size_count_impl() noexcept;

      //  Field-level slot count.  Mirrors pb_field_count's runtime
      //  contract: a Reflected field contributes 1 (parent's header
      //  slot for the nested body length) PLUS the nested type's own
      //  body slot count.  Non-Reflected fields contribute their
      //  type-level count directly.  optional<F> peels to F (worst
      //  case present — both passes short-circuit when absent).
      template <typename F>
      constexpr std::size_t pb_field_consteval_slot_count() noexcept
      {
         using U = std::remove_cvref_t<F>;
         if constexpr (pb_is_optional<U>::value)
            return pb_field_consteval_slot_count<
               typename pb_is_optional<U>::elem>();
         else if constexpr (Reflected<U>)
         {
            constexpr std::size_t inner = pb_dynamic_size_count_impl<U>();
            return inner == pb_size_count_dynamic
                      ? pb_size_count_dynamic
                      : 1 + inner;
         }
         else
            return pb_dynamic_size_count_impl<U>();
      }

      template <typename T, std::size_t... Is>
      constexpr std::size_t pb_dynamic_size_count_struct_helper(
         std::index_sequence<Is...>) noexcept
      {
         using R = ::psio::reflect<T>;
         const std::size_t parts[] = {
            pb_field_consteval_slot_count<
               typename R::template member_type<Is>>()...,
            0  // tail sentinel — guarantees a non-empty array even for
               // empty packs, and is filtered by the loop bound below.
         };
         std::size_t total = 0;
         for (std::size_t i = 0; i < sizeof...(Is); ++i)
         {
            if (parts[i] == pb_size_count_dynamic)
               return pb_size_count_dynamic;
            total += parts[i];
         }
         return total;
      }

      template <typename T>
      constexpr std::size_t pb_dynamic_size_count_impl() noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool> ||
                       std::is_integral_v<U> ||
                       std::is_same_v<U, float> ||
                       std::is_same_v<U, double>)
            return 0;
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
            return 1;
         else if constexpr (is_byte_vector_v<U>)
            return 1;
         else if constexpr (pb_is_optional<U>::value)
            //  optional<T>: when present, contributes T's slots.  Worst-
            //  case allocate as if present; the size+write passes both
            //  short-circuit on `!has_value()` so unused slots stay
            //  unread (lockstep preserved).
            return pb_dynamic_size_count_impl<
               typename pb_is_optional<U>::elem>();
         else if constexpr (pb_is_vector<U>::value)
         {
            using E = typename pb_is_vector<U>::elem;
            if constexpr (is_packed_scalar_v<E>)
               return 1;  // one slot for the packed payload length
            else
               return pb_size_count_dynamic;
         }
         else if constexpr (Reflected<U>)
            return pb_dynamic_size_count_struct_helper<U>(
               std::make_index_sequence<
                  ::psio::reflect<U>::member_count>{});
         else
            return pb_size_count_dynamic;
      }

      template <typename T>
      inline constexpr std::size_t pb_dynamic_size_count_v =
         pb_dynamic_size_count_impl<T>();

      // ── Consteval upper bound for fully-bounded shapes ──────────
      //
      //  For types where pb_dynamic_size_count_v<T> == 0 (no length
      //  prefixes anywhere), the encoded byte count varies with the
      //  values but is bounded by the maximum varint width per
      //  field.  Computing this bound at compile time lets the
      //  encoder allocate a stack buffer and emit in a single walk —
      //  no size pass, no slot cache.

      //  Worst-case bytes for a single value of type V under
      //  encoding Enc.  Returns 0 for types that can't be bounded
      //  at compile time (string, vector, Reflected — which all
      //  imply pb_dynamic_size_count_v > 0 anyway).
      template <typename V, pb_int_enc Enc>
      constexpr std::size_t pb_max_value_bytes() noexcept
      {
         using U = std::remove_cvref_t<V>;
         if constexpr (pb_is_optional<U>::value)
            return pb_max_value_bytes<typename pb_is_optional<U>::elem,
                                       Enc>();
         else if constexpr (std::is_same_v<U, bool>)
            return 1;
         else if constexpr (std::is_integral_v<U>)
         {
            if constexpr (Enc == pb_int_enc::fixed)
               return sizeof(U) <= 4 ? 4 : 8;
            else
               return 10;  // worst-case varint / zigzag for any int
         }
         else if constexpr (std::is_same_v<U, float>)
            return 4;
         else if constexpr (std::is_same_v<U, double>)
            return 8;
         else
            return 0;  // not applicable in K=0 path
      }

      //  Max bytes for the varint-encoded field tag.
      constexpr std::size_t pb_max_tag_bytes(std::uint32_t fnum) noexcept
      {
         const std::uint64_t tag = static_cast<std::uint64_t>(fnum) << 3;
         return tag < (1ULL << 7)  ? 1
              : tag < (1ULL << 14) ? 2
              : tag < (1ULL << 21) ? 3
              : tag < (1ULL << 28) ? 4
                                    : 5;
      }

      template <typename T, std::size_t I>
      constexpr std::size_t pb_field_max_size_at() noexcept
      {
         using R          = ::psio::reflect<T>;
         using F          = std::remove_cvref_t<
            typename R::template member_type<I>>;
         constexpr std::uint32_t fnum = resolve_field_number<T, I>();
         constexpr pb_int_enc    enc  = resolve_int_encoding<T, I>();
         return pb_max_tag_bytes(fnum) + pb_max_value_bytes<F, enc>();
      }

      template <typename T, std::size_t... Is>
      constexpr std::size_t pb_max_size_struct_helper(
         std::index_sequence<Is...>) noexcept
      {
         return (pb_field_max_size_at<T, Is>() + ... + std::size_t(0));
      }

      //  Consteval upper bound on T's encoded byte size.  Returns 0
      //  to signal "not consteval-bounded" (i.e., contains length-
      //  prefixed data whose size depends on runtime values beyond
      //  varint width).
      template <typename T>
      constexpr std::size_t pb_max_size_consteval() noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (Reflected<U> &&
                       pb_dynamic_size_count_v<U> == 0)
            return pb_max_size_struct_helper<U>(
               std::make_index_sequence<
                  ::psio::reflect<U>::member_count>{});
         else
            return 0;
      }

      template <typename T>
      inline constexpr std::size_t pb_max_size_v =
         pb_max_size_consteval<T>();

      //  Per-field consteval byte cost.  Returns the exact (tag +
      //  value) bytes when the field's encoding is fully determined
      //  at compile time (bool / float / double / fixed-encoded
      //  integer at the top level — not wrapped in optional).
      //  Returns 0 to mean "value-dependent — must walk in the size
      //  pass."  These fields contribute 0 slots, so skipping them
      //  in size_collect doesn't break the cache lockstep.
      template <typename T, std::size_t I>
      constexpr std::size_t pb_field_consteval_bytes_at() noexcept
      {
         using R          = ::psio::reflect<T>;
         using F          = std::remove_cvref_t<
            typename R::template member_type<I>>;
         constexpr std::uint32_t fnum = resolve_field_number<T, I>();
         constexpr pb_int_enc    enc  = resolve_int_encoding<T, I>();
         if constexpr (std::is_same_v<F, bool>)
            return pb_max_tag_bytes(fnum) + 1;
         else if constexpr (std::is_same_v<F, float>)
            return pb_max_tag_bytes(fnum) + 4;
         else if constexpr (std::is_same_v<F, double>)
            return pb_max_tag_bytes(fnum) + 8;
         else if constexpr (std::is_integral_v<F> &&
                            !std::is_same_v<F, bool> &&
                            enc == pb_int_enc::fixed)
            return pb_max_tag_bytes(fnum) +
                   (sizeof(F) <= 4 ? 4 : 8);
         else
            return 0;  // dynamic — must walk
      }

      template <typename T, std::size_t... Is>
      constexpr std::size_t pb_consteval_prefix_helper(
         std::index_sequence<Is...>) noexcept
      {
         return (pb_field_consteval_bytes_at<T, Is>() + ... +
                 std::size_t(0));
      }

      template <typename T>
      constexpr std::size_t pb_consteval_prefix_v_impl() noexcept
      {
         using R = ::psio::reflect<T>;
         return pb_consteval_prefix_helper<T>(
            std::make_index_sequence<R::member_count>{});
      }

      template <typename T>
      inline constexpr std::size_t pb_consteval_prefix_v =
         pb_consteval_prefix_v_impl<T>();

      template <typename T>
      std::size_t pb_message_count(const T& v) noexcept;

      template <typename T>
      std::size_t pb_message_size_collect(const T&       v,
                                          std::uint32_t* sizes,
                                          std::size_t&   idx) noexcept;

      template <typename T, typename Sink>
      void pb_message_write_cached(const T&             v,
                                   const std::uint32_t* sizes,
                                   std::size_t&         idx,
                                   Sink&                s);

      // ── pb_field_count: shape-only walk that returns slot count ─
      template <pb_int_enc Enc, typename T>
      std::size_t pb_field_count(const T& v) noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (pb_is_optional<U>::value)
         {
            if (!v.has_value())
               return 0;
            return pb_field_count<Enc, typename pb_is_optional<U>::elem>(*v);
         }
         else if constexpr (is_byte_vector_v<U>)
         {
            //  bytes always emit (matches pb_field_size's behaviour
            //  even for empty payloads).
            return 1;
         }
         else if constexpr (pb_is_vector<U>::value)
         {
            using E = typename pb_is_vector<U>::elem;
            if (v.empty())
               return 0;
            if constexpr (is_packed_scalar_v<E>)
               return 1;  // one slot for the packed payload length
            else if constexpr (Reflected<E>)
            {
               //  one header slot per element + each element's own
               //  inner slot count.  When E's slot count is consteval
               //  (no inner shape-dependent fields), the per-element
               //  contribution is constant and we can skip the
               //  per-element walk.
               constexpr std::size_t e_consteval =
                  pb_dynamic_size_count_v<E>;
               if constexpr (e_consteval != pb_size_count_dynamic)
                  return v.size() * (1 + e_consteval);
               std::size_t k = 0;
               for (auto const& x : v)
                  k += 1 + pb_message_count(x);
               return k;
            }
            else if constexpr (std::is_same_v<E, std::string> ||
                               std::is_same_v<E, std::string_view>)
               return v.size();  // one length slot per element
            else
            {
               static_assert(sizeof(E) == 0,
                             "psio::protobuf: unsupported vector element");
               return 0;
            }
         }
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
            return 1;
         else if constexpr (Reflected<U>)
            return 1 + pb_message_count(v);
         else
            //  bool, integer, float, double — bounded scalar, no slot.
            return 0;
      }

      template <typename T>
      std::size_t pb_message_count(const T& v) noexcept
      {
         //  Short-circuit when every nested field's slot count is
         //  known at compile time — the trait already encodes the
         //  total, so we skip the runtime walk entirely.
         if constexpr (pb_dynamic_size_count_v<T> != pb_size_count_dynamic)
            return pb_dynamic_size_count_v<T>;
         else
         {
            using R          = ::psio::reflect<T>;
            constexpr auto N = R::member_count;
            std::size_t    k = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               ((k += pb_field_count<resolve_int_encoding<T, Is>()>(
                    v.*(R::template member_pointer<Is>))),
                ...);
            }(std::make_index_sequence<N>{});
            return k;
         }
      }

      // ── pb_field_size_collect: populate sizes[idx++] and return
      //                           total byte cost of this field ─────
      template <pb_int_enc Enc, typename T>
      std::size_t pb_field_size_collect(const T&       v,
                                        std::uint32_t  field,
                                        std::uint32_t* sizes,
                                        std::size_t&   idx) noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (pb_is_optional<U>::value)
         {
            if (!v.has_value())
               return 0;
            return pb_field_size_collect<Enc,
                                         typename pb_is_optional<U>::elem>(
               *v, field, sizes, idx);
         }
         else if constexpr (is_byte_vector_v<U>)
         {
            std::uint32_t n = static_cast<std::uint32_t>(v.size());
            sizes[idx++]    = n;
            return tag_size(field, wt::length_delim) +
                   varint_size(n) + n;
         }
         else if constexpr (pb_is_vector<U>::value)
         {
            using E = typename pb_is_vector<U>::elem;
            if (v.empty())
               return 0;
            if constexpr (is_packed_scalar_v<E>)
            {
               std::uint32_t  payload = 0;
               constexpr bool fixed_width =
                  std::is_floating_point_v<E> ||
                  (std::is_integral_v<E> &&
                   !std::is_same_v<E, bool> &&
                   pb_int_wire_type<E, Enc>() != wt::varint);
               if constexpr (fixed_width)
                  payload = static_cast<std::uint32_t>(
                     v.size() * sizeof(E));
               else if constexpr (std::is_integral_v<E> &&
                                  !std::is_same_v<E, bool>)
               {
                  for (auto const& x : v)
                     payload += static_cast<std::uint32_t>(
                        pb_int_value_size<Enc>(x));
               }
               else
               {
                  for (auto const& x : v)
                     payload += static_cast<std::uint32_t>(
                        pb_value_size(x));
               }
               sizes[idx++] = payload;
               return tag_size(field, wt::length_delim) +
                      varint_size(payload) + payload;
            }
            else if constexpr (Reflected<E>)
            {
               //  Reserve element-header slot, recurse to populate
               //  inner slots, patch the reserved slot with the body
               //  size.  idx walks past every inner slot during the
               //  recursive call, mirroring the write pass's order.
               std::size_t total = 0;
               for (auto const& x : v)
               {
                  std::size_t body_slot = idx++;
                  std::size_t body_size =
                     pb_message_size_collect(x, sizes, idx);
                  sizes[body_slot] =
                     static_cast<std::uint32_t>(body_size);
                  total += tag_size(field, wt::length_delim) +
                           varint_size(body_size) + body_size;
               }
               return total;
            }
            else if constexpr (std::is_same_v<E, std::string> ||
                               std::is_same_v<E, std::string_view>)
            {
               std::size_t total = 0;
               for (auto const& x : v)
               {
                  std::uint32_t n =
                     static_cast<std::uint32_t>(x.size());
                  sizes[idx++] = n;
                  total += tag_size(field, wt::length_delim) +
                           varint_size(n) + n;
               }
               return total;
            }
            else
            {
               static_assert(sizeof(E) == 0,
                             "psio::protobuf: unsupported vector element");
               return 0;
            }
         }
         else if constexpr (std::is_same_v<U, bool>)
            return tag_size(field, wt::varint) + 1;
         else if constexpr (std::is_integral_v<U>)
            return tag_size(field, pb_int_wire_type<U, Enc>()) +
                   pb_int_value_size<Enc>(v);
         else if constexpr (std::is_same_v<U, float>)
            return tag_size(field, wt::fixed32) + 4;
         else if constexpr (std::is_same_v<U, double>)
            return tag_size(field, wt::fixed64) + 8;
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
         {
            std::uint32_t n = static_cast<std::uint32_t>(v.size());
            sizes[idx++]    = n;
            return tag_size(field, wt::length_delim) +
                   varint_size(n) + n;
         }
         else if constexpr (Reflected<U>)
         {
            std::size_t body_slot = idx++;
            std::size_t body_size =
               pb_message_size_collect(v, sizes, idx);
            sizes[body_slot] =
               static_cast<std::uint32_t>(body_size);
            return tag_size(field, wt::length_delim) +
                   varint_size(body_size) + body_size;
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::protobuf: unsupported field type");
            return 0;
         }
      }

      //  Outer-level wrapper: skips consteval-byte fields (bool,
      //  float, double, top-level fixed-int) — their bytes are
      //  already in pb_consteval_prefix_v.  Slot count for these
      //  types is 0, so idx stays in lockstep.
      //
      //  optional<consteval-byte> stays runtime-walked because the
      //  contribution depends on presence (0 vs N bytes).
      template <pb_int_enc Enc, typename T>
      std::size_t pb_field_size_collect_outer(const T&       v,
                                              std::uint32_t  field,
                                              std::uint32_t* sizes,
                                              std::size_t&   idx) noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool> ||
                       std::is_same_v<U, float> ||
                       std::is_same_v<U, double> ||
                       (std::is_integral_v<U> &&
                        !std::is_same_v<U, bool> &&
                        Enc == pb_int_enc::fixed))
            return 0;
         else
            return pb_field_size_collect<Enc, T>(v, field, sizes, idx);
      }

      template <typename T>
      std::size_t pb_message_size_collect(const T&       v,
                                          std::uint32_t* sizes,
                                          std::size_t&   idx) noexcept
      {
         using R          = ::psio::reflect<T>;
         constexpr auto N = R::member_count;
         //  Consteval byte contribution from bool / float / double /
         //  fixed-int fields lives in pb_consteval_prefix_v<T>.
         //  pb_field_size_collect_outer short-circuits those fields
         //  back to 0 so the fold doesn't pay for them.
         std::size_t total = pb_consteval_prefix_v<T>;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            ((total += pb_field_size_collect_outer<
                 resolve_int_encoding<T, Is>()>(
                 v.*(R::template member_pointer<Is>),
                 resolve_field_number<T, Is>(),
                 sizes, idx)),
             ...);
         }(std::make_index_sequence<N>{});
         return total;
      }

      // ── pb_field_write_cached: emit bytes, consume sizes[idx++] ──
      template <pb_int_enc Enc, typename T, typename Sink>
      void pb_field_write_cached(const T&             v,
                                 std::uint32_t        field,
                                 const std::uint32_t* sizes,
                                 std::size_t&         idx,
                                 Sink&                s)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (pb_is_optional<U>::value)
         {
            if (!v.has_value())
               return;
            pb_field_write_cached<Enc,
                                  typename pb_is_optional<U>::elem>(
               *v, field, sizes, idx, s);
         }
         else if constexpr (is_byte_vector_v<U>)
         {
            emit_tag(field, wt::length_delim, s);
            std::uint32_t n = sizes[idx++];
            emit_varint(n, s);
            if (n)
               s.write(v.data(), n);
         }
         else if constexpr (pb_is_vector<U>::value)
         {
            using E = typename pb_is_vector<U>::elem;
            if (v.empty())
               return;
            if constexpr (is_packed_scalar_v<E>)
            {
               std::uint32_t payload = sizes[idx++];
               emit_tag(field, wt::length_delim, s);
               emit_varint(payload, s);
               if constexpr (std::is_integral_v<E> &&
                             !std::is_same_v<E, bool>)
               {
                  for (auto const& x : v)
                     pb_int_write_value<Enc>(x, s);
               }
               else
               {
                  for (auto const& x : v)
                     pb_write_value(x, s);
               }
            }
            else if constexpr (Reflected<E>)
            {
               for (auto const& x : v)
               {
                  emit_tag(field, wt::length_delim, s);
                  std::uint32_t body = sizes[idx++];
                  emit_varint(body, s);
                  pb_message_write_cached(x, sizes, idx, s);
               }
            }
            else if constexpr (std::is_same_v<E, std::string> ||
                               std::is_same_v<E, std::string_view>)
            {
               for (auto const& x : v)
               {
                  emit_tag(field, wt::length_delim, s);
                  std::uint32_t n = sizes[idx++];
                  emit_varint(n, s);
                  if (n)
                     s.write(x.data(), n);
               }
            }
         }
         else if constexpr (std::is_same_v<U, bool>)
         {
            emit_tag(field, wt::varint, s);
            pb_write_value(v, s);
         }
         else if constexpr (std::is_integral_v<U>)
         {
            emit_tag(field, pb_int_wire_type<U, Enc>(), s);
            pb_int_write_value<Enc>(v, s);
         }
         else if constexpr (std::is_same_v<U, float>)
         {
            emit_tag(field, wt::fixed32, s);
            pb_write_value(v, s);
         }
         else if constexpr (std::is_same_v<U, double>)
         {
            emit_tag(field, wt::fixed64, s);
            pb_write_value(v, s);
         }
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
         {
            emit_tag(field, wt::length_delim, s);
            std::uint32_t n = sizes[idx++];
            emit_varint(n, s);
            if (n)
               s.write(v.data(), n);
         }
         else if constexpr (Reflected<U>)
         {
            emit_tag(field, wt::length_delim, s);
            std::uint32_t body = sizes[idx++];
            emit_varint(body, s);
            pb_message_write_cached(v, sizes, idx, s);
         }
      }

      //  Write a varint-encoded tag from a compile-time field number
      //  into the start of buf. Returns the number of bytes written.
      //  For typical field numbers (≤ 268435455 i.e. four-byte tag),
      //  unrolls to a small straight-line store sequence.
      template <std::uint32_t F, std::uint8_t WT>
      inline std::size_t pb_write_const_tag(std::uint8_t* buf) noexcept
      {
         constexpr std::uint64_t tag =
            (static_cast<std::uint64_t>(F) << 3) | WT;
         if constexpr (tag < (1ULL << 7))
         {
            buf[0] = static_cast<std::uint8_t>(tag);
            return 1;
         }
         else if constexpr (tag < (1ULL << 14))
         {
            buf[0] = static_cast<std::uint8_t>(tag) |
                     static_cast<std::uint8_t>(0x80);
            buf[1] = static_cast<std::uint8_t>(tag >> 7);
            return 2;
         }
         else if constexpr (tag < (1ULL << 21))
         {
            buf[0] = static_cast<std::uint8_t>(tag) |
                     static_cast<std::uint8_t>(0x80);
            buf[1] = static_cast<std::uint8_t>(tag >> 7) |
                     static_cast<std::uint8_t>(0x80);
            buf[2] = static_cast<std::uint8_t>(tag >> 14);
            return 3;
         }
         else if constexpr (tag < (1ULL << 28))
         {
            buf[0] = static_cast<std::uint8_t>(tag) |
                     static_cast<std::uint8_t>(0x80);
            buf[1] = static_cast<std::uint8_t>(tag >> 7) |
                     static_cast<std::uint8_t>(0x80);
            buf[2] = static_cast<std::uint8_t>(tag >> 14) |
                     static_cast<std::uint8_t>(0x80);
            buf[3] = static_cast<std::uint8_t>(tag >> 21);
            return 4;
         }
         else
         {
            static_assert(F < (1ULL << 25),
                          "psio::protobuf: field number too large for "
                          "consteval tag fast path");
            return 0;
         }
      }

      //  Fused-write for consteval-byte fields: bool / float / double
      //  / pb_fixed integer.  Tag bytes are computed at compile time;
      //  tag + value go through a single sink.write — half the sink
      //  interactions of the generic field write path.
      template <pb_int_enc Enc, std::uint32_t F, typename T, typename Sink>
      inline void pb_emit_consteval_field(const T& v, Sink& s) noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
         {
            std::uint8_t  buf[5];
            std::size_t   n = pb_write_const_tag<F, wt::varint>(buf);
            buf[n++] = v ? std::uint8_t{1} : std::uint8_t{0};
            s.write(buf, n);
         }
         else if constexpr (std::is_same_v<U, float>)
         {
            std::uint8_t  buf[8];
            std::size_t   n = pb_write_const_tag<F, wt::fixed32>(buf);
            std::uint32_t bits;
            std::memcpy(&bits, &v, 4);
            bits = to_le(bits);
            std::memcpy(buf + n, &bits, 4);
            s.write(buf, n + 4);
         }
         else if constexpr (std::is_same_v<U, double>)
         {
            std::uint8_t  buf[12];
            std::size_t   n = pb_write_const_tag<F, wt::fixed64>(buf);
            std::uint64_t bits;
            std::memcpy(&bits, &v, 8);
            bits = to_le(bits);
            std::memcpy(buf + n, &bits, 8);
            s.write(buf, n + 8);
         }
         else if constexpr (std::is_integral_v<U> &&
                            !std::is_same_v<U, bool> &&
                            Enc == pb_int_enc::fixed)
         {
            constexpr std::uint8_t wt_ =
               sizeof(U) <= 4 ? wt::fixed32 : wt::fixed64;
            constexpr std::size_t value_bytes =
               sizeof(U) <= 4 ? 4 : 8;
            std::uint8_t buf[12];
            std::size_t  n = pb_write_const_tag<F, wt_>(buf);
            if constexpr (value_bytes == 4)
            {
               std::uint32_t bits = 0;
               if constexpr (std::is_signed_v<U>)
               {
                  std::int32_t sv = static_cast<std::int32_t>(v);
                  std::memcpy(&bits, &sv, 4);
               }
               else
                  bits = static_cast<std::uint32_t>(v);
               bits = to_le(bits);
               std::memcpy(buf + n, &bits, 4);
            }
            else
            {
               std::uint64_t bits = 0;
               if constexpr (std::is_signed_v<U>)
               {
                  std::int64_t sv = static_cast<std::int64_t>(v);
                  std::memcpy(&bits, &sv, 8);
               }
               else
                  bits = static_cast<std::uint64_t>(v);
               bits = to_le(bits);
               std::memcpy(buf + n, &bits, 8);
            }
            s.write(buf, n + value_bytes);
         }
      }

      //  Max bytes a "primitive" field can produce.  A field is
      //  primitive iff its encoding has no length prefix and its
      //  worst-case byte count is known at compile time:
      //    bool / float / double / fixed-int : exact (max == actual).
      //    varint / zigzag int               : tag (consteval) + 10
      //                                        bytes worst-case varint.
      //  Returns 0 for non-primitive types (string, vector, message,
      //  optional<T>, etc.) — those never join a primitive run.
      template <typename T, std::size_t I>
      constexpr std::size_t pb_field_primitive_max_at() noexcept
      {
         using R          = ::psio::reflect<T>;
         using F          = std::remove_cvref_t<
            typename R::template member_type<I>>;
         constexpr std::uint32_t fnum     = resolve_field_number<T, I>();
         constexpr pb_int_enc    enc      = resolve_int_encoding<T, I>();
         constexpr std::size_t   tag_max  = pb_max_tag_bytes(fnum);
         if constexpr (std::is_same_v<F, bool>)
            return tag_max + 1;
         else if constexpr (std::is_same_v<F, float>)
            return tag_max + 4;
         else if constexpr (std::is_same_v<F, double>)
            return tag_max + 8;
         else if constexpr (std::is_integral_v<F> &&
                            !std::is_same_v<F, bool>)
         {
            if constexpr (enc == pb_int_enc::fixed)
               return tag_max + (sizeof(F) <= 4 ? 4 : 8);
            else
               return tag_max + 10;  // varint / zigzag worst case
         }
         else
            return 0;
      }

      //  Pack one primitive field's tag + value bytes into buf
      //  starting at `pos`, returning the new position.  No sink
      //  interaction — used by the batched run helper.
      template <typename T, std::size_t I>
      inline std::size_t pb_pack_primitive_into(
         const T& v, std::uint8_t* buf, std::size_t pos) noexcept
      {
         using R          = ::psio::reflect<T>;
         using F          = std::remove_cvref_t<
            typename R::template member_type<I>>;
         constexpr std::uint32_t fnum = resolve_field_number<T, I>();
         constexpr pb_int_enc    enc  = resolve_int_encoding<T, I>();
         const auto&             field =
            v.*(R::template member_pointer<I>);
         if constexpr (std::is_same_v<F, bool>)
         {
            pos += pb_write_const_tag<fnum, wt::varint>(buf + pos);
            buf[pos++] = field ? std::uint8_t{1} : std::uint8_t{0};
         }
         else if constexpr (std::is_same_v<F, float>)
         {
            pos += pb_write_const_tag<fnum, wt::fixed32>(buf + pos);
            std::uint32_t bits;
            std::memcpy(&bits, &field, 4);
            bits = to_le(bits);
            std::memcpy(buf + pos, &bits, 4);
            pos += 4;
         }
         else if constexpr (std::is_same_v<F, double>)
         {
            pos += pb_write_const_tag<fnum, wt::fixed64>(buf + pos);
            std::uint64_t bits;
            std::memcpy(&bits, &field, 8);
            bits = to_le(bits);
            std::memcpy(buf + pos, &bits, 8);
            pos += 8;
         }
         else if constexpr (std::is_integral_v<F> &&
                            !std::is_same_v<F, bool> &&
                            enc == pb_int_enc::fixed)
         {
            constexpr std::uint8_t wt_ =
               sizeof(F) <= 4 ? wt::fixed32 : wt::fixed64;
            pos += pb_write_const_tag<fnum, wt_>(buf + pos);
            if constexpr (sizeof(F) <= 4)
            {
               std::uint32_t bits = 0;
               if constexpr (std::is_signed_v<F>)
               {
                  std::int32_t sv = static_cast<std::int32_t>(field);
                  std::memcpy(&bits, &sv, 4);
               }
               else
                  bits = static_cast<std::uint32_t>(field);
               bits = to_le(bits);
               std::memcpy(buf + pos, &bits, 4);
               pos += 4;
            }
            else
            {
               std::uint64_t bits = 0;
               if constexpr (std::is_signed_v<F>)
               {
                  std::int64_t sv = static_cast<std::int64_t>(field);
                  std::memcpy(&bits, &sv, 8);
               }
               else
                  bits = static_cast<std::uint64_t>(field);
               bits = to_le(bits);
               std::memcpy(buf + pos, &bits, 8);
               pos += 8;
            }
         }
         else if constexpr (std::is_integral_v<F> &&
                            !std::is_same_v<F, bool> &&
                            enc == pb_int_enc::default_)
         {
            //  Default-encoded varint integer: tag (consteval) +
            //  varint of value (1–10 bytes runtime).
            pos += pb_write_const_tag<fnum, wt::varint>(buf + pos);
            std::uint64_t u;
            if constexpr (std::is_signed_v<F>)
            {
               auto sv = static_cast<std::int64_t>(field);
               u       = static_cast<std::uint64_t>(sv);
            }
            else
               u = static_cast<std::uint64_t>(field);
            pos += ::psio::varint::leb128::scalar::encode_u64(
               buf + pos, u);
         }
         else if constexpr (std::is_integral_v<F> &&
                            !std::is_same_v<F, bool> &&
                            enc == pb_int_enc::zigzag)
         {
            //  Zigzag-encoded varint integer.
            pos += pb_write_const_tag<fnum, wt::varint>(buf + pos);
            auto sv = static_cast<std::int64_t>(field);
            auto u  = (static_cast<std::uint64_t>(sv) << 1) ^
                     static_cast<std::uint64_t>(sv >> 63);
            pos += ::psio::varint::leb128::scalar::encode_u64(
               buf + pos, u);
         }
         return pos;
      }

      //  End of the primitive-field run that starts at index I — the
      //  smallest J ≥ I such that field J is NOT a primitive (or
      //  member_count if the run continues to the last field).
      template <typename T, std::size_t I>
      constexpr std::size_t pb_primitive_run_end_v_impl() noexcept
      {
         constexpr auto N = ::psio::reflect<T>::member_count;
         if constexpr (I >= N)
            return N;
         else if constexpr (pb_field_primitive_max_at<T, I>() > 0)
            return pb_primitive_run_end_v_impl<T, I + 1>();
         else
            return I;
      }

      template <typename T, std::size_t I>
      inline constexpr std::size_t pb_primitive_run_end_v =
         pb_primitive_run_end_v_impl<T, I>();

      //  Emit a run of consecutive primitive fields [Begin, End) in
      //  one sink.write.  Buffer is sized to the sum of per-field
      //  worst-case bytes (consteval) and packed at runtime; the
      //  actual byte count `pos` may be less than the buffer size
      //  because varint-encoded values can be shorter than 10 bytes.
      template <typename T, std::size_t Begin, std::size_t End,
                typename Sink>
      inline void pb_emit_primitive_run(const T& v, Sink& s) noexcept
      {
         constexpr std::size_t cap =
            []<std::size_t... Js>(std::index_sequence<Js...>) {
               return (pb_field_primitive_max_at<T, Begin + Js>() + ...
                       + std::size_t(0));
            }(std::make_index_sequence<End - Begin>{});
         std::uint8_t buf[cap];
         std::size_t  pos = 0;
         [&]<std::size_t... Js>(std::index_sequence<Js...>) {
            ((pos = pb_pack_primitive_into<T, Begin + Js>(v, buf, pos)),
             ...);
         }(std::make_index_sequence<End - Begin>{});
         s.write(buf, pos);
      }

      //  Tail-recursive walker over reflected fields.  Groups
      //  consecutive primitive fields (bool, float, double, fixed-int,
      //  varint-int) into a single batched write; emits non-primitive
      //  fields individually through the cache-consuming path.  The
      //  whole walk unrolls at compile time.
      template <typename T, std::size_t I, typename Sink>
      inline void pb_walk_fields(const T&             v,
                                 const std::uint32_t* sizes,
                                 std::size_t&         idx,
                                 Sink&                s)
      {
         constexpr auto N = ::psio::reflect<T>::member_count;
         if constexpr (I >= N)
            return;
         else if constexpr (pb_field_primitive_max_at<T, I>() > 0)
         {
            constexpr std::size_t End = pb_primitive_run_end_v<T, I>;
            pb_emit_primitive_run<T, I, End>(v, s);
            pb_walk_fields<T, End>(v, sizes, idx, s);
         }
         else
         {
            using R = ::psio::reflect<T>;
            pb_field_write_cached<resolve_int_encoding<T, I>()>(
               v.*(R::template member_pointer<I>),
               resolve_field_number<T, I>(),
               sizes, idx, s);
            pb_walk_fields<T, I + 1>(v, sizes, idx, s);
         }
      }

      template <typename T, typename Sink>
      void pb_message_write_cached(const T&             v,
                                   const std::uint32_t* sizes,
                                   std::size_t&         idx,
                                   Sink&                s)
      {
         pb_walk_fields<T, 0>(v, sizes, idx, s);
      }

      // ── decode pass ─────────────────────────────────────────────
      //
      //  Streaming walk over a length-delimited message body.  At
      //  each iteration we read a tag, look up the matching field by
      //  number on the C++ side (compile-time fold over the
      //  reflected fields), and route the value into the
      //  corresponding member.  Unknown fields are skipped according
      //  to wire_type so the stream stays aligned.

      inline void skip_field(std::span<const char> src,
                             std::size_t&          pos,
                             std::uint8_t          wire_type)
      {
         switch (wire_type)
         {
            case wt::varint:
               (void)read_varint(src, pos);
               return;
            case wt::fixed64:
               if (pos + 8 > src.size())
                  throw std::runtime_error(
                     "protobuf: truncated fixed64");
               pos += 8;
               return;
            case wt::length_delim:
            {
               std::uint64_t n = read_varint(src, pos);
               if (pos + n > src.size())
                  throw std::runtime_error(
                     "protobuf: length-delim overruns buffer");
               pos += n;
               return;
            }
            case wt::fixed32:
               if (pos + 4 > src.size())
                  throw std::runtime_error(
                     "protobuf: truncated fixed32");
               pos += 4;
               return;
            default:
               throw std::runtime_error(
                  "protobuf: deprecated/unknown wire type");
         }
      }

      template <typename T>
      void pb_read_value(std::span<const char> src, std::size_t& pos,
                         std::uint8_t wire_type, T& out);

      template <typename T>
      void pb_decode_message(T&                    out,
                             std::span<const char> src,
                             std::size_t           start,
                             std::size_t           end);

      template <typename T>
      void pb_read_value(std::span<const char> src, std::size_t& pos,
                         std::uint8_t wire_type, T& out)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
         {
            if (wire_type != wt::varint)
               throw std::runtime_error("protobuf: bool wire-type mismatch");
            out = read_varint(src, pos) != 0;
         }
         else if constexpr (std::is_integral_v<U>)
         {
            if (wire_type != wt::varint)
               throw std::runtime_error("protobuf: int wire-type mismatch");
            std::uint64_t raw = read_varint(src, pos);
            //  Truncate / sign-extend to T as the wire intends —
            //  proto3 sends negative signed ints sign-extended to
            //  u64, so a simple cast back to T recovers the value.
            out = static_cast<U>(raw);
         }
         else if constexpr (std::is_same_v<U, float>)
         {
            if (wire_type != wt::fixed32)
               throw std::runtime_error(
                  "protobuf: float wire-type mismatch");
            if (pos + 4 > src.size())
               throw std::runtime_error("protobuf: truncated f32");
            std::uint32_t bits;
            std::memcpy(&bits, src.data() + pos, 4);
            bits = to_le(bits);
            std::memcpy(&out, &bits, 4);
            pos += 4;
         }
         else if constexpr (std::is_same_v<U, double>)
         {
            if (wire_type != wt::fixed64)
               throw std::runtime_error(
                  "protobuf: double wire-type mismatch");
            if (pos + 8 > src.size())
               throw std::runtime_error("protobuf: truncated f64");
            std::uint64_t bits;
            std::memcpy(&bits, src.data() + pos, 8);
            bits = to_le(bits);
            std::memcpy(&out, &bits, 8);
            pos += 8;
         }
         else if constexpr (std::is_same_v<U, std::string>)
         {
            if (wire_type != wt::length_delim)
               throw std::runtime_error(
                  "protobuf: string wire-type mismatch");
            std::uint64_t n = read_varint(src, pos);
            if (pos + n > src.size())
               throw std::runtime_error(
                  "protobuf: string overruns buffer");
            out.assign(src.data() + pos, n);
            pos += n;
         }
         else if constexpr (Reflected<U>)
         {
            if (wire_type != wt::length_delim)
               throw std::runtime_error(
                  "protobuf: message wire-type mismatch");
            std::uint64_t n = read_varint(src, pos);
            if (pos + n > src.size())
               throw std::runtime_error(
                  "protobuf: sub-message overruns buffer");
            pb_decode_message(out, src, pos, pos + n);
            pos += n;
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::protobuf: unsupported value type");
         }
      }

      //  Read a packed-or-unpacked repeated field's incoming chunk
      //  into the provided vector reference.  The vector accumulates
      //  across multiple wire-tag occurrences (proto3 allows a
      //  repeated field's elements to arrive via mixed packed +
      //  unpacked chunks, all concatenated).
      template <pb_int_enc Enc, typename V>
      void pb_read_repeated(std::span<const char> src, std::size_t& pos,
                            std::uint8_t wire_type, V& out)
      {
         using E = typename pb_is_vector<V>::elem;
         if constexpr (is_packed_scalar_v<E>)
         {
            //  Packed: wire is length-delim with concatenated values
            //  and no per-element tag.
            if (wire_type == wt::length_delim)
            {
               std::uint64_t n   = read_varint(src, pos);
               std::size_t   end = pos + n;
               if (end > src.size())
                  throw std::runtime_error(
                     "protobuf: packed array overruns buffer");

               // Reserve based on expected element count to avoid
               // per-element vector reallocation. For fixed-wire
               // types (fixed32/fixed64) the count is exact;
               // for varint we lower-bound by n/max-elem-size.
               // Either way we save 4-5 reallocs on a 16-element
               // packed array.
               if constexpr (std::is_floating_point_v<E>)
                  out.reserve(out.size() + n / sizeof(E));
               else if constexpr (std::is_integral_v<E> &&
                                  !std::is_same_v<E, bool>)
               {
                  // Varint: 1..varint_max bytes per element. Reserve
                  // for the upper bound (1 byte/elem) — over-reserves
                  // by ~2× on average for typical small ints, but
                  // beats N small reallocations.
                  out.reserve(out.size() + n);
               }
               else
                  out.reserve(out.size() + n);

               while (pos < end)
               {
                  if constexpr (std::is_integral_v<E> &&
                                !std::is_same_v<E, bool>)
                  {
                     std::uint8_t inner_wt = pb_int_wire_type<E, Enc>();
                     E            v        = pb_int_read_value<Enc, E>(
                        src, pos, inner_wt);
                     out.push_back(v);
                  }
                  else
                  {
                     std::uint8_t inner_wt =
                        std::is_floating_point_v<E>
                           ? (sizeof(E) == 4 ? wt::fixed32 : wt::fixed64)
                           : wt::varint;
                     out.emplace_back();
                     pb_read_value(src, pos, inner_wt, out.back());
                  }
               }
            }
            else
            {
               //  Permit a sender that opted out of packing.
               if constexpr (std::is_integral_v<E> &&
                             !std::is_same_v<E, bool>)
               {
                  out.push_back(
                     pb_int_read_value<Enc, E>(src, pos, wire_type));
               }
               else
               {
                  out.emplace_back();
                  pb_read_value(src, pos, wire_type, out.back());
               }
            }
         }
         else
         {
            //  Unpacked: one element per tag occurrence. Read straight
            //  into the vector's storage to avoid the move construction
            //  pjson_value would pay otherwise.
            out.emplace_back();
            pb_read_value(src, pos, wire_type, out.back());
         }
      }

      //  Per-member decode body, indexed by I.  Factored out so the
      //  fast-path dispatch table can take its address as a uniform
      //  function pointer.
      template <typename T, std::size_t I>
      void pb_decode_field_at(T&                    out,
                              std::span<const char> src,
                              std::size_t&          pos,
                              std::uint8_t          wire_type)
      {
         using R  = ::psio::reflect<T>;
         using FT = std::remove_cvref_t<
            typename R::template member_type<I>>;
         auto& fref = out.*(R::template member_pointer<I>);
         constexpr pb_int_enc Enc = resolve_int_encoding<T, I>();

         if constexpr (pb_is_optional<FT>::value)
         {
            using E = typename pb_is_optional<FT>::elem;
            if constexpr (std::is_integral_v<E> &&
                          !std::is_same_v<E, bool>)
            {
               E inner = pb_int_read_value<Enc, E>(src, pos, wire_type);
               fref    = std::move(inner);
            }
            else
            {
               E inner{};
               pb_read_value(src, pos, wire_type, inner);
               fref = std::move(inner);
            }
         }
         else if constexpr (is_byte_vector_v<FT>)
         {
            if (wire_type != wt::length_delim)
               throw std::runtime_error(
                  "protobuf: bytes wire-type mismatch");
            std::uint64_t n = read_varint(src, pos);
            if (pos + n > src.size())
               throw std::runtime_error("protobuf: bytes overrun");
            fref.assign(reinterpret_cast<const std::uint8_t*>(
                           src.data() + pos),
                        reinterpret_cast<const std::uint8_t*>(
                           src.data() + pos + n));
            pos += n;
         }
         else if constexpr (pb_is_vector<FT>::value)
         {
            pb_read_repeated<Enc>(src, pos, wire_type, fref);
         }
         else if constexpr (std::is_integral_v<FT> &&
                            !std::is_same_v<FT, bool>)
         {
            fref = pb_int_read_value<Enc, FT>(src, pos, wire_type);
         }
         else
         {
            pb_read_value(src, pos, wire_type, fref);
         }
      }

      //  Per-T compile-time max field number — sizes the dense
      //  dispatch table.  Empty messages (member_count == 0) hold
      //  the table at size 1 so the array<...> instantiation is
      //  well-formed.
      template <typename T>
      consteval std::uint32_t pb_max_field_number() noexcept
      {
         using R          = ::psio::reflect<T>;
         constexpr auto N = R::member_count;
         if constexpr (N == 0)
            return 0;
         else
         {
            std::uint32_t m = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               ((m = (resolve_field_number<T, Is>() > m
                         ? resolve_field_number<T, Is>()
                         : m)),
                ...);
            }(std::make_index_sequence<N>{});
            return m;
         }
      }

      template <typename T>
      void pb_decode_message(T&                    out,
                             std::span<const char> src,
                             std::size_t           start,
                             std::size_t           end)
      {
         using R          = ::psio::reflect<T>;
         constexpr auto N = R::member_count;
         std::size_t pos  = start;

         //  Per-T dispatch table indexed by field number — O(1)
         //  field_num → member-index lookup regardless of whether
         //  the type uses default field numbers or `attr(field<N>)`
         //  overrides.  Field numbers we don't recognise hold a
         //  nullptr slot (the wire's unknown-field skip path picks
         //  them up).
         //
         //  Size is `max_field_number + 1` so the table is dense
         //  with index 0 unused.  Sparse overrides (e.g. field<100>
         //  on a 3-field type) waste slots but stay constant-time;
         //  pathological large field numbers are the only case where
         //  this matters and they're rare in practice.
         using fn_t = void (*)(T&, std::span<const char>,
                               std::size_t&, std::uint8_t);
         constexpr std::uint32_t max_fn = pb_max_field_number<T>();
         constexpr auto          table =
            []<std::size_t... Is>(std::index_sequence<Is...>)
         {
            std::array<fn_t, max_fn + 1> a{};
            (([&]
              {
                 constexpr auto fn = resolve_field_number<T, Is>();
                 a[fn] = &pb_decode_field_at<T, Is>;
              }()),
             ...);
            return a;
         }(std::make_index_sequence<N>{});

         while (pos < end)
         {
            std::uint64_t tag       = read_varint(src, pos);
            std::uint32_t field_num = static_cast<std::uint32_t>(tag >> 3);
            std::uint8_t  wire_type = static_cast<std::uint8_t>(tag & 0x7);

            if (field_num >= 1 && field_num <= max_fn && table[field_num])
               table[field_num](out, src, pos, wire_type);
            else
               skip_field(src, pos, wire_type);
         }
         if (pos != end)
            throw std::runtime_error(
               "protobuf: message body length mismatch");
      }

   }  // namespace detail::pb_impl

   struct protobuf : format_tag_base<protobuf>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), protobuf,
                             const T& v, std::vector<char>& sink)
      {
         //  The sink path already has a reusable buffer, so writing
         //  directly into it after a size pass beats the stack-buf
         //  + memcpy variant we use for the rvalue return.  Stick
         //  with the cache pipeline here.
         //
         //  Stack-cached size precomputation: count slots, populate
         //  sizes[], then write reading sizes[] for length prefixes.
         //  See .issues/psio-size-cache-design.md.
         constexpr std::size_t      kSlotSbo = 64;
         std::uint32_t              sbo[kSlotSbo];
         std::vector<std::uint32_t> heap;
         std::uint32_t*             sizes = sbo;
         std::size_t                K;
         if constexpr (detail::pb_impl::pb_dynamic_size_count_v<T> !=
                       detail::pb_impl::pb_size_count_dynamic)
            K = detail::pb_impl::pb_dynamic_size_count_v<T>;  // consteval — skip count pass
         else
            K = detail::pb_impl::pb_message_count(v);
         if (K > kSlotSbo)
         {
            heap.resize(K);
            sizes = heap.data();
         }
         std::size_t idx   = 0;
         std::size_t total =
            detail::pb_impl::pb_message_size_collect(v, sizes, idx);
         std::size_t before = sink.size();
         sink.resize(before + total);
         ::psio::fast_buf_stream fbs{sink.data() + before, total};
         idx = 0;
         detail::pb_impl::pb_message_write_cached(v, sizes, idx, fbs);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode),
                                          protobuf, const T& v)
      {
         //  Tier 1 — fully bounded (K=0, consteval upper bound).
         if constexpr (detail::pb_impl::pb_max_size_v<T> > 0 &&
                       detail::pb_impl::pb_max_size_v<T> <= 1024)
         {
            constexpr std::size_t Cap = detail::pb_impl::pb_max_size_v<T>;
            char                  buf[Cap];
            ::psio::fast_buf_stream fbs{buf, Cap};
            std::size_t             idx = 0;
            detail::pb_impl::pb_message_write_cached(
               v, static_cast<const std::uint32_t*>(nullptr), idx, fbs);
            const std::size_t actual = fbs.written();
            std::vector<char> out(buf, buf + actual);
            return out;
         }

         constexpr std::size_t      kSlotSbo = 64;
         std::uint32_t              sbo[kSlotSbo];
         std::vector<std::uint32_t> heap;
         std::uint32_t*             sizes = sbo;
         std::size_t                K;
         if constexpr (detail::pb_impl::pb_dynamic_size_count_v<T> !=
                       detail::pb_impl::pb_size_count_dynamic)
            K = detail::pb_impl::pb_dynamic_size_count_v<T>;  // consteval — skip count pass
         else
            K = detail::pb_impl::pb_message_count(v);
         if (K > kSlotSbo)
         {
            heap.resize(K);
            sizes = heap.data();
         }
         std::size_t idx   = 0;
         std::size_t total =
            detail::pb_impl::pb_message_size_collect(v, sizes, idx);
         std::vector<char>       out(total);
         ::psio::fast_buf_stream fbs{out.data(), out.size()};
         idx = 0;
         detail::pb_impl::pb_message_write_cached(v, sizes, idx, fbs);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), protobuf, T*,
                          std::span<const char> bytes)
      {
         T out{};
         detail::pb_impl::pb_decode_message(out, bytes, 0, bytes.size());
         return out;
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), protobuf,
                                    const T& v)
      {
         return detail::pb_impl::pb_message_size(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>),
                                     protobuf, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(),
                                                         "protobuf");
             !st.ok())
            return st;
         try
         {
            T tmp{};
            detail::pb_impl::pb_decode_message(tmp, bytes, 0, bytes.size());
            return codec_ok();
         }
         catch (const std::exception& e)
         {
            return codec_fail(e.what(), 0, "protobuf");
         }
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     protobuf, T*,
                                     std::span<const char> bytes) noexcept
      {
         //  protobuf has no canonical encoding (e.g. unknown fields
         //  are spec-permitted), so strict mode collapses to the
         //  same check as validate today.  A separate hook can layer
         //  on "reject unknown fields" semantics when needed.
         return tag_invoke(::psio::validate<T>, protobuf{},
                           static_cast<T*>(nullptr), bytes);
      }
   };

}  // namespace psio
