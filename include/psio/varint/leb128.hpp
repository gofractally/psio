#pragma once
//
// libraries/psio/cpp/include/psio/varint/leb128.hpp — LEB128.
//
// Three sub-codecs covering the variants the codebase actually emits:
//
//   uleb     — unsigned LEB128. 7-bit groups, MSB = continuation flag.
//              Max 5 bytes for u32, 10 bytes for u64.
//   sleb     — signed LEB128. Same wire layout, last byte's bit 6
//              sign-extends.  Max 5 bytes for i32, 10 bytes for i64.
//   zigzag   — Avro `long` flavour: zig-zag transform followed by
//              uleb. Max 10 bytes.
//
// Encoders write at most `max_bytes_<n>` bytes; the buffer must have
// that much room. Decoders consume up to `max_bytes_<n>` bytes from
// `[p, p+avail)` and return `ok=false` (with `len=0`) on:
//   • truncated input (continuation bit set on the last available
//     byte),
//   • overflow (more than `max_bytes_<n>` continuation bytes).
//
// Decoders do NOT enforce canonical encoding (i.e. they accept
// redundant trailing zero-payload bytes that fit in the byte budget).
// Callers that need spec strictness — the wasm parser is one — should
// either bound the input by `max_bytes_<n>` and trust the encoder, or
// use `decode_*_strict` which additionally rejects bits beyond the
// declared width on the last byte.
//

#include <psio/varint/detail/cpu.hpp>
#include <psio/varint/result.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__ARM_NEON) && (defined(__aarch64__) || defined(_M_ARM64))
#  include <arm_neon.h>
#endif

namespace psio::varint::leb128 {

   // Maximum encoded byte counts.  ceil(W/7) for an unsigned width-W
   // value; signed forms share the unsigned bound because the sign bit
   // lives in the same byte budget.
   inline constexpr std::size_t max_bytes_u32 = 5;
   inline constexpr std::size_t max_bytes_u64 = 10;
   inline constexpr std::size_t max_bytes_i32 = 5;
   inline constexpr std::size_t max_bytes_i64 = 10;

   // ────────────────────────────────────────────────────────────────
   // scalar — portable byte-loop reference implementation. Always
   // available; correctness baseline for the parity tests.
   // ────────────────────────────────────────────────────────────────
   namespace scalar {

      // ── encode ───────────────────────────────────────────────────
      //
      //  Hybrid layout: the v < 128 fast path is a 4-instruction
      //  straight-line sequence the branch predictor catches every
      //  time when values are small (typical for protobuf field
      //  IDs, lengths, counters).  The else branch unconditionally
      //  packs all 5 / 10 candidate bytes — under -O3 clang turns
      //  the middle bytes into a NEON `dup` + `ushl` + `cmhi` /
      //  `csel` sequence on aarch64 (and a similar shift-and-mask
      //  vector trick on x86_64).  Across mixed-magnitude inputs
      //  this is ~3× faster than the byte-loop because there are no
      //  data-dependent branches to mispredict.

      inline std::size_t encode_u32(std::uint8_t* buf,
                                    std::uint32_t v) noexcept
      {
         if (v < 0x80) [[likely]]
         {
            buf[0] = static_cast<std::uint8_t>(v);
            return 1;
         }
         //  bits = position of MSB (1-indexed); size = ceil(bits/7).
         //  countl_zero(v) is CLZ — single instruction on every
         //  modern target.  v ≥ 0x80 here, so bits ≥ 8.
         const std::size_t bits = 32 - std::countl_zero(v);
         const std::size_t size = (bits + 6) / 7;  // 2..5

         buf[0] = static_cast<std::uint8_t>(v       & 0x7f) |
                  static_cast<std::uint8_t>(0x80);
         buf[1] = static_cast<std::uint8_t>(v >>  7 & 0x7f) |
                  (size > 2 ? std::uint8_t{0x80} : std::uint8_t{0});
         buf[2] = static_cast<std::uint8_t>(v >> 14 & 0x7f) |
                  (size > 3 ? std::uint8_t{0x80} : std::uint8_t{0});
         buf[3] = static_cast<std::uint8_t>(v >> 21 & 0x7f) |
                  (size > 4 ? std::uint8_t{0x80} : std::uint8_t{0});
         buf[4] = static_cast<std::uint8_t>(v >> 28);
         return size;
      }

