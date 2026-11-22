// String types in pssz, and how they pair with Rust:
//
//   std::string            raw bytes               (Rust RawString)
//   psio::utf8_string<>    unbounded UTF-8         (Rust String)
//   psio::utf8_string<N>   UTF-8, at most N bytes
//
// The user type carries the restriction. A std::string is never checked for
// UTF-8 by encode, decode, default validation or views (strict validation
// checks every string). A utf8_string is checked by encode, decode, validate
// and validate_strict, wherever it is nested.

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <array>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace string_fixture
{
   using Text  = psio::utf8_string<>;   // unbounded UTF-8
   using Text4 = psio::utf8_string<4>;  // UTF-8, at most 4 bytes

   struct RawRec
   {
      std::uint8_t                       k;
      std::string                        s;
      std::vector<std::string>           v;
      std::optional<std::string>         o;
      friend bool operator==(const RawRec&, const RawRec&) = default;
   };
   PSIO_REFLECT(RawRec, k, s, v, o)

   template <typename S>
   struct Holder
   {
      std::uint8_t                       k;
      S                                  s;
      std::vector<S>                     v;
      std::array<S, 1>                   a;
      std::variant<std::uint8_t, S>      alt;
      std::tuple<S>                      t;
      std::optional<S>                   o;
      friend bool operator==(const Holder&, const Holder&) = default;
   };
   using TextRec   = Holder<Text>;
   using Text4Rec  = Holder<Text4>;
   using RawHolder = Holder<std::string>;
   PSIO_REFLECT(TextRec, k, s, v, a, alt, t, o)
   PSIO_REFLECT(Text4Rec, k, s, v, a, alt, t, o)
   PSIO_REFLECT(RawHolder, k, s, v, a, alt, t, o)

   // Only bounded members: the automatic width is inferred, not defaulted.
   struct OnlyText4
   {
      Text4       s;
      friend bool operator==(const OnlyText4&, const OnlyText4&) = default;
   };
   PSIO_REFLECT(OnlyText4, s)
}  // namespace string_fixture

namespace
{
   using namespace string_fixture;

   const std::string kOverlong{"\xc0\x80"};  // overlong NUL: not UTF-8
   const std::string kLoneFF{"\xff"};
   const std::string kMulti{"h\xc3\xa9"};     // "hé", 3 bytes

   // ── Compile-time facts the Rust side mirrors ──────────────────────────
   static_assert(!Text::is_bounded && Text4::is_bounded);
   static_assert(Text::max_size_value == psio::utf8_unbounded && Text4::max_size_value == 4);
   // An unbounded UTF-8 string is unbounded exactly like std::string.
   static_assert(!psio::max_encoded_size<std::string>().has_value());
   static_assert(!psio::max_encoded_size<Text>().has_value());
   static_assert(psio::auto_pssz_width_v<std::string> == 4 && psio::auto_pssz_width_v<Text> == 4);
   static_assert(!psio::max_encoded_size<TextRec>().has_value() &&
                 psio::auto_pssz_width_v<TextRec> == 4);
   // The bounded wrapper does not feed bound inference today (only a
   // length_bound member annotation on std::string / std::vector does), so a
   // record of bounded UTF-8 strings still selects four-byte offsets.
   static_assert(!psio::max_encoded_size<Text4>().has_value());
   static_assert(!psio::max_encoded_size<OnlyText4>().has_value() &&
                 psio::auto_pssz_width_v<OnlyText4> == 4);

   // Inherent annotations: UTF-8 always; a length bound only when bounded.
   static_assert(std::tuple_size_v<decltype(psio::inherent_annotations<Text>::value)> == 1);
   static_assert(std::get<psio::utf8_spec>(psio::inherent_annotations<Text>::value).max == 0);
   static_assert(std::tuple_size_v<decltype(psio::inherent_annotations<Text4>::value)> == 2);
   static_assert(std::get<psio::length_bound>(psio::inherent_annotations<Text4>::value).max == 4u);
   static_assert(std::is_same_v<psio::shape_tag_of<Text>, psio::ByteStringShape> &&
                 std::is_same_v<psio::shape_tag_of<Text4>, psio::ByteStringShape>);

   static_assert(psio::detail::pssz_impl::optional_uses_selector_v<Text> &&
                 psio::detail::pssz_impl::optional_uses_selector_v<Text4> &&
                 psio::detail::pssz_impl::optional_uses_selector_v<std::string>);

   std::vector<char> raw(std::string_view text) { return {text.begin(), text.end()}; }

   template <typename F, typename T>
   void accepted(const T& value, const std::vector<char>& wire)
   {
      REQUIRE(psio::encode(F{}, value) == wire);
      REQUIRE(psio::validate<T>(F{}, wire).ok());
      REQUIRE_NOTHROW(psio::validate_or_throw<T>(F{}, wire));
      REQUIRE(psio::validate_strict<T>(F{}, wire).ok());
      REQUIRE(psio::decode<T>(F{}, wire) == value);
      bool unknown = true;
      T    out{};
      REQUIRE(psio::decode_pssz_checked(F{}, wire, out, unknown).ok());
      REQUIRE_FALSE(unknown);
      REQUIRE(out == value);
   }

