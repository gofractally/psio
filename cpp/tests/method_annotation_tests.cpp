// method(...) in PSIO_REFLECT: per-parameter annotations, a cap for the
// argument tuple, returns(...).
//
//   - the metadata: reflect<T>::method_annotations<I>, method_max_dynamic_data<I>,
//     method_param_names<I> (names only, whatever else the list holds);
//   - reflect<T>::method_arg_tuple<I>: the argument tuple type that carries
//     them into the codecs — its bound, its width, the same bytes as the
//     equivalent wrapper-typed tuple, rejection at bound + 1 and over the cap;
//   - evolution: a trailing optional argument under a cap keeps the width;
//     without a cap, outgrowing the inferred width is a compile-time fact;
//   - the interface schema: bounded parameter / result types, Func.attributes.

#include <psio/emit_wit.hpp>
#include <psio/json.hpp>
#include <psio/max_size.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>
#include <psio/structural.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace ma_test
{
   struct token
   {
      void        transfer(const std::string& to, std::uint64_t amount, std::string memo);
      void        transfer_uncapped(std::string to, std::uint64_t amount, std::string memo);
      std::string name_of(std::uint32_t id) const;
      void        plain(std::uint32_t a, std::string b);
      void        batch(std::vector<std::string> names, std::optional<std::uint32_t> tip);
      void        note(std::string text);
      static int  version();
      PSIO_REFLECT(token,
                   method(transfer, attr(to, utf8 | max<16>), amount, attr(memo, utf8 | max<80>),
                          maxDynamicData(256)),
                   method(transfer_uncapped, attr(to, utf8 | max<16>), amount,
                          attr(memo, utf8 | max<80>)),
                   method(name_of, id, returns(utf8 | max<32>)),
                   method(plain, a, b),
                   method(batch, attr(names, max<4> | element(max<8>)), tip),
                   method(note, text, maxDynamicData(64)),
                   method(version))
   };

   // The next revision: transfer takes a trailing optional argument.
   struct token2
   {
      void transfer(const std::string& to, std::uint64_t amount, std::string memo,
                    std::optional<std::uint32_t> fee);
      void transfer_uncapped(std::string to, std::uint64_t amount, std::string memo,
                             std::optional<std::array<std::uint64_t, 20>> big);
      PSIO_REFLECT(token2,
                   method(transfer, attr(to, utf8 | max<16>), amount, attr(memo, utf8 | max<80>),
                          fee, maxDynamicData(256)),
                   method(transfer_uncapped, attr(to, utf8 | max<16>), amount,
                          attr(memo, utf8 | max<80>), big))
   };
   PSIO_INTERFACE(token, types())
}  // namespace ma_test
using ma_test::token;
using ma_test::token2;
using R  = psio::reflect<token>;
using R2 = psio::reflect<token2>;

// ── Metadata ──────────────────────────────────────────────────────────────
static_assert(R::method_count == 7);
static_assert(R::method_param_names<0>.size() == 3 && R::method_param_names<0>[0] == "to" &&
              R::method_param_names<0>[1] == "amount" && R::method_param_names<0>[2] == "memo");
static_assert(R::method_param_names<2>.size() == 1 && R::method_param_names<2>[0] == "id");
static_assert(R::method_param_names<5>.size() == 1 && R::method_param_names<6>.size() == 0);
static_assert(R::method_max_dynamic_data<0> == 256);
static_assert(!R::method_max_dynamic_data<1>.has_value());
static_assert(R::method_max_dynamic_data<5> == 64);
// One spec tuple per listed parameter; a bare name has none.
static_assert(std::tuple_size_v<decltype(R::method_annotations<0>.params)> == 3);
static_assert(std::get<psio::length_bound>(std::get<0>(R::method_annotations<0>.params)).max == 16);
static_assert(std::tuple_size_v<std::tuple_element_t<1, decltype(R::method_annotations<0>.params)>> ==
              0);
static_assert(std::get<psio::length_bound>(std::get<2>(R::method_annotations<0>.params)).max == 80);
static_assert(std::get<psio::length_bound>(psio::method_result_annotations<token, 2>).max == 32);
static_assert(std::tuple_size_v<decltype(R::method_annotations<3>.result)> == 0);