      inline std::size_t encode_u64(std::uint8_t* buf,
                                    std::uint64_t v) noexcept
      {
         if (v < 0x80) [[likely]]
         {
            buf[0] = static_cast<std::uint8_t>(v);
            return 1;
         }
         const std::size_t bits = 64 - std::countl_zero(v);
         const std::size_t size = (bits + 6) / 7;  // 2..10

         buf[0] = static_cast<std::uint8_t>(v       & 0x7f) |
                  static_cast<std::uint8_t>(0x80);
         buf[1] = static_cast<std::uint8_t>(v >>  7 & 0x7f) |
                  (size > 2 ? std::uint8_t{0x80} : std::uint8_t{0});
         buf[2] = static_cast<std::uint8_t>(v >> 14 & 0x7f) |
                  (size > 3 ? std::uint8_t{0x80} : std::uint8_t{0});
         buf[3] = static_cast<std::uint8_t>(v >> 21 & 0x7f) |
                  (size > 4 ? std::uint8_t{0x80} : std::uint8_t{0});
         buf[4] = static_cast<std::uint8_t>(v >> 28 & 0x7f) |
                  (size > 5 ? std::uint8_t{0x80} : std::uint8_t{0});
         buf[5] = static_cast<std::uint8_t>(v >> 35 & 0x7f) |
                  (size > 6 ? std::uint8_t{0x80} : std::uint8_t{0});
         buf[6] = static_cast<std::uint8_t>(v >> 42 & 0x7f) |
                  (size > 7 ? std::uint8_t{0x80} : std::uint8_t{0});
         buf[7] = static_cast<std::uint8_t>(v >> 49 & 0x7f) |
                  (size > 8 ? std::uint8_t{0x80} : std::uint8_t{0});
         buf[8] = static_cast<std::uint8_t>(v >> 56 & 0x7f) |
                  (size > 9 ? std::uint8_t{0x80} : std::uint8_t{0});
         buf[9] = static_cast<std::uint8_t>(v >> 63);
         return size;
      }

      // sleb — emit until the high 7 bits AND the sign bit of the
      // payload byte agree (canonical signed-LEB128 termination).
      inline std::size_t encode_i32(std::uint8_t* buf, std::int32_t v) noexcept
      {
         std::uint8_t* start = buf;
         while (true)
         {
            std::uint8_t b    = static_cast<std::uint8_t>(v) & 0x7f;
            std::int32_t next = v >> 7;  // arithmetic shift — sign-extends
            const bool   sign = (b & 0x40) != 0;
            if ((next == 0 && !sign) || (next == -1 && sign))
            {
               *buf++ = b;
               return static_cast<std::size_t>(buf - start);
            }
            *buf++ = b | 0x80;
            v      = next;
         }
      }

      inline std::size_t encode_i64(std::uint8_t* buf, std::int64_t v) noexcept
      {
         std::uint8_t* start = buf;
         while (true)
         {
            std::uint8_t b    = static_cast<std::uint8_t>(v) & 0x7f;
            std::int64_t next = v >> 7;
            const bool   sign = (b & 0x40) != 0;
            if ((next == 0 && !sign) || (next == -1 && sign))
            {
               *buf++ = b;
               return static_cast<std::size_t>(buf - start);
            }
            *buf++ = b | 0x80;
            v      = next;
         }
      }

      inline std::size_t encode_zigzag64(std::uint8_t* buf, std::int64_t v) noexcept
      {
         std::uint64_t zz = (static_cast<std::uint64_t>(v) << 1) ^
                            static_cast<std::uint64_t>(v >> 63);
         return encode_u64(buf, zz);
      }

      // size — number of bytes a uleb encoding will consume.  Used by
      // two-pass writers (compute size, allocate, encode).
      //
      // Hybrid form: explicit `v < 128 → 1` fast path catches the
      // common case (string lengths, small field IDs, packed payload
      // sizes) at the cost of one well-predicted branch.  CLZ-based
      // arithmetic handles everything else — `v|1` keeps countl_zero
      // defined for v == 0.  std::countl_zero is constexpr in C++20.
      inline constexpr std::size_t size_u32(std::uint32_t v) noexcept
      {
         if (v < 0x80) return 1;
         const std::size_t bits = 32 - std::countl_zero(v);
         return (bits + 6) / 7;
      }

      inline constexpr std::size_t size_u64(std::uint64_t v) noexcept
      {
         if (v < 0x80) return 1;
         const std::size_t bits = 64 - std::countl_zero(v);
         return (bits + 6) / 7;
      }

