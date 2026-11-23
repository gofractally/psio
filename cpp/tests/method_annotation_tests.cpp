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
#include <psio/pssz_ir_compare.hpp>
#include <psio/pssz_view.hpp>
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
// (The cap is not in the inferred bound, as for a capped record:
// docs/pssz-spec.md 3.2. The largest encoding counts the header and the two
// slots at the cap's 2 bytes: 3.7.)
static_assert(psio::inferred_bound<Transfer>() == 1 + (1 + 16) + 8 + (1 + 80));
static_assert(psio::max_encoded_size<Transfer>() == 2 + (2 + 16) + 8 + (2 + 80));
static_assert(psio::effective_max_dynamic_v<Transfer> == 256);
static_assert(psio::auto_pssz_width_v<Transfer> == 2);
// Without a cap: the inferred bound, rounded up to the next width.
static_assert(!psio::max_dynamic_data_v<TransferUncapped>.has_value());
static_assert(psio::inferred_bound<TransferUncapped>() == 1 + (1 + 16) + 8 + (1 + 80));
static_assert(psio::auto_pssz_width_v<TransferUncapped> == 1);
// No annotation: exactly the plain tuple.
static_assert(!psio::inferred_bound<Plain>().has_value());
static_assert(psio::auto_pssz_width_v<Plain> == 4);
// header 1 + slot 1 + 4 x (table slot 1 + 8) + slot 1 + (selector 1 + 4)
static_assert(psio::inferred_bound<Batch>() == 1 + (1 + 4 * (1 + 8)) + (1 + 1 + 4));
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
   static_assert(psio::inferred_bound<W>() == psio::inferred_bound<TransferUncapped>());
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

// The result of a method, in one call each way, and in a schema that does
// not use the Interface envelope.
namespace mr_test
{
   struct Entry
   {
      std::uint64_t id = 0;
      std::string   note;
      friend bool   operator==(const Entry&, const Entry&) = default;
   };
   PSIO_REFLECT(Entry, id, note)
   struct directory
   {
      std::vector<std::string>   names(std::uint32_t limit) const;
      std::optional<Entry>       find(std::uint64_t id) const;
      std::optional<std::string> label(std::uint64_t id) const;
      void                       touch(std::uint64_t id);
      std::string                digest(std::uint64_t id) const;
      std::string                host(std::string host) const;
      PSIO_REFLECT(directory,
                   method(names, limit, returns(max<4> | element(utf8 | max<16>))),
                   method(find, id),
                   method(label, id, returns(utf8 | max<8>)),
                   method(touch, id),
                   method(digest, id, returns(exact<32>)),
                   method(host, attr(host, utf8), returns(utf8)))
   };
   PSIO_INTERFACE(directory, types())
}  // namespace mr_test

