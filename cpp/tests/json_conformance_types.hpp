#pragma once
// Types and checks shared by the JSON / I-JSON conformance emitter
// (json_conformance_emit.cpp) and the ctest that consumes
// conformance/json-vectors.json (json_conformance_tests.cpp). The Rust side
// (rust/psio/tests/json_conformance.rs) maps the same type ids to Rust types.
//
// Type ids:
//   bool u8 i8 u16 i16 u32 i32 u64 i64 f32 f64
//   string        UTF-8 string       (C++ utf8_string<>, Rust String)
//   raw_string    raw byte string    (C++ std::string,   Rust RawString)
//   bytes         byte list          (C++ vector<uint8_t>, Rust Vec<u8>)
//   bytes4        four bytes         (C++ array<uint8_t,4>, Rust [u8; 4])
//   list<T> array<T,N> optional<T> tuple<...>
//   string<=N     UTF-8, at most N bytes   (C++ utf8_string<N>, Rust BoundedString<N>)
//   list<T><=N    at most N elements       (C++ psio::bounded<T,N>, Rust BoundedList<T,N>)
//   bytes<=N      at most N bytes          (C++ psio::bounded<uint8_t,N>, Rust BoundedList<u8,N>)
//   a record, variant or enum by its name
//
// The fixture also records the Schema IR of every type (`schema_ir`, the
// psio::pssz encoding of one psio::Schema holding each type under its id)
// and, per row, the typed psio::pssz bytes of the value (`pssz`). The
// schema-driven tests (cpp/tests/schema_conformance_tests.cpp,
// rust/psio/tests/schema_conformance.rs) use only those: IR + text must give
// the same bytes, and IR + bytes the same texts, as the typed codecs.

