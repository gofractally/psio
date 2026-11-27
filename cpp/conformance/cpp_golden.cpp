// The public C++ codecs define the cross-language wire contract.
#include <psio/conformance.hpp>
#include <psio/bson.hpp>
#include <psio/flatbuf.hpp>
#include <psio/msgpack.hpp>
#include <psio/pjson.hpp>
#include <psio/pjson_to_json.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/protobuf.hpp>

#include <iostream>
#include <bit>
#include <limits>
#include <span>
#include <stdexcept>
#include <tuple>
#include <variant>

struct InteropRecord {
   std::uint32_t id;
   std::string name;
   std::vector<std::uint32_t> values;
   bool active;
   bool operator==(const InteropRecord&) const = default;
};
PSIO_REFLECT(InteropRecord, id, name, values, active)

struct InteropValues { std::vector<std::uint32_t> values; };
PSIO_REFLECT(InteropValues, values)
struct InteropRows { std::vector<InteropRecord> rows; };
PSIO_REFLECT(InteropRows, rows)

struct InteropStrings { std::vector<std::string> values; };
PSIO_REFLECT(InteropStrings, values)
struct InteropNumbers {
   std::uint16_t small = 0; float f = 0; double d = 0; double whole = 0;
   std::vector<float> fs; std::vector<double> ds; std::vector<std::int64_t> is; std::vector<std::uint16_t> us;
};
PSIO_REFLECT(InteropNumbers, small, f, d, whole, fs, ds, is, us)
struct InteropBytes { std::vector<std::uint8_t> values; };
PSIO_REFLECT(InteropBytes, values)

struct InteropEnvelope {
   InteropRecord head;
   std::vector<InteropRecord> rows;
   std::optional<std::string> note;
   std::variant<std::uint32_t, std::string> choice;
   bool operator==(const InteropEnvelope&) const = default;
};
PSIO_REFLECT(InteropEnvelope, head, rows, note, choice)

bool first_fixture = true;

// A row that also carries a JSON text ("json"), escaped, and the canonical
// frame of the same value ("canonical_hex": the row's own bytes when they
// are canonical).
template <typename Bytes>
void emit_json(std::string_view format, std::string_view id, const Bytes& bytes, std::string_view json,
               const std::vector<std::uint8_t>* canonical = nullptr, const bool* is_canonical = nullptr) {
   if (!first_fixture) std::cout << ",\n";
   first_fixture = false;
   std::cout << "  {\"format\":\"" << format << "\",\"id\":\"" << id << "\",\"wire_hex\":\"";
   constexpr char digits[] = "0123456789abcdef";
   for (auto byte : bytes) {
      auto b = static_cast<unsigned char>(byte);
      std::cout << digits[b >> 4] << digits[b & 15];
   }
   std::cout << "\",\"json\":\"";
   for (char c : json) {
      if (c == '"' || c == '\\') std::cout << '\\' << c;
      else if (static_cast<unsigned char>(c) < 0x20)
         std::cout << "\\u00" << digits[static_cast<unsigned char>(c) >> 4] << digits[c & 15];
      else std::cout << c;
   }
   std::cout << "\"";
   if (canonical) {
      std::cout << ",\"canonical_hex\":\"";
      for (auto byte : *canonical) std::cout << digits[byte >> 4] << digits[byte & 15];
      std::cout << "\"";
   }
   if (is_canonical) std::cout << ",\"canonical\":\"" << (*is_canonical ? "yes" : "no") << "\"";
   std::cout << "}";
}

template <typename Bytes>
void emit(std::string_view format, std::string_view id, const Bytes& bytes) {
   if (!first_fixture) std::cout << ",\n";
   first_fixture = false;
   std::cout << "  {\"format\":\"" << format << "\",\"id\":\"" << id
             << "\",\"wire_hex\":\"";
   constexpr char digits[] = "0123456789abcdef";
   for (auto byte : bytes) {
      auto b = static_cast<unsigned char>(byte);
      std::cout << digits[b >> 4] << digits[b & 15];
   }
   std::cout << "\"}";
}

template <typename T>
void typed_fixture(std::string_view id, const T& value) {
   auto bytes = psio::from_struct(value);
   // Every typed encoder writes the canonical form.
   if (!psio::pjson::validate(bytes)) throw std::runtime_error(std::string(id));
   emit("pjson_typed", id, bytes);
}