      // ── decode ───────────────────────────────────────────────────
      inline decode_u32_result decode_u32(const std::uint8_t* p,
                                          std::size_t          avail) noexcept
      {
         std::uint32_t v     = 0;
         unsigned      shift = 0;
         for (std::size_t i = 0; i < avail; ++i)
         {
            if (i == max_bytes_u32) return {0, 0, false};
            const std::uint8_t b = p[i];
            v |= static_cast<std::uint32_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0)
               return {v, static_cast<std::uint8_t>(i + 1), true};
            shift += 7;
         }
         return {0, 0, false};
      }

      inline decode_u64_result decode_u64(const std::uint8_t* p,
                                          std::size_t          avail) noexcept
      {
         std::uint64_t v     = 0;
         unsigned      shift = 0;
         for (std::size_t i = 0; i < avail; ++i)
         {
            if (i == max_bytes_u64) return {0, 0, false};
            const std::uint8_t b = p[i];
            v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0)
               return {v, static_cast<std::uint8_t>(i + 1), true};
            shift += 7;
         }
         return {0, 0, false};
      }

      inline decode_i32_result decode_i32(const std::uint8_t* p,
                                          std::size_t          avail) noexcept
      {
         std::int32_t v     = 0;
         unsigned     shift = 0;
         for (std::size_t i = 0; i < avail; ++i)
         {
            if (i == max_bytes_i32) return {0, 0, false};
            const std::uint8_t b = p[i];
            v |= static_cast<std::int32_t>(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) == 0)
            {
               if (shift < 32 && (b & 0x40))
                  v |= -(static_cast<std::int32_t>(1) << shift);
               return {v, static_cast<std::uint8_t>(i + 1), true};
            }
         }
         return {0, 0, false};
      }

      inline decode_i64_result decode_i64(const std::uint8_t* p,
                                          std::size_t          avail) noexcept
      {
         std::int64_t v     = 0;
         unsigned     shift = 0;
         for (std::size_t i = 0; i < avail; ++i)
         {
            if (i == max_bytes_i64) return {0, 0, false};
            const std::uint8_t b = p[i];
            v |= static_cast<std::int64_t>(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) == 0)
            {
               if (shift < 64 && (b & 0x40))
                  v |= -(static_cast<std::int64_t>(1) << shift);
               return {v, static_cast<std::uint8_t>(i + 1), true};
            }
         }
         return {0, 0, false};
      }

      inline decode_i64_result decode_zigzag64(const std::uint8_t* p,
                                               std::size_t avail) noexcept
      {
         const auto r = decode_u64(p, avail);
         if (!r.ok) return {0, 0, false};
         const std::int64_t v = static_cast<std::int64_t>(
            (r.value >> 1) ^ (~(r.value & 1) + 1));
         return {v, r.len, true};
      }

   }  // namespace scalar

   // ────────────────────────────────────────────────────────────────
   // neon — aarch64 NEON decode fast path.
   //
   //   1. Single 16-byte unaligned load brings the whole varint plus
   //      tail slack into a vector register. (Caller pads input by 16
   //      bytes; truncated buffers fall through to the scalar path.)
   //   2. Continuation-mask is `vshrq_n_s8(x, 7)` — arithmetic shift
   //      gives 0xFF for bytes with MSB set, 0x00 otherwise.
   //   3. The standard `vshrn_n_u16(_, 4)` movemask trick packs that
   //      mask into a u64, one nibble per byte. `ctzll(~packed)`
   //      locates the first terminator byte branchlessly.
   //   4. Payload bytes are masked to 7 bits in NEON, then masked
   //      again per-position so only `len` bytes contribute. The
   //      payload combine is a fully-unrolled OR chain — the compiler
   //      schedules it as a tree of independent shifts/ORs, so length
   //      doesn't drive control flow.
   //
   // Encode is left in scalar. The hot encode path is dominated by the
   // sink interface (rewriting cursors, tracking `written()`) rather
   // than the byte loop, so vectorising the loop alone wouldn't move
   // the wire-rate end-to-end measurement and would make the encoder
   // harder to read.
   // ────────────────────────────────────────────────────────────────
