// A record or tuple whose known members are all fixed-size evolves like any
// other extensible record — old readers read new bytes, new readers read
// old bytes, trailing absent optionals are pruned — exactly when its offset
// WIDTH is kept: an explicit maxDynamicData on both revisions, or an
// appended member that keeps the inferred bound in the same width class.
//
// Appending an unbounded optional to an uncapped bounded type moves the
// width (1 byte -> 4 bytes) and is not a compatible change: the compile-time
// check (psio::pssz_evolves_from) and the run-time one
// (psio::pssz_ir::evolves_from, code "width-changed") refuse exactly those
// pairs, and say to declare a cap.

#include <catch.hpp>

#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_compare.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace ir = psio::pssz_ir;
namespace st = psio::schema_types;

namespace
{
   using Prev = std::optional<std::vector<std::uint64_t>>;

   // The report's pair: no cap. 1-byte offsets, then 4-byte ones.
   struct Open
   {
      std::int32_t  socket = 0;
      std::uint64_t owner  = 0;
   };
   PSIO_REFLECT(Open, socket, owner)
   struct OpenGrown
   {
      std::int32_t  socket = 0;
      std::uint64_t owner  = 0;
      Prev          prev;
   };
   PSIO_REFLECT(OpenGrown, socket, owner, prev)

   // The same pair with room declared from the first revision.
   struct Row
   {
      std::int32_t  socket = 0;
      std::uint64_t owner  = 0;
      PSIO_REFLECT(Row, socket, owner, maxDynamicData(1 << 20))
   };
   struct RowGrown
   {
      std::int32_t  socket = 0;
      std::uint64_t owner  = 0;
      Prev          prev;
      PSIO_REFLECT(RowGrown, socket, owner, prev, maxDynamicData(1 << 20))
   };
   struct RowGrownTwice
   {
      std::int32_t               socket = 0;
      std::uint64_t              owner  = 0;
      Prev                       prev;
      std::optional<std::string> note;
      PSIO_REFLECT(RowGrownTwice, socket, owner, prev, note, maxDynamicData(1 << 20))
   };
   // A narrow cap keeps a narrow width, and bounds what may be appended.
   struct Small
   {
      std::int32_t socket = 0;
      PSIO_REFLECT(Small, socket, maxDynamicData(255))
   };
   struct SmallGrown
   {
      std::int32_t               socket = 0;
      std::optional<std::string> note;
      PSIO_REFLECT(SmallGrown, socket, note, maxDynamicData(255))
   };

   // No cap, and an appended member that stays in the width class.
   struct OpenByte
   {
      std::int32_t                socket = 0;
      std::uint64_t               owner  = 0;
      std::optional<std::uint8_t> flag;
   };
   PSIO_REFLECT(OpenByte, socket, owner, flag)

   // Argument tuples: the method's cap is the tuple's.
   struct V1
   {
      void onError(std::int32_t socket) {}
      void close(std::int32_t socket) {}
      PSIO_REFLECT(V1, method(onError, socket, maxDynamicData(1 << 20)), method(close, socket))
   };
   struct V2
   {
      void onError(std::int32_t socket, std::optional<std::string> reply) {}
      void close(std::int32_t socket, std::optional<std::string> reply) {}
      PSIO_REFLECT(V2,
                   method(onError, socket, reply, maxDynamicData(1 << 20)),
                   method(close, socket, reply))
   };
   template <typename T, std::size_t I>
   using Args = typename psio::reflect<T>::template method_arg_tuple<I>;

   using T1 = std::tuple<std::int32_t>;
   using T2 = std::tuple<std::int32_t, std::optional<std::string>>;
   using T2b = std::tuple<std::int32_t, std::optional<std::uint8_t>>;

   // ── Compile time ──────────────────────────────────────────────────────
   // Width kept: evolves.
   static_assert(psio::auto_pssz_width_v<Row> == 4 && psio::auto_pssz_width_v<RowGrown> == 4);
   static_assert(psio::pssz_evolves_from<Row, RowGrown>);
   static_assert(psio::pssz_evolves_from<RowGrown, RowGrownTwice>);
   static_assert(psio::pssz_evolves_from<Row, RowGrownTwice>);
   static_assert(psio::auto_pssz_width_v<Small> == 1 && psio::auto_pssz_width_v<SmallGrown> == 1);
   static_assert(psio::pssz_evolves_from<Small, SmallGrown>);
   static_assert(psio::auto_pssz_width_v<Open> == 1 && psio::auto_pssz_width_v<OpenByte> == 1);
   static_assert(psio::pssz_evolves_from<Open, OpenByte>);
   static_assert(psio::pssz_evolves_from<T1, T2b>);
   static_assert(psio::pssz_evolves_from<Args<V1, 0>, Args<V2, 0>>);
   static_assert(psio::pssz_evolution_keeps_width_v<Row, RowGrown>);
   // Width moved: refused, and the pair is told apart from other reasons.
   static_assert(psio::auto_pssz_width_v<OpenGrown> == 4);
   static_assert(!psio::pssz_evolves_from<Open, OpenGrown>);
   static_assert(!psio::pssz_evolution_keeps_width_v<Open, OpenGrown>);
   static_assert(psio::auto_pssz_width_v<T1> == 1 && psio::auto_pssz_width_v<T2> == 4);
   static_assert(!psio::pssz_evolves_from<T1, T2>);
   static_assert(!psio::pssz_evolution_keeps_width_v<T1, T2>);
   static_assert(!psio::pssz_evolves_from<Args<V1, 1>, Args<V2, 1>>);
   static_assert(!psio::pssz_evolves_from<Open, Row>);  // adding the cap later moves it too
   // Never backwards.
   static_assert(!psio::pssz_evolves_from<RowGrown, Row>);

