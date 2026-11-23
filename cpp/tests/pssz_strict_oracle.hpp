#pragma once
// The reference implementation of psio::validate_strict<T>(psio::pssz{}, b)
// — "b is the encoding of the value it decodes to, with no unknown fields"
// — as the library decided it before the check became an in-place walk:
// the exact-schema structural validation, then the whole value decoded, its
// semantic specs run, and the value sized and encoded again; the bytes must
// be the input.
//
// It lives in the tests only: pssz_strict_inplace_cases.hpp (generated
// values of many shapes) and pssz_conformance_tests.cpp (the shared fixture)
// compare the library's answer with this one on every input and on the
// mutations of it that for_each_mutation produces. The library no longer
// builds the value.

#include <psio/pssz.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <vector>

namespace strict_oracle
{
   namespace pi = psio::detail::pssz_impl;

   template <std::size_t W, bool Auto, typename T>
   psio::codec_status materialized(std::span<const char> bytes)
   {
      T decoded = pi::decode_value<W, Auto, T>(bytes, 0, bytes.size());
      if constexpr (psio::Reflected<T>)
      {
         auto specs = psio::validate_specs_on_value(decoded);
         if (!specs.ok())
            return specs;
      }
      if (auto status = pi::check_encode_depth_impl(decoded, 0, {}); !status.ok())
         return psio::codec_fail("pssz: noncanonical encoding", 0, "pssz");
      pi::size_check    check;
      const std::size_t size =
         pi::size_of_v<W, Auto, T, pi::no_ann, pi::size_check*>(decoded, &check);
      if constexpr (Auto)
         pi::enforce_max_dynamic<T>(size, &check);
      if (check.failure || size != bytes.size())
         return psio::codec_fail("pssz: noncanonical encoding", 0, "pssz");
      std::vector<char>     canonical(size);
      psio::fast_buf_stream sink(canonical.data(), canonical.size());
      pi::encode_value<W, Auto>(decoded, sink);
      if (!std::equal(canonical.begin(), canonical.end(), bytes.begin()))
         return psio::codec_fail("pssz: noncanonical encoding", 0, "pssz");
      return psio::codec_ok();
   }

   template <std::size_t W, bool Auto, typename T>
   psio::codec_status validate_exact(std::span<const char> bytes) noexcept
   {
      if (auto cap = pi::check_max_dynamic<T>(bytes); !cap.ok())
         return cap;
      // Exact-schema structural validation only (validate_flags::in_place
      // stays off: no leaf is decoded by the walk).
      pi::validate_flags flags{};
      flags.exact = true;
      auto st     = pi::validate_value<W, Auto, T>(bytes, 0, bytes.size(), 0, &flags);
      if (!st.ok())
         return st;
      if (flags.has_unknown)
         return psio::codec_fail("pssz: unknown fields in exact validation", 0, "pssz");
#if defined(__cpp_exceptions)
      try
      {
         return materialized<W, Auto, T>(bytes);
      }
      catch (...)
      {
         return psio::codec_fail("pssz: decode failed during validate_strict", 0, "pssz");
      }
#else
      return materialized<W, Auto, T>(bytes);
#endif
   }

   template <typename T>
   psio::codec_status validate(psio::pssz, std::span<const char> bytes) noexcept
   {
      return validate_exact<psio::auto_pssz_width_v<T>, true, T>(bytes);
   }
   template <typename T, std::size_t W>
   psio::codec_status validate(psio::pssz_<W>, std::span<const char> bytes) noexcept
   {
      return validate_exact<W, false, T>(bytes);
   }

   // ── Inputs: an encoding and its mutations ─────────────────────────────

   using Bytes = std::vector<char>;
   using Rng   = std::mt19937_64;
   inline std::size_t pick(Rng& rng, std::size_t n) { return n ? std::size_t(rng() % n) : 0; }

   // ── Mutations ─────────────────────────────────────────────────────────