#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace json_conf
{
   using Text  = psio::utf8_string<>;
   using Bytes = std::vector<std::uint8_t>;
   using Bytes4 = std::array<std::uint8_t, 4>;

   inline Text text(std::string s) { return Text{std::move(s)}; }

   struct Inner
   {
      std::uint64_t id = 0;
      Text          label;
      bool          flag = false;
      friend bool   operator==(const Inner&, const Inner&) = default;
   };
   PSIO_REFLECT(Inner, id, label, flag)

   struct Outer
   {
      Inner                                 inner;
      std::vector<Inner>                    items;
      std::optional<Inner>                  maybe;
      Bytes                                 blob;
      std::optional<std::string>            note;
      std::uint32_t                         count = 0;
      double                                ratio = 0;
      std::tuple<std::uint32_t, std::int64_t> pair;
      friend bool operator==(const Outer&, const Outer&) = default;
   };
   PSIO_REFLECT(Outer, inner, items, maybe, blob, note, count, ratio, pair)

   // Member-level integer form overrides, in each direction.
   struct Overrides
   {
      std::uint64_t               a = 0;  // json: string
      std::uint64_t               b = 0;  // ijson: number
      std::uint32_t               c = 0;  // json: string, ijson: string
      std::vector<std::uint64_t>  d;      // ijson: number (through the list)
      std::optional<std::int64_t> e;      // json: string (through the optional)
      std::int64_t                f = 0;  // no override
      friend bool operator==(const Overrides&, const Overrides&) = default;
   };
   PSIO_REFLECT(Overrides, a, b, c, d, e, f)

   // Type-level override: applies to every integer member.
   struct TypeLevel
   {
      std::uint64_t x = 0;
      std::int64_t  y = 0;
      std::uint32_t z = 0;
      friend bool   operator==(const TypeLevel&, const TypeLevel&) = default;
   };
   PSIO_REFLECT(TypeLevel, x, y, z)

   struct AllOptional
   {
      std::optional<std::uint32_t> a;
      std::optional<Text>          b;
      friend bool operator==(const AllOptional&, const AllOptional&) = default;
   };
   PSIO_REFLECT(AllOptional, a, b)

   // Alternatives: a primitive of each integer rule, a string, a record, a
   // byte list. ijson names: uint32, uint64, string, Inner, uint8[].
   using Choice = std::variant<std::uint32_t, std::uint64_t, std::string, Inner, Bytes>;

   struct Holder
   {
      Choice              choice;
      std::vector<Choice> choices;
      friend bool operator==(const Holder&, const Holder&) = default;
   };
   PSIO_REFLECT(Holder, choice, choices)

   // A type with a custom text form: "major.minor".
   struct Version
   {
      std::uint16_t major = 0;
      std::uint16_t minor = 0;
      friend bool   operator==(const Version&, const Version&) = default;
   };

   struct VersionText
   {
      static bool parse(std::span<const char> in, Version& out) noexcept
      {
         auto part = [&](std::size_t& i, std::uint16_t& v) {
            std::uint32_t   acc    = 0;
            const std::size_t start = i;
            while (i < in.size() && in[i] >= '0' && in[i] <= '9')
            {
               acc = acc * 10 + static_cast<std::uint32_t>(in[i] - '0');
               if (acc > 65535) return false;
               ++i;
            }
            if (i == start || (in[start] == '0' && i - start > 1)) return false;
            v = static_cast<std::uint16_t>(acc);
            return true;
         };
         std::size_t i = 0;
         if (!part(i, out.major)) return false;
         if (i >= in.size() || in[i] != '.') return false;
         ++i;
         if (!part(i, out.minor)) return false;
         return i == in.size();
      }
      static std::size_t packsize(const Version& v)
      {
         std::string s;
         encode(v, s);
         return s.size();
      }
      static void encode(const Version& v, std::string& s)
      {
         s += std::to_string(v.major);
         s += '.';
         s += std::to_string(v.minor);
      }
      static Version decode(std::span<const char> in)
      {
         Version v;
         (void)parse(in, v);
         return v;
      }
      static psio::codec_status validate(std::span<const char> in) noexcept
      {
         Version v;
         return parse(in, v) ? psio::codec_ok()
                             : psio::codec_fail("Version: expected major.minor", 0, "Version");
      }
      static psio::codec_status validate_strict(std::span<const char> in) noexcept
      {
         return validate(in);
      }
   };

   struct Release
   {
      Version              version;
      Text                 name;
      std::vector<Version> history;
      friend bool operator==(const Release&, const Release&) = default;
   };
   PSIO_REFLECT(Release, version, name, history)

   struct Tree
   {
      std::vector<Tree> children;
      friend bool       operator==(const Tree&, const Tree&) = default;
   };
   PSIO_REFLECT(Tree, children)

   // ── Length bounds, fixed layout, caps, enums ──────────────────────────
   // Rust: name BoundedString<8>, tags BoundedList<u32,3>, memo
   // BoundedRawString<4>, blob BoundedList<u8,2>, note
   // Option<BoundedRawString<3>>. Its inferred bound selects 1-byte offsets.
   struct Bounds
   {
      psio::utf8_string<8>       name;
      std::vector<std::uint32_t> tags;  // max 3 elements
      std::string                memo;  // max 4 bytes
      Bytes                      blob;  // max 2 bytes
      std::optional<std::string> note;  // max 3 bytes when present
      friend bool operator==(const Bounds&, const Bounds&) = default;
   };
   PSIO_REFLECT(Bounds, name, tags, memo, blob, note)

   // Fixed layout (definition_will_not_change): no header.
   struct Fixed
   {
      std::int32_t x  = 0;
      std::int32_t y  = 0;
      bool         on = false;
      friend bool  operator==(const Fixed&, const Fixed&) = default;
   };
   PSIO_REFLECT(Fixed, x, y, on)

   enum class Color : std::uint8_t
   {
      red   = 0,
      green = 1,
      blue  = 7,
   };

   // Explicit maxDynamicData cap: 2-byte offsets whatever the fields hold.
   struct Capped
   {
      std::uint16_t        id = 0;
      std::string          memo;
      std::optional<Fixed> at;
      std::vector<Fixed>   path;
      friend bool operator==(const Capped&, const Capped&) = default;
   };
   PSIO_REFLECT(Capped, id, memo, at, path)

   struct Shapes
   {
      Color                       color = Color::red;
      std::array<Fixed, 2>        corners{};
      float                       scale = 0;
      std::int16_t                delta = 0;
      std::optional<Text>         label;
      std::optional<std::uint8_t> small;
      Capped                      capped;
      std::optional<Bounds>       bounds;
      friend bool operator==(const Shapes&, const Shapes&) = default;
   };
   PSIO_REFLECT(Shapes, color, corners, scale, delta, label, small, capped, bounds)

   using Text4       = psio::utf8_string<4>;
   using ListU16Max3 = psio::bounded<std::uint16_t, 3>;
   using BytesMax2   = psio::bounded<std::uint8_t, 2>;
   inline bool operator==(const ListU16Max3& a, const ListU16Max3& b)
   {
      return a.storage() == b.storage();
   }
   inline bool operator==(const BytesMax2& a, const BytesMax2& b)
   {
      return a.storage() == b.storage();
   }

   using Array3  = std::array<std::uint16_t, 3>;
   using Tuple0  = std::tuple<>;
   using Tuple3  = std::tuple<std::uint32_t, Text, std::uint64_t>;
   using TupleN  = std::tuple<std::tuple<std::uint8_t, std::int64_t>, std::tuple<>,
                              std::vector<Bytes>>;
}  // namespace json_conf

