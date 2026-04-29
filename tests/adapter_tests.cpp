// Adapters — type-scoped format tags.
//
// Exercises the two canonical cases:
//   1. A reflected rich object rendered through a bespoke XML text
//      adapter inside JSON. JSON sees a leaf string whose payload is
//      the XML document; JSON adds quotes + escaping; XML sits opaque.
//   2. A reflected rich object with a delegate_adapter<bin> under
//      binary_category, embedded inside a frac record. Frac sees a
//      variable-length payload (length-prefixed) whose contents are
//      whatever bin wrote.

#include <psio/bin.hpp>
#include <psio/dynamic_bin.hpp>
#include <psio/dynamic_json.hpp>
#include <psio/dynamic_ssz.hpp>
#include <psio/dynamic_value.hpp>
#include <psio/frac.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/adapter.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/reflect.hpp>
#include <psio/schema.hpp>
#include <psio/ssz.hpp>
#include <psio/view.hpp>

#include <algorithm>

#include <catch.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// ── Case 1: XML adapter on a rich reflected struct ────────────────────

struct Widget
{
   std::string              title;
   std::vector<std::int32_t> data;
};
PSIO_REFLECT(Widget, title, data)

// A tiny "XML" serializer — just enough to prove the adapter path.
// The adapter writes characters into a string; it does NOT add
// surrounding quotes or escapes — JSON's outer framing owns that.
struct widget_xml
{
   static std::size_t packsize(const Widget& w) noexcept
   {
      std::string tmp;
      encode(w, tmp);
      return tmp.size();
   }

   static void encode(const Widget& w, std::string& s)
   {
      s += "<widget title=\"";
      s += w.title;
      s += "\">";
      for (auto n : w.data)
      {
         s += "<i>";
         s += std::to_string(n);
         s += "</i>";
      }
      s += "</widget>";
   }

   static Widget decode(std::span<const char> bytes) noexcept
   {
      std::string_view v(bytes.data(), bytes.size());
      Widget           w;

      // Extract title.
      constexpr std::string_view ttag = "title=\"";
      auto t_start = v.find(ttag);
      if (t_start != std::string_view::npos)
      {
         t_start += ttag.size();
         auto t_end = v.find('"', t_start);
         if (t_end != std::string_view::npos)
            w.title = std::string(v.substr(t_start, t_end - t_start));
      }

      // Extract <i>N</i> items.
      auto pos = std::size_t{0};
      while (true)
      {
         auto open = v.find("<i>", pos);
         if (open == std::string_view::npos)
            break;
         open += 3;
         auto close = v.find("</i>", open);
         if (close == std::string_view::npos)
            break;
         std::string num(v.substr(open, close - open));
         w.data.push_back(std::stoi(num));
         pos = close + 4;
      }
      return w;
   }

   static psio::codec_status validate(std::span<const char> bytes) noexcept
   {
      // Structural: starts with "<widget" and ends with "</widget>".
      if (bytes.size() < 16)
         return psio::codec_fail("widget_xml: payload too small", 0,
                                  "widget_xml");
      if (std::memcmp(bytes.data(), "<widget", 7) != 0)
         return psio::codec_fail("widget_xml: missing <widget prefix", 0,
                                  "widget_xml");
      return psio::codec_ok();
   }

   static psio::codec_status
   validate_strict(std::span<const char> bytes) noexcept
   {
      return validate(bytes);
   }
};

PSIO_ADAPTER(Widget, psio::text_category, widget_xml)

// ── Case 2: delegate_adapter<Blob, bin> inside a frac record ───────────

struct Blob
{
   std::string              label;
   std::vector<std::uint8_t> payload;
};
PSIO_REFLECT(Blob, label, payload)

PSIO_ADAPTER(Blob, psio::binary_category,
                 psio::delegate_adapter<Blob, psio::bin>)

struct Envelope
{
   std::int32_t version;
   Blob         body;
   std::string  note;
};
PSIO_REFLECT(Envelope, version, body, note)

// ────────────────────────────────────────────────────────────────────────

TEST_CASE("text adapter routes through JSON as an escaped leaf string",
          "[adapter][json][text]")
{
   Widget w{"hello\nworld", {1, 2, 3}};

   auto json = psio::encode(psio::json{}, w);

   // Outer JSON adds quotes and escapes the inner XML — every " in the
   // XML becomes \", every newline becomes \n.
   REQUIRE(json.front() == '"');
   REQUIRE(json.back() == '"');
   REQUIRE(json.find("\\\"hello\\nworld\\\"") != std::string::npos);
   REQUIRE(json.find("<widget title=") != std::string::npos);
   REQUIRE(json.find("<i>2</i>") != std::string::npos);

   auto back =
      psio::decode<Widget>(psio::json{}, std::span<const char>{json});
   REQUIRE(back.title == "hello\nworld");
   REQUIRE(back.data == std::vector<std::int32_t>{1, 2, 3});
}