TEST_CASE("method results: one call to write, read, validate and view", "[method_annotations][result]")
{
   using mr_test::directory;
   using mr_test::Entry;
   using Names = std::vector<std::string>;

   // names: returns(max<4> | element(utf8 | max<16>)) — a 1-byte offset table.
   const Names two{"alice", "bob"};
   const auto  bytes = psio::method_result_to_pssz<directory, 0>(two);
   REQUIRE(bytes == std::vector<char>{2, 7, 'a', 'l', 'i', 'c', 'e', 'b', 'o', 'b'});
   REQUIRE(bytes != psio::to_pssz(two));  // the plain type has a 4-byte table
   REQUIRE(psio::validate_method_result<directory, 0>(bytes).ok());
   REQUIRE(psio::method_result_from_pssz<directory, 0>(std::span<const char>{bytes}) == two);
   REQUIRE(psio::method_result_from_pssz<directory, 0>(psio::trusted{bytes}) == two);
   std::vector<char> appended{'x'};
   psio::method_result_append_pssz<directory, 0>(appended, two);
   REQUIRE(std::vector<char>(appended.begin() + 1, appended.end()) == bytes);

   // The view reads the result's own offsets; a view of the plain type would not.
   psio::method_result_view<directory, 0> safe{bytes};
   REQUIRE(safe.size() == 2);
   REQUIRE(std::string_view{safe[1]} == "bob");
   const auto checked = psio::validated_method_result_view<directory, 0>(bytes);
   REQUIRE(checked.ok());
   REQUIRE(checked->size() == 2);
   REQUIRE(checked->unpack() == two);

   // Outside returns(...): refused when written, when read, when viewed.
   const Names five(5, "a");
   REQUIRE_THROWS_AS((psio::method_result_to_pssz<directory, 0>(five)), psio::codec_exception);
   const auto frame = psio::detail::pssz_impl::encode_method_result_unchecked<directory, 0>(five);
   REQUIRE_FALSE(psio::validate_method_result<directory, 0>(frame).ok());
   REQUIRE_THROWS_AS((psio::method_result_from_pssz<directory, 0>(std::span<const char>{frame})),
                     psio::codec_exception);
   REQUIRE_FALSE(psio::validated_method_result_view<directory, 0>(frame).ok());

   // find: an optional whose payload cannot be empty. None is zero bytes.
   REQUIRE(psio::method_result_to_pssz<directory, 1>(std::optional<Entry>{}).empty());
   REQUIRE(psio::validate_method_result<directory, 1>({}).ok());
   REQUIRE(psio::method_result_from_pssz<directory, 1>(std::span<const char>{}) == std::nullopt);
   REQUIRE(psio::method_result_from_pssz<directory, 1>(psio::trusted{std::span<const char>{}}) ==
           std::nullopt);
   REQUIRE_FALSE(psio::validated_method_result_view<directory, 1>({})->has_value());
   const std::optional<Entry> some{Entry{7, "ok"}};
   const auto                 some_bytes = psio::method_result_to_pssz<directory, 1>(some);
   REQUIRE(some_bytes == psio::to_pssz(Entry{7, "ok"}));  // the value's own encoding
   REQUIRE(psio::method_result_from_pssz<directory, 1>(std::span<const char>{some_bytes}) == some);
   REQUIRE(psio::validated_method_result_view<directory, 1>(some_bytes)->has_value());

   // label: an optional whose payload can be empty has a selector byte.
   REQUIRE(psio::method_result_to_pssz<directory, 2>(std::optional<std::string>{}) ==
           std::vector<char>{0});
   REQUIRE(psio::method_result_to_pssz<directory, 2>(std::optional<std::string>{""}) ==
           std::vector<char>{1});
   REQUIRE_FALSE(psio::validate_method_result<directory, 2>({}).ok());
   REQUIRE_THROWS_AS(
      (psio::method_result_to_pssz<directory, 2>(std::optional<std::string>{"123456789"})),
      psio::codec_exception);

   // touch returns nothing: no result bytes, and only those.
   static_assert(std::is_void_v<psio::method_result_t<directory, 3>>);
   REQUIRE(psio::validate_method_result<directory, 3>({}).ok());
   psio::method_result_from_pssz<directory, 3>(std::span<const char>{});
   const std::vector<char> one{0};
   REQUIRE_FALSE(psio::validate_method_result<directory, 3>(one).ok());
   REQUIRE_THROWS_AS((psio::method_result_from_pssz<directory, 3>(std::span<const char>{one})),
                     psio::codec_exception);
}

