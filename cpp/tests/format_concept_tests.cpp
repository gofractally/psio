// One generic function, every format (gofractally/psio#18).
//
// `exercise<F, T>` is written once against psio/format_concept.hpp and is
// instantiated for each format through an alias: the fast and the validated
// write, the size, the validating and the trusted read, validation, the
// sink, a buffer, a transcode through pssz, and — where the format declares
// the capability — views and the trusted read of bytes that are not a
// frame. A format that could not be switched to by changing the alias would
// not compile here.
//
// What a format does not have is not skipped silently: `expected<F>` states
// each format's capabilities, and they are held against the concepts. The
// table is today's library; a row changes when the format does.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/bson.hpp>
#include <psio/buffer.hpp>
#include <psio/capnp.hpp>
#include <psio/capnp_view.hpp>
#include <psio/flatbuf.hpp>
#include <psio/flatbuf_view.hpp>
#include <psio/format_concept.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/msgpack.hpp>
#include <psio/mutable_view.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>
#include <psio/transcode.hpp>
#include <psio/wit.hpp>
#include <psio/wit_view.hpp>

#include <catch.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
   // The record of conformance/cpp-golden.json.
   struct FcRecord
   {
      std::uint32_t              id = 0;
      std::string                name;
      std::vector<std::uint32_t> values;
      bool                       active = false;
      friend bool operator==(const FcRecord&, const FcRecord&) = default;
      PSIO_REFLECT(FcRecord, id, name, values, active)
   };

   // ── What each format has, stated ─────────────────────────────────────────
   struct capabilities
   {
      bool text;                  // the encoding is a std::string
      bool sink;                  // encode(F{}, v, sink) exists
      bool appends;               // ... and keeps what the sink held
      bool checked_natively;      // the validated write is the encoder's own
      bool frame_checked_read;    // the trusted read is not for validated bytes only
      bool view;                  // psio::view<T, F>
      bool trusted_view;          // ... with the trusted store
      bool mutable_view;          // psio::mutable_view<T, F>
      bool dynamic;               // encode_dynamic / decode_dynamic by schema
      bool view_unpack;           // view.unpack() gives the T back
      bool size_exact;            // size_of is the size of the encoding, for every value here
      friend bool operator==(const capabilities&, const capabilities&) = default;
   };

   template <typename F>
   inline constexpr capabilities expected{};
   //                                                              text   sink   appnd  chkNat frame  view   trust  mut    dyn    unpack size
   template <> inline constexpr capabilities expected<psio::pssz>    {false, true,  true,  true,  true,  true,  true,  true,  true, true, true};
   template <> inline constexpr capabilities expected<psio::key>     {false, true,  true,  true,  true,  false, false, false, false, false, true};
   template <> inline constexpr capabilities expected<psio::json>    {true,  true,  true,  true,  true,  false, false, false, true, false, true};
   template <> inline constexpr capabilities expected<psio::ijson>   {true,  true,  true,  true,  true,  false, false, false, true, false, true};
   template <> inline constexpr capabilities expected<psio::ssz>     {false, true,  true,  false, false, true,  true,  false, true, false, true};
   template <> inline constexpr capabilities expected<psio::bin>     {false, true,  true,  false, false, false, false, false, true, false, true};
   template <> inline constexpr capabilities expected<psio::borsh>   {false, true,  true,  false, false, false, false, false, false, false, true};
   template <> inline constexpr capabilities expected<psio::bincode> {false, true,  true,  false, false, false, false, false, false, false, true};
   template <> inline constexpr capabilities expected<psio::avro>    {false, true,  true,  false, false, false, false, false, false, false, true};
   template <> inline constexpr capabilities expected<psio::msgpack> {false, true,  true,  false, true,  false, false, false, false, false, true};
   template <> inline constexpr capabilities expected<psio::bson>    {false, true,  true,  false, false, false, false, false, false, false, true};
   // Gaps against the concept (psio#18, B7): flatbuf and capnp REPLACE the
   // sink; wit has no sink overload; the ssz and wit views have no
   // unpack() (their view traits lack it); wit's size_of is not the size of
   // its encoding (27 for a record that encodes to 24 bytes).
   template <> inline constexpr capabilities expected<psio::flatbuf> {false, true,  false, false, false, true,  true,  true,  false, true, true};
   template <> inline constexpr capabilities expected<psio::capnp>   {false, true,  false, false, false, true,  false, true,  false, true, true};
   template <> inline constexpr capabilities expected<psio::protobuf>{false, true,  true,  false, true,  false, false, false, false, false, true};
   template <> inline constexpr capabilities expected<psio::wit>     {false, false, false, false, false, true,  true,  false, false, false, false};

   template <psio::format F, typename T>
   constexpr capabilities actual()
   {
      return capabilities{
         psio::text_format<F>,
         psio::encodes_into<F, T>,
         psio::encodes_into<F, T> && psio::appends_to_sink<F>,
         psio::encodes_checked_natively<F, T>,
         psio::frame_checked_read<F>,
         psio::view_format<F>,
         psio::trusted_view_format<F>,
         psio::mutable_view_format<F>,
         psio::dynamic_format<F, psio::schema, psio::dynamic_value>,
         expected<F>.view_unpack,   // not askable without instantiating the view: held by the code below
         expected<F>.size_exact,    // a run-time property: held by the code below
      };
   }

   template <typename B>
   std::span<const char> span_of(const B& b)
   {
      return std::span<const char>{std::data(b), std::size(b)};
   }

   // ── The one generic function ─────────────────────────────────────────────
   template <psio::format F, typename T>
      requires psio::codec_for<F, T>
   void exercise(const T& value)
   {
      INFO("format " << std::string_view{F::name});
      static_assert(actual<F, T>() == expected<F>, "a format's capabilities changed: update the table");
      static_assert(psio::encodes_into<F, T> == (F::sink_write != psio::sink_write_kind::none));

      // Fast write; the fresh buffer is the format's buffer_type.
      const auto fresh = psio::encode(F{}, value);
      static_assert(std::same_as<std::remove_cvref_t<decltype(fresh)>, typename F::buffer_type>);
      const auto bytes = span_of(fresh);
      if (expected<F>.size_exact)
         CHECK(static_cast<std::size_t>(psio::size_of(F{}, value)) == bytes.size());
      else
         CHECK(static_cast<std::size_t>(psio::size_of(F{}, value)) >= bytes.size());

      // Validate; the validating and the trusted read.
      CHECK(psio::validate<T>(F{}, bytes).ok());
      CHECK(psio::decode_checked<T>(F{}, bytes) == value);
      CHECK(psio::decode<T>(F{}, bytes) == value);

      // Validated write: appends, and the bytes are the fast write's.
      {
         std::vector<char> out{'x'};
         REQUIRE(psio::encode_checked(F{}, value, out).ok());
         REQUIRE(out.size() == bytes.size() + 1);
         CHECK(out.front() == 'x');
         CHECK(std::string_view{out.data() + 1, out.size() - 1} == std::string_view{bytes.data(), bytes.size()});
      }

      // The sink of the fast write: what the format declares.
      if constexpr (psio::encodes_into<F, T>)
      {
         typename F::buffer_type sink;
         sink.push_back('x');
         psio::encode(F{}, value, sink);
         if (F::sink_write == psio::sink_write_kind::appends)
         {
            REQUIRE(sink.size() == bytes.size() + 1);
            CHECK(sink.front() == 'x');
         }
         else
         {
            REQUIRE(sink.size() == bytes.size());
            CHECK(std::string_view{sink.data(), sink.size()} == std::string_view{bytes.data(), bytes.size()});
         }
      }

      // A buffer of the type in the format.
      {
         const psio::buffer<T, F, psio::storage::owning> buffer{value};
         CHECK(std::string_view{buffer.data(), buffer.size()} == std::string_view{bytes.data(), bytes.size()});
         CHECK(buffer.unpack() == value);
         if constexpr (expected<F>.view_unpack)
            CHECK(buffer.view().unpack() == value);
      }

      // Through another format and back.
      {
         const auto as_pssz = psio::transcode<T>(F{}, bytes, psio::pssz{});
         const auto back    = psio::transcode<T>(psio::pssz{}, span_of(as_pssz), F{});
         CHECK(std::string_view{std::data(back), std::size(back)} == std::string_view{bytes.data(), bytes.size()});
      }

      // Views.
      if constexpr (psio::view_format<F>)
      {
         const psio::view<T, F, psio::storage::const_borrow> view{bytes};
         if constexpr (expected<F>.view_unpack)
            CHECK(view.unpack() == value);
         else
            (void)view;
      }
      if constexpr (psio::trusted_view_format<F>)
      {
         const psio::view<T, F, psio::storage::trusted_borrow> view{psio::trusted{bytes}};
         if constexpr (expected<F>.view_unpack)
            CHECK(view.unpack() == value);
         else
            (void)view;
      }

      // Bytes that are not a frame: the validator refuses them, and a
      // trusted read that declares it checks does not return a value. (A
      // read for validated bytes only is not handed them.)
      // (Three bytes: a shorter cut can be a frame — a protobuf message
      // ends at any field, flatbuf and wit end in padding.)
      if (bytes.size() > 8)
      {
         const auto cut = bytes.first(3);
         CHECK_FALSE(psio::validate<T>(F{}, cut).ok());
         if constexpr (psio::frame_checked_read<F>)
            CHECK_THROWS((void)psio::decode<T>(F{}, cut));
      }
   }

   const FcRecord sample{42, "Alice", {1, 256, 65536}, true};
}  // namespace