// ── The argument tuple: bound and width ───────────────────────────────────
using Transfer         = R::method_arg_tuple<0>;
using TransferUncapped = R::method_arg_tuple<1>;
using Plain            = R::method_arg_tuple<3>;
using Batch            = R::method_arg_tuple<4>;
using Note             = R::method_arg_tuple<5>;
using Version          = R::method_arg_tuple<6>;

// It is the tuple of the decayed parameters.
static_assert(std::is_base_of_v<std::tuple<std::string, std::uint64_t, std::string>, Transfer>);
static_assert(std::tuple_size_v<Transfer> == 3);
static_assert(std::is_same_v<std::tuple_element_t<2, Transfer>, std::string>);

// An explicit cap rules: 256 selects 2-byte offsets although the fields
// need at most 1 + (1 + 16) + 8 + (1 + 80) = 107 bytes.
static_assert(psio::max_dynamic_data_v<Transfer> == 256);
// (max_encoded_size is the inferred bound, as for a capped record: the cap
// is not in it. docs/pssz-spec.md 3.2.)
static_assert(psio::max_encoded_size<Transfer>() == 1 + (1 + 16) + 8 + (1 + 80));
static_assert(psio::effective_max_dynamic_v<Transfer> == 256);
static_assert(psio::auto_pssz_width_v<Transfer> == 2);
// Without a cap: the inferred bound, rounded up to the next width.
static_assert(!psio::max_dynamic_data_v<TransferUncapped>.has_value());
static_assert(psio::max_encoded_size<TransferUncapped>() == 1 + (1 + 16) + 8 + (1 + 80));
static_assert(psio::auto_pssz_width_v<TransferUncapped> == 1);
// No annotation: exactly the plain tuple.
static_assert(!psio::max_encoded_size<Plain>().has_value());
static_assert(psio::auto_pssz_width_v<Plain> == 4);
// header 1 + slot 1 + 4 x (table slot 1 + 8) + slot 1 + (selector 1 + 4)
static_assert(psio::max_encoded_size<Batch>() == 1 + (1 + 4 * (1 + 8)) + (1 + 1 + 4));
static_assert(psio::auto_pssz_width_v<Batch> == 1);
static_assert(psio::auto_pssz_width_v<Note> == 1);
static_assert(psio::auto_pssz_width_v<Version> == 1);  // the empty tuple: its header

// ── Evolution ─────────────────────────────────────────────────────────────
using Transfer2         = R2::method_arg_tuple<0>;
using TransferUncapped2 = R2::method_arg_tuple<1>;
// Under the cap the appended optional argument keeps the width: compatible.
static_assert(psio::auto_pssz_width_v<Transfer2> == 2);
static_assert(psio::pssz_evolves_from<Transfer, Transfer2>);
// Without a cap the new argument outgrows the inferred width (116 -> 281):
// the width changes, and that is detected at compile time.
static_assert(psio::auto_pssz_width_v<TransferUncapped2> == 2);
static_assert(!psio::pssz_evolves_from<TransferUncapped, TransferUncapped2>);
// A different parameter bound that changes no width is still the same layout.
static_assert(psio::pssz_evolves_from<Plain, Plain>);

namespace
{
   template <typename T>
   std::vector<char> bytes_of(const T& value)
   {
      return psio::encode(psio::pssz{}, value);
   }
   template <typename T>
   bool encodes(const T& value)
   {
      std::vector<char> out;
      const bool        checked = psio::encode_pssz_checked(psio::pssz{}, value, out).ok();
      bool              plain   = true;
      try
      {
         (void)psio::encode(psio::pssz{}, value);
      }
      catch (const psio::codec_exception&)
      {
         plain = false;
      }
      REQUIRE(checked == plain);
      return checked;
   }
   template <typename T>
   bool reads(const std::vector<char>& bytes)
   {
      const bool valid = psio::validate<T>(psio::pssz{}, std::span<const char>(bytes)).ok();
      bool       decoded = true;
      try
      {
         (void)psio::decode<T>(psio::pssz{}, std::span<const char>(bytes));
      }
      catch (const psio::codec_exception&)
      {
         decoded = false;
      }
      T    out;
      bool unknown = false;
      CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>(bytes), out, unknown)
               .ok() == valid);
      CHECK(valid == decoded);
      return valid;
   }
   template <typename T>
   std::vector<char> unchecked(const T& value)
   {
      return psio::detail::pssz_impl::encode_ignoring_length_bounds(psio::pssz{}, value);
   }
}  // namespace