   std::span<const char> sp(const std::vector<char>& b) { return {b.data(), b.size()}; }

   template <typename Old, typename New>
   ir::compatibility runtime(const char* = nullptr)
   {
      auto a = psio::SchemaBuilder{}.insert<Old>("T").build();
      auto b = psio::SchemaBuilder{}.insert<New>("T").build();
      return ir::evolves_from(a, "T", b, "T");
   }

   // Old reads New's bytes (dropping what it does not know), New reads
   // Old's (absent = None), in the typed codec, the schema-driven codec and
   // through views.
   template <typename Old, typename New>
   void both_ways(const Old& old_value, const New& grown_absent, const New& grown_present)
   {
      const auto old_bytes     = psio::to_pssz(old_value);
      const auto absent_bytes  = psio::to_pssz(grown_absent);
      const auto present_bytes = psio::to_pssz(grown_present);
      // Pruning: the absent appended members leave Old's bytes.
      REQUIRE(absent_bytes == old_bytes);
      REQUIRE(present_bytes != old_bytes);

      // Typed: new reader, old bytes.
      REQUIRE(psio::validate_pssz<New>(old_bytes).ok());
      REQUIRE(psio::validate_strict<New>(psio::pssz{}, sp(old_bytes)).ok());
      REQUIRE(psio::to_pssz(psio::from_pssz<New>(sp(old_bytes))) == old_bytes);
      // Typed: old reader, new bytes.
      bool unknown = false;
      REQUIRE(psio::validate_pssz<Old>(present_bytes).ok());
      REQUIRE(psio::validate_pssz_compatible<Old>(psio::pssz{}, sp(present_bytes), unknown).ok());
      REQUIRE(unknown);
      REQUIRE_FALSE(psio::validate_strict<Old>(psio::pssz{}, sp(present_bytes)).ok());
      REQUIRE(psio::to_pssz(psio::from_pssz<Old>(sp(present_bytes))) == old_bytes);
      Old out{};
      REQUIRE(psio::decode_pssz_checked(psio::pssz{}, sp(present_bytes), out, unknown).ok());
      REQUIRE(unknown);

      // Schema-driven: the same answers from the IR of each revision.
      const auto old_schema = psio::SchemaBuilder{}.insert<Old>("T").build();
      const auto new_schema = psio::SchemaBuilder{}.insert<New>("T").build();
      ir::Value  value;
      unknown = true;
      REQUIRE(ir::decode_auto(new_schema, st::Type{"T"}, sp(old_bytes), value, unknown).ok());
      REQUIRE_FALSE(unknown);
      std::vector<char> again;
      REQUIRE(ir::encode_auto(new_schema, st::Type{"T"}, value, again).ok());
      REQUIRE(again == old_bytes);
      REQUIRE(ir::decode_auto(old_schema, st::Type{"T"}, sp(present_bytes), value, unknown).ok());
      REQUIRE(unknown);
      again.clear();
      REQUIRE(ir::encode_auto(old_schema, st::Type{"T"}, value, again).ok());
      REQUIRE(again == old_bytes);

      // The two checks agree.
      REQUIRE(psio::pssz_evolves_from_v<Old, New>);
      REQUIRE(runtime<Old, New>());
   }
}  // namespace