   template <typename F, typename T>
   void rejected(const std::vector<char>& wire)
   {
      REQUIRE_FALSE(psio::validate<T>(F{}, wire).ok());
      REQUIRE_FALSE(psio::validate_strict<T>(F{}, wire).ok());
      REQUIRE_THROWS_AS(psio::validate_or_throw<T>(F{}, wire), psio::codec_exception);
      REQUIRE_THROWS_AS(psio::decode<T>(F{}, wire), psio::codec_exception);
      bool unknown = false;
      REQUIRE_FALSE(psio::validate_pssz_compatible<T>(F{}, wire, unknown).ok());
      T out{};
      REQUIRE_FALSE(psio::decode_pssz_checked(F{}, wire, out, unknown).ok());
   }

   // The same logical value with every string replaced by `text`, as a raw
   // record frame: built by the raw-string codec, which never checks UTF-8.
   template <typename F, typename S>
   std::vector<char> holder_frame(std::size_t slot, const std::string& text)
   {
      // Mirror Holder<S> with std::string members: identical layout.
      Holder<std::string> value{1, "ok", {"ok"}, {{"ok"}}, std::string("ok"),
                                std::tuple<std::string>{"ok"}, std::string("ok")};
      switch (slot)
      {
         case 0: value.s = text; break;
         case 1: value.v[0] = text; break;
         case 2: value.a[0] = text; break;
         case 3: value.alt = text; break;
         case 4: std::get<0>(value.t) = text; break;
         case 5: value.o = text; break;
      }
      return psio::encode(F{}, value);
   }

   template <typename S>
   Holder<S> good_holder()
   {
      return {1, S{"ok"}, {S{"ok"}}, {{S{"ok"}}}, S{"ok"}, std::tuple<S>{S{"ok"}}, S{"ok"}};
   }
   template <typename S>
   void poison(Holder<S>& value, std::size_t slot, const std::string& text)
   {
      switch (slot)
      {
         case 0: value.s = S{text}; break;
         case 1: value.v[0] = S{text}; break;
         case 2: value.a[0] = S{text}; break;
         case 3: value.alt = S{text}; break;
         case 4: std::get<0>(value.t) = S{text}; break;
         case 5: value.o = S{text}; break;
      }
   }
}  // namespace

#define STRING_FORMATS psio::pssz8, psio::pssz16, psio::pssz32, psio::pssz

TEMPLATE_TEST_CASE("pssz std::string is raw bytes", "[pssz][string]", STRING_FORMATS)
{
   using F = TestType;
   for (const auto& text : {std::string{}, std::string{"hi"}, kMulti})
      accepted<F, std::string>(text, raw(text));

   // Not UTF-8: encode, decode, default validation and views pass it through.
   for (const auto& text : {kOverlong, kLoneFF})
   {
      const auto wire = raw(text);
      REQUIRE(psio::encode(F{}, text) == wire);
      REQUIRE(psio::validate<std::string>(F{}, wire).ok());
      REQUIRE_NOTHROW(psio::validate_or_throw<std::string>(F{}, wire));
      REQUIRE(psio::decode<std::string>(F{}, wire) == text);
      bool        unknown = true;
      std::string out;
      REQUIRE(psio::decode_pssz_checked(F{}, wire, out, unknown).ok());
      REQUIRE(out == text);
      REQUIRE(psio::view<std::string, F, psio::storage::const_borrow>{wire}.view_() == text);
      // Strict validation checks every string.
      REQUIRE_FALSE(psio::validate_strict<std::string>(F{}, wire).ok());

      // Nested: record field, vector element, optional payload.
      const RawRec nested{1, text, {text, "ok"}, text};
      const auto   bytes = psio::encode(F{}, nested);
      REQUIRE(psio::validate<RawRec>(F{}, bytes).ok());
      REQUIRE(psio::decode<RawRec>(F{}, bytes) == nested);
      REQUIRE_FALSE(psio::validate_strict<RawRec>(F{}, bytes).ok());
      auto view = psio::view<RawRec, F, psio::storage::const_borrow>{bytes};
      REQUIRE(view.template get<1>().view_() == text);
      REQUIRE(view.template get<2>()[0].view_() == text);
      REQUIRE(view.template get<3>().get().view_() == text);
   }
}

