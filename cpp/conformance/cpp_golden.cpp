// The public C++ codecs define the cross-language wire contract.
#include <psio/conformance.hpp>
#include <psio/bson.hpp>
#include <psio/flatbuf.hpp>
#include <psio/msgpack.hpp>
#include <psio/pjson.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/protobuf.hpp>

#include <iostream>
#include <bit>
#include <limits>
#include <stdexcept>
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
      records("frac16", psio::frac16{});
      records("bson", psio::bson{});
      records("msgpack", psio::msgpack{});
      records("protobuf", psio::protobuf{});
      records("flatbuf", psio::flatbuf{});
      fixture("frac32", "u32", psio::frac32{}, std::uint32_t{0xdeadbeef});
      fixture("frac32", "string", psio::frac32{}, std::string{"hello"});
      fixture("frac32", "vector", psio::frac32{}, std::vector<std::uint32_t>{1, 2, 3});
      fixture("frac32", "optional_some", psio::frac32{}, std::optional<std::uint32_t>{42});
      fixture("frac32", "optional_none", psio::frac32{}, std::optional<std::uint32_t>{});
      fixture("frac32", "nested", psio::frac32{}, InteropEnvelope{
         {42,"Alice",{1,256,65536},true}, {{0,"",{},false},{7,"Bob",{9},true}},
         "hello", std::string{"chosen"}});
      fixture("frac32", "nested_empty", psio::frac32{}, InteropEnvelope{
         {0,"",{},false}, {}, std::nullopt, std::uint32_t{7}});
      fixture("frac32", "string_vector", psio::frac32{}, std::vector<std::string>{"", "hello", "world"});
      fixture("frac32", "optional_vector", psio::frac32{}, std::vector<std::optional<std::string>>{std::nullopt, "", "hello"});
      fixture("frac32", "optional_empty_string", psio::frac32{}, std::optional<std::string>{""});
      fixture("frac32", "optional_empty_vector", psio::frac32{}, std::optional<std::vector<std::uint32_t>>{{}});
      fixture("frac32", "optional_missing_vector", psio::frac32{}, std::optional<std::vector<std::uint32_t>>{});
      // Vector slots point to complete values. The legacy embedded-container
      // sentinels (0/1) and offsets into the fixed section are not valid here.
      for (unsigned offset = 0; offset < 4; ++offset) {
         std::vector<char> bytes{4,0,0,0,static_cast<char>(offset),0,0,0};
         if (psio::validate<std::vector<std::string>>(psio::frac32{}, std::span<const char>{bytes}).ok() ||
             psio::validate<std::vector<std::optional<std::string>>>(psio::frac32{}, std::span<const char>{bytes}).ok())
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
      for (auto value : numbers) {
         auto bits = std::bit_cast<std::uint64_t>(value);
         emit("pjson_f64", std::to_string(bits), psio::pjson::encode(psio::pjson_value{value}));
      }
      // A deterministic sample exercises shortest-decimal choices across the exponent range.
      std::uint64_t bits = 0x123456789abcdef0ULL;
      for (unsigned i = 0; i < 256; ++i) {
         bits ^= bits << 13; bits ^= bits >> 7; bits ^= bits << 17;
         emit("pjson_f64", std::to_string(bits),
              psio::pjson::encode(psio::pjson_value{std::bit_cast<double>(bits)}));
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
         {"row_reserved_width", {0xc1,0xf0,1,0,0,0,0,0,0,0}}
      }) {
         if (psio::pjson::validate(bytes) || psio::pjson::try_decode(bytes))
            throw std::runtime_error("Accepted invalid pjson: " + id);
         emit("pjson_invalid", id, bytes);
      }
      std::cout << "\n]\n";
   } catch (const std::exception& e) {
      std::cerr << e.what() << '\n';
      return 1;
   }
}