PSIO_FIELD_ATTRS(json_conf::Overrides, a, psio::json_string)
PSIO_FIELD_ATTRS(json_conf::Overrides, b, psio::ijson_number)
PSIO_FIELD_ATTRS(json_conf::Overrides, c, psio::json_string | psio::ijson_string)
PSIO_FIELD_ATTRS(json_conf::Overrides, d, psio::ijson_number)
PSIO_FIELD_ATTRS(json_conf::Overrides, e, psio::json_string)
PSIO_TYPE_ATTRS(json_conf::TypeLevel, psio::json_string | psio::ijson_number)
PSIO_ADAPTER(json_conf::Version, psio::text_category, json_conf::VersionText)
PSIO_FIELD_ATTRS(json_conf::Bounds, tags, psio::length_bound{.max = 3})
PSIO_FIELD_ATTRS(json_conf::Bounds, memo, psio::length_bound{.max = 4})
PSIO_FIELD_ATTRS(json_conf::Bounds, blob, psio::length_bound{.max = 2})
PSIO_FIELD_ATTRS(json_conf::Bounds, note, psio::length_bound{.max = 3})
PSIO_TYPE_ATTRS(json_conf::Fixed, psio::definition_will_not_change{})
PSIO_TYPE_ATTRS(json_conf::Capped, psio::max_dynamic_data_spec{300})

namespace json_conf
{
   // ── Type registry ─────────────────────────────────────────────────────
   template <typename T>
   struct type_name;
#define JSON_CONF_TYPE(T, ID)                         \
   template <>                                        \
   struct type_name<T>                                \
   {                                                  \
      static constexpr std::string_view id = ID;      \
   };
   JSON_CONF_TYPE(bool, "bool")
   JSON_CONF_TYPE(std::uint8_t, "u8")
   JSON_CONF_TYPE(std::int8_t, "i8")
   JSON_CONF_TYPE(std::uint16_t, "u16")
   JSON_CONF_TYPE(std::int16_t, "i16")
   JSON_CONF_TYPE(std::uint32_t, "u32")
   JSON_CONF_TYPE(std::int32_t, "i32")
   JSON_CONF_TYPE(std::uint64_t, "u64")
   JSON_CONF_TYPE(std::int64_t, "i64")
   JSON_CONF_TYPE(float, "f32")
   JSON_CONF_TYPE(double, "f64")
   JSON_CONF_TYPE(Text, "string")
   JSON_CONF_TYPE(std::string, "raw_string")
   JSON_CONF_TYPE(Bytes, "bytes")
   JSON_CONF_TYPE(Bytes4, "bytes4")
   JSON_CONF_TYPE(std::vector<std::uint64_t>, "list<u64>")
   JSON_CONF_TYPE(std::vector<std::uint32_t>, "list<u32>")
   JSON_CONF_TYPE(std::vector<Bytes>, "list<bytes>")
   JSON_CONF_TYPE(std::vector<std::optional<std::int64_t>>, "list<optional<i64>>")
   JSON_CONF_TYPE(Array3, "array<u16,3>")
   JSON_CONF_TYPE(std::optional<std::uint64_t>, "optional<u64>")
   JSON_CONF_TYPE(std::optional<Text>, "optional<string>")
   JSON_CONF_TYPE(std::optional<Bytes>, "optional<bytes>")
   JSON_CONF_TYPE(Tuple0, "tuple<>")
   JSON_CONF_TYPE(Tuple3, "tuple<u32,string,u64>")
   JSON_CONF_TYPE(TupleN, "tuple<tuple<u8,i64>,tuple<>,list<bytes>>")
   JSON_CONF_TYPE(Inner, "Inner")
   JSON_CONF_TYPE(Outer, "Outer")
   JSON_CONF_TYPE(Overrides, "Overrides")
   JSON_CONF_TYPE(TypeLevel, "TypeLevel")
   JSON_CONF_TYPE(AllOptional, "AllOptional")
   JSON_CONF_TYPE(Choice, "Choice")
   JSON_CONF_TYPE(Holder, "Holder")
   JSON_CONF_TYPE(Version, "Version")
   JSON_CONF_TYPE(Release, "Release")
   JSON_CONF_TYPE(Tree, "Tree")
   JSON_CONF_TYPE(Bounds, "Bounds")
   JSON_CONF_TYPE(Fixed, "Fixed")
   JSON_CONF_TYPE(Color, "Color")
   JSON_CONF_TYPE(Capped, "Capped")
   JSON_CONF_TYPE(Shapes, "Shapes")
   JSON_CONF_TYPE(Text4, "string<=4")
   JSON_CONF_TYPE(ListU16Max3, "list<u16><=3")
   JSON_CONF_TYPE(BytesMax2, "bytes<=2")
#undef JSON_CONF_TYPE

