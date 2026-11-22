// Every status-returning entry point on values and inputs that have no
// encoding / are malformed: each must come back with a status. Shared by an
// exception-disabled executable (where a throw would be an abort) and a
// Catch test (where a throw would fail the test).
//
// run_checked_cases() returns 0, or the number of the first case that did
// not report what it must.

#pragma once

#include <psio/annotate.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/schema_builder.hpp>
#include <psio/time.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace never_throws
{
   // A nested record with its own cap: one-byte offsets.
   struct Inner
   {
      std::string text;
      PSIO_REFLECT(Inner, text, maxDynamicData(100))
   };
   // No cap of its own: four-byte offsets, any size.
   struct Outer
   {
      std::uint32_t                id = 0;
      Inner                        inner;
      std::vector<Inner>           more;
      std::optional<Inner>         maybe;
      std::variant<std::uint8_t, Inner> either;
      PSIO_REFLECT(Outer, id, inner, more, maybe, either)
   };
   // Two-byte offsets from the cap; a first payload past 64 KiB cannot be
   // addressed.
   struct Wide
   {
      std::string a;
      std::string b;
      PSIO_REFLECT(Wide, a, b, maxDynamicData(300))
   };
   struct Bounded
   {
      std::string           owner;
      psio::utf8_string<8>  label;
      std::vector<Inner>    rows;
      std::chrono::sys_time<std::chrono::milliseconds> at{};
      double                ratio = 0;
      PSIO_REFLECT(Bounded, attr(owner, max<3>), label, attr(rows, max<2>), at, ratio)
   };
   struct ZeroElements
   {
      std::vector<std::array<std::uint8_t, 0>> nothing;
      PSIO_REFLECT(ZeroElements, nothing)
   };

   using Bytes = std::vector<char>;

   // encode_pssz_checked must fail, leave `out` alone, and say `what`.
   template <typename Fmt, typename T>
   bool refuses(Fmt format, const T& value, std::string_view what)
   {
      Bytes      out{'k', 'e', 'e', 'p'};
      const auto status = psio::encode_pssz_checked(format, value, out);
      return !status.ok() && out == Bytes{'k', 'e', 'e', 'p'} &&
             status.error().what.find(what) != std::string_view::npos;
   }
   template <typename Fmt, typename T>
   bool accepts(Fmt format, const T& value)
   {
      Bytes out{'k'};
      if (!psio::encode_pssz_checked(format, value, out).ok() || out.size() < 1 || out[0] != 'k')
         return false;
      const Bytes body(out.begin() + 1, out.end());
      T           back{};
      bool        unknown = false;
      return body == psio::encode(format, value) &&
             psio::decode_pssz_checked(format, std::span<const char>{body}, back, unknown).ok();
   }

   // decode_pssz_checked / validate / key validate on every truncation and
   // on single-byte corruptions of a valid frame: any answer, no throw.
   template <typename T>
   std::size_t survives_garbage(const Bytes& frame)
   {
      std::size_t accepted = 0;
      auto        feed     = [&](const Bytes& bytes)
      {
         T    out{};
         bool unknown = false;
         accepted += psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{bytes}, out,
                                               unknown)
                        .ok();
         accepted += psio::validate<T>(psio::pssz{}, std::span<const char>{bytes}).ok();
         accepted += psio::validate<T>(psio::pssz_<2>{}, std::span<const char>{bytes}).ok();
         accepted += psio::validate<T>(psio::key{}, std::span<const char>{bytes}).ok();
         T           text_out{};
         accepted += psio::decode_json_checked(psio::json{}, std::span<const char>{bytes}, text_out)
                        .ok();
         accepted += psio::decode_json_checked(psio::ijson{}, std::span<const char>{bytes}, text_out)
                        .ok();
      };
      for (std::size_t n = 0; n <= frame.size(); ++n)
         feed(Bytes(frame.begin(), frame.begin() + n));
      for (std::size_t i = 0; i < frame.size(); ++i)
         for (unsigned char flip : {0x01, 0x80, 0xff})
         {
            Bytes bad = frame;
            bad[i]    = static_cast<char>(static_cast<unsigned char>(bad[i]) ^ flip);
            feed(bad);
         }
      return accepted;
   }

   inline int run_checked_cases()
   {
      const std::string big(200, 'x');

      // ── encode_pssz_checked ────────────────────────────────────────────
      // The top-level cap.
      if (!refuses(psio::pssz{}, Inner{big}, "maxDynamicData")) return 1;
      if (!refuses(psio::pssz_<1>{}, Inner{big}, "")) return 2;
      if (!refuses(psio::pssz_<4>{}, Inner{big}, "maxDynamicData")) return 3;
      // A nested record over ITS cap, in every position a record can be.
      Outer ok;
      ok.inner.text = "fine";
      ok.more       = {Inner{"a"}, Inner{"b"}};
      ok.maybe      = Inner{"c"};
      ok.either     = Inner{"d"};
      if (!accepts(psio::pssz{}, ok)) return 4;
      {
         Outer bad  = ok;
         bad.inner.text = big;
         if (!refuses(psio::pssz{}, bad, "maxDynamicData")) return 5;
      }
      {
         Outer bad = ok;
         bad.more.push_back(Inner{big});
         if (!refuses(psio::pssz{}, bad, "maxDynamicData")) return 6;
      }
      {
         Outer bad = ok;
         bad.maybe = Inner{big};
         if (!refuses(psio::pssz{}, bad, "maxDynamicData")) return 7;
      }
      {
         Outer bad  = ok;
         bad.either = Inner{big};
         if (!refuses(psio::pssz{}, bad, "maxDynamicData")) return 8;
      }
      if (!refuses(psio::pssz{}, std::vector<Inner>{Inner{"a"}, Inner{big}}, "maxDynamicData"))
         return 9;
      if (!refuses(psio::pssz{}, std::optional<Inner>{Inner{big}}, "maxDynamicData")) return 10;
      if (!refuses(psio::pssz{}, std::tuple<std::uint8_t, Inner>{1, Inner{big}}, "maxDynamicData"))
         return 11;
      // A payload the type's static offset width cannot address.
      if (!accepts(psio::pssz{}, Wide{"a", "b"})) return 12;
      if (!refuses(psio::pssz{}, Wide{std::string(70000, 'a'), "b"}, "")) return 13;
      if (!refuses(psio::pssz_<1>{}, Outer{1, Inner{std::string(90, 'a')},
                                           std::vector<Inner>(40, Inner{std::string(90, 'b')}),
                                           std::nullopt, std::uint8_t{0}},
                   "static offset width"))
         return 14;
      if (!refuses(psio::pssz_<2>{}, std::vector<std::string>(3, std::string(40000, 'z')),
                   "static offset width"))
         return 15;
      // Bounds, UTF-8, zero-size elements.
      Bounded fine;
      fine.owner = "bob", fine.label = psio::utf8_string<8>{"ok"}, fine.rows = {Inner{"r"}};
      if (!accepts(psio::pssz{}, fine)) return 16;
      {
         Bounded bad = fine;
         bad.owner   = "bobs";
         if (!refuses(psio::pssz{}, bad, "length_bound")) return 17;
      }
      {
         Bounded bad = fine;
         bad.rows.assign(3, Inner{"r"});
         if (!refuses(psio::pssz{}, bad, "length_bound")) return 18;
      }
      {
         Bounded bad = fine;
         bad.rows    = {Inner{big}};
         if (!refuses(psio::pssz{}, bad, "maxDynamicData")) return 19;
      }
      {
         ZeroElements zero;
         if (!accepts(psio::pssz{}, zero)) return 20;
         zero.nothing.resize(2);
         if (!refuses(psio::pssz{}, zero, "zero-size")) return 21;
      }

      // ── decode / validate ──────────────────────────────────────────────
      {
         const auto frame = psio::encode(psio::pssz{}, ok);
         (void)survives_garbage<Outer>(frame);
         (void)survives_garbage<Bounded>(psio::encode(psio::pssz{}, fine));
         (void)survives_garbage<Wide>(psio::encode(psio::pssz{}, Wide{"left", "right"}));
         (void)survives_garbage<Outer>(psio::encode(psio::key{}, std::tuple<std::uint32_t>{7}));
         const std::string text = psio::encode(psio::json{}, fine);
         (void)survives_garbage<Bounded>(Bytes(text.begin(), text.end()));
      }

      // ── json ───────────────────────────────────────────────────────────
      {
         std::string out = "keep";
         Bounded     bad = fine;
         bad.ratio       = std::numeric_limits<double>::quiet_NaN();
         if (psio::encode_json_checked(psio::json{}, bad, out).ok() || out != "keep") return 30;
         if (psio::encode_json_checked(psio::ijson{}, bad, out).ok() || out != "keep") return 31;
         bad    = fine;
         bad.at = std::chrono::sys_time<std::chrono::milliseconds>::max();
         if (psio::encode_json_checked(psio::json{}, bad, out).ok() || out != "keep") return 32;
         bad       = fine;
         bad.owner = "\xff\xfe";
         if (psio::encode_json_checked(psio::json{}, bad, out).ok() || out != "keep") return 33;
         bad       = fine;
         bad.owner = "four";
         if (psio::encode_json_checked(psio::ijson{}, bad, out).ok() || out != "keep") return 34;
         if (!psio::encode_json_checked(psio::json{}, fine, out).ok()) return 35;
      }

      // ── schema-driven ──────────────────────────────────────────────────
      {
         namespace ir = psio::pssz_ir;
         psio::SchemaBuilder builder;
         builder.insert<Outer>("Outer");
         builder.insert<Bounded>("Bounded");
         const psio::Schema             schema = std::move(builder).build();
         const psio::schema_types::AnyType outer{psio::schema_types::Type{"Outer"}};
         const psio::schema_types::AnyType bounded{psio::schema_types::Type{"Bounded"}};
         // Text of a value whose nested record is over its cap: no bytes.
         Outer small;
         small.inner.text      = "t";
         const std::string good = psio::encode(psio::json{}, small);
         std::string       over = good;
         over.replace(over.find("\"t\""), 3, "\"" + big + "\"");
         Bytes out;
         if (ir::json_to_pssz(schema, outer, psio::json{}, std::span<const char>{over}, out).ok())
            return 40;
         out.clear();
         if (!ir::json_to_pssz(schema, outer, psio::json{}, std::span<const char>{good}, out).ok())
            return 41;
         if (out != psio::encode(psio::pssz{}, small)) return 46;
         std::string text;
         if (!ir::pssz_to_json(schema, outer, psio::ijson{}, std::span<const char>{out}, text).ok())
            return 42;
         // A frame the typed encoder would refuse is refused by the reader.
         const auto frame = psio::detail::pssz_impl::encode_ignoring_length_bounds(
            psio::pssz{}, [&] { auto b = fine; b.owner = "toolong"; return b; }());
         if (ir::validate_pssz(schema, bounded, std::span<const char>{frame}).ok()) return 43;
         text.clear();
         if (ir::pssz_to_json(schema, bounded, psio::json{}, std::span<const char>{frame}, text).ok())
            return 44;
         // A custom handler built from a type goes through the checked codecs.
         ir::CustomHandlers handlers;
         handlers.require_all = true;
         text.clear();
         const auto fine_bytes = psio::encode(psio::pssz{}, fine);
         if (!ir::pssz_to_json(schema, bounded, psio::json{}, std::span<const char>{fine_bytes},
                               text, nullptr, &handlers)
                 .ok())
            return 45;
      }
      return 0;
   }
}  // namespace never_throws
