// The standard integer types have the same names, labels, schemas and bytes
// on every platform, whichever of them the fixed-width typedefs are.
//
// std::int64_t is `long` on LP64 Linux and `long long` on macOS;
// std::uint64_t likewise; std::size_t is `unsigned long` on both, so it IS
// std::uint64_t on Linux and a different type of the same width on macOS
// (and std::ptrdiff_t / std::intptr_t with the signed one). Code keyed on
// the typedefs alone therefore treats one of `long` / `long long` as a
// stranger, and which one depends on the platform.
//
// Each case below is stated for every one of these types, so the type that
// is the stranger HERE is covered on this platform and the other one on the
// other: nothing in this file depends on which platform it is compiled on,
// and a difference between them fails on one of the two.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/bson.hpp>
#include <psio/capnp.hpp>
#include <psio/emit_capnp.hpp>
#include <psio/emit_fbs.hpp>
#include <psio/emit_protobuf.hpp>
#include <psio/emit_pssz.hpp>
#include <psio/emit_wit.hpp>
#include <psio/flatbuf.hpp>
#include <psio/frac.hpp>
#include <psio/get_type_name.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/msgpack.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>
#include <psio/schema_builder.hpp>
#include <psio/ssz.hpp>
#include <psio/variant_label.hpp>

#include <catch.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace
{
   // ── The premises: every one of these is a 64-bit type here ────────────
   static_assert(sizeof(long long) == 8 && sizeof(unsigned long long) == 8);
   static_assert(sizeof(std::int64_t) == 8 && sizeof(std::uint64_t) == 8);
   // (`long` is 32 bits on LLP64; the names below follow its width)
   constexpr bool long_is_64 = sizeof(long) == 8;

   template <typename T>
   constexpr std::string_view name_of = psio::get_type_name(static_cast<const T*>(nullptr));

   // ── Names ─────────────────────────────────────────────────────────────
   static_assert(name_of<std::int64_t> == "int64");
   static_assert(name_of<std::uint64_t> == "uint64");
   static_assert(name_of<long long> == "int64");
   static_assert(name_of<unsigned long long> == "uint64");
   static_assert(name_of<long> == (long_is_64 ? "int64" : "int32"));
   static_assert(name_of<unsigned long> == (long_is_64 ? "uint64" : "uint32"));
   static_assert(name_of<int> == "int32");
   static_assert(name_of<unsigned> == "uint32");
   static_assert(name_of<short> == "int16");
   static_assert(name_of<unsigned short> == "uint16");
   static_assert(name_of<signed char> == "int8");
   static_assert(name_of<unsigned char> == "uint8");
   static_assert(name_of<std::size_t> == (sizeof(std::size_t) == 8 ? "uint64" : "uint32"));
   static_assert(name_of<std::ptrdiff_t> == (sizeof(std::ptrdiff_t) == 8 ? "int64" : "int32"));
   static_assert(name_of<std::intptr_t> == (sizeof(std::intptr_t) == 8 ? "int64" : "int32"));
   static_assert(name_of<std::uintptr_t> == (sizeof(std::uintptr_t) == 8 ? "uint64" : "uint32"));
   // (and inside the names built from them)
   static_assert(name_of<std::vector<long long>> == "int64[]");
   static_assert(name_of<std::optional<unsigned long long>> == "uint64?");
   static_assert(name_of<std::vector<long>> == (long_is_64 ? "int64[]" : "int32[]"));
   static_assert(name_of<std::tuple<long long, unsigned long long>> == "tuple_int64_uint64");

   // ── Variant alternative labels ────────────────────────────────────────
   template <typename I>
   using labelled = std::variant<I, std::string>;
   static_assert(psio::variant_label<labelled<std::int64_t>, 0>() == "int64");
   static_assert(psio::variant_label<labelled<long long>, 0>() == "int64");
   static_assert(psio::variant_label<labelled<unsigned long long>, 0>() == "uint64");
   static_assert(psio::variant_label<labelled<long>, 0>() == (long_is_64 ? "int64" : "int32"));
   static_assert(psio::variant_label<labelled<unsigned long>, 0>() ==
                 (long_is_64 ? "uint64" : "uint32"));
   static_assert(psio::variant_label<labelled<std::size_t>, 0>() == name_of<std::size_t>);
   static_assert(psio::variant_label<labelled<std::ptrdiff_t>, 0>() == name_of<std::ptrdiff_t>);
   static_assert(psio::variant_label<labelled<std::intptr_t>, 0>() == name_of<std::intptr_t>);
   // Two alternatives of one name are labelled by position, on every
   // platform: `long long` and std::int64_t are both int64 whether or not
   // they are one type. (Where they are one type the variant is ill-formed
   // to index by type, not to label.)
   static_assert(psio::variant_label<std::variant<long long, std::string, unsigned long long>, 0>() ==
                 "int64");

   // ── The same record over each spelling of the 64-bit integers ─────────
#define PSIO_TEST_INTEGER_RECORD                                                              \
   struct Row                                                                                 \
   {                                                                                          \
      I                            a = -5;                                                    \
      U                            b = 7;                                                     \
      std::vector<I>               c{1, -2, 3};                                               \
      std::optional<U>             d = 9;                                                     \
      std::array<I, 2>             e{{-1, 1}};                                                \
      std::variant<I, std::string> f = I{-11};                                                \
      std::variant<std::string, U> g = U{12};                                                 \
      std::map<U, I>               h{{1, -1}, {2, -2}};                                       \
      std::tuple<I, U>             i{-3, 3};                                                  \
   };                                                                                         \
   PSIO_REFLECT(Row, a, b, c, d, e, f, g, h, i)                                               \
   struct Flat                                                                                \
   {                                                                                          \
      I              a = -5;                                                                  \
      U              b = 7;                                                                   \
      std::vector<I> c{1, -2, 3};                                                             \
      std::string    s = "x";                                                                 \
   };                                                                                         \
   PSIO_REFLECT(Flat, a, b, c, s)

   namespace fixed
   {
      using I = std::int64_t;
      using U = std::uint64_t;
      PSIO_TEST_INTEGER_RECORD
   }  // namespace fixed
   namespace ll
   {
      using I = long long;
      using U = unsigned long long;
      PSIO_TEST_INTEGER_RECORD
   }  // namespace ll
   namespace sys
   {
      using I = std::ptrdiff_t;
      using U = std::size_t;
      PSIO_TEST_INTEGER_RECORD
   }  // namespace sys
   namespace ptr
   {
      using I = std::intptr_t;
      using U = std::uintptr_t;
      PSIO_TEST_INTEGER_RECORD
   }  // namespace ptr
   namespace lng
   {
      using I = long;
      using U = unsigned long;
      PSIO_TEST_INTEGER_RECORD
   }  // namespace lng

   template <typename Fmt, typename T>
   std::string bytes_of()
   {
      const auto out = psio::encode(Fmt{}, T{});
      return std::string{out.data(), out.size()};
   }
   template <typename T>
   psio::Schema schema_of()
   {
      psio::SchemaBuilder b;
      b.insert<T>("Row");
      return std::move(b).build();
   }
   template <typename T>
   std::string schema_json()
   {
      return psio::encode(psio::json{}, schema_of<T>());
   }

   // Every 64-bit spelling against the fixed-width one.
   template <typename Check>
   void for_each_spelling(Check&& check)
   {
      check(std::type_identity<ll::Row>{}, std::type_identity<ll::Flat>{}, "long long");
      if (sizeof(std::size_t) == 8)
         check(std::type_identity<sys::Row>{}, std::type_identity<sys::Flat>{}, "size_t");
      if (sizeof(std::intptr_t) == 8)
         check(std::type_identity<ptr::Row>{}, std::type_identity<ptr::Flat>{}, "intptr_t");
      if (long_is_64)
         check(std::type_identity<lng::Row>{}, std::type_identity<lng::Flat>{}, "long");
   }
}  // namespace