// The alias is the only thing that changes.
#define FORMAT_CASE(NAME)                                                         \
   TEST_CASE("format concept: " #NAME, "[format][concept]")                       \
   {                                                                              \
      using F = psio::NAME;                                                       \
      static_assert(psio::format<F>);                                             \
      static_assert(psio::codec_for<F, FcRecord>);                                \
      exercise<F, FcRecord>(sample);                                              \
      exercise<F, FcRecord>(FcRecord{});                                          \
   }

FORMAT_CASE(pssz)
FORMAT_CASE(key)
FORMAT_CASE(json)
FORMAT_CASE(ijson)
FORMAT_CASE(ssz)
FORMAT_CASE(bin)
FORMAT_CASE(borsh)
FORMAT_CASE(bincode)
FORMAT_CASE(avro)
FORMAT_CASE(msgpack)
FORMAT_CASE(bson)
FORMAT_CASE(flatbuf)
FORMAT_CASE(capnp)
FORMAT_CASE(protobuf)
FORMAT_CASE(wit)

TEST_CASE("format concept: what is not a format, and what a format does not take", "[format][concept]")
{
   static_assert(!psio::format<int>);
   static_assert(!psio::format<std::string>);
   // Asking is never a hard error.
   static_assert(!psio::codec_for<psio::flatbuf, int>);   // a FlatBuffers root is a table
   static_assert(!psio::codec_for<psio::bson, int>);      // a BSON document is a record
   static_assert(psio::codec_for<psio::pssz, int>);
   static_assert(psio::codec_for<psio::json, std::vector<std::string>>);
   SUCCEED();
}