TEST_CASE("an all-fixed record with a cap evolves both ways, with pruning", "[pssz][evolution]")
{
   both_ways(Row{1, 2}, RowGrown{1, 2, std::nullopt},
             RowGrown{1, 2, std::vector<std::uint64_t>{3}});
   both_ways(RowGrown{1, 2, std::vector<std::uint64_t>{3}},
             RowGrownTwice{1, 2, std::vector<std::uint64_t>{3}, std::nullopt},
             RowGrownTwice{1, 2, std::vector<std::uint64_t>{3}, std::string("n")});
   both_ways(Row{1, 2}, RowGrownTwice{1, 2, std::nullopt, std::nullopt},
             RowGrownTwice{1, 2, std::nullopt, std::string("n")});
   both_ways(Small{7}, SmallGrown{7, std::nullopt}, SmallGrown{7, std::string("note")});

   // Views: the old view over new bytes, the new view over old bytes.
   const auto grown = psio::to_pssz(RowGrown{1, 2, std::vector<std::uint64_t>{3, 4}});
   psio::pssz_view<Row> old_view{grown};
   REQUIRE(old_view.socket() == 1);
   REQUIRE(old_view.owner() == 2);
   const auto                plain = psio::to_pssz(Row{5, 6});
   psio::pssz_view<RowGrown> new_view{plain};
   REQUIRE(new_view.socket() == 5);
   REQUIRE(new_view.owner() == 6);
   REQUIRE_FALSE(new_view.prev().has_value());
   psio::pssz_view<RowGrown> full{grown};
   REQUIRE(full.prev().has_value());
   REQUIRE(full.prev()->size() == 2);
}

TEST_CASE("an uncapped all-fixed record evolves while its width class is kept",
          "[pssz][evolution]")
{
   both_ways(Open{1, 2}, OpenByte{1, 2, std::nullopt}, OpenByte{1, 2, std::uint8_t{9}});
   both_ways(T1{7}, T2b{7, std::nullopt}, T2b{7, std::uint8_t{1}});
}

TEST_CASE("argument tuples evolve under the method's cap", "[pssz][evolution][method]")
{
   // (socket) -> (socket, optional reply): a call with the reply left out
   // is the old call, byte for byte.
   const auto old_call = psio::method_args_to_pssz<V1, 0>(4);
   REQUIRE((psio::method_args_to_pssz<V2, 0>(4, std::nullopt) == old_call));
   REQUIRE(psio::validate_method_args<V2, 0>(old_call).ok());
   const auto new_call = psio::method_args_to_pssz<V2, 0>(4, std::string("gone"));
   REQUIRE(psio::validate_method_args<V1, 0>(new_call).ok());
   REQUIRE(std::get<0>(psio::from_pssz<Args<V1, 0>>(sp(new_call))) == 4);
   REQUIRE_FALSE(std::get<1>(psio::from_pssz<Args<V2, 0>>(sp(old_call))).has_value());

   // Without a cap the two argument lists have different widths: the old
   // call is not a new call.
   const auto open_old = psio::method_args_to_pssz<V1, 1>(4);
   REQUIRE((psio::method_args_to_pssz<V2, 1>(4, std::nullopt) != open_old));
   REQUIRE_FALSE(psio::validate_method_args<V2, 1>(open_old).ok());
}

TEST_CASE("a moved width is refused by both checks, which say what to do",
          "[pssz][evolution]")
{
   // The bytes: 1-byte header against a 4-byte one.
   const auto narrow = psio::to_pssz(Open{1, 2});
   const auto wide   = psio::to_pssz(OpenGrown{1, 2, std::nullopt});
   REQUIRE(narrow.size() == 13);
   REQUIRE(wide.size() == 16);
   REQUIRE_FALSE(psio::validate_pssz<OpenGrown>(narrow).ok());
   REQUIRE_FALSE(psio::validate_pssz<Open>(wide).ok());
   REQUIRE_FALSE(psio::validate_pssz<T2>(psio::to_pssz(T1{7})).ok());
   REQUIRE_FALSE(psio::validate_pssz<T1>(psio::to_pssz(T2{7, std::nullopt})).ok());

   // Run time: the code names the width, at the record, and the reason
   // says to declare a cap.
   for (const auto& no : {runtime<Open, OpenGrown>(), runtime<T1, T2>(), runtime<Open, Row>()})
   {
      REQUIRE_FALSE(no);
      REQUIRE(no.code == "width-changed");
      REQUIRE(no.path == "(root)");
      REQUIRE(no.reason.find("the offset width of the record differs (1 / 4)") == 0);
      REQUIRE(no.reason.find("declare maxDynamicData") != std::string::npos);
   }
   // The same types the compile-time check refused; and it accepts what
   // the run-time check accepts.
   REQUIRE_FALSE(psio::pssz_evolves_from_v<Open, OpenGrown>);
   REQUIRE_FALSE(psio::pssz_evolves_from_v<T1, T2>);
   REQUIRE(runtime<Row, RowGrown>());
   REQUIRE(runtime<Open, OpenByte>());
   REQUIRE(runtime<Small, SmallGrown>());
   // Another reason keeps its own code.
   const auto backwards = runtime<RowGrown, Row>();
   REQUIRE_FALSE(backwards);
   REQUIRE(backwards.code == "fields-removed");
}