   // The Schema IR has the declared type of a member, not a custom text
   // form: a type with a text adapter (and a record holding one) is not
   // expressible. The fixture records "ir": false for these.
   template <typename T>
   inline constexpr bool has_ir = !std::is_same_v<T, Version> && !std::is_same_v<T, Release>;
   // psio::pssz has no encoding for the psio::bounded wrapper (C++ bounds a
   // pssz list with a member length_bound) nor for a text-adapter type.
   template <typename T>
   inline constexpr bool has_pssz = has_ir<T> && !std::is_same_v<T, ListU16Max3> &&
                                    !std::is_same_v<T, BytesMax2>;

   template <typename T>
   std::string type_id() { return std::string(type_name<T>::id); }

   template <typename Fn>
   void for_each_type(Fn&& fn)
   {
      auto each = [&]<typename... Ts>(std::type_identity<Ts>... ids) { (fn(ids), ...); };
      each(std::type_identity<bool>{}, std::type_identity<std::uint8_t>{},
           std::type_identity<std::int8_t>{}, std::type_identity<std::uint16_t>{},
           std::type_identity<std::int16_t>{}, std::type_identity<std::uint32_t>{},
           std::type_identity<std::int32_t>{}, std::type_identity<std::uint64_t>{},
           std::type_identity<std::int64_t>{}, std::type_identity<float>{},
           std::type_identity<double>{}, std::type_identity<Text>{},
           std::type_identity<std::string>{}, std::type_identity<Bytes>{},
           std::type_identity<Bytes4>{}, std::type_identity<std::vector<std::uint64_t>>{},
           std::type_identity<std::vector<std::uint32_t>>{},
           std::type_identity<std::vector<Bytes>>{},
           std::type_identity<std::vector<std::optional<std::int64_t>>>{},
           std::type_identity<std::array<std::uint16_t, 3>>{},
           std::type_identity<std::optional<std::uint64_t>>{},
           std::type_identity<std::optional<Text>>{}, std::type_identity<std::optional<Bytes>>{},
           std::type_identity<Tuple0>{}, std::type_identity<Tuple3>{},
           std::type_identity<TupleN>{}, std::type_identity<Inner>{},
           std::type_identity<Outer>{}, std::type_identity<Overrides>{},
           std::type_identity<TypeLevel>{}, std::type_identity<AllOptional>{},
           std::type_identity<Choice>{}, std::type_identity<Holder>{},
           std::type_identity<Version>{}, std::type_identity<Release>{},
           std::type_identity<Tree>{}, std::type_identity<Bounds>{},
           std::type_identity<Fixed>{}, std::type_identity<Color>{},
           std::type_identity<Capped>{}, std::type_identity<Shapes>{},
           std::type_identity<Text4>{}, std::type_identity<ListU16Max3>{},
           std::type_identity<BytesMax2>{});
   }

   // ── Fixture rows ──────────────────────────────────────────────────────
   //
   // kind "value":  `json` and `ijson` are the two texts of one value.
   // kind "accept": `text` is read by `format` and re-written as `canonical`.
   // kind "reject": `text` is rejected by `format`.
   // A text that is not valid UTF-8 is carried as `text_hex`.
   struct Row
   {
      std::string                name;
      std::string                type;
      std::string                kind;
      std::optional<std::string> format;
      std::optional<std::string> json;
      std::optional<std::string> ijson;
      std::optional<std::string> text;
      std::optional<std::string> text_hex;
      std::optional<std::string> canonical;
      // Typed psio::pssz bytes of the row's value (lowercase hex), for
      // value and accept rows of a type psio::pssz encodes.
      std::optional<std::string> pssz;
   };
   PSIO_REFLECT(Row, name, type, kind, format, json, ijson, text, text_hex, canonical, pssz)