TEST_CASE("method annotations: the argument tuple encodes with its annotations",
          "[method_annotations]")
{
   // What psibase packs: the tuple of the decayed arguments.
   const std::tuple<std::string, std::uint64_t, std::string> args{"alice", 5, "thanks"};

   // The annotated tuple converts from it and is it.
   const TransferUncapped annotated{args};
   CHECK(std::get<0>(annotated) == "alice");
   CHECK(std::apply([](const std::string& to, std::uint64_t amount, const std::string& memo) {
            return to.size() + amount + memo.size();
         },
                    annotated.tuple()) == 16);

   // 1-byte offsets: header, slot, u64, slot, payloads.
   const auto bytes = bytes_of(annotated);
   CHECK(bytes.size() == 1 + 1 + 8 + 1 + 5 + 6);
   // The same bytes as the tuple of the equivalent wrapper types.
   using W = std::tuple<psio::utf8_string<16>, std::uint64_t, psio::utf8_string<80>>;
   static_assert(psio::max_encoded_size<W>() == psio::max_encoded_size<TransferUncapped>());
   CHECK(bytes == bytes_of(W{psio::utf8_string<16>{"alice"}, 5, psio::utf8_string<80>{"thanks"}}));
   // Not those of the plain tuple, whose offsets are 4 wide.
   CHECK(bytes_of(args).size() == 4 + 4 + 8 + 4 + 5 + 6);

   REQUIRE(reads<TransferUncapped>(bytes));
   const auto back = psio::decode<TransferUncapped>(psio::pssz{}, std::span<const char>(bytes));
   CHECK(back.tuple() == args);

   // The capped method: the same values, 2-byte offsets.
   const Transfer capped{args};
   CHECK(bytes_of(capped).size() == 2 + 2 + 8 + 2 + 5 + 6);
   CHECK(reads<Transfer>(bytes_of(capped)));

   // No annotation: byte for byte the plain tuple.
   const std::tuple<std::uint32_t, std::string> plain{7, "x"};
   CHECK(bytes_of(Plain{plain}) == bytes_of(plain));
   CHECK(bytes_of(Version{}) == bytes_of(std::tuple<>{}));
}

TEST_CASE("method annotations: a parameter outside its annotation is refused",
          "[method_annotations]")
{
   const std::string s16(16, 'a'), s17(17, 'a'), s80(80, 'm'), s81(81, 'm');
   // At the bounds.
   CHECK(encodes(TransferUncapped{s16, 1, s80}));
   CHECK(reads<TransferUncapped>(bytes_of(TransferUncapped{s16, 1, s80})));
   // One past, per parameter; not UTF-8.
   for (const auto& over : {TransferUncapped{s17, 1, ""}, TransferUncapped{"", 1, s81},
                            TransferUncapped{"\xff", 1, ""}})
   {
      CHECK_FALSE(encodes(over));
      CHECK_FALSE(reads<TransferUncapped>(unchecked(over)));
   }
   // A nested bound: <= 4 names of <= 8 bytes.
   CHECK(encodes(Batch{std::vector<std::string>(4, "12345678"), std::nullopt}));
   for (const auto& over : {Batch{std::vector<std::string>(5, "a"), std::nullopt},
                            Batch{std::vector<std::string>{"123456789"}, 3u}})
   {
      CHECK_FALSE(encodes(over));
      CHECK_FALSE(reads<Batch>(unchecked(over)));
   }
}

TEST_CASE("method annotations: the cap bounds the argument tuple", "[method_annotations]")
{
   // note(text) under maxDynamicData(64): header 1 + slot 1 + text.
   CHECK(encodes(Note{std::string(62, 'n')}));
   CHECK(bytes_of(Note{std::string(62, 'n')}).size() == 64);
   CHECK_FALSE(encodes(Note{std::string(63, 'n')}));
   // The frame of 65 bytes (a 1-byte header can still describe it).
   auto frame = bytes_of(Note{std::string(62, 'n')});
   frame.push_back('n');
   CHECK_FALSE(reads<Note>(frame));
}