#if defined(__ARM_NEON) && (defined(__aarch64__) || defined(_M_ARM64))
   namespace neon {

      // Build the [0,1,...,15] index vector used by the per-position
      // length mask. Static so the compiler can fold the load.
      inline ::uint8x16_t lane_indices() noexcept
      {
         alignas(16) static constexpr std::uint8_t lanes[16] = {
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
         return vld1q_u8(lanes);
      }

      inline decode_u32_result decode_u32(const std::uint8_t* p,
                                          std::size_t          avail) noexcept
      {
         if (avail == 0) return {0, 0, false};
         // Single-byte hot path.
         if ((p[0] & 0x80) == 0) return {p[0], 1, true};
         // Two- and three-byte fast paths — straight-line specializations
         // for the bulk of bin/wasm payload distributions in
         // [2^7, 2^21).  Beats both the NEON 16-byte setup and the
         // 4-byte SWAR aggregate by avoiding the aggregate's mask +
         // ctz + post-mask steps for these very common sizes.
         if (avail >= 2 && (p[1] & 0x80) == 0)
            return {static_cast<std::uint32_t>(p[0] & 0x7f) |
                       (static_cast<std::uint32_t>(p[1]) << 7),
                    2, true};
         if (avail >= 3 && (p[2] & 0x80) == 0)
            return {static_cast<std::uint32_t>(p[0] & 0x7f) |
                       (static_cast<std::uint32_t>(p[1] & 0x7f) << 7) |
                       (static_cast<std::uint32_t>(p[2]) << 14),
                    3, true};
         if (avail < 16) return scalar::decode_u32(p, avail);

         const ::uint8x16_t v = vld1q_u8(p);
         const ::uint8x16_t cont = vreinterpretq_u8_s8(
            vshrq_n_s8(vreinterpretq_s8_u8(v), 7));
         const ::uint8x8_t  packed =
            vshrn_n_u16(vreinterpretq_u16_u8(cont), 4);
         std::uint64_t      mask;
         std::memcpy(&mask, &packed, 8);
         const std::uint64_t terms = ~mask;
         if (terms == 0) return {0, 0, false};
         const unsigned term_pos =
            static_cast<unsigned>(__builtin_ctzll(terms)) >> 2;
         if (term_pos >= max_bytes_u32) return {0, 0, false};
         const unsigned len = term_pos + 1;

         // SWAR shift-pack: pull 8 payload bytes out of the vector
         // register as one u64 (no memory roundtrip), then OR-merge
         // the 7-bit groups via 5 parallel-issued AND/shift pairs.
         // Garbage in bytes ≥ len is cleared by the final post-mask;
         // no per-position lane mask needed.
         const std::uint64_t lo = vgetq_lane_u64(
            vreinterpretq_u64_u8(vandq_u8(v, vdupq_n_u8(0x7f))), 0);
         std::uint32_t result =
            static_cast<std::uint32_t>(lo & 0x7FULL) |
            (static_cast<std::uint32_t>((lo >> 8)  & 0x7FULL) << 7) |
            (static_cast<std::uint32_t>((lo >> 16) & 0x7FULL) << 14) |
            (static_cast<std::uint32_t>((lo >> 24) & 0x7FULL) << 21) |
            (static_cast<std::uint32_t>((lo >> 32) & 0x7FULL) << 28);
         // Post-mask: bytes beyond the terminator contribute garbage
         // to higher bits; clear them in one AND.
         static constexpr std::uint32_t bit_mask_u32[6] = {
            0u, 0x7Fu, 0x3FFFu, 0x1FFFFFu, 0xFFFFFFFu, 0xFFFFFFFFu};
         result &= bit_mask_u32[len];
         return {result, static_cast<std::uint8_t>(len), true};
      }

      inline decode_u64_result decode_u64(const std::uint8_t* p,
                                          std::size_t          avail) noexcept
      {
         if (avail == 0) return {0, 0, false};
         if ((p[0] & 0x80) == 0) return {p[0], 1, true};
         if (avail >= 2 && (p[1] & 0x80) == 0)
            return {static_cast<std::uint64_t>(p[0] & 0x7f) |
                       (static_cast<std::uint64_t>(p[1]) << 7),
                    2, true};
         if (avail >= 3 && (p[2] & 0x80) == 0)
            return {static_cast<std::uint64_t>(p[0] & 0x7f) |
                       (static_cast<std::uint64_t>(p[1] & 0x7f) << 7) |
                       (static_cast<std::uint64_t>(p[2]) << 14),
                    3, true};
         if (avail < 16) return scalar::decode_u64(p, avail);

         const ::uint8x16_t v = vld1q_u8(p);
         const ::uint8x16_t cont = vreinterpretq_u8_s8(
            vshrq_n_s8(vreinterpretq_s8_u8(v), 7));
         const ::uint8x8_t  packed =
            vshrn_n_u16(vreinterpretq_u16_u8(cont), 4);
         std::uint64_t      mask;
         std::memcpy(&mask, &packed, 8);
         const std::uint64_t terms = ~mask;
         if (terms == 0) return {0, 0, false};
         const unsigned term_pos =
            static_cast<unsigned>(__builtin_ctzll(terms)) >> 2;
         if (term_pos >= max_bytes_u64) return {0, 0, false};
         const unsigned len = term_pos + 1;

         // SWAR shift-pack — see decode_u32 for the rationale.
         const ::uint8x16_t payload_v =
            vandq_u8(v, vdupq_n_u8(0x7f));
         const std::uint64_t lo = vgetq_lane_u64(
            vreinterpretq_u64_u8(payload_v), 0);
         std::uint64_t result =
            (lo & 0x7FULL) |
            ((lo & (0x7FULL << 8))  >> 1) |
            ((lo & (0x7FULL << 16)) >> 2) |
            ((lo & (0x7FULL << 24)) >> 3) |
            ((lo & (0x7FULL << 32)) >> 4) |
            ((lo & (0x7FULL << 40)) >> 5) |
            ((lo & (0x7FULL << 48)) >> 6) |
            ((lo & (0x7FULL << 56)) >> 7);
         if (len > 8)
         {
            // Bytes 8 and 9 land in the high u64 lane.
            const std::uint64_t hi = vgetq_lane_u64(
               vreinterpretq_u64_u8(payload_v), 1);
            result |= (hi & 0x7FULL) << 56;
            // Byte 9 contributes only its bit 0 (canonical 10-byte
            // uleb encodes value bit 63 there); higher bits would
            // shift past 64 anyway.
            if (len > 9) result |= ((hi >> 8) & 1ULL) << 63;
         }
         // Post-mask off any garbage from bytes beyond the terminator.
         static constexpr std::uint64_t bit_mask_u64[11] = {
            0ULL,
            (1ULL <<  7) - 1, (1ULL << 14) - 1, (1ULL << 21) - 1,
            (1ULL << 28) - 1, (1ULL << 35) - 1, (1ULL << 42) - 1,
            (1ULL << 49) - 1, (1ULL << 56) - 1, (1ULL << 63) - 1,
            ~0ULL};
         result &= bit_mask_u64[len];
         return {result, static_cast<std::uint8_t>(len), true};
      }

      inline decode_i64_result decode_zigzag64(const std::uint8_t* p,
                                               std::size_t avail) noexcept
      {
         const auto r = decode_u64(p, avail);
         if (!r.ok) return {0, 0, false};
         const std::int64_t v = static_cast<std::int64_t>(
            (r.value >> 1) ^ (~(r.value & 1) + 1));
         return {v, r.len, true};
      }

   }  // namespace neon
#endif

   // ────────────────────────────────────────────────────────────────
   // fast — top-level fast namespace. Aliased to the best decode path
   // available on the target; encode stays scalar (see neon comment).
   // ────────────────────────────────────────────────────────────────
   namespace fast {

      using scalar::encode_u32;
      using scalar::encode_u64;
      using scalar::encode_i32;
      using scalar::encode_i64;
      using scalar::encode_zigzag64;

      using scalar::size_u32;
      using scalar::size_u64;

#if defined(__ARM_NEON) && (defined(__aarch64__) || defined(_M_ARM64))
      using neon::decode_u32;
      using neon::decode_u64;
      using neon::decode_zigzag64;
#else
      using scalar::decode_u32;
      using scalar::decode_u64;
      using scalar::decode_zigzag64;
#endif
      // sleb sign-extension is harder to vectorise; keep it scalar
      // for now until a measured win justifies the added complexity.
      using scalar::decode_i32;
      using scalar::decode_i64;

   }  // namespace fast

   // ────────────────────────────────────────────────────────────────
   // Top-level — re-export the best path for the build target.
   // ────────────────────────────────────────────────────────────────
   using fast::encode_u32;
   using fast::encode_u64;
   using fast::encode_i32;
   using fast::encode_i64;
   using fast::encode_zigzag64;

   using fast::size_u32;
   using fast::size_u64;

   using fast::decode_u32;
   using fast::decode_u64;
   using fast::decode_i32;
   using fast::decode_i64;
   using fast::decode_zigzag64;

}  // namespace psio::varint::leb128
