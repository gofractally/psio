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
#include <psio/adapter.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/schema.hpp>
#include <psio/ssz.hpp>

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
