#pragma once
//
// psio/stream.hpp — shared sink primitives used by every binary format.
//
// Every encoder walks its shape once and writes bytes into a sink. The
// sink concept is uniform across walks so the *same* walker routine
// can drive:
//
//   - `size_stream`      — just counts bytes (the packsize pass)
//   - `fast_buf_stream`  — raw-cursor write into a pre-sized buffer
//                          (after packsize sized it)
//   - `fixed_buf_stream` — raw-cursor write with per-call bounds check
//                          (when packsize was skipped or untrusted)
//   - `vector_stream`    — grow a std::vector as you write
//   - `string_stream`    — grow a std::string as you write (JSON path)
//
// The shared surface is small:
//
//   s.write(char)                   — single byte
//   s.write(const void*, size_t)    — raw bytes
//   s.put(const T&)                 — sizeof(T) raw bytes of T
//   s.write_raw(const T&)           — alias of put, kept for v1 parity
//   s.rewrite(size_t, const void*, size_t)
//   s.rewrite_raw(size_t, const T&) — patch an earlier offset
//   s.skip(int32_t)                 — advance without writing
//   s.about_to_write(size_t)        — optional reserve hint
//   s.written() const               — bytes emitted so far
//
// The `put` method is a psio addition for clarity at call sites —
// `s.put(len)` reads better than `s.write_raw(len)`; both compile to
// the same thing.
//
// These types are direct ports of v1's `psio/stream.hpp`. See the
// v1 header for the long history of tuning that went into them.