   // f(mutant) for each mutation of `base`; `other` is another encoding of
   // the same type (for splices).
   template <typename F>
   void for_each_mutation(Rng& rng, const Bytes& base, const Bytes& other, F&& f)
   {
      const std::size_t n = base.size();
      f(base);
      // Every byte, set to values that matter to a header, an offset, a
      // selector or a bool; and nudged.
      if (n <= 96)
      {
         static constexpr int values[] = {0, 1, 2, 0xff};
         for (std::size_t i = 0; i < n; ++i)
         {
            for (int v : values)
               if (static_cast<unsigned char>(base[i]) != v)
               {
                  Bytes m = base;
                  m[i]    = static_cast<char>(v);
                  f(m);
               }
            for (int d : {1, -1, 2, 4})
            {
               Bytes m = base;
               m[i]    = static_cast<char>(static_cast<unsigned char>(m[i]) + d);
               f(m);
            }
            // Removed and inserted bytes.
            {
               Bytes m = base;
               m.erase(m.begin() + static_cast<std::ptrdiff_t>(i));
               f(m);
            }
            {
               Bytes m = base;
               m.insert(m.begin() + static_cast<std::ptrdiff_t>(i), char(pick(rng, 3)));
               f(m);
            }
         }
      }
      // Truncations and appended bytes.
      for (std::size_t cut = 1; cut <= n && cut <= 6; ++cut)
      {
         Bytes m(base.begin(), base.end() - static_cast<std::ptrdiff_t>(cut));
         f(m);
      }
      for (int v : {0, 1, 0x61, 0xff})
      {
         Bytes m = base;
         m.push_back(static_cast<char>(v));
         f(m);
         m.push_back(static_cast<char>(v));
         m.push_back(0);
         m.push_back(0);
         f(m);
      }
      if (n == 0)
         return;
      for (int round = 0; round < 24; ++round)
      {
         Bytes m = base;
         switch (pick(rng, 7))
         {
            case 0:  // bit flips
               for (std::size_t k = 0, flips = 1 + pick(rng, 3); k < flips; ++k)
                  m[pick(rng, n)] ^= static_cast<char>(1u << pick(rng, 8));
               break;
            case 1:  // random bytes
               for (std::size_t k = 0, sets = 1 + pick(rng, 2); k < sets; ++k)
                  m[pick(rng, n)] = static_cast<char>(rng());
               break;
            case 2:  // two ranges swapped
            {
               const std::size_t len = 1 + pick(rng, std::min<std::size_t>(8, n / 2 + 1));
               if (2 * len > n) break;
               const std::size_t a = pick(rng, n - 2 * len + 1);
               const std::size_t b = a + len + pick(rng, n - a - 2 * len + 1);
               std::swap_ranges(m.begin() + static_cast<std::ptrdiff_t>(a),
                                m.begin() + static_cast<std::ptrdiff_t>(a + len),
                                m.begin() + static_cast<std::ptrdiff_t>(b));
               break;
            }
            case 3:  // a range duplicated
            {
               const std::size_t len = 1 + pick(rng, std::min<std::size_t>(8, n));
               const std::size_t a   = pick(rng, n - len + 1);
               m.insert(m.begin() + static_cast<std::ptrdiff_t>(a + len),
                        base.begin() + static_cast<std::ptrdiff_t>(a),
                        base.begin() + static_cast<std::ptrdiff_t>(a + len));
               break;
            }
            case 4:  // a range removed
            {
               const std::size_t len = 1 + pick(rng, std::min<std::size_t>(8, n));
               const std::size_t a   = pick(rng, n - len + 1);
               m.erase(m.begin() + static_cast<std::ptrdiff_t>(a),
                       m.begin() + static_cast<std::ptrdiff_t>(a + len));
               break;
            }
            case 5:  // the head of one encoding, the tail of another
            {
               const std::size_t a = pick(rng, n + 1);
               const std::size_t b = pick(rng, other.size() + 1);
               m.assign(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(a));
               m.insert(m.end(), other.begin() + static_cast<std::ptrdiff_t>(b), other.end());
               break;
            }
            default:  // a 16 / 32-bit little-endian field nudged
            {
               const std::size_t width = pick(rng, 2) ? 2 : 4;
               if (n < width) break;
               const std::size_t a = pick(rng, n - width + 1);
               std::uint32_t     v = 0;
               std::memcpy(&v, m.data() + a, width);
               v += static_cast<std::uint32_t>(pick(rng, 9)) - 4;
               std::memcpy(m.data() + a, &v, width);
               break;
            }
         }
         f(m);
      }
   }

   // Whether the library and the reference agree on `input` as a T.
   template <typename T, typename Fmt>
   bool agree(Fmt fmt, const Bytes& input)
   {
      const std::span<const char> span{input};
      return psio::validate_strict<T>(fmt, span).ok() == validate<T>(fmt, span).ok();
   }
}  // namespace strict_oracle