template <typename Fmt, typename T>
void fixture(std::string_view format, std::string_view id, Fmt fmt, const T& value) {
   auto bytes = psio::encode(fmt, value);
   auto span = std::span<const char>{bytes};
   auto status = psio::validate<T>(fmt, span);
   if (!status.ok())
      throw std::runtime_error(std::string(format) + ": " + std::string(id) + ": " +
                               std::string(status.error().what) + " at " + std::to_string(status.error().byte_offset));
   if (psio::decode<T>(fmt, span) != value)
      throw std::runtime_error(std::string(format) + ": " + std::string(id) + ": roundtrip differs");
   emit(format, id, bytes);
}

template <typename Fmt>
void records(std::string_view format, Fmt fmt) {
   fixture(format, "record", fmt, InteropRecord{42, "Alice", {1, 256, 65536}, true});
   fixture(format, "empty_record", fmt, InteropRecord{0, "", {}, false});
}

int main() {
   try {
      std::cout << "[\n";
#define RECORDS(Name, Fmt) records(#Name, Fmt);
      PSIO_FOR_EACH_SYMMETRIC_BINARY_FMT(RECORDS)
      PSIO_FOR_EACH_TEXT_FMT(RECORDS)
#undef RECORDS
      records("frac16", psio::experimental::frac16{});
      records("bson", psio::bson{});
      records("msgpack", psio::msgpack{});
      records("protobuf", psio::protobuf{});
      records("flatbuf", psio::flatbuf{});
      fixture("frac32", "u32", psio::experimental::frac32{}, std::uint32_t{0xdeadbeef});
      fixture("frac32", "string", psio::experimental::frac32{}, std::string{"hello"});
      fixture("frac32", "vector", psio::experimental::frac32{}, std::vector<std::uint32_t>{1, 2, 3});
      fixture("frac32", "optional_some", psio::experimental::frac32{}, std::optional<std::uint32_t>{42});
      fixture("frac32", "optional_none", psio::experimental::frac32{}, std::optional<std::uint32_t>{});
      fixture("frac32", "nested", psio::experimental::frac32{}, InteropEnvelope{
         {42,"Alice",{1,256,65536},true}, {{0,"",{},false},{7,"Bob",{9},true}},
         "hello", std::string{"chosen"}});
      fixture("frac32", "nested_empty", psio::experimental::frac32{}, InteropEnvelope{
         {0,"",{},false}, {}, std::nullopt, std::uint32_t{7}});
      fixture("frac32", "string_vector", psio::experimental::frac32{}, std::vector<std::string>{"", "hello", "world"});
      fixture("frac32", "optional_vector", psio::experimental::frac32{}, std::vector<std::optional<std::string>>{std::nullopt, "", "hello"});
      fixture("frac32", "optional_empty_string", psio::experimental::frac32{}, std::optional<std::string>{""});
      fixture("frac32", "optional_empty_vector", psio::experimental::frac32{}, std::optional<std::vector<std::uint32_t>>{std::in_place});
      fixture("frac32", "optional_missing_vector", psio::experimental::frac32{}, std::optional<std::vector<std::uint32_t>>{});
      // Vector slots point to complete values. The legacy embedded-container
      // sentinels (0/1) and offsets into the fixed section are not valid here.
      for (unsigned offset = 0; offset < 4; ++offset) {
         std::vector<char> bytes{4,0,0,0,static_cast<char>(offset),0,0,0};
         if (psio::validate<std::vector<std::string>>(psio::experimental::frac32{}, std::span<const char>{bytes}).ok() ||
             psio::validate<std::vector<std::optional<std::string>>>(psio::experimental::frac32{}, std::span<const char>{bytes}).ok())
            throw std::runtime_error("C++ accepted a vector offset into its fixed section");
         emit("frac32_invalid_strings", std::to_string(offset), bytes);
         emit("frac32_invalid_optionals", std::to_string(offset), bytes);
      }
      for (auto [id, value] : std::vector<std::pair<std::string, psio::pjson_value>>{
         {"null", psio::pjson_null{}}, {"true", true}, {"false", false},
         {"uint_inline", std::int64_t{5}}, {"uint", std::int64_t{256}},
         {"negative", std::int64_t{-5}}, {"string", std::string{"hello"}},
         {"decimal", psio::pjson_number{12345, -2}}, {"float", 1.0 / 7.0},
         {"bytes", psio::pjson_bytes{0, 127, 255}},
         {"array", psio::pjson_array{std::int64_t{1}, std::string{"hi"}, true}},
         {"object", psio::pjson_object{{"id", std::int64_t{42}}, {"name", std::string{"Alice"}}}}
      }) {
         auto bytes = psio::pjson::encode(value);
         if (!psio::pjson::validate({bytes.data(), bytes.size()}))
            throw std::runtime_error("pjson: " + id);
         emit("pjson", id, bytes);
      }
      const std::vector<double> numbers{0.0, -0.0, 1.0, -1.0, 1.5, 3.14, 0.1,
         1e-4, 1e-5, 1e6, 1e15, 1e20, 1e38, 1.5e308, 5e-324,
         std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
         std::numeric_limits<double>::quiet_NaN()};
      // A number with NO declared type (a JSON number, a JS number): its
      // form is chosen from the value — an integer in the integer forms,
      // else the smaller of decimal and binary64. The id is the bits of the
      // double whose shortest digits the number is. (A TYPED double is its
      // bits, `63` and eight bytes: the pjson_typed row "numbers".)
      const auto f64_row = [](double value) {
         std::uint8_t buf[32];
         const auto   n = psio::pjson_detail::canonical_number_bytes(psio::pjson_detail::float_reads_as(value), buf);
         const std::vector<std::uint8_t> bytes(buf, buf + n);
         if (!psio::pjson::validate(bytes)) throw std::runtime_error("pjson_f64: not canonical");
         emit("pjson_f64", std::to_string(std::bit_cast<std::uint64_t>(value)), bytes);
      };
      for (auto value : numbers) f64_row(value);
      // A deterministic sample exercises shortest-decimal choices across the exponent range.
      std::uint64_t bits = 0x123456789abcdef0ULL;
      for (unsigned i = 0; i < 256; ++i) {
         bits ^= bits << 13; bits ^= bits >> 7; bits ^= bits << 17;
         f64_row(std::bit_cast<double>(bits));
      }
      // Lists of numbers with NO declared element type: the form is the
      // values' — the narrowest integer code; binary64 when it carries
      // every element and is not larger; else a generic array. The id is
      // the list.
      {
         const auto list_row = [](std::string_view format, std::string_view id, const auto& list) {
            using E = typename std::remove_cvref_t<decltype(list)>::value_type;
            std::vector<std::uint8_t> typed(1 + list.size() * sizeof(E) + 2);
            psio::pjson_detail::encode_typed_array_at<E>(typed.data(), 0, std::span<const E>{list.data(), list.size()});
            const auto bytes = psio::pjson::canonicalize(typed);
            if (!psio::pjson::validate(bytes)) throw std::runtime_error("an anonymous list is not canonical: " + std::string(id));
            emit(format, id, bytes);
         };
         using I = std::vector<std::int64_t>;
         list_row("pjson_list_i64", "", I{});
         list_row("pjson_list_i64", "1,2,3", I{1, 2, 3});
         list_row("pjson_list_i64", "0,255", I{0, 255});
         list_row("pjson_list_i64", "0,256", I{0, 256});
         list_row("pjson_list_i64", "65536", I{65536});
         list_row("pjson_list_i64", "4294967296", I{4294967296});
         list_row("pjson_list_i64", "-1,127", I{-1, 127});
         list_row("pjson_list_i64", "-128,128", I{-128, 128});
         list_row("pjson_list_i64", "-32769,5", I{-32769, 5});
         list_row("pjson_list_i64", "-2147483649,5", I{-2147483649ll, 5});
         list_row("pjson_list_i64", "9223372036854775807,-9223372036854775808",
                  I{9223372036854775807ll, -9223372036854775807ll - 1});
         using D = std::vector<double>;
         list_row("pjson_list_f64", "", D{});
         list_row("pjson_list_f64", "1,2,300", D{1, 2, 300});
         list_row("pjson_list_f64", "0.1,0.2", D{0.1, 0.2});
         list_row("pjson_list_f64", "1,0.5", D{1, 0.5});
         list_row("pjson_list_f64", "0.3333333333333333", D{1.0 / 3});
         list_row("pjson_list_f64", "0.1,0.3333333333333333", D{0.1, 1.0 / 3});
         list_row("pjson_list_f64", "0.3333333333333333,0.14285714285714285,0.1111111111111111", D{1.0 / 3, 1.0 / 7, 1.0 / 9});
         list_row("pjson_list_f64", "19.99,5,1e300", D{19.99, 5, 1e300});
         list_row("pjson_list_f64", "1e300", D{1e300});
         list_row("pjson_list_f64", "-1,2", D{-1, 2});
      }
      psio::pjson_array boundaries;
      for (std::int64_t n = -16; n <= 16; ++n) boundaries.emplace_back(n);
      emit("pjson_extra", "signed_inline_boundaries", psio::pjson::encode(psio::pjson_value{boundaries}));
      emit("pjson_extra", "negative_slots", psio::pjson::encode(psio::pjson_value{
         psio::pjson_object{{"negative", std::int64_t{-5}}, {"padding", std::string(250, 'x')}, {"tail", std::int64_t{-15}}}}));
      typed_fixture("u32_vector", InteropValues{{1,256,65536}});
      typed_fixture("empty_u32_vector", InteropValues{{}});
      typed_fixture("record_rows", InteropRows{
              {{42, "Alice", {1,256,65536}, true}, {0, "", {}, false}}});
      typed_fixture("empty_record_rows", InteropRows{{}});
      typed_fixture("strings", InteropStrings{{"hello", "", std::string(300, 'x')}});
      typed_fixture("empty_strings", InteropStrings{{}});
      typed_fixture("bytes", InteropBytes{{0,127,255}});
      typed_fixture("empty_bytes", InteropBytes{{}});
      // A typed number is stored as its type says: an integer in the fewest
      // bytes, a float as its bits at its declared width, a list as a typed
      // array of its declared element code — whatever the values.
      typed_fixture("numbers", InteropNumbers{5, 0.1f, 0.1, 5.0, {0.1f, 0.2f}, {0.1, 0.2}, {1, 2, 3}, {1, 2, 3}});
      typed_fixture("empty_numbers", InteropNumbers{});
      for (auto [id, value] : std::vector<std::pair<std::string, psio::pjson_value>>{
         {"empty_array", psio::pjson_array{}}, {"empty_object", psio::pjson_object{}},
         {"long_key", psio::pjson_object{{std::string(300, 'x'), std::int64_t{7}}}},
         {"suffix_key", psio::pjson_object{{"name.string", std::string{"Alice"}}}},
         {"unicode", std::string{"hello \xF0\x9F\x8C\x8D"}},
         {"duplicates", psio::pjson_object{{"x", std::int64_t{1}}, {"x", std::int64_t{2}}}}
      }) {
         emit("pjson_extra", id, psio::pjson::encode(value));
      }
      for (auto [id, bytes] : std::vector<std::pair<std::string, std::vector<std::uint8_t>>>{
         {"empty", {}}, {"reserved_type", {0xe0}}, {"negative_zero", {0x50,0}},
         {"inline_negative_zero", {0x30}}, {"inline_trailing", {0x35,0}},
         {"unsupported_numeric_string", {0x80,0x25}}, {"unsupported_extension", {0xd0}},
         {"reserved_bytes", {0xa1,0}}, {"truncated_array", {0xb0,0,1,0}},
         {"truncated_object", {0xc0,0,1,0}},
         {"row_key_outside_buffer", {0xc1,0,1,0xff,0xff,0xff,0xfe,0,0,0}},
         {"row_reserved_width", {0xc1,0xf0,1,0,0,0,0,0,0,0}},
         // §3 / §9: the low nibble of null is reserved and must be 0.
         {"null_low_nibble_1", {0x01}}, {"null_low_nibble_15", {0x0f}},
         {"null_low_nibble_in_array", {0xb0,0x00,0x01,0x00,0x01,0x00}}
      }) {
         // Not a frame at all: refused by the lazy tag too.
         if (psio::pjson_lazy::validate(bytes) || psio::pjson_lazy::try_decode(bytes) || psio::pjson::validate(bytes))
            throw std::runtime_error("Accepted invalid pjson: " + id);
         emit("pjson_invalid", id, bytes);
      }
      // Well-formed pjson frames (gofractally/psio#17). For each:
      //   "wire_hex"       the frame; the lazy tag accepts it;
      //   "json"           the JSON text every codec gives for it;
      //   "canonical"      "yes" when psio::pjson accepts it: it is the one
      //                    encoding of its value in the types it is written
      //                    in;
      //   "canonical_hex"  what the canonical pass gives for it as a
      //                    document WITHOUT types — and what the canonical
      //                    transcode of "json" gives (ids not starting
      //                    "bytes").
      for (const auto& [id, bytes, json] : std::vector<std::tuple<std::string, std::vector<std::uint8_t>, std::string>>{
         {"uint_inline", {0x25}, "5"},
         {"nint_inline", {0x35}, "-5"},
         {"u64_max", {0x47,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}, "18446744073709551615"},
         {"u128_max", {0x4f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
          "340282366920938463463374607431768211455"},
         {"negint_128_bits", {0x5f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
          "-340282366920938463463374607431768211455"},
         {"minus_zero_binary16", {0x61,0x00,0x80}, "-0"},
         {"half_binary16", {0x61,0x00,0x38}, "0.5"},
         {"decimal_tenth", {0x70,0x02,0x01}, "0.1"},
         {"decimal_1e400", {0x70,0x02,0x60,0x0c}, "1e400"},
         {"decimal_minus_15e-401", {0x70,0x1d,0x61,0x0c}, "-1.5e-400"},
         {"decimal_scale_2", {0x70,0x02,0x04}, "100"},
         {"decimal_small_fraction", {0x70,0x02,0x07}, "0.0001"},
         {"string_raw_non_ascii", {0x90,0x68,0xc3,0xa9,0xe4,0xb8,0xad}, "\"h\xc3\xa9\xe4\xb8\xad\""},
         {"string_raw_needs_escape", {0x90,0x61,0x22,0x5c,0x0a,0x62}, "\"a\\\"\\\\\\nb\""},
         {"string_escape_form", {0x91,0x5c,0x75,0x30,0x30,0x65,0x39}, "\"\\u00e9\""},
         {"bytes_base64", {0xa0,0x09,0x08}, "\"CQg=\""},
         {"array_generic", {0xb0,0x00,0x21,0x22,0x23,0x00,0x01,0x02,0x03,0x00}, "[1,2,3]"},
         {"array_typed_u16", {0xb6,0x01,0x00,0x02,0x00,0x03,0x00,0x03,0x00}, "[1,2,3]"},
         {"object_duplicate_keys", {0xc0,0x00,0x61,0x21,0x61,0x22,0x1f,0x1f,0x00,0x01,0x02,0x01,0x02,0x00},
          "{\"a\":1,\"a\":2}"},
         {"object_order_kept", {0xc0,0x00,0x62,0x21,0x61,0x22,0x3f,0x1f,0x00,0x01,0x02,0x01,0x02,0x00},
          "{\"b\":1,\"a\":2}"},
         {"row_array", {0xc1,0x00,0x02,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x01,0x11,0xe5,0x78,0x79,
                        0x21,0x22,0x00,0x01,0x23,0x24,0x00,0x01,0x00,0x04,0x02,0x00},
          "[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4}]"},
         // The frame on which the C++ view recursed without end: an object
         // holding a typed array.
         {"typed_array_in_object", {0xc0,0x00,0x76,0x61,0x6c,0x75,0x65,0x73,0xb7,0x01,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
                                    0x00,0x01,0x00,0x03,0x00,0xaf,0x00,0x06,0x01,0x00},
          "{\"values\":[1,256,65536]}"},
         // Valid, not canonical: each has one canonical frame.
         {"binary64_tenth", {0x63,0x9a,0x99,0x99,0x99,0x99,0x99,0xb9,0x3f}, "0.1"},
         {"binary64_one", {0x63,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x3f}, "1"},
         {"binary64_sci_hint", {0x6b,0x00,0x00,0x00,0x00,0x00,0x00,0xf8,0x3f}, "1.5"},
         {"binary32_third", {0x62,0xab,0xaa,0xaa,0x3e}, "0.33333334"},
         {"binary32_tenth", {0x62,0xcd,0xcc,0xcc,0x3d}, "0.1"},
         {"binary64_of_binary32_tenth", {0x63,0x00,0x00,0x00,0xa0,0x99,0x99,0xb9,0x3f}, "0.10000000149011612"},
         {"binary64_third", {0x63,0x55,0x55,0x55,0x55,0x55,0x55,0xd5,0x3f}, "0.3333333333333333"},
         {"uint_wider_than_needed", {0x41,0x05,0x00}, "5"},
         {"decimal_integer", {0x70,0x0a,0x00}, "5"},
         {"decimal_33333334", {0x73,0xac,0x40,0xf9,0x03,0x0f}, "0.33333334"},
         {"typed_i64_small", {0xb4,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00}, "[1]"},
         {"typed_i8_negative", {0xb1,0xff,0x02,0x02,0x00}, "[-1,2]"},
         {"typed_f64_tenths", {0xba,0x9a,0x99,0x99,0x99,0x99,0x99,0xb9,0x3f,0x9a,0x99,0x99,0x99,0x99,0x99,0xc9,0x3f,0x02,0x00},
          "[0.1,0.2]"},
         {"typed_f32_tenths", {0xb9,0xcd,0xcc,0xcc,0x3d,0xcd,0xcc,0x4c,0x3e,0x02,0x00}, "[0.1,0.2]"},
         {"typed_f64_integers", {0xba,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x02,0x00},
          "[1,2]"},
         {"typed_empty", {0xb5,0x00,0x00}, "[]"},
         {"array_mixed_numbers", {0xb0,0x00,0x21,0x70,0x0a,0x01,0x00,0x01,0x02,0x00}, "[1,0.5]"},
         {"array_number_and_string", {0xb0,0x00,0x21,0x90,0x61,0x00,0x01,0x02,0x00}, "[1,\"a\"]"},
         {"array_of_same_shape_objects",
          {0xb0,0x00,0xc0,0x00,0x78,0x21,0x79,0x22,0x11,0xe5,0x00,0x01,0x02,0x01,0x02,0x00,
           0xc0,0x00,0x78,0x23,0x79,0x24,0x11,0xe5,0x00,0x01,0x02,0x01,0x02,0x00,0x00,0x0e,0x02,0x00},
          "[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4}]"},
         {"array_of_one_object", {0xb0,0x00,0xc0,0x00,0x78,0x21,0x11,0x00,0x01,0x01,0x00,0x00,0x01,0x00}, "[{\"x\":1}]"},
         {"array_of_different_objects",
          {0xb0,0x00,0xc0,0x00,0x78,0x21,0x11,0x00,0x01,0x01,0x00,0xc0,0x00,0x79,0x22,0xe5,0x00,0x01,0x01,0x00,0x00,0x09,0x02,0x00},
          "[{\"x\":1},{\"y\":2}]"},
         {"string_escape_form_plain", {0x91,0x68,0x69}, "\"hi\""},
         {"string_escape_form_pair", {0x91,0x5c,0x75,0x64,0x38,0x33,0x64,0x5c,0x75,0x64,0x65,0x30,0x30,0x5c,0x6e},
          "\"\\ud83d\\ude00\\n\""},
         {"nested_generic", {0xc0,0x00,0x61,0xb0,0x00,0xb0,0x00,0x21,0x22,0x00,0x01,0x02,0x00,0xb0,0x00,0x00,0x00,0x00,0x08,0x02,0x00,
                             0x1f,0x00,0x01,0x01,0x00},
          "{\"a\":[[1,2],[]]}"}
      }) {
         const std::span<const std::uint8_t> view{bytes.data(), bytes.size()};
         // (The C++ dynamic value holds 127 bits of magnitude: the two
         // 128-bit integers are valid and have no pjson_value.)
         const bool wide = id == "u128_max" || id == "negint_128_bits";
         if (!psio::pjson_lazy::validate(view) || psio::pjson_lazy::try_decode(view).has_value() == wide)
            throw std::runtime_error("Refused valid pjson: " + id);
         const std::string text = psio::pjson_to_json(view);
         if (text != json)
            throw std::runtime_error("pjson_to_json of " + id + " is " + text + ", not " + json);
         const auto canonical = psio::pjson::canonicalize(view);
         // (A frame the canonical pass leaves alone is canonical; one it
         // changes may be too — a typed array wider than its values is the
         // one encoding of that list OF ITS TYPE.)
         if (!psio::pjson::validate(canonical) || psio::pjson::canonicalize(canonical) != canonical ||
             (canonical == bytes && !psio::pjson::validate(view)))
            throw std::runtime_error("the canonical form of " + id + " is not canonical");
         const bool is_canonical = psio::pjson::validate(view);
         emit_json("pjson_to_json", id, bytes, json, &canonical, &is_canonical);
      }
      std::cout << "\n]\n";
   } catch (const std::exception& e) {
      std::cerr << e.what() << '\n';
      return 1;
   }
}