TEST_CASE("method results: a schema without the Interface envelope carries returns(...)",
          "[method_annotations][result][schema]")
{
   using mr_test::directory;
   namespace ir = psio::schema_types;

   psio::SchemaBuilder iface;
   iface.insert_interface<directory>();
   const auto with = std::move(iface).build();

   psio::SchemaBuilder builder;
   const auto names = builder.insert_method<directory, 0>();
   const auto find  = builder.insert_method<directory, 1>();
   const auto touch = builder.insert_method<directory, 3>();
   const auto host  = builder.insert_method<directory, 5>();
   REQUIRE(builder.status().ok());
   const auto schema = builder.build();

   // The bounded list of bounded UTF-8 strings, not the plain list.
   REQUIRE(names.result);
   const auto& list = std::get<ir::BoundedList>((*names.result)->value);
   REQUIRE(list.maxCount == 4);
   const auto& text = std::get<ir::Custom>((*list.type).value);
   REQUIRE(text.id == "utf8");
   REQUIRE(std::get<ir::BoundedList>((*text.type).value).maxCount == 16);
   // returns(utf8) on a string: the "utf8" node, not the raw "string".
   REQUIRE(std::get<ir::Custom>((*host.result)->value).id == "utf8");
   // The same node the Interface gives the function, for every method.
   const auto same = [&](const ir::FunctionType& fn, std::size_t i) {
      const auto& f = with.interfaces.at(0).funcs.at(i);
      return fn.result.has_value() == f.result.has_value() &&
             (!fn.result ||
              psio::pssz_ir::equivalent(with, **f.result, schema, **fn.result).ok);
   };
   REQUIRE(same(names, 0));
   REQUIRE(same(find, 1));
   REQUIRE(same(touch, 3));
   REQUIRE(same(host, 5));
   REQUIRE_FALSE(touch.result);

   // The parts: the argument list is the node of method_arg_tuple<I>.
   psio::SchemaBuilder parts;
   const auto          args = parts.insert_method_args<directory, 0>();
   REQUIRE(args == parts.insert<psio::reflect<directory>::method_arg_tuple<0>>());
   REQUIRE(parts.insert_method_result<directory, 0>().has_value());
   REQUIRE_FALSE(parts.insert_method_result<directory, 3>().has_value());
   REQUIRE(parts.status().ok());

   // The frame of a result validates against the bare node, and a frame
   // outside returns(...) does not.
   bool       unknown = false;
   const auto good    = psio::method_result_to_pssz<directory, 0>(std::vector<std::string>{"a"});
   REQUIRE(psio::pssz_ir::validate_auto(schema, **names.result, good, &unknown).ok());
   const auto bad = psio::detail::pssz_impl::encode_method_result_unchecked<directory, 0>(
      std::vector<std::string>(5, "a"));
   REQUIRE_FALSE(psio::pssz_ir::validate_auto(schema, **names.result, bad, &unknown).ok());

   // returns(exact<32>) has no type form: a Func carries it as an attribute,
   // a bare result node cannot, and says so instead of dropping it.
   psio::SchemaBuilder lossy;
   (void)lossy.insert_method_result<directory, 4>();
   REQUIRE_FALSE(lossy.status().ok());
   REQUIRE(std::string{lossy.status().error().what}.find("lengthBound") != std::string::npos);
   const auto& digest = with.interfaces.at(0).funcs.at(4);
   REQUIRE(digest.attributes.size() == 1);
   REQUIRE(digest.attributes[0].name == "result.lengthBound");
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
   // A method that names its parameters: an object keyed by the names; a
   // reader also takes the positional array (docs/json.md "Method arguments").
   CHECK(text == R"({"to":"alice","amount":5,"memo":"thanks"})");
   CHECK(psio::decode<TransferUncapped>(psio::json{}, std::span<const char>{text}).tuple() ==
         args.tuple());
   CHECK_THROWS_AS(psio::encode(psio::json{}, TransferUncapped{std::string(17, 'a'), 1, ""}),
                   psio::codec_exception);
   const std::string over = R"(["aaaaaaaaaaaaaaaaa",1,""])";
   CHECK_FALSE(psio::validate<TransferUncapped>(psio::json{}, std::span<const char>{over}).ok());
   CHECK_THROWS_AS(psio::decode<TransferUncapped>(psio::json{}, std::span<const char>{over}),
                   psio::codec_exception);
}