   struct TypeRow
   {
      std::string id;
      bool        ir   = false;  // the Schema IR describes it
      bool        pssz = false;  // rows of this type carry `pssz`
   };
   PSIO_REFLECT(TypeRow, id, ir, pssz)

   struct Fixture
   {
      std::uint32_t        version = 0;
      std::string          schema_ir;
      std::vector<TypeRow> types;
      std::vector<Row>     vectors;
   };
   PSIO_REFLECT(Fixture, version, schema_ir, types, vectors)

   inline std::string input_text(const Row& row)
   {
      if (row.text) return *row.text;
      std::string out;
      const auto& hex = row.text_hex.value();
      for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
         out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
      return out;
   }

   // ── Checks: "" when the row holds for T, else what failed ────────────
   template <typename Fmt, typename T>
   std::string check_text(const std::string& text, T& value)
   {
      const std::span<const char> in{text};
      if (auto st = psio::validate<T>(Fmt{}, in); !st.ok())
         return std::string("validate: ") + std::string(st.error().what);
      if (auto st = psio::validate_strict<T>(Fmt{}, in); !st.ok())
         return std::string("validate_strict: ") + std::string(st.error().what);
      if (auto st = psio::decode_json_checked(Fmt{}, in, value); !st.ok())
         return std::string("decode_json_checked: ") + std::string(st.error().what);
      if (!(psio::decode<T>(Fmt{}, in) == value)) return "decode differs from decode_json_checked";
      auto boxed = psio::make_boxed<T>(Fmt{}, in);
      if (!boxed || !(*boxed == value)) return "make_boxed differs";
      return "";
   }

   template <typename Fmt, typename T>
   std::string check_writes(const T& value, const std::string& want)
   {
      const std::string got = psio::encode(Fmt{}, value);
      if (got != want) return "encode gives " + got + ", want " + want;
      std::string sink = "x";
      psio::encode(Fmt{}, value, sink);
      if (sink != "x" + want) return "sink encode differs";
      std::string checked;
      if (!psio::encode_json_checked(Fmt{}, value, checked).ok() || checked != want)
         return "encode_json_checked differs";
      if (psio::size_of(Fmt{}, value) != want.size()) return "size_of differs";
      if (psio::encode(Fmt{}, value) != got) return "encode is not deterministic";
      return "";
   }

   template <typename Fmt, typename T>
   std::string check_rejects(const std::string& text)
   {
      const std::span<const char> in{text};
      if (psio::validate<T>(Fmt{}, in).ok()) return "validate accepts";
      if (psio::validate_strict<T>(Fmt{}, in).ok()) return "validate_strict accepts";
      T value{};
      if (psio::decode_json_checked(Fmt{}, in, value).ok()) return "decode_json_checked accepts";
      return "";
   }

   template <typename T>
   std::string check_row(const Row& row)
   {
      auto with = [&](auto&& fn) -> std::string {
         if (row.format == "json") return fn(psio::json{});
         if (row.format == "ijson") return fn(psio::ijson{});
         return "unknown format";
      };
      if (row.kind == "value")
      {
         if (!row.json || !row.ijson) return "value row needs json and ijson";
         T a{}, b{};
         if (auto e = check_text<psio::json, T>(*row.json, a); !e.empty()) return "json " + e;
         if (auto e = check_text<psio::ijson, T>(*row.ijson, b); !e.empty()) return "ijson " + e;
         if (!(a == b)) return "json and ijson texts are different values";
         if (auto e = check_writes<psio::json, T>(a, *row.json); !e.empty()) return "json " + e;
         if (auto e = check_writes<psio::ijson, T>(a, *row.ijson); !e.empty()) return "ijson " + e;
         return "";
      }
      if (row.kind == "accept")
      {
         if (!row.canonical) return "accept row needs canonical";
         return with([&]<typename Fmt>(Fmt) -> std::string {
            T a{}, b{};
            if (auto e = check_text<Fmt, T>(input_text(row), a); !e.empty()) return e;
            if (auto e = check_writes<Fmt, T>(a, *row.canonical); !e.empty()) return e;
            if (auto e = check_text<Fmt, T>(*row.canonical, b); !e.empty()) return "canonical " + e;
            if (!(a == b)) return "canonical text is a different value";
            return "";
         });
      }
      if (row.kind == "reject")
         return with([&]<typename Fmt>(Fmt) { return check_rejects<Fmt, T>(input_text(row)); });
      return "unknown kind " + row.kind;
   }
}  // namespace json_conf