TEST_CASE("method annotations: a trailing optional argument evolves under the cap",
          "[method_annotations]")
{
   const Transfer  old_args{"bob", 9, "memo"};
   const Transfer2 new_none{"bob", 9, "memo", std::nullopt};
   const Transfer2 new_some{"bob", 9, "memo", 3u};
   // A new writer that leaves the argument out writes the old bytes.
   CHECK(bytes_of(new_none) == bytes_of(old_args));
   // Old bytes read as the new arguments: the argument is absent.
   const auto as_new = psio::decode<Transfer2>(psio::pssz{}, std::span<const char>(bytes_of(old_args)));
   CHECK(as_new.tuple() == new_none.tuple());
   // New bytes read as the old arguments: the unknown argument is skipped.
   bool     unknown = false;
   Transfer as_old;
   CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>(bytes_of(new_some)), as_old,
                                   unknown)
            .ok());
   CHECK(unknown);
   CHECK(as_old.tuple() == old_args.tuple());
}

TEST_CASE("method annotations: returns(...) is checked on the value", "[method_annotations]")
{
   CHECK(psio::check_method_result<token, 2>(std::string(32, 'n')).ok());
   CHECK_FALSE(psio::check_method_result<token, 2>(std::string(33, 'n')).ok());
   CHECK_FALSE(psio::check_method_result<token, 2>(std::string("\xff")).ok());
}

TEST_CASE("method annotations: the interface schema carries them", "[method_annotations][schema]")
{
   using namespace psio::schema_types;
   const psio::Schema schema = psio::SchemaBuilder{}.insert_interface<token>().build();
   REQUIRE(schema.interfaces.size() == 1);
   const auto& funcs = schema.interfaces[0].funcs;
   REQUIRE(funcs.size() == 7);

   const AnyType u8     = Int{8, false};
   const auto    text   = [&](std::uint64_t n) {
      return AnyType{Custom{Box<AnyType>{AnyType{BoundedList{Box<AnyType>{u8}, n}}}, "utf8"}};
   };
   // Bounded names resolve to the builder's interned u8.
   const auto resolved = [&](const AnyType& type) {
      AnyType out = type;
      if (auto* custom = std::get_if<Custom>(&out.value))
         if (auto* list = std::get_if<BoundedList>(&custom->type->value))
            if (auto* ref = std::get_if<Type>(&list->type->value))
               *list->type = *schema.get(ref->type);
      return out;
   };

   // transfer(to: utf8 <= 16, amount, memo: utf8 <= 80) under a cap of 256.
   CHECK(funcs[0].name == "transfer");
   REQUIRE(funcs[0].params.size() == 3);
   CHECK(funcs[0].params[0].name == "to");
   CHECK(resolved(*funcs[0].params[0].type) == text(16));
   CHECK(std::holds_alternative<Type>(funcs[0].params[1].type->value));
   CHECK(resolved(*funcs[0].params[2].type) == text(80));
   CHECK(funcs[0].attributes == std::vector<Attribute>{{"maxDynamicData", "256"}});
   CHECK(funcs[1].attributes.empty());

   // name_of returns utf8 <= 32.
   REQUIRE(funcs[2].result.has_value());
   CHECK(resolved(**funcs[2].result) == text(32));
   // plain: untouched.
   CHECK(funcs[3].attributes.empty());
   CHECK(std::holds_alternative<Type>(funcs[3].params[1].type->value));
   CHECK(funcs[5].attributes == std::vector<Attribute>{{"maxDynamicData", "64"}});

   // WIT: the cap as an @-attribute above the function.
   const std::string wit = psio::emit_wit(schema);
   CHECK(wit.find("@maxDynamicData(256)\n") != std::string::npos);
   CHECK(wit.find("@maxDynamicData(64)\n") != std::string::npos);
   CHECK(wit.find("transfer: func(") != std::string::npos);
}

TEST_CASE("method annotations: the argument tuple in JSON", "[method_annotations][json]")
{
   const TransferUncapped args{"alice", 5, "thanks"};
   const std::string      text = psio::encode(psio::json{}, args);
   CHECK(text == R"(["alice",5,"thanks"])");
   CHECK(psio::decode<TransferUncapped>(psio::json{}, std::span<const char>{text}).tuple() ==
         args.tuple());
   CHECK_THROWS_AS(psio::encode(psio::json{}, TransferUncapped{std::string(17, 'a'), 1, ""}),
                   psio::codec_exception);
   const std::string over = R"(["aaaaaaaaaaaaaaaaa",1,""])";
   CHECK_FALSE(psio::validate<TransferUncapped>(psio::json{}, std::span<const char>{over}).ok());
   CHECK_THROWS_AS(psio::decode<TransferUncapped>(psio::json{}, std::span<const char>{over}),
                   psio::codec_exception);
}