TEST_CASE("a 64-bit integer encodes the same under every spelling", "[integer-alias]")
{
   for_each_spelling([]<typename R, typename F>(std::type_identity<R>, std::type_identity<F>,
                                                 const char* spelling) {
      INFO(spelling);
      CHECK(bytes_of<psio::pssz, R>() == bytes_of<psio::pssz, fixed::Row>());
      CHECK(bytes_of<psio::key, R>() == bytes_of<psio::key, fixed::Row>());
      CHECK(bytes_of<psio::json, R>() == bytes_of<psio::json, fixed::Row>());
      CHECK(bytes_of<psio::ijson, R>() == bytes_of<psio::ijson, fixed::Row>());
      // (the formats with a narrower type system take the flat record)
      CHECK(bytes_of<psio::experimental::frac32, F>() ==
            bytes_of<psio::experimental::frac32, fixed::Flat>());
      CHECK(bytes_of<psio::bin, F>() == bytes_of<psio::bin, fixed::Flat>());
      CHECK(bytes_of<psio::bincode, F>() == bytes_of<psio::bincode, fixed::Flat>());
      CHECK(bytes_of<psio::borsh, F>() == bytes_of<psio::borsh, fixed::Flat>());
      CHECK(bytes_of<psio::ssz, F>() == bytes_of<psio::ssz, fixed::Flat>());
      CHECK(bytes_of<psio::capnp, F>() == bytes_of<psio::capnp, fixed::Flat>());
      CHECK(bytes_of<psio::flatbuf, F>() == bytes_of<psio::flatbuf, fixed::Flat>());
      CHECK(bytes_of<psio::protobuf, F>() == bytes_of<psio::protobuf, fixed::Flat>());
      CHECK(bytes_of<psio::avro, F>() == bytes_of<psio::avro, fixed::Flat>());
      CHECK(bytes_of<psio::bson, F>() == bytes_of<psio::bson, fixed::Flat>());
      CHECK(bytes_of<psio::msgpack, F>() == bytes_of<psio::msgpack, fixed::Flat>());
   });
}

TEST_CASE("a 64-bit integer has the same schema under every spelling", "[integer-alias]")
{
   const std::string expected = schema_json<fixed::Row>();
   // The tagged form of a variant alternative is keyed by its label.
   CHECK(expected.find("\"int64\"") != std::string::npos);
   CHECK(expected.find("\"uint64\"") != std::string::npos);
   const auto fixed_flat = schema_of<fixed::Flat>();
   for_each_spelling([&]<typename R, typename F>(std::type_identity<R>, std::type_identity<F>,
                                                  const char* spelling) {
      INFO(spelling);
      CHECK(schema_json<R>() == expected);
      CHECK(psio::emit_pssz(schema_of<R>()) == psio::emit_pssz(schema_of<fixed::Row>()));
      CHECK(psio::emit_wit(schema_of<F>()) == psio::emit_wit(fixed_flat));
      CHECK(psio::emit_fbs(schema_of<F>()) == psio::emit_fbs(fixed_flat));
      CHECK(psio::emit_capnp(schema_of<F>()) == psio::emit_capnp(fixed_flat));
      CHECK(psio::emit_protobuf(schema_of<F>()) == psio::emit_protobuf(fixed_flat));
   });
}