TEST_CASE("has_adapter_v reports correctly for registered category",
          "[adapter][traits]")
{
   STATIC_REQUIRE(
      (psio::has_adapter_v<Widget, psio::text_category>));
   STATIC_REQUIRE(
      (!psio::has_adapter_v<Widget, psio::binary_category>));

   STATIC_REQUIRE(
      (psio::has_adapter_v<Blob, psio::binary_category>));
   STATIC_REQUIRE(
      (!psio::has_adapter_v<Blob, psio::text_category>));
}

TEST_CASE("format_has_adapter_v follows format preferred category",
          "[adapter][traits]")
{
   STATIC_REQUIRE(
      (psio::format_has_adapter_v<psio::json, Widget>));
   STATIC_REQUIRE(
      (!psio::format_has_adapter_v<psio::json, Blob>));

   STATIC_REQUIRE(
      (psio::format_has_adapter_v<psio::frac32, Blob>));
   STATIC_REQUIRE(
      (!psio::format_has_adapter_v<psio::frac32, Widget>));
}

TEST_CASE("delegate_adapter<T, bin> routes a record through bin inside frac",
          "[adapter][frac][binary]")
{
   Envelope env{
      .version = 7,
      .body    = Blob{"hdr", {0xDE, 0xAD, 0xBE, 0xEF}},
      .note    = "hi"};

   // Encode the full envelope with frac32. The `body` field — whose
   // type is Blob — has a binary adapter that delegates to bin.
   // Frac frames the projected bytes as a heap-slot variable payload.
   auto bytes = psio::encode(psio::frac32{}, env);

   auto back = psio::decode<Envelope>(psio::frac32{},
                                        std::span<const char>{bytes});
   REQUIRE(back.version == 7);
   REQUIRE(back.body.label == "hdr");
   REQUIRE(back.body.payload ==
           std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
   REQUIRE(back.note == "hi");
}

TEST_CASE("delegate_adapter bytes match standalone bin encode",
          "[adapter][frac][binary]")
{
   Blob b{"tag", {1, 2, 3}};

   // The delegate adapter should produce identical bytes to calling
   // bin directly on the object — nothing of frac's framing appears in
   // the adapter output, only the bin payload.
   std::vector<char> via_proj;
   psio::delegate_adapter<Blob, psio::bin>::encode(b, via_proj);

   auto direct_bin = psio::encode(psio::bin{}, b);
   REQUIRE(via_proj == direct_bin);
}

TEST_CASE("frac correctly classifies a projected type as variable",
          "[adapter][frac][traits]")
{
   // Blob has a binary adapter → frac sees it as opaque runtime-size
   // bytes, so is_fixed<Blob> = false (even though without the
   // adapter Blob would also be variable because it has string +
   // vector fields — the check is that the adapter short-circuits
   // the walk).
   STATIC_REQUIRE(
      !psio::detail::frac_impl::is_fixed_v<Blob>);

   STATIC_REQUIRE(
      psio::detail::frac_impl::has_binary_adapter_v<Blob>);
}

// ── Cross-format adapter dispatch ──────────────────────────────────────
//
// A reflected object with a delegate-to-bin binary adapter should
// round-trip correctly through every binary format. Each format frames
// the adapter output according to its own rules (variable-payload
// heap slot + length for frac/ssz/pssz; u32 length prefix for bin).

TEST_CASE("binary adapter round-trips through bin directly",
          "[adapter][bin]")
{
   Blob b{"x", {1, 2, 3}};
   auto bytes = psio::encode(psio::bin{}, b);
   auto back  = psio::decode<Blob>(psio::bin{}, std::span<const char>{bytes});
   REQUIRE(back.label == "x");
   REQUIRE(back.payload == std::vector<std::uint8_t>{1, 2, 3});
}

TEST_CASE("binary adapter round-trips through ssz",
          "[adapter][ssz]")
{
   Envelope env{.version = 3,
                .body    = Blob{"tag", {9, 8, 7}},
                .note    = "note"};
   auto bytes = psio::encode(psio::ssz{}, env);
   auto back =
      psio::decode<Envelope>(psio::ssz{}, std::span<const char>{bytes});
   REQUIRE(back.version == 3);
   REQUIRE(back.body.label == "tag");
   REQUIRE(back.body.payload == std::vector<std::uint8_t>{9, 8, 7});
   REQUIRE(back.note == "note");
}

TEMPLATE_TEST_CASE("binary adapter round-trips through pssz widths",
                   "[adapter][pssz]", psio::pssz16, psio::pssz32)
{
   using F = TestType;
   Envelope env{.version = 5,
                .body    = Blob{"tag2", {0x11, 0x22, 0x33}},
                .note    = "ok"};
   auto bytes = psio::encode(F{}, env);
   auto back  = psio::decode<Envelope>(F{}, std::span<const char>{bytes});
   REQUIRE(back.version == 5);
   REQUIRE(back.body.label == "tag2");
   REQUIRE(back.body.payload == std::vector<std::uint8_t>{0x11, 0x22, 0x33});
   REQUIRE(back.note == "ok");
}

TEST_CASE("projected payload bytes are opaque to the outer (double framing)",
          "[adapter][framing]")
{
   // The bytes frac writes for a Blob field MUST contain the bin-encoded
   // Blob verbatim (double framing: frac adds its length prefix, bin
   // adds its internal length prefixes — the two layers don't interact).
   Blob b{"hi", {0xAA, 0xBB}};

   auto direct_bin = psio::encode(psio::bin{}, b);

   // The adapter's output matches direct bin encoding.
   std::vector<char> via_proj;
   psio::delegate_adapter<Blob, psio::bin>::encode(b, via_proj);
   REQUIRE(via_proj == direct_bin);
}

// ── Schema integration ───────────────────────────────────────────────────

TEST_CASE("schema_of reports projected descriptor for projected types",
          "[adapter][schema]")
{
   auto widget_sc = psio::schema_of<Widget>();
   REQUIRE(widget_sc.is_projected());
   REQUIRE(widget_sc.as_projected().category ==
           psio::presentation_category::Text);
   REQUIRE(widget_sc.as_projected().logical_name == "Widget");
   REQUIRE(widget_sc.as_projected().presentation_shape->as_primitive() ==
           psio::primitive_kind::String);

   auto blob_sc = psio::schema_of<Blob>();
   REQUIRE(blob_sc.is_projected());
   REQUIRE(blob_sc.as_projected().category ==
           psio::presentation_category::Binary);
   REQUIRE(blob_sc.as_projected().presentation_shape->as_primitive() ==
           psio::primitive_kind::Bytes);
}

// ── Dynamic codec round-trip through a projected type ────────────────────

TEST_CASE("to_dynamic/from_dynamic round-trips a projected type",
          "[adapter][dynamic]")
{
   Widget w{"dyn", {42}};
   auto   dv = psio::to_dynamic(w);
   // Widget's text adapter stores the XML string in the dynamic_value.
   REQUIRE(dv.holds<std::string>());
   REQUIRE(dv.as<std::string>().find("<widget title=\"dyn\">") !=
           std::string::npos);

   auto back = psio::from_dynamic<Widget>(dv);
   REQUIRE(back.title == "dyn");
   REQUIRE(back.data == std::vector<std::int32_t>{42});
}

TEST_CASE("dynamic JSON emits projected types as escaped strings",
          "[adapter][dynamic][json]")
{
   Widget w{"simple", {99}};
   auto   sc = psio::schema_of<Widget>();
   auto   dv = psio::to_dynamic(w);

   auto json_str = psio::json_encode_dynamic(sc, dv);
   REQUIRE(json_str.front() == '"');
   REQUIRE(json_str.back() == '"');
   // Payload is the XML-escaped-into-JSON string.
   REQUIRE(json_str.find("<widget title=") != std::string::npos);

   // Round-trip through dynamic JSON.
   auto dv2 = psio::json_decode_dynamic(
      sc, std::span<const char>{json_str});
   auto back = psio::from_dynamic<Widget>(dv2);
   REQUIRE(back.title == "simple");
   REQUIRE(back.data == std::vector<std::int32_t>{99});
}

TEST_CASE("dynamic SSZ preserves a projected payload round-trip",
          "[adapter][dynamic][ssz]")
{
   Blob b{"sszy", {1, 2, 3, 4, 5}};
   auto sc = psio::schema_of<Blob>();
   auto dv = psio::to_dynamic(b);

   auto ssz_bytes = psio::ssz_encode_dynamic(sc, dv);
   auto dv2 = psio::ssz_decode_dynamic(
      sc, std::span<const char>{ssz_bytes});
   auto back = psio::from_dynamic<Blob>(dv2);

   REQUIRE(back.label == "sszy");
   REQUIRE(back.payload == std::vector<std::uint8_t>{1, 2, 3, 4, 5});
}

// ── Member-level presentation override ────────────────────────────────────
//
// A type registers TWO text adapters under different tags — one for
// a default text presentation (category), one for hex specifically. A
// field annotated with `as<hex_tag>` should use the hex adapter
// even if the enclosing type's default would otherwise pick the
// category's adapter.

struct Hash32
{
   std::array<std::uint8_t, 4> bytes;
};
PSIO_REFLECT(Hash32, bytes)

// Text adapters for Hash32: default text = "raw:NN,NN,..." form;
// hex adapter = "aabbccdd" form.
struct hash32_text
{
   static std::size_t packsize(const Hash32& h) noexcept
   {
      std::string t;
      encode(h, t);
      return t.size();
   }
   static void encode(const Hash32& h, std::string& s)
   {
      s += "raw:";
      for (std::size_t i = 0; i < 4; ++i)
      {
         if (i) s += ',';
         s += std::to_string(h.bytes[i]);
      }
   }
   static Hash32 decode(std::span<const char> b) noexcept
   {
      Hash32 h{};
      // Parse "raw:N,N,N,N"
      std::string_view v(b.data(), b.size());
      auto             comma = v.find(':');
      if (comma == std::string_view::npos) return h;
      auto rest = v.substr(comma + 1);
      std::size_t idx = 0;
      std::size_t start = 0;
      while (start <= rest.size() && idx < 4)
      {
         auto next = rest.find(',', start);
         auto piece = rest.substr(start, next - start);
         std::string tmp(piece);
         h.bytes[idx++] = static_cast<std::uint8_t>(std::stoi(tmp));
         if (next == std::string_view::npos) break;
         start = next + 1;
      }
      return h;
   }
   static psio::codec_status validate(std::span<const char>) noexcept
   {
      return psio::codec_ok();
   }
   static psio::codec_status
   validate_strict(std::span<const char>) noexcept
   {
      return psio::codec_ok();
   }
};

struct hash32_hex
{
   static std::size_t packsize(const Hash32&) noexcept { return 8; }

   // Template-sink so binary formats (vector<char>) and text formats
   // (std::string) can both drive this adapter.
   template <typename Sink>
   static void encode(const Hash32& h, Sink& s)
   {
      char buf[3];
      for (auto b : h.bytes)
      {
         std::snprintf(buf, sizeof(buf), "%02x", b);
         s.insert(s.end(), buf, buf + 2);
      }
   }

   static Hash32 decode(std::span<const char> bytes) noexcept
   {
      Hash32 h{};
      for (std::size_t i = 0; i < 4 && i * 2 + 1 < bytes.size(); ++i)
      {
         unsigned v = 0;
         std::sscanf(bytes.data() + i * 2, "%2x", &v);
         h.bytes[i] = static_cast<std::uint8_t>(v);
      }
      return h;
   }

   static psio::codec_status validate(std::span<const char>) noexcept
   {
      return psio::codec_ok();
   }
   static psio::codec_status
   validate_strict(std::span<const char>) noexcept
   {
      return psio::codec_ok();
   }
};

PSIO_ADAPTER(Hash32, psio::text_category, hash32_text)
PSIO_ADAPTER(Hash32, psio::hex_tag,       hash32_hex)

struct Packet
{
   std::string id;
   Hash32      checksum;  // annotated below with as<hex_tag>
};
PSIO_REFLECT(Packet, id, checksum)

// Member-level override: render `checksum` as hex, not the default
// text presentation.
template <>
inline constexpr auto psio::annotate<&Packet::checksum> = std::tuple{
   psio::as<psio::hex_tag>,
};

TEST_CASE("field without override uses type-default text adapter",
          "[adapter][member-override]")
{
   Hash32 h{{0xDE, 0xAD, 0xBE, 0xEF}};
   auto   s = psio::encode(psio::json{}, h);
   // JSON's category default for Hash32 is text_category ⇒ hash32_text.
   REQUIRE(s.find("raw:222,173,190,239") != std::string::npos);
}

TEST_CASE("field-level as<hex_tag> overrides the default adapter",
          "[adapter][member-override]")
{
   Packet p{"hi", Hash32{{0xCA, 0xFE, 0xBA, 0xBE}}};

   auto json_str = psio::encode(psio::json{}, p);
   // The `checksum` field uses hex adapter — `"cafebabe"` appears
   // as the field's JSON string.
   REQUIRE(json_str.find(R"("checksum":"cafebabe")") != std::string::npos);

   auto back = psio::decode<Packet>(psio::json{},
                                      std::span<const char>{json_str});
   REQUIRE(back.id == "hi");
   REQUIRE(back.checksum.bytes[0] == 0xCA);
   REQUIRE(back.checksum.bytes[3] == 0xBE);
}

// ── Member-override also honored by binary formats ────────────────────────
//
// Register a `hex_tag` adapter for Hash32 that's also usable by
// binary formats (producing ASCII hex bytes). The record walker in
// frac/ssz/pssz/bin should pick the override for the `checksum` field
// and route through the hex adapter regardless of whether those
// formats would have preferred the binary-category adapter.

// Binary formats look up the category via preferred_presentation_category.
// For member-override fields the format bypasses that lookup and uses
// the specific hex_tag adapter even though hex is text-like.

TEST_CASE("member-override works for frac32", "[adapter][member-override][frac]")
{
   Packet p{"hi", Hash32{{0xAB, 0xCD, 0xEF, 0x01}}};
   auto   bytes = psio::encode(psio::frac32{}, p);
   auto   back  = psio::decode<Packet>(psio::frac32{},
                                       std::span<const char>{bytes});
   REQUIRE(back.id == "hi");
   REQUIRE(back.checksum.bytes[0] == 0xAB);
   REQUIRE(back.checksum.bytes[3] == 0x01);

   // Payload of the checksum field should be the hex string "abcdef01"
   // (8 ASCII bytes), not the raw 4 bytes.
   auto direct_hex = std::string{};
   hash32_hex::encode(Hash32{{0xAB, 0xCD, 0xEF, 0x01}}, direct_hex);
   bool hex_present =
      std::search(bytes.begin(), bytes.end(), direct_hex.begin(),
                  direct_hex.end()) != bytes.end();
   REQUIRE(hex_present);
}

TEST_CASE("member-override works for ssz", "[adapter][member-override][ssz]")
{
   Packet p{"n", Hash32{{0x11, 0x22, 0x33, 0x44}}};
   auto   bytes = psio::encode(psio::ssz{}, p);
   auto   back  = psio::decode<Packet>(psio::ssz{},
                                       std::span<const char>{bytes});
   REQUIRE(back.id == "n");
   REQUIRE(back.checksum.bytes[0] == 0x11);
   REQUIRE(back.checksum.bytes[3] == 0x44);
}

TEST_CASE("member-override works for pssz16", "[adapter][member-override][pssz]")
{
   Packet p{"p", Hash32{{0xDE, 0xAD, 0xC0, 0xDE}}};
   auto   bytes = psio::encode(psio::pssz16{}, p);
   auto   back  = psio::decode<Packet>(psio::pssz16{},
                                       std::span<const char>{bytes});
   REQUIRE(back.id == "p");
   REQUIRE(back.checksum.bytes[0] == 0xDE);
   REQUIRE(back.checksum.bytes[3] == 0xDE);
}

TEST_CASE("member-override works for bin", "[adapter][member-override][bin]")
{
   Packet p{"b", Hash32{{0xFE, 0xED, 0xFA, 0xCE}}};
   auto   bytes = psio::encode(psio::bin{}, p);
   auto   back  = psio::decode<Packet>(psio::bin{},
                                       std::span<const char>{bytes});
   REQUIRE(back.id == "b");
   REQUIRE(back.checksum.bytes[0] == 0xFE);
   REQUIRE(back.checksum.bytes[3] == 0xCE);
}

// ── psio::key adapter dispatch ──────────────────────────────────────────
//
// Custom scalar types — fixed-width digests, opaque blobs, types with a
// bespoke sort order — register a `sortable_binary_category` adapter
// and use it transparently as a key. psio::key emits the adapter's
// bytes verbatim (no length prefix), so the adapter is responsible for
// memcmp-sortable output. Round-trips through encode + decode.
//
// `sortable_binary_category` is a separate slot from `binary_category`
// because wire and sort encodings can differ (signed ints, floats).
// For a fixed-width digest the two encodings coincide, so the same
// codec impl is registered in both slots.

struct KeyDigest
{
   std::array<std::uint8_t, 32> bytes{};
   bool operator==(const KeyDigest&) const = default;
};

struct keydigest_codec
{
   static std::size_t packsize(const KeyDigest&) noexcept { return 32; }

   static void encode(const KeyDigest& d, std::vector<char>& s)
   {
      s.insert(s.end(), reinterpret_cast<const char*>(d.bytes.data()),
               reinterpret_cast<const char*>(d.bytes.data()) + 32);
   }

   static KeyDigest decode(std::span<const char> b) noexcept
   {
      KeyDigest d;
      std::memcpy(d.bytes.data(), b.data(), 32);
      return d;
   }

   static psio::codec_status validate(std::span<const char> b) noexcept
   {
      if (b.size() < 32)
         return psio::codec_fail("KeyDigest: short buffer", 0, "key-adapter");
      return psio::codec_ok();
   }

   static psio::codec_status validate_strict(std::span<const char> b) noexcept
   {
      return validate(b);
   }
};

// Wire and sort encodings coincide for this fixed-width digest, so we
// register the same codec impl in both slots. A signed-int wrapper
// would register a 2's-complement codec under binary_category and a
// sign-flipped-BE codec under sortable_binary_category.
PSIO_ADAPTER(KeyDigest, psio::binary_category,          keydigest_codec)
PSIO_ADAPTER(KeyDigest, psio::sortable_binary_category, keydigest_codec)

TEST_CASE("psio::key dispatches into binary_category adapter for scalar types",
          "[adapter][key]")
{
   KeyDigest d{};
   for (std::size_t i = 0; i < 32; ++i)
      d.bytes[i] = static_cast<std::uint8_t>(i);

   auto bytes = psio::encode(psio::key{}, d);
   REQUIRE(bytes.size() == 32);
   for (std::size_t i = 0; i < 32; ++i)
      REQUIRE(static_cast<std::uint8_t>(bytes[i]) == i);

   auto back = psio::decode<KeyDigest>(psio::key{},
                                       std::span<const char>{bytes});
   REQUIRE(back == d);

   REQUIRE(psio::size_of(psio::key{}, d) == 32);
   REQUIRE(psio::validate<KeyDigest>(psio::key{},
                                     std::span<const char>{bytes}).ok());
}

TEST_CASE("psio::key digest adapter sort order is memcmp-sortable",
          "[adapter][key]")
{
   KeyDigest a{}, b{};
   a.bytes[0] = 0x01;
   b.bytes[0] = 0x02;
   auto       ka = psio::encode(psio::key{}, a);
   auto       kb = psio::encode(psio::key{}, b);
   REQUIRE(std::memcmp(ka.data(), kb.data(),
                       std::min(ka.size(), kb.size())) < 0);

   // Two digests differing only in the last byte still order correctly.
   a            = KeyDigest{};
   b            = KeyDigest{};
   a.bytes[31]  = 0x01;
   b.bytes[31]  = 0x02;
   ka           = psio::encode(psio::key{}, a);
   kb           = psio::encode(psio::key{}, b);
   REQUIRE(std::memcmp(ka.data(), kb.data(), 32) < 0);
}

// A reflected record whose first member is an adapted digest, second a
// plain string. Confirms the adapter's bytes embed cleanly mid-record:
// the digest's 32 bytes are followed by the string's NUL-terminated
// encoding. The boundary is preserved by the adapter's fixed packsize.
struct DigestRecord
{
   KeyDigest   d;
   std::string label;
};
PSIO_REFLECT(DigestRecord, d, label)

TEST_CASE("psio::key embeds an adapter-encoded digest inside a record",
          "[adapter][key]")
{
   KeyDigest d{};
   d.bytes[0] = 0xAB;
   d.bytes[31] = 0xCD;

   DigestRecord r{d, "hello"};
   auto         bytes = psio::encode(psio::key{}, r);
   // 32 raw digest bytes + "hello\0\0" = 39 bytes.
   REQUIRE(bytes.size() == 32 + 5 + 2);
   REQUIRE(static_cast<std::uint8_t>(bytes[0])  == 0xAB);
   REQUIRE(static_cast<std::uint8_t>(bytes[31]) == 0xCD);
   REQUIRE(std::string_view(&bytes[32], 5) == "hello");

   auto back = psio::decode<DigestRecord>(psio::key{},
                                          std::span<const char>{bytes});
   REQUIRE(back.d == d);
   REQUIRE(back.label == "hello");
}

// ── Adapter view: zero-copy field access through an adapter-encoded type ─
//
// `view<T, Fmt>` for a type with a registered adapter activates the
// adapter-view specialization: `.bytes()` returns the raw payload
// (zero-copy), `.decode()` reconstructs the owned T. Inside a record,
// `view<Record, Fmt>::get<N>()` for an adapter-typed field N yields
// the adapter view rather than tripping the (now unreachable) record
// walker.

struct PsszEnvelope
{
   std::int32_t version;
   KeyDigest    d;
   std::string  note;
};
PSIO_REFLECT(PsszEnvelope, version, d, note)

TEST_CASE("view<T, pssz>: top-level adapter type exposes bytes() + decode()",
          "[adapter][view][pssz]")
{
   KeyDigest d{};
   for (std::size_t i = 0; i < 32; ++i)
      d.bytes[i] = static_cast<std::uint8_t>(0xA0 + i);

   auto buf = psio::encode(psio::pssz{}, d);

   psio::view<KeyDigest, psio::pssz, psio::storage::const_borrow> v{
       std::span<const char>{buf}};

   // Zero-copy raw access.
   auto raw = v.bytes();
   REQUIRE(raw.size() == 32);
   REQUIRE(static_cast<std::uint8_t>(raw[0])  == 0xA0);
   REQUIRE(static_cast<std::uint8_t>(raw[31]) == 0xBF);

   // Owned-T reconstruction.
   auto back = v.decode();
   REQUIRE(back == d);
}

TEST_CASE("view<Record, pssz>::get<N> on an adapter-typed field",
          "[adapter][view][pssz]")
{
   KeyDigest d{};
   d.bytes[0]  = 0xDE;
   d.bytes[1]  = 0xAD;
   d.bytes[30] = 0xBE;
   d.bytes[31] = 0xEF;

   PsszEnvelope env{77, d, "hi"};
   auto         buf = psio::encode(psio::pssz{}, env);

   psio::view<PsszEnvelope, psio::pssz, psio::storage::const_borrow> rv{
       std::span<const char>{buf}};

   // version is a primitive — returned by value.
   REQUIRE(rv.template get<0>() == 77);

   // d is adapter-typed — returns the adapter view.
   auto dv = rv.template get<1>();
   REQUIRE(dv.bytes().size() == 32);
   REQUIRE(static_cast<std::uint8_t>(dv.bytes()[0])  == 0xDE);
   REQUIRE(static_cast<std::uint8_t>(dv.bytes()[31]) == 0xEF);
   REQUIRE(dv.decode() == d);

   // note is a string — its existing view machinery still works.
   auto nv = rv.template get<2>();
   REQUIRE(std::string_view(nv._psio_data().data(),
                            nv._psio_data().size()) == "hi");
}

// ── ext_int text adapters: uint128 / int128 / uint256 in JSON ────────────
//
// `psio::ext_int.hpp` registers default `text_category` adapters for the
// three big-int types using `0x`-prefixed hex (signed-magnitude for
// int128). JSON's encode/decode chain consults them like any other
// text adapter — these tests cover round-trip, edge values, and the
// negative form for int128.

TEST_CASE("ext_int text adapter: uint128 round-trips through JSON",
          "[adapter][ext_int][json]")
{
   const psio::uint128 zero      = 0;
   const psio::uint128 small     = 0x42;
   const psio::uint128 max64     = (psio::uint128{1} << 64) - 1;
   const psio::uint128 above_64  = (psio::uint128{1} << 64);
   const psio::uint128 max_value = ~psio::uint128{0};

   for (const auto& v : {zero, small, max64, above_64, max_value})
   {
      auto j = psio::encode(psio::json{}, v);
      auto back = psio::decode<psio::uint128>(psio::json{},
                                              std::span<const char>{j});
      REQUIRE(back == v);
      // Sanity-check the textual form: starts with `"0x` and ends with `"`.
      REQUIRE(j.size() >= 5);
      REQUIRE(j.front() == '"');
      REQUIRE(j.back()  == '"');
      REQUIRE(j[1] == '0');
      REQUIRE(j[2] == 'x');
   }

   // Specific shapes.
   REQUIRE(psio::encode(psio::json{}, psio::uint128{0})  == std::string{"\"0x0\""});
   REQUIRE(psio::encode(psio::json{}, psio::uint128{0xFF}) ==
           std::string{"\"0xff\""});
}

TEST_CASE("ext_int text adapter: int128 signed-magnitude hex",
          "[adapter][ext_int][json]")
{
   const psio::int128 zero  =  0;
   const psio::int128 pos   =  0x7e;
   const psio::int128 neg   = -0x7e;
   const psio::int128 lo64  = -(psio::int128{1} << 60);
   const psio::int128 hi    =  (psio::int128{1} << 100);

   for (const auto& v : {zero, pos, neg, lo64, hi})
   {
      auto j    = psio::encode(psio::json{}, v);
      auto back = psio::decode<psio::int128>(psio::json{},
                                             std::span<const char>{j});
      REQUIRE(back == v);
   }

   REQUIRE(psio::encode(psio::json{}, psio::int128{ 0xab}) == std::string{"\"0xab\""});
   REQUIRE(psio::encode(psio::json{}, psio::int128{-0xab}) == std::string{"\"-0xab\""});
}

TEST_CASE("ext_int text adapter: uint256 round-trips through JSON",
          "[adapter][ext_int][json]")
{
   psio::uint256 zero{};
   psio::uint256 small{std::uint64_t{0x42}};
   psio::uint256 mid;
   mid.limb[1] = 0x1234;        // value spans two limbs
   psio::uint256 high;
   high.limb[3] = 0xfedcba9876543210ull;  // top limb only
   psio::uint256 max_value;
   for (auto& l : max_value.limb) l = ~std::uint64_t{0};

   for (const auto& v : {zero, small, mid, high, max_value})
   {
      auto j    = psio::encode(psio::json{}, v);
      auto back = psio::decode<psio::uint256>(psio::json{},
                                              std::span<const char>{j});
      REQUIRE(back == v);
   }

   REQUIRE(psio::encode(psio::json{}, psio::uint256{std::uint64_t{0}}) ==
           std::string{"\"0x0\""});
   REQUIRE(psio::encode(psio::json{}, psio::uint256{std::uint64_t{0xff}}) ==
           std::string{"\"0xff\""});
}

// Confirms that a record with a uint256 field — previously a hard
// compile error in JSON — now serializes via the type-default adapter.
struct TxFee
{
   std::string   sender;
   psio::uint256 amount;
};
PSIO_REFLECT(TxFee, sender, amount)

TEST_CASE("ext_int text adapter: a record with a uint256 field encodes to JSON",
          "[adapter][ext_int][json]")
{
   psio::uint256 amount;
   amount.limb[0] = 0xdeadbeefULL;

   TxFee f{"alice", amount};
   auto  j = psio::encode(psio::json{}, f);
   REQUIRE(j.find("\"sender\":\"alice\"") != std::string::npos);
   REQUIRE(j.find("\"amount\":\"0xdeadbeef\"") != std::string::npos);

   auto back = psio::decode<TxFee>(psio::json{}, std::span<const char>{j});
   REQUIRE(back.sender == "alice");
   REQUIRE(back.amount == amount);
}

// ── float128 — the index_long_double_index Antelope shape ────────────────
//
// `psio::float128` is a 16-byte struct laid out like Berkeley softfloat's
// `float128_t`. Three adapters serve three slots:
//   - text_category           → "0x" + 32 hex chars (raw bits MSB-first)
//   - binary_category         → 16 raw LE bytes
//   - sortable_binary_category → IEEE 754 sort transform + big-endian
//
// Helpers in tests build float128 values from familiar `double` inputs by
// mapping IEEE 754 binary64 onto the high portion of binary128 (a strict
// subset). Real users would bridge from softfloat or platform __float128.

namespace {
   // Convert an IEEE 754 binary64 into a 128-bit binary equivalent.
   // The exponent bias differs (1023 for binary64, 16383 for binary128)
   // and the mantissa width differs (52 vs 112), so we re-bias and
   // shift. Subnormals / inf / NaN handling is best-effort; the tests
   // stick to ordinary finite values.
   psio::float128 from_double(double d)
   {
      std::uint64_t bits;
      std::memcpy(&bits, &d, 8);
      const std::uint64_t sign     = (bits >> 63) & 1;
      const std::uint64_t exp64    = (bits >> 52) & 0x7ff;
      const std::uint64_t frac64   = bits & ((1ull << 52) - 1);

      psio::float128 out{};
      if (exp64 == 0 && frac64 == 0)
      {
         out.limb[1] = sign << 63;          // ±0
         return out;
      }
      // Re-bias: exp128 = exp64 - 1023 + 16383.
      const std::uint64_t exp128 = exp64 + (16383 - 1023);
      // Shift mantissa into the binary128 frame: 112 bits total,
      // top 60 bits go into the high limb, low 52 → high limb low /
      // low limb. The 52-bit mantissa fills the top 52 bits of the
      // 112-bit binary128 fraction.
      const std::uint64_t hi = (sign << 63) | (exp128 << 48) | (frac64 >> 4);
      const std::uint64_t lo = (frac64 & 0xf) << 60;
      out.limb[1] = hi;
      out.limb[0] = lo;
      return out;
   }
}  // namespace

TEST_CASE("ext_int float128: text round-trips through JSON",
          "[adapter][ext_int][json][float128]")
{
   psio::float128 a = from_double(0.0);
   psio::float128 b = from_double(1.5);
   psio::float128 c = from_double(-1.5);
   psio::float128 d = from_double(1e100);

   for (const auto& v : {a, b, c, d})
   {
      auto j    = psio::encode(psio::json{}, v);
      auto back = psio::decode<psio::float128>(psio::json{},
                                               std::span<const char>{j});
      REQUIRE(back == v);
      // Shape check: "0x" + exactly 32 hex chars + closing quote.
      REQUIRE(j.size() == 1 + 2 + 32 + 1);
      REQUIRE(j.front() == '"');
      REQUIRE(j.back()  == '"');
      REQUIRE(j[1] == '0');
      REQUIRE(j[2] == 'x');
   }
}

TEST_CASE("ext_int float128: binary adapter is 16 raw LE bytes",
          "[adapter][ext_int][binary][float128]")
{
   psio::float128 v = from_double(2.5);
   auto bytes = psio::encode(psio::pssz{}, v);
   // pssz frames adapter payloads with a length prefix because adapters
   // are conservatively treated as variable-size; the payload itself
   // must still be exactly 16 bytes for the binary codec.
   REQUIRE(bytes.size() >= 16);
   auto back = psio::decode<psio::float128>(psio::pssz{},
                                            std::span<const char>{bytes});
   REQUIRE(back == v);
}

TEST_CASE("ext_int float128: sortable codec preserves IEEE 754 order",
          "[adapter][ext_int][key][float128]")
{
   const std::vector<double> ds = {
       -1e100, -1.5, -1e-100, -0.0, 0.0, 1e-100, 1.5, 1e100,
   };
   std::vector<std::vector<char>> sorted_bytes;
   for (double x : ds)
      sorted_bytes.push_back(psio::encode(psio::key{}, from_double(x)));

   // memcmp ascending order must match IEEE 754 ascending order. The
   // -0.0 / +0.0 pair is canonicalised to compare equal — they
   // produce identical sort bytes.
   for (std::size_t i = 1; i < sorted_bytes.size(); ++i)
   {
      const auto& a = sorted_bytes[i - 1];
      const auto& b = sorted_bytes[i];
      const int   c = std::memcmp(a.data(), b.data(),
                                  std::min(a.size(), b.size()));
      if (ds[i - 1] == ds[i])  // -0.0, +0.0
         REQUIRE(c == 0);
      else
         REQUIRE(c < 0);
   }

   // Round-trip via the sortable codec.
   for (double x : ds)
   {
      auto v    = from_double(x);
      auto enc  = psio::encode(psio::key{}, v);
      auto back = psio::decode<psio::float128>(psio::key{},
                                                std::span<const char>{enc});
      // -0.0 round-trips to +0.0 (canonicalisation).
      if (x == 0.0 && std::signbit(x))
         REQUIRE(back == from_double(0.0));
      else
         REQUIRE(back == v);
   }
}
