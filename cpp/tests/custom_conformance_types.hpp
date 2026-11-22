#pragma once
// Fixture types for transparent wrappers, custom text forms of any shape and
// type invariants, shared by the three conformance fixtures
// (conformance/pssz-vectors.json, json-vectors.json, time-key-vectors.json).
// conformance/custom-types.md states every rule here in words: the text
// forms and the invariants are re-implemented by hand in Rust and JS.
//
//   Id        transparent over u64                    (a plain named alias)
//   Blob      transparent over bytes, text: base64
//   Endpoint  variant<V4, V6>, text "127.0.0.1:8080" / "[0000:...:0001]:8080"
//   Ratio     tuple<i16, u16>, text "-3/4"
//   Ref       transparent over variant<u64, raw_string>, text "#42" / "@alice";
//             invariant: a name is not empty
//   Name      fixed record {value: u64}; invariant: value < 2^60
//   DerKey    transparent over bytes; invariant: not empty, first byte 0x30
//   Range     record {lo: u32, hi: u32}; invariant: lo <= hi
//   Level     enum over u8 {low, mid, high, top}; invariant: a listed value
//   Guarded   a record holding all of them in every position

#include <psio/adapter.hpp>
#include <psio/invariant.hpp>
#include <psio/reflect.hpp>
#include <psio/transparent.hpp>
#include <psio/wrappers.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace custom_conf
{
   using Bytes = std::vector<std::uint8_t>;

   // ── Text helpers: canonical decimal, exact ────────────────────────────
   // `0 | [1-9][0-9]*` at `in[i...]`, at most `max`; advances i.
   inline bool parse_decimal(std::span<const char> in, std::size_t& i, std::uint64_t max,
                             std::uint64_t& out) noexcept
   {
      const std::size_t start = i;
      std::uint64_t     acc   = 0;
      while (i < in.size() && in[i] >= '0' && in[i] <= '9')
      {
         const auto digit = static_cast<std::uint64_t>(in[i] - '0');
         if (acc > (max - digit) / 10) return false;
         acc = acc * 10 + digit;
         ++i;
      }
      if (i == start || (in[start] == '0' && i - start > 1)) return false;
      out = acc;
      return true;
   }

   template <typename Impl, typename T>
   struct text_codec
   {
      static std::size_t packsize(const T& v)
      {
         std::string s;
         Impl::encode(v, s);
         return s.size();
      }
      static T decode(std::span<const char> in)
      {
         T v{};
         (void)Impl::parse(in, v);
         return v;
      }
      static psio::codec_status validate(std::span<const char> in) noexcept
      {
         T v{};
         return Impl::parse(in, v) ? psio::codec_ok()
                                   : psio::codec_fail(Impl::error, 0, Impl::schema_id);
      }
      static psio::codec_status validate_strict(std::span<const char> in) noexcept
      {
         return validate(in);
      }
   };

   // A transparent wrapper of a string, with no text form and no invariant:
   // a string in every format, so a map keyed by it is a JSON object.
   struct Tag
   {
      std::string text;
      friend bool operator==(const Tag&, const Tag&) = default;
      friend auto operator<=>(const Tag&, const Tag&) = default;
      PSIO_REFLECT(Tag, transparent(), text)
   };

   // ── Id: a transparent wrapper with nothing else ───────────────────────
   struct Id
   {
      std::uint64_t value = 0;
      friend bool   operator==(const Id&, const Id&) = default;
      friend auto operator<=>(const Id&, const Id&) = default;
      PSIO_REFLECT(Id, transparent(), value)
   };

   // ── Blob: transparent over bytes, base64 text ─────────────────────────
   struct Blob
   {
      Bytes       data;
      friend bool operator==(const Blob&, const Blob&) = default;
      friend auto operator<=>(const Blob&, const Blob&) = default;
   };
   PSIO_REFLECT(Blob, transparent(), data)

   // RFC 4648 section 4 base64 with '=' padding. One text per value: the
   // length is a multiple of 4 and the unused bits of the last group are 0.
   struct BlobText : text_codec<BlobText, Blob>
   {
      static constexpr std::string_view schema_id = "Blob";
      static constexpr std::string_view error     = "Blob: expected canonical base64";
      static constexpr std::string_view alphabet =
         "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

      static void encode(const Blob& v, std::string& s)
      {
         const auto& d = v.data;
         for (std::size_t i = 0; i < d.size(); i += 3)
         {
            const std::size_t   n = std::min<std::size_t>(3, d.size() - i);
            const std::uint32_t w = (std::uint32_t{d[i]} << 16) |
                                    (n > 1 ? std::uint32_t{d[i + 1]} << 8 : 0u) |
                                    (n > 2 ? std::uint32_t{d[i + 2]} : 0u);
            s += alphabet[(w >> 18) & 63];
            s += alphabet[(w >> 12) & 63];
            s += n > 1 ? alphabet[(w >> 6) & 63] : '=';
            s += n > 2 ? alphabet[w & 63] : '=';
         }
      }
      static bool parse(std::span<const char> in, Blob& out) noexcept
      {
         if (in.size() % 4 != 0) return false;
         out.data.clear();
         for (std::size_t i = 0; i < in.size(); i += 4)
         {
            const bool  last = i + 4 == in.size();
            std::size_t pad  = 0;
            if (last && in[i + 3] == '=') pad = in[i + 2] == '=' ? 2 : 1;
            std::uint32_t w = 0;
            for (std::size_t k = 0; k < 4; ++k)
            {
               std::uint32_t digit = 0;
               if (k < 4 - pad)
               {
                  const auto at = alphabet.find(in[i + k]);
                  if (at == std::string_view::npos) return false;
                  digit = static_cast<std::uint32_t>(at);
               }
               w = (w << 6) | digit;
            }
            // The bits a padded group does not use are zero.
            if ((pad == 1 && (w & 0xff) != 0) || (pad == 2 && (w & 0xffff) != 0)) return false;
            out.data.push_back(static_cast<std::uint8_t>(w >> 16));
            if (pad < 2) out.data.push_back(static_cast<std::uint8_t>(w >> 8));
            if (pad < 1) out.data.push_back(static_cast<std::uint8_t>(w));
         }
         return true;
      }
   };

   // ── Endpoint: a variant with a text form ──────────────────────────────
   struct V4
   {
      std::array<std::uint8_t, 4> address{};
      std::uint16_t               port = 0;
      friend bool                 operator==(const V4&, const V4&) = default;
      friend auto operator<=>(const V4&, const V4&) = default;
   };
   PSIO_REFLECT(V4, definitionWillNotChange(), address, port)
   struct V6
   {
      std::array<std::uint8_t, 16> address{};
      std::uint16_t                port = 0;
      friend bool                  operator==(const V6&, const V6&) = default;
      friend auto operator<=>(const V6&, const V6&) = default;
   };
   PSIO_REFLECT(V6, definitionWillNotChange(), address, port)

   // The adapter is registered for this exact C++ type: every
   // std::variant<V4, V6> is an Endpoint.
   using Endpoint = std::variant<V4, V6>;

   // V4: "a.b.c.d:port", each octet and the port canonical decimal.
   // V6: "[g:g:g:g:g:g:g:g]:port", each group exactly four lowercase hex
   // digits (the two address bytes, high byte first): no "::" shorthand.
   struct EndpointText : text_codec<EndpointText, Endpoint>
   {
      static constexpr std::string_view schema_id = "Endpoint";
      static constexpr std::string_view error     = "Endpoint: expected address:port";

      static void encode(const Endpoint& v, std::string& s)
      {
         if (const auto* a = std::get_if<V4>(&v))
         {
            for (std::size_t i = 0; i < 4; ++i)
               s += (i ? "." : "") + std::to_string(a->address[i]);
            s += ":" + std::to_string(a->port);
         }
         else
         {
            static constexpr char hex[] = "0123456789abcdef";
            const auto&           b     = std::get<V6>(v);
            s += '[';
            for (std::size_t i = 0; i < 16; ++i)
            {
               if (i && i % 2 == 0) s += ':';
               s += hex[b.address[i] >> 4];
               s += hex[b.address[i] & 15];
            }
            s += "]:" + std::to_string(b.port);
         }
      }
      static bool parse(std::span<const char> in, Endpoint& out) noexcept
      {
         std::size_t   i = 0;
         std::uint64_t n = 0;
         if (!in.empty() && in[0] == '[')
         {
            V6 v;
            i = 1;
            for (std::size_t byte = 0; byte < 16; ++byte)
            {
               if (byte && byte % 2 == 0)
               {
                  if (i >= in.size() || in[i] != ':') return false;
                  ++i;
               }
               unsigned value = 0;
               for (int k = 0; k < 2; ++k, ++i)
               {
                  if (i >= in.size()) return false;
                  const char c = in[i];
                  if (c >= '0' && c <= '9') value = value * 16 + static_cast<unsigned>(c - '0');
                  else if (c >= 'a' && c <= 'f')
                     value = value * 16 + static_cast<unsigned>(c - 'a' + 10);
                  else return false;
               }
               v.address[byte] = static_cast<std::uint8_t>(value);
            }
            if (i + 1 >= in.size() || in[i] != ']' || in[i + 1] != ':') return false;
            i += 2;
            if (!parse_decimal(in, i, 65535, n) || i != in.size()) return false;
            v.port = static_cast<std::uint16_t>(n);
            out    = v;
            return true;
         }
         V4 v;
         for (std::size_t octet = 0; octet < 4; ++octet)
         {
            if (octet)
            {
               if (i >= in.size() || in[i] != '.') return false;
               ++i;
            }
            if (!parse_decimal(in, i, 255, n)) return false;
            v.address[octet] = static_cast<std::uint8_t>(n);
         }
         if (i >= in.size() || in[i] != ':') return false;
         ++i;
         if (!parse_decimal(in, i, 65535, n) || i != in.size()) return false;
         v.port = static_cast<std::uint16_t>(n);
         out    = v;
         return true;
      }
   };

   // ── Ratio: a tuple with a text form ───────────────────────────────────
   using Ratio = std::tuple<std::int16_t, std::uint16_t>;

   // "n/d": n is `0 | -?[1-9][0-9]*` in the i16 range, d canonical decimal.
   struct RatioText : text_codec<RatioText, Ratio>
   {
      static constexpr std::string_view schema_id = "Ratio";
      static constexpr std::string_view error     = "Ratio: expected numerator/denominator";

      static void encode(const Ratio& v, std::string& s)
      {
         s += std::to_string(std::get<0>(v)) + "/" + std::to_string(std::get<1>(v));
      }
      static bool parse(std::span<const char> in, Ratio& out) noexcept
      {
         std::size_t   i        = 0;
         std::uint64_t n        = 0;
         const bool    negative = !in.empty() && in[0] == '-';
         if (negative) i = 1;
         if (!parse_decimal(in, i, negative ? 32768 : 32767, n) || (negative && n == 0))
            return false;
         const auto numerator =
            static_cast<std::int16_t>(negative ? -static_cast<std::int32_t>(n)
                                               : static_cast<std::int32_t>(n));
         if (i >= in.size() || in[i] != '/') return false;
         ++i;
         if (!parse_decimal(in, i, 65535, n) || i != in.size()) return false;
         out = Ratio{numerator, static_cast<std::uint16_t>(n)};
         return true;
      }
   };

   // ── Ref: a transparent wrapper of a variant, with text and invariant ──
   struct Ref
   {
      std::variant<std::uint64_t, std::string> target;
      friend bool operator==(const Ref&, const Ref&) = default;
      friend auto operator<=>(const Ref&, const Ref&) = default;

      static psio::codec_status check(const Ref& r) noexcept
      {
         if (const auto* name = std::get_if<std::string>(&r.target); name && name->empty())
            return psio::codec_fail("Ref: a name is not empty", 0, "Ref");
         return psio::codec_ok();
      }
      PSIO_REFLECT(Ref, transparent(), invariant(check), target)
   };

   // "#" and a canonical decimal u64, or "@" and the name (the rest of the
   // text, verbatim). "@" alone is the text of the empty name, which the
   // invariant then refuses.
   struct RefText : text_codec<RefText, Ref>
   {
      static constexpr std::string_view schema_id = "Ref";
      static constexpr std::string_view error     = "Ref: expected #number or @name";

      static void encode(const Ref& v, std::string& s)
      {
         if (const auto* n = std::get_if<std::uint64_t>(&v.target))
            s += "#" + std::to_string(*n);
         else
            s += "@" + std::get<std::string>(v.target);
      }
      static bool parse(std::span<const char> in, Ref& out) noexcept
      {
         if (in.empty()) return false;
         if (in[0] == '@')
         {
            out.target = std::string(in.data() + 1, in.size() - 1);
            return true;
         }
         std::size_t   i = 1;
         std::uint64_t n = 0;
         if (in[0] != '#' || !parse_decimal(in, i, ~std::uint64_t{0}, n) || i != in.size())
            return false;
         out.target = n;
         return true;
      }
   };

   // ── Name: a canonical-form integer ────────────────────────────────────
   struct Name
   {
      std::uint64_t value = 0;
      friend bool   operator==(const Name&, const Name&) = default;
      friend auto operator<=>(const Name&, const Name&) = default;

      // Twelve 5-bit characters: the top four bits are not part of a name.
      static psio::codec_status check(const Name& n) noexcept
      {
         return (n.value >> 60) == 0
                   ? psio::codec_ok()
                   : psio::codec_fail("Name: value is not a canonical name", 0, "Name");
      }
      PSIO_REFLECT(Name, definitionWillNotChange(), invariant(check), value)
   };

   // ── DerKey: transparent bytes that must look like a DER sequence ──────
   struct DerKey
   {
      Bytes       data;
      friend bool operator==(const DerKey&, const DerKey&) = default;
      friend auto operator<=>(const DerKey&, const DerKey&) = default;
   };
   inline psio::codec_status check_der(const DerKey& k) noexcept
   {
      return !k.data.empty() && k.data[0] == 0x30
                ? psio::codec_ok()
                : psio::codec_fail("DerKey: not a DER sequence", 0, "DerKey");
   }
   // After the type, at namespace scope: the function is named in full.
   PSIO_REFLECT(DerKey, transparent(), invariant(custom_conf::check_der), data)

   // ── Range: an invariant that relates two members ──────────────────────
   struct Range
   {
      std::uint32_t lo = 0;
      std::uint32_t hi = 0;
      friend bool   operator==(const Range&, const Range&) = default;
      friend auto operator<=>(const Range&, const Range&) = default;
      PSIO_REFLECT(Range, lo, hi,
                   invariant([](const Range& r) noexcept {
                      return r.lo <= r.hi
                                ? psio::codec_ok()
                                : psio::codec_fail("Range: lo is greater than hi", 0, "Range");
                   }))
   };

   // ── Level: a closed enum ──────────────────────────────────────────────
   enum class Level : std::uint8_t
   {
      low  = 0,
      mid  = 1,
      high = 2,
      top  = 3,
   };
   PSIO_REFLECT_ENUM(Level, low, mid, high, top)
   inline psio::codec_status check_level(const Level& l) noexcept
   {
      return static_cast<std::uint8_t>(l) <= 3
                ? psio::codec_ok()
                : psio::codec_fail("Level: not a listed level", 0, "Level");
   }

   // ── Guarded: every custom type, in every position ─────────────────────
   struct Guarded
   {
      Name                      owner;
      std::optional<Name>       delegate;
      std::vector<Name>         members;
      DerKey                    key;
      Range                     window;
      std::vector<Level>        levels;
      std::array<Level, 2>      pair{};
      std::variant<Name, Range> either;
      Ref                       ref;
      Id                        id;
      std::optional<Blob>       blob;
      Endpoint                  endpoint;
      std::vector<Ratio>        ratios;
      friend bool operator==(const Guarded&, const Guarded&) = default;
   };
   PSIO_REFLECT(Guarded, owner, delegate, members, key, window, levels, pair, either, ref, id,
                blob, endpoint, ratios)

   // A value of each type that satisfies every invariant.
   inline Guarded guarded_sample()
   {
      Guarded g;
      g.owner    = Name{5};
      g.delegate = Name{(std::uint64_t{1} << 60) - 1};
      g.members  = {Name{0}, Name{77}};
      g.key      = DerKey{{0x30, 0x03, 0x01, 0x02, 0x03}};
      g.window   = Range{3, 9};
      g.levels   = {Level::low, Level::top};
      g.pair     = {Level::mid, Level::high};
      g.either   = Range{1, 1};
      g.ref      = Ref{std::string{"alice"}};
      g.id       = Id{42};
      g.blob     = Blob{{1, 2, 3, 4}};
      g.endpoint = V4{{127, 0, 0, 1}, 8080};
      g.ratios   = {Ratio{-3, 4}, Ratio{0, 1}};
      return g;
   }
}  // namespace custom_conf

PSIO_ADAPTER(custom_conf::Blob, psio::text_category, custom_conf::BlobText)
PSIO_ADAPTER(custom_conf::Endpoint, psio::text_category, custom_conf::EndpointText)
PSIO_ADAPTER(custom_conf::Ratio, psio::text_category, custom_conf::RatioText)
PSIO_ADAPTER(custom_conf::Ref, psio::text_category, custom_conf::RefText)
// An enum is not a reflected record: its invariant is registered here. The
// schema id defaults to the enum's PSIO_REFLECT_ENUM name ("Level").
PSIO_INVARIANT(custom_conf::Level, custom_conf::check_level)

namespace custom_conf
{
   // Every custom type of the fixtures, for a CustomHandlers registry:
   // fn(std::type_identity<T>{}) per type.
   template <typename Fn>
   void for_each_custom(Fn&& fn)
   {
      fn(std::type_identity<Blob>{});
      fn(std::type_identity<Endpoint>{});
      fn(std::type_identity<Ratio>{});
      fn(std::type_identity<Ref>{});
      fn(std::type_identity<Name>{});
      fn(std::type_identity<DerKey>{});
      fn(std::type_identity<Range>{});
      fn(std::type_identity<Level>{});
   }
}  // namespace custom_conf