#include <psio/error.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psio {

   // ── Bitwise-serialization trait ─────────────────────────────────────────
   //
   // Types that are memcpy-safe on the wire — used by the vector bulk
   // write fast path. Arithmetic + enum types qualify automatically.
   // User types opt in via `psio::is_bitwise_copy<T>` specialization.

   template <typename T>
   struct is_bitwise_copy : std::false_type
   {
   };

   template <typename T>
   constexpr bool has_bitwise_serialization();

   namespace detail {
      template <typename T>
      struct is_std_array_of_bitwise : std::false_type
      {
      };
      template <typename U, std::size_t N>
      struct is_std_array_of_bitwise<std::array<U, N>>
         : std::bool_constant<has_bitwise_serialization<U>() &&
                              sizeof(std::array<U, N>) == N * sizeof(U)>
      {
      };
   }  // namespace detail

   template <typename T>
   constexpr bool has_bitwise_serialization()
   {
      if constexpr (std::is_arithmetic_v<T>)
         return true;
      else if constexpr (std::is_enum_v<T>)
      {
         static_assert(!std::is_convertible_v<T, std::underlying_type_t<T>>,
                       "serializing unscoped enum");
         return true;
      }
      else if constexpr (is_bitwise_copy<T>::value)
         return true;
      else if constexpr (detail::is_std_array_of_bitwise<T>::value)
         return true;
      else
         return false;
   }

   // ── size_stream ─────────────────────────────────────────────────────────
   //
   // Zero-overhead packsize counter. Every write op is a `size += n`
   // arithmetic update — compiler inlines it to a running total.

   struct size_stream
   {
      std::size_t size = 0;

      void about_to_write(std::size_t) noexcept {}

      void write(char) noexcept { ++size; }
      void write(const void*, std::size_t n) noexcept { size += n; }

      void rewrite(std::size_t, const void*, std::size_t) noexcept {}

      template <typename T>
      void put(const T&) noexcept
      {
         size += sizeof(T);
      }
      template <typename T>
      void write_raw(const T& v) noexcept
      {
         put(v);
      }
      template <typename T>
      void rewrite_raw(std::size_t, const T&) noexcept
      {
      }

      void        skip(std::int32_t s) noexcept { size += s; }
      std::size_t written() const noexcept { return size; }
   };

   // ── fixed_buf_stream — bounds-checked raw cursor ────────────────────────
   //
   // Use when the caller cannot prove the buffer is big enough — every
   // write checks `pos + n > end`. Throws `codec_exception` on overrun.

   struct fixed_buf_stream
   {
      char* begin;
      char* pos;
      char* end;

      fixed_buf_stream(char* p, std::size_t size) noexcept
         : begin(p), pos(p), end(p + size)
      {
      }

      void about_to_write(std::size_t) noexcept {}

      void write(char ch)
      {
         if (pos >= end)
            throw codec_exception{
               codec_error{"stream overrun", 0, "fixed_buf_stream"}};
         *pos++ = ch;
      }

      void write(const void* src, std::size_t n)
      {
         if (pos + n > end)
            throw codec_exception{
               codec_error{"stream overrun", 0, "fixed_buf_stream"}};
         std::memcpy(pos, src, n);
         pos += n;
      }

      void rewrite(std::size_t offset, const void* src, std::size_t n)
      {
         std::memcpy(begin + offset, src, n);
      }

      template <typename T>
      void put(const T& v)
      {
         write(&v, sizeof(T));
      }
      template <typename T>
      void write_raw(const T& v)
      {
         put(v);
      }
      template <typename T>
      void rewrite_raw(std::size_t offset, const T& v)
      {
         rewrite(offset, &v, sizeof(T));
      }

      void skip(std::int32_t s)
      {
         if ((pos + s > end) || (pos + s < begin))
            throw codec_exception{
               codec_error{"stream overrun", 0, "fixed_buf_stream"}};
         pos += s;
      }

      std::size_t remaining() const noexcept { return end - pos; }
      std::size_t consumed() const noexcept { return pos - begin; }
      std::size_t written() const noexcept { return pos - begin; }
   };

   // ── fast_buf_stream — un-checked raw cursor ─────────────────────────────
   //
   // Assumes packsize has already proved the buffer is exactly the
   // right size. No bounds checks — every write is a bare memcpy plus
   // pointer advance. This is the hot path for encode.

   struct fast_buf_stream
   {
      char* begin;
      char* pos;
      char* end;

      fast_buf_stream(char* p, std::size_t size) noexcept
         : begin(p), pos(p), end(p + size)
      {
      }

      void about_to_write(std::size_t) noexcept {}

      void write(char ch) noexcept { *pos++ = ch; }

      void write(const void* src, std::size_t n) noexcept
      {
         std::memcpy(pos, src, n);
         pos += n;
      }

      void rewrite(std::size_t offset, const void* src, std::size_t n) noexcept
      {
         std::memcpy(begin + offset, src, n);
      }

      template <typename T>
      void put(const T& v) noexcept
      {
         std::memcpy(pos, &v, sizeof(T));
         pos += sizeof(T);
      }
      template <typename T>
      void write_raw(const T& v) noexcept
      {
         put(v);
      }
      template <typename T>
      void rewrite_raw(std::size_t offset, const T& v) noexcept
      {
         std::memcpy(begin + offset, &v, sizeof(T));
      }

      void skip(std::int32_t s) noexcept { pos += s; }

      std::size_t remaining() const noexcept { return end - pos; }
      std::size_t consumed() const noexcept { return pos - begin; }
      std::size_t written() const noexcept { return pos - begin; }
   };

   // ── vector_stream — grow-into-vector ────────────────────────────────────

   struct vector_stream
   {
      std::vector<char>& data;

      explicit vector_stream(std::vector<char>& d) noexcept : data(d) {}

      void about_to_write(std::size_t amount)
      {
         data.reserve(data.size() + amount);
      }

      void write(char ch) { data.push_back(ch); }

      void write(const void* src, std::size_t n)
      {
         const char* s = static_cast<const char*>(src);
         data.insert(data.end(), s, s + n);
      }

      void rewrite(std::size_t offset, const void* src, std::size_t n)
      {
         std::memcpy(data.data() + offset, src, n);
      }

      template <typename T>
      void put(const T& v)
      {
         write(&v, sizeof(T));
      }
      template <typename T>
      void write_raw(const T& v)
      {
         put(v);
      }
      template <typename T>
      void rewrite_raw(std::size_t offset, const T& v)
      {
         rewrite(offset, &v, sizeof(T));
      }

      // Reserve `n` bytes at the current end. The bytes are
      // value-initialized (zero) so callers can subsequently
      // `rewrite()` over them or leave them as zero placeholders.
      void skip(std::int32_t n) { data.resize(data.size() + n); }

      std::size_t written() const noexcept { return data.size(); }
   };

   // ── string_stream — grow-into-string (JSON path) ────────────────────────

   struct string_stream
   {
      std::string& data;

      explicit string_stream(std::string& d) noexcept : data(d) {}

      void about_to_write(std::size_t amount) { data.reserve(data.size() + amount); }

      void write(char ch) { data.push_back(ch); }

      void write(const void* src, std::size_t n)
      {
         const char* s = static_cast<const char*>(src);
         data.insert(data.end(), s, s + n);
      }

      void rewrite(std::size_t offset, const void* src, std::size_t n)
      {
         std::memcpy(data.data() + offset, src, n);
      }

      template <typename T>
      void put(const T& v)
      {
         write(&v, sizeof(T));
      }
      template <typename T>
      void write_raw(const T& v)
      {
         put(v);
      }
      template <typename T>
      void rewrite_raw(std::size_t offset, const T& v)
      {
         rewrite(offset, &v, sizeof(T));
      }

      std::size_t written() const noexcept { return data.size(); }
   };

   // ── Small on-stack buffer ───────────────────────────────────────────────
   //
   // Used by narrow fast paths that build up a few bytes then emit
   // (varint encoders, etc.). Not a stream per se — direct access to
   // `data` and `pos`.

   template <int MaxSize>
   struct small_buffer
   {
      char  data[MaxSize];
      char* pos{data};

      void reverse() noexcept { std::reverse(data, pos); }
   };

}  // namespace psio