namespace
{
   template <typename F, typename S>
   void utf8_paths()
   {
      for (const auto& text : {std::string{}, std::string{"hi"}, kMulti})
         accepted<F, S>(S{text}, raw(text));
      accepted<F, std::optional<S>>(std::nullopt, {0});
      accepted<F, std::optional<S>>(S{}, {1});
      accepted<F, std::optional<S>>(S{"hi"}, {1, 'h', 'i'});
      accepted<F, std::vector<S>>({}, {});
      accepted<F, Holder<S>>(good_holder<S>(), holder_frame<F, S>(99, ""));

      for (const auto& text : {kOverlong, kLoneFF})
      {
         // The wrapper constructor does not check UTF-8; the encoder does.
         const S value{text};
         REQUIRE_THROWS_AS(psio::encode(F{}, value), psio::codec_exception);
         REQUIRE_THROWS_AS(psio::encode(F{}, std::optional<S>{value}), psio::codec_exception);
         rejected<F, S>(raw(text));

         std::vector<char> some{1};
         some.insert(some.end(), text.begin(), text.end());
         rejected<F, std::optional<S>>(some);

         // Every nesting position: field, vector element, array element,
         // variant alternative, tuple member, optional payload.
         for (std::size_t slot = 0; slot < 6; ++slot)
         {
            INFO("slot " << slot);
            auto bad = good_holder<S>();
            poison(bad, slot, text);
            REQUIRE_THROWS_AS(psio::encode(F{}, bad), psio::codec_exception);
            const auto frame = holder_frame<F, S>(slot, text);
            rejected<F, Holder<S>>(frame);
            // The same bytes are a valid record of raw strings.
            REQUIRE(psio::validate<RawHolder>(F{}, frame).ok());
         }
      }
      const auto wire = psio::encode(F{}, good_holder<S>());
      auto       view = psio::view<Holder<S>, F, psio::storage::const_borrow>{wire};
      REQUIRE(view.template get<1>().view_() == "ok");
      REQUIRE(view.template get<2>()[0].view_() == "ok");
      REQUIRE(view.template get<6>().get().view_() == "ok");
      const auto multi = raw(kMulti);
      REQUIRE(psio::view<S, F, psio::storage::const_borrow>{multi}.view_() == kMulti);
   }
}  // namespace

TEMPLATE_TEST_CASE("pssz unbounded utf8_string enforces UTF-8 in every path", "[pssz][string]",
                   STRING_FORMATS)
{
   utf8_paths<TestType, Text>();
}

TEMPLATE_TEST_CASE("pssz bounded utf8_string enforces UTF-8 in every path", "[pssz][string]",
                   STRING_FORMATS)
{
   utf8_paths<TestType, Text4>();
}

TEMPLATE_TEST_CASE("pssz utf8_string<N> enforces its byte bound", "[pssz][string]", STRING_FORMATS)
{
   using F = TestType;
   // Exactly N bytes (counted in bytes, not code points) is accepted.
   accepted<F, Text4>(Text4{"abcd"}, raw("abcd"));
   accepted<F, Text4>(Text4{"\xc3\xa9\xc3\xa9"}, raw("\xc3\xa9\xc3\xa9"));
   // N + 1 bytes cannot be constructed, and is rejected from the wire.
   REQUIRE_THROWS(Text4{"abcde"});
   rejected<F, Text4>(raw("abcde"));
   rejected<F, std::optional<Text4>>(raw("\x01" "abcde"));
   for (std::size_t slot = 0; slot < 6; ++slot)
   {
      INFO("slot " << slot);
      REQUIRE(psio::validate<Text4Rec>(F{}, holder_frame<F, Text4>(slot, "abcd")).ok());
      rejected<F, Text4Rec>(holder_frame<F, Text4>(slot, "abcde"));
   }
   // The unbounded form has no limit.
   const std::string big(100000, 'a');
   if constexpr (std::is_same_v<F, psio::pssz> || std::is_same_v<F, psio::pssz32>)
      accepted<F, Text>(Text{big}, raw(big));
   accepted<F, Text>(Text{"abcde"}, raw("abcde"));
}

TEST_CASE("SchemaBuilder: string types", "[pssz][string][schema]")
{
   namespace ir = psio::schema_types;
   psio::SchemaBuilder builder;
   auto plain     = builder.insert<std::string>();
   auto unbounded = builder.insert<Text>();
   auto bounded   = builder.insert<Text4>();
   auto schema    = std::move(builder).build();
   // All three use the "string" adapter. The unbounded UTF-8 string is over
   // an unbounded byte list (no fabricated maximum); the bounded one over a
   // BoundedList. Note the schema does not distinguish a raw std::string from
   // an unbounded UTF-8 string.
   const auto& p = std::get<ir::Custom>(plain.resolve(schema)->value);
   const auto& u = std::get<ir::Custom>(unbounded.resolve(schema)->value);
   const auto& b = std::get<ir::Custom>(bounded.resolve(schema)->value);
   REQUIRE(p.id == "string");
   REQUIRE(u.id == "string");
   REQUIRE(b.id == "string");
   REQUIRE(std::holds_alternative<ir::List>(p.type->resolve(schema)->value));
   REQUIRE(std::holds_alternative<ir::List>(u.type->resolve(schema)->value));
   REQUIRE(std::get<ir::BoundedList>(b.type->resolve(schema)->value).maxCount == 4);
}
